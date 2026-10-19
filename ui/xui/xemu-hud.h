/*
 * xemu User Interface
 *
 * Subsystem handling primary graphical user interface, which can be controlled
 * via mouse and keyboard or through any attached gamepad.
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

#ifndef XEMU_HUD_H
#define XEMU_HUD_H

#include <SDL3/SDL.h>
#include <epoxy/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Implemented in xemu.c
int xemu_is_fullscreen(void);
void xemu_toggle_fullscreen(void);
SDL_Window *xemu_get_window(void);
void xemu_eject_disc(Error **errp);
void xemu_load_disc(const char *path, Error **errp);
void xemu_main_loop_lock(void);
void xemu_main_loop_unlock(void);

// Implemented in xemu_hud.cc
void xemu_hud_init(SDL_Window *window, void *sdl_gl_context);
void xemu_hud_init_d3d12(SDL_Window *window, void *device,
                         void *command_queue, void *srv_heap,
                         uint64_t font_srv_cpu, uint64_t font_srv_gpu);
void xemu_hud_cleanup(void);
void xemu_hud_update(void);
void xemu_hud_render(void);
void xemu_hud_render_d3d12(void *command_list);
char *xemu_hud_take_screenshot_path(void);
bool xemu_hud_uses_d3d12(void);
bool xemu_hud_load_d3d12_image_texture(void *buf, size_t size, void **resource,
                                       uint64_t *texture_id, int *width,
                                       int *height);
bool xemu_hud_load_d3d12_png_texture(void *buf, size_t size, void **resource,
                                     uint64_t *texture_id, int *width,
                                     int *height);
bool xemu_hud_get_d3d12_icon_texture(uint64_t *texture_id, int *width,
                                     int *height);
// Creates (when *resource is NULL) or updates a Direct3D 12 HUD texture from
// tightly packed RGBA8 pixels. The returned texture_id is an ImTextureID.
bool xemu_hud_upload_d3d12_rgba_texture(const void *pixels, int width,
                                        int height, void **resource,
                                        uint64_t *texture_id);
void xemu_hud_release_d3d12_texture(void *resource);
void xemu_hud_get_renderer_info(const char **vendor, const char **renderer,
                                const char **version, const char **shader);
void xemu_hud_process_sdl_events(SDL_Event *event);
void xemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse);
void xemu_hud_set_framebuffer_texture(GLuint tex, bool flip);
bool xemu_hud_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif
