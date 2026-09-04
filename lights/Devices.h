/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/light/HwLightState.h>
#include <cstdint>
#include <string>

namespace aidl {
namespace android {
namespace hardware {
namespace light {

/**
 * 32-bit representation of a color (AARRGGBB).
 */
struct Color {
    Color();
    Color(uint32_t color);

    uint8_t red;
    uint8_t green;
    uint8_t blue;

    /**
     * Get whether or not the color would produce any light.
     */
    bool isLit() const;

    /**
     * Convert the color to a single brightness value.
     */
    uint8_t toBrightness() const;
};

/**
 * Light effect.
 */
enum class EffectType {
    /**
     * Fixed color.
     */
    FIXED,

    /**
     * Timed blinking.
     */
    TIMED,

    /**
     * Hardware specific effect.
     */
    HARDWARE,
};

/**
 * Device state.
 */
struct LedState {
    Color color;
    EffectType effect = EffectType::FIXED;
    uint32_t onMs = 0;
    uint32_t offMs = 0;

    /**
     * Return whether or not the light should be considered "on".
     */
    bool isLit() const;
};

LedState fromAidl(const HwLightState& value);

/**
 * A Linux LED device used as backlight for this device
 * (/sys/class/leds/lcd-backlight).
 */
class BacklightDevice {
  public:
    BacklightDevice();

    bool isOk() const;
    bool setState(const LedState& state);
    void dump(int fd) const;

  private:
    std::string mName;
    std::string mBasePath;
    uint32_t mMaxBrightness;
};

/**
 * The Transsion TranLED notification LED exposed through the kernel's
 * tran-led-core driver (/sys/.../odm:tran_led_core/tran_led_cmd).
 */
class TranLedDevice {
  public:
    TranLedDevice();

    bool isOk() const;
    bool setState(const LedState& state);
    void dump(int fd) const;

  private:
    std::string mCommandPath;
};

/**
 * All devices exposed by this HAL.
 */
class Devices {
  public:
    Devices();

    void dump(int fd) const;

    bool hasBacklightDevice() const;
    bool hasTranLedDevice() const;

    void setBacklightState(const LedState& state);
    void setTranLedState(const LedState& state);

  private:
    BacklightDevice mBacklightDevice;
    TranLedDevice mTranLedDevice;
};

}  // namespace light
}  // namespace hardware
}  // namespace android
}  // namespace aidl