#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <SDL2/SDL.h>

namespace LUS {

struct WheelDeviceDefinition {
    std::string name;
    uint16_t vendorId;
    uint16_t productId;
    int32_t steeringAxis;
    int32_t throttleAxis;
    int32_t brakeAxis;
    int32_t clutchAxis;
    int32_t handbrakeAxis;
    bool invertThrottleAxis;
    bool invertBrakeAxis;
    bool invertClutchAxis;
    int32_t povHat;
    int32_t menuConfirmButton;
    int32_t menuCancelButton;
    int32_t openMenuButton;
    int32_t jumpButton;
    int32_t alternateJumpButton;
    int32_t toggleHudButton;
    int32_t toggleMusicButton;
    int32_t useItemBackwardButton;
    int32_t useItemForwardButton;
    int32_t gear1Button;
    int32_t gear2Button;
    int32_t gear3Button;
    int32_t gear4Button;
    int32_t gear5Button;
    int32_t gear6Button;
    int32_t reverseButton;
};

struct WheelReading {
    bool connected;
    float steering;
    float throttle;
    float brake;
    float clutch;
    float handbrake;
    bool menuUp;
    bool menuDown;
    bool menuLeft;
    bool menuRight;
    bool menuConfirm;
    bool menuCancel;
    bool openMenu;
    bool jump;
    bool toggleHud;
    bool toggleMusic;
    bool useItemBackward;
    bool useItemForward;
    int32_t requestedGear;
};

struct WheelForceFeedbackState {
    float speedKmh;
    float slopeSteeringForce;
    bool grounded;
    bool hitByItem;
    bool hitByLightning;
    uint16_t surfaceType;
    int16_t courseId;
};

class WheelDevice {
  public:
    explicit WheelDevice(WheelDeviceDefinition definition);

    bool Matches(int32_t deviceIndex) const;
    bool IsOpen() const;
    bool Open(int32_t deviceIndex);
    WheelReading Read();
    void UpdateForceFeedback(const WheelForceFeedbackState& state);

  private:
    void InitializeHaptics();
    void UpdateSteeringWeight(float speedKmh, bool grounded);
    void UpdateSurfaceRumble(const WheelForceFeedbackState& state);
    void UpdateTerrainSteeringKnock(const WheelForceFeedbackState& state);
    void UpdateCoarseTerrainSteeringKnock(const WheelForceFeedbackState& state);
    void UpdateSlopeSteeringBias(const WheelForceFeedbackState& state);
    void PlayPeriodicFeedback(float strength, uint32_t durationMs, uint16_t periodMs);
    void PlayConstantSteeringKick(float strength, uint32_t durationMs);
    void PlaySignedConstantSteeringForce(float signedStrength, uint32_t durationMs, int32_t& effectId);
    float ReadSignedAxis(int32_t axisIndex) const;
    float ReadPositiveAxis(int32_t axisIndex) const;
    float ReadPedalAxis(int32_t axisIndex, bool inverted) const;
    bool ReadButton(int32_t buttonIndex) const;

    WheelDeviceDefinition mDefinition;
    SDL_Joystick* mJoystick;
    SDL_Haptic* mHaptic;
    int32_t mSteeringWeightEffectId;
    int32_t mCenteringForceEffectId;
    int32_t mPeriodicEffectId;
    int32_t mTerrainKickEffectId;
    int32_t mSlopeForceEffectId;
    int32_t mCoarseTerrainKickEffectId;
    uint32_t mNextRumbleTick;
    uint32_t mNextTerrainKickTick;
    uint32_t mNextCoarseTerrainKickTick;
    uint32_t mLightningFeedbackUntilTick;
    uint32_t mFeedbackNoiseState;
    float mLastSteeringInput;
    int32_t mTerrainKickDirection;
    bool mSupportsSteeringWeight;
    bool mSupportsConstantForce;
    bool mSupportsPeriodic;
    bool mSupportsRumble;
    bool mLastHitByItem;
    bool mShifterWasInGear;
};

class WheelDeviceManager {
  public:
    static WheelDeviceManager& Instance();

    WheelReading ReadPlayerOneWheel();
    void UpdatePlayerOneForceFeedback(float speedKmh, float slopeSteeringForce, bool grounded, bool hitByItem,
                                      bool hitByLightning, uint16_t surfaceType, int16_t courseId);

  private:
    WheelDeviceManager();
    void RefreshDevices();

    std::vector<WheelDevice> mDevices;
    int32_t mLastJoystickCount;
};

} // namespace LUS
