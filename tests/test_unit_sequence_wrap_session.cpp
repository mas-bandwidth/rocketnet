/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The wire's 32-bit sequence field wraps inside one session, both directions.
//
// The wire carries the low 32 bits of a 64-bit session sequence and the
// receiver reconstructs the full value (standard/TRANSPORT.md t4 rules 1-3,
// vector t4.sequence-wrap; sdk/rocketnet_protocol.cpp
// rocketnet_reconstruct_sequence). A real connection passes 0xFFFFFFFF only
// after 2^32 packets, days at soak rates, so this case starts both sessions
// through the DEV-only seam ROCKETNET_DEV_SESSION_SEQUENCE_START
// (sdk/rocketnet_test_seams.h) at 0xFFFFFFFF - 1000: read once per process,
// at the server's encryption map create, and each side's first session packet
// carries that sequence. The case runs in its own process (the suite runs
// every case as `test --run <case>`), so the value reaches nothing else.
//
// A message server and client on the in-memory loopback transport, no socket,
// time from 1000.0, both sides updated every step of SEQUENCE_WRAP_DT. Each
// step sends one reliable ordered message each way, SEQUENCE_WRAP_STEPS steps,
// so at least that many session packets go each way and the field wraps
// about a thousand packets in. Each side's link seam records every packet it
// finishes (the 64-bit sequence finish fills in, rocketnet_layers.h
// rocketnet_packet_context_t::sequence) and every ack that reaches it.
//
// Pinned across the wrap, on each side: both sides send past 0xFFFFFFFF;
// every message is delivered exactly once and in order; packets sent past the
// wrap are acked; the RTT samples those acks give and the smoothed RTT stay in
// the step cadence's [dt, 4*dt] (as tests/test_unit_loopback_rtt.cpp derives);
// nothing is resent; every REFUSAL counter on both transports reads zero (a
// replay window or reconstruction broken by the wrap would refuse or fail to
// open the packets past it); and every counter family is conserved.
//
// The planted control: with the seam's start applied and the t4
// reconstruction replaced by the raw 32-bit field (rocketnet_reconstruct_sequence
// returning wire_sequence), the first packet past the wrap reconstructs to
// zero and is ignored and every later one opens under the wrong nonce, so
// nothing past the wrap is delivered or acked, the reliable window fills and
// the case goes red (on the 2026-10-23 run, at the send check
// sequence_wrap_send makes once the window is full). The case is only
// meaningful because that fails.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "portable_crt.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_seams.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"
#include "rocketnet_channel.h"
#include "rocketnet_window_ack.h"
#include "rocketnet_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

#if ROCKETNET_DEV
#define SEQUENCE_WRAP_DT 0.01
#define SEQUENCE_WRAP_STEPS 5000
#define SEQUENCE_WRAP_SETTLE 100
#define SEQUENCE_WRAP_START ( 0xFFFFFFFFULL - 1000ULL )
#define SEQUENCE_WRAP_FIELD_LIMIT 0xFFFFFFFFULL
#define SEQUENCE_WRAP_MAX_PACKETS 16384

static void sequence_wrap_setenv( const char * name, const char * value ) { check( rocketnet_test_env_setenv( name, value, 1 ) == 0 ); }
static void sequence_wrap_unsetenv( const char * name ) { check( rocketnet_test_env_unsetenv( name ) == 0 ); }

static double sequence_wrap_clock = 1000.0;
static double sequence_wrap_time_callback( void * ) { return sequence_wrap_clock; }

// one side's link seam record: per finished packet its id, 64-bit sequence
// and send time; per ack the sequence it names and the RTT sample it gives

struct sequence_wrap_seam_t
{
    uint64_t packet_id[SEQUENCE_WRAP_MAX_PACKETS];
    uint64_t sequence[SEQUENCE_WRAP_MAX_PACKETS];
    double send_time[SEQUENCE_WRAP_MAX_PACKETS];
    int packets;
    int packets_overflow;
    uint64_t lowest_sequence;
    uint64_t highest_sequence;
    int packets_past_wrap;
    int acked;
    int acked_past_wrap;
    int samples_past_wrap;
    double sample_min;
    double sample_max;
    int ( *finish_next )( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes );
    int ( *acked_next )( void * transport, const int ** client_index, const uint64_t ** packet_ids );
};

static struct sequence_wrap_seam_t sequence_wrap_seams[2];      // 0 server, 1 client

static void sequence_wrap_record_finish( struct sequence_wrap_seam_t * seam, uint64_t packet_id, uint64_t sequence )
{
    if ( seam->packets >= SEQUENCE_WRAP_MAX_PACKETS )
    {
        seam->packets_overflow = 1;
        return;
    }

    seam->packet_id[seam->packets] = packet_id;
    seam->sequence[seam->packets] = sequence;
    seam->send_time[seam->packets] = sequence_wrap_clock;
    seam->packets++;
    if ( seam->lowest_sequence == 0 || sequence < seam->lowest_sequence )
    {
        seam->lowest_sequence = sequence;
    }

    if ( sequence > seam->highest_sequence )
    {
        seam->highest_sequence = sequence;
    }

    if ( sequence > SEQUENCE_WRAP_FIELD_LIMIT )
    {
        seam->packets_past_wrap++;
    }
}

static void sequence_wrap_record_acks( struct sequence_wrap_seam_t * seam, int count, const uint64_t * packet_ids )
{
    for ( int k = 0; k < count; k++ )
    {
        // acks name recent packets: search from the newest

        for ( int i = seam->packets - 1; i >= 0; i-- )
        {
            if ( seam->packet_id[i] != packet_ids[k] )
                continue;
            seam->acked++;
            if ( seam->sequence[i] > SEQUENCE_WRAP_FIELD_LIMIT )
            {
                seam->acked_past_wrap++;
                const double sample = sequence_wrap_clock - seam->send_time[i];
                if ( seam->samples_past_wrap == 0 || sample < seam->sample_min )
                {
                    seam->sample_min = sample;
                }

                if ( seam->samples_past_wrap == 0 || sample > seam->sample_max )
                {
                    seam->sample_max = sample;
                }

                seam->samples_past_wrap++;
            }

            break;
        }
    }
}

static int sequence_wrap_server_finish( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    const uint64_t packet_id = context->packet_id;
    const int wire_bytes = sequence_wrap_seams[0].finish_next( transport, context, payload_bytes );
    if ( wire_bytes > 0 )
    {
        sequence_wrap_record_finish( &sequence_wrap_seams[0], packet_id, context->sequence );
    }

    return wire_bytes;
}

static int sequence_wrap_client_finish( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    const uint64_t packet_id = context->packet_id;
    const int wire_bytes = sequence_wrap_seams[1].finish_next( transport, context, payload_bytes );
    if ( wire_bytes > 0 )
    {
        sequence_wrap_record_finish( &sequence_wrap_seams[1], packet_id, context->sequence );
    }

    return wire_bytes;
}

static int sequence_wrap_server_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    const int count = sequence_wrap_seams[0].acked_next( transport, client_index, packet_ids );
    sequence_wrap_record_acks( &sequence_wrap_seams[0], count, *packet_ids );
    return count;
}

static int sequence_wrap_client_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    const int count = sequence_wrap_seams[1].acked_next( transport, client_index, packet_ids );
    sequence_wrap_record_acks( &sequence_wrap_seams[1], count, *packet_ids );
    return count;
}

struct sequence_wrap_pair_t
{
    struct rocketnet_message_server_t * server;
    struct rocketnet_message_client_t * client;
    double time;
};

static bool sequence_wrap_pair_setup( struct sequence_wrap_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    sequence_wrap_clock = pair->time;
    rocketnet_quiet( 1 );

    const uint64_t protocol_id = 0x7365717772617031ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    pair->server = rocketnet_message_server_create( NULL, "127.0.0.1:0", &server_config, NULL, NULL, 0, 0 );
    check( pair->server != NULL );
    if ( !pair->server )
        return false;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d",
              rocketnet_server_transport_port( rocketnet_message_server_transport( pair->server ) ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                     0x5eb0e5a1c3d2f001ULL, 0, 1ULL << 40, 60, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = sequence_wrap_time_callback;

    pair->client = rocketnet_message_client_create( NULL, token, &client_config, NULL, NULL, 0, 0 );
    check( pair->client != NULL );
    return pair->client != NULL;
}

struct sequence_wrap_direction_t
{
    int sent;
    int received;
    int in_order;
};

static void sequence_wrap_drain( struct sequence_wrap_pair_t * pair, int at_client, struct sequence_wrap_direction_t * d )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    for ( ;; )
    {
        const int got = at_client ? rocketnet_message_client_receive( pair->client, 0, &data, &bytes )
                                  : rocketnet_message_server_receive( pair->server, 0, 0, &data, &bytes );
        if ( !got )
            break;

        if ( d && bytes == 8 && data[0] == 0x5E )
        {
            uint32_t index = 0;
            memcpy( &index, data + 4, sizeof( index ) );
            if ( (int) index != d->received )
            {
                d->in_order = 0;
            }

            d->received++;
        }

        if ( at_client )
            rocketnet_message_client_release( pair->client, data );
        else
            rocketnet_message_server_release( pair->server, data );
    }
}

static void sequence_wrap_send( struct sequence_wrap_pair_t * pair, int from_client, struct sequence_wrap_direction_t * d )
{
    uint8_t message[8] = { 0x5E, 0, 0, 0, 0, 0, 0, 0 };
    const uint32_t index = (uint32_t) d->sent;
    memcpy( message + 4, &index, sizeof( index ) );
    const uint32_t id = from_client ? rocketnet_message_client_send( pair->client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, sizeof( message ) ) : rocketnet_message_server_send( pair->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, sizeof( message ) );
    check( id != 0 );

    d->sent++;
}

// one step: both sides update at the step's time, both drain, then each sends
// one message when sending, and both flush
static void sequence_wrap_step( struct sequence_wrap_pair_t * pair, bool send,
                                struct sequence_wrap_direction_t * to_client, struct sequence_wrap_direction_t * to_server )
{
    pair->time += SEQUENCE_WRAP_DT;
    sequence_wrap_clock = pair->time;
    check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_client_update( pair->client, pair->time ) == ROCKETNET_UPDATE_OK );

    sequence_wrap_drain( pair, 1, to_client );
    sequence_wrap_drain( pair, 0, to_server );

    if ( send )
    {
        sequence_wrap_send( pair, 0, to_client );
        sequence_wrap_send( pair, 1, to_server );
    }

    rocketnet_message_server_send_packets( pair->server );
    rocketnet_message_client_send_packets( pair->client );
}

static void sequence_wrap_check_counters( const char * name, const struct rocketnet_counters_t * counters )
{
    char report[256];
    const int conserved = rocketnet_counters_conserved( counters, report, sizeof( report ) );
    if ( !conserved )
    {
        printf( "sequence wrap: %s counters not conserved: %s\n", name, report );
    }

    check( conserved );

    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        if ( rocketnet_counter_class( i ) != ROCKETNET_COUNTER_CLASS_REFUSAL )
            continue;
        const uint64_t value = rocketnet_counter_value( counters, i );
        if ( value != 0 )
        {
            printf( "sequence wrap: %s refusal %s = %llu\n", name, rocketnet_counter_name( i ), (unsigned long long) value );
        }

        check( value == 0 );
    }
}

static void test_sequence_wrap_session_both_directions( void )
{
    char start_text[32];
    snprintf( start_text, sizeof( start_text ), "%llu", (unsigned long long) SEQUENCE_WRAP_START );
    sequence_wrap_setenv( "ROCKETNET_DEV_SESSION_SEQUENCE_START", start_text );

    check( rocketnet_dev_session_sequence_start() == SEQUENCE_WRAP_START );

    struct sequence_wrap_pair_t pair;
    const bool created = sequence_wrap_pair_setup( &pair );
    check( created );
    if ( !created )
    {
        sequence_wrap_unsetenv( "ROCKETNET_DEV_SESSION_SEQUENCE_START" );
        return;
    }

    for ( int i = 0; i < 400 && !( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                                   rocketnet_message_server_client_connected( pair.server, 0 ) );
          i++ )
        sequence_wrap_step( &pair, false, NULL, NULL );
    if ( rocketnet_message_client_state( pair.client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        const struct rocketnet_counters_t * client_counters = rocketnet_client_transport_counters( rocketnet_message_client_transport( pair.client ) );
        printf( "sequence wrap: client state %d after the connect steps\n", rocketnet_message_client_state( pair.client ) );
        for ( int i = 0; i < rocketnet_num_counters(); i++ )
            if ( rocketnet_counter_value( client_counters, i ) != 0 )
                printf( "sequence wrap: client %s = %llu\n", rocketnet_counter_name( i ), (unsigned long long) rocketnet_counter_value( client_counters, i ) );
    }

    check( rocketnet_message_server_client_connected( pair.server, 0 ) );
    check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // the seam is read once per process: unsetting it now leaves the
    // environment as the suite found it and changes nothing for this process

    sequence_wrap_unsetenv( "ROCKETNET_DEV_SESSION_SEQUENCE_START" );

    check( rocketnet_dev_session_sequence_start() == SEQUENCE_WRAP_START );

    struct rn_message_object_t * objects[2] = { &pair.server->object, &pair.client->object };
    memset( sequence_wrap_seams, 0, sizeof( sequence_wrap_seams ) );

    for ( int side = 0; side < 2; side++ )
    {
        sequence_wrap_seams[side].finish_next = objects[side]->link.finish_packet;
        sequence_wrap_seams[side].acked_next = objects[side]->link.acked;
    }

    objects[0]->link.finish_packet = sequence_wrap_server_finish;
    objects[0]->link.acked = sequence_wrap_server_acked;
    objects[1]->link.finish_packet = sequence_wrap_client_finish;
    objects[1]->link.acked = sequence_wrap_client_acked;

    struct sequence_wrap_direction_t to_client = { 0, 0, 1 };
    struct sequence_wrap_direction_t to_server = { 0, 0, 1 };

    for ( int step = 0; step < SEQUENCE_WRAP_STEPS + SEQUENCE_WRAP_SETTLE; step++ )
    {
        sequence_wrap_step( &pair, step < SEQUENCE_WRAP_STEPS, &to_client, &to_server );
    }

    for ( int side = 0; side < 2; side++ )
    {
        objects[side]->link.finish_packet = sequence_wrap_seams[side].finish_next;
        objects[side]->link.acked = sequence_wrap_seams[side].acked_next;
    }

    // every message delivered once, in order, both ways

    check( to_client.sent == SEQUENCE_WRAP_STEPS && to_server.sent == SEQUENCE_WRAP_STEPS );
    check( to_client.received == to_client.sent && to_client.in_order == 1 );
    check( to_server.received == to_server.sent && to_server.in_order == 1 );

    const double low = SEQUENCE_WRAP_DT - 1e-9;
    const double high = 4.0 * SEQUENCE_WRAP_DT + 1e-9;
    const char * side_names[2] = { "server", "client" };

    for ( int side = 0; side < 2; side++ )
    {
        const struct sequence_wrap_seam_t * seam = &sequence_wrap_seams[side];
        char print[16];
        if ( rocketnet_test_getenv( "ROCKETNET_SEQUENCE_WRAP_PRINT", print, sizeof( print ) ) )
            printf( "SEQUENCE_WRAP %s packets=%d lowest=0x%llx highest=0x%llx past_wrap=%d acked=%d acked_past_wrap=%d "
                    "samples_past_wrap=%d sample_min=%.6f sample_max=%.6f srtt=%.6f resent=%llu duplicate=%llu\n",
                    side_names[side], seam->packets, (unsigned long long) seam->lowest_sequence,
                    (unsigned long long) seam->highest_sequence, seam->packets_past_wrap, seam->acked,
                    seam->acked_past_wrap, seam->samples_past_wrap, seam->sample_min, seam->sample_max,
                    objects[side]->connections[0].ordered.ack.srtt,
                    (unsigned long long) objects[side]->counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT],
                    (unsigned long long) objects[side]->counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE] );

        // the session started at the seam's value and the field wrapped,
        // with at least SEQUENCE_WRAP_STEPS packets sent each way

        check( !seam->packets_overflow );
        check( seam->packets >= SEQUENCE_WRAP_STEPS );
        check( seam->lowest_sequence >= SEQUENCE_WRAP_START && seam->lowest_sequence <= SEQUENCE_WRAP_FIELD_LIMIT );
        check( seam->highest_sequence >= SEQUENCE_WRAP_START + SEQUENCE_WRAP_STEPS );
        check( seam->packets_past_wrap >= SEQUENCE_WRAP_STEPS - 1000 );

        // acks and the RTT they sample carry on across the wrap

        check( seam->acked_past_wrap >= SEQUENCE_WRAP_STEPS - 1000 );
        check( seam->samples_past_wrap > 0 );
        check( seam->sample_min >= low && seam->sample_max <= high );

        const double srtt = objects[side]->connections[0].ordered.ack.srtt;
        check( srtt >= low && srtt <= high );

        // nothing delivered was resent or arrived twice

        check( objects[side]->counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT] == 0 );
        check( objects[side]->counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE] == 0 );
    }

    // replay and reconstruction refused nothing past the wrap, and every
    // counter family balances, on both transports and both message layers

    sequence_wrap_check_counters( "server transport", rocketnet_server_transport_counters( rocketnet_message_server_transport( pair.server ) ) );
    sequence_wrap_check_counters( "client transport", rocketnet_client_transport_counters( rocketnet_message_client_transport( pair.client ) ) );
    sequence_wrap_check_counters( "server message", &objects[0]->counters );
    sequence_wrap_check_counters( "client message", &objects[1]->counters );

    rn_test_message_client_stop_and_destroy( pair.client );
    rn_test_message_server_stop_and_destroy( pair.server );
}

// test_sequence_wrap_session_starts_at_wrap_edge: both sessions start at
// 2^32 - 10 through the same seam, so every session packet's reader has a
// highest sent sequence past 2^31 from its first packet, while the other end
// has opened nothing and answers with the empty ack block (largest 0, first
// range length 0, no ranges; standard/TRANSPORT.md t15). The reader
// recognises that block on its wire fields (sdk/rocketnet_ack.cpp
// rocketnet_ack_block_read, tla/AckBlockWrap.tla ReadBlock), the same reader
// on client and server: the client connects, at least
// SEQUENCE_WRAP_EDGE_PAST packets each way go past 0x100000000 and are acked,
// and no bad ack block is counted on either transport. With the empty test
// made on the reconstructed largest (the reader before 2026-10-24) every
// keep-alive counts CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK and the client never
// connects.

#define SEQUENCE_WRAP_EDGE_START ( 0x100000000ULL - 10ULL )
#define SEQUENCE_WRAP_EDGE_STEPS 400
#define SEQUENCE_WRAP_EDGE_PAST 100

static void test_sequence_wrap_session_starts_at_wrap_edge( void )
{
    char start_text[32];
    snprintf( start_text, sizeof( start_text ), "%llu", (unsigned long long) SEQUENCE_WRAP_EDGE_START );
    sequence_wrap_setenv( "ROCKETNET_DEV_SESSION_SEQUENCE_START", start_text );

    check( rocketnet_dev_session_sequence_start() == SEQUENCE_WRAP_EDGE_START );

    struct sequence_wrap_pair_t pair;
    const bool created = sequence_wrap_pair_setup( &pair );
    check( created );
    if ( !created )
    {
        sequence_wrap_unsetenv( "ROCKETNET_DEV_SESSION_SEQUENCE_START" );
        return;
    }

    struct rn_message_object_t * objects[2] = { &pair.server->object, &pair.client->object };
    memset( sequence_wrap_seams, 0, sizeof( sequence_wrap_seams ) );

    for ( int side = 0; side < 2; side++ )
    {
        sequence_wrap_seams[side].finish_next = objects[side]->link.finish_packet;
        sequence_wrap_seams[side].acked_next = objects[side]->link.acked;
    }

    objects[0]->link.finish_packet = sequence_wrap_server_finish;
    objects[0]->link.acked = sequence_wrap_server_acked;
    objects[1]->link.finish_packet = sequence_wrap_client_finish;
    objects[1]->link.acked = sequence_wrap_client_acked;

    for ( int i = 0; i < 400 && !( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                                   rocketnet_message_server_client_connected( pair.server, 0 ) );
          i++ )
        sequence_wrap_step( &pair, false, NULL, NULL );

    const struct rocketnet_counters_t * client_counters = rocketnet_client_transport_counters( rocketnet_message_client_transport( pair.client ) );
    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( rocketnet_message_server_transport( pair.server ) );
    if ( rocketnet_message_client_state( pair.client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        printf( "sequence wrap edge: client state %d after the connect steps, CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK = %llu\n",
                rocketnet_message_client_state( pair.client ),
                (unsigned long long) rocketnet_counter_value( client_counters, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK ) );
    check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_message_server_client_connected( pair.server, 0 ) );

    sequence_wrap_unsetenv( "ROCKETNET_DEV_SESSION_SEQUENCE_START" );

    check( rocketnet_dev_session_sequence_start() == SEQUENCE_WRAP_EDGE_START );

    struct sequence_wrap_direction_t to_client = { 0, 0, 1 };
    struct sequence_wrap_direction_t to_server = { 0, 0, 1 };

    for ( int step = 0; step < SEQUENCE_WRAP_EDGE_STEPS + SEQUENCE_WRAP_SETTLE; step++ )
    {
        sequence_wrap_step( &pair, step < SEQUENCE_WRAP_EDGE_STEPS, &to_client, &to_server );
    }

    for ( int side = 0; side < 2; side++ )
    {
        objects[side]->link.finish_packet = sequence_wrap_seams[side].finish_next;
        objects[side]->link.acked = sequence_wrap_seams[side].acked_next;
    }

    check( to_client.sent == SEQUENCE_WRAP_EDGE_STEPS && to_server.sent == SEQUENCE_WRAP_EDGE_STEPS );
    check( to_client.received == to_client.sent && to_client.in_order == 1 );
    check( to_server.received == to_server.sent && to_server.in_order == 1 );

    for ( int side = 0; side < 2; side++ )
    {
        const struct sequence_wrap_seam_t * seam = &sequence_wrap_seams[side];
        check( !seam->packets_overflow );
        check( seam->lowest_sequence >= SEQUENCE_WRAP_EDGE_START );
        check( seam->packets_past_wrap >= SEQUENCE_WRAP_EDGE_PAST );
        check( seam->acked_past_wrap >= SEQUENCE_WRAP_EDGE_PAST );
    }

    check( rocketnet_counter_value( client_counters, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK ) == 0 );
    check( rocketnet_counter_value( client_counters, ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK ) == 0 );
    check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK ) == 0 );
    check( rocketnet_counter_value( server_counters, ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK ) == 0 );

    sequence_wrap_check_counters( "server transport", server_counters );
    sequence_wrap_check_counters( "client transport", client_counters );
    sequence_wrap_check_counters( "server message", &objects[0]->counters );
    sequence_wrap_check_counters( "client message", &objects[1]->counters );

    rn_test_message_client_stop_and_destroy( pair.client );
    rn_test_message_server_stop_and_destroy( pair.server );
}

// rocketnet_reconstruct_near (sdk/rocketnet_protocol.cpp) on its own, no
// session: of the three candidates sharing the wire value in their low 32
// bits (expected's 2^32 block, the one below, the one above), the nearest to
// expected wins and an exact tie takes the larger. Pins the behaviour the
// session cases above rely on, at each edge: the same block, the block below,
// the block above, a tie, and expected zero (no block below).

#endif // #if ROCKETNET_DEV

static void test_reconstruct_near_picks_nearest_and_larger_on_tie( void )
{
    check( rocketnet_reconstruct_near( 3, 0x100000005ULL ) == 0x100000003ULL );
    check( rocketnet_reconstruct_near( 0xFFFFFFF0U, 0x100000010ULL ) == 0x0FFFFFFF0ULL );
    check( rocketnet_reconstruct_near( 5, 0x1FFFFFFF0ULL ) == 0x200000005ULL );
    check( rocketnet_reconstruct_near( 0, 0x180000000ULL ) == 0x200000000ULL );
    check( rocketnet_reconstruct_near( 0xFFFFFFFFU, 0 ) == 0xFFFFFFFFULL );
}

#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_sequence_wrap_session_both_directions );
REGISTER_UNIT_TEST( test_sequence_wrap_session_starts_at_wrap_edge );
#endif // #if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_reconstruct_near_picks_nearest_and_larger_on_tie );
