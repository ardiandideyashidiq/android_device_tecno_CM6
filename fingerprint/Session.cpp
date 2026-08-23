#include "Session.h"

#include <android-base/logging.h>

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

// TODO(engine-wiring): cancellable operations currently report
// UNABLE_TO_PROCESS. They get wired to Anc* calls one by one as each
// signature is confirmed against anc.hal.so disassembly.

static ndk::ScopedAStatus notReady() {
    return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(
            static_cast<int32_t>(Error::UNABLE_TO_PROCESS), "engine wiring pending");
}

ndk::ScopedAStatus CancellationSignal::cancel() {
    ALOGI("cancellation signalled");
    Engine::get().load();
    if (Engine::get().fnCancel) Engine::get().fnCancel();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::generateChallenge() {
    ALOGI("generateChallenge sensor=%d user=%d", mSensorId, mUserId);
    // TODO: AncGenerateChallenge + onChallengeGenerated.
    return mCb->onChallengeGenerated(0);
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    ALOGI("revokeChallenge %lld", static_cast<long long>(challenge));
    return mCb->onChallengeRevoked(challenge);
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat,
                                   std::shared_ptr<ICancellationSignal>* out) {
    ALOGI("enroll hat.challenge=%lld", static_cast<long long>(hat.challenge));
    armFod();
    setFingerprintStatus(2);
    *out = SharedRefBase::make<CancellationSignal>();
    return notReady();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    ALOGI("authenticate op=%lld", static_cast<long long>(operationId));
    armFod();
    setFingerprintStatus(2);
    *out = SharedRefBase::make<CancellationSignal>();
    return notReady();
}

ndk::ScopedAStatus Session::detectInteraction(
        std::shared_ptr<ICancellationSignal>* out) {
    ALOGI("detectInteraction");
    *out = SharedRefBase::make<CancellationSignal>();
    return notReady();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    ALOGI("enumerateEnrollments");
    // Empty template list until engine enumeration is wired.
    return mCb->onEnrollmentsEnumerated({});
}

ndk::ScopedAStatus Session::removeEnrollments(
        const std::vector<int32_t>& enrollmentIds) {
    ALOGI("removeEnrollments n=%zu", enrollmentIds.size());
    return mCb->onEnrollmentsRemoved(enrollmentIds);
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    ALOGI("getAuthenticatorId");
    return mCb->onAuthenticatorIdRetrieved(0);
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    ALOGI("invalidateAuthenticatorId");
    return mCb->onAuthenticatorIdInvalidated(0);
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& hat) {
    (void)hat;
    ALOGI("resetLockout");
    return mCb->onLockoutCleared();
}

ndk::ScopedAStatus Session::close() {
    ALOGI("close");
    disarmFod();
    setFingerprintStatus(0);
    return mCb->onSessionClosed();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t pointerId, int32_t x,
                                          int32_t y, float minor, float major) {
    (void)minor;
    (void)major;
    ALOGV("onPointerDown id=%d (%d,%d)", pointerId, x, y);
    // Optical sensor: framework reports a finger on the FOD circle. This is
    // where illumination is driven; while the engine is unwired we exercise
    // the full glue chain for live testing.
    illuminateForCapture();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t pointerId) {
    ALOGV("onPointerUp id=%d", pointerId);
    endIllumination();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const OperationContext& context,
        std::shared_ptr<ICancellationSignal>* out) {
    (void)context;
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const HardwareAuthToken& hat,
                                              const OperationContext& context,
                                              std::shared_ptr<ICancellationSignal>* out) {
    (void)context;
    return enroll(hat, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const OperationContext& context,
        std::shared_ptr<ICancellationSignal>* out) {
    (void)context;
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, static_cast<int32_t>(context.x),
                         static_cast<int32_t>(context.y), context.minor,
                         context.major);
}

ndk::ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::onContextChanged(const OperationContext& context) {
    (void)context;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& context) {
    (void)context;
    endIllumination();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool shouldIgnore) {
    ALOGI("setIgnoreDisplayTouches %d", shouldIgnore);
    return ndk::ScopedAStatus::ok();
}

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
