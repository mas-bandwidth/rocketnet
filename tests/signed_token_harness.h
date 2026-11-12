/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A server request-path harness for connect tokens built field by field and
// then signed, so a case can present a token that is wrong in exactly one
// way and still carries a signature that verifies (or, for the strict
// Ed25519 cases, one that is forged without any secret). The request carries
// a return cookie valid for its source address and salt, so it reaches every
// rung past the cookie gate. The sealed private block is sealed with a fixed
// ephemeral key (crypto_box_seal's construction, written out), so every token
// is the same bytes on every run and a case's outcome never depends on a
// random draw. Time is an argument: the token clock is
// ROCKETNET_TEST_CREATE_TS and the server clock starts at 1000.0. Nothing
// here opens a socket, sleeps or reads a clock.

#pragma once

#ifndef ROCKETNET_TESTS_SIGNED_TOKEN_HARNESS_H
#define ROCKETNET_TESTS_SIGNED_TOKEN_HARNESS_H

#include "test_unit_registry.h"

#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_server.h"
#include "rocketnet_ack.h"

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// the zero pads, from the layouts of t6.1 and t6.2

#define SIGNED_TOKEN_SIGNED_PAD_OFFSET ( ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES )
#define SIGNED_TOKEN_SIGNED_PAD_BYTES ( ROCKETNET_SIGNED_CONNECT_DATA_BYTES - SIGNED_TOKEN_SIGNED_PAD_OFFSET )
#define SIGNED_TOKEN_PRIVATE_PAD_OFFSET ( 3 * 8 + 2 * ROCKETNET_KEY_BYTES + ROCKETNET_USER_DATA_BYTES )
#define SIGNED_TOKEN_PRIVATE_PAD_BYTES ( ROCKETNET_PRIVATE_BLOCK_BYTES - SIGNED_TOKEN_PRIVATE_PAD_OFFSET )

struct signed_token_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    uint64_t cookie_reply_sources[64];
};

// one token, field by field, and the one thing wrong with it: a byte of
// either zero pad set (index -1 leaves the pad zero), the sealed block sealed
// to seal_key rather than to the key the signed data names

struct signed_token_spec_t
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    int signed_pad_index;
    uint8_t signed_pad_value;
    int private_pad_index;
    uint8_t private_pad_value;
    const uint8_t * seal_key;
};

static inline void signed_token_spec_default( struct signed_token_spec_t * spec, uint64_t client_id )
{
    memset( spec, 0, sizeof( *spec ) );

    struct rocketnet_signed_connect_data_t * s = &spec->signed_fields;
    s->version = ROCKETNET_WIRE_VERSION;
    s->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    s->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    s->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    s->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    s->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };     // 203.0.113.10:40000
    memcpy( s->ipv4_address, server_address, 6 );
    memcpy( s->server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t * p = &spec->private_fields;
    p->client_id = client_id;
    p->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    p->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( p->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( p->server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    spec->signed_pad_index = -1;
    spec->private_pad_index = -1;
    spec->seal_key = NULL;
}

// crypto_box_seal with a fixed ephemeral secret: epk = X25519 base * esk,
// nonce = BLAKE2b-24( epk || recipient ), output epk || crypto_box_easy

static inline void signed_token_seal( uint8_t * sealed, const uint8_t * block, const uint8_t * recipient )
{
    static const uint8_t ephemeral_secret[32] = {
        0x53, 0x45, 0x41, 0x4c, 0x45, 0x44, 0x2d, 0x45, 0x50, 0x48, 0x45, 0x4d, 0x45, 0x52, 0x41, 0x4c,
        0x2d, 0x46, 0x49, 0x58, 0x45, 0x44, 0x2d, 0x46, 0x4f, 0x52, 0x2d, 0x54, 0x45, 0x53, 0x54, 0x53
    };
    uint8_t ephemeral_public[32];
    rocketnet_unit_check( crypto_scalarmult_base( ephemeral_public, ephemeral_secret ) == 0 );

    uint8_t nonce_input[64];
    memcpy( nonce_input, ephemeral_public, 32 );
    memcpy( nonce_input + 32, recipient, 32 );
    uint8_t nonce[crypto_box_NONCEBYTES];
    rocketnet_unit_check( crypto_generichash( nonce, sizeof( nonce ), nonce_input, sizeof( nonce_input ), NULL, 0 ) == 0 );

    memcpy( sealed, ephemeral_public, 32 );
    rocketnet_unit_check( crypto_box_easy( sealed + 32, block, ROCKETNET_PRIVATE_BLOCK_BYTES, nonce, recipient, ephemeral_secret ) == 0 );
}

// the signed connect data the spec describes, and the backend's signature
// over it

static inline void signed_token_mint( const struct signed_token_spec_t * spec, uint8_t * signed_connect_data, uint8_t * signature )
{
    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &spec->private_fields, private_block );
    if ( spec->private_pad_index >= 0 )
    {
        private_block[SIGNED_TOKEN_PRIVATE_PAD_OFFSET + spec->private_pad_index] = spec->private_pad_value;
    }

    struct rocketnet_signed_connect_data_t fields = spec->signed_fields;
    signed_token_seal( fields.sealed_private_block, private_block,
                       spec->seal_key ? spec->seal_key : fields.server_public_key );

    rocketnet_write_signed_connect_data( &fields, signed_connect_data );
    if ( spec->signed_pad_index >= 0 )
    {
        signed_connect_data[SIGNED_TOKEN_SIGNED_PAD_OFFSET + spec->signed_pad_index] = spec->signed_pad_value;
    }

    rocketnet_unit_check( crypto_sign_detached( signature, NULL, signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                                                rocketnet_test_backend_secret_key ) == 0 );
}

static inline void signed_token_server_setup( struct signed_token_server_t * test, const uint8_t * backend_public_key )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 64 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = &test->counters;
    server->cookie_reply_sources = test->cookie_reply_sources;
    server->cookie_reply_source_words = 64;
    rocketnet_unit_check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );

    rocketnet_server_protocol_update( server, 1000.0 );
}

static inline void signed_token_server_teardown( struct signed_token_server_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
}

// a connection request around the signed data and signature, with a return
// cookie valid for this source address and the harness's salt

static inline void signed_token_request( uint8_t * request, const uint8_t * signed_connect_data, const uint8_t * signature,
                                         const struct rocketnet_address_t * address, const uint8_t * cookie_key )
{
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               signed_connect_data, signature );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    rocketnet_unit_check( rocketnet_generate_cookie_mac( cookie_mac, address, rocketnet_test_salt, cookie_expire, cookie_key ) );

    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
}

// the request through the server's ladder; a refused request is never
// answered

static inline int signed_token_send( struct signed_token_server_t * test, const struct rocketnet_address_t * address,
                                     const uint8_t * signed_connect_data, const uint8_t * signature )
{
    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    signed_token_request( request, signed_connect_data, signature, address, test->server.cookie_key );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    const int result = rocketnet_server_protocol_process_connection_request( &test->server, address,
                                                                             request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                             ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                             response, &response_bytes );
    if ( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
    {
        rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    }
    else
    {
        rocketnet_unit_check( response_bytes == 0 );
    }
    return result;
}

// one spec through a fresh server with the backend's real key, from one
// address: the result, with the counter the case expects read off as the
// only refusal counted

static inline void signed_token_expect_refused( const struct signed_token_spec_t * spec, int counter )
{
    struct signed_token_server_t test;
    signed_token_server_setup( &test, rocketnet_test_backend_public_key );

    uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    signed_token_mint( spec, signed_connect_data, signature );

    struct rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "198.51.100.7:56789" ) );

    const struct rocketnet_counters_t before = test.counters;
    rocketnet_unit_check( signed_token_send( &test, &address, signed_connect_data, signature ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( test.counters.values[counter] == before.values[counter] + 1 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE] == 0 );
    rocketnet_unit_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );

    signed_token_server_teardown( &test );
}

static inline void signed_token_expect_challenged( const struct signed_token_spec_t * spec )
{
    struct signed_token_server_t test;
    signed_token_server_setup( &test, rocketnet_test_backend_public_key );

    uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    signed_token_mint( spec, signed_connect_data, signature );

    struct rocketnet_address_t address;
    rocketnet_unit_check( rocketnet_address_parse( &address, "198.51.100.7:56789" ) );

    rocketnet_unit_check( signed_token_send( &test, &address, signed_connect_data, signature ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 1 );

    signed_token_server_teardown( &test );
}

#endif // #ifndef ROCKETNET_TESTS_SIGNED_TOKEN_HARNESS_H
