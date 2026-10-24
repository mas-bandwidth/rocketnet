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
// clients). ROCKETNET_MAX_CLIENTS lives in rocketnet.h, its one home, which
// compiles for the BPF target too, so the BPF program and the library read the
// same default. A build at another value passes it to both (cmake/EmbedXdp.cmake
// MAX_CLIENTS), and the generated rocketnet_server_xdp.h refuses a library
// whose value differs from the one the embedded program was compiled with.
#include "rocketnet.h"
#define ROCKETNET_XDP_REPLAY_MAX_ENTRIES ( ( ROCKETNET_MAX_CLIENTS + 1023 ) / 1024 * 1024 )
#if ROCKETNET_XDP_REPLAY_MAX_ENTRIES < ROCKETNET_MAX_CLIENTS
#error the XDP replay map must hold an entry for every client
#endif

