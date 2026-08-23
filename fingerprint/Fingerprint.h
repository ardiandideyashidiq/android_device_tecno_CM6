#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>

#include "Session.h"

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

class Fingerprint : public BnFingerprint {
  public:
    ndk::ScopedAStatus getSensorProps(
            std::vector<SensorProps>* out) override;
    ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                     const std::shared_ptr<ISessionCallback>& cb,
                                     std::shared_ptr<ISession>* out) override;
};

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
