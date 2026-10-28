/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The iocp client's posted receives (sdk/rocketnet_client_iocp.h,
// tla/RNClientIocp.tla) on a fake completion port: the port seam records every
// post, writes a datagram into a posted buffer only when the case delivers
// one, queues completions in order, and on close aborts every posted receive,
// releasing the aborts as the case says. No socket, no thread; the fake's
// clock starts at 1000.0 and a dequeue that finds nothing advances it by its
// timeout, so a wait's timeout is read off the clock. Every expected value is
// a literal, never computed by the code under test.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_iocp.h"
#include "rocketnet_address.h"

#include <stdint.h>
#include <string.h>

#define FAKE_IOCP_RECEIVES 4
#define FAKE_IOCP_BUFFER_BYTES 64
#define FAKE_IOCP_MAX_QUEUED 64

struct fake_iocp_port_t
{
    double time;
    int posts;
    int refuse_posts;                   // the next posts refused
    bool posted[FAKE_IOCP_RECEIVES];
    uint8_t * buffer[FAKE_IOCP_RECEIVES];
    int buffer_bytes[FAKE_IOCP_RECEIVES];
    int dequeues;
    double last_timeout;
    int wakes_posted;
    bool closed;
    int aborts_per_dequeue;             // after close: aborts released per dequeue (-1: all at once)
    int aborts_held;                    // aborts closesocket owes, not yet queued
    int num_queued;
    rocketnet_client_iocp_completion_t queued[FAKE_IOCP_MAX_QUEUED];
};

static fake_iocp_port_t fake_iocp;

static bool fake_iocp_post_receive( void * context, int receive, uint8_t * buffer, int buffer_bytes )
{
    rocketnet_unit_check( context == &fake_iocp );
    rocketnet_unit_check( !fake_iocp.closed );
    rocketnet_unit_check( !fake_iocp.posted[receive] );

    if ( fake_iocp.refuse_posts > 0 )
    {
        fake_iocp.refuse_posts--;
        return false;
    }

    fake_iocp.posts++;
    fake_iocp.posted[receive] = true;
    fake_iocp.buffer[receive] = buffer;
    fake_iocp.buffer_bytes[receive] = buffer_bytes;

    return true;
}

static void fake_iocp_queue( int kind, int receive, bool ok, int packet_bytes, const rocketnet_address_t * from )
{
    rocketnet_unit_check( fake_iocp.num_queued < FAKE_IOCP_MAX_QUEUED );

    rocketnet_client_iocp_completion_t * completion = &fake_iocp.queued[fake_iocp.num_queued++];
    memset( completion, 0, sizeof( *completion ) );
    completion->kind = kind;
    completion->receive = receive;
    completion->ok = ok;
    completion->packet_bytes = packet_bytes;
    if ( from )
    {
        completion->from = *from;
    }
}

// closesocket's aborts: each posted receive completes, failed

static void fake_iocp_release_aborts( int count )
{
    for ( int i = 0; i < FAKE_IOCP_RECEIVES && count != 0 && fake_iocp.aborts_held > 0; i++ )
    {
        if ( !fake_iocp.posted[i] )
            continue;
        fake_iocp.posted[i] = false;
        fake_iocp.aborts_held--;
        fake_iocp_queue( ROCKETNET_CLIENT_IOCP_COMPLETION_RECEIVE, i, false, 0, NULL );
        if ( count > 0 )
        {
            count--;
        }
    }
}

static int fake_iocp_dequeue( void * context, rocketnet_client_iocp_completion_t * completions, int max_completions, double timeout_seconds )
{
    rocketnet_unit_check( context == &fake_iocp );
    fake_iocp.dequeues++;
    fake_iocp.last_timeout = timeout_seconds;
    if ( fake_iocp.closed && fake_iocp.aborts_per_dequeue != 0 )
    {
        fake_iocp_release_aborts( fake_iocp.aborts_per_dequeue );
    }

    if ( fake_iocp.num_queued == 0 )
    {
        fake_iocp.time += timeout_seconds;
        return 0;
    }

    const int count = fake_iocp.num_queued < max_completions ? fake_iocp.num_queued : max_completions;
    memcpy( completions, fake_iocp.queued, sizeof( completions[0] ) * (size_t) count );
    memmove( fake_iocp.queued, fake_iocp.queued + count, sizeof( fake_iocp.queued[0] ) * (size_t) ( fake_iocp.num_queued - count ) );
    fake_iocp.num_queued -= count;

    return count;
}

static void fake_iocp_post_wake( void * context )
{
    rocketnet_unit_check( context == &fake_iocp );
    fake_iocp.wakes_posted++;
    fake_iocp_queue( ROCKETNET_CLIENT_IOCP_COMPLETION_WAKE, -1, false, 0, NULL );
}

static void fake_iocp_close( void * context )
{
    rocketnet_unit_check( context == &fake_iocp );
    rocketnet_unit_check( !fake_iocp.closed );
    fake_iocp.closed = true;
    for ( int i = 0; i < FAKE_IOCP_RECEIVES; i++ )
    {
        fake_iocp.aborts_held += fake_iocp.posted[i] ? 1 : 0;
    }
}

static const rocketnet_client_iocp_port_t fake_iocp_port = {
    fake_iocp_post_receive, fake_iocp_dequeue, fake_iocp_post_wake, fake_iocp_close
};

static void fake_iocp_reset()
{
    memset( &fake_iocp, 0, sizeof( fake_iocp ) );
    fake_iocp.time = 1000.0;
}

static rocketnet_address_t fake_iocp_server( uint16_t port )
{
    rocketnet_address_t address;
    memset( &address, 0, sizeof( address ) );
    address.type = ROCKETNET_ADDRESS_IPV4;
    address.data.ipv4[0] = 127;
    address.data.ipv4[3] = 1;
    address.port = port;
    return address;
}

// the kernel completes a posted receive with a datagram: the bytes go into
// the posted buffer only now

static void fake_iocp_deliver( int receive, uint8_t tag, int packet_bytes, uint16_t from_port )
{
    rocketnet_unit_check( fake_iocp.posted[receive] );
    rocketnet_unit_check( packet_bytes <= fake_iocp.buffer_bytes[receive] );
    for ( int i = 0; i < packet_bytes; i++ )
    {
        fake_iocp.buffer[receive][i] = (uint8_t) ( tag + i );
    }
    fake_iocp.posted[receive] = false;

    const rocketnet_address_t from = fake_iocp_server( from_port );
    fake_iocp_queue( ROCKETNET_CLIENT_IOCP_COMPLETION_RECEIVE, receive, true, packet_bytes, &from );
}

static rocketnet_client_iocp_t test_iocp;
static uint8_t test_iocp_buffers[FAKE_IOCP_RECEIVES * FAKE_IOCP_BUFFER_BYTES];

static void test_iocp_create()
{
    fake_iocp_reset();
    memset( test_iocp_buffers, 0xEE, sizeof( test_iocp_buffers ) );
    rocketnet_unit_check( rocketnet_client_iocp_init( &test_iocp, &fake_iocp_port, &fake_iocp, test_iocp_buffers, FAKE_IOCP_BUFFER_BYTES, FAKE_IOCP_RECEIVES ) == 4 );
    rocketnet_unit_check( fake_iocp.posts == 4 );
    rocketnet_unit_check( test_iocp.num_posted == 4 );
}

// receive_batch copies completed receives into the caller's slots in
// completion order, splits at max_packets, and re-posts each buffer after its
// copy; a failed completion and a refused post never shrink the depth

static void test_client_iocp_receive_completes_into_slots()
{
    test_iocp_create();

    rocketnet_address_t from[8];
    uint8_t slots[8 * 32];
    int packet_bytes[8];

    // nothing completed: nothing copied, nothing re-posted

    rocketnet_unit_check( rocketnet_client_iocp_receive_batch( &test_iocp, from, slots, 32, packet_bytes, 8 ) == 0 );
    rocketnet_unit_check( fake_iocp.posts == 4 );

    fake_iocp_deliver( 2, 10, 20, 40000 );
    fake_iocp_deliver( 0, 50, 30, 40001 );
    fake_iocp_deliver( 3, 90, 40, 40002 );
    memset( slots, 0, sizeof( slots ) );
    rocketnet_unit_check( rocketnet_client_iocp_receive_batch( &test_iocp, from, slots, 32, packet_bytes, 2 ) == 2 );
    rocketnet_unit_check( packet_bytes[0] == 20 );
    rocketnet_unit_check( packet_bytes[1] == 30 );
    rocketnet_unit_check( from[0].port == 40000 );
    rocketnet_unit_check( from[1].port == 40001 );
    rocketnet_unit_check( slots[0] == 10 && slots[19] == 29 );
    rocketnet_unit_check( slots[32] == 50 && slots[32 + 29] == 79 );

    // the two copied buffers went back to the kernel; the third is still ready

    rocketnet_unit_check( fake_iocp.posts == 6 );
    rocketnet_unit_check( fake_iocp.posted[2] && fake_iocp.posted[0] && !fake_iocp.posted[3] );
    rocketnet_unit_check( test_iocp.num_posted == 3 );

    // the rest of the split: 40 bytes into a 32-byte slot reports its true
    // size and stores the prefix

    rocketnet_unit_check( rocketnet_client_iocp_receive_batch( &test_iocp, from, slots, 32, packet_bytes, 8 ) == 1 );
    rocketnet_unit_check( packet_bytes[0] == 40 );
    rocketnet_unit_check( from[0].port == 40002 );
    rocketnet_unit_check( slots[0] == 90 && slots[31] == 121 );
    rocketnet_unit_check( fake_iocp.posts == 7 );
    rocketnet_unit_check( test_iocp.num_posted == 4 );
    rocketnet_unit_check( test_iocp.receives_completed == 3 );

    // a failed completion (an ICMP report, say) copies nothing and re-posts;
    // a refused re-post leaves the receive idle until the next call

    fake_iocp.posted[1] = false;
    fake_iocp_queue( ROCKETNET_CLIENT_IOCP_COMPLETION_RECEIVE, 1, false, 0, NULL );
    fake_iocp.refuse_posts = 1;
    rocketnet_unit_check( rocketnet_client_iocp_receive_batch( &test_iocp, from, slots, 32, packet_bytes, 8 ) == 0 );
    rocketnet_unit_check( test_iocp.receives_failed == 1 );
    rocketnet_unit_check( test_iocp.post_refusals == 1 );
    rocketnet_unit_check( test_iocp.num_posted == 3 );
    rocketnet_unit_check( !fake_iocp.posted[1] );
    rocketnet_unit_check( rocketnet_client_iocp_receive_batch( &test_iocp, from, slots, 32, packet_bytes, 8 ) == 0 );
    rocketnet_unit_check( fake_iocp.posted[1] );
    rocketnet_unit_check( test_iocp.num_posted == 4 );
    rocketnet_unit_check( fake_iocp.posts == 8 );

    // receive_batch never waits

    rocketnet_unit_check( fake_iocp.time == 1000.0 );

    fake_iocp.aborts_per_dequeue = -1;
    rocketnet_unit_check( rocketnet_client_iocp_close( &test_iocp, 1, 1.0 ) );
}

// the wake is one posted completion on the port, coalesced while one is
// outstanding; wait consumes it, and honours its timeout when none comes

static void test_client_iocp_wake_is_a_posted_completion()
{
    test_iocp_create();

    rocketnet_client_iocp_wake( &test_iocp );
    rocketnet_client_iocp_wake( &test_iocp );
    rocketnet_unit_check( fake_iocp.wakes_posted == 1 );
    rocketnet_unit_check( fake_iocp.num_queued == 1 );

    rocketnet_unit_check( rocketnet_client_iocp_wait( &test_iocp, false, 0.5 ) );
    rocketnet_unit_check( fake_iocp.dequeues == 1 );
    rocketnet_unit_check( fake_iocp.time == 1000.0 );

    // consumed: the next wait times out after its timeout

    rocketnet_unit_check( !rocketnet_client_iocp_wait( &test_iocp, false, 0.5 ) );
    rocketnet_unit_check( fake_iocp.last_timeout == 0.5 );
    rocketnet_unit_check( fake_iocp.time == 1000.5 );

    // a wake after the consumed one posts again

    rocketnet_client_iocp_wake( &test_iocp );
    rocketnet_unit_check( fake_iocp.wakes_posted == 2 );

    // a wake receive_batch dequeues is owed to the next wait, which returns
    // at once without dequeuing

    rocketnet_address_t from[4];
    uint8_t slots[4 * FAKE_IOCP_BUFFER_BYTES];
    int packet_bytes[4];
    rocketnet_unit_check( rocketnet_client_iocp_receive_batch( &test_iocp, from, slots, FAKE_IOCP_BUFFER_BYTES, packet_bytes, 4 ) == 0 );

    const int dequeues = fake_iocp.dequeues;
    rocketnet_unit_check( rocketnet_client_iocp_wait( &test_iocp, false, 1.0 ) );
    rocketnet_unit_check( fake_iocp.dequeues == dequeues );
    rocketnet_unit_check( fake_iocp.time == 1000.5 );

    // readable: a completed receive ends the wait; with readable false the
    // same completion does not, and the wait runs to its timeout

    fake_iocp_deliver( 1, 7, 12, 40000 );
    rocketnet_unit_check( rocketnet_client_iocp_wait( &test_iocp, true, 1.0 ) );
    rocketnet_unit_check( fake_iocp.time == 1000.5 );
    rocketnet_unit_check( !rocketnet_client_iocp_wait( &test_iocp, false, 0.25 ) );
    rocketnet_unit_check( fake_iocp.time == 1000.75 );
    rocketnet_unit_check( rocketnet_client_iocp_receive_batch( &test_iocp, from, slots, FAKE_IOCP_BUFFER_BYTES, packet_bytes, 4 ) == 1 );
    rocketnet_unit_check( packet_bytes[0] == 12 && slots[0] == 7 );

    fake_iocp.aborts_per_dequeue = -1;
    rocketnet_unit_check( rocketnet_client_iocp_close( &test_iocp, 1, 1.0 ) );
}

// destroy closes the socket and dequeues until every posted receive has
// completed, re-posting nothing; it reports drained only then, and a port
// whose aborts never come is never reported drained (FreedOnlyWhenDrained)

static void test_client_iocp_destroy_drains_posted_receives()
{
    test_iocp_create();

    // one receive completed and dequeued but not copied: ready, not posted

    fake_iocp_deliver( 1, 3, 10, 40000 );
    rocketnet_client_iocp_wake( &test_iocp );
    rocketnet_unit_check( rocketnet_client_iocp_wait( &test_iocp, true, 0.0 ) );
    rocketnet_unit_check( test_iocp.num_posted == 3 );

    // the aborts arrive one per dequeue: three posted, three dequeues

    fake_iocp.aborts_per_dequeue = 1;
    rocketnet_unit_check( rocketnet_client_iocp_close( &test_iocp, 10, 1.0 ) );
    rocketnet_unit_check( fake_iocp.closed );
    rocketnet_unit_check( fake_iocp.aborts_held == 0 );
    rocketnet_unit_check( test_iocp.num_posted == 0 );
    rocketnet_unit_check( fake_iocp.dequeues == 4 );
    rocketnet_unit_check( fake_iocp.posts == 4 );
    rocketnet_unit_check( test_iocp.receives_failed == 0 );
    for ( int i = 0; i < FAKE_IOCP_RECEIVES; i++ )
    {
        rocketnet_unit_check( !fake_iocp.posted[i] );
    }

    // the aborts never come: three dequeues of their timeout, then not drained

    test_iocp_create();
    fake_iocp.aborts_per_dequeue = 0;
    rocketnet_unit_check( !rocketnet_client_iocp_close( &test_iocp, 3, 1.0 ) );
    rocketnet_unit_check( test_iocp.num_posted == 4 );
    rocketnet_unit_check( fake_iocp.dequeues == 3 );
    rocketnet_unit_check( fake_iocp.time == 1003.0 );

    // they come late: the next close drains without closing again

    fake_iocp_release_aborts( -1 );
    rocketnet_unit_check( rocketnet_client_iocp_close( &test_iocp, 3, 1.0 ) );
    rocketnet_unit_check( test_iocp.num_posted == 0 );
    rocketnet_unit_check( fake_iocp.posts == 4 );
}

REGISTER_UNIT_TEST( test_client_iocp_receive_completes_into_slots );
REGISTER_UNIT_TEST( test_client_iocp_wake_is_a_posted_completion );
REGISTER_UNIT_TEST( test_client_iocp_destroy_drains_posted_receives );
