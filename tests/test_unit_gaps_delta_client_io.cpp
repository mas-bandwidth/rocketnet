/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client I/O exit's discard (sdk/rocketnet_client_io.h
// rocketnet_client_io_discard_at_stop, proofs/tla/RNClientIoShutdown.tla
// WorkerExit) against a scripted transport: what each read returns is a
// literal, so the count it leaves in receive_discarded_at_stop and the faults
// it leaves in transport_faults are exact. Single-threaded: no socket, no
// thread, no real time; the clock starts at 1000.

#include "test_unit_registry.h"
#include "client_io_rig.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// a failing check prints the fault it found, then fails as every unit check does

#define delta_io_check( condition, fault )                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if ( !( condition ) )                                                                                          \
        {                                                                                                              \
            printf( "mutation gap: %s\n", fault );                                                                     \
        }                                                                                                              \
        rocketnet_unit_check( condition );                                                                             \
    } while ( 0 )

#define DELTA_IO_SCRIPT 8

// A transport whose reads answer from a script: receive returns script[i] on
// its i-th call (a byte count, or negative for a refused read) and 0 past the
// script's end; receive_batch, when endless_batch is set, answers batch_answer
// on every call, as a sender that keeps the socket full would.

struct delta_io_transport_t
{
    int script[DELTA_IO_SCRIPT];
    int script_length;
    int receive_calls;
    bool endless_batch;
    int batch_answer;
    uint64_t receive_batch_calls;
};

static void * delta_io_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    return context;
}

static void delta_io_destroy( void * transport )
{
    (void) transport;
}

static void delta_io_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    (void) transport;
    (void) to;
    (void) data;
    (void) bytes;
}

static int delta_io_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    delta_io_transport_t * fake = (delta_io_transport_t *) transport;
    const int call = fake->receive_calls++;
    if ( call >= fake->script_length )
    {
        return 0;
    }

    const int bytes = fake->script[call];
    if ( bytes > 0 )
    {
        rocketnet_unit_check( bytes <= capacity );
        memset( data, 0x5A, (size_t) bytes );
        memset( from, 0, sizeof( *from ) );
    }
    return bytes;
}

static int delta_io_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    (void) from;
    (void) packet_data;
    (void) packet_stride;
    (void) packet_bytes;
    delta_io_transport_t * fake = (delta_io_transport_t *) transport;
    rocketnet_unit_check( fake->endless_batch );
    rocketnet_unit_check( max_packets == ROCKETNET_CLIENT_IO_DISCARD_BATCH );
    fake->receive_batch_calls++;
    return fake->batch_answer;
}

static bool delta_io_wait( void * transport, bool readable, double timeout_seconds )
{
    (void) transport;
    (void) readable;
    (void) timeout_seconds;
    return false;
}

static void delta_io_wake( void * transport )
{
    (void) transport;
}

static const rocketnet_client_transport_io_operations_t delta_io_operations_single = {
    delta_io_create, delta_io_destroy, delta_io_send, delta_io_receive,
    NULL, NULL, delta_io_wait, delta_io_wake,
    NULL, NULL, NULL, NULL
};

static const rocketnet_client_transport_io_operations_t delta_io_operations_batch = {
    delta_io_create, delta_io_destroy, delta_io_send, delta_io_receive,
    NULL, delta_io_receive_batch, delta_io_wait, delta_io_wake,
    NULL, NULL, NULL, NULL
};

struct delta_io_clock_t
{
    double now;
};

static double delta_io_time( void * context )
{
    return ( (delta_io_clock_t *) context )->now;
}

// stops an idle I/O core over the scripted transport and runs its one step:
// the exit, whose discard reads the script. Leaves the discard count and the
// faults in *discarded and *faults.

static void delta_io_stop_and_discard( const rocketnet_client_transport_io_operations_t * operations, delta_io_transport_t * fake,
                                       uint64_t * discarded, uint64_t * faults )
{
    rocketnet_client_transport_io_config_t config = { "delta", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( operations, fake, &config, &bind, &transport ) );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    delta_io_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, delta_io_time, &clock );

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9400 );
    rocketnet_client_io_stop( &io, 0, &to );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_EXITED );

    *discarded = io.receive_discarded_at_stop.load( std::memory_order_acquire );
    *faults = io.transport_faults.load( std::memory_order_relaxed );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}

// The single receive's answer is a byte count: every positive count is one
// datagram discarded, a 1-byte datagram as much as any; 0 is a drained socket,
// which ends the discard and is no fault.
// mutation gap: delta sdk/rocketnet_client_io.h:816 relational <= (bytes <= 0 ? -1)
// mutation gap: delta sdk/rocketnet_client_io.h:816 off_by_one 1 (bytes < 1 ? -1)
// mutation gap: delta sdk/rocketnet_client_io.h:816 off_by_one 1 (bytes > 1 ? 1 : 0)
// mutation gap: delta sdk/rocketnet_client_io.h:821 relational <= (got <= 0)
// mutation gap: delta sdk/rocketnet_client_io.h:821 off_by_one 1 (got < 1)

static void test_delta_client_io_discard_counts_small_datagrams_and_a_drained_socket_is_no_fault()
{
    delta_io_transport_t fake = {};
    fake.script[0] = 1;
    fake.script[1] = 16;
    fake.script[2] = 1;
    fake.script_length = 3;

    uint64_t discarded = 0, faults = 0;
    delta_io_stop_and_discard( &delta_io_operations_single, &fake, &discarded, &faults );

    delta_io_check( fake.receive_calls == 4, "the discard did not read until the socket was drained (3 datagrams, then 0)" );
    delta_io_check( discarded == 3, "the discard did not count every datagram on the socket, a 1-byte datagram included" );
    delta_io_check( faults == 0, "a drained socket (receive returned 0) was counted as a transport fault" );
}
REGISTER_UNIT_TEST( test_delta_client_io_discard_counts_small_datagrams_and_a_drained_socket_is_no_fault );

// A refused single receive (a negative answer) ends the discard and is one
// transport fault; the datagram behind it is not read.
// mutation gap: delta sdk/rocketnet_client_io.h:816 off_by_one 0 (bytes < 0 ? -0)
// mutation gap: delta sdk/rocketnet_client_io.h:823 arithmetic -- (transport_faults--)

static void test_delta_client_io_discard_counts_a_refused_read_as_one_fault()
{
    delta_io_transport_t fake = {};
    fake.script[0] = 16;
    fake.script[1] = -1;
    fake.script[2] = 16;
    fake.script_length = 3;

    uint64_t discarded = 0, faults = 0;
    delta_io_stop_and_discard( &delta_io_operations_single, &fake, &discarded, &faults );

    delta_io_check( faults == 1, "a refused read at the exit's discard was not counted as exactly one transport fault" );
    delta_io_check( fake.receive_calls == 2, "the discard did not stop at the refused read" );
    delta_io_check( discarded == 1, "the discard did not count exactly the one datagram read before the refused read" );
}
REGISTER_UNIT_TEST( test_delta_client_io_discard_counts_a_refused_read_as_one_fault );

// A sender that keeps the socket full cannot hold the exit: the batch discard
// reads exactly ROCKETNET_CLIENT_IO_DISCARD_MAX datagrams, a full batch at a
// time, and a full batch (got == ROCKETNET_CLIENT_IO_DISCARD_BATCH) is no fault.
// mutation gap: delta sdk/rocketnet_client_io.h:797 relational <= (discarded <= ROCKETNET_CLIENT_IO_DISCARD_MAX)
// mutation gap: delta sdk/rocketnet_client_io.h:806 relational >= (got >= ROCKETNET_CLIENT_IO_DISCARD_BATCH)

static void test_delta_client_io_discard_of_a_full_socket_stops_at_the_bound()
{
    static_assert( ROCKETNET_CLIENT_IO_DISCARD_MAX % ROCKETNET_CLIENT_IO_DISCARD_BATCH == 0, "the bound is whole batches" );

    delta_io_transport_t fake = {};
    fake.endless_batch = true;
    fake.batch_answer = ROCKETNET_CLIENT_IO_DISCARD_BATCH;

    uint64_t discarded = 0, faults = 0;
    delta_io_stop_and_discard( &delta_io_operations_batch, &fake, &discarded, &faults );

    delta_io_check( discarded == ROCKETNET_CLIENT_IO_DISCARD_MAX, "the discard of a socket kept full did not stop at exactly ROCKETNET_CLIENT_IO_DISCARD_MAX datagrams" );
    delta_io_check( fake.receive_batch_calls == ROCKETNET_CLIENT_IO_DISCARD_MAX / ROCKETNET_CLIENT_IO_DISCARD_BATCH,
                    "the discard of a socket kept full read past its bound" );
    delta_io_check( faults == 0, "a full batch (got == ROCKETNET_CLIENT_IO_DISCARD_BATCH) was counted as a transport fault" );
    delta_io_check( fake.receive_calls == 0, "a batch transport's discard read with the single receive" );
}
REGISTER_UNIT_TEST( test_delta_client_io_discard_of_a_full_socket_stops_at_the_bound );

// A batch answer past the batch is the transport's fault: counted once per
// read, and only the batch is counted as discarded.

static void test_delta_client_io_discard_counts_an_overlong_batch_as_a_fault()
{
    delta_io_transport_t fake = {};
    fake.endless_batch = true;
    fake.batch_answer = ROCKETNET_CLIENT_IO_DISCARD_BATCH + 1;

    uint64_t discarded = 0, faults = 0;
    delta_io_stop_and_discard( &delta_io_operations_batch, &fake, &discarded, &faults );

    delta_io_check( discarded == ROCKETNET_CLIENT_IO_DISCARD_MAX, "an overlong batch answer was not cut to the batch at the exit's discard" );
    delta_io_check( faults == ROCKETNET_CLIENT_IO_DISCARD_MAX / ROCKETNET_CLIENT_IO_DISCARD_BATCH,
                    "an overlong batch answer was not counted as one transport fault per read" );
}
REGISTER_UNIT_TEST( test_delta_client_io_discard_counts_an_overlong_batch_as_a_fault );
