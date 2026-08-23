// android.hardware.biometrics.fingerprint-service.jiiov
// In-tree AIDL fingerprint HAL for TECNO CM6 (JV0307 optical UDFPS).
//
// Hosts the prebuilt Transsion engine (anc.hal.so, loaded via dlopen) and
// supplies the glue the stock stack expected from kernel/framework:
// FOD arming via sysfs/proc nodes and the synthetic TRAN_FULL_HBM_EVENT
// uevent that satisfies the engine's HbmEventDetect wait.
#include "Fingerprint.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::biometrics::fingerprint::jiiov::Fingerprint;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    ABinderProcess_startThreadPool();

    std::shared_ptr<Fingerprint> hal = ndk::SharedRefBase::make<Fingerprint>();

    const char* instance =
            "android.hardware.biometrics.fingerprint.IFingerprint/default";
    binder_status_t status =
            AServiceManager_addService(hal->asBinder().get(), instance);
    if (status != STATUS_OK) {
        LOG(ERROR) << "failed to register " << instance << " status=" << status;
        return 1;
    }
    LOG(INFO) << instance << " registered";

    ABinderProcess_joinThreadPool();
    return 0;
}
