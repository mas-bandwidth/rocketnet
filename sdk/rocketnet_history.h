/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

// The published depth bounds (STANDARD.md _Replay window sizing_): the message
// tier's half-range reconstruction relies on a depth no larger than 32768 and
// transport sequences that rise in packet write order (STANDARD.md _Numbers_).
#define ROCKETNET_HISTORY_PACKETS_MIN 4096
#define ROCKETNET_HISTORY_PACKETS_MAX 32768

// Build-time memory budget per connection, in session sequence numbers.
// No clock, frame frequency, send rate or ACK frequency enters this value.
// Both library targets and internal-header consumers must use the same value.
#ifndef ROCKETNET_HISTORY_PACKETS
#define ROCKETNET_HISTORY_PACKETS 4096
#endif

// The upper bound keeps a contiguous retained run representable in one u16
// ACK range. It also bounds per-connection processing and up-front allocations.
#if ROCKETNET_HISTORY_PACKETS < ROCKETNET_HISTORY_PACKETS_MIN || ROCKETNET_HISTORY_PACKETS > ROCKETNET_HISTORY_PACKETS_MAX || ROCKETNET_HISTORY_PACKETS % 64 != 0
#error ROCKETNET_HISTORY_PACKETS must be a multiple of 64 between ROCKETNET_HISTORY_PACKETS_MIN and ROCKETNET_HISTORY_PACKETS_MAX
#endif
