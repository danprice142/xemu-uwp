/*
 * Copyright (c) 2026 Skitzelles - https://github.com/skitzellez
 * MIT License: free to use in any project; keep this notice.
 *
 * Games, BIOS and hard disk images on any USB drive (FAT32, exFAT, NTFS
 * without the PC permissions step). See uwp-file-redirect.c.
 */

#ifndef XEMU_UWP_FILE_REDIRECT_H
#define XEMU_UWP_FILE_REDIRECT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, first thing in main. Returns the number of imports redirected. */
unsigned xemu_uwp_redirect_file_apis(void);

#ifdef __cplusplus
}
#endif

#endif
