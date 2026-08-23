#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <aidl/android/hardware/biometrics/fingerprint/Error.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>

#include "Engine.h"
#include "Glue.h"

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

class Session;
void setActiveSession(const std::shared_ptr<Session>& s);

using common::ICancellationSignal;
using common::OperationContext;
using ::aidl::android::hardware::keymaster::HardwareAuthToken;

class CancellationSignal : public common::BnCancellationSignal {
  public:
    ndk::ScopedAStatus cancel() override;
};

class Session : public BnSession {
  public:
    Session(int sensorId, int userId);

    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus enroll(const HardwareAuthToken& hat,
                              std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus authenticate(int64_t operationId,
                                    std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteraction(
            std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& enrollmentIds) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(const HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;

    // Udfps surface.
    ndk::ScopedAStatus onPointerDown(int32_t pointerId, int32_t x, int32_t y,
                                     float minor, float major) override;
    ndk::ScopedAStatus onPointerUp(int32_t pointerId) override;
    ndk::ScopedAStatus onUiReady() override;
    ndk::ScopedAStatus authenticateWithContext(
            int64_t operationId, const OperationContext& context,
            std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enrollWithContext(const HardwareAuthToken& hat,
                                         const OperationContext& context,
                                         std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteractionWithContext(
            const OperationContext& context,
            std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus onPointerDownWithContext(const PointerContext& context) override;
    ndk::ScopedAStatus onPointerUpWithContext(const PointerContext& context) override;
    ndk::ScopedAStatus onContextChanged(const OperationContext& context) override;
    ndk::ScopedAStatus onPointerCancelWithContext(const PointerContext& context) override;
    ndk::ScopedAStatus setIgnoreDisplayTouches(bool shouldIgnore) override;

    void setCallback(const std::shared_ptr<ISessionCallback>& cb);

  private:
    void onEngineEvent(uint32_t type, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
    friend void engineNotifyThunk(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
    int mSensorId;
    int mUserId;
    std::shared_ptr<ISessionCallback> mCb;
};

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
