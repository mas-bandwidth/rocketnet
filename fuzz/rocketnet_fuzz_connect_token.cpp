/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The connect token fuzz target (nightly): the server-side token ladder --
// layout split, Ed25519 verification, sealed-block open, inner consistency,
// the identifier -- and the client-side connect parse, on two kinds of input.
//
// TWO HALVES, selected by the input's first byte, because they find different
// bugs. The RAW half feeds arbitrary bytes as a 768-byte token: it hunts
// parser crashes, and it can never pass verification, so it exercises the
// refusal ladder only. The STRUCTURED half starts from a VALID token -- built
// once with the real minting path and the published development keys -- and
// applies fuzz-chosen byte flips: the mutants keep enough structure to reach
// past the cheap checks, which is where the deeper bugs live, and the valid
// baseline makes strong oracles possible.
//
// THE ORACLES, from the standard's own guarantees, each an abort because a
// wrong ANSWER here is a protocol break, not a wrong access:
//
//   - the untouched token must verify, open, and be consistent: the harness's
//     own baseline, checked per input so a state leak between inputs is loud.
//   - any effective flip inside the signed data or the signature MUST fail
//     verification. A mutant that verifies is an Ed25519 forgery.
//   - flips confined to the clear keys and pad -- outside the signature --
//     MUST NOT disturb verification or the open: the signature covers exactly
//     the bytes the standard says it covers, no more and no fewer.
//
// DETERMINISM: every token field is fixed -- vector timestamps, dev keys,
// pattern session keys -- and Ed25519 signing is deterministic, so the signed
// data and signature are identical bytes every run. The sealed block's BYTES
// vary run to run (crypto_box_seal draws a random ephemeral key), but its
// decrypted CONTENT is the fixed private fields, and no control flow branches
// on the sealed bytes' values -- an open either succeeds into fixed plaintext
// or fails on the MAC -- so a crashing input replays. The client connect draws
// a random salt per attempt, same argument.

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#include <cstdio>

#include "../tests/portable_crt.h"

// the valid baseline, minted once: the signed data and signature never vary,
// so every structured input mutates the same bytes

static uint8_t rocketnet_fuzz_base_token[ROCKETNET_CONNECT_TOKEN_BYTES];

// the types the parser reads, one bit each, set as the input walks them. The
// seed generator (ROCKETNET_FUZZ_SEED_DIR) writes one named seed per type and
// refuses to finish until every type is reached, so an input that stops
// covering a type is caught here as INCOMPLETE.

enum
{
    ROCKETNET_FUZZ_CONNECT_RAW = 1ULL << 0,
    ROCKETNET_FUZZ_CONNECT_STRUCTURED = 1ULL << 1,
    ROCKETNET_FUZZ_CONNECT_CLIENT_FLAGS = 1ULL << 2,
    ROCKETNET_FUZZ_CONNECT_CLIENT_WINDOW = 1ULL << 3,
    ROCKETNET_FUZZ_CONNECT_CLIENT_VERSION = 1ULL << 4,
    ROCKETNET_FUZZ_CONNECT_CLIENT_EXPIRED = 1ULL << 5,
    ROCKETNET_FUZZ_CONNECT_CLIENT_IPV4 = 1ULL << 6,
    ROCKETNET_FUZZ_CONNECT_CLIENT_IPV6 = 1ULL << 7,
    ROCKETNET_FUZZ_CONNECT_SIGNATURE_VERIFIED = 1ULL << 8,
    ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED = 1ULL << 9,
    ROCKETNET_FUZZ_CONNECT_PRIVATE_OPENED = 1ULL << 10,
    ROCKETNET_FUZZ_CONNECT_COVERED_MUTATION = 1ULL << 11,
    ROCKETNET_FUZZ_CONNECT_CLEAR_MUTATION = 1ULL << 12
};

static uint64_t rocketnet_fuzz_connect_coverage = 0;

static int rocketnet_fuzz_connect_generate_seed_dir( const char * directory );

// the signed-data regions, from the token layout in rocketnet_connect_token.h:
// [0,640) is covered by the signature (signed data then the signature itself),
// [640,768) is the clear session keys and the pad, outside it

#define FUZZ_TOKEN_COVERED_BYTES  ( ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES )

static void rocketnet_fuzz_build_base_token()
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    memset( &private_fields, 0, sizeof( private_fields ) );

    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_CREATE_TS + 300;
    signed_fields.timeout_seconds = 10;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address_bytes[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address_bytes, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, 32 );

    private_fields.client_id = 1;
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        private_fields.client_to_server_key[i] = (uint8_t) ( 0x10 + i );
        private_fields.server_to_client_key[i] = (uint8_t) ( 0x50 + i );
    }
    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
        private_fields.user_data[i] = (uint8_t) i;

    if ( !rocketnet_write_connect_token( &signed_fields, &private_fields,
                                         rocketnet_dev_backend_secret_key,
                                         rocketnet_fuzz_base_token ) )
        abort();
}

// the server-side ladder over one 768-byte buffer, with the oracle switches
// the caller derives from what it knows it changed. touched_covered means an
// effective flip landed in [0,640); touched_any means one landed anywhere.

static void rocketnet_fuzz_token_ladder( const uint8_t * token_data, int touched_covered, int touched_any, int is_mutant )
{
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const int verified = rocketnet_verify_signed_connect_data( token.signed_connect_data,
                                                               token.signature,
                                                               rocketnet_dev_backend_public_key );

    if ( verified )
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_SIGNATURE_VERIFIED;
    else
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED;

    if ( is_mutant )
    {
        // a mutant whose covered bytes changed and which still verifies is a
        // forgery; a mutant whose covered bytes did NOT change must verify,
        // because the signature covers nothing outside them
        if ( touched_covered && verified )
            abort();
        if ( !touched_covered && !verified )
            abort();
        (void) touched_any;
    }

    if ( verified )
    {
        struct rocketnet_connect_token_private_t private_fields;
        const int opened = rocketnet_open_connect_token_private( &private_fields,
                                                                 token.signed_connect_data,
                                                                 rocketnet_dev_server_public_key,
                                                                 rocketnet_dev_server_secret_key );

        if ( opened )
            rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_PRIVATE_OPENED;

        // verification passing means the signed data -- sealed block included
        // -- is byte-identical to what the backend signed, so the open and the
        // inner consistency must both hold
        if ( is_mutant && !touched_covered )
        {
            if ( !opened )
                abort();
            if ( !rocketnet_connect_token_private_consistent( &private_fields, token.signed_connect_data ) )
                abort();
        }
        else if ( opened )
        {
            if ( !rocketnet_connect_token_private_consistent( &private_fields, token.signed_connect_data ) )
                abort();
        }
    }

    // the identifier is content-determined over the signed data and must be
    // computable for any buffer
    uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_connect_token_identifier( identifier, token.signed_connect_data );

    // the layout readers on their own: pure offset arithmetic over fixed
    // buffers, obliged to survive anything
    struct rocketnet_signed_connect_data_t signed_out;
    rocketnet_read_signed_connect_data( &signed_out, token.signed_connect_data );
    struct rocketnet_connect_token_private_t private_out;
    rocketnet_read_connect_token_private( &private_out, token_data );
}

// the client-side parse: connect runs the pre-send checks and either enters
// _requesting cookie_ or dies to the terminal that names why -- both are
// legal, anything else is a crash the fuzzer keeps

static void rocketnet_fuzz_client_connect( const uint8_t * token_data, uint64_t current_timestamp )
{
    static struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    static struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( client.packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );

    // the fields the client's pre-send checks branch on, read the same way the
    // protocol reads them, so the type bits track the branches the parser takes
    struct rocketnet_signed_connect_data_t signed_fields;
    rocketnet_read_signed_connect_data( &signed_fields, token_data );
    const uint8_t flags = signed_fields.address_flags;
    if ( flags == 0 || ( flags & ~( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) ) != 0 )
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_CLIENT_FLAGS;
    if ( signed_fields.create_timestamp > signed_fields.expire_timestamp )
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_CLIENT_WINDOW;
    if ( signed_fields.version != ROCKETNET_WIRE_VERSION )
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_CLIENT_VERSION;

    const int connected = rocketnet_client_protocol_connect( &client, token_data, 100.0, current_timestamp );

    if ( !connected )
    {
        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED )
            rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_CLIENT_EXPIRED;
    }
    else if ( flags & ROCKETNET_ADDRESS_FLAG_IPV6 )
    {
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_CLIENT_IPV6;
    }
    else
    {
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_CLIENT_IPV4;
    }

    if ( connected )
    {
        // one update builds the cookie request from the parsed fields
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes = 0;
        rocketnet_client_protocol_update( &client, 100.1, packet, &packet_bytes );
        if ( packet_bytes < 0 || packet_bytes > ROCKETNET_SEND_BUDGET )
            abort();
    }
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    if ( sodium_init() < 0 )
        abort();

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    rocketnet_fuzz_build_base_token();

    // the harness's own premise, checked where it is created: the baseline
    // must walk the whole ladder clean before anything is mutated
    rocketnet_fuzz_token_ladder( rocketnet_fuzz_base_token, 0, 0, 0 );

    // the seed generator, driven by the environment: writes one named seed per
    // type into the directory, replaying each before it is written, and fails
    // the run (exit 1, INCOMPLETE) if any type stays uncovered
    char seed_dir[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
    {
        if ( !rocketnet_fuzz_connect_generate_seed_dir( seed_dir ) )
            return 1;
        exit( 0 );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    rocketnet_fuzz_connect_coverage = 0;

    if ( size < 2 )
        return 0;

    // byte 0 selects the half; byte 1 selects the client's clock against the
    // token's fixed validity window, covering not-yet-valid, valid and
    // expired without a wall clock anywhere
    const int raw = data[0] & 1;
    const uint64_t current_timestamp = ROCKETNET_TEST_CREATE_TS + (uint64_t) ( (int64_t) (int8_t) data[1] * 60 );

    const uint8_t * body = data + 2;
    size_t body_size = size - 2;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];

    if ( raw )
    {
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_RAW;

        // arbitrary bytes as a token, zero-padded to the fixed size the way
        // the transport's fixed-length receive path presents it
        if ( body_size > ROCKETNET_CONNECT_TOKEN_BYTES )
            body_size = ROCKETNET_CONNECT_TOKEN_BYTES;
        memset( token_data, 0, sizeof( token_data ) );
        memcpy( token_data, body, body_size );

        rocketnet_fuzz_token_ladder( token_data, 0, 0, 0 );
        rocketnet_fuzz_client_connect( token_data, current_timestamp );
        return 0;
    }

    // the structured half: fuzz-chosen xor flips over the valid baseline,
    // as (offset u16 LE, xor byte) records. An xor of zero changes nothing,
    // and the oracle switches track only EFFECTIVE flips.
    rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_STRUCTURED;

    memcpy( token_data, rocketnet_fuzz_base_token, sizeof( token_data ) );

    size_t offset = 0;
    while ( offset + 3 <= body_size )
    {
        const int position = (int) ( ( (size_t) body[offset] | ( (size_t) body[offset + 1] << 8 ) ) % ROCKETNET_CONNECT_TOKEN_BYTES );
        token_data[position] ^= body[offset + 2];
        offset += 3;
    }

    // what actually changed is measured against the baseline, not inferred
    // from the flip records: two flips on one byte cancel, and an oracle fed
    // the records instead of the bytes would call the resulting identical
    // token a forgery
    const int touched_covered = memcmp( token_data, rocketnet_fuzz_base_token, FUZZ_TOKEN_COVERED_BYTES ) != 0;
    const int touched_any = touched_covered ||
                            memcmp( token_data + FUZZ_TOKEN_COVERED_BYTES,
                                    rocketnet_fuzz_base_token + FUZZ_TOKEN_COVERED_BYTES,
                                    ROCKETNET_CONNECT_TOKEN_BYTES - FUZZ_TOKEN_COVERED_BYTES ) != 0;

    if ( touched_covered )
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_COVERED_MUTATION;
    else
        rocketnet_fuzz_connect_coverage |= ROCKETNET_FUZZ_CONNECT_CLEAR_MUTATION;

    rocketnet_fuzz_token_ladder( token_data, touched_covered, touched_any, 1 );
    rocketnet_fuzz_client_connect( token_data, current_timestamp );

    return 0;
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir> writes one named seed
//  per type the parser reads, each built from real constants and the real
//  minting path, replayed through the target before it is written, and
//  returns failure (INCOMPLETE) unless every type is reached. ROCKETNET_
//  FUZZ_WITHHOLD=<seed name> skips one seed, so the run shows the
//  INCOMPLETE path.
// -----------------------------------------------------------------------

enum
{
    ROCKETNET_FUZZ_CONNECT_SEED_ARBITRARY = 0,
    ROCKETNET_FUZZ_CONNECT_SEED_FLAGS,
    ROCKETNET_FUZZ_CONNECT_SEED_VERSION,
    ROCKETNET_FUZZ_CONNECT_SEED_WINDOW,
    ROCKETNET_FUZZ_CONNECT_SEED_EXPIRED,
    ROCKETNET_FUZZ_CONNECT_SEED_IPV4,
    ROCKETNET_FUZZ_CONNECT_SEED_IPV6,
    ROCKETNET_FUZZ_CONNECT_SEED_VERIFIED,
    ROCKETNET_FUZZ_CONNECT_SEED_COVERED,
    ROCKETNET_FUZZ_CONNECT_SEED_CLEAR
};

struct rocketnet_fuzz_connect_seed_spec_t
{
    const char * name;
    uint64_t mask;
    int kind;
};

static const struct rocketnet_fuzz_connect_seed_spec_t rocketnet_fuzz_connect_seed_specs[] = {
    { "seed_type_signature_rejected", ROCKETNET_FUZZ_CONNECT_RAW | ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED, ROCKETNET_FUZZ_CONNECT_SEED_ARBITRARY },
    { "seed_type_client_flags_invalid", ROCKETNET_FUZZ_CONNECT_RAW | ROCKETNET_FUZZ_CONNECT_CLIENT_FLAGS | ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED, ROCKETNET_FUZZ_CONNECT_SEED_FLAGS },
    { "seed_type_client_version_invalid", ROCKETNET_FUZZ_CONNECT_RAW | ROCKETNET_FUZZ_CONNECT_CLIENT_VERSION | ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED, ROCKETNET_FUZZ_CONNECT_SEED_VERSION },
    { "seed_type_client_window_invalid", ROCKETNET_FUZZ_CONNECT_RAW | ROCKETNET_FUZZ_CONNECT_CLIENT_WINDOW | ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED, ROCKETNET_FUZZ_CONNECT_SEED_WINDOW },
    { "seed_type_client_expired", ROCKETNET_FUZZ_CONNECT_RAW | ROCKETNET_FUZZ_CONNECT_CLIENT_EXPIRED | ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED, ROCKETNET_FUZZ_CONNECT_SEED_EXPIRED },
    { "seed_type_client_ipv4", ROCKETNET_FUZZ_CONNECT_RAW | ROCKETNET_FUZZ_CONNECT_CLIENT_IPV4 | ROCKETNET_FUZZ_CONNECT_SIGNATURE_VERIFIED | ROCKETNET_FUZZ_CONNECT_PRIVATE_OPENED, ROCKETNET_FUZZ_CONNECT_SEED_IPV4 },
    { "seed_type_client_ipv6", ROCKETNET_FUZZ_CONNECT_RAW | ROCKETNET_FUZZ_CONNECT_CLIENT_IPV6 | ROCKETNET_FUZZ_CONNECT_SIGNATURE_VERIFIED | ROCKETNET_FUZZ_CONNECT_PRIVATE_OPENED, ROCKETNET_FUZZ_CONNECT_SEED_IPV6 },
    { "seed_type_signature_verified", ROCKETNET_FUZZ_CONNECT_STRUCTURED | ROCKETNET_FUZZ_CONNECT_SIGNATURE_VERIFIED | ROCKETNET_FUZZ_CONNECT_PRIVATE_OPENED, ROCKETNET_FUZZ_CONNECT_SEED_VERIFIED },
    { "seed_type_covered_mutation", ROCKETNET_FUZZ_CONNECT_STRUCTURED | ROCKETNET_FUZZ_CONNECT_COVERED_MUTATION | ROCKETNET_FUZZ_CONNECT_SIGNATURE_REJECTED, ROCKETNET_FUZZ_CONNECT_SEED_COVERED },
    { "seed_type_clear_mutation", ROCKETNET_FUZZ_CONNECT_STRUCTURED | ROCKETNET_FUZZ_CONNECT_CLEAR_MUTATION | ROCKETNET_FUZZ_CONNECT_SIGNATURE_VERIFIED | ROCKETNET_FUZZ_CONNECT_PRIVATE_OPENED, ROCKETNET_FUZZ_CONNECT_SEED_CLEAR }
};

static void rocketnet_fuzz_connect_write_u32( uint8_t * output, uint32_t value )
{
    output[0] = (uint8_t) value;
    output[1] = (uint8_t) ( value >> 8 );
    output[2] = (uint8_t) ( value >> 16 );
    output[3] = (uint8_t) ( value >> 24 );
}

static void rocketnet_fuzz_connect_write_u64( uint8_t * output, uint64_t value )
{
    for ( int i = 0; i < 8; i++ )
        output[i] = (uint8_t) ( value >> ( i * 8 ) );
}

static void rocketnet_fuzz_connect_fill_header( uint8_t * token, uint32_t version, uint8_t flags, uint64_t create, uint64_t expire, uint32_t timeout )
{
    rocketnet_fuzz_connect_write_u32( token + ROCKETNET_SIGNED_OFFSET_VERSION, version );
    rocketnet_fuzz_connect_write_u64( token + ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID, ROCKETNET_DEV_PROTOCOL_ID );
    rocketnet_fuzz_connect_write_u64( token + ROCKETNET_SIGNED_OFFSET_CREATE, create );
    rocketnet_fuzz_connect_write_u64( token + ROCKETNET_SIGNED_OFFSET_EXPIRE, expire );
    rocketnet_fuzz_connect_write_u32( token + ROCKETNET_SIGNED_OFFSET_TIMEOUT, timeout );
    token[ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS] = flags;
}

static void rocketnet_fuzz_connect_mint_token( uint8_t address_flags, int ipv6, uint8_t * out )
{
    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    memset( &sf, 0, sizeof( sf ) );
    memset( &pf, 0, sizeof( pf ) );
    sf.version = ROCKETNET_WIRE_VERSION;
    sf.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    sf.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    sf.expire_timestamp = ROCKETNET_TEST_CREATE_TS + 300;
    sf.timeout_seconds = 10;
    sf.address_flags = address_flags;
    if ( ipv6 )
    {
        static const uint8_t v6[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0xff, 0xff };
        memcpy( sf.ipv6_address, v6, 18 );
    }
    else
    {
        static const uint8_t v4[6] = { 203, 0, 113, 10, 0x40, 0x9c };
        memcpy( sf.ipv4_address, v4, 6 );
    }
    memcpy( sf.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    pf.client_id = 1;
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        pf.client_to_server_key[i] = (uint8_t) ( 0x10 + i );
        pf.server_to_client_key[i] = (uint8_t) ( 0x50 + i );
    }
    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
        pf.user_data[i] = (uint8_t) i;
    static const uint8_t ephemeral_secret[ROCKETNET_KEY_BYTES] = { 0x21 };
    if ( !rocketnet_write_connect_token_deterministic( &sf, &pf, rocketnet_dev_backend_secret_key, ephemeral_secret, out ) )
        abort();
}

static size_t rocketnet_fuzz_connect_build_seed( int kind, uint8_t * output, size_t capacity )
{
    if ( kind == ROCKETNET_FUZZ_CONNECT_SEED_VERIFIED ||
         kind == ROCKETNET_FUZZ_CONNECT_SEED_COVERED ||
         kind == ROCKETNET_FUZZ_CONNECT_SEED_CLEAR )
    {
        if ( capacity < 5 )
            return 0;
        output[0] = 0;
        output[1] = 0;
        size_t size = 2;
        if ( kind == ROCKETNET_FUZZ_CONNECT_SEED_COVERED )
        {
            output[size++] = 0x00;
            output[size++] = 0x00;
            output[size++] = 0x01;
        }
        else if ( kind == ROCKETNET_FUZZ_CONNECT_SEED_CLEAR )
        {
            output[size++] = 0x80;
            output[size++] = 0x02;
            output[size++] = 0x01;
        }
        return size;
    }

    if ( capacity < 2 + ROCKETNET_CONNECT_TOKEN_BYTES )
        return 0;
    output[0] = 1;
    output[1] = 0;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );

    switch ( kind )
    {
        case ROCKETNET_FUZZ_CONNECT_SEED_ARBITRARY:
            for ( int i = 0; i < 64; i++ )
                token[i] = 0xAA;
            break;
        case ROCKETNET_FUZZ_CONNECT_SEED_FLAGS:
            rocketnet_fuzz_connect_fill_header( token, ROCKETNET_WIRE_VERSION, 0x04,
                                                ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 300, 10 );
            break;
        case ROCKETNET_FUZZ_CONNECT_SEED_VERSION:
            rocketnet_fuzz_connect_fill_header( token, 0, ROCKETNET_ADDRESS_FLAG_IPV4,
                                                ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 300, 10 );
            break;
        case ROCKETNET_FUZZ_CONNECT_SEED_WINDOW:
            rocketnet_fuzz_connect_fill_header( token, ROCKETNET_WIRE_VERSION, ROCKETNET_ADDRESS_FLAG_IPV4,
                                                ROCKETNET_TEST_CREATE_TS + 300, ROCKETNET_TEST_CREATE_TS, 10 );
            break;
        case ROCKETNET_FUZZ_CONNECT_SEED_EXPIRED:
            output[1] = 5;
            rocketnet_fuzz_connect_fill_header( token, ROCKETNET_WIRE_VERSION, ROCKETNET_ADDRESS_FLAG_IPV4,
                                                ROCKETNET_TEST_CREATE_TS, ROCKETNET_TEST_CREATE_TS + 300, 10 );
            break;
        case ROCKETNET_FUZZ_CONNECT_SEED_IPV4:
            rocketnet_fuzz_connect_mint_token( ROCKETNET_ADDRESS_FLAG_IPV4, 0, token );
            break;
        case ROCKETNET_FUZZ_CONNECT_SEED_IPV6:
            rocketnet_fuzz_connect_mint_token( ROCKETNET_ADDRESS_FLAG_IPV6, 1, token );
            break;
        default:
            return 0;
    }

    memcpy( output + 2, token, sizeof( token ) );
    return 2 + ROCKETNET_CONNECT_TOKEN_BYTES;
}

static int rocketnet_fuzz_connect_generate_seed_dir( const char * directory )
{
    char withheld[512];
    const int have_withheld = rocketnet_test_getenv( "ROCKETNET_FUZZ_WITHHOLD", withheld, sizeof( withheld ) );

    const size_t spec_count = sizeof( rocketnet_fuzz_connect_seed_specs ) / sizeof( rocketnet_fuzz_connect_seed_specs[0] );
    uint64_t coverage = 0;

    for ( size_t index = 0; index < spec_count; index++ )
    {
        const struct rocketnet_fuzz_connect_seed_spec_t * spec = &rocketnet_fuzz_connect_seed_specs[index];
        if ( have_withheld && strcmp( withheld, spec->name ) == 0 )
            continue;

        uint8_t data[2 + ROCKETNET_CONNECT_TOKEN_BYTES];
        const size_t size = rocketnet_fuzz_connect_build_seed( spec->kind, data, sizeof( data ) );
        if ( size == 0 )
            return 0;

        rocketnet_fuzz_connect_coverage = 0;
        LLVMFuzzerTestOneInput( data, size );
        coverage |= rocketnet_fuzz_connect_coverage;
    }

    for ( size_t index = 0; index < spec_count; index++ )
    {
        const struct rocketnet_fuzz_connect_seed_spec_t * spec = &rocketnet_fuzz_connect_seed_specs[index];
        if ( ( coverage & spec->mask ) != spec->mask )
        {
            fprintf( stderr, "INCOMPLETE target=rocketnet_fuzz_connect_token missing=%s\n", spec->name );
            return 0;
        }
    }

    for ( size_t index = 0; index < spec_count; index++ )
    {
        const struct rocketnet_fuzz_connect_seed_spec_t * spec = &rocketnet_fuzz_connect_seed_specs[index];
        if ( have_withheld && strcmp( withheld, spec->name ) == 0 )
            continue;

        uint8_t data[2 + ROCKETNET_CONNECT_TOKEN_BYTES];
        const size_t size = rocketnet_fuzz_connect_build_seed( spec->kind, data, sizeof( data ) );
        if ( size == 0 )
            return 0;

        char path[1024];
        snprintf( path, sizeof( path ), "%s/%s", directory, spec->name );
        FILE * f = rocketnet_test_fopen( path, "wb" );
        if ( !f )
            return 0;
        const size_t written = fwrite( data, 1, size, f );
        if ( fclose( f ) != 0 || written != size )
            return 0;
    }

    return 1;
}
