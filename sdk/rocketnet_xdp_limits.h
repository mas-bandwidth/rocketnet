/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once
// Shared by the native transport and its embedded BPF map definition.
#include "rocketnet_history.h"

#define ROCKETNET_XDP_MAX_QUEUES 1024

// The kernel replay window, in sequences behind the map's high-water mark:
// a session packet this far or further behind is dropped in the kernel. It
// is the userspace replay window (ROCKETNET_REPLAY_WINDOW_BITS, the same
// ROCKETNET_HISTORY_PACKETS), so the kernel never drops a reordered packet
// that every other transport accepts. The check is one compare against the
// map's high-water, so the window's size costs no map memory and no
// verifier work. The embedded program is compiled with the library's
// ROCKETNET_HISTORY_PACKETS (cmake/EmbedXdp.cmake).
#define ROCKETNET_XDP_REPLAY_WINDOW ROCKETNET_HISTORY_PACKETS

// The replay map's entry budget, one per live source address and port: every
// client the server holds, rounded up to a multiple of 1024 (1024 at 1000
// clients). This header is also compiled into the BPF program, which cannot
// include rocketnet.h, so the BPF compile is handed ROCKETNET_MAX_CLIENTS on
// its command line (cmake/EmbedXdp.cmake, server_xdp/Makefile).
#ifndef ROCKETNET_MAX_CLIENTS
#error ROCKETNET_MAX_CLIENTS is not defined: include rocketnet.h first, or pass -DROCKETNET_MAX_CLIENTS as the library build does
#endif
#define ROCKETNET_XDP_REPLAY_MAX_ENTRIES ( ( ROCKETNET_MAX_CLIENTS + 1023 ) / 1024 * 1024 )
#if ROCKETNET_XDP_REPLAY_MAX_ENTRIES < ROCKETNET_MAX_CLIENTS
#error the XDP replay map must hold an entry for every client
#endif

