/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_skeleton.cpp -- the vector twins of RN-1 t3
// (standard/TRANSPORT.md, Constants and registries), regenerated through the
// implementation. The one vector is the token identifier of the frozen
// connect token: crypto_generichash, 16 bytes, no key, over the token's 576
// signed connect data bytes, which begin at offset 0 of the token.

#include "test_unit_standard.h"

#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"

static const rn_standard_vector_t standard_skeleton_table[] = {
    RN_STANDARD_VECTOR( "t3.token-identifier", "f520da9d95fd072256b47d755f810f61" ),
};

static int standard_skeleton_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t3.token-identifier" ) == 0 )
    {
        if ( capacity < ROCKETNET_TOKEN_IDENTIFIER_BYTES )
            return -1;
        rocketnet_connect_token_identifier( out, rocketnet_test_frozen_token );
        return ROCKETNET_TOKEN_IDENTIFIER_BYTES;
    }

    return -1;
}

static void test_standard_skeleton_vectors()
{
    const int count = (int) ( sizeof( standard_skeleton_table ) / sizeof( standard_skeleton_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_skeleton_table, count, standard_skeleton_generate ) == 1 );

    // the twin is the frozen identifier the sdk suite already holds

    uint8_t twin[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_unit_check( rn_standard_hex_decode( standard_skeleton_table[0].hex, twin, (int) sizeof( twin ) ) == ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    rocketnet_unit_check( memcmp( twin, rocketnet_test_token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 );

    // the walk's own refusals: a differing byte is found and named, a short
    // vector is found at its end, and bad hex is refused

    const uint8_t a[3] = { 1, 2, 3 };
    const uint8_t b[3] = { 1, 9, 3 };
    rocketnet_unit_check( rn_standard_first_difference( a, 3, a, 3 ) == -1 );
    rocketnet_unit_check( rn_standard_first_difference( a, 3, b, 3 ) == 1 );
    rocketnet_unit_check( rn_standard_first_difference( a, 3, a, 2 ) == 2 );

    uint8_t scratch[2];
    rocketnet_unit_check( rn_standard_hex_decode( "0g", scratch, 2 ) == -1 );
    rocketnet_unit_check( rn_standard_hex_decode( "012", scratch, 2 ) == -1 );
    rocketnet_unit_check( rn_standard_hex_decode( "010203", scratch, 2 ) == -1 );
    rocketnet_unit_check( rn_standard_hex_decode( "0a 0b", scratch, 2 ) == 2 && scratch[0] == 0x0a && scratch[1] == 0x0b );
    rocketnet_unit_check( standard_skeleton_generate( "t3.no-such-vector", scratch, 2 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_skeleton_vectors );
