/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The channel harness every message-type functional test drives: a server
    and a client message object over the real loopback transports, with loss,
    duplication, latency and jitter injected through the in-memory network
    simulator, a counting allocator installed on both ends, and a game clock
    the test advances (never a sleep).

    This card cannot be built. The harness wraps the message objects that
    rocketnet-message-objects-and-c-api delivers (the rocketnet_message_* C
    API in sdk/rocketnet.h and the client/server objects of CHANNELS.md layer
    8). That card is marked landed, but its code is not on BASE: sdk/rocketnet.h
    carries only the message config functions, and no server/client message
    object exists in the tree, so there is nothing for this harness to drive.
    The DONE-WHEN cases (harness_one_message_each_way_lossless,
    harness_one_message_each_way_lossy, harness_no_allocation_after_create)
    therefore cannot run. This header records the harness's designed surface so
    the type cards that build on it have a stable contract once the dependency
    lands.
*/

#pragma once

#ifndef ROCKETNET_CHANNEL_HARNESS_H
#define ROCKETNET_CHANNEL_HARNESS_H

// The harness's surface, as this card specifies it. Each entry names the C
// API the message objects must export for the harness to hold. Until
// rocketnet-message-objects-and-c-api is actually on BASE, none of these
// functions exist and nothing below compiles.
//
//   rocketnet_channel_harness_create      both ends with a counting allocator
//   rocketnet_channel_harness_set_link    loss, duplication, latency, jitter
//   rocketnet_channel_harness_set_budget  send budget in kbps per end
//   rocketnet_channel_harness_pump        bounded updates with a clock argument
//   rocketnet_channel_harness_send        bytes on a channel with a type code
//   rocketnet_channel_harness_delivered   (channel, bytes, hash) each message
//   rocketnet_channel_harness_counters    resends, wire bytes per direction
//   rocketnet_channel_harness_assert_no_allocation_after_create

#endif // ROCKETNET_CHANNEL_HARNESS_H
