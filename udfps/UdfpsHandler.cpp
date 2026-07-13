/*
 * Copyright (C) 2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cutils/properties.h>
#include <log/log.h>
#include <fstream>
#include <string>
#include <unistd.h>

#include "UdfpsHandler.h"

#define LCM_HBM_STATE "/sys/kernel/tran_display/lcm_hbm_state"
#define LCM_DIMMING_STATE "/sys/kernel/tran_display/lcm_dimming_state"

// Extension vtable at offset 0xd0 (reserved[2]) = AncExcuteCommand
typedef int (*ext_cmd_fn_t)(fingerprint_device_t*, uint32_t cmd, uint32_t extra);

// Extension command IDs from anc.hal.so decompilation:
#define EXT_CMD_TOUCH_DOWN       0x0d  // EmUpdateEventState(device, 0, 1)
#define EXT_CMD_TOUCH_UP         0x0e  // EmUpdateEventState(device, 0, 0)
#define EXT_CMD_TRAN_UI          0x0a  // Enable/disable touch detect

class TranssionJiivoUdfpsHandler : public UdfpsHandler {
  public:
    void init(fingerprint_device_t* device) override {
        mDevice = device;
        if (mDevice) {
            mExtCmd = reinterpret_cast<ext_cmd_fn_t>(mDevice->reserved[2]);
        }
    }

    void onFingerDown(uint32_t x, uint32_t y, float minor, float major) override {
        setHbm(true);
        if (mExtCmd) {
            mExtCmd(mDevice, EXT_CMD_TRAN_UI, 1);
            mExtCmd(mDevice, EXT_CMD_TOUCH_DOWN, 0);
        }
        usleep(20000);
    }

    void onFingerUp() override {
        setHbm(false);
        if (mExtCmd) {
            mExtCmd(mDevice, EXT_CMD_TOUCH_UP, 0);
            mExtCmd(mDevice, EXT_CMD_TRAN_UI, 0);
        }
    }

    void onAcquired(int32_t result, int32_t vendorCode) override {
        if (vendorCode == 21) {
            setHbm(true);
        }
    }

    void onAuthenticationSucceeded() override {
        setHbm(false);
    }

    void onAuthenticationFailed() override {
        setHbm(false);
    }

    void cancel() override {
        setHbm(false);
    }

  private:
    fingerprint_device_t* mDevice = nullptr;
    ext_cmd_fn_t mExtCmd = nullptr;

    void setHbm(bool on) {
        std::ofstream hbm(LCM_HBM_STATE);
        if (hbm) hbm << (on ? "1" : "0");
        std::ofstream dim(LCM_DIMMING_STATE);
        if (dim) dim << (on ? "1" : "0");
    }
};

static UdfpsHandler* create() {
    return new TranssionJiivoUdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};
