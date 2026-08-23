// Device glue: FOD touch arming, panel HBM, and the synthetic HBM uevent.
//
// Evidence chain (see session notes):
//  - /sys/devices/platform/hot-area/special_area  arms the focaltech FOD zone;
//    after arming, KEY(195) events arrive on /dev/input/event4 (mtk-tpd).
//  - /proc/fingerprint_status (write-only, value "2") is the stock HAL's
//    per-session arm of the same path.
//  - The engine's HbmEventDetect worker binds NETLINK_KOBJECT_UEVENT and
//    parses TRAN_FULL_HBM_EVENT=<value> (anc.hal.so @0x3c098 socket(AF_NETLINK,
//    SOCK_RAW, 15); strings "get hbm event, TRAN_FULL_HBM_EVENT=%s").
//  - Stock kernel never emits that uevent on this build (no disp_dbg nodes,
//    no DRM prop; sysfs lcm_hbm_state bypasses mtk_disp_notifier), so we set
//    HBM via sysfs and inject the uevent ourselves (verified working as root:
//    sendto group 1 delivered to a bound receiver).
#pragma once

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
        "/sys/devices/platform/hot-area/special_area";
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

inline void armFod() {
    writeNode(kSpecialArea, kSpecialAreaArm);
}

inline void disarmFod() {
    writeNode(kSpecialArea, "0");
}

inline void setFingerprintStatus(int v) {
    writeNode(kFingerprintStatus, std::to_string(v));
}

// Physically illuminates the panel. Does NOT satisfy the engine by itself.
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

// Full illumination sequence used before each capture window.
inline void illuminateForCapture() {
    hbmOn();
    // Give the panel a beat to reach full brightness before telling the
    // engine capture may begin (stock logs ~88ms between acquired and HBM).
    usleep(90000);
    injectUevent("/devices/virtual/fod", "fod",
                 "TRAN_FULL_HBM_EVENT", "FULL_HBM_SET");
}

inline void endIllumination() {
    injectUevent("/devices/virtual/fod", "fod",
                 "TRAN_FULL_HBM_EVENT", "FULL_HBM_CLEAR");
    hbmOff();
}

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
