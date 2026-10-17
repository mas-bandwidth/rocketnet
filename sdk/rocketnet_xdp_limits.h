/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once
// Shared by the native driver and its embedded BPF map definition.
#define ROCKETNET_XDP_MAX_QUEUES 1024

// The kernel replay window, in sequences behind the map's high-water mark:
// a session packet more than this far behind is dropped in the kernel.
#define ROCKETNET_XDP_REPLAY_WINDOW 16

// The replay map's entry budget, one per live source address and port.
#define ROCKETNET_XDP_REPLAY_MAX_ENTRIES 1024
