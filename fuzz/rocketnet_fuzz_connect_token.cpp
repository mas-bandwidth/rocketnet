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

// the valid baseline, minted once: the signed data and signature never vary,
// so every structured input mutates the same bytes

static uint8_t rocketnet_fuzz_base_token[ROCKETNET_CONNECT_TOKEN_BYTES];

// the signed-data regions, from the token layout in rocketnet_connect_token.h:
// [0,640) is covered by the signature (signed data then the signature itself),
// [640,768) is the clear session keys and the pad, outside it

#define FUZZ_TOKEN_COVERED_BYTES  ( ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES )

static void rocketnet_fuzz_build_base_token()
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    memset( &private_fields, 0, sizeof(private_fields) );

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
    memset( &counters, 0, sizeof(counters) );

    static struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( client.packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );

    const int connected = rocketnet_client_protocol_connect( &client, token_data, 100.0, current_timestamp );

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

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
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
        // arbitrary bytes as a token, zero-padded to the fixed size the way
        // the transport's fixed-length receive path presents it
        if ( body_size > ROCKETNET_CONNECT_TOKEN_BYTES )
            body_size = ROCKETNET_CONNECT_TOKEN_BYTES;
        memset( token_data, 0, sizeof(token_data) );
        memcpy( token_data, body, body_size );

        rocketnet_fuzz_token_ladder( token_data, 0, 0, 0 );
        rocketnet_fuzz_client_connect( token_data, current_timestamp );
        return 0;
    }

    // the structured half: fuzz-chosen xor flips over the valid baseline,
    // as (offset u16 LE, xor byte) records. An xor of zero changes nothing,
    // and the oracle switches track only EFFECTIVE flips.
    memcpy( token_data, rocketnet_fuzz_base_token, sizeof(token_data) );

    size_t offset = 0;
    while ( offset + 3 <= body_size )
    {
        const int position = (int) ( ( (size_t) body[offset] | ( (size_t) body[offset+1] << 8 ) ) % ROCKETNET_CONNECT_TOKEN_BYTES );
        token_data[position] ^= body[offset+2];
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

    rocketnet_fuzz_token_ladder( token_data, touched_covered, touched_any, 1 );
    rocketnet_fuzz_client_connect( token_data, current_timestamp );

    return 0;
}
