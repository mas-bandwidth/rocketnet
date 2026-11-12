/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The rule tests/echo_payload.h states, in C++ over the message layer's
    reader, with C linkage so the C test server can call it.
*/

#include "echo_payload.h"

#include "rocketnet_message.h"

#include <stddef.h>

extern "C" const char * rocketnet_echo_payload_refusal( const uint8_t * payload, int bytes )
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
