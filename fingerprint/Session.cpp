/*
 * Copyright (C) 2026 The Project Infinity-X
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Session.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

using ::aidl::android::hardware::keymaster::HardwareAuthenticatorType;
using ::aidl::android::hardware::keymaster::HardwareAuthToken;

static std::mutex gCbMutex;
static std::weak_ptr<ISessionCallback> gSessionCb;
static std::weak_ptr<Session> gActiveSession;

void setActiveSession(const std::shared_ptr<Session>& s) {
    std::lock_guard<std::mutex> l(gCbMutex);
    gActiveSession = s;
}

void engineNotifyThunk(uint32_t type, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> l(gCbMutex);
        s = gActiveSession.lock();
    }
    if (s) s->onEngineEvent(type, a1, a2, a3, a4);
}

static std::shared_ptr<ISessionCallback> activeCallback() {
    std::lock_guard<std::mutex> l(gCbMutex);
    return gSessionCb.lock();
}

static HardwareAuthToken parseHat(const uint8_t* raw) {
    HardwareAuthToken hat{};
    if (raw == nullptr) return hat;
    uint32_t version = 0, type = 0, timestamp = 0;
    memcpy(&version, raw, 4);
    memcpy(&hat.challenge, raw + 4, 8);
    memcpy(&hat.userId, raw + 12, 8);
    memcpy(&hat.authenticatorId, raw + 20, 8);
    memcpy(&type, raw + 28, 4);
    memcpy(&timestamp, raw + 32, 4);
    hat.authenticatorType = static_cast<HardwareAuthenticatorType>(type);
    hat.timestamp.milliSeconds = timestamp;
    hat.mac.resize(32);
    memcpy(hat.mac.data(), raw + 36, 32);
    ALOGD_ENG("hat ver=%u chal=%lld uid=%lld aid=%llu type=%u ts=%u", version,
              (long long)static_cast<int64_t>(hat.challenge),
              (long long)static_cast<int64_t>(hat.userId),
              (unsigned long long)static_cast<uint64_t>(hat.authenticatorId), type,
              timestamp);
    return hat;
}

static void serializeHat(const HardwareAuthToken& hat, uint8_t out[69]) {
    memset(out, 0, 69);
    uint32_t version = 0;
    uint64_t challenge = static_cast<uint64_t>(hat.challenge);
    uint64_t user_id = static_cast<uint64_t>(hat.userId);
    uint64_t authenticator_id = static_cast<uint64_t>(hat.authenticatorId);
    uint32_t type = static_cast<uint32_t>(hat.authenticatorType);
    uint32_t timestamp =
            static_cast<uint32_t>(hat.timestamp.milliSeconds & 0xffffffffu);
    uint8_t hmac[32];
    memset(hmac, 0, sizeof(hmac));
    size_t n = hat.mac.size() < 32 ? hat.mac.size() : 32;
    if (n > 0) memcpy(hmac, hat.mac.data(), n);

    memcpy(out + 0, &version, 4);
    memcpy(out + 4, &challenge, 8);
    memcpy(out + 12, &user_id, 8);
    memcpy(out + 20, &authenticator_id, 8);
    memcpy(out + 28, &type, 4);
    memcpy(out + 32, &timestamp, 4);
    memcpy(out + 36, hmac, 32);
}

// Legacy fingerprint_acquired_info_t -> AIDL AcquiredInfo. Mapping recovered
// from the stock wrapper's notify dispatcher: GOOD->1, PARTIAL->2,
// INSUFFICIENT->3, IMAGER_DIRTY->4, TOO_SLOW->5, TOO_FAST->6, values
// >=1000 are vendor codes passed through as VENDOR + code-1000.
static void emitAcquired(uint64_t legacy) {
    auto cb = activeCallback();
    if (!cb) return;
    int32_t info = static_cast<int32_t>(legacy);
    ALOGD_ENG("acquired legacy=%d", info);
    if (info >= 1000) {
        cb->onAcquired(AcquiredInfo::VENDOR, info - 1000);
        return;
    }
    if (info > 5) {
        ALOGW_ENG("unmapped acquired %d", info);
        cb->onAcquired(AcquiredInfo::VENDOR, info);
        return;
    }
    cb->onAcquired(static_cast<AcquiredInfo>(info + 1), 0);
}

static void emitError(int32_t legacyError) {
    auto cb = activeCallback();
    if (!cb) return;
    Error e = Error::UNABLE_TO_PROCESS;
    switch (legacyError) {
        case 1:
            e = Error::HW_UNAVAILABLE;
            break;
        case 2:
            e = Error::UNABLE_TO_PROCESS;
            break;
        case 3:
            e = Error::TIMEOUT;
            break;
        case 4:
            e = Error::NO_SPACE;
            break;
        case 5:
            e = Error::CANCELED;
            break;
        case 7:
        case 9:
            e = Error::VENDOR;
            break;
        default:
            break;
    }
    ALOGW_ENG("error legacy=%d aidl=%d", legacyError, static_cast<int32_t>(e));
    cb->onError(e, 0);
}

ndk::ScopedAStatus CancellationSignal::cancel() {
    ALOGI_ENG("cancellation requested");
    Engine::get().cancel();
    emitError(5);
    return ndk::ScopedAStatus::ok();
}

Session::Session(int sensorId, int userId)
    : mSensorId(sensorId), mUserId(userId) {}

void Session::setCallback(const std::shared_ptr<ISessionCallback>& cb) {
    mCb = cb;
    std::lock_guard<std::mutex> l(gCbMutex);
    gSessionCb = cb;
}

void Session::onEngineEvent(uint32_t type, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4) {
    switch (type) {
        case 7:
            ALOGI_ENG("challenge generated %llu", (unsigned long long)a1);
            if (mCb) mCb->onChallengeGenerated(static_cast<int64_t>(a1));
            break;
        case 3:
            ALOGI_ENG("enroll fid=%u remaining=%u", (uint32_t)a1, (uint32_t)a3);
            if (mCb) mCb->onEnrollmentProgress(static_cast<int32_t>(a1), static_cast<int32_t>(a3));
            break;
        case 1:
            emitAcquired(a1);
            break;
        case 5: {
            const uint8_t* hatRaw = (a4 == 69 && a3 != 0) ? reinterpret_cast<const uint8_t*>(a3)
                                                          : nullptr;
            HardwareAuthToken hat = parseHat(hatRaw);
            ALOGI_ENG("authenticated fid=%u", (uint32_t)a1);
            if (mCb) mCb->onAuthenticationSucceeded(static_cast<int32_t>(a1), hat);
            break;
        }
        case 4:
        case 6: {
            auto* ids = reinterpret_cast<const int32_t*>(a1);
            uint32_t count = static_cast<uint32_t>(a2);
            std::vector<int32_t> list;
            if (ids != nullptr && count > 0 && count < 1024) {
                list.assign(ids, ids + count);
            }
            if (type == 4) {
                ALOGI_ENG("removed %zu ids", list.size());
                if (mCb) mCb->onEnrollmentsRemoved(list);
            } else {
                ALOGI_ENG("enumerated %zu ids", list.size());
                if (mCb) mCb->onEnrollmentsEnumerated(list);
            }
            break;
        }
        case 8:
        case 9:
        case 10:
            ALOGD_ENG("vendor event type=%u val=%llu", type, (unsigned long long)a1);
            break;
        default:
            ALOGD_ENG("event type=%u a1=%llu a2=%llu", type, (unsigned long long)a1,
                      (unsigned long long)a2);
            break;
    }
}

ndk::ScopedAStatus Session::generateChallenge() {
    ALOGI_ENG("generateChallenge sensor=%d user=%d", mSensorId, mUserId);
    Engine::get().generateChallenge();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    ALOGI_ENG("revokeChallenge %lld", (long long)challenge);
    Engine::get().revokeChallenge(static_cast<uint64_t>(challenge));
    return mCb ? mCb->onChallengeRevoked(challenge) : ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat,
                                   std::shared_ptr<ICancellationSignal>* out) {
    ALOGI_ENG("enroll user=%d challenge=%lld", mUserId,
              (long long)static_cast<int64_t>(hat.challenge));
    if (!Engine::get().ready()) {
        if (mCb) mCb->onError(Error::HW_UNAVAILABLE, 0);
        return ndk::ScopedAStatus::ok();
    }
    uint8_t raw[69];
    serializeHat(hat, raw);
    Engine::get().enroll(raw);
    *out = ndk::SharedRefBase::make<CancellationSignal>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    ALOGI_ENG("authenticate op=%lld", (long long)operationId);
    if (!Engine::get().ready()) {
        if (mCb) mCb->onError(Error::HW_UNAVAILABLE, 0);
        return ndk::ScopedAStatus::ok();
    }
    armFod();
    Engine::get().authenticate(static_cast<uint64_t>(operationId));
    *out = ndk::SharedRefBase::make<CancellationSignal>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    ALOGI_ENG("detectInteraction");
    if (!Engine::get().ready()) {
        if (mCb) mCb->onError(Error::HW_UNAVAILABLE, 0);
        return ndk::ScopedAStatus::ok();
    }
    armFod();
    Engine::get().authenticate(0);
    *out = ndk::SharedRefBase::make<CancellationSignal>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    ALOGI_ENG("enumerateEnrollments");
    Engine::get().enumerate();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    for (int32_t id : enrollmentIds) {
        ALOGI_ENG("removeEnrollment %d", id);
        Engine::get().remove(id);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    uint64_t id = Engine::get().authenticatorId();
    ALOGI_ENG("getAuthenticatorId -> %llu", (unsigned long long)id);
    return mCb ? mCb->onAuthenticatorIdRetrieved(static_cast<int64_t>(id))
               : ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    Engine::get().invalidateAuthenticatorId();
    uint64_t id = Engine::get().authenticatorId();
    ALOGI_ENG("invalidateAuthenticatorId -> new=%llu", (unsigned long long)id);
    return mCb ? mCb->onAuthenticatorIdInvalidated(static_cast<int64_t>(id))
               : ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& hat) {
    (void)hat;
    ALOGI_ENG("resetLockout");
    return mCb ? mCb->onLockoutCleared() : ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    ALOGD_ENG("session closed");
    return mCb ? mCb->onSessionClosed() : ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t pointerId, int32_t x, int32_t y,
                                          float minor, float major) {
    ALOGV_ENG("onPointerDown id=%d (%d,%d)", pointerId, x, y);
    armFod(x, y);
    uint32_t mb = 0, Mb = 0;
    memcpy(&mb, &minor, 4);
    memcpy(&Mb, &major, 4);
    Engine::get().pointerDown(pointerId, x, y, mb, Mb);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t pointerId) {
    ALOGV_ENG("onPointerUp id=%d", pointerId);
    Engine::get().pointerUp(pointerId, 0, 0, 0, 0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    ALOGV_ENG("onUiReady");
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
        const OperationContext& context, std::shared_ptr<ICancellationSignal>* out) {
    (void)context;
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor,
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
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool shouldIgnore) {
    (void)shouldIgnore;
    return ndk::ScopedAStatus::ok();
}

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
