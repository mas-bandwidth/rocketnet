/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The N-buffer handoff (sdk/rocketnet_buffer_handoff.h) in the orders
// proofs/tla/RNBufferHandoff.tla allows, on one thread: no sockets, no real time.
// The owner of every buffer is checked after each exchange (OneOwner), a
// publish with no free buffer or a full ready queue is owed and reported to
// the consumer's next exchange (ArrivedDelivered's wake), and buffers leave
// the ready queue in the order they were published (InOrder).

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_buffer_handoff.h"

#define BUFFER_HANDOFF_TEST_N 3

// each index appears exactly once across producer, consumer, ready and free
static bool buffer_handoff_one_owner( const rocketnet_buffer_handoff_t<BUFFER_HANDOFF_TEST_N> * handoff, int producer, int consumer )
{
    int owners[BUFFER_HANDOFF_TEST_N] = {};
    if ( producer >= 0 )
        owners[producer]++;
    if ( consumer >= 0 )
        owners[consumer]++;
    for ( int i = 0; i < handoff->ready_count; i++ )
        owners[handoff->ready_index[( handoff->ready_head + i ) % BUFFER_HANDOFF_TEST_N]]++;
    for ( int i = 0; i < handoff->free_count; i++ )
        owners[handoff->free_index[( handoff->free_head + i ) % BUFFER_HANDOFF_TEST_N]]++;
    for ( int i = 0; i < BUFFER_HANDOFF_TEST_N; i++ )
    {
        if ( owners[i] != 1 )
            return false;
    }
    return true;
}

static void test_buffer_handoff_init_gives_producer_one()
{
    rocketnet_buffer_handoff_t<BUFFER_HANDOFF_TEST_N> handoff;
    rocketnet_buffer_handoff_init( &handoff, 1 );
    rocketnet_unit_check( handoff.free_count == BUFFER_HANDOFF_TEST_N - 1 );
    rocketnet_unit_check( handoff.ready_count == 0 );
    rocketnet_unit_check( !handoff.owed );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, 1, -1 ) );
    rocketnet_unit_check( rocketnet_buffer_handoff_take( &handoff ) == -1 );
}
REGISTER_UNIT_TEST( test_buffer_handoff_init_gives_producer_one );

// the send rings: MaxReady 2, so the producer publishes twice before the
// consumer takes either, and the consumer takes them oldest first
static void test_buffer_handoff_send_queue_in_order()
{
    rocketnet_buffer_handoff_t<BUFFER_HANDOFF_TEST_N> handoff;
    rocketnet_buffer_handoff_init( &handoff, 0 );
    int producer = 0;
    int consumer = -1;

    const int first = producer;
    producer = rocketnet_buffer_handoff_publish( &handoff, producer, BUFFER_HANDOFF_TEST_N - 1 );
    rocketnet_unit_check( producer >= 0 && producer != first );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );

    const int second = producer;
    producer = rocketnet_buffer_handoff_publish( &handoff, producer, BUFFER_HANDOFF_TEST_N - 1 );
    rocketnet_unit_check( producer >= 0 && producer != first && producer != second );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );

    // no free buffer: refused, owed, and the producer keeps its buffer
    rocketnet_unit_check( rocketnet_buffer_handoff_publish( &handoff, producer, BUFFER_HANDOFF_TEST_N - 1 ) == -1 );
    rocketnet_unit_check( handoff.owed );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );

    consumer = rocketnet_buffer_handoff_take( &handoff );
    rocketnet_unit_check( consumer == first );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );
    rocketnet_buffer_handoff_release( &handoff, consumer );
    consumer = rocketnet_buffer_handoff_take( &handoff );
    rocketnet_unit_check( consumer == second );
    rocketnet_unit_check( rocketnet_buffer_handoff_owed( &handoff ) );
    rocketnet_unit_check( !rocketnet_buffer_handoff_owed( &handoff ) );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );

    // the released ring is free again: the deferred publish passes
    const int third = producer;
    producer = rocketnet_buffer_handoff_publish( &handoff, producer, BUFFER_HANDOFF_TEST_N - 1 );
    rocketnet_unit_check( producer == first );
    rocketnet_buffer_handoff_release( &handoff, consumer );
    consumer = rocketnet_buffer_handoff_take( &handoff );
    rocketnet_unit_check( consumer == third );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );
}
REGISTER_UNIT_TEST( test_buffer_handoff_send_queue_in_order );

// the receive triple buffer: MaxReady 1, so a second publish waits until the
// consumer took the first, and the consumer's exchange reports the owed one
static void test_buffer_handoff_receive_triple_buffer()
{
    rocketnet_buffer_handoff_t<BUFFER_HANDOFF_TEST_N> handoff;
    rocketnet_buffer_handoff_init( &handoff, 0 );
    int producer = 0;
    int consumer = -1;

    const int first = producer;
    producer = rocketnet_buffer_handoff_publish( &handoff, producer, 1 );
    rocketnet_unit_check( producer >= 0 );

    // one is ready and one is free, but the ready one has not been taken
    rocketnet_unit_check( rocketnet_buffer_handoff_publish( &handoff, producer, 1 ) == -1 );
    rocketnet_unit_check( handoff.owed );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );

    consumer = rocketnet_buffer_handoff_take( &handoff );
    rocketnet_unit_check( consumer == first );
    rocketnet_unit_check( rocketnet_buffer_handoff_owed( &handoff ) );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );

    // with the consumer holding one, the producer publishes into the third
    const int second = producer;
    producer = rocketnet_buffer_handoff_publish( &handoff, producer, 1 );
    rocketnet_unit_check( producer >= 0 && producer != first && producer != second );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );

    // ready full and none free: owed until the consumer gives one back
    rocketnet_unit_check( rocketnet_buffer_handoff_publish( &handoff, producer, 1 ) == -1 );
    rocketnet_buffer_handoff_release( &handoff, consumer );
    consumer = rocketnet_buffer_handoff_take( &handoff );
    rocketnet_unit_check( consumer == second );
    rocketnet_unit_check( rocketnet_buffer_handoff_owed( &handoff ) );
    rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );
    rocketnet_unit_check( rocketnet_buffer_handoff_publish( &handoff, producer, 1 ) == first );
}
REGISTER_UNIT_TEST( test_buffer_handoff_receive_triple_buffer );

// a long deterministic run of both sides: every exchange keeps one owner per
// buffer, and the consumer sees published buffers in publish order
static void test_buffer_handoff_long_run_one_owner()
{
    for ( int max_ready = 1; max_ready < BUFFER_HANDOFF_TEST_N; max_ready++ )
    {
        rocketnet_buffer_handoff_t<BUFFER_HANDOFF_TEST_N> handoff;
        rocketnet_buffer_handoff_init( &handoff, 0 );
        int producer = 0;
        int consumer = -1;
        int published[64];
        int num_published = 0;
        int num_taken = 0;
        uint32_t seed = 1000;
        for ( int i = 0; i < 4096; i++ )
        {
            seed = seed * 1664525u + 1013904223u;
            if ( ( seed >> 16 ) & 1 )
            {
                const int buffer = producer;
                const int next = rocketnet_buffer_handoff_publish( &handoff, producer, max_ready );
                if ( next >= 0 )
                {
                    published[num_published++ % 64] = buffer;
                    producer = next;
                }
            }
            else
            {
                if ( consumer >= 0 )
                    rocketnet_buffer_handoff_release( &handoff, consumer );
                consumer = rocketnet_buffer_handoff_take( &handoff );
                if ( consumer >= 0 )
                {
                    rocketnet_unit_check( consumer == published[num_taken % 64] );
                    num_taken++;
                }
                (void) rocketnet_buffer_handoff_owed( &handoff );
            }
            rocketnet_unit_check( handoff.ready_count <= max_ready );
            rocketnet_unit_check( buffer_handoff_one_owner( &handoff, producer, consumer ) );
        }
        rocketnet_unit_check( num_taken > 0 );
    }
}
REGISTER_UNIT_TEST( test_buffer_handoff_long_run_one_owner );
