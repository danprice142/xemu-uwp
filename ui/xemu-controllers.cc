/*
 * xemu Controller Binding Management
 *
 * Copyright (C) 2025 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xemu-controllers.h"
#include "xemu-settings.h"
#include <assert.h>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>

constexpr int controller_button_count = 15;
constexpr int controller_axes_count = 6;

RebindEventResult
ControllerKeyboardRebindingMap::ConsumeRebindEvent(SDL_Event *event)
{
    // Bind on key up
    // This ensures the UI does not immediately respond once the new binding is
    // applied
    if (event->type == SDL_EVENT_KEY_UP) {
        *(g_keyboard_scancode_map[m_table_row]) = event->key.scancode;
        return RebindEventResult::Complete;
    }

    return RebindEventResult::Ignore;
}

RebindEventResult ControllerGamepadRebindingMap::HandleButtonEvent(
    SDL_GamepadButtonEvent *event)
{
    if (m_state->sdl_joystick_id != event->which) {
        return RebindEventResult::Ignore;
    }

    int *button_map[controller_button_count] = {
        &m_state->controller_map->controller_mapping.a,
        &m_state->controller_map->controller_mapping.b,
        &m_state->controller_map->controller_mapping.x,
        &m_state->controller_map->controller_mapping.y,
        &m_state->controller_map->controller_mapping.back,
        &m_state->controller_map->controller_mapping.guide,
        &m_state->controller_map->controller_mapping.start,
        &m_state->controller_map->controller_mapping.lstick_btn,
        &m_state->controller_map->controller_mapping.rstick_btn,
        &m_state->controller_map->controller_mapping.lshoulder,
        &m_state->controller_map->controller_mapping.rshoulder,
        &m_state->controller_map->controller_mapping.dpad_up,
        &m_state->controller_map->controller_mapping.dpad_down,
        &m_state->controller_map->controller_mapping.dpad_left,
        &m_state->controller_map->controller_mapping.dpad_right,
    };

    // FIXME: Allow face buttons to map to axes
    if (m_table_row >= controller_button_count) {
        return RebindEventResult::Ignore;
    }

    // If we only track up events, then we might rebind to a button
    // that was already held down when the rebinding event began
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        m_seen_key_down = true;
        return RebindEventResult::Ignore;
    }

    // Bind on controller button up
    // This ensures the UI does not immediately respond once the new binding is
    // applied
    if (event->type != SDL_EVENT_GAMEPAD_BUTTON_UP || !m_seen_key_down) {
        return RebindEventResult::Ignore;
    }

    *(button_map[m_table_row]) = event->button;

    return RebindEventResult::Complete;
}

RebindEventResult
ControllerGamepadRebindingMap::HandleAxisEvent(SDL_GamepadAxisEvent *event)
{
    if (m_state->sdl_joystick_id != event->which) {
        return RebindEventResult::Ignore;
    }

    // Axis inputs cannot be bound to controller buttons
    if (m_table_row < controller_button_count) {
        return RebindEventResult::Ignore;
    }

    // Requre that the input be sufficiently outside of any deadzone range
    // before using it for rebinding
    if (std::abs(event->value >> 1) <=
        (std::numeric_limits<Sint16>::max() >> 2)) {
        return RebindEventResult::Ignore;
    }

    int *axis_map[controller_axes_count] = {
        &m_state->controller_map->controller_mapping.axis_left_x,
        &m_state->controller_map->controller_mapping.axis_left_y,
        &m_state->controller_map->controller_mapping.axis_right_x,
        &m_state->controller_map->controller_mapping.axis_right_y,
        &m_state->controller_map->controller_mapping.axis_trigger_left,
        &m_state->controller_map->controller_mapping.axis_trigger_right,
    };

    *(axis_map[m_table_row - controller_button_count]) = event->axis;

    return RebindEventResult::Complete;
}

RebindEventResult
ControllerGamepadRebindingMap::ConsumeRebindEvent(SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_REMOVED:
        return (m_state->sdl_joystick_id == event->gdevice.which) ?
                   RebindEventResult::Complete :
                   RebindEventResult::Ignore;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return HandleButtonEvent(&event->gbutton);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return HandleAxisEvent(&event->gaxis);
    default:
        return RebindEventResult::Ignore;
    }
}

#ifdef XEMU_INPUT_UWP
static int *controller_button_mapping_for_row(ControllerState *state, int row)
{
    int *button_map[controller_button_count] = {
        &state->controller_map->controller_mapping.a,
        &state->controller_map->controller_mapping.b,
        &state->controller_map->controller_mapping.x,
        &state->controller_map->controller_mapping.y,
        &state->controller_map->controller_mapping.back,
        &state->controller_map->controller_mapping.guide,
        &state->controller_map->controller_mapping.start,
        &state->controller_map->controller_mapping.lstick_btn,
        &state->controller_map->controller_mapping.rstick_btn,
        &state->controller_map->controller_mapping.lshoulder,
        &state->controller_map->controller_mapping.rshoulder,
        &state->controller_map->controller_mapping.dpad_up,
        &state->controller_map->controller_mapping.dpad_down,
        &state->controller_map->controller_mapping.dpad_left,
        &state->controller_map->controller_mapping.dpad_right,
    };

    return row >= 0 && row < controller_button_count ? button_map[row] :
                                                       nullptr;
}

static int *controller_axis_mapping_for_row(ControllerState *state, int row)
{
    int *axis_map[controller_axes_count] = {
        &state->controller_map->controller_mapping.axis_left_x,
        &state->controller_map->controller_mapping.axis_left_y,
        &state->controller_map->controller_mapping.axis_right_x,
        &state->controller_map->controller_mapping.axis_right_y,
        &state->controller_map->controller_mapping.axis_trigger_left,
        &state->controller_map->controller_mapping.axis_trigger_right,
    };
    int axis_row = row - controller_button_count;

    return axis_row >= 0 && axis_row < controller_axes_count ?
               axis_map[axis_row] :
               nullptr;
}

static SDL_GamepadButton uwp_button_to_sdl_button(uint16_t button)
{
    switch (button) {
    case XEMU_UWP_GAMEPAD_BUTTON_A:
        return SDL_GAMEPAD_BUTTON_SOUTH;
    case XEMU_UWP_GAMEPAD_BUTTON_B:
        return SDL_GAMEPAD_BUTTON_EAST;
    case XEMU_UWP_GAMEPAD_BUTTON_X:
        return SDL_GAMEPAD_BUTTON_WEST;
    case XEMU_UWP_GAMEPAD_BUTTON_Y:
        return SDL_GAMEPAD_BUTTON_NORTH;
    case XEMU_UWP_GAMEPAD_BUTTON_BACK:
        return SDL_GAMEPAD_BUTTON_BACK;
    case XEMU_UWP_GAMEPAD_BUTTON_START:
        return SDL_GAMEPAD_BUTTON_START;
    case XEMU_UWP_GAMEPAD_BUTTON_LSTICK:
        return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case XEMU_UWP_GAMEPAD_BUTTON_RSTICK:
        return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case XEMU_UWP_GAMEPAD_BUTTON_WHITE:
        return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case XEMU_UWP_GAMEPAD_BUTTON_BLACK:
        return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case XEMU_UWP_GAMEPAD_BUTTON_DPAD_UP:
        return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case XEMU_UWP_GAMEPAD_BUTTON_DPAD_DOWN:
        return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case XEMU_UWP_GAMEPAD_BUTTON_DPAD_LEFT:
        return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case XEMU_UWP_GAMEPAD_BUTTON_DPAD_RIGHT:
        return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default:
        return SDL_GAMEPAD_BUTTON_INVALID;
    }
}

static bool axis_active(int16_t value)
{
    return std::abs((int)value) >
        (std::numeric_limits<int16_t>::max() >> 1);
}

ControllerUWPGamepadRebindingMap::ControllerUWPGamepadRebindingMap(
    int table_row, ControllerState *state)
    : RebindingMap(table_row), m_state{ state }, m_has_initial_state{ false },
      m_seen_button{ 0 }
{
    if (m_state != nullptr) {
        m_has_initial_state = xemu_uwp_gamepad_get_state(
            m_state->uwp_gamepad_index, &m_initial_state);
    }
}

RebindEventResult
ControllerUWPGamepadRebindingMap::ConsumeRebindEvent(SDL_Event *event)
{
    (void)event;
    return RebindEventResult::Ignore;
}

RebindEventResult ControllerUWPGamepadRebindingMap::Poll()
{
    static constexpr uint16_t button_masks[] = {
        XEMU_UWP_GAMEPAD_BUTTON_A,
        XEMU_UWP_GAMEPAD_BUTTON_B,
        XEMU_UWP_GAMEPAD_BUTTON_X,
        XEMU_UWP_GAMEPAD_BUTTON_Y,
        XEMU_UWP_GAMEPAD_BUTTON_BACK,
        XEMU_UWP_GAMEPAD_BUTTON_START,
        XEMU_UWP_GAMEPAD_BUTTON_LSTICK,
        XEMU_UWP_GAMEPAD_BUTTON_RSTICK,
        XEMU_UWP_GAMEPAD_BUTTON_WHITE,
        XEMU_UWP_GAMEPAD_BUTTON_BLACK,
        XEMU_UWP_GAMEPAD_BUTTON_DPAD_UP,
        XEMU_UWP_GAMEPAD_BUTTON_DPAD_DOWN,
        XEMU_UWP_GAMEPAD_BUTTON_DPAD_LEFT,
        XEMU_UWP_GAMEPAD_BUTTON_DPAD_RIGHT,
    };
    static constexpr SDL_GamepadAxis axis_ids[] = {
        SDL_GAMEPAD_AXIS_LEFTX,
        SDL_GAMEPAD_AXIS_LEFTY,
        SDL_GAMEPAD_AXIS_RIGHTX,
        SDL_GAMEPAD_AXIS_RIGHTY,
        SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
        SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    };
    static constexpr int axis_indices[] = { 2, 3, 4, 5, 0, 1 };
    XemuUWPGamepadState state;

    if (m_state == nullptr || m_state->type != INPUT_DEVICE_UWP_GAMEPAD ||
        m_state->controller_map == nullptr ||
        !xemu_uwp_gamepad_get_state(m_state->uwp_gamepad_index, &state)) {
        return RebindEventResult::Complete;
    }

    if (m_table_row < controller_button_count) {
        uint16_t initial_buttons =
            m_has_initial_state ? m_initial_state.buttons : 0;
        uint16_t new_buttons = state.buttons & ~initial_buttons;

        if (m_seen_button == 0) {
            for (uint16_t button : button_masks) {
                if (new_buttons & button) {
                    m_seen_button = button;
                    return RebindEventResult::Ignore;
                }
            }
            return RebindEventResult::Ignore;
        }

        if (state.buttons & m_seen_button) {
            return RebindEventResult::Ignore;
        }

        int *mapping = controller_button_mapping_for_row(m_state, m_table_row);
        SDL_GamepadButton button = uwp_button_to_sdl_button(m_seen_button);
        if (mapping != nullptr && button != SDL_GAMEPAD_BUTTON_INVALID) {
            *mapping = button;
        }
        return RebindEventResult::Complete;
    }

    int *mapping = controller_axis_mapping_for_row(m_state, m_table_row);
    if (mapping == nullptr) {
        return RebindEventResult::Complete;
    }

    for (size_t i = 0; i < std::size(axis_ids); i++) {
        int axis_index = axis_indices[i];
        int16_t initial_value =
            m_has_initial_state ? m_initial_state.axis[axis_index] : 0;

        if (!axis_active(initial_value) && axis_active(state.axis[axis_index])) {
            *mapping = axis_ids[i];
            return RebindEventResult::Complete;
        }
    }

    return RebindEventResult::Ignore;
}
#endif
