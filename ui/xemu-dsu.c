/*
 * xemu DSU (UDP controller protocol) client
 *
 * This client uses one UDP socket for every configured player. DSU servers
 * reply to the source address of the most recent client packet, so
 * subscriptions and replies must originate from the same socket. Player
 * independence is provided by four separately configurable server endpoints
 * and remote slots.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qemu/sockets.h"
#include "system/runstate.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "ui/xemu-dsu.h"
#include "ui/xemu-game-library.h"
#include "ui/xemu-library-server.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-stream.h"
#include "ui/xui/xemu-hud.h"

#include <limits.h>

#define DSU_PROTOCOL_VERSION 1001
#define DSU_HEADER_SIZE 20
#define DSU_MAX_PACKET_SIZE 2048
#define DSU_SUBSCRIPTION_INTERVAL_US (2500 * 1000LL)
#define DSU_CONTROLLER_TIMEOUT_US (5000 * 1000LL)
#define DSU_CONTROL_QUEUE_LENGTH 8
#define DSU_DIRECT_QUEUE_LENGTH 16
#define DSU_DIRECT_PROCESS_BUDGET 8
#define DSU_POLL_PACKET_BUDGET 16
#define DSU_POLL_TIME_BUDGET_US 250
#define DSU_POLL_INTERVAL_US 4000
#define DSU_FRONTEND_REFRESH_INTERVAL_US (250 * 1000)
#define DSU_RUMBLE_MOTOR_COUNT 2

#define DSU_MSG_VERSION 0x100000
#define DSU_MSG_CONTROLLER 0x100001
#define DSU_MSG_DATA 0x100002
#define DSU_MSG_MOTOR_INFO 0x110001
#define DSU_MSG_RUMBLE 0x110002
#define DSU_MSG_KEYBOARD 0x120000
#define DSU_MSG_MOUSE 0x120001
#define DSU_MSG_COMMAND 0x130000
#define DSU_MSG_STATE 0x140000
#define DSU_MSG_STREAM_START 0x150000
#define DSU_MSG_STREAM_STOP 0x150001
#define DSU_MSG_STREAM_STATUS 0x150002
#define DSU_MSG_STREAM_CONTROL_REQUEST 0x150003
#define DSU_MSG_PLAYLIST_LIST_REQUEST 0x160000
#define DSU_MSG_PLAYLIST_CONTENTS_REQUEST 0x160001
#define DSU_MSG_PLAYLIST_LIST 0x160002
#define DSU_MSG_PLAYLIST_CONTENTS 0x160003
#define DSU_MSG_APP_LAUNCH 0x180004
#define DSU_MSG_APP_LAUNCH_STATUS 0x180005
#define DSU_MSG_APP_SELECTION 0x180006

#define DSU_PLAYLIST_PAGE_SIZE 3
#define DSU_XBOX_PLAYLIST "Microsoft - Xbox.lpl"

/* Frontend state: flags, three names, the application name and its
 * capabilities. */
#define DSU_STATE_PAYLOAD_SIZE 261
#define DSU_STATE_APP_NAME "xemu"
#define DSU_STATE_CAP_STATE (1u << 0)
#define DSU_STATE_CAP_MAIN_STREAM (1u << 4)
#define DSU_STATE_CAP_LAUNCH_BY_ID (1u << 9)
#define DSU_STATE_CAP_CATALOGUE (1u << 13)
#define DSU_STATE_CAP_CATALOGUE_MEDIA (1u << 14)
#define DSU_STATE_CAP_APP_SELECTION (1u << 15)

#define DSU_APP_LAUNCH_ACCEPTED 0
#define DSU_APP_LAUNCH_UNKNOWN_ENTRY 2
#define DSU_APP_LAUNCH_STATUS_SIZE 144
/* A remote selection is only applied while it is still current. */
#define DSU_APP_SELECTION_TTL_US (2 * 1000 * 1000LL)
#define DSU_LIBRARY_SCAN_ATTEMPTS 30

typedef enum DSUDirectKind {
    DSU_DIRECT_NONE,
    DSU_DIRECT_STREAM,
    DSU_DIRECT_PLAYLIST_LIST,
    DSU_DIRECT_PLAYLIST_CONTENTS,
    DSU_DIRECT_COMMAND,
    DSU_DIRECT_APP_LAUNCH,
} DSUDirectKind;

typedef struct DSUDirectRequest {
    DSUDirectKind kind;
    unsigned player;
    uint8_t page;
    uint32_t command;
    uint32_t revision;
    uint64_t launch_id;
    char path[257];
    char core[257];
    XemuDSUStreamControl stream;
} DSUDirectRequest;

typedef struct DSUPlayerConfig {
    bool enabled;
    bool addon;
    char address[128];
    int port;
    uint8_t slot;
    uint8_t data_types;
} DSUPlayerConfig;

typedef struct DSUEndpoint {
    DSUPlayerConfig config;
    bool address_valid;
    struct sockaddr_in remote;
    int64_t next_subscription_us;
    bool server_id_valid;
    uint32_t server_id;
    int64_t last_received_us;
} DSUEndpoint;

static GMutex s_lock;
static bool s_initialized;
static int s_fd = -1;
static gint s_config_enabled;
static gint s_any_configured;
static int64_t s_next_poll_us;
static uint32_t s_client_id;
static DSUEndpoint s_endpoints[XEMU_DSU_MAX_PLAYERS];
static XemuDSUPlayerState s_players[XEMU_DSU_MAX_PLAYERS];
static uint8_t s_rumble_value[XEMU_DSU_MAX_PLAYERS][DSU_RUMBLE_MOTOR_COUNT];
static uint8_t s_rumble_valid[XEMU_DSU_MAX_PLAYERS];

static XemuDSUStreamControl s_control_queue[DSU_CONTROL_QUEUE_LENGTH];
static unsigned s_control_head;
static unsigned s_control_count;
static uint64_t s_control_sequence;

static DSUDirectRequest s_direct_queue[DSU_DIRECT_QUEUE_LENGTH];
static unsigned s_direct_head;
static unsigned s_direct_count;

static bool s_frontend_state_valid;
static XemuDSUFrontendState s_frontend_state;
static bool s_stream_status_valid;
static XemuDSUStreamStatus s_stream_status;
static int s_stream_observer = -1;
static bool s_stream_observer_active;
static bool s_stream_observer_starting;
static int64_t s_stream_observer_deadline_us;

static bool s_app_selection_pending;
static uint64_t s_app_selection_id;
static int64_t s_app_selection_us;
static bool s_library_server_started;

static uint32_t s_stream_request_token;
/* A frontend started the current stream; it ends if the frontend leaves. */
static bool s_frontend_stream;

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t get_les16(const uint8_t *p)
{
    return (int16_t)get_le16(p);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static float get_le_float(const uint8_t *p)
{
    uint32_t bits = get_le32(p);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void put_le64(uint8_t *p, uint64_t value)
{
    put_le32(p, (uint32_t)value);
    put_le32(p + 4, (uint32_t)(value >> 32));
}

static void put_le_float(uint8_t *p, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    put_le32(p, bits);
}

static uint32_t dsu_crc32(const uint8_t *packet, size_t size)
{
    uint32_t crc = UINT32_MAX;
    size_t i;

    for (i = 0; i < size; i++) {
        uint8_t byte = (i >= 8 && i < 12) ? 0 : packet[i];
        int bit;

        crc ^= byte;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^
                  ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return ~crc;
}

static void copy_fixed_string(char *dst, size_t dst_size,
                              const uint8_t *src, size_t src_size)
{
    size_t length = 0;

    while (length < src_size && src[length] != 0) {
        length++;
    }
    length = MIN(length, dst_size - 1);
    memcpy(dst, src, length);
    dst[length] = 0;
}

static void put_fixed_string(uint8_t *dst, size_t dst_size, const char *src)
{
    memset(dst, 0, dst_size);
    if (src != NULL) {
        memcpy(dst, src, MIN(strlen(src), dst_size - 1));
    }
}

static int32_t add_clamped_i32(int32_t value, int16_t delta)
{
    int64_t result = (int64_t)value + delta;

    return (int32_t)CLAMP(result, INT32_MIN, INT32_MAX);
}

static DSUPlayerConfig read_player_config(unsigned player)
{
    DSUPlayerConfig out = { 0 };
    const char *address = NULL;
    int slot = 0;
    bool gamepad = false;
    bool motion = false;
    bool touch = false;
    bool keyboard = false;
    bool mouse = false;

    switch (player) {
    case 0:
        out.enabled = g_config.input.dsu.player1.enabled;
        out.addon = g_config.input.dsu.player1.addon;
        address = g_config.input.dsu.player1.server_address;
        out.port = g_config.input.dsu.player1.server_port;
        slot = g_config.input.dsu.player1.slot;
        gamepad = g_config.input.dsu.player1.gamepad;
        motion = g_config.input.dsu.player1.motion;
        touch = g_config.input.dsu.player1.touch;
        keyboard = g_config.input.dsu.player1.keyboard;
        mouse = g_config.input.dsu.player1.mouse;
        break;
    case 1:
        out.enabled = g_config.input.dsu.player2.enabled;
        out.addon = g_config.input.dsu.player2.addon;
        address = g_config.input.dsu.player2.server_address;
        out.port = g_config.input.dsu.player2.server_port;
        slot = g_config.input.dsu.player2.slot;
        gamepad = g_config.input.dsu.player2.gamepad;
        motion = g_config.input.dsu.player2.motion;
        touch = g_config.input.dsu.player2.touch;
        keyboard = g_config.input.dsu.player2.keyboard;
        mouse = g_config.input.dsu.player2.mouse;
        break;
    case 2:
        out.enabled = g_config.input.dsu.player3.enabled;
        out.addon = g_config.input.dsu.player3.addon;
        address = g_config.input.dsu.player3.server_address;
        out.port = g_config.input.dsu.player3.server_port;
        slot = g_config.input.dsu.player3.slot;
        gamepad = g_config.input.dsu.player3.gamepad;
        motion = g_config.input.dsu.player3.motion;
        touch = g_config.input.dsu.player3.touch;
        keyboard = g_config.input.dsu.player3.keyboard;
        mouse = g_config.input.dsu.player3.mouse;
        break;
    case 3:
        out.enabled = g_config.input.dsu.player4.enabled;
        out.addon = g_config.input.dsu.player4.addon;
        address = g_config.input.dsu.player4.server_address;
        out.port = g_config.input.dsu.player4.server_port;
        slot = g_config.input.dsu.player4.slot;
        gamepad = g_config.input.dsu.player4.gamepad;
        motion = g_config.input.dsu.player4.motion;
        touch = g_config.input.dsu.player4.touch;
        keyboard = g_config.input.dsu.player4.keyboard;
        mouse = g_config.input.dsu.player4.mouse;
        break;
    default:
        return out;
    }

    g_strlcpy(out.address, address != NULL ? address : "",
              sizeof(out.address));
    out.slot = CLAMP(slot, 0, 3);
    if (gamepad) {
        out.data_types |= XEMU_DSU_DATA_GAMEPAD;
    }
    if (motion) {
        out.data_types |= XEMU_DSU_DATA_MOTION;
    }
    if (touch) {
        out.data_types |= XEMU_DSU_DATA_TOUCH;
    }
    if (keyboard) {
        out.data_types |= XEMU_DSU_DATA_KEYBOARD;
    }
    if (mouse) {
        out.data_types |= XEMU_DSU_DATA_MOUSE;
    }
    return out;
}

static bool any_player_enabled_config(void)
{
    /* Keep the disabled path to four scalar reads. In particular, do not call
     * read_player_config(): it copies/compares endpoint strings and is reached
     * from the input update hot path. */
    return g_config.input.dsu.player1.enabled ||
           g_config.input.dsu.player2.enabled ||
           g_config.input.dsu.player3.enabled ||
           g_config.input.dsu.player4.enabled;
}

static bool configs_equal(const DSUPlayerConfig *a, const DSUPlayerConfig *b)
{
    return a->enabled == b->enabled && a->addon == b->addon &&
           a->port == b->port && a->slot == b->slot &&
           a->data_types == b->data_types &&
           strcmp(a->address, b->address) == 0;
}

static void invalidate_rumble_locked(unsigned player)
{
    if (player < XEMU_DSU_MAX_PLAYERS) {
        s_rumble_valid[player] = 0;
    }
}

static bool resolve_endpoint(DSUEndpoint *endpoint)
{
    struct in_addr address;

    memset(&endpoint->remote, 0, sizeof(endpoint->remote));
    if (!endpoint->config.enabled || endpoint->config.address[0] == 0 ||
        endpoint->config.port < 1 || endpoint->config.port > 65535 ||
        !inet_aton(endpoint->config.address, &address)) {
        return false;
    }
    endpoint->remote.sin_family = AF_INET;
    endpoint->remote.sin_addr = address;
    endpoint->remote.sin_port = htons((uint16_t)endpoint->config.port);
    return true;
}

static void close_socket_locked(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

static bool ensure_socket_locked(void)
{
    if (s_fd >= 0) {
        return true;
    }
    s_fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
    if (s_fd < 0) {
        return false;
    }
    if (!qemu_set_blocking(s_fd, false, NULL)) {
        close_socket_locked();
        return false;
    }
    return true;
}

static bool send_packet_locked(unsigned player, uint32_t message_type,
                               const void *payload, size_t payload_size)
{
    uint8_t packet[DSU_MAX_PACKET_SIZE];
    DSUEndpoint *endpoint;
    size_t packet_size;
    ssize_t sent;

    if (player >= XEMU_DSU_MAX_PLAYERS || payload_size > UINT16_MAX - 4 ||
        payload_size + DSU_HEADER_SIZE > sizeof(packet) || s_fd < 0) {
        return false;
    }
    endpoint = &s_endpoints[player];
    if (!endpoint->address_valid) {
        return false;
    }

    packet_size = DSU_HEADER_SIZE + payload_size;
    memset(packet, 0, DSU_HEADER_SIZE);
    memcpy(packet, "DSUC", 4);
    put_le16(packet + 4, DSU_PROTOCOL_VERSION);
    put_le16(packet + 6, (uint16_t)(payload_size + 4));
    put_le32(packet + 12, s_client_id);
    put_le32(packet + 16, message_type);
    if (payload_size != 0) {
        memcpy(packet + DSU_HEADER_SIZE, payload, payload_size);
    }
    put_le32(packet + 8, dsu_crc32(packet, packet_size));

    sent = sendto(s_fd, packet, packet_size, 0,
                  (struct sockaddr *)&endpoint->remote,
                  sizeof(endpoint->remote));
    return sent == (ssize_t)packet_size;
}

static void build_stream_status_payload(uint8_t payload[280],
                                        const XemuDSUStreamStatus *status)
{
    memset(payload, 0, 280);
    put_le32(payload + 0, status->state);
    put_le32(payload + 4, status->error);
    put_le32(payload + 8, status->stream_type);
    payload[12] = 0;
    put_fixed_string(payload + 16, XEMU_DSU_STREAM_URL_LENGTH, status->url);
    put_le16(payload + 272, status->width);
    put_le16(payload + 274, status->height);
    put_le_float(payload + 276, status->aspect_ratio);
}

static void send_stream_status_to_locked(unsigned player,
                                         const XemuDSUStreamStatus *status)
{
    uint8_t payload[280];

    build_stream_status_payload(payload, status);
    send_packet_locked(player, DSU_MSG_STREAM_STATUS, payload,
                       sizeof(payload));
}

static uint32_t frontend_capabilities(void)
{
    uint32_t capabilities = DSU_STATE_CAP_STATE | DSU_STATE_CAP_LAUNCH_BY_ID |
                            DSU_STATE_CAP_APP_SELECTION;

    if (g_config.display.stream.remote_start) {
        capabilities |= DSU_STATE_CAP_MAIN_STREAM;
    }

    if (xemu_library_server_is_running()) {
        capabilities |= DSU_STATE_CAP_CATALOGUE | DSU_STATE_CAP_CATALOGUE_MEDIA;
    }
    return capabilities;
}

static void send_frontend_state_to_locked(
    unsigned player, const XemuDSUFrontendState *state)
{
    uint8_t payload[DSU_STATE_PAYLOAD_SIZE] = { 0 };

    payload[0] = state->flags;
    put_fixed_string(payload + 1, XEMU_DSU_STATE_TEXT_LENGTH,
                     state->game_name);
    put_fixed_string(payload + 65, XEMU_DSU_STATE_TEXT_LENGTH,
                     state->platform_name);
    put_fixed_string(payload + 129, XEMU_DSU_STATE_TEXT_LENGTH,
                     state->core_name);
    put_fixed_string(payload + 193, XEMU_DSU_STATE_TEXT_LENGTH,
                     DSU_STATE_APP_NAME);
    put_le32(payload + 257, frontend_capabilities());
    send_packet_locked(player, DSU_MSG_STATE, payload, sizeof(payload));
}

static void refresh_config_locked(void)
{
    unsigned player;
    bool any_enabled = false;
    bool any_configured = false;
    bool config_changed = false;

    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        DSUPlayerConfig config = read_player_config(player);
        DSUEndpoint *endpoint = &s_endpoints[player];

        any_enabled |= config.enabled;

        if (!configs_equal(&config, &endpoint->config)) {
            config_changed = true;
            memset(&s_players[player], 0, sizeof(s_players[player]));
            invalidate_rumble_locked(player);
            endpoint->config = config;
            endpoint->address_valid = resolve_endpoint(endpoint);
            endpoint->next_subscription_us = 0;
            endpoint->server_id_valid = false;
        }
        s_players[player].configured = endpoint->address_valid;
        s_players[player].addon = endpoint->config.addon;
        s_players[player].remote_slot = endpoint->config.slot;
        s_players[player].enabled_data_types = endpoint->config.data_types;
        any_configured |= endpoint->address_valid;
    }

    g_atomic_int_set(&s_config_enabled, any_enabled);
    g_atomic_int_set(&s_any_configured, any_configured);

    /* A queued command was authenticated against the endpoint snapshot that
     * received it. Do not carry it across an address/slot/enable transition. */
    if (config_changed) {
        s_direct_head = 0;
        s_direct_count = 0;
    }

    if (s_stream_observer >= 0 &&
        !s_endpoints[s_stream_observer].address_valid) {
        /* There is nowhere to send observer updates after its endpoint has
         * been disabled. The stream itself remains under xemu-stream control. */
        s_stream_observer = -1;
        s_stream_observer_starting = false;
    }
}

static void send_subscription_locked(unsigned player, int64_t now)
{
    DSUEndpoint *endpoint = &s_endpoints[player];
    uint8_t controller_request[5] = { 0 };
    uint8_t data_request[3];

    send_packet_locked(player, DSU_MSG_VERSION, NULL, 0);

    put_le32(controller_request, 1);
    controller_request[4] = endpoint->config.slot;
    send_packet_locked(player, DSU_MSG_CONTROLLER, controller_request,
                       sizeof(controller_request));

    data_request[0] = 1; /* subscribe by slot */
    data_request[1] = endpoint->config.data_types;
    data_request[2] = endpoint->config.slot;
    send_packet_locked(player, DSU_MSG_DATA, data_request,
                       sizeof(data_request));

    if (s_frontend_state_valid) {
        send_frontend_state_to_locked(player, &s_frontend_state);
    }
    if (s_stream_status_valid) {
        send_stream_status_to_locked(player, &s_stream_status);
    }
    /* This is a periodic lease refresh, not a new DSU session, so cached motor
     * state is kept: invalidating it here would make a guest that writes
     * unchanged actuator values retransmit them after every keepalive. Session
     * changes are detected from the sender id, controller reconnect/MAC, data
     * sequence restart, timeout, or endpoint configuration. */
    endpoint->next_subscription_us = now + DSU_SUBSCRIPTION_INTERVAL_US;
}

static bool source_matches(const DSUEndpoint *endpoint,
                           const struct sockaddr_in *source)
{
    return endpoint->address_valid &&
           endpoint->remote.sin_port == source->sin_port &&
           endpoint->remote.sin_addr.s_addr == source->sin_addr.s_addr;
}

static bool validate_packet(const uint8_t *packet, size_t packet_size,
                            uint32_t *server_id, uint32_t *message_type,
                            const uint8_t **payload, size_t *payload_size)
{
    uint16_t length;

    if (packet_size < DSU_HEADER_SIZE || memcmp(packet, "DSUS", 4) != 0 ||
        get_le16(packet + 4) != DSU_PROTOCOL_VERSION) {
        return false;
    }
    length = get_le16(packet + 6);
    if ((size_t)length + 16 != packet_size ||
        get_le32(packet + 8) != dsu_crc32(packet, packet_size)) {
        return false;
    }
    *server_id = get_le32(packet + 12);
    *message_type = get_le32(packet + 16);
    *payload = packet + DSU_HEADER_SIZE;
    *payload_size = packet_size - DSU_HEADER_SIZE;
    return true;
}

static void parse_controller_info_locked(const struct sockaddr_in *source,
                                         const uint8_t *payload,
                                         size_t payload_size)
{
    unsigned player;

    if (payload_size < 12) {
        return;
    }
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        XemuDSUControllerState *state = &s_players[player].controller;
        DSUEndpoint *endpoint = &s_endpoints[player];
        bool connected;

        if (!source_matches(endpoint, source) ||
            payload[0] != endpoint->config.slot) {
            continue;
        }
        connected = payload[1] == 2;
        if ((!state->connected && connected) ||
            memcmp(state->mac, payload + 4, sizeof(state->mac)) != 0) {
            invalidate_rumble_locked(player);
        }
        state->slot = payload[0];
        state->connected = connected;
        state->model = payload[2];
        state->connection_type = payload[3];
        memcpy(state->mac, payload + 4, sizeof(state->mac));
        state->battery = payload[10];
    }
}

static void parse_controller_data_locked(const struct sockaddr_in *source,
                                         const uint8_t *payload,
                                         size_t payload_size, int64_t now)
{
    unsigned player;

    if (payload_size < 80) {
        return;
    }
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        XemuDSUControllerState *state = &s_players[player].controller;
        DSUEndpoint *endpoint = &s_endpoints[player];
        unsigned i;
        bool connected;
        uint32_t packet_number;

        if (!source_matches(endpoint, source) ||
            payload[0] != endpoint->config.slot ||
            !(endpoint->config.data_types &
              (XEMU_DSU_DATA_GAMEPAD | XEMU_DSU_DATA_MOTION |
               XEMU_DSU_DATA_TOUCH))) {
            continue;
        }
        connected = payload[1] == 2 && payload[11] != 0;
        packet_number = get_le32(payload + 12);
        if ((!state->connected && connected) ||
            memcmp(state->mac, payload + 4, sizeof(state->mac)) != 0 ||
            (state->connected && connected && state->last_received_us != 0 &&
             (int32_t)(packet_number - state->packet_number) < 0)) {
            invalidate_rumble_locked(player);
        }
        state->slot = payload[0];
        state->connected = connected;
        state->model = payload[2];
        state->connection_type = payload[3];
        memcpy(state->mac, payload + 4, sizeof(state->mac));
        state->battery = payload[10];
        state->packet_number = packet_number;
        state->buttons = get_le16(payload + 16);
        state->home = payload[18];
        state->touch_button = payload[19];
        for (i = 0; i < 4; i++) {
            state->axes_raw[i] = payload[20 + i];
            state->axes[i] = ((int)payload[20 + i] - 128) * 256;
        }
        memcpy(state->analog_buttons, payload + 24,
               sizeof(state->analog_buttons));
        state->right_trigger = payload[34];
        state->left_trigger = payload[35];

        state->touch[0].active = payload[36] != 0;
        state->touch[0].id = payload[37];
        state->touch[0].x = get_les16(payload + 38);
        state->touch[0].y = get_les16(payload + 40);
        state->touch[1].active = payload[42] != 0;
        state->touch[1].id = payload[43];
        state->touch[1].x = get_les16(payload + 44);
        state->touch[1].y = get_les16(payload + 46);

        state->motion_timestamp = get_le64(payload + 48);
        for (i = 0; i < 3; i++) {
            state->acceleration[i] = get_le_float(payload + 56 + i * 4);
            state->gyroscope[i] = get_le_float(payload + 68 + i * 4);
        }
        state->last_received_us = now;
    }
}

static void parse_keyboard_locked(const struct sockaddr_in *source,
                                  const uint8_t *payload,
                                  size_t payload_size, int64_t now)
{
    unsigned player;

    if (payload_size < 51) {
        return;
    }
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        XemuDSUKeyboardState *state = &s_players[player].keyboard;
        DSUEndpoint *endpoint = &s_endpoints[player];
        unsigned i;

        if (!source_matches(endpoint, source) ||
            payload[0] != endpoint->config.slot ||
            !(endpoint->config.data_types & XEMU_DSU_DATA_KEYBOARD)) {
            continue;
        }
        state->slot = payload[0];
        state->modifiers = get_le16(payload + 1);
        state->character = get_le32(payload + 3);
        for (i = 0; i < XEMU_DSU_KEYBOARD_WORDS; i++) {
            state->key_bitmap[i] = get_le32(payload + 7 + i * 4);
        }
        state->sequence++;
        state->last_received_us = now;
    }
}

static void parse_mouse_locked(const struct sockaddr_in *source,
                               const uint8_t *payload, size_t payload_size,
                               int64_t now)
{
    unsigned player;

    if (payload_size < 15) {
        return;
    }
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        XemuDSUMouseState *state = &s_players[player].mouse;
        DSUEndpoint *endpoint = &s_endpoints[player];

        if (!source_matches(endpoint, source) ||
            payload[0] != endpoint->config.slot ||
            !(endpoint->config.data_types & XEMU_DSU_DATA_MOUSE)) {
            continue;
        }
        state->slot = payload[0];
        state->delta_x = add_clamped_i32(state->delta_x,
                                         get_les16(payload + 1));
        state->delta_y = add_clamped_i32(state->delta_y,
                                         get_les16(payload + 3));
        state->wheel_x = add_clamped_i32(state->wheel_x,
                                         get_les16(payload + 5));
        state->wheel_y = add_clamped_i32(state->wheel_y,
                                         get_les16(payload + 7));
        state->x = get_les16(payload + 9);
        state->y = get_les16(payload + 11);
        state->buttons = get_le16(payload + 13);
        state->sequence++;
        state->last_received_us = now;
    }
}

static void enqueue_stream_control_locked(XemuDSUStreamControl *control)
{
    unsigned tail;

    control->sequence = ++s_control_sequence;
    if (s_control_count == DSU_CONTROL_QUEUE_LENGTH) {
        s_control_head = (s_control_head + 1) % DSU_CONTROL_QUEUE_LENGTH;
        s_control_count--;
    }
    tail = (s_control_head + s_control_count) % DSU_CONTROL_QUEUE_LENGTH;
    s_control_queue[tail] = *control;
    s_control_count++;
}

static void enqueue_direct_request_locked(const DSUDirectRequest *request)
{
    unsigned tail;

    if (s_direct_count == DSU_DIRECT_QUEUE_LENGTH) {
        /* Keep the newest commands. A stale launch/stream request is less
         * useful than the most recently received frontend state. */
        s_direct_head = (s_direct_head + 1) % DSU_DIRECT_QUEUE_LENGTH;
        s_direct_count--;
    }
    tail = (s_direct_head + s_direct_count) % DSU_DIRECT_QUEUE_LENGTH;
    s_direct_queue[tail] = *request;
    s_direct_count++;
}

static bool dequeue_direct_request_locked(DSUDirectRequest *request)
{
    if (s_direct_count == 0) {
        return false;
    }
    *request = s_direct_queue[s_direct_head];
    s_direct_head = (s_direct_head + 1) % DSU_DIRECT_QUEUE_LENGTH;
    s_direct_count--;
    return true;
}

static int first_source_player_locked(const struct sockaddr_in *source)
{
    unsigned player;

    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        if (source_matches(&s_endpoints[player], source)) {
            return player;
        }
    }
    return -1;
}

static bool parse_stream_control_locked(const struct sockaddr_in *source,
                                        uint32_t message_type,
                                        const uint8_t *payload,
                                        size_t payload_size,
                                        XemuDSUStreamControl *direct)
{
    XemuDSUStreamControl control = { 0 };
    int player = first_source_player_locked(source);

    if (player < 0) {
        return false;
    }
    control.player = player;

    switch (message_type) {
    case DSU_MSG_STREAM_START:
        if (payload_size < 269) {
            return false;
        }
        control.action = XEMU_DSU_STREAM_CONTROL_START;
        control.stream_type = get_le32(payload);
        copy_fixed_string(control.url, sizeof(control.url), payload + 4,
                          XEMU_DSU_STREAM_URL_LENGTH);
        control.bitrate_kbps = get_le32(payload + 260);
        control.width = get_le16(payload + 264);
        control.height = get_le16(payload + 266);
        control.fps = payload[268];
        if (payload_size >= 274) {
            control.authorization_token = get_le32(payload + 270);
        }
        break;
    case DSU_MSG_STREAM_STOP:
        control.action = XEMU_DSU_STREAM_CONTROL_STOP;
        break;
    default:
        return false;
    }

    enqueue_stream_control_locked(&control);
    if (direct != NULL) {
        *direct = control;
    }
    return true;
}

static bool parse_frontend_request_locked(const struct sockaddr_in *source,
                                          uint32_t message_type,
                                          const uint8_t *payload,
                                          size_t payload_size,
                                          DSUDirectRequest *direct)
{
    int player = first_source_player_locked(source);

    if (player < 0 || direct == NULL) {
        return false;
    }
    memset(direct, 0, sizeof(*direct));
    direct->player = player;

    switch (message_type) {
    case DSU_MSG_PLAYLIST_LIST_REQUEST:
        if (payload_size < 1) {
            return false;
        }
        direct->kind = DSU_DIRECT_PLAYLIST_LIST;
        direct->page = payload[0];
        return true;
    case DSU_MSG_PLAYLIST_CONTENTS_REQUEST:
        if (payload_size < 1) {
            return false;
        }
        direct->kind = DSU_DIRECT_PLAYLIST_CONTENTS;
        direct->page = payload[0];
        copy_fixed_string(direct->path, sizeof(direct->path), payload + 1,
                          payload_size - 1);
        return true;
    case DSU_MSG_COMMAND:
        if (payload_size < 4 + 256 + 256) {
            return false;
        }
        direct->kind = DSU_DIRECT_COMMAND;
        direct->command = get_le32(payload);
        copy_fixed_string(direct->path, sizeof(direct->path), payload + 4,
                          256);
        copy_fixed_string(direct->core, sizeof(direct->core), payload + 260,
                          256);
        return true;
    default:
        return false;
    }
}

/* Revision and launch id of a catalogue entry. */
static bool parse_app_launch_locked(const struct sockaddr_in *source,
                                    const uint8_t *payload,
                                    size_t payload_size,
                                    DSUDirectRequest *direct)
{
    int player = first_source_player_locked(source);

    if (player < 0 || direct == NULL || payload_size < 12) {
        return false;
    }
    memset(direct, 0, sizeof(*direct));
    direct->kind = DSU_DIRECT_APP_LAUNCH;
    direct->player = player;
    direct->revision = get_le32(payload);
    direct->launch_id = get_le64(payload + 4);
    return true;
}

/* Held for the library screen; see xemu_dsu_take_app_selection(). */
static void parse_app_selection_locked(const uint8_t *payload,
                                       size_t payload_size, int64_t now)
{
    if (payload_size < 12) {
        return;
    }
    s_app_selection_id = get_le64(payload + 4);
    s_app_selection_us = now;
    s_app_selection_pending = true;
}

static bool process_packet_locked(const uint8_t *packet, size_t packet_size,
                                  const struct sockaddr_in *source,
                                  int64_t now, DSUDirectRequest *direct)
{
    const uint8_t *payload;
    size_t payload_size;
    uint32_t server_id;
    uint32_t message_type;
    int source_player;
    unsigned player;

    if (!validate_packet(packet, packet_size, &server_id, &message_type,
                         &payload, &payload_size)) {
        return false;
    }
    source_player = first_source_player_locked(source);
    if (source_player < 0) {
        return false;
    }

    /* Multiple configured players may share one DSU server endpoint. Update
     * every matching slot so a server process restart invalidates each motor
     * cache exactly once. Servers that always report sender id zero rely on
     * the data packet-number rollback check in parse_controller_data_locked()
     * instead. */
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        DSUEndpoint *endpoint = &s_endpoints[player];

        if (!source_matches(endpoint, source)) {
            continue;
        }
        if (endpoint->server_id_valid && endpoint->server_id != server_id) {
            invalidate_rumble_locked(player);
        }
        endpoint->server_id = server_id;
        endpoint->server_id_valid = true;
        endpoint->last_received_us = now;
    }

    switch (message_type) {
    case DSU_MSG_CONTROLLER:
        parse_controller_info_locked(source, payload, payload_size);
        break;
    case DSU_MSG_DATA:
        parse_controller_data_locked(source, payload, payload_size, now);
        break;
    case DSU_MSG_KEYBOARD:
        parse_keyboard_locked(source, payload, payload_size, now);
        break;
    case DSU_MSG_MOUSE:
        parse_mouse_locked(source, payload, payload_size, now);
        break;
    case DSU_MSG_STREAM_START:
    case DSU_MSG_STREAM_STOP:
        if (direct != NULL) {
            memset(direct, 0, sizeof(*direct));
            direct->kind = DSU_DIRECT_STREAM;
            if (!parse_stream_control_locked(source, message_type, payload,
                                             payload_size,
                                             &direct->stream)) {
                direct->kind = DSU_DIRECT_NONE;
            }
            return direct->kind != DSU_DIRECT_NONE;
        }
        break;
    case DSU_MSG_PLAYLIST_LIST_REQUEST:
    case DSU_MSG_PLAYLIST_CONTENTS_REQUEST:
    case DSU_MSG_COMMAND:
        return parse_frontend_request_locked(source, message_type, payload,
                                             payload_size, direct);
    case DSU_MSG_APP_LAUNCH:
        return parse_app_launch_locked(source, payload, payload_size,
                                       direct);
    case DSU_MSG_APP_SELECTION:
        parse_app_selection_locked(payload, payload_size, now);
        break;
    case DSU_MSG_VERSION:
    case DSU_MSG_MOTOR_INFO:
    case DSU_MSG_RUMBLE:
    case DSU_MSG_STATE:
    case DSU_MSG_STREAM_STATUS:
    default:
        break;
    }
    return false;
}

static XemuDSUStreamStatus status_from_control(
    const XemuDSUStreamControl *control, uint32_t state, uint32_t error)
{
    XemuDSUStreamStatus status = { 0 };

    status.state = state;
    status.error = error;
    status.stream_type = control->stream_type;
    g_strlcpy(status.url, control->url, sizeof(status.url));
    return status;
}

static void status_update_stream_geometry(XemuDSUStreamStatus *status)
{
    int width;
    int height;

    if (xemu_stream_get_output_size(&width, &height)) {
        status->width = width;
        status->height = height;
        status->aspect_ratio = (float)width / (float)height;
    }
}

static void apply_main_stream_control(const XemuDSUStreamControl *control);

static void report_status_to_player(unsigned player,
                                    const XemuDSUStreamStatus *status)
{
    g_mutex_lock(&s_lock);
    if (s_initialized && player < XEMU_DSU_MAX_PLAYERS) {
        s_stream_status = *status;
        s_stream_status_valid = true;
        send_stream_status_to_locked(player, status);
    }
    g_mutex_unlock(&s_lock);
}

static void send_playlist_list(unsigned player, uint8_t page)
{
    uint8_t payload[3 + 64 + 256] = { 0 };

    payload[0] = page;
    payload[1] = 1;
    if (page == 0) {
        payload[2] = 1;
        put_fixed_string(payload + 3, 64, DSU_XBOX_PLAYLIST);
        put_fixed_string(payload + 3 + 64, 256, DSU_XBOX_PLAYLIST);
    }

    g_mutex_lock(&s_lock);
    if (s_initialized) {
        send_packet_locked(player, DSU_MSG_PLAYLIST_LIST, payload,
                           page == 0 ? sizeof(payload) : 3);
    }
    g_mutex_unlock(&s_lock);
}

static void send_playlist_contents(unsigned player, uint8_t page,
                                   const char *playlist)
{
    size_t total_entries = 0;
    XemuGameLibraryEntry *entries = NULL;
    size_t start;
    size_t count = 0;
    uint8_t total_pages = 1;

    if (playlist != NULL && strcmp(playlist, DSU_XBOX_PLAYLIST) == 0) {
        /* This runs under the BQL. A network request must not wait on
         * library metadata parsing or enumerate the games directory here. */
        entries =
            xemu_game_library_snapshot_nonblocking(&total_entries);
        size_t calculated =
            (total_entries + DSU_PLAYLIST_PAGE_SIZE - 1) /
            DSU_PLAYLIST_PAGE_SIZE;
        total_pages = (uint8_t)CLAMP(calculated, 1, 255);
        start = (size_t)page * DSU_PLAYLIST_PAGE_SIZE;
        if (page < total_pages && start < total_entries) {
            count = MIN((size_t)DSU_PLAYLIST_PAGE_SIZE,
                        total_entries - start);
        }
    } else {
        start = 0;
    }

    size_t payload_size = 3 + count * (64 + 256 + 256);
    uint8_t *payload = g_malloc0(payload_size);
    payload[0] = page;
    payload[1] = total_pages;
    payload[2] = (uint8_t)count;
    size_t offset = 3;
    for (size_t i = 0; i < count; i++) {
        XemuGameLibraryEntry *entry = &entries[start + i];
        put_fixed_string(payload + offset, 64, entry->label);
        offset += 64;
        put_fixed_string(payload + offset, 256, entry->token);
        offset += 256;
        put_fixed_string(payload + offset, 256, "xemu");
        offset += 256;
    }

    g_mutex_lock(&s_lock);
    if (s_initialized) {
        send_packet_locked(player, DSU_MSG_PLAYLIST_CONTENTS, payload,
                           payload_size);
    }
    g_mutex_unlock(&s_lock);
    g_free(payload);
    xemu_game_library_free_snapshot(entries, total_entries);
}

/* Mounts a library disc and boots it. */
static bool launch_library_disc(const char *path)
{
    Error *error = NULL;

    xemu_load_disc(path, &error);
    if (error != NULL) {
        fprintf(stderr, "DSU: failed to launch '%s': %s\n", path,
                error_get_pretty(error));
        error_free(error);
        return false;
    }
#ifdef CONFIG_UWP
    xemu_settings_save();
#endif
    /* Boot pipelines belong to the new disc's list. */
    nv2a_shader_cache_set_title(xemu_game_library_title_id_for_path(path));
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    if (!runstate_is_running()) {
        vm_start();
    }
    fprintf(stderr, "DSU: mounted and launched '%s'\n", path);
    return true;
}

static void apply_frontend_command(const DSUDirectRequest *request)
{
    if (request->command != 1) {
        fprintf(stderr,
                "DSU: ignoring unsupported frontend command %u\n",
                request->command);
        return;
    }

    char *path = xemu_game_library_resolve_token(request->path);
    if (path == NULL) {
        fprintf(stderr,
                "DSU: rejected launch token outside game allowlist\n");
        return;
    }
    launch_library_disc(path);
    g_free(path);
}

static void apply_app_launch(const DSUDirectRequest *request)
{
    uint8_t payload[DSU_APP_LAUNCH_STATUS_SIZE] = { 0 };
    uint32_t status = DSU_APP_LAUNCH_ACCEPTED;
    const char *message = "Launch accepted";
    char *path = xemu_game_library_resolve_launch_id(request->launch_id);

    if (path == NULL) {
        fprintf(stderr, "DSU: rejected unknown launch id %016" PRIX64 "\n",
                request->launch_id);
        status = DSU_APP_LAUNCH_UNKNOWN_ENTRY;
        message = "Unknown library entry";
    } else if (!launch_library_disc(path)) {
        status = DSU_APP_LAUNCH_UNKNOWN_ENTRY;
        message = "The disc could not be loaded";
    }
    g_free(path);

    put_le32(payload, status);
    put_le32(payload + 4, request->revision);
    put_le64(payload + 8, request->launch_id);
    put_fixed_string(payload + 16, 128, message);

    g_mutex_lock(&s_lock);
    if (s_initialized) {
        send_packet_locked(request->player, DSU_MSG_APP_LAUNCH_STATUS,
                           payload, sizeof(payload));
    }
    g_mutex_unlock(&s_lock);
}

static void apply_direct_request(const DSUDirectRequest *request)
{
    switch (request->kind) {
    case DSU_DIRECT_STREAM:
        apply_main_stream_control(&request->stream);
        break;
    case DSU_DIRECT_PLAYLIST_LIST:
        send_playlist_list(request->player, request->page);
        break;
    case DSU_DIRECT_PLAYLIST_CONTENTS:
        send_playlist_contents(request->player, request->page, request->path);
        break;
    case DSU_DIRECT_COMMAND:
        apply_frontend_command(request);
        break;
    case DSU_DIRECT_APP_LAUNCH:
        apply_app_launch(request);
        break;
    case DSU_DIRECT_NONE:
    default:
        break;
    }
}

static void apply_main_stream_control(const XemuDSUStreamControl *control)
{
    XemuDSUStreamStatus status;
    bool active;
    /* A frontend answering xemu's own request is always allowed. */
    bool requested = control->authorization_token != 0 &&
                     control->authorization_token == s_stream_request_token;

    if (!g_config.display.stream.remote_start && !requested) {
        status = status_from_control(control, XEMU_DSU_STREAM_ERROR, 1);
        report_status_to_player(control->player, &status);
        return;
    }

    if (control->action == XEMU_DSU_STREAM_CONTROL_START) {
        if (control->url[0] == 0) {
            status = status_from_control(control, XEMU_DSU_STREAM_ERROR, 2);
            report_status_to_player(control->player, &status);
            return;
        }
        xemu_stream_request_start_with_params(
            control->url, control->bitrate_kbps, control->fps);
        s_frontend_stream = true;
        status = status_from_control(control, XEMU_DSU_STREAM_STOPPED, 0);
        active = xemu_stream_is_ready();
        g_mutex_lock(&s_lock);
        s_stream_status = status;
        s_stream_status_valid = true;
        s_stream_observer = control->player;
        s_stream_observer_active = active;
        s_stream_observer_starting = !active;
        s_stream_observer_deadline_us =
            g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        g_mutex_unlock(&s_lock);
        if (active) {
            status = status_from_control(control, XEMU_DSU_STREAM_ACTIVE, 0);
            status_update_stream_geometry(&status);
            report_status_to_player(control->player, &status);
        }
    } else if (control->action == XEMU_DSU_STREAM_CONTROL_STOP) {
        /* The frontend's stream ends here. A configured local broadcast may
         * carry on, but it is not the frontend's stream: reporting it as
         * active makes the frontend stop it again, endlessly. */
        xemu_stream_request_stop();
        s_frontend_stream = false;
        status = status_from_control(control, XEMU_DSU_STREAM_STOPPED, 0);
        report_status_to_player(control->player, &status);
        g_mutex_lock(&s_lock);
        s_stream_observer = -1;
        s_stream_observer_active = false;
        s_stream_observer_starting = false;
        g_mutex_unlock(&s_lock);
    }
}

static void monitor_stream_status(void)
{
    XemuDSUStreamStatus status;
    bool active = xemu_stream_is_ready();
    bool worker_active = xemu_stream_is_active();
    int64_t now = g_get_monotonic_time();
    int observer;

    g_mutex_lock(&s_lock);
    observer = s_stream_observer;
    if (!s_initialized || observer < 0) {
        g_mutex_unlock(&s_lock);
        return;
    }
    if (s_stream_observer_starting) {
        if (active) {
            s_stream_observer_starting = false;
        } else if (now >= s_stream_observer_deadline_us && !worker_active) {
            s_stream_observer_starting = false;
            status = s_stream_status_valid ? s_stream_status
                                           : (XemuDSUStreamStatus){ 0 };
            status.state = XEMU_DSU_STREAM_ERROR;
            status.error = 1;
            s_stream_status = status;
            s_stream_status_valid = true;
            send_stream_status_to_locked(observer, &status);
            g_mutex_unlock(&s_lock);
            return;
        } else {
            g_mutex_unlock(&s_lock);
            return;
        }
    }
    if (active == s_stream_observer_active) {
        g_mutex_unlock(&s_lock);
        return;
    }
    s_stream_observer_active = active;
    if (s_stream_status_valid) {
        status = s_stream_status;
    } else {
        memset(&status, 0, sizeof(status));
    }
    status.state = active ? XEMU_DSU_STREAM_ACTIVE : XEMU_DSU_STREAM_STOPPED;
    status.error = 0;
    if (active) {
        status_update_stream_geometry(&status);
    }
    s_stream_status = status;
    s_stream_status_valid = true;
    send_stream_status_to_locked(observer, &status);
    g_mutex_unlock(&s_lock);
}

/* A frontend is connected while its server keeps sending. */
static bool endpoint_connected_locked(unsigned player, int64_t now)
{
    const DSUEndpoint *endpoint = &s_endpoints[player];

    return endpoint->address_valid && endpoint->last_received_us != 0 &&
           now - endpoint->last_received_us < DSU_CONTROLLER_TIMEOUT_US;
}

/* Several players may share one server; address each server once. */
static bool endpoint_seen_earlier_locked(unsigned player)
{
    for (unsigned earlier = 0; earlier < player; earlier++) {
        if (s_endpoints[earlier].address_valid &&
            s_endpoints[earlier].remote.sin_addr.s_addr ==
                s_endpoints[player].remote.sin_addr.s_addr &&
            s_endpoints[earlier].remote.sin_port ==
                s_endpoints[player].remote.sin_port) {
            return true;
        }
    }
    return false;
}

bool xemu_dsu_frontend_connected(void)
{
    bool connected = false;
    int64_t now = g_get_monotonic_time();

    if (!s_initialized || !g_atomic_int_get(&s_any_configured)) {
        return false;
    }
    g_mutex_lock(&s_lock);
    for (unsigned player = 0; player < XEMU_DSU_MAX_PLAYERS && !connected;
         player++) {
        connected = endpoint_connected_locked(player, now);
    }
    g_mutex_unlock(&s_lock);
    return connected;
}

bool xemu_dsu_request_stream(bool start)
{
    uint8_t payload[6];
    bool sent = false;
    int64_t now = g_get_monotonic_time();

    if (!s_initialized || !g_atomic_int_get(&s_any_configured)) {
        return false;
    }
    g_mutex_lock(&s_lock);
    if (++s_stream_request_token == 0) {
        ++s_stream_request_token;
    }
    payload[0] = start ? 1 : 0;
    payload[1] = 0x01; /* main stream */
    put_le32(payload + 2, s_stream_request_token);
    for (unsigned player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        if (endpoint_connected_locked(player, now) &&
            !endpoint_seen_earlier_locked(player)) {
            sent |= send_packet_locked(player, DSU_MSG_STREAM_CONTROL_REQUEST,
                                       payload, sizeof(payload));
        }
    }
    g_mutex_unlock(&s_lock);
    return sent;
}

bool xemu_dsu_frontend_stream_active(void)
{
    return s_frontend_stream && xemu_stream_is_active();
}

/* Streams follow the frontend: it is asked for one once per running title,
 * and a stream it started ends when it stops answering. */
static void update_frontend_stream(const XemuGameLibraryState *library)
{
    static uint32_t requested_title_id;
    static bool was_connected;
    bool connected = xemu_dsu_frontend_connected();

    if (connected != was_connected) {
        fprintf(stderr, "DSU: frontend %s\n",
                connected ? "connected" : "disconnected");
        was_connected = connected;
    }
    if (!library->running || library->title_id == 0) {
        requested_title_id = 0;
    }
    if (!connected) {
        if (s_frontend_stream) {
            s_frontend_stream = false;
            xemu_stream_request_stop();
            g_mutex_lock(&s_lock);
            s_stream_observer = -1;
            s_stream_observer_active = false;
            s_stream_observer_starting = false;
            g_mutex_unlock(&s_lock);
        }
        return;
    }
    if (g_config.display.stream.remote_start && library->running &&
        library->title_id != 0 && library->title_id != requested_title_id &&
        !xemu_stream_is_active()) {
        bool sent = xemu_dsu_request_stream(true);
        fprintf(stderr, "DSU: stream request for '%s' %s\n",
                library->game_name != NULL ? library->game_name : "",
                sent ? "sent" : "not sent");
        requested_title_id = library->title_id;
    }
}

/* The catalogue is only served to configured DSU servers. Runs on the library
 * server thread. */
static bool library_peer_allowed(uint32_t address)
{
    bool allowed = false;
    unsigned player;

    if (!s_initialized) {
        return false;
    }
    g_mutex_lock(&s_lock);
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        if (s_endpoints[player].address_valid &&
            s_endpoints[player].remote.sin_addr.s_addr == address) {
            allowed = true;
            break;
        }
    }
    g_mutex_unlock(&s_lock);
    return allowed;
}

/* The catalogue is built from a published library scan. Schedule one when
 * nothing else has, retrying briefly while the library is busy. */
static void request_library_scan(void)
{
    static int attempts;
    static int64_t next_attempt_us;
    int64_t now = g_get_monotonic_time();
    XemuGameLibraryEntry *entries;
    size_t count = 0;

    if (attempts >= DSU_LIBRARY_SCAN_ATTEMPTS || now < next_attempt_us) {
        return;
    }
    next_attempt_us = now + G_USEC_PER_SEC;
    entries = xemu_game_library_snapshot_nonblocking(&count);
    attempts = entries != NULL ? DSU_LIBRARY_SCAN_ATTEMPTS : attempts + 1;
    xemu_game_library_free_snapshot(entries, count);
}

static void refresh_frontend_state(void)
{
    static int64_t next_refresh_us;
    XemuGameLibraryState library = { 0 };
    XemuDSUFrontendState frontend = { 0 };
    bool changed;
    int64_t now = g_get_monotonic_time();

    /* This runs with the main-loop lock held, and building the state
     * allocates strings and touches the metadata cache. Frontend identity does
     * not change at render cadence, so rate-limit the refresh. */
    if (now < next_refresh_us) {
        return;
    }
    next_refresh_us = now + DSU_FRONTEND_REFRESH_INTERVAL_US;

    xemu_game_library_refresh_running_title();
    if (!xemu_game_library_get_current_state(&library)) {
        return;
    }
    frontend.flags = library.running ? 1 : 0;
    g_strlcpy(frontend.game_name,
              library.game_name != NULL ? library.game_name : "",
              sizeof(frontend.game_name));
    g_strlcpy(frontend.platform_name,
              library.platform_name != NULL ? library.platform_name : "",
              sizeof(frontend.platform_name));
    g_strlcpy(frontend.core_name,
              library.core_name != NULL ? library.core_name : "",
              sizeof(frontend.core_name));

    g_mutex_lock(&s_lock);
    changed = !s_frontend_state_valid ||
              memcmp(&s_frontend_state, &frontend, sizeof(frontend)) != 0;
    g_mutex_unlock(&s_lock);
    if (changed) {
        xemu_dsu_send_frontend_state(&frontend);
    }
    update_frontend_stream(&library);
    xemu_game_library_free_current_state(&library);
}

void xemu_dsu_init(void)
{
    if (s_initialized) {
        return;
    }
    g_mutex_init(&s_lock);
    g_mutex_lock(&s_lock);
    memset(s_endpoints, 0, sizeof(s_endpoints));
    memset(s_players, 0, sizeof(s_players));
    memset(s_rumble_value, 0, sizeof(s_rumble_value));
    memset(s_rumble_valid, 0, sizeof(s_rumble_valid));
    s_control_head = 0;
    s_control_count = 0;
    s_control_sequence = 0;
    s_direct_head = 0;
    s_direct_count = 0;
    s_frontend_state_valid = false;
    s_stream_status_valid = false;
    s_stream_observer = -1;
    s_stream_observer_active = false;
    s_stream_observer_starting = false;
    s_stream_observer_deadline_us = 0;
    s_next_poll_us = 0;
    g_atomic_int_set(&s_config_enabled, 0);
    g_atomic_int_set(&s_any_configured, 0);
    s_client_id = g_random_int();
    if (s_client_id == 0) {
        s_client_id = 1;
    }
    s_initialized = true;
    g_mutex_unlock(&s_lock);
}

void xemu_dsu_shutdown(void)
{
    if (!s_initialized) {
        return;
    }
    xemu_library_server_stop();
    s_library_server_started = false;
    g_mutex_lock(&s_lock);
    close_socket_locked();
    g_atomic_int_set(&s_config_enabled, 0);
    g_atomic_int_set(&s_any_configured, 0);
    s_initialized = false;
    memset(s_endpoints, 0, sizeof(s_endpoints));
    memset(s_players, 0, sizeof(s_players));
    memset(s_rumble_valid, 0, sizeof(s_rumble_valid));
    s_direct_head = 0;
    s_direct_count = 0;
    g_mutex_unlock(&s_lock);
    g_mutex_clear(&s_lock);
}

void xemu_dsu_poll_network(void)
{
    uint8_t packet[DSU_MAX_PACKET_SIZE];
    int64_t now;
    int64_t receive_deadline;
    unsigned player;
    unsigned packets = 0;

    if (!s_initialized) {
        return;
    }

    now = g_get_monotonic_time();
    /* DSU servers publish controller state at video/input cadence, so a
     * 250 Hz receive cadence is sufficient. This function is called outside
     * the BQL so the bounded packet drain cannot stall the emulation thread. */
    if (now < s_next_poll_us) {
        return;
    }
    s_next_poll_us = now + DSU_POLL_INTERVAL_US;

    g_mutex_lock(&s_lock);
    if (!s_initialized) {
        g_mutex_unlock(&s_lock);
        return;
    }
    if (!g_atomic_int_get(&s_any_configured)) {
        close_socket_locked();
        g_mutex_unlock(&s_lock);
        return;
    }
    if (!ensure_socket_locked()) {
        g_mutex_unlock(&s_lock);
        return;
    }
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        DSUEndpoint *endpoint = &s_endpoints[player];

        if (endpoint->address_valid && s_fd >= 0 &&
            now >= endpoint->next_subscription_us) {
            send_subscription_locked(player, now);
        }
        if (s_players[player].controller.connected &&
            s_players[player].controller.last_received_us != 0 &&
            now - s_players[player].controller.last_received_us >
                DSU_CONTROLLER_TIMEOUT_US) {
            s_players[player].controller.connected = false;
            invalidate_rumble_locked(player);
        }
    }
    g_mutex_unlock(&s_lock);

    if (!g_atomic_int_get(&s_any_configured)) {
        return;
    }

    receive_deadline = g_get_monotonic_time() + DSU_POLL_TIME_BUDGET_US;
    while (packets < DSU_POLL_PACKET_BUDGET &&
           g_get_monotonic_time() < receive_deadline) {
        struct sockaddr_in source;
        socklen_t source_size = sizeof(source);
        DSUDirectRequest direct = { 0 };
        ssize_t received;
        g_mutex_lock(&s_lock);
        if (s_fd < 0) {
            g_mutex_unlock(&s_lock);
            break;
        }
        received = recvfrom(s_fd, packet, sizeof(packet), 0,
                            (struct sockaddr *)&source, &source_size);
        if (received <= 0) {
            g_mutex_unlock(&s_lock);
            break;
        }
        packets++;
        now = g_get_monotonic_time();
        if (process_packet_locked(packet, received, &source, now, &direct)) {
            enqueue_direct_request_locked(&direct);
        }
        g_mutex_unlock(&s_lock);
    }
}

void xemu_dsu_process_pending(void)
{
    unsigned processed = 0;

    if (!s_initialized) {
        return;
    }

    /* Configuration belongs to the UI/BQL domain. Publish a synchronized
     * endpoint snapshot for xemu_dsu_poll_network() instead of reading config
     * from the unlocked network path. */
    if (!any_player_enabled_config() &&
        !g_atomic_int_get(&s_config_enabled)) {
        return;
    }

    g_mutex_lock(&s_lock);
    refresh_config_locked();
    g_mutex_unlock(&s_lock);

    while (processed < DSU_DIRECT_PROCESS_BUDGET) {
        DSUDirectRequest direct;
        unsigned player;
        bool valid;

        g_mutex_lock(&s_lock);
        if (!dequeue_direct_request_locked(&direct)) {
            g_mutex_unlock(&s_lock);
            break;
        }
        player = direct.kind == DSU_DIRECT_STREAM
                     ? direct.stream.player
                     : direct.player;
        valid = s_initialized && player < XEMU_DSU_MAX_PLAYERS &&
                s_endpoints[player].address_valid;
        g_mutex_unlock(&s_lock);

        if (valid) {
            apply_direct_request(&direct);
        }
        processed++;
    }

    if (g_atomic_int_get(&s_any_configured)) {
        /* The catalogue server must be listening before the first state
         * advertises it: frontends read the catalogue once per connection. */
        if (!s_library_server_started) {
            s_library_server_started = true;
            xemu_library_server_start(XEMU_LIBRARY_SERVER_PORT,
                                      library_peer_allowed);
        }
        if (xemu_library_server_is_running()) {
            request_library_scan();
        }
        monitor_stream_status();
        refresh_frontend_state();
    }
}

bool xemu_dsu_is_active(void)
{
    return s_initialized && g_atomic_int_get(&s_any_configured);
}

bool xemu_dsu_get_player_state(unsigned player, XemuDSUPlayerState *out)
{
    bool configured;

    if (!s_initialized || !g_atomic_int_get(&s_any_configured) ||
        player >= XEMU_DSU_MAX_PLAYERS || out == NULL) {
        return false;
    }
    g_mutex_lock(&s_lock);
    *out = s_players[player];
    configured = out->configured;
    g_mutex_unlock(&s_lock);
    return configured;
}

bool xemu_dsu_player_is_addon(unsigned player)
{
    bool addon = false;

    if (!s_initialized || !g_atomic_int_get(&s_any_configured) ||
        player >= XEMU_DSU_MAX_PLAYERS) {
        return false;
    }
    g_mutex_lock(&s_lock);
    addon = s_players[player].configured && s_players[player].addon;
    g_mutex_unlock(&s_lock);
    return addon;
}

bool xemu_dsu_consume_mouse(unsigned player, XemuDSUMouseState *out)
{
    bool configured;

    if (!s_initialized || !g_atomic_int_get(&s_any_configured) ||
        player >= XEMU_DSU_MAX_PLAYERS || out == NULL) {
        return false;
    }
    g_mutex_lock(&s_lock);
    configured = s_players[player].configured &&
                 (s_players[player].enabled_data_types & XEMU_DSU_DATA_MOUSE);
    *out = s_players[player].mouse;
    s_players[player].mouse.delta_x = 0;
    s_players[player].mouse.delta_y = 0;
    s_players[player].mouse.wheel_x = 0;
    s_players[player].mouse.wheel_y = 0;
    g_mutex_unlock(&s_lock);
    return configured;
}

bool xemu_dsu_send_rumble(unsigned player, uint8_t motor, uint8_t intensity)
{
    uint8_t payload[10] = { 0 };
    bool sent;

    if (!s_initialized || !g_atomic_int_get(&s_any_configured) ||
        player >= XEMU_DSU_MAX_PLAYERS) {
        return false;
    }
    g_mutex_lock(&s_lock);
    if (motor < DSU_RUMBLE_MOTOR_COUNT &&
        (s_rumble_valid[player] & (1u << motor)) &&
        s_rumble_value[player][motor] == intensity) {
        g_mutex_unlock(&s_lock);
        return true;
    }
    payload[0] = 1; /* target by slot */
    payload[1] = s_endpoints[player].config.slot;
    memcpy(payload + 2, s_players[player].controller.mac, 6);
    payload[8] = motor;
    payload[9] = intensity;
    sent = send_packet_locked(player, DSU_MSG_RUMBLE, payload,
                              sizeof(payload));
    if (sent && motor < DSU_RUMBLE_MOTOR_COUNT) {
        s_rumble_value[player][motor] = intensity;
        s_rumble_valid[player] |= 1u << motor;
    }
    g_mutex_unlock(&s_lock);
    return sent;
}

bool xemu_dsu_take_stream_control(XemuDSUStreamControl *out)
{
    if (!s_initialized || out == NULL) {
        return false;
    }
    g_mutex_lock(&s_lock);
    if (s_control_count == 0) {
        g_mutex_unlock(&s_lock);
        return false;
    }
    *out = s_control_queue[s_control_head];
    s_control_head = (s_control_head + 1) % DSU_CONTROL_QUEUE_LENGTH;
    s_control_count--;
    g_mutex_unlock(&s_lock);
    return true;
}

void xemu_dsu_report_stream_status(const XemuDSUStreamStatus *status)
{
    unsigned player;

    if (!s_initialized || status == NULL) {
        return;
    }
    g_mutex_lock(&s_lock);
    s_stream_status = *status;
    s_stream_status.url[XEMU_DSU_STREAM_URL_LENGTH] = 0;
    s_stream_status_valid = true;
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        if (s_endpoints[player].address_valid) {
            send_stream_status_to_locked(player, &s_stream_status);
        }
    }
    g_mutex_unlock(&s_lock);
}

void xemu_dsu_send_frontend_state(const XemuDSUFrontendState *state)
{
    unsigned player;

    if (!s_initialized) {
        return;
    }
    g_mutex_lock(&s_lock);
    if (state == NULL) {
        memset(&s_frontend_state, 0, sizeof(s_frontend_state));
        s_frontend_state_valid = false;
        g_mutex_unlock(&s_lock);
        return;
    }
    s_frontend_state = *state;
    s_frontend_state.game_name[XEMU_DSU_STATE_TEXT_LENGTH] = 0;
    s_frontend_state.platform_name[XEMU_DSU_STATE_TEXT_LENGTH] = 0;
    s_frontend_state.core_name[XEMU_DSU_STATE_TEXT_LENGTH] = 0;
    s_frontend_state_valid = true;
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        if (s_endpoints[player].address_valid) {
            send_frontend_state_to_locked(player, &s_frontend_state);
        }
    }
    g_mutex_unlock(&s_lock);
}

bool xemu_dsu_take_app_selection(uint64_t *launch_id)
{
    bool taken = false;

    if (!s_initialized || launch_id == NULL) {
        return false;
    }
    g_mutex_lock(&s_lock);
    if (s_app_selection_pending) {
        s_app_selection_pending = false;
        if (g_get_monotonic_time() - s_app_selection_us <
            DSU_APP_SELECTION_TTL_US) {
            *launch_id = s_app_selection_id;
            taken = true;
        }
    }
    g_mutex_unlock(&s_lock);
    return taken;
}

void xemu_dsu_send_app_selection(uint32_t revision, uint64_t launch_id)
{
    uint8_t payload[12];
    unsigned player;

    if (!s_initialized || !g_atomic_int_get(&s_any_configured)) {
        return;
    }
    put_le32(payload, revision);
    put_le64(payload + 4, launch_id);
    g_mutex_lock(&s_lock);
    for (player = 0; player < XEMU_DSU_MAX_PLAYERS; player++) {
        if (s_endpoints[player].address_valid) {
            send_packet_locked(player, DSU_MSG_APP_SELECTION, payload,
                               sizeof(payload));
        }
    }
    g_mutex_unlock(&s_lock);
}
