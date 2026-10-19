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

#include "xemu-launch-uri.h"

#include <ctype.h>
#include <glib.h>
#include <string.h>

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static char *uri_decode_component(const char *src, size_t len)
{
    GString *out = g_string_sized_new(len);

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);

            if (hi >= 0 && lo >= 0) {
                g_string_append_c(out, (char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        g_string_append_c(out, src[i] == '+' ? ' ' : src[i]);
    }

    return g_string_free(out, false);
}

static bool str_eq_ci(const char *a, const char *b)
{
    return g_ascii_strcasecmp(a, b) == 0;
}

static bool str_has_suffix_ci(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);

    if (suffix_len > str_len) {
        return false;
    }

    return g_ascii_strcasecmp(str + str_len - suffix_len, suffix) == 0;
}

static bool token_is_exe(const char *token)
{
    return str_has_suffix_ci(token, ".exe") || str_eq_ci(token, "xemu");
}

static bool token_is_dvd_path_switch(const char *token)
{
    return str_eq_ci(token, "-dvd_path") ||
           str_eq_ci(token, "--dvd_path") ||
           str_eq_ci(token, "/dvd_path");
}

static bool token_looks_like_disc_path(const char *token)
{
    if (token[0] == '\0') {
        return false;
    }

    if (strstr(token, ":\\") != NULL || strstr(token, ":/") != NULL ||
        g_str_has_prefix(token, "\\\\") || g_str_has_prefix(token, "/") ||
        strchr(token, '\\') != NULL || strchr(token, '/') != NULL) {
        return true;
    }

    return str_has_suffix_ci(token, ".iso") ||
           str_has_suffix_ci(token, ".xiso") ||
           str_has_suffix_ci(token, ".xiso.iso") ||
           str_has_suffix_ci(token, ".cci") ||
           str_has_suffix_ci(token, ".img") ||
           str_has_suffix_ci(token, ".bin");
}

static char *next_cmd_token(const char **cursor)
{
    const char *p = *cursor;
    GString *token;

    while (*p != '\0' && g_ascii_isspace(*p)) {
        p++;
    }

    if (*p == '\0') {
        *cursor = p;
        return NULL;
    }

    token = g_string_new(NULL);
    if (*p == '"') {
        p++;
        while (*p != '\0') {
            if (*p == '"') {
                p++;
                break;
            }
            g_string_append_c(token, *p++);
        }
    } else {
        while (*p != '\0' && !g_ascii_isspace(*p)) {
            g_string_append_c(token, *p++);
        }
    }

    *cursor = p;
    return g_string_free(token, false);
}

static char *cmd_to_dvd_path(const char *cmd)
{
    GPtrArray *tokens = g_ptr_array_new_with_free_func(g_free);
    const char *cursor = cmd;
    char *token;
    char *path = NULL;
    guint start = 0;

    while ((token = next_cmd_token(&cursor)) != NULL) {
        if (token[0] != '\0') {
            g_ptr_array_add(tokens, token);
        } else {
            g_free(token);
        }
    }

    if (tokens->len == 0) {
        goto done;
    }

    if (token_is_exe(g_ptr_array_index(tokens, 0))) {
        start = 1;
    }

    for (guint i = start; i + 1 < tokens->len; i++) {
        if (token_is_dvd_path_switch(g_ptr_array_index(tokens, i))) {
            path = g_strdup(g_ptr_array_index(tokens, i + 1));
            goto done;
        }
    }

    for (guint i = start; i < tokens->len; i++) {
        const char *candidate = g_ptr_array_index(tokens, i);

        if (token_looks_like_disc_path(candidate)) {
            path = g_strdup(candidate);
            goto done;
        }
    }

    for (guint i = tokens->len; i > start; i--) {
        const char *candidate = g_ptr_array_index(tokens, i - 1);

        if (candidate[0] != '-') {
            path = g_strdup(candidate);
            goto done;
        }
    }

done:
    g_ptr_array_free(tokens, true);
    return path;
}

static bool uri_has_query_scheme(const char *uri, const char **query)
{
    const char *qmark = strchr(uri, '?');
    const char *colon;
    char *scheme;
    bool supported_scheme;

    if (qmark == NULL) {
        return false;
    }

    colon = memchr(uri, ':', qmark - uri);
    if (colon == NULL || colon == uri) {
        return false;
    }

    scheme = g_strndup(uri, colon - uri);
    supported_scheme = str_eq_ci(scheme, "xemu");
    g_free(scheme);
    if (!supported_scheme) {
        return false;
    }

    *query = qmark + 1;
    return true;
}

static void replace_string(char **dst, char *value)
{
    g_free(*dst);
    *dst = value;
}

bool xemu_launch_uri_parse(const char *uri, XemuLaunchUri *out)
{
    const char *query;
    char *cmd = NULL;
    char *explicit_path = NULL;
    bool found = false;

    memset(out, 0, sizeof(*out));

    if (uri == NULL || !uri_has_query_scheme(uri, &query)) {
        return false;
    }

    while (*query != '\0' && *query != '#') {
        const char *part_end = query;
        const char *eq;

        while (*part_end != '\0' && *part_end != '&' && *part_end != '#') {
            part_end++;
        }

        eq = memchr(query, '=', part_end - query);
        if (eq != NULL && eq > query) {
            char *key = uri_decode_component(query, eq - query);
            const char *value_end = part_end;
            char *value;

            if (str_eq_ci(key, "launchOnExit") ||
                str_eq_ci(key, "launch_on_exit")) {
                while (*value_end != '\0' && *value_end != '#') {
                    value_end++;
                }
            }

            value = uri_decode_component(eq + 1, value_end - eq - 1);

            if (str_eq_ci(key, "cmd")) {
                replace_string(&cmd, value);
                value = NULL;
            } else if (str_eq_ci(key, "dvd_path") ||
                       str_eq_ci(key, "path") ||
                       str_eq_ci(key, "game")) {
                replace_string(&explicit_path, value);
                value = NULL;
            } else if (str_eq_ci(key, "launchOnExit") ||
                       str_eq_ci(key, "launch_on_exit")) {
                replace_string(&out->launch_on_exit, value);
                value = NULL;
            }

            g_free(value);
            g_free(key);

            if (value_end != part_end) {
                query = value_end;
                continue;
            }
        }

        query = *part_end == '&' ? part_end + 1 : part_end;
    }

    if (explicit_path != NULL && explicit_path[0] != '\0') {
        out->dvd_path = g_steal_pointer(&explicit_path);
    } else if (cmd != NULL) {
        out->dvd_path = cmd_to_dvd_path(cmd);
    }

    found = (out->dvd_path != NULL && out->dvd_path[0] != '\0') ||
            (out->launch_on_exit != NULL && out->launch_on_exit[0] != '\0');

    g_free(explicit_path);
    g_free(cmd);

    if (!found) {
        xemu_launch_uri_clear(out);
    }

    return found;
}

void xemu_launch_uri_clear(XemuLaunchUri *launch)
{
    g_free(launch->dvd_path);
    g_free(launch->launch_on_exit);
    memset(launch, 0, sizeof(*launch));
}
