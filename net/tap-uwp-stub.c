/*
 * QEMU System Emulator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "net/net.h"
#include "clients.h"

int net_init_tap(const Netdev *netdev, const char *name,
                 NetClientState *peer, Error **errp)
{
    (void)netdev;
    (void)name;
    (void)peer;

    error_setg(errp, "tap network backend is not supported in a UWP app "
               "container; use user/slirp, socket, stream, or dgram");
    return -1;
}
