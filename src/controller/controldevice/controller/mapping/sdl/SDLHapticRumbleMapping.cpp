#include "SDLHapticRumbleMapping.h"

#include "public/bridge/consolevariablebridge.h"
#include "utils/StringHelper.h"
#include "Context.h"
#include "controller/controldeck/ControlDeck.h"

namespace Ship {
SDLHapticRumbleMapping::SDLHapticRumbleMapping(uint8_t portIndex, uint8_t lowFrequencyIntensityPercentage,
                                               uint8_t highFrequencyIntensityPercentage)
    : ControllerRumbleMapping(PhysicalDeviceType::SDLGamepad, portIndex, lowFrequencyIntensityPercentage,
                              highFrequencyIntensityPercentage) {
}

SDLHapticRumbleMapping::~SDLHapticRumbleMapping() {
    StopRumble();
}

void SDLHapticRumbleMapping::StartRumble() {
    for (const auto& [instanceId, haptic] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLHapticsForPort(
             mPortIndex)) {
        if (mEffectIds.contains(instanceId)) {
            if (mRunningEffectIds.contains(instanceId)) {
                continue;
            }
            SDL_HapticRunEffect(haptic, mEffectIds[instanceId], 1);
            mRunningEffectIds.insert(instanceId);
            continue;
        }

        auto effect = BuildEffect(haptic);
        auto effectId = SDL_HapticNewEffect(haptic, &effect);
        if (effectId >= 0) {
            mEffectIds[instanceId] = effectId;
            SDL_HapticRunEffect(haptic, effectId, 1);
            mRunningEffectIds.insert(instanceId);
        }
    }
}

void SDLHapticRumbleMapping::StopRumble() {
    for (const auto& [instanceId, haptic] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLHapticsForPort(
             mPortIndex)) {
        if (!mEffectIds.contains(instanceId)) {
            continue;
        }

        SDL_HapticStopEffect(haptic, mEffectIds[instanceId]);
        SDL_HapticDestroyEffect(haptic, mEffectIds[instanceId]);
        mEffectIds.erase(instanceId);
        mRunningEffectIds.erase(instanceId);
    }
}

void SDLHapticRumbleMapping::SetLowFrequencyIntensity(uint8_t intensityPercentage) {
    mLowFrequencyIntensityPercentage = intensityPercentage;
    StopRumble();
}

void SDLHapticRumbleMapping::SetHighFrequencyIntensity(uint8_t intensityPercentage) {
    mHighFrequencyIntensityPercentage = intensityPercentage;
    StopRumble();
}

std::string SDLHapticRumbleMapping::GetRumbleMappingId() {
    return StringHelper::Sprintf("P%d-Haptic", mPortIndex);
}

void SDLHapticRumbleMapping::SaveToConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".RumbleMappings." + GetRumbleMappingId();
    CVarSetString(StringHelper::Sprintf("%s.RumbleMappingClass", mappingCvarKey.c_str()).c_str(),
                  "SDLHapticRumbleMapping");
    CVarSetInteger(StringHelper::Sprintf("%s.LowFrequencyIntensity", mappingCvarKey.c_str()).c_str(),
                   mLowFrequencyIntensityPercentage);
    CVarSetInteger(StringHelper::Sprintf("%s.HighFrequencyIntensity", mappingCvarKey.c_str()).c_str(),
                   mHighFrequencyIntensityPercentage);
    CVarSave();
}

void SDLHapticRumbleMapping::EraseFromConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".RumbleMappings." + GetRumbleMappingId();

    CVarClear(StringHelper::Sprintf("%s.RumbleMappingClass", mappingCvarKey.c_str()).c_str());
    CVarClear(StringHelper::Sprintf("%s.LowFrequencyIntensity", mappingCvarKey.c_str()).c_str());
    CVarClear(StringHelper::Sprintf("%s.HighFrequencyIntensity", mappingCvarKey.c_str()).c_str());

    CVarSave();
}

std::string SDLHapticRumbleMapping::GetPhysicalDeviceName() {
    return "SDL Haptic Joystick";
}

SDL_HapticEffect SDLHapticRumbleMapping::BuildEffect(SDL_Haptic* haptic) {
    SDL_HapticEffect effect;
    memset(&effect, 0, sizeof(effect));

    const auto features = SDL_HapticQuery(haptic);
    const auto magnitude = GetMagnitude();

    if (features & SDL_HAPTIC_SINE) {
        effect.type = SDL_HAPTIC_SINE;
        effect.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
        effect.periodic.direction.dir[0] = 1;
        effect.periodic.direction.dir[1] = 0;
        effect.periodic.period = 40;
        effect.periodic.magnitude = magnitude;
        effect.periodic.length = SDL_HAPTIC_INFINITY;
        effect.periodic.attack_length = 15;
        effect.periodic.fade_length = 0;
        return effect;
    }

    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.constant.direction.dir[0] = 1;
    effect.constant.direction.dir[1] = 0;
    effect.constant.length = SDL_HAPTIC_INFINITY;
    effect.constant.level = magnitude;
    effect.constant.attack_length = 10;
    effect.constant.fade_length = 0;
    return effect;
}

uint16_t SDLHapticRumbleMapping::GetMagnitude() const {
    const auto intensity =
        (mLowFrequencyIntensityPercentage + mHighFrequencyIntensityPercentage) / 2.0f / 100.0f;
    return static_cast<uint16_t>(INT16_MAX * intensity);
}
} // namespace Ship
