/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    backend_check_token: the C library's own token checks, run on one token
    a backend minted. Reads the base64 token from stdin and processes it as a
    server does, in the standard's order: decode, verify the signature over
    the signed connect data against the supplied backend public key (or the
    development public key when omitted), open
    the sealed private block with the development server keypair, and check
    the inner protocol id and expire agree with the signed ones. Then checks
    the fields against the expected values on the command line:

        backend_check_token <client id> <protocol id> < token.txt

    The backend must be run with ROCKETNET_BACKEND_SERVER_PUBLIC_KEY set to
    the development server public key for its tokens to open here. Supply the
    matching backend public key as the optional third argument when minting
    with a generated signing key. tools/run_backend_examples.py runs it on a
    token from each backend. This is a functional test of the backends:
    it uses the library's internal headers, which a game never includes.
*/

#include "rocketnet.h"
#include "rocketnet_base64.h"
#include "rocketnet_connect_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); return 1; } } while ( 0 )

int main( int argc, char ** argv )
{
    if ( argc != 3 && argc != 4 )
    {
        printf( "usage: backend_check_token <client id> <protocol id> [backend public key base64] < token.txt\n" );
        return 1;
    }
    const uint64_t expected_client_id = strtoull( argv[1], NULL, 0 );
    const uint64_t expected_protocol_id = strtoull( argv[2], NULL, 0 );

    char text[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES + 2];
    CHECK( fgets( text, sizeof( text ), stdin ) != NULL );
    text[strcspn( text, "\r\n" )] = '\0';
    CHECK( strlen( text ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK( rocketnet_base64_decode_data( text, token, sizeof( token ) ) == ROCKETNET_CONNECT_TOKEN_BYTES );

    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    memcpy( backend_public_key, rocketnet_dev_backend_public_key, sizeof( backend_public_key ) );
    if ( argc == 4 )
        CHECK( rocketnet_base64_decode_data( argv[3], backend_public_key, sizeof( backend_public_key ) ) == ROCKETNET_BACKEND_PUBLIC_KEY_BYTES );

    struct rocketnet_connect_token_t parts;
    rocketnet_read_connect_token( &parts, token );
    CHECK( rocketnet_verify_signed_connect_data( parts.signed_connect_data, parts.signature, backend_public_key ) );

    struct rocketnet_signed_connect_data_t signed_fields;
    rocketnet_read_signed_connect_data( &signed_fields, parts.signed_connect_data );
    CHECK( signed_fields.version == ROCKETNET_WIRE_VERSION );
    CHECK( signed_fields.protocol_id == expected_protocol_id );
    CHECK( signed_fields.expire_timestamp > signed_fields.create_timestamp );
    CHECK( signed_fields.timeout_seconds > 0 );
    CHECK( memcmp( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES ) == 0 );

    struct rocketnet_connect_token_private_t private_fields;
    CHECK( rocketnet_open_connect_token_private( &private_fields, parts.signed_connect_data, rocketnet_dev_server_public_key, rocketnet_dev_server_secret_key ) );
    CHECK( rocketnet_connect_token_private_consistent( &private_fields, parts.signed_connect_data ) );
    CHECK( private_fields.client_id == expected_client_id );
    CHECK( memcmp( private_fields.client_to_server_key, parts.client_to_server_key, ROCKETNET_KEY_BYTES ) == 0 );
    CHECK( memcmp( private_fields.server_to_client_key, parts.server_to_client_key, ROCKETNET_KEY_BYTES ) == 0 );

    // the public client's pre-send checks take it too
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "portable";
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL );
    CHECK( client != NULL );
    const enum rocketnet_status_t connect_status = rocketnet_client_connect( client, text );
    rocketnet_client_disconnect( client );
    const double shutdown_deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_client_disconnected( client ) && rocketnet_time() < shutdown_deadline )
    {
        // A generous failure watchdog, not a performance threshold. The
        // portable client normally owns an asynchronous worker, while update
        // also completes an explicitly step-driven client.
        rocketnet_client_update( client, 1000.0 );
        rocketnet_sleep( 0.001 );
    }
    if ( !rocketnet_client_disconnected( client ) )
    {
        printf( "FAIL: client shutdown did not complete\n" );
        return 1;
    }
    rocketnet_client_destroy( client );
    const enum rocketnet_status_t term_status = rocketnet_term();
    CHECK( connect_status == ROCKETNET_STATUS_OK );
    CHECK( term_status == ROCKETNET_STATUS_OK );

    printf( "token for client %llx, protocol id 0x%llx, address flags %d, expires %llu s after create, timeout %d s\n",
            (unsigned long long) private_fields.client_id, (unsigned long long) signed_fields.protocol_id, signed_fields.address_flags,
            (unsigned long long) ( signed_fields.expire_timestamp - signed_fields.create_timestamp ), signed_fields.timeout_seconds );
    printf( "PASS\n" );
    return 0;
}
