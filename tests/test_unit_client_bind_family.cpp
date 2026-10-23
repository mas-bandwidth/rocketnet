/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client bind-family choice (rocketnet_platform_client_bind_family in
// sdk/rocketnet_platform.cpp): a host with no IPv6 stack binds IPv4 for an
// IPv4 server, refuses an IPv6 server, and every other combination follows
// the server family and the platform's dual-stack capability. No sockets and
// no clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"
#include "rocketnet_client_io.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_test_vectors.h"
#include "fake_client_driver.h"
#include "unit_shutdown.h"

#include <stdint.h>
#include <string.h>

// sdk/rocketnet_client_protocol.cpp: the server address a token selects on a
// host with or without IPv6
void rocketnet_client_protocol_pick_server_address( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                    bool ipv6_available,
                                                    struct rocketnet_address_t * address );

static void expect_bind_family( int server_address_type, bool ipv6_available, bool dual_stack, int expected )
{
    rocketnet_unit_check( rocketnet_platform_client_bind_family( server_address_type, ipv6_available, dual_stack ) == expected );
}

// An IPv4 server on a host with no IPv6 stack binds IPv4 whether or not the
// platform's dual-stack capability is reported.
static void test_client_bind_family_ipv4_server_without_ipv6()
{
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, false, true, ROCKETNET_ADDRESS_IPV4 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, false, false, ROCKETNET_ADDRESS_IPV4 );
}

// With IPv6 available, an IPv4 server uses dual stack where the platform has
// it and IPv4 alone where it does not.
static void test_client_bind_family_ipv4_server_with_ipv6()
{
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, true, true, ROCKETNET_ADDRESS_IPV6 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV4, true, false, ROCKETNET_ADDRESS_IPV4 );
}

// An IPv6 server binds IPv6 whenever an IPv6 socket can be made, and refuses
// the create when the host has no IPv6 stack.
static void test_client_bind_family_ipv6_server()
{
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, true, true, ROCKETNET_ADDRESS_IPV6 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, true, false, ROCKETNET_ADDRESS_IPV6 );
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, false, true, ROCKETNET_ADDRESS_NONE );
    expect_bind_family( ROCKETNET_ADDRESS_IPV6, false, false, ROCKETNET_ADDRESS_NONE );
}

// A token naming 127.0.0.1:40000 and [::1]:40000. On a host with no IPv6
// the client selects the IPv4 address, whose bind family is IPv4, and races
// nothing; on a host with IPv6 it selects the IPv6 address as the primary of
// the family race, and the transport dials the IPv4 address too until the
// first cookie commits the attempt (TRANSPORT.md t11 rule 3,
// test_client_connect_keeps_the_family_that_answers). A token naming only
// one family selects that family whatever the host and races nothing, and
// an IPv6-only token on a host with no IPv6 has no bind family (the create
// refuses it).
static void test_client_pick_server_address_by_host_capability()
{
    struct rocketnet_signed_connect_data_t token;
    memset( &token, 0, sizeof(token) );
    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    const uint8_t loopback_ipv6[18] = { 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0x01,0x00, 0x40,0x9c };
    memcpy( token.ipv4_address, loopback_ipv4, sizeof(loopback_ipv4) );
    memcpy( token.ipv6_address, loopback_ipv6, sizeof(loopback_ipv6) );

    struct rocketnet_address_t ipv4_expected;
    struct rocketnet_address_t ipv6_expected;
    rocketnet_unit_check( rocketnet_address_parse( &ipv4_expected, "127.0.0.1:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &ipv6_expected, "[::1]:40000" ) );

    struct rocketnet_address_t picked;

    // both families named, a host with no IPv6: the IPv4 address
    token.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;
    rocketnet_client_protocol_pick_server_address( &token, false, &picked );
    rocketnet_unit_check( picked.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv4_expected ) );
    rocketnet_unit_check( rocketnet_platform_client_bind_family( picked.type, false, false ) == ROCKETNET_ADDRESS_IPV4 );

    // both families named, a host with IPv6: the IPv6 address is the
    // race's primary, and the race's other server is the token's IPv4
    // address, the address a host without IPv6 selects
    rocketnet_client_protocol_pick_server_address( &token, true, &picked );
    rocketnet_unit_check( picked.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv6_expected ) );
    struct rocketnet_signed_connect_data_t ipv4_only = token;
    ipv4_only.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    rocketnet_client_protocol_pick_server_address( &ipv4_only, false, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv4_expected ) );

    // one family named: that family on either host
    token.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    rocketnet_client_protocol_pick_server_address( &token, true, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv4_expected ) );
    rocketnet_client_protocol_pick_server_address( &token, false, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv4_expected ) );

    token.address_flags = ROCKETNET_ADDRESS_FLAG_IPV6;
    rocketnet_client_protocol_pick_server_address( &token, true, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv6_expected ) );
    rocketnet_client_protocol_pick_server_address( &token, false, &picked );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv6_expected ) );
    rocketnet_unit_check( rocketnet_platform_client_bind_family( picked.type, false, false ) == ROCKETNET_ADDRESS_NONE );
}

REGISTER_UNIT_TEST( test_client_bind_family_ipv4_server_without_ipv6 );
REGISTER_UNIT_TEST( test_client_bind_family_ipv4_server_with_ipv6 );
REGISTER_UNIT_TEST( test_client_bind_family_ipv6_server );
REGISTER_UNIT_TEST( test_client_pick_server_address_by_host_capability );

// The family race (TRANSPORT.md t11 rule 3, tla/ClientConnection.tla
// OneRequestFamily and CommitsOnAnswer, tla/ServerConnection.tla InFlight).
// A token naming 203.0.113.10:40000 and [2001:db8::1]:40000 on a host with
// both families; the IPv6 route is broken, so nothing sent to the IPv6
// server is ever answered, and a real server protocol answers on IPv4. The
// client transport runs step-driven on the fake client driver (no socket,
// no thread), its clock and the server's from 1000.0. It opens and connects
// a second socket for the IPv4 server at create, sends its cookie request to
// both, keeps IPv4 when the cookie arrives there, sends its connection
// request to IPv4 only, never sends a request carrying a cookie to IPv6,
// closes the IPv6 socket at the commit, and reaches connected well inside
// the token's timeout. A late cookie from the IPv6 server is dropped and
// counted as a foreign source.

struct bind_family_clock_t
{
    double now;
};

static double bind_family_time( void * context )
{
    return ( (bind_family_clock_t *) context )->now;
}

static const int32_t bind_family_timeout_seconds = 5;

static void bind_family_mint_both( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = bind_family_timeout_seconds;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;
    const uint8_t ipv4_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, ipv4_address, 6 );
    const uint8_t ipv6_address[18] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x40, 0x9c };
    memcpy( signed_fields.ipv6_address, ipv6_address, 18 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void bind_family_server_setup( struct rocketnet_server_protocol_t * server, struct rocketnet_counters_t * counters )
{
    memset( server, 0, sizeof(*server) );
    memset( counters, 0, sizeof(*counters) );
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 4 * 4 );
    server->counters = counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history &&
                          server->client_slots && server->acks && server->reassembly );
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );
}

static void bind_family_server_teardown( struct rocketnet_server_protocol_t * server )
{
    rocketnet_encryption_map_destroy( server->encryption_map );
    rocketnet_used_token_history_destroy( server->used_token_history );
    rocketnet_client_slots_destroy( server->client_slots );
    rocketnet_server_acks_destroy( server->acks );
    rocketnet_reassembly_set_destroy( server->reassembly );
}

// a connection request whose return cookie field is not all zero
static bool bind_family_request_carries_cookie( const rocketnet_fake_client_datagram_t * datagram )
{
    if ( datagram->bytes != ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES || datagram->data[0] != ROCKETNET_CONNECTION_REQUEST_PACKET )
        return false;
    for ( int i = 0; i < ROCKETNET_COOKIE_FIELD_BYTES; i++ )
        if ( datagram->data[ROCKETNET_PREFILTER_BYTES + i] != 0 )
            return true;
    return false;
}

static void test_client_connect_keeps_the_family_that_answers()
{
    rocketnet_address_t ipv4_server, ipv6_server, client_address;
    rocketnet_unit_check( rocketnet_address_parse( &ipv4_server, "203.0.113.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &ipv6_server, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );

    static struct rocketnet_server_protocol_t server;
    static struct rocketnet_counters_t server_counters;
    bind_family_server_setup( &server, &server_counters );

    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.connect = rocketnet_fake_client_connect;
    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    bind_family_clock_t clock = { 1000.0 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    bind_family_mint_both( token );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.time_callback = bind_family_time;
    memcpy( config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_at_timestamp(
        &clock, token, &config, clock.now, ROCKETNET_TEST_CREATE_TS, &operations, fake );
    rocketnet_unit_check( transport != NULL );

    // both sockets were opened and connected at create, the primary to the
    // IPv6 server and the second to the IPv4 server, before any send
    rocketnet_unit_check( fake->connect_calls == 2 );
    rocketnet_unit_check( rocketnet_address_equal( &fake->connected_to, &ipv4_server ) );
    rocketnet_unit_check( fake->calls_before_connect == 0 );

    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    rocketnet_unit_check( io != NULL );

    int forwarded = 0;
    int cookie_requests_to_ipv6 = 0, cookie_requests_to_ipv4 = 0, requests_to_ipv6 = 0;
    int sent_to_ipv6_after_commit = 0;
    int destroys_at_commit = -1;
    bool late_cookie_sent = false;
    uint8_t late_cookie[ROCKETNET_RECEIVE_CEILING];
    int late_cookie_bytes = 0;
    double connected_time = 0.0;

    for ( clock.now = 1000.0; clock.now < 1000.0 + bind_family_timeout_seconds; clock.now += 0.01 )
    {
        rocketnet_client_io_step( io, false );
        rocketnet_client_transport_update( transport, clock.now );
        rocketnet_client_io_step( io, false );

        const int state = rocketnet_client_transport_state( transport );
        if ( destroys_at_commit < 0 && state >= ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
        {
            // the commit is observed by the reading step, which closes the
            // losing socket at its next wait or receive
            rocketnet_client_io_step( io, false );
            destroys_at_commit = fake->destroy_calls;
        }

        for ( ; forwarded < fake->num_sent; forwarded++ )
        {
            const rocketnet_fake_client_datagram_t * datagram = &fake->sent[forwarded];
            const bool to_ipv6 = rocketnet_address_equal( &datagram->address, &ipv6_server );
            const bool to_ipv4 = rocketnet_address_equal( &datagram->address, &ipv4_server );
            rocketnet_unit_check( to_ipv6 || to_ipv4 );
            if ( datagram->bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES && datagram->data[0] == ROCKETNET_CONNECTION_REQUEST_PACKET )
            {
                if ( to_ipv6 ) requests_to_ipv6++;
                if ( bind_family_request_carries_cookie( datagram ) )
                {
                    if ( to_ipv6 ) cookie_requests_to_ipv6++;
                    if ( to_ipv4 ) cookie_requests_to_ipv4++;
                }
            }
            if ( destroys_at_commit >= 0 && to_ipv6 )
                sent_to_ipv6_after_commit++;

            // the broken IPv6 route: nothing sent there reaches a server
            if ( !to_ipv4 )
                continue;

            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            uint8_t payload[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0, payload_bytes = 0, payload_client_index = -1;
            rocketnet_server_protocol_process_packet( &server, &client_address, datagram->data, datagram->bytes,
                                                      ROCKETNET_TEST_CREATE_TS, clock.now, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
            {
                rocketnet_fake_client_push( fake, &ipv4_server, response, response_bytes );
                if ( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES && late_cookie_bytes == 0 )
                {
                    memcpy( late_cookie, response, (size_t) response_bytes );
                    late_cookie_bytes = response_bytes;
                }
            }
        }

        // after the commit the same cookie arrives from the IPv6 server: the
        // answer that lost the race, already in the receive path
        if ( destroys_at_commit >= 0 && !late_cookie_sent && late_cookie_bytes > 0 )
        {
            rocketnet_fake_client_push( fake, &ipv6_server, late_cookie, late_cookie_bytes );
            late_cookie_sent = true;
        }

        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            connected_time = clock.now;
            break;
        }
    }

    // connected over IPv4, before the token's timeout
    rocketnet_unit_check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_unit_check( connected_time > 0.0 && connected_time < 1000.0 + bind_family_timeout_seconds );

    // the cookie request went to both families; the request carrying the
    // cookie went to IPv4 only, never to IPv6
    rocketnet_unit_check( requests_to_ipv6 > 0 );
    rocketnet_unit_check( cookie_requests_to_ipv4 > 0 );
    rocketnet_unit_check( cookie_requests_to_ipv6 == 0 );
    rocketnet_unit_check( sent_to_ipv6_after_commit == 0 );

    // the losing family's socket closed at the commit; the late IPv6 cookie
    // was dropped and counted
    rocketnet_unit_check( destroys_at_commit == 1 );
    rocketnet_unit_check( late_cookie_sent );
    rocketnet_unit_check( rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] >= 1 );

    rn_test_client_transport_stop_and_destroy( transport );
    rocketnet_unit_check( fake->destroy_calls == 2 );
    delete fake;
    bind_family_server_teardown( &server );
}

REGISTER_UNIT_TEST( test_client_connect_keeps_the_family_that_answers );
