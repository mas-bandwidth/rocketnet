/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Cases for the round 2 survivors of the 2026-11-10 mutation hunt (run 2)
// over sdk/rocketnet_server_transport.cpp that the round 1 cases left open:
// the public calls' refusals of a NULL argument or a stopped transport, the
// staged finish of a maximum frame, the link simulator's lock misses, the
// staged receive view and the batch receive bounds, the maintenance sweep's
// cursor and its returned frame, the replay commit, the counted refusal of a
// response frame, the slots without a mapping or a connection id, the
// delivery capture and the development seams and fixtures. Each case names
// the mutants it kills (`mutation gap: hunt 2/<id>`).
//
// The fakes are round 1's (tests/test_unit_gaps_st_ring_transport.cpp, the
// file included just before this one): a buffered transport that records
// every datagram and a staged one with a fixed staging. No socket, no
// thread, no wall clock: the clock argument starts at 1000.0. This file is
// compiled into the loopback translation unit
// (tests/test_unit_driver_loopback.cpp), which includes the server
// transport, so the cases read its internal layout.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_internal.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_replay.h"

#include <stdio.h>
#include <string.h>

// a call that must refuse at its guard: the assert the guard fires is
// counted, not fatal, for the length of the scope
struct r2_st_asserts_counted_t
{
    rocketnet_assert_function_t saved;
    r2_st_asserts_counted_t()
    {
        saved = rn_assert_function;
        g_st_ring_asserts = 0;
        rocketnet_assert_function( st_ring_assert_hook );
    }
    ~r2_st_asserts_counted_t() { rocketnet_assert_function( saved ); }
};

// ----------------------------------------------------------------
// The public calls' guards
// ----------------------------------------------------------------

// Every exported call refuses a NULL argument at its guard and returns its
// refusal value, writing through nothing. A guard whose return is deleted
// goes on to write or read through the NULL: the case dies at that call,
// and the case's name names the refusal it was checking.
// mutation gap: hunt 2/5671 (payloads, client_index NULL: 0 to 1)
// mutation gap: hunt 2/5672 (payloads, client_index NULL: return deleted)
// mutation gap: hunt 2/5674 (payloads, payload_data NULL: 0 to 1)
// mutation gap: hunt 2/5675 (payloads, payload_data NULL: return deleted)
// mutation gap: hunt 2/5677 (payloads, payload_bytes NULL: 0 to 1)
// mutation gap: hunt 2/5678 (payloads, payload_bytes NULL: return deleted)
// mutation gap: hunt 2/5686 (payload_connection_ids, NULL: 0 to 1)
// mutation gap: hunt 2/5687 (payload_connection_ids, NULL: return deleted)
// mutation gap: hunt 2/5692 (payload, client_index NULL: return deleted)
// mutation gap: hunt 2/5694 (payload, payload_bytes NULL: return deleted)
static void test_gaps_r2_st_payload_reads_refuse_null_outs()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    // one payload in the batch, so a refusal that answered with the batch
    // count would answer 1, not 0
    server->payloads.num_payloads = 1;
    server->payloads.client_index[0] = 0;
    server->payloads.payload_bytes[0] = 4;
    server->payloads.payload_data[0] = server->payload_storage;
    server->payloads.connection_id[0] = 7;

    {
        r2_st_asserts_counted_t counted;
        const int * client_index = NULL;
        uint8_t * const * payload_data = NULL;
        const int * payload_bytes = NULL;
        const uint64_t * connection_id = NULL;

        st_ring_expect( rocketnet_server_transport_payloads( server, NULL, &payload_data, &payload_bytes ) == 0, "payloads with a NULL client_index out-pointer did not refuse with 0" );
        st_ring_expect( payload_data == NULL && payload_bytes == NULL, "payloads with a NULL client_index wrote the other out-pointers" );
        st_ring_expect( rocketnet_server_transport_payloads( server, &client_index, NULL, &payload_bytes ) == 0, "payloads with a NULL payload_data out-pointer did not refuse with 0" );
        st_ring_expect( rocketnet_server_transport_payloads( server, &client_index, &payload_data, NULL ) == 0, "payloads with a NULL payload_bytes out-pointer did not refuse with 0" );
        st_ring_expect( client_index == NULL && payload_data == NULL, "payloads refused for a NULL out-pointer wrote the others" );
        st_ring_expect( rocketnet_server_transport_payload_connection_ids( server, NULL ) == 0, "payload_connection_ids with a NULL out-pointer did not refuse with 0" );

        int index = 5;
        int bytes = 5;
        st_ring_expect( rocketnet_server_transport_payload( server, 0, NULL, &bytes ) == NULL, "payload with a NULL client_index out-pointer did not refuse with NULL" );
        st_ring_expect( bytes == 5, "payload refused for a NULL client_index wrote payload_bytes" );
        st_ring_expect( rocketnet_server_transport_payload( server, 0, &index, NULL ) == NULL, "payload with a NULL payload_bytes out-pointer did not refuse with NULL" );
        st_ring_expect( index == 5, "payload refused for a NULL payload_bytes wrote client_index" );
        (void) connection_id;

        st_ring_expect( g_st_ring_asserts == 6, "a NULL out-pointer was not refused at its guard (one assert per refused call)" );
    }

    // the same calls with every argument take the batch
    const int * client_index = NULL;
    uint8_t * const * payload_data = NULL;
    const int * payload_bytes = NULL;
    st_ring_expect( rocketnet_server_transport_payloads( server, &client_index, &payload_data, &payload_bytes ) == 1, "payloads with every argument did not return the batch" );

    server->payloads.num_payloads = 0;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// mutation gap: hunt 2/5710 (start_packet, context NULL: return deleted)
// mutation gap: hunt 2/5770 (packet_payload, context NULL: return deleted)
// mutation gap: hunt 2/5772 (packet_payload, capacity NULL: return deleted)
// mutation gap: hunt 2/5782 (abort_packet, context NULL: return deleted)
// mutation gap: hunt 2/5827 (finish_packet, context NULL: 0 to 1)
// mutation gap: hunt 2/5828 (finish_packet, context NULL: return deleted)
// mutation gap: hunt 2/5907 (send_packet, payload_data NULL: return deleted)
static void test_gaps_r2_st_packet_calls_refuse_null_arguments()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
    rocketnet_unit_check( frame );
    const uint64_t ring_before = server->payload_ring.num_packets;

    {
        r2_st_asserts_counted_t counted;
        st_ring_expect( rocketnet_server_transport_start_packet( server, 0, 16, NULL ) == NULL, "start_packet with a NULL context did not refuse with NULL" );
        st_ring_expect( server->payload_ring.num_packets == ring_before, "start_packet with a NULL context took a ring slot" );

        int capacity = 77;
        st_ring_expect( rocketnet_server_transport_packet_payload( server, NULL, &capacity ) == NULL, "packet_payload with a NULL context did not refuse with NULL" );
        st_ring_expect( capacity == 77, "packet_payload refused for a NULL context wrote the capacity" );
        st_ring_expect( rocketnet_server_transport_packet_payload( server, &context, NULL ) == NULL, "packet_payload with a NULL capacity out-pointer did not refuse with NULL" );

        rocketnet_server_transport_abort_packet( server, NULL );
        st_ring_expect( rocketnet_server_transport_finish_packet( server, NULL, 16 ) == 0, "finish_packet with a NULL context did not refuse with 0" );

        const uint64_t ring_at_send = server->payload_ring.num_packets;
        rocketnet_server_transport_send_packet( server, 0, NULL, 16 );
        st_ring_expect( server->payload_ring.num_packets == ring_at_send, "send_packet with NULL payload_data started a packet" );

        st_ring_expect( g_st_ring_asserts == 6, "a NULL argument to a packet call was not refused at its guard (one assert per refused call)" );
    }

    // the lease the case opened is still whole: it finishes
    memset( frame + context.payload_index, 0x31, 16 );
    st_ring_expect( rocketnet_server_transport_finish_packet( server, &context, 16 ) > 0, "the open lease did not finish after the refused calls" );
    rocketnet_server_transport_send_packets( server );
    st_ring_expect( f->num_datagrams == 1, "the refused calls changed what was sent" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// mutation gap: hunt 2/5451 (begin_message_update, NULL: return deleted)
// mutation gap: hunt 2/6074 (client_rtt, NULL: return deleted)
// mutation gap: hunt 2/6097 (stats, stats NULL: return deleted)
// mutation gap: hunt 2/6115 (acked_packets, client_index NULL: 0 to 1)
// mutation gap: hunt 2/6116 (acked_packets, client_index NULL: return deleted)
// mutation gap: hunt 2/6118 (acked_packets, packet_ids NULL: 0 to 1)
// mutation gap: hunt 2/6119 (acked_packets, packet_ids NULL: return deleted)
// mutation gap: hunt 2/6126 (acked_connection_ids, NULL: 0 to 1)
// mutation gap: hunt 2/6127 (acked_connection_ids, NULL: return deleted)
// mutation gap: hunt 2/6132 (client_stats, stats NULL: return deleted)
// mutation gap: hunt 2/5405 (advertised_address, NULL: return deleted)
// mutation gap: hunt 2/6171 (delivery_capture, NULL: return deleted)
static void test_gaps_r2_st_reads_refuse_null_arguments()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    // one acked entry, so a refusal that answered with the count would answer 1
    server->protocol.acks->num_acked = 1;

    {
        r2_st_asserts_counted_t counted;

        rocketnet_server_transport_begin_message_update( NULL );
        st_ring_expect( rocketnet_server_transport_client_rtt( NULL, 0 ) == 0.0, "client_rtt with a NULL transport did not refuse with 0.0" );
        rocketnet_server_transport_stats( server, NULL );

        const int * client_index = NULL;
        const uint64_t * packet_ids = NULL;
        st_ring_expect( rocketnet_server_transport_acked_packets( server, NULL, &packet_ids ) == 0, "acked_packets with a NULL client_index out-pointer did not refuse with 0" );
        st_ring_expect( packet_ids == NULL, "acked_packets refused for a NULL client_index wrote packet_ids" );
        st_ring_expect( rocketnet_server_transport_acked_packets( server, &client_index, NULL ) == 0, "acked_packets with a NULL packet_ids out-pointer did not refuse with 0" );
        st_ring_expect( client_index == NULL, "acked_packets refused for a NULL packet_ids wrote client_index" );
        st_ring_expect( rocketnet_server_transport_acked_connection_ids( server, NULL ) == 0, "acked_connection_ids with a NULL out-pointer did not refuse with 0" );

        rocketnet_server_transport_client_stats( server, 0, NULL );

        rocketnet_address_t address;
        memset( &address, 0, sizeof( address ) );
        st_ring_expect( !rocketnet_server_transport_advertised_address( NULL, ROCKETNET_ADDRESS_IPV4, &address ), "advertised_address with a NULL transport did not refuse" );
        st_ring_expect( !rocketnet_server_transport_advertised_address( server, ROCKETNET_ADDRESS_IPV4, NULL ), "advertised_address with a NULL address out-pointer did not refuse" );
        st_ring_expect( address.type == 0, "advertised_address refused for a NULL transport wrote the address" );

        struct rocketnet_delivery_capture_t capture;
        memset( &capture, 0, sizeof( capture ) );
        rocketnet_server_transport_delivery_capture( NULL, 0, &capture );
        st_ring_expect( capture.connected == 0, "delivery_capture with a NULL transport filled the capture" );
        rocketnet_server_transport_delivery_capture( server, 0, NULL );

        st_ring_expect( g_st_ring_asserts == 11, "a NULL argument to a read was not refused at its guard (one assert per refused call)" );
    }

    server->protocol.acks->num_acked = 0;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A transport that is stopping has no advertised address and starts no
// packet: both refuse, in every build.
// mutation gap: hunt 2/5408 (advertised_address, not running: return deleted)
// mutation gap: hunt 2/5715 (start_packet, not running: return deleted)
static void test_gaps_r2_st_stopping_transport_refuses_address_and_start()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_server_transport_advertised_address( server, ROCKETNET_ADDRESS_IPV4, &address ) );

    server->state = ROCKETNET_SERVER_TRANSPORT_STOPPING;
    memset( &address, 0, sizeof( address ) );
    st_ring_expect( !rocketnet_server_transport_advertised_address( server, ROCKETNET_ADDRESS_IPV4, &address ), "a stopping transport still returned its advertised address" );
    st_ring_expect( address.type == 0, "a stopping transport wrote its advertised address" );

    const uint64_t ring_before = server->payload_ring.num_packets;
    struct rocketnet_packet_context_t context;
    st_ring_expect( rocketnet_server_transport_start_packet( server, 0, 16, &context ) == NULL, "a stopping transport started a packet" );
    st_ring_expect( server->payload_ring.num_packets == ring_before, "a stopping transport took a ring slot for a start" );

    server->state = ROCKETNET_SERVER_TRANSPORT_RUNNING;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// send_packet to a slot with no connection: start refuses, and the copy of
// the caller's payload is never made.
// mutation gap: hunt 2/5916 (send_packet, refused start: return deleted)
static void test_gaps_r2_st_send_packet_to_a_free_slot_copies_nothing()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    uint8_t payload[32];
    st_ring_fill( payload, sizeof( payload ), 0x41 );
    rocketnet_server_transport_send_packet( server, 1, payload, sizeof( payload ) );
    rocketnet_server_transport_send_packets( server );
    st_ring_expect( f->num_datagrams == 0, "send_packet to a free slot sent a datagram" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The staged finish of a maximum frame
// ----------------------------------------------------------------

static int g_r2_st_finished_bytes = -1;
static void r2_st_finish_recording( void * p, uint64_t id, uint8_t * frame, int bytes )
{
    g_r2_st_finished_bytes = bytes;
    st_ring_finish_packet( p, id, frame, bytes );
}

// A payload frame that fills the send budget exactly makes a wire frame of
// exactly the receive ceiling: 10 header bytes, an ack block of one loss
// range (11 bytes), a payload of 1415 bytes and the 16-byte tag are 1452.
// (A maximum payload of 1416 bytes leaves the ack block no room past its
// 7-byte minimum, so its frame is 1449.) The staged transport finishes it.
// mutation gap: hunt 2/4562 (bytes > ROCKETNET_RECEIVE_CEILING to >=)
static void test_gaps_r2_st_staged_maximum_frame_is_finished()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 8;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );
    operations.finish_packet = r2_st_finish_recording;
    g_r2_st_finished_bytes = -1;

    // the client's accepted packets 5 and 7, with 6 lost: one loss range
    const int mapping_index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 );
    rocketnet_unit_check( mapping_index >= 0 );
    struct rocketnet_replay_protection_t * accepted = &rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index )->accepted_session;
    rocketnet_replay_advance( accepted, 5 );
    rocketnet_replay_advance( accepted, 7 );
    const int payload_bytes = ROCKETNET_RECEIVE_CEILING - 10 - ( ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 ) - 16;
    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    rocketnet_unit_check( rocketnet_session_ack_block_build( ack_block, payload_bytes, accepted ) == ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 );

    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    st_ring_fill( payload, sizeof( payload ), 0x52 );
    const uint64_t pending_sent = server->stats_pending_packets_sent;
    rocketnet_server_transport_send_packet( server, 0, payload, payload_bytes );

    st_ring_expect( f->finishes == 1, "a payload frame of exactly the receive ceiling was not finished on the staged transport" );
    st_ring_expect( g_r2_st_finished_bytes == ROCKETNET_RECEIVE_CEILING, "a payload frame filling the send budget was not finished at exactly the receive ceiling" );
    st_ring_expect( server->stats_pending_packets_sent == pending_sent + 1, "a payload frame of exactly the receive ceiling was not counted sent" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The link simulator's lock
// ----------------------------------------------------------------

// The case holds the simulator's lock, as another producer would: a try on
// it from this thread fails (the platform mutex is not recursive).
static void r2_st_hold_link_lock( rocketnet_server_transport_t * server )
{
    rocketnet_platform_mutex_acquire( &server->link_simulator_mutex );
    const bool second = rocketnet_platform_mutex_try_acquire( &server->link_simulator_mutex );
    rocketnet_unit_check( !second );
}

static void r2_st_release_link_lock( rocketnet_server_transport_t * server )
{
    rocketnet_platform_mutex_release( &server->link_simulator_mutex );
}

// a datagram held by the simulator, as a send through it leaves one: to
// port 2000, which no client uses
static void r2_st_simulator_hold( rocketnet_server_transport_t * server, double time )
{
    rocketnet_address_t to;
    rocketnet_unit_check( rocketnet_address_parse( &to, "127.0.0.1:2000" ) );
    uint8_t packet[24];
    memset( packet, 0x66, sizeof( packet ) );
    rocketnet_network_simulator_send( server->link_simulator, &server->link_simulator_from, &to, packet, sizeof( packet ), time );
}

static int r2_st_datagrams_to_port( const st_ring_fake_t * f, int port )
{
    int count = 0;
    for ( int i = 0; i < f->num_datagrams; i++ )
    {
        if ( f->datagrams[i].to.port == port )
        {
            count++;
        }
    }
    return count;
}

// A send through an active simulator whose lock another producer holds is
// dropped and counted once in SEND_RING_FULL: the real miss, not the
// development busy seam.
// mutation gap: hunt 2/4587 (SEND_RING_FULL add 1 to 2)
// mutation gap: hunt 2/4588 (SEND_RING_FULL add 1 to 0)
// mutation gap: hunt 2/4589 (SEND_RING_FULL add deleted)
static void test_gaps_r2_st_link_lock_miss_counts_ring_full_once()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );
    rocketnet_unit_check( server->link_simulator );
    rocketnet_network_simulator_config( server->link_simulator, 0.010, 0.0, 0.0f, 0.0f );

    uint8_t payload[32];
    st_ring_fill( payload, sizeof( payload ), 0x71 );
    rocketnet_server_transport_send_packet( server, 0, payload, sizeof( payload ) );

    const uint64_t ring_full = server->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL];
    r2_st_hold_link_lock( server );
    rocketnet_server_transport_send_packets( server );
    r2_st_release_link_lock( server );

    st_ring_expect( server->counters.values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + 1, "a send dropped on a busy simulator lock was not counted exactly once in SEND_RING_FULL" );
    st_ring_expect( f->num_datagrams == 0, "a send dropped on a busy simulator lock reached the transport" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The end-of-send window flush with the lock busy keeps the reverse window
// for the next flush: it neither flushes without the lock nor releases a
// lock it does not hold.
// mutation gap: hunt 2/4623 (link_flush, lock miss: return deleted)
static void test_gaps_r2_st_link_flush_keeps_the_window_while_the_lock_is_busy()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );
    rocketnet_unit_check( server->link_simulator );
    rocketnet_unit_check( rocketnet_network_simulator_patterns( server->link_simulator, 0, 2 ) );

    rocketnet_address_t from;
    rocketnet_address_t to;
    uint8_t received[ROCKETNET_RECEIVE_CEILING];

    r2_st_hold_link_lock( server );
    r2_st_simulator_hold( server, 1000.0 );
    rocketnet_server_transport_send_packets( server );
    const int flushed = rocketnet_network_simulator_receive_any( server->link_simulator, 2000.0, &from, &to, received );
    r2_st_release_link_lock( server );

    st_ring_expect( flushed == 0, "the reverse window was flushed while another producer held the simulator lock" );

    // with the lock free, the next flush emits the window
    rocketnet_server_transport_send_packets( server );
    r2_st_hold_link_lock( server );
    const int later = rocketnet_network_simulator_receive_any( server->link_simulator, 2000.0, &from, &to, received );
    r2_st_release_link_lock( server );
    st_ring_expect( later > 0, "the window kept on a lock miss did not leave at the next flush" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The update's drain flushes the reverse window only at the stop: an
// ordinary drain leaves the window to the end-of-update flush, so a packet
// in the window leaves at the next update's drain, not this one's.
// mutation gap: hunt 2/4628 (shutdown && patterns to ||)
static void test_gaps_r2_st_drain_flushes_the_window_only_at_stop()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );
    rocketnet_unit_check( server->link_simulator );
    rocketnet_unit_check( rocketnet_network_simulator_patterns( server->link_simulator, 0, 2 ) );

    r2_st_hold_link_lock( server );
    r2_st_simulator_hold( server, 1000.0 );
    r2_st_release_link_lock( server );

    rocketnet_server_transport_update( server, 1000.5 );
    st_ring_expect( r2_st_datagrams_to_port( f, 2000 ) == 0, "an ordinary update's drain flushed the reverse window and sent its packet in the same update" );

    rocketnet_server_transport_update( server, 1001.0 );
    st_ring_expect( r2_st_datagrams_to_port( f, 2000 ) == 1, "the window flushed at the end of the update did not leave at the next drain" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A drain that finds the simulator's lock busy stops and leaves the held
// packets for the next drain: it never takes from the simulator without the
// lock, nor releases a lock it does not hold.
// mutation gap: hunt 2/4641 (drain, lock miss: break deleted)
static void test_gaps_r2_st_drain_waits_while_the_lock_is_busy()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );
    rocketnet_unit_check( server->link_simulator );
    rocketnet_network_simulator_config( server->link_simulator, 0.010, 0.0, 0.0f, 0.0f );

    r2_st_hold_link_lock( server );
    r2_st_simulator_hold( server, 1000.0 );
    rocketnet_server_transport_update( server, 1001.0 );
    r2_st_release_link_lock( server );
    st_ring_expect( r2_st_datagrams_to_port( f, 2000 ) == 0, "a drain took a held packet from the simulator while another producer held its lock" );

    rocketnet_server_transport_update( server, 1002.0 );
    st_ring_expect( r2_st_datagrams_to_port( f, 2000 ) == 1, "the packet left held on a lock miss did not leave at the next drain" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The receive bounds
// ----------------------------------------------------------------

struct r2_st_view_t
{
    rocketnet_server_socket_process_packets_t view;
    rocketnet_address_t from[4];
    int packet_bytes[4];
    uint8_t * packet_data[4];
    uint8_t data[4][ROCKETNET_RECEIVE_CEILING];
};

static r2_st_view_t g_r2_st_view;

static rocketnet_server_socket_process_packets_t * r2_st_receive_view( void * )
{
    return &g_r2_st_view.view;
}

static void r2_st_view_reset( int num_packets )
{
    memset( &g_r2_st_view, 0, sizeof( g_r2_st_view ) );
    g_r2_st_view.view.capacity = 4;
    g_r2_st_view.view.num_packets = num_packets;
    g_r2_st_view.view.from = g_r2_st_view.from;
    g_r2_st_view.view.packet_bytes = g_r2_st_view.packet_bytes;
    g_r2_st_view.view.packet_data = g_r2_st_view.packet_data;
    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_unit_check( rocketnet_address_parse( &g_r2_st_view.from[i], "198.51.100.7:56789" ) );
        g_r2_st_view.packet_data[i] = g_r2_st_view.data[i];
    }
}

// A staged view holding no packet is the normal idle update, not a fault;
// a negative byte count is a fault counted as no size class, and only a
// count past the ceiling is PREFILTER_TOO_LARGE.
// mutation gap: hunt 2/5534 (num_packets < 0 to <= 0)
// mutation gap: hunt 2/5552 (packet_bytes > ceiling negated)
static void test_gaps_r2_st_staged_view_bounds()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 8;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 0 );
    operations.receive = r2_st_receive_view;

    uint64_t * counters = server->counters.values;

    r2_st_view_reset( 0 );
    const uint64_t faults = server->transport_faults;
    rocketnet_server_transport_update( server, 1000.0 );
    st_ring_expect( server->transport_faults == faults, "an empty staged receive view (the idle update) was counted a transport fault" );

    r2_st_view_reset( 1 );
    g_r2_st_view.packet_bytes[0] = -5;
    const uint64_t too_large = counters[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];
    rocketnet_server_transport_update( server, 1001.0 );
    st_ring_expect( server->transport_faults == faults + 1, "a negative byte count in the staged view was not a transport fault" );
    st_ring_expect( counters[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large, "a negative byte count in the staged view was counted PREFILTER_TOO_LARGE" );

    r2_st_view_reset( 1 );
    g_r2_st_view.packet_bytes[0] = ROCKETNET_RECEIVE_CEILING + 1;
    rocketnet_server_transport_update( server, 1002.0 );
    st_ring_expect( server->transport_faults == faults + 2, "a byte count past the ceiling in the staged view was not a transport fault" );
    st_ring_expect( counters[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == too_large + 1, "a byte count past the ceiling in the staged view was not counted PREFILTER_TOO_LARGE" );

    r2_st_view_reset( 0 );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// the buffered receive: how many datagrams the next receive_batch returns,
// each a 1-byte runt the prefilter drops
static int g_r2_st_batch_count = 0;
static int r2_st_receive_batch_count( void *, rocketnet_address_t * from, uint8_t * data, int, int * packet_bytes, int max_packets )
{
    const int count = g_r2_st_batch_count < 0 ? max_packets : g_r2_st_batch_count;
    for ( int i = 0; i < count; i++ )
    {
        rocketnet_unit_check( rocketnet_address_parse( &from[i], "198.51.100.7:56789" ) );
        data[i * ROCKETNET_RECEIVE_CEILING] = 0x01;
        packet_bytes[i] = 1;
    }
    g_r2_st_batch_count = 0;
    return count;
}

// A receive_batch that returns nothing (the idle update) or exactly the
// slots it was asked for is within its contract: no transport fault.
// mutation gap: hunt 2/5558 (num_received < 0 to < 1)
// mutation gap: hunt 2/5563 (num_received > batch_capacity to >=)
static void test_gaps_r2_st_receive_batch_bounds()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 0 );
    operations.receive_batch = r2_st_receive_batch_count;

    const uint64_t faults = server->transport_faults;
    g_r2_st_batch_count = 0;
    rocketnet_server_transport_update( server, 1000.0 );
    st_ring_expect( server->transport_faults == faults, "an empty receive_batch (the idle update) was counted a transport fault" );

    g_r2_st_batch_count = -1;
    rocketnet_server_transport_update( server, 1001.0 );
    st_ring_expect( server->transport_faults == faults, "a receive_batch returning exactly the slots asked for was counted a transport fault" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The maintenance sweep
// ----------------------------------------------------------------

static int g_r2_st_unreachable_port = -1;
static int g_r2_st_finish_order[64];
static int g_r2_st_num_finished = 0;

static uint8_t * r2_st_start_unreachable( void * p, const rocketnet_address_t * to, uint64_t * id )
{
    if ( to->port == g_r2_st_unreachable_port )
    {
        *id = ROCKETNET_TRANSPORT_PACKET_UNREACHABLE;
        return NULL;
    }
    return st_ring_start_packet( p, to, id );
}

static void r2_st_finish_ordered( void * p, uint64_t id, uint8_t * frame, int bytes )
{
    st_ring_fake_t * f = (st_ring_fake_t *) p;
    if ( g_r2_st_num_finished < (int) ( sizeof( g_r2_st_finish_order ) / sizeof( g_r2_st_finish_order[0] ) ) )
    {
        g_r2_st_finish_order[g_r2_st_num_finished++] = st_ring_client_of( &f->reserved_to );
    }
    st_ring_finish_packet( p, id, frame, bytes );
}

// A slot the transport cannot address (no link-layer destination) is
// counted SEND_NO_LINK_ADDRESS once per visit and passed: the cursor moves
// past it, so the next update does not open on it again before the
// clients the room can serve.
// mutation gap: hunt 2/5623 (cursor after an unreachable visit deleted)
static void test_gaps_r2_st_sweep_passes_an_unreachable_slot()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 2;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 2 );
    operations.start_packet = r2_st_start_unreachable;
    g_r2_st_unreachable_port = ST_RING_PORT_BASE + 1;

    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t no_link = counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS];

    rocketnet_server_transport_update( server, 1000.0 );
    st_ring_expect( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == no_link + 1, "the unreachable slot was not counted once in its first sweep" );
    st_ring_expect( f->finished_to[0] == 1, "the reachable slot was not served in the first sweep" );

    // room for one frame: the sweep opens on slot 0 and stops at slot 1
    f->capacity = 1;
    rocketnet_server_transport_update( server, 1010.0 );
    st_ring_expect( counters[ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS] == no_link + 1, "the sweep opened on the unreachable slot it had passed and counted it again" );
    st_ring_expect( f->finished_to[0] == 2, "the room went to the unreachable slot instead of the next reachable one" );

    g_r2_st_unreachable_port = -1;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A full pass over every slot ends with the cursor back where it began, so
// with every client due the update serves them in slot order each time.
// mutation gap: hunt 2/5634 (cursor ( i + 1 ) to ( i + 2 ))
static void test_gaps_r2_st_sweep_full_pass_keeps_its_start()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 8;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 3, 3 );
    operations.finish_packet = r2_st_finish_ordered;

    double time = 1000.0;
    for ( int update = 0; update < 3; update++ )
    {
        g_r2_st_num_finished = 0;
        rocketnet_server_transport_update( server, time );
        st_ring_expect( g_r2_st_num_finished == 3, "a sweep with room for every due client did not serve all three" );
        st_ring_expect( g_r2_st_finish_order[0] == 0 && g_r2_st_finish_order[1] == 1 && g_r2_st_finish_order[2] == 2,
                        "a full sweep did not end with the cursor on the slot it began at (the next sweep started elsewhere)" );
        time += 10.0;
    }

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A due slot whose keep-alive and ack both write nothing gives its
// reserved frame back to the staged transport.
// mutation gap: hunt 2/5633 (abort of the unwritten maintenance frame deleted)
static void test_gaps_r2_st_sweep_returns_an_unwritten_frame()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 8;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );

    // sequence 0 is never sealed: the keep-alive write fails, and no ack is owed
    const int mapping_index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, 0 );
    rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index )->send_sequence_session = 0;

    {
        r2_st_asserts_counted_t counted;
        rocketnet_server_transport_update( server, 1000.0 );
    }

    st_ring_expect( f->starts == 1, "the due slot's maintenance frame was not reserved" );
    st_ring_expect( f->finishes == 0, "a maintenance frame with nothing written was finished" );
    st_ring_expect( f->aborts == 1, "a maintenance frame with nothing written was not given back to the staged transport" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The replay commit
// ----------------------------------------------------------------

static int g_r2_st_foreign_contexts = 0;
static st_ring_fake_t * g_r2_st_replay_fake = NULL;

static bool r2_st_replay_forget( void * p, const rocketnet_address_t * address )
{
    if ( p != g_r2_st_replay_fake )
    {
        g_r2_st_foreign_contexts++;
        return true;
    }
    return st_ring_replay_forget( p, address );
}

static bool r2_st_replay_mark( void * p, const rocketnet_address_t * address, uint32_t high_water )
{
    if ( p != g_r2_st_replay_fake )
    {
        g_r2_st_foreign_contexts++;
        return true;
    }
    return st_ring_replay_mark( p, address, high_water );
}

// The update's replay walk hands the transport its own instance with its
// own mark, and commits the marks once per update.
// mutation gap: hunt 2/5652 (sink context deleted)
// mutation gap: hunt 2/5654 (sink mark deleted)
// mutation gap: hunt 2/5655 (replay_commit deleted)
static void test_gaps_r2_st_replay_marks_are_committed()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );

    operations.replay_forget = r2_st_replay_forget;
    operations.replay_mark = r2_st_replay_mark;
    operations.replay_commit = st_ring_replay_commit;
    server->replay_marks = rocketnet_server_replay_marks_create( server->context, 2 );
    rocketnet_unit_check( server->replay_marks );
    g_r2_st_replay_fake = f;
    g_r2_st_foreign_contexts = 0;

    {
        r2_st_asserts_counted_t counted;
        st_ring_fill_stack_call( 0x5A );
        rocketnet_server_transport_update( server, 1000.0 );
        st_ring_expect( g_st_ring_asserts == 0, "the replay walk was handed an incomplete sink" );
    }

    st_ring_expect( g_r2_st_foreign_contexts == 0, "the replay walk handed the transport's operations a context that is not its instance" );
    st_ring_expect( f->marks >= 1, "the replay walk did not mark the connected client through the transport's replay_mark" );
    st_ring_expect( f->commits == 1, "the update did not commit the replay marks to the transport once" );

    rocketnet_server_transport_update( server, 1001.0 );
    st_ring_expect( f->commits == 2, "the second update did not commit the replay marks" );

    g_r2_st_replay_fake = NULL;
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// A refused response frame is counted
// ----------------------------------------------------------------

// A connection request without a cookie, from a stranger, to a staged
// transport whose staging is full: the cookie response's frame is refused
// at its start, uncounted there, and counted SEND_RING_FULL by the receive
// path.
// mutation gap: hunt 2/5493 (count of the refused response frame deleted)
static void test_gaps_r2_st_refused_response_frame_is_counted()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 0;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 0 );
    operations.receive = r2_st_receive_view;

    // the test vectors' server identity
    struct rocketnet_server_protocol_t * protocol = &server->protocol;
    protocol->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( protocol->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( protocol->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( protocol->server_public_key, rocketnet_test_server_public_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &protocol->advertised_ipv4, "203.0.113.10:40000" ) );
    rocketnet_server_transport_set_test_timestamp( server, ROCKETNET_TEST_CREATE_TS + 1 );

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 1;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    r2_st_view_reset( 1 );
    rocketnet_write_connection_request_packet( g_r2_st_view.data[0], tag, NULL, rocketnet_test_salt, rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );
    g_r2_st_view.packet_bytes[0] = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;

    const uint64_t * counters = protocol->counters->values;
    const uint64_t no_cookie = counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE];
    const uint64_t ring_full = counters[ROCKETNET_COUNTER_SEND_RING_FULL];
    rocketnet_server_transport_update( server, 1000.0 );

    st_ring_expect( counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] == no_cookie + 1, "the cookie-less request did not reach the cookie answer" );
    st_ring_expect( counters[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full + 1, "a response frame refused by a full staging was not counted SEND_RING_FULL" );
    st_ring_expect( f->finishes == 0, "a refused response frame was finished" );

    r2_st_view_reset( 0 );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// Slots without a mapping or a connection id
// ----------------------------------------------------------------

// a connected slot holding mapping index -1 (a state the slot API permits),
// in slot 0 of a server with no other client
static void r2_st_seat_without_mapping( rocketnet_server_transport_t * server )
{
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
    rocketnet_unit_check( rocketnet_client_slots_assign( server->protocol.client_slots, 99, -1, 99, user_data ) == 0 );
    server->protocol.connection_ids[0] = ++server->protocol.next_connection_id;
}

// Disconnecting a connected slot with no mapping ends it locally: the slot
// is free, the missing mapping is counted SLOT_WITHOUT_MAPPING once, and the
// disconnect is counted.
// mutation gap: hunt 2/5920 (note_missing_mapping in close_client_locally deleted)
// mutation gap: hunt 2/6015 (close_client_locally for mapping -1 deleted)
static void test_gaps_r2_st_disconnect_without_mapping_ends_the_slot()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 0 );
    r2_st_seat_without_mapping( server );

    const uint64_t * counters = server->protocol.counters->values;
    const uint64_t without_mapping = counters[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING];
    const uint64_t disconnected = counters[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT];

    rocketnet_server_transport_disconnect_client( server, 0 );

    st_ring_expect( !rocketnet_client_slots_connected( server->protocol.client_slots, 0 ), "a connected slot without a mapping stayed connected after disconnect_client" );
    st_ring_expect( counters[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING] == without_mapping + 1, "the disconnect of a slot without a mapping was not counted SLOT_WITHOUT_MAPPING once" );
    st_ring_expect( counters[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT] == disconnected + 1, "the disconnect of a slot without a mapping was not counted" );
    st_ring_expect( f->starts == 0, "a burst was staged for a slot with no address" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A payload frame from a slot whose connection id is 0 (no connection the
// frame can be matched to) is charged to the server totals only, never to
// the slot.
// mutation gap: hunt 2/5844 (send_slot_holds, connection id 0: return deleted)
static void test_gaps_r2_st_frame_without_connection_id_charges_no_slot()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );
    server->protocol.connection_ids[0] = 0;

    const uint64_t slot_sent = server->client_wire_totals[0].packets_sent;
    const uint64_t server_sent = server->server_stats.packets_sent;

    uint8_t payload[32];
    st_ring_fill( payload, sizeof( payload ), 0x29 );
    rocketnet_server_transport_send_packet( server, 0, payload, sizeof( payload ) );
    rocketnet_server_transport_send_packets( server );

    st_ring_expect( f->num_datagrams == 1, "the payload frame did not leave" );
    st_ring_expect( server->server_stats.packets_sent == server_sent + 1, "the payload frame was not charged to the server totals" );
    st_ring_expect( server->client_wire_totals[0].packets_sent == slot_sent, "a frame with connection id 0 was charged to a slot whose connection id is also 0" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// An immediate disconnect burst whose first start is refused (the staging
// refuses the destination with room left) stops there: nothing is written
// into the refused frame.
// mutation gap: hunt 2/6039 (immediate burst, refused reserve: break deleted)
static void test_gaps_r2_st_immediate_burst_stops_at_a_refused_start()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    f->capacity = 64;
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_STAGED, 2, 1 );

    rocketnet_server_transport_begin_message_update( server );
    f->refuse_without_id = true;
    rocketnet_server_transport_disconnect_client( server, 0 );
    f->refuse_without_id = false;

    st_ring_expect( !rocketnet_client_slots_connected( server->protocol.client_slots, 0 ), "the disconnected slot stayed connected" );
    st_ring_expect( server->disconnect_burst_count == 0, "the immediate burst was deferred, so the refused start was not on the immediate path" );
    st_ring_expect( f->finishes == 0, "a disconnect frame was finished after its start was refused" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// rtt, heard age and the delivery capture
// ----------------------------------------------------------------

// mutation gap: hunt 2/6073 (client_rtt guard negated)
static void test_gaps_r2_st_client_rtt_reads_the_endpoint()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 1 );

    server->protocol.acks->endpoints[0].rtt = 50;
    st_ring_expect( rocketnet_server_transport_client_rtt( server, 0 ) == 0.05, "client_rtt of a connected slot did not read its endpoint's rtt" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// A slot whose mapping is gone reads as heard never and captures as not
// connected; a slot out of range leaves the capture untouched; the highest
// sent sequence is the one before the next to send.
// mutation gap: hunt 2/6166 (heard_age, mapping -1: return deleted)
// mutation gap: hunt 2/6178 (delivery_capture, index out of range: return deleted)
// mutation gap: hunt 2/6184 (delivery_capture, mapping -1: return deleted)
// mutation gap: hunt 2/6200 (highest_sent_sequence - 1 to - 0)
static void test_gaps_r2_st_capture_and_heard_age_bounds()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 0 );
    r2_st_seat_without_mapping( server );
    st_ring_connect( server, 1 );
    server->time = 1004.0;

    st_ring_expect( rocketnet_server_transport_heard_age( server, 0 ) == -1.0, "heard_age of a connected slot without a mapping did not read -1" );
    st_ring_expect( rocketnet_server_transport_heard_age( server, 1 ) == 4.0, "heard_age of a connected slot did not read the time since its last receive" );

    struct rocketnet_delivery_capture_t capture;
    memset( &capture, 0, sizeof( capture ) );
    rocketnet_server_transport_delivery_capture( server, 0, &capture );
    st_ring_expect( capture.connected == 0 && capture.server_slot == 0, "the capture of a slot without a mapping was filled" );
    rocketnet_server_transport_delivery_capture( server, 2, &capture );
    st_ring_expect( capture.connected == 0 && capture.server_slot == 0, "the capture of a slot past max_clients was filled" );
    rocketnet_server_transport_delivery_capture( server, -1, &capture );
    st_ring_expect( capture.connected == 0, "the capture of slot -1 was filled" );

    const int mapping_index = rocketnet_client_slots_mapping_index( server->protocol.client_slots, 1 );
    rocketnet_encryption_map_entry( server->protocol.encryption_map, mapping_index )->send_sequence_session = 5;
    rocketnet_server_transport_delivery_capture( server, 1, &capture );
    st_ring_expect( capture.connected == 1 && capture.server_slot == 1, "the capture of a connected slot was not filled" );
    st_ring_expect( capture.highest_sent_sequence == 4, "the capture's highest sent sequence is not the one before the next to send" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The transport's own counters, read through counters()
// ----------------------------------------------------------------

static void r2_st_prefilter_counters( void *, uint64_t * values )
{
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
    {
        values[i] = (uint64_t) ( 100 + i );
    }
}

// A transport that reports prefilter counters has them read into the
// transport's counters on every read.
// mutation gap: hunt 2/6079 (prefilter_counters call deleted)
// mutation gap: hunt 2/6080 (counters_apply_prefilter deleted)
static void test_gaps_r2_st_counters_read_the_prefilter()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 0 );
    operations.prefilter_counters = r2_st_prefilter_counters;

    st_ring_fill_stack_call( 0xC3 );
    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    rocketnet_unit_check( counters );
    st_ring_expect( counters->values[ROCKETNET_COUNTER_XDP_KERNEL_REDIRECTED] == 100, "the transport's prefilter counters were not read into XDP_KERNEL_REDIRECTED" );
    st_ring_expect( counters->values[ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL] == 101, "the transport's prefilter counters were not read into XDP_KERNEL_DROPPED_TOO_SMALL" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// ----------------------------------------------------------------
// The development seams and fixtures
// ----------------------------------------------------------------

// The burst-key seam refuses an index outside the ring and a short buffer.
// mutation gap: hunt 2/5999 (refusal 0 to 1)
// mutation gap: hunt 2/6000 (refusal: return deleted)
static void test_gaps_r2_st_burst_key_seam_refuses_out_of_range()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 0 );

    uint8_t key[ROCKETNET_KEY_BYTES + 8];
    memset( key, 0xEE, sizeof( key ) );
    int head = -7;
    st_ring_expect( rocketnet_server_transport_disconnect_burst_key_for_test( server, server->disconnect_burst_capacity, key, sizeof( key ), &head, NULL ) == 0, "the burst-key seam accepted an index past the ring" );
    st_ring_expect( rocketnet_server_transport_disconnect_burst_key_for_test( server, -1, key, sizeof( key ), &head, NULL ) == 0, "the burst-key seam accepted index -1" );
    st_ring_expect( rocketnet_server_transport_disconnect_burst_key_for_test( server, 0, key, ROCKETNET_KEY_BYTES - 1, &head, NULL ) == 0, "the burst-key seam accepted a buffer shorter than a key" );
    st_ring_expect( head == -7 && key[0] == 0xEE, "a refused burst-key read wrote its outputs" );
    st_ring_expect( rocketnet_server_transport_disconnect_burst_key_for_test( server, 0, key, sizeof( key ), &head, NULL ) == 1, "the burst-key seam refused ring index 0" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

// The seat seam refuses when it cannot seat: the encryption map is full, or
// the slot API hands back a slot other than the one the seam seats.
// mutation gap: hunt 2/4399 (map full: return false deleted)
// mutation gap: hunt 2/4404 (other slot: return false deleted)
static void test_gaps_r2_st_seat_seam_refuses_what_it_cannot_seat()
{
    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 0 );

    // slot 0 taken (by a client on another port): the seam's first seat
    // lands in slot 1, not the slot 0 it seats
    rocketnet_unit_check( st_ring_connect_port( server, 3 ) == 0 );
    st_ring_expect( !rocketnet_server_transport_seat_clients_for_test( server, 1 ), "the seat seam returned true for a slot it did not seat" );
    rn_test_server_transport_stop_and_destroy( server );
    delete f;

    // every map entry taken by a stranger: the seam cannot add a mapping
    f = new st_ring_fake_t();
    server = st_ring_server( f, &operations, ST_RING_BUFFERED, 2, 0 );
    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    int added = 0;
    for ( int port = 5000; port < 5000 + 4096; port++ )
    {
        address.port = (uint16_t) port;
        if ( rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 1000.0 ) < 0 )
        {
            break;
        }
        added++;
    }
    rocketnet_unit_check( added > 0 );
    st_ring_expect( !rocketnet_server_transport_seat_clients_for_test( server, 1 ), "the seat seam returned true with the encryption map full" );
    st_ring_expect( !rocketnet_client_slots_connected( server->protocol.client_slots, 0 ), "the seat seam seated a slot with the encryption map full" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;
}

static bool g_r2_st_alloc_failure_armed = false;
static void * r2_st_alloc_failure_malloc( void *, size_t bytes )
{
    return g_r2_st_alloc_failure_armed ? NULL : malloc( bytes );
}
static void r2_st_alloc_failure_free( void *, void * p )
{
    free( p );
}

// the worker starts initialized: terminate, and initialize again with an
// allocator the case can fail
static void r2_st_failing_allocator_begin()
{
    g_r2_st_alloc_failure_armed = false;
    rocketnet_unit_check( rocketnet_test_loopback_hub_release() );
    rocketnet_unit_check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    rocketnet_unit_check( rocketnet_init_internal( NULL, r2_st_alloc_failure_malloc, r2_st_alloc_failure_free, &status ) );
}

static void r2_st_failing_allocator_end()
{
    g_r2_st_alloc_failure_armed = false;
    rocketnet_unit_check( rocketnet_test_loopback_hub_release() );
    rocketnet_unit_check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    rocketnet_unit_check( rocketnet_init_internal( NULL, NULL, NULL, &status ) );
}

// The slot-clean seam names what it finds, and why it could not look: a
// connected slot by its connected flag; a free slot is clean while another
// client's mapping stands; a free slot's dirty ack endpoint is named; a
// failed scratch allocation reads "could not run", not "not reset".
// mutation gap: hunt 2/4430 (member connected deleted)
// mutation gap: hunt 2/4435 (mapping client_index == to !=)
// mutation gap: hunt 2/4436 (mapping client_index == negated)
// mutation gap: hunt 2/4437 (member client_index deleted)
// mutation gap: hunt 2/4449 (member endpoints deleted)
// mutation gap: hunt 2/4446 (allocation_failed = true deleted)
static void test_gaps_r2_st_slot_clean_seam_names_the_member()
{
    r2_st_failing_allocator_begin();

    st_ring_fake_t * f = new st_ring_fake_t();
    rocketnet_server_transport_io_operations_t operations;
    rocketnet_server_transport_t * server = st_ring_server( f, &operations, ST_RING_BUFFERED, 3, 0 );

    char failure[256];

    st_ring_connect( server, 0 );
    memset( failure, 0, sizeof( failure ) );
    st_ring_expect( !rocketnet_server_transport_slot_clean_for_test( server, 0, failure, sizeof( failure ) ), "a connected slot passed as clean" );
    st_ring_expect( strstr( failure, "rocketnet_client_slots_t.connected" ) != NULL, "the failure for a connected slot did not name its connected flag" );

    st_ring_expect( rocketnet_server_transport_slot_clean_for_test( server, 1, failure, sizeof( failure ) ), "a free slot was not clean while another client's mapping stands" );

    rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:3002" ) );
    const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 1000.0 );
    rocketnet_unit_check( index >= 0 );
    rocketnet_encryption_map_control( server->protocol.encryption_map, index )->client_index = 2;
    memset( failure, 0, sizeof( failure ) );
    st_ring_expect( !rocketnet_server_transport_slot_clean_for_test( server, 2, failure, sizeof( failure ) ), "a free slot a mapping still names passed as clean" );
    st_ring_expect( strstr( failure, "rocketnet_encryption_mapping_control_t.client_index" ) != NULL, "the failure did not name the mapping's client_index" );
    rocketnet_encryption_map_control( server->protocol.encryption_map, index )->client_index = -1;

    server->protocol.acks->endpoints[1].rtt = 9;
    memset( failure, 0, sizeof( failure ) );
    st_ring_expect( !rocketnet_server_transport_slot_clean_for_test( server, 1, failure, sizeof( failure ) ), "a free slot with a dirty ack endpoint passed as clean" );
    st_ring_expect( strstr( failure, "rocketnet_server_acks_t.endpoints" ) != NULL, "the failure did not name the dirty ack endpoint" );
    server->protocol.acks->endpoints[1].rtt = 0;

    g_r2_st_alloc_failure_armed = true;
    memset( failure, 0, sizeof( failure ) );
    const bool clean = rocketnet_server_transport_slot_clean_for_test( server, 1, failure, sizeof( failure ) );
    g_r2_st_alloc_failure_armed = false;
    st_ring_expect( !clean, "the slot-clean check passed without its scratch memory" );
    st_ring_expect( strstr( failure, "allocation failed" ) != NULL, "a slot-clean check that could not allocate read as a slot not reset, not as a check that could not run" );

    rn_test_server_transport_stop_and_destroy( server );
    delete f;

    r2_st_failing_allocator_end();
}

// The reflush fixtures answer -1 when their transport cannot be allocated
// and -2 for a transport kind not built here, never a flush count.
// mutation gap: hunt 2/6219 (reflush_flags allocation failure -1 to -2)
// mutation gap: hunt 2/6220 (reflush_flags allocation failure -1 to 0)
// mutation gap: hunt 2/6221 (reflush_flags allocation failure: return deleted)
// mutation gap: hunt 2/6237 (not built -2 to -3)
// mutation gap: hunt 2/6238 (not built -2 to -1)
// mutation gap: hunt 2/6239 (not built: return deleted)
// mutation gap: hunt 2/6241 (staged_simulator_flush allocation failure -1 to -2)
// mutation gap: hunt 2/6242 (staged_simulator_flush allocation failure -1 to 0)
// mutation gap: hunt 2/6243 (staged_simulator_flush allocation failure: return deleted)
static void test_gaps_r2_st_reflush_fixtures_refuse()
{
    int not_built = -1;
    for ( int kind = 0; kind < ROCKETNET_NUM_TRANSPORTS && not_built < 0; kind++ )
    {
        const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( kind );
        if ( !definition || !definition->backend || !definition->backend->server )
        {
            not_built = kind;
        }
    }
    if ( not_built >= 0 )
    {
        st_ring_expect( rocketnet_server_transport_test_reflush( not_built ) == -2, "the reflush fixture did not answer -2 for a transport kind not built here" );
    }

    r2_st_failing_allocator_begin();
    g_r2_st_alloc_failure_armed = true;
    const int reflush = rocketnet_server_transport_test_reflush_flags( ROCKETNET_TRANSPORT_STAGED );
    const int simulator_flush = rocketnet_server_transport_test_staged_simulator_flush();
    g_r2_st_alloc_failure_armed = false;
    r2_st_failing_allocator_end();

    st_ring_expect( reflush == -1, "the reflush fixture did not answer -1 when its transport could not be allocated" );
    st_ring_expect( simulator_flush == -1, "the staged simulator flush fixture did not answer -1 when its transport could not be allocated" );
}

REGISTER_UNIT_TEST( test_gaps_r2_st_payload_reads_refuse_null_outs );
REGISTER_UNIT_TEST( test_gaps_r2_st_packet_calls_refuse_null_arguments );
REGISTER_UNIT_TEST( test_gaps_r2_st_reads_refuse_null_arguments );
REGISTER_UNIT_TEST( test_gaps_r2_st_stopping_transport_refuses_address_and_start );
REGISTER_UNIT_TEST( test_gaps_r2_st_send_packet_to_a_free_slot_copies_nothing );
REGISTER_UNIT_TEST( test_gaps_r2_st_staged_maximum_frame_is_finished );
REGISTER_UNIT_TEST( test_gaps_r2_st_link_lock_miss_counts_ring_full_once );
REGISTER_UNIT_TEST( test_gaps_r2_st_link_flush_keeps_the_window_while_the_lock_is_busy );
REGISTER_UNIT_TEST( test_gaps_r2_st_drain_flushes_the_window_only_at_stop );
REGISTER_UNIT_TEST( test_gaps_r2_st_drain_waits_while_the_lock_is_busy );
REGISTER_UNIT_TEST( test_gaps_r2_st_staged_view_bounds );
REGISTER_UNIT_TEST( test_gaps_r2_st_receive_batch_bounds );
REGISTER_UNIT_TEST( test_gaps_r2_st_sweep_passes_an_unreachable_slot );
REGISTER_UNIT_TEST( test_gaps_r2_st_sweep_full_pass_keeps_its_start );
REGISTER_UNIT_TEST( test_gaps_r2_st_sweep_returns_an_unwritten_frame );
REGISTER_UNIT_TEST( test_gaps_r2_st_replay_marks_are_committed );
REGISTER_UNIT_TEST( test_gaps_r2_st_refused_response_frame_is_counted );
REGISTER_UNIT_TEST( test_gaps_r2_st_disconnect_without_mapping_ends_the_slot );
REGISTER_UNIT_TEST( test_gaps_r2_st_frame_without_connection_id_charges_no_slot );
REGISTER_UNIT_TEST( test_gaps_r2_st_immediate_burst_stops_at_a_refused_start );
REGISTER_UNIT_TEST( test_gaps_r2_st_client_rtt_reads_the_endpoint );
REGISTER_UNIT_TEST( test_gaps_r2_st_capture_and_heard_age_bounds );
REGISTER_UNIT_TEST( test_gaps_r2_st_counters_read_the_prefilter );
REGISTER_UNIT_TEST( test_gaps_r2_st_burst_key_seam_refuses_out_of_range );
REGISTER_UNIT_TEST( test_gaps_r2_st_seat_seam_refuses_what_it_cannot_seat );
REGISTER_UNIT_TEST( test_gaps_r2_st_slot_clean_seam_names_the_member );
REGISTER_UNIT_TEST( test_gaps_r2_st_reflush_fixtures_refuse );
