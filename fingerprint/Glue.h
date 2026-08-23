// Device glue: FOD touch arming, panel HBM, and the synthetic HBM uevent.
//
// Evidence chain (see session notes):
//  - The real FOD node is /sys/devices/platform/soc/11019000.spi5/spi_master/
//    spi5/spi5.0/special_area (focaltech_ft3683g); the /sys/devices/platform/
//    hot-area symlink path rejects writes with EACCES. Writing "1 ..." arms
//    it ("enable"); KEY(195) events then arrive on the touch input device.
//  - /proc/fingerprint_status (write-only, value "2") is the stock HAL's
//    per-session arm of the same path.
//  - The engine's HbmEventDetect worker binds NETLINK_KOBJECT_UEVENT and
//    parses TRAN_FULL_HBM_EVENT=%s; without that event PressEnroll fails
//    with "wait hbm ready time out, ret value: 37" (~630 ms after touch).
#pragma once

#include <cutils/properties.h>
#include <thread>
#include <utils/Log.h>

#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

static constexpr const char* kSpecialArea =
        "/sys/devices/platform/soc/11019000.spi5/spi_master/spi5/spi5.0/"
        "special_area";
static constexpr const char* kSpecialAreaArm =
        "1 0 0 0 7920 34544 9360 35984 1";
static constexpr const char* kFingerprintStatus = "/proc/fingerprint_status";
static constexpr const char* kLcmHbmState =
        "/sys/kernel/tran_display/lcm_hbm_state";

inline bool writeNode(const char* path, const std::string& val) {
    FILE* f = fopen(path, "w");
    if (!f) {
        ALOGE("writeNode %s failed: %s", path, strerror(errno));
        return false;
    }
    size_t n = fwrite(val.data(), 1, val.size(), f);
    bool ok = (n == val.size());
    if (!ok) ALOGE("writeNode %s short write (%zu/%zu)", path, n, val.size());
    fclose(f);
    return ok;
}

inline void setFingerprintStatus(int v) {
    writeNode(kFingerprintStatus, std::to_string(v));
}

inline void armFod() {
    writeNode(kSpecialArea, kSpecialAreaArm);
    setFingerprintStatus(2);
}

inline void armFod(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    armFod();
}

inline void disarmFod() {
    writeNode(kSpecialArea, "0");
    setFingerprintStatus(0);
}

inline void hbmOn() {
    writeNode(kLcmHbmState, "1");
}

inline void hbmOff() {
    writeNode(kLcmHbmState, "0");
}

// Broadcasts a kernel-style uevent on NETLINK_KOBJECT_UEVENT multicast
// group 1. Requires CAP_NET_ADMIN (we run as root). The engine's
// HbmEventDetect thread receives it like any kernel-originated event.
inline bool injectUevent(const std::string& devpath,
                         const std::string& subsystem,
                         const std::string& key, const std::string& value) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        ALOGE("uevent socket failed: %s", strerror(errno));
        return false;
    }
    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = 0;
    sa.nl_groups = 1;

    std::string payload = "change@" + devpath;
    payload.push_back('\0');
    payload += "ACTION=change";
    payload.push_back('\0');
    payload += "SUBSYSTEM=" + subsystem;
    payload.push_back('\0');
    payload += key + "=" + value;
    payload.push_back('\0');
    static uint32_t seq = 1;
    payload += "SEQNUM=" + std::to_string(seq++);
    payload.push_back('\0');

    ssize_t r = sendto(fd, payload.data(), payload.size(), 0,
                       reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    close(fd);
    if (r < 0) {
        ALOGE("injectUevent sendto failed: %s", strerror(errno));
        return false;
    }
    ALOGI("injected %s=%s (%zd bytes)", key.c_str(), value.c_str(), r);
    return true;
}

inline void illuminateForCapture() {
    hbmOn();
    usleep(90000);
    injectUevent("/devices/virtual/fod", "fod", "TRAN_FULL_HBM_EVENT",
                 "FULL_HBM_SET");
}

inline void endIllumination() {
    injectUevent("/devices/virtual/fod", "fod", "TRAN_FULL_HBM_EVENT",
                 "FULL_HBM_CLEAR");
    hbmOff();
}

// Debug hook: `setprop vendor.fp.hbm_inject 1` fires the full-illumination
// sequence by hand so the HBM-wait hypothesis can be validated on device
// without a rebuild of the capture path.
inline void initOnce() {
    static bool done = [] {
        armFod();
        static std::thread t([] {
            char buf[PROP_VALUE_MAX] = {0};
            while (true) {
                const prop_info* pi =
                        __system_property_find("vendor.fp.hbm_inject");
                if (pi != nullptr) {
                    __system_property_read_callback(
                            pi,
                            [](void* cookie, const char*, const char* value,
                               uint32_t) {
                                strncpy(static_cast<char*>(cookie), value,
                                        PROP_VALUE_MAX - 1);
                            },
                            buf);
                    if (buf[0] == '1') {
                        ALOGI("manual hbm inject (set)");
                        illuminateForCapture();
                        property_set("vendor.fp.hbm_inject", "0");
                        buf[0] = 0;
                    } else if (buf[0] == '2') {
                        ALOGI("manual hbm inject (clear)");
                        endIllumination();
                        property_set("vendor.fp.hbm_inject", "0");
                        buf[0] = 0;
                    }
                }
                usleep(80000);
            }
        });
        t.detach();
        return true;
    }();
    (void)done;
}

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
