/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_hydrogen.h"
#include "rocketnet_base64.h"

#include <stdio.h>

int main()
{
    hydro_sign_keypair keypair;
    hydro_sign_keygen( &keypair );

    char public_key_string[256];
    char private_key_string[256];
    rocketnet_base64_encode_data( keypair.pk, hydro_sign_PUBLICKEYBYTES, public_key_string, sizeof(public_key_string) );
    rocketnet_base64_encode_data( keypair.sk, hydro_sign_SECRETKEYBYTES, private_key_string, sizeof(private_key_string) );

    printf( "\nbuyer public key base64 = %s\n\n", public_key_string );
    printf( "buyer private key base64 = %s\n\n", private_key_string );

    printf( "const uint8_t buyer_public_key[] = { " );
    for ( int i = 0; i < (int) hydro_sign_PUBLICKEYBYTES; i++ )
    {
        printf( "0x%02x", keypair.pk[i] );
        if ( i != hydro_sign_PUBLICKEYBYTES - 1 )
        {
            printf( ", " );
        }
        else
        {
            printf( " };\n\n" );
        }
    }

    printf( "const uint8_t buyer_private_key[] = { " );
    for ( int i = 0; i < (int) hydro_sign_SECRETKEYBYTES; i++ )
    {
        printf( "0x%02x", keypair.sk[i] );
        if ( i != hydro_sign_SECRETKEYBYTES - 1 )
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
