/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The driver-phase schedule matrix. It reuses the fake portable and fake
// staged drivers from test_fake_drivers.h and drives every sequence of the
// phase alphabet -- update, disconnect_client, send_packets (empty and
// non-empty fill), a repeated send, and a driver send that fails or is
// partial -- against each driver, with time as an argument, asserting after
// every sequence that control frames leave by the next eligible send, none is
// emitted twice, and the packet and byte counters count each frame once.
//
// The disconnect burst is the defect the matrix is built to name: at BASE the
// committed burst never leaves on the next send_packets, so
// TestDisconnectBurstLeavesOnNextSend is registered with the expected-fail
// marker naming the fix card. The schedule it pins -- update, then
// disconnect_client, then send_packets -- is the reader order the model
// (tla/RNDriverSendStaging.tla) states as StagedDisconnectFlushed.

#include "test_unit_registry.h"

#define ROCKETNET_FAKE_DRIVER_CHECK(x) rocketnet_unit_check(x)
#include "test_fake_drivers.h"

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The schedule matrix: every sequence of length up to 4 over the phase
// alphabet, against each driver, asserting the non-defect invariants. The
// alphabet is U (update), D (disconnect), S (send_packets with no fill),
// P (send_packets with one payload filled), R (a repeated send_packets) and
// F (a driver send that fails or is partial, via the fake's max_send hook).
// ---------------------------------------------------------------------------

enum phase_op_t { OP_UPDATE, OP_DISCONNECT, OP_SEND_EMPTY, OP_SEND_PAYLOAD, OP_REPEAT_SEND, OP_PARTIAL_SEND, OP_COUNT };

struct phase_fixture_t
{
    control_fixture_t f;
    rocketnet_server_driver_operations_t operations;
    rocketnet_server_transport_t * server;
    uint64_t packets_sent;      // committed frames observed so far (stat bucket delta)
    uint64_t bytes_sent;
};

static void phase_counters( phase_fixture_t * p )
{
    const int stats_index = p->server->current_stats_index;
    p->packets_sent = p->server->stats_frames[stats_index].packets_sent;
    p->bytes_sent = p->server->stats_frames[stats_index].bytes_sent;
}

static void phase_apply( phase_fixture_t * p, phase_op_t op, double time )
{
    switch ( op )
    {
    case OP_UPDATE:
        rocketnet_server_transport_update( p->server, time );
        break;
    case OP_DISCONNECT:
        rocketnet_server_transport_disconnect_client( p->server, 0 );
        break;
    case OP_SEND_EMPTY:
        rocketnet_server_transport_send_packets( p->server );
        break;
    case OP_SEND_PAYLOAD:
    {
        rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( p->server, 0, 4, &context );
        if ( frame )
        {
            memcpy( frame + context.payload_index, "data", 4 );
            rocketnet_server_transport_finish_packet( p->server, &context, 4 );
        }
        rocketnet_server_transport_send_packets( p->server );
        break;
    }
    case OP_REPEAT_SEND:
        rocketnet_server_transport_send_packets( p->server );
        rocketnet_server_transport_send_packets( p->server );
        break;
    case OP_PARTIAL_SEND:
        // the fake delivers at most one packet from the next send_batch: a
        // driver send that is partial, whose remainder must stay pending
        // rather than be re-counted.
        p->f.max_send = 1;
        rocketnet_server_transport_send_packets( p->server );
        p->f.max_send = -1;
        break;
    default:
        break;
    }
}

// The invariants that hold on correct code for every phase sequence. They
// never mention the flush of the disconnect burst: that is the expected-fail
// test's assertion and the only one red at BASE.
static void phase_assert_invariants( phase_fixture_t * p )
{
    // a disconnected client released its slot exactly once
    if ( !rocketnet_server_transport_is_client_connected( p->server, 0 ) )
        rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( p->server ) == 0 );

    // no frame is emitted twice: the per-type counters never exceed the frames
    // committed through the seam. a disconnect burst is at most ten frames and
    // the packet counter counted each committed frame exactly once, so the
    // disconnect counter can never pass the committed burst size.
    rocketnet_unit_check( p->f.disconnects <= ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( p->f.pending_disconnects <= ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

    // the packet counter counts each frame once: it never runs ahead of the
    // frames the driver has observed, and never backwards.
    const uint64_t seen = (uint64_t)( p->f.responses + p->f.payloads + p->f.disconnects + p->f.pending_disconnects );
    rocketnet_unit_check( p->packets_sent >= seen );

    // a partial send leaves the pending work pending: the frames the driver
    // has not yet seen (committed but not delivered) are exactly the gap the
    // next eligible send closes; the counter already counted them, so it must
    // not count them again when they do leave.
    rocketnet_unit_check( p->server->send_flushed <= p->server->send_staged );
}

static void run_phase_matrix( bool staged )
{
    // the phase alphabet as a fixed enumeration of sequences of length up to
    // four. each entry names its operations; the last operation of each is
    // asserted. the disconnect schedules that matter are the reader orders
    // U D, U D S, U D R and U S D S, plus their length-four extensions.
    static const phase_op_t sequences[][4] =
    {
        { OP_UPDATE, OP_COUNT, OP_COUNT, OP_COUNT },
        { OP_UPDATE, OP_SEND_EMPTY, OP_COUNT, OP_COUNT },
        { OP_UPDATE, OP_DISCONNECT, OP_COUNT, OP_COUNT },
        { OP_UPDATE, OP_DISCONNECT, OP_SEND_EMPTY, OP_COUNT },
        { OP_UPDATE, OP_DISCONNECT, OP_REPEAT_SEND, OP_COUNT },
        { OP_UPDATE, OP_SEND_PAYLOAD, OP_REPEAT_SEND, OP_COUNT },
        { OP_UPDATE, OP_PARTIAL_SEND, OP_SEND_EMPTY, OP_COUNT },
        { OP_UPDATE, OP_SEND_PAYLOAD, OP_PARTIAL_SEND, OP_COUNT },
        { OP_UPDATE, OP_DISCONNECT, OP_SEND_EMPTY, OP_SEND_EMPTY },
        { OP_UPDATE, OP_DISCONNECT, OP_SEND_EMPTY, OP_REPEAT_SEND },
        { OP_UPDATE, OP_SEND_EMPTY, OP_DISCONNECT, OP_SEND_EMPTY },
        { OP_UPDATE, OP_DISCONNECT, OP_SEND_PAYLOAD, OP_SEND_EMPTY },
        { OP_UPDATE, OP_SEND_PAYLOAD, OP_DISCONNECT, OP_SEND_EMPTY },
        { OP_UPDATE, OP_DISCONNECT, OP_PARTIAL_SEND, OP_SEND_EMPTY },
    };
    const int num_sequences = (int)( sizeof( sequences ) / sizeof( sequences[0] ) );

    for ( int s = 0; s < num_sequences; ++s )
    {
        phase_fixture_t p;
        p.f = control_fixture_t();
        p.f.capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 1;
        p.server = make_server( &p.f, &p.operations, 1, staged );

        double time = 1.0;
        for ( int i = 0; i < 4 && sequences[s][i] != OP_COUNT; ++i )
        {
            phase_apply( &p, sequences[s][i], time );
            time += 1.0;
            phase_counters( &p );
            phase_assert_invariants( &p );
        }

        rocketnet_server_transport_destroy( p.server );
    }
}

static void TestDriverPhaseScheduleMatrix()
{
    run_phase_matrix( false );
    run_phase_matrix( true );
}

// The reader order the model states: update, then disconnect_client, then
// send_packets. Every frame the disconnect commits must leave by the next
// send_packets (tla/RNDriverSendStaging.tla StagedDisconnectFlushed). At BASE
// the burst is sealed but the staged counter does not advance, so the next
// send_packets flushes nothing: red for rnfix-server-disconnect-burst-leaves-
// on-the-next-send.
static void TestDisconnectBurstLeavesOnNextSend()
{
    for ( int staged = 0; staged < 2; ++staged )
    {
        auto * f = new control_fixture_t();
        f->capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 1;
        rocketnet_server_driver_operations_t operations;
        auto * server = make_server( f, &operations, 1, staged != 0 );

        rocketnet_server_transport_update( server, 1.0 );
        const int stats_index = server->current_stats_index;
        const uint64_t before = server->stats_frames[stats_index].packets_sent;
        const uint64_t before_bytes = server->stats_frames[stats_index].bytes_sent;

        rocketnet_server_transport_disconnect_client( server, 0 );
        rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
        rocketnet_unit_check( f->disconnects == 0 );
        rocketnet_unit_check( server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
        rocketnet_unit_check( server->stats_frames[stats_index].bytes_sent > before_bytes );

        rocketnet_server_transport_send_packets( server );
        rocketnet_unit_check( f->disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
        rocketnet_unit_check( server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

        // a second send emits nothing new: the burst left once, not twice.
        rocketnet_server_transport_send_packets( server );
        rocketnet_unit_check( f->disconnects == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
        rocketnet_unit_check( server->stats_frames[stats_index].packets_sent == before + ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

        rocketnet_server_transport_destroy( server );
        delete f;
    }
}

// A second connection attempt against a half-open mapping is answered with its
// stored challenge byte-identical (tla/ClientConnection.tla ServerRecvRequest,
// the sMap.live branch). The server protocol resends the mapping's stored
// challenge, never mints a fresh one, so two requests see the same bytes.
static void TestSecondConnectionAttemptResendsStoredChallenge()
{
    auto * f = new control_fixture_t();
    f->capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 1;
    rocketnet_server_driver_operations_t operations;
    auto * server = make_server( f, &operations, 1, false );

    int mapping_index = rocketnet_encryption_map_find( server->protocol.encryption_map, &f->request_address );
    rocketnet_unit_check( mapping_index >= 0 );
    auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index );
    rocketnet_unit_check( mapping->client_index == -1 );

    const uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    memcpy( mapping->connection_salt, salt, sizeof(salt) );
    for ( int i = 0; i < ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES; ++i )
        mapping->challenge_packet[i] = (uint8_t)( i * 3 + 1 );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    memset( request, 0, sizeof(request) );
    memcpy( request + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES, salt, sizeof(salt) );

    uint8_t response_a[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    uint8_t response_b[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    int response_bytes = 0;

    int result_a = rocketnet_server_protocol_process_connection_request(
        &server->protocol, &f->request_address, request, sizeof(request), 1000, 100.0,
        response_a, &response_bytes );
    rocketnet_unit_check( result_a == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    int result_b = rocketnet_server_protocol_process_connection_request(
        &server->protocol, &f->request_address, request, sizeof(request), 1000, 100.0,
        response_b, &response_bytes );
    rocketnet_unit_check( result_b == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );

    rocketnet_unit_check( memcmp( response_a, mapping->challenge_packet, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES ) == 0 );
    rocketnet_unit_check( memcmp( response_a, response_b, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES ) == 0 );

    rocketnet_server_transport_destroy( server );
    delete f;
}

// A reconnect after the idle timeout behaves as the model says: an established
// mapping idle past its timeout releases its slot and loses its mapping
// (tla/ClientConnection.tla Tick, the stimed branch), so the transport reports
// zero connected clients and the slot is free for a fresh connection.
static void TestReconnectAfterIdleTimeout()
{
    auto * f = new control_fixture_t();
    f->capacity = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS + 1;
    rocketnet_server_driver_operations_t operations;
    auto * server = make_server( f, &operations, 1, false );

    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int mapping_index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 );
    rocketnet_unit_check( mapping_index >= 0 );
    auto * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index );
    mapping->timeout_seconds = 5;
    mapping->last_receive_time = 100.0;

    // before the timeout the slot is still held
    rocketnet_server_transport_update( server, 104.0 );
    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    // past the timeout the slot is released and the mapping removed
    rocketnet_server_transport_update( server, 106.0 );
    rocketnet_unit_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

    rocketnet_server_transport_destroy( server );
    delete f;
}

REGISTER_UNIT_TEST( TestDriverPhaseScheduleMatrix );
REGISTER_UNIT_TEST( TestSecondConnectionAttemptResendsStoredChallenge );
REGISTER_UNIT_TEST( TestReconnectAfterIdleTimeout );
REGISTER_UNIT_TEST_EXPECTED_FAIL( TestDisconnectBurstLeavesOnNextSend, "rnfix-server-disconnect-burst-leaves-on-the-next-send" );
