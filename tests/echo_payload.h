/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    What the test server's raw payload echo (tests/client_server/server.c)
    may send back. The echo returns a client's packet payload unchanged, as
    if the server had sent it. That is sound for a payload the server could
    have sent byte for byte: raw bytes that are not message records, the
    unreliable records (codes 0 and 1, one record each, no block) and the
    layer's control records. It is not sound for a numbered application
    message: an unreliable message over 1,411 bytes is promoted to its
    reliable twin, and an echo of it hands the client its own send numbers as
    the server's, which the client refuses as a protocol violation once they
    run past its receive window (docs/XDP-PACKET-BLASTER.md). Such a payload
    is refused by name instead of echoed; the echo measures the transport
    echo of unreliable records, nothing more.

    No allocation, no syscall, no global state.
*/

#pragma once

#ifndef ROCKETNET_TESTS_ECHO_PAYLOAD_H
#define ROCKETNET_TESTS_ECHO_PAYLOAD_H

#include "rocketnet_message.h"

#include <stdint.h>

// NULL when the payload may be echoed; otherwise what makes it a payload
// only its sender's peer could answer, for the server to print.

static inline const char * rocketnet_echo_payload_refusal( const uint8_t * payload, int bytes )
{
    struct rocketnet_message_limits_t limits;
    limits.num_channels = 65536;
    limits.enabled_codes = rocketnet_message_enabled_codes( 0xFFu );
    limits.reliable_messages_in_flight = 65536;
    limits.max_message_bytes = 64 * 1024 * 1024;
    limits.receive_message_buffer_bytes = 64 * 1024 * 1024;

    struct rocketnet_message_reader_t reader;
    if ( rocketnet_message_reader_begin( &reader, payload, bytes, &limits ) != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        return NULL;    // not message records: a raw transport payload, echoed as it came
    }

    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        if ( record.block )
        {
            return "a fragment of a numbered block (a message over one record, promoted)";
        }
        if ( record.control )
        {
            continue;
        }
        if ( record.code != ROCKETNET_MESSAGE_CODE_UNRELIABLE && record.code != ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED )
        {
            return "a numbered application message (a reliable code, or an unreliable message over 1411 bytes promoted)";
        }
    }
    return NULL;
}

#endif // #ifndef ROCKETNET_TESTS_ECHO_PAYLOAD_H
