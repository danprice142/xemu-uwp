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

#include "qemu/osdep.h"

#ifdef CONFIG_UWP

#include "ui/uwp-lifecycle.h"
#include "ui/xemu-main.h"
#include "ui/xemu-settings.h"

#include <mutex>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.ExtendedExecution.Foreground.h>

namespace {

namespace fg = winrt::Windows::ApplicationModel::ExtendedExecution::Foreground;

std::mutex g_lifecycle_mutex;
fg::ExtendedExecutionForegroundSession g_foreground_session{ nullptr };
winrt::event_token g_revoked_token{};
bool g_in_background;
bool g_logged_init;

const char *foreground_result_name(fg::ExtendedExecutionForegroundResult result)
{
    switch (result) {
    case fg::ExtendedExecutionForegroundResult::Allowed:
        return "Allowed";
    case fg::ExtendedExecutionForegroundResult::Denied:
        return "Denied";
    default:
        return "unknown";
    }
}

const char *foreground_revoked_reason_name(
    fg::ExtendedExecutionForegroundRevokedReason reason)
{
    switch (reason) {
    case fg::ExtendedExecutionForegroundRevokedReason::Resumed:
        return "Resumed";
    case fg::ExtendedExecutionForegroundRevokedReason::SystemPolicy:
        return "SystemPolicy";
    default:
        return "unknown";
    }
}

void clear_foreground_session_locked(void)
{
    if (g_foreground_session) {
        if (g_revoked_token.value != 0) {
            g_foreground_session.Revoked(g_revoked_token);
            g_revoked_token.value = 0;
        }
        g_foreground_session.Close();
        g_foreground_session = nullptr;
    }
}

void on_foreground_session_revoked(
    winrt::Windows::Foundation::IInspectable const &,
    fg::ExtendedExecutionForegroundRevokedEventArgs const &args)
{
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    fprintf(stderr, "xemu UWP: extended execution revoked: reason=%s\n",
            foreground_revoked_reason_name(args.Reason()));
    g_revoked_token.value = 0;
    g_foreground_session = nullptr;
}

void request_foreground_session(void)
{
    fg::ExtendedExecutionForegroundSession session{ nullptr };
    winrt::event_token revoked_token{};
    fg::ExtendedExecutionForegroundResult result;

    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (g_foreground_session) {
            return;
        }
    }

    try {
        session = fg::ExtendedExecutionForegroundSession();
        session.Description(L"xemu emulation session");
        session.Reason(fg::ExtendedExecutionForegroundReason::Unconstrained);
        revoked_token = session.Revoked(&on_foreground_session_revoked);
        result = session.RequestExtensionAsync().get();

        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        fprintf(stderr, "xemu UWP: extended execution request result=%s\n",
                foreground_result_name(result));
        if (result == fg::ExtendedExecutionForegroundResult::Allowed) {
            clear_foreground_session_locked();
            g_foreground_session = session;
            g_revoked_token = revoked_token;
        } else {
            session.Revoked(revoked_token);
            session.Close();
        }
    } catch (winrt::hresult_error const &e) {
        fprintf(stderr,
                "xemu UWP: failed to start extended execution request: "
                "HRESULT=0x%08x\n",
                (unsigned int)e.code().value);
        if (session) {
            if (revoked_token.value != 0) {
                session.Revoked(revoked_token);
            }
            session.Close();
        }
    }
}

} // namespace

extern "C" void xemu_uwp_lifecycle_init(void)
{
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (!g_logged_init) {
            fprintf(stderr,
                    "xemu UWP: requesting unconstrained foreground extended "
                    "execution\n");
            g_logged_init = true;
        }
    }
    request_foreground_session();
}

extern "C" void xemu_uwp_lifecycle_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    clear_foreground_session_locked();
}

extern "C" void xemu_uwp_lifecycle_handle_sdl_event(const SDL_Event *event)
{
    if (event == nullptr) {
        return;
    }

    switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        /* Quitting from the Xbox Home menu suspends the app, then ends the
         * process without further events. */
        xemu_uwp_suspend_vm();
        xemu_settings_save();
        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            g_in_background = true;
        }
        fprintf(stderr, "xemu UWP: SDL_EVENT_WILL_ENTER_BACKGROUND\n");
        break;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        fprintf(stderr, "xemu UWP: SDL_EVENT_DID_ENTER_BACKGROUND\n");
        break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        fprintf(stderr, "xemu UWP: SDL_EVENT_WILL_ENTER_FOREGROUND\n");
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            g_in_background = false;
        }
        fprintf(stderr, "xemu UWP: SDL_EVENT_DID_ENTER_FOREGROUND\n");
        request_foreground_session();
        xemu_uwp_resume_vm();
        break;
    case SDL_EVENT_TERMINATING:
        xemu_settings_save();
        fprintf(stderr, "xemu UWP: SDL_EVENT_TERMINATING\n");
        break;
    default:
        break;
    }
}

extern "C" bool xemu_uwp_lifecycle_is_in_background(void)
{
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    return g_in_background;
}

#else

#include "ui/uwp-lifecycle.h"

extern "C" void xemu_uwp_lifecycle_init(void)
{
}

extern "C" void xemu_uwp_lifecycle_shutdown(void)
{
}

extern "C" void xemu_uwp_lifecycle_handle_sdl_event(const SDL_Event *event)
{
    (void)event;
}

extern "C" bool xemu_uwp_lifecycle_is_in_background(void)
{
    return false;
}

#endif
