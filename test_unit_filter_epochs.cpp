/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The receiver's filter epoch window (tla/RNFilterEpochs.tla, standard/TRANSPORT.md
// t5.1): a rotation leaves two generations live, and an
// epoch the table does not hold is refused on the comparison before any MAC.
// Pure table computation; not wired into a receive path.

#include "test_unit_registry.h"

#include "sdk/rocketnet_protocol.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static void fill_magic( uint8_t * magic, uint8_t seed )
{
    for ( int i = 0; i < ROCKETNET_FILTER_MAGIC_BYTES; i++ )
        magic[i] = (uint8_t) ( seed + i * 17 );
}

// build a prefilter header whose value is the keyed filter value under magic
static void build_packet( uint8_t * packet, int packet_bytes, uint8_t packet_type,
                          uint8_t epoch, const uint8_t * magic, const uint8_t * four )
{
    memset( packet, 0, (size_t) packet_bytes );
    packet[0] = packet_type;
    packet[1] = epoch;
    memcpy( packet + ROCKETNET_PREFILTER_BYTES, four, 4 );

    uint8_t value[ROCKETNET_FILTER_VALUE_BYTES];
    rocketnet_keyed_filter_value( magic, packet_type, (uint16_t) packet_bytes,
                                  packet + ROCKETNET_PREFILTER_BYTES, value );
    memcpy( packet + 2, value, ROCKETNET_FILTER_VALUE_BYTES );
}

// A rotation does not break packets already in flight: with generations 1 and
// 2 installed, a packet carrying either live epoch and its generation's value
// is accepted, and each costs exactly one MAC (RNFilterEpochs Check).
static void filter_epoch_window_accepts_two_generations()
{
    struct rocketnet_filter_epochs_t epochs;
    memset( &epochs, 0, sizeof(epochs) );

    uint8_t magic_one[ROCKETNET_FILTER_MAGIC_BYTES];
    uint8_t magic_two[ROCKETNET_FILTER_MAGIC_BYTES];
    fill_magic( magic_one, 0x10 );
    fill_magic( magic_two, 0x60 );

    // install takes 1 first, then only the next generation, and never 0
    struct rocketnet_filter_epochs_t refused;
    memset( &refused, 0, sizeof(refused) );
    check( rocketnet_filter_epochs_install( &refused, 0, magic_one ) == 0 );
    check( rocketnet_filter_epochs_install( &refused, 2, magic_one ) == 0 );
    check( rocketnet_filter_epochs_install( &refused, 1, magic_one ) == 1 );
    check( rocketnet_filter_epochs_install( &refused, 1, magic_two ) == 0 );
    check( rocketnet_filter_epochs_install( &refused, 3, magic_two ) == 0 );
    check( rocketnet_filter_epochs_install( &refused, 2, magic_two ) == 1 );

    // generations wrap 255 back to 1, never to 0
    struct rocketnet_filter_epochs_t wrapped;
    memset( &wrapped, 0, sizeof(wrapped) );
    for ( int e = 1; e <= 255; e++ )
        check( rocketnet_filter_epochs_install( &wrapped, (uint8_t) e, magic_one ) == 1 );
    check( wrapped.epochs[wrapped.live_count - 1] == 255 );
    check( rocketnet_filter_epochs_install( &wrapped, 1, magic_two ) == 1 );
    check( rocketnet_filter_epochs_install( &wrapped, 0, magic_two ) == 0 );
    check( rocketnet_filter_epochs_install( &wrapped, 3, magic_two ) == 0 );

    check( rocketnet_filter_epochs_install( &epochs, 1, magic_one ) == 1 );
    check( rocketnet_filter_epochs_install( &epochs, 2, magic_two ) == 1 );

    const uint8_t four[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t packet[32];

    const uint64_t macs_before = rocketnet_filter_epochs_mac_count();

    build_packet( packet, sizeof(packet), 0x05, 1, magic_one, four );
    check( rocketnet_filter_epochs_check( &epochs, packet, sizeof(packet) ) == 1 );
    check( rocketnet_filter_epochs_mac_count() - macs_before == 1 );

    build_packet( packet, sizeof(packet), 0x05, 2, magic_two, four );
    check( rocketnet_filter_epochs_check( &epochs, packet, sizeof(packet) ) == 1 );
    check( rocketnet_filter_epochs_mac_count() - macs_before == 2 );

    // the value is the generation's: one altered value byte on a live epoch is
    // refused after its one MAC
    build_packet( packet, sizeof(packet), 0x05, 2, magic_two, four );
    packet[2] ^= 0x01;
    check( rocketnet_filter_epochs_check( &epochs, packet, sizeof(packet) ) == 0 );
    check( rocketnet_filter_epochs_mac_count() - macs_before == 3 );
}

// Epoch 0 and an epoch naming no live generation are both refused on the
// comparison, before any MAC (RNFilterEpochs Check, DeadEpochRefusedWithoutMac
// and NeverKeyedAtZero). A retired generation is dead too, and its magic is
// wiped from the table.
static void filter_epoch_window_refuses_before_any_mac()
{
    struct rocketnet_filter_epochs_t epochs;
    memset( &epochs, 0, sizeof(epochs) );

    uint8_t magics[5][ROCKETNET_FILTER_MAGIC_BYTES];
    for ( int i = 0; i < 5; i++ )
    {
        fill_magic( magics[i], (uint8_t) ( 0x20 + i * 0x30 ) );
        check( rocketnet_filter_epochs_install( &epochs, (uint8_t) ( i + 1 ), magics[i] ) == 1 );
    }

    const uint8_t four[4] = { 0xde, 0xad, 0xbe, 0xef };
    uint8_t packet[32];

    // epoch 0: never keyed, no MAC
    build_packet( packet, sizeof(packet), 0x05, 0, magics[0], four );
    uint64_t macs_before = rocketnet_filter_epochs_mac_count();
    check( rocketnet_filter_epochs_check( &epochs, packet, sizeof(packet) ) == 0 );
    check( rocketnet_filter_epochs_mac_count() == macs_before );

    // epoch 6 names no live generation, no MAC
    build_packet( packet, sizeof(packet), 0x05, 6, magics[4], four );
    macs_before = rocketnet_filter_epochs_mac_count();
    check( rocketnet_filter_epochs_check( &epochs, packet, sizeof(packet) ) == 0 );
    check( rocketnet_filter_epochs_mac_count() == macs_before );

    // the fifth install retired generation 1: dead, and its magic is gone
    build_packet( packet, sizeof(packet), 0x05, 1, magics[0], four );
    macs_before = rocketnet_filter_epochs_mac_count();
    check( rocketnet_filter_epochs_check( &epochs, packet, sizeof(packet) ) == 0 );
    check( rocketnet_filter_epochs_mac_count() == macs_before );

    int retired_magic_survives = 0;
    for ( int i = 0; i < (int) sizeof(epochs); i++ )
        if ( ( (const uint8_t *) &epochs )[i] == magics[0][0] )
            retired_magic_survives = 1;
    check( !retired_magic_survives );

    // a live epoch still costs exactly one MAC
    build_packet( packet, sizeof(packet), 0x05, 5, magics[4], four );
    macs_before = rocketnet_filter_epochs_mac_count();
    check( rocketnet_filter_epochs_check( &epochs, packet, sizeof(packet) ) == 1 );
    check( rocketnet_filter_epochs_mac_count() - macs_before == 1 );
}

REGISTER_UNIT_TEST( filter_epoch_window_accepts_two_generations );
REGISTER_UNIT_TEST( filter_epoch_window_refuses_before_any_mac );
