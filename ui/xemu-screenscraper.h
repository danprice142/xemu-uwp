/*
 * ScreenScraper provider/cache support for the xemu game library.
 */

#ifndef XEMU_SCREENSCRAPER_H
#define XEMU_SCREENSCRAPER_H

#include <stdint.h>

#include <string>
#include <unordered_map>

namespace xemu::screenscraper {

struct Request {
    uint32_t title_id = 0;
    std::string rom_filename;
    uint64_t rom_size = 0;
    /* Download every selected media file again, even when a valid copy
     * already exists. */
    bool overwrite = false;
};

/* Descriptive fields from the provider, as UTF-8 text. */
struct Details {
    std::string synopsis;
    std::string developer;
    std::string publisher;
    std::string genre;
    std::string players;
    std::string release_date;
    /* Community score out of 20. */
    std::string rating;

    bool empty() const
    {
        return synopsis.empty() && developer.empty() && publisher.empty() &&
               genre.empty() && players.empty() && release_date.empty() &&
               rating.empty();
    }
};

/* Indexes of general.screenscraper.box_art_style / large_box_art_style. */
enum BoxArtStyle {
    kBoxArt2D,
    kBoxArt3D,
    kBoxArtMixV1,
    kBoxArtMixV2,
    kBoxArtFanArt,
    kBoxArtScreenshot,
    kBoxArtLogo,
    kBoxArtBack,
    kBoxArtDisc,
};

/* Indexes of general.screenscraper.background. */
enum BackgroundStyle {
    kBackgroundOff,
    kBackgroundFanArt,
    kBackgroundScreenshot,
    kBackgroundVideo,
    kBackgroundMixV1,
    kBackgroundMixV2,
};

using MediaPaths = std::unordered_map<std::string, std::string>;

struct Metadata {
    uint32_t requested_title_id = 0;
    std::string serial;
    std::string game_id;
    std::string rom_id;
    std::string name;
    /* Absolute local paths, keyed by the ScreenScraper media type. */
    MediaPaths media_paths;
    Details details;
};

struct MediaSettings {
    bool box_front = true;
    bool box_3d = false;
    bool box_back = false;
    bool wheels = true;
    bool physical_media = false;
    bool mix_v1 = false;
    bool mix_v2 = false;
    bool fan_art = true;
    bool screenshots = true;
    bool videos = false;
};

/*
 * Workers consume immutable copies of the UI-owned settings.  The revision is
 * used to retry a title if the user changes policy while it is being fetched.
 */
struct SettingsSnapshot {
    uint64_t revision = 0;
    std::string username;
    int box_art_style = kBoxArt2D;
    int large_box_art_style = kBoxArt2D;
    int background_style = kBackgroundFanArt;
    MediaSettings media;
    /* Absolute root of the downloaded_media tree. */
    std::string media_root;
};

/* Default downloaded_media root used when general.screenscraper.media_dir is
 * unset (XEMU_UWP_DEFAULT_MEDIA_DIR on Xbox). */
std::string default_media_root();

/* Call on the settings/UI thread after loading or changing g_config. */
void sync_settings_from_config();

/* Thread-safe copy of the last settings published by the UI thread. */
SettingsSnapshot settings_snapshot();

std::string serial_for_title(uint32_t title_id);

/*
 * Loads the sanitized normalized cache.  A usable stale cache is returned as
 * metadata with fresh=false.  No network or credential access occurs here.
 */
bool load_cached(const std::string &cache_root, const Request &request,
                 const SettingsSnapshot &settings, Metadata *metadata,
                 bool *fresh);

/* Fetches metadata and selected media sequentially. UWP credentials are read
 * only for this call and are never written to disk or diagnostics. */
bool fetch(const std::string &cache_root, const Request &request,
           const SettingsSnapshot &settings, Metadata *metadata);

/* Selects the configured UI artwork, with box-2D as the final fallback. */
std::string choose_cover(const Metadata &metadata,
                         const SettingsSnapshot &settings);

/* Large box art, falling back to the list artwork and then box-2D. */
std::string choose_large_art(const MediaPaths &paths,
                             const SettingsSnapshot &settings);

/* Background still: the configured image, or for video the image shown
 * until the video plays. Empty when backgrounds are off. */
std::string choose_background(const MediaPaths &paths,
                              const SettingsSnapshot &settings);

/* Video background, empty unless backgrounds use video. */
std::string choose_video(const MediaPaths &paths,
                         const SettingsSnapshot &settings);

/* Clear logo, HD first. */
std::string choose_logo(const MediaPaths &paths);

} // namespace xemu::screenscraper

#endif /* XEMU_SCREENSCRAPER_H */
