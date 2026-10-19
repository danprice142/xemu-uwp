/*
 * xemu remote library server
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef XEMU_LIBRARY_SERVER_H
#define XEMU_LIBRARY_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Network frontends read the catalogue one port above the DSU port. */
#define XEMU_LIBRARY_SERVER_PORT 26761

/* Decides whether a peer (IPv4, network byte order) may be served. Called on
 * the server thread. */
typedef bool (*XemuLibraryPeerFilter)(uint32_t address);

/* Listens synchronously, then serves allowed peers on a worker thread. */
bool xemu_library_server_start(uint16_t port, XemuLibraryPeerFilter filter);
void xemu_library_server_stop(void);
bool xemu_library_server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
