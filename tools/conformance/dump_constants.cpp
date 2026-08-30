/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The implementation's side of the STANDARD.md conformance check (#111,
// serialize's tools/conformance pattern). This program prints the wire
// constants THE COMPILER SEES -- expanded macros, not header text -- and
// check_standard.py compares them against the numbers STANDARD.md
// publishes. Neither side reads the other: the document is parsed as a
// document, and these values arrive through the same preprocessor every
// packet-handling translation unit uses.
//
// A failure means the specification and the implementation disagree.
// Decide which one is wrong and fix that one. A specification nothing
// verifies drifts, and a drifted specification is worse than none,
// because independent implementations are built on it.

#include "rocketnet.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_fragment.h"

#include <stdio.h>

static void print_int( const char * name, long long value )
{
    printf( "%s %lld\n", name, value );
}

int main()
{
    const uint8_t test_filter_value[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

    printf( "ROCKETNET_VERSION 0x%08X\n", (unsigned int) ROCKETNET_VERSION );

    printf( "ROCKETNET_TEST_FILTER_VALUE" );
    for ( int i = 0; i < ROCKETNET_FILTER_VALUE_BYTES; i++ )
        printf( " %02x", test_filter_value[i] );
    printf( "\n" );

    print_int( "ROCKETNET_PREFILTER_BYTES", ROCKETNET_PREFILTER_BYTES );
    print_int( "ROCKETNET_FILTER_VALUE_BYTES", ROCKETNET_FILTER_VALUE_BYTES );
    print_int( "ROCKETNET_FILTER_EPOCH_UNKEYED", ROCKETNET_FILTER_EPOCH_UNKEYED );

    print_int( "ROCKETNET_AD_BYTES", ROCKETNET_AD_BYTES );
    print_int( "ROCKETNET_AD_WITH_EPHEMERAL_BYTES", ROCKETNET_AD_WITH_EPHEMERAL_BYTES );

    print_int( "ROCKETNET_PACKET_OVERHEAD_BYTES", ROCKETNET_PACKET_OVERHEAD_BYTES );
    print_int( "ROCKETNET_SEQUENCE_BYTES", ROCKETNET_SEQUENCE_BYTES );
    print_int( "ROCKETNET_PACKET_TAG_BYTES", ROCKETNET_PACKET_TAG_BYTES );

    print_int( "ROCKETNET_ACK_BLOCK_MIN_BYTES", ROCKETNET_ACK_BLOCK_MIN_BYTES );
    print_int( "ROCKETNET_ACK_BLOCK_MAX_RANGES", ROCKETNET_ACK_BLOCK_MAX_RANGES );

    print_int( "ROCKETNET_FRAGMENT_HEADER_BYTES", ROCKETNET_FRAGMENT_HEADER_BYTES );
    print_int( "ROCKETNET_MAX_FRAGMENTS", ROCKETNET_MAX_FRAGMENTS );

    print_int( "ROCKETNET_SEND_BUDGET", ROCKETNET_SEND_BUDGET );
    print_int( "ROCKETNET_RECEIVE_CEILING", ROCKETNET_RECEIVE_CEILING );

    print_int( "ROCKETNET_CONNECT_TOKEN_BYTES", ROCKETNET_CONNECT_TOKEN_BYTES );
    print_int( "ROCKETNET_SIGNED_CONNECT_DATA_BYTES", ROCKETNET_SIGNED_CONNECT_DATA_BYTES );
    print_int( "ROCKETNET_SEALED_BLOCK_BYTES", ROCKETNET_SEALED_BLOCK_BYTES );
    print_int( "ROCKETNET_PRIVATE_BLOCK_BYTES", ROCKETNET_PRIVATE_BLOCK_BYTES );
    print_int( "ROCKETNET_KEY_BYTES", ROCKETNET_KEY_BYTES );

    print_int( "ROCKETNET_CLIENT_STATE_MIN", ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
    print_int( "ROCKETNET_CLIENT_STATE_MAX", ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // the packet types, and the wire size each one is checked against: a
    // fixed total for the handshake types, a floor for the session types

    print_int( "PACKET_TYPE_0", ROCKETNET_CONNECTION_REQUEST_PACKET );
    print_int( "PACKET_TYPE_1", ROCKETNET_CONNECTION_DENIED_PACKET );
    print_int( "PACKET_TYPE_2", ROCKETNET_CONNECTION_CHALLENGE_PACKET );
    print_int( "PACKET_TYPE_3", ROCKETNET_CONNECTION_RESPONSE_PACKET );
    print_int( "PACKET_TYPE_4", ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );
    print_int( "PACKET_TYPE_5", ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    print_int( "PACKET_TYPE_6", ROCKETNET_CONNECTION_DISCONNECT_PACKET );
    print_int( "PACKET_TYPE_7", ROCKETNET_CONNECTION_COOKIE_PACKET );
    print_int( "PACKET_TYPE_8", ROCKETNET_CONNECTION_ACK_PACKET );
    print_int( "PACKET_TYPE_9", ROCKETNET_CONNECTION_FRAGMENT_PACKET );

    print_int( "WIRE_SIZE_0", ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    print_int( "WIRE_SIZE_1", ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );
    print_int( "WIRE_SIZE_2", ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    print_int( "WIRE_SIZE_3", ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    print_int( "WIRE_SIZE_4", ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    print_int( "WIRE_SIZE_5", ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES );
    print_int( "WIRE_SIZE_6", ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
    print_int( "WIRE_SIZE_7", ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
    print_int( "WIRE_SIZE_8", ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
    print_int( "WIRE_SIZE_9", ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES );

    return 0;
}
