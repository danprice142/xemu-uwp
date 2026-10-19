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
#include "common.hh"
#include "scene-manager.hh"
#include "widgets.hh"
#include "main-menu.hh"
#include "font-manager.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "xemu-hud.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "reporting.hh"
#include "qapi/error.h"
#include "actions.hh"

#include <algorithm>

#include "../xemu-input.h"
#include "../xemu-dsu.h"
#include "../xemu-notifications.h"
#include "../xemu-settings.h"
#include "../xemu-monitor.h"
#include "../xemu-version.h"
#include "../xemu-net.h"
#include "../xemu-stream.h"
#include "../xemu-os-utils.h"
#include "../xemu-xbe.h"
#include "../xemu-game-library.h"
#include "../xemu-screenscraper.h"

#ifdef CONFIG_UWP
#include "../uwp-password-vault.h"

#include <glib/gstdio.h>
#include <thread>
#endif

#include "../thirdparty/fatx/fatx.h"

#define DEFAULT_XMU_SIZE 8388608

MainMenuScene g_main_menu;

MainMenuTabView::~MainMenuTabView() {}
void MainMenuTabView::Draw()
{
}

void MainMenuGeneralView::Draw()
{
#if defined(_WIN32) && !defined(CONFIG_UWP)
    SectionTitle("Updates");
    Toggle("Check for updates", &g_config.general.updates.check,
           "Check for updates whenever xemu is opened");
#endif

#if defined(__x86_64__)
    SectionTitle("Performance");
    Toggle("Hard FPU emulation", &g_config.perf.hard_fpu,
           "Use hardware-accelerated floating point emulation (requires restart)");
#endif

    bool shader_settings_changed =
        Toggle("Cache shaders to disk", &g_config.perf.cache_shaders,
               "Reduce stutter in games by caching previously generated "
               "shaders");
    ImGui::BeginDisabled(!g_config.perf.cache_shaders);
    shader_settings_changed |=
        Toggle("Precompile shaders", &g_config.perf.precompile_shaders,
               "Compile a game's previously seen shaders when it starts");
    ImGui::BeginDisabled(!g_config.perf.precompile_shaders);
    Toggle("Wait for shaders before starting",
           &g_config.perf.precompile_shaders_wait,
           "Show a progress screen at launch until shaders are ready; B "
           "starts the game early");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (shader_settings_changed) {
        nv2a_shader_cache_settings_changed();
    }

    SectionTitle("Miscellaneous");
    Toggle("Show game library at launch", &g_config.general.library_on_launch,
           "Open the game library instead of starting the console");
    Toggle("Skip startup animation", &g_config.general.skip_boot_anim,
           "Skip the full Xbox boot animation sequence");
    FilePicker("Screenshot output directory", g_config.general.screenshot_dir,
               nullptr, 0, true, [](const char *path) {
                   xemu_settings_set_string(&g_config.general.screenshot_dir, path);
               });
#ifdef CONFIG_UWP
    StorageLocationSelector(
        "Games directory", g_config.general.games_dir,
        XEMU_UWP_DEV_FILES_GAMES_DIR, XEMU_UWP_DEFAULT_GAMES_DIR,
        [](const char *path) {
            if (path && *path) {
                g_mkdir_with_parents(path, 0755);
            }
            xemu_settings_set_string(&g_config.general.games_dir, path);
            xemu_game_library_refresh();
        });
#else
    FilePicker("Games directory", g_config.general.games_dir, nullptr, 0, true,
               [](const char *path) {
                   xemu_settings_set_string(&g_config.general.games_dir, path);
               });
#endif

#ifdef CONFIG_UWP
    SectionTitle("ScreenScraper");

    static constexpr const char *kScreenScraperVaultResource =
        "xemu/screenscraper";
    static bool credentials_initialized = false;
    static bool password_saved = false;
    static std::string username;
    static std::string password;
    static std::string credential_status;

    if (!credentials_initialized) {
        username = g_config.general.screenscraper.username
            ? g_config.general.screenscraper.username
            : "";
        char *error = nullptr;
        password_saved = !username.empty() && xemu_uwp_password_vault_has(
            kScreenScraperVaultResource, username.c_str(), &error);
        if (error != nullptr) {
            credential_status = error;
            g_free(error);
        }
        credentials_initialized = true;
    }

    ImGui::TextUnformatted("Username");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##screenscraper_username",
                                 "ScreenScraper username", &username)) {
        xemu_settings_set_string(&g_config.general.screenscraper.username,
                                 username.c_str());
        password_saved = false;
        credential_status.clear();
    }
    TextFieldActivateWithA();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char *error = nullptr;
        password_saved = !username.empty() && xemu_uwp_password_vault_has(
            kScreenScraperVaultResource, username.c_str(), &error);
        if (error != nullptr) {
            credential_status = error;
        }
        g_free(error);
        xemu_game_library_screenscraper_settings_changed();
    }

    ImGui::TextUnformatted("Password");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint(
        "##screenscraper_password",
        password_saved ? "Saved in Windows Password Vault"
                       : "ScreenScraper password",
        &password, ImGuiInputTextFlags_Password);
    TextFieldActivateWithA();

    bool can_save = !username.empty() && !password.empty();
    ImGui::BeginDisabled(!can_save);
    if (ImGui::Button("Save credentials")) {
        char *error = nullptr;
        if (xemu_uwp_password_vault_store(kScreenScraperVaultResource,
                                          username.c_str(),
                                          password.c_str(), &error)) {
            password_saved = true;
            credential_status = "Password saved in Windows Password Vault";
            std::fill(password.begin(), password.end(), '\0');
            password.clear();
            xemu_game_library_screenscraper_settings_changed();
        } else if (error != nullptr) {
            credential_status = error;
        }
        g_free(error);
    }
    ImGui::EndDisabled();

    if (password_saved) {
        ImGui::SameLine();
        if (ImGui::Button("Remove saved password")) {
            char *error = nullptr;
            if (xemu_uwp_password_vault_remove(kScreenScraperVaultResource,
                                               username.c_str(),
                                               &error)) {
                password_saved = false;
                credential_status = "Saved password removed";
                xemu_game_library_screenscraper_settings_changed();
            } else if (error != nullptr) {
                credential_status = error;
            }
            g_free(error);
        }
    }

    if (username.empty() || !password_saved) {
        ImGui::TextWrapped("Artwork and game details are only downloaded "
                           "with your own ScreenScraper account.");
    }

    if (!credential_status.empty()) {
        ImGui::TextWrapped("%s", credential_status.c_str());
    }

    {
        static std::string media_dir_display;
        const char *configured_media_dir =
            g_config.general.screenscraper.media_dir;
        media_dir_display = (configured_media_dir && *configured_media_dir)
                                ? configured_media_dir
                                : xemu::screenscraper::default_media_root();
        StorageLocationSelector(
            "Downloaded media location", media_dir_display.c_str(),
            XEMU_UWP_DEV_FILES_MEDIA_DIR, XEMU_UWP_DEFAULT_MEDIA_DIR,
            [](const char *path) {
                if (path && *path) {
                    std::string system_dir = std::string(path) + "\\xbox";
                    g_mkdir_with_parents(system_dir.c_str(), 0755);
                }
                xemu_settings_set_string(
                    &g_config.general.screenscraper.media_dir, path);
                xemu::screenscraper::sync_settings_from_config();
                xemu_game_library_screenscraper_settings_changed();
            });
    }

    {
        static constexpr const char *kBoxArtTypes =
            "2D box\0"
            "3D box\0"
            "Mix image v1\0"
            "Mix image v2\0"
            "Fan art\0"
            "Screenshot\0"
            "Game logo (HD preferred)\0"
            "2D box back\0"
            "Disc\0";
        auto &scraper = g_config.general.screenscraper;
        auto &media = scraper.media;

        // A displayed media type is always downloaded as well.
        auto enable_box_art_download = [&media](int style) {
            switch (style) {
            case xemu::screenscraper::kBoxArt3D:
                media.box_3d = true;
                break;
            case xemu::screenscraper::kBoxArtMixV1:
                media.mix_v1 = true;
                break;
            case xemu::screenscraper::kBoxArtMixV2:
                media.mix_v2 = true;
                break;
            case xemu::screenscraper::kBoxArtFanArt:
                media.fan_art = true;
                break;
            case xemu::screenscraper::kBoxArtScreenshot:
                media.screenshots = true;
                break;
            case xemu::screenscraper::kBoxArtLogo:
                media.wheels = true;
                break;
            case xemu::screenscraper::kBoxArtBack:
                media.box_back = true;
                break;
            case xemu::screenscraper::kBoxArtDisc:
                media.physical_media = true;
                break;
            default:
                media.box_front = true;
                break;
            }
        };

        SectionTitle("Library artwork");
        int box_art = scraper.box_art_style;
        if (ChevronCombo("Game list box art", &box_art, kBoxArtTypes,
                         "Artwork shown for each game in the library row")) {
            scraper.box_art_style = (CONFIG_GENERAL_SCREENSCRAPER_BOX_ART_STYLE)box_art;
            enable_box_art_download(box_art);
            xemu_game_library_screenscraper_settings_changed();
        }
        int large_box_art = scraper.large_box_art_style;
        if (ChevronCombo("Large box art", &large_box_art, kBoxArtTypes,
                         "Artwork shown beside the selected game's details")) {
            scraper.large_box_art_style =
                (CONFIG_GENERAL_SCREENSCRAPER_LARGE_BOX_ART_STYLE)large_box_art;
            enable_box_art_download(large_box_art);
            xemu_game_library_screenscraper_settings_changed();
        }
        int background = scraper.background;
        if (ChevronCombo("Dynamic background", &background,
                         "Off\0"
                         "Fan art\0"
                         "Screenshot\0"
                         "Video\0"
                         "Mix image v1\0"
                         "Mix image v2\0",
                         "Artwork shown behind the library for the selected "
                         "game")) {
            scraper.background = (CONFIG_GENERAL_SCREENSCRAPER_BACKGROUND)background;
            switch (background) {
            case xemu::screenscraper::kBackgroundFanArt:
                media.fan_art = true;
                break;
            case xemu::screenscraper::kBackgroundScreenshot:
                media.screenshots = true;
                break;
            case xemu::screenscraper::kBackgroundVideo:
                media.videos = true;
                break;
            case xemu::screenscraper::kBackgroundMixV1:
                media.mix_v1 = true;
                break;
            case xemu::screenscraper::kBackgroundMixV2:
                media.mix_v2 = true;
                break;
            default:
                break;
            }
            xemu_game_library_screenscraper_settings_changed();
        }

        SectionTitle("Artwork downloads");
        bool media_changed = false;
        media_changed |= Toggle("2D box", &media.box_front);
        media_changed |= Toggle("3D box", &media.box_3d);
        media_changed |= Toggle("2D box back", &media.box_back);
        media_changed |= Toggle("Game logo (HD preferred)", &media.wheels);
        media_changed |= Toggle("Disc", &media.physical_media);
        media_changed |= Toggle("Mix image v1", &media.mix_v1);
        media_changed |= Toggle("Mix image v2", &media.mix_v2);
        media_changed |= Toggle("Fan art", &media.fan_art);
        media_changed |= Toggle("Screenshot", &media.screenshots);
        media_changed |= Toggle("Video", &media.videos,
                                "Video previews use more storage and "
                                "bandwidth");
        if (media_changed) {
            xemu_game_library_screenscraper_settings_changed();
        }

        Toggle("Overwrite existing assets", &scraper.overwrite_assets,
               "Library downloads replace saved game details and artwork");

        // The download runs on library workers; this only polls progress.
        static bool download_needs_credentials = false;
        bool have_credentials = !username.empty() && password_saved;
        XemuGameLibraryDownloadStatus download;
        xemu_game_library_download_status(&download);
        ImGui::BeginDisabled(download.running);
        if (ImGui::Button("Download library assets")) {
            download_needs_credentials = !have_credentials;
            if (have_credentials) {
                xemu_game_library_download_assets(
                    scraper.overwrite_assets,
                    [](uint32_t total, uint32_t failed, void *) {
                        std::string message;
                        if (total == 0) {
                            message = "No games to download artwork for";
                        } else if (failed == 0) {
                            message = string_format(
                                "Artwork downloaded for %u game%s", total,
                                total == 1 ? "" : "s");
                        } else {
                            message = string_format(
                                "Artwork downloaded for %u of %u games",
                                total - failed, total);
                        }
                        RunOnMainThread([message]() {
                            xemu_queue_notification(message.c_str());
                        });
                    },
                    nullptr);
            }
        }
        ImGui::EndDisabled();
        if (download.running) {
            if (download.total == 0) {
                ImGui::TextUnformatted("Preparing library download...");
            } else {
                ImGui::Text("%s: %u of %u",
                            download.identifying ? "Identifying games"
                                                 : "Downloading artwork",
                            download.done, download.total);
            }
        } else if (download_needs_credentials && !have_credentials) {
            ImGui::TextWrapped("Save your ScreenScraper username and "
                               "password first");
        }
    }
#endif
    // toggle("Throttle DVD/HDD speeds", &g_config.general.throttle_io,
    //        "Limit DVD/HDD throughput to approximate Xbox load times");
}

bool MainMenuInputView::ConsumeRebindEvent(SDL_Event *event)
{
    if (!m_rebinding) {
        return false;
    }

    RebindEventResult rebind_result = m_rebinding->ConsumeRebindEvent(event);
    if (rebind_result == RebindEventResult::Complete) {
        m_rebinding = nullptr;
    }

    return rebind_result == RebindEventResult::Ignore;
}

bool MainMenuInputView::IsInputRebinding()
{
    return m_rebinding != nullptr;
}

void MainMenuInputView::Draw()
{
    if (m_rebinding &&
        m_rebinding->Poll() == RebindEventResult::Complete) {
        m_rebinding = nullptr;
    }

    SectionTitle("Controllers");
    ImGui::PushFont(g_font_mgr.m_menu_font_small);

    static int active = 0;

    // Output dimensions of texture
    float t_w = 512, t_h = 512;
    // Dimensions of (port+label)s
    float b_x = 0, b_x_stride = 100, b_y = 400;
    float b_w = 68, b_h = 81;
    // Dimensions of controller (rendered at origin)
    float controller_width  = 477.0f;
    float controller_height = 395.0f;
    // Dimensions of XMU
    float xmu_x = 0, xmu_x_stride = 256, xmu_y = 0;
    float xmu_w = 256, xmu_h = 256;
    bool use_gl_previews = controller_fbo != NULL && xmu_fbo != NULL;
    ImTextureID id = 0;

    // Setup rendering to fbo for controller and port images
    if (use_gl_previews) {
        controller_fbo->Target();
        id = (ImTextureID)(intptr_t)controller_fbo->Texture();
    }

    //
    // Render buttons with icons of the Xbox style port sockets with
    // circular numbers above them. These buttons can be activated to
    // configure the associated port, like a tabbed interface.
    //
    ImVec4 color_active(0.50, 0.86, 0.54, 0.12);
    ImVec4 color_inactive(0, 0, 0, 0);

    // Begin a 4-column layout to render the ports
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        g_viewport_mgr.Scale(ImVec2(0, 12)));
    ImGui::Columns(4, "mixed", false);

    const int port_padding = 8;
    for (int i = 0; i < 4; i++) {
        bool is_selected = (i == active);
        bool port_is_bound = (xemu_input_get_bound(i) != NULL);

        // Set an X offset to center the image button within the column
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            (int)((ImGui::GetColumnWidth() - b_w * g_viewport_mgr.m_scale -
                   2 * port_padding * g_viewport_mgr.m_scale) /
                  2));

        // We are using the same texture for all buttons, but ImageButton
        // uses the texture as a unique ID. Push a new ID now to resolve
        // the conflict.
        ImGui::PushID(i);
        float x = b_x + i * b_x_stride;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              is_selected ? color_active : color_inactive);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            g_viewport_mgr.Scale(ImVec2(port_padding, port_padding)));
        bool activated;
        if (use_gl_previews) {
            activated = ImGui::ImageButton(
                "port_image_button",
                id,
                ImVec2(b_w * g_viewport_mgr.m_scale,
                       b_h * g_viewport_mgr.m_scale),
                ImVec2(x / t_w, (b_y + b_h) / t_h),
                ImVec2((x + b_w) / t_w, b_y / t_h));
        } else {
            activated = ImGui::InvisibleButton(
                "port_preview_button",
                ImVec2(b_w * g_viewport_mgr.m_scale,
                       b_h * g_viewport_mgr.m_scale));
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        if (activated) {
            active = i;
            m_rebinding = nullptr;
        }

        uint32_t port_color = 0xafafafff;
        bool is_hovered = ImGui::IsItemHovered();
        if (is_hovered) {
            port_color = 0xffffffff;
        } else if (is_selected || port_is_bound) {
            port_color = 0x81dc8a00;
        }

        if (use_gl_previews) {
            RenderControllerPort(x, b_y, i, port_color);
        }

        ImGui::PopID();
        ImGui::NextColumn();
    }
    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::Columns(1);

    //
    // Render device driver combo
    //

    // List available device drivers
    const char *driver = bound_drivers[active];

    if (strcmp(driver, DRIVER_DUKE) == 0)
        driver = DRIVER_DUKE_DISPLAY_NAME;
    else if (strcmp(driver, DRIVER_S) == 0)
        driver = DRIVER_S_DISPLAY_NAME;
    else if (strcmp(driver, DRIVER_LIGHTGUN) == 0)
        driver = DRIVER_LIGHTGUN_DISPLAY_NAME;

    ImGui::Columns(2, "", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth()*0.25);

    ImGui::Text("Emulated Device");
    ImGui::SameLine(0, 0);
    ImGui::NextColumn();

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("###InputDrivers", driver,
                          ImGuiComboFlags_NoArrowButton)) {
        const char *available_drivers[] = { DRIVER_DUKE, DRIVER_S,
                                            DRIVER_LIGHTGUN };
        const char *driver_display_names[] = { DRIVER_DUKE_DISPLAY_NAME,
                                               DRIVER_S_DISPLAY_NAME,
                                               DRIVER_LIGHTGUN_DISPLAY_NAME };
        bool is_selected = false;
        int num_drivers = sizeof(driver_display_names) / sizeof(driver_display_names[0]);
        for (int i = 0; i < num_drivers; i++) {
            const char *iter = driver_display_names[i];
            is_selected = strcmp(driver, iter) == 0;
            ImGui::PushID(iter);
            if (ImGui::Selectable(iter, is_selected)) {
                for (int j = 0; j < num_drivers; j++) {
                    if (iter == driver_display_names[j])
                        bound_drivers[active] = available_drivers[j];
                }
                xemu_input_bind(active, bound_controllers[active], 1);
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }
    DrawComboChevron();

    ImGui::NextColumn();

    //
    // Render input device combo
    //

    ImGui::Text("Input Device");
    ImGui::SameLine(0, 0);
    ImGui::NextColumn();

    // List available input devices
    const char *not_connected = "Not Connected";
    ControllerState *bound_state = xemu_input_get_bound(active);

    // Get current controller name
    const char *name;
    if (bound_state == NULL) {
        name = not_connected;
    } else {
        name = bound_state->name;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("###InputDevices", name, ImGuiComboFlags_NoArrowButton))
    {
        // Handle "Not connected"
        bool is_selected = bound_state == NULL;
        if (ImGui::Selectable(not_connected, is_selected)) {
            xemu_input_bind(active, NULL, 1);
            bound_state = NULL;
        }
        if (is_selected) {
            ImGui::SetItemDefaultFocus();
        }

        // Handle all available input devices
        ControllerState *iter;
        QTAILQ_FOREACH(iter, &available_controllers, entry) {
            is_selected = bound_state == iter;
            ImGui::PushID(iter);
            const char *selectable_label = iter->name;
            char buf[128];
            if (iter->bound >= 0) {
                snprintf(buf, sizeof(buf), "%s (Port %d)", iter->name, iter->bound+1);
                selectable_label = buf;
            }
            if (ImGui::Selectable(selectable_label, is_selected)) {
                xemu_input_bind(active, iter, 1);

                // FIXME: We want to bind the XMU here, but we can't because we
                // just unbound it and we need to wait for Qemu to release the
                // file

                // If we previously had no controller connected, we can rebind
                // the XMU
                if (bound_state == NULL)
                    xemu_input_rebind_xmu(active);

                bound_state = iter;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }
    DrawComboChevron();

    ImGui::Columns(1);

    //
    // Add a separator between input selection and controller graphic
    //
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y / 2));

    //
    // Render controller image
    //
    bool device_selected = false;
    static ControllerState disconnected_state{};
    ControllerState *preview_state = bound_state;

    if (bound_state) {
        device_selected = true;
        if (use_gl_previews) {
            RenderController(0, 0, 0x81dc8a00, 0x0f0f0f00, bound_state);
        }
    } else {
        preview_state = &disconnected_state;
        if (use_gl_previews) {
            RenderController(0, 0, 0x1f1f1f00, 0x0f0f0f00, preview_state);
        }
    }

    ImVec2 cur = ImGui::GetCursorPos();

    ImVec2 controller_display_size;
    if (ImGui::GetContentRegionMax().x < controller_width*g_viewport_mgr.m_scale) {
        controller_display_size.x = ImGui::GetContentRegionMax().x;
        controller_display_size.y =
            controller_display_size.x * controller_height / controller_width;
    } else {
        controller_display_size =
            ImVec2(controller_width * g_viewport_mgr.m_scale,
                   controller_height * g_viewport_mgr.m_scale);
    }

    ImVec2 pos;
    if (use_gl_previews) {
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            (int)((ImGui::GetColumnWidth() - controller_display_size.x) / 2.0));

        ImGui::Image(id,
            controller_display_size,
            ImVec2(0, controller_height/t_h),
            ImVec2(controller_width/t_w, 0));
        pos = ImGui::GetCursorPos();
        if (!device_selected) {
            const char *msg = "Please select an available input device";
            ImVec2 dim = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPosX(cur.x + (controller_display_size.x-dim.x)/2);
            ImGui::SetCursorPosY(cur.y + (controller_display_size.y-dim.y)/2);
            ImGui::Text("%s", msg);
        }
        controller_fbo->Restore();
    } else {
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            (int)((ImGui::GetColumnWidth() - controller_display_size.x) / 2.0));
        ImGui::InvisibleButton("controller_preview", controller_display_size);
        pos = ImGui::GetCursorPos();
        if (!device_selected) {
            const char *msg = "Please select an available input device";
            ImVec2 dim = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPosX(cur.x + (controller_display_size.x-dim.x)/2);
            ImGui::SetCursorPosY(cur.y + (controller_display_size.y-dim.y)/2);
            ImGui::Text("%s", msg);
        }
    }

    ImGui::PopFont();
    ImGui::SetCursorPos(pos);

    if (bound_state) {
        ImGui::PushID(active);

        SectionTitle("Expansion Slots");
        // Begin a 2-column layout to render the expansion slots
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            g_viewport_mgr.Scale(ImVec2(0, 12)));
        ImGui::Columns(2, "mixed", false);

        if (use_gl_previews) {
            xmu_fbo->Target();
            id = (ImTextureID)(intptr_t)xmu_fbo->Texture();
        }

        static const SDL_DialogFileFilter img_file_filters[] = {
            { ".img Files", "img" },
            { "All Files", "*" }
        };
        const char *comboLabels[2] = { "###ExpansionSlotA",
                                       "###ExpansionSlotB" };
        for (int i = 0; i < 2; i++) {
            // Display a combo box to allow the user to choose the type of
            // peripheral they want to use
            enum peripheral_type selected_type =
                bound_state->peripheral_types[i];
            const char *peripheral_type_names[2] = { "None", "Memory Unit" };
            const char *selected_peripheral_type =
                peripheral_type_names[selected_type];
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(comboLabels[i], selected_peripheral_type,
                                  ImGuiComboFlags_NoArrowButton)) {
                // Handle all available peripheral types
                for (int j = 0; j < 2; j++) {
                    bool is_selected = selected_type == j;
                    ImGui::PushID(j);
                    const char *selectable_label = peripheral_type_names[j];

                    if (ImGui::Selectable(selectable_label, is_selected)) {
                        // Free any existing peripheral
                        if (bound_state->peripherals[i] != NULL) {
                            if (bound_state->peripheral_types[i] ==
                                PERIPHERAL_XMU) {
                                // Another peripheral was already bound.
                                // Unplugging
                                xemu_input_unbind_xmu(active, i);
                            }

                            // Free the existing state
                            g_free((void *)bound_state->peripherals[i]);
                            bound_state->peripherals[i] = NULL;
                        }

                        // Change the peripheral type to the newly selected type
                        bound_state->peripheral_types[i] =
                            (enum peripheral_type)j;

                        // Allocate state for the new peripheral
                        if (j == PERIPHERAL_XMU) {
                            bound_state->peripherals[i] =
                                g_malloc(sizeof(XmuState));
                            memset(bound_state->peripherals[i], 0,
                                   sizeof(XmuState));
                        }

                        xemu_save_peripheral_settings(
                            active, i, bound_state->peripheral_types[i], NULL);
                    }

                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }

                    ImGui::PopID();
                }

                ImGui::EndCombo();
            }
            DrawComboChevron();

            // Set an X offset to center the image button within the column
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() +
                (int)((ImGui::GetColumnWidth() -
                       xmu_w * g_viewport_mgr.m_scale -
                       2 * port_padding * g_viewport_mgr.m_scale) /
                      2));

            selected_type = bound_state->peripheral_types[i];
            if (selected_type == PERIPHERAL_XMU) {
                float x = xmu_x + i * xmu_x_stride;
                float y = xmu_y;

                XmuState *xmu = (XmuState *)bound_state->peripherals[i];
                if (use_gl_previews) {
                    if (xmu->filename != NULL && strlen(xmu->filename) > 0) {
                        RenderXmu(x, y, 0x81dc8a00, 0x0f0f0f00);

                    } else {
                        RenderXmu(x, y, 0x1f1f1f00, 0x0f0f0f00);
                    }
                }

                ImVec2 xmu_display_size;
                if (ImGui::GetContentRegionMax().x <
                    xmu_h * g_viewport_mgr.m_scale) {
                    xmu_display_size.x = ImGui::GetContentRegionMax().x / 2;
                    xmu_display_size.y = xmu_display_size.x * xmu_h / xmu_w;
                } else {
                    xmu_display_size = ImVec2(xmu_w * g_viewport_mgr.m_scale,
                                              xmu_h * g_viewport_mgr.m_scale);
                }

                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() +
                    (int)((ImGui::GetColumnWidth() - xmu_display_size.x) /
                          2.0));

                if (use_gl_previews) {
                    ImGui::Image(id, xmu_display_size, ImVec2(0.5f * i, 1),
                                 ImVec2(0.5f * (i + 1), 0));
                } else {
                    ImGui::PushID(i);
                    ImGui::InvisibleButton("xmu_preview", xmu_display_size);
                    ImGui::PopID();
                }

                // Button to generate a new XMU
                ImGui::PushID(i);
                if (ImGui::Button("New Image", ImVec2(250, 0))) {
                    int port = active;
                    int slot = i;
                    ShowSaveFileDialog(img_file_filters, 2, nullptr, [port, slot](const char *new_path) {
                        if (create_fatx_image(new_path, DEFAULT_XMU_SIZE)) {
                            // XMU was created successfully. Bind it
                            xemu_input_bind_xmu(port, slot, new_path, false);
                        } else {
                            // Show alert message
                            char *msg = g_strdup_printf(
                                "Unable to create XMU image at %s", new_path);
                            xemu_queue_error_message(msg);
                            g_free(msg);
                        }
                    });
                }

                int port = active;
                int slot = i;
                FilePicker("Image", xmu->filename, img_file_filters, 2, false,
                           [port, slot](const char *path) {
                               if (strlen(path) > 0) {
                                   xemu_input_bind_xmu(port, slot, path, false);
                               } else {
                                   xemu_input_unbind_xmu(port, slot);
                               }
                           });

                ImGui::PopID();
            }

            ImGui::NextColumn();
        }

        if (use_gl_previews) {
            xmu_fbo->Restore();
        }

        ImGui::PopStyleVar(); // ItemSpacing
        ImGui::Columns(1);

        SectionTitle("Mapping");
        ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Header];
        tc.w = 0.0f;
        ImGui::PushStyleColor(ImGuiCol_Header, tc);

        if (ImGui::CollapsingHeader("Input Mapping")) {
            float p = ImGui::GetFrameHeight() * 0.3;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(p, p));
            if (ImGui::BeginTable("input_remap_tbl", 2,
                                  ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("Emulated Input");
                ImGui::TableSetupColumn("Host Input");
                ImGui::TableHeadersRow();

                PopulateTableController(bound_state);

                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }

        bool is_gamepad = bound_state->type == INPUT_DEVICE_SDL_GAMEPAD;
#ifdef XEMU_INPUT_UWP
        is_gamepad = is_gamepad ||
            bound_state->type == INPUT_DEVICE_UWP_GAMEPAD;
#endif

        if (is_gamepad && bound_state->controller_map != nullptr) {
            Toggle("Enable Rumble",
                   &bound_state->controller_map->enable_rumble);
            Toggle("Invert Left X Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_left_x);
            Toggle("Invert Left Y Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_left_y);
            Toggle("Invert Right X Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_right_x);
            Toggle("Invert Right Y Axis",
                   &bound_state->controller_map->controller_mapping
                        .invert_axis_right_y);
        }

        bool can_reset_mapping =
            bound_state->type == INPUT_DEVICE_SDL_KEYBOARD ||
            bound_state->type == INPUT_DEVICE_SDL_GAMEPAD
#ifdef XEMU_INPUT_UWP
            || bound_state->type == INPUT_DEVICE_UWP_GAMEPAD
#endif
            ;
        if (can_reset_mapping && ImGui::Button("Reset to Default")) {
            xemu_input_reset_input_mapping(bound_state);
        }

        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    SectionTitle("Options");
    Toggle("Auto-bind controllers", &g_config.input.auto_bind,
           "Bind newly connected controllers to any open port");
    Toggle("Background controller input capture",
           &g_config.input.background_input_capture,
           "Capture even if window is unfocused (requires restart)");

    if (strcmp(bound_drivers[active], DRIVER_LIGHTGUN) == 0) {
        SectionTitle("Lightgun");
        ImGui::SliderFloat("Aim smoothing",
                           &g_config.input.lightgun_smoothing,
                           0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Aim sensitivity",
                           &g_config.input.lightgun_sensitivity,
                           0.1f, 3.0f, "%.2fx");
    }

    SectionTitle("DSU Network Input");
    bool *dsu_enabled = nullptr;
    bool *dsu_addon = nullptr;
    bool *dsu_gamepad = nullptr;
    bool *dsu_motion = nullptr;
    int *dsu_motion_target = nullptr;
    float *dsu_motion_sensitivity_horizontal = nullptr;
    float *dsu_motion_sensitivity_vertical = nullptr;
    bool *dsu_touch = nullptr;
    bool *dsu_keyboard = nullptr;
    bool *dsu_mouse = nullptr;
    const char **dsu_address = nullptr;
    int *dsu_port = nullptr;
    int *dsu_slot = nullptr;
#define SELECT_DSU_PLAYER(n)                                                \
    dsu_enabled = &g_config.input.dsu.player##n.enabled;                    \
    dsu_addon = &g_config.input.dsu.player##n.addon;                        \
    dsu_gamepad = &g_config.input.dsu.player##n.gamepad;                    \
    dsu_motion = &g_config.input.dsu.player##n.motion;                      \
    dsu_motion_target = &g_config.input.dsu.player##n.motion_target;        \
    dsu_motion_sensitivity_horizontal =                                    \
        &g_config.input.dsu.player##n.motion_sensitivity_horizontal;        \
    dsu_motion_sensitivity_vertical =                                      \
        &g_config.input.dsu.player##n.motion_sensitivity_vertical;          \
    dsu_touch = &g_config.input.dsu.player##n.touch;                        \
    dsu_keyboard = &g_config.input.dsu.player##n.keyboard;                  \
    dsu_mouse = &g_config.input.dsu.player##n.mouse;                        \
    dsu_address = &g_config.input.dsu.player##n.server_address;             \
    dsu_port = &g_config.input.dsu.player##n.server_port;                  \
    dsu_slot = &g_config.input.dsu.player##n.slot
    switch (active) {
    case 0: SELECT_DSU_PLAYER(1); break;
    case 1: SELECT_DSU_PLAYER(2); break;
    case 2: SELECT_DSU_PLAYER(3); break;
    default: SELECT_DSU_PLAYER(4); break;
    }
#undef SELECT_DSU_PLAYER

    Toggle("Enable DSU for this port", dsu_enabled,
           "Connect to a DSU server over UDP");
    if (*dsu_enabled) {
        static char address_buffers[4][128];
        static const char *loaded_addresses[4] = {};
        if (loaded_addresses[active] != *dsu_address) {
            g_strlcpy(address_buffers[active],
                      *dsu_address ? *dsu_address : "",
                      sizeof(address_buffers[active]));
            loaded_addresses[active] = *dsu_address;
        }

        ImGui::Columns(2, "dsu_network", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.25f);
        ImGui::Text("Server address");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("###dsu_address", address_buffers[active],
                             sizeof(address_buffers[active]))) {
            xemu_settings_set_string(dsu_address, address_buffers[active]);
            loaded_addresses[active] = *dsu_address;
        }
        TextFieldActivateWithA();
        ImGui::NextColumn();
        ImGui::Text("Server port");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt("###dsu_port", dsu_port)) {
            *dsu_port = CLAMP(*dsu_port, 1, 65535);
        }
        ImGui::NextColumn();
        ImGui::Text("Remote slot");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt("###dsu_slot", dsu_slot)) {
            *dsu_slot = CLAMP(*dsu_slot, 0, 3);
        }
        ImGui::Columns(1);

        Toggle("Auxiliary controller", dsu_addon,
               "Merge DSU input with the local controller on this port");
        Toggle("Gamepad", dsu_gamepad);
        Toggle("Motion", dsu_motion,
               "Map DSU gyroscope motion to an Xbox input");
        if (*dsu_motion) {
            ChevronCombo("Motion target", dsu_motion_target,
                         "Mouse movement\0"
                         "Right stick\0"
                         "Left stick\0"
                         "D-Pad\0"
                         "Lightgun aim\0",
                         "Choose the input controlled by gyroscope motion");
            ImGui::SliderFloat("Motion horizontal sensitivity",
                               dsu_motion_sensitivity_horizontal,
                               0.1f, 5.0f, "%.2fx");
            ImGui::SliderFloat("Motion vertical sensitivity",
                               dsu_motion_sensitivity_vertical,
                               0.1f, 5.0f, "%.2fx");
        }
        Toggle("Touch / lightgun aim", dsu_touch);
        Toggle("Keyboard", dsu_keyboard);
        Toggle("Mouse", dsu_mouse);

        XemuDSUPlayerState dsu_state;
        if (xemu_dsu_get_player_state(active, &dsu_state)) {
            ImGui::TextWrapped("Status: %s (slot %u)",
                               dsu_state.controller.connected
                                   ? "controller connected"
                                   : "waiting for server",
                               dsu_state.remote_slot);
        }
    }
}

void MainMenuInputView::Hide()
{
    m_rebinding = nullptr;
}

void MainMenuInputView::PopulateTableController(ControllerState *state)
{
    if (!state)
        return;

    // Must match g_keyboard_scancode_map and the controller
    // button map below.
    static constexpr const char *face_button_index_to_name_map[15] = {
        "A",
        "B",
        "X",
        "Y",
        "Back",
        "Guide",
        "Start",
        "Left Stick Button",
        "Right Stick Button",
        "White",
        "Black",
        "DPad Up",
        "DPad Down",
        "DPad Left",
        "DPad Right",
    };

    // Must match g_keyboard_scancode_map[15:]. Each axis requires
    // two keys for the positive and negative direction with the
    // exception of the triggers, which only require one each.
    static constexpr const char *keyboard_stick_index_to_name_map[10] = {
        "Left Stick Up",
        "Left Stick Left",
        "Left Stick Right",
        "Left Stick Down",
        "Left Trigger",
        "Right Stick Up",
        "Right Stick Left",
        "Right Stick Right",
        "Right Stick Down",
        "Right Trigger",
    };

    // Must match controller axis map below.
    static constexpr const char *gamepad_axis_index_to_name_map[6] = {
        "Left Stick Axis X",
        "Left Stick Axis Y",
        "Right Stick Axis X",
        "Right Stick Axis Y",
        "Left Trigger Axis",
        "Right Trigger Axis",
    };

    bool is_keyboard = state->type == INPUT_DEVICE_SDL_KEYBOARD;

    int num_axis_mappings;
    const char *const *axis_index_to_name_map;
    if (is_keyboard) {
      num_axis_mappings = std::size(keyboard_stick_index_to_name_map);
      axis_index_to_name_map = keyboard_stick_index_to_name_map;
    } else {
      num_axis_mappings = std::size(gamepad_axis_index_to_name_map);
      axis_index_to_name_map = gamepad_axis_index_to_name_map;
    }

    constexpr int num_face_buttons = std::size(face_button_index_to_name_map);
    const int table_rows = num_axis_mappings + num_face_buttons;
    for (int i = 0; i < table_rows; ++i) {
        ImGui::TableNextRow();

        // Button/Axis Name Column
        ImGui::TableSetColumnIndex(0);

        if (i < num_face_buttons) {
          ImGui::Text("%s", face_button_index_to_name_map[i]);
        } else {
          ImGui::Text("%s", axis_index_to_name_map[i - num_face_buttons]);
        }

        // Button Binding Column
        ImGui::TableSetColumnIndex(1);

        if (m_rebinding && m_rebinding->GetTableRow() == i) {
            ImGui::Text("Press an input to rebind");
            continue;
        }

        const char *remap_button_text = "Invalid";
        if (is_keyboard) {
          // g_keyboard_scancode_map includes both face buttons and axis buttons.
            int keycode = *(g_keyboard_scancode_map[i]);
            if (keycode != SDL_SCANCODE_UNKNOWN) {
                remap_button_text =
                    SDL_GetScancodeName(static_cast<SDL_Scancode>(keycode));
            }
        } else if (state->controller_map != nullptr &&
                   i < num_face_buttons) {
                int *button_map[num_face_buttons] = {
                    &state->controller_map->controller_mapping.a,
                    &state->controller_map->controller_mapping.b,
                    &state->controller_map->controller_mapping.x,
                    &state->controller_map->controller_mapping.y,
                    &state->controller_map->controller_mapping.back,
                    &state->controller_map->controller_mapping.guide,
                    &state->controller_map->controller_mapping.start,
                    &state->controller_map->controller_mapping.lstick_btn,
                    &state->controller_map->controller_mapping.rstick_btn,
                    &state->controller_map->controller_mapping.lshoulder,
                    &state->controller_map->controller_mapping.rshoulder,
                    &state->controller_map->controller_mapping.dpad_up,
                    &state->controller_map->controller_mapping.dpad_down,
                    &state->controller_map->controller_mapping.dpad_left,
                    &state->controller_map->controller_mapping.dpad_right,
                };

                int button = *(button_map[i]);
                if (button != SDL_GAMEPAD_BUTTON_INVALID) {
                    remap_button_text = SDL_GetGamepadStringForButton(
                        static_cast<SDL_GamepadButton>(button));
                }
        } else if (state->controller_map != nullptr) {
          int *axis_map[6] = {
            &state->controller_map->controller_mapping.axis_left_x,
            &state->controller_map->controller_mapping.axis_left_y,
            &state->controller_map->controller_mapping.axis_right_x,
            &state->controller_map->controller_mapping.axis_right_y,
            &state->controller_map->controller_mapping
              .axis_trigger_left,
            &state->controller_map->controller_mapping
              .axis_trigger_right,
          };
          int axis = *(axis_map[i - num_face_buttons]);
          if (axis != SDL_GAMEPAD_AXIS_INVALID) {
            remap_button_text = SDL_GetGamepadStringForAxis(
                static_cast<SDL_GamepadAxis>(axis));
          }
        }

        ImGui::PushID(i);
        float tw = ImGui::CalcTextSize(remap_button_text).x;
        auto &style = ImGui::GetStyle();
        float max_button_width =
          tw + g_viewport_mgr.m_scale * 2 * style.FramePadding.x;

        float min_button_width = ImGui::GetColumnWidth(1) / 2;
        float button_width = std::max(min_button_width, max_button_width);

        bool can_rebind = is_keyboard || state->controller_map != nullptr;
        if (!can_rebind) {
            ImGui::TextUnformatted(remap_button_text);
        } else if (ImGui::Button(remap_button_text, ImVec2(button_width, 0))) {
          if (is_keyboard) {
            m_rebinding =
              std::make_unique<ControllerKeyboardRebindingMap>(i);
#ifdef XEMU_INPUT_UWP
          } else if (state->type == INPUT_DEVICE_UWP_GAMEPAD) {
            m_rebinding =
              std::make_unique<ControllerUWPGamepadRebindingMap>(i, state);
#endif
          } else {
            m_rebinding =
              std::make_unique<ControllerGamepadRebindingMap>(i,
                  state);
          }
        }
        ImGui::PopID();
    }
}

void MainMenuDisplayView::Draw()
{
    SectionTitle("Renderer");
#ifdef CONFIG_UWP
    int uwp_renderer = 0;
    ImGui::BeginDisabled();
    ChevronCombo("Backend", &uwp_renderer,
                 "Direct3D 12\0",
                 "Select desired renderer implementation");
    ImGui::EndDisabled();
#else
    ChevronCombo("Backend", &g_config.display.renderer,
                 "Null\0"
                 "OpenGL\0"
#ifdef CONFIG_VULKAN
                 "Vulkan\0"
#endif
#ifdef CONFIG_D3D12
                 "Direct3D 12\0"
#endif
                 ,
                 "Select desired renderer implementation");
#endif
    int rendering_scale = nv2a_get_surface_scale_factor() - 1;
    if (ChevronCombo("Internal resolution scale", &rendering_scale,
                     "1x\0"
                     "2x\0"
                     "3x\0"
                     "4x\0"
                     "5x\0"
                     "6x\0"
                     "7x\0"
                     "8x\0"
                     "9x\0"
                     "10x\0",
                     "Increase surface scaling factor for higher quality")) {
        nv2a_set_surface_scale_factor(rendering_scale+1);
    }

#ifdef CONFIG_D3D12
    Toggle("FSR 1 upscaling", &g_config.display.fsr,
           "Rebuild edges and sharpen the game image when it is scaled up "
           "to the screen");
    char sharpness[48];
    snprintf(sharpness, sizeof(sharpness),
             "Sharpening after FSR 1 upscaling (%d%%)",
             (int)lroundf(g_config.display.fsr_sharpness * 100));
    ImGui::BeginDisabled(!g_config.display.fsr);
    Slider("FSR 1 sharpness", &g_config.display.fsr_sharpness, sharpness);
    ImGui::EndDisabled();

    auto &textures = g_config.display.custom_textures;

    SectionTitle("Custom textures");
    Toggle("Load custom textures", &textures.load,
           "Replace game textures with the files in "
           "load\\textures\\<title ID>");
    ImGui::BeginDisabled(!textures.load);
    Toggle("Prefetch custom textures", &textures.prefetch,
           "Load a game's whole texture pack in the background when it "
           "starts, instead of each texture when it first appears");
    ImGui::EndDisabled();
    Toggle("Dump textures", &textures.dump,
           "Save each new game texture to dump\\textures\\<title ID>, "
           "named as a custom texture must be");
#ifdef CONFIG_UWP
    StorageLocationSelector(
        "Custom textures folder",
        (textures.path && *textures.path) ? textures.path :
                                            XEMU_UWP_DEV_FILES_SYSTEM_DIR,
        XEMU_UWP_DEV_FILES_SYSTEM_DIR, XEMU_UWP_USB_SYSTEM_DIR,
        [](const char *path) {
            xemu_settings_set_string(&g_config.display.custom_textures.path,
                                     path);
            nv2a_custom_textures_settings_changed();
        });
#else
    FilePicker("Custom textures folder", textures.path, nullptr, 0, true,
               [](const char *path) {
                   xemu_settings_set_string(
                       &g_config.display.custom_textures.path, path);
                   nv2a_custom_textures_settings_changed();
               });
#endif
    {
        XemuGameLibraryState state = {};

        if (xemu_game_library_get_current_state(&state) && state.running &&
            state.title_id != 0) {
            ImGui::TextWrapped("This game's title ID is %08X: its textures "
                               "go in load\\textures\\%08X",
                               state.title_id, state.title_id);
        }
        xemu_game_library_free_current_state(&state);
    }

    SectionTitle("Streaming");
    Toggle("Broadcast stream", &g_config.display.stream.broadcast_enabled,
           "Keep the broadcast active while enabled");
    Toggle("Remote start", &g_config.display.stream.remote_start,
           "Allow third-party apps to start/stop the broadcast over the "
           "control port");
    {
        static char stream_url[512];
        static bool stream_url_loaded = false;
        static int stream_control_port;
        static bool stream_control_port_loaded = false;
        if (!stream_url_loaded) {
            g_strlcpy(stream_url,
                      g_config.display.stream.url ? g_config.display.stream.url
                                                  : "",
                      sizeof(stream_url));
            stream_url_loaded = true;
        }
        if (!stream_control_port_loaded) {
            stream_control_port = g_config.display.stream.control_port;
            stream_control_port_loaded = true;
        }

        float size_ratio = 0.5;
        float width = ImGui::GetColumnWidth() * size_ratio;
        ImGui::PushFont(g_font_mgr.m_menu_font_small);
        PrepareComboTitleDescription(
            "Broadcast URL",
            "Stream output URL, e.g. udp://192.168.1.22:26960", size_ratio);
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputText("###stream_url", stream_url, sizeof(stream_url))) {
            xemu_settings_set_string(&g_config.display.stream.url, stream_url);
        }
        TextFieldActivateWithA();
        PrepareComboTitleDescription(
            "Remote control port",
            "UDP port for START/STOP commands when remote start is enabled",
            size_ratio);
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputInt("###stream_ctl_port", &stream_control_port)) {
            if (stream_control_port < 1) {
                stream_control_port = 1;
            }
            if (stream_control_port > 65535) {
                stream_control_port = 65535;
            }
            g_config.display.stream.control_port = stream_control_port;
        }
        ImGui::PopFont();
    }
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    if (!xemu_stream_is_supported()) {
        ImGui::TextWrapped(
            "Streaming is not available in this build (ffmpeg missing).");
    } else {
        ImGui::TextWrapped("Status: %s", xemu_stream_get_status());
    }
    ImGui::PopFont();
#endif

    SectionTitle("Window");
#ifndef CONFIG_UWP
    bool fs = xemu_is_fullscreen();
    if (Toggle("Fullscreen", &fs, "Enable fullscreen now")) {
        xemu_toggle_fullscreen();
    }
    Toggle("Fullscreen on startup",
           &g_config.display.window.fullscreen_on_startup,
           "Start xemu in fullscreen when opened");
    Toggle("Exclusive fullscreen",
           &g_config.display.window.fullscreen_exclusive,
           "May improve responsiveness, but slows window switching");
#endif
#ifdef CONFIG_UWP
    ImGui::BeginDisabled();
#endif
    if (ChevronCombo("Window size", &g_config.display.window.startup_size,
                     "Last Used\0"
                     "640x480\0"
                     "720x480\0"
                     "1280x720\0"
                     "1280x800\0"
                     "1280x960\0"
                     "1920x1080\0"
                     "2560x1440\0"
                     "2560x1600\0"
                     "2560x1920\0"
                     "3840x2160\0",
                     "Select preferred startup window size")) {
    }
#ifdef CONFIG_UWP
    ImGui::EndDisabled();
#endif
    Toggle("Vertical refresh sync", &g_config.display.window.vsync,
           "Sync to screen vertical refresh to reduce tearing artifacts");

    SectionTitle("Interface");
    Toggle("Show main menu bar", &g_config.display.ui.show_menubar,
           "Show main menu bar when mouse is activated");
    Toggle("Show notifications", &g_config.display.ui.show_notifications,
           "Display notifications in upper-right corner");
    Toggle("Show FPS counter", &g_config.display.ui.show_fps,
           "Display the guest frame rate in the upper-left corner");
    Toggle("Hide mouse cursor", &g_config.display.ui.hide_cursor,
           "Hide the mouse cursor when it is not moving");

    int ui_scale_idx;
    if (g_config.display.ui.auto_scale) {
        ui_scale_idx = 0;
    } else {
        ui_scale_idx = g_config.display.ui.scale;
        if (ui_scale_idx < 1) ui_scale_idx = 1;
        else if (ui_scale_idx > 2) ui_scale_idx = 2;
    }
    if (ChevronCombo("UI scale", &ui_scale_idx,
                     "Auto\0"
                     "1x\0"
                     "2x\0",
                     "Interface element scale")) {
        if (ui_scale_idx == 0) {
            g_config.display.ui.auto_scale = true;
        } else {
            g_config.display.ui.auto_scale = false;
            g_config.display.ui.scale = ui_scale_idx;
        }
    }
    Toggle("Animations", &g_config.display.ui.use_animations,
           "Enable xemu user interface animations");
    ChevronCombo("Filter method", &g_config.display.filtering,
                 "Linear\0"
                 "Nearest\0",
                 "Select framebuffer texture filtering");
    ChevronCombo("Display mode", &g_config.display.ui.fit,
                 "Center\0"
                 "Scale\0"
                 "Stretch\0",
                 "Select how the framebuffer should fit or scale into the window");
    ChevronCombo("Aspect ratio", &g_config.display.ui.aspect_ratio,
                 "Native\0"
                 "Auto (Default)\0"
                 "4:3\0"
                 "16:9\0",
                 "Select the displayed aspect ratio; with Auto or 16:9, games "
                 "that support widescreen render in it from the next launch");
}

void MainMenuAudioView::Draw()
{
    SectionTitle("Volume");
    char buf[32];
    snprintf(buf, sizeof(buf), "Limit output volume (%d%%)",
             (int)(g_config.audio.volume_limit * 100));
    Slider("Output volume limit", &g_config.audio.volume_limit, buf);

    SectionTitle("Quality");
    Toggle("Real-time DSP processing", &g_config.audio.use_dsp,
           "Enable improved audio accuracy (experimental)");
    Toggle("DSP JIT engine", &g_config.audio.use_dsp_jit,
           "Use DSP JIT engine");

}

#ifndef CONFIG_UWP
NetworkInterface::NetworkInterface(pcap_if_t *pcap_desc, char *_friendlyname)
{
    m_pcap_name = pcap_desc->name;
    m_description = pcap_desc->description ?: pcap_desc->name;
    if (_friendlyname) {
        char *tmp =
            g_strdup_printf("%s (%s)", _friendlyname, m_description.c_str());
        m_friendly_name = tmp;
        g_free((gpointer)tmp);
    } else {
        m_friendly_name = m_description;
    }
}
#else
NetworkInterface::NetworkInterface(const char *name, const char *description)
{
    m_pcap_name = name;
    m_description = description ?: name;
    m_friendly_name = m_description;
}
#endif

NetworkInterfaceManager::NetworkInterfaceManager()
{
    m_current_iface = NULL;
    m_failed_to_load_lib = false;
}

void NetworkInterfaceManager::Refresh(void)
{
#ifdef CONFIG_UWP
    m_ifaces.clear();
    m_current_iface = NULL;
    m_failed_to_load_lib = true;
#else
    pcap_if_t *alldevs, *iter;
    char err[PCAP_ERRBUF_SIZE];

    if (xemu_net_is_enabled()) {
        return;
    }

#if defined(_WIN32)
    if (pcap_load_library()) {
        m_failed_to_load_lib = true;
        return;
    }
#endif

    m_ifaces.clear();
    m_current_iface = NULL;

    if (pcap_findalldevs(&alldevs, err)) {
        return;
    }

    for (iter=alldevs; iter != NULL; iter=iter->next) {
#if defined(_WIN32)
        char *friendly_name = get_windows_interface_friendly_name(iter->name);
        m_ifaces.emplace_back(new NetworkInterface(iter, friendly_name));
        if (friendly_name) {
            g_free((gpointer)friendly_name);
        }
#else
        m_ifaces.emplace_back(new NetworkInterface(iter));
#endif
        if (!strcmp(g_config.net.pcap.netif, iter->name)) {
            m_current_iface = m_ifaces.back().get();
        }
    }

    pcap_freealldevs(alldevs);
#endif
}

void NetworkInterfaceManager::Select(NetworkInterface &iface)
{
    m_current_iface = &iface;
    xemu_settings_set_string(&g_config.net.pcap.netif,
                             iface.m_pcap_name.c_str());
}

bool NetworkInterfaceManager::IsCurrent(NetworkInterface &iface)
{
    return &iface == m_current_iface;
}

MainMenuNetworkView::MainMenuNetworkView()
{
    should_refresh = true;
}

void MainMenuNetworkView::Draw()
{
    SectionTitle("Adapter");
    bool enabled = xemu_net_is_enabled();
    g_config.net.enable = enabled;
    if (Toggle("Enable", &g_config.net.enable,
               enabled ? "Virtual network connected; changing a setting "
                         "reconnects it" :
                         "Connect virtual network cable to machine")) {
        if (enabled) {
            xemu_net_disable();
        } else {
            xemu_net_enable();
        }
        enabled = xemu_net_is_enabled();
    }

    // Settings stay editable while connected; a change reconnects the
    // virtual cable with the new configuration.
    bool appearing = ImGui::IsWindowAppearing();
    bool reconnect = false;
#ifdef CONFIG_UWP
    if (g_config.net.backend == CONFIG_NET_BACKEND_PCAP) {
        g_config.net.backend = CONFIG_NET_BACKEND_NAT;
    }
    if (ChevronCombo(
            "Attached to", &g_config.net.backend,
            "NAT\0"
            "UDP Tunnel\0",
            "Controls what the virtual network controller interfaces with")) {
        appearing = true;
        reconnect = true;
    }
#else
    if (ChevronCombo(
            "Attached to", &g_config.net.backend,
            "NAT\0"
            "UDP Tunnel\0"
            "Bridged Adapter\0",
            "Controls what the virtual network controller interfaces with")) {
        appearing = true;
        reconnect = true;
    }
#endif
    SectionTitle("Options");
    switch (g_config.net.backend) {
    case CONFIG_NET_BACKEND_PCAP:
        reconnect |= DrawPcapOptions(appearing);
        break;
    case CONFIG_NET_BACKEND_NAT:
        reconnect |= DrawNatOptions(appearing);
        break;
    case CONFIG_NET_BACKEND_UDP:
        reconnect |= DrawUdpOptions(appearing);
        break;
    default: break;
    }

    if (reconnect && enabled) {
        xemu_net_disable();
        xemu_net_enable();
    }
}

bool MainMenuNetworkView::DrawPcapOptions(bool appearing)
{
    bool changed = false;

    if (iface_mgr.get() == nullptr) {
        iface_mgr.reset(new NetworkInterfaceManager());
        iface_mgr->Refresh();
    }

    if (iface_mgr->m_failed_to_load_lib) {
#ifdef CONFIG_UWP
        ImGui::Text("%s", "Bridged adapter networking is not available in UWP.");
#elif defined(_WIN32)
        const char *msg = "npcap library could not be loaded.\n"
                          "To use this backend, please install npcap.";
        ImGui::Text("%s", msg);
        ImGui::Dummy(ImVec2(0,10*g_viewport_mgr.m_scale));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-120*g_viewport_mgr.m_scale)/2);
        if (ImGui::Button("Install npcap", ImVec2(120*g_viewport_mgr.m_scale, 0))) {
            SDL_OpenURL("https://nmap.org/npcap/");
        }
#endif
    } else {
        const char *selected_display_name =
            (iface_mgr->m_current_iface ?
                 iface_mgr->m_current_iface->m_friendly_name.c_str() :
                 g_config.net.pcap.netif);
        float combo_width = ImGui::GetColumnWidth();
        float combo_size_ratio = 0.5;
        combo_width *= combo_size_ratio;
        PrepareComboTitleDescription("Network interface",
                                     "Host network interface to bridge with",
                                     combo_size_ratio);
        ImGui::SetNextItemWidth(combo_width);
        ImGui::PushFont(g_font_mgr.m_menu_font_small);
        if (ImGui::BeginCombo("###network_iface", selected_display_name,
                              ImGuiComboFlags_NoArrowButton)) {
            if (should_refresh) {
                iface_mgr->Refresh();
                should_refresh = false;
            }

            int i = 0;
            for (auto &iface : iface_mgr->m_ifaces) {
                bool is_selected = iface_mgr->IsCurrent((*iface));
                ImGui::PushID(i++);
                if (ImGui::Selectable(iface->m_friendly_name.c_str(),
                                      is_selected)) {
                    iface_mgr->Select((*iface));
                    changed = true;
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        } else {
            should_refresh = true;
        }
        ImGui::PopFont();
        DrawComboChevron();
    }
    return changed;
}

bool MainMenuNetworkView::DrawNatOptions(bool appearing)
{
    static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
    bool changed = false;
    WidgetTitleDescriptionItem(
        "Port Forwarding",
        "Configure xemu to forward connections to guest on these ports");
    float p = ImGui::GetFrameHeight() * 0.3;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(p, p));
    if (ImGui::BeginTable("port_forward_tbl", 4, flags))
    {
        ImGui::TableSetupColumn("Host Port");
        ImGui::TableSetupColumn("Guest Port");
        ImGui::TableSetupColumn("Protocol");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();

        for (unsigned int row = 0; row < g_config.net.nat.forward_ports_count; row++)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", g_config.net.nat.forward_ports[row].host);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", g_config.net.nat.forward_ports[row].guest);

            ImGui::TableSetColumnIndex(2);
            switch (g_config.net.nat.forward_ports[row].protocol) {
            case CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_TCP:
                ImGui::TextUnformatted("TCP"); break;
            case CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP:
                ImGui::TextUnformatted("UDP"); break;
            default: assert(0);
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(row);
            if (ImGui::Button("Remove")) {
                remove_net_nat_forward_ports(row);
                changed = true;
            }
            ImGui::PopID();
        }

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        static char buf[8] = {"1234"};
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth());
        ImGui::InputText("###hostport", buf, sizeof(buf));
        TextFieldActivateWithA();

        ImGui::TableSetColumnIndex(1);
        static char buf2[8] = {"1234"};
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth());
        ImGui::InputText("###guestport", buf2, sizeof(buf2));
        TextFieldActivateWithA();

        ImGui::TableSetColumnIndex(2);
        static CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL protocol =
            CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_TCP;
        assert(sizeof(protocol) >= sizeof(int));
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth());
        ImGui::Combo("###protocol", &protocol, "TCP\0UDP\0");

        ImGui::TableSetColumnIndex(3);
        if (ImGui::Button("Add")) {
            int host, guest;
            if (sscanf(buf, "%d", &host) == 1 &&
                sscanf(buf2, "%d", &guest) == 1) {
                add_net_nat_forward_ports(host, guest, protocol);
                changed = true;
            }
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    return changed;
}

bool MainMenuNetworkView::DrawUdpOptions(bool appearing)
{
    bool changed = false;

    if (appearing) {
        strncpy(remote_addr, g_config.net.udp.remote_addr,
                sizeof(remote_addr) - 1);
        strncpy(local_addr, g_config.net.udp.bind_addr, sizeof(local_addr) - 1);
    }

    float size_ratio = 0.5;
    float width = ImGui::GetColumnWidth() * size_ratio;
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    PrepareComboTitleDescription(
        "Remote Address",
        "Destination addr:port to forward packets to (1.2.3.4:9968)",
        size_ratio);
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText("###remote_host", remote_addr, sizeof(remote_addr))) {
        xemu_settings_set_string(&g_config.net.udp.remote_addr, remote_addr);
    }
    /* Reconnect once editing finishes, not on every keystroke. */
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    TextFieldActivateWithA();
    PrepareComboTitleDescription(
        "Bind Address", "Local addr:port to receive packets on (0.0.0.0:9968)",
        size_ratio);
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText("###local_host", local_addr, sizeof(local_addr))) {
        xemu_settings_set_string(&g_config.net.udp.bind_addr, local_addr);
    }
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    TextFieldActivateWithA();
    ImGui::PopFont();
    return changed;
}

MainMenuSnapshotsView::MainMenuSnapshotsView() : MainMenuTabView()
{
    xemu_snapshots_mark_dirty();

    m_search_regex = NULL;
    m_current_title_id = 0;
}

MainMenuSnapshotsView::~MainMenuSnapshotsView()
{
    g_free(m_search_regex);
}

static bool GetSnapshotThumbnailTexture(XemuSnapshotData *data,
                                        ImTextureID *texture_id,
                                        int *width, int *height)
{
    if (texture_id == NULL || width == NULL || height == NULL) {
        return false;
    }

    if (xemu_hud_uses_d3d12()) {
        if (data != NULL && data->thumbnail_png_size &&
            data->d3d12_thumbnail_resource == NULL) {
            xemu_hud_load_d3d12_png_texture(
                data->thumbnail_png, data->thumbnail_png_size,
                &data->d3d12_thumbnail_resource,
                &data->d3d12_thumbnail_texture_id,
                &data->thumbnail_width, &data->thumbnail_height);
        }

        if (data != NULL && data->d3d12_thumbnail_texture_id != 0) {
            *texture_id = (ImTextureID)data->d3d12_thumbnail_texture_id;
            *width = data->thumbnail_width;
            *height = data->thumbnail_height;
            return *width > 0 && *height > 0;
        }

        uint64_t icon_id = 0;
        if (xemu_hud_get_d3d12_icon_texture(&icon_id, width, height)) {
            *texture_id = (ImTextureID)icon_id;
            return true;
        }

        return false;
    }

#ifdef CONFIG_OPENGL
    if (data != NULL && data->thumbnail_png_size && data->gl_thumbnail == 0) {
        GLuint thumbnail;
        glGenTextures(1, &thumbnail);
        if (xemu_snapshots_load_png_to_texture(
                thumbnail, data->thumbnail_png, data->thumbnail_png_size,
                &data->thumbnail_width, &data->thumbnail_height)) {
            data->gl_thumbnail = thumbnail;
        } else {
            glDeleteTextures(1, &thumbnail);
        }
    }

    GLuint thumbnail = data != NULL && data->gl_thumbnail ?
        data->gl_thumbnail : g_icon_tex;
    if (thumbnail == 0) {
        return false;
    }

    if (data == NULL || data->gl_thumbnail == 0 || data->thumbnail_width <= 0 ||
        data->thumbnail_height <= 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, thumbnail);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, height);
    } else {
        *width = data->thumbnail_width;
        *height = data->thumbnail_height;
    }

    *texture_id = (ImTextureID)(uint64_t)thumbnail;
    return *width > 0 && *height > 0;
#else
    return false;
#endif
}

bool MainMenuSnapshotsView::BigSnapshotButton(QEMUSnapshotInfo *snapshot,
                                              XemuSnapshotData *data,
                                              int current_snapshot_binding)
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();

    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    ImVec2 ts_sub = ImGui::CalcTextSize(snapshot->name);
    ImGui::PopFont();

    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        g_viewport_mgr.Scale(ImVec2(5, 5)));

    ImGui::PushFont(g_font_mgr.m_menu_font_medium);

    ImVec2 ts_title = ImGui::CalcTextSize(snapshot->name);
    ImVec2 thumbnail_size = g_viewport_mgr.Scale(
        ImVec2(XEMU_SNAPSHOT_THUMBNAIL_WIDTH, XEMU_SNAPSHOT_THUMBNAIL_HEIGHT));
    ImVec2 thumbnail_pos(style.FramePadding.x, style.FramePadding.y);
    ImVec2 name_pos(thumbnail_pos.x + thumbnail_size.x +
                        style.FramePadding.x * 2,
                    thumbnail_pos.y);
    ImVec2 title_pos(name_pos.x,
                     name_pos.y + ts_title.y + style.FramePadding.x);
    ImVec2 date_pos(name_pos.x,
                    title_pos.y + ts_title.y + style.FramePadding.x);
    ImVec2 binding_pos(name_pos.x,
                       date_pos.y + ts_title.y + style.FramePadding.x);
    ImVec2 button_size(-FLT_MIN,
                       fmax(thumbnail_size.y + style.FramePadding.y * 2,
                            ts_title.y + ts_sub.y + style.FramePadding.y * 3));

    bool load = ImGui::Button("###button", button_size);

    ImGui::PopFont();

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    draw_list->PushClipRect(p0, p1, true);

    ImTextureID thumbnail = 0;
    int thumbnail_width = 0;
    int thumbnail_height = 0;
    bool has_thumbnail = GetSnapshotThumbnailTexture(
        data, &thumbnail, &thumbnail_width, &thumbnail_height);

    // Draw black background behind thumbnail
    ImVec2 thumbnail_min(p0.x + thumbnail_pos.x, p0.y + thumbnail_pos.y);
    ImVec2 thumbnail_max(thumbnail_min.x + thumbnail_size.x,
                         thumbnail_min.y + thumbnail_size.y);
    draw_list->AddRectFilled(thumbnail_min, thumbnail_max, IM_COL32_BLACK);

    // Draw centered thumbnail image
    if (has_thumbnail) {
        int scaled_width, scaled_height;
        ScaleDimensions(thumbnail_width, thumbnail_height, thumbnail_size.x,
                        thumbnail_size.y, &scaled_width, &scaled_height);
        ImVec2 img_min =
            ImVec2(thumbnail_min.x + (thumbnail_size.x - scaled_width) / 2,
                   thumbnail_min.y + (thumbnail_size.y - scaled_height) / 2);
        ImVec2 img_max =
            ImVec2(img_min.x + scaled_width, img_min.y + scaled_height);
        draw_list->AddImage(thumbnail, img_min, img_max);
    }

    // Snapshot title
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    draw_list->AddText(ImVec2(p0.x + name_pos.x, p0.y + name_pos.y),
                       IM_COL32(255, 255, 255, 255), snapshot->name);
    ImGui::PopFont();

    // Snapshot XBE title name
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    const char *title_name = data->xbe_title_name ? data->xbe_title_name :
                                                    "(Unknown XBE Title Name)";
    draw_list->AddText(ImVec2(p0.x + title_pos.x, p0.y + title_pos.y),
                       IM_COL32(255, 255, 255, 200), title_name);

    // Snapshot date
    g_autoptr(GDateTime) date =
        g_date_time_new_from_unix_local(snapshot->date_sec);
    char *date_buf = g_date_time_format(date, "%Y-%m-%d %H:%M:%S");
    draw_list->AddText(ImVec2(p0.x + date_pos.x, p0.y + date_pos.y),
                       IM_COL32(255, 255, 255, 200), date_buf);
    g_free(date_buf);

    // Snapshot keyboard binding
    if (current_snapshot_binding != -1) {
        char *binding_text =
            g_strdup_printf("Bound to F%d", current_snapshot_binding + 5);
        draw_list->AddText(ImVec2(p0.x + binding_pos.x, p0.y + binding_pos.y),
                           IM_COL32(255, 255, 255, 200), binding_text);
        g_free(binding_text);
    }

    ImGui::PopFont();
    draw_list->PopClipRect();
    ImGui::PopStyleVar(2);

    return load;
}

void MainMenuSnapshotsView::ClearSearch()
{
    m_search_buf.clear();

    if (m_search_regex) {
        g_free(m_search_regex);
        m_search_regex = NULL;
    }
}

int MainMenuSnapshotsView::OnSearchTextUpdate(ImGuiInputTextCallbackData *data)
{
    GError *gerr = NULL;
    MainMenuSnapshotsView *win = (MainMenuSnapshotsView *)data->UserData;

    if (win->m_search_regex) {
        g_free(win->m_search_regex);
        win->m_search_regex = NULL;
    }

    if (data->BufTextLen == 0) {
        return 0;
    }

    char *buf = g_strdup_printf("(.*)%s(.*)", data->Buf);
    win->m_search_regex =
        g_regex_new(buf, (GRegexCompileFlags)0, (GRegexMatchFlags)0, &gerr);
    g_free(buf);
    if (gerr) {
        win->m_search_regex = NULL;
        return 1;
    }

    return 0;
}

void MainMenuSnapshotsView::Draw()
{
    g_snapshot_mgr.Refresh();

    SectionTitle("Snapshots");
    Toggle("Filter by current title",
           &g_config.general.snapshots.filter_current_game,
           "Only display snapshots created while running the currently running "
           "XBE");

    if (g_config.general.snapshots.filter_current_game) {
        struct xbe *xbe = xemu_get_xbe_info();
        if (xbe && xbe->cert) {
            if (xbe->cert->m_titleid != m_current_title_id) {
                char *title_name = g_utf16_to_utf8(xbe->cert->m_title_name, 40,
                                                   NULL, NULL, NULL);
                if (title_name) {
                    m_current_title_name = title_name;
                    g_free(title_name);
                } else {
                    m_current_title_name.clear();
                }

                m_current_title_id = xbe->cert->m_titleid;
            }
        } else {
            m_current_title_name.clear();
            m_current_title_id = 0;
        }
    }

    ImGui::SetNextItemWidth(ImGui::GetColumnWidth() * 0.8);
    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    ImGui::InputTextWithHint("##search", "Search or name new snapshot...",
                             &m_search_buf, ImGuiInputTextFlags_CallbackEdit,
                             &OnSearchTextUpdate, this);
    TextFieldActivateWithA();

    bool snapshot_with_create_name_exists = false;
    for (int i = 0; i < g_snapshot_mgr.m_snapshots_len; ++i) {
        if (g_strcmp0(m_search_buf.c_str(),
                      g_snapshot_mgr.m_snapshots[i].name) == 0) {
            snapshot_with_create_name_exists = true;
            break;
        }
    }

    ImGui::SameLine();
    if (snapshot_with_create_name_exists) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 0, 0, 1));
    }
    if (ImGui::Button(snapshot_with_create_name_exists ? "Replace" : "Create",
                      ImVec2(-FLT_MIN, 0))) {
        xemu_snapshots_save(m_search_buf.empty() ? NULL : m_search_buf.c_str(),
                            NULL);
        ClearSearch();
    }
    if (snapshot_with_create_name_exists) {
        ImGui::PopStyleColor(3);
    }

    if (snapshot_with_create_name_exists && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A snapshot with the name \"%s\" already exists. "
                          "This button will overwrite the existing snapshot.",
                          m_search_buf.c_str());
    }
    ImGui::PopFont();

    bool at_least_one_snapshot_displayed = false;

    for (int i = g_snapshot_mgr.m_snapshots_len - 1; i >= 0; i--) {
        if (g_config.general.snapshots.filter_current_game &&
            g_snapshot_mgr.m_extra_data[i].xbe_title_name &&
            m_current_title_name.size() &&
            strcmp(m_current_title_name.c_str(),
                   g_snapshot_mgr.m_extra_data[i].xbe_title_name)) {
            continue;
        }

        if (m_search_regex) {
            GMatchInfo *match;
            bool keep_entry = false;

            g_regex_match(m_search_regex, g_snapshot_mgr.m_snapshots[i].name,
                          (GRegexMatchFlags)0, &match);
            keep_entry |= g_match_info_matches(match);
            g_match_info_free(match);

            if (g_snapshot_mgr.m_extra_data[i].xbe_title_name) {
                g_regex_match(m_search_regex,
                              g_snapshot_mgr.m_extra_data[i].xbe_title_name,
                              (GRegexMatchFlags)0, &match);
                keep_entry |= g_match_info_matches(match);
                g_free(match);
            }

            if (!keep_entry) {
                continue;
            }
        }

        QEMUSnapshotInfo *snapshot = &g_snapshot_mgr.m_snapshots[i];
        XemuSnapshotData *data = &g_snapshot_mgr.m_extra_data[i];

        int current_snapshot_binding = -1;
        for (int j = 0; j < 4; ++j) {
            if (g_strcmp0(*(g_snapshot_shortcut_index_key_map[j]),
                          snapshot->name) == 0) {
                assert(current_snapshot_binding == -1);
                current_snapshot_binding = j;
            }
        }

        ImGui::PushID(i);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool load = BigSnapshotButton(snapshot, data, current_snapshot_binding);

        // FIXME: Provide context menu control annotation
        if (ImGui::IsItemHovered() &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft)) {
            ImGui::SetNextWindowPos(pos);
            ImGui::OpenPopup("Snapshot Options");
        }

        DrawSnapshotContextMenu(snapshot, data, current_snapshot_binding);

        ImGui::PopID();

        if (load) {
            ActionLoadSnapshotChecked(snapshot->name);
        }

        at_least_one_snapshot_displayed = true;
    }

    if (!at_least_one_snapshot_displayed) {
        ImGui::Dummy(g_viewport_mgr.Scale(ImVec2(0, 16)));
        const char *msg;
        if (g_snapshot_mgr.m_snapshots_len) {
            if (!m_search_buf.empty()) {
                msg = "Press Create to create new snapshot";
            } else {
                msg = "No snapshots match filter criteria";
            }
        } else {
            msg = "No snapshots to display";
        }
        ImVec2 dim = ImGui::CalcTextSize(msg);
        ImVec2 cur = ImGui::GetCursorPos();
        ImGui::SetCursorPosX(cur.x + (ImGui::GetColumnWidth() - dim.x) / 2);
        ImGui::TextColored(ImVec4(0.94f, 0.94f, 0.94f, 0.70f), "%s", msg);
    }
}

void MainMenuSnapshotsView::DrawSnapshotContextMenu(
    QEMUSnapshotInfo *snapshot, XemuSnapshotData *data,
    int current_snapshot_binding)
{
    if (!ImGui::BeginPopupContextItem("Snapshot Options")) {
        return;
    }

    if (ImGui::MenuItem("Load")) {
        ActionLoadSnapshotChecked(snapshot->name);
    }

    if (ImGui::BeginMenu("Keybinding")) {
        for (int i = 0; i < 4; ++i) {
            char *item_name = g_strdup_printf("Bind to F%d", i + 5);

            if (ImGui::MenuItem(item_name)) {
                if (current_snapshot_binding >= 0) {
                    xemu_settings_set_string(g_snapshot_shortcut_index_key_map
                                                 [current_snapshot_binding],
                                             "");
                }
                xemu_settings_set_string(g_snapshot_shortcut_index_key_map[i],
                                         snapshot->name);
                current_snapshot_binding = i;

                ImGui::CloseCurrentPopup();
            }

            g_free(item_name);
        }

        if (current_snapshot_binding >= 0) {
            if (ImGui::MenuItem("Unbind")) {
                xemu_settings_set_string(
                    g_snapshot_shortcut_index_key_map[current_snapshot_binding],
                    "");
                current_snapshot_binding = -1;
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();

    Error *err = NULL;

    if (ImGui::MenuItem("Replace")) {
        xemu_snapshots_save(snapshot->name, &err);
    }

    if (ImGui::MenuItem("Delete")) {
        xemu_snapshots_delete(snapshot->name, &err);
    }

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }

    ImGui::EndPopup();
}

#ifdef CONFIG_UWP
// Streams a file to its destination through a temporary file.
static bool CopySystemFile(const std::string &source,
                           const std::string &destination)
{
    std::string temporary = destination + ".tmp";
    FILE *in = g_fopen(source.c_str(), "rb");
    if (in == nullptr) {
        return false;
    }
    FILE *out = g_fopen(temporary.c_str(), "wb");
    if (out == nullptr) {
        fclose(in);
        return false;
    }

    std::vector<char> buffer(1 << 20);
    bool ok = true;
    size_t count;
    while ((count = fread(buffer.data(), 1, buffer.size(), in)) > 0) {
        if (fwrite(buffer.data(), 1, count, out) != count) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    if (ok && g_rename(temporary.c_str(), destination.c_str()) != 0) {
        ok = false;
    }
    if (!ok) {
        g_remove(temporary.c_str());
    }
    return ok;
}

// Points a system file setting at a new location, creating its folder. When
// the new location has no file yet the current one is copied there first; the
// hard disk image copies on a worker thread, and only from a stopped machine
// so the image is consistent.
static void SelectSystemFile(const char **setting, const char *path,
                             bool large, std::function<void()> applied)
{
    std::string destination = path ? path : "";
    std::string source = *setting ? *setting : "";
    auto apply = [setting, destination, applied]() {
        xemu_settings_set_string(setting, destination.c_str());
        applied();
    };

    if (destination.empty()) {
        apply();
        return;
    }
    gchar *folder = g_path_get_dirname(destination.c_str());
    g_mkdir_with_parents(folder, 0755);
    g_free(folder);

    if (source.empty() ||
        g_ascii_strcasecmp(source.c_str(), destination.c_str()) == 0 ||
        !g_file_test(source.c_str(), G_FILE_TEST_IS_REGULAR) ||
        g_file_test(destination.c_str(), G_FILE_TEST_EXISTS)) {
        apply();
        return;
    }

    if (!large) {
        if (CopySystemFile(source, destination)) {
            apply();
        } else {
            xemu_queue_notification("Could not copy the file to the new "
                                    "location");
        }
        return;
    }

    if (runstate_is_running()) {
        xemu_queue_notification("Stop the running game before moving the "
                                "hard disk image");
        return;
    }
    xemu_queue_notification("Copying the hard disk image...");
    std::thread([source, destination, apply]() {
        bool ok = CopySystemFile(source, destination);
        RunOnMainThread([ok, apply]() {
            if (ok) {
                apply();
                xemu_queue_notification("Hard disk image copied");
            } else {
                xemu_queue_notification("Could not copy the hard disk image");
            }
        });
    }).detach();
}
#endif

MainMenuSystemView::MainMenuSystemView() : m_dirty(false)
{
}

void MainMenuSystemView::Draw()
{
    static const SDL_DialogFileFilter rom_file_filters[] = {
        { ".bin Files", "bin" },
        { ".rom Files", "rom" },
        { "All Files", "*" }
    };
    static const SDL_DialogFileFilter qcow_file_filters[] = {
        { ".qcow2 Files", "qcow2" },
        { "All Files", "*" }
    };

    if (m_dirty) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1),
                           "Application restart required to apply settings");
    }

    if ((int)g_config.sys.avpack == CONFIG_SYS_AVPACK_NONE) {
        ImGui::TextColored(ImVec4(1,0,0,1), "Setting AV Pack to NONE disables video output.");
    }

    SectionTitle("System Configuration");

    if (ChevronCombo(
            "System Memory", &g_config.sys.mem_limit,
            "64 MiB (Default)\0"
            "128 MiB\0",
            "Increase to 128 MiB for debug or homebrew applications")) {
        m_dirty = true;
    }

    if (ChevronCombo(
            "AV Pack", &g_config.sys.avpack,
            "SCART\0HDTV (Default)\0VGA\0RFU\0S-Video\0Composite\0None\0",
            "Select the attached AV pack")) {
        m_dirty = true;
    }

    SectionTitle("Files");
#ifdef CONFIG_UWP
    auto file_changed = [this]() {
        m_dirty = true;
        g_main_menu.UpdateAboutViewConfigInfo();
    };
    StorageFileSelector("MCPX Boot ROM", g_config.sys.files.bootrom_path,
                        XEMU_UWP_DEV_FILES_SYSTEM_DIR, XEMU_UWP_USB_SYSTEM_DIR,
                        "mcpx_1.0.bin", rom_file_filters, 3,
                        [file_changed](const char *path) {
                            SelectSystemFile(&g_config.sys.files.bootrom_path,
                                             path, false, file_changed);
                        });
    StorageFileSelector("Flash ROM (BIOS)", g_config.sys.files.flashrom_path,
                        XEMU_UWP_DEV_FILES_SYSTEM_DIR, XEMU_UWP_USB_SYSTEM_DIR,
                        "Complex_4627v1.03.bin", rom_file_filters, 3,
                        [file_changed](const char *path) {
                            SelectSystemFile(&g_config.sys.files.flashrom_path,
                                             path, false, file_changed);
                        });
    StorageFileSelector("Hard Disk", g_config.sys.files.hdd_path,
                        XEMU_UWP_DEV_FILES_SYSTEM_DIR, XEMU_UWP_USB_SYSTEM_DIR,
                        "xbox_hdd.qcow2", qcow_file_filters, 2,
                        [file_changed](const char *path) {
                            SelectSystemFile(&g_config.sys.files.hdd_path,
                                             path, true, file_changed);
                        });
    StorageFileSelector("EEPROM", g_config.sys.files.eeprom_path,
                        XEMU_UWP_DEV_FILES_SYSTEM_DIR, XEMU_UWP_USB_SYSTEM_DIR,
                        "eeprom.bin", rom_file_filters, 3,
                        [file_changed](const char *path) {
                            SelectSystemFile(&g_config.sys.files.eeprom_path,
                                             path, false, file_changed);
                        });
#else
    FilePicker("MCPX Boot ROM", g_config.sys.files.bootrom_path,
               rom_file_filters, 3, false, [this](const char *path) {
                   xemu_settings_set_string(&g_config.sys.files.bootrom_path, path);
                   m_dirty = true;
                   g_main_menu.UpdateAboutViewConfigInfo();
               });
    FilePicker("Flash ROM (BIOS)", g_config.sys.files.flashrom_path,
               rom_file_filters, 3, false, [this](const char *path) {
                   xemu_settings_set_string(&g_config.sys.files.flashrom_path, path);
                   m_dirty = true;
                   g_main_menu.UpdateAboutViewConfigInfo();
               });
    FilePicker("Hard Disk", g_config.sys.files.hdd_path,
               qcow_file_filters, 2, false, [this](const char *path) {
                   xemu_settings_set_string(&g_config.sys.files.hdd_path, path);
                   m_dirty = true;
               });
    FilePicker("EEPROM", g_config.sys.files.eeprom_path,
               rom_file_filters, 3, false, [this](const char *path) {
                   xemu_settings_set_string(&g_config.sys.files.eeprom_path, path);
                   m_dirty = true;
               });
#endif
}

MainMenuAboutView::MainMenuAboutView() : m_config_info_text{ NULL }
{
}

void MainMenuAboutView::UpdateConfigInfoText()
{
    if (m_config_info_text) {
        g_free(m_config_info_text);
    }

    gchar *bootrom_checksum =
        GetFileMD5Checksum(g_config.sys.files.bootrom_path);
    if (!bootrom_checksum) {
        bootrom_checksum = g_strdup("None");
    }

    gchar *flash_rom_checksum =
        GetFileMD5Checksum(g_config.sys.files.flashrom_path);
    if (!flash_rom_checksum) {
        flash_rom_checksum = g_strdup("None");
    }

    m_config_info_text = g_strdup_printf("MCPX Boot ROM MD5 Hash:        %s\n"
                                         "Flash ROM (BIOS) MD5 Hash:     %s",
                                         bootrom_checksum, flash_rom_checksum);
    g_free(bootrom_checksum);
    g_free(flash_rom_checksum);
}

void MainMenuAboutView::Draw()
{
    static const char *build_info_text = NULL;
    if (build_info_text == NULL) {
        build_info_text =
            g_strdup_printf("Version:      %s\n"
                            "Commit:       %s\n"
                            "Date:         %s",
                            xemu_version, xemu_commit, xemu_date);
    }

    static const char *sys_info_text = NULL;
    if (sys_info_text == NULL) {
        const char *gpu_vendor = NULL;
        const char *gpu_renderer = NULL;
        const char *gpu_version = NULL;
        const char *gpu_shader_version = NULL;
        xemu_hud_get_renderer_info(&gpu_vendor, &gpu_renderer, &gpu_version,
                                   &gpu_shader_version);
        sys_info_text = g_strdup_printf(
            "CPU:          %s\nOS Platform:  %s\nOS Version:   "
            "%s\nManufacturer: %s\n"
            "GPU Model:    %s\nDriver:       %s\nShader:       %s",
            xemu_get_cpu_info(), SDL_GetPlatform(), xemu_get_os_info(),
            gpu_vendor, gpu_renderer, gpu_version, gpu_shader_version);
    }

    if (m_config_info_text == NULL) {
        UpdateConfigInfoText();
    }

    Logo();

    SectionTitle("Build Information");
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGui::InputTextMultiline("##build_info", (char *)build_info_text,
                              strlen(build_info_text) + 1,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();

    SectionTitle("System Information");
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGui::InputTextMultiline("###systeminformation", (char *)sys_info_text,
                              strlen(sys_info_text) + 1,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();

    SectionTitle("Config Information");
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGui::InputTextMultiline("##config_info", (char *)m_config_info_text,
                              strlen(m_config_info_text) + 1,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();

    SectionTitle("Community");

    ImGui::TextUnformatted("Join the community on");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f * g_viewport_mgr.m_scale);
    if (ImGui::BeginCombo("##community_discords", "Discord")) {
        if (ImGui::Selectable("Xbox Emulation Hub")) {
            SDL_OpenURL("https://discord.gg/WCmxvvxHqu");
        }
        if (ImGui::Selectable("Revives Community Server")) {
            SDL_OpenURL("https://discord.com/invite/f82Tr22vRZ");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Text("for support and discussion");

    ImGui::TextUnformatted("Visit the");
    ImGui::SameLine();
    if (ImGui::SmallButton("Emulation Revival app store")) {
        SDL_OpenURL("https://emulationrevival.github.io/");
    }
    ImGui::SameLine();
    ImGui::Text("for more Xbox apps and emulators");
}

// SetKeyboardFocusHere() also activates its target, which would open a
// combo or flip a toggle; this only moves the navigation focus there.
static void FocusNextItem()
{
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;

    ImGui::SetNavWindow(window);
    ImGui::NavMoveRequestSubmit(
        ImGuiDir_None, ImGuiDir_Down,
        ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi |
            ImGuiNavMoveFlags_NoSelect,
        window->Appearing ? ImGuiScrollFlags_KeepVisibleEdgeX |
                                ImGuiScrollFlags_AlwaysCenterY :
                            ImGuiScrollFlags_KeepVisibleEdgeX |
                                ImGuiScrollFlags_KeepVisibleEdgeY);
    g.NavTabbingDir = 1;
    g.NavTabbingCounter = 1;
}

MainMenuTabButton::MainMenuTabButton(std::string text, std::string icon)
    : m_icon(icon), m_text(text)
{
}

bool MainMenuTabButton::Draw(bool selected)
{
    ImGuiStyle &style = ImGui::GetStyle();

    ImU32 col = selected ?
                    ImGui::GetColorU32(style.Colors[ImGuiCol_ButtonHovered]) :
                    IM_COL32(0, 0, 0, 0);

    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          selected ? col : IM_COL32(32, 32, 32, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          selected ? col : IM_COL32(32, 32, 32, 255));
    int p = ImGui::GetTextLineHeight() * 0.5;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(p, p));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5));
    ImGui::PushFont(g_font_mgr.m_menu_font);

    ImVec2 button_size = ImVec2(-FLT_MIN, 0);
    auto text = string_format("%s %s", m_icon.c_str(), m_text.c_str());
    ImGui::PushID(this);
    bool status = ImGui::Button(text.c_str(), button_size);
    ImGui::PopID();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    return status;
}

MainMenuScene::MainMenuScene()
    : m_animation(0.12, 0.12), m_general_button("General", ICON_FA_GEARS),
      m_input_button("Input", ICON_FA_GAMEPAD),
      m_display_button("Display", ICON_FA_TV),
      m_audio_button("Audio", ICON_FA_VOLUME_HIGH),
      m_network_button("Network", ICON_FA_NETWORK_WIRED),
      m_snapshots_button("Snapshots", ICON_FA_CLOCK_ROTATE_LEFT),
      m_system_button("System", ICON_FA_MICROCHIP),
      m_about_button("About", ICON_FA_CIRCLE_INFO)
{
    m_had_focus_last_frame = false;
    m_focus_view = false;
    m_content_active = false;
    m_focus_tab = false;
    m_had_active_item_last_frame = false;
    m_tabs.push_back(&m_general_button);
    m_tabs.push_back(&m_input_button);
    m_tabs.push_back(&m_display_button);
    m_tabs.push_back(&m_audio_button);
    m_tabs.push_back(&m_network_button);
    m_tabs.push_back(&m_snapshots_button);
    m_tabs.push_back(&m_system_button);
    m_tabs.push_back(&m_about_button);

    m_views.push_back(&m_general_view);
    m_views.push_back(&m_input_view);
    m_views.push_back(&m_display_view);
    m_views.push_back(&m_audio_view);
    m_views.push_back(&m_network_view);
    m_views.push_back(&m_snapshots_view);
    m_views.push_back(&m_system_view);
    m_views.push_back(&m_about_view);

    m_current_view_index = 0;
    m_next_view_index = m_current_view_index;
}

void MainMenuScene::ShowSettings()
{
    SetNextViewIndexWithFocus(g_config.general.last_viewed_menu_index);
}

void MainMenuScene::ShowSnapshots()
{
    SetNextViewIndexWithFocus(5);
}

void MainMenuScene::ShowSystem()
{
    SetNextViewIndexWithFocus(6);
}

void MainMenuScene::ShowAbout()
{
    SetNextViewIndexWithFocus(7);
}

void MainMenuScene::SetNextViewIndexWithFocus(int i)
{
    m_focus_view = true;
    m_content_active = true;
    m_focus_tab = false;
    SetNextViewIndex(i);

    if (!g_scene_mgr.IsDisplayingScene()) {
        g_scene_mgr.PushScene(*this);
    }
}

void MainMenuScene::Show()
{
    // Opened without a specific page: start on the category list.
    if (!m_focus_view) {
        m_content_active = false;
        m_focus_tab = true;
    }
    m_had_focus_last_frame = false;
    m_background.Show();
    m_nav_control_view.Show();
    m_animation.EaseIn();
}

void MainMenuScene::Hide()
{
#ifdef CONFIG_UWP
    xemu_settings_save();
#endif
    m_views[m_current_view_index]->Hide();
    m_background.Hide();
    m_nav_control_view.Hide();
    m_animation.EaseOut();
}

bool MainMenuScene::IsAnimating()
{
    return m_animation.IsAnimating();
}

void MainMenuScene::SetNextViewIndex(int i)
{
    m_views[m_current_view_index]->Hide();
    m_next_view_index = i % m_tabs.size();
    g_config.general.last_viewed_menu_index = i;
}

void MainMenuScene::HandleInput()
{
    bool nofocus = !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
    bool focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows |
                                        ImGuiFocusedFlags_NoPopupHierarchy);

    if (nofocus) {
        Hide();
        return;
    }

    // Require focus for two frames: B that just closed a popup or ended a
    // text edit is not also a back press.
    bool item_active = ImGui::IsAnyItemActive();
    bool back = focus && m_had_focus_last_frame && !item_active &&
                !m_had_active_item_last_frame && !IsInputRebinding() &&
                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    if (back) {
        if (m_content_active) {
            // Back from a page returns to its category.
            m_content_active = false;
            m_focus_tab = true;
        } else {
            Hide();
            return;
        }
    }

    if (focus && m_had_focus_last_frame) {
        int step = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
            step = m_tabs.size() - 1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) {
            step = 1;
        }
        if (step != 0) {
            SetNextViewIndex((m_current_view_index + step) % m_tabs.size());
            if (m_content_active) {
                m_focus_view = true;
            }
        }
    }

    m_had_focus_last_frame = focus;
    m_had_active_item_last_frame = item_active;
}

void MainMenuScene::UpdateAboutViewConfigInfo()
{
    m_about_view.UpdateConfigInfoText();
}

bool MainMenuScene::ConsumeRebindEvent(SDL_Event *event)
{
    return m_input_view.ConsumeRebindEvent(event);
}

bool MainMenuScene::IsInputRebinding()
{
    return m_input_view.IsInputRebinding();
}

bool MainMenuScene::Draw()
{
    m_animation.Step();
    m_background.Draw();
    m_nav_control_view.Draw();

    ImGuiIO &io = ImGui::GetIO();
    float t = m_animation.GetSinInterpolatedValue();
    float window_alpha = t;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, window_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    ImVec4 extents = g_viewport_mgr.GetExtents();
    ImVec2 window_pos = ImVec2(io.DisplaySize.x / 2, extents.y);
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, ImVec2(0.5, 0));

    ImVec2 max_size = g_viewport_mgr.Scale(ImVec2(800, 0));
    float x = fmin(io.DisplaySize.x - extents.x - extents.z, max_size.x);
    float y = io.DisplaySize.y - extents.y - extents.w;
    ImGui::SetNextWindowSize(ImVec2(x, y));

    if (ImGui::Begin("###MainWindow", NULL,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoSavedSettings)) {
        //
        // Nav menu
        //

        float width = ImGui::GetWindowWidth();
        float nav_width = width * 0.3;
        float content_width = width - nav_width;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(26, 26, 26, 255));

        ImGui::BeginChild("###MainWindowNav", ImVec2(nav_width, -1), true,
                          ImGuiWindowFlags_NavFlattened);

        bool move_focus_to_tab = m_focus_tab && !m_content_active;
        m_focus_tab = false;
        if (m_current_view_index != m_next_view_index) {
            m_current_view_index = m_next_view_index;
            if (!m_focus_view && !m_content_active) {
                move_focus_to_tab = true;
            }
        }

        // Only the active pane takes directional navigation, so the
        // controller cannot drift between the category list and a page.
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, m_content_active);
        int i = 0;
        for (auto &button : m_tabs) {
            bool pressed = button->Draw(i == m_current_view_index);
            if (move_focus_to_tab && i == m_current_view_index) {
                ImGui::FocusItem();
                move_focus_to_tab = false;
            }
            if (pressed) {
                // A opens the page.
                if (i != m_current_view_index) {
                    SetNextViewIndex(i);
                }
                m_content_active = true;
                m_focus_view = true;
            } else if (!m_content_active && i != m_current_view_index &&
                       g_input_mgr.IsNavigatingWithController() &&
                       ImGui::IsItemFocused()) {
                // The highlighted category's page shows alongside.
                SetNextViewIndex(i);
            }
            if (i == m_current_view_index) {
                ImGui::SetItemDefaultFocus();
            }
            i++;
        }
        ImGui::PopItemFlag();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        //
        // Content
        //
        ImGui::SameLine();
        int s = ImGui::GetTextLineHeight() * 0.75;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s, s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(s, s));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                            6 * g_viewport_mgr.m_scale);

        ImGui::PushID(m_current_view_index);
        ImGui::BeginChild("###MainWindowContent", ImVec2(content_width, -1),
                          true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding |
                              ImGuiWindowFlags_NavFlattened);

        if (!g_input_mgr.IsNavigatingWithController()) {
            // Close button
            ImGui::PushFont(g_font_mgr.m_menu_font);
            ImGuiStyle &style = ImGui::GetStyle();
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 128));
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32_BLACK_TRANS);
            ImVec2 pos = ImGui::GetCursorPos();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x -
                                 style.FramePadding.x * 2.0f -
                                 ImGui::GetTextLineHeight());
            if (ImGui::Button(ICON_FA_XMARK)) {
                Hide();
            }
            ImGui::SetCursorPos(pos);
            ImGui::PopStyleColor(2);
            ImGui::PopFont();
        }

        ImGui::PushFont(g_font_mgr.m_default_font);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, !m_content_active);
        if (m_focus_view && m_current_view_index == m_next_view_index) {
            FocusNextItem();
            m_focus_view = false;
        }
        m_views[m_current_view_index]->Draw();
        ImGui::PopItemFlag();

        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleVar(3);

        HandleInput();
    }
    ImGui::End();
    ImGui::PopStyleVar(5);

    return !m_animation.IsComplete();
}
