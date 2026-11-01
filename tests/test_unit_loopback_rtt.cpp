/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message layer's RTT and reliable resends on a zero-latency link.
//
// A message server and client on the in-memory loopback transport, no socket,
// time from 1000.0, both sides updated every step of LOOPBACK_RTT_DT. One side
// sends one small reliable message per step; the other side sends nothing of
// its own. The sender's link seam records each packet's send time
// (finish_packet) and the time its ack reaches the layer (acked), so every
// RTT sample the window takes (rn_window_ack_on_ack) is seen.
//
// Each step updates client and server at times separated by LOOPBACK_RTT_DT. The
// sender writes after both updates, the receiver consumes it on the next
// step, and the receiver emits the ack on its next update and it reaches the sender on the
// following step. Payload ACK eligibility is committed after message processing,
// so the opposite update order may need one additional step. The sample bound
// is [dt, 4*dt] = [10, 40] ms; this follows
// from the test's update cadence,
// not the transport's keep-alive rate (rocketnet_client_protocol_update and
// rocketnet_server_write_keep_alive).
//
// The link loses nothing. A delivered message must not be resent. The resend
// delay is the adaptive resend delay, and the receiver's ack arrives by 4*dt, so both update
// orders assert zero resends.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "portable_crt.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"
#include "rocketnet_channel.h"
#include "rocketnet_window_ack.h"
#include "rocketnet_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

#define LOOPBACK_RTT_DT 0.01
#define LOOPBACK_RTT_ROUNDS 40
#define LOOPBACK_RTT_SETTLE 60
#define LOOPBACK_RTT_MAX_PACKETS 1024

enum loopback_rtt_side_t
{
    LOOPBACK_RTT_SERVER = 0,
    LOOPBACK_RTT_CLIENT = 1
};

#if ROCKETNET_DEV
static double loopback_rtt_clock = 1000.0;
static double loopback_rtt_time_callback( void * ) { return loopback_rtt_clock; }

// the sender's seam record: send time per packet id, and the samples its acks give
static struct
{
    uint64_t packet_id[LOOPBACK_RTT_MAX_PACKETS];
    double send_time[LOOPBACK_RTT_MAX_PACKETS];
    int packets;
    int samples;
    double sample_min;
    double sample_max;
    int ( *finish_next )( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes );
    int ( *acked_next )( void * transport, const int ** client_index, const uint64_t ** packet_ids );
} loopback_rtt_seam;

static int loopback_rtt_finish( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    const uint64_t packet_id = context->packet_id;
    const int wire_bytes = loopback_rtt_seam.finish_next( transport, context, payload_bytes );
    if ( wire_bytes > 0 && loopback_rtt_seam.packets < LOOPBACK_RTT_MAX_PACKETS )
    {
        loopback_rtt_seam.packet_id[loopback_rtt_seam.packets] = packet_id;
        loopback_rtt_seam.send_time[loopback_rtt_seam.packets] = loopback_rtt_clock;
        loopback_rtt_seam.packets++;
    }

    return wire_bytes;
}

static int loopback_rtt_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    const int count = loopback_rtt_seam.acked_next( transport, client_index, packet_ids );
    for ( int k = 0; k < count; k++ )
    {
        for ( int i = 0; i < loopback_rtt_seam.packets; i++ )
        {
            if ( loopback_rtt_seam.packet_id[i] != ( *packet_ids )[k] )
                continue;

            const double sample = loopback_rtt_clock - loopback_rtt_seam.send_time[i];
            if ( loopback_rtt_seam.samples == 0 || sample < loopback_rtt_seam.sample_min )
            {
                loopback_rtt_seam.sample_min = sample;
            }
            if ( loopback_rtt_seam.samples == 0 || sample > loopback_rtt_seam.sample_max )
            {
                loopback_rtt_seam.sample_max = sample;
            }
            loopback_rtt_seam.samples++;
            break;
        }
    }

    return count;
}

struct loopback_rtt_pair_t
{
    struct rocketnet_message_server_t * server;
    struct rocketnet_message_client_t * client;
    double time;
};

static bool loopback_rtt_pair_setup( struct loopback_rtt_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    loopback_rtt_clock = pair->time;
    rocketnet_quiet( 1 );

    const uint64_t protocol_id = 0x6c6f6f7072747431ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

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
    rocketnet_unit_check( pair->server != NULL );
    if ( !pair->server )
        return false;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d",
              rocketnet_server_transport_port( rocketnet_message_server_transport( pair->server ) ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                                    0x2545f4914f6cdd1dULL, 0, 1ULL << 40, 60, NULL,
                                                                    token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = loopback_rtt_time_callback;

    pair->client = rocketnet_message_client_create( NULL, token, &client_config, NULL, NULL, 0, 0 );
    rocketnet_unit_check( pair->client != NULL );

    return pair->client != NULL;
}

struct loopback_rtt_measures_t
{
    int sent;
    int received;
    int in_order;
    uint64_t resent;                // MESSAGE_RESENT on the sender
    uint64_t duplicate;             // MESSAGE_DUPLICATE on the receiver
    double srtt;
    double resend_delay;
};

// one step: both sides update at the step's time, the receiver drains, the
// sender queues one message while sending, both flush

static void loopback_rtt_step( struct loopback_rtt_pair_t * pair, int side, bool send, struct loopback_rtt_measures_t * m )
{
    pair->time += LOOPBACK_RTT_DT;
    loopback_rtt_clock = pair->time;
    rocketnet_unit_check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_update( pair->client, pair->time ) == ROCKETNET_UPDATE_OK );

    if ( m )
    {
        const uint8_t * data = NULL;
        int bytes = 0;

        for ( ;; )
        {
            const int got = side == LOOPBACK_RTT_SERVER ? rocketnet_message_client_receive( pair->client, 0, &data, &bytes )
                                                        : rocketnet_message_server_receive( pair->server, 0, 0, &data, &bytes );
            if ( !got )
                break;

            if ( bytes == 8 && data[0] == 0xE7 )
            {
                uint32_t index = 0;
                memcpy( &index, data + 4, sizeof( index ) );
                if ( (int) index != m->received )
                {
                    m->in_order = 0;
                }
                m->received++;
            }

            if ( side == LOOPBACK_RTT_SERVER )
                rocketnet_message_client_release( pair->client, data );
            else
                rocketnet_message_server_release( pair->server, data );
        }
    }

    if ( m && send )
    {
        uint8_t message[8] = { 0xE7, 0, 0, 0, 0, 0, 0, 0 };
        const uint32_t index = (uint32_t) m->sent;
        memcpy( message + 4, &index, sizeof( index ) );

        const uint32_t id = side == LOOPBACK_RTT_SERVER ? rocketnet_message_server_send( pair->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, sizeof( message ) ) : rocketnet_message_client_send( pair->client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, message, sizeof( message ) );
        rocketnet_unit_check( id != 0 );
        m->sent++;
    }

    rocketnet_message_server_send_packets( pair->server );
    rocketnet_message_client_send_packets( pair->client );
}

static void loopback_rtt_run( int side, struct loopback_rtt_measures_t * m )
{
    memset( m, 0, sizeof( *m ) );
    m->in_order = 1;

    struct loopback_rtt_pair_t pair;
    rocketnet_unit_check( loopback_rtt_pair_setup( &pair ) );

    for ( int i = 0; i < 400 && !( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                                   rocketnet_message_server_client_connected( pair.server, 0 ) );
          i++ )
    {
        loopback_rtt_step( &pair, side, false, NULL );
    }
    rocketnet_unit_check( rocketnet_message_server_client_connected( pair.server, 0 ) );
    rocketnet_unit_check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    struct rn_message_object_t * sender = side == LOOPBACK_RTT_SERVER ? &pair.server->object : &pair.client->object;
    struct rn_message_object_t * receiver = side == LOOPBACK_RTT_SERVER ? &pair.client->object : &pair.server->object;
    memset( &loopback_rtt_seam, 0, sizeof( loopback_rtt_seam ) );
    loopback_rtt_seam.finish_next = sender->link.finish_packet;
    loopback_rtt_seam.acked_next = sender->link.acked;
    sender->link.finish_packet = loopback_rtt_finish;
    sender->link.acked = loopback_rtt_acked;

    for ( int round = 0; round < LOOPBACK_RTT_ROUNDS + LOOPBACK_RTT_SETTLE; round++ )
    {
        loopback_rtt_step( &pair, side, round < LOOPBACK_RTT_ROUNDS, m );
    }

    sender->link.finish_packet = loopback_rtt_seam.finish_next;
    sender->link.acked = loopback_rtt_seam.acked_next;

    m->resent = sender->counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];
    m->duplicate = receiver->counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE];
    m->srtt = sender->connections[0].ordered.ack.srtt;
    m->resend_delay = sender->connections[0].ordered.ack.resend_delay;

    char rtt_print[256];
    if ( rocketnet_test_getenv( "ROCKETNET_LOOPBACK_RTT_PRINT", rtt_print, sizeof( rtt_print ) ) )
        printf( "LOOPBACK_RTT %s sent=%d received=%d in_order=%d packets=%d samples=%d sample_min=%.6f sample_max=%.6f "
                "srtt=%.6f resend_delay=%.6f resent=%llu duplicate=%llu\n",
                side == LOOPBACK_RTT_SERVER ? "server" : "client", m->sent, m->received, m->in_order,
                loopback_rtt_seam.packets, loopback_rtt_seam.samples, loopback_rtt_seam.sample_min, loopback_rtt_seam.sample_max,
                m->srtt, m->resend_delay, (unsigned long long) m->resent, (unsigned long long) m->duplicate );

    rn_test_message_client_stop_and_destroy( pair.client );
    rn_test_message_server_stop_and_destroy( pair.server );
}

static void loopback_rtt_check( int side )
{
    struct loopback_rtt_measures_t m;
    loopback_rtt_run( side, &m );

    // every message delivered once, in order, on a link that loses nothing

    rocketnet_unit_check( m.sent == LOOPBACK_RTT_ROUNDS );
    rocketnet_unit_check( m.received == m.sent );
    rocketnet_unit_check( m.in_order == 1 );

    // every sample and its smoothed value follow the two-step loopback cadence

    const double low = LOOPBACK_RTT_DT - 1e-9;
    const double high = 4.0 * LOOPBACK_RTT_DT + 1e-9;
    rocketnet_unit_check( loopback_rtt_seam.samples > 0 );
    rocketnet_unit_check( loopback_rtt_seam.sample_min >= low );
    rocketnet_unit_check( loopback_rtt_seam.sample_max <= high );
    rocketnet_unit_check( m.srtt >= low && m.srtt <= high );

    // nothing delivered is resent

    rocketnet_unit_check( m.resent == 0 );
    rocketnet_unit_check( m.duplicate == 0 );
}

static void test_loopback_transport_rtt_server_sends( void ) { loopback_rtt_check( LOOPBACK_RTT_SERVER ); }
static void test_loopback_transport_rtt_client_sends( void ) { loopback_rtt_check( LOOPBACK_RTT_CLIENT ); }

REGISTER_UNIT_TEST( test_loopback_transport_rtt_server_sends );
REGISTER_UNIT_TEST( test_loopback_transport_rtt_client_sends );
#endif // #if ROCKETNET_DEV

// Retain a full maximum resend delay payload budget without acknowledging any record.
// Exercise each documented typical fixed-pool size and the large-span path.

static void test_max_resend_delay_payload_reserve_without_acks()
{
    const int budgets[] = { 1000, 10000 };
    const int payloads[] = { 128, 512, 1024, 1153, 16384 };

    for ( int side = 0; side < 2; ++side )
    {
        rocketnet_message_config_t config;
        rocketnet_message_default_config( &config );
        config.client_send_kbps = config.server_send_kbps = budgets[side];

        rocketnet_counters_t counters;
        memset( &counters, 0, sizeof( counters ) );

        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
        rocketnet_unit_check( arena != NULL );

        rn_region_t * region = rn_arena_region( arena, 0 );
        rocketnet_unit_check( config.message_resend_delay_max_ms > 0 );

        const uint64_t budget = (uint64_t) budgets[side] * 125 * (uint64_t) config.message_resend_delay_max_ms / 1000;

        for ( int p = 0; p < 5; ++p )
        {
            rn_region_carve( region );

            uint64_t held = 0;
            while ( held < budget )
            {
                const int bytes = budget - held < (uint64_t) payloads[p] ? (int) ( budget - held ) : payloads[p];
                const uint32_t descriptor = bytes >= RN_SPAN_MIN_BYTES ? rn_region_send_alloc_span( region, bytes ) : rn_region_send_alloc( region, bytes );
                rocketnet_unit_check( descriptor != RN_DESCRIPTOR_NONE );
                held += (uint64_t) bytes;
            }

            rocketnet_unit_check( !rn_region_exhausted( region ) );
        }

        printf( "maximum resend delay reserve: %d kbps, %llu payload bytes, %llu bytes/connection, %llu bytes/1000\n",
                budgets[side], (unsigned long long) budget,
                (unsigned long long) rocketnet_message_connection_bytes( &config ),
                (unsigned long long) rocketnet_message_connection_bytes( &config ) * 1000 );
        rn_arena_destroy( arena );
    }
}
REGISTER_UNIT_TEST( test_max_resend_delay_payload_reserve_without_acks );
