#include "public/bridge/controllerbridge.h"
#include "public/bridge/consolevariablebridge.h"
#include "controller/controldeck/ControlDeck.h"
#include "Context.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace {

struct PersistentEffectIds {
    int32_t spring = -1;
    int32_t damper = -1;
    uint16_t springStrength = 0;
    uint16_t damperStrength = 0;
    int32_t autocenter = -1;
    int32_t gain = -1;
    bool enabled = false;
};

std::unordered_map<uint8_t, std::unordered_map<int32_t, PersistentEffectIds>> sPersistentEffects;
std::unordered_map<uint8_t, std::unordered_map<int32_t, std::vector<int32_t>>> sTransientEffects;

uint16_t ClampStrength(uint16_t strength) {
    return std::min<uint16_t>(strength, INT16_MAX);
}

int32_t StrengthToPercent(uint16_t strength) {
    if (strength == 0) {
        return 0;
    }

    return std::clamp(static_cast<int32_t>(30 + ((ClampStrength(strength) * 70) / INT16_MAX)), 0, 100);
}

void SetDirection(SDL_HapticDirection& direction, float x, float y) {
    direction.type = SDL_HAPTIC_CARTESIAN;
    direction.dir[0] = static_cast<Sint32>(std::clamp(x, -1.0f, 1.0f) * INT16_MAX);
    direction.dir[1] = static_cast<Sint32>(std::clamp(y, -1.0f, 1.0f) * INT16_MAX);
    direction.dir[2] = 0;
}

SDL_HapticEffect BuildConditionEffect(uint16_t type, uint16_t strength) {
    SDL_HapticEffect effect;
    memset(&effect, 0, sizeof(effect));

    effect.type = type;
    effect.condition.type = type;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    SetDirection(effect.condition.direction, 0.0f, 0.0f);

    const auto coeff = static_cast<Sint16>(ClampStrength(strength));
    for (uint8_t axis = 0; axis < 2; axis++) {
        effect.condition.right_sat[axis] = UINT16_MAX;
        effect.condition.left_sat[axis] = UINT16_MAX;
        effect.condition.right_coeff[axis] = coeff;
        effect.condition.left_coeff[axis] = coeff;
        effect.condition.deadband[axis] = 0;
        effect.condition.center[axis] = 0;
    }

    return effect;
}

SDL_HapticEffect BuildPeriodicEffect(uint16_t type, float directionX, float directionY, uint16_t strength,
                                     uint16_t periodMs, uint16_t lengthMs, uint16_t fadeMs) {
    SDL_HapticEffect effect;
    memset(&effect, 0, sizeof(effect));
    effect.type = type;
    effect.periodic.type = type;
    SetDirection(effect.periodic.direction, directionX, directionY);
    effect.periodic.length = lengthMs;
    effect.periodic.period = std::max<uint16_t>(periodMs, 10);
    effect.periodic.magnitude = ClampStrength(strength);
    effect.periodic.attack_length = std::min<uint16_t>(lengthMs / 6, 15);
    effect.periodic.fade_length = std::min<uint16_t>(fadeMs, lengthMs);
    return effect;
}

void DestroyPersistentEffect(SDL_Haptic* haptic, int32_t& effectId) {
    if (effectId < 0) {
        return;
    }

    SDL_HapticStopEffect(haptic, effectId);
    SDL_HapticDestroyEffect(haptic, effectId);
    effectId = -1;
}

void TrackTransientEffect(uint8_t port, int32_t instanceId, SDL_Haptic* haptic, int32_t effectId) {
    if (effectId < 0) {
        return;
    }

    auto& effects = sTransientEffects[port][instanceId];
    effects.push_back(effectId);
    while (effects.size() > 4) {
        SDL_HapticStopEffect(haptic, effects.front());
        SDL_HapticDestroyEffect(haptic, effects.front());
        effects.erase(effects.begin());
    }
}

void DestroyTransientEffects(uint8_t port, int32_t instanceId, SDL_Haptic* haptic) {
    for (auto effectId : sTransientEffects[port][instanceId]) {
        SDL_HapticStopEffect(haptic, effectId);
        SDL_HapticDestroyEffect(haptic, effectId);
    }
    sTransientEffects[port][instanceId].clear();
}

} // namespace

extern "C" {

void ControllerBlockGameInput(uint16_t inputBlockId) {
    Ship::Context::GetInstance()->GetControlDeck()->BlockGameInput(static_cast<int32_t>(inputBlockId));
}

void ControllerUnblockGameInput(uint16_t inputBlockId) {
    Ship::Context::GetInstance()->GetControlDeck()->UnblockGameInput(static_cast<int32_t>(inputBlockId));
}

uint8_t ControllerHasForceFeedback(uint8_t port) {
    return !Ship::Context::GetInstance()
                ->GetControlDeck()
                ->GetConnectedPhysicalDeviceManager()
                ->GetConnectedSDLHapticsForPort(port)
                .empty();
}

void ControllerFFBPlayConstant(uint8_t port, float directionX, float directionY, uint16_t strength, uint16_t lengthMs) {
    ControllerFFBPlayConstantWithEnvelope(port, directionX, directionY, strength, lengthMs,
                                          std::min<uint16_t>(lengthMs / 5, 20), std::min<uint16_t>(lengthMs / 2, 80));
}

void ControllerFFBPlayConstantWithEnvelope(uint8_t port, float directionX, float directionY, uint16_t strength,
                                           uint16_t lengthMs, uint16_t attackMs, uint16_t fadeMs) {
    for (const auto& [instanceId, haptic] : Ship::Context::GetInstance()
                                             ->GetControlDeck()
                                             ->GetConnectedPhysicalDeviceManager()
                                             ->GetConnectedSDLHapticsForPort(port)) {
        if (!(SDL_HapticQuery(haptic) & SDL_HAPTIC_CONSTANT)) {
            continue;
        }

        SDL_HapticEffect effect;
        memset(&effect, 0, sizeof(effect));
        effect.type = SDL_HAPTIC_CONSTANT;
        effect.constant.type = SDL_HAPTIC_CONSTANT;
        SetDirection(effect.constant.direction, directionX, directionY);
        effect.constant.length = lengthMs;
        effect.constant.level = static_cast<Sint16>(ClampStrength(strength));
        effect.constant.attack_length = std::min<uint16_t>(attackMs, lengthMs);
        effect.constant.fade_length = std::min<uint16_t>(fadeMs, lengthMs);

        auto effectId = SDL_HapticNewEffect(haptic, &effect);
        if (effectId >= 0) {
            SDL_HapticRunEffect(haptic, effectId, 1);
            TrackTransientEffect(port, instanceId, haptic, effectId);
        }
    }
}

void ControllerFFBPlayPeriodic(uint8_t port, float directionX, float directionY, uint16_t strength, uint16_t periodMs,
                               uint16_t lengthMs) {
    ControllerFFBPlayPeriodicWithFade(port, directionX, directionY, strength, periodMs, lengthMs,
                                      std::min<uint16_t>(lengthMs / 2, 90));
}

void ControllerFFBPlayPeriodicWithFade(uint8_t port, float directionX, float directionY, uint16_t strength,
                                       uint16_t periodMs, uint16_t lengthMs, uint16_t fadeMs) {
    for (const auto& [instanceId, haptic] : Ship::Context::GetInstance()
                                             ->GetControlDeck()
                                             ->GetConnectedPhysicalDeviceManager()
                                             ->GetConnectedSDLHapticsForPort(port)) {
        if (!(SDL_HapticQuery(haptic) & SDL_HAPTIC_SINE)) {
            continue;
        }

        auto effect = BuildPeriodicEffect(SDL_HAPTIC_SINE, directionX, directionY, strength, periodMs, lengthMs, fadeMs);

        auto effectId = SDL_HapticNewEffect(haptic, &effect);
        if (effectId >= 0) {
            SDL_HapticRunEffect(haptic, effectId, 1);
            TrackTransientEffect(port, instanceId, haptic, effectId);
        }
    }
}

void ControllerFFBUpdateSpringDamper(uint8_t port, uint8_t enabled, uint16_t springStrength, uint16_t damperStrength) {
    for (const auto& [instanceId, haptic] : Ship::Context::GetInstance()
                                             ->GetControlDeck()
                                             ->GetConnectedPhysicalDeviceManager()
                                             ->GetConnectedSDLHapticsForPort(port)) {
        auto& state = sPersistentEffects[port][instanceId];
        const auto features = SDL_HapticQuery(haptic);
        if (!enabled) {
            DestroyPersistentEffect(haptic, state.spring);
            DestroyPersistentEffect(haptic, state.damper);
            if ((features & SDL_HAPTIC_AUTOCENTER) && state.autocenter != 0) {
                SDL_HapticSetAutocenter(haptic, 0);
                state.autocenter = 0;
            }
            state.enabled = false;
            continue;
        }

        const auto autocenter = StrengthToPercent(springStrength);
        if ((features & SDL_HAPTIC_AUTOCENTER) && state.autocenter != autocenter) {
            SDL_HapticSetAutocenter(haptic, autocenter);
            state.autocenter = autocenter;
        }

        const auto gain = std::max(78, StrengthToPercent(damperStrength));
        if ((features & SDL_HAPTIC_GAIN) && state.gain != gain) {
            SDL_HapticSetGain(haptic, gain);
            state.gain = gain;
        }

        if (CVarGetInteger("gSidewinderFFB.ConditionEffects", 0) == 0) {
            DestroyPersistentEffect(haptic, state.spring);
            DestroyPersistentEffect(haptic, state.damper);
            state.springStrength = springStrength;
            state.damperStrength = damperStrength;
            state.enabled = true;
            continue;
        }

        if (!state.enabled || state.springStrength != springStrength) {
            DestroyPersistentEffect(haptic, state.spring);
            if (features & SDL_HAPTIC_SPRING) {
                auto effect = BuildConditionEffect(SDL_HAPTIC_SPRING, springStrength);
                state.spring = SDL_HapticNewEffect(haptic, &effect);
                if (state.spring >= 0) {
                    SDL_HapticRunEffect(haptic, state.spring, 1);
                }
            }
            state.springStrength = springStrength;
        }

        if (!state.enabled || state.damperStrength != damperStrength) {
            DestroyPersistentEffect(haptic, state.damper);
            if (features & SDL_HAPTIC_DAMPER) {
                auto effect = BuildConditionEffect(SDL_HAPTIC_DAMPER, damperStrength);
                state.damper = SDL_HapticNewEffect(haptic, &effect);
                if (state.damper >= 0) {
                    SDL_HapticRunEffect(haptic, state.damper, 1);
                }
            }
            state.damperStrength = damperStrength;
        }

        state.enabled = true;
    }
}

void ControllerFFBStopAll(uint8_t port) {
    for (const auto& [instanceId, haptic] : Ship::Context::GetInstance()
                                             ->GetControlDeck()
                                             ->GetConnectedPhysicalDeviceManager()
                                             ->GetConnectedSDLHapticsForPort(port)) {
        if (sPersistentEffects[port].contains(instanceId)) {
            DestroyPersistentEffect(haptic, sPersistentEffects[port][instanceId].spring);
            DestroyPersistentEffect(haptic, sPersistentEffects[port][instanceId].damper);
            if (SDL_HapticQuery(haptic) & SDL_HAPTIC_AUTOCENTER) {
                SDL_HapticSetAutocenter(haptic, 0);
                sPersistentEffects[port][instanceId].autocenter = 0;
            }
            sPersistentEffects[port][instanceId].enabled = false;
        }
        DestroyTransientEffects(port, instanceId, haptic);
        SDL_HapticStopAll(haptic);
    }
}
}
