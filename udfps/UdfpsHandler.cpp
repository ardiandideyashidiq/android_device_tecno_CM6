/*
 * Copyright (C) 2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cutils/properties.h>
#include <log/log.h>
#include <fstream>
#include <string>

#include "UdfpsHandler.h"

#define LCM_HBM_STATE "/sys/kernel/tran_display/lcm_hbm_state"
#define LCM_DIMMING_STATE "/sys/kernel/tran_display/lcm_dimming_state"

class TranssionJiivoUdfpsHandler : public UdfpsHandler {
  public:
    void init(fingerprint_device_t* device) override {
        mDevice = device;
    }

    void onFingerDown(uint32_t x, uint32_t y, float minor, float major) override {
        setHbm(true);
    }

    void onFingerUp() override {
        setHbm(false);
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

    void setHbm(bool on) {
        std::ofstream hbm(LCM_HBM_STATE);
        if (hbm) {
            hbm << (on ? "1" : "0");
        }

        std::ofstream dim(LCM_DIMMING_STATE);
        if (dim) {
            dim << (on ? "1" : "0");
        }
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
