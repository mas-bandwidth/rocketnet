/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_key_schedule.cpp -- the vector twins of RN-1 t7
// (standard/TRANSPORT.md, Key schedule), regenerated through the
// implementation. The four key vectors come from the frozen token keys, salt,
// session nonce and ephemeral keypair; the last is the one-byte result of a
// derivation whose remote key is the small-order point, which must refuse it.

#include "test_unit_standard.h"

#include "rocketnet_keys.h"
#include "rocketnet_connect_token.h"

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif
#include "rocketnet_test_vectors.h"

#include <string.h>

static const rn_standard_vector_t standard_key_schedule_table[] = {
    RN_STANDARD_VECTOR( "t7.handshake-client-to-server", "4b4d630cdb00c87b73d46b3421d891c24cfd0ab928c987515aabbbd78be27350" ),
    RN_STANDARD_VECTOR( "t7.handshake-server-to-client", "86045b5f8e2da94dbf445a658086d63d50b73b1e666c76c4952fabf998c0c0e0" ),
    RN_STANDARD_VECTOR( "t7.session-client-to-server", "aedc4656e3f82b7e8791279588c7f6204ed2c1bc0c17ba9779c04e3b1c9ef43c" ),
    RN_STANDARD_VECTOR( "t7.session-server-to-client", "3c637e37b101b82c9edaa9a3b55a31a8a3bc108f133ba05e8a416b3cfb730ef0" ),
    RN_STANDARD_VECTOR( "t7.small-order-key-refused", "00" ),
    RN_STANDARD_VECTOR( "t7.signature-verification", "0101000001" ),
};

static int standard_key_schedule_derive( const uint8_t * remote_ephemeral_public_key, struct rocketnet_connection_keys_t * keys )
{
    return rocketnet_derive_connection_keys( keys,
                                             rocketnet_test_token_c2s_key,
                                             rocketnet_test_token_s2c_key,
                                             rocketnet_test_salt,
                                             rocketnet_test_session_nonce,
                                             rocketnet_test_client_eph_secret,
                                             remote_ephemeral_public_key,
                                             rocketnet_test_client_eph_public,
                                             rocketnet_test_server_eph_public );
}

// Public synthetic fixture. The first signature uses seed bytes 0..31 and
// deterministic Ed25519 signing. The second uses the same signing scalar
// with r = SHA-512("public alternative nonce fixture") reduced modulo L.
// This exercises verification, canonical-S refusal and content-based identity.
static int standard_key_schedule_verify_signatures( uint8_t * out, int capacity )
{
    if ( capacity < 5 )
        return -1;

    uint8_t public_key[32], signatures[3][64];
    rocketnet_unit_check( rn_standard_hex_decode( "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8", public_key, 32 ) == 32 );
    rocketnet_unit_check( rn_standard_hex_decode( "5ebb1841a19d7dc4f15bfc4d1129ca25e0b89592e7707191046dc3b9c0d55468febcf49e12a5cb474f05c82607820f87591ca54e13d9d6bbeb288beccdd1e404", signatures[0], 64 ) == 64 );
    rocketnet_unit_check( rn_standard_hex_decode( "c1feb7cd2979a67e446dce907b856166f65c876623531d5e51ae737beea27912dba084da7f6ff698e0001e75f846b23a7c54eb9a84fdaa3213477d318cd6a505", signatures[1], 64 ) == 64 );
    rocketnet_unit_check( rn_standard_hex_decode( "5ebb1841a19d7dc4f15bfc4d1129ca25e0b89592e7707191046dc3b9c0d55468eb90eafb2c08de9f25a2bfc9e57bee9b591ca54e13d9d6bbeb288beccdd1e414", signatures[2], 64 ) == 64 );

    uint8_t token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES + 64];
    for ( int i = 0; i < ROCKETNET_SIGNED_CONNECT_DATA_BYTES; i++ )
        token[i] = (uint8_t) i;
    out[0] = (uint8_t) ( crypto_sign_verify_detached( signatures[0], token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, public_key ) == 0 );
    out[1] = (uint8_t) ( crypto_sign_verify_detached( signatures[1], token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, public_key ) == 0 );
    rocketnet_unit_check( memcmp( signatures[0], signatures[1], 64 ) != 0 );
    token[0] ^= 1;
    out[2] = (uint8_t) ( crypto_sign_verify_detached( signatures[0], token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, public_key ) == 0 );
    token[0] ^= 1;
    out[3] = (uint8_t) ( crypto_sign_verify_detached( signatures[2], token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, public_key ) == 0 );

    uint8_t identifiers[2][ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < 2; i++ )
    {
        memcpy( token + ROCKETNET_SIGNED_CONNECT_DATA_BYTES, signatures[i], 64 );
        rocketnet_connect_token_identifier( identifiers[i], token );
    }
    out[4] = (uint8_t) ( memcmp( identifiers[0], identifiers[1], ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 );
    return 5;
}

static int standard_key_schedule_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t7.signature-verification" ) == 0 )
        return standard_key_schedule_verify_signatures( out, capacity );

    if ( strcmp( id, "t7.small-order-key-refused" ) == 0 )
    {
        if ( capacity < 1 )
            return -1;

        const uint8_t zero_point[32] = { 0 };
        struct rocketnet_connection_keys_t keys;
        memset( &keys, 0, sizeof( keys ) );
        out[0] = (uint8_t) standard_key_schedule_derive( zero_point, &keys );
        return 1;
    }

    enum
    {
        HANDSHAKE_C2S,
        HANDSHAKE_S2C,
        SESSION_C2S,
        SESSION_S2C
    } which;
    if ( strcmp( id, "t7.handshake-client-to-server" ) == 0 )
        which = HANDSHAKE_C2S;
    else if ( strcmp( id, "t7.handshake-server-to-client" ) == 0 )
        which = HANDSHAKE_S2C;
    else if ( strcmp( id, "t7.session-client-to-server" ) == 0 )
        which = SESSION_C2S;
    else if ( strcmp( id, "t7.session-server-to-client" ) == 0 )
        which = SESSION_S2C;
    else
        return -1;

    if ( capacity < 32 )
        return -1;

    struct rocketnet_connection_keys_t keys;
    memset( &keys, 0, sizeof( keys ) );
    if ( !standard_key_schedule_derive( rocketnet_test_server_eph_public, &keys ) )
        return -1;

    if ( which == HANDSHAKE_C2S )
        memcpy( out, keys.handshake_client_to_server, 32 );
    else if ( which == HANDSHAKE_S2C )
        memcpy( out, keys.handshake_server_to_client, 32 );
    else if ( which == SESSION_C2S )
        memcpy( out, keys.session_client_to_server, 32 );
    else
        memcpy( out, keys.session_server_to_client, 32 );

    return 32;
}

static void test_standard_key_schedule_vectors()
{
    const int count = (int) ( sizeof( standard_key_schedule_table ) / sizeof( standard_key_schedule_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_key_schedule_table, count, standard_key_schedule_generate ) == count );
}

REGISTER_UNIT_TEST( test_standard_key_schedule_vectors );
