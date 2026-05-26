#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToAxisDirectionMapping.h"
#include <spdlog/spdlog.h>
#include "ship/utils/StringHelper.h"
#include "ship/window/gui/IconsFontAwesome4.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include <cmath>

#define MAX_SDL_RANGE (float)INT16_MAX
#define KART_CONTROLLER_SLOPE_STEERING_FORCE_CVAR "gArcadeKart.ControllerSlopeSteeringForce"
#define KART_MAX_SLOPE_STEERING_EXPONENT 3.0f

namespace Ship {
SDLAxisDirectionToAxisDirectionMapping::SDLAxisDirectionToAxisDirectionMapping(uint8_t portIndex, StickIndex stickIndex,
                                                                               Direction direction,
                                                                               int32_t sdlControllerAxis,
                                                                               int32_t axisDirection)
    : ControllerInputMapping(PhysicalDeviceType::SDLGamepad),
      ControllerAxisDirectionMapping(PhysicalDeviceType::SDLGamepad, portIndex, stickIndex, direction),
      SDLAxisDirectionToAnyMapping(sdlControllerAxis, axisDirection) {
}

static float ApplySlopeSteeringResistance(float axisDirectionValue, StickIndex stickIndex, Direction direction) {
    if (stickIndex != LEFT_STICK || (direction != LEFT && direction != RIGHT)) {
        return axisDirectionValue;
    }

    float slopeSteeringForce =
        Context::GetInstance()->GetConsoleVariables()->GetFloat(KART_CONTROLLER_SLOPE_STEERING_FORCE_CVAR, 0.0f);
    bool isFightingSlope = (slopeSteeringForce > 0.0f && direction == LEFT) ||
                           (slopeSteeringForce < 0.0f && direction == RIGHT);
    if (!isFightingSlope) {
        return axisDirectionValue;
    }

    float slopeResistance = std::clamp(fabs(slopeSteeringForce) / 0.75f, 0.0f, 1.0f);
    float tuningExponent = 1.0f + (slopeResistance * (KART_MAX_SLOPE_STEERING_EXPONENT - 1.0f));
    float normalizedInput = std::clamp(axisDirectionValue / MAX_AXIS_RANGE, 0.0f, 1.0f);
    float tunedInput = powf(normalizedInput, tuningExponent);
    return tunedInput * MAX_AXIS_RANGE;
}

float SDLAxisDirectionToAxisDirectionMapping::GetNormalizedAxisDirectionValue() {
    if (Context::GetInstance()->GetControlDeck()->GamepadGameInputBlocked()) {
        return 0.0f;
    }

    // todo: i don't like making a vector here, not sure what a better solution is
    std::vector<float> normalizedValues = {};
    for (const auto& [instanceId, gamepad] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(
             mPortIndex)) {
        const auto axisValue = SDL_GameControllerGetAxis(gamepad, mControllerAxis);

        if ((mAxisDirection == POSITIVE && axisValue < 0) || (mAxisDirection == NEGATIVE && axisValue > 0)) {
            normalizedValues.push_back(0.0f);
            continue;
        }

        // scale {-32768 ... +32767} to {-MAX_AXIS_RANGE ... +MAX_AXIS_RANGE}
        // and use the absolute value of it
        normalizedValues.push_back(
            ApplySlopeSteeringResistance(fabs(axisValue * MAX_AXIS_RANGE / MAX_SDL_RANGE), mStickIndex, mDirection));
    }

    if (normalizedValues.size() == 0) {
        return 0.0f;
    }

    return *std::max_element(normalizedValues.begin(), normalizedValues.end());
}

std::string SDLAxisDirectionToAxisDirectionMapping::GetAxisDirectionMappingId() {
    return StringHelper::Sprintf("P%d-S%d-D%d-SDLA%d-AD%s", mPortIndex, mStickIndex, mDirection, mControllerAxis,
                                 mAxisDirection == 1 ? "P" : "N");
}

void SDLAxisDirectionToAxisDirectionMapping::SaveToConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".AxisDirectionMappings." + GetAxisDirectionMappingId();
    Ship::Context::GetInstance()->GetConsoleVariables()->SetString(
        StringHelper::Sprintf("%s.AxisDirectionMappingClass", mappingCvarKey.c_str()).c_str(),
        "SDLAxisDirectionToAxisDirectionMapping");
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.Stick", mappingCvarKey.c_str()).c_str(), mStickIndex);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.Direction", mappingCvarKey.c_str()).c_str(), mDirection);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.SDLControllerAxis", mappingCvarKey.c_str()).c_str(), mControllerAxis);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.AxisDirection", mappingCvarKey.c_str()).c_str(), mAxisDirection);
    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

void SDLAxisDirectionToAxisDirectionMapping::EraseFromConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".AxisDirectionMappings." + GetAxisDirectionMappingId();
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.Stick", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.Direction", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.AxisDirectionMappingClass", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.SDLControllerAxis", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.AxisDirection", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

int8_t SDLAxisDirectionToAxisDirectionMapping::GetMappingType() {
    return MAPPING_TYPE_GAMEPAD;
}

std::string SDLAxisDirectionToAxisDirectionMapping::GetPhysicalDeviceName() {
    return SDLAxisDirectionToAnyMapping::GetPhysicalDeviceName();
}

std::string SDLAxisDirectionToAxisDirectionMapping::GetPhysicalInputName() {
    return SDLAxisDirectionToAnyMapping::GetPhysicalInputName();
}
} // namespace Ship
