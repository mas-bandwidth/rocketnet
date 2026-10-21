/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client trusts only the server (TRANSPORT.md t11.1, t11 rule 8). A
// connection cookie's salt rides in the clear, so anyone who sees the
// request can echo it: an accepted cookie is stored, and in requesting
// cookie (1) it advances the client, but it never refreshes the receive
// time and never gives a round-trip sample. Both come only from the
// challenge and later packets that open under the attempt's keys. The
// handshake deadline is unchanged.
//
// The cookie here is built from the attempt's own salt with a MAC the
// client cannot check, exactly as an observer of the request could build
// it.
//
// The client transport connects a transport's socket to the server the
// token selected (rocketnet_client_transport_io_operations_t.connect), once,
// before anything is sent or received, so the kernel drops datagrams from
// any other source; a refused connect refuses the transport. The fake
// client driver (test/fake_client_driver.h) stands in for the socket here;
// functional/platform_primitives.cpp checks the real socket over loopback.
// Time is an argument; the clock starts at 1000.0 and nothing here sleeps,
// opens a socket or reads a clock.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_client_io.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static const int32_t client_trusts_timeout_seconds = 5;

static void client_trusts_mint( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = client_trusts_timeout_seconds;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// connects at 1000.0 and sends the first request at 1000.0
static void client_trusts_connect( struct rocketnet_client_protocol_t * client, struct rocketnet_counters_t * counters )
{
    memset( counters, 0, sizeof(*counters) );
    rocketnet_client_protocol_init( client, counters );
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_trusts_mint( token_data );
    check( rocketnet_client_protocol_connect( client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
    check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( client, 1000.0, packet, &packet_bytes );
    check( packet_bytes > 0 );
    check( client->state_first_send_time == 1000.0 );
}

// a cookie echoing the attempt's salt: what anyone who saw the request can send
static void client_trusts_deliver_cookie( struct rocketnet_client_protocol_t * client, double time )
{
    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( cookie, client->packet_filter, client->connection_salt,
                                              ROCKETNET_TEST_CREATE_TS + 3, rocketnet_test_cookie_mac );
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_packet( client, cookie, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES, time,
                                                     payload, sizeof(payload), &payload_bytes ) );
}

// a cookie in requesting cookie (1) and again in sending connection request
// (2) is stored and advances the client, but leaves the receive time at the
// attempt's start, so the token timeout still runs from 1000.0
static void test_client_cookie_does_not_refresh_idle_timer()
{
    static struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    client_trusts_connect( &client, &counters );
    check( client.last_receive_time == 1000.0 );

    client_trusts_deliver_cookie( &client, 1003.0 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    check( client.has_cookie );
    check( counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_STORED] == 1 );
    check( client.last_receive_time == 1000.0 );

    client_trusts_deliver_cookie( &client, 1004.5 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    check( counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_STORED] == 2 );
    check( client.last_receive_time == 1000.0 );

    // just inside the timeout the attempt still stands; at the timeout,
    // counted from the attempt's start and not from either cookie, it ends
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, 1004.9, packet, &packet_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    rocketnet_client_protocol_update( &client, 1000.0 + client_trusts_timeout_seconds, packet, &packet_bytes );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT );
}

// a cookie gives no round-trip sample, and leaving requesting cookie disarms
// the stamp, so the next sample measures from the first request of sending
// connection request, never from the attempt's first request
static void test_client_cookie_takes_no_rtt_sample()
{
    static struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
    client_trusts_connect( &client, &counters );
    check( client.rtt_estimate == 0.0 );

    client_trusts_deliver_cookie( &client, 1000.05 );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    check( client.rtt_estimate == 0.0 );
    check( client.state_first_send_time == -1.0 );

    // the first request of sending connection request arms the stamp afresh
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, 1000.2, packet, &packet_bytes );
    check( packet_bytes > 0 );
    check( client.state_first_send_time == 1000.2 );

    // a second cookie in sending connection request neither samples nor
    // disarms: the challenge still owns that sample
    client_trusts_deliver_cookie( &client, 1000.25 );
    check( client.rtt_estimate == 0.0 );
    check( client.state_first_send_time == 1000.2 );
}

struct client_trusts_clock_t
{
    double now;
};

static double client_trusts_time( void * context )
{
    return ( (client_trusts_clock_t *) context )->now;
}

// a connect token naming 127.0.0.1:40000
static void client_trusts_transport_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

// the transport connects its socket to the dialed server once, before the
// first send or receive, and every send then goes to that server
static void test_client_transport_connects_socket_to_server()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.connect = rocketnet_fake_client_connect;

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    client_trusts_clock_t clock = { 1000.0 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_trusts_transport_token( token );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_trusts_time;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven( &clock, token, &config, clock.now, &operations, fake );
    check( transport != NULL );

    rocketnet_address_t server;
    check( rocketnet_address_parse( &server, "127.0.0.1:40000" ) );
    check( fake->connect_calls == 1 );
    check( rocketnet_address_equal( &fake->connected_to, &server ) );
    check( fake->calls_before_connect == 0 );
    check( fake->destroy_calls == 0 );

    rocketnet_client_transport_update( transport, clock.now );
    rocketnet_client_io_step( rocketnet_client_transport_client_io( transport ), false );
    check( fake->num_sent > 0 );
    for ( int i = 0; i < fake->num_sent; i++ )
        check( rocketnet_address_equal( &fake->sent[i].address, &server ) );
    check( fake->connect_calls == 1 );

    rocketnet_client_transport_destroy( transport );
    check( fake->destroy_calls == 1 );
    delete fake;
}

// a connect the kernel refuses refuses the transport: it is closed, nothing
// is sent, and with no other rung the create fails as the ladder exhausted
static void test_client_transport_refuses_when_connect_fails()
{
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.connect = rocketnet_fake_client_connect;

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();
    fake->refuse_connect = true;
    client_trusts_clock_t clock = { 1000.0 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    client_trusts_transport_token( token );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = client_trusts_time;
    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_ex( &clock, token, &config, clock.now, &operations, fake, &status );
    check( transport == NULL );
    check( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
    check( fake->connect_calls >= 1 );
    check( fake->destroy_calls == fake->connect_calls );
    check( fake->num_sent == 0 );
    delete fake;
}

REGISTER_UNIT_TEST( test_client_cookie_does_not_refresh_idle_timer );
REGISTER_UNIT_TEST( test_client_transport_connects_socket_to_server );
REGISTER_UNIT_TEST( test_client_transport_refuses_when_connect_fails );
REGISTER_UNIT_TEST( test_client_cookie_takes_no_rtt_sample );
