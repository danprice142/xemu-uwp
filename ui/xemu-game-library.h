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

#ifndef XEMU_GAME_LIBRARY_H
#define XEMU_GAME_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * All strings in snapshots are UTF-8 and owned by the snapshot.  Call
 * xemu_game_library_free_snapshot() when the caller is finished with it.
 */
typedef struct XemuGameLibraryEntry {
    char *token;
    char *label;
    char *path;
    /* Artwork chosen by the library settings; empty when not downloaded. */
    char *cover_path;
    char *large_art_path;
    /* Background still (also shown before a video background plays). */
    char *background_path;
    char *video_path;
    /* Clear logo. */
    char *logo_path;
    /* Provider metadata; empty strings when unknown. */
    char *synopsis;
    char *developer;
    char *publisher;
    char *genre;
    char *players;
    char *release_date;
    /* Community score out of 20. */
    char *rating;
    uint32_t title_id;
    bool has_metadata;
} XemuGameLibraryEntry;

/*
 * Set during machine setup when the game library opens before the guest
 * starts; the UI clears it once the library is shown.
 */
extern bool xemu_game_library_startup_pending;

typedef struct XemuGameLibraryState {
    bool running;
    uint32_t title_id;
    char *content_token;
    char *game_name;
    char *platform_name;
    char *core_name;
} XemuGameLibraryState;

/* Rescan the configured games directory and update the launch allowlist. */
void xemu_game_library_refresh(void);

/* Monotonically increases whenever the visible library data changes. */
uint64_t xemu_game_library_generation(void);

XemuGameLibraryEntry *xemu_game_library_snapshot(size_t *count);

/*
 * Return only the currently published in-memory snapshot.  This function
 * never waits for cache initialization or scans/stats the games directory.
 * If no scan has completed yet, it schedules one below-normal-priority scan
 * worker and returns an empty snapshot until a later request observes it.
 */
XemuGameLibraryEntry *xemu_game_library_snapshot_nonblocking(size_t *count);

/*
 * As xemu_game_library_snapshot_nonblocking(), but *available is false when
 * the library was busy and nothing was read, so a caller can keep what it
 * shows instead of treating the library as empty.
 */
XemuGameLibraryEntry *xemu_game_library_snapshot_try(size_t *count,
                                                     bool *available);

void xemu_game_library_free_snapshot(XemuGameLibraryEntry *entries,
                                     size_t count);

/*
 * Resolve only tokens in the current scanned allowlist.  The returned path is
 * allocated with g_malloc() and must be released with g_free().
 */
char *xemu_game_library_resolve_token(const char *token);

/*
 * Compact 64-bit identity of a library token (the leading digest digits),
 * used by remote frontends.  Returns 0 for anything that is not a token.
 */
uint64_t xemu_game_library_launch_id(const char *token);

/* xemu_game_library_resolve_token() keyed by launch id. */
char *xemu_game_library_resolve_launch_id(uint64_t launch_id);

/*
 * Remote catalogue: a JSON document listing every entry with its launch id,
 * metadata and available media types, in display order.  Returns NULL while
 * no scan has been published.  Release with g_free().
 */
char *xemu_game_library_catalogue_json(void);

/* Revision stamped on the catalogue and on remote selection updates. */
uint32_t xemu_game_library_catalogue_revision(void);

/*
 * File for one catalogue media type (0 marquee, 1 cover, 2 3D box, 3 mix
 * image, 4 back cover, 5 physical media, 6 screenshot, 7 fan art, 8 title
 * screen, 9 video), or NULL.  Release with g_free().
 */
char *xemu_game_library_catalogue_media(uint64_t launch_id,
                                        unsigned media_type);

/*
 * Associate a running XBE with its disc.  This makes filename-first entries
 * progressively gain XBE/ScreenScraper names and artwork without blocking
 * the UI.
 */
void xemu_game_library_note_running_title(const char *disc_path,
                                          uint32_t title_id,
                                          const char *xbe_name);

/*
 * Title ID the library has recorded for a disc image, or 0 when unknown.
 * Never scans; returns 0 until the library cache has loaded.
 */
uint32_t xemu_game_library_title_id_for_path(const char *path);

/* Poll the running XBE at a throttled rate.  Call only from the main/UI thread. */
void xemu_game_library_refresh_running_title(void);

bool xemu_game_library_get_current_state(XemuGameLibraryState *state);
void xemu_game_library_free_current_state(XemuGameLibraryState *state);

/* Apply ScreenScraper media/style changes without blocking the UI thread. */
void xemu_game_library_screenscraper_settings_changed(void);

typedef struct XemuGameLibraryDownloadStatus {
    bool running;
    /* Reading title IDs from unidentified disc images. */
    bool identifying;
    /* Images read, or titles downloaded, so far. */
    uint32_t done;
    uint32_t total;
} XemuGameLibraryDownloadStatus;

/* Runs on a worker thread when a library download finishes. */
typedef void (*XemuGameLibraryDownloadDone)(uint32_t total, uint32_t failed,
                                            void *opaque);

/*
 * Download metadata and media for every library title, reading the title ID
 * of unidentified disc images first.  With overwrite, cached metadata and
 * existing media are downloaded again.  Returns false while a download is
 * already running.  Never blocks on disc or network I/O.
 */
bool xemu_game_library_download_assets(bool overwrite,
                                       XemuGameLibraryDownloadDone done,
                                       void *opaque);

/* Lock-free; safe to poll every frame. */
void xemu_game_library_download_status(XemuGameLibraryDownloadStatus *status);

/* Returns the LocalState/preferences-backed ScreenScraper root via g_malloc. */
char *xemu_game_library_dup_cache_path(void);

/* Alias of xemu_game_library_dup_cache_path(). */
char *xemu_game_library_dup_xdb_cache_path(void);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_GAME_LIBRARY_H */
