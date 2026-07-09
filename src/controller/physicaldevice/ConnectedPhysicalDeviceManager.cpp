#include "ConnectedPhysicalDeviceManager.h"

namespace Ship {
namespace {

bool IsSideWinderForceFeedback2Name(const char* name) {
    return name != nullptr && std::string(name).find("SideWinder Force Feedback 2") != std::string::npos;
}

void AddSideWinderGameControllerMapping(int32_t deviceIndex) {
    const auto name = SDL_JoystickNameForIndex(deviceIndex);
    if (!IsSideWinderForceFeedback2Name(name)) {
        return;
    }

    char guidString[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(deviceIndex), guidString, sizeof(guidString));
    const auto mapping =
        std::string(guidString) +
        ",SideWinder Force Feedback 2 Joystick,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,leftshoulder:b4,"
        "rightshoulder:b5,leftstick:b8,rightstick:b9,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
    SDL_GameControllerAddMapping(mapping.c_str());
}

} // namespace

ConnectedPhysicalDeviceManager::ConnectedPhysicalDeviceManager() {
}

ConnectedPhysicalDeviceManager::~ConnectedPhysicalDeviceManager() {
    for (const auto& [instanceId, haptic] : mConnectedSDLHaptics) {
        SDL_HapticClose(haptic);
    }
    for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
        SDL_GameControllerClose(gamepad);
    }
    for (const auto& [instanceId, joystick] : mConnectedSDLHapticJoysticks) {
        SDL_JoystickClose(joystick);
    }
}

std::unordered_map<int32_t, SDL_GameController*>
ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadsForPort(uint8_t portIndex) {
    std::unordered_map<int32_t, SDL_GameController*> result;

    for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
        if (!PortIsIgnoringInstanceId(portIndex, instanceId)) {
            result[instanceId] = gamepad;
        }
    }

    return result;
}

std::unordered_map<int32_t, SDL_Haptic*> ConnectedPhysicalDeviceManager::GetConnectedSDLHapticsForPort(
    uint8_t portIndex) {
    std::unordered_map<int32_t, SDL_Haptic*> result;

    for (const auto& [instanceId, haptic] : mConnectedSDLHaptics) {
        if (!PortIsIgnoringInstanceId(portIndex, instanceId)) {
            result[instanceId] = haptic;
        }
    }

    return result;
}

std::unordered_map<int32_t, std::string> ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadNames() {
    return mConnectedSDLGamepadNames;
}

bool ConnectedPhysicalDeviceManager::HasConnectedSideWinderForceFeedback2ForPort(uint8_t portIndex) {
    for (const auto& [instanceId, name] : mConnectedSDLGamepadNames) {
        if (!PortIsIgnoringInstanceId(portIndex, instanceId) &&
            name.find("SideWinder Force Feedback 2") != std::string::npos) {
            return true;
        }
    }

    for (const auto& [instanceId, name] : mConnectedSDLHapticNames) {
        if (!PortIsIgnoringInstanceId(portIndex, instanceId) &&
            name.find("SideWinder Force Feedback 2") != std::string::npos) {
            return true;
        }
    }

    return false;
}

std::unordered_set<int32_t> ConnectedPhysicalDeviceManager::GetIgnoredInstanceIdsForPort(uint8_t portIndex) {
    return mIgnoredInstanceIds[portIndex];
}

bool ConnectedPhysicalDeviceManager::PortIsIgnoringInstanceId(uint8_t portIndex, int32_t instanceId) {
    return GetIgnoredInstanceIdsForPort(portIndex).contains(instanceId);
}

void ConnectedPhysicalDeviceManager::IgnoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    mIgnoredInstanceIds[portIndex].insert(instanceId);
}

void ConnectedPhysicalDeviceManager::UnignoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    mIgnoredInstanceIds[portIndex].erase(instanceId);
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceConnect(int32_t sdlDeviceIndex) {
    RefreshConnectedSDLGamepads();
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceDisconnect(int32_t sdlJoystickInstanceId) {
    RefreshConnectedSDLGamepads();
}

void ConnectedPhysicalDeviceManager::RefreshConnectedSDLGamepads() {
    for (const auto& [instanceId, haptic] : mConnectedSDLHaptics) {
        SDL_HapticClose(haptic);
    }
    for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
        SDL_GameControllerClose(gamepad);
    }
    for (const auto& [instanceId, joystick] : mConnectedSDLHapticJoysticks) {
        SDL_JoystickClose(joystick);
    }

    mConnectedSDLGamepads.clear();
    mConnectedSDLHapticJoysticks.clear();
    mConnectedSDLHaptics.clear();
    mConnectedSDLGamepadNames.clear();
    mConnectedSDLHapticNames.clear();

    for (int32_t i = 0; i < SDL_NumJoysticks(); i++) {
        AddSideWinderGameControllerMapping(i);

        if (SDL_IsGameController(i)) {
            auto gamepad = SDL_GameControllerOpen(i);
            auto gamepadJoystick = SDL_GameControllerGetJoystick(gamepad);
            auto instanceId = SDL_JoystickInstanceID(gamepadJoystick);
            auto name = SDL_GameControllerName(gamepad);

            mConnectedSDLGamepads[instanceId] = gamepad;
            mConnectedSDLGamepadNames[instanceId] = name;

            if (SDL_JoystickIsHaptic(gamepadJoystick) == SDL_TRUE) {
                auto haptic = SDL_HapticOpenFromJoystick(gamepadJoystick);
                if (haptic != nullptr) {
                    if (SDL_HapticQuery(haptic) & SDL_HAPTIC_GAIN) {
                        SDL_HapticSetGain(haptic, 100);
                    }
                    if (SDL_HapticQuery(haptic) & SDL_HAPTIC_AUTOCENTER) {
                        SDL_HapticSetAutocenter(haptic, 25);
                    }

                    mConnectedSDLHaptics[instanceId] = haptic;
                    mConnectedSDLHapticNames[instanceId] = name;
                }
            }

            for (uint8_t port = 1; port < 4; port++) {
                mIgnoredInstanceIds[port].insert(instanceId);
            }

            continue;
        }

        auto joystick = SDL_JoystickOpen(i);
        if (joystick == nullptr) {
            continue;
        }

        if (SDL_JoystickIsHaptic(joystick) != SDL_TRUE) {
            SDL_JoystickClose(joystick);
            continue;
        }

        auto haptic = SDL_HapticOpenFromJoystick(joystick);
        if (haptic == nullptr) {
            SDL_JoystickClose(joystick);
            continue;
        }

        if (SDL_HapticQuery(haptic) & SDL_HAPTIC_GAIN) {
            SDL_HapticSetGain(haptic, 100);
        }
        if (SDL_HapticQuery(haptic) & SDL_HAPTIC_AUTOCENTER) {
            SDL_HapticSetAutocenter(haptic, 25);
        }

        auto instanceId = SDL_JoystickInstanceID(joystick);
        mConnectedSDLHapticJoysticks[instanceId] = joystick;
        mConnectedSDLHaptics[instanceId] = haptic;
        mConnectedSDLHapticNames[instanceId] = SDL_JoystickName(joystick);

        for (uint8_t port = 1; port < 4; port++) {
            mIgnoredInstanceIds[port].insert(instanceId);
        }
    }
}
} // namespace Ship
