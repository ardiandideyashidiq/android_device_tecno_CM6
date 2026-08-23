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
#pragma once

#include <android/log.h>
#include <dlfcn.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

#define ENG_LOG_TAG "fingerprint.jiiov"
#define ALOGV_ENG(...) \
    __android_log_print(ANDROID_LOG_VERBOSE, ENG_LOG_TAG, __VA_ARGS__)
#define ALOGD_ENG(...) __android_log_print(ANDROID_LOG_DEBUG, ENG_LOG_TAG, __VA_ARGS__)
#define ALOGI_ENG(...) __android_log_print(ANDROID_LOG_INFO, ENG_LOG_TAG, __VA_ARGS__)
#define ALOGW_ENG(...) __android_log_print(ANDROID_LOG_WARN, ENG_LOG_TAG, __VA_ARGS__)
#define ALOGE_ENG(...) __android_log_print(ANDROID_LOG_ERROR, ENG_LOG_TAG, __VA_ARGS__)

// ABI derived by disassembling odm/lib64/hw/jiiov.fingerprint.default.so
// (adapter) and odm/lib64/anc.hal.so (engine):
//
//  - GetFingerprintDevice() returns a process-wide singleton handle.
//  - InitFingerprintDevice(handle) -> InitFingerprintManager(handle); 0 == ok.
//  - AncSetNotifyCallback(handle, table) stores the table at *(void**)handle
//    (see AncSetNotifyCallback @0x27100: str x1, [x0]).
//  - The table holds 10 host callbacks. The engine invokes slot N with event
//    fields starting in w1/x1; the adapter wrappers (0x42d0..0x47b8) build an
//    80-byte legacy fingerprint_msg_t (type @+0, data @+8) from them:
//      slot 0 -> type 7   : challenge generated, u64 @x1
//             1 -> type 8   : vendor event, u32 @w1
//             2 -> type 9   : vendor event, u32 @w1
//             3 -> type 10  : vendor event, u32 @w1
//             4 -> type 3   : enrolling, finger_id @w1, samples_remaining @w3
//             5 -> type 1   : acquired, info @w1 (legacy enum)
//             6 -> type 5   : authenticated, fid @w1, 69-byte HAT @x3 iff w4==69
//             7 -> type -1  : raw subtype, u32 @w1
//             8 -> type 4   : removed, ids[] @x1, count @w2
//             9 -> type 6   : enumerated, ids[] @x1, count @w2
//
class Engine {
  public:
    using Notify = std::function<void(uint32_t type, uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4)>;

    static Engine& get() {
        static Engine inst;
        return inst;
    }

    bool init() {
        std::lock_guard<std::mutex> l(mApiMutex);
        if (mInited) return true;

        mLib = dlopen("/odm/lib64/anc.hal.so", RTLD_NOW);
        if (mLib == nullptr) mLib = dlopen("anc.hal.so", RTLD_NOW);
        if (mLib == nullptr) {
            ALOGE_ENG("dlopen anc.hal.so failed: %s", dlerror());
            return false;
        }

        auto getDevice = reinterpret_cast<void* (*)()>(dlsym(mLib, "GetFingerprintDevice"));
        auto initDevice =
                reinterpret_cast<int (*)(void*)>(dlsym(mLib, "InitFingerprintDevice"));
        auto setNotify =
                reinterpret_cast<void (*)(void*, void*)>(dlsym(mLib, "AncSetNotifyCallback"));
        if (getDevice == nullptr || initDevice == nullptr || setNotify == nullptr) {
            ALOGE_ENG("missing core symbols: %s", dlerror());
            return false;
        }

        mGenerateChallenge = reinterpret_cast<int (*)(void*)>(dlsym(mLib, "AncGenerateChallenge"));
        mRevokeChallenge =
                reinterpret_cast<int (*)(void*, uint64_t)>(dlsym(mLib, "AncRevokeChallenge"));
        mAuthenticatorId =
                reinterpret_cast<uint64_t (*)(void*)>(dlsym(mLib, "AncGetAuthenticatorId"));
        mInvalidateAuthId =
                reinterpret_cast<int (*)(void*)>(dlsym(mLib, "AncInvalidateAuthenticatorId"));
        mEnroll = reinterpret_cast<int (*)(void*, const uint8_t*)>(dlsym(mLib, "AncEnroll"));
        mAuthenticate =
                reinterpret_cast<int (*)(void*, uint64_t)>(dlsym(mLib, "AncAuthenticate"));
        mCancel = reinterpret_cast<int (*)(void*)>(dlsym(mLib, "AncCancel"));
        mEnumerate = reinterpret_cast<int (*)(void*)>(dlsym(mLib, "AncEnumerate"));
        mRemove = reinterpret_cast<int (*)(void*, int32_t)>(dlsym(mLib, "AncRemove"));
        mSetActiveGroup = reinterpret_cast<int (*)(void*, int32_t, const char*)>(
                dlsym(mLib, "AncSetActiveGroup"));
        mPointerDown = reinterpret_cast<int (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                                uintptr_t)>(
                dlsym(mLib, "AncOnPointerDown"));
        mPointerUp =
                reinterpret_cast<int (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                         uintptr_t)>(dlsym(mLib, "AncOnPointerUp"));

        mHandle = getDevice();
        if (mHandle == nullptr) {
            ALOGE_ENG("GetFingerprintDevice returned null");
            return false;
        }
        if (initDevice(mHandle) != 0) {
            ALOGE_ENG("InitFingerprintDevice failed");
            mHandle = nullptr;
            return false;
        }

        setNotify(mHandle, reinterpret_cast<void*>(const_cast<CbFn*>(kCallbacks)));
        mInited = true;
        ALOGI_ENG("engine initialized, handle=%p", mHandle);
        return true;
    }

    bool ready() const { return mInited; }

    void setNotify(Notify&& n) {
        std::lock_guard<std::mutex> l(mNotifyMutex);
        mNotify = std::move(n);
    }

    // All operations below require init() to have succeeded. They mirror the
    // adapter's marshaling exactly (arg1 = handle).
    void generateChallenge() { invoke([this](void* h) { return mGenerateChallenge(h); }); }
    void revokeChallenge(uint64_t ch) { invoke([&](void* h) { return mRevokeChallenge(h, ch); }); }
    uint64_t authenticatorId() { return mInited ? mAuthenticate ? mAuthenticatorId(mHandle) : 0 : 0; }
    void invalidateAuthenticatorId() { invoke([this](void* h) { return mInvalidateAuthId(h); }); }
    void enroll(const uint8_t hat[69]) { invoke([&](void* h) { return mEnroll(h, hat); }); }
    void authenticate(uint64_t operationId) {
        invoke([&](void* h) { return mAuthenticate(h, operationId); });
    }
    void cancel() { invoke([this](void* h) { return mCancel(h); }); }
    void enumerate() { invoke([this](void* h) { return mEnumerate(h); }); }
    void remove(int32_t fid) { invoke([&](void* h) { return mRemove(h, fid); }); }
    void setActiveGroup(int32_t gid, const char* path) {
        invoke([&](void* h) { return mSetActiveGroup(h, gid, path); });
    }
    void pointerDown(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
        invoke([&](void* h) { return mPointerDown(h, a1, a2, a3, a4, a5); });
    }
    void pointerUp(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
        invoke([&](void* h) { return mPointerUp(h, a1, a2, a3, a4, a5); });
    }

  private:
    template <typename F>
    void invoke(F&& f) {
        if (!mInited) {
            ALOGE_ENG("op skipped, engine not initialized");
            return;
        }
        f(mHandle);
    }

    static void onEvent(uint32_t type, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
        Engine& e = get();
        Notify n;
        {
            std::lock_guard<std::mutex> l(e.mNotifyMutex);
            n = e.mNotify;
        }
        if (n) n(type, a1, a2, a3, a4);
    }

#define ENGINE_CB(name, t, x1, x2, x3, x4)                        \
    static void name(uint64_t _a0, uint64_t a1, uint64_t a2,      \
                     uint64_t a3, uint64_t a4) {                  \
        (void)_a0; (void)a1; (void)a2; (void)a3; (void)a4;        \
        onEvent(t, x1, x2, x3, x4);                               \
    }

    ENGINE_CB(cbChallengeGenerated, 7, a1, 0, 0, 0)
    ENGINE_CB(cbVendor8, 8, a1, 0, 0, 0)
    ENGINE_CB(cbVendor9, 9, a1, 0, 0, 0)
    ENGINE_CB(cbVendor10, 10, a1, 0, 0, 0)
    ENGINE_CB(cbEnrollProgress, 3, a1, 0, a3, 0)
    ENGINE_CB(cbAcquired, 1, a1, 0, 0, 0)
    ENGINE_CB(cbAuthenticated, 5, a1, 0, a3, a4)
    ENGINE_CB(cbRawSubtype, 0xFFFFFFFFu, a1, 0, 0, 0)
    ENGINE_CB(cbRemoved, 4, a1, a2, 0, 0)
    ENGINE_CB(cbEnumerated, 6, a1, a2, 0, 0)

#undef ENGINE_CB

    using CbFn = void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    static constexpr CbFn kCallbacks[10] = {
            cbChallengeGenerated, cbVendor8,  cbVendor9,   cbVendor10, cbEnrollProgress,
            cbAcquired,           cbAuthenticated, cbRawSubtype, cbRemoved, cbEnumerated,
    };

    void* mLib = nullptr;
    void* mHandle = nullptr;
    bool mInited = false;
    std::mutex mApiMutex;
    std::mutex mNotifyMutex;
    Notify mNotify;

    int (*mGenerateChallenge)(void*) = nullptr;
    int (*mRevokeChallenge)(void*, uint64_t) = nullptr;
    uint64_t (*mAuthenticatorId)(void*) = nullptr;
    int (*mInvalidateAuthId)(void*) = nullptr;
    int (*mEnroll)(void*, const uint8_t*) = nullptr;
    int (*mAuthenticate)(void*, uint64_t) = nullptr;
    int (*mCancel)(void*) = nullptr;
    int (*mEnumerate)(void*) = nullptr;
    int (*mRemove)(void*, int32_t) = nullptr;
    int (*mSetActiveGroup)(void*, int32_t, const char*) = nullptr;
    int (*mPointerDown)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) = nullptr;
    int (*mPointerUp)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) = nullptr;
};

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
