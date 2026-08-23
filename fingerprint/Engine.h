// Engine binding layer: dlopen(anc.hal.so) and resolve the vendor C API.
//
// Signatures marked PROVISIONAL were inferred from the exported symbol list;
// each must be confirmed against anc.hal.so disassembly before the call site
// is enabled. Unverified functions are resolved but never invoked.
#pragma once

#include <dlfcn.h>
#include <utils/Log.h>

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace jiiov {

class Engine {
  public:
    static Engine& get() {
        static Engine inst;
        return inst;
    }

    // Resolves symbols; safe to call repeatedly. Returns false if any core
    // symbol is missing (engine unusable).
    bool load() {
        if (mHandle) return mCoreComplete;
        mHandle = dlopen("anc.hal.so", RTLD_NOW);
        if (!mHandle) {
            ALOGE("dlopen anc.hal.so failed: %s", dlerror());
            return false;
        }
#define SYM(var, name)                                    \
    do {                                                  \
        var = reinterpret_cast<decltype(var)>(            \
                dlsym(mHandle, name));                    \
        if (!(var))                                       \
            ALOGE("missing symbol %s", name);             \
    } while (0)

        // Core session operations.
        SYM(fnSetNotifyCallback, "AncSetNotifyCallback");
        SYM(fnEnroll, "AncEnroll");
        SYM(fnAuthenticate, "AncAuthenticate");
        SYM(fnDetectInteraction, "AncDetectInteraction");
        SYM(fnCancel, "AncCancel");
        SYM(fnEnumerate, "AncEnumerate");
        SYM(fnRemove, "AncRemove");
        SYM(fnSetActiveGroup, "AncSetActiveGroup");
        SYM(fnGenerateChallenge, "AncGenerateChallenge");
        SYM(fnRevokeChallenge, "AncRevokeChallenge");
        SYM(fnGetAuthenticatorId, "AncGetAuthenticatorId");
        SYM(fnInvalidateAuthenticatorId, "AncInvalidateAuthenticatorId");
        SYM(fnResetLockout, "AncResetLockout");

        // Udfps / extension surface (resolved for later phases).
        SYM(fnOnPointerDown, "AncOnPointerDown");
        SYM(fnOnPointerUp, "AncOnPointerUp");
        SYM(fnOnUiReady, "AncOnUiReady");
        SYM(fnExcuteCommand, "AncExcuteCommand");
#undef SYM

        mCoreComplete = fnSetNotifyCallback && fnEnroll && fnAuthenticate &&
                        fnCancel && fnGetAuthenticatorId;
        ALOGI("engine load complete=%d", mCoreComplete);
        return mCoreComplete;
    }

    void* handle() const { return mHandle; }

    // --- PROVISIONAL typedefs: verify against disassembly before use. ---
    using FnSetNotifyCallback = int (*)(void* callback);
    using FnCancel = int (*)();

    FnSetNotifyCallback fnSetNotifyCallback = nullptr;
    FnCancel fnCancel = nullptr;

    // Resolved but signature-unverified; do not call until validated.
    void *fnEnroll = nullptr;
    void *fnAuthenticate = nullptr;
    void *fnDetectInteraction = nullptr;
    void *fnEnumerate = nullptr;
    void *fnRemove = nullptr;
    void *fnSetActiveGroup = nullptr;
    void *fnGenerateChallenge = nullptr;
    void *fnRevokeChallenge = nullptr;
    void *fnGetAuthenticatorId = nullptr;
    void *fnInvalidateAuthenticatorId = nullptr;
    void *fnResetLockout = nullptr;
    void *fnOnPointerDown = nullptr;
    void *fnOnPointerUp = nullptr;
    void *fnOnUiReady = nullptr;
    void *fnExcuteCommand = nullptr;

  private:
    void* mHandle = nullptr;
    bool mCoreComplete = false;
};

}  // namespace jiiov
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
}  // namespace aidl
