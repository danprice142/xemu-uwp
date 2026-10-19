/*
 * xemu DSU (UDP controller protocol) client
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef XEMU_DSU_H
#define XEMU_DSU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_DSU_MAX_PLAYERS 4
#define XEMU_DSU_KEYBOARD_WORDS 11
#define XEMU_DSU_STATE_TEXT_LENGTH 64
#define XEMU_DSU_STREAM_URL_LENGTH 256

enum {
    XEMU_DSU_DATA_GAMEPAD = 1 << 0,
    XEMU_DSU_DATA_MOTION = 1 << 1,
    XEMU_DSU_DATA_TOUCH = 1 << 2,
    XEMU_DSU_DATA_KEYBOARD = 1 << 3,
    XEMU_DSU_DATA_MOUSE = 1 << 4,
};

enum {
    XEMU_DSU_STREAM_STOPPED = 0,
    XEMU_DSU_STREAM_ACTIVE = 1,
    XEMU_DSU_STREAM_ERROR = 2,
};

typedef struct XemuDSUTouchPoint {
    bool active;
    uint8_t id;
    int16_t x;
    int16_t y;
} XemuDSUTouchPoint;

typedef struct XemuDSUControllerState {
    bool connected;
    uint8_t slot;
    uint8_t model;
    uint8_t connection_type;
    uint8_t mac[6];
    uint8_t battery;
    uint32_t packet_number;

    /* Native DSU button mask. Consumers can map only the buttons they use. */
    uint16_t buttons;
    uint8_t home;
    uint8_t touch_button;
    uint8_t axes_raw[4];
    int16_t axes[4];
    uint8_t analog_buttons[12];
    uint8_t left_trigger;
    uint8_t right_trigger;

    XemuDSUTouchPoint touch[2];
    uint64_t motion_timestamp;
    float acceleration[3];
    float gyroscope[3];
    int64_t last_received_us;
} XemuDSUControllerState;

typedef struct XemuDSUKeyboardState {
    uint8_t slot;
    uint16_t modifiers;
    uint32_t character;
    uint32_t key_bitmap[XEMU_DSU_KEYBOARD_WORDS];
    uint64_t sequence;
    int64_t last_received_us;
} XemuDSUKeyboardState;

typedef struct XemuDSUMouseState {
    uint8_t slot;
    int32_t delta_x;
    int32_t delta_y;
    int32_t wheel_x;
    int32_t wheel_y;
    int16_t x;
    int16_t y;
    uint16_t buttons;
    uint64_t sequence;
    int64_t last_received_us;
} XemuDSUMouseState;

typedef struct XemuDSUPlayerState {
    bool configured;
    bool addon;
    uint8_t remote_slot;
    uint8_t enabled_data_types;
    XemuDSUControllerState controller;
    XemuDSUKeyboardState keyboard;
    XemuDSUMouseState mouse;
} XemuDSUPlayerState;

typedef enum XemuDSUStreamControlAction {
    XEMU_DSU_STREAM_CONTROL_START,
    XEMU_DSU_STREAM_CONTROL_STOP,
} XemuDSUStreamControlAction;

typedef struct XemuDSUStreamControl {
    XemuDSUStreamControlAction action;
    unsigned player;
    uint32_t stream_type;
    char url[XEMU_DSU_STREAM_URL_LENGTH + 1];
    uint32_t bitrate_kbps;
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    /* Echo of the token in xemu's stream request, 0 when unsolicited. */
    uint32_t authorization_token;
    uint64_t sequence;
} XemuDSUStreamControl;

typedef struct XemuDSUStreamStatus {
    uint32_t state;
    uint32_t error;
    uint32_t stream_type;
    char url[XEMU_DSU_STREAM_URL_LENGTH + 1];
    uint16_t width;
    uint16_t height;
    float aspect_ratio;
} XemuDSUStreamStatus;

typedef struct XemuDSUFrontendState {
    uint8_t flags;
    char game_name[XEMU_DSU_STATE_TEXT_LENGTH + 1];
    char platform_name[XEMU_DSU_STATE_TEXT_LENGTH + 1];
    char core_name[XEMU_DSU_STATE_TEXT_LENGTH + 1];
} XemuDSUFrontendState;

void xemu_dsu_init(void);
void xemu_dsu_shutdown(void);

/* Drain the nonblocking DSU socket and refresh subscriptions. This must be
 * called by the UI thread without the BQL so packet bursts cannot stall guest
 * execution. It only touches cached configuration and synchronized snapshots. */
void xemu_dsu_poll_network(void);

/* Refresh configuration and apply queued frontend/stream requests. The caller
 * must hold the BQL because this may mount a disc, reset/start the guest, or
 * inspect frontend state. */
void xemu_dsu_process_pending(void);

/* Lock-free hot-path check. True only while at least one enabled DSU endpoint
 * has a valid address and port. */
bool xemu_dsu_is_active(void);

bool xemu_dsu_get_player_state(unsigned player, XemuDSUPlayerState *out);
bool xemu_dsu_player_is_addon(unsigned player);

/* Returns accumulated relative motion once and clears only the delta/wheel
 * fields. Absolute position, buttons, and the stored snapshot are preserved. */
bool xemu_dsu_consume_mouse(unsigned player, XemuDSUMouseState *out);

bool xemu_dsu_send_rumble(unsigned player, uint8_t motor,
                          uint8_t intensity);

/* Main-stream START/STOP requests are forwarded to xemu-stream when Remote
 * Start Broadcast is enabled and retained for consumers that need to observe
 * those requests. */
bool xemu_dsu_take_stream_control(XemuDSUStreamControl *out);
void xemu_dsu_report_stream_status(const XemuDSUStreamStatus *status);

/* Send and cache the current frontend/game state. The cached state is sent to
 * endpoints again when their DSU subscription is refreshed. */
void xemu_dsu_send_frontend_state(const XemuDSUFrontendState *state);

/* Library entry a network frontend asked to highlight, by launch id. Taken
 * once, and only while the request is recent. */
bool xemu_dsu_take_app_selection(uint64_t *launch_id);

/* Report the highlighted library entry to network frontends. */
void xemu_dsu_send_app_selection(uint32_t revision, uint64_t launch_id);

/* Whether a DSU server (network frontend) is currently answering. */
bool xemu_dsu_frontend_connected(void);

/* Ask connected frontends to start or stop receiving the main stream; they
 * answer with their own stream request. False when none is connected. */
bool xemu_dsu_request_stream(bool start);

/* Whether the running stream was started by a frontend. */
bool xemu_dsu_frontend_stream_active(void);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_DSU_H */
