/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The connect token's layouts past the cryptography, yojimbo's
// fuzz_netcode_connect_token. rocketnet_fuzz_connect_token mutates a whole
// token, and every effective mutation of the signed data fails the Ed25519
// verify and every mutation of the sealed block fails the box's MAC -- which
// is the point of them, and which also means the readers behind them, and
// every check the request ladder makes on a verified token, are unreachable
// by mutation there. This target reaches them directly.
//
// THE SELECTOR BYTE: input[0] & 3 chooses the surface, the rest is its bytes.
//   0 = the decrypted private block (ROCKETNET_PRIVATE_BLOCK_BYTES): read,
//       then checked for consistency against a signed-data layout taken
//       from the bytes after it
//   1 = the signed connect data layout (ROCKETNET_SIGNED_CONNECT_DATA_BYTES):
//       read
//   2 = a token MINTED with the published development keys from fuzz-chosen
//       signed and private fields, carried in a real connection request
//       through the server's request ladder, cookie gate included: every
//       check the ladder makes after the signature verifies and the block
//       opens, on fuzz-chosen field values
//   3 = the same ladder over fuzz-chosen signed-data BYTES, the sealed block
//       replaced by a fuzz-chosen raw private block sealed to the dev server
//       key, and the whole signed with the dev backend key: arbitrary layouts
//       (nonzero padding included) behind a valid signature and a valid seal
// Short input is zero-filled, so every input is a whole layout.
//
// THE ORACLES:
//   - write(read(bytes)) reproduces the bytes over the fields the layout
//     defines, and zero-fills the padding the writer owns; the readers
//     ignore padding, so padding is not compared
//   - consistency holds exactly when the private protocol id and expire equal
//     the signed ones (rocketnet_connect_token.h)
//   - the ladder's acceptance implies the standard's plainest rules: the wire
//     version, this server's protocol id, an expire after now, no address
//     flag bit past the two defined, and this server's public key
//   - the server's counters are conserved after every ladder run
//
// No state survives an input: the server and its tables are built and
// destroyed inside each one. The seal draws a random ephemeral key, but no
// branch turns on its value -- an open succeeds into the fixed plaintext or
// fails on the MAC -- so a crashing input replays.

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_fragment.h"
#include "rocketnet_ack.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#include "../tests/portable_crt.h"

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
        return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

// the bytes the private writer fills with fields; the rest of the block is
// its zero padding
#define FUZZ_PRIVATE_FIELD_BYTES ( 3 * 8 + 2 * ROCKETNET_KEY_BYTES + ROCKETNET_USER_DATA_BYTES )
// and the signed writer's
#define FUZZ_SIGNED_FIELD_BYTES ( ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES )

static_assert( FUZZ_PRIVATE_FIELD_BYTES <= ROCKETNET_PRIVATE_BLOCK_BYTES, "the private fields fit the block" );
static_assert( FUZZ_SIGNED_FIELD_BYTES <= ROCKETNET_SIGNED_CONNECT_DATA_BYTES, "the signed fields fit the region" );

#define FUZZ_NOW ( ROCKETNET_TEST_CREATE_TS + 100 )

static const uint8_t fuzz_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
static bool fuzz_verbose;

// copy up to bytes from the input into out, zero-filling the rest
static void fuzz_take( const uint8_t * data, size_t size, size_t * pos, uint8_t * out, size_t bytes )
{
    memset( out, 0, bytes );
    const size_t available = *pos < size ? size - *pos : 0;
    const size_t n = available < bytes ? available : bytes;
    if ( n > 0 )
        memcpy( out, data + *pos, n );
    *pos += n;
}

static bool fuzz_all_zero( const uint8_t * p, size_t bytes )
{
    for ( size_t i = 0; i < bytes; i++ )
        if ( p[i] != 0 )
            return false;
    return true;
}

// -----------------------------------------------------------------------
//  0 and 1: the layouts
// -----------------------------------------------------------------------

static void fuzz_private_layout( const uint8_t * data, size_t size )
{
    size_t pos = 0;
    static uint8_t block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    static uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    fuzz_take( data, size, &pos, block, sizeof( block ) );
    fuzz_take( data, size, &pos, signed_data, sizeof( signed_data ) );

    struct rocketnet_connect_token_private_t fields;
    memset( &fields, 0, sizeof( fields ) );
    rocketnet_read_connect_token_private( &fields, block );

    static uint8_t rewritten[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &fields, rewritten );
    rocketnet_fuzz_require( memcmp( rewritten, block, FUZZ_PRIVATE_FIELD_BYTES ) == 0, "private block write(read) reproduces its fields" );
    rocketnet_fuzz_require( fuzz_all_zero( rewritten + FUZZ_PRIVATE_FIELD_BYTES, ROCKETNET_PRIVATE_BLOCK_BYTES - FUZZ_PRIVATE_FIELD_BYTES ),
                            "private block writer zero-fills its padding" );

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    rocketnet_read_signed_connect_data( &signed_fields, signed_data );
    const bool expected = fields.protocol_id == signed_fields.protocol_id && fields.expire_timestamp == signed_fields.expire_timestamp;
    rocketnet_fuzz_require( ( rocketnet_connect_token_private_consistent( &fields, signed_data ) != 0 ) == expected,
                            "consistency is exactly protocol id and expire equal" );
}

static void fuzz_signed_layout( const uint8_t * data, size_t size )
{
    size_t pos = 0;
    static uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    fuzz_take( data, size, &pos, signed_data, sizeof( signed_data ) );

    struct rocketnet_signed_connect_data_t fields;
    memset( &fields, 0, sizeof( fields ) );
    rocketnet_read_signed_connect_data( &fields, signed_data );

    static uint8_t rewritten[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    rocketnet_write_signed_connect_data( &fields, rewritten );
    rocketnet_fuzz_require( memcmp( rewritten, signed_data, FUZZ_SIGNED_FIELD_BYTES ) == 0, "signed data write(read) reproduces its fields" );
    rocketnet_fuzz_require( fuzz_all_zero( rewritten + FUZZ_SIGNED_FIELD_BYTES, ROCKETNET_SIGNED_CONNECT_DATA_BYTES - FUZZ_SIGNED_FIELD_BYTES ),
                            "signed data writer zero-fills its padding" );
}

// -----------------------------------------------------------------------
//  2 and 3: the request ladder past the signature
// -----------------------------------------------------------------------

struct fuzz_server_t
{
    struct rocketnet_server_protocol_t protocol;
    struct rocketnet_counters_t counters;
};

// a server exactly as tools/differential/netcode_decisions.cpp builds one,
// on the development keys, advertised at the vector address
static void fuzz_server_create( struct fuzz_server_t * server )
{
    memset( server, 0, sizeof( *server ) );
    struct rocketnet_server_protocol_t * p = &server->protocol;
    p->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    p->send_rate = 10.0;
    memcpy( p->packet_filter, fuzz_filter, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( p->backend_public_key, rocketnet_dev_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( p->server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( p->server_secret_key, rocketnet_dev_server_secret_key, ROCKETNET_KEY_BYTES );
    rocketnet_address_parse( &p->advertised_ipv4, "203.0.113.10:40000" );
    for ( int i = 0; i < ROCKETNET_COOKIE_KEY_BYTES; i++ )
        p->cookie_key[i] = (uint8_t) ( 0xC0 + i );
    p->cookie_lifetime_seconds = 3;
    p->handshake_deadline_seconds = 5.0;
    p->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    p->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    p->client_slots = rocketnet_client_slots_create( NULL, 4 );
    p->acks = rocketnet_server_acks_create( NULL, 4 );
    p->reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 16 );
    p->counters = &server->counters;
    if ( !p->encryption_map || !p->used_token_history || !p->client_slots || !p->acks || !p->reassembly )
        abort();
    rocketnet_reassembly_set_begin_update( p->reassembly );
    rocketnet_server_protocol_update( p, (double) FUZZ_NOW );
}

static void fuzz_server_destroy( struct fuzz_server_t * server )
{
    struct rocketnet_server_protocol_t * p = &server->protocol;
    rocketnet_encryption_map_destroy( p->encryption_map );
    rocketnet_used_token_history_destroy( p->used_token_history );
    rocketnet_client_slots_destroy( p->client_slots );
    rocketnet_server_acks_destroy( p->acks );
    rocketnet_reassembly_set_destroy( p->reassembly );
}

// the request, through the cookie gate as a client would: an empty-cookie
// request, then the same request echoing the cookie the server returned.
// returns the ladder's final outcome
static int fuzz_ladder( struct fuzz_server_t * server, const uint8_t * signed_data, const uint8_t * signature )
{
    static const uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t ephemeral_public[ROCKETNET_KEY_BYTES];
    uint8_t ephemeral_secret[ROCKETNET_KEY_BYTES];
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
        ephemeral_secret[i] = (uint8_t) ( 0x40 + i );
    crypto_scalarmult_base( ephemeral_public, ephemeral_secret );

    static uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_write_connection_request_packet( request, fuzz_filter, NULL, salt, ephemeral_public, signed_data, signature );

    struct rocketnet_address_t from;
    rocketnet_address_parse( &from, "198.51.100.7:56789" );
    uint8_t response[256];
    int response_bytes = 0;
    int result = rocketnet_server_protocol_process_connection_request( &server->protocol, &from, request, (int) sizeof( request ),
                                                                       FUZZ_NOW, (double) FUZZ_NOW, response, &response_bytes );
    if ( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
    {
        rocketnet_fuzz_require( response_bytes == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES, "a cookie response is the cookie's size" );
        memcpy( request + ROCKETNET_PREFILTER_BYTES, response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );
        response_bytes = 0;
        result = rocketnet_server_protocol_process_connection_request( &server->protocol, &from, request, (int) sizeof( request ),
                                                                       FUZZ_NOW, (double) FUZZ_NOW, response, &response_bytes );
    }
    rocketnet_fuzz_require( response_bytes >= 0 && response_bytes <= (int) sizeof( response ), "a response fits its buffer" );
    if ( fuzz_verbose )
        printf( "ladder outcome %d\n", result );

    char report[240];
    rocketnet_fuzz_require( rocketnet_counters_conserved( &server->counters, report, sizeof( report ) ) != 0, "the server's counters are conserved" );
    return result;
}

static void fuzz_acceptance_implies( const uint8_t * signed_data, const struct fuzz_server_t * server )
{
    struct rocketnet_signed_connect_data_t f;
    memset( &f, 0, sizeof( f ) );
    rocketnet_read_signed_connect_data( &f, signed_data );
    rocketnet_fuzz_require( f.version == ROCKETNET_WIRE_VERSION, "an accepted token carries the wire version" );
    rocketnet_fuzz_require( f.protocol_id == server->protocol.protocol_id, "an accepted token names this protocol" );
    rocketnet_fuzz_require( f.expire_timestamp > FUZZ_NOW, "an accepted token has not expired" );
    rocketnet_fuzz_require( ( f.address_flags & ~( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) ) == 0, "an accepted token sets only the defined address flags" );
    rocketnet_fuzz_require( memcmp( f.server_public_key, server->protocol.server_public_key, ROCKETNET_KEY_BYTES ) == 0, "an accepted token is sealed to this server" );
}

// selector 2: fields chosen by the input, the private block minted by the
// real writer. The fields lean plausible -- each defaults to the value an
// accepted token carries and the input XORs into it -- so the ladder is
// reached past its first refusal
static void fuzz_minted( const uint8_t * data, size_t size )
{
    size_t pos = 0;
    uint8_t raw[160];
    fuzz_take( data, size, &pos, raw, sizeof( raw ) );
    const uint8_t * r = raw;

    uint64_t x[4];
    for ( int i = 0; i < 4; i++ )
    {
        x[i] = 0;
        for ( int b = 0; b < 8; b++ )
            x[i] |= (uint64_t) r[i * 8 + b] << ( 8 * b );
    }
    r += 32;

    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    memset( &sf, 0, sizeof( sf ) );
    memset( &pf, 0, sizeof( pf ) );
    sf.version = ROCKETNET_WIRE_VERSION ^ (uint32_t) ( x[0] >> 32 );
    sf.protocol_id = ROCKETNET_DEV_PROTOCOL_ID ^ x[1];
    sf.create_timestamp = ( FUZZ_NOW - 10 ) ^ x[2];
    sf.expire_timestamp = ( FUZZ_NOW + 300 ) ^ x[3];
    sf.timeout_seconds = (int32_t) ( 10u ^ (uint32_t) x[0] );
    sf.address_flags = (uint8_t) ( ROCKETNET_ADDRESS_FLAG_IPV4 ^ r[0] );
    const uint8_t advertised[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    for ( int i = 0; i < 6; i++ )
        sf.ipv4_address[i] = (uint8_t) ( advertised[i] ^ r[1 + i] );
    for ( int i = 0; i < 18; i++ )
        sf.ipv6_address[i] = r[7 + i];
    memcpy( sf.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    sf.server_public_key[0] ^= (uint8_t) ( r[25] & r[26] & 1 );     // rarely, another server's token
    pf.client_id = x[0] ^ x[1];
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        pf.client_to_server_key[i] = (uint8_t) ( 0x10 + i );
        pf.server_to_client_key[i] = (uint8_t) ( 0x50 + i );
    }
    for ( int i = 0; i < 128 - 27; i++ )
        pf.user_data[i] = r[27 + i];

    static const uint8_t ephemeral_secret[ROCKETNET_KEY_BYTES] = { 0x21 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_connect_token_deterministic( &sf, &pf, rocketnet_dev_backend_secret_key, ephemeral_secret, token ) )
        return;     // a crypto refusal, e.g. a server key that is not a curve point

    struct rocketnet_connect_token_t ct;
    rocketnet_read_connect_token( &ct, token );

    struct fuzz_server_t server;
    fuzz_server_create( &server );
    if ( fuzz_ladder( &server, ct.signed_connect_data, ct.signature ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
        fuzz_acceptance_implies( ct.signed_connect_data, &server );
    fuzz_server_destroy( &server );
}

// selector 3: raw signed bytes and a raw private block, sealed and signed
static void fuzz_raw_signed( const uint8_t * data, size_t size )
{
    size_t pos = 0;
    static uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    static uint8_t block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    fuzz_take( data, size, &pos, signed_data, sizeof( signed_data ) );
    fuzz_take( data, size, &pos, block, sizeof( block ) );

    // the server key the box seals to is the one the signed data names, so a
    // token for another key is still sealed consistently with itself
    const uint8_t * seal_key = signed_data + ROCKETNET_SIGNED_OFFSET_SERVER_KEY;
    if ( crypto_box_seal( signed_data + ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK, block, ROCKETNET_PRIVATE_BLOCK_BYTES, seal_key ) != 0 )
        return;
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    if ( crypto_sign_detached( signature, NULL, signed_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, rocketnet_dev_backend_secret_key ) != 0 )
        abort();

    struct fuzz_server_t server;
    fuzz_server_create( &server );
    if ( fuzz_ladder( &server, signed_data, signature ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
    {
        fuzz_acceptance_implies( signed_data, &server );
        // and the block that opened is the one sealed, so its consistency is
        // exactly the protocol id and expire equality the layout states
        struct rocketnet_connect_token_private_t fields;
        rocketnet_read_connect_token_private( &fields, block );
        rocketnet_fuzz_require( rocketnet_connect_token_private_consistent( &fields, signed_data ) != 0, "an accepted token's private block is consistent" );
    }
    fuzz_server_destroy( &server );
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir> writes this target's
//  seeds there from the real write paths -- a token minted with the
//  development keys, its signed data and its private block -- and the
//  replay continues as usual. ROCKETNET_FUZZ_VERBOSE=1 prints each ladder
//  outcome, so a seed can be seen to reach the challenge.
// -----------------------------------------------------------------------

static void fuzz_write_seed( const char * dir, const char * name, uint8_t selector, const uint8_t * a, size_t a_bytes, const uint8_t * b, size_t b_bytes )
{
    char path[1024];
    if ( snprintf( path, sizeof( path ), "%s/%s", dir, name ) >= (int) sizeof( path ) )
        abort(); // a truncated path would write some other file
    FILE * f = rocketnet_test_fopen( path, "wb" );
    if ( !f )
        abort();
    fwrite( &selector, 1, 1, f );
    if ( a_bytes )
        fwrite( a, 1, a_bytes, f );
    if ( b_bytes )
        fwrite( b, 1, b_bytes, f );
    fclose( f );
}

static void fuzz_write_seeds( const char * dir )
{
    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    memset( &sf, 0, sizeof( sf ) );
    memset( &pf, 0, sizeof( pf ) );
    sf.version = ROCKETNET_WIRE_VERSION;
    sf.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    sf.create_timestamp = FUZZ_NOW - 10;
    sf.expire_timestamp = FUZZ_NOW + 300;
    sf.timeout_seconds = 10;
    sf.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t advertised[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( sf.ipv4_address, advertised, 6 );
    memcpy( sf.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    pf.client_id = 7;
    pf.protocol_id = sf.protocol_id;
    pf.expire_timestamp = sf.expire_timestamp;
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        pf.client_to_server_key[i] = (uint8_t) ( 0x10 + i );
        pf.server_to_client_key[i] = (uint8_t) ( 0x50 + i );
    }
    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
        pf.user_data[i] = (uint8_t) i;

    static const uint8_t ephemeral_secret[ROCKETNET_KEY_BYTES] = { 0x21 };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_connect_token_deterministic( &sf, &pf, rocketnet_dev_backend_secret_key, ephemeral_secret, token ) )
        abort();
    uint8_t block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &pf, block );

    fuzz_write_seed( dir, "private_minted_block", 0, block, sizeof( block ), token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES );
    fuzz_write_seed( dir, "signed_minted_data", 1, token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, NULL, 0 );
    fuzz_write_seed( dir, "minted_accepted_defaults", 2, NULL, 0, NULL, 0 );
    fuzz_write_seed( dir, "raw_signed_minted", 3, token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, block, sizeof( block ) );
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;
    if ( sodium_init() < 0 )
        abort();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    char verbose[8];
    fuzz_verbose = rocketnet_test_getenv( "ROCKETNET_FUZZ_VERBOSE", verbose, sizeof( verbose ) ) && verbose[0] == '1';
    char seed_dir[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
        fuzz_write_seeds( seed_dir );
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    if ( size < 1 )
        return 0;
    const uint8_t * body = data + 1;
    const size_t body_size = size - 1;
    switch ( data[0] & 3 )
    {
        case 0: fuzz_private_layout( body, body_size ); break;
        case 1: fuzz_signed_layout( body, body_size ); break;
        case 2: fuzz_minted( body, body_size ); break;
        case 3: fuzz_raw_signed( body, body_size ); break;
    }
    return 0;
}
