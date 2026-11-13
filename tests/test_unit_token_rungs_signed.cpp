/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Second witnesses for admission rungs that one case each used to hold:
// the address flags (t6.2, t6.4
// step 2, t12 rule 6), the signed zero pad (t1.4 rule 3, t6.2), the signed
// server key (t6.4 step 3, t12 rule 6), the private zero pad (t1.4 rule 3,
// t6.1) and the sealed expiry (t6.1, t6.4 step 6). The first witnesses edit
// bytes of a valid request after it was signed, or set the first or last pad
// byte; these present tokens the backend really signed, each wrong in one
// field, through the whole request path past the cookie gate, and read the
// refusal off its own counter. So a removed rung shows here as an admitted
// token (a challenge) or as the refusal moving to a later rung.

#include "signed_token_harness.h"

#define check rocketnet_unit_check

// address flags: an unknown bit beside a valid family, an unknown bit alone,
// every bit, and no bit. Each token is signed, names 203.0.113.10:40000 in
// its IPv4 field and is refused at the flags rung, not later.

static void test_token_rung_address_flags_signed_tokens()
{
    struct signed_token_spec_t spec;
    signed_token_spec_default( &spec, 1 );
    signed_token_expect_challenged( &spec );

    const uint8_t flags[4] = { 0x81, 0x04, 0xFF, 0x00 };
    for ( int i = 0; i < 4; i++ )
    {
        signed_token_spec_default( &spec, 1 );
        spec.signed_fields.address_flags = flags[i];
        signed_token_expect_refused( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS );
    }
}

REGISTER_UNIT_TEST( test_token_rung_address_flags_signed_tokens );

// the signed zero pad: a middle byte with only its high bit set, and a whole
// pad of 0xFF, each signed. The refusal is counted with the version refusal.

static void test_token_rung_signed_pad_signed_tokens()
{
    struct signed_token_spec_t spec;
    signed_token_spec_default( &spec, 2 );
    spec.signed_pad_index = SIGNED_TOKEN_SIGNED_PAD_BYTES / 2;
    spec.signed_pad_value = 0x80;
    signed_token_expect_refused( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION );

    for ( int i = 1; i < SIGNED_TOKEN_SIGNED_PAD_BYTES - 1; i += 7 )
    {
        signed_token_spec_default( &spec, 2 );
        spec.signed_pad_index = i;
        spec.signed_pad_value = 0xFF;
        signed_token_expect_refused( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION );
    }
}

REGISTER_UNIT_TEST( test_token_rung_signed_pad_signed_tokens );

// the signed server key names another server. Two shapes: a token minted
// whole for that other server (its block sealed to it), and a token that
// names the other server but whose block is sealed to this one, so it would
// open here. Both are refused at the key rung, before the cookie gate's
// work, and neither reaches the handshake work budget, the signature or the
// sealed block's open (the work is read in a development build; the budget
// in every build).

static void test_token_rung_server_key_signed_tokens()
{
    static const uint8_t other_secret[32] = {
        0x4f, 0x54, 0x48, 0x45, 0x52, 0x2d, 0x53, 0x45, 0x52, 0x56, 0x45, 0x52, 0x2d, 0x4b, 0x45, 0x59,
        0x2d, 0x46, 0x4f, 0x52, 0x2d, 0x55, 0x4e, 0x49, 0x54, 0x2d, 0x43, 0x41, 0x53, 0x45, 0x53, 0x21
    };
    uint8_t other_public[32];
    check( crypto_scalarmult_base( other_public, other_secret ) == 0 );
    check( memcmp( other_public, rocketnet_test_server_public_key, 32 ) != 0 );

    struct signed_token_spec_t spec;
    signed_token_spec_default( &spec, 3 );
    memcpy( spec.signed_fields.server_public_key, other_public, 32 );
    signed_token_expect_refused( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY );
    signed_token_expect_refused_unworked( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY );

    signed_token_spec_default( &spec, 3 );
    memcpy( spec.signed_fields.server_public_key, other_public, 32 );
    spec.seal_key = rocketnet_test_server_public_key;
    signed_token_expect_refused( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY );
    signed_token_expect_refused_unworked( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY );
}

REGISTER_UNIT_TEST( test_token_rung_server_key_signed_tokens );

// the private zero pad: a middle byte with only its high bit set. The block
// is sealed to this server and the token is signed, so the seal opens; the
// opener refuses the block for its pad, and so does the server, counting it
// as a block that did not open. The control, the same token with the pad
// zero, opens to the fields it was minted with.

static void test_token_rung_private_pad_signed_token()
{
    struct signed_token_spec_t spec;
    signed_token_spec_default( &spec, 4 );

    uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    signed_token_mint( &spec, signed_connect_data, signature );

    struct rocketnet_connect_token_private_t opened;
    memset( &opened, 0, sizeof( opened ) );
    check( rocketnet_open_connect_token_private( &opened, signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) == 1 );
    check( opened.client_id == 4 );
    check( opened.expire_timestamp == ROCKETNET_TEST_EXPIRE_TS );

    spec.private_pad_index = SIGNED_TOKEN_PRIVATE_PAD_BYTES / 2;
    spec.private_pad_value = 0x80;
    signed_token_mint( &spec, signed_connect_data, signature );
    check( rocketnet_verify_signed_connect_data( signed_connect_data, signature, rocketnet_test_backend_public_key ) == 1 );

    memset( &opened, 0x5a, sizeof( opened ) );
    check( rocketnet_open_connect_token_private( &opened, signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) == 0 );
    check( opened.client_id == 0x5a5a5a5a5a5a5a5aULL );

    signed_token_expect_refused( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK );
}

REGISTER_UNIT_TEST( test_token_rung_private_pad_signed_token );

// the sealed expiry against the signed one: one second either side and the
// top bit, each at the consistency check itself, and one second early
// through the server. The sealed protocol id is equal in every case, so the
// expiry alone decides.

static void test_token_rung_sealed_expire_signed_tokens()
{
    struct signed_token_spec_t spec;
    signed_token_spec_default( &spec, 5 );

    uint8_t signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    signed_token_mint( &spec, signed_connect_data, signature );

    struct rocketnet_connect_token_private_t fields = spec.private_fields;
    check( rocketnet_connect_token_private_consistent( &fields, signed_connect_data ) == 1 );

    const uint64_t sealed_expire[3] = { ROCKETNET_TEST_EXPIRE_TS + 1, ROCKETNET_TEST_EXPIRE_TS - 1, ROCKETNET_TEST_EXPIRE_TS ^ ( 1ULL << 63 ) };
    for ( int i = 0; i < 3; i++ )
    {
        fields.expire_timestamp = sealed_expire[i];
        check( rocketnet_connect_token_private_consistent( &fields, signed_connect_data ) == 0 );
    }

    signed_token_spec_default( &spec, 5 );
    spec.private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS - 1;
    signed_token_expect_refused( &spec, ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH );
}

REGISTER_UNIT_TEST( test_token_rung_sealed_expire_signed_tokens );
