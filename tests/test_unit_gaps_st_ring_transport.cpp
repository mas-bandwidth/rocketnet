/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Cases for the survivors of the 2026-11-10 mutation hunt (run 2) over
// sdk/rocketnet_server_transport.cpp: the portable payload ring and its
// flush, the shared staged start/finish/abort path, the disconnect burst
// drain, the oversize receive account, the maintenance sweep cursor, the
// replay sink, the accepted-payload batch bound and the slot-clean check.
// Each case names the mutant it kills (`mutation gap: hunt 2/<id>`).
//
// Two fake transports stand in for a socket: a buffered one whose
// send_batch records every datagram (destination, length, first bytes), and
// a staged one with a fixed staging that counts every start, finish and
// abort. No socket, no thread, no wall clock: the clock argument starts at
// 1000.0. This file is compiled into the loopback translation unit
// (tests/test_unit_driver_loopback.cpp), which includes the server
// transport, so the cases read its internal layout.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_internal.h"
#include "rocketnet_test_seams.h"

#include <stdio.h>
#include <string.h>

// a failed expectation names the fault before the check prints its line; the
// condition is evaluated once
#define st_ring_expect( condition, fault )                                                                         \
do                                                                                                                 \
{                                                                                                                  \
    const bool st_ring_held = (condition);                                                                         \
    if ( !st_ring_held )                                                                                           \
    {                                                                                                              \
        printf( "fault: %s\n", fault );                                                                            \
        fflush( stdout );                                                                                          \
    }                                                                                                              \
    rocketnet_unit_check( st_ring_held );                                                                          \
} while(0)

static const int ST_RING_MAX_CLIENTS = 4;
static const int ST_RING_MAX_DATAGRAMS = 256;
static const int ST_RING_PORT_BASE = 1000;

// the staged fake's one frame: every start shares it, read at its finish
alignas( 16 ) static uint8_t st_ring_staged_frame[ROCKETNET_FRAME_STRIDE];

struct st_ring_datagram_t
{
    rocketnet_address_t to;
    int bytes;
    uint8_t head[16];
};

struct st_ring_fake_t
{
    // buffered: every datagram send_batch was handed, in order
    st_ring_datagram_t datagrams[ST_RING_MAX_DATAGRAMS];
    int num_datagrams;
    int buffered_aborts;                                // abort_packet calls on the buffered transport

    // staged: a fixed staging of `capacity` frames per flush
    rocketnet_address_t reserved_to;
    int capacity;
    int used;
    int starts;
    int finishes;
    int aborts;
    uint64_t last_abort_id;                             // the backend id the last abort was handed
    bool refuse_next_filling;                           // the next start finds the staging filled and refuses
    bool refuse_without_id;                             // starts refuse and leave the id untouched
    bool start_leaves_id;                               // starts succeed and leave the id untouched
    int finished_to[ST_RING_MAX_CLIENTS];               // frames finished, by client (port - 1000)
    int disconnects_flushed[ST_RING_MAX_CLIENTS];       // disconnect packets flushed, by client
    int disconnects_staged[ST_RING_MAX_CLIENTS];
    int order[4 * ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS * ST_RING_MAX_CLIENTS];
    int num_order;                                      // the client of each disconnect packet, in seal order
    int first_payload_client;                           // the client of the first non-disconnect frame finished, -1: none

    // receive: one datagram handed to the next receive_batch, 0 bytes: none
    rocketnet_address_t receive_from;
    int receive_bytes;

    // replay sink
    int forgets;
    int marks;
    int commits;
    rocketnet_address_t forgotten;

    int stops;
    rocketnet_server_socket_process_packets_t received;
};

static int st_ring_client_of( const rocketnet_address_t * address )
{
    const int client = address->port - ST_RING_PORT_BASE;
    return client >= 0 && client < ST_RING_MAX_CLIENTS ? client : -1;
}

static void st_ring_destroy( void * ) {}
static void st_ring_stop( void * p ) { ( (st_ring_fake_t *) p )->stops++; }
static bool st_ring_stopped( void * p ) { return ( (st_ring_fake_t *) p )->stops > 0; }

static void st_ring_send_batch( void * p, const rocketnet_platform_send_t * sends, int num_sends )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    for ( int i = 0; i < num_sends; i++ )
    {
        rocketnet_unit_check( f->num_datagrams < ST_RING_MAX_DATAGRAMS );
        st_ring_datagram_t * d = &f->datagrams[f->num_datagrams++];
        d->to = *sends[i].to;
        d->bytes = sends[i].packet_bytes;
        memset( d->head, 0, sizeof( d->head ) );
        const int head_bytes = sends[i].packet_bytes < (int) sizeof( d->head ) ? sends[i].packet_bytes : (int) sizeof( d->head );
        if ( head_bytes > 0 )
        {
            memcpy( d->head, sends[i].packet_data, (size_t) head_bytes );
        }
    }
}

static int st_ring_receive_batch( void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * packet_bytes, int max_packets )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    if ( f->receive_bytes <= 0 || max_packets < 1 )
    {
        return 0;
    }

    // a truncated datagram: the transport stores at most its stride and
    // reports the datagram's whole length

    from[0] = f->receive_from;
    const int stored = f->receive_bytes < stride ? f->receive_bytes : stride;
    memset( data, 0x5C, (size_t) stored );
    packet_bytes[0] = f->receive_bytes;
    f->receive_bytes = 0;
    return 1;
}

static rocketnet_server_socket_process_packets_t * st_ring_receive( void * p )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    f->received.num_packets = 0;
    return &f->received;
}

static void st_ring_buffered_abort_packet( void * p, uint64_t, uint8_t * )
{
    ( (st_ring_fake_t *) p )->buffered_aborts++;
}

static uint8_t * st_ring_start_packet( void * p, const rocketnet_address_t * to, uint64_t * id )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    if ( f->refuse_without_id )
    {
        return NULL;
    }
    if ( f->refuse_next_filling )
    {
        // another producer took the rest of the staging first
        f->refuse_next_filling = false;
        f->used = f->capacity;
        return NULL;
    }
    if ( f->used >= f->capacity )
    {
        return NULL;
    }
    f->reserved_to = *to;
    f->starts++;
    if ( !f->start_leaves_id )
    {
        *id = (uint64_t) f->used;
    }
    f->used++;
    return st_ring_staged_frame;
}

static void st_ring_finish_packet( void * p, uint64_t, uint8_t * frame, int )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    f->finishes++;
    const int client = st_ring_client_of( &f->reserved_to );
    if ( client >= 0 )
    {
        f->finished_to[client]++;
    }
    if ( frame[ROCKETNET_FRAME_WIRE_OFFSET] != ROCKETNET_CONNECTION_DISCONNECT_PACKET )
    {
        if ( f->first_payload_client < 0 )
        {
            f->first_payload_client = client;
        }
        return;
    }
    rocketnet_unit_check( client >= 0 );
    f->disconnects_staged[client]++;
    rocketnet_unit_check( f->num_order < (int) ( sizeof( f->order ) / sizeof( f->order[0] ) ) );
    f->order[f->num_order++] = client;
}

static void st_ring_abort_packet( void * p, uint64_t id, uint8_t * )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    f->aborts++;
    f->last_abort_id = id;
}

static void st_ring_flush( void * p )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    for ( int i = 0; i < ST_RING_MAX_CLIENTS; i++ )
    {
        f->disconnects_flushed[i] += f->disconnects_staged[i];
        f->disconnects_staged[i] = 0;
    }
    f->used = 0;
}

static int st_ring_send_room( void * p )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    return f->capacity - f->used;
}

static bool st_ring_send_idle( void * p )
{
    return ( (st_ring_fake_t *) p )->used == 0;
}

static bool st_ring_replay_forget( void * p, const rocketnet_address_t * address )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    f->forgets++;
    f->forgotten = *address;
    return true;
}

static bool st_ring_replay_mark( void * p, const rocketnet_address_t *, uint32_t )
{
    ( (st_ring_fake_t *) p )->marks++;
    return true;
}

static void st_ring_replay_commit( void * p )
{
    ( (st_ring_fake_t *) p )->commits++;
}

// A client from 127.0.0.1 port 1000 + i connected in the slot the slot API
// hands it, with a fresh connection id, as the handshake would leave it.
// Returns the slot.
static int st_ring_connect_port( rocketnet_server_transport_t * server, int i )
{
    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    address.port = (uint16_t) ( ST_RING_PORT_BASE + i );

    const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 1000.0 );
    rocketnet_unit_check( index >= 0 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, index );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    const uint64_t owner_generation = rocketnet_encryption_map_owner_generation( server->protocol.encryption_map, index );
    const int slot = rocketnet_client_slots_assign( server->protocol.client_slots, (uint64_t) ( i + 1 ), index,
                                                    owner_generation, user_data );
    rocketnet_unit_check( slot >= 0 );
    mapping->control->client_index = slot;
    mapping->client_id = (uint64_t) ( i + 1 );
    mapping->control->established = 1;
    mapping->control->timeout_seconds = 1000;
    mapping->control->last_receive_time = 1000.0;
    memset( &mapping->keys, i + 1, sizeof( mapping->keys ) );
    server->protocol.connection_ids[slot] = ++server->protocol.next_connection_id;
    return slot;
}

// Slot i connected from port 1000 + i.
static void st_ring_connect( rocketnet_server_transport_t * server, int i )
{
    rocketnet_unit_check( st_ring_connect_port( server, i ) == i );
}

enum st_ring_kind_t
{
    ST_RING_BUFFERED,
    ST_RING_STAGED
};

// A server transport of max_clients slots, the first `clients` connected
// (ports 1000 + i), over the buffered or the staged fake.
static rocketnet_server_transport_t * st_ring_server( st_ring_fake_t * f, rocketnet_server_transport_io_operations_t * operations,
                                                      st_ring_kind_t kind, int max_clients, int clients )
{
    rocketnet_unit_check( max_clients <= ST_RING_MAX_CLIENTS && clients <= max_clients );
    f->first_payload_client = -1;

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config_current( &config );
    config.max_clients = max_clients;
    config.protocol_id = 1;
    config.token_epoch_unix_seconds = 1700000000ULL;       // the update never reads the wall clock
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();

    // production create refuses dev keys and zero keys; any non-zero, non-dev
    // bytes satisfy the key check for this fixture, which runs no handshake

    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );

    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    rocketnet_unit_check( server );
    rn_test_server_io_stop_and_close( &server->io );

    memset( operations, 0, sizeof( *operations ) );
    operations->destroy = st_ring_destroy;
    operations->stop = st_ring_stop;
    operations->stopped = st_ring_stopped;
    operations->send_batch = st_ring_send_batch;
    operations->receive_batch = st_ring_receive_batch;
    operations->receive = st_ring_receive;
    if ( kind == ST_RING_STAGED )
    {
        operations->flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
        operations->start_packet = st_ring_start_packet;
        operations->finish_packet = st_ring_finish_packet;
        operations->abort_packet = st_ring_abort_packet;
        operations->flush = st_ring_flush;
        operations->send_room = st_ring_send_room;
        operations->send_idle = st_ring_send_idle;
    }
    else
    {
        // a buffered transport never has a frame of its own to abort; the
        // operation is here only to count a call that must not happen
        operations->flags = ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
        operations->abort_packet = st_ring_buffered_abort_packet;
    }
    server->io.instance = f;
    server->io.operations = operations;

    for ( int i = 0; i < clients; i++ )
    {
        st_ring_connect( server, i );
    }

    return server;
}

static void st_ring_fill( uint8_t * payload, int bytes, uint8_t value )
{
    memset( payload, value, (size_t) bytes );
}

static int st_ring_datagrams_to( const st_ring_fake_t * f, int client, int from_index )
{
    int count = 0;
    for ( int i = from_index; i < f->num_datagrams; i++ )
    {
        if ( st_ring_client_of( &f->datagrams[i].to ) == client )
        {
            count++;
        }
    }
    return count;
}

// ----------------------------------------------------------------
// The portable payload ring
// ----------------------------------------------------------------

// A ring slot started and never finished (the caller dropped the lease, and
// send_packets reset it) is not a datagram: the flush skips it. The slot
// held a finished packet to client A in the previous fill; its length must
// not survive into the new start, or the flush resends A's stale bytes, with
// A's length, to client B's address.
// mutation gap: hunt 2/5745 (ring->info[slot].wire_bytes = 0 deleted)
// mutation gap: hunt 2/5744 (ring->info[slot].wire_bytes = 1)
static void test_gaps_st_ring_unfinished_slot_sends_nothing()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 2 );

    uint8_t payload[64];
    st_ring_fill( payload, sizeof( payload ), 0xA1 );
    rocketnet_server_transport_send_packet( server, 0, payload, sizeof( payload ) );
    rocketnet_server_transport_send_packets( server );
    st_ring_expect( f->num_datagrams == 1, "the finished packet to client A did not leave as one datagram" );
    st_ring_expect( st_ring_client_of( &f->datagrams[0].to ) == 0, "the packet to client A went to another address" );

    const uint64_t packets_sent = server->server_stats.packets_sent;
    const uint64_t bytes_sent = server->server_stats.bytes_sent;
    const uint64_t b_packets_sent = server->client_wire_totals[1].packets_sent;

    // client B: the same ring slot started, its lease dropped unfinished

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 1, 64, &context );
    rocketnet_unit_check( frame );
    memset( frame + context.payload_index, 0xB2, 64 );
    rocketnet_server_transport_send_packets( server );

    st_ring_expect( f->num_datagrams == 1, "an unfinished ring slot was sent at the flush (stale wire_bytes from the slot's previous fill)" );
    st_ring_expect( st_ring_datagrams_to( f, 1, 0 ) == 0, "a datagram was sent to client B, whose packet was never finished" );
    st_ring_expect( server->server_stats.packets_sent == packets_sent, "server packets_sent counted a datagram for an unfinished ring slot" );
    st_ring_expect( server->server_stats.bytes_sent == bytes_sent, "server bytes_sent counted bytes for an unfinished ring slot" );
    st_ring_expect( server->client_wire_totals[1].packets_sent == b_packets_sent, "client B was charged a datagram for its unfinished packet" );

    // the ring still works: B's next finished packet leaves whole

    rocketnet_server_transport_send_packet( server, 1, payload, 32 );
    rocketnet_server_transport_send_packets( server );
    st_ring_expect( f->num_datagrams == 2 && st_ring_client_of( &f->datagrams[1].to ) == 1, "client B's finished packet did not leave after the dropped one" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// An aborted ring slot is not a datagram either: nothing reaches the socket
// and nothing is counted sent.
// mutation gap: hunt 2/5764 (abort leaves wire_bytes 1)
static void test_gaps_st_ring_aborted_slot_sends_nothing()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    const uint64_t packets_sent = server->server_stats.packets_sent;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 64, &context );
    rocketnet_unit_check( frame );
    memset( frame + context.payload_index, 0xC3, 64 );
    rocketnet_server_transport_abort_packet( server, &context );
    rocketnet_server_transport_send_packets( server );

    st_ring_expect( f->num_datagrams == 0, "an aborted ring slot was sent at the flush" );
    st_ring_expect( server->server_stats.packets_sent == packets_sent, "server packets_sent counted an aborted ring slot" );
    st_ring_expect( f->buffered_aborts == 0, "a portable abort called the transport's abort_packet" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// An aborted slot ahead of a finished one in the ring: the flush skips the
// aborted slot both when it builds the batch and when it charges the
// results, so the one datagram is charged to the client it went to.
// mutation gap: hunt 2/5884 (wire_bytes <= 0 to < 0 at the charge loop)
static void test_gaps_st_ring_aborted_slot_does_not_shift_charges()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 2 );

    const uint64_t packets_sent = server->server_stats.packets_sent;
    const uint64_t bytes_sent = server->server_stats.bytes_sent;

    // client A: started and aborted (ring slot 0)

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 64, &context );
    rocketnet_unit_check( frame );
    rocketnet_server_transport_abort_packet( server, &context );

    // client B: one packet (ring slot 1)

    uint8_t payload[48];
    st_ring_fill( payload, sizeof( payload ), 0xD4 );
    rocketnet_server_transport_send_packet( server, 1, payload, sizeof( payload ) );
    rocketnet_server_transport_send_packets( server );

    st_ring_expect( f->num_datagrams == 1, "the flush did not send exactly client B's one datagram" );
    st_ring_expect( st_ring_client_of( &f->datagrams[0].to ) == 1, "the one datagram did not go to client B" );
    const uint64_t wire = (uint64_t) ( f->datagrams[0].bytes + rocketnet_wire_header_bytes( f->datagrams[0].to.type ) );

    st_ring_expect( server->client_wire_totals[1].packets_sent == 1, "client B's datagram was not charged to client B (charge shifted onto the aborted slot)" );
    st_ring_expect( server->client_wire_totals[1].bytes_sent == wire, "client B's bytes_sent does not match its datagram" );
    st_ring_expect( server->client_wire_totals[0].packets_sent == 0 && server->client_wire_totals[0].bytes_sent == 0, "client A was charged for its aborted packet" );
    st_ring_expect( server->server_stats.packets_sent == packets_sent + 1, "server packets_sent is not exactly the one datagram" );
    st_ring_expect( server->server_stats.bytes_sent == bytes_sent + wire, "server bytes_sent is not exactly the one datagram" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// Every refused start hands the caller back a zeroed context: no frame and
// packet id 0, whatever the context held before the call.
// mutation gap: hunt 2/5711 (memset( context, 1, ... ))
static void test_gaps_st_ring_refused_start_zeroes_context()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    struct rocketnet_packet_context_t context;

    // request out of range

    memset( &context, 0xEE, sizeof( context ) );
    st_ring_expect( rocketnet_server_transport_start_packet( server, 0, 0, &context ) == NULL, "a zero-byte start was not refused" );
    st_ring_expect( context.frame == NULL, "a start refused for its size left a non-NULL frame in the context" );
    st_ring_expect( context.packet_id == 0, "a start refused for its size left a nonzero packet id in the context" );

    // no client in the slot

    memset( &context, 0xEE, sizeof( context ) );
    st_ring_expect( rocketnet_server_transport_start_packet( server, 1, 16, &context ) == NULL, "a start to an empty slot was not refused" );
    st_ring_expect( context.frame == NULL, "a start refused for an empty slot left a non-NULL frame in the context" );
    st_ring_expect( context.packet_id == 0, "a start refused for an empty slot left a nonzero packet id in the context" );

    // a stopped transport

    rocketnet_server_transport_stop( server );
    memset( &context, 0xEE, sizeof( context ) );
    st_ring_expect( rocketnet_server_transport_start_packet( server, 0, 16, &context ) == NULL, "a start on a stopping transport was not refused" );
    st_ring_expect( context.frame == NULL, "a start refused on a stopping transport left a non-NULL frame in the context" );
    st_ring_expect( context.packet_id == 0, "a start refused on a stopping transport left a nonzero packet id in the context" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The shared staged start, finish and abort
// ----------------------------------------------------------------

// Leaves a run of `value` bytes in the stack below the caller, where the
// next call's locals land, so a local read before it is written reads a
// known value (0xFF: ROCKETNET_TRANSPORT_PACKET_UNREACHABLE, rather than a
// lucky zero; 0x00: a NULL pointer an assert names). Called through a
// volatile pointer so it is never inlined away.
static void st_ring_fill_stack( uint8_t value )
{
    volatile uint8_t junk[16384];
    for ( int i = 0; i < (int) sizeof( junk ); i++ )
    {
        junk[i] = value;
    }
}
static void ( *volatile st_ring_fill_stack_call )( uint8_t ) = st_ring_fill_stack;

// The packet id a staged start hands the transport starts at 0: a transport
// that refuses without writing it is counted by the refusal it is (room
// left: neither SEND_NO_LINK_ADDRESS nor SEND_RING_FULL), and one that
// succeeds without writing it gives the caller packet id 0. Also runs under
// the MSan leg, where the uninitialised read is reported directly.
// mutation gap: hunt 2/4552 (*id = 0 deleted)
static void test_gaps_st_ring_staged_start_id_starts_at_zero()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 8;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );
    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t no_link = counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS];
    const uint64_t ring_full = counters[ROCKETNET_COUNTER_SEND_RING_FULL];

    struct rocketnet_packet_context_t context;

    f->refuse_without_id = true;
    st_ring_fill_stack_call( 0xFF );
    st_ring_expect( rocketnet_server_transport_start_packet( server, 0, 16, &context ) == NULL, "the refusing transport's start was not refused" );
    st_ring_expect( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == no_link, "a refusal that wrote no id was counted SEND_NO_LINK_ADDRESS (the id was read uninitialised)" );
    st_ring_expect( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full, "a refusal with room left was counted SEND_RING_FULL" );
    st_ring_expect( context.packet_id == 0, "a refused staged start left a nonzero packet id" );

    f->refuse_without_id = false;
    f->start_leaves_id = true;
    st_ring_fill_stack_call( 0xFF );
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
    st_ring_expect( frame != NULL, "the staged start was refused" );
    f->last_abort_id = 12345;
    rocketnet_server_transport_abort_packet( server, &context );
    st_ring_expect( f->aborts == 1, "the staged abort did not reach the transport" );
    st_ring_expect( f->last_abort_id == 0, "a staged start whose transport wrote no id kept a garbage backend id (handed back at the abort)" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

static int g_st_ring_asserts = 0;
static void st_ring_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    printf( "assert: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    g_st_ring_asserts++;
}

// A sealed write that fails on the staged path: the frame goes back to the
// transport with one abort; it is never finished, never handed to the link
// with 0 bytes and never counted sent. The sequence 0 makes the seal refuse
// after start_payload took it (a write of sequence 0 is never sealed).
// mutation gap: hunt 2/5809 (wire_bytes == 0 to == 1)
static void test_gaps_st_ring_staged_seal_failure_aborts()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 8;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );

    const int mapping_index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 );
    rocketnet_unit_check( mapping_index >= 0 );
    rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index )->send_sequence_session = 0;

    const uint64_t packets_sent = server->server_stats.packets_sent;
    const uint64_t pending_sent = server->stats_pending_packets_sent;

    rocketnet_assert_function_t saved = rn_assert_function;
    g_st_ring_asserts = 0;
    rocketnet_assert_function( st_ring_assert_hook );

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
    rocketnet_unit_check( frame );
    memset( frame + context.payload_index, 0x11, 16 );
    const int wire_bytes = rocketnet_server_transport_finish_packet( server, &context, 16 );

    rocketnet_assert_function( saved );

    st_ring_expect( wire_bytes == 0, "a finish whose seal failed did not return 0" );
    st_ring_expect( f->aborts == 1, "a frame whose seal failed was not aborted back to the staged transport" );
    st_ring_expect( f->finishes == 0, "a frame whose seal failed was finished (committed with 0 bytes)" );
    st_ring_expect( g_st_ring_asserts == 0, "a frame whose seal failed reached the staged commit (assert on a 0-byte finish)" );
    st_ring_expect( server->server_stats.packets_sent == packets_sent, "a frame whose seal failed was counted sent" );
    st_ring_expect( server->stats_pending_packets_sent == pending_sent, "a frame whose seal failed was charged to the pending sent stats" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// start_payload refusing a slot that lost its mapping between start and
// finish: the frame is aborted there, once, and the missing mapping is
// counted once, by that refusal; the seal is never reached.
// mutation gap: hunt 2/5806 (abort after the start_payload refusal deleted)
static void test_gaps_st_ring_start_payload_refusal_aborts_once()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 1, 1 );
    const uint64_t * counters = server->protocol.counters->values;

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
    rocketnet_unit_check( frame );
    memset( frame + context.payload_index, 0x22, 16 );

    // the slot ends and is taken again by a connection without a mapping (a
    // state the slot API permits) before the caller finishes

    rocketnet_server_transport_disconnect_client( server, 0 );
    rocketnet_client_slots_publish( server->protocol.client_slots );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    rocketnet_unit_check( rocketnet_client_slots_assign( server->protocol.client_slots, 99, -1, 99, user_data ) == 0 );

    const int aborts = f->aborts;
    const int finishes = f->finishes;
    const uint64_t without_mapping = counters[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING];

    st_ring_expect( rocketnet_server_transport_finish_packet( server, &context, 16 ) == 0, "a finish to a slot without a mapping did not return 0" );
    st_ring_expect( f->aborts == aborts + 1, "the frame was not aborted exactly once" );
    st_ring_expect( f->finishes == finishes, "the frame to a slot without a mapping was finished" );
    st_ring_expect( counters[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING] == without_mapping + 1,
                    "the missing mapping was counted more than once: the finish went on to the seal after start_payload refused" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A control frame that is reserved and then given up (here a disconnect
// burst whose sealed write fails) goes back through the staged transport's
// abort on a staged transport, and never calls abort_packet on a buffered
// one (its reserved slot is simply reused).
// mutation gap: hunt 2/4719 (the staged test in abort_send_frame negated)
static void test_gaps_st_ring_abort_send_frame_by_transport_kind()
{
    for ( int kind = 0; kind < 2; kind++ )
    {
        st_ring_fake_t * f = new st_ring_fake_t();
        f->capacity = 64;
        rocketnet_server_transport_io_operations_t operations;
        rocketnet_server_transport_t * server = st_ring_server( f, &operations, kind == 0 ? ST_RING_STAGED : ST_RING_BUFFERED, 2, 1 );

        rocketnet_dev_fail_disconnect_sealed_writes( 1 );
        rocketnet_server_transport_disconnect_client_shutdown( server, 0 );
        rocketnet_server_transport_stop( server );
        for ( int poll = 0; poll < 8 && !rocketnet_server_transport_stopped( server ); poll++ )
        {
        }
        rocketnet_dev_fail_disconnect_sealed_writes( 0 );

        if ( kind == 0 )
        {
            st_ring_expect( f->aborts == 1, "a staged frame given up after its sealed write failed was not aborted back to the transport (leaked)" );
            st_ring_expect( f->starts == f->finishes + f->aborts, "staged starts do not balance finishes and aborts" );
        }
        else
        {
            st_ring_expect( f->buffered_aborts == 0, "a buffered transport's control frame was aborted through the transport's abort_packet" );
            st_ring_expect( f->num_datagrams == 0, "a burst whose sealed write failed was sent" );
        }

        rn_test_server_transport_stop_and_destroy( server );
        delete f;
    }
}

// ----------------------------------------------------------------
// The disconnect burst drain
// ----------------------------------------------------------------

// A start refused because the staging filled (no room left) leaves the
// waiting burst waiting: it is not abandoned, and its packets leave whole
// once the staging has room again.
// mutation gap: hunt 2/5948 (packet_id != UNREACHABLE to ==)
static void test_gaps_st_ring_full_staging_keeps_the_burst()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );

    rocketnet_server_transport_disconnect_client_shutdown( server, 0 );
    f->refuse_next_filling = true;
    rocketnet_server_transport_stop( server );

    st_ring_expect( server->disconnect_burst_count == 1, "a burst whose start met a full staging was abandoned instead of waiting" );
    st_ring_expect( f->disconnects_flushed[0] == 0, "the burst left in the step whose start was refused" );

    for ( int poll = 0; poll < 8 && !rocketnet_server_transport_stopped( server ); poll++ )
    {
    }

    st_ring_expect( rocketnet_server_transport_stopped( server ), "the stop did not finish" );
    st_ring_expect( f->disconnects_flushed[0] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS, "the waiting burst's packets were lost after a full-staging refusal" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A deferred burst's datagrams belong to the server totals only: the slot
// it left may already hold another connection. The client still in slot 0
// is charged only for the frames that went to it.
// mutation gap: hunt 2/5954 (frame.client_index = -1 to 0)
static void test_gaps_st_ring_deferred_burst_charges_no_slot()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 4;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 2 );

    rocketnet_server_transport_disconnect_client( server, 1 );
    st_ring_expect( server->disconnect_burst_count == 1, "the kick with too little room did not defer its burst" );

    double time = 1000.0;
    for ( int update = 0; update < 16 && f->disconnects_flushed[1] < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS; update++ )
    {
        time += 1.0 / 60.0;
        rocketnet_server_transport_update( server, time );
    }

    st_ring_expect( f->disconnects_flushed[1] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS, "the deferred burst did not leave whole" );
    st_ring_expect( server->client_wire_totals[0].packets_sent == (uint64_t) f->finished_to[0],
                    "slot 0 was charged for datagrams that did not go to it (the deferred burst of slot 1)" );
    st_ring_expect( server->client_wire_totals[1].packets_sent == 0, "the ended slot 1 was charged for its deferred burst" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The immediate burst is committed before the slot is released, so the
// slot's own wire totals take every packet of it.
// mutation gap: hunt 2/6040 (frame.client_index = client_index deleted)
static void test_gaps_st_ring_immediate_burst_charges_its_slot()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );

    const uint64_t before = server->client_wire_totals[0].packets_sent;
    rocketnet_server_transport_disconnect_client( server, 0 );

    st_ring_expect( server->disconnect_burst_count == 0, "the kick with room deferred its burst" );
    st_ring_expect( f->disconnects_staged[0] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS, "the immediate burst was not staged whole" );
    st_ring_expect( server->client_wire_totals[0].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS,
                    "the immediate burst was not charged to its own slot's wire totals" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// Three bursts drained in one pass of 29 seals: one per burst per pass in
// ring order, so after nine whole passes the head's tenth seal completes
// it and it pops; the cursor falls back with it, and the 29th seal goes to
// the new head (the second burst), not past it to the third.
// mutation gap: hunt 2/5986 (cursor > 0 to > 1 at the pop)
// mutation gap: hunt 2/5987 (cursor > 0 negated at the pop)
static void test_gaps_st_ring_cursor_falls_with_the_head()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 3 * ( ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS - 1 ) + 2;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 3, 3 );

    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_server_transport_disconnect_client_shutdown( server, i );
    }
    rocketnet_server_transport_stop( server );

    const int seals = f->capacity;
    st_ring_expect( f->num_order == seals, "the first stop step did not seal as much as the room took" );
    st_ring_expect( f->order[seals - 2] == 0, "the head burst's last packet was not the second-to-last seal" );
    st_ring_expect( f->order[seals - 1] == 1, "after the head burst popped the next seal skipped the new head burst (the cursor did not fall with the pop)" );

    for ( int poll = 0; poll < 8 && !rocketnet_server_transport_stopped( server ); poll++ )
    {
    }
    for ( int i = 0; i < 3; i++ )
    {
        st_ring_expect( f->disconnects_flushed[i] == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS, "a burst did not leave whole" );
    }

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The stop abandons its waiting bursts for want of room (none left, nothing
// in flight) and resets the drain cursor with them. Here the stop goes on,
// because the link simulator still holds packets, and a direct transport
// user defers new bursts in the stop: the first seal goes to the new head
// burst, not to wherever the old cursor pointed.
// mutation gap: hunt 2/5366 (disconnect_burst_cursor = 0 deleted at the abandon)
static void test_gaps_st_ring_abandon_resets_the_cursor()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 2;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 3, 3 );

    // every packet doubled: the simulator holds what the room cannot take
    rocketnet_unit_check( rocketnet_server_transport_network_simulator( server, 0, 0, 0, 100 ) );

    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_server_transport_disconnect_client_shutdown( server, i );
    }
    rocketnet_server_transport_stop( server );

    // one partial pass: the first two bursts have one seal each, the cursor is past them
    st_ring_expect( server->disconnect_burst_count == 3 && server->disconnect_burst_cursor == 2, "the first stop step was not one partial pass over two of three bursts" );
    st_ring_expect( rocketnet_network_simulator_holds_any( server->link_simulator ), "the link simulator holds nothing after the first stop step" );

    // no room and nothing in flight: the waiting bursts are abandoned, and the
    // held packets keep the stop going
    f->capacity = 0;
    st_ring_expect( !rocketnet_server_transport_stopped( server ), "the stop finished with packets still in the link simulator" );
    st_ring_expect( server->disconnect_burst_count == 0, "the bursts were not abandoned for want of room" );

    // three new bursts deferred during the stop, then room for one seal
    rocketnet_client_slots_publish( server->protocol.client_slots );
    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_server_transport_disconnect_client_shutdown( server, st_ring_connect_port( server, i ) );
    }
    rocketnet_unit_check( server->disconnect_burst_count == 3 );
    f->capacity = 1;
    rocketnet_server_transport_stopped( server );

    const int head = server->disconnect_burst_head;
    const int capacity = server->disconnect_burst_capacity;
    st_ring_expect( server->disconnect_bursts[head].sealed == 1, "the first seal after the abandon did not go to the head burst (a stale cursor)" );
    st_ring_expect( server->disconnect_bursts[( head + 2 ) % capacity].sealed == 0, "the first seal after the abandon went to the third burst (the cursor left past the abandoned bursts)" );

    f->capacity = 64;
    for ( int poll = 0; poll < 16 && !rocketnet_server_transport_stopped( server ); poll++ )
    {
    }
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// begin_message_update on a transport that is not running opens no
// allowance: a kick during the stop with the update's allowance spent
// defers its burst exactly as it would without the begin call.
// mutation gap: hunt 2/5454 (the not-running return deleted)
static void test_gaps_st_ring_begin_update_while_stopping_opens_nothing()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 2 );

    // this update's allowance is spent (as by the kicks of an update before)

    server->disconnect_seals_left = 0;
    rocketnet_server_transport_stop( server );
    rocketnet_unit_check( server->state == ROCKETNET_SERVER_TRANSPORT_STOPPING );
    rocketnet_unit_check( rocketnet_client_slots_connected( server->protocol.client_slots, 0 ) );

    const uint64_t seals = server->disconnect_seals;
    rocketnet_server_transport_begin_message_update( server );
    rocketnet_server_transport_disconnect_client( server, 0 );

    st_ring_expect( server->disconnect_seals == seals, "a kick during the stop sealed at once: begin_message_update opened an allowance on a stopping transport" );
    st_ring_expect( server->disconnect_burst_count == 1, "a kick during the stop with no allowance did not defer its burst" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// Receive, sweep, replay sink, accepted payloads
// ----------------------------------------------------------------

// An oversize datagram (past the receive ceiling, within the UDP envelope)
// from an address no client holds: it counts in the server's received
// totals, and in no slot's.
// mutation gap: hunt 2/5583 (wire totals charged to slot 0)
// mutation gap: hunt 2/5584 (the server received totals not charged)
static void test_gaps_st_ring_oversize_datagram_accounting()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    rocketnet_unit_check( rocketnet_address_parse( &f->receive_from, "127.0.0.1:0" ) );
    f->receive_from.port = 2999;
    f->receive_bytes = ROCKETNET_RECEIVE_CEILING + 1;
    rocketnet_unit_check( f->receive_bytes <= ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES );

    const uint64_t packets_received = server->server_stats.packets_received;
    const uint64_t bytes_received = server->server_stats.bytes_received;
    const uint64_t slot_received = server->client_wire_totals[0].packets_received;
    const uint64_t slot_bytes = server->client_wire_totals[0].bytes_received;

    rocketnet_server_transport_update( server, 1000.0 );

    const uint64_t wire = (uint64_t) ( ROCKETNET_RECEIVE_CEILING + 1 + rocketnet_wire_header_bytes( f->receive_from.type ) );
    st_ring_expect( server->server_stats.packets_received == packets_received + 1, "an oversize datagram was not counted in the server's packets_received" );
    st_ring_expect( server->server_stats.bytes_received == bytes_received + wire, "an oversize datagram's bytes were not counted in the server's bytes_received" );
    st_ring_expect( server->client_wire_totals[0].packets_received == slot_received, "an oversize datagram from a stranger was charged to slot 0's received totals" );
    st_ring_expect( server->client_wire_totals[0].bytes_received == slot_bytes, "an oversize datagram's bytes were charged to slot 0" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The maintenance sweep's cursor rests on an empty slot it passed: a client
// that takes that slot before the next update is served first then. Slots
// 0-2 connected and 3 empty; one sweep with room for all; slot 3 connects;
// the next sweep with room for one keep-alive serves slot 3.
// mutation gap: hunt 2/5595 (the empty-slot continue deleted)
static void test_gaps_st_ring_sweep_cursor_rests_on_empty_slot()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 4, 3 );

    double time = 1001.0;
    rocketnet_server_transport_update( server, time );
    st_ring_expect( f->finished_to[0] == 1 && f->finished_to[1] == 1 && f->finished_to[2] == 1, "the first sweep did not serve slots 0-2 once each" );
    st_ring_expect( server->maintenance_next_client == 3, "the sweep's cursor did not rest on the empty slot 3 it passed last" );

    st_ring_connect( server, 3 );
    f->capacity = 1;
    f->first_payload_client = -1;
    time += 1.0;
    rocketnet_server_transport_update( server, time );

    st_ring_expect( f->first_payload_client == 3, "the slot connected at the sweep's cursor was not served first by the next sweep" );

    f->capacity = 64;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The replay sink the update hands the walk carries the transport's forget:
// a client seen connected and then gone is forgotten at the next update,
// with its address.
// mutation gap: hunt 2/5653 (replay_sink.forget = ... deleted)
static void test_gaps_st_ring_replay_walk_forgets_a_departed_client()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );

    // the replay operations, and the records the create keeps for them
    rocketnet_unit_check( server->replay_marks == NULL );
    operations.replay_forget = st_ring_replay_forget;
    operations.replay_mark = st_ring_replay_mark;
    operations.replay_commit = st_ring_replay_commit;
    server->replay_marks = rocketnet_server_replay_marks_create( server->context, 2 );
    rocketnet_unit_check( server->replay_marks );

    // a sink without the forget is reported by the walk's assert, named here;
    // the zeroed stack makes an unset forget NULL rather than garbage
    rocketnet_assert_function_t saved = rn_assert_function;
    g_st_ring_asserts = 0;
    rocketnet_assert_function( st_ring_assert_hook );

    double time = 1000.0;
    st_ring_fill_stack_call( 0x00 );
    rocketnet_server_transport_update( server, time );
    st_ring_expect( g_st_ring_asserts == 0, "the replay walk was handed a sink without the transport's forget" );
    st_ring_expect( f->marks >= 1 && f->forgets == 0, "the walk did not mark the connected client" );

    rocketnet_server_transport_disconnect_client( server, 0 );
    time += 1.0 / 60.0;
    st_ring_fill_stack_call( 0x00 );
    rocketnet_server_transport_update( server, time );
    rocketnet_assert_function( saved );

    st_ring_expect( g_st_ring_asserts == 0, "the replay walk was handed a sink without the transport's forget" );
    st_ring_expect( f->forgets == 1, "the departed client was not forgotten through the transport's replay_forget" );
    st_ring_expect( f->forgotten.port == ST_RING_PORT_BASE, "the forget did not name the departed client's address" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// accept_payload takes only this update's batch entries: an index at or
// past num_payloads names an entry left from an earlier update, and is
// refused (the stale sequence is not accepted, the ack debt not raised).
// mutation gap: hunt 2/6212 (k >= 0 && k < n to ||)
// mutation gap: hunt 2/6213 (k < n to <=)
static void test_gaps_st_ring_accept_payload_ignores_stale_entries()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    rocketnet_server_payloads_t * batch = &server->payloads;
    rocketnet_unit_check( batch->capacity >= 6 );

    // an earlier update's entries for the connected client, sequences 5..10,
    // still in the arrays; this update's batch is empty
    for ( int k = 0; k < 6; k++ )
    {
        batch->client_index[k] = 0;
        batch->connection_id[k] = rocketnet_server_protocol_connection_id( &server->protocol, 0 );
        batch->sequence[k] = (uint64_t) ( 5 + k );
    }
    batch->num_payloads = 0;

    const int mapping_index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 );
    rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index );
    const uint64_t debt = server->protocol.acks->endpoints[0].ack_debt;

    rocketnet_server_transport_accept_payload( server, 0 );
    rocketnet_server_transport_accept_payload( server, 5 );
    st_ring_expect( !rocketnet_replay_already_received( &mapping->accepted_session, 5 ) &&
                        !rocketnet_replay_already_received( &mapping->accepted_session, 10 ),
                    "accept_payload accepted a stale entry past an empty batch" );
    st_ring_expect( server->protocol.acks->endpoints[0].ack_debt == debt, "accept_payload of a stale entry raised the ack debt" );

    // one entry this update: index 1 (== num_payloads) is stale

    batch->num_payloads = 1;
    rocketnet_server_transport_accept_payload( server, 1 );
    st_ring_expect( !rocketnet_replay_already_received( &mapping->accepted_session, 6 ), "accept_payload accepted the entry at index num_payloads" );
    rocketnet_server_transport_accept_payload( server, 0 );
    st_ring_expect( rocketnet_replay_already_received( &mapping->accepted_session, 5 ), "accept_payload refused this update's own entry" );

    batch->num_payloads = 0;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The slot-clean check (the reset witness the connection-reset cases use)
// ----------------------------------------------------------------

// A clean slot clears the caller's failure text; a mapping at encryption-map
// index 0 still naming the slot is found; a stale connection id on a free
// slot is named.
// mutation gap: hunt 2/4427 (failure[0] to failure[1])
// mutation gap: hunt 2/4432 (iterator -1 to 0)
// mutation gap: hunt 2/4442 (the connection_ids member name deleted)
static void test_gaps_st_ring_slot_clean_check()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 0 );

    char failure[256];
    memset( failure, 'x', sizeof( failure ) );
    st_ring_expect( rocketnet_server_transport_slot_clean_for_test( server, 1, failure, sizeof( failure ) ), "a never-used slot was not clean" );
    st_ring_expect( failure[0] == '\0', "a clean result left the caller's failure text in place" );

    // a mapping at map index 0 whose control names slot 1, slot 1 not connected

    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    address.port = 3001;
    const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 1000.0 );
    st_ring_expect( index == 0, "the first mapping of an empty map was not at index 0" );
    rocketnet_encryption_map_control( server->protocol.encryption_map, index )->client_index = 1;
    memset( failure, 0, sizeof( failure ) );
    st_ring_expect( !rocketnet_server_transport_slot_clean_for_test( server, 1, failure, sizeof( failure ) ), "a mapping at map index 0 naming the slot was not found" );
    st_ring_expect( strstr( failure, "rocketnet_encryption_mapping_control_t.client_index" ) != NULL, "the failure did not name the mapping's client_index" );
    rocketnet_encryption_map_control( server->protocol.encryption_map, index )->client_index = -1;

    // a stale connection id on the free slot

    server->protocol.connection_ids[1] = 77;
    memset( failure, 0, sizeof( failure ) );
    st_ring_expect( !rocketnet_server_transport_slot_clean_for_test( server, 1, failure, sizeof( failure ) ), "a stale connection id on a free slot passed as clean" );
    st_ring_expect( strstr( failure, "rocketnet_server_protocol_t.connection_ids" ) != NULL, "the failure did not name the stale connection id" );
    server->protocol.connection_ids[1] = 0;

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

REGISTER_UNIT_TEST( test_gaps_st_ring_unfinished_slot_sends_nothing );
REGISTER_UNIT_TEST( test_gaps_st_ring_aborted_slot_sends_nothing );
REGISTER_UNIT_TEST( test_gaps_st_ring_aborted_slot_does_not_shift_charges );
REGISTER_UNIT_TEST( test_gaps_st_ring_refused_start_zeroes_context );
REGISTER_UNIT_TEST( test_gaps_st_ring_staged_start_id_starts_at_zero );
REGISTER_UNIT_TEST( test_gaps_st_ring_staged_seal_failure_aborts );
REGISTER_UNIT_TEST( test_gaps_st_ring_start_payload_refusal_aborts_once );
REGISTER_UNIT_TEST( test_gaps_st_ring_abort_send_frame_by_transport_kind );
REGISTER_UNIT_TEST( test_gaps_st_ring_full_staging_keeps_the_burst );
REGISTER_UNIT_TEST( test_gaps_st_ring_deferred_burst_charges_no_slot );
REGISTER_UNIT_TEST( test_gaps_st_ring_immediate_burst_charges_its_slot );
REGISTER_UNIT_TEST( test_gaps_st_ring_cursor_falls_with_the_head );
REGISTER_UNIT_TEST( test_gaps_st_ring_abandon_resets_the_cursor );
REGISTER_UNIT_TEST( test_gaps_st_ring_begin_update_while_stopping_opens_nothing );
REGISTER_UNIT_TEST( test_gaps_st_ring_oversize_datagram_accounting );
REGISTER_UNIT_TEST( test_gaps_st_ring_sweep_cursor_rests_on_empty_slot );
REGISTER_UNIT_TEST( test_gaps_st_ring_replay_walk_forgets_a_departed_client );
REGISTER_UNIT_TEST( test_gaps_st_ring_accept_payload_ignores_stale_entries );
REGISTER_UNIT_TEST( test_gaps_st_ring_slot_clean_check );
