/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client receive handoff's latency rule, on both client shapes: a
// datagram the reading thread has banked before the main thread's receive
// call begins is returned by that call, not by the next one.
//
// The I/O core (sdk/rocketnet_client_io.h) runs the gso, portable and
// loopback clients on Linux, msgx on macOS and iocp on Windows; it is driven
// here through its own operations (rocketnet_client_io_step for the I/O
// thread, rocketnet_client_io_expose for update) against the fake client
// transport, so every platform's client is
// guarded on every platform. The BASE (legacy) receive path is driven through
// the step-driven client transport (rocketnet_client_transport_step_legacy_for_test
// for its receive worker, rocketnet_client_transport_update for update).
//
// Unit tests: no thread, no socket, no real time; the clock is an argument
// that starts at 1000.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "client_io_rig.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_config.h"

#include <string.h>

#define check rocketnet_unit_check

static double client_receive_latency_time( void * context )
{
    return *(const double *) context;
}

static void client_receive_latency_open( rocketnet_fake_client_transport_io_t * fake, rocketnet_client_transport_io_t * transport )
{
    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    check( rocketnet_client_transport_io_open( &rocketnet_fake_client_operations_io, fake, &config, &bind, transport ) );
}

// I/O core. Each frame the I/O thread banks datagrams in separate steps (one
// per wake, as datagrams arrive spread over the frame), then update's expose
// begins: it returns every one of them, in arrival order. Three frames, so the steady state is
// covered, not only the first frame after a quiet start. On the rule before
// this fix the second datagram of a frame waited in the I/O thread's own
// buffer for the next update.

static void test_client_io_receive_returns_everything_banked_before_the_call()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_t transport = {};
    client_receive_latency_open( &fake, &transport );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    double now = 1000.0;
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_receive_latency_time, &now );

    rocketnet_address_t from = {};
    rocketnet_address_load_ipv4( &from, 0x7f000001, 9250 );

    uint8_t datagram[8] = {};
    uint8_t next = 0x10;

    for ( int frame = 0; frame < 3; frame++ )
    {
        const int per_frame = 2 + frame;
        const uint8_t first = next;

        for ( int i = 0; i < per_frame; i++ )
        {
            datagram[0] = next++;
            rocketnet_fake_client_push( &fake, &from, datagram, sizeof( datagram ) );
            check( rocketnet_client_io_step( &io, false ) == ROCKETNET_CLIENT_IO_WORKED );
        }

        const rocketnet_client_io_received_t * received = rocketnet_client_io_rig_expose( rig, &io );
        check( received != NULL );
        check( received != NULL && received->num_packets == per_frame );
        for ( int i = 0; received != NULL && i < received->num_packets && i < per_frame; i++ )
        {
            check( received->packet_data[i][0] == (uint8_t) ( first + i ) );
        }

        now += 1.0 / 60.0;
    }

    // nothing is left for a later call

    check( rocketnet_client_io_rig_expose( rig, &io ) == NULL );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_io_receive_returns_everything_banked_before_the_call );

// BASE path (development builds: the step-driven client transport is
// development scaffolding). A connect token naming 127.0.0.1:40000.

#if ROCKETNET_DEV

static void client_receive_latency_token( uint8_t * token )
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

static uint64_t client_receive_latency_received( rocketnet_client_transport_t * transport )
{
    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( transport, &stats );
    return stats.packets_received;
}

// The BASE receive worker commits its count on every drain. Each frame it drains several times (one datagram each), then
// update begins: the update takes every datagram banked before it. On the
// rule before this fix the worker published a batch only while a buffer was
// free, so the third drain of a frame found none and its datagram waited in
// the worker's buffer for the next update.

static void test_client_legacy_receive_returns_everything_banked_before_the_call()
{
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_receive_latency_token( token );

    double now = 1000.0;
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_receive_latency_time;

    rocketnet_client_transport_t * transport =
        rocketnet_client_transport_create_step_driven( &now, token, &config, now, &rocketnet_fake_client_operations_base, fake );
    check( transport != NULL );

    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );

    const uint8_t datagram[64] = {};
    uint64_t banked = client_receive_latency_received( transport );

    for ( int frame = 0; frame < 3; frame++ )
    {
        const int per_frame = 3 + frame;

        for ( int i = 0; i < per_frame; i++ )
        {
            rocketnet_fake_client_push( fake, &server, datagram, sizeof( datagram ) );
            rocketnet_client_transport_step_legacy_for_test( transport, now );
        }
        banked += (uint64_t) per_frame;

        rocketnet_client_transport_update( transport, now );
        check( client_receive_latency_received( transport ) == banked );

        now += 1.0 / 60.0;
    }

    rn_test_client_transport_stop_and_destroy( transport );
    delete fake;
}
REGISTER_UNIT_TEST( test_client_legacy_receive_returns_everything_banked_before_the_call );
#endif // #if ROCKETNET_DEV
