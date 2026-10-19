/*
 * Backend-neutral pointer/lightgun aim conversion.
 *
 * Host input backends supply normalized coordinates (or convert window
 * pixels through the published game display rectangle). The result is an
 * Xbox lightgun left-stick position plus an on-screen flag.
 */

#ifndef XEMU_POINTER_AIM_H
#define XEMU_POINTER_AIM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct XemuPointerDisplayRect {
    int x;
    int y;
    int width;
    int height;
} XemuPointerDisplayRect;

typedef struct XemuPointerAimState {
    float filtered_x;
    float filtered_y;
    float filtered_dx;
    float filtered_dy;
    uint64_t timestamp_ns;
    bool valid;
} XemuPointerAimState;

typedef struct XemuPointerAimResult {
    int16_t axis_x;
    int16_t axis_y;
    bool onscreen;
} XemuPointerAimResult;

#ifdef __cplusplus
extern "C" {
#endif

/* Publish/query the game image rectangle in output-window pixels. */
void xemu_pointer_aim_set_display_rect(int x, int y, int width, int height);
bool xemu_pointer_aim_get_display_rect(XemuPointerDisplayRect *rect);

void xemu_pointer_aim_reset(XemuPointerAimState *state);

/*
 * Convert top-left-origin normalized coordinates into Xbox lightgun axes.
 * normalized_x/y normally occupy [0, 1], but values outside that range are
 * accepted and produce a clamped axis with onscreen=false. `active=false`
 * resets the filter and produces a neutral, off-screen result.
 */
void xemu_pointer_aim_update_normalized(
    XemuPointerAimState *state, float normalized_x, float normalized_y,
    bool active, float smoothing, float sensitivity, uint64_t timestamp_ns,
    XemuPointerAimResult *result);

/* DSU touch points use unsigned coordinates in [0, 32767]. */
void xemu_pointer_aim_update_dsu_touch(
    XemuPointerAimState *state, uint16_t touch_x, uint16_t touch_y,
    bool active, float smoothing, float sensitivity, uint64_t timestamp_ns,
    XemuPointerAimResult *result);

/* Convert a host pointer in window pixels to top-left normalized coordinates. */
bool xemu_pointer_aim_normalize_pixels(int pointer_x, int pointer_y,
                                      int fallback_width,
                                      int fallback_height,
                                      float *normalized_x,
                                      float *normalized_y);

#ifdef __cplusplus
}
#endif

#endif
