#include "libultraship/controller/controldevice/controller/Controller.h"
#include "libultraship/controller/wheel/WheelDevice.h"
#include <memory>
#include <algorithm>
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#if __APPLE__
#include <SDL_events.h>
#else
#include <SDL2/SDL_events.h>
#endif
#include <spdlog/spdlog.h>
#include "ship/utils/StringHelper.h"
#include "ship/controller/controldeck/ControlDeck.h"

#define M_TAU 6.2831853071795864769252867665590057 // 2 * pi
#define MINIMUM_RADIUS_TO_MAP_NOTCH 0.9

#define MAX_SDL_AXIS_VALUE (float)INT16_MAX
#define KART_MENU_CONFIRM_BUTTON 0x0040
#define KART_MENU_CANCEL_BUTTON 0x0080
#define KART_CLUTCH_BUTTON_VALUE_INDEX 16
#define KART_SHIFT_GEAR_1_BUTTON 0x00020000
#define KART_SHIFT_GEAR_2_BUTTON 0x00040000
#define KART_SHIFT_GEAR_3_BUTTON 0x00080000
#define KART_SHIFT_GEAR_4_BUTTON 0x00100000
#define KART_SHIFT_GEAR_5_BUTTON 0x00200000
#define KART_SHIFT_GEAR_6_BUTTON 0x00400000
#define KART_SHIFT_REVERSE_BUTTON 0x00800000
#define KART_SHIFT_NEUTRAL_BUTTON 0x01000000
#define KART_USE_ITEM_FORWARD_BUTTON 0x02000000
#define KART_USE_ITEM_BACKWARD_BUTTON 0x04000000
#define KART_DRIFT_BUTTON_VALUE_INDEX 4
#define WHEEL_STICK_RANGE 85.0f
#define WHEEL_HANDBRAKE_THRESHOLD 0.25f
#define WHEEL_GEAR_REVERSE -1
#define WHEEL_GEAR_NEUTRAL 0

namespace LUS {
Controller::Controller(uint8_t portIndex, std::vector<CONTROLLERBUTTONS_T> bitmasks)
    : Ship::Controller(portIndex, bitmasks) {
}

static float GetNormalizedTriggerValue(uint8_t portIndex, SDL_GameControllerAxis axis) {
    float triggerValue = 0.0f;

    if (Ship::Context::GetInstance()->GetControlDeck()->GamepadGameInputBlocked()) {
        return triggerValue;
    }

    for (const auto& [instanceId, gamepad] : Ship::Context::GetInstance()
                                                ->GetControlDeck()
                                                ->GetConnectedPhysicalDeviceManager()
                                                ->GetConnectedSDLGamepadsForPort(portIndex)) {
        const auto axisValue = SDL_GameControllerGetAxis(gamepad, axis);
        if (axisValue > 0) {
            triggerValue = std::max(triggerValue, axisValue / MAX_SDL_AXIS_VALUE);
        }
    }

    return triggerValue;
}

static void ApplyWheelReading(OSContPad& pad, const WheelReading& reading) {
    if (pad.stick_x == 0) {
        pad.stick_x = static_cast<int8_t>(std::clamp(reading.steering * WHEEL_STICK_RANGE, -WHEEL_STICK_RANGE, WHEEL_STICK_RANGE));
    }

    if (reading.connected) {
        pad.right_trigger = reading.throttle;
        pad.left_trigger = reading.brake;
    } else {
        if (pad.right_trigger == 0) {
            pad.right_trigger = reading.throttle;
        }
        if (pad.left_trigger == 0) {
            pad.left_trigger = reading.brake;
        }
    }

    pad.button_value[KART_CLUTCH_BUTTON_VALUE_INDEX] =
        std::max(pad.button_value[KART_CLUTCH_BUTTON_VALUE_INDEX], reading.clutch);

    if (reading.handbrake > WHEEL_HANDBRAKE_THRESHOLD) {
        pad.button |= BTN_R;
        pad.button_value[KART_DRIFT_BUTTON_VALUE_INDEX] =
            std::max(pad.button_value[KART_DRIFT_BUTTON_VALUE_INDEX], reading.handbrake);
    }

    if (reading.menuUp) {
        pad.button |= BTN_DUP;
    }
    if (reading.menuDown) {
        pad.button |= BTN_DDOWN;
    }
    if (reading.menuLeft) {
        pad.button |= BTN_DLEFT;
    }
    if (reading.menuRight) {
        pad.button |= BTN_DRIGHT;
    }
    if (reading.menuConfirm) {
        pad.button |= KART_MENU_CONFIRM_BUTTON;
    }
    if (reading.menuCancel) {
        pad.button |= KART_MENU_CANCEL_BUTTON;
    }
    if (reading.openMenu) {
        pad.button |= BTN_START;
    }
    if (reading.useItemBackward) {
        pad.button |= KART_USE_ITEM_BACKWARD_BUTTON;
    }
    if (reading.useItemForward) {
        pad.button |= KART_USE_ITEM_FORWARD_BUTTON;
    }

    switch (reading.requestedGear) {
        case WHEEL_GEAR_REVERSE:
            pad.button |= KART_SHIFT_REVERSE_BUTTON;
            break;
        case WHEEL_GEAR_NEUTRAL:
            pad.button |= KART_SHIFT_NEUTRAL_BUTTON;
            break;
        case 1:
            pad.button |= KART_SHIFT_GEAR_1_BUTTON;
            break;
        case 2:
            pad.button |= KART_SHIFT_GEAR_2_BUTTON;
            break;
        case 3:
            pad.button |= KART_SHIFT_GEAR_3_BUTTON;
            break;
        case 4:
            pad.button |= KART_SHIFT_GEAR_4_BUTTON;
            break;
        case 5:
            pad.button |= KART_SHIFT_GEAR_5_BUTTON;
            break;
        case 6:
            pad.button |= KART_SHIFT_GEAR_6_BUTTON;
            break;
        default:
            break;
    }
}

void Controller::ReadToPad(void* pad) {
    ReadToOSContPad((OSContPad*)pad);
}

void Controller::ReadToOSContPad(OSContPad* pad) {
    OSContPad padToBuffer = { 0 };

    // Button Inputs
    for (auto [bitmask, button] : mButtons) {
        button->UpdatePad(padToBuffer.button, padToBuffer.button_value);
    }

    // Stick Inputs
    GetLeftStick()->UpdatePad(padToBuffer.stick_x, padToBuffer.stick_y);
    GetRightStick()->UpdatePad(padToBuffer.right_stick_x, padToBuffer.right_stick_y);

    // Gyro
    GetGyro()->UpdatePad(padToBuffer.gyro_x, padToBuffer.gyro_y);

    padToBuffer.left_trigger = GetNormalizedTriggerValue(GetPortIndex(), SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    padToBuffer.right_trigger = GetNormalizedTriggerValue(GetPortIndex(), SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

    if (GetPortIndex() == 0) {
        ApplyWheelReading(padToBuffer, WheelDeviceManager::Instance().ReadPlayerOneWheel());
    }

    mPadBuffer.push_front(padToBuffer);
    if (pad != nullptr) {
        auto& padFromBuffer = mPadBuffer[std::min(
            mPadBuffer.size() - 1,
            (size_t)Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_SIMULATED_INPUT_LAG, 0))];

        pad->button |= padFromBuffer.button;

        if (pad->stick_x == 0) {
            pad->stick_x = padFromBuffer.stick_x;
        }
        if (pad->stick_y == 0) {
            pad->stick_y = padFromBuffer.stick_y;
        }

        if (pad->right_stick_x == 0) {
            pad->right_stick_x = padFromBuffer.right_stick_x;
        }
        if (pad->right_stick_y == 0) {
            pad->right_stick_y = padFromBuffer.right_stick_y;
        }

        if (pad->left_trigger == 0) {
            pad->left_trigger = padFromBuffer.left_trigger;
        }
        if (pad->right_trigger == 0) {
            pad->right_trigger = padFromBuffer.right_trigger;
        }

        for (size_t i = 0; i < CONTROLLER_BUTTON_VALUE_COUNT; i++) {
            pad->button_value[i] = std::max(pad->button_value[i], padFromBuffer.button_value[i]);
        }

        if (pad->gyro_x == 0) {
            pad->gyro_x = padFromBuffer.gyro_x;
        }
        if (pad->gyro_y == 0) {
            pad->gyro_y = padFromBuffer.gyro_y;
        }
    }

    while (mPadBuffer.size() > 6) {
        mPadBuffer.pop_back();
    }
}
} // namespace LUS
