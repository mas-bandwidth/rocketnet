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
#include "rocketnet_test_vectors.h"

#include <string.h>

static const rn_standard_vector_t standard_key_schedule_table[] = {
    RN_STANDARD_VECTOR( "t7.handshake-client-to-server", "4b4d630cdb00c87b73d46b3421d891c24cfd0ab928c987515aabbbd78be27350" ),
    RN_STANDARD_VECTOR( "t7.handshake-server-to-client", "86045b5f8e2da94dbf445a658086d63d50b73b1e666c76c4952fabf998c0c0e0" ),
    RN_STANDARD_VECTOR( "t7.session-client-to-server", "aedc4656e3f82b7e8791279588c7f6204ed2c1bc0c17ba9779c04e3b1c9ef43c" ),
    RN_STANDARD_VECTOR( "t7.session-server-to-client", "3c637e37b101b82c9edaa9a3b55a31a8a3bc108f133ba05e8a416b3cfb730ef0" ),
    RN_STANDARD_VECTOR( "t7.small-order-key-refused", "00" ),
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

static int standard_key_schedule_generate( const char * id, uint8_t * out, int capacity )
{
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
