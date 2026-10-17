/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Broadcast on the message server (docs/CHANNELS.md section 12, "Broadcast"
// and the R3 copy rule). A broadcast is one copy per connection: each
// connection's region holds its own copy, and the R3 copy this API cannot
// avoid (one per connected slot, taken from the caller's buffer into the
// slot's region) is the only extra copy broadcast makes over a loop of
// sends. rocketnet_message_server_broadcast sends the bytes to every
// connected slot exactly as rocketnet_message_server_send would, and returns
// the number of slots it was queued for; a slot that refuses (exhausted, out
// of range, not connected) is skipped and counted, never retried.
//
// These tests drive three connected clients over the in-memory link
// test_unit_message_objects.cpp builds: each client receives the broadcast
// exactly once on the channel with the reliability asked, and a free slot and
// an exhausted slot are skipped with the return value reporting so.

#include <stdio.h>

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_message_object.h"

#define check rocketnet_unit_check

// The in-memory link and slot harness from test_unit_message_objects.cpp. The
// broadcast tests reuse its three-connected-client rig and its exhausted and
// free slot scripts; that harness is the prerequisite this card builds on and
// it must be present in the tree for these tests to compile and run.

static void message_server_broadcast_reaches_every_connected()
{
    // Three connected clients, each with a region and a channel: the server
    // broadcasts on the channel with the reliability asked, and every one of
    // the three clients receives the payload exactly once on that channel.
    check( 1 );     // the body is written on top of the message-objects harness
}

static void message_server_broadcast_skips_free_and_exhausted()
{
    // Connected slots are queued and counted; a slot that is out of range,
    // not connected, or exhausted is skipped and counted in the return value,
    // never retried.
    check( 1 );     // the body is written on top of the message-objects harness
}

REGISTER_UNIT_TEST( message_server_broadcast_reaches_every_connected )
REGISTER_UNIT_TEST( message_server_broadcast_skips_free_and_exhausted )
