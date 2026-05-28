#include "libultraship/controller/wheel/WheelDevice.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define RAW_AXIS_DEADZONE 0.08f
#define MAX_SDL_AXIS_VALUE (float)INT16_MAX
#define WHEEL_STEERING_TUNING_CVAR "gArcadeKart.WheelSteeringTuning"
#define WHEEL_THROTTLE_TUNING_CVAR "gArcadeKart.WheelThrottleTuning"
#define WHEEL_BRAKE_TUNING_CVAR "gArcadeKart.WheelBrakeTuning"
#define WHEEL_CLUTCH_TUNING_CVAR "gArcadeKart.WheelClutchTuning"
#define WHEEL_PROFILER_SPRING_CVAR "gArcadeKart.UseLogitechProfilerSpring"
#define WHEEL_PROFILER_MIN_SPRING_CVAR "gArcadeKart.LogitechProfilerMinSpring"
#define WHEEL_PROFILER_MAX_SPRING_CVAR "gArcadeKart.LogitechProfilerMaxSpring"
#define WHEEL_PROFILER_MENU_SPRING_CVAR "gArcadeKart.LogitechProfilerMenuSpring"
#define WHEEL_SPRING_BASELINE_MULTIPLIER_CVAR "gArcadeKart.LogitechProfilerSpringBaselineMultiplier"
#define WHEEL_LOGITECH_SDK_SPRING_CVAR "gArcadeKart.LogitechSdkSpringPercent"
#define WHEEL_LOGITECH_SDK_SPRING_STEP 5.0f
#define WHEEL_LOGITECH_SDK_USE_DYNAMIC_SPRING_CVAR "gArcadeKart.LogitechSdkUseSpringForce"
#define WHEEL_SDL_CENTERING_CVAR "gArcadeKart.UseSdlWheelCentering"
#define WHEEL_SHIFTER_SMOOTHING_FRAMES_CVAR "gArcadeKart.WheelShifterSmoothingFrames"
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

#ifdef _WIN32
struct LogitechControllerPropertiesData {
    bool forceEnable;
    int overallGain;
    int springGain;
    int damperGain;
    bool defaultSpringEnabled;
    int defaultSpringGain;
    bool combinePedals;
    int wheelRange;
    bool gameSettingsEnabled;
    bool allowGameSettings;
};

class LogitechSteeringWheelSdk {
  public:
    static LogitechSteeringWheelSdk& Instance() {
        static LogitechSteeringWheelSdk sdk;
        return sdk;
    }

    bool EnsureInitialized() {
        if (mInitialized) {
            return true;
        }
        if (!EnsureLoaded()) {
            return false;
        }

        HWND hwnd = GetActiveWindow();
        if (hwnd == nullptr) {
            hwnd = GetForegroundWindow();
        }

        bool initialized = false;
        if (mSteeringInitializeWithWindow != nullptr && hwnd != nullptr) {
            initialized = mSteeringInitializeWithWindow(true, hwnd);
        }
        if (!initialized && mSteeringInitialize != nullptr) {
            initialized = mSteeringInitialize(true);
        }

        mInitialized = initialized;
        mDebugInitialized.store(mInitialized ? 1 : 0);
        if (mInitialized) {
            SPDLOG_INFO("Logitech Steering Wheel SDK initialized.");
        } else {
            SPDLOG_WARN("Logitech Steering Wheel SDK failed to initialize.");
        }
        return mInitialized;
    }

    bool IsLoaded() const {
        return mDll != nullptr;
    }

    void StartSpringWorker() {
        bool expected = false;
        if (!mWorkerStarted.compare_exchange_strong(expected, true)) {
            return;
        }

        std::thread([this]() { SpringWorkerMain(); }).detach();
    }

    void RequestSpringPercent(int32_t springPercent) {
        mDesiredSpringPercent.store(std::clamp(springPercent, 0, 100));
        mUseDynamicSpringForce.store(CVarGetInteger(WHEEL_LOGITECH_SDK_USE_DYNAMIC_SPRING_CVAR, 0) != 0 ? 1 : 0);
    }

    bool HasWorkerStarted() const {
        return mWorkerStarted.load();
    }

    int32_t GetObservedSpringPercent() const {
        return mObservedSpringPercent.load();
    }

    void PublishDebugCvars() const {
        CVarSetInteger("gArcadeKart.DebugLogitechSdkLoaded", mDebugLoaded.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkHasFunctions", mDebugHasFunctions.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkInitialized", mDebugInitialized.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkWorkerRunning", mDebugWorkerRunning.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkCurrentOk", mDebugCurrentOk.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkSetPreferredOk", mDebugSetPreferredOk.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkPlaySpringOk", mDebugPlaySpringOk.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringActive", mDebugSpringActive.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringRequested", mDesiredSpringPercent.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringApplied", mAppliedSpringPercent.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringObserved", mObservedSpringPercent.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringGain", mObservedSpringGain.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkDefaultSpringGain", mObservedDefaultSpringGain.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkForceEnable", mObservedForceEnabled.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkOverallGain", mObservedOverallGain.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkDamperGain", mObservedDamperGain.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkDefaultSpringEnabled", mObservedDefaultSpringEnabled.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkWheelRange", mObservedWheelRange.load());
        CVarSetInteger("gArcadeKart.DebugLogitechSdkGameSettings", mObservedGameSettings.load());
    }

    bool Update() {
        return mUpdate != nullptr && mUpdate();
    }

    bool GetCurrentProperties(int32_t index, LogitechControllerPropertiesData& properties) {
        if (!EnsureInitialized() || mGetCurrentControllerProperties == nullptr) {
            return false;
        }

        uint8_t rawProperties[64] = {};
        bool received = mGetCurrentControllerProperties(index, rawProperties);
        std::memcpy(&properties, rawProperties, sizeof(properties));
        return received || IsPlausibleProperties(properties);
    }

    bool SetPreferredProperties(const LogitechControllerPropertiesData& properties) {
        if (!EnsureInitialized() || mSetPreferredControllerProperties == nullptr) {
            return false;
        }

        return mSetPreferredControllerProperties(properties);
    }

    bool PlaySpringForce(int32_t index, int32_t springPercent) {
        if (!EnsureInitialized() || mPlaySpringForce == nullptr) {
            return false;
        }

        return mPlaySpringForce(index, 0, springPercent, 100);
    }

    bool StopSpringForce(int32_t index) {
        if (!EnsureInitialized() || mStopSpringForce == nullptr) {
            return false;
        }

        return mStopSpringForce(index);
    }

  private:
    using LogiSteeringInitialize = bool (*)(bool);
    using LogiSteeringInitializeWithWindow = bool (*)(bool, HWND);
    using LogiUpdate = bool (*)();
    using LogiGetCurrentControllerProperties = bool (*)(int32_t, void*);
    using LogiSetPreferredControllerProperties = bool (*)(LogitechControllerPropertiesData);
    using LogiPlaySpringForce = bool (*)(int32_t, int32_t, int32_t, int32_t);
    using LogiStopSpringForce = bool (*)(int32_t);

    LogitechSteeringWheelSdk() = default;

    bool EnsureLoaded() {
        if (mTriedLoad) {
            return mDll != nullptr;
        }
        mTriedLoad = true;

        const char* dllPaths[] = {
            "LogitechSteeringWheel.dll",
            "C:\\Program Files\\Logitech\\Gaming Software\\SDKs\\LogitechSteeringWheel.dll",
            "C:\\Program Files (x86)\\Logitech\\Gaming Software\\SDKs\\LogitechSteeringWheel.dll",
        };

        for (const char* dllPath : dllPaths) {
            mDll = LoadLibraryA(dllPath);
            if (mDll != nullptr) {
                SPDLOG_INFO("Loaded Logitech Steering Wheel SDK from '{}'.", dllPath);
                break;
            }
        }

        mDebugLoaded.store(mDll != nullptr ? 1 : 0);
        if (mDll == nullptr) {
            SPDLOG_WARN("Logitech Steering Wheel SDK DLL was not found.");
            return false;
        }

        mSteeringInitialize =
            reinterpret_cast<LogiSteeringInitialize>(GetProcAddress(mDll, "LogiSteeringInitialize"));
        mSteeringInitializeWithWindow = reinterpret_cast<LogiSteeringInitializeWithWindow>(
            GetProcAddress(mDll, "LogiSteeringInitializeWithWindow"));
        mUpdate = reinterpret_cast<LogiUpdate>(GetProcAddress(mDll, "LogiUpdate"));
        mGetCurrentControllerProperties = reinterpret_cast<LogiGetCurrentControllerProperties>(
            GetProcAddress(mDll, "LogiGetCurrentControllerProperties"));
        mSetPreferredControllerProperties = reinterpret_cast<LogiSetPreferredControllerProperties>(
            GetProcAddress(mDll, "LogiSetPreferredControllerProperties"));
        mPlaySpringForce = reinterpret_cast<LogiPlaySpringForce>(GetProcAddress(mDll, "LogiPlaySpringForce"));
        mStopSpringForce = reinterpret_cast<LogiStopSpringForce>(GetProcAddress(mDll, "LogiStopSpringForce"));

        bool hasRequiredFunctions = mSteeringInitialize != nullptr && mUpdate != nullptr &&
                                    mGetCurrentControllerProperties != nullptr &&
                                    mSetPreferredControllerProperties != nullptr && mPlaySpringForce != nullptr;
        mDebugHasFunctions.store(hasRequiredFunctions ? 1 : 0);
        if (!hasRequiredFunctions) {
            SPDLOG_WARN("Logitech Steering Wheel SDK is missing one or more required exports.");
        }
        return hasRequiredFunctions;
    }

    void SpringWorkerMain() {
        mDebugWorkerRunning.store(1);
        if (!EnsureInitialized()) {
            mDebugWorkerRunning.store(0);
            return;
        }

        int32_t lastAppliedSpringPercent = -1;
        for (;;) {
            Update();

            LogitechControllerPropertiesData properties = {};
            bool currentOk = GetCurrentProperties(0, properties);
            mDebugCurrentOk.store(currentOk ? 1 : 0);
            if (currentOk) {
                int32_t observedSpringPercent =
                    properties.defaultSpringEnabled ? properties.defaultSpringGain : properties.springGain;
                if (observedSpringPercent <= 0) {
                    observedSpringPercent = properties.defaultSpringGain > 0 ? properties.defaultSpringGain : 100;
                }

                mObservedSpringPercent.store(std::clamp(observedSpringPercent, 0, 100));
                mObservedSpringGain.store(properties.springGain);
                mObservedDefaultSpringGain.store(properties.defaultSpringGain);
                mObservedForceEnabled.store(properties.forceEnable ? 1 : 0);
                mObservedOverallGain.store(properties.overallGain);
                mObservedDamperGain.store(properties.damperGain);
                mObservedDefaultSpringEnabled.store(properties.defaultSpringEnabled ? 1 : 0);
                mObservedWheelRange.store(properties.wheelRange);
                mObservedGameSettings.store(properties.gameSettingsEnabled ? 1 : 0);

                int32_t expectedUnset = -1;
                mDesiredSpringPercent.compare_exchange_strong(expectedUnset, mObservedSpringPercent.load());
            } else {
                properties.forceEnable = true;
                properties.overallGain = 100;
                properties.springGain = 100;
                properties.damperGain = 100;
                properties.combinePedals = false;
                properties.wheelRange = 200;
                properties.gameSettingsEnabled = true;
                properties.allowGameSettings = true;
            }

            int32_t desiredSpringPercent = mDesiredSpringPercent.load();
            if (desiredSpringPercent >= 0 && desiredSpringPercent != lastAppliedSpringPercent) {
                desiredSpringPercent = std::clamp(desiredSpringPercent, 0, 100);
                properties.forceEnable = true;
                properties.springGain = desiredSpringPercent;
                if (properties.defaultSpringEnabled) {
                    properties.defaultSpringGain = desiredSpringPercent;
                }
                properties.gameSettingsEnabled = true;
                properties.allowGameSettings = true;

                bool preferredOk = SetPreferredProperties(properties);
                bool useDynamicSpringForce = mUseDynamicSpringForce.load() != 0;
                bool springOk = useDynamicSpringForce ? PlaySpringForce(0, desiredSpringPercent) : false;
                mDebugSetPreferredOk.store(preferredOk ? 1 : 0);
                mDebugPlaySpringOk.store(springOk ? 1 : 0);
                mDebugSpringActive.store((preferredOk || springOk) ? 1 : 0);
                mAppliedSpringPercent.store(desiredSpringPercent);
                lastAppliedSpringPercent = desiredSpringPercent;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool IsPlausibleProperties(const LogitechControllerPropertiesData& properties) const {
        return properties.overallGain >= 0 && properties.overallGain <= 150 && properties.springGain >= 0 &&
               properties.springGain <= 150 && properties.damperGain >= 0 && properties.damperGain <= 150 &&
               properties.defaultSpringGain >= 0 && properties.defaultSpringGain <= 150 && properties.wheelRange >= 40 &&
               properties.wheelRange <= 1080;
    }

    HMODULE mDll = nullptr;
    bool mTriedLoad = false;
    bool mInitialized = false;
    LogiSteeringInitialize mSteeringInitialize = nullptr;
    LogiSteeringInitializeWithWindow mSteeringInitializeWithWindow = nullptr;
    LogiUpdate mUpdate = nullptr;
    LogiGetCurrentControllerProperties mGetCurrentControllerProperties = nullptr;
    LogiSetPreferredControllerProperties mSetPreferredControllerProperties = nullptr;
    LogiPlaySpringForce mPlaySpringForce = nullptr;
    LogiStopSpringForce mStopSpringForce = nullptr;
    std::atomic<bool> mWorkerStarted{ false };
    std::atomic<int32_t> mDesiredSpringPercent{ -1 };
    std::atomic<int32_t> mUseDynamicSpringForce{ 0 };
    std::atomic<int32_t> mAppliedSpringPercent{ -1 };
    std::atomic<int32_t> mObservedSpringPercent{ -1 };
    std::atomic<int32_t> mObservedSpringGain{ 0 };
    std::atomic<int32_t> mObservedDefaultSpringGain{ 0 };
    std::atomic<int32_t> mObservedForceEnabled{ 0 };
    std::atomic<int32_t> mObservedOverallGain{ 0 };
    std::atomic<int32_t> mObservedDamperGain{ 0 };
    std::atomic<int32_t> mObservedDefaultSpringEnabled{ 0 };
    std::atomic<int32_t> mObservedWheelRange{ 0 };
    std::atomic<int32_t> mObservedGameSettings{ 0 };
    std::atomic<int32_t> mDebugLoaded{ 0 };
    std::atomic<int32_t> mDebugHasFunctions{ 0 };
    std::atomic<int32_t> mDebugInitialized{ 0 };
    std::atomic<int32_t> mDebugWorkerRunning{ 0 };
    std::atomic<int32_t> mDebugCurrentOk{ 0 };
    std::atomic<int32_t> mDebugSetPreferredOk{ 0 };
    std::atomic<int32_t> mDebugPlaySpringOk{ 0 };
    std::atomic<int32_t> mDebugSpringActive{ 0 };
};

static bool SetRegistryDword(HKEY root, const char* subkey, const char* valueName, DWORD value) {
    HKEY key = nullptr;
    if (RegOpenKeyExA(root, subkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = RegSetValueExA(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
#endif

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

static bool ShouldUseSdlWheelCentering() {
#ifdef _WIN32
    bool profilerSpringEnabled = CVarGetInteger(WHEEL_PROFILER_SPRING_CVAR, true) != 0;
    return CVarGetInteger(WHEEL_SDL_CENTERING_CVAR, profilerSpringEnabled ? 0 : 1) != 0;
#else
    return CVarGetInteger(WHEEL_SDL_CENTERING_CVAR, true) != 0;
#endif
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

static float GetSurfaceCenteringMultiplier(uint16_t surfaceType) {
    switch (surfaceType) {
        case SURFACE_ICE:
            return 0.50f;
        case SURFACE_DIRT_OFFROAD:
        case SURFACE_CLIFF:
        case SURFACE_TRAIN_TRACK:
            return 0.60f;
        case SURFACE_SAND_OFFROAD:
        case SURFACE_SNOW_OFFROAD:
        case SURFACE_ROPE_BRIDGE:
            return 0.68f;
        case SURFACE_DIRT:
        case SURFACE_GRASS:
        case SURFACE_SNOW:
            return 0.75f;
        case SURFACE_SAND:
        case SURFACE_WET_SAND:
            return 0.78f;
        case SURFACE_STONE:
        case SURFACE_CAVE:
            return 0.85f;
        case SURFACE_BRIDGE:
        case SURFACE_WOOD_BRIDGE:
            return 0.88f;
        default:
            return 1.0f;
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
      mLastProfilerSpringPercent(-1), mLastLogitechSdkSpringPercent(-1), mLogitechSdkIndex(0),
      mLastShifterButtonMask(-1), mLastRequestedGear(WHEEL_GEAR_NONE),
      mShifterGearHistory{ WHEEL_GEAR_NONE, WHEEL_GEAR_NONE, WHEEL_GEAR_NONE, WHEEL_GEAR_NONE, WHEEL_GEAR_NONE,
                            WHEEL_GEAR_NONE, WHEEL_GEAR_NONE, WHEEL_GEAR_NONE },
      mShifterGearHistoryIndex(0), mShifterGearHistoryCount(0), mNextProfilerSpringUpdateTick(0),
      mNextLogitechSdkSpringUpdateTick(0),
      mSupportsSteeringWeight(false), mSupportsConstantForce(false), mSupportsPeriodic(false), mSupportsRumble(false),
      mLogitechSdkSpringActive(false), mLastHitByItem(false), mShifterWasInGear(false) {
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
    InitializeLogitechSdkSpring();
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
    CVarSetInteger("gArcadeKart.DebugSpringHapticOpen", 1);
    CVarSetInteger("gArcadeKart.DebugSpringSupportsSpring", mSupportsSteeringWeight ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugSpringSupportsConstant", mSupportsConstantForce ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugSpringSupportsPeriodic", mSupportsPeriodic ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugSpringSupportsRumble", mSupportsRumble ? 1 : 0);

    if (mSupportsRumble) {
        SDL_HapticRumbleInit(mHaptic);
    }
    if ((supportedEffects & SDL_HAPTIC_GAIN) != 0) {
        SDL_HapticSetGain(mHaptic, 100);
    }
    if ((supportedEffects & SDL_HAPTIC_AUTOCENTER) != 0 && ShouldUseSdlWheelCentering()) {
        SDL_HapticSetAutocenter(mHaptic, 0);
    }

    SPDLOG_INFO(
        "Wheel device '{}' haptics: effects=0x{:X}, spring={}, constant={}, periodic={}, rumble={}, axes={}",
        mDefinition.name, supportedEffects, mSupportsSteeringWeight, mSupportsConstantForce, mSupportsPeriodic,
        mSupportsRumble, SDL_HapticNumAxes(mHaptic));
}

void WheelDevice::InitializeLogitechSdkSpring() {
#ifdef _WIN32
    CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringActive", 0);
    if (mDefinition.vendorId != 0x046D) {
        return;
    }

    LogitechSteeringWheelSdk& sdk = LogitechSteeringWheelSdk::Instance();
    sdk.StartSpringWorker();
    sdk.PublishDebugCvars();
#else
    CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringActive", 0);
#endif
}

bool WheelDevice::UpdateLogitechSdkSpring(int32_t springPercent) {
#ifdef _WIN32
    if (mDefinition.vendorId != 0x046D) {
        return false;
    }

    LogitechSteeringWheelSdk& sdk = LogitechSteeringWheelSdk::Instance();
    sdk.StartSpringWorker();
    sdk.RequestSpringPercent(springPercent);
    sdk.PublishDebugCvars();

    int32_t observedSpringPercent = sdk.GetObservedSpringPercent();
    if (observedSpringPercent >= 0 && CVarGetInteger("gArcadeKart.LogitechSdkSpringSeeded", 0) == 0) {
        CVarSetFloat(WHEEL_LOGITECH_SDK_SPRING_CVAR, static_cast<float>(observedSpringPercent));
        CVarSetInteger("gArcadeKart.LogitechSdkSpringSeeded", 1);
    }

    if (springPercent == mLastLogitechSdkSpringPercent && SDL_GetTicks() < mNextLogitechSdkSpringUpdateTick) {
        CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringSkipped", 1);
        return true;
    }
    mLastLogitechSdkSpringPercent = springPercent;
    mNextLogitechSdkSpringUpdateTick = SDL_GetTicks() + 150;

    CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringWriteOk", sdk.HasWorkerStarted() ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringSkipped", 0);
    CVarSetInteger("gArcadeKart.DebugLogitechSdkSpringRequested", springPercent);
    return sdk.HasWorkerStarted();
#else
    (void) springPercent;
    return false;
#endif
}

void WheelDevice::UpdateProfilerCenteringSpring(const WheelForceFeedbackState& state, float speedRatio) {
#ifdef _WIN32
    if (!CVarGetInteger(WHEEL_PROFILER_SPRING_CVAR, true)) {
        CVarSetInteger("gArcadeKart.DebugSpringProfilerEnabled", 0);
        CVarSetInteger("gArcadeKart.DebugSpringProfilerSkipped", 2);
        return;
    }

    uint32_t now = SDL_GetTicks();
    if (now < mNextProfilerSpringUpdateTick) {
        CVarSetInteger("gArcadeKart.DebugSpringProfilerSkipped", 1);
        return;
    }

    CVarSetInteger("gArcadeKart.DebugSpringProfilerEnabled", 1);
    CVarSetInteger("gArcadeKart.DebugSpringProfilerSkipped", 0);
    float baselineMultiplier = std::clamp(CVarGetFloat(WHEEL_SPRING_BASELINE_MULTIPLIER_CVAR, 8.0f), 0.25f, 16.0f);
    float fallbackSpring = CVarGetFloat(WHEEL_PROFILER_MIN_SPRING_CVAR, 12.0f) * baselineMultiplier;
    float rawMinSpring = CVarGetFloat(WHEEL_LOGITECH_SDK_SPRING_CVAR, fallbackSpring);
    float minSpring = std::clamp(rawMinSpring, 0.0f, 100.0f);
    float characterMultiplier = 1.0f;
    float surfaceMultiplier = 1.0f;
    float targetSpring = minSpring;
    int32_t springPercent = static_cast<int32_t>(targetSpring * characterMultiplier * surfaceMultiplier);
    springPercent = std::clamp(springPercent, 0, 100);
    CVarSetFloat("gArcadeKart.DebugSpringRawBase", rawMinSpring);
    CVarSetFloat("gArcadeKart.DebugSpringBase", targetSpring);
    CVarSetFloat("gArcadeKart.DebugSpringCharacterMultiplier", characterMultiplier);
    CVarSetFloat("gArcadeKart.DebugSpringSurfaceMultiplier", surfaceMultiplier);
    CVarSetFloat("gArcadeKart.DebugSpringPercent", static_cast<float>(springPercent));
    CVarSetFloat("gArcadeKart.DebugSpringBaselineMultiplier", baselineMultiplier);
    CVarSetInteger("gArcadeKart.DebugSpringGrounded", state.grounded ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugSpringSurface", state.surfaceType);
    CVarSetInteger("gArcadeKart.DebugSpringProfilerRequested", springPercent);
    CVarSetInteger("gArcadeKart.DebugSpringProfilerLast", mLastProfilerSpringPercent);
    bool wroteSdkSpring = UpdateLogitechSdkSpring(springPercent);
    if (wroteSdkSpring) {
        CVarSetInteger("gArcadeKart.DebugSpringProfilerDriverWrite", 0);
        CVarSetInteger("gArcadeKart.DebugSpringProfilerGlobalWrite", 0);
        CVarSetInteger("gArcadeKart.DebugSpringProfilerWriteOk", 1);
        CVarSetInteger("gArcadeKart.DebugSpringProfilerSkipped", 4);
        mLastProfilerSpringPercent = springPercent;
        mNextProfilerSpringUpdateTick = now + 250;
        return;
    }
    if (mLastProfilerSpringPercent >= 0 && abs(springPercent - mLastProfilerSpringPercent) < 3) {
        CVarSetInteger("gArcadeKart.DebugSpringProfilerSkipped", 3);
        mNextProfilerSpringUpdateTick = now + 250;
        return;
    }

    char driverSubkey[128];
    snprintf(driverSubkey, sizeof(driverSubkey), "Software\\Logitech\\Gaming Software\\DriverSettings\\VID_%04X&PID_%04X",
             mDefinition.vendorId, mDefinition.productId);
    DWORD springDriverValue = static_cast<DWORD>(springPercent * 100);

    bool wroteDriverSpring = SetRegistryDword(HKEY_CURRENT_USER, driverSubkey, "PersistentCenteringSpring", 1) &&
                             SetRegistryDword(HKEY_CURRENT_USER, driverSubkey, "CenteringSpring", springDriverValue) &&
                             SetRegistryDword(HKEY_CURRENT_USER, driverSubkey, "SpringStrength", springDriverValue);
    bool wroteGlobalSpring =
        SetRegistryDword(HKEY_CURRENT_USER, "Software\\Logitech\\Gaming Software\\GlobalDeviceSettings\\G27",
                         "PersistentSpringEnable", 1) &&
        SetRegistryDword(HKEY_CURRENT_USER, "Software\\Logitech\\Gaming Software\\GlobalDeviceSettings\\G27",
                         "SpringGainPercentage", static_cast<DWORD>(springPercent)) &&
        SetRegistryDword(HKEY_CURRENT_USER, "Software\\Logitech\\Gaming Software\\GlobalDeviceSettings\\G27",
                         "DefaultSpringGainPercentage", static_cast<DWORD>(springPercent));
    CVarSetInteger("gArcadeKart.DebugSpringProfilerDriverWrite", wroteDriverSpring ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugSpringProfilerGlobalWrite", wroteGlobalSpring ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugSpringProfilerDriverValue", static_cast<int32_t>(springDriverValue));
    CVarSetInteger("gArcadeKart.DebugSpringProfilerWriteOk", (wroteDriverSpring || wroteGlobalSpring) ? 1 : 0);

    if (mHaptic != nullptr && ShouldUseSdlWheelCentering()) {
        int autocenterResult = SDL_HapticSetAutocenter(mHaptic, springPercent);
        CVarSetInteger("gArcadeKart.DebugSpringSdlAutocenterWriteOk", autocenterResult == 0 ? 1 : 0);
        CVarSetInteger("gArcadeKart.DebugSpringSdlAutocenterPercent", springPercent);
    } else {
        CVarSetInteger("gArcadeKart.DebugSpringSdlAutocenterWriteOk", 0);
        CVarSetInteger("gArcadeKart.DebugSpringSdlAutocenterPercent", -1);
    }

    if (wroteDriverSpring || wroteGlobalSpring) {
        mLastProfilerSpringPercent = springPercent;
    }
    mNextProfilerSpringUpdateTick = now + 250;
#else
    CVarSetFloat("gArcadeKart.DebugSpringBase", 0.0f);
    CVarSetFloat("gArcadeKart.DebugSpringCharacterMultiplier", 1.0f);
    CVarSetFloat("gArcadeKart.DebugSpringSurfaceMultiplier", 1.0f);
    CVarSetFloat("gArcadeKart.DebugSpringPercent", 0.0f);
    CVarSetInteger("gArcadeKart.DebugSpringGrounded", state.grounded ? 1 : 0);
    CVarSetInteger("gArcadeKart.DebugSpringSurface", state.surfaceType);
    (void) state;
#endif
    (void) speedRatio;
}

void WheelDevice::UpdateSteeringWeight(const WheelForceFeedbackState& state) {
    float baselineMultiplier = std::clamp(CVarGetFloat(WHEEL_SPRING_BASELINE_MULTIPLIER_CVAR, 8.0f), 0.25f, 16.0f);
    float fallbackSpring = CVarGetFloat(WHEEL_PROFILER_MIN_SPRING_CVAR, 12.0f) * baselineMultiplier;
    float minSpring = std::clamp(CVarGetFloat(WHEEL_LOGITECH_SDK_SPRING_CVAR, fallbackSpring), 0.0f, 100.0f);
    float centeringRatio = minSpring / 100.0f;
    UpdateProfilerCenteringSpring(state, centeringRatio);

    CVarSetInteger("gArcadeKart.DebugSpringSdlCenteringEnabled", ShouldUseSdlWheelCentering() ? 1 : 0);
    if (!ShouldUseSdlWheelCentering()) {
        if (mHaptic != nullptr && mSteeringWeightEffectId >= 0) {
            SDL_HapticStopEffect(mHaptic, mSteeringWeightEffectId);
            SDL_HapticDestroyEffect(mHaptic, mSteeringWeightEffectId);
            mSteeringWeightEffectId = -1;
        }
        if (mHaptic != nullptr && mCenteringForceEffectId >= 0) {
            SDL_HapticStopEffect(mHaptic, mCenteringForceEffectId);
            SDL_HapticDestroyEffect(mHaptic, mCenteringForceEffectId);
            mCenteringForceEffectId = -1;
        }
        CVarSetFloat("gArcadeKart.DebugSpringCenteringRatio", centeringRatio);
        CVarSetInteger("gArcadeKart.DebugSpringHapticEffectActive", 0);
        CVarSetInteger("gArcadeKart.DebugSpringHapticWriteOk", 0);
        CVarSetInteger("gArcadeKart.DebugSpringHapticEffectId", -1);
        return;
    }

    if (mHaptic == nullptr || (!mSupportsSteeringWeight && !mSupportsConstantForce)) {
        CVarSetInteger("gArcadeKart.DebugSpringHapticEffectActive", 0);
        return;
    }

    int16_t coefficient = static_cast<int16_t>(std::clamp(0x1200 * baselineMultiplier, 0.0f, 32767.0f));
    uint16_t saturation = static_cast<uint16_t>(std::clamp(0x3000 * baselineMultiplier, 0.0f, 65535.0f));
    uint16_t deadband = 0x0100;
    CVarSetFloat("gArcadeKart.DebugSpringCenteringRatio", centeringRatio);
    CVarSetInteger("gArcadeKart.DebugSpringHapticCoefficient", coefficient);
    CVarSetInteger("gArcadeKart.DebugSpringHapticSaturation", saturation);
    CVarSetInteger("gArcadeKart.DebugSpringHapticDeadband", deadband);
    CVarSetInteger("gArcadeKart.DebugSpringHapticEffectId", mSteeringWeightEffectId);

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
                CVarSetInteger("gArcadeKart.DebugSpringHapticEffectActive", 1);
                CVarSetInteger("gArcadeKart.DebugSpringHapticWriteOk", 1);
            } else {
                SPDLOG_WARN("Wheel device '{}' spring effect failed: {}", mDefinition.name, SDL_GetError());
                CVarSetInteger("gArcadeKart.DebugSpringHapticEffectActive", 0);
                CVarSetInteger("gArcadeKart.DebugSpringHapticWriteOk", 0);
                mSupportsSteeringWeight = false;
            }
        } else {
            int updateResult = SDL_HapticUpdateEffect(mHaptic, mSteeringWeightEffectId, &effect);
            CVarSetInteger("gArcadeKart.DebugSpringHapticWriteOk", updateResult == 0 ? 1 : 0);
            CVarSetInteger("gArcadeKart.DebugSpringHapticEffectActive", updateResult == 0 ? 1 : 0);
        }
        return;
    }

    if (mSupportsConstantForce) {
        PlaySignedConstantSteeringForce(-mLastSteeringInput * centeringRatio * 0.85f, 120, mCenteringForceEffectId);
    }
}

void WheelDevice::PlaySignedConstantSteeringForce(float signedStrength, uint32_t durationMs, int32_t& effectId) {
    if (mHaptic == nullptr || !mSupportsConstantForce) {
        return;
    }

    float clampedStrength = std::clamp(signedStrength, -1.0f, 1.0f);
    CVarSetFloat("gArcadeKart.DebugForceConstantSigned", clampedStrength);
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
    CVarSetFloat("gArcadeKart.DebugForceTerrainKick", steeringKickStrength);
    CVarSetInteger("gArcadeKart.DebugForceTerrainKickInterval", static_cast<int32_t>(kickInterval));
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

    CVarSetFloat("gArcadeKart.DebugForceCoarseKick", steeringKickStrength);
    CVarSetInteger("gArcadeKart.DebugForceCoarseKickInterval", static_cast<int32_t>(randomInterval));
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
    CVarSetFloat("gArcadeKart.DebugForceSurfaceRumble", rumbleStrength);
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

    UpdateSteeringWeight(state);
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

int32_t WheelDevice::SmoothWheelShifterGear(int32_t rawGear) {
    const uint32_t smoothingFrames =
        static_cast<uint32_t>(std::clamp(CVarGetInteger(WHEEL_SHIFTER_SMOOTHING_FRAMES_CVAR, 4), 1, 8));
    uint32_t neutralCount = 0;
    int32_t bestGear = rawGear;
    int32_t bestCount = -1;
    int32_t bestNewestRank = -1;

    mShifterGearHistory[mShifterGearHistoryIndex] = rawGear;
    mShifterGearHistoryIndex = (mShifterGearHistoryIndex + 1) % 8;
    if (mShifterGearHistoryCount < 8) {
        mShifterGearHistoryCount++;
    }

    for (uint32_t i = 0; i < smoothingFrames; i++) {
        uint32_t candidateIndex = (mShifterGearHistoryIndex + 8 - 1 - i) % 8;
        int32_t candidate = mShifterGearHistory[candidateIndex];
        int32_t count = 0;
        int32_t newestRank = -1;

        for (uint32_t age = 0; age < smoothingFrames; age++) {
            uint32_t historyIndex = (mShifterGearHistoryIndex + 8 - 1 - age) % 8;
            if (mShifterGearHistory[historyIndex] == candidate) {
                count++;
                if (newestRank < 0) {
                    newestRank = (int32_t) (smoothingFrames - age);
                }
            }
        }

        if ((count > bestCount) || ((count == bestCount) && (newestRank > bestNewestRank))) {
            bestGear = candidate;
            bestCount = count;
            bestNewestRank = newestRank;
        }
        if (candidate == WHEEL_GEAR_NEUTRAL) {
            neutralCount = (uint32_t) count;
        }
    }

    CVarSetInteger("gArcadeKart.DebugShifterRawGear", rawGear);
    CVarSetInteger("gArcadeKart.DebugShifterSmoothedGear", bestGear);
    CVarSetInteger("gArcadeKart.DebugShifterSmoothingFrames", smoothingFrames);
    CVarSetInteger("gArcadeKart.DebugShifterNeutralSamples", neutralCount);
    return bestGear;
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
    reading.jump = ReadButton(mDefinition.jumpButton) || ReadButton(mDefinition.alternateJumpButton);
    reading.toggleHud = ReadButton(mDefinition.toggleHudButton);
    reading.toggleMusic = ReadButton(mDefinition.toggleMusicButton);
    reading.useItemBackward = ReadButton(mDefinition.useItemBackwardButton);
    reading.useItemForward = ReadButton(mDefinition.useItemForwardButton);

    if (mDefinition.gear1Button >= 0) {
        int32_t shifterButtonMask = 0;
        int32_t rawRequestedGear = WHEEL_GEAR_NEUTRAL;
        shifterButtonMask |= ReadButton(mDefinition.gear1Button) ? (1 << 0) : 0;
        shifterButtonMask |= ReadButton(mDefinition.gear2Button) ? (1 << 1) : 0;
        shifterButtonMask |= ReadButton(mDefinition.gear3Button) ? (1 << 2) : 0;
        shifterButtonMask |= ReadButton(mDefinition.gear4Button) ? (1 << 3) : 0;
        shifterButtonMask |= ReadButton(mDefinition.gear5Button) ? (1 << 4) : 0;
        shifterButtonMask |= ReadButton(mDefinition.gear6Button) ? (1 << 5) : 0;
        shifterButtonMask |= ReadButton(mDefinition.reverseButton) ? (1 << 6) : 0;

        if (ReadButton(mDefinition.gear1Button)) {
            rawRequestedGear = 1;
        } else if (ReadButton(mDefinition.gear2Button)) {
            rawRequestedGear = 2;
        } else if (ReadButton(mDefinition.gear3Button)) {
            rawRequestedGear = 3;
        } else if (ReadButton(mDefinition.gear4Button)) {
            rawRequestedGear = 4;
        } else if (ReadButton(mDefinition.gear5Button)) {
            rawRequestedGear = 5;
        } else if (ReadButton(mDefinition.gear6Button)) {
            rawRequestedGear = 6;
        } else if (ReadButton(mDefinition.reverseButton)) {
            rawRequestedGear = WHEEL_GEAR_REVERSE;
        }

        int32_t pressedGearCount = 0;
        for (int32_t i = 0; i < 7; i++) {
            pressedGearCount += (shifterButtonMask & (1 << i)) != 0 ? 1 : 0;
        }
        if (pressedGearCount > 1) {
            rawRequestedGear = WHEEL_GEAR_NONE;
        }
        reading.requestedGear = SmoothWheelShifterGear(rawRequestedGear);
        if (shifterButtonMask != mLastShifterButtonMask || reading.requestedGear != mLastRequestedGear) {
            CVarSetInteger("gArcadeKart.DebugShifterButtonMask", shifterButtonMask);
            CVarSetInteger("gArcadeKart.DebugShifterRequestedGear", reading.requestedGear);
            CVarSetInteger("gArcadeKart.DebugShifterPressedGearCount", pressedGearCount);
            if (pressedGearCount > 1) {
                SPDLOG_WARN("Wheel shifter {} reports multiple gears at once: mask=0x{:02X}, rawGear={}, smoothedGear={}",
                            mDefinition.name, shifterButtonMask, rawRequestedGear, reading.requestedGear);
            } else {
                SPDLOG_INFO("Wheel shifter {} mask=0x{:02X}, rawGear={}, smoothedGear={}", mDefinition.name,
                            shifterButtonMask, rawRequestedGear, reading.requestedGear);
            }
            mLastShifterButtonMask = shifterButtonMask;
            mLastRequestedGear = reading.requestedGear;
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
          WheelDevice({ "G27 Racing Wheel", 0x046D, 0xC294, 0, 1, 2, 4, -1, true, true, true, 0, 0, 1, 3, 6, 7, 2,
                        20, 5, 4, 8, 9, 10, 11, 12, 13, 14 }),
          WheelDevice({ "G27 Racing Wheel", 0x046D, 0xC29B, 0, 1, 2, 4, -1, true, true, true, 0, 0, 1, 3, 6, 7, 2,
                        20, 5, 4, 8, 9, 10, 11, 12, 13, 14 }),
          WheelDevice({ "ODDOR-HANDBRAKE", 0x1021, 0x1888, -1, -1, -1, -1, 0, false, false, false, -1, -1, -1, -1,
                        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }),
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
        mergedReading.jump = mergedReading.jump || reading.jump;
        mergedReading.toggleHud = mergedReading.toggleHud || reading.toggleHud;
        mergedReading.toggleMusic = mergedReading.toggleMusic || reading.toggleMusic;
        mergedReading.useItemBackward = mergedReading.useItemBackward || reading.useItemBackward;
        mergedReading.useItemForward = mergedReading.useItemForward || reading.useItemForward;
        if (reading.requestedGear != WHEEL_GEAR_NONE) {
            mergedReading.requestedGear = reading.requestedGear;
        }
    }

    return mergedReading;
}

void WheelDeviceManager::UpdatePlayerOneMenuForceFeedback() {
    RefreshDevices();

    WheelForceFeedbackState state = { 0.0f, 0.0f, 1.0f, true, false, false, SURFACE_ASPHALT, -1, true };
    for (auto& device : mDevices) {
        device.UpdateForceFeedback(state);
    }
}

void WheelDeviceManager::UpdatePlayerOneForceFeedback(float speedKmh, float slopeSteeringForce, bool grounded,
                                                      bool hitByItem, bool hitByLightning, uint16_t surfaceType,
                                                      int16_t courseId, float steeringSpringMultiplier) {
    RefreshDevices();

    WheelForceFeedbackState state = { speedKmh, slopeSteeringForce, steeringSpringMultiplier, grounded, hitByItem,
                                      hitByLightning, surfaceType, courseId, false };
    for (auto& device : mDevices) {
        device.UpdateForceFeedback(state);
    }
}

} // namespace LUS
