/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Devices.h>

#define LOG_TAG "Devices"

#include <aidl/android/hardware/light/FlashMode.h>
#include <android-base/logging.h>
#include <cstdio>
#include <fstream>

namespace aidl {
namespace android {
namespace hardware {
namespace light {

namespace {

uint32_t scaleBrightness(uint8_t brightness, uint32_t maxBrightness) {
    return brightness * maxBrightness / 0xFF;
}

template <typename T>
bool writeToFile(const std::string& file, const T content) {
    std::ofstream fileStream(file);

    if (!fileStream) {
        return false;
    }

    fileStream << content;
    return true;
}

}  // namespace

Color::Color() : red(0), green(0), blue(0) {}

Color::Color(uint32_t color) {
    // Extract brightness from AARRGGBB.
    uint8_t alpha = (color >> 24) & 0xFF;

    // Retrieve each of the RGB colors
    red = (color >> 16) & 0xFF;
    green = (color >> 8) & 0xFF;
    blue = color & 0xFF;

    // Scale RGB colors if a brightness has been applied by the user
    if (alpha > 0 && alpha < 0xFF) {
        red = red * alpha / 0xFF;
        green = green * alpha / 0xFF;
        blue = blue * alpha / 0xFF;
    }
}

bool Color::isLit() const {
    return !!red || !!green || !!blue;
}

static constexpr uint8_t kRedWeight = 77;
static constexpr uint8_t kGreenWeight = 150;
static constexpr uint8_t kBlueWeight = 29;

uint8_t Color::toBrightness() const {
    return (kRedWeight * red + kGreenWeight * green + kBlueWeight * blue) >> 8;
}

bool LedState::isLit() const {
    return color.isLit();
}

LedState fromAidl(const HwLightState& value) {
    LedState state{
            .color = Color(value.color),
            .effect = EffectType::FIXED,
    };

    switch (value.flashMode) {
        case FlashMode::NONE:
            state.effect = EffectType::FIXED;
            break;
        case FlashMode::TIMED:
            state.effect = EffectType::TIMED;
            state.onMs = value.flashOnMs;
            state.offMs = value.flashOffMs;
            break;
        case FlashMode::HARDWARE:
            state.effect = EffectType::HARDWARE;
            break;
        default:
            LOG(ERROR) << "Unknown flash mode: " << static_cast<int>(value.flashMode);
            state.effect = EffectType::FIXED;
            break;
    }

    return state;
}

// The TranLED kernel driver (tran-led-core) is the same one Transsion uses on
// stock. It controls the red notification LED physically driven by the AWINIC
// AW2023 I2C LED driver. Command format:
//
//   <led_mode> <led_color> <brightness> <rise_time_ms> <hold_time_ms> <fall_time_ms>
//
// Modes verified on the live device:
//   0 : off
//   1 : static on (rise/hold/fall times unused)
//   2 : hard full-power blink; on-time = rise + hold, off-time = fall.
//       Verified: "2 1 255 0 1000 1000" blinks evenly ~1s on / ~1s off, and
//       "2 1 255 400 400 4000" shows a long dark gap (slower blink).
//   3 : smooth breathe/breath; rise/hold/fall all drive the fade.
//       Verified: "3 1 255 250 1500 250" breathes properly.
//   4/5/6 : other Transsion effect modes accepted by the driver.
//
// led_color=1 is the red channel.
static constexpr char kTranLedColorRed = 1;
static constexpr char kTranLedModeStatic = 1;
static constexpr char kTranLedModeTimed = 2;
static constexpr char kTranLedModeBreathe = 3;

BacklightDevice::BacklightDevice()
    : mName("lcd-backlight"), mBasePath("/sys/class/leds/" + mName + "/") {
    std::ifstream maxBrightnessStream(mBasePath + "max_brightness");
    if (maxBrightnessStream) {
        maxBrightnessStream >> mMaxBrightness;
    }
};

bool BacklightDevice::isOk() const {
    return std::ifstream(mBasePath + "brightness").good();
}

bool BacklightDevice::setState(const LedState& state) {
    return writeToFile(mBasePath + "brightness", scaleBrightness(state.color.toBrightness(), mMaxBrightness));
}

void BacklightDevice::dump(int fd) const {
    dprintf(fd, "Backlight: name: %s", mName.c_str());
    dprintf(fd, ", is ok: %d", isOk());
    dprintf(fd, ", base path: %s", mBasePath.c_str());
    dprintf(fd, ", max brightness: %u\n", mMaxBrightness);
}

TranLedDevice::TranLedDevice()
    : mCommandPath("/sys/devices/platform/odm/odm:tran_led_core/tran_led_cmd") {}

bool TranLedDevice::isOk() const {
    return std::ifstream(mCommandPath).good();
}

bool TranLedDevice::setState(const LedState& state) {
    if (!state.isLit()) {
        return writeToFile(mCommandPath, "0 0 0 0 0 0");
    }

    uint32_t brightness = state.color.toBrightness();
    if (brightness == 0) {
        brightness = 1;
    }

    std::string command;
    switch (state.effect) {
        case EffectType::FIXED:
            command = std::to_string(kTranLedModeStatic) + " " + std::to_string(kTranLedColorRed) +
                      " " + std::to_string(brightness) + " 0 0 0";
            break;
        case EffectType::TIMED: {
            // Hard blink: rise=0, on-time = hold (frame onMs... verified above),
            // off-time = fall. Guard against 0/0 which the driver treats poorly.
            uint32_t onMs = state.onMs > 0 ? state.onMs : 1;
            uint32_t offMs = state.offMs > 0 ? state.offMs : 1;
            command = std::to_string(kTranLedModeTimed) + " " + std::to_string(kTranLedColorRed) +
                      " " + std::to_string(brightness) + " 0 " + std::to_string(onMs) + " " +
                      std::to_string(offMs);
            break;
        }
        case EffectType::HARDWARE:
            command = std::to_string(kTranLedModeBreathe) + " " + std::to_string(kTranLedColorRed) +
                      " " + std::to_string(brightness) + " 250 1500 250";
            break;
    }

    return writeToFile(mCommandPath, command);
}

void TranLedDevice::dump(int fd) const {
    dprintf(fd, "TranLed: command path: %s", mCommandPath.c_str());
    dprintf(fd, ", is ok: %d\n", isOk());
}

Devices::Devices() : mBacklightDevice(), mTranLedDevice() {
    if (!hasBacklightDevice()) {
        LOG(INFO) << "No backlight device found";
    }

    if (!hasTranLedDevice()) {
        LOG(INFO) << "No tran LED device found";
    }
}

bool Devices::hasBacklightDevice() const {
    return mBacklightDevice.isOk();
}

bool Devices::hasTranLedDevice() const {
    return mTranLedDevice.isOk();
}

void Devices::setBacklightState(const LedState& state) {
    mBacklightDevice.setState(state);
}

void Devices::setTranLedState(const LedState& state) {
    mTranLedDevice.setState(state);
}

void Devices::dump(int fd) const {
    dprintf(fd, "Devices:\n");
    mBacklightDevice.dump(fd);
    mTranLedDevice.dump(fd);
    dprintf(fd, "\n");

    return;
}

}  // namespace light
}  // namespace hardware
}  // namespace android
}  // namespace aidl