/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// standard/TRANSPORT.md t7 rule 9: a verifier of a connect token MUST use
// strict Ed25519 verification, and MUST reject an R component or public key
// that is of small order. These cases pin the two strict checks that make
// that true (sodium/sodium.c, _crypto_sign_ed25519_verify_detached: the
// ge25519_has_small_order test of the signature's R and of the public key),
// with vectors on which the cofactorless verification equation
// encode( [S]B - [k]A ) == R holds byte for byte. On such a vector the
// encoding check is the only thing that refuses: remove it and the vector
// verifies. A vector on which the equation fails would be refused with or
// without the check and pins nothing.
//
// The small-order encodings are libsodium's list (ge25519_has_small_order in
// sodium/sodium.c; upstream src/libsodium/crypto_core/ed25519/ref10/
// ed25519_ref10.c), which compares bytes 0..30 and byte 31 with its sign bit
// cleared, so each entry stands for itself and for its sign-bit twin: the
// eight points of the curve's 8-torsion in their canonical encodings, and the
// non-canonical encodings of the same points. The construction of the forged
// vectors is the one "Taming the many EdDSAs" (Chalkias, Garillot and
// Nikolaenko, SSR 2020, and its ed25519-speccheck vectors) uses for its
// small-order cases.
//
// How the vectors were made (a generator over this repository's own
// sodium/sodium.c, kept with the review record): the signed message is the
// 576-byte buffer m[i] = i with m[0] replaced by a counter, the one free
// byte. B is the base point, T8 the order-8 point 26e8..05.
//  - R of small order: the public key is the mixed-order point A = B + T8
//    (canonical, not of small order, decodes). With S = k = SHA-512( R || A
//    || m ) mod L, [S]B - [k]A = -[k]T8, a torsion point; the counter is the
//    first that makes it equal R. A non-canonical R can never equal the
//    canonical encoding the verifier computes, so those rows are refused by
//    the byte compare as well (equation_holds 0); they are kept so every
//    encoding on the list is presented.
//  - public key of small order: R = B, S = 1. [1]B - [k]A = B exactly when
//    [k]A is the identity; the counter is the first that makes it so.
// The controls are correct strict signatures, built the same way without
// secrets (S = k + 1, R = B), and must verify: they show the vectors are
// refused for their encoding and not because the harness is wrong.
//
// The server case is the forgery this check exists to stop: a server whose
// backend public key is a small-order point admits tokens nobody signed
// when this check is gone.
// rocketnet_server_transport_create refuses only the all-zero backend key, so
// for a server configured with any other small-order key this check is the
// only protection. Unit cases: no socket, no clock, no sleep.

#include "signed_token_harness.h"
#include "test_unit_standard.h"

#define check rocketnet_unit_check

static const uint8_t strict_base_point[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
};

// A = B + T8: a mixed-order key, canonical and not of small order

static const char strict_mixed_key_hex[] = "da99e28ba529cdde35a25fba9059e78ecaee239f99755b9b1aa4f65df00803e2";

struct strict_r_vector_t
{
    const char * name;
    int counter;
    int equation_holds;
    const char * signature_hex;     // R || S
};

static const strict_r_vector_t strict_r_vectors[] = {
    { "identity (order 1), canonical", 13, 1, "0100000000000000000000000000000000000000000000000000000000000000d03ce72b6daec669fed899f24880cde0cc16638ba368b31c1d90fdf0fc8e680d" },
    { "identity, x = 0 with the sign bit set", 0, 0, "0100000000000000000000000000000000000000000000000000000000000080088b4930834568213104d1c5ec4c975dc9217a668c0131431106dc4880c15c03" },
    { "identity as y = p + 1, non-canonical", 0, 0, "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f79cbb651d502713d3cb88ef5fdda1aabb60fb883355d699e4e3e4f2bf61f310a" },
    { "identity as y = p + 1 with the sign bit, non-canonical", 0, 0, "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff3e4633d2d7bee8e0ead753d0f370ffbcb5f75d0a55576de060dafc4888a3c105" },
    { "y = -1 (order 2), canonical", 9, 1, "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7fc4bd1d83b7a9923fe026817ff1783068907ab89b42c4875f07f29cbd1bb83605" },
    { "y = -1, x = 0 with the sign bit set", 0, 0, "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff8c42820936a6a0d5b2035831b78ec3c365fb1452bdd7d57594dddb8866aff506" },
    { "y = 0, x positive (order 4), canonical", 5, 1, "0000000000000000000000000000000000000000000000000000000000000000c6e7bb47d1a86ac484aca9f9dfa4faad88f4a5908fe6b4abe8ff4e3a0f93b809" },
    { "y = 0, x negative (order 4), canonical", 10, 1, "00000000000000000000000000000000000000000000000000000000000000804a32f64c4af8514809e92ebfd4d84d4794450e32feaa7800ad7bb4b81b15f90b" },
    { "y = 0 as y = p (order 4), non-canonical", 0, 0, "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f10233666302a1458746b3e2271a7a3d7bf65469a37f47de841e0cc502dfe4a0d" },
    { "y = 0 as y = p with the sign bit (order 4), non-canonical", 0, 0, "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff8c71cda9a199f7502c1390f95e8d425c83dad531586e58218c1f270d95a2ce02" },
    { "order 8, 26e8..05, canonical", 11, 1, "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05a7c03b373531b0d37bf7fcbe4eb97b93c9be54d9094a36e698c6353d95b62b0c" },
    { "order 8, 26e8..85, canonical", 20, 1, "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85714e2da9141590fa2ca4e0e086ddace73db841c72c34d3212b49672db88f9500" },
    { "order 8, c717..7a, canonical", 20, 1, "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a2d92d03bfc134e31b4d85e410571820dc277cdfb5062398cf4f74182c25f5501" },
    { "order 8, c717..fa, canonical", 10, 1, "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fab3e68360b2ff0834698255c2fb078499be7a15a83bf32ba723738f44d782300b" },
};

struct strict_key_vector_t
{
    const char * name;
    int counter;
    const char * key_hex;
};

// every row: R = B, S = 1, and the equation holds at its counter. The
// non-canonical rows whose y is not reduced (ee.., ed..) are also refused by
// the canonical-key check; the two x = 0 sign-bit rows pass libsodium's
// canonical check, so only the small-order check refuses them.

static const strict_key_vector_t strict_key_vectors[] = {
    { "identity (order 1), canonical", 0, "0100000000000000000000000000000000000000000000000000000000000000" },
    { "identity, x = 0 with the sign bit set", 0, "0100000000000000000000000000000000000000000000000000000000000080" },
    { "identity as y = p + 1, non-canonical", 0, "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f" },
    { "identity as y = p + 1 with the sign bit, non-canonical", 0, "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" },
    { "y = -1 (order 2), canonical", 0, "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f" },
    { "y = -1, x = 0 with the sign bit set", 2, "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" },
    { "y = 0, x positive (order 4), canonical", 4, "0000000000000000000000000000000000000000000000000000000000000000" },
    { "y = 0, x negative (order 4), canonical", 0, "0000000000000000000000000000000000000000000000000000000000000080" },
    { "y = 0 as y = p (order 4), non-canonical", 2, "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f" },
    { "y = 0 as y = p with the sign bit (order 4), non-canonical", 0, "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" },
    { "order 8, 26e8..05, canonical", 2, "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05" },
    { "order 8, 26e8..85, canonical", 1, "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85" },
    { "order 8, c717..7a, canonical", 13, "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a" },
    { "order 8, c717..fa, canonical", 7, "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa" },
};

static const int strict_num_r_vectors = (int) ( sizeof( strict_r_vectors ) / sizeof( strict_r_vectors[0] ) );
static const int strict_num_key_vectors = (int) ( sizeof( strict_key_vectors ) / sizeof( strict_key_vectors[0] ) );

static void strict_message( uint8_t * message, int counter )
{
    for ( int i = 0; i < ROCKETNET_SIGNED_CONNECT_DATA_BYTES; i++ )
    {
        message[i] = (uint8_t) i;
    }
    message[0] = (uint8_t) counter;
}

static int strict_verify( int counter, const char * signature_hex, const uint8_t * public_key )
{
    uint8_t message[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    strict_message( message, counter );
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    check( rn_standard_hex_decode( signature_hex, signature, ROCKETNET_SIGNATURE_BYTES ) == ROCKETNET_SIGNATURE_BYTES );
    return rocketnet_verify_signed_connect_data( message, signature, public_key );
}

// R of small order, each encoding on the list, under a key that passes every
// key check: refused. Beside them, two correct strict signatures built the
// same way: verified.

static void test_strict_ed25519_small_order_r_refused()
{
    uint8_t mixed_key[32];
    check( rn_standard_hex_decode( strict_mixed_key_hex, mixed_key, 32 ) == 32 );

    // controls: R = B and S = k + 1 under B (counter 0) and under A (counter
    // 8, where [k]T8 is the identity)

    check( strict_verify( 0, "5866666666666666666666666666666666666666666666666666666666666666e05e4e1cf4a3d4feeed9e82265897af641882911d12d9bcacb2779112e96880a", strict_base_point ) == 1 );
    check( strict_verify( 8, "5866666666666666666666666666666666666666666666666666666666666666210d9fd5092d2f7d2479fa389a2c5ff29db7d3569951eeeaa5ebdecff3ca9d07", mixed_key ) == 1 );

    // the judgement's witness: key B (secret scalar 1), R = the identity,
    // S = k: the equation holds and only the small-order R check refuses

    int admitted = 0;
    if ( strict_verify( 0, "01000000000000000000000000000000000000000000000000000000000000004e68490ddf770a0960616671416709fc29b743db4d754865521bb8d5d3ef820b", strict_base_point ) )
    {
        printf( "strict ed25519: a signature whose R is of small order verified: the identity under the base point\n" );
        admitted++;
    }

    int equation_rows = 0;
    for ( int i = 0; i < strict_num_r_vectors; i++ )
    {
        const strict_r_vector_t * v = &strict_r_vectors[i];
        if ( strict_verify( v->counter, v->signature_hex, mixed_key ) )
        {
            printf( "strict ed25519: a signature whose R is of small order verified: %s\n", v->name );
            admitted++;
        }
        equation_rows += v->equation_holds;
    }
    check( equation_rows == 8 );        // the eight torsion points, canonical
    check( admitted == 0 );
}

REGISTER_UNIT_TEST( test_strict_ed25519_small_order_r_refused );

// a public key of small order, each encoding on the list, with R = B and
// S = 1 at a message where the equation holds: refused

static void test_strict_ed25519_small_order_key_refused()
{
    int admitted = 0;
    for ( int i = 0; i < strict_num_key_vectors; i++ )
    {
        const strict_key_vector_t * v = &strict_key_vectors[i];
        uint8_t key[32];
        check( rn_standard_hex_decode( v->key_hex, key, 32 ) == 32 );
        if ( strict_verify( v->counter, "58666666666666666666666666666666666666666666666666666666666666660100000000000000000000000000000000000000000000000000000000000000", key ) )
        {
            printf( "strict ed25519: a signature under a small-order public key verified: %s\n", v->name );
            admitted++;
        }
    }
    check( admitted == 0 );
}

REGISTER_UNIT_TEST( test_strict_ed25519_small_order_key_refused );

// The forgery through the server's whole request path: a server whose backend
// public key is each small-order encoding, and 64 tokens per key that are
// right in every field but signed by nobody (R = B, S = 1; the counter here
// is the client id inside the sealed block, which moves the hash). Every one
// reaches the signature rung and is refused there. With the small-order key
// check removed, about half the tokens under the order-2 key verify, and an
// eighth under an order-8 key. The sealed block is sealed deterministically,
// so the same tokens are presented on every run. Beside it, a token the
// backend really signed is challenged by the same harness with the real key.

static void test_strict_ed25519_forged_token_under_small_order_backend_key_refused()
{
    {
        struct signed_token_spec_t spec;
        signed_token_spec_default( &spec, 1 );
        signed_token_expect_challenged( &spec );
    }

    const int tokens = 64;
    uint8_t forged_signature[ROCKETNET_SIGNATURE_BYTES];
    memset( forged_signature, 0, sizeof( forged_signature ) );
    memcpy( forged_signature, strict_base_point, 32 );
    forged_signature[32] = 1;

    int admitted = 0;
    for ( int k = 0; k < strict_num_key_vectors; k++ )
    {
        uint8_t key[32];
        check( rn_standard_hex_decode( strict_key_vectors[k].key_hex, key, 32 ) == 32 );

        struct signed_token_server_t test;
        signed_token_server_setup( &test, key );

        int admitted_here = 0;
        for ( int i = 0; i < tokens; i++ )
        {
            struct signed_token_spec_t spec;
            signed_token_spec_default( &spec, (uint64_t) ( 0x7000 + i ) );

            uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
            uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
            signed_token_mint( &spec, signed_connect_data, signature );

            struct rocketnet_address_t address;
            char address_string[64];
            snprintf( address_string, sizeof( address_string ), "10.9.%d.%d:56789", k + 1, i + 1 );
            check( rocketnet_address_parse( &address, address_string ) );

            if ( signed_token_send( &test, &address, signed_connect_data, forged_signature ) != ROCKETNET_SERVER_PROCESS_IGNORED )
            {
                admitted_here++;
            }
        }

        if ( admitted_here != 0 )
        {
            printf( "strict ed25519: %d of %d forged tokens admitted under the backend key %s\n", admitted_here, tokens, strict_key_vectors[k].name );
        }
        else
        {
            check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] == (uint64_t) tokens );
            check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE] == 0 );
            check( rocketnet_encryption_map_num_mappings( test.server.encryption_map ) == 0 );
        }
        admitted += admitted_here;

        signed_token_server_teardown( &test );
    }

    check( admitted == 0 );
}

REGISTER_UNIT_TEST( test_strict_ed25519_forged_token_under_small_order_backend_key_refused );
