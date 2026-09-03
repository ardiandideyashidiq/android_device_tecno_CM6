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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <thread>

#include <cutils/properties.h>
#include <endian.h>

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
// True while an enroll/authenticate/detectInteraction op is in flight.
// Gates panel illumination so idle touches never light up the display.
static std::atomic<bool> gOpActive{false};
static std::atomic<bool> gIlluminating{false};
// Set when `setprop vendor.fp.fpctl 3` requests a purge of orphaned
// TEE-side templates: next enumerated-list event removes every id.
static std::atomic<bool> gRemoveAllPending{false};
// True while the current op was started by detectInteraction(); needed
// because both detect and authenticate drive the same engine op and the
// engine signals both outcomes through one event (see onEngineEvent 5).
static std::atomic<bool> gDetectActive{false};

// Legacy vendor codes (see emitAcquired): 1002 = finger down, 1003 = up.
// The optical sensor can only capture under full-panel illumination, and the
// engine's PressEnroll/PressAuth stages block until the TRAN_FULL_HBM_EVENT
// uevent arrives (decompiled gate: strncmp "TRAN_FULL_HBM_EVENT=" +
// strcmp "FULL_HBM_SET" in InitHbmEventDetectWorker @0x3c430).
static void startCaptureLight() {
    if (gIlluminating.exchange(true)) return;
    ALOGI_ENG("finger down -> illuminating for capture");
    illuminateForCapture();
}

static void stopCaptureLight() {
    if (!gIlluminating.exchange(false)) return;
    ALOGI_ENG("finger up/op end -> ending illumination");
    endIllumination();
}

// Defer template deletion off the caller: Fpm* ops hold the manager mutex
// across their whole run (and across notify callbacks), so calling
// FpmRemove synchronously from an engine-event context self-deadlocks.
static void scheduleRemove(std::vector<int32_t> ids) {
    std::thread([ids = std::move(ids)] {
        for (int32_t id : ids) {
            ALOGI_ENG("deferred remove id=%d", id);
            Engine::get().remove(id);
            usleep(50000);
        }
    }).detach();
}

void setActiveSession(const std::shared_ptr<Session>& s) {
    std::lock_guard<std::mutex> l(gCbMutex);
    gActiveSession = s;
}

// Debug/control hook: `setprop vendor.fp.fpctl 3` purges orphaned TEE
// templates (enumerate -> remove all). Started once with the first session.
static void startCtlWatcher() {
    static bool started = [] {
        static std::thread t([] {
            char buf[PROP_VALUE_MAX] = {0};
            while (true) {
                const prop_info* pi = __system_property_find("vendor.fp.fpctl");
                if (pi != nullptr) {
                    __system_property_read_callback(
                            pi,
                            [](void* cookie, const char*, const char* value,
                               uint32_t) {
                                strncpy(static_cast<char*>(cookie), value,
                                        PROP_VALUE_MAX - 1);
                            },
                            buf);
                    if (buf[0] == '3') {
                        ALOGI_ENG("fpctl: purge requested");
                        if (gOpActive.exchange(false)) Engine::get().cancel();
                        stopCaptureLight();
                        gRemoveAllPending = true;
                        Engine::get().enumerate();
                        property_set("vendor.fp.fpctl", "0");
                        buf[0] = 0;
                    }
                }
                usleep(120000);
            }
        });
        t.detach();
        return true;
    }();
    (void)started;
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
    uint64_t type = 0, millis = 0;
    memcpy(&hat.challenge, raw + 1, 8);
    memcpy(&hat.userId, raw + 9, 8);
    memcpy(&hat.authenticatorId, raw + 17, 8);
    memcpy(&type, raw + 25, 4);
    memcpy(&millis, raw + 29, 8);
    hat.authenticatorType = static_cast<HardwareAuthenticatorType>(be32toh(type));
    hat.timestamp.milliSeconds = static_cast<int64_t>(be64toh(millis));
    hat.mac.resize(32);
    memcpy(hat.mac.data(), raw + 37, 32);
    ALOGD_ENG("hat chal=%lld uid=%lld aid=%llu type=%u ts=%lld",
              (long long)static_cast<int64_t>(hat.challenge),
              (long long)static_cast<int64_t>(hat.userId),
              (unsigned long long)static_cast<uint64_t>(hat.authenticatorId),
              be32toh(type), (long long)static_cast<int64_t>(be64toh(millis)));
    return hat;
}

// Legacy 69-byte token layout as marshaled by the stock wrapper
// (@0x91a0): u8 version=0 | challenge | userId | authenticatorId |
// authenticatorType (BE32) | timestamp (BE64) | hmac[32].
static void serializeHat(const HardwareAuthToken& hat, uint8_t out[69]) {
    memset(out, 0, 69);
    uint64_t challenge = static_cast<uint64_t>(hat.challenge);
    uint64_t user_id = static_cast<uint64_t>(hat.userId);
    uint64_t authenticator_id = static_cast<uint64_t>(hat.authenticatorId);
    uint32_t type =
            htobe32(static_cast<uint32_t>(hat.authenticatorType));
    uint64_t timestamp =
            htobe64(static_cast<uint64_t>(hat.timestamp.milliSeconds));
    uint8_t hmac[32];
    memset(hmac, 0, sizeof(hmac));
    size_t n = hat.mac.size() < 32 ? hat.mac.size() : 32;
    if (n > 0) memcpy(hmac, hat.mac.data(), n);

    out[0] = 0;
    memcpy(out + 1, &challenge, 8);
    memcpy(out + 9, &user_id, 8);
    memcpy(out + 17, &authenticator_id, 8);
    memcpy(out + 25, &type, 4);
    memcpy(out + 29, &timestamp, 8);
    memcpy(out + 37, hmac, 32);
    ALOGD_ENG("enroll hat: mac_len=%zu chal=%lld", hat.mac.size(),
              (long long)static_cast<int64_t>(hat.challenge));
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
    if (info == 1002) {
        // Finger down: notify the framework FIRST so it can move the OS
        // display into DOZE (physically illuminating the AMOLED panel) before
        // capture begins. The engine PressAuth stage then still blocks on the
        // HBM_SET uevent fired by startCaptureLight() below, giving the panel
        // time to light. This keeps the panel OFF while idle and lights it
        // only for the capture.
        if (gOpActive.load()) {
            cb->onAcquired(AcquiredInfo::VENDOR, info - 1000);
            startCaptureLight();
        }
        return;
    } else if (info == 1003) {
        stopCaptureLight();
    }
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
    gOpActive = false;
    gDetectActive = false;
    stopCaptureLight();
    cb->onError(e, 0);
}

ndk::ScopedAStatus CancellationSignal::cancel() {
    ALOGI_ENG("cancellation requested");
    gOpActive = false;
    gDetectActive = false;
    Engine::get().cancel();
    stopCaptureLight();
    emitError(5);
    return ndk::ScopedAStatus::ok();
}

Session::Session(int sensorId, int userId)
    : mSensorId(sensorId), mUserId(userId) {}

void Session::setCallback(const std::shared_ptr<ISessionCallback>& cb) {
    mCb = cb;
    std::lock_guard<std::mutex> l(gCbMutex);
    gSessionCb = cb;
    startCtlWatcher();
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
            if (a3 == 0) {
                gOpActive = false;
                stopCaptureLight();
            }
            if (mCb) mCb->onEnrollmentProgress(static_cast<int32_t>(a1), static_cast<int32_t>(a3));
            break;
        case 1:
            emitAcquired(a1);
            break;
        case 5: {
            // The engine emits this event for every completed capture:
            // fid != 0 means a genuine match (with the 69-byte HAT in
            // a3/a4), fid == 0 means either detect-interaction completion
            // or a non-match. The framework trusts onAuthenticationSucceeded
            // unconditionally (AidlResponseHandler passes authenticated=true
            // regardless of the id), so a blind forward here unlocks the
            // device for any finger. Non-matches must go through
            // onAuthenticationFailed() instead.
            const uint32_t fid = static_cast<uint32_t>(a1);
            ALOGI_ENG("auth result fid=%u", fid);
            if (!mCb) break;
            if (fid != 0) {
                gOpActive = false;
                stopCaptureLight();
                const uint8_t* hatRaw =
                        (a4 == 69 && a3 != 0) ? reinterpret_cast<const uint8_t*>(a3)
                                              : nullptr;
                mCb->onAuthenticationSucceeded(static_cast<int32_t>(fid),
                                               parseHat(hatRaw));
            } else if (gDetectActive.exchange(false)) {
                gOpActive = false;
                stopCaptureLight();
                mCb->onAuthenticationSucceeded(0, HardwareAuthToken{});
            } else if (gOpActive.load()) {
                // Non-match inside a live authenticate(): the framework
                // retries further touches within the SAME operation, so
                // gOpActive/illumination must stay armed or every capture
                // after the first failure goes dark.
                mCb->onAuthenticationFailed();
            }
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
            if (type == 6 && gRemoveAllPending.exchange(false)) {
                std::vector<int32_t> doomed;
                for (int32_t id : list) {
                    if (id != 0) doomed.push_back(id);
                }
                if (doomed.empty()) {
                    ALOGI_ENG("fpctl purge: nothing to remove");
                } else {
                    ALOGI_ENG("fpctl purge: scheduling removal of %zu ids",
                              doomed.size());
                    scheduleRemove(doomed);
                }
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
    gDetectActive = false;
    gOpActive = true;
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
    gDetectActive = false;
    gOpActive = true;
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
    gDetectActive = true;
    gOpActive = true;
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
        scheduleRemove({id});
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
