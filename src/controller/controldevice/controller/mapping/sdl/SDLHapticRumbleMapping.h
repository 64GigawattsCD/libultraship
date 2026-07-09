#pragma once

#include "controller/controldevice/controller/mapping/ControllerRumbleMapping.h"
#include <SDL2/SDL.h>
#include <unordered_map>
#include <unordered_set>

namespace Ship {
class SDLHapticRumbleMapping final : public ControllerRumbleMapping {
  public:
    SDLHapticRumbleMapping(uint8_t portIndex, uint8_t lowFrequencyIntensityPercentage,
                           uint8_t highFrequencyIntensityPercentage);
    ~SDLHapticRumbleMapping();

    void StartRumble() override;
    void StopRumble() override;
    void SetLowFrequencyIntensity(uint8_t intensityPercentage) override;
    void SetHighFrequencyIntensity(uint8_t intensityPercentage) override;

    std::string GetRumbleMappingId() override;
    void SaveToConfig() override;
    void EraseFromConfig() override;

    std::string GetPhysicalDeviceName() override;

  private:
    SDL_HapticEffect BuildEffect(SDL_Haptic* haptic);
    uint16_t GetMagnitude() const;

    std::unordered_map<int32_t, int32_t> mEffectIds;
    std::unordered_set<int32_t> mRunningEffectIds;
};
} // namespace Ship
