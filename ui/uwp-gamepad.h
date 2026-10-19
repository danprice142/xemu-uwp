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

#ifndef XEMU_UWP_GAMEPAD_H
#define XEMU_UWP_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>

#define XEMU_UWP_MAX_GAMEPADS 8
#define XEMU_UWP_GAMEPAD_AXIS_COUNT 6

enum xemu_uwp_gamepad_button_mask {
    XEMU_UWP_GAMEPAD_BUTTON_A          = (1 << 0),
    XEMU_UWP_GAMEPAD_BUTTON_B          = (1 << 1),
    XEMU_UWP_GAMEPAD_BUTTON_X          = (1 << 2),
    XEMU_UWP_GAMEPAD_BUTTON_Y          = (1 << 3),
    XEMU_UWP_GAMEPAD_BUTTON_DPAD_LEFT  = (1 << 4),
    XEMU_UWP_GAMEPAD_BUTTON_DPAD_UP    = (1 << 5),
    XEMU_UWP_GAMEPAD_BUTTON_DPAD_RIGHT = (1 << 6),
    XEMU_UWP_GAMEPAD_BUTTON_DPAD_DOWN  = (1 << 7),
    XEMU_UWP_GAMEPAD_BUTTON_BACK       = (1 << 8),
    XEMU_UWP_GAMEPAD_BUTTON_START      = (1 << 9),
    XEMU_UWP_GAMEPAD_BUTTON_WHITE      = (1 << 10),
    XEMU_UWP_GAMEPAD_BUTTON_BLACK      = (1 << 11),
    XEMU_UWP_GAMEPAD_BUTTON_LSTICK     = (1 << 12),
    XEMU_UWP_GAMEPAD_BUTTON_RSTICK     = (1 << 13),
};

typedef struct XemuUWPGamepadState {
    uint64_t timestamp;
    uint16_t buttons;
    int16_t axis[XEMU_UWP_GAMEPAD_AXIS_COUNT];
} XemuUWPGamepadState;

#ifdef __cplusplus
extern "C" {
#endif

int xemu_uwp_gamepad_count(void);
bool xemu_uwp_gamepad_get_state(int index, XemuUWPGamepadState *state);
bool xemu_uwp_gamepad_set_vibration(int index, uint16_t left_motor,
                                    uint16_t right_motor);

#ifdef __cplusplus
}
#endif

#endif
