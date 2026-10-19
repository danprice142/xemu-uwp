/*
 * xemu Settings Management
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

#ifndef XEMU_CONTROLLERS_H
#define XEMU_CONTROLLERS_H

#include "xemu-input.h"
#include <SDL3/SDL.h>
#ifdef XEMU_INPUT_UWP
#include "uwp-gamepad.h"
#endif

enum class RebindEventResult {
    Ignore,
    Complete,
};

struct RebindingMap {
protected:
    int m_table_row;
    RebindingMap(int table_row) : m_table_row{ table_row }
    {
    }

public:
    virtual RebindEventResult ConsumeRebindEvent(SDL_Event *event) = 0;
    virtual RebindEventResult Poll()
    {
        return RebindEventResult::Ignore;
    }

    int GetTableRow() const
    {
        return m_table_row;
    }

    virtual ~RebindingMap() = default;
};

struct ControllerKeyboardRebindingMap : public virtual RebindingMap {
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;

    ControllerKeyboardRebindingMap(int table_row) : RebindingMap(table_row)
    {
    }
};

class ControllerGamepadRebindingMap : public virtual RebindingMap {
    ControllerState *m_state;
    bool m_seen_key_down;

    RebindEventResult HandleButtonEvent(SDL_GamepadButtonEvent *event);
    RebindEventResult HandleAxisEvent(SDL_GamepadAxisEvent *event);

public:
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;
    ControllerGamepadRebindingMap(int table_row, ControllerState *state)
        : RebindingMap(table_row), m_state{ state }, m_seen_key_down{ false }
    {
    }
};

#ifdef XEMU_INPUT_UWP
class ControllerUWPGamepadRebindingMap : public virtual RebindingMap {
    ControllerState *m_state;
    XemuUWPGamepadState m_initial_state;
    bool m_has_initial_state;
    uint16_t m_seen_button;

public:
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;
    RebindEventResult Poll() override;
    ControllerUWPGamepadRebindingMap(int table_row, ControllerState *state);
};
#endif

#endif // XEMU_CONTROLLERS_H
