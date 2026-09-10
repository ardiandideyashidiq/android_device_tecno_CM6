/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"

#include <android-base/logging.h>
#include <thread>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static constexpr float Q_FACTOR = 11.0;
static constexpr float PWLE_FREQUENCY_RESOLUTION_HZ = 1.0;
static constexpr float PWLE_FREQUENCY_MIN_HZ = 140.0;
static constexpr float RESONANT_FREQUENCY_HZ = 150.0;
static constexpr float PWLE_FREQUENCY_MAX_HZ = 160.0;
static constexpr float PWLE_LEVEL_MIN = 0.0;
static constexpr float PWLE_LEVEL_MAX = 1.0;
static constexpr float PWLE_BW_MAP_SIZE =
        1 + ((PWLE_FREQUENCY_MAX_HZ - PWLE_FREQUENCY_MIN_HZ) / PWLE_FREQUENCY_RESOLUTION_HZ);

#ifdef VIBRATOR_SUPPORTS_EFFECTS
Vibrator::Vibrator() {
    if (exists(kVibratorStrength)) {
        mVibratorStrengthSupported = true;
        mVibratorStrengthMax = getNode(kVibratorStrengthMax, 9);
    }
}
#endif

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    LOG(VERBOSE) << "Vibrator reporting capabilities";
    *_aidl_return = IVibrator::CAP_ON_CALLBACK;
    *_aidl_return |= IVibrator::CAP_GET_RESONANT_FREQUENCY;
    *_aidl_return |= IVibrator::CAP_GET_Q_FACTOR;
    *_aidl_return |= IVibrator::CAP_FREQUENCY_CONTROL;

#ifdef VIBRATOR_SUPPORTS_EFFECTS
    *_aidl_return |= IVibrator::CAP_PERFORM_CALLBACK;

    if (mVibratorStrengthSupported) *_aidl_return |= IVibrator::CAP_AMPLITUDE_CONTROL;
#endif

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    LOG(VERBOSE) << "Vibrator off";
    return setNode(kVibratorActivate, 0);
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    ndk::ScopedAStatus status;

    LOG(VERBOSE) << "Vibrator on for timeoutMs: " << timeoutMs;

    status = activate(timeoutMs);
    if (!status.isOk()) return status;

    if (callback != nullptr) {
        std::thread([=] {
            LOG(VERBOSE) << "Starting on on another thread";
            usleep(timeoutMs * 1000);
            LOG(VERBOSE) << "Notifying on complete";
            if (!callback->onComplete().isOk()) {
                LOG(ERROR) << "Failed to call onComplete";
            }
        }).detach();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    ndk::ScopedAStatus status;
    int32_t timeoutMs;

    if (vibEffects.find(effect) == vibEffects.end())
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    if (vibStrengths.find(strength) == vibStrengths.end())
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

#ifdef VIBRATOR_SUPPORTS_EFFECTS
    setAmplitude(vibStrengths[strength]);
#endif

    timeoutMs = vibEffects[effect];

    status = activate(timeoutMs);
    if (!status.isOk()) return status;

    if (callback != nullptr) {
        std::thread([=] {
            LOG(VERBOSE) << "Starting perform on another thread";
            usleep(timeoutMs * 1000);
            LOG(VERBOSE) << "Notifying perform complete";
            callback->onComplete();
        }).detach();
    }

    *_aidl_return = timeoutMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    for (auto const& pair : vibEffects) _aidl_return->push_back(pair.first);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
#ifdef VIBRATOR_SUPPORTS_EFFECTS
    int32_t intensity;
#endif

    if (amplitude <= 0.0f || amplitude > 1.0f)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));

#ifdef VIBRATOR_SUPPORTS_EFFECTS
    LOG(VERBOSE) << "Setting amplitude: " << amplitude;

    intensity = amplitude * mVibratorStrengthMax;

    LOG(VERBOSE) << "Setting intensity: " << intensity;

    if (mVibratorStrengthSupported) setNode(kVibratorStrength, intensity);
#endif

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool enabled __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(
        std::vector<CompositePrimitive>* supported __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive __unused,
                                                  int32_t* durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite __unused,
                                     const std::shared_ptr<IVibratorCallback>& callback __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(
        std::vector<Effect>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t id __unused, Effect effect __unused,
                                            EffectStrength strength __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t id __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz) {
    *resonantFreqHz = RESONANT_FREQUENCY_HZ;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor) {
    *qFactor = Q_FACTOR;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* freqResolutionHz) {
    *freqResolutionHz = PWLE_FREQUENCY_RESOLUTION_HZ;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* freqMinimumHz) {
    *freqMinimumHz = PWLE_FREQUENCY_MIN_HZ;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) {
    int32_t capabilities = 0;
    int halfMapSize = PWLE_BW_MAP_SIZE / 2;
    Vibrator::getCapabilities(&capabilities);
    if (capabilities & IVibrator::CAP_FREQUENCY_CONTROL) {
        std::vector<float> bandwidthAmplitudeMap(PWLE_BW_MAP_SIZE, PWLE_LEVEL_MAX);
        for (int i = 0; i < halfMapSize; ++i) {
            bandwidthAmplitudeMap[halfMapSize + i + 1] =
                    bandwidthAmplitudeMap[halfMapSize + i] - 0.01;
            bandwidthAmplitudeMap[halfMapSize - i - 1] =
                    bandwidthAmplitudeMap[halfMapSize - i] - 0.01;
        }
        *_aidl_return = bandwidthAmplitudeMap;
        return ndk::ScopedAStatus::ok();
    } else {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
}

ndk::ScopedAStatus Vibrator::getFrequencyToOutputAccelerationMap(
        std::vector<FrequencyAccelerationMapEntry>* _aidl_return) {
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!(capabilities & IVibrator::CAP_FREQUENCY_CONTROL)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    std::vector<FrequencyAccelerationMapEntry> frequencyToOutputAccelerationMap;

    std::vector<std::pair<float, float>> frequencyToOutputAccelerationData = {
            {30.0f, 0.01f},  {46.0f, 0.09f},  {50.0f, 0.1f},   {55.0f, 0.12f},  {62.0f, 0.66f},
            {83.0f, 0.82f},  {85.0f, 0.85f},  {92.0f, 1.05f},  {107.0f, 1.63f}, {115.0f, 1.72f},
            {123.0f, 1.81f}, {135.0f, 2.23f}, {144.0f, 2.47f}, {145.0f, 2.5f},  {150.0f, 3.0f},
            {175.0f, 2.51f}, {181.0f, 2.41f}, {190.0f, 2.28f}, {200.0f, 2.08f}, {204.0f, 1.96f},
            {205.0f, 1.9f},  {224.0f, 1.7f},  {235.0f, 1.5f},  {242.0f, 1.46f}, {253.0f, 1.41f},
            {263.0f, 1.39f}, {65.0f, 1.38f},  {278.0f, 1.37f}, {294.0f, 1.35f}, {300.0f, 1.34f}};
    for (const auto& entry : frequencyToOutputAccelerationData) {
        frequencyToOutputAccelerationMap.push_back(
                FrequencyAccelerationMapEntry(/*frequency=*/entry.first,
                                              /*maxOutputAcceleration=*/entry.second));
    }

    *_aidl_return = frequencyToOutputAccelerationMap;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMaxMillis(int32_t* maxDurationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMinMillis(int32_t* minDurationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleV2CompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::performVendorEffect(
        const VendorEffect& effect __unused,
        const std::shared_ptr<IVibratorCallback>& callback __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwleV2(const CompositePwleV2& composite __unused,
                                           const std::shared_ptr<IVibratorCallback>& callback
                                                   __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* supported __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& composite __unused,
                                         const std::shared_ptr<IVibratorCallback>& callback
                                                 __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
