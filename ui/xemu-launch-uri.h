/*
 * xemu Windows URI launch helpers
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_LAUNCH_URI_H
#define XEMU_LAUNCH_URI_H

#include <stdbool.h>

typedef struct XemuLaunchUri {
    char *dvd_path;
    char *launch_on_exit;
} XemuLaunchUri;

bool xemu_launch_uri_parse(const char *uri, XemuLaunchUri *out);
void xemu_launch_uri_clear(XemuLaunchUri *launch);

#endif
