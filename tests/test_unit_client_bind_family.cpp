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
#include "rocketnet_server_protocol.h"
#include "rocketnet_test_vectors.h"
#include "fake_client_driver.h"
#include "unit_shutdown.h"

#include <stdint.h>
#include <stdio.h>
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
    memset( &token, 0, sizeof( token ) );

    const uint8_t loopback_ipv4[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    const uint8_t loopback_ipv6[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };
    memcpy( token.ipv4_address, loopback_ipv4, sizeof( loopback_ipv4 ) );
    memcpy( token.ipv6_address, loopback_ipv6, sizeof( loopback_ipv6 ) );

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

// The family race (TRANSPORT.md t11 rule 3, proofs/tla/ClientConnection.tla
// OneRequestFamily and CommitsOnAnswer, proofs/tla/ServerConnection.tla InFlight).
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

#if ROCKETNET_DEV
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
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = bind_family_timeout_seconds;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;

    const uint8_t ipv4_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, ipv4_address, 6 );

    // eight little-endian groups and the port, as the token carries them

    const uint8_t ipv6_address[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };
    memcpy( signed_fields.ipv6_address, ipv6_address, 18 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static void bind_family_server_setup( struct rocketnet_server_protocol_t * server, struct rocketnet_counters_t * counters )
{
    memset( server, 0, sizeof( *server ) );
    memset( counters, 0, sizeof( *counters ) );
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history &&
                          server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );
}

static void bind_family_server_teardown( struct rocketnet_server_protocol_t * server )
{
    rocketnet_encryption_map_destroy( server->encryption_map );
    rocketnet_used_token_history_destroy( server->used_token_history );
    rocketnet_client_slots_destroy( server->client_slots );
    rocketnet_server_acks_destroy( server->acks );
}

// a connection request whose return cookie field is not all zero

static bool bind_family_request_carries_cookie( const rocketnet_fake_client_datagram_t * datagram )
{
    if ( datagram->bytes != ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES || datagram->data[0] != ROCKETNET_CONNECTION_REQUEST_PACKET )
    {
        return false;
    }
    for ( int i = 0; i < ROCKETNET_COOKIE_FIELD_BYTES; i++ )
    {
        if ( datagram->data[ROCKETNET_PREFILTER_BYTES + i] != 0 )
        {
            return true;
        }
    }

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
    operations.close = rocketnet_fake_client_close;

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    bind_family_clock_t clock = { 1000.0 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    bind_family_mint_both( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
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
    int closes_at_commit = -1;
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
        if ( closes_at_commit < 0 && state >= ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
        {
            // the commit is observed by the reading step, which closes the
            // losing socket's handles at its next wait or receive and frees
            // nothing (proofs/tla/RNClientRace.tla ReaderStep)
            rocketnet_client_io_step( io, false );
            closes_at_commit = fake->close_calls;
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
                if ( to_ipv6 )
                {
                    requests_to_ipv6++;
                }

                if ( bind_family_request_carries_cookie( datagram ) )
                {
                    if ( to_ipv6 )
                    {
                        cookie_requests_to_ipv6++;
                    }
                    if ( to_ipv4 )
                    {
                        cookie_requests_to_ipv4++;
                    }
                }
            }

            if ( closes_at_commit >= 0 && to_ipv6 )
            {
                sent_to_ipv6_after_commit++;
            }

            // the broken IPv6 route: nothing sent there reaches a server

            if ( !to_ipv4 )
            {
                continue;
            }

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

        if ( closes_at_commit >= 0 && !late_cookie_sent && late_cookie_bytes > 0 )
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

    // the losing family's socket closed at the commit, its memory kept for
    // destroy; the late IPv6 cookie was dropped and counted

    rocketnet_unit_check( closes_at_commit == 1 );
    rocketnet_unit_check( destroys_at_commit == 0 );
    rocketnet_unit_check( late_cookie_sent );
    rocketnet_unit_check( rocketnet_client_transport_counters( transport )->values[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] >= 1 );

    // destroy frees both families, the closed one too, and closes nothing more

    rn_test_client_transport_stop_and_destroy( transport );
    rocketnet_unit_check( fake->close_calls == 1 );
    rocketnet_unit_check( fake->destroy_calls == 2 );
    delete fake;
    bind_family_server_teardown( &server );
}

REGISTER_UNIT_TEST( test_client_connect_keeps_the_family_that_answers );

// The race won by the primary family: the same token and server, but the IPv4
// route is broken and IPv6 answers. The client commits to IPv6 (the primary,
// family 0), closes the IPv4 socket and connects. A late cookie from the IPv4
// server, the second family that lost, is then foreign like any other source:
// dropped and counted once, and the connection carries on: a payload from the
// server is read whole, after a read into a NULL buffer consumed nothing.
// Mutation that must bite: keep the race open at a commit to family 0, commit
// the second family whichever answered, or drop the NULL buffer guard. Time is
// an argument, from 1000.0.

static void bind_family_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}

static void test_client_race_won_by_the_primary_drops_the_second()
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
    operations.close = rocketnet_fake_client_close;

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    bind_family_clock_t clock = { 1000.0 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    bind_family_mint_both( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.time_callback = bind_family_time;
    memcpy( config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_at_timestamp(
        &clock, token, &config, clock.now, ROCKETNET_TEST_CREATE_TS, &operations, fake );
    rocketnet_unit_check( transport != NULL );

    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( transport );
    rocketnet_unit_check( io != NULL );

    const uint64_t * counters = rocketnet_client_transport_counters( transport )->values;
    int forwarded = 0;
    int closes_at_commit = -1;
    uint8_t late_cookie[ROCKETNET_RECEIVE_CEILING];
    int late_cookie_bytes = 0;
    uint64_t foreign_before_late = 0;
    bool late_cookie_sent = false;

    for ( clock.now = 1000.0; clock.now < 1000.0 + bind_family_timeout_seconds; clock.now += 0.01 )
    {
        rocketnet_client_io_step( io, false );
        rocketnet_client_transport_update( transport, clock.now );
        rocketnet_client_io_step( io, false );

        const int state = rocketnet_client_transport_state( transport );
        if ( closes_at_commit < 0 && state >= ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
        {
            rocketnet_client_io_step( io, false );
            closes_at_commit = fake->close_calls;
        }

        for ( ; forwarded < fake->num_sent; forwarded++ )
        {
            const rocketnet_fake_client_datagram_t * datagram = &fake->sent[forwarded];
            const bool to_ipv6 = rocketnet_address_equal( &datagram->address, &ipv6_server );
            const bool to_ipv4 = rocketnet_address_equal( &datagram->address, &ipv4_server );
            rocketnet_unit_check( to_ipv6 || to_ipv4 );

            // the broken IPv4 route: nothing sent there reaches a server

            if ( !to_ipv6 )
            {
                continue;
            }

            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            uint8_t payload[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0, payload_bytes = 0, payload_client_index = -1;
            rocketnet_server_protocol_process_packet( &server, &client_address, datagram->data, datagram->bytes,
                                                      ROCKETNET_TEST_CREATE_TS, clock.now, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );

            if ( response_bytes > 0 )
            {
                rocketnet_fake_client_push( fake, &ipv6_server, response, response_bytes );

                if ( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES && late_cookie_bytes == 0 )
                {
                    memcpy( late_cookie, response, (size_t) response_bytes );
                    late_cookie_bytes = response_bytes;
                }
            }
        }

        // after the commit the same cookie arrives from the IPv4 server

        if ( closes_at_commit >= 0 && !late_cookie_sent && late_cookie_bytes > 0 )
        {
            foreign_before_late = counters[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE];
            rocketnet_fake_client_push( fake, &ipv4_server, late_cookie, late_cookie_bytes );
            late_cookie_sent = true;
        }

        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            break;
        }
    }

    // connected over IPv6; the IPv4 socket closed at the commit; the late
    // IPv4 cookie was dropped as foreign, once

    rocketnet_unit_check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_unit_check( closes_at_commit == 1 );
    rocketnet_unit_check( late_cookie_sent );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] == foreign_before_late + 1 );

    // a payload from the server over IPv6 is queued; a read into a NULL
    // buffer is a counted no-op that consumes nothing, and the next read
    // with a buffer gets the payload whole

    const uint8_t sent_payload[4] = { 0xC1, 0xC2, 0xC3, 0xC4 };
    uint8_t wire[ROCKETNET_RECEIVE_CEILING];
    const int wire_bytes = rocketnet_server_protocol_write_payload( &server, 0, sent_payload, (int) sizeof( sent_payload ), clock.now, wire );
    rocketnet_unit_check( wire_bytes > 0 );
    rocketnet_fake_client_push( fake, &ipv6_server, wire, wire_bytes );
    clock.now += 0.01;
    rocketnet_client_io_step( io, false );
    rocketnet_client_transport_update( transport, clock.now );

    rocketnet_assert_function( bind_family_quiet_assert );
    const uint64_t misuse = rocketnet_api_misuse_count();
    rocketnet_unit_check( rocketnet_client_transport_receive_packet( transport, NULL, 64 ) == 0 );
    rocketnet_unit_check( rocketnet_api_misuse_count() == misuse + 1 );
    rocketnet_assert_function( NULL );

    uint8_t received[64];
    rocketnet_unit_check( rocketnet_client_transport_receive_packet( transport, received, (int) sizeof( received ) ) == (int) sizeof( sent_payload ) );
    rocketnet_unit_check( memcmp( received, sent_payload, sizeof( sent_payload ) ) == 0 );

    rn_test_client_transport_stop_and_destroy( transport );
    rocketnet_unit_check( fake->close_calls == 1 );
    rocketnet_unit_check( fake->destroy_calls == 2 );
    delete fake;
    bind_family_server_teardown( &server );
}

REGISTER_UNIT_TEST( test_client_race_won_by_the_primary_drops_the_second );

// The family race at its commit, on the same token, server and broken IPv6
// route, with one fake socket per family: race_pair_t's create hands out
// fake[0] for the primary (IPv6) family and fake[1] for the IPv4 family, so
// each family's sends, receives and send failures are its own.

struct race_pair_t
{
    rocketnet_fake_client_transport_io_t fake[2];
    int creates;
    uint64_t send_failures[2];
    int progress_refusals[2];
    int progress_calls[2];
};

static race_pair_t * race_pair;

static void * race_pair_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    race_pair_t * pair = (race_pair_t *) context;
    rocketnet_unit_check( pair->creates < 2 );
    return pair->creates < 2 ? &pair->fake[pair->creates++] : NULL;
}

static uint64_t race_pair_send_failures( void * instance )
{
    return race_pair->send_failures[instance == &race_pair->fake[1] ? 1 : 0];
}

static int race_pair_send_progress( void * instance, const rocketnet_platform_send_t * sends, int count )
{
    const int family = instance == &race_pair->fake[1] ? 1 : 0;
    rocketnet_unit_check( race_pair->fake[family].close_calls == 0 );
    race_pair->progress_calls[family]++;
    if ( race_pair->progress_refusals[family] > 0 )
    {
        race_pair->progress_refusals[family]--;
        return 0;
    }
    rocketnet_fake_client_send_batch( instance, sends, count );
    return count;
}

// a batch read never asks a socket for nothing: the race reads the second
// family only with room left after the first
static int race_pair_receive_batch( void * instance, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_unit_check( max_packets > 0 );
    return rocketnet_fake_client_receive_batch( instance, from, packet_data, packet_stride, packet_bytes, max_packets );
}

struct race_harness_t
{
    rocketnet_address_t ipv4_server, ipv6_server, client_address;
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    rocketnet_client_transport_io_operations_t operations;
    race_pair_t pair;
    bind_family_clock_t clock;
    rocketnet_client_transport_t * transport;
    rocketnet_client_io_t * io;
    int forwarded;      // fake[1]'s sends already handed to the server
};

static void race_harness_setup( race_harness_t * h, bool progress = false, bool batch = false )
{
    rocketnet_unit_check( rocketnet_address_parse( &h->ipv4_server, "203.0.113.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &h->ipv6_server, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &h->client_address, "198.51.100.7:56789" ) );

    bind_family_server_setup( &h->server, &h->server_counters );

    h->operations = batch ? rocketnet_fake_client_operations_batch : rocketnet_fake_client_operations_io;
    if ( batch )
    {
        h->operations.receive_batch = race_pair_receive_batch;
    }
    h->operations.create = race_pair_create;
    h->operations.connect = rocketnet_fake_client_connect;
    h->operations.close = rocketnet_fake_client_close;
    h->operations.send_failures = race_pair_send_failures;
    if ( progress )
    {
        h->operations.send_batch = rocketnet_fake_client_send_batch;
        h->operations.send_batch_progress = race_pair_send_progress;
    }
    race_pair = &h->pair;

    h->clock.now = 1000.0;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    bind_family_mint_both( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.time_callback = bind_family_time;
    memcpy( config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    h->transport = rocketnet_client_transport_create_step_driven_at_timestamp(
        &h->clock, token, &config, h->clock.now, ROCKETNET_TEST_CREATE_TS, &h->operations, &h->pair );
    rocketnet_unit_check( h->transport != NULL );
    rocketnet_unit_check( h->pair.creates == 2 );
    rocketnet_unit_check( rocketnet_address_equal( &h->pair.fake[0].connected_to, &h->ipv6_server ) );
    rocketnet_unit_check( rocketnet_address_equal( &h->pair.fake[1].connected_to, &h->ipv4_server ) );

    h->io = rocketnet_client_transport_client_io( h->transport );
    rocketnet_unit_check( h->io != NULL );
}

static void race_harness_teardown( race_harness_t * h )
{
    rn_test_client_transport_stop_and_destroy( h->transport );
    race_pair = NULL;
    bind_family_server_teardown( &h->server );
}

// the IPv4 socket's sends reach the server, whose answers arrive on the IPv4
// socket; nothing sent on the IPv6 socket is answered

static void race_harness_forward( race_harness_t * h )
{
    rocketnet_fake_client_transport_io_t * ipv4 = &h->pair.fake[1];
    for ( ; h->forwarded < ipv4->num_sent; h->forwarded++ )
    {
        const rocketnet_fake_client_datagram_t * datagram = &ipv4->sent[h->forwarded];
        rocketnet_unit_check( rocketnet_address_equal( &datagram->address, &h->ipv4_server ) );

        uint8_t response[ROCKETNET_RECEIVE_CEILING];
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int response_bytes = 0, payload_bytes = 0, payload_client_index = -1;
        rocketnet_server_protocol_process_packet( &h->server, &h->client_address, datagram->data, datagram->bytes,
                                                  ROCKETNET_TEST_CREATE_TS, h->clock.now, response, &response_bytes,
                                                  payload, &payload_bytes, &payload_client_index );
        if ( response_bytes > 0 )
        {
            rocketnet_fake_client_push( ipv4, &h->ipv4_server, response, response_bytes );
        }
    }
}

static uint64_t race_harness_send_failed( race_harness_t * h )
{
    return rocketnet_client_transport_counters( h->transport )->values[ROCKETNET_COUNTER_SEND_FAILED];
}

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// proofs/tla/RNClientRace.tla PostCommitSlotsKept. The update that resends the
// cookie request meets the I/O thread holding the mutex: the hand-off is LATE
// and the Filling ring stays bound to the IPv6 server, the protocol's server
// before the commit. The next update takes the IPv4 cookie, commits the race
// to IPv4, and commits the connection request into that same ring; the
// commit rebinds the ring to the IPv4 server, so the request and the cookie
// request before it leave for IPv4 in that update and none is dropped as the
// losing family's. The test holds the mutex on its own thread, so it runs
// where the platform mutex is not recursive (a Windows critical section is).

static void test_client_race_commit_rebinds_a_late_ring()
{
    static race_harness_t h;
    memset( &h, 0, sizeof( h ) );
    race_harness_setup( &h );

    rocketnet_client_io_step( h.io, false );
    rocketnet_client_transport_update( h.transport, h.clock.now );
    rocketnet_client_io_step( h.io, false );
    race_harness_forward( &h );
    rocketnet_unit_check( h.pair.fake[1].inbound_count == 1 );

    h.clock.now = 1000.2;
    const uint64_t deferred_before = rocketnet_client_transport_counters( h.transport )->values[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED];
    rocketnet_platform_mutex_acquire( h.io->mutex );
    rocketnet_client_transport_update( h.transport, h.clock.now );
    rocketnet_platform_mutex_release( h.io->mutex );
    rocketnet_unit_check( rocketnet_client_transport_counters( h.transport )->values[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED] == deferred_before + 1 );
    rocketnet_unit_check( rocketnet_client_transport_state( h.transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    // the reading step takes the cookie; no ring was handed off, so it sends nothing

    const int ipv6_sent = h.pair.fake[0].num_sent;
    const int ipv4_sent = h.pair.fake[1].num_sent;
    rocketnet_client_io_step( h.io, false );
    rocketnet_unit_check( h.pair.fake[0].num_sent == ipv6_sent && h.pair.fake[1].num_sent == ipv4_sent );

    h.clock.now = 1000.4;
    rocketnet_client_transport_update( h.transport, h.clock.now );
    rocketnet_client_io_step( h.io, false );
    rocketnet_unit_check( rocketnet_client_transport_state( h.transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

    // both slots of the ring reached IPv4, the connection request carrying
    // the cookie among them; nothing went to IPv6

    int cookie_requests_to_ipv4 = 0;
    for ( int i = ipv4_sent; i < h.pair.fake[1].num_sent; i++ )
    {
        if ( bind_family_request_carries_cookie( &h.pair.fake[1].sent[i] ) )
        {
            cookie_requests_to_ipv4++;
        }
    }

    if ( h.pair.fake[1].num_sent - ipv4_sent != 2 || cookie_requests_to_ipv4 != 1 )
    {
        printf( "test_client_race_commit_rebinds_a_late_ring: %d sent to IPv4, %d carrying the cookie, %d to IPv6\n",
                h.pair.fake[1].num_sent - ipv4_sent, cookie_requests_to_ipv4, h.pair.fake[0].num_sent - ipv6_sent );
    }

    rocketnet_unit_check( h.pair.fake[1].num_sent - ipv4_sent == 2 );
    rocketnet_unit_check( cookie_requests_to_ipv4 == 1 );
    rocketnet_unit_check( h.pair.fake[0].num_sent == ipv6_sent );

    race_harness_teardown( &h );
}

REGISTER_UNIT_TEST( test_client_race_commit_rebinds_a_late_ring );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// proofs/tla/RNClientRace.tla SendFailedMonotonic. Sends failed on both sockets
// while the race was in flight, three on IPv6, the family that loses. The
// update that commits to IPv4 reads the counters before the reading step
// closes the IPv6 socket: SEND_FAILED still holds IPv6's three, kept at the
// commit, and the close keeps the same count.

static void test_client_race_send_failed_holds_across_the_commit()
{
    static race_harness_t h;
    memset( &h, 0, sizeof( h ) );
    race_harness_setup( &h );

    rocketnet_client_io_step( h.io, false );
    rocketnet_client_transport_update( h.transport, h.clock.now );
    rocketnet_client_io_step( h.io, false );
    race_harness_forward( &h );

    h.pair.send_failures[0] = 3;
    h.pair.send_failures[1] = 1;
    const uint64_t before = race_harness_send_failed( &h );
    rocketnet_unit_check( before == 4 );

    rocketnet_client_io_step( h.io, false );
    h.clock.now = 1000.05;
    rocketnet_client_transport_update( h.transport, h.clock.now );
    rocketnet_unit_check( rocketnet_client_transport_state( h.transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    rocketnet_unit_check( h.pair.fake[0].close_calls == 0 );

    const uint64_t at_commit = race_harness_send_failed( &h );
    if ( at_commit != before )
    {
        printf( "test_client_race_send_failed_holds_across_the_commit: SEND_FAILED %llu before the commit, %llu after it\n",
                (unsigned long long) before, (unsigned long long) at_commit );
    }
    rocketnet_unit_check( at_commit == before );

    rocketnet_client_io_step( h.io, false );
    rocketnet_unit_check( h.pair.fake[0].close_calls == 1 );
    rocketnet_unit_check( race_harness_send_failed( &h ) == before );

    race_harness_teardown( &h );
}

REGISTER_UNIT_TEST( test_client_race_send_failed_holds_across_the_commit );
// Exercise the real family adapter directly; only the physical transports
// are fake. A refused copy must not make its already-sent twin repeat.
extern bool rocketnet_client_transport_commit_race_for_test( rocketnet_client_transport_t *, int );

static void test_client_race_partial_fanout_retries_physical_copy_once()
{
    race_harness_t * storage = new race_harness_t();
    race_harness_t & h = *storage;
    race_harness_setup( &h, true );
    const rocketnet_client_transport_io_t * io = h.io->transport;
    const uint8_t data[] = { 10, 20 };
    rocketnet_platform_send_t sends[2] = {
        { &h.ipv6_server, &data[0], 1 }, { &h.ipv6_server, &data[1], 1 }
    };
    h.pair.progress_refusals[1] = 1;
    rocketnet_unit_check( io->operations->send_batch_progress( io->instance, sends, 2 ) == 0 );
    rocketnet_unit_check( h.pair.fake[0].num_sent == 1 && h.pair.fake[1].num_sent == 0 );
    rocketnet_unit_check( io->operations->send_batch_progress( io->instance, sends, 2 ) == 2 );
    for ( int family = 0; family < 2; family++ )
    {
        rocketnet_unit_check( h.pair.fake[family].num_sent == 2 );
        rocketnet_unit_check( h.pair.fake[family].sent[0].data[0] == 10 );
        rocketnet_unit_check( h.pair.fake[family].sent[1].data[0] == 20 );
    }
    race_harness_teardown( &h );
    delete storage;
}
REGISTER_UNIT_TEST( test_client_race_partial_fanout_retries_physical_copy_once );

static void test_client_race_partial_fanout_commit_keeps_winning_copy()
{
    for ( int blocked = 0; blocked < 2; blocked++ )
    {
        for ( int winner = 0; winner < 2; winner++ )
        {
            race_harness_t * storage = new race_harness_t();
            race_harness_t & h = *storage;
            race_harness_setup( &h, true );
            const rocketnet_client_transport_io_t * io = h.io->transport;
            const uint8_t data = 42;
            rocketnet_platform_send_t send = { &h.ipv6_server, &data, 1 };
            h.pair.progress_refusals[blocked] = 1;
            rocketnet_unit_check( io->operations->send_batch_progress( io->instance, &send, 1 ) == 0 );
            rocketnet_unit_check( h.pair.fake[blocked].num_sent == 0 && h.pair.fake[1 - blocked].num_sent == 1 );
            rocketnet_unit_check( h.pair.progress_calls[0] == 1 && h.pair.progress_calls[1] == 1 );
            rocketnet_unit_check( rocketnet_client_transport_commit_race_for_test( h.transport, winner ) );
            rocketnet_address_t from;
            uint8_t receive[16];
            rocketnet_unit_check( io->operations->receive( io->instance, &from, receive, sizeof( receive ) ) == 0 );
            rocketnet_unit_check( h.pair.fake[1 - winner].close_calls == 1 );
            rocketnet_unit_check( io->operations->send_batch_progress( io->instance, &send, 1 ) == 1 );
            for ( int family = 0; family < 2; family++ )
            {
                rocketnet_unit_check( h.pair.fake[family].num_sent == ( family != blocked || family == winner ? 1 : 0 ) );
            }
            rocketnet_unit_check( h.pair.fake[winner].sent[0].data[0] == 42 );
            race_harness_teardown( &h );
            delete storage;
        }
    }
}
REGISTER_UNIT_TEST( test_client_race_partial_fanout_commit_keeps_winning_copy );

static void test_client_race_partial_fanout_shutdown_attempts_only_missing_copy()
{
    race_harness_t * storage = new race_harness_t();
    race_harness_t & h = *storage;
    race_harness_setup( &h, true );
    const rocketnet_client_transport_io_t * io = h.io->transport;
    const uint8_t data = 73;
    rocketnet_platform_send_t send = { &h.ipv6_server, &data, 1 };
    h.pair.progress_refusals[1] = 1000;
    rocketnet_unit_check( io->operations->send_batch_progress( io->instance, &send, 1 ) == 0 );
    rocketnet_unit_check( h.pair.fake[0].num_sent == 1 && h.pair.fake[1].num_sent == 0 );
    io->operations->send_batch( io->instance, &send, 1 );
    rocketnet_unit_check( h.pair.fake[0].num_sent == 1 && h.pair.fake[1].num_sent == 1 );
    rocketnet_unit_check( h.pair.fake[0].sent[0].data[0] == 73 && h.pair.fake[1].sent[0].data[0] == 73 );
    rocketnet_unit_check( h.pair.progress_calls[0] == 1 && h.pair.progress_calls[1] == 1 );
    race_harness_teardown( &h );
    delete storage;
}
REGISTER_UNIT_TEST( test_client_race_partial_fanout_shutdown_attempts_only_missing_copy );

// The race to its end with one fake per family, for both outcomes
// (TRANSPORT.md t11 rule 3; proofs/tla/RNClientRace.tla CallerSend, CallerRead,
// ReaderStep, CallerCommit, CallerDestroy). The winning family's server
// answers (the same server protocol for either family); the losing family's
// route never reaches it, and its server's cookie arrives never, early (read
// before the commit, processed after it) or late (after the reader closed
// that family). Each outcome runs on the per-packet table (receive, wait,
// wake) and on the batch table (send_batch, receive_batch too).
//
// Checked: before the commit every packet to the primary server went to the
// second server too, on its own socket, and both families were woken; the
// commit keeps the winning family and the protocol's server is its server;
// from the commit on the losing family is never sent to, read, waited on or
// woken, and its socket is closed once by the reading step; the winning
// family is read, woken and waited on (the race's poll while in flight, the
// I/O guard after); an early losing cookie is dropped as a foreign source
// exactly once and changes nothing, a late one is never read; the losing
// family's send failures (all its sends fail when it never answers) are
// kept in SEND_FAILED at the commit and failures after its close add
// nothing; the connection reaches CONNECTED, and its disconnect burst leaves
// on the winning family only, the whole burst, also when the disconnect
// comes as the client answers the challenge (a ring still bound to the
// primary server from before the commit). Destroy frees each family once
// and closes nothing more. Time is an argument, from 1000.0.

enum race_loser_cookie_t
{
    RACE_LOSER_NEVER,
    RACE_LOSER_EARLY,
    RACE_LOSER_LATE
};

struct race_world_t
{
    race_harness_t h;
    int winner;
    int loser;
    race_loser_cookie_t loser_cookie;
    rocketnet_address_t server[2];
    int forwarded[2];
    uint8_t cookie[ROCKETNET_RECEIVE_CEILING];
    int cookie_bytes;
    bool loser_cookie_sent;
    bool committed;
    rocketnet_fake_client_transport_io_t loser_at_commit;   // the losing fake's counters just after the committing update
    int winner_sent_at_commit;
    int winner_wakes_at_commit;
    uint64_t send_failed_at_commit;
};

// the winning family's new sends reach the server, its answers come back on
// the winning socket from the winning server; the losing family's sends are
// lost and, when its server never answers, each one failed

static void race_world_forward( race_world_t * w )
{
    race_harness_t * h = &w->h;
    for ( int family = 0; family < 2; family++ )
    {
        rocketnet_fake_client_transport_io_t * fake = &h->pair.fake[family];
        for ( ; w->forwarded[family] < fake->num_sent; w->forwarded[family]++ )
        {
            const rocketnet_fake_client_datagram_t * datagram = &fake->sent[w->forwarded[family]];
            rocketnet_unit_check( rocketnet_address_equal( &datagram->address, &w->server[family] ) );

            if ( family != w->winner )
            {
                if ( w->loser_cookie == RACE_LOSER_NEVER )
                {
                    h->pair.send_failures[family]++;
                }
                continue;
            }

            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            uint8_t payload[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0, payload_bytes = 0, payload_client_index = -1;
            rocketnet_server_protocol_process_packet( &h->server, &h->client_address, datagram->data, datagram->bytes,
                                                      ROCKETNET_TEST_CREATE_TS, h->clock.now, response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
            {
                rocketnet_fake_client_push( fake, &w->server[family], response, response_bytes );
                if ( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES && w->cookie_bytes == 0 )
                {
                    memcpy( w->cookie, response, (size_t) response_bytes );
                    w->cookie_bytes = response_bytes;
                }
            }
        }
    }
}

static int race_world_state( race_world_t * w )
{
    return rocketnet_client_transport_state( w->h.transport );
}

// one update of the caller with the reading steps around it; the early
// losing cookie is pushed once the winning cookie was read and read by one
// more step before the update that commits

static void race_world_tick( race_world_t * w )
{
    race_harness_t * h = &w->h;
    rocketnet_fake_client_transport_io_t * winner = &h->pair.fake[w->winner];
    rocketnet_fake_client_transport_io_t * loser = &h->pair.fake[w->loser];

    const bool cookie_waiting = w->cookie_bytes > 0 && winner->inbound_count > 0 &&
                                race_world_state( w ) == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE;

    rocketnet_client_io_step( h->io, false );

    if ( cookie_waiting && w->loser_cookie == RACE_LOSER_EARLY && !w->loser_cookie_sent )
    {
        rocketnet_unit_check( winner->inbound_count == 0 );
        rocketnet_fake_client_push( loser, &w->server[w->loser], w->cookie, w->cookie_bytes );
        rocketnet_client_io_step( h->io, false );
        rocketnet_unit_check( loser->inbound_count == 0 );
        w->loser_cookie_sent = true;
    }

    if ( !w->committed )
    {
        // in flight: every packet to the primary went to both servers, each
        // on its own family's socket, and every hand-off woke both

        rocketnet_unit_check( h->pair.fake[0].num_sent == h->pair.fake[1].num_sent );
        for ( int i = 0; i < h->pair.fake[0].num_sent; i++ )
        {
            rocketnet_unit_check( h->pair.fake[0].sent[i].bytes == h->pair.fake[1].sent[i].bytes );
            rocketnet_unit_check( memcmp( h->pair.fake[0].sent[i].data, h->pair.fake[1].sent[i].data, (size_t) h->pair.fake[0].sent[i].bytes ) == 0 );
        }
        rocketnet_unit_check( h->pair.fake[0].wake_calls == h->pair.fake[1].wake_calls );
        rocketnet_unit_check( h->pair.fake[0].close_calls == 0 && h->pair.fake[1].close_calls == 0 );
    }

    const int state_before = race_world_state( w );
    rocketnet_client_transport_update( h->transport, h->clock.now );

    if ( state_before == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE && race_world_state( w ) == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
    {
        rocketnet_unit_check( !w->committed );
        w->committed = true;
        w->loser_at_commit = *loser;
        w->winner_sent_at_commit = winner->num_sent;
        w->winner_wakes_at_commit = winner->wake_calls;
        w->send_failed_at_commit = race_harness_send_failed( h );
    }

    rocketnet_client_io_step( h->io, false );
    race_world_forward( w );

    if ( w->committed )
    {
        // the reading step closed the losing socket once; nothing touches it
        // again: no send, read, wait or wake

        rocketnet_unit_check( loser->close_calls == 1 );
        rocketnet_unit_check( winner->close_calls == 0 );
        rocketnet_unit_check( loser->num_sent == w->loser_at_commit.num_sent );
        rocketnet_unit_check( loser->send_calls == w->loser_at_commit.send_calls );
        rocketnet_unit_check( loser->send_batch_calls == w->loser_at_commit.send_batch_calls );
        rocketnet_unit_check( loser->receive_calls == w->loser_at_commit.receive_calls );
        rocketnet_unit_check( loser->receive_batch_calls == w->loser_at_commit.receive_batch_calls );
        rocketnet_unit_check( loser->wait_calls == w->loser_at_commit.wait_calls );
        rocketnet_unit_check( loser->wake_calls == w->loser_at_commit.wake_calls );

        // every later send goes to the winning server on the winning socket

        for ( int i = w->winner_sent_at_commit; i < winner->num_sent; i++ )
        {
            rocketnet_unit_check( rocketnet_address_equal( &winner->sent[i].address, &w->server[w->winner] ) );
        }

        if ( w->loser_cookie == RACE_LOSER_LATE && !w->loser_cookie_sent )
        {
            rocketnet_fake_client_push( loser, &w->server[w->loser], w->cookie, w->cookie_bytes );
            w->loser_cookie_sent = true;
        }
    }

    h->clock.now += 0.01;
}

static race_world_t * race_world_create( bool batch, int winner, race_loser_cookie_t loser_cookie )
{
    race_world_t * w = new race_world_t();
    race_harness_t * h = &w->h;
    race_harness_setup( h, false, batch );
    w->winner = winner;
    w->loser = 1 - winner;
    w->loser_cookie = loser_cookie;
    w->server[0] = h->ipv6_server;
    w->server[1] = h->ipv4_server;
    return w;
}

// ticks until the state is reached, a terminal state or the token's timeout

static void race_world_until( race_world_t * w, int until_state )
{
    while ( w->h.clock.now < 1000.0 + bind_family_timeout_seconds )
    {
        race_world_tick( w );

        const int state = race_world_state( w );
        if ( state == until_state || state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED || state < 0 )
        {
            break;
        }
    }
}

static void race_world_destroy( race_world_t * w )
{
    race_harness_teardown( &w->h );
    delete w;
}

static void race_world_run( bool batch, int winner, race_loser_cookie_t loser_cookie, bool disconnect_at_response )
{
    race_world_t * w = race_world_create( batch, winner, loser_cookie );
    race_harness_t * h = &w->h;

    const uint64_t * counters = rocketnet_client_transport_counters( h->transport )->values;
    const uint64_t foreign_before = counters[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE];

    // in flight and idle: the reading step polls the primary socket, for the
    // race's poll at most, readable, and reports readable so the second is read

    rocketnet_client_io_step( h->io, true );
    rocketnet_unit_check( h->pair.fake[0].wait_calls == 1 && h->pair.fake[1].wait_calls == 0 );
    rocketnet_unit_check( h->pair.fake[0].last_wait_readable );
    rocketnet_unit_check( h->pair.fake[0].last_wait_timeout == 0.001 );

    bool disconnected = false;
    int winner_sent_at_disconnect = 0;
    race_world_until( w, disconnect_at_response ? ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE
                                                : ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_fake_client_transport_io_t * won = &h->pair.fake[winner];
    rocketnet_fake_client_transport_io_t * lost = &h->pair.fake[w->loser];

    rocketnet_unit_check( w->committed );
    rocketnet_unit_check( race_world_state( w ) == ( disconnect_at_response ? ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE
                                                                            : ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ) );

    // the winning family was sent to and woken after the commit

    rocketnet_unit_check( won->num_sent > w->winner_sent_at_commit );
    rocketnet_unit_check( won->wake_calls > w->winner_wakes_at_commit );

    // the losing cookie: early, dropped as foreign once; late, never read;
    // never, nothing foreign

    rocketnet_unit_check( w->loser_cookie_sent == ( loser_cookie != RACE_LOSER_NEVER ) );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_CLIENT_RECEIVE_FOREIGN_SOURCE] == foreign_before + ( loser_cookie == RACE_LOSER_EARLY ? 1 : 0 ) );
    rocketnet_unit_check( lost->inbound_count == ( loser_cookie == RACE_LOSER_LATE ? 1 : 0 ) );

    // the losing family's send failures were kept at the commit; failures
    // on it after its close add nothing

    const uint64_t send_failed = race_harness_send_failed( h );
    rocketnet_unit_check( send_failed == w->send_failed_at_commit );
    rocketnet_unit_check( send_failed == ( loser_cookie == RACE_LOSER_NEVER ? (uint64_t) w->loser_at_commit.num_sent : 0 ) );
    h->pair.send_failures[w->loser] += 5;
    rocketnet_unit_check( race_harness_send_failed( h ) == send_failed );

    // connected and idle: the reading step waits on the winning family alone,
    // for the I/O guard

    rocketnet_client_io_step( h->io, false );
    const int won_waits = won->wait_calls;
    rocketnet_client_io_step( h->io, true );
    rocketnet_unit_check( won->wait_calls == won_waits + 1 );
    rocketnet_unit_check( won->last_wait_readable );
    rocketnet_unit_check( won->last_wait_timeout == h->io->guard_timeout_seconds );
    rocketnet_unit_check( lost->wait_calls == w->loser_at_commit.wait_calls );

    // the disconnect burst leaves whole, on the winning family only

    winner_sent_at_disconnect = won->num_sent;
    rocketnet_client_transport_disconnect( h->transport );
    disconnected = race_world_state( w ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
    rocketnet_client_io_step( h->io, false );
    rocketnet_client_io_step( h->io, false );
    rocketnet_unit_check( disconnected );

    int burst = 0;
    for ( int i = winner_sent_at_disconnect; i < won->num_sent; i++ )
    {
        rocketnet_unit_check( rocketnet_address_equal( &won->sent[i].address, &w->server[winner] ) );
        if ( won->sent[i].data[0] == ROCKETNET_CONNECTION_DISCONNECT_PACKET )
        {
            burst++;
        }
    }
    if ( burst != ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS )
    {
        printf( "race_world_run( batch %d, winner %d, loser cookie %d, disconnect at response %d ): %d disconnect packets on the winning family\n",
                batch ? 1 : 0, winner, (int) loser_cookie, disconnect_at_response ? 1 : 0, burst );
    }
    rocketnet_unit_check( burst == ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    rocketnet_unit_check( lost->num_sent == w->loser_at_commit.num_sent );

    // destroy frees each family once; the losing one stays closed once and
    // the kept one is closed by its destroy, not by close

    race_harness_teardown( h );
    rocketnet_unit_check( lost->num_sent == w->loser_at_commit.num_sent );
    rocketnet_unit_check( won->destroy_calls == 1 && lost->destroy_calls == 1 );
    rocketnet_unit_check( won->close_calls == 0 && lost->close_calls == 1 );
    delete w;
}

// The client's payload accessor reads only this update's batch (the twin of
// test_server_transport_reads_refuse_null_outputs_and_stale_indexes): after an
// update that delivered two payloads, the next delivers one, and index 1
// still holds the earlier update's entry; index 1 (the count), -1 and
// INT32_MAX give NULL and 0 bytes, index 0 the new payload. Mutation that
// must bite: `||` to `&&`, or `>=` to `>`, in the index check.

static void race_world_deliver( race_world_t * w, const uint8_t * data, int bytes )
{
    race_harness_t * h = &w->h;
    uint8_t wire[ROCKETNET_RECEIVE_CEILING];
    const int wire_bytes = rocketnet_server_protocol_write_payload( &h->server, 0, data, bytes, h->clock.now, wire );
    rocketnet_unit_check( wire_bytes > 0 );
    rocketnet_fake_client_push( &h->pair.fake[w->winner], &w->server[w->winner], wire, wire_bytes );
}

static void test_client_transport_payload_refuses_indexes_outside_the_batch()
{
    race_world_t * w = race_world_create( false, 1, RACE_LOSER_NEVER );
    race_harness_t * h = &w->h;
    race_world_until( w, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_unit_check( race_world_state( w ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const uint8_t first[3] = { 0xA1, 0xA2, 0xA3 };
    const uint8_t second[5] = { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5 };
    const uint8_t third[2] = { 0xC1, 0xC2 };

    race_world_deliver( w, first, (int) sizeof( first ) );
    race_world_deliver( w, second, (int) sizeof( second ) );
    h->clock.now += 0.01;
    rocketnet_client_io_step( h->io, false );
    rocketnet_client_transport_update( h->transport, h->clock.now );
    rocketnet_unit_check( rocketnet_client_transport_num_payloads( h->transport ) == 2 );

    race_world_deliver( w, third, (int) sizeof( third ) );
    h->clock.now += 0.01;
    rocketnet_client_io_step( h->io, false );
    rocketnet_client_transport_update( h->transport, h->clock.now );
    rocketnet_unit_check( rocketnet_client_transport_num_payloads( h->transport ) == 1 );

    int bytes = 9;
    uint8_t * payload = rocketnet_client_transport_payload( h->transport, 0, &bytes );
    rocketnet_unit_check( payload != NULL && bytes == (int) sizeof( third ) );
    rocketnet_unit_check( payload != NULL && memcmp( payload, third, sizeof( third ) ) == 0 );

    const int outside[3] = { 1, -1, INT32_MAX };
    for ( int i = 0; i < 3; i++ )
    {
        bytes = 9;
        rocketnet_unit_check( rocketnet_client_transport_payload( h->transport, outside[i], &bytes ) == NULL );
        rocketnet_unit_check( bytes == 0 );
    }

    race_world_destroy( w );
}

REGISTER_UNIT_TEST( test_client_transport_payload_refuses_indexes_outside_the_batch );

// The hand-off's statistics count what goes on the wire (the cumulative
// totals of rocketnet_client_transport_stats): a send_packets with nothing
// committed hands nothing off and wakes nothing; a payload started and
// aborted leaves a slot of 0 bytes that is neither sent nor counted; the
// packets and bytes counted (wire bytes plus the IPv4 header) equal the
// datagrams the winning family's socket took. Mutation that must bite:
// count a slot of 0 bytes, or hand off an empty ring.

static void test_client_hand_off_counts_only_what_it_sends()
{
    race_world_t * w = race_world_create( false, 1, RACE_LOSER_NEVER );
    race_harness_t * h = &w->h;
    race_world_until( w, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_unit_check( race_world_state( w ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_client_io_step( h->io, false );

    rocketnet_fake_client_transport_io_t * won = &h->pair.fake[w->winner];
    const uint64_t * counters = rocketnet_client_transport_counters( h->transport )->values;

    // nothing committed: no hand-off, no wake, nothing deferred

    const int wakes = won->wake_calls;
    const uint64_t deferred = counters[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED];
    rocketnet_client_transport_send_packets( h->transport );
    rocketnet_unit_check( won->wake_calls == wakes );
    rocketnet_unit_check( counters[ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED] == deferred );

    struct rocketnet_client_transport_stats_t before;
    rocketnet_client_transport_stats( h->transport, &before );
    const int sent_before = won->num_sent;

    struct rocketnet_packet_context_t aborted;
    rocketnet_unit_check( rocketnet_client_transport_start_packet( h->transport, 100, &aborted ) != NULL );
    rocketnet_client_transport_abort_packet( h->transport, &aborted );

    struct rocketnet_packet_context_t context;
    rocketnet_unit_check( rocketnet_client_transport_start_packet( h->transport, 100, &context ) != NULL );
    int capacity = 0;
    uint8_t * payload = rocketnet_client_transport_packet_payload( h->transport, &context, &capacity );
    rocketnet_unit_check( payload != NULL && capacity >= 40 );
    if ( payload )
    {
        memset( payload, 0x5C, 40 );
    }
    const int wire_bytes = rocketnet_client_transport_finish_packet( h->transport, &context, 40 );
    rocketnet_unit_check( wire_bytes > 40 );

    rocketnet_client_transport_send_packets( h->transport );
    rocketnet_client_io_step( h->io, false );

    struct rocketnet_client_transport_stats_t after;
    rocketnet_client_transport_stats( h->transport, &after );

    uint64_t wire_total = 0;
    for ( int i = sent_before; i < won->num_sent; i++ )
    {
        wire_total += (uint64_t) ( won->sent[i].bytes + rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 ) );
    }
    rocketnet_unit_check( won->num_sent == sent_before + 1 );
    rocketnet_unit_check( won->sent[sent_before].bytes == wire_bytes );
    rocketnet_unit_check( after.packets_sent == before.packets_sent + (uint64_t) ( won->num_sent - sent_before ) );
    rocketnet_unit_check( after.bytes_sent == before.bytes_sent + wire_total );

    race_world_destroy( w );
}

REGISTER_UNIT_TEST( test_client_hand_off_counts_only_what_it_sends );

static void test_client_race_each_family_wins_and_the_other_is_dropped()
{
    for ( int batch = 0; batch < 2; batch++ )
    {
        for ( int winner = 0; winner < 2; winner++ )
        {
            race_world_run( batch != 0, winner, RACE_LOSER_NEVER, false );
            race_world_run( batch != 0, winner, RACE_LOSER_EARLY, false );
            race_world_run( batch != 0, winner, RACE_LOSER_LATE, false );
            race_world_run( batch != 0, winner, RACE_LOSER_NEVER, true );
        }
    }
}

REGISTER_UNIT_TEST( test_client_race_each_family_wins_and_the_other_is_dropped );

// A race nobody answers, destroyed in flight: each family is freed once and
// neither is closed (only a commit closes a family before destroy).

static void test_client_race_destroyed_in_flight_frees_both()
{
    for ( int batch = 0; batch < 2; batch++ )
    {
        race_harness_t * h = new race_harness_t();
        race_harness_setup( h, false, batch != 0 );
        for ( int i = 0; i < 20; i++ )
        {
            rocketnet_client_io_step( h->io, false );
            rocketnet_client_transport_update( h->transport, h->clock.now );
            rocketnet_client_io_step( h->io, false );
            h->clock.now += 0.01;
        }
        rocketnet_unit_check( rocketnet_client_transport_state( h->transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
        rocketnet_unit_check( h->pair.fake[0].num_sent > 0 && h->pair.fake[0].num_sent == h->pair.fake[1].num_sent );
        race_harness_teardown( h );
        for ( int family = 0; family < 2; family++ )
        {
            rocketnet_unit_check( h->pair.fake[family].destroy_calls == 1 );
            rocketnet_unit_check( h->pair.fake[family].close_calls == 0 );
        }
        delete h;
    }
}

REGISTER_UNIT_TEST( test_client_race_destroyed_in_flight_frees_both );

// In flight with the reading step's buffer full (one buffer published and
// not yet taken, the next filled to ROCKETNET_NUM_CLIENT_PACKETS): the step
// waits on the wake alone (FullWaitsOnWake), and during the race that wait
// is the primary family's, for the whole I/O guard, not the race's poll and
// not readable; the second family is not waited on.

static void test_client_race_full_buffer_waits_on_the_primary_wake_alone()
{
    for ( int batch = 0; batch < 2; batch++ )
    {
        race_harness_t * h = new race_harness_t();
        race_harness_setup( h, false, batch != 0 );
        rocketnet_fake_client_transport_io_t * primary = &h->pair.fake[0];
        const uint8_t junk[1] = { 0xEE };

        rocketnet_fake_client_push( primary, &h->ipv6_server, junk, 1 );
        rocketnet_client_io_step( h->io, false );
        for ( int round = 0; round < ROCKETNET_NUM_CLIENT_PACKETS / 256; round++ )
        {
            for ( int i = 0; i < 256; i++ )
            {
                rocketnet_fake_client_push( primary, &h->ipv6_server, junk, 1 );
            }
            rocketnet_client_io_step( h->io, false );
            rocketnet_unit_check( primary->inbound_count == 0 );
        }
        rocketnet_unit_check( h->io->fill_slots == ROCKETNET_NUM_CLIENT_PACKETS );

        const int waits = primary->wait_calls;
        rocketnet_client_io_step( h->io, true );
        rocketnet_unit_check( primary->wait_calls == waits + 1 );
        rocketnet_unit_check( !primary->last_wait_readable );
        rocketnet_unit_check( primary->last_wait_timeout == h->io->guard_timeout_seconds );
        rocketnet_unit_check( h->pair.fake[1].wait_calls == 0 );

        race_harness_teardown( h );
        delete h;
    }
}

REGISTER_UNIT_TEST( test_client_race_full_buffer_waits_on_the_primary_wake_alone );

#endif // #if ROCKETNET_DEV
