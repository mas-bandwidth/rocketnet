/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// rocketnet_max_unfragmented_message_bytes (sdk/rocketnet.h): the one-record
// limit of each reliability, the rows of standard/MESSAGES.md m2.2, with the
// WIDE header on a channel of 256 or more; 0 for an unknown reliability or a
// channel out of range; and a pure function, so it counts no misuse and
// works whatever the library state. No socket, no clock.

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_layers.h"

#define check rocketnet_unit_check

static void test_max_unfragmented_message_bytes_are_the_record_limits()
{
    const uint64_t misuse = rocketnet_api_misuse_count();

    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_UNRELIABLE, 0 ) == ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_UNRELIABLE, 0 ) == 1411 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 0 ) == 1411 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 0 ) == 1405 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE, 0 ) == 1408 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 0 ) == 1408 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0 ) == 1401 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0 ) == 1401 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, 0 ) == 1399 );

    // the last channel; the channel width changes only the sequenced headers

    const int last = ROCKETNET_MAX_CHANNELS - 1;
    const int wide = last >= 256 ? 1 : 0;
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_UNRELIABLE, last ) == 1411 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, last ) == 1405 - wide );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE_ORDERED, last ) == 1401 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, last ) == 1399 - wide );

    // every limit fits one packet's payload

    for ( int reliability = ROCKETNET_RELIABILITY_UNRELIABLE; reliability <= ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT; reliability++ )
    {
        const int bytes = rocketnet_max_unfragmented_message_bytes( (enum rocketnet_reliability_t) reliability, 0 );
        check( bytes > 0 && bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    }

    // refusals: 0, and nothing counted

    // through int variables, as test_unit_contract_audit does: a constant
    // outside the enum's range is a conversion error under gcc
    int below = -1;
    int above = ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT + 1;
    check( rocketnet_max_unfragmented_message_bytes( (enum rocketnet_reliability_t) below, 0 ) == 0 );
    check( rocketnet_max_unfragmented_message_bytes( (enum rocketnet_reliability_t) above, 0 ) == 0 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE, -1 ) == 0 );
    check( rocketnet_max_unfragmented_message_bytes( ROCKETNET_RELIABILITY_RELIABLE, ROCKETNET_MAX_CHANNELS ) == 0 );

    check( rocketnet_api_misuse_count() == misuse );
}

REGISTER_UNIT_TEST( test_max_unfragmented_message_bytes_are_the_record_limits );
