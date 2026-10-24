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

// The replay map's entry budget, one per live source address and port.
#define ROCKETNET_XDP_REPLAY_MAX_ENTRIES 1024
