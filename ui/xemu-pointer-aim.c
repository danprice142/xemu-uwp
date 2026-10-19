/*
 * Backend-neutral pointer/lightgun aim conversion.
 *
 * This is independent of any host input API (Raw Input, evdev, SDL, HID) so
 * the same conversion can serve both local pointers and the DSU touch source.
 */

#include "qemu/osdep.h"

#include "ui/xemu-pointer-aim.h"

#include <math.h>

static XemuPointerDisplayRect game_display_rect;

static float clampf(float value, float minimum, float maximum)
{
    return MIN(MAX(value, minimum), maximum);
}

void xemu_pointer_aim_set_display_rect(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        memset(&game_display_rect, 0, sizeof(game_display_rect));
        return;
    }

    game_display_rect.x = x;
    game_display_rect.y = y;
    game_display_rect.width = width;
    game_display_rect.height = height;
}

bool xemu_pointer_aim_get_display_rect(XemuPointerDisplayRect *rect)
{
    if (rect == NULL) {
        return false;
    }

    *rect = game_display_rect;
    return rect->width > 0 && rect->height > 0;
}

void xemu_pointer_aim_reset(XemuPointerAimState *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void xemu_pointer_aim_update_normalized(
    XemuPointerAimState *state, float normalized_x, float normalized_y,
    bool active, float smoothing, float sensitivity, uint64_t timestamp_ns,
    XemuPointerAimResult *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    if (state == NULL || !active || !isfinite(normalized_x) ||
        !isfinite(normalized_y)) {
        xemu_pointer_aim_reset(state);
        return;
    }

    /* Convert [0,1], top-left origin to [-1,1], bottom-left origin. */
    float x = 2.0f * normalized_x - 1.0f;
    float y = 1.0f - 2.0f * normalized_y;

    smoothing = clampf(smoothing, 0.0f, 1.0f);
    if (smoothing > 0.0f) {
        if (!state->valid) {
            state->filtered_x = x;
            state->filtered_y = y;
            state->filtered_dx = 0.0f;
            state->filtered_dy = 0.0f;
            state->valid = true;
        } else {
            float dt = 1.0f / 60.0f;
            if (timestamp_ns > state->timestamp_ns) {
                dt = (float)(timestamp_ns - state->timestamp_ns) / 1.0e9f;
            }
            dt = clampf(dt, 1.0e-4f, 0.1f);

            const float two_pi = 6.28318530718f;
            float derivative_alpha =
                1.0f / (1.0f + 1.0f / (two_pi * dt));
            float raw_dx = (x - state->filtered_x) / dt;
            float raw_dy = (y - state->filtered_y) / dt;
            state->filtered_dx +=
                derivative_alpha * (raw_dx - state->filtered_dx);
            state->filtered_dy +=
                derivative_alpha * (raw_dy - state->filtered_dy);

            float minimum_cutoff = 6.0f - 5.5f * smoothing;
            const float beta = 5.0f;
            float cutoff_x = minimum_cutoff + beta * fabsf(state->filtered_dx);
            float cutoff_y = minimum_cutoff + beta * fabsf(state->filtered_dy);
            float alpha_x =
                1.0f / (1.0f + 1.0f / (two_pi * cutoff_x * dt));
            float alpha_y =
                1.0f / (1.0f + 1.0f / (two_pi * cutoff_y * dt));
            state->filtered_x += alpha_x * (x - state->filtered_x);
            state->filtered_y += alpha_y * (y - state->filtered_y);
        }

        x = state->filtered_x;
        y = state->filtered_y;
    } else {
        state->valid = false;
    }

    state->timestamp_ns = timestamp_ns;

    if (sensitivity > 0.0f) {
        x *= sensitivity;
        y *= sensitivity;
    }

    result->onscreen = x >= -1.0f && x <= 1.0f && y >= -1.0f && y <= 1.0f;
    x = clampf(x, -1.0f, 1.0f);
    y = clampf(y, -1.0f, 1.0f);
    result->axis_x = (int16_t)(x * 32767.0f);
    result->axis_y = (int16_t)(y * 32767.0f);
}

void xemu_pointer_aim_update_dsu_touch(
    XemuPointerAimState *state, uint16_t touch_x, uint16_t touch_y,
    bool active, float smoothing, float sensitivity, uint64_t timestamp_ns,
    XemuPointerAimResult *result)
{
    const float scale = 1.0f / 32767.0f;
    xemu_pointer_aim_update_normalized(
        state, (float)touch_x * scale, (float)touch_y * scale, active,
        smoothing, sensitivity, timestamp_ns, result);
}

bool xemu_pointer_aim_normalize_pixels(int pointer_x, int pointer_y,
                                      int fallback_width,
                                      int fallback_height,
                                      float *normalized_x,
                                      float *normalized_y)
{
    if (normalized_x == NULL || normalized_y == NULL) {
        return false;
    }

    XemuPointerDisplayRect rect;
    if (!xemu_pointer_aim_get_display_rect(&rect)) {
        if (fallback_width <= 0 || fallback_height <= 0) {
            return false;
        }
        rect.x = 0;
        rect.y = 0;
        rect.width = fallback_width;
        rect.height = fallback_height;
    }

    *normalized_x = (float)(pointer_x - rect.x) / (float)rect.width;
    *normalized_y = (float)(pointer_y - rect.y) / (float)rect.height;
    return true;
}
