/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The refused-destination table every segmenting server transport shares
// (sdk/rocketnet_segment_refused.h: gso, io_uring, iocp). Pure: no sockets,
// no syscall, no wall clock. The seed is per transport instance: two
// instances place the same address in different entries, and in each the
// lookup matches the whole address.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet_segment_refused.h"

static struct rocketnet_address_t segment_refused_ipv4( uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port )
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof( address ) );
    address.type = ROCKETNET_ADDRESS_IPV4;
    address.data.ipv4[0] = a;
    address.data.ipv4[1] = b;
    address.data.ipv4[2] = c;
    address.data.ipv4[3] = d;
    address.port = port;
    return address;
}

static struct rocketnet_address_t segment_refused_ipv6( uint16_t last, uint16_t port )
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof( address ) );
    address.type = ROCKETNET_ADDRESS_IPV6;
    address.data.ipv6[0] = 0x2001;
    address.data.ipv6[1] = 0x0db8;
    address.data.ipv6[7] = last;
    address.port = port;
    return address;
}

static void test_segment_refused_seed_places_addresses_per_instance()
{
    static struct rocketnet_segment_refused_t first;
    static struct rocketnet_segment_refused_t second;
    memset( &first, 0, sizeof( first ) );
    memset( &second, 0, sizeof( second ) );
    first.seed = 0x12345678u;
    second.seed = 0x9abcdef0u;

    // 512 addresses, one client's ports and many hosts: under different seeds
    // an address takes a different entry far more often than the 1 in 64 a
    // shared hash would give (a shared hash: always the same entry)

    int moved = 0;
    for ( int i = 0; i < 512; i++ )
    {
        const struct rocketnet_address_t address = segment_refused_ipv4( 10, 0, (uint8_t) ( i >> 4 ), (uint8_t) i, (uint16_t) ( 40000 + ( i & 15 ) ) );
        const int a = rocketnet_segment_refused_entry( &first, &address );
        const int b = rocketnet_segment_refused_entry( &second, &address );
        rocketnet_unit_check( a >= 0 && a < ROCKETNET_SEGMENT_REFUSED_ENTRIES );
        rocketnet_unit_check( b >= 0 && b < ROCKETNET_SEGMENT_REFUSED_ENTRIES );
        moved += a != b ? 1 : 0;
    }
    rocketnet_unit_check( moved >= 448 );

    // the same seed places an address in the same entry every time
    const struct rocketnet_address_t fixed = segment_refused_ipv4( 192, 168, 1, 7, 50000 );
    rocketnet_unit_check( rocketnet_segment_refused_entry( &first, &fixed ) == rocketnet_segment_refused_entry( &first, &fixed ) );

    // two addresses sharing an entry under the first seed (an attacker's pick
    // against that instance) are found apart under the second
    struct rocketnet_address_t victim = segment_refused_ipv4( 172, 16, 0, 1, 50000 );
    struct rocketnet_address_t aimed = victim;
    bool found = false;
    for ( int port = 1; port < 65536 && !found; port++ )
    {
        aimed.port = (uint16_t) port;
        found = port != victim.port && rocketnet_segment_refused_entry( &first, &aimed ) == rocketnet_segment_refused_entry( &first, &victim );
    }
    rocketnet_unit_check( found );
    rocketnet_unit_check( rocketnet_segment_refused_entry( &second, &aimed ) != rocketnet_segment_refused_entry( &second, &victim ) );

    // lookups match the whole address in each instance: the aimed address
    // refusing takes the victim's entry in the first instance only
    rocketnet_unit_check( !rocketnet_segment_refused_add( &first, &victim ) );
    rocketnet_unit_check( !rocketnet_segment_refused_add( &second, &victim ) );
    rocketnet_unit_check( rocketnet_segment_refused_contains( &first, &victim ) );
    rocketnet_unit_check( rocketnet_segment_refused_contains( &second, &victim ) );
    rocketnet_unit_check( !rocketnet_segment_refused_contains( &first, &aimed ) );
    rocketnet_unit_check( !rocketnet_segment_refused_contains( &second, &aimed ) );

    rocketnet_unit_check( !rocketnet_segment_refused_add( &first, &aimed ) );
    rocketnet_unit_check( !rocketnet_segment_refused_add( &second, &aimed ) );
    rocketnet_unit_check( rocketnet_segment_refused_contains( &first, &aimed ) );
    rocketnet_unit_check( !rocketnet_segment_refused_contains( &first, &victim ) );   // displaced: pays one refusal again
    rocketnet_unit_check( rocketnet_segment_refused_contains( &second, &aimed ) );
    rocketnet_unit_check( rocketnet_segment_refused_contains( &second, &victim ) );   // kept: a different entry here
    rocketnet_unit_check( first.destinations == 1 );
    rocketnet_unit_check( second.destinations == 2 );

    // an IPv6 destination: the whole address, every word
    const struct rocketnet_address_t v6 = segment_refused_ipv6( 0x0042, 50000 );
    struct rocketnet_address_t v6_other = v6;
    v6_other.data.ipv6[4] = 1;
    rocketnet_unit_check( !rocketnet_segment_refused_add( &second, &v6 ) );
    rocketnet_unit_check( rocketnet_segment_refused_contains( &second, &v6 ) );
    rocketnet_unit_check( !rocketnet_segment_refused_contains( &second, &v6_other ) );
    rocketnet_unit_check( !rocketnet_segment_refused_contains( &second, &fixed ) );
}

REGISTER_UNIT_TEST( test_segment_refused_seed_places_addresses_per_instance );

// the transport-wide rule under any seed: off only when the threshold of
// distinct destinations refused with no segmented send ever accepted

static void test_segment_refused_threshold_under_seeds()
{
    const uint32_t seeds[] = { 0u, 1u, 0x80000000u, 0xdeadbeefu };
    for ( int s = 0; s < (int) ( sizeof( seeds ) / sizeof( seeds[0] ) ); s++ )
    {
        for ( int accepted = 0; accepted < 2; accepted++ )
        {
            static struct rocketnet_segment_refused_t table;
            memset( &table, 0, sizeof( table ) );
            table.seed = seeds[s];
            table.accepted = accepted != 0;

            int added = 0;
            for ( int i = 0; i < 4096 && added < ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD; i++ )
            {
                const struct rocketnet_address_t address = segment_refused_ipv4( 10, 1, (uint8_t) ( i >> 8 ), (uint8_t) i, 50000 );
                if ( rocketnet_segment_refused_contains( &table, &address ) ||
                     table.destination[rocketnet_segment_refused_entry( &table, &address )].type != ROCKETNET_ADDRESS_NONE )
                {
                    continue;                       // an occupied entry: not a new destination for the count
                }
                const bool off = rocketnet_segment_refused_add( &table, &address );
                added++;
                rocketnet_unit_check( off == ( !accepted && added == ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD ) );
                rocketnet_unit_check( rocketnet_segment_refused_contains( &table, &address ) );
            }
            rocketnet_unit_check( added == ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD );
            rocketnet_unit_check( table.destinations == ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD );
        }
    }
}

REGISTER_UNIT_TEST( test_segment_refused_threshold_under_seeds );
