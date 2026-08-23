#include "Fingerprint.h"

#include <android-base/logging.h>

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

// CM6: JV0307 optical sensor under AMOLED, circle at (540, 2204) r=91
// (matches overlay FrameworksResTarget config_udfps_sensor_props).
static constexpr int32_t kSensorId = 0;
static constexpr int32_t kSensorStrength = 2;  // STRONG
static constexpr int32_t kMaxEnrollments = 5;
static constexpr bool kHalControlsIllumination = true;
static constexpr bool kSupportsDetectInteraction = true;

ndk::ScopedAStatus Fingerprint::getSensorProps(std::vector<SensorProps>* out) {
    SensorProps p;

    p.commonProps.sensorId = kSensorId;
    p.commonProps.sensorStrength = static_cast<common::SensorStrength>(kSensorStrength);
    p.commonProps.maxEnrollmentsPerUser = kMaxEnrollments;

    p.sensorType = FingerprintSensorType::UNDER_DISPLAY_OPTICAL;
    p.supportsDetectInteraction = kSupportsDetectInteraction;
    p.halHandlesDisplayTouches = false;
    p.halControlsIllumination = kHalControlsIllumination;

    SensorLocation loc;
    loc.displayId = 0;
    loc.sensorLocationX = 540;
    loc.sensorLocationY = 2204;
    loc.sensorRadius = 91;
    loc.sensorShape = SensorShape::CIRCLE;
    p.sensorLocations = {loc};

    *out = {p};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Fingerprint::createSession(
        int32_t sensorId, int32_t userId,
        const std::shared_ptr<ISessionCallback>& cb,
        std::shared_ptr<ISession>* out) {
    ALOGI("createSession sensor=%d user=%d", sensorId, userId);
    auto session = SharedRefBase::make<Session>(sensorId, userId);
    session->setCallback(cb);
    armFod();
    *out = session;
    return ndk::ScopedAStatus::ok();
}

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
