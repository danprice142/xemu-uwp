/*
 * ScreenScraper API v2 provider and local metadata/media cache.
 *
 * Source URLs returned by ScreenScraper contain credentials. They exist only
 * in worker-local memory long enough to download a media object; cache files
 * contain sanitized responses, hashes, and relative local paths only.
 */

#include "qemu/osdep.h"

#include "ui/xemu-screenscraper.h"

#include "ui/xemu-settings.h"

#if defined(CONFIG_UWP) || defined(XEMU_UWP)
#include "screenscraper-build-secrets.h"
#endif

#ifdef XEMU_UWP
#include "ui/uwp-password-vault.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/base.h>
#endif

#include <glib/gstdio.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace xemu::screenscraper {
namespace {

constexpr const char *kApiEndpoint =
    "https://api.screenscraper.fr/api2/jeuInfos.php";
constexpr const char *kVaultResource = "xemu/screenscraper";
constexpr int kXboxSystemId = 32;
constexpr int64_t kMetadataMaxAgeSeconds = 7 * 24 * 60 * 60;
constexpr gsize kMaxResponseBytes = 8 * 1024 * 1024;
constexpr gsize kMaxImageBytes = 32 * 1024 * 1024;
constexpr gsize kMaxManualBytes = 128 * 1024 * 1024;
constexpr gsize kMaxVideoBytes = 512 * 1024 * 1024;

struct MediaCandidate {
    std::string type;
    std::string region;
    std::string support;
    std::string version;
    std::string format;
    std::string url;
    std::string crc;
    std::string md5;
    std::string sha1;
    uint64_t size = 0;
    size_t response_index = 0;
};

static std::mutex published_settings_mutex;
static SettingsSnapshot published_settings;

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

static bool read_file_limited(const std::string &path, gsize max_size,
                              std::string *contents)
{
    GStatBuf stat = {};
    if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR) ||
        g_stat(path.c_str(), &stat) != 0 || stat.st_size < 0 ||
        (uint64_t)stat.st_size > max_size) {
        return false;
    }
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
    bool ok = g_file_set_contents(path.c_str(), (const char *)data, size,
                                  &error);
    g_clear_error(&error);
    g_remove(temporary.c_str());
    return ok;
}

static bool file_is_fresh(const std::string &path)
{
    GStatBuf stat = {};
    if (g_stat(path.c_str(), &stat) != 0) {
        return false;
    }
    time_t now = time(nullptr);
    return stat.st_mtime >= now ||
           now - stat.st_mtime <= kMetadataMaxAgeSeconds;
}

static std::string scalar_text(const json &value)
{
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    return {};
}

static std::string member_string(const json &object, const char *key)
{
    if (!object.is_object()) {
        return {};
    }
    auto value = object.find(key);
    if (value == object.end() || !value->is_string()) {
        return {};
    }
    return value->get_ref<const std::string &>();
}

static bool member_bool(const json &object, const char *key,
                        bool fallback = false)
{
    if (!object.is_object()) {
        return fallback;
    }
    auto value = object.find(key);
    return value != object.end() && value->is_boolean()
               ? value->get<bool>()
               : fallback;
}

static uint64_t scalar_size(const json &value)
{
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        int64_t result = value.get<int64_t>();
        return result > 0 ? (uint64_t)result : 0;
    }
    if (value.is_string()) {
        char *end = nullptr;
        unsigned long long result = g_ascii_strtoull(
            value.get_ref<const std::string &>().c_str(), &end, 10);
        return end != nullptr && *end == '\0' ? (uint64_t)result : 0;
    }
    return 0;
}

static std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return g_ascii_tolower(c); });
    return value;
}

static std::string safe_component(const std::string &input,
                                  const char *fallback)
{
    std::string result;
    result.reserve(input.size());
    for (unsigned char c : input) {
        if (g_ascii_isalnum(c) || c == '-' || c == '_') {
            result.push_back((char)g_ascii_tolower(c));
        } else if (c == ' ' || c == '.') {
            result.push_back('-');
        }
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    return result.empty() ? fallback : result;
}

static int region_rank(const std::string &region)
{
    static constexpr std::array<const char *, 6> preference = {
        "wor", "us", "eu", "uk", "jp", "cus"
    };
    std::string normalized = lowercase(region);
    for (size_t i = 0; i < preference.size(); i++) {
        if (normalized == preference[i]) {
            return (int)i;
        }
    }
    /* Regionless media should beat an arbitrary unrecognized region. */
    return normalized.empty() ? (int)preference.size()
                              : (int)preference.size() + 1;
}

static std::string choose_localized_text(const json &items)
{
    if (!items.is_array()) {
        return {};
    }
    std::string best;
    int best_rank = INT_MAX;
    for (const json &item : items) {
        if (!item.is_object()) {
            continue;
        }
        std::string text = member_string(item, "text");
        if (text.empty()) {
            continue;
        }
        int rank = region_rank(member_string(item, "region"));
        if (best.empty() || rank < best_rank) {
            best = std::move(text);
            best_rank = rank;
        }
    }
    return best;
}

/* Bumped when the parsing of provider details changes. */
static constexpr int kDetailsVersion = 2;

/* Decodes the HTML character references the provider leaves in text. */
static std::string decode_entities(const std::string &text)
{
    if (text.find('&') == std::string::npos) {
        return text;
    }
    static const std::pair<const char *, const char *> kNamed[] = {
        { "quot", "\"" }, { "amp", "&" },  { "apos", "'" },
        { "lt", "<" },    { "gt", ">" },   { "nbsp", " " },
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        size_t semi = text[i] == '&' ? text.find(';', i + 1) : std::string::npos;
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back(text[i++]);
            continue;
        }
        std::string name = text.substr(i + 1, semi - i - 1);
        bool decoded = false;
        if (!name.empty() && name[0] == '#') {
            char *end = nullptr;
            bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            unsigned long code =
                strtoul(name.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10);
            if (end != nullptr && *end == '\0' && code > 0 && code < 0x110000) {
                char utf8[8];
                int length = g_unichar_to_utf8((gunichar)code, utf8);
                out.append(utf8, length);
                decoded = true;
            }
        } else {
            for (const auto &[entity, value] : kNamed) {
                if (name == entity) {
                    out += value;
                    decoded = true;
                    break;
                }
            }
        }
        if (decoded) {
            i = semi + 1;
        } else {
            out.push_back(text[i++]);
        }
    }
    return out;
}

static void decode_details(Details *details)
{
    for (std::string *field :
         { &details->synopsis, &details->developer, &details->publisher,
           &details->genre, &details->players, &details->release_date,
           &details->rating }) {
        *field = decode_entities(*field);
    }
}

/* Text in English when available, otherwise the first language given. */
static std::string choose_language_text(const json &items)
{
    if (!items.is_array()) {
        return {};
    }
    std::string fallback;
    for (const json &item : items) {
        std::string text = member_string(item, "text");
        if (text.empty()) {
            continue;
        }
        if (lowercase(member_string(item, "langue")) == "en") {
            return text;
        }
        if (fallback.empty()) {
            fallback = std::move(text);
        }
    }
    return fallback;
}

/* "text" of an { "id", "text" } member, or a plain string member. */
static std::string member_text(const json &object, const char *key)
{
    if (!object.is_object()) {
        return {};
    }
    auto value = object.find(key);
    if (value == object.end()) {
        return {};
    }
    if (value->is_object()) {
        return member_string(*value, "text");
    }
    return scalar_text(*value);
}

static Details parse_details(const json &game)
{
    Details details;
    auto synopsis = game.find("synopsis");
    if (synopsis != game.end()) {
        details.synopsis = choose_language_text(*synopsis);
    }
    details.developer = member_text(game, "developpeur");
    details.publisher = member_text(game, "editeur");
    details.players = member_text(game, "joueurs");
    details.rating = member_text(game, "note");
    auto dates = game.find("dates");
    if (dates != game.end()) {
        details.release_date = choose_localized_text(*dates);
    }

    /* The main genre, or the first one listed. */
    auto genres = game.find("genres");
    if (genres != game.end() && genres->is_array()) {
        for (int pass = 0; pass < 2 && details.genre.empty(); pass++) {
            for (const json &genre : *genres) {
                if (pass == 0 && member_text(genre, "principale") != "1") {
                    continue;
                }
                auto genre_names = genre.find("noms");
                if (genre_names != genre.end()) {
                    details.genre = choose_language_text(*genre_names);
                }
                if (!details.genre.empty()) {
                    break;
                }
            }
        }
    }
    decode_details(&details);
    return details;
}

static std::string game_dir(const std::string &root, const std::string &serial)
{
    return path_to_utf8(std::filesystem::path(root) / "games" / serial);
}

static std::string manifest_path(const std::string &root,
                                 const std::string &serial)
{
    return path_to_utf8(std::filesystem::path(game_dir(root, serial)) /
                        "metadata.json");
}

static std::string sanitized_response_path(const std::string &root,
                                           const std::string &serial)
{
    return path_to_utf8(std::filesystem::path(game_dir(root, serial)) /
                        "response.sanitized.json");
}

static bool safe_relative_path(const std::string &relative)
{
    std::filesystem::path path(relative);
    if (relative.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory() || relative.find(':') != std::string::npos) {
        return false;
    }
    for (const auto &part : path) {
        if (part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

static std::string policy_signature(const SettingsSnapshot &settings)
{
    const auto &media = settings.media;
    return std::to_string(settings.box_art_style) + "," +
           std::to_string(settings.large_box_art_style) + "," +
           std::to_string(settings.background_style) +
           ":" + (media.box_front ? "1" : "0") +
           (media.box_3d ? "1" : "0") +
           (media.box_back ? "1" : "0") +
           (media.wheels ? "1" : "0") +
           (media.physical_media ? "1" : "0") +
           (media.mix_v1 ? "1" : "0") +
           (media.mix_v2 ? "1" : "0") +
           (media.fan_art ? "1" : "0") +
           (media.screenshots ? "1" : "0") +
           (media.videos ? "1" : "0") +
           ":" + settings.media_root;
}

static void rebuild_media_paths(const std::string &media_root,
                                const json &entries, Metadata *metadata)
{
    metadata->media_paths.clear();
    using MediaRank = std::tuple<int, int, std::string>;
    std::unordered_map<std::string, MediaRank> ranks;
    if (!entries.is_array()) {
        return;
    }
    for (const json &entry : entries) {
        if (!entry.is_object()) {
            continue;
        }
        std::string type = member_string(entry, "type");
        std::string relative = member_string(entry, "path");
        if (type.empty() || !safe_relative_path(relative)) {
            continue;
        }
        std::string absolute = path_to_utf8(
            std::filesystem::path(media_root) / relative);
        if (!g_file_test(absolute.c_str(), G_FILE_TEST_IS_REGULAR)) {
            continue;
        }
        MediaRank rank{
            member_bool(entry, "current") ? 0 : 1,
            region_rank(member_string(entry, "region")),
            relative,
        };
        auto current = ranks.find(type);
        if (current == ranks.end() || rank < current->second) {
            ranks[type] = rank;
            metadata->media_paths[type] = std::move(absolute);
        }
    }
}

static bool parse_manifest(const std::string &media_root,
                           const Request &request,
                           const std::string &contents, Metadata *metadata,
                           json *media_entries, bool *download_complete,
                           std::string *stored_policy)
{
    try {
        json root_json = json::parse(contents);
        if (!root_json.is_object() ||
            root_json.value("provider", std::string()) != "screenscraper" ||
            root_json.value("system_id", 0) != kXboxSystemId ||
            root_json.value("title_id", (uint32_t)0) != request.title_id ||
            root_json.value("serial", std::string()) !=
                serial_for_title(request.title_id)) {
            return false;
        }
        const json &stored_media = root_json.value("media", json::array());
        if (!stored_media.is_array()) {
            return false;
        }
        metadata->requested_title_id = request.title_id;
        metadata->serial = root_json.value("serial", std::string());
        metadata->game_id = root_json.value("game_id", std::string());
        metadata->rom_id = root_json.value("rom_id", std::string());
        metadata->name = root_json.value("name", std::string());
        if (metadata->name.empty()) {
            return false;
        }
        /* Older layouts are re-read from the cached response instead. */
        const json &details = root_json.value("details", json::object());
        if (details.is_object() &&
            details.value("version", 0) == kDetailsVersion) {
            metadata->details.synopsis = member_string(details, "synopsis");
            metadata->details.developer = member_string(details, "developer");
            metadata->details.publisher = member_string(details, "publisher");
            metadata->details.genre = member_string(details, "genre");
            metadata->details.players = member_string(details, "players");
            metadata->details.release_date =
                member_string(details, "release_date");
            metadata->details.rating = member_string(details, "rating");
            decode_details(&metadata->details);
        }
        *media_entries = stored_media;
        *download_complete = root_json.value("download_complete", false);
        *stored_policy = root_json.value("download_policy", std::string());
        rebuild_media_paths(media_root, stored_media, metadata);
        return true;
    } catch (const json::exception &) {
        return false;
    }
}

/* Media types for a box art style, best first. */
static std::vector<std::string> box_art_types(int style)
{
    switch (style) {
    case kBoxArt3D:
        return { "box-3D" };
    case kBoxArtMixV1:
        return { "mixrbv1" };
    case kBoxArtMixV2:
        return { "mixrbv2" };
    case kBoxArtFanArt:
        return { "fanart" };
    case kBoxArtScreenshot:
        return { "ss" };
    case kBoxArtLogo:
        return { "wheel-hd", "wheel" };
    case kBoxArtBack:
        return { "box-2D-back" };
    case kBoxArtDisc:
        return { "support-2D" };
    default:
        return { "box-2D" };
    }
}

static std::vector<std::string> background_types(int style)
{
    switch (style) {
    case kBackgroundFanArt:
        return { "fanart" };
    case kBackgroundScreenshot:
        return { "ss" };
    case kBackgroundVideo:
        return { "video-normalized", "video" };
    case kBackgroundMixV1:
        return { "mixrbv1" };
    case kBackgroundMixV2:
        return { "mixrbv2" };
    default:
        return {};
    }
}

static std::string first_media_path(const MediaPaths &paths,
                                    const std::vector<std::string> &types)
{
    for (const std::string &type : types) {
        auto it = paths.find(type);
        if (it != paths.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return {};
}

static std::vector<std::vector<std::string>> requested_media_groups(
    const SettingsSnapshot &settings)
{
    const auto &media = settings.media;
    std::vector<std::vector<std::string>> groups;
    auto add = [&groups](std::initializer_list<const char *> types) {
        std::vector<std::string> group;
        for (const char *type : types) {
            group.emplace_back(type);
        }
        groups.emplace_back(std::move(group));
    };
    if (media.box_front) {
        add({ "box-2D" });
    }
    if (media.box_3d) {
        add({ "box-3D" });
    }
    if (media.box_back) {
        add({ "box-2D-back" });
    }
    if (media.wheels) {
        add({ "wheel-hd", "wheel" });
    }
    if (media.physical_media) {
        add({ "support-2D" });
    }
    if (media.mix_v1) {
        add({ "mixrbv1" });
    }
    if (media.mix_v2) {
        add({ "mixrbv2" });
    }
    if (media.fan_art) {
        add({ "fanart" });
    }
    if (media.screenshots) {
        add({ "ss" });
    }
    if (media.videos) {
        add({ "video-normalized", "video" });
    }

    /* Whatever the library displays is fetched even if its download is off,
     * along with the 2D box fallback. */
    groups.push_back(box_art_types(settings.box_art_style));
    groups.push_back(box_art_types(settings.large_box_art_style));
    std::vector<std::string> background =
        background_types(settings.background_style);
    if (!background.empty()) {
        groups.push_back(std::move(background));
    }
    if (settings.background_style == kBackgroundVideo) {
        /* Shown until the video plays. */
        groups.push_back({ "fanart" });
        groups.push_back({ "ss" });
    }
    groups.push_back({ "box-2D" });
    return groups;
}

static std::string normalized_format(std::string format)
{
    format = lowercase(format);
    if (!format.empty() && format.front() == '.') {
        format.erase(format.begin());
    }
    if (format == "jpeg") {
        return "jpg";
    }
    if (format == "png" || format == "jpg" || format == "gif" ||
        format == "webp" || format == "mp4" || format == "pdf") {
        return format;
    }
    return {};
}

static gsize size_limit_for(const MediaCandidate &media)
{
    std::string format = normalized_format(media.format);
    if (format == "mp4") {
        return kMaxVideoBytes;
    }
    if (format == "pdf") {
        return kMaxManualBytes;
    }
    return kMaxImageBytes;
}

static bool media_url_is_safe(const std::string &url)
{
    GError *error = nullptr;
    GUri *uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, &error);
    if (uri == nullptr) {
        g_clear_error(&error);
        return false;
    }
    const char *scheme = g_uri_get_scheme(uri);
    const char *host_value = g_uri_get_host(uri);
    int port = g_uri_get_port(uri);
    std::string host = host_value != nullptr ? lowercase(host_value) : "";
    bool host_allowed = host == "screenscraper.fr" ||
                        (host.size() > strlen(".screenscraper.fr") &&
                         g_str_has_suffix(host.c_str(), ".screenscraper.fr"));
    bool safe = scheme != nullptr &&
                g_ascii_strcasecmp(scheme, "https") == 0 && host_allowed &&
                (port == -1 || port == 443);
    g_uri_unref(uri);
    return safe;
}

static bool media_signature_valid(const std::string &format,
                                  const uint8_t *data, size_t size)
{
    if (format == "jpg") {
        return size >= 3 && data[0] == 0xff && data[1] == 0xd8 &&
               data[2] == 0xff;
    }
    if (format == "png") {
        static constexpr uint8_t signature[] = {
            0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
        };
        return size >= sizeof(signature) &&
               memcmp(data, signature, sizeof(signature)) == 0;
    }
    if (format == "gif") {
        return size >= 6 && (memcmp(data, "GIF87a", 6) == 0 ||
                             memcmp(data, "GIF89a", 6) == 0);
    }
    if (format == "webp") {
        return size >= 12 && memcmp(data, "RIFF", 4) == 0 &&
               memcmp(data + 8, "WEBP", 4) == 0;
    }
    if (format == "mp4") {
        return size >= 12 && memcmp(data + 4, "ftyp", 4) == 0;
    }
    if (format == "pdf") {
        return size >= 5 && memcmp(data, "%PDF-", 5) == 0;
    }
    return false;
}

static size_t media_signature_size(const std::string &format)
{
    if (format == "jpg") {
        return 3;
    }
    if (format == "png") {
        return 8;
    }
    if (format == "gif") {
        return 6;
    }
    if (format == "webp" || format == "mp4") {
        return 12;
    }
    if (format == "pdf") {
        return 5;
    }
    return 0;
}

class MediaValidator {
public:
    explicit MediaValidator(const MediaCandidate &candidate)
        : media(candidate), format(normalized_format(candidate.format)),
          max_size(size_limit_for(candidate)),
          required_signature_size(media_signature_size(format))
    {
        if (!media.sha1.empty()) {
            sha1 = g_checksum_new(G_CHECKSUM_SHA1);
        }
        if (!media.md5.empty()) {
            md5 = g_checksum_new(G_CHECKSUM_MD5);
        }
        ready = required_signature_size != 0 &&
                (media.sha1.empty() || sha1 != nullptr) &&
                (media.md5.empty() || md5 != nullptr);
    }

    ~MediaValidator()
    {
        if (sha1 != nullptr) {
            g_checksum_free(sha1);
        }
        if (md5 != nullptr) {
            g_checksum_free(md5);
        }
    }

    MediaValidator(const MediaValidator &) = delete;
    MediaValidator &operator=(const MediaValidator &) = delete;

    bool update(const uint8_t *data, size_t size)
    {
        if (!ready || total_size > max_size || size > max_size - total_size) {
            return false;
        }
        size_t prefix_bytes = std::min(size, prefix.size() - prefix_size);
        if (prefix_bytes != 0) {
            memcpy(prefix.data() + prefix_size, data, prefix_bytes);
            prefix_size += prefix_bytes;
        }
        if (!signature_checked && prefix_size >= required_signature_size) {
            if (!media_signature_valid(format, prefix.data(), prefix_size)) {
                return false;
            }
            signature_checked = true;
        }
        if (sha1 != nullptr) {
            g_checksum_update(sha1, data, size);
        }
        if (md5 != nullptr) {
            g_checksum_update(md5, data, size);
        }
        total_size += size;
        return true;
    }

    bool finish()
    {
        return ready && total_size != 0 && total_size <= max_size &&
               (media.size == 0 || total_size == media.size) &&
               signature_checked &&
               checksum_matches(sha1, media.sha1) &&
               checksum_matches(md5, media.md5);
    }

    uint64_t size() const
    {
        return total_size;
    }

private:
    static bool checksum_matches(GChecksum *checksum,
                                 const std::string &expected)
    {
        if (expected.empty()) {
            return true;
        }
        const char *computed = checksum != nullptr
                                   ? g_checksum_get_string(checksum)
                                   : nullptr;
        return computed != nullptr &&
               g_ascii_strcasecmp(computed, expected.c_str()) == 0;
    }

    const MediaCandidate &media;
    std::string format;
    uint64_t max_size;
    size_t required_signature_size;
    uint64_t total_size = 0;
    std::array<uint8_t, 12> prefix{};
    size_t prefix_size = 0;
    GChecksum *sha1 = nullptr;
    GChecksum *md5 = nullptr;
    bool ready = false;
    bool signature_checked = false;
};

class TemporaryMediaFile {
public:
    explicit TemporaryMediaFile(std::string path) : path_(std::move(path)) {}

    ~TemporaryMediaFile()
    {
        if (file_ != nullptr) {
            fclose(file_);
        }
        if (!released_) {
            g_remove(path_.c_str());
        }
    }

    TemporaryMediaFile(const TemporaryMediaFile &) = delete;
    TemporaryMediaFile &operator=(const TemporaryMediaFile &) = delete;

    bool open()
    {
        g_remove(path_.c_str());
        file_ = g_fopen(path_.c_str(), "wb");
        return file_ != nullptr;
    }

    bool write(const uint8_t *data, size_t size)
    {
        return file_ != nullptr && fwrite(data, 1, size, file_) == size;
    }

    bool close()
    {
        if (file_ == nullptr) {
            return false;
        }
        FILE *file = file_;
        file_ = nullptr;
        return fclose(file) == 0;
    }

    const std::string &path() const
    {
        return path_;
    }

    void release()
    {
        released_ = true;
    }

private:
    std::string path_;
    FILE *file_ = nullptr;
    bool released_ = false;
};

#ifdef XEMU_UWP
class ApartmentScope {
public:
    ApartmentScope()
    {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            owns_apartment = true;
        } catch (const winrt::hresult_error &exception) {
            if (exception.code() != RPC_E_CHANGED_MODE) {
                throw;
            }
        }
    }
    ~ApartmentScope()
    {
        if (owns_apartment) {
            winrt::uninit_apartment();
        }
    }
private:
    bool owns_apartment = false;
};

using AsyncClock = std::chrono::steady_clock;
using AsyncDeadline = AsyncClock::time_point;

static constexpr auto kAsyncPollInterval = std::chrono::milliseconds(50);
static constexpr auto kAsyncCancelGrace = std::chrono::seconds(2);
static constexpr auto kHttpHeaderTimeout = std::chrono::seconds(30);
static constexpr auto kHttpReadIdleTimeout = std::chrono::seconds(30);
static constexpr auto kApiOverallTimeout = std::chrono::seconds(90);

template<typename TAsync>
static bool wait_for_async(const TAsync &operation, AsyncDeadline deadline)
{
    using winrt::Windows::Foundation::AsyncStatus;
    for (;;) {
        AsyncDeadline now = AsyncClock::now();
        AsyncStatus status = operation.Status();
        if (now >= deadline) {
            if (status == AsyncStatus::Started) {
                operation.Cancel();
                AsyncDeadline cancel_deadline = now + kAsyncCancelGrace;
                while (operation.Status() == AsyncStatus::Started &&
                       AsyncClock::now() < cancel_deadline) {
                    std::this_thread::sleep_for(kAsyncPollInterval);
                }
            }
            return false;
        }
        if (status == AsyncStatus::Completed) {
            return true;
        }
        if (status != AsyncStatus::Started) {
            return false;
        }
        std::this_thread::sleep_until(
            std::min(deadline, now + kAsyncPollInterval));
    }
}

static AsyncDeadline operation_deadline(AsyncDeadline overall,
                                        std::chrono::seconds timeout)
{
    return std::min(overall, AsyncClock::now() + timeout);
}

static AsyncDeadline media_overall_deadline(const MediaCandidate &media)
{
    std::string format = normalized_format(media.format);
    if (format == "mp4") {
        return AsyncClock::now() + std::chrono::minutes(45);
    }
    if (format == "pdf") {
        return AsyncClock::now() + std::chrono::minutes(15);
    }
    return AsyncClock::now() + std::chrono::minutes(5);
}
#endif

static bool download_url_to_memory(const std::string &url, gsize max_size,
                                   std::vector<uint8_t> *data, int *status)
{
    data->clear();
    *status = -1;
    if (!media_url_is_safe(url)) {
        return false;
    }
#ifdef XEMU_UWP
    try {
        ApartmentScope apartment;
        winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
        filter.AllowAutoRedirect(false);
        winrt::Windows::Web::Http::HttpClient client(filter);
        AsyncDeadline overall = AsyncClock::now() + kApiOverallTimeout;
        auto get_operation = client.GetAsync(
            winrt::Windows::Foundation::Uri(winrt::to_hstring(url)),
            winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
        if (!wait_for_async(
                get_operation,
                operation_deadline(overall, kHttpHeaderTimeout))) {
            return false;
        }
        auto response = get_operation.GetResults();
        *status = static_cast<int>(response.StatusCode());
        if (*status != 200) {
            return false;
        }

        auto content_length = response.Content().Headers().ContentLength();
        if (content_length && content_length.Value() > max_size) {
            return false;
        }

        auto stream_operation = response.Content().ReadAsInputStreamAsync();
        if (!wait_for_async(
                stream_operation,
                operation_deadline(overall, kHttpHeaderTimeout))) {
            return false;
        }
        auto stream = stream_operation.GetResults();
        constexpr uint32_t kReadChunkBytes = 64 * 1024;
        for (;;) {
            winrt::Windows::Storage::Streams::Buffer buffer(kReadChunkBytes);
            auto read_operation = stream.ReadAsync(
                buffer, kReadChunkBytes,
                winrt::Windows::Storage::Streams::InputStreamOptions::Partial);
            if (!wait_for_async(
                    read_operation,
                    operation_deadline(overall, kHttpReadIdleTimeout))) {
                data->clear();
                return false;
            }
            auto read = read_operation.GetResults();
            uint32_t length = read.Length();
            if (length == 0) {
                break;
            }
            if (data->size() > max_size ||
                length > max_size - data->size()) {
                data->clear();
                return false;
            }
            size_t offset = data->size();
            data->resize(offset + length);
            auto reader = winrt::Windows::Storage::Streams::DataReader::
                FromBuffer(read);
            reader.ReadBytes(winrt::array_view<uint8_t>(
                data->data() + offset, data->data() + offset + length));
        }
        return !data->empty();
    } catch (...) {
        *status = -1;
        return false;
    }
#else
    /* Developer credentials deliberately do not exist in desktop builds. */
    (void)url;
    (void)max_size;
    return false;
#endif
}

static bool publish_temporary_media(TemporaryMediaFile *temporary,
                                    const std::string &target)
{
    if (g_rename(temporary->path().c_str(), target.c_str()) == 0) {
        temporary->release();
        return true;
    }

    /* POSIX rename replaces atomically. Windows CRT rename may not, so only
     * remove an existing regular cache file after the replacement is fully
     * downloaded and validated. The target is never exposed partially. */
    if (!g_file_test(target.c_str(), G_FILE_TEST_IS_REGULAR) ||
        g_remove(target.c_str()) != 0 ||
        g_rename(temporary->path().c_str(), target.c_str()) != 0) {
        return false;
    }
    temporary->release();
    return true;
}

static bool download_media_to_file(const MediaCandidate &media,
                                   const std::string &target)
{
    if (!media_url_is_safe(media.url)) {
        return false;
    }
#ifdef XEMU_UWP
    try {
        ApartmentScope apartment;
        winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
        filter.AllowAutoRedirect(false);
        winrt::Windows::Web::Http::HttpClient client(filter);
        AsyncDeadline overall = media_overall_deadline(media);
        auto get_operation = client.GetAsync(
            winrt::Windows::Foundation::Uri(winrt::to_hstring(media.url)),
            winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
        if (!wait_for_async(
                get_operation,
                operation_deadline(overall, kHttpHeaderTimeout))) {
            return false;
        }
        auto response = get_operation.GetResults();
        if (static_cast<int>(response.StatusCode()) != 200) {
            return false;
        }

        uint64_t max_size = size_limit_for(media);
        auto content_length = response.Content().Headers().ContentLength();
        if (content_length &&
            (content_length.Value() > max_size ||
             (media.size != 0 && content_length.Value() != media.size))) {
            return false;
        }

        std::filesystem::path parent =
            std::filesystem::path(target).parent_path();
        if (!parent.empty() &&
            g_mkdir_with_parents(path_to_utf8(parent).c_str(), 0755) != 0) {
            return false;
        }

        TemporaryMediaFile temporary(target + ".tmp");
        if (!temporary.open()) {
            return false;
        }
        MediaValidator validator(media);
        auto stream_operation = response.Content().ReadAsInputStreamAsync();
        if (!wait_for_async(
                stream_operation,
                operation_deadline(overall, kHttpHeaderTimeout))) {
            return false;
        }
        auto stream = stream_operation.GetResults();
        constexpr uint32_t kReadChunkBytes = 64 * 1024;
        std::array<uint8_t, kReadChunkBytes> chunk{};
        for (;;) {
            winrt::Windows::Storage::Streams::Buffer buffer(kReadChunkBytes);
            auto read_operation = stream.ReadAsync(
                buffer, kReadChunkBytes,
                winrt::Windows::Storage::Streams::InputStreamOptions::Partial);
            if (!wait_for_async(
                    read_operation,
                    operation_deadline(overall, kHttpReadIdleTimeout))) {
                return false;
            }
            auto read = read_operation.GetResults();
            uint32_t length = read.Length();
            if (length == 0) {
                break;
            }
            auto reader = winrt::Windows::Storage::Streams::DataReader::
                FromBuffer(read);
            reader.ReadBytes(winrt::array_view<uint8_t>(
                chunk.data(), chunk.data() + length));
            if (!validator.update(chunk.data(), length) ||
                !temporary.write(chunk.data(), length)) {
                return false;
            }
        }
        if (!temporary.close() || !validator.finish() ||
            (content_length && validator.size() != content_length.Value())) {
            return false;
        }
        return publish_temporary_media(&temporary, target);
    } catch (...) {
        return false;
    }
#else
    (void)media;
    (void)target;
    return false;
#endif
}

static std::string uri_escape(const std::string &value)
{
    char *escaped = g_uri_escape_string(value.c_str(), nullptr, true);
    std::string result = escaped != nullptr ? escaped : "";
    g_free(escaped);
    return result;
}

static void secure_clear(std::string *value)
{
    if (value->empty()) {
        return;
    }
#ifdef _WIN32
    SecureZeroMemory(value->data(), value->size());
#else
    volatile char *bytes = value->data();
    for (size_t i = 0; i < value->size(); i++) {
        bytes[i] = 0;
    }
#endif
    value->clear();
}

static bool build_api_url(const Request &request,
                          const SettingsSnapshot &settings,
                          std::string *url)
{
#if defined(CONFIG_UWP) || defined(XEMU_UWP)
    const std::string &personal_username = settings.username;
    std::string personal_password;
#ifdef XEMU_UWP
    if (!personal_username.empty()) {
        char *vault_error = nullptr;
        char *password = xemu_uwp_password_vault_retrieve(
            kVaultResource, personal_username.c_str(), &vault_error);
        g_free(vault_error);
        if (password != nullptr) {
            personal_password = password;
            SecureZeroMemory(password, strlen(password));
            g_free(password);
        }
    }
#endif

    /* Downloads count against the user's own ScreenScraper account; without
     * one, nothing is requested on the developer allowance. */
    if (personal_username.empty() || personal_password.empty()) {
        secure_clear(&personal_password);
        return false;
    }

    *url = std::string(kApiEndpoint) +
           "?devid=" + uri_escape(XEMU_SCREENSCRAPER_DEV_ID) +
           "&devpassword=" + uri_escape(XEMU_SCREENSCRAPER_DEV_PASSWORD) +
           "&softname=xemu&output=json&systemeid=" +
           std::to_string(kXboxSystemId) + "&romtype=iso&serialnum=" +
           uri_escape(serial_for_title(request.title_id));
    if (!request.rom_filename.empty()) {
        *url += "&romnom=" + uri_escape(request.rom_filename);
    }
    if (request.rom_size != 0) {
        *url += "&romtaille=" + std::to_string(request.rom_size);
    }
    if (!personal_username.empty() && !personal_password.empty()) {
        *url += "&ssid=" + uri_escape(personal_username) +
                "&sspassword=" + uri_escape(personal_password);
    }
    secure_clear(&personal_password);
    return true;
#else
    (void)request;
    (void)url;
    return false;
#endif
}

static void sanitize_json(json *value)
{
    if (value->is_object()) {
        for (auto it = value->begin(); it != value->end(); ++it) {
            std::string key = lowercase(it.key());
            if (key.find("url") != std::string::npos ||
                key.find("password") != std::string::npos ||
                key == "devid" || key == "ssid") {
                it.value() = nullptr;
            } else {
                sanitize_json(&it.value());
            }
        }
    } else if (value->is_array()) {
        for (json &item : *value) {
            sanitize_json(&item);
        }
    } else if (value->is_string()) {
        std::string text = lowercase(value->get_ref<const std::string &>());
        if (text.find("http://") != std::string::npos ||
            text.find("https://") != std::string::npos ||
            text.find("devid=") != std::string::npos ||
            text.find("devpassword=") != std::string::npos ||
            text.find("ssid=") != std::string::npos ||
            text.find("sspassword=") != std::string::npos) {
            *value = nullptr;
        }
    }
}

static std::vector<MediaCandidate> parse_media(const json &items)
{
    std::vector<MediaCandidate> result;
    if (!items.is_array()) {
        return result;
    }
    size_t index = 0;
    for (const json &item : items) {
        size_t response_index = index++;
        if (!item.is_object() || member_string(item, "parent") != "jeu") {
            continue;
        }
        MediaCandidate media;
        media.type = member_string(item, "type");
        media.region = lowercase(member_string(item, "region"));
        media.support = member_string(item, "support");
        media.version = member_string(item, "version");
        media.format = member_string(item, "format");
        media.url = member_string(item, "url");
        media.crc = lowercase(member_string(item, "crc"));
        media.md5 = lowercase(member_string(item, "md5"));
        media.sha1 = lowercase(member_string(item, "sha1"));
        auto size = item.find("size");
        if (size != item.end()) {
            media.size = scalar_size(*size);
        }
        media.response_index = response_index;
        if (!media.type.empty() && media_url_is_safe(media.url) &&
            !normalized_format(media.format).empty() &&
            (media.size == 0 || media.size <= size_limit_for(media))) {
            result.emplace_back(std::move(media));
        }
    }
    return result;
}

static const MediaCandidate *best_candidate(
    const std::vector<MediaCandidate> &media,
    const std::vector<std::string> &types)
{
    for (const std::string &type : types) {
        const MediaCandidate *best = nullptr;
        int best_rank = INT_MAX;
        for (const auto &candidate : media) {
            if (candidate.type != type) {
                continue;
            }
            int rank = region_rank(candidate.region);
            if (best == nullptr || rank < best_rank ||
                (rank == best_rank &&
                 candidate.response_index < best->response_index)) {
                best = &candidate;
                best_rank = rank;
            }
        }
        if (best != nullptr) {
            return best;
        }
    }
    return nullptr;
}

/*
 * Media files use the downloaded_media directory layout so the tree can be
 * shared with other front ends:
 *
 *   <media root>/xbox/<media type directory>/<rom file stem>.<ext>
 *
 * Manifest "path" entries are stored relative to <media root>.
 */
static const char *media_type_directory(const std::string &type)
{
    static const std::pair<const char *, const char *> kDirectories[] = {
        { "box-2D", "covers" },
        { "box-3D", "3dboxes" },
        { "box-2D-back", "backcovers" },
        { "box-texture", "boxtextures" },
        { "mixrbv1", "miximages-v1" },
        { "mixrbv2", "miximages" },
        { "ss", "screenshots" },
        { "sstitle", "titlescreens" },
        { "wheel", "marquees" },
        { "wheel-hd", "marquees" },
        { "wheel-carbon", "marquees" },
        { "wheel-steel", "marquees" },
        { "fanart", "fanart" },
        { "support-2D", "physicalmedia" },
        { "video", "videos" },
        { "video-normalized", "videos" },
        { "manuel", "manuals" },
        { "steamgrid", "steamgrid" },
    };
    for (const auto &[api_type, directory] : kDirectories) {
        if (type == api_type) {
            return directory;
        }
    }
    return nullptr;
}

static std::string rom_stem(const Request &request)
{
    std::filesystem::path rom(request.rom_filename);
    std::string stem = path_to_utf8(rom.stem());
    if (stem.empty()) {
        stem = path_to_utf8(rom.filename());
    }
    /* Reject path separators and reserved characters so the media name can
     * never escape its type directory. */
    std::string cleaned;
    cleaned.reserve(stem.size());
    for (unsigned char c : stem) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            cleaned.push_back('_');
        } else {
            cleaned.push_back((char)c);
        }
    }
    while (!cleaned.empty() &&
           (cleaned.back() == ' ' || cleaned.back() == '.')) {
        cleaned.pop_back();
    }
    if (cleaned.empty() || cleaned == "." || cleaned == "..") {
        cleaned = "game";
    }
    return cleaned;
}

static std::string target_relative_path(const Request &request,
                                        const MediaCandidate &media,
                                        bool disambiguate)
{
    const char *directory = media_type_directory(media.type);
    std::string type_directory = directory != nullptr
                                     ? directory
                                     : safe_component(media.type, "media");
    std::string name = rom_stem(request);
    if (disambiguate) {
        /* "Every available media" downloads several variants of one type;
         * suffix them so they cannot collide inside the type directory. */
        name += "-" + safe_component(media.region, "none");
        if (!media.support.empty()) {
            name += "-" + safe_component(media.support, "support");
        }
        if (!media.version.empty()) {
            name += "-" + safe_component(media.version, "version");
        }
        name += "-" + std::to_string(media.response_index);
    }
    name += "." + normalized_format(media.format);
    return path_to_utf8(std::filesystem::path("xbox") / type_directory / name);
}

static bool existing_payload_valid(const std::string &path,
                                   const MediaCandidate &media)
{
    GStatBuf stat = {};
    if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR) ||
        g_stat(path.c_str(), &stat) != 0 || stat.st_size <= 0) {
        return false;
    }
    uint64_t file_size = (uint64_t)stat.st_size;
    if (file_size > size_limit_for(media) ||
        (media.size != 0 && file_size != media.size)) {
        return false;
    }

    FILE *file = g_fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    MediaValidator validator(media);
    std::array<uint8_t, 64 * 1024> chunk{};
    bool valid = true;
    for (;;) {
        size_t length = fread(chunk.data(), 1, chunk.size(), file);
        if (length != 0 && !validator.update(chunk.data(), length)) {
            valid = false;
            break;
        }
        if (length != chunk.size()) {
            if (ferror(file)) {
                valid = false;
            }
            break;
        }
    }
    if (valid) {
        valid = validator.finish() && validator.size() == file_size;
    }
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

static json media_manifest_entry(const std::string &relative,
                                 const MediaCandidate &media)
{
    return {
        { "type", media.type },
        { "region", media.region },
        { "support", media.support },
        { "version", media.version },
        { "format", normalized_format(media.format) },
        { "size", media.size },
        { "crc", media.crc },
        { "md5", media.md5 },
        { "sha1", media.sha1 },
        { "path", relative },
        { "current", true },
    };
}

static bool download_media(const std::string &media_root,
                           const Request &request,
                           const MediaCandidate &media, bool disambiguate,
                           json *manifest_entry)
{
    std::string relative = target_relative_path(request, media, disambiguate);
    std::string absolute = path_to_utf8(std::filesystem::path(media_root) /
                                        relative);
    /* An overwrite keeps a valid existing file if the new download fails. */
    bool present = request.overwrite
                       ? download_media_to_file(media, absolute) ||
                             existing_payload_valid(absolute, media)
                       : existing_payload_valid(absolute, media) ||
                             download_media_to_file(media, absolute);
    if (!present) {
        return false;
    }
    *manifest_entry = media_manifest_entry(relative, media);
    return true;
}

static bool normalize_cached_media_entry(const std::string &media_root,
                                         const json &entry,
                                         json *normalized)
{
    if (!entry.is_object()) {
        return false;
    }

    MediaCandidate media;
    media.type = member_string(entry, "type");
    media.region = lowercase(member_string(entry, "region"));
    media.support = member_string(entry, "support");
    media.version = member_string(entry, "version");
    media.format = member_string(entry, "format");
    media.crc = lowercase(member_string(entry, "crc"));
    media.md5 = lowercase(member_string(entry, "md5"));
    media.sha1 = lowercase(member_string(entry, "sha1"));
    auto size = entry.find("size");
    if (size != entry.end()) {
        media.size = scalar_size(*size);
    }

    std::string relative = member_string(entry, "path");
    if (media.type.empty() || normalized_format(media.format).empty() ||
        !safe_relative_path(relative) ||
        (media.size != 0 && media.size > size_limit_for(media))) {
        return false;
    }
    std::string absolute = path_to_utf8(std::filesystem::path(media_root) /
                                        relative);
    if (!existing_payload_valid(absolute, media)) {
        return false;
    }

    *normalized = media_manifest_entry(relative, media);
    (*normalized)["current"] = false;
    return true;
}

static bool cached_media_entry_fast_valid(const std::string &media_root,
                                          const json &entry)
{
    /* Library scans can cover many large videos/manuals. Freshness therefore
     * uses bounded structural checks here; download and retention paths still
     * stream the entire payload through the declared SHA-1/MD5 validators. */
    if (!entry.is_object()) {
        return false;
    }

    MediaCandidate media;
    media.type = member_string(entry, "type");
    media.format = member_string(entry, "format");
    auto size = entry.find("size");
    if (size != entry.end()) {
        media.size = scalar_size(*size);
    }

    std::string relative = member_string(entry, "path");
    std::string format = normalized_format(media.format);
    if (media.type.empty() || format.empty() || !safe_relative_path(relative) ||
        (media.size != 0 && media.size > size_limit_for(media))) {
        return false;
    }

    std::string absolute = path_to_utf8(std::filesystem::path(media_root) /
                                        relative);
    GStatBuf stat = {};
    if (!g_file_test(absolute.c_str(), G_FILE_TEST_IS_REGULAR) ||
        g_stat(absolute.c_str(), &stat) != 0 || stat.st_size <= 0) {
        return false;
    }
    uint64_t file_size = (uint64_t)stat.st_size;
    if (file_size > size_limit_for(media) ||
        (media.size != 0 && file_size != media.size)) {
        return false;
    }

    size_t required = media_signature_size(format);
    std::array<uint8_t, 12> prefix{};
    FILE *file = g_fopen(absolute.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    size_t length = fread(prefix.data(), 1, required, file);
    bool valid = length == required && !ferror(file) &&
                 media_signature_valid(format, prefix.data(), length);
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

static bool current_media_entries_fast_valid(const std::string &media_root,
                                             const json &entries)
{
    if (!entries.is_array()) {
        return false;
    }
    for (const json &entry : entries) {
        if (!entry.is_object() || !member_bool(entry, "current")) {
            continue;
        }
        if (!cached_media_entry_fast_valid(media_root, entry)) {
            return false;
        }
    }
    return true;
}

static json load_previous_media_entries(const std::string &root,
                                        const std::string &media_root,
                                        const Request &request)
{
    std::string contents;
    if (!read_file_limited(
            manifest_path(root, serial_for_title(request.title_id)),
            kMaxResponseBytes, &contents)) {
        return json::array();
    }

    Metadata ignored_metadata;
    json entries;
    bool ignored_complete = false;
    std::string ignored_policy;
    if (!parse_manifest(media_root, request, contents, &ignored_metadata,
                        &entries, &ignored_complete, &ignored_policy)) {
        return json::array();
    }

    json result = json::array();
    for (const json &entry : entries) {
        json normalized;
        if (normalize_cached_media_entry(media_root, entry, &normalized)) {
            result.push_back(std::move(normalized));
        }
    }
    return result;
}

static void append_key_field(std::string *key, const std::string &value)
{
    *key += std::to_string(value.size());
    key->push_back(':');
    *key += value;
}

static std::string media_logical_key(const json &entry)
{
    std::string type = member_string(entry, "type");
    if (type.empty()) {
        return {};
    }
    std::string key;
    append_key_field(&key, type);
    append_key_field(&key, lowercase(member_string(entry, "region")));
    append_key_field(&key, member_string(entry, "support"));
    append_key_field(&key, member_string(entry, "version"));
    return key;
}

static std::string media_path_key(const json &entry)
{
    std::string path = member_string(entry, "path");
    std::replace(path.begin(), path.end(), '\\', '/');
#ifdef _WIN32
    path = lowercase(std::move(path));
#endif
    return path;
}

static void insert_canonical_entry(std::map<std::string, json> *entries,
                                   json entry)
{
    std::string path = media_path_key(entry);
    if (path.empty()) {
        return;
    }
    auto current = entries->find(path);
    if (current == entries->end() || entry.dump() < current->second.dump()) {
        (*entries)[path] = std::move(entry);
    }
}

static json merge_media_entries(const json &previous, const json &current)
{
    std::map<std::string, json> previous_by_path;
    std::map<std::string, json> current_by_path;
    if (previous.is_array()) {
        for (json entry : previous) {
            entry["current"] = false;
            insert_canonical_entry(&previous_by_path, std::move(entry));
        }
    }
    if (current.is_array()) {
        for (json entry : current) {
            entry["current"] = true;
            insert_canonical_entry(&current_by_path, std::move(entry));
        }
    }

    std::map<std::string, size_t> previous_logical_counts;
    std::map<std::string, size_t> current_logical_counts;
    for (const auto &[path, entry] : previous_by_path) {
        (void)path;
        previous_logical_counts[media_logical_key(entry)]++;
    }
    for (const auto &[path, entry] : current_by_path) {
        (void)path;
        current_logical_counts[media_logical_key(entry)]++;
    }

    std::set<std::string> replaced_logical_keys;
    for (const auto &[key, count] : current_logical_counts) {
        auto previous_count = previous_logical_counts.find(key);
        if (!key.empty() && count == 1 &&
            previous_count != previous_logical_counts.end() &&
            previous_count->second == 1) {
            replaced_logical_keys.insert(key);
        }
    }

    std::map<std::string, json> merged;
    for (auto &[path, entry] : previous_by_path) {
        if (replaced_logical_keys.count(media_logical_key(entry)) == 0) {
            merged.emplace(path, std::move(entry));
        }
    }
    for (auto &[path, entry] : current_by_path) {
        merged[path] = std::move(entry);
    }

    json result = json::array();
    for (auto &[path, entry] : merged) {
        (void)path;
        result.push_back(std::move(entry));
    }
    return result;
}

} // namespace

std::string default_media_root()
{
#ifdef CONFIG_UWP
    return XEMU_UWP_DEFAULT_MEDIA_DIR;
#else
    const char *base = xemu_settings_get_base_path();
    return path_to_utf8(std::filesystem::path(base ? base : ".") /
                        "screenscraper" / "downloaded_media");
#endif
}

void sync_settings_from_config()
{
    SettingsSnapshot next;
    next.username = g_config.general.screenscraper.username != nullptr
                        ? g_config.general.screenscraper.username
                        : "";
    next.box_art_style = (int)g_config.general.screenscraper.box_art_style;
    next.large_box_art_style =
        (int)g_config.general.screenscraper.large_box_art_style;
    next.background_style = (int)g_config.general.screenscraper.background;
    const auto &media = g_config.general.screenscraper.media;
    next.media.box_front = media.box_front;
    next.media.box_3d = media.box_3d;
    next.media.box_back = media.box_back;
    next.media.wheels = media.wheels;
    next.media.physical_media = media.physical_media;
    next.media.mix_v1 = media.mix_v1;
    next.media.mix_v2 = media.mix_v2;
    next.media.fan_art = media.fan_art;
    next.media.screenshots = media.screenshots;
    next.media.videos = media.videos;
    next.media_root = default_media_root();
    const char *configured_media_dir = g_config.general.screenscraper.media_dir;
    if (configured_media_dir != nullptr && configured_media_dir[0] != '\0') {
        next.media_root = configured_media_dir;
    }

    std::lock_guard<std::mutex> lock(published_settings_mutex);
    next.revision = published_settings.revision + 1;
    published_settings = std::move(next);
}

SettingsSnapshot settings_snapshot()
{
    std::lock_guard<std::mutex> lock(published_settings_mutex);
    return published_settings;
}

std::string serial_for_title(uint32_t title_id)
{
    unsigned char first = (title_id >> 24) & 0xff;
    unsigned char second = (title_id >> 16) & 0xff;
    if (!g_ascii_isalnum(first) || !g_ascii_isalnum(second)) {
        return {};
    }
    char serial[16];
    snprintf(serial, sizeof(serial), "%c%c-%03u",
             g_ascii_toupper(first), g_ascii_toupper(second),
             title_id & 0xffff);
    return serial;
}

bool load_cached(const std::string &cache_root, const Request &request,
                 const SettingsSnapshot &settings, Metadata *metadata,
                 bool *fresh)
{
    *fresh = false;
    if (request.title_id == 0 || serial_for_title(request.title_id).empty()) {
        return false;
    }
    std::string path = manifest_path(cache_root,
                                     serial_for_title(request.title_id));
    std::string contents;
    if (!read_file_limited(path, kMaxResponseBytes, &contents)) {
        return false;
    }
    json media_entries;
    bool download_complete = false;
    std::string stored_policy;
    if (!parse_manifest(settings.media_root, request, contents, metadata,
                        &media_entries, &download_complete, &stored_policy)) {
        return false;
    }
    if (metadata->details.empty()) {
        std::string response_text;
        if (read_file_limited(sanitized_response_path(
                                  cache_root, serial_for_title(request.title_id)),
                              kMaxResponseBytes, &response_text)) {
            try {
                json response = json::parse(response_text);
                const json &game = response.at("response").at("jeu");
                if (game.is_object()) {
                    metadata->details = parse_details(game);
                }
            } catch (const json::exception &) {
            }
        }
    }
    *fresh = file_is_fresh(path) && download_complete &&
             stored_policy == policy_signature(settings) &&
             current_media_entries_fast_valid(settings.media_root,
                                              media_entries);
    return true;
}

static bool fetch_impl(const std::string &cache_root, const Request &request,
                       const SettingsSnapshot &settings, Metadata *metadata)
{
    const std::string serial = serial_for_title(request.title_id);
    if (serial.empty()) {
        return false;
    }

    std::string api_url;
    if (!build_api_url(request, settings, &api_url)) {
        return false;
    }
    std::vector<uint8_t> response_data;
    int status = -1;
    bool downloaded = download_url_to_memory(
        api_url, kMaxResponseBytes, &response_data, &status);
    secure_clear(&api_url);
    if (!downloaded) {
        /* 429/430/431 quota responses stop this serial worker. A later
         * library refresh retries without publishing or destroying cache. */
        return false;
    }

    json response;
    try {
        response = json::parse(response_data.begin(), response_data.end());
    } catch (const json::exception &) {
        return false;
    }
    const json *game = nullptr;
    try {
        game = &response.at("response").at("jeu");
    } catch (const json::exception &) {
        return false;
    }
    if (!game->is_object()) {
        return false;
    }

    Metadata result;
    result.requested_title_id = request.title_id;
    result.serial = serial;
    auto id = game->find("id");
    if (id != game->end()) {
        result.game_id = scalar_text(*id);
    }
    auto rom_id = game->find("romid");
    if (rom_id != game->end()) {
        result.rom_id = scalar_text(*rom_id);
    }
    auto names = game->find("noms");
    if (names != game->end()) {
        result.name = choose_localized_text(*names);
    }
    if (result.name.empty()) {
        result.name = request.rom_filename;
    }
    if (result.game_id.empty() || result.name.empty()) {
        return false;
    }
    result.details = parse_details(*game);

    auto media_value = game->find("medias");
    std::vector<MediaCandidate> candidates =
        media_value != game->end() ? parse_media(*media_value)
                                   : std::vector<MediaCandidate>{};

    /* Persist a useful representation of the response, but never its media
     * URLs or any echoed credential fields. */
    json sanitized = response;
    sanitize_json(&sanitized);
    std::string sanitized_text = sanitized.dump(2);
    if (!write_file_atomic(sanitized_response_path(cache_root, serial),
                           sanitized_text.data(), sanitized_text.size())) {
        return false;
    }

    json previous_media = load_previous_media_entries(
        cache_root, settings.media_root, request);
    json downloaded_media = json::array();
    bool complete = true;
    std::set<size_t> selected_indexes;
    for (const auto &types : requested_media_groups(settings)) {
        const MediaCandidate *candidate = best_candidate(candidates, types);
        /* Absence in the provider response is a complete result. */
        if (candidate == nullptr ||
            !selected_indexes.insert(candidate->response_index).second) {
            continue;
        }
        json entry;
        if (download_media(settings.media_root, request, *candidate, false,
                           &entry)) {
            downloaded_media.push_back(std::move(entry));
        } else {
            complete = false;
        }
    }

    json manifest_media = merge_media_entries(previous_media,
                                               downloaded_media);

    json manifest = {
        { "version", 1 },
        { "provider", "screenscraper" },
        { "system_id", kXboxSystemId },
        { "title_id", request.title_id },
        { "serial", serial },
        { "game_id", result.game_id },
        { "rom_id", result.rom_id },
        { "name", result.name },
        { "details",
          {
              { "version", kDetailsVersion },
              { "synopsis", result.details.synopsis },
              { "developer", result.details.developer },
              { "publisher", result.details.publisher },
              { "genre", result.details.genre },
              { "players", result.details.players },
              { "release_date", result.details.release_date },
              { "rating", result.details.rating },
          } },
        { "download_policy", policy_signature(settings) },
        { "download_complete", complete },
        { "media", manifest_media },
    };
    std::string manifest_text = manifest.dump(2);
    if (!write_file_atomic(manifest_path(cache_root, serial),
                           manifest_text.data(), manifest_text.size())) {
        return false;
    }
    rebuild_media_paths(settings.media_root, manifest_media, &result);
    *metadata = std::move(result);
    return true;
}

bool fetch(const std::string &cache_root, const Request &request,
           const SettingsSnapshot &settings, Metadata *metadata)
{
    try {
        return fetch_impl(cache_root, request, settings, metadata);
    } catch (const json::exception &) {
        return false;
    }
}

std::string choose_cover(const Metadata &metadata,
                         const SettingsSnapshot &settings)
{
    std::string path = first_media_path(
        metadata.media_paths, box_art_types(settings.box_art_style));
    return !path.empty() ? path
                         : first_media_path(metadata.media_paths, { "box-2D" });
}

std::string choose_large_art(const MediaPaths &paths,
                             const SettingsSnapshot &settings)
{
    std::string path =
        first_media_path(paths, box_art_types(settings.large_box_art_style));
    if (path.empty()) {
        path = first_media_path(paths, box_art_types(settings.box_art_style));
    }
    return !path.empty() ? path : first_media_path(paths, { "box-2D" });
}

std::string choose_background(const MediaPaths &paths,
                              const SettingsSnapshot &settings)
{
    if (settings.background_style == kBackgroundVideo) {
        return first_media_path(paths, { "fanart", "ss" });
    }
    return first_media_path(paths, background_types(settings.background_style));
}

std::string choose_video(const MediaPaths &paths,
                         const SettingsSnapshot &settings)
{
    if (settings.background_style != kBackgroundVideo) {
        return {};
    }
    return first_media_path(paths, { "video-normalized", "video" });
}

std::string choose_logo(const MediaPaths &paths)
{
    return first_media_path(paths, { "wheel-hd", "wheel" });
}

} // namespace xemu::screenscraper
