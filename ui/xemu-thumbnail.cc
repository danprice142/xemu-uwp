/*
 * xemu User Interface
 *
 * Copyright (C) 2020-2022 Matt Borgerson
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

#include <cstdint>
#include <fpng.h>
#include <vector>

#include "hw/xbox/nv2a/nv2a_regs.h"
#include "xemu-snapshots.h"
#include "xui/gl-helpers.hh"
#include "xui/xemu-hud.h"

#if defined(CONFIG_D3D12) && defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_4.h>
#endif

static GLuint display_tex = 0;
static bool display_flip = false;
static NV2ADisplayFrame display_frame;
static bool display_frame_valid = false;

void xemu_snapshots_set_framebuffer_texture(GLuint tex, bool flip)
{
    display_tex = tex;
    display_flip = flip;
}

void xemu_snapshots_set_display_frame(const NV2ADisplayFrame *frame)
{
    if (frame == NULL) {
        memset(&display_frame, 0, sizeof(display_frame));
        display_frame_valid = false;
        return;
    }

    display_frame = *frame;
    display_frame_valid = true;
}

bool xemu_snapshots_load_png_to_texture(GLuint tex, void *buf, size_t size,
                                        int *out_width, int *out_height)
{
#ifdef CONFIG_OPENGL
    std::vector<uint8_t> pixels;
    unsigned int width, height, channels;
    if (fpng::fpng_decode_memory(buf, size, pixels, width, height, channels,
                                 3) != fpng::FPNG_DECODE_SUCCESS) {
        return false;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, pixels.data());

    if (out_width) {
        *out_width = (int)width;
    }
    if (out_height) {
        *out_height = (int)height;
    }

    return true;
#else
    (void)tex;
    (void)buf;
    (void)size;
    (void)out_width;
    (void)out_height;
    return false;
#endif
}

#if defined(CONFIG_D3D12) && defined(_WIN32)
static uint8_t expand_5_to_8(uint16_t v)
{
    return (uint8_t)((v << 3) | (v >> 2));
}

static uint8_t expand_6_to_8(uint16_t v)
{
    return (uint8_t)((v << 2) | (v >> 4));
}

static bool d3d12_thumbnail_encode_rgb(const std::vector<uint8_t> &src_rgb,
                                       uint32_t src_width,
                                       uint32_t src_height, size_t *size,
                                       void **out_buf)
{
    int width = (int)src_width;
    int height = (int)src_height;
    std::vector<uint8_t> scaled;
    std::vector<uint8_t> png;

    if (src_rgb.empty() || src_width == 0 || src_height == 0 ||
        size == NULL || out_buf == NULL) {
        return false;
    }

    ScaleDimensions((int)src_width, (int)src_height,
                    2 * XEMU_SNAPSHOT_THUMBNAIL_WIDTH,
                    2 * XEMU_SNAPSHOT_THUMBNAIL_HEIGHT, &width, &height);
    if (width <= 0 || height <= 0) {
        return false;
    }

    scaled.resize((size_t)width * height * 3);
    for (int y = 0; y < height; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * src_height / height);
        for (int x = 0; x < width; x++) {
            uint32_t sx = (uint32_t)((uint64_t)x * src_width / width);
            const uint8_t *src =
                src_rgb.data() + ((size_t)sy * src_width + sx) * 3;
            uint8_t *dst = scaled.data() + ((size_t)y * width + x) * 3;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
    }

    if (!fpng::fpng_encode_image_to_memory(scaled.data(), (uint32_t)width,
                                           (uint32_t)height, 3, png)) {
        return false;
    }

    *out_buf = g_malloc(png.size());
    memcpy(*out_buf, png.data(), png.size());
    *size = png.size();
    return true;
}

static bool d3d12_cpu_surface_to_rgb(const NV2ADisplayFrameCpuSurface *surface,
                                     std::vector<uint8_t> &rgb,
                                     uint32_t *width, uint32_t *height)
{
    const uint8_t *src;
    uint64_t source_size;

    if (surface == NULL || surface->data == NULL || surface->width == 0 ||
        surface->height == 0 || surface->bytes_per_pixel == 0 ||
        surface->pitch < surface->width * surface->bytes_per_pixel) {
        return false;
    }

    source_size = (uint64_t)(surface->height - 1) * surface->pitch +
                  (uint64_t)surface->width * surface->bytes_per_pixel;
    if (source_size > surface->data_size) {
        return false;
    }

    src = (const uint8_t *)surface->data;
    rgb.resize((size_t)surface->width * surface->height * 3);
    for (uint32_t y = 0; y < surface->height; y++) {
        const uint8_t *src_row = src + (uint64_t)y * surface->pitch;
        uint8_t *dst_row = rgb.data() + (size_t)y * surface->width * 3;

        switch (surface->color_format) {
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
            for (uint32_t x = 0; x < surface->width; x++) {
                const uint8_t *s = src_row + x * 4u;
                uint8_t *d = dst_row + x * 3u;
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
            for (uint32_t x = 0; x < surface->width; x++) {
                uint16_t pixel;
                uint8_t *d = dst_row + x * 3u;
                memcpy(&pixel, src_row + x * 2u, sizeof(pixel));
                d[0] = expand_5_to_8((pixel >> 11) & 0x1f);
                d[1] = expand_6_to_8((pixel >> 5) & 0x3f);
                d[2] = expand_5_to_8(pixel & 0x1f);
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
            for (uint32_t x = 0; x < surface->width; x++) {
                uint16_t pixel;
                uint8_t *d = dst_row + x * 3u;
                memcpy(&pixel, src_row + x * 2u, sizeof(pixel));
                d[0] = expand_5_to_8((pixel >> 10) & 0x1f);
                d[1] = expand_5_to_8((pixel >> 5) & 0x1f);
                d[2] = expand_5_to_8(pixel & 0x1f);
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
            for (uint32_t x = 0; x < surface->width; x++) {
                uint8_t v = src_row[x];
                uint8_t *d = dst_row + x * 3u;
                d[0] = v;
                d[1] = v;
                d[2] = v;
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
            for (uint32_t x = 0; x < surface->width; x++) {
                const uint8_t *s = src_row + x * 2u;
                uint8_t *d = dst_row + x * 3u;
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[0];
            }
            break;

        default:
            return false;
        }
    }

    *width = surface->width;
    *height = surface->height;
    return true;
}

static bool d3d12_readback_to_rgb(ID3D12Resource *readback,
                                  const D3D12_PLACED_SUBRESOURCE_FOOTPRINT
                                      *layout,
                                  DXGI_FORMAT format, uint32_t width,
                                  uint32_t height, std::vector<uint8_t> &rgb)
{
    D3D12_RANGE read_range = {};
    D3D12_RANGE written_range = {};
    uint8_t *mapped = NULL;
    HRESULT hr;

    read_range.Begin = (SIZE_T)layout->Offset;
    read_range.End = (SIZE_T)(layout->Offset +
                              (uint64_t)layout->Footprint.RowPitch * height);
    hr = readback->Map(0, &read_range, (void **)&mapped);
    if (FAILED(hr)) {
        return false;
    }

    rgb.resize((size_t)width * height * 3);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src =
            mapped + layout->Offset + (uint64_t)y * layout->Footprint.RowPitch;
        uint8_t *dst = rgb.data() + (size_t)y * width * 3;

        switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 2];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 0];
            }
            break;

        case DXGI_FORMAT_R8G8B8A8_UNORM:
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
            break;

        case DXGI_FORMAT_B5G6R5_UNORM:
            for (uint32_t x = 0; x < width; x++) {
                uint16_t pixel;
                memcpy(&pixel, src + x * 2u, sizeof(pixel));
                dst[x * 3 + 0] = expand_5_to_8((pixel >> 11) & 0x1f);
                dst[x * 3 + 1] = expand_6_to_8((pixel >> 5) & 0x3f);
                dst[x * 3 + 2] = expand_5_to_8(pixel & 0x1f);
            }
            break;

        case DXGI_FORMAT_B5G5R5A1_UNORM:
            for (uint32_t x = 0; x < width; x++) {
                uint16_t pixel;
                memcpy(&pixel, src + x * 2u, sizeof(pixel));
                dst[x * 3 + 0] = expand_5_to_8((pixel >> 10) & 0x1f);
                dst[x * 3 + 1] = expand_5_to_8((pixel >> 5) & 0x1f);
                dst[x * 3 + 2] = expand_5_to_8(pixel & 0x1f);
            }
            break;

        case DXGI_FORMAT_R8_UNORM:
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x];
                dst[x * 3 + 1] = src[x];
                dst[x * 3 + 2] = src[x];
            }
            break;

        case DXGI_FORMAT_R8G8_UNORM:
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 2 + 0];
                dst[x * 3 + 1] = src[x * 2 + 1];
                dst[x * 3 + 2] = src[x * 2 + 0];
            }
            break;

        default:
            readback->Unmap(0, &written_range);
            return false;
        }
    }

    readback->Unmap(0, &written_range);
    return true;
}

static bool d3d12_resource_to_rgb(const NV2ADisplayFrameD3D12 *frame,
                                  std::vector<uint8_t> &rgb,
                                  uint32_t *out_width,
                                  uint32_t *out_height)
{
    ID3D12Device *device = (ID3D12Device *)frame->device;
    ID3D12CommandQueue *queue = (ID3D12CommandQueue *)frame->command_queue;
    ID3D12Resource *resource = (ID3D12Resource *)frame->resource;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *command_list = NULL;
    ID3D12Resource *readback = NULL;
    ID3D12Fence *fence = NULL;
    HANDLE event = NULL;
    D3D12_RESOURCE_DESC resource_desc = {};
    D3D12_RESOURCE_DESC readback_desc = {};
    D3D12_HEAP_PROPERTIES heap_props = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    D3D12_TEXTURE_COPY_LOCATION src = {};
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    D3D12_BOX src_box = {};
    DXGI_FORMAT format = (DXGI_FORMAT)frame->dxgi_format;
    D3D12_RESOURCE_STATES original_state =
        (D3D12_RESOURCE_STATES)frame->resource_state;
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    uint32_t scale = frame->resource_scale ? frame->resource_scale : 1;
    uint32_t src_x = frame->source_x * scale;
    uint32_t src_y = frame->source_y * scale;
    uint32_t src_w = frame->source_width * scale;
    uint32_t src_h = frame->source_height * scale;
    bool transitioned = false;
    bool ok = false;
    HRESULT hr;

    if (device == NULL || queue == NULL || resource == NULL ||
        src_w == 0 || src_h == 0 || format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    resource_desc = resource->GetDesc();
    if (src_x >= resource_desc.Width || src_y >= resource_desc.Height) {
        return false;
    }
    {
        uint64_t remaining_w = resource_desc.Width - src_x;
        uint32_t remaining_h = (uint32_t)resource_desc.Height - src_y;

        if (src_w > remaining_w) {
            src_w = (uint32_t)remaining_w;
        }
        if (src_h > remaining_h) {
            src_h = remaining_h;
        }
    }
    if (src_w == 0 || src_h == 0) {
        return false;
    }

    resource_desc.Width = src_w;
    resource_desc.Height = src_h;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = format;
    device->GetCopyableFootprints(&resource_desc, 0, 1, 0, &layout,
                                  &num_rows, &row_size, &total_size);
    if (total_size == 0 || num_rows == 0) {
        return false;
    }

    heap_props.Type = D3D12_HEAP_TYPE_READBACK;
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = total_size;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.Format = DXGI_FORMAT_UNKNOWN;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &readback_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        goto cleanup;
    }

    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        goto cleanup;
    }
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   allocator, NULL,
                                   IID_PPV_ARGS(&command_list));
    if (FAILED(hr)) {
        goto cleanup;
    }

    if (original_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = resource;
        barriers[0].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = original_state;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        command_list->ResourceBarrier(1, &barriers[0]);
        transitioned = true;
    }

    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;
    src_box.left = src_x;
    src_box.top = src_y;
    src_box.front = 0;
    src_box.right = src_x + src_w;
    src_box.bottom = src_y + src_h;
    src_box.back = 1;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &src_box);

    if (transitioned) {
        barriers[1] = barriers[0];
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.StateAfter = original_state;
        command_list->ResourceBarrier(1, &barriers[1]);
    }

    hr = command_list->Close();
    if (FAILED(hr)) {
        goto cleanup;
    }
    {
        ID3D12CommandList *lists[] = { command_list };
        queue->ExecuteCommandLists(1, lists);
    }

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                             IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        goto cleanup;
    }
#ifdef CONFIG_UWP
    event = CreateEventExW(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
#else
    event = CreateEvent(NULL, FALSE, FALSE, NULL);
#endif
    if (event == NULL) {
        goto cleanup;
    }
    hr = queue->Signal(fence, 1);
    if (FAILED(hr)) {
        goto cleanup;
    }
    if (fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, event);
        if (FAILED(hr)) {
            goto cleanup;
        }
#ifdef CONFIG_UWP
        if (WaitForSingleObjectEx(event, INFINITE, FALSE) != WAIT_OBJECT_0) {
            goto cleanup;
        }
#else
        if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) {
            goto cleanup;
        }
#endif
    }

    ok = d3d12_readback_to_rgb(readback, &layout, format, src_w, src_h, rgb);
    if (ok) {
        *out_width = src_w;
        *out_height = src_h;
    }

cleanup:
    if (event != NULL) {
        CloseHandle(event);
    }
    if (fence != NULL) {
        fence->Release();
    }
    if (command_list != NULL) {
        command_list->Release();
    }
    if (allocator != NULL) {
        allocator->Release();
    }
    if (readback != NULL) {
        readback->Release();
    }
    return ok;
}

static void *d3d12_create_framebuffer_thumbnail_png(size_t *size)
{
    std::vector<uint8_t> rgb;
    uint32_t width = 0;
    uint32_t height = 0;
    void *buf = NULL;

    if (size == NULL || !display_frame_valid) {
        return NULL;
    }
    *size = 0;

    if (display_frame.type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE) {
        if (!d3d12_resource_to_rgb(&display_frame.d3d12, rgb, &width,
                                   &height)) {
            return NULL;
        }
    } else if (display_frame.type == NV2A_DISPLAY_FRAME_CPU_SURFACE) {
        if (!d3d12_cpu_surface_to_rgb(&display_frame.cpu, rgb, &width,
                                      &height)) {
            return NULL;
        }
    } else {
        return NULL;
    }

    if (!d3d12_thumbnail_encode_rgb(rgb, width, height, size, &buf)) {
        return NULL;
    }

    return buf;
}
#endif

void *xemu_snapshots_create_framebuffer_thumbnail_png(size_t *size)
{
#if defined(CONFIG_D3D12) && defined(_WIN32)
    if (xemu_hud_uses_d3d12()) {
        return d3d12_create_framebuffer_thumbnail_png(size);
    }
#endif

#ifdef CONFIG_OPENGL
    /*
     * Avoids crashing if a snapshot is made on a thread with no GL context
     * Normally, this is not an issue, but it is better to fail safe than assert
     * here.
     * FIXME: Allow for dispatching a thumbnail request to the UI thread to
     * remove this altogether.
     */
    if (!SDL_GL_GetCurrentContext() || display_tex == 0) {
        return NULL;
    }

    std::vector<uint8_t> png;
    if (!RenderFramebufferToPng(display_tex, display_flip, png,
                                2 * XEMU_SNAPSHOT_THUMBNAIL_WIDTH,
                                2 * XEMU_SNAPSHOT_THUMBNAIL_HEIGHT)) {
        return NULL;
    }

    void *buf = g_malloc(png.size());
    memcpy(buf, png.data(), png.size());
    *size = png.size();
    return buf;
#else
    if (size != NULL) {
        *size = 0;
    }
    return NULL;
#endif
}
