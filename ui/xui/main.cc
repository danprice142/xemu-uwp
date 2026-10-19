//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <unordered_map>
#include <vector>
#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <functional>
#include <assert.h>
#include <fpng.h>

#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "actions.hh"
#include "common.hh"
#include "data/xemu_64x64.png.h"
#if defined(CONFIG_D3D12) && defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgiformat.h>
#include <imgui_impl_dx12.h>
#include "ui/d3d12-presenter.h"
#endif
#include "xemu-hud.h"
#include "ui/xemu-dsu.h"
#include "ui/xemu-game-library.h"
#include "ui/xemu-stream.h"
#include "ui/uwp-osk.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "scene.hh"
#include "scene-manager.hh"
#include "main-menu.hh"
#include "popup-menu.hh"
#include "notifications.hh"
#include "monitor.hh"
#include "debug.hh"
#include "welcome.hh"
#include "game-library.hh"
#include "ui/xemu-game-library.h"
#include "menubar.hh"
#include "compat.hh"
#if defined(_WIN32)
#include "update.hh"
#endif

bool g_screenshot_pending;
const char *g_snapshot_pending_load_name;

float g_main_menu_height;

static ImGuiStyle g_base_style;
static float g_last_scale;
static int g_vsync;
static GLuint g_tex;
static bool g_flip_req;

enum class HudRendererBackend {
    None,
    OpenGL,
    D3D12,
};

static HudRendererBackend g_renderer_backend = HudRendererBackend::None;

#if defined(CONFIG_D3D12) && defined(_WIN32)
static constexpr UINT D3D12_HUD_FIRST_USER_SRV_INDEX =
    XEMU_D3D12_PRESENTER_SRV_COUNT;
static constexpr UINT D3D12_HUD_MAX_SRV_COUNT = 1024;
static ID3D12Device *g_d3d12_device;
static ID3D12CommandQueue *g_d3d12_command_queue;
static ID3D12DescriptorHeap *g_d3d12_srv_heap;
static UINT g_d3d12_srv_descriptor_size;
static UINT g_d3d12_next_user_srv_index = D3D12_HUD_FIRST_USER_SRV_INDEX;

/* HUD textures are released only after the frames that may sample them
 * have retired on the GPU; their SRV slots are then reused. */
struct D3D12HudDeferredRelease {
    IUnknown *resource;
    UINT srv_index;
    uint64_t fence_value;
};
static std::vector<D3D12HudDeferredRelease> g_d3d12_deferred_releases;
static std::vector<UINT> g_d3d12_free_srv_indices;
static std::unordered_map<ID3D12Resource *, UINT> g_d3d12_resource_srv_index;
static ID3D12Fence *g_d3d12_release_fence;
static uint64_t g_d3d12_release_fence_value;
static ID3D12Resource *g_d3d12_icon_resource;
static uint64_t g_d3d12_icon_texture_id;
static int g_d3d12_icon_width;
static int g_d3d12_icon_height;
static char g_d3d12_vendor[128];
static char g_d3d12_renderer[256];
static char g_d3d12_version[128];
static char g_d3d12_shader[128];
#endif

static FontRendererBackend GetFontRendererBackend()
{
    switch (g_renderer_backend) {
    case HudRendererBackend::D3D12:
        return FontRendererBackend::D3D12;
    case HudRendererBackend::OpenGL:
    case HudRendererBackend::None:
    default:
        return FontRendererBackend::OpenGL;
    }
}

static void InitializeStyle()
{
    g_font_mgr.Rebuild(GetFontRendererBackend());

    ImGui::StyleColorsDark();
    ImVec4 *c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.98f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 0.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.13f, 0.87f, 0.16f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.25f, 0.75f, 0.10f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.83f, 0.49f, 0.04f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.26f, 0.67f, 0.23f, 0.95f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.21f, 0.54f, 0.19f, 0.99f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.24f, 0.60f, 0.21f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.16f, 0.16f, 0.16f, 0.73f);

    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0;
    s.FrameRounding = 6.0;
    s.PopupRounding = 6.0;
    g_base_style = s;
}

static void InitializeHudContext(bool init_custom_rendering)
{
    xemu_monitor_init();
    g_vsync = g_config.display.window.vsync;

    if (init_custom_rendering) {
        InitCustomRendering();
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
}

static void InitializeHudState()
{
    ImPlot::CreateContext();

#if defined(_WIN32) && !defined(CONFIG_UWP)
    if (!g_config.general.show_welcome && g_config.general.updates.check) {
        update_window.CheckForUpdates();
    }
#endif
    g_last_scale = g_viewport_mgr.m_scale;
    InitializeStyle();
    g_main_menu.SetNextViewIndex(g_config.general.last_viewed_menu_index);
    first_boot_window.is_open = g_config.general.show_welcome;
}

#if defined(CONFIG_D3D12) && defined(_WIN32)
static D3D12_CPU_DESCRIPTOR_HANDLE D3D12HudCpuDescriptor(UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        g_d3d12_srv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)g_d3d12_srv_descriptor_size * index;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE D3D12HudGpuDescriptor(UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle =
        g_d3d12_srv_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (UINT64)g_d3d12_srv_descriptor_size * index;
    return handle;
}

static bool D3D12HudAllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                             D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
    UINT index;

    if (g_d3d12_srv_heap == NULL) {
        return false;
    }
    if (!g_d3d12_free_srv_indices.empty()) {
        index = g_d3d12_free_srv_indices.back();
        g_d3d12_free_srv_indices.pop_back();
    } else if (g_d3d12_next_user_srv_index < D3D12_HUD_MAX_SRV_COUNT) {
        index = g_d3d12_next_user_srv_index++;
    } else {
        return false;
    }
    *cpu = D3D12HudCpuDescriptor(index);
    *gpu = D3D12HudGpuDescriptor(index);
    return true;
}

static void D3D12HudCollectDeferredReleases(bool all)
{
    uint64_t completed = UINT64_MAX;

    if (!all && g_d3d12_release_fence != NULL) {
        completed = g_d3d12_release_fence->GetCompletedValue();
    }
    size_t kept = 0;
    for (size_t i = 0; i < g_d3d12_deferred_releases.size(); i++) {
        D3D12HudDeferredRelease &entry = g_d3d12_deferred_releases[i];
        if (entry.fence_value > completed) {
            g_d3d12_deferred_releases[kept++] = entry;
            continue;
        }
        entry.resource->Release();
        if (entry.srv_index != UINT_MAX && !all) {
            g_d3d12_free_srv_indices.push_back(entry.srv_index);
        }
    }
    g_d3d12_deferred_releases.resize(kept);
}

static void D3D12HudReleaseResources()
{
    D3D12HudCollectDeferredReleases(true);
    g_d3d12_free_srv_indices.clear();
    g_d3d12_resource_srv_index.clear();
    if (g_d3d12_release_fence != NULL) {
        g_d3d12_release_fence->Release();
        g_d3d12_release_fence = NULL;
    }
    g_d3d12_release_fence_value = 0;
    if (g_d3d12_icon_resource != NULL) {
        g_d3d12_icon_resource->Release();
        g_d3d12_icon_resource = NULL;
    }
    g_d3d12_icon_texture_id = 0;
    g_d3d12_icon_width = 0;
    g_d3d12_icon_height = 0;

    if (g_d3d12_srv_heap != NULL) {
        g_d3d12_srv_heap->Release();
        g_d3d12_srv_heap = NULL;
    }
    if (g_d3d12_command_queue != NULL) {
        g_d3d12_command_queue->Release();
        g_d3d12_command_queue = NULL;
    }
    if (g_d3d12_device != NULL) {
        g_d3d12_device->Release();
        g_d3d12_device = NULL;
    }
    g_d3d12_srv_descriptor_size = 0;
    g_d3d12_next_user_srv_index = D3D12_HUD_FIRST_USER_SRV_INDEX;
    g_d3d12_vendor[0] = '\0';
    g_d3d12_renderer[0] = '\0';
    g_d3d12_version[0] = '\0';
    g_d3d12_shader[0] = '\0';
}

static const char *D3D12FeatureLevelName(D3D_FEATURE_LEVEL level)
{
    switch (level) {
    case D3D_FEATURE_LEVEL_12_2:
        return "Direct3D 12 feature level 12_2";
    case D3D_FEATURE_LEVEL_12_1:
        return "Direct3D 12 feature level 12_1";
    case D3D_FEATURE_LEVEL_12_0:
        return "Direct3D 12 feature level 12_0";
    case D3D_FEATURE_LEVEL_11_1:
        return "Direct3D 12 feature level 11_1";
    case D3D_FEATURE_LEVEL_11_0:
    default:
        return "Direct3D 12 feature level 11_0";
    }
}

static const char *D3D12ShaderModelName(D3D_SHADER_MODEL model)
{
    switch (model) {
    case D3D_SHADER_MODEL_6_7:
        return "Shader Model 6.7";
    case D3D_SHADER_MODEL_6_6:
        return "Shader Model 6.6";
    case D3D_SHADER_MODEL_6_5:
        return "Shader Model 6.5";
    case D3D_SHADER_MODEL_6_4:
        return "Shader Model 6.4";
    case D3D_SHADER_MODEL_6_3:
        return "Shader Model 6.3";
    case D3D_SHADER_MODEL_6_2:
        return "Shader Model 6.2";
    case D3D_SHADER_MODEL_6_1:
        return "Shader Model 6.1";
    case D3D_SHADER_MODEL_6_0:
        return "Shader Model 6.0";
    default:
        return "Shader Model 5.1";
    }
}

static void D3D12HudUpdateRendererInfo()
{
    if (g_d3d12_device == NULL || g_d3d12_renderer[0] != '\0') {
        return;
    }

    snprintf(g_d3d12_vendor, sizeof(g_d3d12_vendor), "Direct3D 12");
    snprintf(g_d3d12_renderer, sizeof(g_d3d12_renderer), "Unknown DXGI adapter");
    snprintf(g_d3d12_version, sizeof(g_d3d12_version),
             "Direct3D 12 feature level 11_0");
    snprintf(g_d3d12_shader, sizeof(g_d3d12_shader), "Shader Model 6.4");

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_query = {};
    feature_query.NumFeatureLevels = (UINT)(sizeof(feature_levels) /
                                            sizeof(feature_levels[0]));
    feature_query.pFeatureLevelsRequested = feature_levels;
    if (SUCCEEDED(g_d3d12_device->CheckFeatureSupport(
            D3D12_FEATURE_FEATURE_LEVELS, &feature_query,
            sizeof(feature_query)))) {
        snprintf(g_d3d12_version, sizeof(g_d3d12_version), "%s",
                 D3D12FeatureLevelName(feature_query.MaxSupportedFeatureLevel));
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shader_query = {};
    shader_query.HighestShaderModel = D3D_SHADER_MODEL_6_7;
    if (SUCCEEDED(g_d3d12_device->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL, &shader_query,
            sizeof(shader_query)))) {
        snprintf(g_d3d12_shader, sizeof(g_d3d12_shader), "%s",
                 D3D12ShaderModelName(shader_query.HighestShaderModel));
    }

    IDXGIFactory4 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return;
    }

    LUID device_luid = g_d3d12_device->GetAdapterLuid();
    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *adapter = NULL;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.AdapterLuid.LowPart == device_luid.LowPart &&
            desc.AdapterLuid.HighPart == device_luid.HighPart) {
            char description[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                description, sizeof(description), NULL, NULL);
            snprintf(g_d3d12_vendor, sizeof(g_d3d12_vendor),
                     "DXGI vendor 0x%04x", desc.VendorId);
            snprintf(g_d3d12_renderer, sizeof(g_d3d12_renderer),
                     "%s (vendor=0x%04x device=0x%04x dedicated_vram=%llu MiB)",
                     description[0] ? description : "Unknown DXGI adapter",
                     desc.VendorId, desc.DeviceId,
                     (unsigned long long)(desc.DedicatedVideoMemory /
                                          (1024 * 1024)));
            adapter->Release();
            break;
        }

        adapter->Release();
    }

    factory->Release();
}
#endif

void xemu_hud_init(SDL_Window* window, void* sdl_gl_context)
{
#ifdef CONFIG_OPENGL
    InitializeHudContext(true);

    // Setup Platform/Renderer bindings
    ImGui_ImplSDL3_InitForOpenGL(window, sdl_gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");
    g_renderer_backend = HudRendererBackend::OpenGL;
    InitializeHudState();
#else
    (void)window;
    (void)sdl_gl_context;
#endif
}

void xemu_hud_init_d3d12(SDL_Window *window, void *device,
                         void *command_queue, void *srv_heap,
                         uint64_t font_srv_cpu, uint64_t font_srv_gpu)
{
#if defined(CONFIG_D3D12) && defined(_WIN32)
    InitializeHudContext(false);

    ImGui_ImplSDL3_InitForD3D(window);

    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = (ID3D12Device *)device;
    init_info.CommandQueue = (ID3D12CommandQueue *)command_queue;
    init_info.NumFramesInFlight = 3;
    init_info.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = (ID3D12DescriptorHeap *)srv_heap;
    init_info.LegacySingleSrvCpuDescriptor.ptr = (SIZE_T)font_srv_cpu;
    init_info.LegacySingleSrvGpuDescriptor.ptr = (UINT64)font_srv_gpu;

    D3D12HudReleaseResources();
    g_d3d12_device = (ID3D12Device *)device;
    g_d3d12_command_queue = (ID3D12CommandQueue *)command_queue;
    g_d3d12_srv_heap = (ID3D12DescriptorHeap *)srv_heap;
    g_d3d12_device->AddRef();
    g_d3d12_command_queue->AddRef();
    g_d3d12_srv_heap->AddRef();
    g_d3d12_srv_descriptor_size =
        g_d3d12_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_d3d12_next_user_srv_index = D3D12_HUD_FIRST_USER_SRV_INDEX;

    fprintf(stderr,
            "Direct3D 12 HUD: initializing ImGui DX12 backend "
            "device=%p queue=%p heap=%p font_cpu=0x%" PRIx64
            " font_gpu=0x%" PRIx64 " first_user_srv=%u\n",
            device, command_queue, srv_heap, font_srv_cpu, font_srv_gpu,
            (unsigned)D3D12_HUD_FIRST_USER_SRV_INDEX);
    fflush(stderr);

    if (!ImGui_ImplDX12_Init(&init_info)) {
        fprintf(stderr,
                "Direct3D 12 HUD: ImGui_ImplDX12_Init failed "
                "device=%p queue=%p heap=%p font_cpu=0x%" PRIx64
                " font_gpu=0x%" PRIx64 "\n",
                device, command_queue, srv_heap, font_srv_cpu, font_srv_gpu);
        fflush(stderr);
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        D3D12HudReleaseResources();
        return;
    }

    g_renderer_backend = HudRendererBackend::D3D12;
    InitializeHudState();
    InitCustomRendering();
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        fprintf(stderr,
                "Direct3D 12 HUD: ImGui_ImplDX12_CreateDeviceObjects failed "
                "device=%p queue=%p heap=%p font_cpu=0x%" PRIx64
                " font_gpu=0x%" PRIx64 "\n",
                device, command_queue, srv_heap, font_srv_cpu, font_srv_gpu);
        fflush(stderr);
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        g_renderer_backend = HudRendererBackend::None;
        D3D12HudReleaseResources();
        return;
    }
    fprintf(stderr, "Direct3D 12 HUD: ImGui DX12 backend initialized\n");
    fflush(stderr);
#else
    (void)window;
    (void)device;
    (void)command_queue;
    (void)srv_heap;
    (void)font_srv_cpu;
    (void)font_srv_gpu;
#endif
}

void xemu_hud_cleanup(void)
{
    xemu_stream_shutdown();
    xemu_dsu_shutdown();

    if (g_renderer_backend == HudRendererBackend::None) {
        return;
    }

#if defined(CONFIG_D3D12) && defined(_WIN32)
    if (g_renderer_backend == HudRendererBackend::D3D12) {
        ImGui_ImplDX12_Shutdown();
    }
#endif
#ifdef CONFIG_OPENGL
    if (g_renderer_backend == HudRendererBackend::OpenGL) {
        ImGui_ImplOpenGL3_Shutdown();
    }
#endif
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    g_renderer_backend = HudRendererBackend::None;
#if defined(CONFIG_D3D12) && defined(_WIN32)
    D3D12HudReleaseResources();
#endif
}

bool xemu_hud_is_initialized(void)
{
    return g_renderer_backend != HudRendererBackend::None;
}

void xemu_hud_process_sdl_events(SDL_Event *event)
{
    if (g_renderer_backend == HudRendererBackend::None) {
        return;
    }

    // Ignore inputs that are consumed by rebinding
    if (g_main_menu.ConsumeRebindEvent(event)) {
        return;
    }

    ImGui_ImplSDL3_ProcessEvent(event);
}

void xemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse)
{
    if (g_renderer_backend == HudRendererBackend::None) {
        if (kbd) *kbd = 0;
        if (mouse) *mouse = 0;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (kbd) *kbd = io.WantCaptureKeyboard;
    if (mouse) *mouse = io.WantCaptureMouse;
}

void xemu_hud_set_framebuffer_texture(GLuint tex, bool flip)
{
    g_tex = tex;
    g_flip_req = flip;
}

/*
 * Background shader precompile: progress in the top-right corner while the
 * game runs, and a notification when it completes. The running title poll
 * selects the shader list of games launched outside the library.
 */
static void UpdateShaderPrecompileStatus(bool visible)
{
    static bool tracking;
    NV2AShaderPrecompileStatus status;

    xemu_game_library_refresh_running_title();
    nv2a_shader_precompile_status(&status);

    bool background = status.state == NV2A_SHADER_PRECOMPILE_COMPILING &&
                      !status.foreground;
    if (status.foreground) {
        /* The wait screen shows this run. */
        tracking = false;
    } else if (background) {
        /* Only a game's run seen behind it is announced, not title 0's
         * run at startup. */
        if (runstate_is_running() && status.title_id != 0) {
            tracking = true;
        }
    } else if (status.state == NV2A_SHADER_PRECOMPILE_DONE) {
        if (tracking) {
            g_autofree char *msg = g_strdup_printf(
                "Shaders ready (%u pipelines)", status.done);
            xemu_queue_notification(msg);
        }
        tracking = false;
    } else if (status.state != NV2A_SHADER_PRECOMPILE_COMPILING) {
        tracking = false;
    }

    /* A game without saved shaders stutters as each one is built; say
     * once that the next launch precompiles them. */
    static uint32_t first_run_title;
    if (status.state == NV2A_SHADER_PRECOMPILE_DONE && status.recorded == 0 &&
        status.title_id != 0 && status.title_id != first_run_title &&
        runstate_is_running()) {
        first_run_title = status.title_id;
        xemu_queue_notification("First run: this game's shaders are saved "
                                "as it plays and precompile next launch");
    }

    if (!background || !visible || !g_config.display.ui.show_notifications ||
        !runstate_is_running() || status.total == 0) {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float margin = 10.0f * g_viewport_mgr.m_scale;
    unsigned int percent =
        (status.done + status.failed) * 100 / status.total;

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin,
               viewport->WorkPos.y + margin),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("##shader_precompile", NULL,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoInputs)) {
        ImGui::Text("Compiling shaders %u%%", percent);
    }
    ImGui::End();
}

void xemu_hud_update(void)
{
    if (g_renderer_backend == HudRendererBackend::None) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    uint32_t now = SDL_GetTicks();

    g_viewport_mgr.Update();
    g_font_mgr.Update(GetFontRendererBackend());
    if (g_last_scale != g_viewport_mgr.m_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(g_viewport_mgr.m_scale);
        g_last_scale = g_viewport_mgr.m_scale;
    }

    if (!first_boot_window.is_open) {
        int ww, wh;
        SDL_GetWindowSizeInPixels(xemu_get_window(), &ww, &wh);
#ifdef CONFIG_OPENGL
        if (g_renderer_backend == HudRendererBackend::OpenGL) {
            RenderFramebuffer(g_tex, ww, wh, g_flip_req);
        }
#endif
    }

#if defined(CONFIG_D3D12) && defined(_WIN32)
    if (g_renderer_backend == HudRendererBackend::D3D12) {
        ImGui_ImplDX12_NewFrame();
    } else
#endif
#ifdef CONFIG_OPENGL
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
#endif
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_NewFrame();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_input_mgr.Update();

    ImGui::NewFrame();
    ProcessKeyboardShortcuts();

    // Drive the UWP on-screen keyboard from ImGui's text-input state so text
    // fields are editable with a gamepad.
    xemu_uwp_osk_set_visible(io.WantTextInput);

#if defined(CONFIG_RENDERDOC)
    if (g_capture_renderdoc_frame) {
        nv2a_dbg_renderdoc_capture_frames(1, false);
        g_capture_renderdoc_frame = false;
    }
#endif

    if (xemu_game_library_startup_pending && !first_boot_window.is_open) {
        xemu_game_library_startup_pending = false;
        g_scene_mgr.PushScene(g_game_library);
    }

    if (g_config.display.ui.show_menubar && !first_boot_window.is_open &&
        !g_game_library.IsActive()) {
        // Auto-hide main menu after 5s of inactivity
        static uint32_t last_check = 0;
        float alpha = 1.0;
        const uint32_t timeout = 5000;
        const float fade_duration = 1000.0;
        bool menu_wakeup = g_input_mgr.MouseMoved();
        if (menu_wakeup) {
            last_check = now;
        }
        if ((now-last_check) > timeout) {
            if (g_config.display.ui.use_animations) {
                float t = fmin((float)((now-last_check)-timeout)/fade_duration, 1);
                alpha = 1-t;
                if (t >= 1) {
                    alpha = 0;
                }
            } else {
                alpha = 0;
            }
        }
        if (alpha > 0.0) {
            ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Text];
            tc.w = alpha;
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
            ImGui::SetNextWindowBgAlpha(alpha);
            ShowMainMenu();
            ImGui::PopStyleColor();
        } else {
            g_main_menu_height = 0;
        }
    }

    static uint32_t last_mouse_move = 0;
    if (g_input_mgr.MouseMoved()) {
        last_mouse_move = now;
    }

    // FIXME: Handle time wrap around
    if (g_config.display.ui.hide_cursor && (now - last_mouse_move) > 3000) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
        !g_scene_mgr.IsDisplayingScene()) {

        // If the guide button is pressed, wake the ui
        bool menu_button = false;
        uint32_t buttons = g_input_mgr.CombinedButtons();
        if (buttons & CONTROLLER_BUTTON_GUIDE) {
            menu_button = true;
        }

        // Allow controllers without a guide button to also work
        if ((buttons & CONTROLLER_BUTTON_BACK) &&
            (buttons & CONTROLLER_BUTTON_START)) {
            menu_button = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
            g_scene_mgr.PushScene(g_main_menu);
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (menu_button ||
                   (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    !ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemHovered())) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
#ifndef CONFIG_UWP
            xemu_toggle_fullscreen();
#endif
        }

        bool mod_key_down = ImGui::IsKeyDown(ImGuiKey_ModShift);
        for (int f_key = 0; f_key < 4; ++f_key) {
            if (ImGui::IsKeyPressed((enum ImGuiKey)(ImGuiKey_F5 + f_key))) {
                ActionActivateBoundSnapshot(f_key, mod_key_down);
                break;
            }
        }
    }

    first_boot_window.Draw();
    monitor_window.Draw();
    apu_window.Draw();
    video_window.Draw();
    compatibility_reporter_window.Draw();
#if defined(_WIN32) && !defined(CONFIG_UWP)
    update_window.Draw();
#endif
    g_scene_mgr.Draw();
    if (!first_boot_window.is_open) notification_manager.Draw();
    UpdateShaderPrecompileStatus(!first_boot_window.is_open);
    if (g_config.display.ui.show_fps && !first_boot_window.is_open) {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        float margin = 10.0f * g_viewport_mgr.m_scale;

        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + margin,
                                       viewport->WorkPos.y + margin));
        ImGui::SetNextWindowBgAlpha(0.5f);
        if (ImGui::Begin("##fps_counter", NULL,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoInputs)) {
            if (g_nv2a_stats.show_frame_number) {
                ImGui::Text("%u FPS  #%u", g_nv2a_stats.increment_fps,
                            g_nv2a_stats.snapshot_frame);
            } else {
                ImGui::Text("%u FPS", g_nv2a_stats.increment_fps);
            }
        }
        ImGui::End();
    }
    g_snapshot_mgr.Draw();

    // static bool show_demo = true;
    // if (show_demo) ImGui::ShowDemoWindow(&show_demo);
}

void xemu_hud_render()
{
#ifdef CONFIG_OPENGL
    if (g_renderer_backend != HudRendererBackend::OpenGL) {
        return;
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (g_vsync != g_config.display.window.vsync) {
        g_vsync = g_config.display.window.vsync;
        SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
    }

    if (g_screenshot_pending) {
        SaveScreenshot(g_tex, g_flip_req);
        g_screenshot_pending = false;
    }
#endif
}

void xemu_hud_render_d3d12(void *command_list)
{
#if defined(CONFIG_D3D12) && defined(_WIN32)
    ID3D12GraphicsCommandList *d3d12_command_list =
        (ID3D12GraphicsCommandList *)command_list;

    if (g_renderer_backend != HudRendererBackend::D3D12 ||
        d3d12_command_list == NULL || g_d3d12_srv_heap == NULL) {
        return;
    }

    D3D12HudCollectDeferredReleases(false);
    ImGui::Render();
    {
        ID3D12DescriptorHeap *heaps[] = { g_d3d12_srv_heap };
        d3d12_command_list->SetDescriptorHeaps(1, heaps);
    }
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), d3d12_command_list);

    if (g_vsync != g_config.display.window.vsync) {
        g_vsync = g_config.display.window.vsync;
    }
#else
    (void)command_list;
#endif
}

char *xemu_hud_take_screenshot_path(void)
{
    char fname[128];

    if (!g_screenshot_pending) {
        return NULL;
    }
    g_screenshot_pending = false;

    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp) {
        strftime(fname, sizeof(fname), "xemu-%Y-%m-%d-%H-%M-%S.png", tmp);
    } else {
        strcpy(fname, "xemu.png");
    }

    const char *output_dir = g_config.general.screenshot_dir;
    if (!strlen(output_dir)) {
        output_dir = ".";
    }

    return g_strdup_printf("%s/%s", output_dir, fname);
}

bool xemu_hud_uses_d3d12(void)
{
    return g_renderer_backend == HudRendererBackend::D3D12;
}

void xemu_hud_get_renderer_info(const char **vendor, const char **renderer,
                                const char **version, const char **shader)
{
    static const char *unknown_vendor = "Unknown vendor";
    static const char *unknown_renderer = "Unknown renderer";
    static const char *unknown_version = "Unknown driver";
    static const char *unknown_shader = "Unknown shader model";

#if defined(CONFIG_D3D12) && defined(_WIN32)
    if (g_renderer_backend == HudRendererBackend::D3D12) {
        D3D12HudUpdateRendererInfo();
        if (vendor) {
            *vendor = g_d3d12_vendor[0] ? g_d3d12_vendor : unknown_vendor;
        }
        if (renderer) {
            *renderer = g_d3d12_renderer[0] ? g_d3d12_renderer :
                                               unknown_renderer;
        }
        if (version) {
            *version = g_d3d12_version[0] ? g_d3d12_version :
                                             unknown_version;
        }
        if (shader) {
            *shader = g_d3d12_shader[0] ? g_d3d12_shader : unknown_shader;
        }
        return;
    }
#endif

#ifdef CONFIG_OPENGL
    const char *gl_vendor = (const char *)glGetString(GL_VENDOR);
    const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    const char *gl_shader =
        (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);

    if (vendor) {
        *vendor = gl_vendor ? gl_vendor : unknown_vendor;
    }
    if (renderer) {
        *renderer = gl_renderer ? gl_renderer : unknown_renderer;
    }
    if (version) {
        *version = gl_version ? gl_version : unknown_version;
    }
    if (shader) {
        *shader = gl_shader ? gl_shader : unknown_shader;
    }
#else
    if (vendor) {
        *vendor = unknown_vendor;
    }
    if (renderer) {
        *renderer = unknown_renderer;
    }
    if (version) {
        *version = unknown_version;
    }
    if (shader) {
        *shader = unknown_shader;
    }
#endif
}

void xemu_hud_release_d3d12_texture(void *resource)
{
#if defined(CONFIG_D3D12) && defined(_WIN32)
    ID3D12Resource *texture = (ID3D12Resource *)resource;
    UINT srv_index = UINT_MAX;

    if (texture == NULL) {
        return;
    }
    auto slot = g_d3d12_resource_srv_index.find(texture);
    if (slot != g_d3d12_resource_srv_index.end()) {
        srv_index = slot->second;
        g_d3d12_resource_srv_index.erase(slot);
    }
    if (g_d3d12_device != NULL && g_d3d12_command_queue != NULL) {
        if (g_d3d12_release_fence == NULL &&
            FAILED(g_d3d12_device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&g_d3d12_release_fence)))) {
            g_d3d12_release_fence = NULL;
        }
        if (g_d3d12_release_fence != NULL) {
            uint64_t value = ++g_d3d12_release_fence_value;
            if (SUCCEEDED(g_d3d12_command_queue->Signal(
                    g_d3d12_release_fence, value))) {
                g_d3d12_deferred_releases.push_back(
                    { texture, srv_index, value });
                return;
            }
        }
    }
    texture->Release();
    if (srv_index != UINT_MAX) {
        g_d3d12_free_srv_indices.push_back(srv_index);
    }
#else
    (void)resource;
#endif
}

#if defined(CONFIG_D3D12) && defined(_WIN32)
// Releases objects once the queue passes its current position.
static bool D3D12HudDeferReleases(IUnknown *const *objects, size_t count)
{
    if (g_d3d12_release_fence == NULL &&
        FAILED(g_d3d12_device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12_release_fence)))) {
        g_d3d12_release_fence = NULL;
        return false;
    }
    uint64_t value = ++g_d3d12_release_fence_value;
    if (FAILED(g_d3d12_command_queue->Signal(g_d3d12_release_fence, value))) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        g_d3d12_deferred_releases.push_back({ objects[i], UINT_MAX, value });
    }
    return true;
}

// Creates (when *resource is NULL) or refreshes an RGBA8 shader-resource
// texture. The copy is queued ahead of the frames that sample the texture;
// its transient objects are released once the queue passes it, so the
// caller (holding the main loop lock) does not wait for the GPU.
static bool D3D12HudUploadRgba(const uint8_t *pixels, uint32_t width,
                               uint32_t height, void **resource,
                               uint64_t *texture_id)
{
    if (g_d3d12_device == NULL || g_d3d12_command_queue == NULL ||
        g_d3d12_srv_heap == NULL || pixels == NULL || resource == NULL ||
        texture_id == NULL || width == 0 || height == 0 ||
        width > 16384 || height > 16384) {
        return false;
    }

    ID3D12Resource *texture = (ID3D12Resource *)*resource;
    bool created = false;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = {};

    if (texture != NULL) {
        D3D12_RESOURCE_DESC existing = texture->GetDesc();
        if (existing.Width != width || existing.Height != height) {
            return false;
        }
    } else {
        if (!D3D12HudAllocSrv(&srv_cpu, &srv_gpu)) {
            return false;
        }

        D3D12_RESOURCE_DESC texture_desc = {};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = width;
        texture_desc.Height = height;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES heap_props = {};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        HRESULT hr = g_d3d12_device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &texture_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&texture));
        if (FAILED(hr)) {
            return false;
        }
        created = true;
    }

    UINT upload_pitch =
        (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    UINT64 upload_size = (UINT64)upload_pitch * height;

    D3D12_RESOURCE_DESC upload_desc = {};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

    ID3D12Resource *upload = NULL;
    HRESULT hr = g_d3d12_device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&upload));
    if (FAILED(hr)) {
        if (created) {
            texture->Release();
        }
        return false;
    }

    void *mapped = NULL;
    D3D12_RANGE read_range = { 0, 0 };
    hr = upload->Map(0, &read_range, &mapped);
    if (FAILED(hr)) {
        upload->Release();
        if (created) {
            texture->Release();
        }
        return false;
    }

    for (uint32_t y = 0; y < height; y++) {
        memcpy((uint8_t *)mapped + (uint64_t)y * upload_pitch,
               pixels + (uint64_t)y * width * 4, width * 4);
    }
    upload->Unmap(0, NULL);

    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *command_list = NULL;
    ID3D12Fence *fence = NULL;
    HANDLE event = NULL;
    bool ok = false;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    D3D12_RESOURCE_BARRIER barrier = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};

    hr = g_d3d12_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        goto cleanup;
    }

    hr = g_d3d12_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        IID_PPV_ARGS(&command_list));
    if (FAILED(hr)) {
        goto cleanup;
    }

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!created) {
        // Existing texture: it was left readable by the previous upload.
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        command_list->ResourceBarrier(1, &barrier);
    }

    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = upload_pitch;

    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1, &barrier);

    hr = command_list->Close();
    if (FAILED(hr)) {
        goto cleanup;
    }

    {
        ID3D12CommandList *command_lists[] = { command_list };
        g_d3d12_command_queue->ExecuteCommandLists(1, command_lists);
    }

    {
        IUnknown *transient[] = { upload, command_list, allocator };

        if (D3D12HudDeferReleases(transient, 3)) {
            upload = NULL;
            command_list = NULL;
            allocator = NULL;
            goto publish;
        }
    }

    hr = g_d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
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

    hr = g_d3d12_command_queue->Signal(fence, 1);
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

publish:
    if (created) {
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        g_d3d12_device->CreateShaderResourceView(texture, &srv_desc, srv_cpu);
        g_d3d12_resource_srv_index[texture] = (UINT)(
            (srv_gpu.ptr -
             g_d3d12_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr) /
            g_d3d12_srv_descriptor_size);
        *resource = texture;
        *texture_id = (uint64_t)srv_gpu.ptr;
    }
    ok = true;

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
    if (upload != NULL) {
        upload->Release();
    }
    if (!ok && created) {
        texture->Release();
    }
    return ok;
}
#endif

bool xemu_hud_upload_d3d12_rgba_texture(const void *pixels, int width,
                                        int height, void **resource,
                                        uint64_t *texture_id)
{
#if defined(CONFIG_D3D12) && defined(_WIN32)
    if (g_renderer_backend != HudRendererBackend::D3D12 || width <= 0 ||
        height <= 0) {
        return false;
    }
    return D3D12HudUploadRgba((const uint8_t *)pixels, (uint32_t)width,
                              (uint32_t)height, resource, texture_id);
#else
    (void)pixels;
    (void)width;
    (void)height;
    (void)resource;
    (void)texture_id;
    return false;
#endif
}

bool xemu_hud_load_d3d12_image_texture(void *buf, size_t size, void **resource,
                                       uint64_t *texture_id, int *out_width,
                                       int *out_height)
{
#if defined(CONFIG_D3D12) && defined(_WIN32)
    if (g_renderer_backend != HudRendererBackend::D3D12 ||
        g_d3d12_device == NULL || g_d3d12_command_queue == NULL ||
        g_d3d12_srv_heap == NULL || buf == NULL || size == 0 ||
        resource == NULL || texture_id == NULL) {
        return false;
    }

    if (*resource != NULL && *texture_id != 0) {
        return true;
    }

    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    if (size <= UINT32_MAX &&
        fpng::fpng_decode_memory(buf, (uint32_t)size, pixels, width, height,
                                 channels, 4) == fpng::FPNG_DECODE_SUCCESS) {
        /* Fast path for xemu's own FPNG screenshots. */
        if (width == 0 || height == 0 || width > 16384 || height > 16384) {
            return false;
        }
    } else {
        if (size > INT_MAX) {
            return false;
        }
        int decoded_width = 0;
        int decoded_height = 0;
        int decoded_channels = 0;
        stbi_set_flip_vertically_on_load(0);
        stbi_uc *decoded = stbi_load_from_memory(
            (const stbi_uc *)buf, (int)size, &decoded_width, &decoded_height,
            &decoded_channels, 4);
        if (decoded == NULL || decoded_width <= 0 || decoded_height <= 0 ||
            decoded_width > 16384 || decoded_height > 16384) {
            stbi_image_free(decoded);
            return false;
        }
        width = decoded_width;
        height = decoded_height;
        channels = 4;
        pixels.assign(decoded,
                      decoded + (size_t)decoded_width * decoded_height * 4);
        stbi_image_free(decoded);
    }

    if (!D3D12HudUploadRgba(pixels.data(), width, height, resource,
                            texture_id)) {
        return false;
    }
    if (out_width) {
        *out_width = (int)width;
    }
    if (out_height) {
        *out_height = (int)height;
    }
    return true;
#else
    (void)buf;
    (void)size;
    (void)resource;
    (void)texture_id;
    (void)out_width;
    (void)out_height;
    return false;
#endif
}

bool xemu_hud_load_d3d12_png_texture(void *buf, size_t size, void **resource,
                                     uint64_t *texture_id, int *out_width,
                                     int *out_height)
{
    return xemu_hud_load_d3d12_image_texture(
        buf, size, resource, texture_id, out_width, out_height);
}

bool xemu_hud_get_d3d12_icon_texture(uint64_t *texture_id, int *width,
                                     int *height)
{
#if defined(CONFIG_D3D12) && defined(_WIN32)
    if (g_d3d12_icon_resource == NULL || g_d3d12_icon_texture_id == 0) {
        void *resource = NULL;
        uint64_t id = 0;
        if (!xemu_hud_load_d3d12_png_texture(
                (void *)xemu_64x64_data, xemu_64x64_size, &resource, &id,
                &g_d3d12_icon_width, &g_d3d12_icon_height)) {
            return false;
        }
        g_d3d12_icon_resource = (ID3D12Resource *)resource;
        g_d3d12_icon_texture_id = id;
    }

    if (texture_id) {
        *texture_id = g_d3d12_icon_texture_id;
    }
    if (width) {
        *width = g_d3d12_icon_width;
    }
    if (height) {
        *height = g_d3d12_icon_height;
    }
    return true;
#else
    (void)texture_id;
    (void)width;
    (void)height;
    return false;
#endif
}
