#define LOG_TAG "android.hardware.biometrics.fingerprint@2.3-jiiov-service"
#define LOG_VERBOSE "android.hardware.biometrics.fingerprint@2.3-jiiov-service"

#include <hardware/hw_auth_token.h>

#include <hardware/hardware.h>
#include <hardware/fingerprint.h>
#include "BiometricsFingerprint.h"

#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <string.h>
#include <poll.h>

namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace V2_3 {
namespace implementation {

static const uint16_t kVersion = HARDWARE_MODULE_API_VERSION(2, 1);

using RequestStatus =
        android::hardware::biometrics::fingerprint::V2_1::RequestStatus;
using FingerprintError =
        android::hardware::biometrics::fingerprint::V2_1::FingerprintError;
using FingerprintAcquiredInfo =
        android::hardware::biometrics::fingerprint::V2_1::FingerprintAcquiredInfo;

BiometricsFingerprint *BiometricsFingerprint::sInstance = nullptr;

int BiometricsFingerprint::tran_fp_opendev() {
    const char* devnode = nullptr;
    int fd;
    int ret = -1;

    const char* devnodes[] = {
        "/dev/jiiov_fp", "/dev/fortsense_fp", "/dev/focaltech_fp",
        "/dev/elan_fp", "/dev/goodix_fp", "/dev/egis_fp",
        "/dev/fingerprint", "/dev/silead_fp", "/dev/fpc_fp",
        nullptr
    };

    for (int i = 0; devnodes[i]; i++) {
        fd = open(devnodes[i], O_RDWR);
        if (fd < 0) {
            continue;
        }
        ALOGD("tran_fp_opendev opened %s", devnodes[i]);
        int val = 0;
        ret = ioctl(fd, 0x7404, &val);
        if (ret < 0) {
            ALOGE("tran_fp_opendev ioctl 0x7404 failed on %s, ret=%d", devnodes[i], ret);
        } else {
            ALOGD("tran_fp_opendev ioctl 0x7404 success on %s, val=%d", devnodes[i], val);
            ret = val;
        }
        close(fd);
        if (ret >= 0) break;
    }
    if (ret < 0) {
        ALOGE("tran_fp_opendev: no device found or all ioctls failed");
    }
    return ret;
}

void BiometricsFingerprint::netlinkThread() {
    struct sockaddr_nl sa;
    int nl_fd;

    nl_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_USERSOCK);
    if (nl_fd < 0) {
        ALOGE("netlinkThread: socket create failed");
        return;
    }

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 0;
    sa.nl_pid = static_cast<__u32>(getpid());

    if (bind(nl_fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        ALOGE("netlinkThread: bind failed");
        close(nl_fd);
        return;
    }

    ALOGD("netlinkThread: started, pid=%u", sa.nl_pid);

    char buf[4096];
    struct pollfd pfd = {nl_fd, POLLIN, 0};

    while (true) {
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) break;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_nl nladdr;
        socklen_t addrlen = sizeof(nladdr);
        ssize_t len = recvfrom(nl_fd, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<struct sockaddr*>(&nladdr), &addrlen);
        if (len <= 0) continue;

        buf[len] = '\0';
        ALOGV("netlinkThread: received %zd bytes from pid=%u", len, nladdr.nl_pid);
    }

    close(nl_fd);
    ALOGD("netlinkThread: exiting");
}

BiometricsFingerprint::BiometricsFingerprint() : mClientCallback(nullptr), mDevice(nullptr) {
    sInstance = this;

    mDevice = openHal();
    if (!mDevice) {
        ALOGE("Can't open HAL module");
    }

    mNetlinkThread = std::thread(netlinkThread);
    mNetlinkThread.detach();

    tran_fp_opendev();
}

BiometricsFingerprint::~BiometricsFingerprint() {
    ALOGV("~BiometricsFingerprint()");
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return;
    }
    int err;
    if (0 != (err = mDevice->common.close(
            reinterpret_cast<hw_device_t*>(mDevice)))) {
        ALOGE("Can't close fingerprint module, error: %d", err);
        return;
    }
    mDevice = nullptr;
}

Return<RequestStatus> BiometricsFingerprint::ErrorFilter(int32_t error) {
    switch(error) {
        case 0: return RequestStatus::SYS_OK;
        case -2: return RequestStatus::SYS_ENOENT;
        case -4: return RequestStatus::SYS_EINTR;
        case -5: return RequestStatus::SYS_EIO;
        case -11: return RequestStatus::SYS_EAGAIN;
        case -12: return RequestStatus::SYS_ENOMEM;
        case -13: return RequestStatus::SYS_EACCES;
        case -14: return RequestStatus::SYS_EFAULT;
        case -16: return RequestStatus::SYS_EBUSY;
        case -22: return RequestStatus::SYS_EINVAL;
        case -28: return RequestStatus::SYS_ENOSPC;
        case -110: return RequestStatus::SYS_ETIMEDOUT;
        default:
            ALOGE("An unknown error returned from fingerprint vendor library: %d", error);
            return RequestStatus::SYS_UNKNOWN;
    }
}

FingerprintError BiometricsFingerprint::VendorErrorFilter(int32_t error,
            int32_t* vendorCode) {
    *vendorCode = 0;
    switch(error) {
        case FINGERPRINT_ERROR_HW_UNAVAILABLE:
            return FingerprintError::ERROR_HW_UNAVAILABLE;
        case FINGERPRINT_ERROR_UNABLE_TO_PROCESS:
            return FingerprintError::ERROR_UNABLE_TO_PROCESS;
        case FINGERPRINT_ERROR_TIMEOUT:
            return FingerprintError::ERROR_TIMEOUT;
        case FINGERPRINT_ERROR_NO_SPACE:
            return FingerprintError::ERROR_NO_SPACE;
        case FINGERPRINT_ERROR_CANCELED:
            return FingerprintError::ERROR_CANCELED;
        case FINGERPRINT_ERROR_UNABLE_TO_REMOVE:
            return FingerprintError::ERROR_UNABLE_TO_REMOVE;
        case FINGERPRINT_ERROR_LOCKOUT:
            return FingerprintError::ERROR_LOCKOUT;
        default:
            if (error >= FINGERPRINT_ERROR_VENDOR_BASE) {
                *vendorCode = error - FINGERPRINT_ERROR_VENDOR_BASE;
                return FingerprintError::ERROR_VENDOR;
            }
    }
    ALOGE("Unknown error from fingerprint vendor library: %d", error);
    return FingerprintError::ERROR_UNABLE_TO_PROCESS;
}

FingerprintAcquiredInfo BiometricsFingerprint::VendorAcquiredFilter(
        int32_t info, int32_t* vendorCode) {
    *vendorCode = 0;
    switch(info) {
        case FINGERPRINT_ACQUIRED_GOOD:
            return FingerprintAcquiredInfo::ACQUIRED_GOOD;
        case FINGERPRINT_ACQUIRED_PARTIAL:
            return FingerprintAcquiredInfo::ACQUIRED_PARTIAL;
        case FINGERPRINT_ACQUIRED_INSUFFICIENT:
            return FingerprintAcquiredInfo::ACQUIRED_INSUFFICIENT;
        case FINGERPRINT_ACQUIRED_IMAGER_DIRTY:
            return FingerprintAcquiredInfo::ACQUIRED_IMAGER_DIRTY;
        case FINGERPRINT_ACQUIRED_TOO_SLOW:
            return FingerprintAcquiredInfo::ACQUIRED_TOO_SLOW;
        case FINGERPRINT_ACQUIRED_TOO_FAST:
            return FingerprintAcquiredInfo::ACQUIRED_TOO_FAST;
        default:
            if (info >= FINGERPRINT_ACQUIRED_VENDOR_BASE) {
                *vendorCode = info - FINGERPRINT_ACQUIRED_VENDOR_BASE;
                return FingerprintAcquiredInfo::ACQUIRED_VENDOR;
            }
    }
    ALOGE("Unknown acquiredmsg from fingerprint vendor library: %d", info);
    return FingerprintAcquiredInfo::ACQUIRED_INSUFFICIENT;
}

Return<uint64_t> BiometricsFingerprint::setNotify(
        const sp<IBiometricsFingerprintClientCallback>& clientCallback) {
    std::lock_guard<std::mutex> lock(mClientCallbackMutex);
    mClientCallback = clientCallback;
    return reinterpret_cast<uint64_t>(mDevice);
}

Return<uint64_t> BiometricsFingerprint::preEnroll()  {
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return 0;
    }
    return mDevice->pre_enroll(mDevice);
}

Return<RequestStatus> BiometricsFingerprint::enroll(const hidl_array<uint8_t, 69>& hat,
        uint32_t gid, uint32_t timeoutSec) {
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return RequestStatus::SYS_UNKNOWN;
    }
    const hw_auth_token_t* authToken =
        reinterpret_cast<const hw_auth_token_t*>(hat.data());
    return ErrorFilter(mDevice->enroll(mDevice, authToken, gid, timeoutSec));
}

static void set_hbm(int on) {
    const char* path = "/sys/kernel/tran_display/lcm_hbm_state";
    FILE* f = fopen(path, "w");
    if (!f) {
        ALOGE("set_hbm: failed to open %s", path);
        return;
    }
    fwrite(on ? "1" : "0", 1, 1, f);
    fclose(f);
    ALOGD("set_hbm(%d)", on);
}

Return<bool> BiometricsFingerprint::isUdfps(uint32_t sensorId) {
    ALOGD("isUdfps(sensorId=%d)", sensorId);
    return true;
}

Return<void> BiometricsFingerprint::onFingerDown(uint32_t x, uint32_t y, float minor, float major) {
    ALOGD("onFingerDown(x=%u, y=%u, minor=%f, major=%f)", x, y, minor, major);
    set_hbm(1);
    return Void();
}

Return<void> BiometricsFingerprint::onFingerUp() {
    ALOGD("onFingerUp");
    set_hbm(0);
    return Void();
}

Return<RequestStatus> BiometricsFingerprint::postEnroll() {
    set_hbm(0);
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return RequestStatus::SYS_UNKNOWN;
    }
    return ErrorFilter(mDevice->post_enroll(mDevice));
}

Return<uint64_t> BiometricsFingerprint::getAuthenticatorId() {
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return 0;
    }
    return mDevice->get_authenticator_id(mDevice);
}

Return<RequestStatus> BiometricsFingerprint::cancel() {
    set_hbm(0);
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return RequestStatus::SYS_UNKNOWN;
    }
    return ErrorFilter(mDevice->cancel(mDevice));
}

Return<RequestStatus> BiometricsFingerprint::enumerate()  {
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return RequestStatus::SYS_UNKNOWN;
    }
    return ErrorFilter(mDevice->enumerate(mDevice));
}

Return<RequestStatus> BiometricsFingerprint::remove(uint32_t gid, uint32_t fid) {
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return RequestStatus::SYS_UNKNOWN;
    }
    return ErrorFilter(mDevice->remove(mDevice, gid, fid));
}

Return<RequestStatus> BiometricsFingerprint::setActiveGroup(uint32_t gid,
        const hidl_string& storePath) {
    if (storePath.size() >= PATH_MAX || storePath.size() <= 0) {
        ALOGE("Bad path length: %zd", storePath.size());
        return RequestStatus::SYS_EINVAL;
    }
    if (access(storePath.c_str(), W_OK)) {
        return RequestStatus::SYS_EINVAL;
    }
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return RequestStatus::SYS_UNKNOWN;
    }

    return ErrorFilter(mDevice->set_active_group(mDevice, gid,
                                                    storePath.c_str()));
}

Return<RequestStatus> BiometricsFingerprint::authenticate(uint64_t operationId,
        uint32_t gid) {
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return RequestStatus::SYS_UNKNOWN;
    }
    return ErrorFilter(mDevice->authenticate(mDevice, operationId, gid));
}

IBiometricsFingerprint* BiometricsFingerprint::getInstance() {
    if (!sInstance) {
      sInstance = new BiometricsFingerprint();
    }
    return sInstance;
}

fingerprint_device_t* BiometricsFingerprint::openHal() {
    ALOGD("Opening fingerprint hal library...");
    void* handle = dlopen("fingerprint.jiiov.so", RTLD_NOW);
    if (!handle) {
        ALOGE("dlopen fingerprint.jiiov.so failed: %s", dlerror());
        return nullptr;
    }

    hw_module_t* hmi = (hw_module_t*)dlsym(handle, "HMI");
    if (!hmi || !hmi->methods || !hmi->methods->open) {
        ALOGE("Invalid HMI module (hmi=%p, methods=%p)", hmi, hmi ? hmi->methods : nullptr);
        dlclose(handle);
        return nullptr;
    }

    hw_device_t* device = nullptr;
    int ret = hmi->methods->open(hmi, nullptr, &device);
    if (ret != 0) {
        ALOGE("HMI open failed: %d", ret);
        dlclose(handle);
        return nullptr;
    }
    if (!device) {
        ALOGE("HMI open returned null device");
        dlclose(handle);
        return nullptr;
    }

    fingerprint_device_t* fp_device =
        reinterpret_cast<fingerprint_device_t*>(device);

    if (kVersion != fp_device->common.version) {
        ALOGE("Wrong fp version. Expected %d, got %d", kVersion, fp_device->common.version);
        dlclose(handle);
        return nullptr;
    }

    if (0 != (fp_device->set_notify(fp_device, BiometricsFingerprint::notify))) {
        ALOGE("Can't register fingerprint module callback");
        dlclose(handle);
        return nullptr;
    }

    return fp_device;
}

void BiometricsFingerprint::notify(const fingerprint_msg_t *msg) {
    BiometricsFingerprint* thisPtr = static_cast<BiometricsFingerprint*>(
            BiometricsFingerprint::getInstance());
    std::lock_guard<std::mutex> lock(thisPtr->mClientCallbackMutex);
    if (thisPtr == nullptr || thisPtr->mClientCallback == nullptr) {
        ALOGE("Receiving callbacks before the client callback is registered.");
        return;
    }
    const uint64_t devId = reinterpret_cast<uint64_t>(thisPtr->mDevice);
    switch (msg->type) {
        case FINGERPRINT_ERROR: {
                set_hbm(0);
                int32_t vendorCode = 0;
                FingerprintError result = VendorErrorFilter(msg->data.error, &vendorCode);
                ALOGD("onError(%d)", result);
                if (!thisPtr->mClientCallback->onError(devId, result, vendorCode).isOk()) {
                    ALOGE("failed to invoke fingerprint onError callback");
                }
            }
            break;
        case FINGERPRINT_ACQUIRED: {
                int32_t vendorCode = 0;
                FingerprintAcquiredInfo result =
                    VendorAcquiredFilter(msg->data.acquired.acquired_info, &vendorCode);
                ALOGD("onAcquired(%d)", result);
                set_hbm(1);
                if (!thisPtr->mClientCallback->onAcquired(devId, result, vendorCode).isOk()) {
                    ALOGE("failed to invoke fingerprint onAcquired callback");
                }
            }
            break;
        case FINGERPRINT_TEMPLATE_ENROLLING:
            ALOGD("onEnrollResult(fid=%d, gid=%d, rem=%d)",
                msg->data.enroll.finger.fid,
                msg->data.enroll.finger.gid,
                msg->data.enroll.samples_remaining);
            if (msg->data.enroll.samples_remaining == 0) {
                set_hbm(0);
            }
            if (!thisPtr->mClientCallback->onEnrollResult(devId,
                    msg->data.enroll.finger.fid,
                    msg->data.enroll.finger.gid,
                    msg->data.enroll.samples_remaining).isOk()) {
                ALOGE("failed to invoke fingerprint onEnrollResult callback");
            }
            break;
        case FINGERPRINT_TEMPLATE_REMOVED:
            ALOGD("onRemove(fid=%d, gid=%d, rem=%d)",
                msg->data.removed.finger.fid,
                msg->data.removed.finger.gid,
                msg->data.removed.remaining_templates);
            if (!thisPtr->mClientCallback->onRemoved(devId,
                    msg->data.removed.finger.fid,
                    msg->data.removed.finger.gid,
                    msg->data.removed.remaining_templates).isOk()) {
                ALOGE("failed to invoke fingerprint onRemoved callback");
            }
            break;
        case FINGERPRINT_AUTHENTICATED:
            set_hbm(0);
            if (msg->data.authenticated.finger.fid != 0) {
                ALOGD("onAuthenticated(fid=%d, gid=%d)",
                    msg->data.authenticated.finger.fid,
                    msg->data.authenticated.finger.gid);
                const uint8_t* hat =
                    reinterpret_cast<const uint8_t *>(&msg->data.authenticated.hat);
                const hidl_vec<uint8_t> token(
                    std::vector<uint8_t>(hat, hat + sizeof(msg->data.authenticated.hat)));
                if (!thisPtr->mClientCallback->onAuthenticated(devId,
                        msg->data.authenticated.finger.fid,
                        msg->data.authenticated.finger.gid,
                        token).isOk()) {
                    ALOGE("failed to invoke fingerprint onAuthenticated callback");
                }
            } else {
                if (!thisPtr->mClientCallback->onAuthenticated(devId,
                        msg->data.authenticated.finger.fid,
                        msg->data.authenticated.finger.gid,
                        hidl_vec<uint8_t>()).isOk()) {
                    ALOGE("failed to invoke fingerprint onAuthenticated callback");
                }
            }
            break;
        case FINGERPRINT_TEMPLATE_ENUMERATING:
            ALOGD("onEnumerate(fid=%d, gid=%d, rem=%d)",
                msg->data.enumerated.finger.fid,
                msg->data.enumerated.finger.gid,
                msg->data.enumerated.remaining_templates);
            if (!thisPtr->mClientCallback->onEnumerate(devId,
                    msg->data.enumerated.finger.fid,
                    msg->data.enumerated.finger.gid,
                    msg->data.enumerated.remaining_templates).isOk()) {
                ALOGE("failed to invoke fingerprint onEnumerate callback");
            }
            break;
    }
}

} // namespace implementation
}  // namespace V2_3
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
