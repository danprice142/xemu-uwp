/*
 * xemu native UWP host entry point
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"

#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>

/*
 * SDL's public platform probe only enables WinRT automatically for MSVC.
 * This build links to an SDL3-UWP DLL from MinGW, so normalize this one
 * entry source before including SDL_main.h and let SDL emit its WinRT
 * startup wrapper.
 */
#include <SDL3/SDL_platform_defines.h>
#ifdef SDL_PLATFORM_WIN32
#undef SDL_PLATFORM_WIN32
#endif
#ifndef SDL_PLATFORM_WINRT
#define SDL_PLATFORM_WINRT 1
#endif
#include <SDL3/SDL_main.h>

#include "xemu-main.h"
#include "xemu-game-library.h"
#include "xui/actions.hh"

extern "C" {
#include "xemu-launch-uri.h"
}

namespace activation = winrt::Windows::ApplicationModel::Activation;

namespace {

static char xemu_uwp_argv0[] = "xemu";
static char *xemu_uwp_argv[] = {
    xemu_uwp_argv0,
    nullptr,
};

std::mutex g_activation_lock;
std::string g_pending_activation_uri;

std::string protocol_uri(const activation::IActivatedEventArgs &args)
{
    if (args == nullptr ||
        args.Kind() != activation::ActivationKind::Protocol) {
        return std::string();
    }
    auto uri = args.as<activation::ProtocolActivatedEventArgs>().Uri();
    return uri != nullptr ? winrt::to_string(uri.RawUri()) : std::string();
}

/* The launch request this process was started for, if any. */
char *cold_start_launch_uri()
{
    std::string uri;
    try {
        uri = protocol_uri(
            winrt::Windows::ApplicationModel::AppInstance::
                GetActivatedEventArgs());
    } catch (...) {
        return nullptr;
    }

    /* Anything that is not a launch request would reach QEMU's command line
     * as a disk image. */
    XemuLaunchUri launch = {};
    bool valid = !uri.empty() && xemu_launch_uri_parse(uri.c_str(), &launch);
    xemu_launch_uri_clear(&launch);
    return valid ? g_strdup(uri.c_str()) : nullptr;
}

/* Activations that reach a running instance carry the next launch request.
 * The handler runs on the view thread without the main loop lock, so it only
 * queues the URI. */
void queue_running_activation(
    const winrt::Windows::ApplicationModel::Core::CoreApplicationView &,
    const activation::IActivatedEventArgs &args)
{
    try {
        auto previous = args.PreviousExecutionState();
        if (previous != activation::ApplicationExecutionState::Running &&
            previous != activation::ApplicationExecutionState::Suspended) {
            return;
        }
        std::string uri = protocol_uri(args);
        if (!uri.empty()) {
            std::lock_guard<std::mutex> lock(g_activation_lock);
            g_pending_activation_uri = std::move(uri);
        }
    } catch (...) {
    }
}

} // namespace

void xemu_uwp_process_activation(void)
{
    std::string uri;
    {
        std::lock_guard<std::mutex> lock(g_activation_lock);
        uri.swap(g_pending_activation_uri);
    }
    if (uri.empty()) {
        return;
    }

    XemuLaunchUri launch = {};
    if (!xemu_launch_uri_parse(uri.c_str(), &launch)) {
        fprintf(stderr, "xemu UWP: ignoring protocol activation '%s'\n",
                uri.c_str());
        return;
    }
    if (launch.dvd_path != NULL && launch.dvd_path[0] != '\0') {
        fprintf(stderr, "xemu UWP: protocol launch DVD path: %s\n",
                launch.dvd_path);
        xemu_game_library_startup_pending = false;
        ActionLoadDiscFile(launch.dvd_path);
    }
    xemu_launch_uri_clear(&launch);
}

int main(int argc, char **argv)
{
    std::vector<char *> launch_argv;
    char *activation_uri = cold_start_launch_uri();

    if (argc <= 0 || argv == nullptr) {
        argc = 1;
        argv = xemu_uwp_argv;
    }

    if (activation_uri != nullptr) {
        launch_argv.reserve((size_t)argc + 2);
        for (int i = 0; i < argc; i++) {
            launch_argv.push_back(argv[i]);
        }
        launch_argv.push_back(activation_uri);
        launch_argv.push_back(nullptr);
        argc = (int)launch_argv.size() - 1;
        argv = launch_argv.data();
    }

    auto view =
        winrt::Windows::ApplicationModel::Core::CoreApplication::GetCurrentView();
    winrt::event_token token = view.Activated(queue_running_activation);
    int status = xemu_main(argc, argv);
    view.Activated(token);
    return status;
}
