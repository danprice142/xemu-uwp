/*
 * xemu Input Management
 *
 * Copyright (C) 2020-2021 Matt Borgerson
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


#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/timer.h"
#include "qemu/config-file.h"
#include "ui/input.h"

#include "xemu-input.h"
#include "xemu-dsu.h"
#include "xemu-notifications.h"
#include "xemu-pointer-aim.h"
#include "xemu-settings.h"
#ifdef CONFIG_UWP
#include "uwp-gamepad.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "system/blockdev.h"

// #define DEBUG_INPUT

#ifdef DEBUG_INPUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US  2500
#define XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US 2500

#if 0
static void xemu_input_print_controller_state(ControllerState *state)
{
    DPRINTF("     A = %d,      B = %d,     X = %d,     Y = %d\n"
           "  Left = %d,     Up = %d, Right = %d,  Down = %d\n"
           "  Back = %d,  Start = %d, White = %d, Black = %d\n"
           "Lstick = %d, Rstick = %d, Guide = %d\n"
           "\n"
           "LTrig   = %.3f, RTrig   = %.3f\n"
           "LStickX = %.3f, RStickX = %.3f\n"
           "LStickY = %.3f, RStickY = %.3f\n\n",
        !!(state->buttons & CONTROLLER_BUTTON_A),
        !!(state->buttons & CONTROLLER_BUTTON_B),
        !!(state->buttons & CONTROLLER_BUTTON_X),
        !!(state->buttons & CONTROLLER_BUTTON_Y),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_LEFT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_UP),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_RIGHT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_DOWN),
        !!(state->buttons & CONTROLLER_BUTTON_BACK),
        !!(state->buttons & CONTROLLER_BUTTON_START),
        !!(state->buttons & CONTROLLER_BUTTON_WHITE),
        !!(state->buttons & CONTROLLER_BUTTON_BLACK),
        !!(state->buttons & CONTROLLER_BUTTON_LSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_RSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_GUIDE),
        state->axis[CONTROLLER_AXIS_LTRIG],
        state->axis[CONTROLLER_AXIS_RTRIG],
        state->axis[CONTROLLER_AXIS_LSTICK_X],
        state->axis[CONTROLLER_AXIS_RSTICK_X],
        state->axis[CONTROLLER_AXIS_LSTICK_Y],
        state->axis[CONTROLLER_AXIS_RSTICK_Y]
        );
}
#endif

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);
ControllerState *bound_controllers[4] = { NULL, NULL, NULL, NULL };
const char *bound_drivers[4] = { DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE,
                                 DRIVER_DUKE };
int test_mode;

static ControllerState dsu_controllers[XEMU_DSU_MAX_PLAYERS];
static char dsu_controller_names[XEMU_DSU_MAX_PLAYERS][32];
static XemuPointerAimState dsu_pointer_aim[XEMU_DSU_MAX_PLAYERS];

enum XemuDSUMotionTarget {
    XEMU_DSU_MOTION_MOUSE = 0,
    XEMU_DSU_MOTION_RIGHT_STICK,
    XEMU_DSU_MOTION_LEFT_STICK,
    XEMU_DSU_MOTION_DPAD,
    XEMU_DSU_MOTION_LIGHTGUN,
};

typedef struct XemuDSUMotionConfig {
    bool enabled;
    int target;
    float horizontal_sensitivity;
    float vertical_sensitivity;
} XemuDSUMotionConfig;

typedef struct XemuDSUMotionMapperState {
    bool target_valid;
    bool logged_mapping;
    int target;
    uint32_t packet_number;
    int64_t last_received_us;
    double lightgun_x;
    double lightgun_y;
    double mouse_remainder_x;
    double mouse_remainder_y;
} XemuDSUMotionMapperState;

static XemuDSUMotionMapperState
    dsu_motion_mapper[XEMU_DSU_MAX_PLAYERS];

static XemuDSUMotionConfig xemu_input_get_dsu_motion_config(unsigned player)
{
    XemuDSUMotionConfig config = {
        .enabled = false,
        .target = XEMU_DSU_MOTION_RIGHT_STICK,
        .horizontal_sensitivity = 1.0f,
        .vertical_sensitivity = 1.0f,
    };

#define LOAD_DSU_MOTION_CONFIG(n)                                           \
    config.enabled = g_config.input.dsu.player##n.motion;                   \
    config.target = g_config.input.dsu.player##n.motion_target;             \
    config.horizontal_sensitivity =                                        \
        g_config.input.dsu.player##n.motion_sensitivity_horizontal;         \
    config.vertical_sensitivity =                                          \
        g_config.input.dsu.player##n.motion_sensitivity_vertical
    switch (player) {
    case 0: LOAD_DSU_MOTION_CONFIG(1); break;
    case 1: LOAD_DSU_MOTION_CONFIG(2); break;
    case 2: LOAD_DSU_MOTION_CONFIG(3); break;
    case 3: LOAD_DSU_MOTION_CONFIG(4); break;
    default: break;
    }
#undef LOAD_DSU_MOTION_CONFIG

    config.target = CLAMP(config.target, XEMU_DSU_MOTION_MOUSE,
                          XEMU_DSU_MOTION_LIGHTGUN);
    if (!isfinite(config.horizontal_sensitivity)) {
        config.horizontal_sensitivity = 1.0f;
    }
    if (!isfinite(config.vertical_sensitivity)) {
        config.vertical_sensitivity = 1.0f;
    }
    config.horizontal_sensitivity =
        CLAMP(config.horizontal_sensitivity, 0.1f, 5.0f);
    config.vertical_sensitivity =
        CLAMP(config.vertical_sensitivity, 0.1f, 5.0f);
    return config;
}

static void xemu_input_reset_dsu_motion_mapper(unsigned player, int target)
{
    XemuDSUMotionMapperState *state = &dsu_motion_mapper[player];
    memset(state, 0, sizeof(*state));
    state->target_valid = true;
    state->target = target;
}

static bool xemu_input_take_dsu_motion_sample(
    unsigned player, int target, const XemuDSUControllerState *controller,
    double *dt)
{
    XemuDSUMotionMapperState *state = &dsu_motion_mapper[player];

    if (!state->target_valid || state->target != target) {
        xemu_input_reset_dsu_motion_mapper(player, target);
    }
    if (state->packet_number == controller->packet_number &&
        state->last_received_us == controller->last_received_us) {
        return false;
    }

    if (state->last_received_us > 0 &&
        controller->last_received_us > state->last_received_us) {
        *dt = (controller->last_received_us - state->last_received_us) /
              1000000.0;
    } else {
        *dt = 1.0 / 60.0;
    }
    *dt = CLAMP(*dt, 1.0 / 1000.0, 0.1);
    state->packet_number = controller->packet_number;
    state->last_received_us = controller->last_received_us;
    return true;
}

static int16_t xemu_input_motion_axis(float rate)
{
    int value = (int)lrintf(rate * 16384.0f);
    return (int16_t)CLAMP(value, INT16_MIN, INT16_MAX);
}

static const char *xemu_input_dsu_motion_target_name(int target)
{
    static const char *names[] = {
        "mouse", "right stick", "left stick", "D-Pad", "lightgun"
    };
    return names[CLAMP(target, XEMU_DSU_MOTION_MOUSE,
                       XEMU_DSU_MOTION_LIGHTGUN)];
}

static bool xemu_input_port_accepts_local(unsigned port)
{
    ControllerState *state = bound_controllers[port];
    if (state == NULL) {
        return true;
    }
    if (state->type != INPUT_DEVICE_DSU_GAMEPAD) {
        return false;
    }

    /* A replacement DSU controller owns its port. Only an auxiliary DSU
     * controller may share the port with a newly connected local gamepad. */
    return xemu_dsu_player_is_addon(port);
}

enum {
    DSU_BTN_SHARE = 0x0001,
    DSU_BTN_L3 = 0x0002,
    DSU_BTN_R3 = 0x0004,
    DSU_BTN_OPTIONS = 0x0008,
    DSU_BTN_DPAD_UP = 0x0010,
    DSU_BTN_DPAD_RIGHT = 0x0020,
    DSU_BTN_DPAD_DOWN = 0x0040,
    DSU_BTN_DPAD_LEFT = 0x0080,
    DSU_BTN_L2 = 0x0100,
    DSU_BTN_R2 = 0x0200,
    DSU_BTN_L1 = 0x0400,
    DSU_BTN_R1 = 0x0800,
    DSU_BTN_TRIANGLE = 0x1000,
    DSU_BTN_CIRCLE = 0x2000,
    DSU_BTN_CROSS = 0x4000,
    DSU_BTN_SQUARE = 0x8000,
};

static const char **port_index_to_settings_key_map[] = {
    &g_config.input.bindings.port1,
    &g_config.input.bindings.port2,
    &g_config.input.bindings.port3,
    &g_config.input.bindings.port4,
};

static const char **port_index_to_driver_settings_key_map[] = {
    &g_config.input.bindings.port1_driver,
    &g_config.input.bindings.port2_driver,
    &g_config.input.bindings.port3_driver, 
    &g_config.input.bindings.port4_driver
};

static int *peripheral_types_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_type_0,
      &g_config.input.peripherals.port1.peripheral_type_1 },
    { &g_config.input.peripherals.port2.peripheral_type_0,
      &g_config.input.peripherals.port2.peripheral_type_1 },
    { &g_config.input.peripherals.port3.peripheral_type_0,
      &g_config.input.peripherals.port3.peripheral_type_1 },
    { &g_config.input.peripherals.port4.peripheral_type_0,
      &g_config.input.peripherals.port4.peripheral_type_1 }
};

static const char **peripheral_params_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_param_0,
      &g_config.input.peripherals.port1.peripheral_param_1 },
    { &g_config.input.peripherals.port2.peripheral_param_0,
      &g_config.input.peripherals.port2.peripheral_param_1 },
    { &g_config.input.peripherals.port3.peripheral_param_0,
      &g_config.input.peripherals.port3.peripheral_param_1 },
    { &g_config.input.peripherals.port4.peripheral_param_0,
      &g_config.input.peripherals.port4.peripheral_param_1 }
};

int *g_keyboard_scancode_map[25] = {
    &g_config.input.keyboard_controller_scancode_map.a,
    &g_config.input.keyboard_controller_scancode_map.b,
    &g_config.input.keyboard_controller_scancode_map.x,
    &g_config.input.keyboard_controller_scancode_map.y,
    &g_config.input.keyboard_controller_scancode_map.back,
    &g_config.input.keyboard_controller_scancode_map.guide,
    &g_config.input.keyboard_controller_scancode_map.start,
    &g_config.input.keyboard_controller_scancode_map.lstick_btn,
    &g_config.input.keyboard_controller_scancode_map.rstick_btn,
    &g_config.input.keyboard_controller_scancode_map.white,
    &g_config.input.keyboard_controller_scancode_map.black,
    &g_config.input.keyboard_controller_scancode_map.dpad_up,
    &g_config.input.keyboard_controller_scancode_map.dpad_down,
    &g_config.input.keyboard_controller_scancode_map.dpad_left,
    &g_config.input.keyboard_controller_scancode_map.dpad_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_up,
    &g_config.input.keyboard_controller_scancode_map.lstick_left,
    &g_config.input.keyboard_controller_scancode_map.lstick_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_down,
    &g_config.input.keyboard_controller_scancode_map.ltrigger,
    &g_config.input.keyboard_controller_scancode_map.rstick_up,
    &g_config.input.keyboard_controller_scancode_map.rstick_left,
    &g_config.input.keyboard_controller_scancode_map.rstick_right,
    &g_config.input.keyboard_controller_scancode_map.rstick_down,
    &g_config.input.keyboard_controller_scancode_map.rtrigger,
};

static void check_and_reset_in_range(int *btn, int min, int max,
                                     const char *message)
{
    if (*btn < min || *btn >= max) {
        fprintf(stderr, "%s\n", message);
        *btn = min;
    }
}

static void xemu_input_bindings_set_in_range(ControllerState *con)
{
#define CHECK_RESET_BUTTON(btn)                                            \
    check_and_reset_in_range(&con->controller_map->controller_mapping.btn, \
                             SDL_GAMEPAD_BUTTON_INVALID,                   \
                             SDL_GAMEPAD_BUTTON_COUNT,                     \
                             "Invalid entry for button " #btn ", resetting")

    CHECK_RESET_BUTTON(a);
    CHECK_RESET_BUTTON(b);
    CHECK_RESET_BUTTON(x);
    CHECK_RESET_BUTTON(y);
    CHECK_RESET_BUTTON(dpad_left);
    CHECK_RESET_BUTTON(dpad_up);
    CHECK_RESET_BUTTON(dpad_right);
    CHECK_RESET_BUTTON(dpad_down);
    CHECK_RESET_BUTTON(back);
    CHECK_RESET_BUTTON(start);
    CHECK_RESET_BUTTON(lshoulder);
    CHECK_RESET_BUTTON(rshoulder);
    CHECK_RESET_BUTTON(lstick_btn);
    CHECK_RESET_BUTTON(rstick_btn);
    CHECK_RESET_BUTTON(guide);

#undef CHECK_RESET_BUTTON

#define CHECK_RESET_AXIS(axis)                                              \
    check_and_reset_in_range(&con->controller_map->controller_mapping.axis, \
                             SDL_GAMEPAD_AXIS_INVALID,                      \
                             SDL_GAMEPAD_AXIS_COUNT,                        \
                             "Invalid entry for button " #axis ", resetting")

    CHECK_RESET_AXIS(axis_trigger_left);
    CHECK_RESET_AXIS(axis_trigger_right);
    CHECK_RESET_AXIS(axis_left_x);
    CHECK_RESET_AXIS(axis_left_y);
    CHECK_RESET_AXIS(axis_right_x);
    CHECK_RESET_AXIS(axis_right_y);

#undef CHECK_RESET_AXIS
}

static bool xemu_input_get_gamepad_mapping_id(ControllerState *con,
                                              char *guid, size_t guid_size)
{
    if (con->type == INPUT_DEVICE_SDL_GAMEPAD) {
        SDL_GUIDToString(con->sdl_joystick_guid, guid, guid_size);
        return true;
    }

#ifdef CONFIG_UWP
    if (con->type == INPUT_DEVICE_UWP_GAMEPAD) {
        snprintf(guid, guid_size, "uwp-gamepad-%d", con->uwp_gamepad_index);
        return true;
    }
#endif

    return false;
}

static void xemu_input_bindings_reload_map(ControllerState *con)
{
    char guid[35] = { 0 };
    if (!xemu_input_get_gamepad_mapping_id(con, guid, sizeof(guid))) {
        return;
    }
    if (!xemu_settings_load_gamepad_mapping(guid, &con->controller_map)) {
        return;
    }
    xemu_input_bindings_set_in_range(con);

    // If this controller did not exist in the mapping array, the config will
    // have been reallocated. Any gamepad mapping pointers for other controllers
    // are now invalid, and need to be reloaded.
    ControllerState *iter, *next;
    bool is_new_mapping;
    QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
        if (iter == con ||
            !xemu_input_get_gamepad_mapping_id(iter, guid, sizeof(guid))) {
            continue;
        }

        memset(guid, 0, sizeof(guid));
        xemu_input_get_gamepad_mapping_id(iter, guid, sizeof(guid));

        is_new_mapping =
            xemu_settings_load_gamepad_mapping(guid, &iter->controller_map);
        assert(!is_new_mapping &&
               "Existing controller GUIDs should exist in the config");

        xemu_input_bindings_set_in_range(iter);
    }
}

static const char *get_bound_driver(int port)
{
    assert(port >= 0 && port <= 3);
    const char *driver = *port_index_to_driver_settings_key_map[port];

    // If the driver in the config is NULL, empty, or unrecognized 
    // then default to DRIVER_DUKE
    if (driver == NULL)
        return DRIVER_DUKE;
    if (strlen(driver) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_DUKE) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_S) == 0)
        return DRIVER_S;
    if (strcmp(driver, DRIVER_LIGHTGUN) == 0)
        return DRIVER_LIGHTGUN;

    return DRIVER_DUKE;
}

static const int port_map[4] = { 3, 4, 1, 2 };

#ifdef CONFIG_UWP
static int last_uwp_gamepad_count = -1;
static int64_t last_uwp_gamepad_scan_ts;
static char uwp_gamepad_names[XEMU_UWP_MAX_GAMEPADS][64];

static ControllerState *xemu_input_find_uwp_gamepad(int index)
{
    ControllerState *iter;
    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        if (iter->type == INPUT_DEVICE_UWP_GAMEPAD &&
            iter->uwp_gamepad_index == index) {
            return iter;
        }
    }

    return NULL;
}

static void xemu_input_free_controller(ControllerState *state)
{
    for (int i = 0; i < 2; i++) {
        if (state->peripherals[i]) {
            g_free(state->peripherals[i]);
        }
    }
    free(state);
}

static void xemu_input_remove_uwp_gamepad(ControllerState *state)
{
    fprintf(stderr, "xemu input: Windows.Gaming.Input gamepad removed "
            "index=%d name='%s'\n",
            state->uwp_gamepad_index, state->name);

    if (state->bound >= 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Port %d disconnected", state->bound + 1);
        xemu_queue_notification(buf);
        xemu_input_bind(state->bound, NULL, 0);
    }

    QTAILQ_REMOVE(&available_controllers, state, entry);
    xemu_input_free_controller(state);
}

static void xemu_input_remove_sdl_gamepads_for_uwp(void)
{
    ControllerState *iter, *next;
    QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
        if (iter->type != INPUT_DEVICE_SDL_GAMEPAD) {
            continue;
        }

        fprintf(stderr,
                "xemu input: replacing SDL gamepad with native "
                "Windows.Gaming.Input path name='%s' instance=%d\n",
                iter->name ? iter->name : "(null)", iter->sdl_joystick_id);

        if (iter->bound >= 0) {
            xemu_input_bind(iter->bound, NULL, 0);
        }

        QTAILQ_REMOVE(&available_controllers, iter, entry);
        if (iter->sdl_gamepad) {
            SDL_CloseGamepad(iter->sdl_gamepad);
        }
        xemu_input_free_controller(iter);
    }
}

static void xemu_input_add_uwp_gamepad(int index)
{
    if (xemu_input_find_uwp_gamepad(index)) {
        return;
    }

    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_UWP_GAMEPAD;
    snprintf(uwp_gamepad_names[index], sizeof(uwp_gamepad_names[index]),
             "Xbox UWP Gamepad %d", index + 1);
    new_con->name = uwp_gamepad_names[index];
    new_con->uwp_gamepad_index = index;
    new_con->bound = -1;
    new_con->peripheral_types[0] = PERIPHERAL_NONE;
    new_con->peripheral_types[1] = PERIPHERAL_NONE;
    new_con->peripherals[0] = NULL;
    new_con->peripherals[1] = NULL;

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
    xemu_input_bindings_reload_map(new_con);

    fprintf(stderr,
            "xemu input: Windows.Gaming.Input gamepad added index=%d "
            "name='%s'\n",
            index, new_con->name);

    int port = 0;
    bool did_bind = false;
    while (!did_bind) {
        port = xemu_input_get_controller_default_bind_port(new_con, port);
        if (port < 0) {
            break;
        } else if (xemu_input_port_accepts_local(port)) {
            xemu_input_bind(port, new_con, 0);
            did_bind = true;
            break;
        } else {
            port++;
        }
    }

    if (!did_bind && g_config.input.auto_bind) {
        for (port = 0; port < 4; port++) {
            if (xemu_input_port_accepts_local(port)) {
                xemu_input_bind(port, new_con, 1);
                did_bind = true;
                break;
            }
        }
    }

    if (did_bind) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d",
                 new_con->name, port + 1);
        xemu_queue_notification(buf);
        xemu_input_rebind_xmu(port);
    }
}

static void xemu_input_scan_uwp_gamepads(bool force)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (!force && ABS(now - last_uwp_gamepad_scan_ts) < 1000000) {
        return;
    }
    last_uwp_gamepad_scan_ts = now;

    int count = xemu_uwp_gamepad_count();
    if (count != last_uwp_gamepad_count) {
        fprintf(stderr,
                "xemu input: Windows.Gaming.Input gamepad count=%d\n",
                count);
        last_uwp_gamepad_count = count;
    }

    if (count > 0) {
        xemu_input_remove_sdl_gamepads_for_uwp();
    }

    ControllerState *iter, *next;
    QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
        if (iter->type == INPUT_DEVICE_UWP_GAMEPAD &&
            iter->uwp_gamepad_index >= count) {
            xemu_input_remove_uwp_gamepad(iter);
        }
    }

    for (int i = 0; i < count; i++) {
        xemu_input_add_uwp_gamepad(i);
    }
}
#endif

static void xemu_input_init_dsu_controllers(void)
{
    for (unsigned player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        ControllerState *state = &dsu_controllers[player];

        memset(state, 0, sizeof(*state));
        snprintf(dsu_controller_names[player],
                 sizeof(dsu_controller_names[player]),
                 "DSU Player %u", player + 1);
        state->type = INPUT_DEVICE_DSU_GAMEPAD;
        state->name = dsu_controller_names[player];
        state->bound = -1;
        state->peripheral_types[0] = PERIPHERAL_NONE;
        state->peripheral_types[1] = PERIPHERAL_NONE;
        xemu_pointer_aim_reset(&dsu_pointer_aim[player]);
        xemu_input_reset_dsu_motion_mapper(
            player, xemu_input_get_dsu_motion_config(player).target);
    }
}

static void xemu_input_restore_saved_binding(unsigned port)
{
    ControllerState *iter;

    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        if (iter->bound < 0 &&
            xemu_input_get_controller_default_bind_port(iter, 0) ==
                (int)port) {
            xemu_input_bind(port, iter, 0);
            xemu_input_rebind_xmu(port);
            return;
        }
    }
}

static void xemu_input_sync_dsu_bindings(void)
{
    for (unsigned player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        XemuDSUPlayerState dsu;
        XemuDSUMotionConfig motion =
            xemu_input_get_dsu_motion_config(player);
        ControllerState *synthetic = &dsu_controllers[player];
        bool enabled = xemu_dsu_get_player_state(player, &dsu) &&
                       ((dsu.enabled_data_types &
                         (XEMU_DSU_DATA_GAMEPAD | XEMU_DSU_DATA_TOUCH |
                          XEMU_DSU_DATA_MOUSE)) ||
                        ((dsu.enabled_data_types & XEMU_DSU_DATA_MOTION) &&
                         motion.target != XEMU_DSU_MOTION_MOUSE));

        if (enabled && bound_controllers[player] == NULL) {
            xemu_input_bind(player, synthetic, 0);
            char message[96];
            snprintf(message, sizeof(message),
                     "DSU input configured for port %u", player + 1);
            xemu_queue_notification(message);
        } else if (!enabled && bound_controllers[player] == synthetic) {
            xemu_input_bind(player, NULL, 0);
            xemu_input_restore_saved_binding(player);
        }
    }
}

static void xemu_input_update_dsu_motion_mouse(void)
{
    bool queued = false;

    for (unsigned player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        XemuDSUPlayerState dsu;
        XemuDSUMotionConfig config =
            xemu_input_get_dsu_motion_config(player);
        XemuDSUMotionMapperState *state = &dsu_motion_mapper[player];
        double dt;

        if (!config.enabled || config.target != XEMU_DSU_MOTION_MOUSE ||
            !xemu_dsu_get_player_state(player, &dsu) ||
            !(dsu.enabled_data_types & XEMU_DSU_DATA_MOTION) ||
            !dsu.controller.connected) {
            if (state->target_valid &&
                state->target == XEMU_DSU_MOTION_MOUSE) {
                xemu_input_reset_dsu_motion_mapper(player, config.target);
            }
            continue;
        }
        if (!xemu_input_take_dsu_motion_sample(
                player, config.target, &dsu.controller, &dt)) {
            continue;
        }

        /* DSU gyroscope values are in radians/second. Convert yaw and pitch
         * into relative pointer movement and retain fractional pixels so
         * low-rate motion stays smooth. */
        state->mouse_remainder_x +=
            dsu.controller.gyroscope[1] * config.horizontal_sensitivity *
            dt * 1000.0;
        state->mouse_remainder_y +=
            -dsu.controller.gyroscope[0] * config.vertical_sensitivity *
            dt * 1000.0;
        int dx = (int)state->mouse_remainder_x;
        int dy = (int)state->mouse_remainder_y;
        state->mouse_remainder_x -= dx;
        state->mouse_remainder_y -= dy;
        if (dx != 0) {
            qemu_input_queue_rel(NULL, INPUT_AXIS_X, dx);
            queued = true;
        }
        if (dy != 0) {
            qemu_input_queue_rel(NULL, INPUT_AXIS_Y, dy);
            queued = true;
        }
        if ((dx != 0 || dy != 0) && !state->logged_mapping) {
            fprintf(stderr,
                    "xemu input: DSU motion player %u -> mouse "
                    "delta={%d,%d} sensitivity={%.2f,%.2f}\n",
                    player + 1, dx, dy, config.horizontal_sensitivity,
                    config.vertical_sensitivity);
            state->logged_mapping = true;
        }
    }
    if (queued) {
        qemu_input_event_sync();
    }
}

void xemu_input_init(void)
{
    xemu_dsu_init();
    xemu_input_init_dsu_controllers();

    if (g_config.input.background_input_capture) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "Failed to initialize SDL gamepad subsystem: %s\n", SDL_GetError());
        exit(1);
    }

    // Create the keyboard input (always first)
    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_SDL_KEYBOARD;
    new_con->name = "Keyboard";
    new_con->bound = -1;
    new_con->peripheral_types[0] = PERIPHERAL_NONE;
    new_con->peripheral_types[1] = PERIPHERAL_NONE;
    new_con->peripherals[0] = NULL;
    new_con->peripherals[1] = NULL;

    for (int i = 0; i < 25; i++) {
        static const char *format_str =
            "WARNING: Keyboard controller map scancode out of range "
            "(%d) : Disabled\n";
        char buf[128];
        snprintf(buf, sizeof(buf), format_str, i);
        check_and_reset_in_range(g_keyboard_scancode_map[i],
                                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_COUNT, buf);
    }

    bound_drivers[0] = get_bound_driver(0);
    bound_drivers[1] = get_bound_driver(1);
    bound_drivers[2] = get_bound_driver(2);
    bound_drivers[3] = get_bound_driver(3);

    // Check to see if we should auto-bind the keyboard
    int port = xemu_input_get_controller_default_bind_port(new_con, 0);
    if (port >= 0) {
        xemu_input_bind(port, new_con, 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
        xemu_queue_notification(buf);
        xemu_input_rebind_xmu(port);
    }

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);

#ifdef CONFIG_UWP
    xemu_input_scan_uwp_gamepads(true);
#endif
}

int xemu_input_get_controller_default_bind_port(ControllerState *state, int start)
{
    char guid[35] = { 0 };
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        snprintf(guid, sizeof(guid), "keyboard");
#ifdef CONFIG_UWP
    } else if (state->type == INPUT_DEVICE_UWP_GAMEPAD) {
        snprintf(guid, sizeof(guid), "uwp-gamepad-%d",
                 state->uwp_gamepad_index);
#endif
    }

    for (int i = start; i < 4; i++) {
        if (strcmp(guid, *port_index_to_settings_key_map[i]) == 0) {
            return i;
        }
    }

    return -1;
}

void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter)
{
    int *peripheral_type_ptr =
        peripheral_types_settings_map[player_index][peripheral_index];
    const char **peripheral_param_ptr =
        peripheral_params_settings_map[player_index][peripheral_index];

    assert(peripheral_type_ptr);
    assert(peripheral_param_ptr);

    *peripheral_type_ptr = peripheral_type;
    xemu_settings_set_string(
        peripheral_param_ptr,
        peripheral_parameter == NULL ? "" : peripheral_parameter);
}

void xemu_input_process_sdl_events(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        DPRINTF("Controller Added: %d\n", event->gdevice.which);

#ifdef CONFIG_UWP
        int uwp_count = xemu_uwp_gamepad_count();
        if (uwp_count > 0) {
            fprintf(stderr,
                    "xemu input: ignoring SDL gamepad add instance=%d "
                    "because Windows.Gaming.Input reports %d native "
                    "gamepad(s)\n",
                    event->gdevice.which, uwp_count);
            return;
        }
#endif

        // Attempt to open the added controller
        SDL_Gamepad *sdl_con;
        sdl_con = SDL_OpenGamepad(event->gdevice.which);
        if (sdl_con == NULL) {
            fprintf(stderr,
                    "xemu input: SDL_OpenGamepad failed instance=%d "
                    "error='%s'\n",
                    event->gdevice.which, SDL_GetError());
            return;
        }

        // Success! Create a new node to track this controller and continue init
        ControllerState *new_con = malloc(sizeof(ControllerState));
        memset(new_con, 0, sizeof(ControllerState));
        new_con->type                 = INPUT_DEVICE_SDL_GAMEPAD;
        new_con->name                 = SDL_GetGamepadName(sdl_con);
        new_con->sdl_gamepad          = sdl_con;
        new_con->sdl_joystick         = SDL_GetGamepadJoystick(new_con->sdl_gamepad);
        new_con->sdl_joystick_id      = SDL_GetJoystickID(new_con->sdl_joystick);
        new_con->sdl_joystick_guid    = SDL_GetJoystickGUID(new_con->sdl_joystick);
        new_con->bound                = -1;
        new_con->peripheral_types[0] = PERIPHERAL_NONE;
        new_con->peripheral_types[1] = PERIPHERAL_NONE;
        new_con->peripherals[0] = NULL;
        new_con->peripherals[1] = NULL;

        char guid_buf[35] = { 0 };
        SDL_GUIDToString(new_con->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
        DPRINTF("Opened %s (%s)\n", new_con->name, guid_buf);

        QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
        xemu_input_bindings_reload_map(new_con);

        // Do not replace binding for a currently bound device. In the case that
        // the same GUID is specified multiple times, on different ports, allow
        // any available port to be bound.
        //
        // This can happen naturally with X360 wireless receiver, in which each
        // controller gets the same GUID (go figure). We cannot remember which
        // controller is which in this case, but we can try to tolerate this
        // situation by binding to any previously bound port with this GUID. The
        // upside in this case is that a person can use the same GUID on all
        // ports and just needs to bind to the receiver and never needs to hit
        // this dialog.


        // Attempt to re-bind to port previously bound to
        int port = 0;
        bool did_bind = false;
        while (!did_bind) {
            port = xemu_input_get_controller_default_bind_port(new_con, port);
            if (port < 0) {
                // No (additional) default mappings
                break;
            } else if (xemu_input_port_accepts_local(port)) {
                xemu_input_bind(port, new_con, 0);
                did_bind = true;
                break;
            } else {
                // Try again for another port
                port++;
            }
        }

        // Try to bind to any open port, and if so remember the binding
        if (!did_bind && g_config.input.auto_bind) {
            for (port = 0; port < 4; port++) {
                if (xemu_input_port_accepts_local(port)) {
                    xemu_input_bind(port, new_con, 1);
                    did_bind = true;
                    break;
                }
            }
        }

        if (did_bind) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
            xemu_queue_notification(buf);
            xemu_input_rebind_xmu(port);
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        DPRINTF("Controller Removed: %d\n", event->gdevice.which);
        int handled = 0;
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE(iter, &available_controllers, entry, next) {
            if (iter->type != INPUT_DEVICE_SDL_GAMEPAD) continue;

            if (iter->sdl_joystick_id == event->gdevice.which) {
                DPRINTF("Device removed: %s\n", iter->name);

                // Disconnect
                if (iter->bound >= 0) {
                    // Queue a notification to inform user controller disconnected
                    // FIXME: Probably replace with a callback registration thing,
                    // but this works well enough for now.
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Port %d disconnected", iter->bound+1);
                    xemu_queue_notification(buf);

                    // Unbind the controller, but don't save the unbinding in
                    // case the controller is reconnected
                    xemu_input_bind(iter->bound, NULL, 0);
                }

                // Unlink
                QTAILQ_REMOVE(&available_controllers, iter, entry);

                // Deallocate
                if (iter->sdl_gamepad) {
                    SDL_CloseGamepad(iter->sdl_gamepad);
                }

                for (int i = 0; i < 2; i++) {
                    if (iter->peripherals[i])
                        g_free(iter->peripherals[i]);
                }
                free(iter);

                handled = 1;
                break;
            }
        }
        if (!handled) {
            DPRINTF("Could not find handle for joystick instance\n");
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMAPPED) {
        DPRINTF("Controller Remapped: %d\n", event->gdevice.which);
    }
}

void xemu_input_update_controller(ControllerState *state)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_input_updated_ts) <
        XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US) {
        return;
    }

    if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_input_update_sdl_kbd_controller_state(state);
    } else if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        xemu_input_update_sdl_controller_state(state);
#ifdef CONFIG_UWP
    } else if (state->type == INPUT_DEVICE_UWP_GAMEPAD) {
        xemu_input_update_uwp_gamepad_controller_state(state);
#endif
    }

    state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

void xemu_input_update_controllers(void)
{
    static bool dsu_was_active;

    xemu_dsu_process_pending();
    bool dsu_active = xemu_dsu_is_active();
    if (dsu_active || dsu_was_active) {
        /* Run once on the active -> disabled edge to unbind synthetic
         * controllers and clear motion mapper state. */
        xemu_input_update_dsu_motion_mouse();
        xemu_input_sync_dsu_bindings();
    }
    dsu_was_active = dsu_active;

#ifdef CONFIG_UWP
    xemu_input_scan_uwp_gamepads(false);
#endif

    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_controller(iter);
    }
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_rumble(iter);
    }
}

void xemu_input_update_sdl_kbd_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    const bool *kbd = SDL_GetKeyboardState(NULL);

#define KBD_STATE(btn) \
    (kbd[g_config.input.keyboard_controller_scancode_map.btn])

    state->buttons |= KBD_STATE(a) << 0;
    state->buttons |= KBD_STATE(b) << 1;
    state->buttons |= KBD_STATE(x) << 2;
    state->buttons |= KBD_STATE(y) << 3;
    state->buttons |= KBD_STATE(dpad_left) << 4;
    state->buttons |= KBD_STATE(dpad_up) << 5;
    state->buttons |= KBD_STATE(dpad_right) << 6;
    state->buttons |= KBD_STATE(dpad_down) << 7;
    state->buttons |= KBD_STATE(back) << 8;
    state->buttons |= KBD_STATE(start) << 9;
    state->buttons |= KBD_STATE(white) << 10;
    state->buttons |= KBD_STATE(black) << 11;
    state->buttons |= KBD_STATE(lstick_btn) << 12;
    state->buttons |= KBD_STATE(rstick_btn) << 13;
    state->buttons |= KBD_STATE(guide) << 14;

    if (KBD_STATE(lstick_up))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = 32767;
    if (KBD_STATE(lstick_left))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = -32768;
    if (KBD_STATE(lstick_right))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = 32767;
    if (KBD_STATE(lstick_down))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = -32768;
    if (KBD_STATE(ltrigger))
        state->axis[CONTROLLER_AXIS_LTRIG] = 32767;

    if (KBD_STATE(rstick_up))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = 32767;
    if (KBD_STATE(rstick_left))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = -32768;
    if (KBD_STATE(rstick_right))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = 32767;
    if (KBD_STATE(rstick_down))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = -32768;
    if (KBD_STATE(rtrigger))
        state->axis[CONTROLLER_AXIS_RTRIG] = 32767;

#undef KBD_STATE
}

static void xemu_input_apply_gamepad_axis_inversion(ControllerState *state);

void xemu_input_update_sdl_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

#define SDL_MASK_BUTTON(state, btn, idx)                  \
    (SDL_GetGamepadButton(                                \
         (state)->sdl_gamepad,                            \
         (state)->controller_map->controller_mapping.btn) \
     << idx)

    state->buttons |= SDL_MASK_BUTTON(state, a, 0);
    state->buttons |= SDL_MASK_BUTTON(state, b, 1);
    state->buttons |= SDL_MASK_BUTTON(state, x, 2);
    state->buttons |= SDL_MASK_BUTTON(state, y, 3);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_left, 4);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_up, 5);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_right, 6);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_down, 7);
    state->buttons |= SDL_MASK_BUTTON(state, back, 8);
    state->buttons |= SDL_MASK_BUTTON(state, start, 9);
    state->buttons |= SDL_MASK_BUTTON(state, lshoulder, 10);
    state->buttons |= SDL_MASK_BUTTON(state, rshoulder, 11);
    state->buttons |= SDL_MASK_BUTTON(state, lstick_btn, 12);
    state->buttons |= SDL_MASK_BUTTON(state, rstick_btn, 13);
    state->buttons |= SDL_MASK_BUTTON(state, guide, 14);

#undef SDL_MASK_BUTTON

#define SDL_GET_AXIS(state, axis)    \
    SDL_GetGamepadAxis(              \
        (state)->sdl_gamepad,        \
        (state)->controller_map->controller_mapping.axis)

    state->axis[0] = SDL_GET_AXIS(state, axis_trigger_left);
    state->axis[1] = SDL_GET_AXIS(state, axis_trigger_right);
    state->axis[2] = SDL_GET_AXIS(state, axis_left_x);
    state->axis[3] = SDL_GET_AXIS(state, axis_left_y);
    state->axis[4] = SDL_GET_AXIS(state, axis_right_x);
    state->axis[5] = SDL_GET_AXIS(state, axis_right_y);

#undef SDL_GET_AXIS

    xemu_input_apply_gamepad_axis_inversion(state);

    // xemu_input_print_controller_state(state);
}

static void xemu_input_apply_gamepad_axis_inversion(ControllerState *state)
{
#define INVERT_AXIS(controller_axis)                                \
    state->axis[controller_axis] = -1 - state->axis[controller_axis]

    if (state->controller_map->controller_mapping.invert_axis_left_x) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_left_y) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_Y);
    }

    if (state->controller_map->controller_mapping.invert_axis_right_x) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_right_y) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_Y);
    }

#undef INVERT_AXIS
}

#ifdef CONFIG_UWP
static bool xemu_input_uwp_mapped_button(
    const XemuUWPGamepadState *state, int button)
{
    switch ((SDL_GamepadButton)button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_A;
    case SDL_GAMEPAD_BUTTON_EAST:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_B;
    case SDL_GAMEPAD_BUTTON_WEST:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_X;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_Y;
    case SDL_GAMEPAD_BUTTON_BACK:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_BACK;
    case SDL_GAMEPAD_BUTTON_START:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_LSTICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_RSTICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_WHITE;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_BLACK;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return state->buttons & XEMU_UWP_GAMEPAD_BUTTON_DPAD_RIGHT;
    default:
        return false;
    }
}

static int16_t xemu_input_uwp_mapped_axis(
    const XemuUWPGamepadState *state, int axis)
{
    switch ((SDL_GamepadAxis)axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        return state->axis[2];
    case SDL_GAMEPAD_AXIS_LEFTY:
        return -1 - state->axis[3];
    case SDL_GAMEPAD_AXIS_RIGHTX:
        return state->axis[4];
    case SDL_GAMEPAD_AXIS_RIGHTY:
        return -1 - state->axis[5];
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        return state->axis[0];
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return state->axis[1];
    default:
        return 0;
    }
}

void xemu_input_update_uwp_gamepad_controller_state(ControllerState *state)
{
    XemuUWPGamepadState uwp_state;
    bool has_input = false;

    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    if (!xemu_uwp_gamepad_get_state(state->uwp_gamepad_index, &uwp_state)) {
        return;
    }

    if (state->controller_map == NULL) {
        xemu_input_bindings_reload_map(state);
    }
    if (state->controller_map == NULL) {
        return;
    }

#define UWP_MASK_BUTTON(state, btn, idx)                                \
    (xemu_input_uwp_mapped_button(                                       \
         &uwp_state,                                                     \
         (state)->controller_map->controller_mapping.btn)                \
     << idx)

    state->buttons |= UWP_MASK_BUTTON(state, a, 0);
    state->buttons |= UWP_MASK_BUTTON(state, b, 1);
    state->buttons |= UWP_MASK_BUTTON(state, x, 2);
    state->buttons |= UWP_MASK_BUTTON(state, y, 3);
    state->buttons |= UWP_MASK_BUTTON(state, dpad_left, 4);
    state->buttons |= UWP_MASK_BUTTON(state, dpad_up, 5);
    state->buttons |= UWP_MASK_BUTTON(state, dpad_right, 6);
    state->buttons |= UWP_MASK_BUTTON(state, dpad_down, 7);
    state->buttons |= UWP_MASK_BUTTON(state, back, 8);
    state->buttons |= UWP_MASK_BUTTON(state, start, 9);
    state->buttons |= UWP_MASK_BUTTON(state, lshoulder, 10);
    state->buttons |= UWP_MASK_BUTTON(state, rshoulder, 11);
    state->buttons |= UWP_MASK_BUTTON(state, lstick_btn, 12);
    state->buttons |= UWP_MASK_BUTTON(state, rstick_btn, 13);
    state->buttons |= UWP_MASK_BUTTON(state, guide, 14);

#undef UWP_MASK_BUTTON

#define UWP_GET_AXIS(state, axis)                                      \
    xemu_input_uwp_mapped_axis(                                        \
        &uwp_state, (state)->controller_map->controller_mapping.axis)

    state->axis[0] = UWP_GET_AXIS(state, axis_trigger_left);
    state->axis[1] = UWP_GET_AXIS(state, axis_trigger_right);
    state->axis[2] = UWP_GET_AXIS(state, axis_left_x);
    state->axis[3] = UWP_GET_AXIS(state, axis_left_y);
    state->axis[4] = UWP_GET_AXIS(state, axis_right_x);
    state->axis[5] = UWP_GET_AXIS(state, axis_right_y);

#undef UWP_GET_AXIS

    xemu_input_apply_gamepad_axis_inversion(state);

    for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
        if (state->axis[i] > 3276 || state->axis[i] < -3276) {
            has_input = true;
        }
    }
    state->uwp_gamepad_last_timestamp = uwp_state.timestamp;

    if (state->buttons) {
        has_input = true;
    }

    if (has_input && !state->uwp_gamepad_logged_input) {
        fprintf(stderr,
                "xemu input: first Windows.Gaming.Input gamepad input "
                "index=%d buttons=0x%04x axes={lt=%d,rt=%d,lx=%d,ly=%d,"
                "rx=%d,ry=%d} timestamp=%" PRIu64 "\n",
                state->uwp_gamepad_index, state->buttons,
                state->axis[CONTROLLER_AXIS_LTRIG],
                state->axis[CONTROLLER_AXIS_RTRIG],
                state->axis[CONTROLLER_AXIS_LSTICK_X],
                state->axis[CONTROLLER_AXIS_LSTICK_Y],
                state->axis[CONTROLLER_AXIS_RSTICK_X],
                state->axis[CONTROLLER_AXIS_RSTICK_Y],
                state->uwp_gamepad_last_timestamp);
        state->uwp_gamepad_logged_input = true;
    }
}
#endif

void xemu_input_update_rumble(ControllerState *state)
{
#ifdef CONFIG_UWP
    if (state->type == INPUT_DEVICE_UWP_GAMEPAD) {
        if (state->controller_map == NULL ||
            !state->controller_map->enable_rumble) {
            return;
        }
        if (state->uwp_last_rumble_valid &&
            state->uwp_last_rumble_l == state->rumble_l &&
            state->uwp_last_rumble_r == state->rumble_r) {
            return;
        }
        int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (ABS(now - state->last_rumble_updated_ts) <
            XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US) {
            return;
        }

        xemu_uwp_gamepad_set_vibration(state->uwp_gamepad_index,
                                       state->rumble_l, state->rumble_r);
        state->uwp_last_rumble_l = state->rumble_l;
        state->uwp_last_rumble_r = state->rumble_r;
        state->uwp_last_rumble_valid = true;
        state->last_rumble_updated_ts =
            qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        return;
    }
#endif

    if (state->type != INPUT_DEVICE_SDL_GAMEPAD) {
        return;
    }

    if (!state->controller_map->enable_rumble) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_rumble_updated_ts) <
        XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US) {
        return;
    }

    SDL_RumbleGamepad(state->sdl_gamepad, state->rumble_l, state->rumble_r, 250);
    state->last_rumble_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

static void xemu_input_map_dsu_gamepad(const XemuDSUControllerState *dsu,
                                       XemuInputSample *sample)
{
    uint16_t buttons = dsu->buttons;

    if (buttons & DSU_BTN_CROSS)      sample->buttons |= CONTROLLER_BUTTON_A;
    if (buttons & DSU_BTN_CIRCLE)     sample->buttons |= CONTROLLER_BUTTON_B;
    if (buttons & DSU_BTN_SQUARE)     sample->buttons |= CONTROLLER_BUTTON_X;
    if (buttons & DSU_BTN_TRIANGLE)   sample->buttons |= CONTROLLER_BUTTON_Y;
    if (buttons & DSU_BTN_DPAD_LEFT)  sample->buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
    if (buttons & DSU_BTN_DPAD_UP)    sample->buttons |= CONTROLLER_BUTTON_DPAD_UP;
    if (buttons & DSU_BTN_DPAD_RIGHT) sample->buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;
    if (buttons & DSU_BTN_DPAD_DOWN)  sample->buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
    if (buttons & DSU_BTN_SHARE)      sample->buttons |= CONTROLLER_BUTTON_BACK;
    if (buttons & DSU_BTN_OPTIONS)    sample->buttons |= CONTROLLER_BUTTON_START;
    if (buttons & DSU_BTN_L1)         sample->buttons |= CONTROLLER_BUTTON_WHITE;
    if (buttons & DSU_BTN_R1)         sample->buttons |= CONTROLLER_BUTTON_BLACK;
    if (buttons & DSU_BTN_L3)         sample->buttons |= CONTROLLER_BUTTON_LSTICK;
    if (buttons & DSU_BTN_R3)         sample->buttons |= CONTROLLER_BUTTON_RSTICK;
    if (dsu->home)                    sample->buttons |= CONTROLLER_BUTTON_GUIDE;

    sample->axis[CONTROLLER_AXIS_LTRIG] =
        dsu->left_trigger ? (int16_t)(dsu->left_trigger * 128) :
        ((buttons & DSU_BTN_L2) ? INT16_MAX : 0);
    sample->axis[CONTROLLER_AXIS_RTRIG] =
        dsu->right_trigger ? (int16_t)(dsu->right_trigger * 128) :
        ((buttons & DSU_BTN_R2) ? INT16_MAX : 0);
    sample->axis[CONTROLLER_AXIS_LSTICK_X] = dsu->axes[0];
    sample->axis[CONTROLLER_AXIS_LSTICK_Y] = dsu->axes[1];
    sample->axis[CONTROLLER_AXIS_RSTICK_X] = dsu->axes[2];
    sample->axis[CONTROLLER_AXIS_RSTICK_Y] = dsu->axes[3];
}

static void xemu_input_map_dsu_motion(unsigned player,
                                      const XemuDSUControllerState *controller,
                                      const XemuDSUMotionConfig *config,
                                      XemuInputSample *sample)
{
    float horizontal = controller->gyroscope[1] *
                       config->horizontal_sensitivity;
    float vertical = -controller->gyroscope[0] *
                     config->vertical_sensitivity;
    int16_t axis_x = xemu_input_motion_axis(horizontal);
    int16_t axis_y = xemu_input_motion_axis(vertical);

    switch (config->target) {
    case XEMU_DSU_MOTION_RIGHT_STICK:
        sample->axis[CONTROLLER_AXIS_RSTICK_X] = axis_x;
        sample->axis[CONTROLLER_AXIS_RSTICK_Y] = axis_y;
        break;
    case XEMU_DSU_MOTION_LEFT_STICK:
        sample->axis[CONTROLLER_AXIS_LSTICK_X] = axis_x;
        sample->axis[CONTROLLER_AXIS_LSTICK_Y] = axis_y;
        break;
    case XEMU_DSU_MOTION_DPAD:
        if (axis_x <= -8192) {
            sample->buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
        } else if (axis_x >= 8192) {
            sample->buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;
        }
        if (axis_y <= -8192) {
            sample->buttons |= CONTROLLER_BUTTON_DPAD_UP;
        } else if (axis_y >= 8192) {
            sample->buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
        }
        break;
    case XEMU_DSU_MOTION_LIGHTGUN: {
        XemuDSUMotionMapperState *state = &dsu_motion_mapper[player];
        double dt;

        if (xemu_input_take_dsu_motion_sample(
                player, config->target, controller, &dt)) {
            /* A 90-degree rotation moves across the full lightgun range at
             * 1.0x sensitivity. The state is relative and begins centered. */
            const double units_per_radian = 65534.0 / G_PI;
            state->lightgun_x += horizontal * dt * units_per_radian;
            state->lightgun_y += vertical * dt * units_per_radian;
            state->lightgun_x = CLAMP(state->lightgun_x,
                                      (double)INT16_MIN,
                                      (double)INT16_MAX);
            state->lightgun_y = CLAMP(state->lightgun_y,
                                      (double)INT16_MIN,
                                      (double)INT16_MAX);
        }
        sample->axis[CONTROLLER_AXIS_LSTICK_X] =
            (int16_t)lrint(state->lightgun_x);
        sample->axis[CONTROLLER_AXIS_LSTICK_Y] =
            (int16_t)lrint(state->lightgun_y);
        sample->buttons |= CONTROLLER_BUTTON_LIGHTGUN_ONSCREEN;
        break;
    }
    case XEMU_DSU_MOTION_MOUSE:
    default:
        break;
    }

    XemuDSUMotionMapperState *state = &dsu_motion_mapper[player];
    if (!state->logged_mapping &&
        (fabsf(horizontal) >= 0.001f || fabsf(vertical) >= 0.001f)) {
        fprintf(stderr,
                "xemu input: DSU motion player %u -> %s "
                "gyro={%.3f,%.3f} output={buttons=0x%04x,lx=%d,ly=%d,"
                "rx=%d,ry=%d} sensitivity={%.2f,%.2f}\n",
                player + 1,
                xemu_input_dsu_motion_target_name(config->target),
                horizontal, vertical, sample->buttons,
                sample->axis[CONTROLLER_AXIS_LSTICK_X],
                sample->axis[CONTROLLER_AXIS_LSTICK_Y],
                sample->axis[CONTROLLER_AXIS_RSTICK_X],
                sample->axis[CONTROLLER_AXIS_RSTICK_Y],
                config->horizontal_sensitivity,
                config->vertical_sensitivity);
        state->logged_mapping = true;
    }
}

static void xemu_input_merge_sample(XemuInputSample *base,
                                    const XemuInputSample *addon)
{
    base->buttons |= addon->buttons;
    base->axis[CONTROLLER_AXIS_LTRIG] =
        MAX(base->axis[CONTROLLER_AXIS_LTRIG],
            addon->axis[CONTROLLER_AXIS_LTRIG]);
    base->axis[CONTROLLER_AXIS_RTRIG] =
        MAX(base->axis[CONTROLLER_AXIS_RTRIG],
            addon->axis[CONTROLLER_AXIS_RTRIG]);
    for (int axis = CONTROLLER_AXIS_LSTICK_X;
         axis < CONTROLLER_AXIS__COUNT; axis++) {
        if (abs((int)addon->axis[axis]) > abs((int)base->axis[axis])) {
            base->axis[axis] = addon->axis[axis];
        }
    }
}

bool xemu_input_get_dsu_ui_sample(XemuInputSample *out)
{
    bool any = false;

    memset(out, 0, sizeof(*out));
    for (unsigned player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        XemuDSUPlayerState dsu;
        XemuInputSample sample = { 0 };

        if (!xemu_dsu_get_player_state(player, &dsu) ||
            !(dsu.enabled_data_types & XEMU_DSU_DATA_GAMEPAD) ||
            !dsu.controller.connected) {
            continue;
        }
        xemu_input_map_dsu_gamepad(&dsu.controller, &sample);
        out->buttons |= sample.buttons;
        for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
            if (abs(sample.axis[i]) > abs(out->axis[i])) {
                out->axis[i] = sample.axis[i];
            }
        }
        any = true;
    }
    return any;
}

bool xemu_input_get_effective_state(unsigned index, XemuInputSample *out)
{
    XemuDSUPlayerState dsu;
    XemuDSUMotionConfig motion;
    ControllerState *local;
    bool has_gamepad;
    bool has_motion;
    bool uses_controller_port;

    if (index >= XEMU_DSU_MAX_PLAYERS || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    local = bound_controllers[index];
    if (local != NULL && local->type != INPUT_DEVICE_DSU_GAMEPAD) {
        xemu_input_update_controller(local);
        out->buttons = local->buttons;
        memcpy(out->axis, local->axis, sizeof(out->axis));
    }

    if (!xemu_dsu_get_player_state(index, &dsu)) {
        return local != NULL;
    }

    motion = xemu_input_get_dsu_motion_config(index);
    has_gamepad = dsu.enabled_data_types & XEMU_DSU_DATA_GAMEPAD;
    has_motion = motion.enabled &&
                 (dsu.enabled_data_types & XEMU_DSU_DATA_MOTION) &&
                 motion.target != XEMU_DSU_MOTION_MOUSE;
    uses_controller_port = has_gamepad || has_motion ||
                           (dsu.enabled_data_types &
                            (XEMU_DSU_DATA_TOUCH | XEMU_DSU_DATA_MOUSE));
    if (!uses_controller_port) {
        return local != NULL;
    }

    if (!dsu.addon) {
        memset(out, 0, sizeof(*out));
    }

    if (has_gamepad && dsu.controller.connected) {
        XemuInputSample remote = { 0 };
        xemu_input_map_dsu_gamepad(&dsu.controller, &remote);
        xemu_input_merge_sample(out, &remote);
    }

    if (has_motion && dsu.controller.connected &&
        (motion.target != XEMU_DSU_MOTION_LIGHTGUN ||
         strcmp(bound_drivers[index], DRIVER_LIGHTGUN) == 0)) {
        XemuInputSample remote_motion = { 0 };
        xemu_input_map_dsu_motion(index, &dsu.controller, &motion,
                                  &remote_motion);
        xemu_input_merge_sample(out, &remote_motion);
    } else if (!motion.enabled || !dsu.controller.connected) {
        xemu_input_reset_dsu_motion_mapper(index, motion.target);
    }

    if (strcmp(bound_drivers[index], DRIVER_LIGHTGUN) == 0 &&
        (dsu.enabled_data_types & XEMU_DSU_DATA_TOUCH)) {
        XemuPointerAimResult aim;
        const XemuDSUTouchPoint *touch = dsu.controller.touch[0].active
                                             ? &dsu.controller.touch[0]
                                             : &dsu.controller.touch[1];
        bool active = dsu.controller.connected && touch->active;

        xemu_pointer_aim_update_dsu_touch(
            &dsu_pointer_aim[index], (uint16_t)touch->x,
            (uint16_t)touch->y, active,
            g_config.input.lightgun_smoothing,
            g_config.input.lightgun_sensitivity,
            (uint64_t)g_get_monotonic_time() * 1000, &aim);
        if (active) {
            out->axis[CONTROLLER_AXIS_LSTICK_X] = aim.axis_x;
            out->axis[CONTROLLER_AXIS_LSTICK_Y] = aim.axis_y;
            if (aim.onscreen) {
                out->buttons |= CONTROLLER_BUTTON_LIGHTGUN_ONSCREEN;
            }
        }
    }

    if (strcmp(bound_drivers[index], DRIVER_LIGHTGUN) == 0 &&
        (dsu.enabled_data_types & XEMU_DSU_DATA_MOUSE)) {
        if (dsu.mouse.buttons & 0x0001) {
            out->buttons |= CONTROLLER_BUTTON_A;
        }
        if (dsu.mouse.buttons & 0x0002) {
            out->buttons |= CONTROLLER_BUTTON_B;
        }
        if (dsu.mouse.buttons & 0x0004) {
            out->buttons |= CONTROLLER_BUTTON_START;
        }
    }

    return true;
}

void xemu_input_set_rumble(unsigned index, uint16_t left, uint16_t right)
{
    XemuDSUPlayerState dsu;
    ControllerState *local;
    bool configured;

    if (index >= XEMU_DSU_MAX_PLAYERS) {
        return;
    }
    configured = xemu_dsu_get_player_state(index, &dsu) &&
                 (dsu.enabled_data_types & XEMU_DSU_DATA_GAMEPAD);
    local = bound_controllers[index];

    if (local != NULL && local->type != INPUT_DEVICE_DSU_GAMEPAD &&
        (!configured || dsu.addon)) {
        local->rumble_l = left;
        local->rumble_r = right;
        xemu_input_update_rumble(local);
    }
    if (configured) {
        xemu_dsu_send_rumble(index, 0, left >> 8);
        xemu_dsu_send_rumble(index, 1, right >> 8);
    }
}

ControllerState *xemu_input_get_bound(int index)
{
    return bound_controllers[index];
}

void xemu_input_bind(int index, ControllerState *state, int save)
{
    // FIXME: Attempt to disable rumble when unbinding so it's not left
    // in rumble mode

    // Unbind existing controller
    if (bound_controllers[index]) {
        assert(bound_controllers[index]->device != NULL);
        Error *err = NULL;

        // Unbind any XMUs
        for (int i = 0; i < 2; i++) {
            if (bound_controllers[index]->peripherals[i]) {
                // If this was an XMU, unbind the XMU
                if (bound_controllers[index]->peripheral_types[i] ==
                    PERIPHERAL_XMU)
                    xemu_input_unbind_xmu(index, i);

                // Free up the XmuState and set the peripheral type to none
                g_free(bound_controllers[index]->peripherals[i]);
                bound_controllers[index]->peripherals[i] = NULL;
                bound_controllers[index]->peripheral_types[i] = PERIPHERAL_NONE;
            }
        }

        qdev_unplug((DeviceState *)bound_controllers[index]->device, &err);
        assert(err == NULL);

        bound_controllers[index]->bound = -1;
        bound_controllers[index]->device = NULL;
        bound_controllers[index] = NULL;
    }

    // Save this controller's GUID in settings for auto re-connect
    if (save) {
        char guid_buf[35] = { 0 };
        if (state) {
            if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
                SDL_GUIDToString(state->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
            } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
                snprintf(guid_buf, sizeof(guid_buf), "keyboard");
#ifdef CONFIG_UWP
            } else if (state->type == INPUT_DEVICE_UWP_GAMEPAD) {
                snprintf(guid_buf, sizeof(guid_buf), "uwp-gamepad-%d",
                         state->uwp_gamepad_index);
#endif
            }
        }
        if (state == NULL || state->type != INPUT_DEVICE_DSU_GAMEPAD) {
            xemu_settings_set_string(port_index_to_settings_key_map[index],
                                     guid_buf);
        }
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index],
                                 bound_drivers[index]);
    }

    // Bind new controller
    if (state) {
        if (state->bound >= 0) {
            // Device was already bound to another port. Unbind it.
            xemu_input_bind(state->bound, NULL, 1);
        }

        bound_controllers[index] = state;
        bound_controllers[index]->bound = index;

        char *tmp;

        // Create controller's internal USB hub.
        QDict *usbhub_qdict = qdict_new();
        qdict_put_str(usbhub_qdict, "driver", "usb-hub");
        tmp = g_strdup_printf("1.%d", port_map[index]);
        qdict_put_str(usbhub_qdict, "port", tmp);
        qdict_put_int(usbhub_qdict, "ports", 3);
        QemuOpts *usbhub_opts = qemu_opts_from_qdict(qemu_find_opts("device"), usbhub_qdict, &error_abort);
        DeviceState *usbhub_dev = qdev_device_add(usbhub_opts, &error_abort);
        g_free(tmp);

        // Create XID controller. This is connected to Port 1 of the controller's internal USB Hub
        QDict *qdict = qdict_new();

        // Specify device driver
        qdict_put_str(qdict, "driver", bound_drivers[index]);

        // Specify device identifier
        static int id_counter = 0;
        tmp = g_strdup_printf("gamepad_%d", id_counter++);
        qdict_put_str(qdict, "id", tmp);
        g_free(tmp);

        // Specify index/port
        qdict_put_int(qdict, "index", index);
        tmp = g_strdup_printf("1.%d.1", port_map[index]);
        qdict_put_str(qdict, "port", tmp);
        g_free(tmp);

        // Create the device
        QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &error_abort);
        DeviceState *dev = qdev_device_add(opts, &error_abort);
        assert(dev);

        // Unref for eventual cleanup
        qobject_unref(usbhub_qdict);
        object_unref(OBJECT(usbhub_dev));
        qobject_unref(qdict);
        object_unref(OBJECT(dev));

        state->device = usbhub_dev;
    }
}

bool xemu_input_bind_xmu(int player_index, int expansion_slot_index,
                         const char *filename, bool is_rebind)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *player = bound_controllers[player_index];
    enum peripheral_type peripheral_type =
        player->peripheral_types[expansion_slot_index];
    if (peripheral_type != PERIPHERAL_XMU)
        return false;

    XmuState *xmu = (XmuState *)player->peripherals[expansion_slot_index];

    // Unbind existing XMU
    if (xmu->dev != NULL) {
        xemu_input_unbind_xmu(player_index, expansion_slot_index);
    }

    if (filename == NULL)
        return false;

    // Look for any other XMUs that are using this file, and unbind them
    for (int player_i = 0; player_i < 4; player_i++) {
        ControllerState *state = bound_controllers[player_i];
        if (state != NULL) {
            for (int peripheral_i = 0; peripheral_i < 2; peripheral_i++) {
                if (state->peripheral_types[peripheral_i] == PERIPHERAL_XMU) {
                    XmuState *xmu_i =
                        (XmuState *)state->peripherals[peripheral_i];
                    assert(xmu_i);

                    if (xmu_i->filename != NULL &&
                        strcmp(xmu_i->filename, filename) == 0) {
                        char *buf =
                            g_strdup_printf("This XMU is already mounted on "
                                            "player %d slot %c\r\n",
                                            player_i + 1, 'A' + peripheral_i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                        return false;
                    }
                }
            }
        }
    }

    xmu->filename = g_strdup(filename);

    const int xmu_map[2] = { 2, 3 };
    char *tmp;

    static int id_counter = 0;
    tmp = g_strdup_printf("xmu_%d", id_counter++);

    // Add the file as a drive
    QDict *qdict1 = qdict_new();
    qdict_put_str(qdict1, "id", tmp);
    qdict_put_str(qdict1, "format", "raw");
    qdict_put_str(qdict1, "file", filename);

    QemuOpts *drvopts =
        qemu_opts_from_qdict(qemu_find_opts("drive"), qdict1, &error_abort);

    DriveInfo *dinfo = drive_new(drvopts, 0, &error_abort);
    assert(dinfo);

    // Create the usb-storage device
    QDict *qdict2 = qdict_new();

    // Specify device driver
    qdict_put_str(qdict2, "driver", "usb-storage");

    // Specify device identifier
    qdict_put_str(qdict2, "drive", tmp);
    g_free(tmp);

    // Specify index/port
    tmp = g_strdup_printf("1.%d.%d", port_map[player_index],
                          xmu_map[expansion_slot_index]);
    qdict_put_str(qdict2, "port", tmp);
    g_free(tmp);

    // Create the device
    QemuOpts *opts =
        qemu_opts_from_qdict(qemu_find_opts("device"), qdict2, &error_abort);

    DeviceState *dev = qdev_device_add(opts, &error_abort);
    assert(dev);

    xmu->dev = (void *)dev;

    // Unref for eventual cleanup
    qobject_unref(qdict1);
    qobject_unref(qdict2);

    if (!is_rebind) {
        xemu_save_peripheral_settings(player_index, expansion_slot_index,
                                      peripheral_type, xmu->filename);
    }

    return true;
}

void xemu_input_unbind_xmu(int player_index, int expansion_slot_index)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *state = bound_controllers[player_index];
    if (state->peripheral_types[expansion_slot_index] != PERIPHERAL_XMU)
        return;

    XmuState *xmu = (XmuState *)state->peripherals[expansion_slot_index];
    if (xmu != NULL) {
        if (xmu->dev != NULL) {
            qdev_unplug((DeviceState *)xmu->dev, &error_abort);
            object_unref(OBJECT(xmu->dev));
            xmu->dev = NULL;
        }

        g_free((void *)xmu->filename);
        xmu->filename = NULL;
    }
}

void xemu_input_rebind_xmu(int port)
{
    // Try to bind peripherals back to controller
    for (int i = 0; i < 2; i++) {
        enum peripheral_type peripheral_type =
            (enum peripheral_type)(*peripheral_types_settings_map[port][i]);

        // If peripheralType is out of range, change the settings for this
        // controller and peripheral port to default
        if (peripheral_type < PERIPHERAL_NONE ||
            peripheral_type >= PERIPHERAL_TYPE_COUNT) {
            xemu_save_peripheral_settings(port, i, PERIPHERAL_NONE, NULL);
            peripheral_type = PERIPHERAL_NONE;
        }

        const char *param = *peripheral_params_settings_map[port][i];

        if (peripheral_type == PERIPHERAL_XMU) {
            if (param != NULL && strlen(param) > 0) {
                // This is an XMU and needs to be bound to this controller
                if (qemu_access(param, R_OK | W_OK) == 0) {
                    bound_controllers[port]->peripheral_types[i] =
                        peripheral_type;
                    bound_controllers[port]->peripherals[i] =
                        g_malloc(sizeof(XmuState));
                    memset(bound_controllers[port]->peripherals[i], 0,
                           sizeof(XmuState));
                    bool did_bind = xemu_input_bind_xmu(port, i, param, true);
                    if (did_bind) {
                        char *buf =
                            g_strdup_printf("Connected XMU %s to port %d%c",
                                            param, port + 1, 'A' + i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                    }
                } else {
                    char *buf =
                        g_strdup_printf("Unable to bind XMU at %s to port %d%c",
                                        param, port + 1, 'A' + i);
                    xemu_queue_error_message(buf);
                    g_free(buf);
                }
            }
        }
    }
}

void xemu_input_set_test_mode(int enabled)
{
    test_mode = enabled;
}

int xemu_input_get_test_mode(void)
{
    return test_mode;
}

void xemu_input_reset_input_mapping(ControllerState *state)
{
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        char guid[35] = { 0 };
        xemu_input_get_gamepad_mapping_id(state, guid, sizeof(guid));
        xemu_settings_reset_controller_mapping(guid);
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_settings_reset_keyboard_mapping();
#ifdef CONFIG_UWP
    } else if (state->type == INPUT_DEVICE_UWP_GAMEPAD) {
        char guid[35] = { 0 };
        xemu_input_get_gamepad_mapping_id(state, guid, sizeof(guid));
        xemu_settings_reset_controller_mapping(guid);
#endif
    }

    xemu_input_bindings_reload_map(state);
}
