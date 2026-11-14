/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's portable control buffer at its edges (sdk/rocketnet_server_
// transport.cpp, rocketnet_server_reserve_send_frame): an update that commits
// more control frames than the buffer holds flushes it when it is exactly
// full, before the next frame is reserved, so that frame goes into the first
// slot again and never past the end; an update that fills it to exactly its
// capacity sends it whole at the update's flush. The server runs on the
// in-memory loopback transport with its slots seated by the unit seam (no
// handshake), and its send_batch is swapped for a recorder: no socket, no
// thread, and the clock is an argument. It runs at 1.0, as test_unit_portable_tx.cpp does: the seam
// seats its slots with no receive yet (last_receive_time 0) and a 1000 s timeout, so an update at
// 1000.0 times them all out (server_protocol.cpp, the timeout check in the update).
//
// The buffer's arrays are carved from the transport's one allocation, so a
// write one slot past one of them lands in the next carve, where no guard
// allocator or sanitizer sees it; the recorder sees it instead, as a batch
// larger than the buffer, a frame not at its wire length, a frame repeated
// byte for byte, or a frame missing from a count fixed in advance.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_driver.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_server_protocol.h"
#include "unit_shutdown.h"
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

bool rocketnet_server_transport_seat_clients_for_test( struct rocketnet_server_transport_t * server, int clients );

#define SEND_EDGE_CLIENTS 512
#define SEND_EDGE_MAX_SENDS 2048
#define SEND_EDGE_MAX_CALLS 16
#define SEND_EDGE_FRAME_BYTES 64

// Each frame is kept whole (every one here is far below 64 bytes; a longer
// one fails the exact-length check before its bytes are compared).
struct send_edge_record_t
{
    int calls;
    int call_sends[SEND_EDGE_MAX_CALLS];
    int count;
    uint16_t port[SEND_EDGE_MAX_SENDS];
    int bytes[SEND_EDGE_MAX_SENDS];
    uint8_t type[SEND_EDGE_MAX_SENDS];
    uint8_t data[SEND_EDGE_MAX_SENDS][SEND_EDGE_FRAME_BYTES];
};

static send_edge_record_t * send_edge_record;

static void send_edge_send_batch( void * instance, const rocketnet_platform_send_t * sends, int num_sends )
{
    (void) instance;
    send_edge_record_t * record = send_edge_record;
    rocketnet_unit_check( record->calls < SEND_EDGE_MAX_CALLS );
    if ( record->calls < SEND_EDGE_MAX_CALLS )
    {
        record->call_sends[record->calls] = num_sends;
    }
    record->calls++;
    for ( int i = 0; i < num_sends && record->count < SEND_EDGE_MAX_SENDS; i++ )
    {
        record->port[record->count] = sends[i].to->port;
        record->bytes[record->count] = sends[i].packet_bytes;
        record->type[record->count] = sends[i].packet_bytes > 0 ? sends[i].packet_data[0] : 0xFF;
        if ( sends[i].packet_bytes > 0 )
        {
            const int kept = sends[i].packet_bytes < SEND_EDGE_FRAME_BYTES ? sends[i].packet_bytes : SEND_EDGE_FRAME_BYTES;
            memcpy( record->data[record->count], sends[i].packet_data, (size_t) kept );
        }
        record->count++;
    }
}

static struct rocketnet_server_transport_t * send_edge_server( struct rocketnet_server_transport_io_operations_t * operations,
                                                               const struct rocketnet_server_transport_io_operations_t ** loopback,
                                                               void ** loopback_instance )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = SEND_EDGE_CLIENTS;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &config, sizeof( config ) );
    rocketnet_unit_check( server != NULL );
    if ( !server )
    {
        return NULL;
    }
    rocketnet_unit_check( rocketnet_server_transport_seat_clients_for_test( server, SEND_EDGE_CLIENTS ) );

    // the loopback's own table, copied with send_batch recorded

    const struct rocketnet_server_transport_io_operations_t * swapped = operations;
    void * instance = NULL;
    memset( operations, 0, sizeof( *operations ) );
    rocketnet_server_transport_swap_io_for_test( server, &swapped, &instance );
    *loopback = swapped;
    *loopback_instance = instance;
    *operations = *swapped;
    operations->send_batch = send_edge_send_batch;
    swapped = operations;
    rocketnet_server_transport_swap_io_for_test( server, &swapped, &instance );
    rocketnet_unit_check( instance == NULL );
    return server;
}

static void send_edge_restore( struct rocketnet_server_transport_t * server,
                               const struct rocketnet_server_transport_io_operations_t * loopback, void * loopback_instance )
{
    const struct rocketnet_server_transport_io_operations_t * swapped = loopback;
    void * instance = loopback_instance;
    rocketnet_server_transport_swap_io_for_test( server, &swapped, &instance );
    rn_test_server_transport_stop_and_destroy( server );
}

// Every frame a client is owed went out once, whole: keep-alives to each
// seated port but the disconnected one, the disconnect burst to that one,
// no batch larger than the buffer. The lengths are the wire's own: no seated
// client has sent anything, so no frame carries an ack and each is its type's
// minimum (rocketnet_wire.h). Each frame is sealed at its own sequence, so no
// two are the same bytes: a frame written twice, or a slot written over with
// another frame's bytes, shows as a repeat.

static void send_edge_check( const send_edge_record_t * record, int capacity, int disconnected, int burst )
{
    const int keep_alive_bytes = ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
    const int disconnect_bytes = ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES;
    int disconnects = 0;
    int * seen = (int *) calloc( SEND_EDGE_CLIENTS, sizeof( int ) );
    rocketnet_unit_check( seen != NULL );
    if ( !seen )
    {
        return;
    }

    for ( int i = 0; i < record->count; i++ )
    {
        const int client = (int) record->port[i] - 1000;
        rocketnet_unit_check( client >= 0 && client < SEND_EDGE_CLIENTS );
        rocketnet_unit_check( record->bytes[i] > 0 && record->bytes[i] <= ROCKETNET_RECEIVE_CEILING );
        if ( client < 0 || client >= SEND_EDGE_CLIENTS )
        {
            continue;
        }
        if ( record->type[i] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET )
        {
            rocketnet_unit_check( client != disconnected );
            rocketnet_unit_check( record->bytes[i] == keep_alive_bytes );
            seen[client]++;
        }
        else
        {
            rocketnet_unit_check( record->type[i] == ROCKETNET_CONNECTION_DISCONNECT_PACKET );
            rocketnet_unit_check( client == disconnected );
            rocketnet_unit_check( record->bytes[i] == disconnect_bytes );
            disconnects++;
        }
    }

    for ( int i = 0; i < record->count; i++ )
    {
        for ( int j = i + 1; j < record->count; j++ )
        {
            if ( record->bytes[i] == record->bytes[j] && record->bytes[i] <= SEND_EDGE_FRAME_BYTES )
            {
                rocketnet_unit_check( memcmp( record->data[i], record->data[j], (size_t) record->bytes[i] ) != 0 );
            }
        }
    }

    for ( int client = 0; client < SEND_EDGE_CLIENTS; client++ )
    {
        rocketnet_unit_check( seen[client] == ( client == disconnected ? 0 : 1 ) );
    }
    rocketnet_unit_check( disconnects == burst );

    int total = 0;
    for ( int call = 0; call < record->calls && call < SEND_EDGE_MAX_CALLS; call++ )
    {
        rocketnet_unit_check( record->call_sends[call] > 0 && record->call_sends[call] <= capacity );
        total += record->call_sends[call];
    }
    rocketnet_unit_check( total == record->count );

    free( seen );
}

// One update owes exactly the buffer's capacity: every seated client's
// keep-alive, sent in one batch of exactly the capacity at the update's flush.

static void test_server_control_buffer_filled_exactly_is_sent_whole()
{
    const int capacity = rocketnet_server_send_batch_packets( SEND_EDGE_CLIENTS );
    rocketnet_unit_check( capacity == SEND_EDGE_CLIENTS );

    send_edge_record = (send_edge_record_t *) calloc( 1, sizeof( send_edge_record_t ) );
    rocketnet_unit_check( send_edge_record != NULL );

    struct rocketnet_server_transport_io_operations_t operations;
    const struct rocketnet_server_transport_io_operations_t * loopback = NULL;
    void * loopback_instance = NULL;
    struct rocketnet_server_transport_t * server = send_edge_server( &operations, &loopback, &loopback_instance );
    if ( server && send_edge_record )
    {
        rocketnet_server_transport_update( server, 1.0 );

        rocketnet_unit_check( send_edge_record->calls == 1 );
        rocketnet_unit_check( send_edge_record->count == capacity );
        send_edge_check( send_edge_record, capacity, -1, 0 );

        send_edge_restore( server, loopback, loopback_instance );
    }

    free( send_edge_record );
    send_edge_record = NULL;
}

REGISTER_UNIT_TEST( test_server_control_buffer_filled_exactly_is_sent_whole );

// One update owes more than the capacity: a client disconnected by the
// server owes its disconnect burst, every other seated client its
// keep-alive. The buffer is flushed when exactly full, as a batch of exactly
// the capacity, and the frames after it go in a second batch from the first
// slot; none is written past the buffer, lost, repeated or changed.

static void test_server_control_buffer_full_is_flushed_before_the_next_frame()
{
    const int capacity = rocketnet_server_send_batch_packets( SEND_EDGE_CLIENTS );

    send_edge_record = (send_edge_record_t *) calloc( 1, sizeof( send_edge_record_t ) );
    rocketnet_unit_check( send_edge_record != NULL );

    struct rocketnet_server_transport_io_operations_t operations;
    const struct rocketnet_server_transport_io_operations_t * loopback = NULL;
    void * loopback_instance = NULL;
    struct rocketnet_server_transport_t * server = send_edge_server( &operations, &loopback, &loopback_instance );
    if ( server && send_edge_record )
    {
        const int disconnected = 7;
        rocketnet_server_transport_disconnect_client( server, disconnected );
        rocketnet_server_transport_update( server, 1.0 );

        // the whole burst is owed: ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS
        // copies, a number the server's own count does not set
        const int burst = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
        if ( send_edge_record->calls != 2 || send_edge_record->call_sends[0] != capacity ||
             send_edge_record->count != burst + ( SEND_EDGE_CLIENTS - 1 ) )
        {
            printf( "test_server_control_buffer_full_is_flushed_before_the_next_frame: %d batches, first %d, %d frames\n",
                    send_edge_record->calls, send_edge_record->call_sends[0], send_edge_record->count );
        }
        rocketnet_unit_check( send_edge_record->count == burst + ( SEND_EDGE_CLIENTS - 1 ) );
        rocketnet_unit_check( send_edge_record->calls == 2 );
        rocketnet_unit_check( send_edge_record->call_sends[0] == capacity );
        rocketnet_unit_check( send_edge_record->call_sends[1] == send_edge_record->count - capacity );
        send_edge_check( send_edge_record, capacity, disconnected, burst );

        send_edge_restore( server, loopback, loopback_instance );
    }

    free( send_edge_record );
    send_edge_record = NULL;
}

REGISTER_UNIT_TEST( test_server_control_buffer_full_is_flushed_before_the_next_frame );

#endif // #if ROCKETNET_DEV
