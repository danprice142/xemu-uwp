/*
 * xemu UWP gamepad bridge
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"

#ifdef CONFIG_UWP

#include "ui/uwp-gamepad.h"

#include <algorithm>
#include <cmath>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

namespace {

namespace wgi = winrt::Windows::Gaming::Input;

int16_t scale_signed_axis(double value)
{
    value = std::clamp(value, -1.0, 1.0);
    int scaled = (int)std::lrint(value >= 0.0 ? value * 32767.0
                                              : value * 32768.0);
    scaled = std::clamp(scaled, -32768, 32767);
    return (int16_t)scaled;
}

int16_t scale_trigger(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return (int16_t)std::clamp((int)std::lrint(value * 32767.0), 0, 32767);
}

bool get_gamepad(int index, wgi::Gamepad *gamepad)
{
    if (index < 0 || gamepad == nullptr) {
        return false;
    }

    auto gamepads = wgi::Gamepad::Gamepads();
    if ((uint32_t)index >= gamepads.Size()) {
        return false;
    }

    *gamepad = gamepads.GetAt((uint32_t)index);
    return *gamepad != nullptr;
}

bool has_button(uint32_t buttons, wgi::GamepadButtons button)
{
    return (buttons & static_cast<uint32_t>(button)) != 0;
}

} // namespace

extern "C" int xemu_uwp_gamepad_count(void)
{
    try {
        uint32_t count = wgi::Gamepad::Gamepads().Size();
        return (int)std::min<uint32_t>(count, XEMU_UWP_MAX_GAMEPADS);
    } catch (winrt::hresult_error const &e) {
        fprintf(stderr,
                "xemu input: Windows.Gaming.Input gamepad enumeration "
                "failed HRESULT=0x%08x\n",
                (unsigned int)e.code().value);
    } catch (...) {
        fprintf(stderr,
                "xemu input: Windows.Gaming.Input gamepad enumeration "
                "failed with unknown exception\n");
    }

    return 0;
}

extern "C" bool xemu_uwp_gamepad_get_state(int index,
                                           XemuUWPGamepadState *state)
{
    if (state == nullptr) {
        return false;
    }

    memset(state, 0, sizeof(*state));

    try {
        wgi::Gamepad gamepad{ nullptr };
        if (!get_gamepad(index, &gamepad)) {
            return false;
        }

        wgi::GamepadReading reading = gamepad.GetCurrentReading();
        uint32_t buttons = static_cast<uint32_t>(reading.Buttons);

        state->timestamp = reading.Timestamp;
        if (has_button(buttons, wgi::GamepadButtons::A)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_A;
        }
        if (has_button(buttons, wgi::GamepadButtons::B)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_B;
        }
        if (has_button(buttons, wgi::GamepadButtons::X)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_X;
        }
        if (has_button(buttons, wgi::GamepadButtons::Y)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_Y;
        }
        if (has_button(buttons, wgi::GamepadButtons::DPadLeft)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_DPAD_LEFT;
        }
        if (has_button(buttons, wgi::GamepadButtons::DPadUp)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_DPAD_UP;
        }
        if (has_button(buttons, wgi::GamepadButtons::DPadRight)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_DPAD_RIGHT;
        }
        if (has_button(buttons, wgi::GamepadButtons::DPadDown)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_DPAD_DOWN;
        }
        if (has_button(buttons, wgi::GamepadButtons::View)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_BACK;
        }
        if (has_button(buttons, wgi::GamepadButtons::Menu)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_START;
        }
        if (has_button(buttons, wgi::GamepadButtons::LeftShoulder)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_WHITE;
        }
        if (has_button(buttons, wgi::GamepadButtons::RightShoulder)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_BLACK;
        }
        if (has_button(buttons, wgi::GamepadButtons::LeftThumbstick)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_LSTICK;
        }
        if (has_button(buttons, wgi::GamepadButtons::RightThumbstick)) {
            state->buttons |= XEMU_UWP_GAMEPAD_BUTTON_RSTICK;
        }

        state->axis[0] = scale_trigger(reading.LeftTrigger);
        state->axis[1] = scale_trigger(reading.RightTrigger);
        state->axis[2] = scale_signed_axis(reading.LeftThumbstickX);
        state->axis[3] = scale_signed_axis(reading.LeftThumbstickY);
        state->axis[4] = scale_signed_axis(reading.RightThumbstickX);
        state->axis[5] = scale_signed_axis(reading.RightThumbstickY);

        return true;
    } catch (winrt::hresult_error const &e) {
        fprintf(stderr,
                "xemu input: Windows.Gaming.Input read failed index=%d "
                "HRESULT=0x%08x\n",
                index, (unsigned int)e.code().value);
    } catch (...) {
        fprintf(stderr,
                "xemu input: Windows.Gaming.Input read failed index=%d "
                "with unknown exception\n",
                index);
    }

    return false;
}

extern "C" bool xemu_uwp_gamepad_set_vibration(int index,
                                               uint16_t left_motor,
                                               uint16_t right_motor)
{
    try {
        wgi::Gamepad gamepad{ nullptr };
        if (!get_gamepad(index, &gamepad)) {
            return false;
        }

        wgi::GamepadVibration vibration{};
        vibration.LeftMotor = (double)left_motor / 65535.0;
        vibration.RightMotor = (double)right_motor / 65535.0;
        vibration.LeftTrigger = 0.0;
        vibration.RightTrigger = 0.0;
        gamepad.Vibration(vibration);
        return true;
    } catch (winrt::hresult_error const &e) {
        fprintf(stderr,
                "xemu input: Windows.Gaming.Input vibration failed index=%d "
                "HRESULT=0x%08x\n",
                index, (unsigned int)e.code().value);
    } catch (...) {
        fprintf(stderr,
                "xemu input: Windows.Gaming.Input vibration failed index=%d "
                "with unknown exception\n",
                index);
    }

    return false;
}

#endif
