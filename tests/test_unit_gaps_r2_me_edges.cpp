/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Mutation gaps (round 2, group r2-me) at exact edges of small rules: the
// size planner's overflow bound, a one-byte coalesced receive, the stats
// window's bucket width, the cookie-reply bitmap's power-of-two size, the
// transport io flags, the send budget's largest ack block, the packet lease
// ticket counter and the client io init's deferred hand-off flags. Each
// expected value is a literal or the rule's own definition, never the code
// under test. No sockets, no threads, no real time; clocks start at 1000.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_coalesce.h"
#include "rocketnet_stats_window.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_driver.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_packet_lease.h"
#include "client_io_rig.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// mutation gap: me rocketnet_memory.h:28 count > ... -> count >= ...
//
// An add whose product lands exactly on UINT64_MAX - total fits: only a
// product past it overflows.

static void test_gaps_r2_me_memory_add_takes_the_exact_maximum()
{
    uint64_t total = 0;
    const bool whole = rocketnet_memory_add( total, UINT64_MAX, 1 );
    if ( !whole || total != UINT64_MAX )
    {
        printf( "fault: rocketnet_memory_add( 0, UINT64_MAX, 1 ) refused or gave %llu: a sum that exactly reaches UINT64_MAX was called an overflow\n",
                (unsigned long long) total );
    }
    check( whole );
    check( total == UINT64_MAX );

    total = 0;
    check( rocketnet_memory_add( total, 1, UINT64_MAX ) );
    check( total == UINT64_MAX );

    total = 10;
    check( rocketnet_memory_add( total, ( UINT64_MAX - 10 ) / 5, 5 ) );
    check( total == 10 + ( UINT64_MAX - 10 ) / 5 * 5 );

    // one past it is the overflow

    total = 1;
    check( !rocketnet_memory_add( total, UINT64_MAX, 1 ) );
    check( total == 1 );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_memory_add_takes_the_exact_maximum );

// mutation gap: me rocketnet_coalesce.h:39 message_bytes <= 0 -> <= 1
//
// A one-byte datagram is one segment of one byte, with or without a stride.

static void test_gaps_r2_me_coalesce_split_one_byte_is_one_segment()
{
    int offsets[4] = { -1, -1, -1, -1 };
    int lengths[4] = { -1, -1, -1, -1 };

    const int no_stride = rocketnet_coalesce_split( 1, 0, offsets, lengths, 4 );
    if ( no_stride != 1 )
    {
        printf( "fault: a one-byte coalesced receive split into %d segments, not 1: the datagram is lost\n", no_stride );
    }
    check( no_stride == 1 );
    check( offsets[0] == 0 );
    check( lengths[0] == 1 );

    offsets[0] = -1;
    lengths[0] = -1;
    check( rocketnet_coalesce_split( 1, 1200, offsets, lengths, 4 ) == 1 );
    check( offsets[0] == 0 );
    check( lengths[0] == 1 );

    check( rocketnet_coalesce_split( 0, 1200, offsets, lengths, 4 ) == 0 );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_coalesce_split_one_byte_is_one_segment );

// mutation gap: me rocketnet_stats_window.h:22 1.0 / double( N - 1 ) -> 1.0 / double( N - 0 )
//
// N buckets of 1/(N-1) second each hold at least a whole second. With N = 4
// a bucket is a third of a second: 0.3 s keeps the bucket, 0.34 s moves on.

struct gaps_stats_bucket_t
{
    uint64_t frame;
    double time;
    int value;
};

static void test_gaps_r2_me_stats_window_bucket_is_a_third_at_four()
{
    gaps_stats_bucket_t buckets[4] = {};
    int index = 0;

    index = rocketnet_stats_window_advance( buckets, index, 1, 1000.0 );
    check( index == 1 );
    check( buckets[1].frame == 1 && buckets[1].time == 1000.0 );

    const int held = rocketnet_stats_window_advance( buckets, index, 2, 1000.3 );
    if ( held != index )
    {
        printf( "fault: the stats window moved to a new bucket after 0.3 s with 4 buckets: a bucket is narrower than 1/3 s, so the window holds less than a second\n" );
    }
    check( held == index );

    index = rocketnet_stats_window_advance( buckets, index, 3, 1000.34 );
    check( index == 2 );
    check( buckets[2].frame == 3 && buckets[2].time == 1000.34 );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_stats_window_bucket_is_a_third_at_four );

// mutation gap: me rocketnet_server_protocol.h:85 bits < bits_needed -> bits <= bits_needed
//
// The cookie-reply source bitmap is the next power of two at or above
// FACTOR times the receive bank: a bound that is already a power of two is
// taken as it is, never doubled. At the default ROCKETNET_MAX_CLIENTS (1000)
// no client count makes the bound a power of two (the bank is 10240 up to 640
// clients, then 16 per client up to 1000), so this case reaches that edge
// only in a larger build: at -DROCKETNET_MAX_CLIENTS=10000, 1024 and 2048
// clients make it 2^18 and 2^19.

static uint64_t gaps_next_power_of_two( uint64_t value )
{
    uint64_t power = 1;
    while ( power < value )
    {
        power <<= 1;
    }
    return power;
}

static void test_gaps_r2_me_cookie_reply_bitmap_is_the_next_power_of_two()
{
    const int clients[] = { 1, 2, 16, 100, 512, 640, 641, 1000, 1024, 2048, ROCKETNET_MAX_CLIENTS };
    for ( size_t i = 0; i < sizeof( clients ) / sizeof( clients[0] ); i++ )
    {
        const int m = clients[i];
        const uint64_t needed = (uint64_t) ROCKETNET_SERVER_COOKIE_REPLY_SOURCE_FACTOR * (uint64_t) rocketnet_server_uring_bank_packets( m );
        uint64_t expected = gaps_next_power_of_two( needed );
        if ( expected < 64 )
        {
            expected = 64;
        }
        const uint64_t bits = (uint64_t) rocketnet_server_cookie_reply_source_words( m ) * 64;
        if ( bits != expected )
        {
            printf( "fault: the cookie-reply bitmap for %d clients is %llu bits, not %llu, the next power of two at or above %llu\n",
                    m, (unsigned long long) bits, (unsigned long long) expected, (unsigned long long) needed );
        }
        check( bits == expected );
    }
}
REGISTER_UNIT_TEST( test_gaps_r2_me_cookie_reply_bitmap_is_the_next_power_of_two );

// mutation gap: me rocketnet_driver.h:47 BINDS_PUBLIC_ADDRESS = 1 << 3 -> 0 << 3
//
// Each io flag is its own nonzero bit, so a transport that declares one is
// seen to have it.

static void test_gaps_r2_me_transport_io_flags_are_distinct_bits()
{
    const unsigned int flags[] = {
        ROCKETNET_TRANSPORT_STAGED,
        ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
        ROCKETNET_TRANSPORT_FINISH_FINAL,
        ROCKETNET_TRANSPORT_BINDS_PUBLIC_ADDRESS,
        ROCKETNET_TRANSPORT_REFLUSHES,
    };
    const char * names[] = { "STAGED", "FLUSH_ON_UPDATE", "FINISH_FINAL", "BINDS_PUBLIC_ADDRESS", "REFLUSHES" };

    unsigned int seen = 0;
    for ( size_t i = 0; i < sizeof( flags ) / sizeof( flags[0] ); i++ )
    {
        const bool single = flags[i] != 0 && ( flags[i] & ( flags[i] - 1 ) ) == 0;
        if ( !single || ( seen & flags[i] ) != 0 )
        {
            printf( "fault: transport io flag %s is 0x%x, not a nonzero bit of its own\n", names[i], flags[i] );
        }
        check( single );
        check( ( seen & flags[i] ) == 0 );
        seen |= flags[i];

        rocketnet_server_transport_io_operations_t operations;
        memset( &operations, 0, sizeof( operations ) );
        operations.flags = flags[i];
        rocketnet_server_transport_io_t transport = { &operations, NULL };
        check( rocketnet_server_transport_io_has( transport, flags[i] ) );
    }
}
REGISTER_UNIT_TEST( test_gaps_r2_me_transport_io_flags_are_distinct_bits );

#if ROCKETNET_DEV

// mutation gap: me rocketnet_send_budget.h:146 > ROCKETNET_ACK_BLOCK_MAX_BYTES -> >= (dev-only)
//
// The largest ack block the seal path writes is accepted: its packet's wire
// bytes are the overhead, the block and the payload.

static void test_gaps_r2_me_send_budget_wire_bytes_takes_the_largest_block()
{
    const int bytes = rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MAX_BYTES, 1 );
    if ( bytes != ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MAX_BYTES + 1 )
    {
        printf( "fault: a packet with the largest ack block (%d bytes) and one payload byte reads as %d wire bytes, not %d\n",
                ROCKETNET_ACK_BLOCK_MAX_BYTES, bytes, ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MAX_BYTES + 1 );
    }
    check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MAX_BYTES + 1 );
    check( rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MAX_BYTES + 1, 1 ) == 0 );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_send_budget_wire_bytes_takes_the_largest_block );

#endif // #if ROCKETNET_DEV

// mutation gap: me rocketnet.cpp:1097 compare_exchange_weak -> !( compare_exchange_weak )
//
// A ticket is claimed before it is returned: the counter has moved past
// every ticket handed out, so no other caller can be handed it. The first
// ticket a process takes is 1 (the counter's start; 0 is exhaustion), and
// each one after it is one more. A counter that returns its value after the
// claim instead hands out the next caller's ticket: on one thread that skips
// 1, and between two threads it gives both the same ticket (that race is
// functional territory; this case runs in its own worker process, so it takes
// the process's first tickets).

static void test_gaps_r2_me_packet_lease_tickets_are_claimed_in_order()
{
    const uint64_t first = rocketnet_next_packet_lease_id();
    if ( first != 1 )
    {
        printf( "fault: the first packet lease ticket in this process is %llu, not 1: the counter returned a ticket it had not claimed\n",
                (unsigned long long) first );
    }
    check( first == 1 );

    uint64_t previous = first;
    for ( int i = 0; i < 1000; i++ )
    {
        const uint64_t ticket = rocketnet_next_packet_lease_id();
        check( ticket == previous + 1 );
        previous = ticket;
    }
}
REGISTER_UNIT_TEST( test_gaps_r2_me_packet_lease_tickets_are_claimed_in_order );

// mutation gap: me rocketnet_client_io.h:199 io->to_held[i] = false deleted
//
// Init is the whole contract of a fresh io: a reused io whose rings held a
// deferred destination starts with none bound.

static double gaps_client_io_time( void * context )
{
    return *(const double *) context;
}

static void test_gaps_r2_me_client_io_init_unbinds_every_ring()
{
    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rocketnet_client_transport_io_t transport = {};
    double now = 1000.0;

    rocketnet_client_io_t * io = new rocketnet_client_io_t();
    rocketnet_client_io_rig_init( rig, io, &transport, gaps_client_io_time, &now );

    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        io->to_held[i] = true;
        rocketnet_address_load_ipv4( &io->to[i], 0x7f000001, (uint16_t) ( 9000 + i ) );
    }

    rocketnet_client_io_rig_init( rig, io, &transport, gaps_client_io_time, &now );

    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        if ( io->to_held[i] )
        {
            printf( "fault: after init ring %d still holds a bound destination from before: the next hand-off would send to the old address\n", i );
        }
        check( !io->to_held[i] );
    }

    delete io;
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_client_io_init_unbinds_every_ring );
