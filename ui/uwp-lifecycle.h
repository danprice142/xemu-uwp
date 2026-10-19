/*
 * xemu UWP lifecycle helpers
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_UWP_LIFECYCLE_H
#define XEMU_UWP_LIFECYCLE_H

#include <SDL3/SDL_events.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_uwp_lifecycle_init(void);
void xemu_uwp_lifecycle_shutdown(void);
void xemu_uwp_lifecycle_handle_sdl_event(const SDL_Event *event);
bool xemu_uwp_lifecycle_is_in_background(void);

#ifdef __cplusplus
}
#endif

#endif
