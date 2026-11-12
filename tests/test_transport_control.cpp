/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Compile the actual transport here to seed protocol state and substitute a
// deterministic transport.
#include "native_shutdown.h"
#include "../sdk/rocketnet_server_protocol.h"
#include "../sdk/rocketnet_replay.h"
// This target compiles the transport into its own deterministic fixture.
#include "../sdk/rocketnet_server_transport.cpp"
// and the client transport, for the connection reset's write set
#include "../sdk/rocketnet_client_transport.cpp"
#include "fake_client_driver.h"
#include "rocketnet_connect_token.h"
#include "loopback_hub.h"

#define CHECK( x ) do { if (!(x)) { fprintf(stderr, "control check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

// The fake staged frame must satisfy the public packet lease's 16-byte
// alignment contract. Its intentional struct padding triggers MSVC C4324.
#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4324 )
#endif // #if defined( _MSC_VER )
struct control_fixture_t
{
    rocketnet_address_t request_address;
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    int requests;
    int responses;
    int payloads;
    int aborts;
    int start_calls;
    int disconnects;
    int pending_disconnects;
    bool staged;
    int keep_alives[ROCKETNET_MAX_CLIENTS];
    int acks[ROCKETNET_MAX_CLIENTS];
    int capacity;
    int used;
    uint8_t * lease_frames;
    int lease_starts;
    rocketnet_address_t reserved_to;
    alignas( 16 ) uint8_t frame[ROCKETNET_FRAME_STRIDE];
    rocketnet_server_socket_process_packets_t received;     // a view of the received_ arrays
    rocketnet_address_t received_from[4];
    int received_bytes[4];
    uint8_t * received_data[4];
    int feed_count;
    rocketnet_address_t feed_from[4];
    uint8_t feed_data[4][ROCKETNET_RECEIVE_CEILING];
    int feed_bytes[4];
    bool unreachable;
    uint16_t unreachable_port;
    int accepted[4];
    int num_accepted;
    rocketnet_server_socket_process_packets_t * crafted;    // the staged view a guard case hands the update, NULL for none
    int crafted_count;                                      // the count an unstaged guard case's receive_batch returns
    int crafted_entries;                                    // the leading entries whose byte counts crafted_bytes gives
    int crafted_bytes[8];
    int crafted_capacity;                                   // the slots the update asked receive_batch for
    int room;                                               // send_room's answer on the buffered path (sweep cases)
    int sent_count;                                         // keep-alives and acks recorded, in send order
    uint16_t sent_ports[512];                               // the port each one went to
};
#if defined( _MSC_VER )
#pragma warning( pop )
#endif // #if defined( _MSC_VER )

static void record( control_fixture_t * f, const rocketnet_address_t * to, const uint8_t * packet )
{
    if ( to->port == f->request_address.port )
    {
        ++f->responses;
        return;
    }

    const int client = to->port - 1000;
    CHECK( client >= 0 && client < ROCKETNET_MAX_CLIENTS );
    if ( ( packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET || packet[0] == ROCKETNET_CONNECTION_ACK_PACKET ) &&
         f->sent_count < (int) ( sizeof( f->sent_ports ) / sizeof( f->sent_ports[0] ) ) )
    {
        f->sent_ports[f->sent_count++] = to->port;
    }
    if ( packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET )
    {
        ++f->keep_alives[client];
    }
    else if ( packet[0] == ROCKETNET_CONNECTION_ACK_PACKET )
    {
        ++f->acks[client];
    }
    else if ( packet[0] == ROCKETNET_CONNECTION_PAYLOAD_PACKET )
    {
        ++f->payloads;
    }
    else if ( packet[0] == ROCKETNET_CONNECTION_DISCONNECT_PACKET )
    {
        if ( f->staged )
        {
            ++f->pending_disconnects;
        }
        else
        {
            ++f->disconnects;
        }
    }
    else
    {
        CHECK( false );
    }
}
static void fixture_destroy( void * ) {}
static void fixture_stop( void * ) {}
static bool fixture_stopped( void * ) { return true; }
static void send_batch( void * p, const rocketnet_platform_send_t * sends, int count )
{
    CHECK( count > 0 && count <= rocketnet_server_send_batch_packets( ROCKETNET_MAX_CLIENTS ) );
    for ( int i = 0; i < count; ++i )
    {
        record( (control_fixture_t *) p, sends[i].to, sends[i].packet_data );
    }
}
static int receive_batch( void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity )
{
    auto * f = (control_fixture_t *) p;
    if ( f->feed_count )
    {
        // fed client packets, delivered once
        const int n = f->feed_count;
        CHECK( n <= capacity );

        for ( int i = 0; i < n; ++i )
        {
            from[i] = f->feed_from[i];
            memcpy( data + stride * i, f->feed_data[i], (size_t) f->feed_bytes[i] );
            bytes[i] = f->feed_bytes[i];
        }

        f->feed_count = 0;
        return n;
    }

    CHECK( f->requests <= capacity );

    for ( int i = 0; i < f->requests; ++i )
    {
        from[i] = f->request_address;
        memcpy( data + stride * i, f->request, sizeof( f->request ) );
        bytes[i] = sizeof( f->request );
    }

    return f->requests;
}
static uint8_t * start_packet( void * p, const rocketnet_address_t * to, uint64_t * id )
{
    auto * f = (control_fixture_t *) p;
    ++f->start_calls;

    if ( f->unreachable || ( f->unreachable_port && to->port == f->unreachable_port ) )
    {
        *id = ROCKETNET_TRANSPORT_PACKET_UNREACHABLE;
        return NULL;
    }

    if ( f->lease_frames )
    {
        if ( f->lease_starts == f->capacity )
        {
            return NULL;
        }
        f->reserved_to = *to;
        *id = (uint64_t) f->lease_starts;
        return f->lease_frames + (size_t) f->lease_starts++ * ROCKETNET_FRAME_STRIDE;
    }

    if ( f->used == f->capacity )
    {
        return NULL;
    }
    f->reserved_to = *to;
    *id = (uint64_t) f->used;

    return f->frame;
}
static void finish_packet( void * p, uint64_t, uint8_t * frame, int )
{
    auto * f = (control_fixture_t *) p;
    record( f, &f->reserved_to, frame + ROCKETNET_FRAME_WIRE_OFFSET );
    ++f->used;
}
static void abort_packet( void * p, uint64_t, uint8_t * ) { ++( (control_fixture_t *) p )->aborts; }
static void client_accepted( void * p, int view_index )
{
    auto * f = (control_fixture_t *) p;
    CHECK( f->num_accepted < 4 );
    f->accepted[f->num_accepted++] = view_index;
}
static void flush( void * p )
{
    auto * f = (control_fixture_t *) p;
    f->disconnects += f->pending_disconnects;
    f->pending_disconnects = 0;
    f->used = 0;
}
static rocketnet_server_socket_process_packets_t * receive( void * p )
{
    auto * f = (control_fixture_t *) p;
    f->received.capacity = 4;
    f->received.from = f->received_from;
    f->received.packet_bytes = f->received_bytes;
    f->received.packet_data = f->received_data;
    if ( f->feed_count )
    {
        // fed client packets, delivered once
        f->received.num_packets = f->feed_count;
        for ( int i = 0; i < f->feed_count; ++i )
        {
            f->received.from[i] = f->feed_from[i];
            f->received.packet_data[i] = f->feed_data[i];
            f->received.packet_bytes[i] = f->feed_bytes[i];
        }

        f->feed_count = 0;
    }
    else
    {
        f->received.num_packets = 0;
    }

    return &f->received;
}

static rocketnet_server_transport_t * make_server( control_fixture_t * f, rocketnet_server_transport_io_operations_t * operations, int clients, bool staged )
{
    f->staged = staged;

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = clients;

    // the real adapter is closed immediately below; all behavior under test
    // runs through the deterministic buffered or staged fixture operations

    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = rocketnet_test_loopback_hub();

    auto * server = rocketnet_server_transport_create_current( NULL, "127.0.0.1:0", &config );
    CHECK( server );
    rn_native_server_io_stop_and_close( &server->io );
    *operations = {};
    operations->destroy = fixture_destroy;
    operations->stop = fixture_stop;
    operations->stopped = fixture_stopped;
    operations->send_batch = send_batch;
    operations->receive_batch = receive_batch;

    if ( staged )
    {
        operations->flags = ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE;
        operations->start_packet = start_packet;
        operations->finish_packet = finish_packet;
        operations->abort_packet = abort_packet;
        operations->flush = flush;
        operations->receive = receive;
    }

    server->io.instance = f;
    server->io.operations = operations;

    for ( int i = 0; i < clients; ++i )
    {
        rocketnet_address_t address;
        CHECK( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
        address.port = (uint16_t) ( 1000 + i );

        int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 100.0 );
        CHECK( index >= 0 );

        auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, index );
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
        const uint64_t owner_generation =
            rocketnet_encryption_map_owner_generation( server->protocol.encryption_map, index );
        CHECK( rocketnet_client_slots_assign( server->protocol.client_slots, (uint64_t) i + 1, index,
                                              owner_generation, user_data ) == i );
        mapping->control->client_index = i;
        mapping->client_id = (uint64_t) i + 1;
        mapping->control->established = 1;
        mapping->control->timeout_seconds = 1000;

        // Synthetic connections have test keys; this fixture does not
        // model authentication. It exercises the real update/send scheduler.

        memset( &mapping->keys, i + 1, sizeof( mapping->keys ) );
    }

    CHECK( rocketnet_address_parse( &f->request_address, "127.0.0.1:20000" ) );

    int pending = rocketnet_encryption_map_add( server->protocol.encryption_map, &f->request_address, 100.0 );
    CHECK( pending >= 0 );

    auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, pending );
    mapping->challenge_packet[0] = ROCKETNET_CONNECTION_CHALLENGE_PACKET;

    uint8_t * cursor = f->request;
    rocketnet_write_prefilter_header( &cursor, ROCKETNET_CONNECTION_REQUEST_PACKET, server->protocol.packet_filter );

    return server;
}

static void seed_stats_connection_ids( rocketnet_server_transport_t * server, int clients )
{
    // The fixture assigns slots directly. Production assigns these generation
    // ids at connect.

    for ( int i = 0; i < clients; ++i )
    {
        server->protocol.connection_ids[i] = ++server->protocol.next_connection_id;
    }
}

static void seed_stats_rtt_samples( rocketnet_server_transport_t * server, int clients )
{
    // generate the sample through the ACK producer, so the endpoint's running
    // stats see the same 17 ms observation a real ack would give them

    rocketnet_ack_block_t block = {};
    block.first_range_length = 1;

    for ( int i = 0; i < clients; ++i )
    {
        auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map,
                                                         rocketnet_client_slots_mapping_index( server->protocol.client_slots, i ) );
        CHECK( mapping );

        // account for the synthetic send so maintenance uses the next wire sequence

        const uint64_t sequence = mapping->send_sequence_session++;
        block.largest = sequence;

        auto * endpoint = &server->protocol.acks->endpoints[i];
        CHECK( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, 100, 0, 0, 0.0 ) == ROCKETNET_ACK_SENT_RECORDED );

        uint64_t acked_ids[1] = {};
        uint64_t unknown = 0, batch_full = 0;
        int acked_count = 0;
        CHECK( rocketnet_ack_endpoint_process_block( endpoint, &block, 0.017,
                                                     acked_ids, 1, &acked_count, &unknown, &batch_full ) == 1 );
        CHECK( acked_count == 0 && unknown == 0 && batch_full == 0 );
        CHECK( endpoint->rtt_history[sequence % ROCKETNET_RTT_HISTORY_SIZE] == 17.0f );
    }
}

static void test_buffered_control_pressure( int clients = 300 )
{
    auto * f = new control_fixture_t();
    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, clients, false );
    seed_stats_connection_ids( server, clients );
    seed_stats_rtt_samples( server, clients );
    f->requests = ROCKETNET_SERVER_MAX_RECEIVE_PACKETS;

    // the per-slot stats are current as the acks land (proofs/tla/RNServerStats.tla):
    // every slot's RTT is checked after a few updates, and the response and
    // ACK/keep-alive checks run on every individual update

    for ( int step = 0; step < 4; ++step )
    {
        double now = 1.0 + step;

        // Half the clients owe standalone ACKs while their keep-alive clock
        // is not due; the other half need keep-alives. All indices are due
        // for one kind of control packet on every update.

        for ( int i = 1; i < clients; i += 2 )
        {
            auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map,
                                                             rocketnet_client_slots_mapping_index( server->protocol.client_slots, i ) );
            mapping->last_keep_alive_send_time = now;
            server->protocol.acks->endpoints[i].ack_debt = ROCKETNET_ACK_DEBT_THRESHOLD;
        }

        rocketnet_server_transport_update( server, now );
        CHECK( f->responses == ( step + 1 ) * f->requests );
        for ( int i = 0; i < clients; ++i )
        {
            CHECK( ( i % 2 ? f->acks[i] : f->keep_alives[i] ) == step + 1 );
        }
    }

    for ( int i = 0; i < clients; ++i )
    {
        CHECK( server->protocol.acks->endpoints[i].rtt == 17.0f );
    }
    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_buffered_control_pressure_%d_clients\n", clients );
}

static void test_disconnect_burst_next_send( bool staged )
{
    auto * f = new control_fixture_t();
    f->capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 1;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 1, staged );
    rocketnet_server_transport_update( server, 1.0 );

    const int stats_index = server->current_stats_index;
    const uint64_t before = server->stats_frames[stats_index].packets_sent;

    rocketnet_server_transport_disconnect_client( server, 0 );
    CHECK( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    CHECK( f->disconnects == 0 );
    CHECK( server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

    rocketnet_server_transport_send_packets( server );
    CHECK( f->disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    CHECK( server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_disconnect_burst_next_send_%s\n", staged ? "staged" : "portable" );
}

static void test_staged_control_fairness()
{
    auto * f = new control_fixture_t();
    f->capacity = 2;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 12, true );
    seed_stats_connection_ids( server, 12 );
    seed_stats_rtt_samples( server, 12 );

    // every slot remains due, but only two frames are available per update

    for ( int step = 0; step < 6; ++step )
    {
        rocketnet_server_transport_update( server, 1.0 + step );
    }

    for ( int i = 0; i < 12; ++i )
    {
        CHECK( f->keep_alives[i] == 1 );
        CHECK( server->protocol.acks->endpoints[i].rtt == 17.0f );
    }

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_staged_control_fairness\n" );
}

static void test_staged_unreachable_client_does_not_pin_sweep()
{
    // proofs/tla/RNServerMaintenanceSweep.tla BoundedWait, on the code. The
    // transport refuses every start to client 0 for want of a link-layer
    // destination, with room left. The sweep counts SEND_NO_LINK_ADDRESS once
    // per update and still serves clients 1 and 2 in the same update; client
    // 0's keep-alive stays due. Mutation that must bite: break on the
    // unreachable refusal as on a full staging; the sweep then stops at
    // client 0 every update and clients 1 and 2 are never served. Time is an
    // argument.

    auto * f = new control_fixture_t();
    f->capacity = 16;
    f->unreachable_port = 1000;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 3, true );
    seed_stats_connection_ids( server, 3 );
    seed_stats_rtt_samples( server, 3 );

    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t ring_full = counters[ROCKETNET_COUNTER_SEND_RING_FULL];
    const uint64_t no_link = counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS];

    for ( int step = 0; step < 3; ++step )
    {
        rocketnet_server_transport_update( server, 1.0 + step );
        CHECK( f->keep_alives[0] == 0 );
        CHECK( f->keep_alives[1] == step + 1 );
        CHECK( f->keep_alives[2] == step + 1 );
        CHECK( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == no_link + (uint64_t) step + 1 );
    }

    CHECK( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_staged_unreachable_client_does_not_pin_sweep\n" );
}

static void test_staged_idle_maintenance_preflight()
{
    // A connected but idle slot should not touch staged frame ownership.
    // The boundary, the pre-ACK keep-alive clock, the post-ACK send clock,
    // and ACK debt each still request exactly one frame when due.

    auto * f = new control_fixture_t();
    f->capacity = 16;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 1, true );
    server->protocol.send_rate = 1.0;

    auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map,
                                                     rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 ) );
    mapping->control->last_receive_time = 1000.0;
    mapping->last_keep_alive_send_time = 1000.0;
    mapping->last_send_time = 1000.0;

    rocketnet_server_transport_update( server, 1000.0 );
    rocketnet_server_transport_update( server, 1000.5 );
    CHECK( f->start_calls == 0 && f->aborts == 0 );

    // before any keep-alive is acknowledged, ordinary traffic does not
    // suppress the keep-alive at the exact one-second boundary

    mapping->last_send_time = 1000.75;
    rocketnet_server_transport_update( server, 1001.0 );
    CHECK( f->start_calls == 1 && f->keep_alives[0] == 1 && f->acks[0] == 0 );

    // once acknowledged, any send stamps the clock used for suppression

    server->protocol.acks->endpoints[0].keep_alive_acked = 1;
    mapping->last_send_time = 1001.0;
    rocketnet_server_transport_update( server, 1001.5 );
    CHECK( f->start_calls == 1 && f->aborts == 0 );
    rocketnet_server_transport_update( server, 1002.0 );
    CHECK( f->start_calls == 2 && f->keep_alives[0] == 2 );

    // a single owed ACK is due before the next paced keep-alive

    server->protocol.acks->endpoints[0].ack_debt = 1;
    rocketnet_server_transport_update( server, 1002.25 );
    CHECK( f->start_calls == 3 && f->acks[0] == 1 );
    CHECK( server->protocol.acks->endpoints[0].ack_debt == 0 );

    // when both are due, the existing writer keeps keep-alive priority and
    // its ACK block clears the debt without a second reservation

    server->protocol.acks->endpoints[0].ack_debt = 1;
    rocketnet_server_transport_update( server, 1003.25 );
    CHECK( f->start_calls == 4 && f->keep_alives[0] == 3 && f->acks[0] == 1 );
    CHECK( server->protocol.acks->endpoints[0].ack_debt == 0 && f->aborts == 0 );

    // The slot API permits a connected slot with no mapping. The preflight
    // must still count that broken state, without indexing
    // an address at -1 or reserving a staged frame.

    rocketnet_client_slots_release( server->protocol.client_slots, 0 );
    rocketnet_client_slots_publish( server->protocol.client_slots );     // the update's end

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    CHECK( rocketnet_client_slots_assign( server->protocol.client_slots, 9, -1, 9, user_data ) == 0 );

    const uint64_t missing_before = server->protocol.counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING];
    rocketnet_server_transport_update( server, 1003.5 );
    CHECK( server->protocol.counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING] == missing_before + 1 );
    CHECK( f->start_calls == 4 );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_staged_idle_maintenance_preflight\n" );
}

static void test_staged_lease_table_full()
{
    // Keep every staged frame outstanding. Distinct 16-aligned frames make
    // this a real lease-capacity check rather than a hash-collision stress.

    const int lease_limit = 131072 / 2;
    auto * f = new control_fixture_t();
    f->capacity = lease_limit + 1;
    f->lease_frames = (uint8_t *) rocketnet_malloc( NULL, (size_t) f->capacity * ROCKETNET_FRAME_STRIDE );
    CHECK( f->lease_frames );

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 1, true );

    for ( int i = 0; i < lease_limit; ++i )
    {
        rocketnet_packet_context_t context;
        CHECK( rocketnet_server_transport_start_packet( server, 0, 64, &context ) );
    }

    rocketnet_packet_context_t refused;
    CHECK( !rocketnet_server_transport_start_packet( server, 0, 64, &refused ) );
    CHECK( server->protocol.counters->values[ROCKETNET_COUNTER_SEND_LEASE_FULL] == 1 );
    CHECK( f->aborts == 1 );
    rn_native_server_transport_stop_and_destroy( server );
    rocketnet_free( NULL, f->lease_frames );
    delete f;
    printf( "test_staged_lease_table_full\n" );
}

static void test_entries_carry_connection_id( bool staged )
{
    // standard/MESSAGES.md m1.4 rule 5: the transport's own batches carry each
    // entry's connection id beside its client index. Two slots hold
    // connection ids 1 and 2; the server sends each client three packets
    // through the public send path, then each client's sealed payload,
    // whose ack block acknowledges those three, arrives in one update. Read
    // through the public accessors, the payload and acked id batches run
    // parallel to their client index arrays, and every entry's id is the id
    // its slot holds. Mutation that must bite: drop the tag copy at the
    // transport's payload append sites (buffered or staged); the payload id
    // checks go red. Time is an argument; the clock starts at 1000.0.

    auto * f = new control_fixture_t();
    f->capacity = 16;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 2, staged );
    auto * protocol = &server->protocol;
    const double time = 1000.0;

    // the fixture assigns its slots directly, so each takes its id here the
    // way the protocol takes one when it assigns a slot

    for ( int i = 0; i < 2; ++i )
    {
        protocol->connection_ids[i] = ++protocol->next_connection_id;

        auto * mapping = rocketnet_encryption_map_entry( protocol->encryption_map,
                                                         rocketnet_client_slots_mapping_index( protocol->client_slots, i ) );
        mapping->control->last_receive_time = time;
    }

    CHECK( rocketnet_server_transport_client_connection_id( server, 0 ) == 1 );
    CHECK( rocketnet_server_transport_client_connection_id( server, 1 ) == 2 );

    uint64_t sequences[2][3];

    for ( int c = 0; c < 2; ++c )
    {
        for ( int k = 0; k < 3; ++k )
        {
            rocketnet_packet_context_t context;
            CHECK( rocketnet_server_transport_start_packet( server, c, 8, &context ) );

            int capacity = 0;
            uint8_t * payload = rocketnet_server_transport_packet_payload( server, &context, &capacity );
            CHECK( payload && capacity >= 8 );
            memset( payload, 0x50 + k, 8 );

            // the server assigns the sequence when finish accepts the payload
            // (test_unit_sequence_burn.cpp; proofs/tla/SessionAcks.tla
            // BurnedNeverNamed), so it is read after finish, never at start

            CHECK( rocketnet_server_transport_finish_packet( server, &context, 8 ) > 0 );
            sequences[c][k] = context.sequence;
            CHECK( sequences[c][k] != 0 );
        }
    }

    rocketnet_server_transport_send_packets( server );

    for ( int c = 0; c < 2; ++c )
    {
        auto * mapping = rocketnet_encryption_map_entry( protocol->encryption_map,
                                                         rocketnet_client_slots_mapping_index( protocol->client_slots, c ) );
        rocketnet_replay_protection_t replay;
        memset( &replay, 0, sizeof( replay ) );
        for ( int k = 0; k < 3; ++k )
        {
            rocketnet_replay_advance( &replay, sequences[c][k] );
        }

        uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 4];
        const int block = rocketnet_ack_block_build( plaintext, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay );
        CHECK( block > 0 );
        memset( plaintext + block, 0xA0 + c, 4 );
        CHECK( rocketnet_address_parse( &f->feed_from[c], "127.0.0.1:0" ) );
        f->feed_from[c].port = (uint16_t) ( 1000 + c );
        f->feed_bytes[c] = rocketnet_write_sealed_packet( f->feed_data[c], ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                          protocol->packet_filter, 1, NULL, plaintext, block + 4, protocol->protocol_id,
                                                          mapping->connection_salt, NULL, mapping->keys.session_client_to_server );
        CHECK( f->feed_bytes[c] > 0 );
    }

    f->feed_count = 2;
    rocketnet_server_transport_update( server, time + 0.001 );

    const int * client_index = NULL;
    uint8_t * const * payload_data = NULL;
    const int * payload_bytes = NULL;
    const uint64_t * connection_id = NULL;
    const int num_payloads = rocketnet_server_transport_payloads( server, &client_index, &payload_data, &payload_bytes );
    CHECK( num_payloads == 2 );
    CHECK( rocketnet_server_transport_payload_connection_ids( server, &connection_id ) == num_payloads );

    for ( int i = 0; i < num_payloads; ++i )
    {
        CHECK( payload_bytes[i] == 4 && payload_data[i][0] == 0xA0 + client_index[i] );
        CHECK( connection_id[i] != 0 );
        CHECK( connection_id[i] == rocketnet_server_transport_client_connection_id( server, client_index[i] ) );
    }

    const int * acked_client_index = NULL;
    const uint64_t * acked_ids = NULL;
    const int num_acked = rocketnet_server_transport_acked_packets( server, &acked_client_index, &acked_ids );
    CHECK( num_acked == 6 );
    CHECK( rocketnet_server_transport_acked_connection_ids( server, &connection_id ) == num_acked );

    int acked_per_client[2] = {};

    for ( int i = 0; i < num_acked; ++i )
    {
        CHECK( acked_client_index[i] >= 0 && acked_client_index[i] < 2 );
        ++acked_per_client[acked_client_index[i]];
        CHECK( connection_id[i] != 0 );
        CHECK( connection_id[i] == rocketnet_server_transport_client_connection_id( server, acked_client_index[i] ) );
    }

    CHECK( acked_per_client[0] == 3 && acked_per_client[1] == 3 );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_entries_carry_connection_id_%s\n", staged ? "staged" : "portable" );
}

static void test_staged_link_address_hooks()
{
    // TRANSPORTS.md, xdp (the reply-MAC rule), from the transport's side. Fed
    // one update holding a packet from an unknown address (dropped) and a
    // payload that opens under client 0's keys (view index 1), the transport
    // tells the transport about the accepted packet only. Then the transport
    // refuses every start for want of a link-layer destination: the
    // transport counts SEND_NO_LINK_ADDRESS, never SEND_RING_FULL, and sends
    // nothing. Mutation that must bite: drop the client_accepted call, or the
    // unreachable test in the start wrapper; a check below goes red. Time
    // is an argument; the clock starts at 1000.0.

    auto * f = new control_fixture_t();
    f->capacity = 16;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 1, true );
    operations.client_accepted = client_accepted;

    auto * protocol = &server->protocol;
    const double time = 1000.0;
    protocol->connection_ids[0] = ++protocol->next_connection_id;

    auto * mapping = rocketnet_encryption_map_entry( protocol->encryption_map,
                                                     rocketnet_client_slots_mapping_index( protocol->client_slots, 0 ) );
    mapping->control->last_receive_time = time;

    CHECK( rocketnet_address_parse( &f->feed_from[0], "127.0.0.1:3000" ) );
    memset( f->feed_data[0], 0x5A, 64 );
    f->feed_bytes[0] = 64;

    rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 4];
    const int block = rocketnet_ack_block_build( plaintext, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay );
    CHECK( block > 0 );
    memset( plaintext + block, 0xA0, 4 );
    CHECK( rocketnet_address_parse( &f->feed_from[1], "127.0.0.1:1000" ) );
    f->feed_bytes[1] = rocketnet_write_sealed_packet( f->feed_data[1], ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                      protocol->packet_filter, 1, NULL, plaintext, block + 4, protocol->protocol_id,
                                                      mapping->connection_salt, NULL, mapping->keys.session_client_to_server );
    CHECK( f->feed_bytes[1] > 0 );
    f->feed_count = 2;
    rocketnet_server_transport_update( server, time + 0.001 );
    CHECK( f->num_accepted == 1 && f->accepted[0] == 1 );

    const int keep_alives = f->keep_alives[0], acks = f->acks[0], payloads = f->payloads;
    f->unreachable = true;

    const uint64_t ring_full = protocol->counters->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    rocketnet_packet_context_t context;
    CHECK( !rocketnet_server_transport_start_packet( server, 0, 64, &context ) );
    CHECK( protocol->counters->values[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == 1 );
    rocketnet_server_transport_update( server, time + 1.0 );
    CHECK( protocol->counters->values[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] > 1 );
    CHECK( protocol->counters->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full );
    CHECK( f->keep_alives[0] == keep_alives && f->acks[0] == acks && f->payloads == payloads );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_staged_link_address_hooks\n" );
}

static void test_staged_link_address_taught_at_view_zero()
{
    // The reply-MAC rule at the first receive view: the payload that opens
    // under client 0's keys is view 0 of the update and the unknown address's
    // packet is view 1. The transport is told about view 0 and only view 0,
    // so the client's link-layer source is learnt wherever its packet sits
    // in the batch. Time is an argument; the clock starts at 1000.0.

    auto * f = new control_fixture_t();
    f->capacity = 16;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 1, true );
    operations.client_accepted = client_accepted;

    auto * protocol = &server->protocol;
    const double time = 1000.0;
    protocol->connection_ids[0] = ++protocol->next_connection_id;

    auto * mapping = rocketnet_encryption_map_entry( protocol->encryption_map,
                                                     rocketnet_client_slots_mapping_index( protocol->client_slots, 0 ) );
    mapping->control->last_receive_time = time;

    rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 4];
    const int block = rocketnet_ack_block_build( plaintext, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay );
    CHECK( block > 0 );
    memset( plaintext + block, 0xA0, 4 );
    CHECK( rocketnet_address_parse( &f->feed_from[0], "127.0.0.1:1000" ) );
    f->feed_bytes[0] = rocketnet_write_sealed_packet( f->feed_data[0], ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                      protocol->packet_filter, 1, NULL, plaintext, block + 4, protocol->protocol_id,
                                                      mapping->connection_salt, NULL, mapping->keys.session_client_to_server );
    CHECK( f->feed_bytes[0] > 0 );

    CHECK( rocketnet_address_parse( &f->feed_from[1], "127.0.0.1:3000" ) );
    memset( f->feed_data[1], 0x5A, 64 );
    f->feed_bytes[1] = 64;
    f->feed_count = 2;

    rocketnet_server_transport_update( server, time + 0.001 );
    CHECK( f->num_accepted == 1 && f->accepted[0] == 0 );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_staged_link_address_taught_at_view_zero\n" );
}

static void test_staged_refused_response_is_counted()
{
    // A connection request elicits a response; the staged transport refuses
    // its frame. Every refused start is counted once, by what refused it: a
    // full staging is SEND_RING_FULL, a refusal for want of a link-layer
    // destination is SEND_NO_LINK_ADDRESS, and nothing is sent. Then the
    // transport has room and the response goes out uncounted. Client 0's
    // keep-alive is kept not due so the receive path's response is the only
    // start. Mutation that must bite: drop the receive path's count of a
    // refused response frame. Time is an argument.

    auto * f = new control_fixture_t();
    f->capacity = 0;

    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 1, true );

    auto * protocol = &server->protocol;
    const uint64_t * counters = protocol->counters->values;
    auto * mapping = rocketnet_encryption_map_entry( protocol->encryption_map,
                                                     rocketnet_client_slots_mapping_index( protocol->client_slots, 0 ) );

    for ( int step = 0; step < 3; ++step )
    {
        const double now = 1.0 + step;
        mapping->last_keep_alive_send_time = now;

        if ( step == 1 )
        {
            f->capacity = 16;
            f->unreachable_port = f->request_address.port;
        }
        else if ( step == 2 )
        {
            f->unreachable_port = 0;
        }

        const uint64_t ring_full = counters[ROCKETNET_COUNTER_SEND_RING_FULL];
        const uint64_t no_link = counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS];
        const int start_calls = f->start_calls;

        f->feed_from[0] = f->request_address;
        memcpy( f->feed_data[0], f->request, sizeof( f->request ) );
        f->feed_bytes[0] = (int) sizeof( f->request );
        f->feed_count = 1;
        rocketnet_server_transport_update( server, now );

        CHECK( f->start_calls == start_calls + 1 );
        CHECK( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + ( step == 0 ? 1 : 0 ) );
        CHECK( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == no_link + ( step == 1 ? 1 : 0 ) );
        CHECK( f->responses == ( step == 2 ? 1 : 0 ) );
    }

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_staged_refused_response_is_counted\n" );
}

static void test_deferred_accept_reads_only_this_batch()
{
    // A deferring application accepts payloads by their index in this
    // update's batch. An index outside the batch accepts nothing, even where a
    // slot past the batch still holds an entry for the same live connection
    // from an earlier update: no sequence is marked received and no ACK is
    // owed. The entry inside the batch is accepted once. Mutation that must
    // bite: let the index reach the slot past the batch. Time is an argument.

    auto * f = new control_fixture_t();
    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 1, false );
    seed_stats_connection_ids( server, 1 );
    rocketnet_server_transport_defer_payload_acks( server );
    rocketnet_server_transport_update( server, 1.0 );

    auto * protocol = &server->protocol;
    auto * mapping = rocketnet_encryption_map_entry( protocol->encryption_map,
                                                     rocketnet_client_slots_mapping_index( protocol->client_slots, 0 ) );
    const uint64_t connection_id = protocol->connection_ids[0];

    struct rocketnet_server_payloads_t * batch = &server->payloads;
    CHECK( batch->capacity >= 2 );
    batch->client_index[0] = 0;
    batch->connection_id[0] = connection_id;
    batch->sequence[0] = 5;
    batch->client_index[1] = 0;
    batch->connection_id[1] = connection_id;
    batch->sequence[1] = 9;
    batch->num_payloads = 1;

    const auto debt = protocol->acks->endpoints[0].ack_debt;
    rocketnet_server_transport_accept_payload( server, 1 );
    rocketnet_server_transport_accept_payload( server, -1 );
    CHECK( protocol->acks->endpoints[0].ack_debt == debt );
    CHECK( !rocketnet_replay_already_received( &mapping->accepted_session, 9 ) );

    rocketnet_server_transport_accept_payload( server, 0 );
    CHECK( protocol->acks->endpoints[0].ack_debt == debt + 1 );
    CHECK( rocketnet_replay_already_received( &mapping->accepted_session, 5 ) );
    CHECK( !rocketnet_replay_already_received( &mapping->accepted_session, 9 ) );

    batch->num_payloads = 0;
    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_deferred_accept_reads_only_this_batch\n" );
}

// The receive guards (rocketnet_server_transport_update, both receive paths):
// what the I/O core hands the update is checked before the protocol sees it.
// A missing view, a negative count or a count past the view's capacity, and
// a byte count that is negative or past the bound, is a transport fault: it
// is counted in transport_faults, the bad entry is not processed and gets no
// reply, and every good entry around it is processed exactly once. Each good
// entry here is the fixture's connection request, so each one processed is
// one response; the trailing slots past the count hold requests too, so a
// walk past the count shows as an extra response. Time is an argument.

#define GUARD_BYTES 16
#define GUARD_FILL 0xEE

struct guard_expect_t
{
    uint64_t faults;
    uint64_t too_large;
    uint64_t received;      // packets the update counted received (stats and PACKETS_RECEIVED)
    uint64_t bytes;         // their bytes, wire header included
    int responses;
};

struct guard_counts_t
{
    uint64_t faults;
    uint64_t too_large;
    uint64_t packets_received;
};

static guard_counts_t guard_counts( rocketnet_server_transport_t * server )
{
    guard_counts_t counts;
    counts.faults = server->transport_faults;
    counts.too_large = server->counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];
    counts.packets_received = server->counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    return counts;
}

// one update of a fresh server; its one stats frame holds this update's receives

static void guard_check( rocketnet_server_transport_t * server, control_fixture_t * f, const guard_counts_t & before, const guard_expect_t & expect )
{
    const guard_counts_t after = guard_counts( server );
    const rocketnet_server_stats_frame_t * stats = &server->stats_frames[server->current_stats_index];
    CHECK( after.faults - before.faults == expect.faults );
    CHECK( after.too_large - before.too_large == expect.too_large );
    CHECK( after.packets_received - before.packets_received == expect.received );
    CHECK( stats->packets_received == expect.received );
    CHECK( stats->bytes_received == expect.bytes );
    CHECK( f->responses == expect.responses );
    CHECK( f->aborts == 0 );
}

static rocketnet_server_transport_t * guard_server( control_fixture_t * f, rocketnet_server_transport_io_operations_t * operations, bool staged )
{
    auto * server = make_server( f, operations, 1, staged );

    // client 0's keep-alive is not due, so the requests' responses are the only sends

    auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map,
                                                     rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 ) );
    mapping->last_keep_alive_send_time = 1.0;
    return server;
}

// The staged view: each entry's buffer is its own allocation holding the
// request (or the entry's whole count, when that is larger and within the
// ceiling), followed by guard bytes that must be untouched after the update.

#define STAGED_GUARD_CAPACITY 6

struct staged_guard_batch_t
{
    rocketnet_server_socket_process_packets_t view;
    rocketnet_address_t from[STAGED_GUARD_CAPACITY];
    int bytes[STAGED_GUARD_CAPACITY];
    uint8_t * data[STAGED_GUARD_CAPACITY];
    int sizes[STAGED_GUARD_CAPACITY];
};

static rocketnet_server_socket_process_packets_t * receive_crafted( void * p )
{
    return ( (control_fixture_t *) p )->crafted;
}

static void staged_guard_run( int num_packets, const int * bytes, int entries, const guard_expect_t & expect, bool null_view = false )
{
    auto * f = new control_fixture_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    auto * server = guard_server( f, &operations, true );
    operations.receive = receive_crafted;

    const int request_bytes = (int) sizeof( f->request );
    auto * batch = new staged_guard_batch_t();
    for ( int i = 0; i < STAGED_GUARD_CAPACITY; ++i )
    {
        batch->from[i] = f->request_address;
        batch->bytes[i] = i < entries ? bytes[i] : request_bytes;
        const int claimed = batch->bytes[i];
        batch->sizes[i] = claimed > request_bytes && claimed <= ROCKETNET_RECEIVE_CEILING ? claimed : request_bytes;
        batch->data[i] = (uint8_t *) malloc( (size_t) ( batch->sizes[i] + GUARD_BYTES ) );
        CHECK( batch->data[i] );
        memset( batch->data[i], 0x5A, (size_t) batch->sizes[i] );
        memcpy( batch->data[i], f->request, (size_t) request_bytes );
        memset( batch->data[i] + batch->sizes[i], GUARD_FILL, GUARD_BYTES );
    }
    batch->view.num_packets = num_packets;
    batch->view.capacity = STAGED_GUARD_CAPACITY;
    batch->view.from = batch->from;
    batch->view.packet_bytes = batch->bytes;
    batch->view.packet_data = batch->data;
    f->crafted = null_view ? NULL : &batch->view;

    const guard_counts_t before = guard_counts( server );
    rocketnet_server_transport_update( server, 2.0 );
    guard_check( server, f, before, expect );

    for ( int i = 0; i < STAGED_GUARD_CAPACITY; ++i )
    {
        for ( int j = 0; j < GUARD_BYTES; ++j )
        {
            CHECK( batch->data[i][batch->sizes[i] + j] == GUARD_FILL );
        }
        free( batch->data[i] );
    }
    delete batch;
    rn_native_server_transport_stop_and_destroy( server );
    delete f;
}

static void test_staged_receive_guards()
{
    const int r = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
    const int ceiling = ROCKETNET_RECEIVE_CEILING;
    const uint64_t header = (uint64_t) rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 );
    const uint64_t request = (uint64_t) r + header;
    const int none[1] = { 0 };

    // a missing view, and a negative count: nothing processed, one fault
    staged_guard_run( 0, none, 0, { 1, 0, 0, 0, 0 }, true );
    staged_guard_run( -1, none, 0, { 1, 0, 0, 0, 0 } );

    // an empty view, and a full one: no fault, every entry once
    staged_guard_run( 0, none, 0, { 0, 0, 0, 0, 0 } );
    staged_guard_run( STAGED_GUARD_CAPACITY, none, 0, { 0, 0, STAGED_GUARD_CAPACITY, STAGED_GUARD_CAPACITY * request, STAGED_GUARD_CAPACITY } );

    // a count one past the capacity, and far past it: one fault, the capacity processed once
    staged_guard_run( STAGED_GUARD_CAPACITY + 1, none, 0, { 1, 0, STAGED_GUARD_CAPACITY, STAGED_GUARD_CAPACITY * request, STAGED_GUARD_CAPACITY } );
    staged_guard_run( INT32_MAX, none, 0, { 1, 0, STAGED_GUARD_CAPACITY, STAGED_GUARD_CAPACITY * request, STAGED_GUARD_CAPACITY } );

    // one bad entry first, in the middle and last of five: negative, one past the ceiling, far past it
    const int bad_first[5] = { -1, r, r, r, r };
    const int bad_middle[5] = { r, r, ceiling + 1, r, r };
    const int bad_last[5] = { r, r, r, r, 1 << 20 };
    staged_guard_run( 5, bad_first, 5, { 1, 0, 4, 4 * request, 4 } );
    staged_guard_run( 5, bad_middle, 5, { 1, 1, 4, 4 * request, 4 } );
    staged_guard_run( 5, bad_last, 5, { 1, 1, 4, 4 * request, 4 } );

    // the bounds themselves are good: an empty datagram and one of exactly
    // the ceiling are processed (neither is a request, so no response)
    const int edges[3] = { 0, ceiling, r };
    staged_guard_run( 3, edges, 3, { 0, 0, 3, (uint64_t) ceiling + request + 2 * header, 1 } );

    printf( "test_staged_receive_guards\n" );
}

// A flood of malformed datagrams from a connected client's address: each
// passes the prefilter header as a connection request but is one byte short
// of a request's size, so it fails the shape check. Each is counted
// received and CONNECTION_REQUEST_WRONG_SIZE, and none is looked up in the
// mapping table (the client's own wire totals stay untouched) or takes a
// send frame. The last entry is a well-formed request from the pending
// address, which is looked up and answered. Time is an argument.

static void test_staged_malformed_flood_takes_no_lookup()
{
    auto * f = new control_fixture_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    auto * server = guard_server( f, &operations, true );
    operations.receive = receive_crafted;

    auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map,
                                                     rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 ) );
    mapping->last_keep_alive_send_time = 2.0;

    const int request_bytes = (int) sizeof( f->request );
    auto * batch = new staged_guard_batch_t();
    for ( int i = 0; i < STAGED_GUARD_CAPACITY; ++i )
    {
        const bool last = i == STAGED_GUARD_CAPACITY - 1;
        batch->from[i] = f->request_address;
        if ( !last )
        {
            batch->from[i].port = 1000;
        }
        batch->bytes[i] = last ? request_bytes : request_bytes - 1;
        batch->sizes[i] = request_bytes;
        batch->data[i] = (uint8_t *) malloc( (size_t) request_bytes );
        CHECK( batch->data[i] );
        memcpy( batch->data[i], f->request, (size_t) request_bytes );
    }
    batch->view.num_packets = STAGED_GUARD_CAPACITY;
    batch->view.capacity = STAGED_GUARD_CAPACITY;
    batch->view.from = batch->from;
    batch->view.packet_bytes = batch->bytes;
    batch->view.packet_data = batch->data;
    f->crafted = &batch->view;

    const uint64_t * counters = server->counters.values;
    const uint64_t wrong_size = counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE];
    const uint64_t received = server->server_stats.packets_received;
    const rocketnet_server_wire_totals_t client_before = server->client_wire_totals[0];

    rocketnet_server_transport_update( server, 2.0 );

    const int flood = STAGED_GUARD_CAPACITY - 1;
    CHECK( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] - wrong_size == (uint64_t) flood );
    CHECK( server->server_stats.packets_received - received == (uint64_t) STAGED_GUARD_CAPACITY );
    CHECK( server->client_wire_totals[0].packets_received == client_before.packets_received );
    CHECK( server->client_wire_totals[0].bytes_received == client_before.bytes_received );
    CHECK( f->start_calls == 1 );
    CHECK( f->responses == 1 );
    CHECK( server->transport_faults == 0 );

    for ( int i = 0; i < STAGED_GUARD_CAPACITY; ++i )
    {
        free( batch->data[i] );
    }
    delete batch;
    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_staged_malformed_flood_takes_no_lookup\n" );
}

// The unstaged receive: receive_batch writes into the server's own receive
// buffer, a request in every slot it was given; the guard case sets the count
// it returns and the leading entries' byte counts.

static int receive_batch_crafted( void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity )
{
    auto * f = (control_fixture_t *) p;
    f->crafted_capacity = capacity;
    for ( int i = 0; i < capacity; ++i )
    {
        from[i] = f->request_address;
        memcpy( data + stride * i, f->request, sizeof( f->request ) );
        bytes[i] = i < f->crafted_entries ? f->crafted_bytes[i] : (int) sizeof( f->request );
    }
    // INT32_MAX stands for one past the slots asked for, whatever their number
    return f->crafted_count == INT32_MAX ? capacity + 1 : f->crafted_count;
}

static void unstaged_guard_run( int count, const int * bytes, int entries, const guard_expect_t & expect )
{
    auto * f = new control_fixture_t();
    rocketnet_server_transport_io_operations_t operations;
    auto * server = guard_server( f, &operations, false );
    operations.receive_batch = receive_batch_crafted;

    f->crafted_count = count;
    f->crafted_entries = entries;
    for ( int i = 0; i < entries; ++i )
    {
        f->crafted_bytes[i] = bytes[i];
    }

    const guard_counts_t before = guard_counts( server );
    rocketnet_server_transport_update( server, 2.0 );
    guard_check( server, f, before, expect );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
}

// the capacity the update asks receive_batch for, read from one empty update

static int unstaged_guard_capacity()
{
    auto * f = new control_fixture_t();
    rocketnet_server_transport_io_operations_t operations;
    auto * server = guard_server( f, &operations, false );
    operations.receive_batch = receive_batch_crafted;
    rocketnet_server_transport_update( server, 2.0 );
    const int capacity = f->crafted_capacity;
    CHECK( capacity > 5 );
    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    return capacity;
}

static void test_unstaged_receive_guards()
{
    const int r = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
    const int ceiling = ROCKETNET_RECEIVE_CEILING;
    const int datagram = ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES;
    CHECK( datagram > ceiling );
    const uint64_t header = (uint64_t) rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 );
    const uint64_t request = (uint64_t) r + header;
    const int none[1] = { 0 };
    const int capacity = unstaged_guard_capacity();
    const uint64_t all = (uint64_t) capacity;

    // a negative count: nothing processed, one fault; an empty read: no fault
    unstaged_guard_run( -1, none, 0, { 1, 0, 0, 0, 0 } );
    unstaged_guard_run( 0, none, 0, { 0, 0, 0, 0, 0 } );

    // exactly the slots asked for: no fault; one past them: one fault, the
    // slots processed once
    unstaged_guard_run( capacity, none, 0, { 0, 0, all, all * request, capacity } );
    unstaged_guard_run( INT32_MAX, none, 0, { 1, 0, all, all * request, capacity } );

    // one bad entry first, in the middle and last of five: negative, one past
    // the datagram bound, far past it
    const int bad_first[5] = { -1, r, r, r, r };
    const int bad_middle[5] = { r, r, datagram + 1, r, r };
    const int bad_last[5] = { r, r, r, r, 1 << 20 };
    unstaged_guard_run( 5, bad_first, 5, { 1, 0, 4, 4 * request, 4 } );
    unstaged_guard_run( 5, bad_middle, 5, { 1, 1, 4, 4 * request, 4 } );
    unstaged_guard_run( 5, bad_last, 5, { 1, 1, 4, 4 * request, 4 } );

    // the bounds: an empty datagram and one of exactly the ceiling are
    // processed; one of exactly the datagram bound is no fault, but past the
    // stored prefix it is counted received and too large and not processed
    const int edges[4] = { 0, ceiling, datagram, r };
    unstaged_guard_run( 4, edges, 4, { 0, 1, 4, (uint64_t) ceiling + (uint64_t) datagram + request + 3 * header, 1 } );

    printf( "test_unstaged_receive_guards\n" );
}

// The maintenance sweep's resume point (rocketnet_server_transport_update;
// proofs/tla/RNServerMaintenanceSweep.tla). Each update visits the slots in
// ring order from the cursor; an update that runs out of room stops on the
// first due slot it cannot serve, and the next update starts on it; a pass
// that visits every slot leaves the cursor where it began. A wrong resume
// point (one slot past, or the same slot again) still serves every client in
// most runs, so these cases check the exact order of service, update by
// update, against an independent model of the rule, under the three ways a
// sweep can run out of room: a staged transport refusing the reserve, a
// staged transport reporting its room, and the portable control buffer
// against the room the transport reports. Time is an argument; keep-alives
// fall due every 1 / ROCKETNET_DEFAULT_SEND_RATE seconds, so an update a
// second later finds every client due again and one a millisecond later
// finds due only those not yet served. The disconnect allowance is held
// below one burst, so every disconnect waits in the ring and no burst takes
// the room a sweep is given.

#define SWEEP_SLOTS 8

enum
{
    SWEEP_STAGED_FULL,      // staged, no send_room: the refused reserve ends the sweep
    SWEEP_STAGED_ROOM,      // staged, send_room = frames left: the room test ends it before a reserve
    SWEEP_BUFFERED_ROOM,    // the portable control buffer, send_room = this update's budget
    SWEEP_MODES
};

struct sweep_model_t
{
    int slots;
    int mode;
    int cursor;
    double interval;                    // the keep-alive interval, 1 / the server's send rate
    bool connected[SWEEP_SLOTS];
    bool mapped[SWEEP_SLOTS];
    bool internal_error[SWEEP_SLOTS];
    bool held[SWEEP_SLOTS];             // released; assignable after the next update's publish
    bool assignable[SWEEP_SLOTS];
    uint16_t port[SWEEP_SLOTS];
    double last_keep_alive[SWEEP_SLOTS];
};

struct sweep_step_t
{
    int served[SWEEP_SLOTS];
    int served_count;
    int ended[SWEEP_SLOTS];
    int ended_count;
    int missing;            // SLOT_WITHOUT_MAPPING counted
    int no_link;            // SEND_NO_LINK_ADDRESS counted
};

static bool sweep_due( const sweep_model_t * m, int slot, double now )
{
    return now - m->last_keep_alive[slot] >= m->interval;
}

// The rule. Slots released before the update are published by the protocol
// update that opens it; a slot ended inside the sweep is published by the next.
static void sweep_model_update( sweep_model_t * m, double now, int budget, uint16_t unreachable_port, sweep_step_t * out )
{
    *out = {};
    for ( int slot = 0; slot < m->slots; ++slot )
    {
        if ( m->held[slot] )
        {
            m->held[slot] = false;
            m->assignable[slot] = true;
        }
    }

    const int first = m->cursor;
    int cursor = first;
    for ( int offset = 0; offset < m->slots; ++offset )
    {
        const int i = ( first + offset ) % m->slots;
        const int next = ( i + 1 ) % m->slots;
        cursor = i;
        if ( !m->connected[i] )
        {
            continue;
        }
        if ( m->internal_error[i] )
        {
            m->internal_error[i] = false;
            m->connected[i] = false;
            m->held[i] = true;
            out->ended[out->ended_count++] = i;
            cursor = next;
            continue;
        }
        if ( !m->mapped[i] )
        {
            out->missing++;
            cursor = next;
            continue;
        }
        if ( !sweep_due( m, i, now ) )
        {
            cursor = next;
            continue;
        }
        const bool unreachable = unreachable_port != 0 && m->port[i] == unreachable_port;
        if ( m->mode != SWEEP_STAGED_FULL && out->served_count >= budget )
        {
            break;
        }
        if ( unreachable )
        {
            out->no_link++;
            cursor = next;
            continue;
        }
        if ( out->served_count >= budget )
        {
            break;
        }
        out->served[out->served_count++] = i;
        m->last_keep_alive[i] = now;
        cursor = next;
    }
    m->cursor = cursor;
}

struct sweep_run_t
{
    control_fixture_t * f;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server;
    sweep_model_t model;
    uint64_t next_client_id;
    uint64_t next_generation;
    uint16_t next_port;
    int served_in_round[SWEEP_SLOTS];
    bool whole_round[SWEEP_SLOTS];      // connected, mapped and reachable through this whole round
};

static int sweep_send_room( void * p )
{
    auto * f = (control_fixture_t *) p;
    return f->staged ? f->capacity - f->used : f->room;
}

static sweep_run_t * sweep_create( int mode, int slots )
{
    CHECK( slots >= 2 && slots <= SWEEP_SLOTS );
    auto * run = new sweep_run_t();
    run->f = new control_fixture_t();
    run->server = make_server( run->f, &run->operations, slots, mode != SWEEP_BUFFERED_ROOM );
    if ( mode != SWEEP_STAGED_FULL )
    {
        run->operations.send_room = sweep_send_room;
    }
    run->server->disconnect_packets_per_update = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS - 1;
    run->server->disconnect_seals_left = run->server->disconnect_packets_per_update;
    CHECK( run->server->maintenance_next_client == 0 );

    run->model.slots = slots;
    run->model.mode = mode;
    run->model.interval = 1.0 / ( run->server->protocol.send_rate > 0.0 ? run->server->protocol.send_rate : ROCKETNET_DEFAULT_SEND_RATE );
    CHECK( run->model.interval >= 0.1 );
    for ( int slot = 0; slot < slots; ++slot )
    {
        auto * mapping = rocketnet_encryption_map_entry( run->server->protocol.encryption_map,
                                                         rocketnet_client_slots_mapping_index( run->server->protocol.client_slots, slot ) );
        CHECK( mapping );
        run->model.connected[slot] = true;
        run->model.mapped[slot] = true;
        run->model.port[slot] = (uint16_t) ( 1000 + slot );
        run->model.last_keep_alive[slot] = mapping->last_keep_alive_send_time;
    }
    run->next_client_id = 1000;
    run->next_generation = 1ull << 40;
    run->next_port = 1100;
    return run;
}

static void sweep_destroy( sweep_run_t * run )
{
    rn_native_server_transport_stop_and_destroy( run->server );
    delete run->f;
    delete run;
}

// A client connects into a slot the server has published free: with an
// address (as a handshake leaves it), or with none (a connected slot with
// mapping index -1, which the slot API permits and the sweep counts).
static int sweep_join( sweep_run_t * run, bool mapped )
{
    auto * server = run->server;
    int mapping_index = -1;
    uint64_t generation = run->next_generation++;
    rocketnet_address_t address;
    CHECK( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    address.port = run->next_port;
    if ( mapped )
    {
        mapping_index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 100.0 );
        CHECK( mapping_index >= 0 );
        generation = rocketnet_encryption_map_owner_generation( server->protocol.encryption_map, mapping_index );
    }
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    const int slot = rocketnet_client_slots_assign( server->protocol.client_slots, run->next_client_id++, mapping_index, generation, user_data );
    CHECK( slot >= 0 && slot < run->model.slots && run->model.assignable[slot] );
    if ( mapped )
    {
        auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index );
        mapping->control->client_index = slot;
        mapping->client_id = run->next_client_id - 1;
        mapping->control->established = 1;
        mapping->control->timeout_seconds = 1000;
        memset( &mapping->keys, slot + 1, sizeof( mapping->keys ) );
        run->model.last_keep_alive[slot] = mapping->last_keep_alive_send_time;
        run->next_port++;
    }
    server->protocol.acks->endpoints[slot].internal_error = 0;

    run->model.assignable[slot] = false;
    run->model.connected[slot] = true;
    run->model.mapped[slot] = mapped;
    run->model.port[slot] = mapped ? address.port : 0;
    run->whole_round[slot] = false;
    return slot;
}

static void sweep_leave( sweep_run_t * run, int slot )
{
    rocketnet_server_transport_disconnect_client( run->server, slot );
    CHECK( !rocketnet_client_slots_connected( run->server->protocol.client_slots, slot ) );
    run->model.connected[slot] = false;
    run->model.internal_error[slot] = false;
    run->model.held[slot] = true;
    run->whole_round[slot] = false;
}

static void sweep_print( const char * what, const int * slots, int count, const uint16_t * ports, int port_count )
{
    fprintf( stderr, "sweep %s:", what );
    for ( int k = 0; k < count; ++k )
    {
        fprintf( stderr, " %d", slots[k] );
    }
    fprintf( stderr, " | ports" );
    for ( int k = 0; k < port_count; ++k )
    {
        fprintf( stderr, " %d", (int) ports[k] );
    }
    fprintf( stderr, "\n" );
}

// One update with this budget of frames: the slots served, in order, and the
// counters and ends, exactly as the model has them.
static void sweep_update( sweep_run_t * run, double now, int budget )
{
    auto * f = run->f;
    auto * server = run->server;
    if ( run->model.mode == SWEEP_BUFFERED_ROOM )
    {
        f->room = budget;
    }
    else
    {
        f->capacity = budget;
    }

    sweep_step_t want;
    sweep_model_update( &run->model, now, budget, run->model.mode == SWEEP_BUFFERED_ROOM ? 0 : f->unreachable_port, &want );

    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t missing = counters[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING];
    const uint64_t no_link = counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS];
    f->sent_count = 0;

    rocketnet_server_transport_update( server, now );

    bool same = f->sent_count == want.served_count;
    for ( int k = 0; same && k < want.served_count; ++k )
    {
        same = f->sent_ports[k] == run->model.port[want.served[k]];
    }
    if ( !same )
    {
        fprintf( stderr, "sweep mode %d, %d slots, budget %d at %.3f\n", run->model.mode, run->model.slots, budget, now );
        sweep_print( "want", want.served, want.served_count, f->sent_ports, f->sent_count );
    }
    CHECK( f->sent_count == want.served_count );
    for ( int k = 0; k < want.served_count; ++k )
    {
        CHECK( f->sent_ports[k] == run->model.port[want.served[k]] );
        run->served_in_round[want.served[k]]++;
    }
    CHECK( counters[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING] - missing == (uint64_t) want.missing );
    CHECK( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] - no_link == (uint64_t) want.no_link );
    for ( int k = 0; k < want.ended_count; ++k )
    {
        CHECK( server->protocol.end_reasons[want.ended[k]] == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        run->whole_round[want.ended[k]] = false;
    }
    for ( int slot = 0; slot < run->model.slots; ++slot )
    {
        CHECK( ( rocketnet_client_slots_connected( server->protocol.client_slots, slot ) != 0 ) == run->model.connected[slot] );
    }
}

// Slots still owed a keep-alive this round that the sweep can serve.
static int sweep_owed( const sweep_run_t * run, double now )
{
    int owed = 0;
    for ( int slot = 0; slot < run->model.slots; ++slot )
    {
        const bool unreachable = run->model.mode != SWEEP_BUFFERED_ROOM && run->f->unreachable_port != 0 &&
                                 run->model.port[slot] == run->f->unreachable_port;
        if ( run->model.connected[slot] && run->model.mapped[slot] && !unreachable && sweep_due( &run->model, slot, now ) )
        {
            ++owed;
        }
    }
    return owed;
}

// A pass that visits every slot leaves the cursor where it began, whatever
// the last slot it visited was: served, not due, without a mapping, ended for
// an internal fault, or refused for want of a link-layer address. Four slots;
// slot 3 is put in the given state, the first update (room for all) passes
// over every slot from slot 0, and the next update, with every client due and
// room for one frame, serves slot 0. A resume point one past (or, where the
// last visit served or skipped a due-later client, the same slot again)
// serves slot 1 or slot 3 instead. A pass that visited slot 0 twice ends one
// past it as well.

enum
{
    SWEEP_LAST_SERVED,
    SWEEP_LAST_NOT_DUE,
    SWEEP_LAST_NO_MAPPING,
    SWEEP_LAST_INTERNAL_ERROR,
    SWEEP_LAST_UNREACHABLE,
    SWEEP_LAST_KINDS
};

static void test_sweep_full_pass_keeps_its_start()
{
    for ( int mode = 0; mode < SWEEP_MODES; ++mode )
    {
        for ( int kind = 0; kind < SWEEP_LAST_KINDS; ++kind )
        {
            if ( kind == SWEEP_LAST_UNREACHABLE && mode == SWEEP_BUFFERED_ROOM )
            {
                continue;       // the control buffer never refuses an address
            }
            sweep_run_t * run = sweep_create( mode, 4 );
            auto * server = run->server;
            const int last = 3;
            if ( kind == SWEEP_LAST_NOT_DUE )
            {
                auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map,
                                                                 rocketnet_client_slots_mapping_index( server->protocol.client_slots, last ) );
                mapping->last_keep_alive_send_time = 0.95;
                run->model.last_keep_alive[last] = 0.95;
            }
            else if ( kind == SWEEP_LAST_NO_MAPPING )
            {
                rocketnet_client_slots_release( server->protocol.client_slots, last );
                rocketnet_client_slots_publish( server->protocol.client_slots );
                run->model.connected[last] = false;
                run->model.assignable[last] = true;
                CHECK( sweep_join( run, false ) == last );
            }
            else if ( kind == SWEEP_LAST_INTERNAL_ERROR )
            {
                server->protocol.acks->endpoints[last].internal_error = 1;
                run->model.internal_error[last] = true;
            }
            else if ( kind == SWEEP_LAST_UNREACHABLE )
            {
                run->f->unreachable_port = run->model.port[last];
            }

            sweep_update( run, 1.0, 4 );
            CHECK( run->f->sent_count == ( kind == SWEEP_LAST_SERVED ? 4 : 3 ) );
            sweep_update( run, 2.0, 1 );
            CHECK( run->f->sent_count == 1 && run->f->sent_ports[0] == 1000 );
            sweep_destroy( run );
        }
    }
    printf( "test_sweep_full_pass_keeps_its_start\n" );
}

// N clients, every one due at each round, and a budget of 1, 2, N - 1 or N
// frames per update: each round serves every client exactly once, in ring
// order, across the wrap, and ends when nobody is owed; each update serves
// exactly what the model does.

static void test_sweep_each_client_once_per_round()
{
    const int sizes[] = { 3, 4, 5, 7 };
    for ( int mode = 0; mode < SWEEP_MODES; ++mode )
    {
        for ( int s = 0; s < (int) ( sizeof( sizes ) / sizeof( sizes[0] ) ); ++s )
        {
            const int n = sizes[s];
            const int budgets[] = { 1, 2, n - 1, n };
            for ( int b = 0; b < (int) ( sizeof( budgets ) / sizeof( budgets[0] ) ); ++b )
            {
                const int budget = budgets[b];
                sweep_run_t * run = sweep_create( mode, n );
                const int updates = ( n + budget - 1 ) / budget;
                int expect_first = 0;
                for ( int round = 0; round < 2 * n; ++round )
                {
                    memset( run->served_in_round, 0, sizeof( run->served_in_round ) );
                    int order[SWEEP_SLOTS];
                    int order_count = 0;
                    for ( int u = 0; u < updates; ++u )
                    {
                        const double now = 1.0 + round + 0.001 * u;
                        sweep_update( run, now, budget );
                        for ( int k = 0; k < run->f->sent_count && order_count < SWEEP_SLOTS; ++k )
                        {
                            order[order_count++] = run->f->sent_ports[k] - 1000;
                        }
                    }
                    CHECK( sweep_owed( run, 1.0 + round + 0.001 * updates ) == 0 );
                    CHECK( order_count == n );
                    for ( int k = 0; k < n; ++k )
                    {
                        CHECK( run->served_in_round[k] == 1 );
                        CHECK( order[k] == ( order[0] + k ) % n );
                    }
                    // the next round starts where this round's last update began:
                    // that update's pass visited every slot
                    CHECK( order[0] == expect_first );
                    expect_first = order[( updates - 1 ) * budget];
                }
                sweep_destroy( run );
            }
        }
    }
    printf( "test_sweep_each_client_once_per_round\n" );
}

// Clients connect and leave mid-round, slots end for an internal fault, a slot
// connects with no mapping, and (staged) one address at a time has no
// link-layer destination; budgets vary from 0 to 3 frames. Each update
// serves exactly what the model does; each round ends with nobody owed; a
// client connected, mapped and reachable through a whole round is served
// exactly once in it; every waiting disconnect burst leaves at last.

static uint32_t sweep_random( uint32_t * state )
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static int sweep_pick( const sweep_run_t * run, uint32_t * state, bool connected, bool mapped )
{
    int candidates[SWEEP_SLOTS];
    int count = 0;
    for ( int slot = 0; slot < run->model.slots; ++slot )
    {
        if ( connected ? ( run->model.connected[slot] && ( !mapped || run->model.mapped[slot] ) ) : run->model.assignable[slot] )
        {
            candidates[count++] = slot;
        }
    }
    return count == 0 ? -1 : candidates[sweep_random( state ) % (uint32_t) count];
}

static int sweep_connected( const sweep_run_t * run )
{
    int count = 0;
    for ( int slot = 0; slot < run->model.slots; ++slot )
    {
        count += run->model.connected[slot] ? 1 : 0;
    }
    return count;
}

static void test_sweep_follows_the_rule_as_clients_come_and_go()
{
    const int sizes[] = { 4, 5, 7 };
    uint32_t state = 12345;
    for ( int mode = 0; mode < SWEEP_MODES; ++mode )
    {
        for ( int s = 0; s < (int) ( sizeof( sizes ) / sizeof( sizes[0] ) ); ++s )
        {
            const int n = sizes[s];
            sweep_run_t * run = sweep_create( mode, n );
            bool unmapped_joined = false;
            for ( int round = 0; round < 8; ++round )
            {
                const double start = 1.0 + round;
                memset( run->served_in_round, 0, sizeof( run->served_in_round ) );
                for ( int slot = 0; slot < n; ++slot )
                {
                    run->whole_round[slot] = run->model.connected[slot] && run->model.mapped[slot] &&
                                             !( mode != SWEEP_BUFFERED_ROOM && run->f->unreachable_port != 0 &&
                                                run->model.port[slot] == run->f->unreachable_port );
                }
                int u = 0;
                for ( ; u < 8 * n && ( u == 0 || sweep_owed( run, start + 0.001 * u ) > 0 ); ++u )
                {
                    const uint32_t event = sweep_random( &state ) % 10;
                    if ( event == 0 && sweep_connected( run ) > 2 )
                    {
                        sweep_leave( run, sweep_pick( run, &state, true, false ) );
                    }
                    else if ( event == 1 && sweep_pick( run, &state, false, false ) >= 0 )
                    {
                        sweep_join( run, true );
                    }
                    else if ( event == 2 && sweep_connected( run ) > 2 )
                    {
                        const int slot = sweep_pick( run, &state, true, true );
                        if ( slot >= 0 )
                        {
                            run->server->protocol.acks->endpoints[slot].internal_error = 1;
                            run->model.internal_error[slot] = true;
                            run->whole_round[slot] = false;
                        }
                    }
                    else if ( event == 3 && !unmapped_joined && sweep_pick( run, &state, false, false ) >= 0 )
                    {
                        sweep_join( run, false );
                        unmapped_joined = true;
                    }
                    else if ( event == 4 && mode != SWEEP_BUFFERED_ROOM )
                    {
                        const int slot = sweep_pick( run, &state, true, true );
                        const uint16_t port = ( sweep_random( &state ) % 2 == 0 || slot < 0 ) ? (uint16_t) 0 : run->model.port[slot];
                        for ( int k = 0; k < n; ++k )
                        {
                            if ( run->model.port[k] != 0 && ( run->model.port[k] == port || run->model.port[k] == run->f->unreachable_port ) )
                            {
                                run->whole_round[k] = false;
                            }
                        }
                        run->f->unreachable_port = port;
                    }
                    const int budget = (int) ( sweep_random( &state ) % 4 );
                    sweep_update( run, start + 0.001 * u, budget );
                }
                CHECK( sweep_owed( run, start + 0.001 * u ) == 0 );
                for ( int slot = 0; slot < n; ++slot )
                {
                    if ( run->whole_round[slot] && run->model.connected[slot] )
                    {
                        CHECK( run->served_in_round[slot] == 1 );
                    }
                    CHECK( run->served_in_round[slot] <= 1 );
                }
            }

            // the waiting bursts leave in later updates with room and nobody due

            run->f->unreachable_port = 0;
            for ( int u = 0; u < 64 && run->server->disconnect_burst_count > 0; ++u )
            {
                sweep_update( run, 9.0 + 0.0001 * u, 8 );
            }
            CHECK( run->server->disconnect_burst_count == 0 );
            sweep_destroy( run );
        }
    }
    printf( "test_sweep_follows_the_rule_as_clients_come_and_go\n" );
}

// The disconnect allowance (rocketnet_server_transport_update, after the
// sweep; proofs/tla/ServerSession.tla SealWaiting, ServerStopSpread.tla
// SealOne): a direct transport update opens disconnect_packets_per_update
// and seals no more waiting disconnect packets than that allowance, the send
// room, and the packets waiting. Every burst here waits (it is made while the
// transport reports no room), the portable control buffer takes the frames,
// and nobody's keep-alive is due in the drain updates. Each update seals
// exactly min( allowance, room, waiting ): the allowance at 1, one burst less
// one, one burst, one burst and one, and two bursts either side; the room one
// under, at and over the allowance; a single waiting burst.

struct burst_case_t
{
    int allowance;
    int room;
    int bursts;
};

static void test_disconnect_seals_per_update_at_each_bound()
{
    const int burst = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
    const burst_case_t cases[] = {
        { 1, 1000, 1 },
        { 1, 0, 1 },
        { burst - 1, 1000, 1 },
        { burst, 1000, 1 },
        { burst + 1, 1000, 1 },
        { 2 * burst - 1, 1000, 2 },
        { 2 * burst, 1000, 2 },
        { 2 * burst + 1, 1000, 2 },
        { burst, burst - 1, 2 },
        { burst, burst, 2 },
        { burst, burst + 1, 2 },
        { 2, 1, 3 },
        { ROCKETNET_DEFAULT_SERVER_DISCONNECT_PACKETS_PER_UPDATE, 1000, 6 },
    };

    for ( int c = 0; c < (int) ( sizeof( cases ) / sizeof( cases[0] ) ); ++c )
    {
        const burst_case_t * test = &cases[c];
        auto * f = new control_fixture_t();
        rocketnet_server_transport_io_operations_t operations;
        auto * server = make_server( f, &operations, 6, false );
        operations.send_room = sweep_send_room;
        server->disconnect_packets_per_update = test->allowance;
        f->room = 1000;
        rocketnet_server_transport_update( server, 1.0 );
        CHECK( f->sent_count == 6 && f->disconnects == 0 );

        f->room = 0;
        for ( int k = 0; k < test->bursts; ++k )
        {
            rocketnet_server_transport_disconnect_client( server, k );
        }
        CHECK( server->disconnect_burst_count == test->bursts );
        CHECK( server->disconnect_seals == 0 );

        int waiting = test->bursts * burst;
        int room = test->room;
        for ( int u = 1; u <= 4 * burst && waiting > 0; ++u )
        {
            if ( room == 0 && u == 3 )
            {
                room = 1000;    // nothing leaves without room; then it all does
            }
            f->room = room;
            const uint64_t seals = server->disconnect_seals;
            const int disconnects = f->disconnects;
            rocketnet_server_transport_update( server, 1.0 + 0.001 * u );
            int want = test->allowance < room ? test->allowance : room;
            want = want < waiting ? want : waiting;
            const int sealed = (int) ( server->disconnect_seals - seals );
            if ( sealed != want )
            {
                fprintf( stderr, "disconnect allowance %d, room %d, %d bursts: update %d sealed %d, want %d\n",
                         test->allowance, room, test->bursts, u, sealed, want );
            }
            CHECK( sealed == want );
            CHECK( f->disconnects - disconnects == want );
            CHECK( f->sent_count == 6 );    // no keep-alive was due: the drain had the room alone
            waiting -= want;
        }
        CHECK( waiting == 0 );
        CHECK( server->disconnect_burst_count == 0 );
        CHECK( f->disconnects == test->bursts * burst );

        rn_native_server_transport_stop_and_destroy( server );
        delete f;
    }
    printf( "test_disconnect_seals_per_update_at_each_bound\n" );
}

// One allowance per update, shared with the message layer's kicks: the
// message update opens it (rocketnet_server_transport_begin_message_update)
// before its kicks, and the transport update that follows drains only what
// they left; the next direct update opens a fresh one. What a drain spends is
// gone until the next update opens the allowance again, so a kick between
// updates whose burst the rest cannot cover waits: from one opening to the
// next, no more than the allowance is sealed.

static void test_disconnect_allowance_shared_with_the_message_update()
{
    const int burst = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
    const int allowance = burst + burst / 2;
    auto * f = new control_fixture_t();
    rocketnet_server_transport_io_operations_t operations;
    auto * server = make_server( f, &operations, 6, false );
    operations.send_room = sweep_send_room;
    server->disconnect_packets_per_update = allowance;
    f->room = 1000;
    rocketnet_server_transport_update( server, 1.0 );
    CHECK( f->sent_count == 6 );

    // the message update opens the allowance; its first kick seals a whole
    // burst at once, the second waits for what is left of it

    rocketnet_server_transport_begin_message_update( server );
    rocketnet_server_transport_disconnect_client( server, 0 );
    CHECK( server->disconnect_seals == (uint64_t) burst );
    rocketnet_server_transport_disconnect_client( server, 1 );
    CHECK( server->disconnect_burst_count == 1 );
    CHECK( server->disconnect_seals == (uint64_t) burst );

    rocketnet_server_transport_update( server, 1.001 );
    CHECK( server->disconnect_seals == (uint64_t) allowance );

    // a kick between updates finds the allowance spent and waits; the next
    // direct update opens a whole allowance for both waiting bursts

    rocketnet_server_transport_disconnect_client( server, 2 );
    CHECK( server->disconnect_burst_count == 2 );
    CHECK( server->disconnect_seals == (uint64_t) allowance );
    rocketnet_server_transport_update( server, 1.002 );
    CHECK( server->disconnect_seals == (uint64_t) ( 2 * allowance ) );
    CHECK( server->disconnect_burst_count == 0 );

    // that update's drain spent its allowance: this kick waits for the next one

    rocketnet_server_transport_disconnect_client( server, 3 );
    CHECK( server->disconnect_burst_count == 1 );
    CHECK( server->disconnect_seals == (uint64_t) ( 2 * allowance ) );
    rocketnet_server_transport_update( server, 1.003 );
    CHECK( server->disconnect_seals == (uint64_t) ( 2 * allowance + burst ) );
    CHECK( server->disconnect_burst_count == 0 );
    CHECK( f->disconnects == 4 * burst );
    CHECK( f->sent_count == 6 );

    rn_native_server_transport_stop_and_destroy( server );
    delete f;
    printf( "test_disconnect_allowance_shared_with_the_message_update\n" );
}

#if ROCKETNET_DEV

// The client transport's connection reset (rocketnet_client_connection_reset,
// the transport-owned part of ClientSession End) writes only what it owns:
// the packet leases, the fill rings' counts and the stats cursor. Every other
// byte of the transport, the stop burst's storage poisoned so a stray zero
// shows, is the same after it. The rings are one array in the transport's one
// allocation, so a write past the last ring lands in the members that follow
// (the stop burst on every layout), where no sanitizer sees it.

static void control_client_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

static double control_client_time( void * )
{
    return 1000.0;
}

static bool control_client_owned_by_reset( size_t offset )
{
    const size_t leases = offsetof( rocketnet_client_transport_t, packet_leases );
    if ( offset >= leases && offset < leases + sizeof( ( (rocketnet_client_transport_t *) 0 )->packet_leases ) )
    {
        return true;
    }
    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        const size_t count = offsetof( rocketnet_client_transport_t, payload_ring ) + (size_t) ring * sizeof( rocketnet_client_payload_ring_t ) +
                             offsetof( rocketnet_client_payload_ring_t, num_packets );
        if ( offset >= count && offset < count + sizeof( uint64_t ) )
        {
            return true;
        }
    }
    const size_t stats = offsetof( rocketnet_client_transport_t, stats_counted );
    return offset >= stats && offset < stats + sizeof( ( (rocketnet_client_transport_t *) 0 )->stats_counted );
}

static void test_client_connection_reset_writes_only_what_it_owns()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_base;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    control_client_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = control_client_time;

    rocketnet_client_transport_t * client = rocketnet_client_transport_create_step_driven( NULL, token, &config, 1000.0, &operations, fake );
    CHECK( client != NULL );
    CHECK( !client->io_thread );

    memset( client->stop_burst, 0xA5, sizeof( client->stop_burst ) );
    client->payload_ring[0].num_packets = 3;
    client->stats_counted = 3;

    const size_t bytes = sizeof( rocketnet_client_transport_t );
    uint8_t * before = (uint8_t *) malloc( bytes );
    CHECK( before != NULL );
    memcpy( before, (const void *) client, bytes );

    rocketnet_client_connection_reset( client );

    CHECK( client->payload_ring[0].num_packets == 0 );
    CHECK( client->stats_counted == 0 );

    const uint8_t * after = (const uint8_t *) client;
    size_t changed = 0;
    for ( size_t offset = 0; offset < bytes; offset++ )
    {
        if ( after[offset] != before[offset] && !control_client_owned_by_reset( offset ) )
        {
            if ( changed == 0 )
            {
                fprintf( stderr, "connection reset wrote byte %zu of the transport (stop_burst is at %zu)\n", offset,
                         offsetof( rocketnet_client_transport_t, stop_burst ) );
            }
            changed++;
        }
    }
    CHECK( changed == 0 );

    // with the I/O thread design the rings other than the Filling one are the
    // worker's (handed off, or its own): the reset leaves their counts alone
    // and resets only the Filling ring's, whichever ring that is

    const bool io_thread = client->io_thread;
    const int send_fill = client->client_io.send_fill;
    client->io_thread = true;
    for ( int fill = 0; fill < ROCKETNET_CLIENT_IO_NUM_RINGS; fill++ )
    {
        client->client_io.send_fill = fill;
        for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
        {
            client->payload_ring[ring].num_packets = 5;
        }
        rocketnet_client_connection_reset( client );
        for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
        {
            CHECK( client->payload_ring[ring].num_packets == ( ring == fill ? 0u : 5u ) );
        }
    }
    client->io_thread = io_thread;
    client->client_io.send_fill = send_fill;
    for ( int ring = 0; ring < ROCKETNET_CLIENT_IO_NUM_RINGS; ring++ )
    {
        client->payload_ring[ring].num_packets = 0;
    }

    free( before );
    rn_native_client_transport_stop_and_destroy( client );
    delete fake;
    printf( "test_client_connection_reset_writes_only_what_it_owns\n" );
}

#endif // #if ROCKETNET_DEV

int main( int argc, char ** argv )
{
    // every case here pins loopback in its config and drives fixture
    // operations; a whole-suite server pin would override that pin (the
    // environment wins, by design) and an xdp pin refuses the port-0 create,
    // so this binary runs without one
#if defined( _WIN32 )
    _putenv_s( "ROCKETNET_SERVER_TRANSPORT", "" );
#else
    unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
#endif // #if defined( _WIN32 )

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_forbid_xdp_commit();

    if ( argc == 2 )
    {
        if ( strcmp( argv[1], "disconnect-burst-portable" ) == 0 )
        {
            test_disconnect_burst_next_send( false );
        }
        else if ( strcmp( argv[1], "disconnect-burst-staged" ) == 0 )
        {
            test_disconnect_burst_next_send( true );
        }
        else
        {
            CHECK( false );
        }
        CHECK( rocketnet_test_loopback_hub_release() );
        rocketnet_term();
        return 0;
    }

    CHECK( argc == 1 );
    test_buffered_control_pressure();
    test_buffered_control_pressure( 1000 );
    test_disconnect_burst_next_send( false );
    test_disconnect_burst_next_send( true );
    test_staged_control_fairness();
    test_staged_unreachable_client_does_not_pin_sweep();
    test_staged_idle_maintenance_preflight();
    test_staged_lease_table_full();
    test_entries_carry_connection_id( false );
    test_entries_carry_connection_id( true );
    test_staged_link_address_hooks();
    test_staged_link_address_taught_at_view_zero();
    test_staged_refused_response_is_counted();
    test_deferred_accept_reads_only_this_batch();
    test_staged_receive_guards();
    test_unstaged_receive_guards();
    test_staged_malformed_flood_takes_no_lookup();
    test_sweep_full_pass_keeps_its_start();
    test_sweep_each_client_once_per_round();
    test_sweep_follows_the_rule_as_clients_come_and_go();
    test_disconnect_seals_per_update_at_each_bound();
#if ROCKETNET_DEV
    test_client_connection_reset_writes_only_what_it_owns();
#endif // #if ROCKETNET_DEV
    test_disconnect_allowance_shared_with_the_message_update();
    CHECK( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
}
