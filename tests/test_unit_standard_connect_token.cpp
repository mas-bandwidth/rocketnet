/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_connect_token.cpp -- the vector twins of RN-1 t6
// (standard/TRANSPORT.md, Connect token), regenerated through the
// implementation. The frozen token and its identifier are the sdk's shipped
// constants; the three negative vectors are minted with the published test
// keys and the fixed ephemeral X25519 secrets, so their bytes are stable and
// a conforming server must refuse each for the reason its vector names.

#include "test_unit_standard.h"

#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )

static const uint8_t rn_test_wrong_server_public_key[32] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
    0xdd, 0xee, 0xff, 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01
};

static void rn_test_fill_signed( struct rocketnet_signed_connect_data_t * sf )
{
    memset( sf, 0, sizeof( *sf ) );
    sf->version = ROCKETNET_WIRE_VERSION;
    sf->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    sf->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    sf->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    sf->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    sf->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    sf->ipv4_address[0] = 127;
    sf->ipv4_address[1] = 0;
    sf->ipv4_address[2] = 0;
    sf->ipv4_address[3] = 1;
    sf->ipv4_address[4] = 0x40;
    sf->ipv4_address[5] = 0x9c;
    memcpy( sf->server_public_key, rocketnet_test_server_public_key, 32 );
}

static void rn_test_fill_private( struct rocketnet_connect_token_private_t * pf )
{
    memset( pf, 0, sizeof( *pf ) );
    pf->client_id = ROCKETNET_TEST_CLIENT_ID;
    memcpy( pf->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( pf->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

#if ROCKETNET_DEV

static const rn_standard_vector_t standard_connect_token_table[] = {
    RN_STANDARD_VECTOR( "t6.frozen-token", "931a7c4e887766554433221100f9ac68000000002cfaac68000000000a00000001cb00710a409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801cb310fdd303d6aad8c7900dfc4252d94d2134f1e6e17320f4c5b1bd4f2879da3ce2f287720861887378edf01de9befc09f1bf01fd3df19e2a79e60859e896346eb60c08f686721d81946e781e1ae2c3716a465880a3a5f068ede0b039cf5281a97cd36717766b5bb13869838ecb333d9eeb1042bf7a7b8f62f9cbb020ddbda6cb5f44e4def43666642ff8e6f835a5f295dd8d7bf2268684b2a93ec17ac8fdc4c4f05e5b09927b6afac3ec0f00508e9d43316600c6b745d6dbb9c68f12d569a19573d460ba34e801d744d6db6a0a01bd35b4e08d5203bbbd44367ef89bcbb3827fd80295f2b9404cdd2979e3aa17513ff9f42b6706248ea762282028d4fe0cf71156554f26bf90cb2bfcfef438b5e68ad2501f9e3be24bc0b33651ec756f60f423f324d90323adc9f6c8f1762b02ba31c7e79626067fe7f109594a803d330502feb03f61e9297ec11aadebcd69bac0bfd05c1c5c144ae23bcd114c35322e797843e95ccb8bc9748fda08197ce6d5a10c83bcb0eb7ff471d738a742f68fa3eb77dfadd320a11a55420e3ccd03f15efc183a5dd35cf8657fa712b7c97496000e6a5468d800d49d1345ce185dc0f7bc7147a9000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000006b3692b49d36b7a276303340ab8399b98561ac6881fc7aa2bb60ffe07732ea7121fe5044ebb5f53dbe768b800c30d75370eb9beb4914ae133da64ea6b09e760d0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" ),
    RN_STANDARD_VECTOR( "t6.token-identifier", "f520da9d95fd072256b47d755f810f61" ),
    RN_STANDARD_VECTOR( "t6.bad-address-flags", "931a7c4e887766554433221100f9ac68000000002cfaac68000000000a000000057f000001409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801c392d174a38b3b1beafaf1fe824870841c5fa531bc6eafdb6402c124664488c1ca2b957f072747ef5ea9e73f1a1d1c2d1186072cd71305c22e1e48455146f1b4d5303ab2f8c0cf00862b27bb946f0e68d7110d0d02c7d32a8325457e1984828353d13d884013499648a55d51f5dad4c374d1bd6c240e77007e155113cbda35738507b96401aa610cf2f5c42e84a929390bbafc5da0ea875d2613b47d3a06cd0e0178deeb75fad63dd866c56a50915ff448c3f2ff14dc2dbb060e5c5fc88565a00c42ae4a657a3387e5db997a9f962275733d029a8f6f1f3bb09a1e3e6ad155fcd02e46942c06ac87597ee1e14eedfa4b64c431f63effcb24aef3e22ec05f2efb14a59bc5a80a12ecbd0d4c751bc21623537d5a5a68b983a8b694a9b9ded5ee7d68c90c20d0de3fb88455ebcc69527b652e8588c9098a3afc7615d659192fecc28290663e362fce1b3dff5f074bb04fae373d2a4b0145c1234a0087c4473122179682c1fa136c2f1c39f18b3487878ef0553dba3e901a6a1b358a73c8d34d2fde7ff83ad04a1c765695f9ab95fe82e3171e6a0d515dafdff12b0912a4cac7eec77141019834f3eb8af6a8d23bfba36794a00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000806f9f78d4609e475dae145d4d0cdc9bcc3f462cd883102dd1dd1c65e68b39f3fb89f8825047650fa50de6b070e0011a90a52f7cac9139a785f2b6b652fd7f070102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" ),
    RN_STANDARD_VECTOR( "t6.wrong-server-key", "931a7c4e887766554433221100f9ac68000000002cfaac68000000000a000000017f000001409c000000000000000000000000000000000000112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f00123b7bb8c91ae008711fb12846780bcdf1e065f821bdfec49f57e7c7dcd4c4823d210cb80cd26aed63b32c303f7a1c5c2351e3b77875ce2fa402077ece66829706fb8b49ced850c90cc84bce77d3e1cf75362ec49c0236a44bf637fbbcfaad7bbb1598363d28afb57c7d559f78edc84969546eb3f9e32388a4d3bf9a2bd23594c0b92afd5a95bf17b4eeec31440d358e6392052c3e2b0f2cc2dbea39fd148d78077579870ea94f3b5176ee62fc4ac79296bba3a67cda2c623572cf8bfec808b3c3e9ee443decf1431a7c6787c032fd91765d56342da1f0259b0313b73b55fbeb91f3aefc02d6f879131971176bf0337a9a80ea139a3b7922e0b80de8427a333842d3116085630784a39fab96ad23ec9276077be6fff3802854168efd36972893beaacc0864f4f785cfe0d8a0423f9c79eed62407a78d50b4121adff381ac90ce32c0b37f8ab3f9d068e023bbd89c01fd3f4cc968eb1d59c945dfa4f7dc0ee5d5caa4c399b3a4fcf8324c83eac3f78da50a1d88d47c24a43e10883063ee189f5cd3a17ea20c5f87abfae42a26c75d536d105900d91691596514ac8c1c16e4c927ca3560d59c3de9e860ee16fc36e4a402c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000f7cf8658f09f181696e599c0d209d2615c8b62edc5d6f16a69c759b3919a3780d2c7640ebd57b279aedffe7432cb63e43e2601b2c6a09c30069f321c22c3e30b0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" ),
    RN_STANDARD_VECTOR( "t6.sealed-mismatch", "931a7c4e887766554433221100f9ac68000000002cfaac68000000000a000000017f000001409c0000000000000000000000000000000000003d2490889d374c296fab1d5770cb71476a06f74ef2f2540cf2215f7614f4801c9fd7ad6dcff4298dd3f96d5b1b2af910a0535b1488d7f8fabb349a982880b615c9ae236b18ad611ebf16e23c9dcc80b20b7b3a4aad40f42063e2ea1612d14e23e63458b6045c44325a1f38a0d6d0f4d0e3745b28e49a872f30e8b92cd2ec71a7f17cbe71ff641e76202c266e24303bd015bef903f57ceac4dd4d2d8fd68a8016d4fc43a20bf45d5cf7774230589f987b20fa7464ce1cd0f552f1eba7a287f9b9f2e141d9b65068747978ba608923c0136f24c957a2867c04aefd7870c1cf539316ef9a6f1fec93ab6a9ecf6645de7291e75847945ee91cf4f53c36de56d7cbb5175330115a67142085c47d795002b62979fb54eb33f4564a345852b51feae84b07ae426d123ffd42802a44a41fb01f030a5ff31ac4df3fd82650a24364899abba5b30ff280705600b01cead1fbac12f68d13d4ef1948574242b7244e1842850b684391d8dd58e985199f03ecd5f1b228b62e6b384b18b06f9a171a39a664ef496a9f871193fa316debdadedaca4400fa4409c5be843079c0702b52eedd595e5837b89ce20a271974ebb384175f54dcb921eafb6465befffee03d07717e567e800b7b222a31b0117b1d4dfc8e6689143f000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000006199efa8e8670140c31b0e6ceab42981f77d8851690187b215699bb24deac5525fc15a4622f72e68f7af40cf2bd93efd5a4d79cafb1e9cd9f540fc5af3f003050102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" ),
};

// crypto_box_seal's construction with a caller-supplied ephemeral X25519
// secret, so the sealed block is stable: epk = X25519(esk, basepoint),
// nonce = BLAKE2b-192(epk || server_public_key), output epk || crypto_box_easy.
static int rn_test_seal_with_ephemeral( uint8_t * c, const uint8_t * m, unsigned long long mlen,
                                        const uint8_t * server_public_key, const uint8_t * esk )
{
    uint8_t epk[crypto_box_PUBLICKEYBYTES];
    uint8_t nonce[crypto_box_NONCEBYTES];
    if ( crypto_scalarmult_base( epk, esk ) != 0 )
        return -1;

    crypto_generichash_state state;
    if ( crypto_generichash_init( &state, NULL, 0, crypto_box_NONCEBYTES ) != 0 ||
         crypto_generichash_update( &state, epk, crypto_box_PUBLICKEYBYTES ) != 0 ||
         crypto_generichash_update( &state, server_public_key, crypto_box_PUBLICKEYBYTES ) != 0 ||
         crypto_generichash_final( &state, nonce, crypto_box_NONCEBYTES ) != 0 )
        return -1;
    if ( crypto_box_easy( c + crypto_box_PUBLICKEYBYTES, m, mlen, nonce, server_public_key, esk ) != 0 )
        return -1;
    memcpy( c, epk, crypto_box_PUBLICKEYBYTES );

    return 0;
}

static int standard_connect_token_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t6.frozen-token" ) == 0 )
    {
        if ( capacity < ROCKETNET_CONNECT_TOKEN_BYTES )
            return -1;
        memcpy( out, rocketnet_test_frozen_token, ROCKETNET_CONNECT_TOKEN_BYTES );
        return ROCKETNET_CONNECT_TOKEN_BYTES;
    }

    if ( strcmp( id, "t6.token-identifier" ) == 0 )
    {
        if ( capacity < ROCKETNET_TOKEN_IDENTIFIER_BYTES )
            return -1;
        rocketnet_connect_token_identifier( out, rocketnet_test_frozen_token );
        return ROCKETNET_TOKEN_IDENTIFIER_BYTES;
    }

    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;

    if ( strcmp( id, "t6.bad-address-flags" ) == 0 )
    {
        if ( capacity < ROCKETNET_CONNECT_TOKEN_BYTES )
            return -1;
        rn_test_fill_signed( &sf );
        sf.address_flags = 0x05;
        rn_test_fill_private( &pf );
        return rocketnet_write_connect_token_deterministic( &sf, &pf, rocketnet_test_backend_secret_key,
                                                            rocketnet_test_token_vector_ephemeral_secret[1], out )
                   ? ROCKETNET_CONNECT_TOKEN_BYTES
                   : -1;
    }

    if ( strcmp( id, "t6.wrong-server-key" ) == 0 )
    {
        if ( capacity < ROCKETNET_CONNECT_TOKEN_BYTES )
            return -1;
        rn_test_fill_signed( &sf );
        memcpy( sf.server_public_key, rn_test_wrong_server_public_key, 32 );
        rn_test_fill_private( &pf );
        return rocketnet_write_connect_token_deterministic( &sf, &pf, rocketnet_test_backend_secret_key,
                                                            rocketnet_test_token_vector_ephemeral_secret[2], out )
                   ? ROCKETNET_CONNECT_TOKEN_BYTES
                   : -1;
    }

    if ( strcmp( id, "t6.sealed-mismatch" ) == 0 )
    {
        if ( capacity < ROCKETNET_CONNECT_TOKEN_BYTES )
            return -1;

        // a private block whose protocol id disagrees with the signed one

        rn_test_fill_signed( &sf );
        rn_test_fill_private( &pf );
        pf.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
        pf.protocol_id = ROCKETNET_TEST_PROTOCOL_ID + 1;

        uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
        rocketnet_write_connect_token_private( &pf, private_block );
        if ( rn_test_seal_with_ephemeral( sf.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES,
                                          rocketnet_test_server_public_key, rocketnet_test_token_vector_ephemeral_secret[3] ) != 0 )
            return -1;

        uint8_t signed_buffer[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
        rocketnet_write_signed_connect_data( &sf, signed_buffer );

        uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
        crypto_sign_detached( signature, NULL, signed_buffer, sizeof( signed_buffer ), rocketnet_test_backend_secret_key );

        uint8_t * p = out;
        memcpy( p, signed_buffer, ROCKETNET_SIGNED_CONNECT_DATA_BYTES );
        p += ROCKETNET_SIGNED_CONNECT_DATA_BYTES;
        memcpy( p, signature, ROCKETNET_SIGNATURE_BYTES );
        p += ROCKETNET_SIGNATURE_BYTES;
        memcpy( p, pf.client_to_server_key, ROCKETNET_KEY_BYTES );
        p += ROCKETNET_KEY_BYTES;
        memcpy( p, pf.server_to_client_key, ROCKETNET_KEY_BYTES );
        p += ROCKETNET_KEY_BYTES;
        memset( p, 0, (size_t) ( ROCKETNET_CONNECT_TOKEN_BYTES - ( p - out ) ) );
        return ROCKETNET_CONNECT_TOKEN_BYTES;
    }

    return -1;
}

#endif // #if ROCKETNET_DEV

static void test_standard_connect_token_vectors()
{
#if ROCKETNET_DEV
    const int count = (int) ( sizeof( standard_connect_token_table ) / sizeof( standard_connect_token_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_connect_token_table, count, standard_connect_token_generate ) == count );
#endif // #if ROCKETNET_DEV

    // the frozen token's identifier is the t3 twin

    uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_connect_token_identifier( identifier, rocketnet_test_frozen_token );
    rocketnet_unit_check( memcmp( identifier, rocketnet_test_token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ) == 0 );

    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    struct rocketnet_connect_token_t parsed;
    struct rocketnet_connect_token_private_t opened;

    // production fresh-ephemeral token minting opens and verifies cleanly

    rn_test_fill_signed( &sf );
    rn_test_fill_private( &pf );
    rocketnet_unit_check( rocketnet_write_connect_token( &sf, &pf, rocketnet_test_backend_secret_key, token ) );

    rocketnet_read_connect_token( &parsed, token );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature, rocketnet_test_backend_public_key ) );
    rocketnet_unit_check( rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );
    rocketnet_unit_check( rocketnet_connect_token_private_consistent( &opened, parsed.signed_connect_data ) );

    // the three refusals: build each negative vector's token and assert the
    // server-side refusal that its vector names.

    // bad address flags: the flag byte carries a bit beyond IPv4 and IPv6

    rn_test_fill_signed( &sf );
    sf.address_flags = 0x05;
    rn_test_fill_private( &pf );
#if ROCKETNET_DEV
    rocketnet_unit_check( rocketnet_write_connect_token_deterministic( &sf, &pf, rocketnet_test_backend_secret_key,
                                                                       rocketnet_test_token_vector_ephemeral_secret[1], token ) );
#else
    rocketnet_unit_check( rocketnet_write_connect_token( &sf, &pf, rocketnet_test_backend_secret_key, token ) );
#endif // #if ROCKETNET_DEV
    rocketnet_read_connect_token( &parsed, token );
    rocketnet_unit_check( parsed.signed_connect_data[ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS] == 0x05 );
    rocketnet_unit_check( ( parsed.signed_connect_data[ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS] & ~( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) ) != 0 );

    // wrong server key: the block is sealed to another server public key, so
    // the server's own key cannot open it, even though the signature verifies

    rn_test_fill_signed( &sf );
    memcpy( sf.server_public_key, rn_test_wrong_server_public_key, 32 );
    rn_test_fill_private( &pf );
#if ROCKETNET_DEV
    rocketnet_unit_check( rocketnet_write_connect_token_deterministic( &sf, &pf, rocketnet_test_backend_secret_key,
                                                                       rocketnet_test_token_vector_ephemeral_secret[2], token ) );
#else
    rocketnet_unit_check( rocketnet_write_connect_token( &sf, &pf, rocketnet_test_backend_secret_key, token ) );
#endif // #if ROCKETNET_DEV
    rocketnet_read_connect_token( &parsed, token );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature, rocketnet_test_backend_public_key ) );
    rocketnet_unit_check( !rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );

    // sealed mismatch: the block opens but its inner protocol id disagrees
    // with the signed one

    rn_test_fill_signed( &sf );
    rn_test_fill_private( &pf );
    pf.protocol_id = ROCKETNET_TEST_PROTOCOL_ID + 1;

    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &pf, private_block );
#if ROCKETNET_DEV
    rocketnet_unit_check( rn_test_seal_with_ephemeral( sf.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES,
                                                       rocketnet_test_server_public_key, rocketnet_test_token_vector_ephemeral_secret[3] ) == 0 );
#else
    rocketnet_unit_check( crypto_box_seal( sf.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES,
                                           rocketnet_test_server_public_key ) == 0 );
#endif // #if ROCKETNET_DEV
    uint8_t signed_buffer[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    rocketnet_write_signed_connect_data( &sf, signed_buffer );

    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    crypto_sign_detached( signature, NULL, signed_buffer, sizeof( signed_buffer ), rocketnet_test_backend_secret_key );
    rocketnet_unit_check( rocketnet_verify_signed_connect_data( signed_buffer, signature, rocketnet_test_backend_public_key ) );
    rocketnet_unit_check( rocketnet_open_connect_token_private( &opened, signed_buffer, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) );
    rocketnet_unit_check( !rocketnet_connect_token_private_consistent( &opened, signed_buffer ) );
}

REGISTER_UNIT_TEST( test_standard_connect_token_vectors );

// The zero fields of the token (TRANSPORT.md t1 rule 3, t6.1 to t6.3): a
// receiver drops a structure whose zero[n] field is not all zero. The outer
// pad (bytes 704..767) is checked where the client admits the token, the
// signed pad (521..575) and the address bytes of an absent family before the
// server admits the request, and the private pad (344..383) right after the
// sealed block opens. Each field is poked at its first and last byte; the
// signed buffer is re-signed and the private block re-sealed so every refusal
// is the zero field's alone. Time is an argument and nothing here opens a
// socket.

#define RN_ZERO_OUTER_OFFSET        704
#define RN_ZERO_OUTER_BYTES         64
#define RN_ZERO_SIGNED_OFFSET       521
#define RN_ZERO_SIGNED_BYTES        55
#define RN_ZERO_PRIVATE_OFFSET      344
#define RN_ZERO_PRIVATE_BYTES       40

ROCKETNET_STATIC_ASSERT( RN_ZERO_OUTER_OFFSET + RN_ZERO_OUTER_BYTES == ROCKETNET_CONNECT_TOKEN_BYTES, "the outer pad ends the token" );
ROCKETNET_STATIC_ASSERT( RN_ZERO_SIGNED_OFFSET + RN_ZERO_SIGNED_BYTES == ROCKETNET_SIGNED_CONNECT_DATA_BYTES, "the signed pad ends the signed data" );
ROCKETNET_STATIC_ASSERT( RN_ZERO_PRIVATE_OFFSET + RN_ZERO_PRIVATE_BYTES == ROCKETNET_PRIVATE_BLOCK_BYTES, "the private pad ends the private block" );

// what the token carries: the family of its one address, and the first byte
// to set in each zero field (-1 leaves the field zero). absent_index is a
// byte of the absent family's address field.

struct rn_zero_token_t
{
    int ipv6;
    int outer_index;
    int signed_index;
    int private_index;
    int absent_index;
};

static void rn_zero_mint( uint8_t * token, const struct rn_zero_token_t * spec )
{
    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    rn_test_fill_signed( &sf );
    rn_test_fill_private( &pf );
    pf.protocol_id = sf.protocol_id;
    pf.expire_timestamp = sf.expire_timestamp;

    if ( spec->ipv6 )
    {
        // [2001:db8::1]:40000, the absent family being IPv4

        sf.address_flags = ROCKETNET_ADDRESS_FLAG_IPV6;
        memset( sf.ipv4_address, 0, sizeof( sf.ipv4_address ) );
        uint8_t * p = sf.ipv6_address;
        rocketnet_write_uint16( &p, 0x2001 );
        rocketnet_write_uint16( &p, 0x0db8 );
        for ( int i = 0; i < 5; i++ )
            rocketnet_write_uint16( &p, 0 );
        rocketnet_write_uint16( &p, 1 );
        rocketnet_write_uint16( &p, 40000 );
        if ( spec->absent_index >= 0 )
            sf.ipv4_address[spec->absent_index] = 0x01;
    }
    else
    {
        // 203.0.113.10:40000, the absent family being IPv6

        const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
        memcpy( sf.ipv4_address, server_address, sizeof( server_address ) );
        if ( spec->absent_index >= 0 )
            sf.ipv6_address[spec->absent_index] = 0x01;
    }

    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &pf, private_block );
    if ( spec->private_index >= 0 )
        private_block[RN_ZERO_PRIVATE_OFFSET + spec->private_index] = 0x01;
    rocketnet_unit_check( crypto_box_seal( sf.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES,
                                           rocketnet_test_server_public_key ) == 0 );

    uint8_t signed_buffer[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    rocketnet_write_signed_connect_data( &sf, signed_buffer );
    if ( spec->signed_index >= 0 )
        signed_buffer[RN_ZERO_SIGNED_OFFSET + spec->signed_index] = 0x01;

    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    crypto_sign_detached( signature, NULL, signed_buffer, sizeof( signed_buffer ), rocketnet_test_backend_secret_key );

    uint8_t * p = token;
    memcpy( p, signed_buffer, ROCKETNET_SIGNED_CONNECT_DATA_BYTES );
    p += ROCKETNET_SIGNED_CONNECT_DATA_BYTES;
    memcpy( p, signature, ROCKETNET_SIGNATURE_BYTES );
    p += ROCKETNET_SIGNATURE_BYTES;
    memcpy( p, pf.client_to_server_key, ROCKETNET_KEY_BYTES );
    p += ROCKETNET_KEY_BYTES;
    memcpy( p, pf.server_to_client_key, ROCKETNET_KEY_BYTES );
    p += ROCKETNET_KEY_BYTES;
    memset( p, 0, (size_t) ( ROCKETNET_CONNECT_TOKEN_BYTES - ( p - token ) ) );
    if ( spec->outer_index >= 0 )
        token[RN_ZERO_OUTER_OFFSET + spec->outer_index] = 0x01;
}

static const struct rn_zero_token_t rn_zero_none = { 0, -1, -1, -1, -1 };

static int rn_zero_client_admits( const uint8_t * token )
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    static struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    const int admitted = rocketnet_client_protocol_connect( &client, token, 1000.0, ROCKETNET_TEST_CREATE_TS );
    if ( admitted )
        return client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE;

    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = -1;
    rocketnet_client_protocol_update( &client, 1000.0, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );
    return 0;
}

#if ROCKETNET_DEV
extern void ( *rocketnet_token_private_block_hook )( uint8_t * block, int bytes, int at_exit );

static int rn_zero_exit_seen;
static int rn_zero_exit_clean;

static void rn_zero_block_seen( uint8_t * block, int bytes, int at_exit )
{
    if ( at_exit != 1 )
        return;
    rn_zero_exit_seen++;
    rn_zero_exit_clean = sodium_is_zero( block, (size_t) bytes );
}
#endif // #if ROCKETNET_DEV

// returns the process result, with the request ladder's counters in *counters

static int rn_zero_server_process( const uint8_t * token, struct rocketnet_counters_t * counters, int * response_bytes )
{
    static struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );
    memset( counters, 0, sizeof( *counters ) );
    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server.packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server.server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server.server_secret_key, rocketnet_test_server_secret_key, 32 );
    rocketnet_unit_check( rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &server.advertised_ipv6, "[2001:db8::1]:40000" ) );

    memcpy( server.cookie_key, rocketnet_test_cookie_key, 32 );
    server.cookie_lifetime_seconds = 3;
    server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server.handshake_deadline_seconds = 5.0;
    server.max_timeout_seconds = 0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    server.counters = counters;
    rocketnet_unit_check( server.encryption_map && server.used_token_history && server.client_slots );

    rocketnet_server_protocol_update( &server, 1000.0 );

    struct rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, token[ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS] == ROCKETNET_ADDRESS_FLAG_IPV6 ? "[2001:db8::7]:56789" : "198.51.100.7:56789" ) );

    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, token );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               parsed.signed_connect_data, parsed.signature );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    rocketnet_unit_check( rocketnet_generate_cookie_mac( cookie_mac, &client_address, rocketnet_test_salt, cookie_expire, server.cookie_key ) );

    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    *response_bytes = 0;
    const int result = rocketnet_server_protocol_process_connection_request( &server, &client_address,
                                                                             request, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
                                                                             ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                             response, response_bytes );

    const int mapped = rocketnet_encryption_map_find( server.encryption_map, &client_address ) != -1;
    rocketnet_unit_check( mapped == ( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE ) );

    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_client_slots_destroy( server.client_slots );

    return result;
}

static void rn_zero_server_refuses( const struct rn_zero_token_t * spec, int counter )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rn_zero_mint( token, spec );

    struct rocketnet_counters_t counters;
    int response_bytes = -1;
    rocketnet_unit_check( rn_zero_server_process( token, &counters, &response_bytes ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( response_bytes == 0 );
    rocketnet_unit_check( counters.values[counter] == 1 );
}

static void test_standard_connect_token_zero_fields()
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    // all-zero fields are accepted, at every layer

    rn_zero_mint( token, &rn_zero_none );
    rocketnet_unit_check( rn_zero_client_admits( token ) );
    {
        struct rocketnet_counters_t counters;
        int response_bytes = 0;
        rocketnet_unit_check( rn_zero_server_process( token, &counters, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
        rocketnet_unit_check( response_bytes == ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
    }
    {
        struct rn_zero_token_t v6 = rn_zero_none;
        v6.ipv6 = 1;
        rn_zero_mint( token, &v6 );
        struct rocketnet_counters_t counters;
        int response_bytes = 0;
        rocketnet_unit_check( rn_zero_server_process( token, &counters, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    }

    // the outer pad, first and last byte: the client refuses the token

    const int outer_indices[2] = { 0, RN_ZERO_OUTER_BYTES - 1 };
    for ( int i = 0; i < 2; i++ )
    {
        struct rn_zero_token_t spec = rn_zero_none;
        spec.outer_index = outer_indices[i];
        rn_zero_mint( token, &spec );
        rocketnet_unit_check( !rn_zero_client_admits( token ) );
    }

    // the signed pad, first and last byte: the server ignores the request

    const int signed_indices[2] = { 0, RN_ZERO_SIGNED_BYTES - 1 };
    for ( int i = 0; i < 2; i++ )
    {
        struct rn_zero_token_t spec = rn_zero_none;
        spec.signed_index = signed_indices[i];
        rn_zero_server_refuses( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS );
    }

    // the address bytes of the absent family, first and last byte, in each family

    const int absent_ipv6_indices[2] = { 0, 17 };
    const int absent_ipv4_indices[2] = { 0, 5 };
    for ( int i = 0; i < 2; i++ )
    {
        struct rn_zero_token_t spec = rn_zero_none;
        spec.absent_index = absent_ipv6_indices[i];
        rn_zero_server_refuses( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS );

        spec = rn_zero_none;
        spec.ipv6 = 1;
        spec.absent_index = absent_ipv4_indices[i];
        rn_zero_server_refuses( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS );
    }

    // the private pad, first and last byte: the sealed block opens but the
    // server refuses it as one that did not, and its staging is wiped

    const int private_indices[2] = { 0, RN_ZERO_PRIVATE_BYTES - 1 };
    for ( int i = 0; i < 2; i++ )
    {
        struct rn_zero_token_t spec = rn_zero_none;
        spec.private_index = private_indices[i];

        rn_zero_server_refuses( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK );

        rn_zero_mint( token, &spec );
        struct rocketnet_connect_token_t parsed;
        rocketnet_read_connect_token( &parsed, token );
        rocketnet_unit_check( rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature, rocketnet_test_backend_public_key ) );

        struct rocketnet_connect_token_private_t opened;
        memset( &opened, 0xaa, sizeof( opened ) );
        struct rocketnet_connect_token_private_t untouched = opened;
#if ROCKETNET_DEV
        rn_zero_exit_seen = 0;
        rn_zero_exit_clean = 0;
        rocketnet_token_private_block_hook = rn_zero_block_seen;
#endif // #if ROCKETNET_DEV
        const int opened_ok = rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key );
#if ROCKETNET_DEV
        rocketnet_token_private_block_hook = NULL;
        rocketnet_unit_check( rn_zero_exit_seen == 1 );
        rocketnet_unit_check( rn_zero_exit_clean );
#endif // #if ROCKETNET_DEV
        rocketnet_unit_check( !opened_ok );
        rocketnet_unit_check( memcmp( &opened, &untouched, sizeof( opened ) ) == 0 );
    }
}

REGISTER_UNIT_TEST( test_standard_connect_token_zero_fields );
