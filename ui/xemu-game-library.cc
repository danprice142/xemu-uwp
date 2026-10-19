/*
 * xemu game library and ScreenScraper metadata cache
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"

#include "ui/xemu-game-library.h"

#include "qemu/bswap.h"
#include "ui/xemu-screenscraper.h"
#include "ui/xemu-settings.h"
#include "xemu-xbe.h"

extern "C" {
#include "qemu/thread.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "hw/xbox/nv2a/nv2a.h"
}

#include <glib/gstdio.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr const char *kLibraryScheme = "xemu-library://";
constexpr const char *kPlatformName = "Microsoft - Xbox";
constexpr const char *kCoreName = "xemu";
constexpr int64_t kRunningTitlePollUsec = 2 * G_USEC_PER_SEC;
constexpr int64_t kRunningTitleResetSettleUsec = 4 * G_USEC_PER_SEC;

struct LibraryEntry {
    std::string token;
    std::string identity;
    std::string path;
    std::string filename_label;
    std::string xbe_name;
    std::string metadata_name;
    std::string screenscraper_game_id;
    std::unordered_map<std::string, std::string> media_paths;
    xemu::screenscraper::Details details;
    std::string cover_path;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t title_id = 0;

    const std::string &label() const
    {
        if (!metadata_name.empty()) {
            return metadata_name;
        }
        if (!xbe_name.empty()) {
            return xbe_name;
        }
        return filename_label;
    }
};

struct CachedRecord {
    std::string identity;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t title_id = 0;
    std::string xbe_name;
    std::string metadata_name;
    std::string screenscraper_game_id;
    /* The image was read and holds no recognisable title. */
    bool image_probed = false;
};

/* An unidentified library image whose title is read on a worker. */
struct ImageProbe {
    std::string token;
    std::string identity;
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
};

struct CurrentState {
    bool running = false;
    uint32_t title_id = 0;
    std::string token;
    std::string game_name;
    std::string disc_path;
    std::string xbe_name;
};

static std::string path_to_utf8(const std::filesystem::path &path)
{
    auto value = path.u8string();
#if defined(__cpp_char8_t)
    return std::string(reinterpret_cast<const char *>(value.data()),
                       value.size());
#else
    return value;
#endif
}

static std::string normalize_identity(const std::filesystem::path &input)
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(input, ec);
    if (ec) {
        path = input;
        ec.clear();
    }

    std::filesystem::path canonical =
        std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        path = canonical;
    }

    std::string identity = path_to_utf8(path.lexically_normal());
#ifdef _WIN32
    std::replace(identity.begin(), identity.end(), '\\', '/');
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return g_ascii_tolower(c); });
#endif
    return identity;
}

static std::string token_for_identity(const std::string &identity)
{
    char *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, (const guchar *)identity.data(), identity.size());
    std::string token = kLibraryScheme;
    if (digest != nullptr) {
        token += digest;
        g_free(digest);
    }
    return token;
}

/* The leading 16 digest digits of a token. */
static uint64_t launch_id_for_token(const std::string &token)
{
    size_t prefix = strlen(kLibraryScheme);
    if (token.size() < prefix + 16 ||
        token.compare(0, prefix, kLibraryScheme) != 0) {
        return 0;
    }
    uint64_t id = 0;
    for (size_t i = prefix; i < prefix + 16; i++) {
        int digit = g_ascii_xdigit_value(token[i]);
        if (digit < 0) {
            return 0;
        }
        id = (id << 4) | (uint64_t)digit;
    }
    return id;
}

/* Provider media for each catalogue media type, in preference order. */
static const char *const kCatalogueMedia[][3] = {
    { "wheel-hd", "wheel", nullptr },
    { "box-2D", nullptr, nullptr },
    { "box-3D", nullptr, nullptr },
    { "mixrbv2", "mixrbv1", nullptr },
    { "box-2D-back", nullptr, nullptr },
    { "support-2D", nullptr, nullptr },
    { "ss", nullptr, nullptr },
    { "fanart", nullptr, nullptr },
    { nullptr, nullptr, nullptr },
    { "video-normalized", "video", nullptr },
};

static std::string catalogue_media_path(
    const std::unordered_map<std::string, std::string> &media,
    unsigned media_type)
{
    if (media_type >= G_N_ELEMENTS(kCatalogueMedia)) {
        return std::string();
    }
    for (const char *key : kCatalogueMedia[media_type]) {
        if (key == nullptr) {
            break;
        }
        auto it = media.find(key);
        if (it != media.end() && !it->second.empty() &&
            g_file_test(it->second.c_str(), G_FILE_TEST_IS_REGULAR)) {
            return it->second;
        }
    }
    return std::string();
}

/* Provider dates (YYYY, YYYY-MM or YYYY-MM-DD) as YYYYMMDDTHHMMSS. */
static std::string catalogue_release_date(const std::string &date)
{
    int year = 0, month = 1, day = 1;
    if (sscanf(date.c_str(), "%4d-%2d-%2d", &year, &month, &day) < 1 ||
        year < 1971 || year > 9999) {
        return std::string();
    }
    char text[32];
    snprintf(text, sizeof(text), "%04d%02d%02dT000000", year,
             std::clamp(month, 1, 12), std::clamp(day, 1, 31));
    return text;
}

/* JSON output needs valid UTF-8; file and provider names are not checked. */
static std::string valid_utf8(const std::string &text)
{
    if (g_utf8_validate(text.c_str(), text.size(), nullptr)) {
        return text;
    }
    gchar *valid = g_utf8_make_valid(text.c_str(), text.size());
    std::string result(valid);
    g_free(valid);
    return result;
}

/* Provider scores are out of 20; the catalogue carries a 0..1 fraction. */
static std::string catalogue_rating(const std::string &rating)
{
    char *end = nullptr;
    double score = g_ascii_strtod(rating.c_str(), &end);
    if (rating.empty() || end == rating.c_str()) {
        return std::string();
    }
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    return g_ascii_formatd(text, sizeof(text), "%.3f",
                           std::clamp(score / 20.0, 0.0, 1.0));
}

static bool fingerprint_file(const std::filesystem::path &path,
                             uint64_t *size, int64_t *mtime)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return false;
    }

    uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }

    auto write_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return false;
    }

    *size = file_size;
    *mtime = write_time.time_since_epoch().count();
    return true;
}

static bool is_supported_disc(const std::filesystem::path &path)
{
    std::string extension = path_to_utf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return g_ascii_tolower(c); });
    return extension == ".iso" || extension == ".xiso";
}

static bool read_file_limited(const std::string &path, gsize max_size,
                              std::string *contents)
{
    char *data = nullptr;
    gsize size = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(path.c_str(), &data, &size, &error)) {
        g_clear_error(&error);
        return false;
    }

    if (size > max_size) {
        g_free(data);
        return false;
    }

    contents->assign(data, size);
    g_free(data);
    return true;
}

static bool write_file_atomic(const std::string &path, const void *data,
                              gsize size)
{
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() &&
        g_mkdir_with_parents(path_to_utf8(parent).c_str(), 0755) != 0) {
        return false;
    }

    std::string temporary = path + ".tmp";
    GError *error = nullptr;
    if (!g_file_set_contents(temporary.c_str(), (const char *)data, size,
                             &error)) {
        g_clear_error(&error);
        return false;
    }

    if (g_rename(temporary.c_str(), path.c_str()) == 0) {
        return true;
    }

    /* Some Windows filesystems do not replace an existing file on rename. */
    bool ok = g_file_set_contents(path.c_str(), (const char *)data, size,
                                  &error);
    g_clear_error(&error);
    g_remove(temporary.c_str());
    return ok;
}

/* Read-only positional access to a disc image. */
class DiscImageReader {
public:
    DiscImageReader() = default;
    DiscImageReader(const DiscImageReader &) = delete;
    DiscImageReader &operator=(const DiscImageReader &) = delete;

    ~DiscImageReader()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
#else
        if (file != nullptr) {
            fclose(file);
        }
#endif
    }

    bool open(const std::string &path)
    {
#ifdef _WIN32
        gunichar2 *wide =
            g_utf8_to_utf16(path.c_str(), -1, nullptr, nullptr, nullptr);
        if (wide == nullptr) {
            return false;
        }
        /* Never deny sharing: the running title may have this image open. */
        handle = CreateFileW((const wchar_t *)wide, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
        g_free(wide);
        LARGE_INTEGER file_size;
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileSizeEx(handle, &file_size) || file_size.QuadPart < 0) {
            return false;
        }
        size = (uint64_t)file_size.QuadPart;
        return true;
#else
        file = g_fopen(path.c_str(), "rb");
        if (file == nullptr || fseeko(file, 0, SEEK_END) != 0) {
            return false;
        }
        off_t end = ftello(file);
        if (end < 0) {
            return false;
        }
        size = (uint64_t)end;
        return true;
#endif
    }

    /* Ranges past the end fail without setting io_error. */
    bool read(uint64_t offset, void *buffer, size_t length)
    {
        if (offset > size || length > size - offset) {
            return false;
        }
#ifdef _WIN32
        uint8_t *out = (uint8_t *)buffer;
        while (length > 0) {
            OVERLAPPED overlapped = {};
            overlapped.Offset = (DWORD)offset;
            overlapped.OffsetHigh = (DWORD)(offset >> 32);
            DWORD chunk = (DWORD)std::min<size_t>(length, 1024 * 1024);
            DWORD transferred = 0;
            if (!ReadFile(handle, out, chunk, &transferred, &overlapped) ||
                transferred == 0) {
                io_error = true;
                return false;
            }
            out += transferred;
            offset += transferred;
            length -= transferred;
        }
        return true;
#else
        if (fseeko(file, (off_t)offset, SEEK_SET) != 0 ||
            fread(buffer, 1, length, file) != length) {
            io_error = true;
            return false;
        }
        return true;
#endif
    }

    bool io_error = false;

private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    FILE *file = nullptr;
#endif
    uint64_t size = 0;
};

enum class DiscProbe {
    kIdentified,
    /* Not an XDVDFS image with a readable default.xbe; never retried. */
    kUnknown,
    /* Open or read failure; retried by a later scan. */
    kUnreadable,
};

struct DiscIdentity {
    uint32_t title_id = 0;
    std::string name;
};

constexpr uint64_t kXdvdfsSectorSize = 2048;
/* Volume descriptor: magic, u32 root directory sector, u32 root size. */
constexpr uint64_t kXdvdfsVolumeOffset = 32 * kXdvdfsSectorSize;
constexpr char kXdvdfsMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kXdvdfsMagicLength = sizeof(kXdvdfsMagic) - 1;
/* Game partition offsets: extracted images, then full XGD1/XGD2/XGD3
 * dumps. */
constexpr uint64_t kXdvdfsPartitionOffsets[] = {
    0, 0x18300000, 0xFD90000, 0x2080000,
};
/* Child offsets are 16-bit dword counts, so nothing past 256 KiB (plus one
 * entry) is reachable. */
constexpr size_t kXdvdfsMaxDirectoryBytes = 0x40000 + 0x200;
constexpr size_t kXdvdfsEntryHeader = 14;
constexpr uint8_t kXdvdfsDirectoryAttribute = 0x10;

/*
 * Finds a file in an XDVDFS directory: a binary tree of 4-byte aligned
 * entries (u16 left and right child offsets in dwords, u32 sector, u32 size,
 * u8 attributes, u8 name length, name), padded with 0xFF.  The whole tree is
 * walked since image tools differ in name ordering.
 */
static bool find_xdvdfs_file(const std::vector<uint8_t> &directory,
                             const char *name, uint32_t *sector,
                             uint32_t *size)
{
    size_t name_length = strlen(name);
    std::vector<bool> visited(directory.size() / 4 + 1, false);
    std::vector<size_t> pending = { 0 };
    while (!pending.empty()) {
        size_t offset = pending.back();
        pending.pop_back();
        if (offset + kXdvdfsEntryHeader > directory.size() ||
            visited[offset / 4]) {
            continue;
        }
        visited[offset / 4] = true;

        const uint8_t *entry = directory.data() + offset;
        uint16_t left = lduw_le_p(entry);
        uint16_t right = lduw_le_p(entry + 2);
        uint8_t length = entry[13];
        if (left == 0xffff || length == 0 ||
            offset + kXdvdfsEntryHeader + length > directory.size()) {
            continue;
        }
        if (length == name_length &&
            !(entry[12] & kXdvdfsDirectoryAttribute) &&
            g_ascii_strncasecmp((const char *)entry + kXdvdfsEntryHeader,
                                name, name_length) == 0) {
            *sector = ldl_le_p(entry + 4);
            *size = ldl_le_p(entry + 8);
            return true;
        }
        if (left != 0) {
            pending.push_back((size_t)left * 4);
        }
        if (right != 0) {
            pending.push_back((size_t)right * 4);
        }
    }
    return false;
}

/* Title ID and name from the certificate of the XBE at offset. */
static DiscProbe read_xbe_identity(DiscImageReader *reader, uint64_t offset,
                                   uint32_t xbe_size, DiscIdentity *identity)
{
    constexpr size_t kCertificateBytes =
        offsetof(struct xbe_certificate, m_title_name) +
        sizeof(xbe_certificate::m_title_name);
    uint8_t header[4096];
    size_t length = std::min<size_t>(xbe_size, sizeof(header));
    if (length < sizeof(struct xbe_header)) {
        return DiscProbe::kUnknown;
    }
    if (!reader->read(offset, header, length)) {
        return reader->io_error ? DiscProbe::kUnreadable : DiscProbe::kUnknown;
    }
    if (ldl_le_p(header) != 0x48454258 /* XBEH */) {
        return DiscProbe::kUnknown;
    }
    uint32_t base = ldl_le_p(header + offsetof(struct xbe_header, m_base));
    uint32_t certificate_address =
        ldl_le_p(header + offsetof(struct xbe_header, m_certificate_addr));
    if (certificate_address < base) {
        return DiscProbe::kUnknown;
    }
    uint64_t certificate_offset = certificate_address - base;
    if (certificate_offset + kCertificateBytes > xbe_size) {
        return DiscProbe::kUnknown;
    }

    uint8_t outside[kCertificateBytes];
    const uint8_t *certificate = outside;
    if (certificate_offset + kCertificateBytes <= length) {
        certificate = header + certificate_offset;
    } else if (!reader->read(offset + certificate_offset, outside,
                             sizeof(outside))) {
        return reader->io_error ? DiscProbe::kUnreadable
                                : DiscProbe::kUnknown;
    }

    uint32_t title_id =
        ldl_le_p(certificate + offsetof(struct xbe_certificate, m_titleid));
    if (title_id == 0) {
        return DiscProbe::kUnknown;
    }
    /* Converted as the running-title probe does, so both agree. */
    constexpr size_t kTitleNameLength = 40;
    gunichar2 title_name[kTitleNameLength];
    for (size_t i = 0; i < kTitleNameLength; i++) {
        title_name[i] = lduw_le_p(
            certificate + offsetof(struct xbe_certificate, m_title_name) +
            i * 2);
    }
    char *converted = g_utf16_to_utf8(title_name, kTitleNameLength, nullptr,
                                      nullptr, nullptr);
    identity->title_id = title_id;
    identity->name = converted != nullptr ? g_strstrip(converted) : "";
    g_free(converted);
    return DiscProbe::kIdentified;
}

/* Reads the title of default.xbe from a plain disc image. */
static DiscProbe probe_disc_identity(const std::string &path,
                                     DiscIdentity *identity)
{
    DiscImageReader reader;
    if (!reader.open(path)) {
        return DiscProbe::kUnreadable;
    }

    for (uint64_t partition : kXdvdfsPartitionOffsets) {
        uint8_t volume[kXdvdfsMagicLength + 8];
        if (!reader.read(partition + kXdvdfsVolumeOffset, volume,
                         sizeof(volume))) {
            if (reader.io_error) {
                return DiscProbe::kUnreadable;
            }
            continue;
        }
        if (memcmp(volume, kXdvdfsMagic, kXdvdfsMagicLength) != 0) {
            continue;
        }

        uint32_t root_sector = ldl_le_p(volume + kXdvdfsMagicLength);
        uint32_t root_size = ldl_le_p(volume + kXdvdfsMagicLength + 4);
        if (root_sector == 0 || root_size < kXdvdfsEntryHeader) {
            return DiscProbe::kUnknown;
        }
        std::vector<uint8_t> directory(
            std::min<size_t>(root_size, kXdvdfsMaxDirectoryBytes));
        if (!reader.read(partition + root_sector * kXdvdfsSectorSize,
                         directory.data(), directory.size())) {
            return reader.io_error ? DiscProbe::kUnreadable
                                   : DiscProbe::kUnknown;
        }

        uint32_t xbe_sector = 0;
        uint32_t xbe_size = 0;
        if (!find_xdvdfs_file(directory, "default.xbe", &xbe_sector,
                              &xbe_size)) {
            return DiscProbe::kUnknown;
        }
        return read_xbe_identity(&reader,
                                 partition + xbe_sector * kXdvdfsSectorSize,
                                 xbe_size, identity);
    }
    return DiscProbe::kUnknown;
}

class GameLibrary {
public:
    void refresh();
    uint64_t generation();
    XemuGameLibraryEntry *snapshot(size_t *count);
    XemuGameLibraryEntry *snapshot_nonblocking(size_t *count,
                                               bool *available = nullptr);
    char *resolve_token(const char *token);
    char *resolve_launch_id(uint64_t launch_id);
    char *catalogue_json();
    uint32_t catalogue_revision();
    char *catalogue_media(uint64_t launch_id, unsigned media_type);
    void note_running_title(const char *disc_path, uint32_t title_id,
                            const char *xbe_name);
    uint32_t title_id_for_path(const char *path);
    void refresh_running_title();
    bool get_current_state(XemuGameLibraryState *state);
    char *dup_cache_path();
    void screenscraper_settings_changed();
    bool start_library_download(bool overwrite,
                                XemuGameLibraryDownloadDone done,
                                void *opaque);
    void download_status(XemuGameLibraryDownloadStatus *status) const;

private:
    struct FetchFlags {
        /* Bypass the cache and download metadata and media again. */
        bool force = false;
        /* Counted by the running library download. */
        bool batch = false;
    };

    struct InitializationData {
        std::string cache_root;
        std::unordered_map<std::string, CachedRecord> records;
    };

    struct InitializationWork {
        GameLibrary *library;
        std::string cache_root;
    };

    struct ScanWork {
        GameLibrary *library;
        std::string directory;
    };

    struct RunningTitleWork {
        GameLibrary *library;
        std::string disc_path;
        uint32_t title_id;
        std::string xbe_name;
        uint64_t sequence;
    };

    struct CacheSnapshot {
        std::string cache_root;
        bool write_index = false;
        uint64_t index_revision = 0;
        std::unordered_map<std::string, CachedRecord> records;
    };

    bool prewarm_initialization();
    void ensure_initialized();
    void register_reset_handler_locked();
    static void *initialization_worker(void *opaque);
    static InitializationData load_initialization_data(
        const std::string &cache_root);
    static void load_index(InitializationData *data);
    void finish_initialization(InitializationData data);
    CacheSnapshot snapshot_cache_locked(bool index);
    void queue_cache_snapshot(CacheSnapshot snapshot);
    static void *persistence_worker(void *opaque);
    void persist_cache_snapshot(CacheSnapshot snapshot);
    xemu::screenscraper::Request request_for_title_locked(
        uint32_t requested) const;
    void schedule_metadata_fetch(uint32_t requested, bool force = false,
                                 bool batch = false);
    static void *metadata_worker(void *opaque);
    void finish_metadata_fetch(uint32_t requested,
                               const xemu::screenscraper::Metadata *metadata);
    void queue_identification(std::vector<ImageProbe> probes);
    void start_identify_worker();
    static void *identify_worker(void *opaque);
    void identify_image(const ImageProbe &probe, bool schedule_fetch = true);
    void load_identified_metadata(const ImageProbe &probe, uint32_t title_id,
                                  const std::string &root,
                                  bool schedule_fetch);
    void persist_identified_records();
    void run_library_download(bool overwrite, const std::string &games_dir);
    void note_library_download(bool failed);
    void finish_library_download();
    void refresh_directory(const std::string &configured);
    static void *scan_worker(void *opaque);
    static void *screenscraper_settings_worker(void *opaque);
    void apply_screenscraper_settings_change(const std::string &games_dir);
    XemuGameLibraryEntry *snapshot_locked(size_t *count) const;
    void process_running_title(const RunningTitleWork &work);
    static void *running_title_worker(void *opaque);
    void set_not_running();
    static void guest_reset(void *opaque);

    std::mutex mutex;
    std::mutex persistence_mutex;
    std::mutex persistence_queue_mutex;
    std::condition_variable persistence_queue_condition;
    std::mutex screenscraper_settings_queue_mutex;
    std::condition_variable initialization_condition;
    bool initialized = false;
    bool initialization_in_progress = false;
    std::atomic<bool> initialization_ready{ false };
    bool scan_complete = false;
    bool background_scan_in_progress = false;
    std::string cache_root;
    std::unordered_map<std::string, CachedRecord> records;
    std::vector<LibraryEntry> entries;
    /* Titles waiting in metadata_fetch_queue. */
    std::unordered_map<uint32_t, FetchFlags> queued_fetches;
    std::deque<uint32_t> metadata_fetch_queue;
    uint32_t fetch_in_flight = 0;
    bool metadata_worker_started = false;
    std::deque<ImageProbe> identify_queue;
    bool identify_worker_started = false;
    bool identify_records_changed = false;
    /* Library download request, run by the identification worker. */
    bool download_request_pending = false;
    bool download_overwrite = false;
    std::string download_games_dir;
    XemuGameLibraryDownloadDone download_done_callback = nullptr;
    void *download_done_opaque = nullptr;
    std::atomic<bool> download_running{ false };
    std::atomic<bool> download_identifying{ false };
    std::atomic<uint32_t> download_done{ 0 };
    std::atomic<uint32_t> download_total{ 0 };
    std::atomic<uint32_t> download_failed{ 0 };
    bool screenscraper_settings_change_pending = false;
    bool screenscraper_settings_worker_started = false;
    std::string screenscraper_settings_games_dir;
    CurrentState current;
    uint64_t visible_generation = 1;
    uint64_t next_index_revision = 0;
    uint64_t last_index_persist_attempt = 0;
    CacheSnapshot pending_persistence;
    bool persistence_pending = false;
    bool persistence_worker_started = false;
    bool reset_handler_registered = false;
    std::atomic<int64_t> next_running_poll{ 0 };
    std::atomic<uint64_t> running_identity_generation{ 1 };
    std::atomic<uint64_t> running_title_work_sequence{ 0 };
    uint64_t observed_running_identity_generation = 0;
    /* Main thread: the disc the running title was last checked against. */
    std::string last_disc_path;
    bool disc_check_pending = false;
};

static GameLibrary &game_library()
{
    /* Process-lifetime by design: detached workers may outlive the UI. */
    static GameLibrary *library = new GameLibrary();
    return *library;
}

void GameLibrary::register_reset_handler_locked()
{
    if (!reset_handler_registered) {
        /* This helper is called only from public main/BQL-thread entry points.
         * Cache parsing may run on a detached worker, but reset registration
         * must remain on the emulator thread. */
        qemu_register_reset(guest_reset, this);
        reset_handler_registered = true;
    }
}

GameLibrary::InitializationData GameLibrary::load_initialization_data(
    const std::string &cache_root)
{
    InitializationData data;
    data.cache_root = cache_root;
    g_mkdir_with_parents(
        path_to_utf8(std::filesystem::path(data.cache_root) / "games").c_str(),
        0755);
    load_index(&data);
    return data;
}

void GameLibrary::finish_initialization(InitializationData data)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!initialized) {
            cache_root = std::move(data.cache_root);
            records = std::move(data.records);
            initialized = true;
            initialization_ready.store(true, std::memory_order_release);
        }
        initialization_in_progress = false;
    }
    initialization_condition.notify_all();
}

void *GameLibrary::initialization_worker(void *opaque)
{
    std::unique_ptr<InitializationWork> work((InitializationWork *)opaque);
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    InitializationData data = load_initialization_data(work->cache_root);
    work->library->finish_initialization(std::move(data));
    return nullptr;
}

bool GameLibrary::prewarm_initialization()
{
    if (initialization_ready.load(std::memory_order_acquire)) {
        return true;
    }

    InitializationWork *work = nullptr;
    {
        /* HUD and DSU callers hold the BQL.  The initialization worker can
         * briefly own this mutex while publishing a parsed LocalState cache;
         * never make guest execution wait for that publication.  A later UI
         * tick will either observe initialization_ready or retry the prewarm. */
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        register_reset_handler_locked();
        if (initialized) {
            return true;
        }
        if (!initialization_in_progress) {
            const char *base = xemu_settings_get_base_path();
            std::string root = path_to_utf8(
                std::filesystem::path(base ? base : ".") / "screenscraper");
            initialization_in_progress = true;
            work = new InitializationWork{ this, std::move(root) };
        }
    }

    if (work != nullptr) {
        QemuThread thread;
        qemu_thread_create(&thread, "screenscraper_init", initialization_worker,
                           work,
                           QEMU_THREAD_DETACHED);
    }
    return false;
}

void GameLibrary::ensure_initialized()
{
    if (initialization_ready.load(std::memory_order_acquire)) {
        return;
    }

    std::string root;
    {
        std::unique_lock<std::mutex> lock(mutex);
        register_reset_handler_locked();
        if (initialized) {
            return;
        }
        if (initialization_in_progress) {
            initialization_condition.wait(
                lock, [this] { return initialized; });
            return;
        }
        const char *base = xemu_settings_get_base_path();
        root = path_to_utf8(std::filesystem::path(base ? base : ".") /
                            "screenscraper");
        initialization_in_progress = true;
    }

    finish_initialization(load_initialization_data(root));
}

void GameLibrary::load_index(InitializationData *data)
{
    std::string contents;
    std::string path = path_to_utf8(std::filesystem::path(data->cache_root) /
                                    "library.json");
    if (!read_file_limited(path, 8 * 1024 * 1024, &contents)) {
        return;
    }

    try {
        json root = json::parse(contents);
        const json &stored = root.at("entries");
        if (!stored.is_object()) {
            return;
        }
        for (auto it = stored.begin(); it != stored.end(); ++it) {
            if (!it.value().is_object() ||
                it.key().compare(0, strlen(kLibraryScheme), kLibraryScheme) !=
                    0) {
                continue;
            }
            CachedRecord record;
            record.identity = it.value().value("identity", std::string());
            record.size = it.value().value("size", (uint64_t)0);
            record.mtime = it.value().value("mtime", (int64_t)0);
            record.title_id = it.value().value("title_id", (uint32_t)0);
            record.xbe_name = it.value().value("xbe_name", std::string());
            record.metadata_name =
                it.value().value("metadata_name", std::string());
            record.screenscraper_game_id =
                it.value().value("screenscraper_game_id", std::string());
            record.image_probed = it.value().value("image_probed", false);
            if (!record.identity.empty()) {
                data->records.emplace(it.key(), std::move(record));
            }
        }
    } catch (const json::exception &) {
        fprintf(stderr, "xemu game library: ignoring invalid library cache\n");
    }
}

GameLibrary::CacheSnapshot GameLibrary::snapshot_cache_locked(
    bool index)
{
    CacheSnapshot snapshot;

    snapshot.cache_root = cache_root;
    if (index) {
        snapshot.write_index = true;
        snapshot.index_revision = ++next_index_revision;
        snapshot.records = records;
    }
    return snapshot;
}

void GameLibrary::persist_cache_snapshot(CacheSnapshot snapshot)
{
    if (!snapshot.write_index) {
        return;
    }

    /*
     * LocalState can be slow on Xbox. Never serialize or touch storage while
     * holding the library state mutex: HUD/title queries take that mutex while
     * the BQL is held. The separate persistence mutex orders writers, while
     * revisions prevent a delayed older snapshot from overwriting newer data.
     */
    std::lock_guard<std::mutex> persistence_lock(persistence_mutex);

    if (snapshot.write_index &&
        snapshot.index_revision > last_index_persist_attempt) {
        json root;
        root["version"] = 2;
        root["entries"] = json::object();
        for (const auto &[token, record] : snapshot.records) {
            root["entries"][token] = {
                { "identity", record.identity },
                { "size", record.size },
                { "mtime", record.mtime },
                { "title_id", record.title_id },
                { "xbe_name", record.xbe_name },
                { "metadata_name", record.metadata_name },
                { "screenscraper_game_id",
                  record.screenscraper_game_id },
                { "image_probed", record.image_probed },
            };
        }
        std::string serialized = root.dump(2);
        write_file_atomic(
            path_to_utf8(std::filesystem::path(snapshot.cache_root) /
                         "library.json"),
            serialized.data(), serialized.size());
        last_index_persist_attempt = snapshot.index_revision;
    }
}

void GameLibrary::queue_cache_snapshot(CacheSnapshot snapshot)
{
    if (!snapshot.write_index) {
        return;
    }

    bool start_worker = false;
    {
        std::lock_guard<std::mutex> queue_lock(persistence_queue_mutex);
        pending_persistence.cache_root = snapshot.cache_root;
        if (snapshot.write_index &&
            (!pending_persistence.write_index ||
             snapshot.index_revision > pending_persistence.index_revision)) {
            pending_persistence.write_index = true;
            pending_persistence.index_revision = snapshot.index_revision;
            pending_persistence.records = std::move(snapshot.records);
        }
        persistence_pending = true;
        if (!persistence_worker_started) {
            persistence_worker_started = true;
            start_worker = true;
        }
    }

    if (start_worker) {
        QemuThread thread;
        qemu_thread_create(&thread, "screenscraper_persist",
                           persistence_worker, this, QEMU_THREAD_DETACHED);
    }
    persistence_queue_condition.notify_one();
}

void *GameLibrary::persistence_worker(void *opaque)
{
    GameLibrary *library = (GameLibrary *)opaque;

#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    for (;;) {
        CacheSnapshot snapshot;
        {
            std::unique_lock<std::mutex> queue_lock(
                library->persistence_queue_mutex);
            library->persistence_queue_condition.wait(
                queue_lock,
                [library] { return library->persistence_pending; });
            snapshot = std::move(library->pending_persistence);
            library->pending_persistence = CacheSnapshot{};
            library->persistence_pending = false;
        }
        library->persist_cache_snapshot(std::move(snapshot));
    }
}

xemu::screenscraper::Request GameLibrary::request_for_title_locked(
    uint32_t requested) const
{
    xemu::screenscraper::Request request;
    request.title_id = requested;
    for (const LibraryEntry &entry : entries) {
        if (entry.title_id == requested) {
            request.rom_filename = path_to_utf8(
                std::filesystem::path(entry.path).filename());
            request.rom_size = entry.size;
            return request;
        }
    }
    for (const auto &[token, record] : records) {
        (void)token;
        if (record.title_id == requested) {
            request.rom_filename = path_to_utf8(
                std::filesystem::path(record.identity).filename());
            request.rom_size = record.size;
            break;
        }
    }
    return request;
}

void GameLibrary::schedule_metadata_fetch(uint32_t requested, bool force,
                                          bool batch)
{
    if (requested == 0) {
        return;
    }

    ensure_initialized();
    bool start_worker = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        /* A plain refresh of the title being fetched is redundant; forced
         * and library downloads queue it again behind the current fetch. */
        if (!force && !batch && requested == fetch_in_flight) {
            return;
        }
        auto [queued, inserted] = queued_fetches.try_emplace(requested);
        queued->second.force |= force;
        queued->second.batch |= batch;
        if (!inserted) {
            return;
        }
        metadata_fetch_queue.push_back(requested);
        if (!metadata_worker_started) {
            metadata_worker_started = true;
            start_worker = true;
        }
    }

    if (start_worker) {
        QemuThread thread;
        qemu_thread_create(&thread, "screenscraper_metadata", metadata_worker,
                           this, QEMU_THREAD_DETACHED);
    }
}

void *GameLibrary::metadata_worker(void *opaque)
{
    GameLibrary *library = (GameLibrary *)opaque;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    /*
     * A library refresh or policy change may enqueue many stale titles at
     * once; one worker drains the queue instead of a thread per title.
     */
    for (;;) {
        uint32_t requested = 0;
        FetchFlags flags;
        {
            std::lock_guard<std::mutex> lock(library->mutex);
            if (library->metadata_fetch_queue.empty()) {
                library->metadata_worker_started = false;
                return nullptr;
            }
            requested = library->metadata_fetch_queue.front();
            library->metadata_fetch_queue.pop_front();
            auto queued = library->queued_fetches.find(requested);
            if (queued != library->queued_fetches.end()) {
                flags = queued->second;
                library->queued_fetches.erase(queued);
            }
            library->fetch_in_flight = requested;
        }

        xemu::screenscraper::Metadata metadata;
        bool have_metadata = false;
        bool fetch_failed = false;
        uint64_t settings_revision = 0;
        try {
            xemu::screenscraper::SettingsSnapshot settings =
                xemu::screenscraper::settings_snapshot();
            settings_revision = settings.revision;
            bool fresh = false;
            xemu::screenscraper::Request request;
            std::string root;

            {
                std::lock_guard<std::mutex> lock(library->mutex);
                request = library->request_for_title_locked(requested);
                root = library->cache_root;
            }
            request.overwrite = flags.force;

            /* A forced fetch keeps the cached copy if the download fails. */
            have_metadata = xemu::screenscraper::load_cached(
                root, request, settings, &metadata, &fresh);
            if (!have_metadata || !fresh || flags.force) {
                xemu::screenscraper::Metadata downloaded;
                if (xemu::screenscraper::fetch(root, request, settings,
                                               &downloaded)) {
                    metadata = std::move(downloaded);
                    have_metadata = true;
                } else {
                    fetch_failed = true;
                }
            }
        } catch (...) {
            /* No C++ exception may cross the detached QEMU thread entry point. */
            have_metadata = false;
            fetch_failed = true;
        }

        try {
            library->finish_metadata_fetch(
                requested, have_metadata ? &metadata : nullptr);
        } catch (...) {
            /* Best-effort cleanup keeps a failed title from remaining pending. */
            try {
                library->finish_metadata_fetch(requested, nullptr);
            } catch (...) {
            }
        }
        if (flags.batch) {
            try {
                library->note_library_download(fetch_failed ||
                                               !have_metadata);
            } catch (...) {
            }
        }

        try {
            if (xemu::screenscraper::settings_snapshot().revision !=
                settings_revision) {
                library->schedule_metadata_fetch(requested);
            }
        } catch (...) {
        }
    }
}

void GameLibrary::finish_metadata_fetch(uint32_t requested,
    const xemu::screenscraper::Metadata *metadata)
{
    CacheSnapshot cache_snapshot;
    xemu::screenscraper::SettingsSnapshot settings;
    if (metadata != nullptr) {
        settings = xemu::screenscraper::settings_snapshot();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (fetch_in_flight == requested) {
            fetch_in_flight = 0;
        }
        if (metadata == nullptr) {
            return;
        }

        bool records_changed = false;
        for (auto &[token, record] : records) {
            if (record.title_id == requested &&
                (record.metadata_name != metadata->name ||
                 record.screenscraper_game_id != metadata->game_id)) {
                record.metadata_name = metadata->name;
                record.screenscraper_game_id = metadata->game_id;
                records_changed = true;
            }
        }
        for (auto &entry : entries) {
            if (entry.title_id == requested) {
                entry.metadata_name = metadata->name;
                entry.screenscraper_game_id = metadata->game_id;
                entry.media_paths = metadata->media_paths;
                entry.details = metadata->details;
                entry.cover_path = xemu::screenscraper::choose_cover(
                    *metadata, settings);
            }
        }
        if (current.title_id == requested) {
            current.game_name = metadata->name;
        }

        /* Always publish a new visible generation, but only persist the
         * index when a stored record changed. */
        visible_generation++;
        cache_snapshot = snapshot_cache_locked(records_changed);
    }
    queue_cache_snapshot(std::move(cache_snapshot));
}

void GameLibrary::refresh()
{
    /* Explicit refreshes from the Games menu are synchronous. */
    ensure_initialized();

    const char *configured = g_config.general.games_dir;
    refresh_directory(configured ? configured : "");
}

void GameLibrary::refresh_directory(const std::string &configured)
{

    std::vector<LibraryEntry> scanned;
    CacheSnapshot cache_snapshot;
    std::filesystem::path directory(configured);
    std::error_code ec;

    if (!directory.empty() && std::filesystem::is_directory(directory, ec) &&
        !ec) {
        std::filesystem::directory_iterator iterator(
            directory, std::filesystem::directory_options::skip_permission_denied,
            ec);
        std::filesystem::directory_iterator end;
        while (!ec && iterator != end) {
            const std::filesystem::directory_entry &file = *iterator;
            const std::filesystem::path &path = file.path();
            uint64_t size = 0;
            int64_t mtime = 0;
            if (is_supported_disc(path) &&
                fingerprint_file(path, &size, &mtime)) {
                LibraryEntry entry;
                entry.path = path_to_utf8(path);
                entry.identity = normalize_identity(path);
                entry.token = token_for_identity(entry.identity);
                entry.filename_label = path_to_utf8(path.stem());
                entry.size = size;
                entry.mtime = mtime;
                if (entry.token.size() > strlen(kLibraryScheme)) {
                    scanned.emplace_back(std::move(entry));
                }
            }
            iterator.increment(ec);
        }
    }

    std::vector<uint32_t> fetches;
    std::vector<ImageProbe> probes;
    bool records_changed = false;
    std::string root;
    {
        std::lock_guard<std::mutex> lock(mutex);
        root = cache_root;
        for (auto &entry : scanned) {
            auto existing = records.find(entry.token);
            bool probed = false;
            if (existing != records.end() &&
                existing->second.identity == entry.identity &&
                existing->second.size == entry.size &&
                existing->second.mtime == entry.mtime) {
                entry.title_id = existing->second.title_id;
                entry.xbe_name = existing->second.xbe_name;
                entry.metadata_name = existing->second.metadata_name;
                entry.screenscraper_game_id =
                    existing->second.screenscraper_game_id;
                probed = existing->second.image_probed;
            } else {
                CachedRecord record;
                record.identity = entry.identity;
                record.size = entry.size;
                record.mtime = entry.mtime;
                records[entry.token] = std::move(record);
                records_changed = true;
            }
            /* Titles of games never run are read from their images. */
            if (entry.title_id == 0 && !probed) {
                probes.push_back({ entry.token, entry.identity, entry.path,
                                   entry.size, entry.mtime });
            }
        }
    }

    /* Cache reads stay outside the state mutex so HUD and DSU polling
     * never inherit LocalState latency from a directory scan. */
    xemu::screenscraper::SettingsSnapshot screenscraper_settings =
        xemu::screenscraper::settings_snapshot();
    for (auto &entry : scanned) {
        if (entry.title_id == 0) {
            continue;
        }
        xemu::screenscraper::Request request{
            entry.title_id,
            path_to_utf8(std::filesystem::path(entry.path).filename()),
            entry.size,
        };
        xemu::screenscraper::Metadata metadata;
        bool fresh = false;
        if (xemu::screenscraper::load_cached(
                root, request, screenscraper_settings, &metadata, &fresh)) {
            entry.metadata_name = metadata.name;
            entry.screenscraper_game_id = metadata.game_id;
            entry.media_paths = metadata.media_paths;
            entry.details = metadata.details;
            entry.cover_path = xemu::screenscraper::choose_cover(
                metadata, screenscraper_settings);
        }
        if (!fresh) {
            fetches.push_back(entry.title_id);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &entry : scanned) {
            auto stored = records.find(entry.token);
            if (stored != records.end() && entry.title_id != 0 &&
                (stored->second.metadata_name != entry.metadata_name ||
                 stored->second.screenscraper_game_id !=
                     entry.screenscraper_game_id)) {
                stored->second.metadata_name = entry.metadata_name;
                stored->second.screenscraper_game_id =
                    entry.screenscraper_game_id;
                records_changed = true;
            }
        }
        std::sort(scanned.begin(), scanned.end(),
                  [](const LibraryEntry &a, const LibraryEntry &b) {
                      int by_label = g_utf8_collate(a.label().c_str(),
                                                   b.label().c_str());
                      return by_label != 0 ? by_label < 0 : a.path < b.path;
                  });
        entries = std::move(scanned);
        scan_complete = true;
        if (!current.token.empty() &&
            std::none_of(entries.begin(), entries.end(),
                         [this](const LibraryEntry &entry) {
                             return entry.token == current.token;
                         })) {
            current.token.clear();
        }
        visible_generation++;
        if (records_changed) {
            cache_snapshot = snapshot_cache_locked(true);
        }
    }

    queue_cache_snapshot(std::move(cache_snapshot));

    std::sort(fetches.begin(), fetches.end());
    fetches.erase(std::unique(fetches.begin(), fetches.end()), fetches.end());
    for (uint32_t title_id : fetches) {
        schedule_metadata_fetch(title_id);
    }

    /* Image reads never run on this caller, which may be the UI thread. */
    queue_identification(std::move(probes));
}

void GameLibrary::start_identify_worker()
{
    QemuThread thread;
    qemu_thread_create(&thread, "game_identify", identify_worker, this,
                       QEMU_THREAD_DETACHED);
}

void GameLibrary::queue_identification(std::vector<ImageProbe> probes)
{
    if (probes.empty()) {
        return;
    }
    bool start_worker = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (ImageProbe &probe : probes) {
            identify_queue.push_back(std::move(probe));
        }
        start_worker = !identify_worker_started;
        identify_worker_started = true;
    }
    if (start_worker) {
        start_identify_worker();
    }
}

void *GameLibrary::identify_worker(void *opaque)
{
    GameLibrary *library = (GameLibrary *)opaque;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    /* One worker reads images in scan order, then runs any requested library
     * download, so a disc is never read twice at once. */
    constexpr unsigned kProbesPerPersist = 32;
    unsigned unsaved_probes = 0;
    for (;;) {
        ImageProbe probe;
        bool download = false;
        bool overwrite = false;
        bool idle = false;
        std::string games_dir;
        {
            std::lock_guard<std::mutex> lock(library->mutex);
            if (library->download_request_pending) {
                library->download_request_pending = false;
                download = true;
                overwrite = library->download_overwrite;
                games_dir = std::move(library->download_games_dir);
            } else if (!library->identify_queue.empty()) {
                probe = std::move(library->identify_queue.front());
                library->identify_queue.pop_front();
            } else {
                library->identify_worker_started = false;
                idle = true;
            }
        }

        /* No C++ exception may cross the detached QEMU thread entry point. */
        if (idle) {
            try {
                library->persist_identified_records();
            } catch (...) {
            }
            return nullptr;
        }
        try {
            if (download) {
                library->run_library_download(overwrite, games_dir);
            } else {
                library->identify_image(probe);
                /* A suspended app keeps most of a long first scan. */
                if (++unsaved_probes >= kProbesPerPersist) {
                    unsaved_probes = 0;
                    library->persist_identified_records();
                }
            }
        } catch (...) {
            if (download) {
                try {
                    library->finish_library_download();
                } catch (...) {
                }
            }
        }
    }
}

void GameLibrary::identify_image(const ImageProbe &probe, bool schedule_fetch)
{
    auto unchanged = [&probe](const CachedRecord &record) {
        return record.identity == probe.identity &&
               record.size == probe.size && record.mtime == probe.mtime;
    };
    auto update_entries_locked = [this, &probe](const CachedRecord &record) {
        bool updated = false;
        for (LibraryEntry &entry : entries) {
            if (entry.token == probe.token && entry.identity == probe.identity &&
                entry.size == probe.size && entry.mtime == probe.mtime &&
                entry.title_id == 0) {
                entry.title_id = record.title_id;
                entry.xbe_name = record.xbe_name;
                entry.metadata_name = record.metadata_name;
                entry.screenscraper_game_id = record.screenscraper_game_id;
                visible_generation++;
                updated = true;
            }
        }
        return updated;
    };

    std::string root;
    uint32_t known_title_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto record = records.find(probe.token);
        if (record == records.end() || !unchanged(record->second) ||
            (record->second.title_id == 0 && record->second.image_probed)) {
            return;
        }
        if (record->second.title_id != 0) {
            /* Identified while queued: only a rescan publishing the entry
             * in between can have left it without the title. */
            if (!update_entries_locked(record->second)) {
                return;
            }
            known_title_id = record->second.title_id;
            root = cache_root;
        }
    }
    if (known_title_id != 0) {
        load_identified_metadata(probe, known_title_id, root, schedule_fetch);
        return;
    }

    DiscIdentity disc;
    DiscProbe result = probe_disc_identity(probe.path, &disc);
    if (result == DiscProbe::kUnreadable) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto record = records.find(probe.token);
        if (record == records.end() || !unchanged(record->second) ||
            record->second.title_id != 0) {
            return;
        }
        if (result == DiscProbe::kIdentified) {
            record->second.title_id = disc.title_id;
            record->second.xbe_name = disc.name;
            update_entries_locked(record->second);
        } else {
            record->second.image_probed = true;
        }
        identify_records_changed = true;
        root = cache_root;
    }
    if (result == DiscProbe::kIdentified) {
        load_identified_metadata(probe, disc.title_id, root, schedule_fetch);
    }
}

void GameLibrary::load_identified_metadata(const ImageProbe &probe,
                                           uint32_t title_id,
                                           const std::string &root,
                                           bool schedule_fetch)
{
    /* As refresh_directory() does for known titles: publish cached
     * artwork now and queue a download when it is missing or stale. */
    xemu::screenscraper::SettingsSnapshot settings =
        xemu::screenscraper::settings_snapshot();
    xemu::screenscraper::Request request{
        title_id,
        path_to_utf8(std::filesystem::path(probe.path).filename()),
        probe.size,
    };
    xemu::screenscraper::Metadata metadata;
    bool fresh = false;
    if (xemu::screenscraper::load_cached(root, request, settings, &metadata,
                                         &fresh)) {
        std::lock_guard<std::mutex> lock(mutex);
        auto record = records.find(probe.token);
        if (record != records.end() && record->second.title_id == title_id &&
            (record->second.metadata_name != metadata.name ||
             record->second.screenscraper_game_id != metadata.game_id)) {
            record->second.metadata_name = metadata.name;
            record->second.screenscraper_game_id = metadata.game_id;
            identify_records_changed = true;
        }
        for (LibraryEntry &entry : entries) {
            if (entry.token == probe.token && entry.title_id == title_id) {
                entry.metadata_name = metadata.name;
                entry.screenscraper_game_id = metadata.game_id;
                entry.media_paths = metadata.media_paths;
                entry.details = metadata.details;
                entry.cover_path =
                    xemu::screenscraper::choose_cover(metadata, settings);
                visible_generation++;
            }
        }
    }
    if (!fresh && schedule_fetch) {
        schedule_metadata_fetch(title_id);
    }
}

void GameLibrary::persist_identified_records()
{
    CacheSnapshot cache_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!identify_records_changed) {
            return;
        }
        identify_records_changed = false;
        cache_snapshot = snapshot_cache_locked(true);
    }
    queue_cache_snapshot(std::move(cache_snapshot));
}

bool GameLibrary::start_library_download(bool overwrite,
                                         XemuGameLibraryDownloadDone done,
                                         void *opaque)
{
    /* Initialization, the scan and every image read happen on the
     * identification worker. */
    prewarm_initialization();
    const char *configured = g_config.general.games_dir;
    std::string games_dir = configured != nullptr ? configured : "";

    bool start_worker = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (download_running.load(std::memory_order_acquire)) {
            return false;
        }
        download_identifying.store(true, std::memory_order_relaxed);
        download_done.store(0, std::memory_order_relaxed);
        download_total.store(0, std::memory_order_relaxed);
        download_failed.store(0, std::memory_order_relaxed);
        download_running.store(true, std::memory_order_release);
        download_done_callback = done;
        download_done_opaque = opaque;
        download_request_pending = true;
        download_overwrite = overwrite;
        download_games_dir = std::move(games_dir);
        start_worker = !identify_worker_started;
        identify_worker_started = true;
    }
    if (start_worker) {
        start_identify_worker();
    }
    return true;
}

void GameLibrary::run_library_download(bool overwrite,
                                       const std::string &games_dir)
{
    ensure_initialized();
    bool scanned = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        scanned = scan_complete;
    }
    if (!scanned) {
        refresh_directory(games_dir);
    }

    /* Identify every image first so its title is downloaded as well; the
     * whole library is queued below, so nothing is fetched yet. */
    std::vector<ImageProbe> probes;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const LibraryEntry &entry : entries) {
            if (entry.title_id == 0) {
                probes.push_back({ entry.token, entry.identity, entry.path,
                                   entry.size, entry.mtime });
            }
        }
    }
    download_total.store((uint32_t)probes.size(), std::memory_order_relaxed);
    for (const ImageProbe &probe : probes) {
        identify_image(probe, false);
        download_done.fetch_add(1, std::memory_order_relaxed);
    }
    persist_identified_records();

    std::vector<uint32_t> titles;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const LibraryEntry &entry : entries) {
            if (entry.title_id != 0) {
                titles.push_back(entry.title_id);
            }
        }
    }
    std::sort(titles.begin(), titles.end());
    titles.erase(std::unique(titles.begin(), titles.end()), titles.end());

    /* The total is published before any title is queued, so the last
     * completion is the only one that can reach it. */
    download_done.store(0, std::memory_order_relaxed);
    download_failed.store(0, std::memory_order_relaxed);
    download_total.store((uint32_t)titles.size(), std::memory_order_relaxed);
    download_identifying.store(false, std::memory_order_relaxed);
    if (titles.empty()) {
        finish_library_download();
        return;
    }
    for (uint32_t title_id : titles) {
        schedule_metadata_fetch(title_id, overwrite, true);
    }
}

void GameLibrary::note_library_download(bool failed)
{
    if (!download_running.load(std::memory_order_acquire)) {
        return;
    }
    if (failed) {
        download_failed.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t done = download_done.fetch_add(1, std::memory_order_relaxed) + 1;
    if (done >= download_total.load(std::memory_order_relaxed)) {
        finish_library_download();
    }
}

void GameLibrary::finish_library_download()
{
    XemuGameLibraryDownloadDone done = nullptr;
    void *opaque = nullptr;
    uint32_t total = download_total.load(std::memory_order_relaxed);
    uint32_t failed = download_failed.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!download_running.load(std::memory_order_relaxed)) {
            return;
        }
        done = download_done_callback;
        opaque = download_done_opaque;
        download_done_callback = nullptr;
        download_done_opaque = nullptr;
        download_identifying.store(false, std::memory_order_relaxed);
        download_running.store(false, std::memory_order_release);
    }
    if (done != nullptr) {
        done(total, failed, opaque);
    }
}

void GameLibrary::download_status(XemuGameLibraryDownloadStatus *status) const
{
    status->running = download_running.load(std::memory_order_acquire);
    status->identifying = download_identifying.load(std::memory_order_relaxed);
    status->done = download_done.load(std::memory_order_relaxed);
    status->total = download_total.load(std::memory_order_relaxed);
}

void *GameLibrary::scan_worker(void *opaque)
{
    std::unique_ptr<ScanWork> work((ScanWork *)opaque);
    GameLibrary *library = work->library;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    /* Initialization may still be loading on its own worker. Any wait and all
     * directory I/O stay here, never on the BQL/UI caller. */
    library->ensure_initialized();
    library->refresh_directory(work->directory);

    std::lock_guard<std::mutex> lock(library->mutex);
    library->background_scan_in_progress = false;
    return nullptr;
}

uint64_t GameLibrary::generation()
{
    /* Generation polling must never wait for LocalState cache parsing. */
    prewarm_initialization();
    std::lock_guard<std::mutex> lock(mutex);
    return visible_generation;
}

XemuGameLibraryEntry *GameLibrary::snapshot(size_t *count)
{
    if (count == nullptr) {
        return nullptr;
    }
    *count = 0;

    ensure_initialized();
    bool needs_scan = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        needs_scan = !scan_complete;
    }
    if (needs_scan) {
        refresh();
    }

    std::lock_guard<std::mutex> lock(mutex);
    return snapshot_locked(count);
}

XemuGameLibraryEntry *GameLibrary::snapshot_locked(size_t *count) const
{
    if (entries.empty()) {
        return nullptr;
    }

    XemuGameLibraryEntry *result = g_new0(XemuGameLibraryEntry, entries.size());
    const xemu::screenscraper::SettingsSnapshot settings =
        xemu::screenscraper::settings_snapshot();
    for (size_t i = 0; i < entries.size(); i++) {
        const LibraryEntry &entry = entries[i];
        result[i].token = g_strdup(entry.token.c_str());
        result[i].label = g_strdup(entry.label().c_str());
        result[i].path = g_strdup(entry.path.c_str());
        result[i].cover_path = g_strdup(entry.cover_path.c_str());
        result[i].large_art_path = g_strdup(
            xemu::screenscraper::choose_large_art(entry.media_paths, settings)
                .c_str());
        result[i].background_path = g_strdup(
            xemu::screenscraper::choose_background(entry.media_paths, settings)
                .c_str());
        result[i].video_path = g_strdup(
            xemu::screenscraper::choose_video(entry.media_paths, settings)
                .c_str());
        result[i].logo_path = g_strdup(
            xemu::screenscraper::choose_logo(entry.media_paths).c_str());
        result[i].synopsis = g_strdup(entry.details.synopsis.c_str());
        result[i].developer = g_strdup(entry.details.developer.c_str());
        result[i].publisher = g_strdup(entry.details.publisher.c_str());
        result[i].genre = g_strdup(entry.details.genre.c_str());
        result[i].players = g_strdup(entry.details.players.c_str());
        result[i].release_date = g_strdup(entry.details.release_date.c_str());
        result[i].rating = g_strdup(entry.details.rating.c_str());
        result[i].title_id = entry.title_id;
        result[i].has_metadata = !entry.metadata_name.empty();
    }
    *count = entries.size();
    return result;
}

XemuGameLibraryEntry *GameLibrary::snapshot_nonblocking(size_t *count,
                                                        bool *available)
{
    if (available != nullptr) {
        *available = false;
    }
    if (count == nullptr) {
        return nullptr;
    }
    *count = 0;

    /* This only starts cache parsing on a worker. It never waits for parsing
     * to finish, and the scan worker below performs all directory I/O. */
    prewarm_initialization();

    bool start_scan = false;
    XemuGameLibraryEntry *result = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return nullptr;
        }
        if (!scan_complete && !background_scan_in_progress) {
            background_scan_in_progress = true;
            start_scan = true;
        }
        result = snapshot_locked(count);
        if (available != nullptr) {
            *available = true;
        }
    }

    if (start_scan) {
        const char *configured = g_config.general.games_dir;
        ScanWork *work = new ScanWork{
            this, configured ? configured : ""
        };
        QemuThread thread;
        qemu_thread_create(&thread, "game_scan", scan_worker, work,
                           QEMU_THREAD_DETACHED);
    }
    return result;
}

void GameLibrary::screenscraper_settings_changed()
{
    /* The settings UI only records durable work and returns. A dedicated
     * worker waits for initialization/library state and coalesces rapid edits
     * without ever losing the final policy when the state mutex is busy. */
    prewarm_initialization();
    const char *configured = g_config.general.games_dir;
    std::string games_dir = configured != nullptr ? configured : "";

    bool start_worker = false;
    {
        std::lock_guard<std::mutex> lock(screenscraper_settings_queue_mutex);
        screenscraper_settings_change_pending = true;
        screenscraper_settings_games_dir = std::move(games_dir);
        if (!screenscraper_settings_worker_started) {
            screenscraper_settings_worker_started = true;
            start_worker = true;
        }
    }
    if (start_worker) {
        QemuThread thread;
        qemu_thread_create(&thread, "screenscraper_settings",
                           screenscraper_settings_worker, this,
                           QEMU_THREAD_DETACHED);
    }
}

void *GameLibrary::screenscraper_settings_worker(void *opaque)
{
    GameLibrary *library = (GameLibrary *)opaque;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    for (;;) {
        std::string games_dir;
        {
            std::lock_guard<std::mutex> lock(
                library->screenscraper_settings_queue_mutex);
            if (!library->screenscraper_settings_change_pending) {
                library->screenscraper_settings_worker_started = false;
                return nullptr;
            }
            library->screenscraper_settings_change_pending = false;
            games_dir = std::move(library->screenscraper_settings_games_dir);
        }
        try {
            library->apply_screenscraper_settings_change(games_dir);
        } catch (...) {
            /* No C++ exception may cross the detached QEMU thread entry point. */
        }
    }
}

void GameLibrary::apply_screenscraper_settings_change(
    const std::string &games_dir)
{
    ensure_initialized();
    xemu::screenscraper::SettingsSnapshot screenscraper_settings =
        xemu::screenscraper::settings_snapshot();
    std::vector<uint32_t> fetches;
    bool start_scan = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (LibraryEntry &entry : entries) {
            xemu::screenscraper::Metadata metadata;
            metadata.media_paths = entry.media_paths;
            std::string cover = xemu::screenscraper::choose_cover(
                metadata, screenscraper_settings);
            if (entry.cover_path != cover) {
                entry.cover_path = std::move(cover);
                visible_generation++;
            }
            if (entry.title_id != 0) {
                fetches.push_back(entry.title_id);
            }
        }
        if (!scan_complete && !background_scan_in_progress) {
            background_scan_in_progress = true;
            start_scan = true;
        }
    }

    if (start_scan) {
        ScanWork *work = new ScanWork{ this, games_dir };
        QemuThread thread;
        qemu_thread_create(&thread, "game_scan", scan_worker, work,
                           QEMU_THREAD_DETACHED);
    }

    std::sort(fetches.begin(), fetches.end());
    fetches.erase(std::unique(fetches.begin(), fetches.end()), fetches.end());
    for (uint32_t title_id : fetches) {
        schedule_metadata_fetch(title_id);
    }
}

char *GameLibrary::resolve_token(const char *token)
{
    if (token == nullptr ||
        strncmp(token, kLibraryScheme, strlen(kLibraryScheme)) != 0) {
        return nullptr;
    }

    ensure_initialized();
    bool needs_scan = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        needs_scan = !scan_complete;
    }
    if (needs_scan) {
        refresh();
    }

    std::lock_guard<std::mutex> lock(mutex);
    for (const auto &entry : entries) {
        if (entry.token == token) {
            uint64_t size = 0;
            int64_t mtime = 0;
            if (!fingerprint_file(entry.path, &size, &mtime) ||
                size != entry.size || mtime != entry.mtime) {
                return nullptr;
            }
            return g_strdup(entry.path.c_str());
        }
    }
    return nullptr;
}

char *GameLibrary::resolve_launch_id(uint64_t launch_id)
{
    if (launch_id == 0) {
        return nullptr;
    }

    ensure_initialized();
    bool needs_scan = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        needs_scan = !scan_complete;
    }
    if (needs_scan) {
        refresh();
    }

    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &entry : entries) {
            if (launch_id_for_token(entry.token) == launch_id) {
                token = entry.token;
                break;
            }
        }
    }
    return token.empty() ? nullptr : resolve_token(token.c_str());
}

uint32_t GameLibrary::catalogue_revision()
{
    uint32_t revision = (uint32_t)generation();
    return revision != 0 ? revision : 1;
}

char *GameLibrary::catalogue_json()
{
    struct Item {
        uint64_t launch_id;
        std::string label;
        std::string path;
        std::unordered_map<std::string, std::string> media;
        xemu::screenscraper::Details details;
        uint64_t size;
    };

    /* The first scan runs on a worker scheduled from the main thread; give
     * it time to publish rather than serve an empty catalogue. */
    prewarm_initialization();
    for (int attempt = 0; attempt < 300; attempt++) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (scan_complete) {
                break;
            }
        }
        g_usleep(100 * 1000);
    }

    std::vector<Item> items;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!scan_complete) {
            return nullptr;
        }
        items.reserve(entries.size());
        for (const LibraryEntry &entry : entries) {
            uint64_t launch_id = launch_id_for_token(entry.token);
            if (launch_id != 0) {
                items.push_back({ launch_id, entry.label(), entry.path,
                                  entry.media_paths, entry.details,
                                  entry.size });
            }
        }
    }
    uint32_t revision = catalogue_revision();

    /* Same order as the library screen. */
    std::stable_sort(items.begin(), items.end(),
                     [](const Item &a, const Item &b) {
                         return g_ascii_strcasecmp(a.label.c_str(),
                                                   b.label.c_str()) < 0;
                     });

    json list = json::array();
    for (const Item &item : items) {
        char token[17];
        snprintf(token, sizeof(token), "%016" PRIX64, item.launch_id);

        gchar *base = g_path_get_basename(item.path.c_str());
        std::string file = base;
        g_free(base);
        std::string format;
        size_t dot = file.find_last_of('.');
        if (dot != std::string::npos && dot > 0) {
            format = file.substr(dot + 1);
            std::transform(format.begin(), format.end(), format.begin(),
                           [](unsigned char c) { return g_ascii_tolower(c); });
            file.resize(dot);
        }

        uint32_t media = 0;
        for (unsigned type = 0; type < G_N_ELEMENTS(kCatalogueMedia); type++) {
            if (!catalogue_media_path(item.media, type).empty()) {
                media |= 1u << type;
            }
        }

        json value = {
            { "label", valid_utf8(item.label) },
            { "path", std::string("xemu://") + token },
            { "file", valid_utf8(file) },
            { "core", kCoreName },
            { "core_path", "" },
            { "db", kPlatformName },
            { "crc32", "" },
            { "img", "" },
            { "img_full", "" },
            { "img_short", "" },
            { "runtime", "" },
            { "last_played", "" },
            { "app", kCoreName },
            { "launch_revision", revision },
            { "launch_token", token },
            { "media", media },
            { "format", valid_utf8(format) },
            { "size", item.size },
        };
        const xemu::screenscraper::Details &details = item.details;
        if (!details.synopsis.empty()) {
            value["desc"] = valid_utf8(details.synopsis);
        }
        if (!details.genre.empty()) {
            value["genre"] = valid_utf8(details.genre);
        }
        if (!details.developer.empty()) {
            value["dev"] = valid_utf8(details.developer);
        }
        if (!details.publisher.empty()) {
            value["pub"] = valid_utf8(details.publisher);
        }
        if (!details.players.empty()) {
            value["players"] = valid_utf8(details.players);
        }
        std::string released = catalogue_release_date(details.release_date);
        if (!released.empty()) {
            value["released"] = released;
        }
        std::string rating = catalogue_rating(details.rating);
        if (!rating.empty()) {
            value["rating"] = rating;
        }
        list.push_back(std::move(value));
    }

    size_t count = list.size();
    json root = {
        { "version", 1 },
        { "application", kCoreName },
        { "playlists", json::array({ json{ { "name", kPlatformName },
                                           { "count", count },
                                           { "entries", std::move(list) } } }) },
    };
    try {
        return g_strdup(root.dump().c_str());
    } catch (const json::exception &) {
        return nullptr;
    }
}

char *GameLibrary::catalogue_media(uint64_t launch_id, unsigned media_type)
{
    std::unordered_map<std::string, std::string> media;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto entry = std::find_if(entries.begin(), entries.end(),
                                  [launch_id](const LibraryEntry &candidate) {
                                      return launch_id_for_token(
                                                 candidate.token) == launch_id;
                                  });
        if (launch_id == 0 || entry == entries.end()) {
            return nullptr;
        }
        media = entry->media_paths;
    }
    std::string path = catalogue_media_path(media, media_type);
    return path.empty() ? nullptr : g_strdup(path.c_str());
}

void GameLibrary::note_running_title(const char *disc_path, uint32_t title_id,
                                     const char *xbe_name)
{
    if (disc_path == nullptr || disc_path[0] == '\0' || title_id == 0) {
        return;
    }
    /* Launches outside the library learn their title here; its shaders
     * precompile in the background. */
    nv2a_shader_cache_set_title(title_id);

    std::string path_copy(disc_path);
    std::string name = xbe_name != nullptr ? xbe_name : "";
    uint64_t sequence;

    /* Only start initialization here. The title worker may wait for it, but
     * the HUD/BQL caller never does. */
    prewarm_initialization();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (current.running && current.title_id == title_id &&
            current.disc_path == path_copy && current.xbe_name == name) {
            return;
        }

        sequence = running_title_work_sequence.fetch_add(
                       1, std::memory_order_relaxed) +
                   1;
        current.running = true;
        current.title_id = title_id;
        current.token.clear();
        current.game_name = name;
        current.disc_path = path_copy;
        current.xbe_name = name;
        if (current.game_name.empty()) {
            current.game_name =
                path_to_utf8(std::filesystem::path(path_copy).stem());
        }
    }

    RunningTitleWork *work = new RunningTitleWork{
        this, std::move(path_copy), title_id, std::move(name), sequence
    };
    QemuThread thread;
    qemu_thread_create(&thread, "game_title", running_title_worker, work,
                       QEMU_THREAD_DETACHED);
}

void *GameLibrary::running_title_worker(void *opaque)
{
    std::unique_ptr<RunningTitleWork> work((RunningTitleWork *)opaque);
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    work->library->ensure_initialized();
    work->library->process_running_title(*work);
    return nullptr;
}

void GameLibrary::process_running_title(const RunningTitleWork &work)
{
    const std::string &disc_path = work.disc_path;
    const uint32_t title_id = work.title_id;
    const std::string &name = work.xbe_name;
    std::filesystem::path path(disc_path);
    std::string identity = normalize_identity(path);
    std::string token = token_for_identity(identity);
    uint64_t size = 0;
    int64_t mtime = 0;
    fingerprint_file(path, &size, &mtime);
    bool visible_changed = false;
    bool record_changed = false;
    bool metadata_missing = true;
    std::string root;
    CacheSnapshot cache_snapshot;

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (work.sequence != running_title_work_sequence.load(
                                 std::memory_order_relaxed) ||
            !current.running || current.title_id != title_id ||
            current.disc_path != disc_path || current.xbe_name != name) {
            return;
        }

        CachedRecord &record = records[token];
        std::string previous_identity = record.identity;
        uint64_t previous_size = record.size;
        int64_t previous_mtime = record.mtime;
        uint32_t previous_title_id = record.title_id;
        std::string previous_xbe_name = record.xbe_name;
        if (record.identity != identity || record.size != size ||
            record.mtime != mtime) {
            record = {};
            record.identity = identity;
            record.size = size;
            record.mtime = mtime;
        }
        record.title_id = title_id;
        record.xbe_name = name;

        record_changed = previous_identity != record.identity ||
                         previous_size != record.size ||
                         previous_mtime != record.mtime ||
                         previous_title_id != record.title_id ||
                         previous_xbe_name != record.xbe_name;

        current.running = true;
        current.title_id = title_id;
        current.token.clear();
        current.game_name = !record.metadata_name.empty()
            ? record.metadata_name
            : name;
        current.disc_path = disc_path;
        current.xbe_name = name;

        for (auto &entry : entries) {
            if (entry.token == token && entry.identity == identity) {
                std::string previous_label = entry.label();
                uint32_t previous_entry_title_id = entry.title_id;
                std::string previous_cover = entry.cover_path;
                entry.title_id = title_id;
                entry.xbe_name = name;
                entry.metadata_name = record.metadata_name;
                entry.screenscraper_game_id =
                    record.screenscraper_game_id;
                if (previous_entry_title_id != title_id) {
                    /* Never leave prior-title artwork attached while the new
                     * title's sanitized cache is checked off-mutex. */
                    entry.cover_path.clear();
                    entry.media_paths.clear();
                    entry.details = {};
                }
                current.token = token;
                current.game_name = entry.label();
                visible_changed = previous_label != entry.label() ||
                                  previous_entry_title_id != entry.title_id ||
                                  previous_cover != entry.cover_path;
                break;
            }
        }

        if (current.game_name.empty()) {
            current.game_name = path_to_utf8(path.stem());
        }
        if (visible_changed) {
            visible_generation++;
        }
        if (record_changed) {
            cache_snapshot = snapshot_cache_locked(true);
        }
        root = cache_root;
        metadata_missing = record.metadata_name.empty();
    }

    queue_cache_snapshot(std::move(cache_snapshot));

    /* Storage checks intentionally occur without the library mutex. */
    xemu::screenscraper::SettingsSnapshot screenscraper_settings =
        xemu::screenscraper::settings_snapshot();
    xemu::screenscraper::Request request{
        title_id, path_to_utf8(path.filename()), size
    };
    xemu::screenscraper::Metadata metadata;
    bool cache_fresh = false;
    bool have_metadata = xemu::screenscraper::load_cached(
        root, request, screenscraper_settings, &metadata, &cache_fresh);

    if (have_metadata) {
        CacheSnapshot metadata_snapshot;
        std::lock_guard<std::mutex> lock(mutex);
        if (work.sequence == running_title_work_sequence.load(
                                 std::memory_order_relaxed) &&
            current.running && current.title_id == title_id &&
            current.disc_path == disc_path) {
            CachedRecord &record = records[token];
            if (record.metadata_name != metadata.name ||
                record.screenscraper_game_id != metadata.game_id) {
                record.metadata_name = metadata.name;
                record.screenscraper_game_id = metadata.game_id;
                metadata_snapshot = snapshot_cache_locked(true);
            }
            for (auto &entry : entries) {
                if (entry.token == token && entry.identity == identity &&
                    entry.title_id == title_id) {
                    std::string published_cover =
                        xemu::screenscraper::choose_cover(
                            metadata, screenscraper_settings);
                    entry.metadata_name = metadata.name;
                    entry.screenscraper_game_id = metadata.game_id;
                    entry.media_paths = metadata.media_paths;
                    entry.details = metadata.details;
                    if (entry.cover_path != published_cover) {
                        entry.cover_path = std::move(published_cover);
                    }
                    current.game_name = entry.label();
                    visible_generation++;
                    break;
                }
            }
        }
        queue_cache_snapshot(std::move(metadata_snapshot));
    }

    if (metadata_missing || !cache_fresh) {
        schedule_metadata_fetch(title_id);
    }
}

void GameLibrary::set_not_running()
{
    running_title_work_sequence.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex);
    current.running = false;
    current.title_id = 0;
    current.token.clear();
    current.game_name.clear();
    current.disc_path.clear();
    current.xbe_name.clear();
}

uint32_t GameLibrary::title_id_for_path(const char *path)
{
    if (path == nullptr || path[0] == '\0' ||
        !initialization_ready.load(std::memory_order_acquire)) {
        return 0;
    }

    std::filesystem::path disc(path);
    std::string identity = normalize_identity(disc);
    std::string token = token_for_identity(identity);
    uint64_t size = 0;
    int64_t mtime = 0;
    if (!fingerprint_file(disc, &size, &mtime)) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex);
    for (const auto &entry : entries) {
        if (entry.token == token && entry.identity == identity &&
            entry.size == size && entry.mtime == mtime &&
            entry.title_id != 0) {
            return entry.title_id;
        }
    }
    auto record = records.find(token);
    if (record != records.end() && record->second.identity == identity &&
        record->second.size == size && record->second.mtime == mtime) {
        return record->second.title_id;
    }
    return 0;
}

void GameLibrary::guest_reset(void *opaque)
{
    GameLibrary *library = static_cast<GameLibrary *>(opaque);

    /* The XBE header can only change when the guest launches another image.
     * A reset is the normal Xbox image-launch path, so invalidate the cached
     * identity here instead of synchronizing the CPU and rereading the whole
     * XBE header every two seconds while a title is stable. Delay the probe a
     * little so it observes the image loaded after reset rather than the old
     * header that is still present during early firmware startup. */
    library->running_identity_generation.fetch_add(
        1, std::memory_order_relaxed);
    library->running_title_work_sequence.fetch_add(
        1, std::memory_order_relaxed);
    library->next_running_poll.store(
        g_get_monotonic_time() + kRunningTitleResetSettleUsec,
        std::memory_order_relaxed);
}

void GameLibrary::refresh_running_title()
{
    const char *disc_path = g_config.sys.files.dvd_path;
    bool no_disc = disc_path == nullptr || disc_path[0] == '\0';
    if (no_disc) {
        disc_path = "";
    }

    /* A machine held after reset still has the previous image in memory,
     * and a disc swapped while it runs is followed by a reset, so the first
     * probe comes a full settle period after either. */
    bool held = !no_disc && !runstate_is_running();
    bool swapped = last_disc_path != disc_path;
    if (swapped) {
        last_disc_path = disc_path;
        disc_check_pending = true;
    }
    if (held || swapped) {
        next_running_poll.store(
            g_get_monotonic_time() + kRunningTitleResetSettleUsec,
            std::memory_order_relaxed);
    }

    /* The HUD and the DSU client poll this from the main loop. Start the
     * LocalState prewarm once, then leave the frame immediately until it is
     * published. */
    if (!prewarm_initialization()) {
        return;
    }

    /* Without a disc the session records into title 0's list. */
    if (no_disc) {
        NV2AShaderPrecompileStatus status;
        nv2a_shader_precompile_status(&status);
        if (status.title_id != 0) {
            nv2a_shader_cache_set_title(0);
        }
    }

    if (held || disc_check_pending) {
        /* A different disc already makes the running title stale. */
        bool stale = false;
        {
            std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
            if (lock.owns_lock()) {
                disc_check_pending = false;
                stale = current.running && current.disc_path != disc_path;
            }
        }
        if (stale) {
            set_not_running();
        }
    }
    if (held) {
        return;
    }

    int64_t now = g_get_monotonic_time();
    int64_t deadline = next_running_poll.load(std::memory_order_relaxed);
    if (now < deadline) {
        return;
    }

    /* Clearing a stale title needs no probe, so it also runs while the
     * machine is held. */
    if (no_disc) {
        if (next_running_poll.compare_exchange_strong(
                deadline, now + kRunningTitlePollUsec,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            set_not_running();
        }
        return;
    }
    if (!next_running_poll.compare_exchange_strong(
            deadline, now + kRunningTitlePollUsec,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        /* Another caller claimed this poll interval. */
        return;
    }

    uint64_t identity_generation =
        running_identity_generation.load(std::memory_order_relaxed);
    {
        /* Metadata workers may copy the complete title cache while holding
         * this mutex.  This poll runs under the BQL, so defer a busy probe
         * instead of turning cache maintenance into an emulation hitch. */
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            next_running_poll.store(now + G_USEC_PER_SEC / 4,
                                    std::memory_order_relaxed);
            return;
        }
        if (current.running && current.disc_path == disc_path &&
            observed_running_identity_generation == identity_generation) {
            return;
        }
    }
    std::string probed_disc_path(disc_path);

    struct xbe *xbe = xemu_get_xbe_info();
    if (xbe == nullptr || xbe->cert == nullptr) {
        set_not_running();
        return;
    }

    uint32_t title_id = ldl_le_p(&xbe->cert->m_titleid);
    constexpr size_t title_name_length = 40;
    gunichar2 title_name[title_name_length];
    for (size_t i = 0; i < title_name_length; i++) {
        title_name[i] = lduw_le_p(&xbe->cert->m_title_name[i]);
    }
    char *converted = g_utf16_to_utf8(
        title_name, title_name_length, nullptr, nullptr, nullptr);
    if (converted != nullptr) {
        g_strstrip(converted);
    }

    note_running_title(probed_disc_path.c_str(), title_id,
                       converted ? converted : "");
    g_free(converted);

    std::lock_guard<std::mutex> lock(mutex);
    if (current.running && current.disc_path == probed_disc_path &&
        current.title_id == title_id) {
        observed_running_identity_generation = identity_generation;
    }
}

bool GameLibrary::get_current_state(XemuGameLibraryState *state)
{
    if (state == nullptr) {
        return false;
    }
    memset(state, 0, sizeof(*state));

    if (!prewarm_initialization()) {
        return false;
    }

    /* The DSU client queries this from the BQL-held input update.  Returning no
     * update for one poll is preferable to blocking the guest behind a
     * metadata/cache worker; the previously published frontend state remains
     * valid and the caller retries on its next throttled refresh. */
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    state->running = current.running;
    state->title_id = current.title_id;
    state->content_token = g_strdup(current.token.c_str());
    state->game_name = g_strdup(current.game_name.c_str());
    state->platform_name = g_strdup(kPlatformName);
    state->core_name = g_strdup(kCoreName);
    return true;
}

char *GameLibrary::dup_cache_path()
{
    ensure_initialized();
    std::lock_guard<std::mutex> lock(mutex);
    return g_strdup(cache_root.c_str());
}

} // namespace

extern "C" {

void xemu_game_library_refresh(void)
{
    game_library().refresh();
}

uint64_t xemu_game_library_generation(void)
{
    return game_library().generation();
}

XemuGameLibraryEntry *xemu_game_library_snapshot(size_t *count)
{
    return game_library().snapshot(count);
}

XemuGameLibraryEntry *xemu_game_library_snapshot_nonblocking(size_t *count)
{
    return game_library().snapshot_nonblocking(count);
}

XemuGameLibraryEntry *xemu_game_library_snapshot_try(size_t *count,
                                                     bool *available)
{
    return game_library().snapshot_nonblocking(count, available);
}

void xemu_game_library_free_snapshot(XemuGameLibraryEntry *entries,
                                     size_t count)
{
    if (entries == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        g_free(entries[i].token);
        g_free(entries[i].label);
        g_free(entries[i].path);
        g_free(entries[i].cover_path);
        g_free(entries[i].large_art_path);
        g_free(entries[i].background_path);
        g_free(entries[i].video_path);
        g_free(entries[i].logo_path);
        g_free(entries[i].synopsis);
        g_free(entries[i].developer);
        g_free(entries[i].publisher);
        g_free(entries[i].genre);
        g_free(entries[i].players);
        g_free(entries[i].release_date);
        g_free(entries[i].rating);
    }
    g_free(entries);
}

bool xemu_game_library_startup_pending;

char *xemu_game_library_resolve_token(const char *token)
{
    return game_library().resolve_token(token);
}

uint64_t xemu_game_library_launch_id(const char *token)
{
    return token != nullptr ? launch_id_for_token(token) : 0;
}

char *xemu_game_library_resolve_launch_id(uint64_t launch_id)
{
    return game_library().resolve_launch_id(launch_id);
}

char *xemu_game_library_catalogue_json(void)
{
    return game_library().catalogue_json();
}

uint32_t xemu_game_library_catalogue_revision(void)
{
    return game_library().catalogue_revision();
}

char *xemu_game_library_catalogue_media(uint64_t launch_id,
                                        unsigned media_type)
{
    return game_library().catalogue_media(launch_id, media_type);
}

void xemu_game_library_note_running_title(const char *disc_path,
                                          uint32_t title_id,
                                          const char *xbe_name)
{
    game_library().note_running_title(disc_path, title_id, xbe_name);
}

uint32_t xemu_game_library_title_id_for_path(const char *path)
{
    return game_library().title_id_for_path(path);
}

void xemu_game_library_refresh_running_title(void)
{
    game_library().refresh_running_title();
}

bool xemu_game_library_get_current_state(XemuGameLibraryState *state)
{
    return game_library().get_current_state(state);
}

void xemu_game_library_free_current_state(XemuGameLibraryState *state)
{
    if (state == nullptr) {
        return;
    }
    g_free(state->content_token);
    g_free(state->game_name);
    g_free(state->platform_name);
    g_free(state->core_name);
    memset(state, 0, sizeof(*state));
}

char *xemu_game_library_dup_xdb_cache_path(void)
{
    return game_library().dup_cache_path();
}

char *xemu_game_library_dup_cache_path(void)
{
    return game_library().dup_cache_path();
}

void xemu_game_library_screenscraper_settings_changed(void)
{
    xemu::screenscraper::sync_settings_from_config();
    game_library().screenscraper_settings_changed();
}

bool xemu_game_library_download_assets(bool overwrite,
                                       XemuGameLibraryDownloadDone done,
                                       void *opaque)
{
    return game_library().start_library_download(overwrite, done, opaque);
}

void xemu_game_library_download_status(XemuGameLibraryDownloadStatus *status)
{
    if (status != nullptr) {
        game_library().download_status(status);
    }
}

} // extern "C"
