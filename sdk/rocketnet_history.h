/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

// Build-time memory budget per connection, in session sequence numbers.
// No clock, frame frequency, send rate or ACK frequency enters this value.
// Both library targets and internal-header consumers must use the same value.
#ifndef ROCKETNET_HISTORY_PACKETS
#define ROCKETNET_HISTORY_PACKETS 4096
#endif

// The upper bound keeps a contiguous retained run representable in one u16
// ACK range. It also bounds per-peer processing and up-front allocations.
#if ROCKETNET_HISTORY_PACKETS < 4096 || ROCKETNET_HISTORY_PACKETS > 32768 || ROCKETNET_HISTORY_PACKETS % 64 != 0
#error ROCKETNET_HISTORY_PACKETS must be a multiple of 64 between 4096 and 32768
#endif
