/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Transport results the transports trust: a batch count, a byte count, a missing
// wait, the loopback destination, and the GRO cmsg. No socket, no thread, no
// wall clock. The clock argument is 1000.0. This file is compiled into the
// loopback translation unit, which includes the server transport.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "client_io_rig.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"

#include <stdio.h>
#include <string.h>

// The GRO case reads a Linux recvmmsg header: the GSO server, and with it
// rocketnet_server_gso_gro_segment_bytes, is built on Linux only.
#if defined( __linux__ )
#include <sys/socket.h>
#include <netinet/in.h>
#include "loopback_hub.h"

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

extern int rocketnet_server_gso_gro_segment_bytes( struct msghdr * header, int message_bytes, uint64_t * faults );
#endif // #if defined(__linux__)

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

struct transport_results_clock_t
{
    double now;
};

static double transport_results_time( void * context )
{
    return ( (transport_results_clock_t *) context )->now;
}

struct transport_results_lie_t
{
    int mode;
    int calls;
    int max_seen;
};

static void * transport_results_lie_create( void * context, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * )
{
    return context;
}

static void transport_results_lie_destroy( void * ) {}
static void transport_results_lie_send( void *, const rocketnet_address_t *, const uint8_t *, int ) {}

static int transport_results_lie_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    transport_results_lie_t * lie = (transport_results_lie_t *) transport;
    lie->calls++;
    lie->max_seen = capacity;
    if ( lie->calls > 1 )
        return 0;
    memset( from, 0, sizeof( *from ) );
    from->type = ROCKETNET_ADDRESS_IPV4;
    from->port = 1;
    if ( lie->mode == 2 )
        return -1;
    if ( lie->mode == 3 )
        return ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES + 1;
    if ( data && capacity > 0 )
    {
        data[0] = 0x21;
    }

    return 1;
}

static int transport_results_lie_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    transport_results_lie_t * lie = (transport_results_lie_t *) transport;
    lie->calls++;
    lie->max_seen = max_packets;
    if ( lie->calls > 1 )
        return 0;

    if ( lie->mode == 1 )
    {
        for ( int i = 0; i < max_packets; i++ )
        {
            memset( &from[i], 0, sizeof( from[i] ) );
            from[i].type = ROCKETNET_ADDRESS_IPV4;
            from[i].port = 1;
            packet_bytes[i] = ( i == 0 ) ? 4 : 0;
            if ( i == 0 )
            {
                memset( packet_data, 0x11, 4 );
            }
        }

        return max_packets + 1;
    }

    if ( lie->mode == 6 && max_packets >= 3 )
    {
        memset( from, 0, 3 * sizeof( *from ) );
        memset( packet_data, 0xcc, (size_t) packet_stride * 3 );
        for ( int i = 0; i < 3; i++ )
        {
            from[i].type = ROCKETNET_ADDRESS_IPV4;
        }
        packet_bytes[0] = -1;
        packet_bytes[1] = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES;
        packet_bytes[2] = 4;
        memset( packet_data + packet_stride, 0xa5, (size_t) packet_stride );
        memset( packet_data + 2 * packet_stride, 0x22, 4 );
        return 3;
    }

    if ( lie->mode == 7 && max_packets >= 2 )
    {
        memset( from, 0, 2 * sizeof( *from ) );
        from[0].type = from[1].type = ROCKETNET_ADDRESS_IPV4;
        packet_bytes[0] = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES + 1;
        packet_bytes[1] = 4;
        memset( packet_data + packet_stride, 0x22, 4 );
        return 2;
    }

    if ( lie->mode == 4 && max_packets >= 2 )
    {
        memset( &from[0], 0, sizeof( from[0] ) );
        memset( &from[1], 0, sizeof( from[1] ) );
        from[0].type = from[1].type = ROCKETNET_ADDRESS_IPV4;
        from[0].port = from[1].port = 1;
        packet_bytes[0] = -1;
        packet_bytes[1] = 4;
        memset( packet_data + packet_stride, 0x22, 4 );
        return 2;
    }

    return 0;
}

static rocketnet_client_io_rig_t * transport_results_rig = NULL;

static void transport_results_open_io( transport_results_lie_t * lie, rocketnet_client_transport_io_operations_t * ops,
                                       rocketnet_client_transport_io_t * transport, rocketnet_client_io_t * io,
                                       rocketnet_client_receive_buffer_t ** buffers, transport_results_clock_t * clock, bool batch )
{
    *ops = rocketnet_fake_client_operations_base;
    ops->create = transport_results_lie_create;
    ops->destroy = transport_results_lie_destroy;
    ops->send = transport_results_lie_send;
    ops->receive = transport_results_lie_receive;
    ops->receive_batch = batch ? transport_results_lie_receive_batch : NULL;
    ops->wait = NULL;
    ops->wake = NULL;

    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( ops, lie, &config, &bind, transport ) );

    // every caller passes io value-initialised: zeroed, as init expects

    transport_results_rig = rocketnet_client_io_rig_create();
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
    {
        buffers[i] = transport_results_rig->buffers[i];
    }
    clock->now = 1000.0;
    rocketnet_client_io_rig_init( transport_results_rig, io, transport, transport_results_time, clock );
}

static void transport_results_close_io( rocketnet_client_transport_io_t * transport, rocketnet_client_receive_buffer_t ** buffers )
{
    rocketnet_client_transport_io_close( transport );
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_BUFFERS; i++ )
    {
        buffers[i] = NULL;
    }
    rocketnet_client_io_rig_destroy( transport_results_rig );
    transport_results_rig = NULL;
}

// A receive_batch that returns one past the count it was asked for does not
// index the stack arrays past that count. The extra return is one fault. The
// slots it was asked to fill are kept, including a zero-length slot.

static void test_transport_results_client_batch_over_count_is_a_fault()
{
    transport_results_lie_t lie = {};
    lie.mode = 1;

    rocketnet_client_transport_io_operations_t ops;
    rocketnet_client_transport_io_t transport = {};
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    transport_results_clock_t clock;
    rocketnet_client_io_t io = {};
    transport_results_open_io( &lie, &ops, &transport, &io, buffers, &clock, true );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.transport_faults == 1 );

    rocketnet_client_receive_buffer_t * received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == lie.max_seen );
    rocketnet_unit_check( received->packet_bytes[0] == 4 );
    rocketnet_unit_check( received->packet_data[0][0] == 0x11 );

    transport_results_close_io( &transport, buffers );
}
REGISTER_UNIT_TEST( test_transport_results_client_batch_over_count_is_a_fault );

static void test_transport_results_client_negative_byte_count_is_a_fault()
{
    transport_results_lie_t lie = {};
    lie.mode = 2;

    rocketnet_client_transport_io_operations_t ops;
    rocketnet_client_transport_io_t transport = {};
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    transport_results_clock_t clock;
    rocketnet_client_io_t io = {};
    transport_results_open_io( &lie, &ops, &transport, &io, buffers, &clock, false );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( io.transport_faults == 1 );
    rocketnet_unit_check( rocketnet_client_io_expose( &io ) == NULL );

    transport_results_close_io( &transport, buffers );
}
REGISTER_UNIT_TEST( test_transport_results_client_negative_byte_count_is_a_fault );

static void test_transport_results_client_oversized_byte_count_is_a_fault()
{
    transport_results_lie_t lie = {};
    lie.mode = 3;

    rocketnet_client_transport_io_operations_t ops;
    rocketnet_client_transport_io_t transport = {};
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    transport_results_clock_t clock;
    rocketnet_client_io_t io = {};
    transport_results_open_io( &lie, &ops, &transport, &io, buffers, &clock, false );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( io.transport_faults == 1 );
    rocketnet_unit_check( rocketnet_client_io_expose( &io ) == NULL );
    rocketnet_unit_check( lie.max_seen == ROCKETNET_RECEIVE_PACKET_BYTES );

    transport_results_close_io( &transport, buffers );
}
REGISTER_UNIT_TEST( test_transport_results_client_oversized_byte_count_is_a_fault );

// A negative byte count in a batch is refused and counted. The later slot
// whose count fits the stride is kept.

static void test_transport_results_client_batch_refuses_a_bad_byte_count()
{
    transport_results_lie_t lie = {};
    lie.mode = 4;

    rocketnet_client_transport_io_operations_t ops;
    rocketnet_client_transport_io_t transport = {};
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    transport_results_clock_t clock;
    rocketnet_client_io_t io = {};
    transport_results_open_io( &lie, &ops, &transport, &io, buffers, &clock, true );

    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.transport_faults == 1 );

    rocketnet_client_receive_buffer_t * received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 1 );
    rocketnet_unit_check( received->packet_bytes[0] == 4 );
    rocketnet_unit_check( received->packet_data[0][0] == 0x22 );

    transport_results_close_io( &transport, buffers );
}
REGISTER_UNIT_TEST( test_transport_results_client_batch_refuses_a_bad_byte_count );

// A truncated slot after a refused slot keeps its original length, but
// compaction must not read or overwrite the following slot's payload.

static void test_transport_results_client_truncated_batch_copies_only_prefix()
{
    transport_results_lie_t lie = {};
    lie.mode = 6;

    rocketnet_client_transport_io_operations_t ops;
    rocketnet_client_transport_io_t transport = {};
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    transport_results_clock_t clock;
    rocketnet_client_io_t io = {};
    transport_results_open_io( &lie, &ops, &transport, &io, buffers, &clock, true );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.transport_faults == 1 );

    rocketnet_client_receive_buffer_t * received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 2 );
    rocketnet_unit_check( received->packet_bytes[0] == ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES );
    for ( int i = 0; i < ROCKETNET_FRAME_SIZE; i++ )
    {
        rocketnet_unit_check( received->packet_data[0][i] == 0xa5 );
    }
    rocketnet_unit_check( received->packet_bytes[1] == 4 );
    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_unit_check( received->packet_data[1][i] == 0x22 );
    }
    transport_results_close_io( &transport, buffers );
}
REGISTER_UNIT_TEST( test_transport_results_client_truncated_batch_copies_only_prefix );

static void test_transport_results_client_batch_impossible_length_is_a_fault()
{
    transport_results_lie_t lie = {};
    lie.mode = 7;

    rocketnet_client_transport_io_operations_t ops;
    rocketnet_client_transport_io_t transport = {};
    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
    transport_results_clock_t clock;
    rocketnet_client_io_t io = {};
    transport_results_open_io( &lie, &ops, &transport, &io, buffers, &clock, true );
    rocketnet_unit_check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    rocketnet_unit_check( io.transport_faults == 1 );

    rocketnet_client_receive_buffer_t * received = rocketnet_client_io_expose( &io );
    rocketnet_unit_check( received != NULL );
    rocketnet_unit_check( received->num_packets == 1 );
    rocketnet_unit_check( received->packet_bytes[0] == 4 );
    rocketnet_unit_check( received->packet_data[0][0] == 0x22 );
    transport_results_close_io( &transport, buffers );
}
REGISTER_UNIT_TEST( test_transport_results_client_batch_impossible_length_is_a_fault );

// A datagram past the UDP envelope reads PREFILTER_TOO_LARGE, on the batch
// path and the per-packet path. The step that refuses it leaves the main
// thread's counter alone; the fold at update adds it, once.

static void test_transport_results_client_oversize_counts_at_the_fold()
{
    for ( int batch = 0; batch < 2; batch++ )
    {
        transport_results_lie_t lie = {};
        lie.mode = batch ? 7 : 3;

        rocketnet_client_transport_io_operations_t ops;
        rocketnet_client_transport_io_t transport = {};
        rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS] = {};
        transport_results_clock_t clock;
        rocketnet_client_io_t io = {};
        transport_results_open_io( &lie, &ops, &transport, &io, buffers, &clock, batch != 0 );

        uint64_t too_large = 0;
        io.prefilter_too_large = &too_large;

        rocketnet_client_io_step( &io, false );
        rocketnet_unit_check( io.transport_faults == 1 );
        rocketnet_unit_check( too_large == 0 );

        rocketnet_client_io_expose( &io );
        rocketnet_client_io_fold_counters( &io );
        rocketnet_unit_check( too_large == 1 );

        rocketnet_client_io_fold_counters( &io );
        rocketnet_unit_check( too_large == 1 );

        transport_results_close_io( &transport, buffers );
    }
}
REGISTER_UNIT_TEST( test_transport_results_client_oversize_counts_at_the_fold );

// A transport open allows wait and wake both absent. A blocked idle step does
// not call wait.

static void test_transport_results_client_without_wait_skips_the_call()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( &rocketnet_fake_client_operations_base, &fake, &config, &bind, &transport ) );
    rocketnet_unit_check( transport.operations->wait == NULL );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    transport_results_clock_t clock = { 1000.0 };
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, transport_results_time, &clock );

    rocketnet_unit_check( rocketnet_client_io_step( &io, true ) == ROCKETNET_CLIENT_IO_IDLE );
    rocketnet_unit_check( fake.wait_calls == 0 );
    rocketnet_unit_check( io.transport_faults == 0 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_transport_results_client_without_wait_skips_the_call );

struct transport_results_server_lie_t
{
    int mode;
    int calls;
    int asked;
    int workers_exited;
    int send_calls;
};

static void transport_results_server_destroy( void * ) {}
static void transport_results_server_stop( void * ) {}
static bool transport_results_server_stopped( void * ) { return true; }
static void transport_results_server_send_batch( void *, const rocketnet_platform_send_t *, int ) {}

static int transport_results_server_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity )
{
    transport_results_server_lie_t * lie = (transport_results_server_lie_t *) transport;
    lie->calls++;
    lie->asked = capacity;
    if ( lie->calls > 1 )
        return 0;
    if ( lie->mode == 1 )
        return -1;

    if ( lie->mode == 2 )
    {
        for ( int i = 0; i < capacity; i++ )
        {
            memset( &from[i], 0, sizeof( from[i] ) );
            from[i].type = ROCKETNET_ADDRESS_IPV4;
            from[i].port = 9;
            bytes[i] = -1;
        }

        return capacity + 1;
    }

    memset( &from[0], 0, sizeof( from[0] ) );
    from[0].type = ROCKETNET_ADDRESS_IPV4;
    from[0].port = 9;

    if ( lie->mode == 3 )
    {
        bytes[0] = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES + 1;
        return 1;
    }

    if ( lie->mode == 6 )
    {
        bytes[0] = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES;
        return 1;
    }

    if ( lie->mode == 4 )
    {
        bytes[0] = -5;
        return 1;
    }

    bytes[0] = 4;
    if ( stride >= 4 )
    {
        memset( data, 0x33, 4 );
    }

    return 1;
}

static rocketnet_server_transport_t * transport_results_make_server( transport_results_server_lie_t * lie, rocketnet_server_transport_io_operations_t * operations )
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config_current( &config );
    config.max_clients = 1;
    config.protocol_id = 1;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );

    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    if ( !server )
    {
        printf( "create_ex status=%d\n", create_status );
    }
    rocketnet_unit_check( server );
    rn_test_server_io_stop_and_close( &server->io );
    memset( operations, 0, sizeof( *operations ) );
    operations->destroy = transport_results_server_destroy;
    operations->stop = transport_results_server_stop;
    operations->stopped = transport_results_server_stopped;
    operations->send_batch = transport_results_server_send_batch;
    operations->receive_batch = transport_results_server_receive_batch;
    server->io.instance = lie;
    server->io.operations = operations;
    rocketnet_server_transport_set_test_timestamp( server, 1000 );

    return server;
}

static void test_transport_results_server_receive_checks_counts()
{
    transport_results_server_lie_t lie = {};
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = transport_results_make_server( &lie, &operations );

    lie.mode = 1;
    lie.calls = 0;

    const uint64_t received_before = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( server->transport_faults == 1 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before );

    const uint64_t faults_after_negative = server->transport_faults;
    lie.mode = 2;
    lie.calls = 0;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( lie.asked > 0 );
    rocketnet_unit_check( server->transport_faults == faults_after_negative + (uint64_t) lie.asked + 1 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before );

    const uint64_t faults_after_over = server->transport_faults;
    lie.mode = 3;
    lie.calls = 0;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( server->transport_faults == faults_after_over + 1 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before );

    lie.mode = 4;
    lie.calls = 0;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( server->transport_faults == faults_after_over + 2 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before );

    lie.mode = 5;
    lie.calls = 0;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( server->transport_faults == faults_after_over + 2 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before + 1 );

    rn_test_server_transport_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_transport_results_server_receive_checks_counts );

// A staged transport's receive view is checked like a batch count: a NULL view,
// a count past the view's capacity, and a byte count that is negative or past
// the receive ceiling (the staged transports drop larger datagrams themselves)
// are transport faults. None crashes update, reads past the view, or wraps
// bytes_received.

struct transport_results_staged_lie_t
{
    int mode;
    rocketnet_server_socket_process_packets_t view;
    rocketnet_address_t from[3];
    int packet_bytes[3];
    uint8_t * packet_data[3];
    uint8_t data[3][ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES + 2];
};

static uint8_t * transport_results_staged_start( void *, const rocketnet_address_t *, uint64_t * ) { return NULL; }
static void transport_results_staged_finish( void *, uint64_t, uint8_t *, int ) {}
static void transport_results_staged_abort( void *, uint64_t, uint8_t * ) {}
static void transport_results_staged_flush( void * ) {}

static rocketnet_server_socket_process_packets_t * transport_results_staged_receive( void * transport )
{
    transport_results_staged_lie_t * lie = (transport_results_staged_lie_t *) transport;
    if ( lie->mode == 0 )
        return NULL;

    lie->view.from = lie->from;
    lie->view.packet_bytes = lie->packet_bytes;
    lie->view.packet_data = lie->packet_data;
    for ( int i = 0; i < 3; i++ )
    {
        memset( &lie->from[i], 0, sizeof( lie->from[i] ) );
        lie->from[i].type = ROCKETNET_ADDRESS_IPV4;
        lie->from[i].port = 9;
        lie->packet_bytes[i] = 4;
        lie->packet_data[i] = lie->data[i];
        memset( lie->data[i], 0x33, 4 );
    }

    if ( lie->mode == 1 )
    {
        // three packets in a view of two
        lie->view.capacity = 2;
        lie->view.num_packets = 3;
    }
    else
    {
        lie->view.capacity = 3;
        lie->view.num_packets = 3;
        lie->packet_bytes[0] = -5;
        lie->packet_bytes[1] = ( lie->mode == 2 ) ? ROCKETNET_RECEIVE_CEILING + 1 : ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES + 1;
    }

    lie->mode = 0;
    return &lie->view;
}

static uint64_t transport_results_bytes_received( const rocketnet_server_transport_t * server )
{
    uint64_t bytes = 0;
    for ( int i = 0; i < ROCKETNET_NUM_SERVER_STATS_FRAMES; i++ )
        bytes += server->stats_frames[i].bytes_received;
    return bytes;
}

static void test_transport_results_server_staged_receive_checks_the_view()
{
    transport_results_server_lie_t unused = {};
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = transport_results_make_server( &unused, &operations );

    static transport_results_staged_lie_t lie;
    memset( &lie, 0, sizeof( lie ) );
    operations.flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
    operations.start_packet = transport_results_staged_start;
    operations.finish_packet = transport_results_staged_finish;
    operations.abort_packet = transport_results_staged_abort;
    operations.flush = transport_results_staged_flush;
    operations.receive = transport_results_staged_receive;
    server->io.instance = &lie;

    const uint64_t received_before = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    const uint64_t bytes_before = transport_results_bytes_received( server );

    // a count past capacity: one fault, only the capacity's packets are read

    lie.mode = 1;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( server->transport_faults == 1 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before + 2 );

    // a negative and an oversize byte count: one fault each, skipped; the valid
    // third packet is read, at the receive ceiling and past the UDP envelope

    for ( int mode = 2; mode <= 3; mode++ )
    {
        const uint64_t faults = server->transport_faults;
        const uint64_t received = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
        lie.mode = mode;
        rocketnet_server_transport_update( server, 1000.0 );
        rocketnet_unit_check( server->transport_faults == faults + 2 );
        rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received + 1 );
    }

    // a NULL view: one fault, nothing read

    const uint64_t faults_before_null = server->transport_faults;
    const uint64_t received_before_null = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    lie.mode = 0;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( server->transport_faults == faults_before_null + 1 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before_null );

    // only the four valid packets were counted, so bytes_received did not wrap

    const uint64_t bytes_after = transport_results_bytes_received( server );
    rocketnet_unit_check( bytes_after >= bytes_before && bytes_after - bytes_before < 1024 );

    rn_test_server_transport_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_transport_results_server_staged_receive_checks_the_view );

// proofs/tla/ServerSession.tla: NoReceiveUnlessRunning and NoAdmitUnlessRunning.
// A stopping or stopped server reads no packet and admits no client.
// rocketnet_server_update returns while !server->running
// (sdk/rocketnet_api.cpp:1797). rocketnet_server_transport_update returns
// while the transport is not RUNNING (sdk/rocketnet_server_transport.cpp:2388).

static bool transport_results_stopping_stopped( void * transport )
{
    transport_results_server_lie_t * lie = (transport_results_server_lie_t *) transport;
    return lie->workers_exited != 0;
}

static int transport_results_stopping_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int )
{
    transport_results_server_lie_t * lie = (transport_results_server_lie_t *) transport;
    lie->calls++;
    memset( &from[0], 0, sizeof( from[0] ) );
    from[0].type = ROCKETNET_ADDRESS_IPV4;
    from[0].port = 9;
    bytes[0] = 4;
    if ( stride >= 4 )
    {
        memset( data, 0x33, 4 );
    }

    return 1;
}

static void transport_results_stopping_send( void * transport, const rocketnet_platform_send_t *, int )
{
    transport_results_server_lie_t * lie = (transport_results_server_lie_t *) transport;
    lie->send_calls++;
}

static rocketnet_server_transport_t * transport_results_stopping_server( transport_results_server_lie_t * lie, rocketnet_server_transport_io_operations_t * operations )
{
    rocketnet_server_transport_t * server = transport_results_make_server( lie, operations );
    operations->stopped = transport_results_stopping_stopped;
    operations->receive_batch = transport_results_stopping_receive;
    operations->send_batch = transport_results_stopping_send;
    lie->workers_exited = 0;
    lie->calls = 0;
    lie->send_calls = 0;

    return server;
}

static void transport_results_stopping_release( rocketnet_server_transport_t * server, transport_results_server_lie_t * lie )
{
    lie->workers_exited = 1;
    rn_test_server_transport_stop_and_destroy( server );
}

static void test_server_stopping_reads_no_packet()
{
    transport_results_server_lie_t lie = {};
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = transport_results_stopping_server( &lie, &operations );
    const uint64_t received_before = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_RUNNING );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_server_transport_update( server, 1001.0 );
    rocketnet_unit_check( lie.calls == 0 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before );
    rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPING );
    transport_results_stopping_release( server, &lie );
}
REGISTER_UNIT_TEST( test_server_stopping_reads_no_packet );

static void test_server_stopped_reads_no_packet()
{
    transport_results_server_lie_t lie = {};
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = transport_results_stopping_server( &lie, &operations );
    const uint64_t received_before = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( lie.calls == 0 );
    rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPING );
    lie.workers_exited = 1;
    rocketnet_server_transport_update( server, 1002.0 );
    rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_server_transport_update( server, 1003.0 );
    rocketnet_unit_check( lie.calls == 0 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before );
    rn_test_server_transport_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_server_stopped_reads_no_packet );

static void test_server_stopping_admits_no_client()
{
    transport_results_server_lie_t lie = {};
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = transport_results_stopping_server( &lie, &operations );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_server_transport_update( server, 1001.0 );
    rocketnet_unit_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPING );
    rocketnet_unit_check( rocketnet_server_transport_is_client_connected( server, 0 ) == 0 );
    rocketnet_unit_check( lie.send_calls == 0 );
    rocketnet_unit_check( lie.calls == 0 );
    transport_results_stopping_release( server, &lie );
}
REGISTER_UNIT_TEST( test_server_stopping_admits_no_client );

static void test_transport_results_server_truncated_datagram_refused()
{
    transport_results_server_lie_t lie = {};
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = transport_results_make_server( &lie, &operations );
    const uint64_t before = server->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];
    const uint64_t received_before = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    lie.mode = 6;
    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_unit_check( server->transport_faults == 0 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == before + 1 );
    rocketnet_unit_check( server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == received_before + 1 );
    rn_test_server_transport_stop_and_destroy( server );
}
REGISTER_UNIT_TEST( test_transport_results_server_truncated_datagram_refused );

static void transport_results_set_ipv4( rocketnet_address_t * address, int a, int b, int c, int d, uint16_t port )
{
    memset( address, 0, sizeof( *address ) );
    address->type = ROCKETNET_ADDRESS_IPV4;
    address->data.ipv4[0] = (uint8_t) a;
    address->data.ipv4[1] = (uint8_t) b;
    address->data.ipv4[2] = (uint8_t) c;
    address->data.ipv4[3] = (uint8_t) d;
    address->port = port;
}

// A datagram reaches the endpoint bound to its destination, or a wildcard of
// that family. 10.0.0.1:p does not reach an endpoint bound to 127.0.0.1:p.

static void test_loopback_transport_io_results_matches_destination_or_wildcard()
{
    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.loopback_hub = rocketnet_test_loopback_hub();

    const rocketnet_server_transport_io_config_t server_setup = { "loopback", &server_config, NULL, NULL };
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", &client_config, NULL };

    rocketnet_address_t server_address, client_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );

    rocketnet_server_transport_io_t server = {};
    rocketnet_client_transport_io_t client = {};
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    const uint8_t data[] = { 9, 8, 7, 6 };
    rocketnet_address_t other;
    transport_results_set_ipv4( &other, 10, 0, 0, 1, server_address.port );
    client.operations->send( client.instance, &other, data, (int) sizeof( data ) );

    rocketnet_address_t from;
    uint8_t buf[16];
    int bytes = 0;
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, buf, 16, &bytes, 1 ) == 0 );

    rocketnet_address_t exact = server_address;
    client.operations->send( client.instance, &exact, data, (int) sizeof( data ) );
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, buf, 16, &bytes, 1 ) == 1 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );

    rn_test_server_io_stop_and_close( &server );
    rocketnet_client_transport_io_close( &client );

    rocketnet_unit_check( rocketnet_address_parse( &server_address, "0.0.0.0:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_unit_check( rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_address, &client ) );
    transport_results_set_ipv4( &other, 10, 0, 0, 1, server_address.port );
    client.operations->send( client.instance, &other, data, (int) sizeof( data ) );
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, buf, 16, &bytes, 1 ) == 1 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );

    rn_test_server_io_stop_and_close( &server );
    rocketnet_client_transport_io_close( &client );
}
REGISTER_UNIT_TEST( test_loopback_transport_io_results_matches_destination_or_wildcard );

#if defined( __linux__ )

static void transport_results_fill_gro( struct msghdr * header, char * control, size_t control_bytes, int stride, bool valid_len )
{
    memset( header, 0, sizeof( *header ) );
    memset( control, 0, control_bytes );
    header->msg_control = control;
    header->msg_controllen = (socklen_t) control_bytes;

    struct cmsghdr * cmsg = CMSG_FIRSTHDR( header );
    rocketnet_unit_check( cmsg != NULL );
    cmsg->cmsg_level = IPPROTO_UDP;
    cmsg->cmsg_type = UDP_GRO;
    cmsg->cmsg_len = valid_len ? CMSG_LEN( sizeof( int ) ) : (socklen_t) sizeof( struct cmsghdr );
    if ( valid_len )
    {
        memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
    }
}

// The GRO cmsg is read only when the message is not MSG_CTRUNC and cmsg_len
// holds an int. Anything else is one fault and one datagram. The header is
// built in memory; no socket.

static void test_gso_transport_io_results_gro_cmsg_is_checked()
{
    const int message_bytes = 300;
    const int stride = 100;
    alignas( 16 ) char control[CMSG_SPACE( sizeof( int ) )];

    struct msghdr header;
    uint64_t faults = 0;

    transport_results_fill_gro( &header, control, sizeof( control ), stride, true );
    faults = 0;
    rocketnet_unit_check( rocketnet_server_gso_gro_segment_bytes( &header, message_bytes, &faults ) == stride );
    rocketnet_unit_check( faults == 0 );

    transport_results_fill_gro( &header, control, sizeof( control ), stride, true );
    header.msg_flags = MSG_CTRUNC;
    faults = 0;
    rocketnet_unit_check( rocketnet_server_gso_gro_segment_bytes( &header, message_bytes, &faults ) == message_bytes );
    rocketnet_unit_check( faults == 1 );

    transport_results_fill_gro( &header, control, sizeof( control ), stride, false );
    faults = 0;
    rocketnet_unit_check( rocketnet_server_gso_gro_segment_bytes( &header, message_bytes, &faults ) == message_bytes );
    rocketnet_unit_check( faults == 1 );

    memset( &header, 0, sizeof( header ) );
    faults = 0;
    rocketnet_unit_check( rocketnet_server_gso_gro_segment_bytes( &header, message_bytes, &faults ) == message_bytes );
    rocketnet_unit_check( faults == 0 );
}
REGISTER_UNIT_TEST( test_gso_transport_io_results_gro_cmsg_is_checked );

#endif // #if defined(__linux__)
