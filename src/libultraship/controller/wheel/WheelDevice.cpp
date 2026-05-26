#include "libultraship/controller/wheel/WheelDevice.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

#define RAW_AXIS_DEADZONE 0.08f
#define MAX_SDL_AXIS_VALUE (float)INT16_MAX
#define WHEEL_STEERING_TUNING_CVAR "gArcadeKart.WheelSteeringTuning"
#define WHEEL_THROTTLE_TUNING_CVAR "gArcadeKart.WheelThrottleTuning"
#define WHEEL_BRAKE_TUNING_CVAR "gArcadeKart.WheelBrakeTuning"
#define WHEEL_CLUTCH_TUNING_CVAR "gArcadeKart.WheelClutchTuning"
#define WHEEL_GEAR_NONE -2
#define WHEEL_GEAR_REVERSE -1
#define WHEEL_GEAR_NEUTRAL 0
#define SURFACE_AIRBORNE 0
#define SURFACE_ASPHALT 1
#define SURFACE_DIRT 2
#define SURFACE_SAND 3
#define SURFACE_STONE 4
#define SURFACE_SNOW 5
#define SURFACE_BRIDGE 6
#define SURFACE_SAND_OFFROAD 7
#define SURFACE_GRASS 8
#define SURFACE_ICE 9
#define SURFACE_WET_SAND 10
#define SURFACE_SNOW_OFFROAD 11
#define SURFACE_CLIFF 12
#define SURFACE_DIRT_OFFROAD 13
#define SURFACE_TRAIN_TRACK 14
#define SURFACE_CAVE 15
#define SURFACE_ROPE_BRIDGE 16
#define SURFACE_WOOD_BRIDGE 17
#define SURFACE_BOOST_RAMP_WOOD 0xFC
#define SURFACE_BOOST_RAMP_ASPHALT 0xFE
#define TRACK_CHOCO_MOUNTAIN 1
#define TRACK_WARIO_STADIUM 14
#define WHEEL_MAX_FEEDBACK_SPEED_KMH 150.0f

namespace LUS {

static const WheelReading sEmptyWheelReading = { 0 };

static float ApplyWheelSteeringTuning(float rawSteeringInput) {
    if (rawSteeringInput == 0.0f) {
        return 0.0f;
    }

    float tuningConstant = std::clamp(CVarGetFloat(WHEEL_STEERING_TUNING_CVAR, 0.8f), 0.25f, 2.0f);
    float inputMagnitude = fabs(rawSteeringInput);
    float inputSign = rawSteeringInput < 0.0f ? -1.0f : 1.0f;
    float tunedSteeringInput = inputSign * powf(inputMagnitude, tuningConstant);
    return std::clamp(tunedSteeringInput, -1.0f, 1.0f);
}

static float ApplyWheelPedalTuning(float rawPedalInput, const char* tuningCvar) {
    if (rawPedalInput == 0.0f) {
        return 0.0f;
    }

    float tuningConstant = std::clamp(CVarGetFloat(tuningCvar, 1.0f), 0.25f, 2.0f);
    float inputMagnitude = std::clamp(rawPedalInput, 0.0f, 1.0f);
    float tunedPedalInput = powf(inputMagnitude, tuningConstant);
    return std::clamp(tunedPedalInput, 0.0f, 1.0f);
}

static float GetSurfaceRumbleStrength(uint16_t surfaceType) {
    switch (surfaceType) {
        case SURFACE_DIRT:
            return 0.13f;
        case SURFACE_DIRT_OFFROAD:
            return 0.24f;
        case SURFACE_GRASS:
            return 0.19f;
        case SURFACE_SAND:
        case SURFACE_WET_SAND:
            return 0.14f;
        case SURFACE_SAND_OFFROAD:
            return 0.22f;
        case SURFACE_SNOW:
        case SURFACE_SNOW_OFFROAD:
            return 0.15f;
        case SURFACE_STONE:
        case SURFACE_CAVE:
            return 0.11f;
        case SURFACE_BRIDGE:
        case SURFACE_WOOD_BRIDGE:
            return 0.10f;
        case SURFACE_ROPE_BRIDGE:
            return 0.18f;
        case SURFACE_TRAIN_TRACK:
            return 0.26f;
        case SURFACE_ICE:
            return 0.02f;
        case SURFACE_CLIFF:
            return 0.20f;
        case SURFACE_BOOST_RAMP_WOOD:
        case SURFACE_BOOST_RAMP_ASPHALT:
            return 0.28f;
        case SURFACE_ASPHALT:
        default:
            return 0.04f;
    }
}

static float GetSurfaceSteeringKnockStrength(uint16_t surfaceType) {
    switch (surfaceType) {
        case SURFACE_TRAIN_TRACK:
            return 0.70f;
        case SURFACE_DIRT_OFFROAD:
        case SURFACE_SAND_OFFROAD:
        case SURFACE_CLIFF:
            return 0.55f;
        case SURFACE_GRASS:
        case SURFACE_ROPE_BRIDGE:
            return 0.42f;
        case SURFACE_DIRT:
        case SURFACE_STONE:
        case SURFACE_CAVE:
            return 0.32f;
        case SURFACE_BRIDGE:
        case SURFACE_WOOD_BRIDGE:
        case SURFACE_BOOST_RAMP_WOOD:
        case SURFACE_BOOST_RAMP_ASPHALT:
            return 0.28f;
        case SURFACE_SAND:
        case SURFACE_WET_SAND:
        case SURFACE_SNOW:
        case SURFACE_SNOW_OFFROAD:
            return 0.18f;
        default:
            return 0.0f;
    }
}

static uint32_t GetSurfaceRumbleInterval(uint16_t surfaceType) {
    if (surfaceType == SURFACE_ICE) {
        return 35;
    }

    switch (surfaceType) {
        case SURFACE_BRIDGE:
        case SURFACE_WOOD_BRIDGE:
        case SURFACE_ROPE_BRIDGE:
        case SURFACE_TRAIN_TRACK:
            return 45;
        case SURFACE_DIRT_OFFROAD:
        case SURFACE_SAND_OFFROAD:
        case SURFACE_GRASS:
        case SURFACE_CLIFF:
            return 70;
        case SURFACE_SNOW:
        case SURFACE_SNOW_OFFROAD:
        case SURFACE_SAND:
        case SURFACE_WET_SAND:
            return 90;
        default:
            return 110;
    }
}

static uint32_t GetSurfaceSteeringKnockInterval(uint16_t surfaceType) {
    switch (surfaceType) {
        case SURFACE_TRAIN_TRACK:
        case SURFACE_BRIDGE:
        case SURFACE_WOOD_BRIDGE:
        case SURFACE_ROPE_BRIDGE:
            return 42;
        case SURFACE_DIRT_OFFROAD:
        case SURFACE_SAND_OFFROAD:
        case SURFACE_CLIFF:
            return 58;
        case SURFACE_GRASS:
        case SURFACE_DIRT:
            return 72;
        default:
            return 90;
    }
}

static bool IsDirtHeavyCourse(int16_t courseId) {
    return courseId == TRACK_CHOCO_MOUNTAIN || courseId == TRACK_WARIO_STADIUM;
}

static bool ShouldApplyCoarseDirtKnock(const WheelForceFeedbackState& state) {
    if (!IsDirtHeavyCourse(state.courseId)) {
        return false;
    }

    return state.surfaceType == SURFACE_DIRT || state.surfaceType == SURFACE_DIRT_OFFROAD ||
           state.surfaceType == SURFACE_CLIFF;
}

WheelDevice::WheelDevice(WheelDeviceDefinition definition)
    : mDefinition(definition), mJoystick(nullptr), mHaptic(nullptr), mSteeringWeightEffectId(-1),
      mCenteringForceEffectId(-1), mPeriodicEffectId(-1), mTerrainKickEffectId(-1), mSlopeForceEffectId(-1),
      mCoarseTerrainKickEffectId(-1), mNextRumbleTick(0), mNextTerrainKickTick(0), mNextCoarseTerrainKickTick(0),
      mLightningFeedbackUntilTick(0), mFeedbackNoiseState(0x1234ABCD), mLastSteeringInput(0.0f), mTerrainKickDirection(1),
      mSupportsSteeringWeight(false), mSupportsConstantForce(false), mSupportsPeriodic(false), mSupportsRumble(false),
      mLastHitByItem(false), mShifterWasInGear(false) {
}

bool WheelDevice::Matches(int32_t deviceIndex) const {
    const char* deviceName = SDL_JoystickNameForIndex(deviceIndex);
    bool nameMatches = deviceName != nullptr && std::string(deviceName).find(mDefinition.name) != std::string::npos;
    bool vidPidMatches = SDL_JoystickGetDeviceVendor(deviceIndex) == mDefinition.vendorId &&
                         SDL_JoystickGetDeviceProduct(deviceIndex) == mDefinition.productId;
    return nameMatches || vidPidMatches;
}

bool WheelDevice::IsOpen() const {
    return mJoystick != nullptr;
}

bool WheelDevice::Open(int32_t deviceIndex) {
    if (IsOpen()) {
        return true;
    }

    mJoystick = SDL_JoystickOpen(deviceIndex);
    InitializeHaptics();
    SPDLOG_INFO("Wheel device '{}' opened: axes={}, buttons={}, hats={}, haptic={}", mDefinition.name,
                IsOpen() ? SDL_JoystickNumAxes(mJoystick) : 0, IsOpen() ? SDL_JoystickNumButtons(mJoystick) : 0,
                IsOpen() ? SDL_JoystickNumHats(mJoystick) : 0, mHaptic != nullptr);
    return IsOpen();
}

void WheelDevice::InitializeHaptics() {
    if ((SDL_WasInit(SDL_INIT_HAPTIC) & SDL_INIT_HAPTIC) == 0) {
        SDL_InitSubSystem(SDL_INIT_HAPTIC);
    }

    if (!IsOpen() || mHaptic != nullptr || !SDL_JoystickIsHaptic(mJoystick)) {
        if (IsOpen()) {
            SPDLOG_INFO("Wheel device '{}' does not expose SDL haptics.", mDefinition.name);
        }
        return;
    }

    mHaptic = SDL_HapticOpenFromJoystick(mJoystick);
    if (mHaptic == nullptr) {
        SPDLOG_WARN("Wheel device '{}' haptic open failed: {}", mDefinition.name, SDL_GetError());
        return;
    }

    uint32_t supportedEffects = SDL_HapticQuery(mHaptic);
    mSupportsSteeringWeight = (supportedEffects & SDL_HAPTIC_SPRING) != 0;
    mSupportsConstantForce = (supportedEffects & SDL_HAPTIC_CONSTANT) != 0;
    mSupportsPeriodic = (supportedEffects & (SDL_HAPTIC_SINE | SDL_HAPTIC_TRIANGLE)) != 0;
    mSupportsRumble = SDL_HapticRumbleSupported(mHaptic) == SDL_TRUE;

    if (mSupportsRumble) {
        SDL_HapticRumbleInit(mHaptic);
    }
    if ((supportedEffects & SDL_HAPTIC_GAIN) != 0) {
        SDL_HapticSetGain(mHaptic, 100);
    }
    if ((supportedEffects & SDL_HAPTIC_AUTOCENTER) != 0) {
        SDL_HapticSetAutocenter(mHaptic, 0);
    }

    SPDLOG_INFO(
        "Wheel device '{}' haptics: effects=0x{:X}, spring={}, constant={}, periodic={}, rumble={}, axes={}",
        mDefinition.name, supportedEffects, mSupportsSteeringWeight, mSupportsConstantForce, mSupportsPeriodic,
        mSupportsRumble, SDL_HapticNumAxes(mHaptic));
}

void WheelDevice::UpdateSteeringWeight(float speedKmh, bool grounded) {
    if (mHaptic == nullptr || (!mSupportsSteeringWeight && !mSupportsConstantForce)) {
        return;
    }

    float speedRatio = grounded ? std::clamp(speedKmh / WHEEL_MAX_FEEDBACK_SPEED_KMH, 0.0f, 1.0f) : 0.0f;
    int16_t coefficient = static_cast<int16_t>(0x0500 + (speedRatio * 0x4800));
    uint16_t saturation = static_cast<uint16_t>(0x1800 + (speedRatio * 0xC000));
    uint16_t deadband = grounded ? 0x0500 : 0xFFFF;

    if (mSupportsSteeringWeight) {
        SDL_HapticEffect effect;
        std::memset(&effect, 0, sizeof(effect));
        effect.type = SDL_HAPTIC_SPRING;
        effect.condition.type = SDL_HAPTIC_SPRING;
        effect.condition.length = SDL_HAPTIC_INFINITY;
        effect.condition.right_sat[0] = saturation;
        effect.condition.left_sat[0] = saturation;
        effect.condition.right_coeff[0] = -coefficient;
        effect.condition.left_coeff[0] = coefficient;
        effect.condition.deadband[0] = deadband;
        effect.condition.center[0] = 0;

        if (mSteeringWeightEffectId < 0) {
            mSteeringWeightEffectId = SDL_HapticNewEffect(mHaptic, &effect);
            if (mSteeringWeightEffectId >= 0) {
                SDL_HapticRunEffect(mHaptic, mSteeringWeightEffectId, SDL_HAPTIC_INFINITY);
            } else {
                SPDLOG_WARN("Wheel device '{}' spring effect failed: {}", mDefinition.name, SDL_GetError());
                mSupportsSteeringWeight = false;
            }
        } else {
            SDL_HapticUpdateEffect(mHaptic, mSteeringWeightEffectId, &effect);
        }
        return;
    }

    if (mSupportsConstantForce) {
        PlaySignedConstantSteeringForce(-mLastSteeringInput * speedRatio * 0.85f, 120, mCenteringForceEffectId);
    }
}

void WheelDevice::PlaySignedConstantSteeringForce(float signedStrength, uint32_t durationMs, int32_t& effectId) {
    if (mHaptic == nullptr || !mSupportsConstantForce) {
        return;
    }

    float clampedStrength = std::clamp(signedStrength, -1.0f, 1.0f);
    SDL_HapticEffect effect;
    std::memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.constant.direction.dir[0] = clampedStrength < 0.0f ? -1 : 1;
    effect.constant.length = durationMs;
    effect.constant.level = static_cast<int16_t>(fabs(clampedStrength) * 32767.0f);
    effect.constant.attack_length = durationMs / 5;
    effect.constant.fade_length = durationMs / 3;

    if (effectId < 0) {
        effectId = SDL_HapticNewEffect(mHaptic, &effect);
        if (effectId < 0) {
            SPDLOG_WARN("Wheel device '{}' constant steering force failed: {}", mDefinition.name, SDL_GetError());
            return;
        }
    } else {
        SDL_HapticUpdateEffect(mHaptic, effectId, &effect);
    }
    SDL_HapticRunEffect(mHaptic, effectId, 1);
}

void WheelDevice::PlayPeriodicFeedback(float strength, uint32_t durationMs, uint16_t periodMs) {
    if (mHaptic == nullptr || !mSupportsPeriodic) {
        return;
    }

    SDL_HapticEffect effect;
    std::memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_SINE;
    effect.periodic.type = SDL_HAPTIC_SINE;
    effect.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.periodic.length = durationMs;
    effect.periodic.period = periodMs;
    effect.periodic.magnitude = static_cast<int16_t>(std::clamp(strength, 0.0f, 1.0f) * 32767.0f);
    effect.periodic.fade_length = durationMs / 3;

    if (mPeriodicEffectId < 0) {
        mPeriodicEffectId = SDL_HapticNewEffect(mHaptic, &effect);
        if (mPeriodicEffectId < 0) {
            SPDLOG_WARN("Wheel device '{}' periodic effect failed: {}", mDefinition.name, SDL_GetError());
            mSupportsPeriodic = false;
            return;
        }
    } else {
        SDL_HapticUpdateEffect(mHaptic, mPeriodicEffectId, &effect);
    }
    SDL_HapticRunEffect(mHaptic, mPeriodicEffectId, 1);
}

void WheelDevice::PlayConstantSteeringKick(float strength, uint32_t durationMs) {
    if (mHaptic == nullptr || !mSupportsConstantForce) {
        return;
    }

    mTerrainKickDirection = -mTerrainKickDirection;

    SDL_HapticEffect effect;
    std::memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.constant.direction.dir[0] = mTerrainKickDirection;
    effect.constant.length = durationMs;
    effect.constant.level = static_cast<int16_t>(std::clamp(strength, 0.0f, 1.0f) * 32767.0f);
    effect.constant.attack_length = durationMs / 4;
    effect.constant.fade_length = durationMs / 3;

    if (mTerrainKickEffectId < 0) {
        mTerrainKickEffectId = SDL_HapticNewEffect(mHaptic, &effect);
        if (mTerrainKickEffectId < 0) {
            SPDLOG_WARN("Wheel device '{}' terrain steering kick failed: {}", mDefinition.name, SDL_GetError());
            return;
        }
    } else {
        SDL_HapticUpdateEffect(mHaptic, mTerrainKickEffectId, &effect);
    }
    SDL_HapticRunEffect(mHaptic, mTerrainKickEffectId, 1);
}

void WheelDevice::UpdateSlopeSteeringBias(const WheelForceFeedbackState& state) {
    if (mHaptic == nullptr || !mSupportsConstantForce || !state.grounded) {
        return;
    }

    if (fabs(state.slopeSteeringForce) < 0.02f) {
        return;
    }

    PlaySignedConstantSteeringForce(state.slopeSteeringForce, 130, mSlopeForceEffectId);
}

void WheelDevice::UpdateTerrainSteeringKnock(const WheelForceFeedbackState& state) {
    if (mHaptic == nullptr || !mSupportsConstantForce || !state.grounded || state.surfaceType == SURFACE_AIRBORNE) {
        return;
    }

    uint32_t now = SDL_GetTicks();
    if (now < mNextTerrainKickTick) {
        return;
    }

    float speedRatio = std::clamp(state.speedKmh / WHEEL_MAX_FEEDBACK_SPEED_KMH, 0.0f, 1.0f);
    if (speedRatio < 0.08f) {
        return;
    }

    float terrainStrength = GetSurfaceSteeringKnockStrength(state.surfaceType);
    if (terrainStrength <= 0.0f) {
        return;
    }

    float steeringKickStrength = std::clamp(terrainStrength * (0.25f + (speedRatio * 0.95f)), 0.0f, 0.85f);
    uint32_t kickInterval = GetSurfaceSteeringKnockInterval(state.surfaceType);
    PlayConstantSteeringKick(steeringKickStrength, std::min<uint32_t>(kickInterval + 10, 120));
    mNextTerrainKickTick = now + kickInterval;
}

void WheelDevice::UpdateCoarseTerrainSteeringKnock(const WheelForceFeedbackState& state) {
    if (mHaptic == nullptr || !mSupportsConstantForce || !state.grounded || !ShouldApplyCoarseDirtKnock(state)) {
        return;
    }

    uint32_t now = SDL_GetTicks();
    if (now < mNextCoarseTerrainKickTick) {
        return;
    }

    float speedRatio = std::clamp(state.speedKmh / WHEEL_MAX_FEEDBACK_SPEED_KMH, 0.0f, 1.0f);
    if (speedRatio < 0.10f) {
        return;
    }

    mFeedbackNoiseState = (mFeedbackNoiseState * 1664525u) + 1013904223u;
    float randomStrength = 0.65f + (((mFeedbackNoiseState >> 16) & 0xFF) / 255.0f * 0.35f);
    float randomDirection = (mFeedbackNoiseState & 0x100) != 0 ? 1.0f : -1.0f;
    uint32_t randomInterval = 135 + ((mFeedbackNoiseState >> 8) % 165);
    uint32_t randomDuration = 75 + ((mFeedbackNoiseState >> 20) % 80);
    float steeringKickStrength = std::clamp(randomDirection * randomStrength * (0.35f + speedRatio * 0.75f),
                                            -0.95f, 0.95f);

    PlaySignedConstantSteeringForce(steeringKickStrength, randomDuration, mCoarseTerrainKickEffectId);
    mNextCoarseTerrainKickTick = now + randomInterval;
}

void WheelDevice::UpdateSurfaceRumble(const WheelForceFeedbackState& state) {
    if (mHaptic == nullptr || (!mSupportsRumble && !mSupportsPeriodic)) {
        mLastHitByItem = state.hitByItem;
        return;
    }

    uint32_t now = SDL_GetTicks();
    if (state.hitByLightning && mLightningFeedbackUntilTick == 0) {
        mLightningFeedbackUntilTick = now + 950;
    }

    if (mLightningFeedbackUntilTick > now) {
        if (!mLastHitByItem || now >= mNextRumbleTick) {
            float remainingRatio = (mLightningFeedbackUntilTick - now) / 950.0f;
            float lightningStrength = std::clamp(remainingRatio * remainingRatio, 0.0f, 1.0f);
            if (mSupportsPeriodic) {
                PlayPeriodicFeedback(lightningStrength, 120, 18);
            } else {
                SDL_HapticRumblePlay(mHaptic, lightningStrength, 95);
            }
            mNextRumbleTick = now + 70;
        }
        mLastHitByItem = true;
        return;
    }
    mLightningFeedbackUntilTick = 0;

    if (state.hitByItem) {
        if (!mLastHitByItem || now >= mNextRumbleTick) {
            if (mSupportsPeriodic) {
                PlayPeriodicFeedback(1.0f, 120, 18);
            } else {
                SDL_HapticRumblePlay(mHaptic, 1.0f, 95);
            }
            mNextRumbleTick = now + 60;
        }
        mLastHitByItem = true;
        return;
    }

    mLastHitByItem = false;
    if (!state.grounded || state.surfaceType == SURFACE_AIRBORNE || now < mNextRumbleTick) {
        return;
    }

    float speedRatio = std::clamp(state.speedKmh / WHEEL_MAX_FEEDBACK_SPEED_KMH, 0.0f, 1.0f);
    if (speedRatio < 0.04f) {
        return;
    }

    float surfaceStrength = GetSurfaceRumbleStrength(state.surfaceType);
    float rumbleStrength = std::clamp(surfaceStrength * (0.30f + (speedRatio * 0.90f)), 0.0f, 0.65f);
    if (rumbleStrength > 0.0f) {
        if (mSupportsPeriodic) {
            uint16_t periodMs = state.surfaceType == SURFACE_ICE ? 7 : static_cast<uint16_t>(GetSurfaceRumbleInterval(state.surfaceType));
            uint32_t durationMs = state.surfaceType == SURFACE_ICE ? 80 : 70;
            PlayPeriodicFeedback(rumbleStrength, durationMs, periodMs);
        } else {
            SDL_HapticRumblePlay(mHaptic, rumbleStrength, 55);
        }
        mNextRumbleTick = now + GetSurfaceRumbleInterval(state.surfaceType);
    }
}

void WheelDevice::UpdateForceFeedback(const WheelForceFeedbackState& state) {
    if (!IsOpen()) {
        return;
    }

    UpdateSteeringWeight(state.speedKmh, state.grounded);
    UpdateSlopeSteeringBias(state);
    UpdateTerrainSteeringKnock(state);
    UpdateCoarseTerrainSteeringKnock(state);
    UpdateSurfaceRumble(state);
}

float WheelDevice::ReadSignedAxis(int32_t axisIndex) const {
    if (!IsOpen() || axisIndex < 0 || axisIndex >= SDL_JoystickNumAxes(mJoystick)) {
        return 0.0f;
    }

    float value = SDL_JoystickGetAxis(mJoystick, axisIndex) / MAX_SDL_AXIS_VALUE;
    if (fabs(value) < RAW_AXIS_DEADZONE) {
        return 0.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

float WheelDevice::ReadPositiveAxis(int32_t axisIndex) const {
    if (!IsOpen() || axisIndex < 0 || axisIndex >= SDL_JoystickNumAxes(mJoystick)) {
        return 0.0f;
    }

    float value = SDL_JoystickGetAxis(mJoystick, axisIndex) / MAX_SDL_AXIS_VALUE;
    if (value < RAW_AXIS_DEADZONE) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

float WheelDevice::ReadPedalAxis(int32_t axisIndex, bool inverted) const {
    if (!IsOpen() || axisIndex < 0 || axisIndex >= SDL_JoystickNumAxes(mJoystick)) {
        return 0.0f;
    }

    float rawValue = SDL_JoystickGetAxis(mJoystick, axisIndex) / MAX_SDL_AXIS_VALUE;
    if (fabs(rawValue) < RAW_AXIS_DEADZONE) {
        return 0.0f;
    }

    float value = inverted ? ((1.0f - rawValue) * 0.5f) : ((rawValue + 1.0f) * 0.5f);
    if (value < RAW_AXIS_DEADZONE) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

bool WheelDevice::ReadButton(int32_t buttonIndex) const {
    if (!IsOpen() || buttonIndex < 0 || buttonIndex >= SDL_JoystickNumButtons(mJoystick)) {
        return false;
    }

    return SDL_JoystickGetButton(mJoystick, buttonIndex) != 0;
}

WheelReading WheelDevice::Read() {
    WheelReading reading = sEmptyWheelReading;
    reading.requestedGear = WHEEL_GEAR_NONE;
    if (!IsOpen()) {
        return reading;
    }

    reading.connected = true;
    float rawSteeringInput = ReadSignedAxis(mDefinition.steeringAxis);
    reading.steering = ApplyWheelSteeringTuning(rawSteeringInput);
    mLastSteeringInput = reading.steering;
    reading.throttle = ApplyWheelPedalTuning(ReadPedalAxis(mDefinition.throttleAxis, mDefinition.invertThrottleAxis),
                                             WHEEL_THROTTLE_TUNING_CVAR);
    reading.brake =
        ApplyWheelPedalTuning(ReadPedalAxis(mDefinition.brakeAxis, mDefinition.invertBrakeAxis), WHEEL_BRAKE_TUNING_CVAR);
    reading.clutch = ApplyWheelPedalTuning(ReadPedalAxis(mDefinition.clutchAxis, mDefinition.invertClutchAxis),
                                           WHEEL_CLUTCH_TUNING_CVAR);
    reading.handbrake = ReadPositiveAxis(mDefinition.handbrakeAxis);
    if (reading.brake > 0.10f && reading.throttle > 0.10f && fabs(reading.brake - reading.throttle) < 0.08f) {
        reading.throttle = 0.0f;
    }

    if (mDefinition.povHat >= 0 && mDefinition.povHat < SDL_JoystickNumHats(mJoystick)) {
        uint8_t hat = SDL_JoystickGetHat(mJoystick, mDefinition.povHat);
        reading.menuUp = (hat & SDL_HAT_UP) != 0;
        reading.menuDown = (hat & SDL_HAT_DOWN) != 0;
        reading.menuLeft = (hat & SDL_HAT_LEFT) != 0;
        reading.menuRight = (hat & SDL_HAT_RIGHT) != 0;
    }

    reading.menuConfirm = ReadButton(mDefinition.menuConfirmButton);
    reading.menuCancel = ReadButton(mDefinition.menuCancelButton);
    reading.openMenu = ReadButton(mDefinition.openMenuButton);
    reading.useItemBackward = ReadButton(mDefinition.useItemBackwardButton);
    reading.useItemForward = ReadButton(mDefinition.useItemForwardButton);

    if (mDefinition.gear1Button >= 0) {
        if (ReadButton(mDefinition.gear1Button)) {
            reading.requestedGear = 1;
        } else if (ReadButton(mDefinition.gear2Button)) {
            reading.requestedGear = 2;
        } else if (ReadButton(mDefinition.gear3Button)) {
            reading.requestedGear = 3;
        } else if (ReadButton(mDefinition.gear4Button)) {
            reading.requestedGear = 4;
        } else if (ReadButton(mDefinition.gear5Button)) {
            reading.requestedGear = 5;
        } else if (ReadButton(mDefinition.gear6Button)) {
            reading.requestedGear = 6;
        } else if (ReadButton(mDefinition.reverseButton)) {
            reading.requestedGear = WHEEL_GEAR_REVERSE;
        } else if (mShifterWasInGear) {
            reading.requestedGear = WHEEL_GEAR_NEUTRAL;
        }

        mShifterWasInGear = reading.requestedGear > WHEEL_GEAR_NEUTRAL ||
                             reading.requestedGear == WHEEL_GEAR_REVERSE;
    }

    return reading;
}

WheelDeviceManager& WheelDeviceManager::Instance() {
    static WheelDeviceManager manager;
    return manager;
}

WheelDeviceManager::WheelDeviceManager()
    : mDevices({
          WheelDevice({ "G27 Racing Wheel", 0x046D, 0xC294, 0, 1, 2, 4, -1, true, true, true, 0, 0, 1, 3, 5, 4, 8, 9,
                        10, 11, 12, 13, 14 }),
          WheelDevice({ "G27 Racing Wheel", 0x046D, 0xC29B, 0, 1, 2, 4, -1, true, true, true, 0, 0, 1, 3, 5, 4, 8, 9,
                        10, 11, 12, 13, 14 }),
          WheelDevice({ "ODDOR-HANDBRAKE", 0x1021, 0x1888, -1, -1, -1, -1, 0, false, false, false, -1, -1, -1, -1,
                        -1, -1, -1, -1, -1, -1, -1, -1, -1 }),
      }),
      mLastJoystickCount(-1) {
}

void WheelDeviceManager::RefreshDevices() {
    int32_t joystickCount = SDL_NumJoysticks();
    if (joystickCount == mLastJoystickCount) {
        return;
    }

    mLastJoystickCount = joystickCount;
    for (int32_t i = 0; i < joystickCount; i++) {
        for (auto& device : mDevices) {
            if (!device.IsOpen() && device.Matches(i)) {
                device.Open(i);
            }
        }
    }
}

WheelReading WheelDeviceManager::ReadPlayerOneWheel() {
    WheelReading mergedReading = sEmptyWheelReading;
    mergedReading.requestedGear = WHEEL_GEAR_NONE;

    RefreshDevices();
    for (auto& device : mDevices) {
        WheelReading reading = device.Read();
        if (fabs(reading.steering) > fabs(mergedReading.steering)) {
            mergedReading.steering = reading.steering;
        }
        mergedReading.connected = mergedReading.connected || reading.connected;
        mergedReading.throttle = std::max(mergedReading.throttle, reading.throttle);
        mergedReading.brake = std::max(mergedReading.brake, reading.brake);
        mergedReading.clutch = std::max(mergedReading.clutch, reading.clutch);
        mergedReading.handbrake = std::max(mergedReading.handbrake, reading.handbrake);
        mergedReading.menuUp = mergedReading.menuUp || reading.menuUp;
        mergedReading.menuDown = mergedReading.menuDown || reading.menuDown;
        mergedReading.menuLeft = mergedReading.menuLeft || reading.menuLeft;
        mergedReading.menuRight = mergedReading.menuRight || reading.menuRight;
        mergedReading.menuConfirm = mergedReading.menuConfirm || reading.menuConfirm;
        mergedReading.menuCancel = mergedReading.menuCancel || reading.menuCancel;
        mergedReading.openMenu = mergedReading.openMenu || reading.openMenu;
        mergedReading.useItemBackward = mergedReading.useItemBackward || reading.useItemBackward;
        mergedReading.useItemForward = mergedReading.useItemForward || reading.useItemForward;
        if (reading.requestedGear != WHEEL_GEAR_NONE) {
            mergedReading.requestedGear = reading.requestedGear;
        }
    }

    return mergedReading;
}

void WheelDeviceManager::UpdatePlayerOneForceFeedback(float speedKmh, float slopeSteeringForce, bool grounded,
                                                      bool hitByItem, bool hitByLightning, uint16_t surfaceType,
                                                      int16_t courseId) {
    RefreshDevices();

    WheelForceFeedbackState state = { speedKmh, slopeSteeringForce, grounded, hitByItem, hitByLightning, surfaceType,
                                      courseId };
    for (auto& device : mDevices) {
        device.UpdateForceFeedback(state);
    }
}

} // namespace LUS
