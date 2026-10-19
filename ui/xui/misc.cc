//
// xemu User Interface
//
// Copyright (C) 2026 Matt Borgerson
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
#include "misc.hh"
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_platform_defines.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#ifdef CONFIG_UWP
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#endif

void RunOnMainThread(std::function<void()> &&func)
{
    auto p = new std::function<void()>(std::move(func));
    if (!SDL_RunOnMainThread(
            [](void *userdata) {
                std::unique_ptr<std::function<void()>> f(
                    static_cast<std::function<void()> *>(userdata));
                xemu_main_loop_lock();
                (*f)();
                xemu_main_loop_unlock();
            },
            p, false)) {
        delete p;
    }
}

#ifndef CONFIG_UWP
static void FileDialogCallbackWrapper(void *userdata,
                                      const char *const *filelist, int filter)
{
    std::unique_ptr<FileDialogCallback> callback(
        static_cast<FileDialogCallback *>(userdata));
    if (filelist && filelist[0]) {
        std::string path = filelist[0];
        RunOnMainThread([callback = std::move(*callback),
                         path = std::move(path)]() { callback(path.c_str()); });
    }
}

// Workaround SDL3 default_location handling:
// - Linux: only supports folder paths, not file paths as documented
// - Windows/macOS: directories need trailing separator for proper display
static std::string NormalizeDefaultLocation(const char *default_location)
{
    if (!default_location || !*default_location) {
        return {};
    }

    try {
        std::filesystem::path path(default_location);
#if defined(SDL_PLATFORM_LINUX)
        if (std::filesystem::is_regular_file(path)) {
            return path.parent_path().string();
        }
#elif defined(SDL_PLATFORM_WINDOWS) || defined(SDL_PLATFORM_MACOS)
        if (std::filesystem::is_directory(path)) {
            return (path / "").string();
        }
        // Prevent a crash in SDL3 file dialog
        if (!std::filesystem::exists(path)) {
            return {};
        }
#endif
    } catch (...) {
        // Fall through to return original path
    }
    return default_location;
}
#else
namespace foundation = winrt::Windows::Foundation;
namespace storage = winrt::Windows::Storage;
namespace storage_access = winrt::Windows::Storage::AccessCache;
namespace pickers = winrt::Windows::Storage::Pickers;

static std::atomic<bool> g_uwp_picker_active{ false };

static bool BeginUwpPicker(const char *kind)
{
    bool expected = false;
    if (!g_uwp_picker_active.compare_exchange_strong(expected, true)) {
        return false;
    }

    fprintf(stderr, "xemu UWP: opening %s picker\n", kind);
    return true;
}

static void EndUwpPicker(void)
{
    g_uwp_picker_active.store(false);
}

static void ReportUwpPickerError(const char *operation,
                                 winrt::hresult_error const &error)
{
    fprintf(stderr, "xemu UWP: %s failed: HRESULT=0x%08x %s\n",
            operation, (unsigned int)error.code().value,
            winrt::to_string(error.message()).c_str());
}

static std::vector<winrt::hstring> GetUwpFileTypes(
    const SDL_DialogFileFilter *filters, int nfilters, bool allow_wildcard)
{
    std::vector<winrt::hstring> result;

    for (int i = 0; filters != nullptr && i < nfilters; i++) {
        std::string pattern = filters[i].pattern ? filters[i].pattern : "";
        size_t start = 0;

        while (start <= pattern.size()) {
            size_t end = pattern.find(';', start);
            std::string type = pattern.substr(
                start, end == std::string::npos ? std::string::npos :
                                                  end - start);

            if (type == "*") {
                if (allow_wildcard) {
                    winrt::hstring wildcard = L"*";
                    if (std::find(result.begin(), result.end(), wildcard) ==
                        result.end()) {
                        result.push_back(wildcard);
                    }
                }
            } else if (!type.empty()) {
                if (type.rfind("*.", 0) == 0) {
                    type.erase(0, 1);
                } else if (type[0] != '.') {
                    type.insert(type.begin(), '.');
                }

                winrt::hstring file_type = winrt::to_hstring(type);
                if (std::find(result.begin(), result.end(), file_type) ==
                    result.end()) {
                    result.push_back(file_type);
                }
            }

            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    return result;
}

template<typename StorageItem>
static void CompleteUwpPickerSelection(
    StorageItem const &item,
    std::shared_ptr<FileDialogCallback> const &callback)
{
    if (!item) {
        return;
    }

    winrt::hstring selected_path = item.Path();
    if (selected_path.empty()) {
        fprintf(stderr,
                "xemu UWP: selected storage item has no filesystem path\n");
        return;
    }

    std::string path = winrt::to_string(selected_path);

    try {
        std::string token = "xemu-path-" +
            std::to_string(std::hash<std::string>{}(path));
        storage_access::StorageApplicationPermissions::FutureAccessList().AddOrReplace(
            winrt::to_hstring(token),
            item.template as<storage::IStorageItem>());
    } catch (winrt::hresult_error const &error) {
        ReportUwpPickerError("remembering selected storage item", error);
    }

    RunOnMainThread([callback, path = std::move(path)]() {
        (*callback)(path.c_str());
    });
}

static void ShowUwpOpenFileDialog(const SDL_DialogFileFilter *filters,
                                  int nfilters,
                                  FileDialogCallback callback)
{
    if (!BeginUwpPicker("file")) {
        return;
    }

    try {
        pickers::FileOpenPicker picker;
        picker.ViewMode(pickers::PickerViewMode::List);
        picker.SuggestedStartLocation(pickers::PickerLocationId::ComputerFolder);

        auto types = GetUwpFileTypes(filters, nfilters, true);
        if (types.empty()) {
            types.push_back(L"*");
        }
        for (auto const &type : types) {
            picker.FileTypeFilter().Append(type);
        }

        auto cb = std::make_shared<FileDialogCallback>(std::move(callback));
        auto operation = picker.PickSingleFileAsync();
        operation.Completed(
            [cb](auto const &result, foundation::AsyncStatus status) {
                EndUwpPicker();
                if (status != foundation::AsyncStatus::Completed) {
                    return;
                }
                try {
                    CompleteUwpPickerSelection(result.GetResults(), cb);
                } catch (winrt::hresult_error const &error) {
                    ReportUwpPickerError("open-file picker completion", error);
                }
            });
    } catch (winrt::hresult_error const &error) {
        EndUwpPicker();
        ReportUwpPickerError("opening file picker", error);
    }
}

static void ShowUwpSaveFileDialog(const SDL_DialogFileFilter *filters,
                                  int nfilters,
                                  const char *default_location,
                                  FileDialogCallback callback)
{
    if (!BeginUwpPicker("save-file")) {
        return;
    }

    try {
        pickers::FileSavePicker picker;
        picker.SuggestedStartLocation(pickers::PickerLocationId::ComputerFolder);

        bool added_type = false;
        for (int i = 0; filters != nullptr && i < nfilters; i++) {
            auto types = GetUwpFileTypes(&filters[i], 1, false);
            if (types.empty()) {
                continue;
            }

            auto values = winrt::single_threaded_vector<winrt::hstring>();
            for (auto const &type : types) {
                values.Append(type);
            }
            picker.FileTypeChoices().Insert(
                winrt::to_hstring(filters[i].name ? filters[i].name : "Files"),
                values);
            added_type = true;
        }
        if (!added_type) {
            auto values = winrt::single_threaded_vector<winrt::hstring>();
            values.Append(L".dat");
            picker.FileTypeChoices().Insert(L"Files", values);
        }

        std::string suggested_name = "xemu-file";
        if (default_location != nullptr && *default_location != '\0') {
            std::filesystem::path path(default_location);
            if (path.has_filename()) {
                suggested_name = path.filename().string();
            }
        }
        picker.SuggestedFileName(winrt::to_hstring(suggested_name));

        auto cb = std::make_shared<FileDialogCallback>(std::move(callback));
        auto operation = picker.PickSaveFileAsync();
        operation.Completed(
            [cb](auto const &result, foundation::AsyncStatus status) {
                EndUwpPicker();
                if (status != foundation::AsyncStatus::Completed) {
                    return;
                }
                try {
                    CompleteUwpPickerSelection(result.GetResults(), cb);
                } catch (winrt::hresult_error const &error) {
                    ReportUwpPickerError("save-file picker completion", error);
                }
            });
    } catch (winrt::hresult_error const &error) {
        EndUwpPicker();
        ReportUwpPickerError("opening save-file picker", error);
    }
}

static void ShowUwpOpenFolderDialog(FileDialogCallback callback)
{
    if (!BeginUwpPicker("folder")) {
        return;
    }

    try {
        pickers::FolderPicker picker;
        picker.ViewMode(pickers::PickerViewMode::List);
        picker.SuggestedStartLocation(pickers::PickerLocationId::ComputerFolder);
        picker.FileTypeFilter().Append(L"*");

        auto cb = std::make_shared<FileDialogCallback>(std::move(callback));
        auto operation = picker.PickSingleFolderAsync();
        operation.Completed(
            [cb](auto const &result, foundation::AsyncStatus status) {
                EndUwpPicker();
                if (status != foundation::AsyncStatus::Completed) {
                    return;
                }
                try {
                    CompleteUwpPickerSelection(result.GetResults(), cb);
                } catch (winrt::hresult_error const &error) {
                    ReportUwpPickerError("folder picker completion", error);
                }
            });
    } catch (winrt::hresult_error const &error) {
        EndUwpPicker();
        ReportUwpPickerError("opening folder picker", error);
    }
}
#endif

void ShowOpenFileDialog(const SDL_DialogFileFilter *filters, int nfilters,
                        const char *default_location,
                        FileDialogCallback callback)
{
#ifdef CONFIG_UWP
    (void)default_location;
    ShowUwpOpenFileDialog(filters, nfilters, std::move(callback));
#else
    auto *cb = new FileDialogCallback(std::move(callback));
    std::string normalized = NormalizeDefaultLocation(default_location);
    SDL_ShowOpenFileDialog(FileDialogCallbackWrapper, cb, xemu_get_window(),
                           filters, nfilters,
                           normalized.empty() ? nullptr : normalized.c_str(),
                           false);
#endif
}

void ShowSaveFileDialog(const SDL_DialogFileFilter *filters, int nfilters,
                        const char *default_location,
                        FileDialogCallback callback)
{
#ifdef CONFIG_UWP
    ShowUwpSaveFileDialog(filters, nfilters, default_location,
                          std::move(callback));
#else
    auto *cb = new FileDialogCallback(std::move(callback));
    SDL_ShowSaveFileDialog(FileDialogCallbackWrapper, cb, xemu_get_window(),
                           filters, nfilters, default_location);
#endif
}

void ShowOpenFolderDialog(const char *default_location,
                          FileDialogCallback callback)
{
#ifdef CONFIG_UWP
    (void)default_location;
    ShowUwpOpenFolderDialog(std::move(callback));
#else
    auto *cb = new FileDialogCallback(std::move(callback));
    SDL_ShowOpenFolderDialog(FileDialogCallbackWrapper, cb,
                             xemu_get_window(), default_location, false);
#endif
}
