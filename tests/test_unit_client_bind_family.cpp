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
#include "rocketnet_client_protocol.h"
#include "rocketnet_wire.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

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
// the client selects the IPv4 address, whose bind family is IPv4; on a host
// with IPv6 it selects the IPv6 address. A token naming only one family
// selects that family whatever the host, and an IPv6-only token on a host
// with no IPv6 has no bind family (the create refuses it).
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

    // both families named, a host with IPv6: the IPv6 address
    rocketnet_client_protocol_pick_server_address( &token, true, &picked );
    rocketnet_unit_check( picked.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( rocketnet_address_equal( &picked, &ipv6_expected ) );

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

// The family race: when a connect token names both an IPv4 and an IPv6 server
// address and the host can use both, the client sends its opening handshake
// (cookie request) to both addresses and commits to whichever family answers
// first with a valid cookie; the connection request then goes only to that
// family, and a late answer from the other family is dropped and counted.
// This test drives the protocol state machine in memory -- no sockets, no
// real time: the client's clock starts at 1000.0 -- with a token naming both
// families, nothing answering on IPv6, and the server answering on IPv4. It
// must reach CONNECTED over IPv4 before the token's lifetime expires, and it
// must never send a connection request on IPv6.

static void bind_family_mint_token( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = 10;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;

    const uint8_t ipv4_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, ipv4_address, 6 );

    const uint8_t ipv6_address[18] = {
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01,
        0x40, 0x9c                                                    // 2001:db8::1:40000
    };
    memcpy( signed_fields.ipv6_address, ipv6_address, 18 );

    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = 7;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// a valid cookie echoing this attempt's salt, exactly as the server would send
// it on either family

static int bind_family_feed_cookie( struct rocketnet_client_protocol_t * client, double current_time )
{
    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    const uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES] = { 0 };
    rocketnet_write_connection_cookie_packet( cookie, client->packet_filter, client->connection_salt,
                                              0, cookie_mac );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    return rocketnet_client_protocol_process_packet( client, cookie, sizeof(cookie),
                                                     current_time, payload, sizeof(payload), &payload_bytes );
}

// a valid challenge the client will accept, mirroring the server's construction

static int bind_family_feed_challenge( struct rocketnet_client_protocol_t * client, double current_time )
{
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memset( session_nonce, 0x5A, sizeof(session_nonce) );

    struct rocketnet_connection_keys_t keys;
    rocketnet_derive_handshake_keys( &keys, client->token.client_to_server_key, client->token.server_to_client_key,
                                     client->connection_salt, session_nonce );

    uint8_t challenge[ROCKETNET_RECEIVE_CEILING];
    const int ch_bytes = rocketnet_write_sealed_packet( challenge, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
                                                        client->packet_filter, 1, session_nonce,
                                                        rocketnet_test_server_eph_public, ROCKETNET_KEY_BYTES,
                                                        client->signed_fields.protocol_id, client->connection_salt,
                                                        client->client_ephemeral_public_key,
                                                        keys.handshake_server_to_client );
    rocketnet_unit_check( ch_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    memset( &keys, 0, sizeof(keys) );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    return rocketnet_client_protocol_process_packet( client, challenge, ch_bytes,
                                                     current_time, payload, sizeof(payload), &payload_bytes );
}

// the first keep-alive, sealed under the client's own derived server-to-client
// session key, which carries the client index and slot count that confirm the
// connection

static int bind_family_feed_keep_alive( struct rocketnet_client_protocol_t * client, double current_time )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 8];
    memset( plaintext, 0, sizeof(plaintext) );
    uint8_t * p = plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES;
    rocketnet_write_uint32( &p, 0 );
    rocketnet_write_uint32( &p, 4 );

    uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
    const int bytes = rocketnet_write_sealed_packet( keep_alive, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                     client->packet_filter, 1, NULL,
                                                     plaintext, (int) sizeof(plaintext),
                                                     client->signed_fields.protocol_id, client->connection_salt,
                                                     NULL, client->keys.session_server_to_client );
    rocketnet_unit_check( bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    return rocketnet_client_protocol_process_packet( client, keep_alive, bytes,
                                                     current_time, payload, sizeof(payload), &payload_bytes );
}

static void test_client_connect_keeps_the_family_that_answers()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    bind_family_mint_token( token_data );

    // the client clock starts at 1000.0, the token's lifetime expires well
    // after the handshake completes

    const double t0 = 1000.0;
    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, token_data, t0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    // both families are in flight: the second address is the IPv4 server (the
    // primary is IPv6 per the preference) and the race is active

    struct rocketnet_address_t second;
    rocketnet_unit_check( rocketnet_client_protocol_second_address( &client, &second ) == 1 );
    rocketnet_unit_check( second.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( client.server_address.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( rocketnet_client_protocol_race_active( &client ) == 1 );

    // the first update sends the cookie request (the transport sends it to
    // both families while the race is active)

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, t0, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes > 0 );

    // nothing answers on IPv6; the server answers on IPv4. the first valid
    // cookie commits the attempt to the family that answered: the race turns
    // off, so the connection request that follows goes only to the committed
    // family and never to IPv6

    rocketnet_unit_check( bind_family_feed_cookie( &client, t0 + 0.01 ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    rocketnet_unit_check( rocketnet_client_protocol_race_active( &client ) == 0 );

    // a late cookie from the other family is dropped and counted, and does not
    // re-open the race

    const uint64_t late_drops = counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE];
    rocketnet_unit_check( bind_family_feed_cookie( &client, t0 + 0.02 ) == 0 );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE] == late_drops + 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    rocketnet_unit_check( rocketnet_client_protocol_race_active( &client ) == 0 );

    // the connection request is the only thing the committed attempt sends

    rocketnet_client_protocol_update( &client, t0 + 0.11, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes > 0 );

    // the server answers the request on the committed family; the handshake
    // completes to CONNECTED before the token's lifetime elapses

    rocketnet_unit_check( bind_family_feed_challenge( &client, t0 + 0.12 ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    rocketnet_unit_check( bind_family_feed_keep_alive( &client, t0 + 0.13 ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // token lifetime is expire - create; well beyond the handshake's 0.13s

    const uint64_t lifetime = ROCKETNET_TEST_EXPIRE_TS - ROCKETNET_TEST_CREATE_TS;
    rocketnet_unit_check( (uint64_t) ( t0 + 0.13 - t0 ) < lifetime );
}

REGISTER_UNIT_TEST( test_client_connect_keeps_the_family_that_answers );
