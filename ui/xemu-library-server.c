/*
 * xemu remote library server
 *
 * Read-only HTTP service publishing the game catalogue and its artwork to
 * network controller frontends:
 *
 *   GET /catalogue                          library as JSON
 *   GET /media/<type>?revision=<n>&token=<id>  one media file
 *
 * Only peers accepted by the caller's filter are served, one request per
 * connection, in turn, on a single worker thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "ui/xemu-game-library.h"
#include "ui/xemu-library-server.h"

#include <glib/gstdio.h>

#define LIBRARY_REQUEST_LIMIT 4096
#define LIBRARY_FILE_CHUNK (64 * 1024)
#define LIBRARY_IO_TIMEOUT_MS 15000
#define LIBRARY_ACCEPT_IDLE_US (20 * 1000)
#define LIBRARY_MEDIA_TYPES 10

static QemuThread s_thread;
static int s_listen_fd = -1;
static gint s_stop;
static bool s_running;
static XemuLibraryPeerFilter s_filter;

static bool send_all(int fd, const void *data, size_t size)
{
    const uint8_t *bytes = data;

    while (size > 0) {
        ssize_t sent = send(fd, bytes, MIN(size, (size_t)INT_MAX), 0);

        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        bytes += sent;
        size -= sent;
    }
    return true;
}

static bool send_header(int fd, const char *status, const char *content_type,
                        uint64_t length)
{
    char *header = g_strdup_printf("HTTP/1.1 %s\r\n"
                                   "Content-Type: %s\r\n"
                                   "Content-Length: %" PRIu64 "\r\n"
                                   "Access-Control-Allow-Origin: *\r\n"
                                   "Cache-Control: no-cache\r\n"
                                   "Connection: close\r\n\r\n",
                                   status, content_type, length);
    bool ok = send_all(fd, header, strlen(header));

    g_free(header);
    return ok;
}

static void send_text(int fd, const char *status, const char *content_type,
                      const char *body)
{
    size_t length = strlen(body);

    if (send_header(fd, status, content_type, length) && length != 0) {
        send_all(fd, body, length);
    }
}

static void send_not_found(int fd)
{
    send_text(fd, "404 Not Found", "text/plain", "404 Not Found");
}

static const char *content_type_for(const char *path)
{
    static const struct {
        const char *extension;
        const char *type;
    } types[] = {
        { ".png", "image/png" },   { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" }, { ".webp", "image/webp" },
        { ".mp4", "video/mp4" },   { ".mkv", "video/x-matroska" },
        { ".webm", "video/webm" },
    };
    const char *dot = strrchr(path, '.');

    if (dot != NULL) {
        for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
            if (g_ascii_strcasecmp(dot, types[i].extension) == 0) {
                return types[i].type;
            }
        }
    }
    return "application/octet-stream";
}

static void send_file(int fd, const char *path)
{
    FILE *file = g_fopen(path, "rb");
    uint8_t *chunk;
    off_t length;

    if (file == NULL) {
        send_not_found(fd);
        return;
    }
    if (fseeko(file, 0, SEEK_END) != 0 || (length = ftello(file)) < 0 ||
        fseeko(file, 0, SEEK_SET) != 0) {
        fclose(file);
        send_not_found(fd);
        return;
    }
    if (!send_header(fd, "200 OK", content_type_for(path), length)) {
        fclose(file);
        return;
    }

    chunk = g_malloc(LIBRARY_FILE_CHUNK);
    while (!g_atomic_int_get(&s_stop)) {
        size_t count = fread(chunk, 1, LIBRARY_FILE_CHUNK, file);

        if (count == 0 || !send_all(fd, chunk, count)) {
            break;
        }
    }
    g_free(chunk);
    fclose(file);
}

/* URL-decoded value of key in an HTTP query string, or NULL. */
static char *query_value(const char *query, const char *key)
{
    size_t key_length = strlen(key);

    while (query != NULL && *query != 0) {
        const char *end = strchr(query, '&');
        size_t length = end != NULL ? (size_t)(end - query) : strlen(query);

        if (length > key_length && query[key_length] == '=' &&
            strncmp(query, key, key_length) == 0) {
            char *raw = g_strndup(query + key_length + 1,
                                  length - key_length - 1);
            char *value = g_uri_unescape_string(raw, NULL);

            g_free(raw);
            return value;
        }
        query = end != NULL ? end + 1 : NULL;
    }
    return NULL;
}

static void send_media(int fd, const char *type_text, const char *query)
{
    char *end = NULL;
    guint64 type = g_ascii_strtoull(type_text, &end, 10);
    char *token = query_value(query, "token");
    guint64 launch_id = 0;
    char *path = NULL;

    /* Tokens are stable across revisions, so the revision is not checked. */
    if (end != type_text && *end == 0 && type < LIBRARY_MEDIA_TYPES &&
        token != NULL && strlen(token) == 16) {
        launch_id = g_ascii_strtoull(token, &end, 16);
        if (*end == 0) {
            path = xemu_game_library_catalogue_media(launch_id,
                                                     (unsigned)type);
        }
    }
    if (path != NULL) {
        send_file(fd, path);
    } else {
        send_not_found(fd);
    }
    g_free(path);
    g_free(token);
}

static void handle_client(int fd)
{
    char request[LIBRARY_REQUEST_LIMIT + 1];
    size_t received = 0;
    char *line_end;
    char *target;
    char *space;
    char *query;

    while (received < LIBRARY_REQUEST_LIMIT) {
        ssize_t count = recv(fd, request + received,
                             LIBRARY_REQUEST_LIMIT - received, 0);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        received += count;
        request[received] = 0;
        if (strstr(request, "\r\n\r\n") != NULL) {
            break;
        }
    }
    if (received == 0) {
        return;
    }
    request[received] = 0;

    line_end = strstr(request, "\r\n");
    if (line_end == NULL) {
        send_text(fd, "400 Bad Request", "text/plain", "400 Bad Request");
        return;
    }
    *line_end = 0;
    if (strncmp(request, "GET ", 4) != 0) {
        send_text(fd, "405 Method Not Allowed", "text/plain",
                  "405 Method Not Allowed");
        return;
    }
    target = request + 4;
    space = strchr(target, ' ');
    if (space == NULL) {
        send_text(fd, "400 Bad Request", "text/plain", "400 Bad Request");
        return;
    }
    *space = 0;
    query = strchr(target, '?');
    if (query != NULL) {
        *query++ = 0;
    }

    if (strcmp(target, "/catalogue") == 0) {
        char *body = xemu_game_library_catalogue_json();

        if (body != NULL) {
            send_text(fd, "200 OK", "application/json", body);
        } else {
            send_text(fd, "503 Service Unavailable", "text/plain",
                      "503 Service Unavailable");
        }
        g_free(body);
    } else if (g_str_has_prefix(target, "/media/")) {
        send_media(fd, target + strlen("/media/"), query);
    } else {
        send_not_found(fd);
    }
}

static void set_io_timeouts(int fd)
{
#ifdef _WIN32
    DWORD timeout = LIBRARY_IO_TIMEOUT_MS;
#else
    struct timeval timeout = {
        .tv_sec = LIBRARY_IO_TIMEOUT_MS / 1000,
        .tv_usec = (LIBRARY_IO_TIMEOUT_MS % 1000) * 1000,
    };
#endif

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)&timeout,
               sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const void *)&timeout,
               sizeof(timeout));
}

static void *library_server_thread(void *opaque)
{
    while (!g_atomic_int_get(&s_stop)) {
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        int fd = qemu_accept(s_listen_fd, (struct sockaddr *)&peer,
                             &peer_size);

        if (fd < 0) {
            g_usleep(LIBRARY_ACCEPT_IDLE_US);
            continue;
        }
        if (peer.sin_family != AF_INET || s_filter == NULL ||
            !s_filter(peer.sin_addr.s_addr)) {
            close(fd);
            continue;
        }
        /* Accepted sockets inherit the listener's non-blocking mode. */
        if (qemu_set_blocking(fd, true, NULL)) {
            set_io_timeouts(fd);
            handle_client(fd);
        }
        close(fd);
    }
    return NULL;
}

bool xemu_library_server_start(uint16_t port, XemuLibraryPeerFilter filter)
{
    struct sockaddr_in address = { 0 };
    int fd;

    if (s_running) {
        return true;
    }
    s_filter = filter;
    fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    socket_set_fast_reuse(fd);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 8) < 0 || !qemu_set_blocking(fd, false, NULL)) {
        fprintf(stderr, "Library server: cannot listen on port %u\n", port);
        close(fd);
        return false;
    }

    s_listen_fd = fd;
    g_atomic_int_set(&s_stop, 0);
    qemu_thread_create(&s_thread, "library-server", library_server_thread,
                       NULL, QEMU_THREAD_JOINABLE);
    s_running = true;
    fprintf(stderr, "Library server listening on port %u\n", port);
    return true;
}

void xemu_library_server_stop(void)
{
    if (!s_running) {
        return;
    }
    g_atomic_int_set(&s_stop, 1);
    qemu_thread_join(&s_thread);
    close(s_listen_fd);
    s_listen_fd = -1;
    s_running = false;
}

bool xemu_library_server_is_running(void)
{
    return s_running;
}
