//
// xemu User Interface
//
// No-OpenGL helper implementation for D3D12-only builds.
//
// The controller, controller port and XMU previews use the same mask
// textures and two-colour decal composition as the OpenGL HUD, rendered here
// by a small software rasterizer into an RGBA canvas that is uploaded to a
// Direct3D 12 texture for ImGui.
//

#include "gl-helpers.hh"
#include "xemu-hud.h"
#include "data/controller_mask.png.h"
#include "data/controller_mask_s.png.h"
#include "data/xmu_mask.png.h"
#include "stb_image.h"

#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

Fbo *controller_fbo;
Fbo *xmu_fbo;
Fbo *logo_fbo;
GLuint g_icon_tex;

GLint Fbo::vp[4];
GLint Fbo::original_fbo;
bool Fbo::blend;

namespace {

// Decoded mask texture. Rows are stored bottom-up to match the OpenGL
// texture convention used by the decal coordinates in tex_items.
struct SoftTexture {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// Render target: float RGBA canvas (bottom-up rows) plus the D3D12 texture
// that mirrors it.
struct SoftCanvas {
    int w = 0, h = 0;
    std::vector<float> rgba;
    std::vector<uint8_t> uploaded;
    bool dirty = true;
    void *resource = nullptr;
    uint64_t texture_id = 0;
};

enum class BlendMode {
    Overwrite,              // glBlendFunc(GL_ONE, GL_ZERO)
    OneMinusDstAlphaOne,    // glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE)
    SrcAlphaOneMinusSrcAlpha // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
};

struct rect {
    int x, y, w, h;
};

static const struct rect tex_items[] = {
    { 0, 148, 467, 364 }, // obj_controller
    { 0, 81, 67, 67 }, // obj_lstick
    { 0, 14, 67, 67 }, // obj_rstick
    { 67, 104, 68, 44 }, // obj_port_socket
    { 67, 76, 28, 28 }, // obj_port_lbl_1
    { 67, 48, 28, 28 }, // obj_port_lbl_2
    { 67, 20, 28, 28 }, // obj_port_lbl_3
    { 95, 76, 28, 28 }, // obj_port_lbl_4
    { 0, 0, 512, 512 } // obj_xmu
};

enum tex_item_names {
    obj_controller,
    obj_lstick,
    obj_rstick,
    obj_port_socket,
    obj_port_lbl_1,
    obj_port_lbl_2,
    obj_port_lbl_3,
    obj_port_lbl_4,
    obj_xmu
};

SoftTexture g_controller_duke_mask;
SoftTexture g_controller_s_mask;
SoftTexture g_xmu_mask;
SoftCanvas *g_current_canvas;
const SoftTexture *g_current_texture;
BlendMode g_blend_mode = BlendMode::Overwrite;
bool g_custom_rendering_initialized;

bool LoadSoftTexture(const unsigned char *buf, unsigned int size,
                     SoftTexture *texture)
{
    // Flip vertically so texel rows match the OpenGL convention.
    stbi_set_flip_vertically_on_load(1);
    int width = 0, height = 0, channels = 0;
    unsigned char *data =
        stbi_load_from_memory(buf, (int)size, &width, &height, &channels, 4);
    stbi_set_flip_vertically_on_load(0);
    if (data == NULL || width <= 0 || height <= 0) {
        stbi_image_free(data);
        return false;
    }
    texture->w = width;
    texture->h = height;
    texture->rgba.assign(data, data + (size_t)width * height * 4);
    stbi_image_free(data);
    return true;
}

inline float ClampUnit(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Bilinear sample with GL_CLAMP_TO_BORDER semantics (transparent black
// border). Coordinates are normalized texture coordinates.
void SampleTexture(const SoftTexture *tex, float u, float v, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (tex == NULL || tex->w == 0 || tex->h == 0) {
        return;
    }
    float px = u * tex->w - 0.5f;
    float py = v * tex->h - 0.5f;
    int x0 = (int)floorf(px);
    int y0 = (int)floorf(py);
    float fx = px - x0;
    float fy = py - y0;
    auto fetch = [&](int x, int y, float weight) {
        if (weight <= 0.0f || x < 0 || y < 0 || x >= tex->w || y >= tex->h) {
            return;
        }
        const uint8_t *p = &tex->rgba[((size_t)y * tex->w + x) * 4];
        out[0] += p[0] * (weight / 255.0f);
        out[1] += p[1] * (weight / 255.0f);
        out[2] += p[2] * (weight / 255.0f);
        out[3] += p[3] * (weight / 255.0f);
    };
    fetch(x0, y0, (1 - fx) * (1 - fy));
    fetch(x0 + 1, y0, fx * (1 - fy));
    fetch(x0, y0 + 1, (1 - fx) * fy);
    fetch(x0 + 1, y0 + 1, fx * fy);
}

// Software version of the OpenGL "mask" decal shader + fixed-function blend:
//   out = fill; out.rgb += mix(secondary, primary, t.r); out.a += t.a - t.b
void RenderDecal(float x, float y, float w, float h, float tex_x,
                 float tex_y, float tex_w, float tex_h, uint32_t primary,
                 uint32_t secondary, uint32_t fill)
{
    SoftCanvas *canvas = g_current_canvas;
    const SoftTexture *tex = g_current_texture;
    if (canvas == NULL || tex == NULL) {
        return;
    }

    int ix = (int)x, iy = (int)y, iw = (int)w, ih = (int)h;
    int itx = (int)tex_x, ity = (int)tex_y, itw = (int)tex_w,
        ith = (int)tex_h;
    if (iw <= 0 || ih <= 0) {
        return;
    }

#define COL(color, c) ((float)(((color) >> ((c) * 8)) & 0xff) / 255.0f)
    float prim[4] = { COL(primary, 3), COL(primary, 2), COL(primary, 1),
                      COL(primary, 0) };
    float sec[4] = { COL(secondary, 3), COL(secondary, 2), COL(secondary, 1),
                     COL(secondary, 0) };
    float fil[4] = { COL(fill, 3), COL(fill, 2), COL(fill, 1), COL(fill, 0) };
#undef COL

    float tw = (float)tex->w, th = (float)tex->h;

    int x_start = ix < 0 ? 0 : ix;
    int y_start = iy < 0 ? 0 : iy;
    int x_end = (ix + iw) > canvas->w ? canvas->w : (ix + iw);
    int y_end = (iy + ih) > canvas->h ? canvas->h : (iy + ih);

    for (int py = y_start; py < y_end; py++) {
        float v = ((py - iy) + 0.5f) / (float)ih;
        float tv = (ity + v * ith) / th;
        float *row = &canvas->rgba[(size_t)py * canvas->w * 4];
        for (int px = x_start; px < x_end; px++) {
            float u = ((px - ix) + 0.5f) / (float)iw;
            float tu = (itx + u * itw) / tw;
            float t[4];
            SampleTexture(tex, tu, tv, t);

            float src[4];
            src[0] = fil[0] + sec[0] + (prim[0] - sec[0]) * t[0];
            src[1] = fil[1] + sec[1] + (prim[1] - sec[1]) * t[0];
            src[2] = fil[2] + sec[2] + (prim[2] - sec[2]) * t[0];
            src[3] = fil[3] + t[3] - t[2];
            for (float &c : src) {
                c = ClampUnit(c);
            }

            float *dst = &row[(size_t)px * 4];
            switch (g_blend_mode) {
            case BlendMode::Overwrite:
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
                break;
            case BlendMode::OneMinusDstAlphaOne: {
                float f = 1.0f - dst[3];
                dst[0] = ClampUnit(src[0] * f + dst[0]);
                dst[1] = ClampUnit(src[1] * f + dst[1]);
                dst[2] = ClampUnit(src[2] * f + dst[2]);
                dst[3] = ClampUnit(src[3] * f + dst[3]);
                break;
            }
            case BlendMode::SrcAlphaOneMinusSrcAlpha: {
                float a = src[3];
                dst[0] = ClampUnit(src[0] * a + dst[0] * (1.0f - a));
                dst[1] = ClampUnit(src[1] * a + dst[1] * (1.0f - a));
                dst[2] = ClampUnit(src[2] * a + dst[2] * (1.0f - a));
                dst[3] = ClampUnit(src[3] * a + dst[3] * (1.0f - a));
                break;
            }
            }
        }
    }
    canvas->dirty = true;
}

void RenderMeter(float x, float y, float width, float height, float p,
                 uint32_t color_bg, uint32_t color_fg)
{
    RenderDecal(x, y, width, height, 0, 0, 1, 1, 0, 0, color_bg);
    RenderDecal(x, y, width * p, height, 0, 0, 1, 1, 0, 0, color_fg);
}

// Shared body of the Duke and Controller S previews. Mirrors gl-helpers.cc.
void RenderMaskedController(const SoftTexture *mask, const struct rect &jewel,
                            const struct rect &lstick_ctr,
                            const struct rect &rstick_ctr,
                            const struct rect buttons[12], float frame_x,
                            float frame_y, uint32_t primary_color,
                            uint32_t secondary_color, ControllerState *state)
{
    uint8_t alpha = 0;
    uint32_t now = SDL_GetTicks();
    float t;

    g_current_texture = mask;

    // A 5 pixel margin around the controller leaves room to wiggle it
    // while rumble is active
    frame_x += 5;
    frame_y += 5;
    float original_frame_x = frame_x;
    float original_frame_y = frame_y;

    float rumble_l = 0;
    float rumble_r = 0;

    g_blend_mode = BlendMode::Overwrite;

    uint32_t jewel_color = secondary_color;

    const uint32_t animate_guide_button_duration = 2000;
    if (state->buttons & CONTROLLER_BUTTON_GUIDE) {
        state->animate_guide_button_end = now + animate_guide_button_duration;
    }

    if (now < state->animate_guide_button_end) {
        t = 1.0f - (float)(state->animate_guide_button_end - now) /
                       (float)animate_guide_button_duration;
        float sin_wav = (1 - sin(M_PI * t / 2.0f));

        alpha = sin_wav * 255.0f;
        jewel_color = primary_color + alpha;

        frame_x += ((float)(rand() % 5) - 2.5) * (1 - t);
        frame_y += ((float)(rand() % 5) - 2.5) * (1 - t);
        rumble_l = rumble_r = sin_wav;
    }

    // Render controller texture
    RenderDecal(frame_x + 0, frame_y + 0, tex_items[obj_controller].w,
                tex_items[obj_controller].h, tex_items[obj_controller].x,
                tex_items[obj_controller].y, tex_items[obj_controller].w,
                tex_items[obj_controller].h, primary_color, secondary_color,
                0);

    g_blend_mode = BlendMode::OneMinusDstAlphaOne; // Blend with cutouts
    RenderDecal(frame_x + jewel.x, frame_y + jewel.y, jewel.w, jewel.h, 0, 0,
                1, 1, 0, 0, jewel_color);

    // The controller has alpha cutouts where the buttons are. Draw a surface
    // behind the buttons if they are activated
    for (int i = 0; i < 12; i++) {
        if (state->buttons & (1 << i)) {
            RenderDecal(frame_x + buttons[i].x, frame_y + buttons[i].y,
                        buttons[i].w, buttons[i].h, 0, 0, 1, 1, 0, 0,
                        primary_color + 0xff);
        }
    }

    g_blend_mode = BlendMode::SrcAlphaOneMinusSrcAlpha;

    // Render left thumbstick
    float w = tex_items[obj_lstick].w;
    float h = tex_items[obj_lstick].h;
    float c_x = frame_x + lstick_ctr.x;
    float c_y = frame_y + lstick_ctr.y;
    float lstick_x = (float)state->axis[CONTROLLER_AXIS_LSTICK_X] / 32768.0;
    float lstick_y = (float)state->axis[CONTROLLER_AXIS_LSTICK_Y] / 32768.0;
    RenderDecal((int)(c_x - w / 2.0f + 10.0f * lstick_x),
                (int)(c_y - h / 2.0f + 10.0f * lstick_y), w, h,
                tex_items[obj_lstick].x, tex_items[obj_lstick].y, w, h,
                (state->buttons & CONTROLLER_BUTTON_LSTICK) ? secondary_color :
                                                              primary_color,
                (state->buttons & CONTROLLER_BUTTON_LSTICK) ? primary_color :
                                                              secondary_color,
                0);

    // Render right thumbstick
    w = tex_items[obj_rstick].w;
    h = tex_items[obj_rstick].h;
    c_x = frame_x + rstick_ctr.x;
    c_y = frame_y + rstick_ctr.y;
    float rstick_x = (float)state->axis[CONTROLLER_AXIS_RSTICK_X] / 32768.0;
    float rstick_y = (float)state->axis[CONTROLLER_AXIS_RSTICK_Y] / 32768.0;
    RenderDecal((int)(c_x - w / 2.0f + 10.0f * rstick_x),
                (int)(c_y - h / 2.0f + 10.0f * rstick_y), w, h,
                tex_items[obj_rstick].x, tex_items[obj_rstick].y, w, h,
                (state->buttons & CONTROLLER_BUTTON_RSTICK) ? secondary_color :
                                                              primary_color,
                (state->buttons & CONTROLLER_BUTTON_RSTICK) ? primary_color :
                                                              secondary_color,
                0);

    g_blend_mode = BlendMode::Overwrite;

    // Render trigger bars
    float ltrig = state->axis[CONTROLLER_AXIS_LTRIG] / 32767.0;
    float rtrig = state->axis[CONTROLLER_AXIS_RTRIG] / 32767.0;
    const uint32_t animate_trigger_duration = 1000;
    if ((ltrig > 0) || (rtrig > 0)) {
        state->animate_trigger_end = now + animate_trigger_duration;
        rumble_l = fmax(rumble_l, ltrig);
        rumble_r = fmax(rumble_r, rtrig);
    }

    // Animate trigger alpha down after a period of inactivity
    alpha = 0x80;
    if (state->animate_trigger_end > now) {
        t = 1.0f - (float)(state->animate_trigger_end - now) /
                       (float)animate_trigger_duration;
        float sin_wav = (1 - sin(M_PI * t / 2.0f));
        alpha += fmin(sin_wav * 0x40, 0x80);
    }

    RenderMeter(original_frame_x + 10,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                ltrig, primary_color + alpha, primary_color + 0xff);
    RenderMeter(original_frame_x + tex_items[obj_controller].w - 160,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                rtrig, primary_color + alpha, primary_color + 0xff);

    // Apply rumble updates
    state->rumble_l = (int)(rumble_l * (float)0xffff);
    state->rumble_r = (int)(rumble_r * (float)0xffff);
}

void RenderDukeController(float frame_x, float frame_y,
                          uint32_t primary_color, uint32_t secondary_color,
                          ControllerState *state)
{
    const struct rect jewel = { 177, 172, 113, 118 };
    const struct rect lstick_ctr = { 93, 246, 0, 0 };
    const struct rect rstick_ctr = { 342, 148, 0, 0 };
    const struct rect buttons[12] = {
        { 367, 187, 30, 38 }, // A
        { 368, 229, 30, 38 }, // B
        { 330, 204, 30, 38 }, // X
        { 331, 247, 30, 38 }, // Y
        { 82, 121, 31, 47 }, // D-Left
        { 104, 160, 44, 25 }, // D-Up
        { 141, 121, 31, 47 }, // D-Right
        { 104, 105, 44, 25 }, // D-Down
        { 187, 94, 34, 24 }, // Back
        { 246, 94, 36, 26 }, // Start
        { 348, 288, 30, 38 }, // White
        { 386, 268, 30, 38 }, // Black
    };
    RenderMaskedController(&g_controller_duke_mask, jewel, lstick_ctr,
                           rstick_ctr, buttons, frame_x, frame_y,
                           primary_color, secondary_color, state);
}

void RenderControllerS(float frame_x, float frame_y, uint32_t primary_color,
                       uint32_t secondary_color, ControllerState *state)
{
    const struct rect jewel = { 194, 213, 84, 84 };
    const struct rect lstick_ctr = { 103, 254, 0, 0 };
    const struct rect rstick_ctr = { 295, 176, 0, 0 };
    const struct rect buttons[12] = {
        { 347, 200, 34, 34 }, // A
        { 381, 235, 34, 34 }, // B
        { 313, 235, 34, 34 }, // X
        { 347, 270, 34, 34 }, // Y
        { 123, 165, 31, 26 }, // D-Left
        { 150, 187, 26, 31 }, // D-Up
        { 173, 165, 31, 26 }, // D-Right
        { 150, 135, 26, 31 }, // D-Down
        { 45, 195, 20, 24 }, // Back
        { 70, 163, 26, 26 }, // Start
        { 352, 145, 30, 30 }, // White
        { 388, 172, 30, 30 }, // Black
    };
    RenderMaskedController(&g_controller_s_mask, jewel, lstick_ctr,
                           rstick_ctr, buttons, frame_x, frame_y,
                           primary_color, secondary_color, state);
}

} // namespace

Fbo::Fbo(int width, int height)
{
    w = width;
    h = height;
    fbo = 0;
    tex = 0;
    SoftCanvas *canvas = new SoftCanvas;
    canvas->w = width;
    canvas->h = height;
    canvas->rgba.assign((size_t)width * height * 4, 0.0f);
    impl = canvas;
}

Fbo::~Fbo()
{
    SoftCanvas *canvas = (SoftCanvas *)impl;
    if (canvas != NULL) {
        xemu_hud_release_d3d12_texture(canvas->resource);
        delete canvas;
    }
    impl = NULL;
}

void Fbo::Target()
{
    SoftCanvas *canvas = (SoftCanvas *)impl;
    if (canvas == NULL) {
        return;
    }
    // Equivalent of binding the FBO and clearing it to transparent black
    std::fill(canvas->rgba.begin(), canvas->rgba.end(), 0.0f);
    canvas->dirty = true;
    g_current_canvas = canvas;
    g_blend_mode = BlendMode::Overwrite;
}

// Push the composed canvas to the GPU texture. Uploads are synchronous, so
// skip them whenever the image is unchanged (idle controller preview).
static void FlushCanvas(SoftCanvas *canvas)
{
    if (canvas == NULL || !canvas->dirty) {
        return;
    }
    std::vector<uint8_t> pixels((size_t)canvas->w * canvas->h * 4);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = (uint8_t)(ClampUnit(canvas->rgba[i]) * 255.0f + 0.5f);
    }
    if (pixels != canvas->uploaded || canvas->resource == NULL) {
        if (xemu_hud_upload_d3d12_rgba_texture(pixels.data(), canvas->w,
                                               canvas->h, &canvas->resource,
                                               &canvas->texture_id)) {
            canvas->uploaded = std::move(pixels);
        }
    }
    canvas->dirty = false;
}

void Fbo::Restore()
{
    // Equivalent of unbinding the FBO: the frame's drawing is complete, so
    // publish it before ImGui's draw data is rendered.
    FlushCanvas((SoftCanvas *)impl);
    if (g_current_canvas == impl) {
        g_current_canvas = NULL;
    }
}

uint64_t Fbo::Texture()
{
    SoftCanvas *canvas = (SoftCanvas *)impl;
    if (canvas == NULL) {
        return 0;
    }
    if (canvas->resource == NULL) {
        // First use: create the texture now so the ImGui draw command gets a
        // valid identifier; Restore() uploads the finished contents.
        FlushCanvas(canvas);
        canvas->dirty = true;
    }
    return canvas->texture_id;
}

void InitCustomRendering(void)
{
    delete controller_fbo;
    controller_fbo = NULL;
    delete xmu_fbo;
    xmu_fbo = NULL;

    bool ok = LoadSoftTexture(controller_mask_data, controller_mask_size,
                              &g_controller_duke_mask);
    ok = LoadSoftTexture(controller_mask_s_data, controller_mask_s_size,
                         &g_controller_s_mask) && ok;
    ok = LoadSoftTexture(xmu_mask_data, xmu_mask_size, &g_xmu_mask) && ok;
    if (!ok) {
        fprintf(stderr, "xemu HUD: failed to decode controller mask images\n");
        return;
    }

    controller_fbo = new Fbo(512, 512);
    xmu_fbo = new Fbo(512, 256);
    g_custom_rendering_initialized = true;
}

void RenderLogo(uint32_t time)
{
    (void)time;
}

void RenderController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state)
{
    if (!g_custom_rendering_initialized || g_current_canvas == NULL ||
        state == NULL) {
        return;
    }

    if (strcmp(bound_drivers[state->bound], DRIVER_S) == 0) {
        RenderControllerS(frame_x, frame_y, primary_color, secondary_color,
                          state);
    } else if (strcmp(bound_drivers[state->bound], DRIVER_DUKE) == 0) {
        RenderDukeController(frame_x, frame_y, primary_color, secondary_color,
                             state);
    }
}

void RenderControllerPort(float frame_x, float frame_y, int i,
                          uint32_t port_color)
{
    if (!g_custom_rendering_initialized || g_current_canvas == NULL) {
        return;
    }

    g_current_texture = &g_controller_duke_mask;
    g_blend_mode = BlendMode::Overwrite;

    // Render port socket
    RenderDecal(frame_x, frame_y, tex_items[obj_port_socket].w,
                tex_items[obj_port_socket].h, tex_items[obj_port_socket].x,
                tex_items[obj_port_socket].y, tex_items[obj_port_socket].w,
                tex_items[obj_port_socket].h, port_color, port_color, 0);

    frame_x += (tex_items[obj_port_socket].w - tex_items[obj_port_lbl_1].w) / 2;
    frame_y += tex_items[obj_port_socket].h + 8;

    // Render port label
    RenderDecal(frame_x, frame_y, tex_items[obj_port_lbl_1 + i].w,
                tex_items[obj_port_lbl_1 + i].h, tex_items[obj_port_lbl_1 + i].x,
                tex_items[obj_port_lbl_1 + i].y, tex_items[obj_port_lbl_1 + i].w,
                tex_items[obj_port_lbl_1 + i].h, port_color, port_color, 0);
}

void RenderXmu(float frame_x, float frame_y, uint32_t primary_color,
               uint32_t secondary_color)
{
    if (!g_custom_rendering_initialized || g_current_canvas == NULL) {
        return;
    }

    g_current_texture = &g_xmu_mask;
    g_blend_mode = BlendMode::Overwrite;

    RenderDecal(frame_x, frame_y, 256, 256, tex_items[obj_xmu].x,
                tex_items[obj_xmu].y, tex_items[obj_xmu].w,
                tex_items[obj_xmu].h, primary_color, secondary_color, 0);
}

void RenderFramebuffer(GLint tex, int width, int height, bool flip)
{
    (void)tex;
    (void)width;
    (void)height;
    (void)flip;
}

void RenderFramebuffer(GLint tex, int width, int height, bool flip,
                       float scale[2])
{
    (void)tex;
    (void)width;
    (void)height;
    (void)flip;
    (void)scale;
}

bool RenderFramebufferToPng(GLuint tex, bool flip, std::vector<uint8_t> &png,
                            int max_width, int max_height)
{
    (void)tex;
    (void)flip;
    (void)png;
    (void)max_width;
    (void)max_height;
    return false;
}

void SaveScreenshot(GLuint tex, bool flip)
{
    (void)tex;
    (void)flip;
}

// Scale <src> proportionally to fit in <max>
void ScaleDimensions(int src_width, int src_height, int max_width,
                     int max_height, int *out_width, int *out_height)
{
    if (src_width <= 0 || src_height <= 0 || max_height <= 0) {
        *out_width = 0;
        *out_height = 0;
        return;
    }
    float w_ratio = (float)max_width / (float)max_height;
    float t_ratio = (float)src_width / (float)src_height;

    if (w_ratio >= t_ratio) {
        *out_width = (float)max_width * t_ratio / w_ratio;
        *out_height = max_height;
    } else {
        *out_width = max_width;
        *out_height = (float)max_height * w_ratio / t_ratio;
    }
}
