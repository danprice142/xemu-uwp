/*
 * xemu UWP on-screen keyboard helper
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"

#include "ui/uwp-osk.h"

#ifdef CONFIG_UWP

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

namespace {

namespace vm = winrt::Windows::UI::ViewManagement;

bool g_osk_visible;
bool g_osk_logged_error;
bool g_osk_logged_failure;

} // namespace

extern "C" void xemu_uwp_osk_set_visible(bool visible)
{
    if (visible == g_osk_visible) {
        return;
    }

    try {
        vm::InputPane pane = vm::InputPane::GetForCurrentView();
        if (!pane) {
            return;
        }
        if (visible) {
            /* TryShow can transiently fail while focus is moving into an
             * ImGui text field. Only latch success so the next UI frame can
             * retry instead of permanently suppressing the Xbox OSK. */
            bool shown = pane.TryShow();
            if (shown || !g_osk_logged_failure) {
                fprintf(stderr, "xemu UWP: on-screen keyboard %s\n",
                        shown ? "shown" : "not shown");
                g_osk_logged_failure = !shown;
            }
            if (shown) {
                g_osk_visible = true;
            }
        } else {
            pane.TryHide();
            g_osk_visible = false;
        }
    } catch (winrt::hresult_error const &e) {
        if (!g_osk_logged_error) {
            fprintf(stderr,
                    "xemu UWP: on-screen keyboard unavailable: "
                    "HRESULT=0x%08x\n",
                    (unsigned int)e.code().value);
            g_osk_logged_error = true;
        }
    }
}

#else

extern "C" void xemu_uwp_osk_set_visible(bool visible)
{
    (void)visible;
}

#endif
