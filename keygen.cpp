/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_base64.h"
#include "rocketnet_connect_token.h"     /* rocketnet_generate_backend_keypair: internal, an SDK tool */

#include <sodium.h>

#include <stdio.h>

int main()
{
    if ( sodium_init() < 0 )
    {
        fprintf( stderr, "error: failed to initialize sodium\n" );
        return 1;
    }

    uint8_t public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t private_key[crypto_sign_SECRETKEYBYTES];     /* the Ed25519 signing secret, 64 bytes */
    if ( rocketnet_generate_backend_keypair( public_key, private_key ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "error: failed to generate backend keypair\n" );
        return 1;
    }

    char public_key_string[256];
    char private_key_string[256];
    rocketnet_base64_encode_data( public_key, sizeof( public_key ), public_key_string, sizeof( public_key_string ) );
    rocketnet_base64_encode_data( private_key, sizeof( private_key ), private_key_string, sizeof( private_key_string ) );

    printf( "\nbackend public key base64 = %s\n\n", public_key_string );
    printf( "backend private key base64 = %s\n\n", private_key_string );

    printf( "const uint8_t backend_public_key[] = { " );

    for ( int i = 0; i < (int) sizeof( public_key ); i++ )
    {
        printf( "0x%02x", public_key[i] );
        if ( i != (int) sizeof( public_key ) - 1 )
        {
            printf( ", " );
        }
        else
        {
            printf( " };\n\n" );
        }
    }

    printf( "const uint8_t backend_private_key[] = { " );

    for ( int i = 0; i < (int) sizeof( private_key ); i++ )
    {
        printf( "0x%02x", private_key[i] );
        if ( i != (int) sizeof( private_key ) - 1 )
        {
            printf( ", " );
        }
        else
        {
            printf( " };\n\n" );
        }
    }

    return 0;
}
