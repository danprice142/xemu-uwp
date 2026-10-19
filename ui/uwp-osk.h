/*
 * xemu UWP on-screen keyboard helper
 *
 * Shows/hides the system virtual keyboard (WinRT InputPane) so text fields can
 * be edited with a gamepad on Xbox. No-op on non-UWP builds.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_UWP_OSK_H
#define XEMU_UWP_OSK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Request the on-screen keyboard be shown (visible=true) or hidden. Idempotent;
 * safe to call every frame with ImGui's WantTextInput. */
void xemu_uwp_osk_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

#endif
