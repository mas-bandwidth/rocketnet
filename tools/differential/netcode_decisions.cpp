/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Cross-implementation differential: the token/server admission DECISION.

    The decision machinery (the case model, the rocketnet decider, the
    netcode 1.02 live decider and the verdict) lives in decide.h, shared with
    the libFuzzer target tests/fuzz/rocketnet_fuzz_differential.cpp so both run
    the same deciders. This file is the fixed gate: for each case in
    cases.tsv it drives rocketnet's connect-token admission path in process
    (the token open and the server's connection request ladder, with time
    passed in as an argument) and compares the accept/refuse decision against
    the netcode 1.02 reference decision. The two libraries' wire formats
    differ, so the bytes are never compared -- only the decision and the
    refusal reason class.

    The netcode side has two modes:

      * reference -- the decision recorded in cases.tsv, taken from the
        netcode 1.02 source (mas-bandwidth/netcode at the pinned tag). This
        runs anywhere and is what the table is diffed against by default.

      * live -- when built with -DROCKETNET_DIFFERENTIAL_NETCODE_DIR, the
        same decision is computed by driving the netcode 1.02 library
        (mas-bandwidth/netcode tag v1.2.1, version info "NETCODE 1.02"):
        mint a connect token, let a client emit the connection request,
        and hand that packet to a server that does not open a socket.
        The harness reports when the live answer disagrees with the
        reference column.

    Verdicts:

      agree            both libraries reach the same decision
      rocketnet-weaker netcode refuses, rocketnet admits (a finding)
      rocketnet-stricter rocketnet refuses, netcode admits (judged against
                        REQUIREMENTS.md: no property weaker than netcode 1.02)
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "decide.h"

// --- the case table -----------------------------------------------------

// One row per case, in the order cases.tsv lists them. The id names the
// mutation this harness applies to the minted token before admission.
enum
{
    CASE_VALID_SIGNATURE = 0,
    CASE_EXPIRED_TOKEN,
    CASE_BAD_SIGNATURE,
    CASE_WRONG_SERVER_ADDRESS,
    CASE_REPLAYED_TOKEN,
    CASE_FULL_SERVER,
    CASE_WRONG_PROTOCOL_ID,
    CASE_TIMEOUT_OUT_OF_RANGE,
    CASE_COUNT
};

struct case_row_t
{
    const char * id;
    const char * description;
    const char * netcode_reference;      // accept, or refuse:<class>
};

static struct case_row_t g_cases[CASE_COUNT];

// Each fixed case maps to one abstract differential case (decide.h). The
// default is a valid token; the switch applies the case's mutation.
static void differential_case_for_id( int case_id, struct differential_case_t * c )
{
    memset( c, 0, sizeof( *c ) );
    c->create_offset = -10;
    c->expire_offset = 300;
    c->timeout_seconds = 10;

    switch ( case_id )
    {
        case CASE_EXPIRED_TOKEN:
            c->expire_offset = -1;
            break;
        case CASE_WRONG_PROTOCOL_ID:
            c->protocol_wrong = 1;
            break;
        case CASE_TIMEOUT_OUT_OF_RANGE:
            c->timeout_seconds = 0;
            break;
        case CASE_WRONG_SERVER_ADDRESS:
            c->address_mode = 1;
            break;
        case CASE_BAD_SIGNATURE:
            c->flip_authenticator = 1;
            break;
        case CASE_FULL_SERVER:
            c->occupied_slots = 8;
            break;
        case CASE_REPLAYED_TOKEN:
            c->replay = 1;
            break;
        default:
            break;
    }
}

// --- the fuzz corpus mode -----------------------------------------------------
//
// Each tests/fuzz/corpus_connect_token file is read the way
// tests/fuzz/rocketnet_fuzz_connect_token.cpp reads it: byte 0 bit 0 picks the raw
// half (the rest of the input is the token, zero-padded to 768 bytes) or the
// structured half (the rest is (u16 offset, xor) records over a valid
// baseline token); byte 1, as a signed count of minutes, moves the clock
// against the token's fixed validity window (created at
// ROCKETNET_TEST_CREATE_TS, expiring 300 seconds later). Here the clock is the
// server's.
//
// A structured mutation is mapped by the token FIELD each changed byte lands
// in (measured against the baseline, so flips that cancel change nothing) to
// the same relative byte of the equivalent netcode 1.02 field, with the same
// xor. Fields netcode 1.02 does not carry where the server reads them are
// not comparable, and a case that touches one is reported as such rather
// than guessed:
//
//   rocketnet field                 netcode 1.02 equivalent
//   version         [0,4)           request version info, byte for byte
//   protocol id     [4,12)          request protocol id
//   create          [12,20)         -- (the request carries no create time)
//   expire          [20,28)         request expire timestamp
//   timeout         [28,32)         -- (sealed inside the private token)
//   address         [32,57)         -- (sealed inside the private token)
//   server key      [57,89)         -- (netcode has no server public key)
//   sealed block    [89,521)        request encrypted private token, same offset
//   signed pad      [521,576)       --
//   signature       [576,640)       the private token's MAC, offset mod 16
//   clear keys/pad  [640,768)       the public token's client keys and pad,
//                                   before the client writes its request
//
// The clock maps to netcode's expire_seconds (300 minus the offset, zero when
// that is not positive): netcode stamps and checks expiry on its own wall
// clock and keeps no create time, so a token "not yet created" is simply a
// valid one there. The raw half maps to a netcode request whose bytes after
// the packet type are the same input bytes, zero-padded.

enum
{
    FIELD_VERSION = 0,
    FIELD_PROTOCOL,
    FIELD_CREATE,
    FIELD_EXPIRE,
    FIELD_TIMEOUT,
    FIELD_ADDRESS,
    FIELD_SERVER_KEY,
    FIELD_SEALED,
    FIELD_SIGNED_PAD,
    FIELD_SIGNATURE,
    FIELD_CLEAR,
    FIELD_COUNT
};

static const char * const g_field_names[FIELD_COUNT] = { "version", "protocol", "create", "expire", "timeout", "address", "server-key", "sealed", "signed-pad", "signature", "clear" };
static const int g_field_comparable[FIELD_COUNT] = { 1, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1 };

static int token_field( int offset )
{
    if ( offset < ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID )
        return FIELD_VERSION;
    if ( offset < ROCKETNET_SIGNED_OFFSET_CREATE )
        return FIELD_PROTOCOL;
    if ( offset < ROCKETNET_SIGNED_OFFSET_EXPIRE )
        return FIELD_CREATE;
    if ( offset < ROCKETNET_SIGNED_OFFSET_TIMEOUT )
        return FIELD_EXPIRE;
    if ( offset < ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS )
        return FIELD_TIMEOUT;
    if ( offset < ROCKETNET_SIGNED_OFFSET_SERVER_KEY )
        return FIELD_ADDRESS;
    if ( offset < ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK )
        return FIELD_SERVER_KEY;
    if ( offset < ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES )
        return FIELD_SEALED;
    if ( offset < ROCKETNET_SIGNED_CONNECT_DATA_BYTES )
        return FIELD_SIGNED_PAD;
    if ( offset < ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES )
        return FIELD_SIGNATURE;

    return FIELD_CLEAR;
}

#define CORPUS_BASE_WINDOW_SECONDS 300

struct corpus_case_t
{
    char name[128];
    int raw;
    int64_t clock_offset;                           // seconds, the server clock against the token's create time
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];    // the rocketnet token as the harness builds it
    uint8_t delta[ROCKETNET_CONNECT_TOKEN_BYTES];    // structured: token xor baseline
    int raw_bytes;                                  // raw: how many input bytes the token holds
    unsigned touched;                               // structured: bit per field class changed
};

static uint8_t g_corpus_base[ROCKETNET_CONNECT_TOKEN_BYTES];

// the fuzz harness's baseline, minted to the differential server's test keys:
// the same field values, so every offset lands in the same field
static int corpus_build_base()
{
    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    memset( &sf, 0, sizeof( sf ) );
    memset( &pf, 0, sizeof( pf ) );
    sf.version = ROCKETNET_WIRE_VERSION;
    sf.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    sf.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    sf.expire_timestamp = ROCKETNET_TEST_CREATE_TS + CORPUS_BASE_WINDOW_SECONDS;
    sf.timeout_seconds = 10;
    sf.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 10, 40000 );
    memcpy( sf.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    pf.client_id = 1;

    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        pf.client_to_server_key[i] = (uint8_t) ( 0x10 + i );
        pf.server_to_client_key[i] = (uint8_t) ( 0x50 + i );
    }

    for ( int i = 0; i < ROCKETNET_USER_DATA_BYTES; i++ )
    {
        pf.user_data[i] = (uint8_t) i;
    }

    return rocketnet_write_connect_token( &sf, &pf, rocketnet_test_backend_secret_key, g_corpus_base );
}

static int corpus_read( const char * path, struct corpus_case_t * c )
{
    memset( c, 0, sizeof( *c ) );

    const char * slash = strrchr( path, '/' );
    snprintf( c->name, sizeof( c->name ), "%s", slash ? slash + 1 : path );

    FILE * f = fopen( path, "rb" );
    if ( !f )
        return 0;

    uint8_t data[4096];
    const size_t size = fread( data, 1, sizeof( data ), f );

    fclose( f );
    if ( size < 2 )
        return 0;

    c->raw = data[0] & 1;
    c->clock_offset = (int64_t) (int8_t) data[1] * 60;

    const uint8_t * body = data + 2;
    size_t body_size = size - 2;

    if ( c->raw )
    {
        if ( body_size > ROCKETNET_CONNECT_TOKEN_BYTES )
        {
            body_size = ROCKETNET_CONNECT_TOKEN_BYTES;
        }

        memcpy( c->token, body, body_size );
        c->raw_bytes = (int) body_size;
        return 1;
    }

    memcpy( c->token, g_corpus_base, sizeof( c->token ) );

    for ( size_t offset = 0; offset + 3 <= body_size; offset += 3 )
    {
        const int position = (int) ( ( (size_t) body[offset] | ( (size_t) body[offset + 1] << 8 ) ) % ROCKETNET_CONNECT_TOKEN_BYTES );
        c->token[position] ^= body[offset + 2];
    }

    for ( int i = 0; i < ROCKETNET_CONNECT_TOKEN_BYTES; i++ )
    {
        c->delta[i] = (uint8_t) ( c->token[i] ^ g_corpus_base[i] );
        if ( c->delta[i] )
        {
            c->touched |= 1u << token_field( i );
        }
    }

    return 1;
}

static int corpus_comparable( const struct corpus_case_t * c )
{
    for ( int field = 0; field < FIELD_COUNT; field++ )
        if ( ( c->touched & ( 1u << field ) ) && !g_field_comparable[field] )
            return 0;
    return 1;
}

static void corpus_describe( const struct corpus_case_t * c, char * out, size_t out_size )
{
    char fields[160] = "";

    if ( c->raw )
    {
        snprintf( fields, sizeof( fields ), "raw" );
    }
    else if ( !c->touched )
    {
        snprintf( fields, sizeof( fields ), "untouched" );
    }
    else
    {
        for ( int field = 0; field < FIELD_COUNT; field++ )
        {
            if ( !( c->touched & ( 1u << field ) ) )
                continue;

            const size_t used = strlen( fields );
            snprintf( fields + used, sizeof( fields ) - used, "%s%s", used ? "+" : "", g_field_names[field] );
        }
    }

    snprintf( out, out_size, "%s,clock%+lldm", fields, (long long) ( c->clock_offset / 60 ) );
}

static struct decision_t rocketnet_decide_corpus( const struct corpus_case_t * c )
{
    struct decision_t d;
    memset( &d, 0, sizeof( d ) );

    const uint64_t now = (uint64_t) ( (int64_t) ROCKETNET_TEST_CREATE_TS + c->clock_offset );

    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    rocketnet_server_setup( &server, &counters, now );

    struct rocketnet_connect_token_t ct;
    rocketnet_read_connect_token( &ct, c->token );
    rocketnet_admit_request( &server, &counters, ct.signed_connect_data, ct.signature, now, &d );

    rocketnet_server_teardown( &server );

    return d;
}

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

// the netcode 1.02 request: type, 13 bytes of version info, protocol id,
// expire timestamp, 24-byte nonce, then the 1024-byte encrypted private
// token whose last 16 bytes are its MAC
enum
{
    NC_REQUEST_VERSION = 1,
    NC_REQUEST_PROTOCOL = 14,
    NC_REQUEST_EXPIRE = 22,
    NC_REQUEST_PRIVATE = 54,
    NC_REQUEST_BYTES = 1078,
    NC_REQUEST_MAC = NC_REQUEST_BYTES - NETCODE_MAC_BYTES,
    // in the public token, after version, protocol, create, expire, nonce,
    // private data, timeout, the server count and one IPv4 entry (7 bytes)
    NC_TOKEN_CLIENT_KEYS = 13 + 8 + 8 + 8 + 24 + 1024 + 4 + 4 + 7
};

static int netcode_decide_corpus( const struct corpus_case_t * c, char * reason, size_t reason_size )
{
    uint8_t private_key[NETCODE_KEY_BYTES];
    uint8_t token[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t request[NC_PIPE_BYTES];
    int request_bytes = 0;
    struct netcode_address_t from;

    nc_private_key( private_key );

    struct netcode_server_t * server = nc_open_server( NC_SERVER_ADDRESS, 8, NC_PROTOCOL_ID, private_key );
    struct netcode_client_t * client = nc_open_client( NC_CLIENT_ADDRESS );

    int64_t expire_seconds = CORPUS_BASE_WINDOW_SECONDS - c->clock_offset;
    if ( expire_seconds < 0 )
    {
        expire_seconds = 0;
    }

    if ( !server || !client || !nc_mint( token, NC_SERVER_ADDRESS, (int) expire_seconds, 10, 1, NC_PROTOCOL_ID, private_key ) )
    {
        if ( client )
        {
            netcode_client_destroy( client );
        }

        if ( server )
        {
            netcode_server_destroy( server );
        }

        snprintf( reason, reason_size, "refuse:mint" );
        return 0;
    }

    if ( !c->raw )
        for ( int i = ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES; i < ROCKETNET_CONNECT_TOKEN_BYTES; i++ )
        {
            token[NC_TOKEN_CLIENT_KEYS + ( i - ( ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES ) )] ^= c->delta[i];
        }

    netcode_client_connect( client, token );
    if ( netcode_client_state( client ) != NETCODE_CLIENT_STATE_SENDING_CONNECTION_REQUEST ||
         !nc_capture_request( client, request, &request_bytes ) || request_bytes != NC_REQUEST_BYTES ||
         !nc_parse( &from, NC_CLIENT_ADDRESS ) )
    {
        netcode_client_destroy( client );
        netcode_server_destroy( server );
        snprintf( reason, reason_size, "refuse:mint" );
        return 0;
    }

    netcode_client_destroy( client );

    if ( c->raw )
    {
        memset( request + 1, 0, NC_REQUEST_BYTES - 1 );
        memcpy( request + 1, c->token, (size_t) ( c->raw_bytes < NC_REQUEST_BYTES - 1 ? c->raw_bytes : NC_REQUEST_BYTES - 1 ) );
    }
    else
    {
        for ( int i = 0; i < ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES; i++ )
        {
            if ( !c->delta[i] )
                continue;

            switch ( token_field( i ) )
            {
                case FIELD_VERSION:   request[NC_REQUEST_VERSION + i] ^= c->delta[i]; break;
                case FIELD_PROTOCOL:  request[NC_REQUEST_PROTOCOL + ( i - ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID )] ^= c->delta[i]; break;
                case FIELD_EXPIRE:    request[NC_REQUEST_EXPIRE + ( i - ROCKETNET_SIGNED_OFFSET_EXPIRE )] ^= c->delta[i]; break;
                case FIELD_SEALED:    request[NC_REQUEST_PRIVATE + ( i - ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK )] ^= c->delta[i]; break;
                case FIELD_SIGNATURE: request[NC_REQUEST_MAC + ( i - ROCKETNET_SIGNED_CONNECT_DATA_BYTES ) % NETCODE_MAC_BYTES] ^= c->delta[i]; break;

                default: break;     // not comparable; the caller never asks
            }
        }
    }

    nc_feed( server, &from, request, request_bytes );
    nc_classify( reason, reason_size );
    netcode_server_destroy( server );

    return strncmp( reason, "accept", 6 ) == 0;
}

#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

// the corpus reference: file name, then the netcode 1.02 decision the live
// run recorded for it
struct corpus_reference_t
{
    char name[128];
    char decision[64];
};

static struct corpus_reference_t g_corpus_reference[256];
static int g_corpus_reference_count = 0;

static void corpus_read_reference( const char * path )
{
    FILE * f = fopen( path, "rb" );
    if ( !f )
        return;

    char line[512];
    while ( fgets( line, sizeof( line ), f ) && g_corpus_reference_count < 256 )
    {
        if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' )
            continue;
        line[strcspn( line, "\r\n" )] = 0;

        char * tab = strchr( line, '\t' );
        if ( !tab )
            continue;
        *tab++ = 0;

        char * tab2 = strchr( tab, '\t' );
        const char * decision = tab2 ? tab2 + 1 : tab;
        struct corpus_reference_t * r = &g_corpus_reference[g_corpus_reference_count++];

        {
            size_t len = strlen( line );
            if ( len >= sizeof( r->name ) )
                len = sizeof( r->name ) - 1;
            memcpy( r->name, line, len );
            r->name[len] = 0;
        }
        snprintf( r->decision, sizeof( r->decision ), "%s", decision );
    }

    fclose( f );
}

static const char * corpus_reference_for( const char * name )
{
    for ( int i = 0; i < g_corpus_reference_count; i++ )
        if ( strcmp( g_corpus_reference[i].name, name ) == 0 )
            return g_corpus_reference[i].decision;
    return NULL;
}

static int run_corpus( const char * reference_path, int count, const char ** paths )
{
    if ( !corpus_build_base() )
    {
        fprintf( stderr, "could not mint the corpus baseline token\n" );
        return 1;
    }

    corpus_read_reference( reference_path );

    int weaker = 0;

    printf( "# corpus\tfields\tnetcode\trocketnet\tverdict\n" );
    for ( int i = 0; i < count; i++ )
    {
        static struct corpus_case_t c;
        if ( !corpus_read( paths[i], &c ) )
        {
            printf( "%s\t-\t-\t-\tunreadable\n", c.name );
            continue;
        }

        char fields[192];
        corpus_describe( &c, fields, sizeof( fields ) );

        const struct decision_t rn = rocketnet_decide_corpus( &c );
        char rocketnet[72];
        if ( rn.accept )
        {
            strcpy( rocketnet, "accept" );
        }
        else
        {
            snprintf( rocketnet, sizeof( rocketnet ), "refuse:%s", rn.reason );
        }

        if ( !corpus_comparable( &c ) )
        {
            printf( "%s\t%s\tnot-comparable\t%s\tnot-comparable\n", c.name, fields, rocketnet );
            continue;
        }

        const char * reference = corpus_reference_for( c.name );
        const char * netcode = reference;
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
        static char live[64];
        netcode_decide_corpus( &c, live, sizeof( live ) );
        if ( reference && strcmp( reference, live ) != 0 )
        {
            fprintf( stderr, "live netcode disagrees with the corpus reference on %s (%s, reference %s)\n", c.name, live, reference );
        }

        netcode = live;
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
        if ( !netcode )
        {
            printf( "%s\t%s\tneeds-NETCODE_DIR\t%s\tnot-run\n", c.name, fields, rocketnet );
            continue;
        }

        const char * v = verdict( &rn, netcode );
        if ( strcmp( v, "rocketnet-weaker" ) == 0 )
        {
            weaker++;
        }

        printf( "%s\t%s\t%s\t%s\t%s\n", c.name, fields, netcode, rocketnet, v );
    }

    if ( weaker )
    {
        fprintf( stderr, "FINDING: %d corpus case(s) where netcode 1.02 refuses and rocketnet admits\n", weaker );
    }

    return 0;
}

// --- transport --------------------------------------------------------------

static int parse_case_table( const char * path )
{
    FILE * f = fopen( path, "rb" );
    if ( !f )
        return -1;

    char line[512];
    int n = 0;
    while ( fgets( line, sizeof( line ), f ) )
    {
        if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' )
            continue;

        char * nl = strchr( line, '\n' );
        if ( nl )
        {
            *nl = 0;
        }

        nl = strchr( line, '\r' );
        if ( nl )
        {
            *nl = 0;
        }

        char * id = line;
        char * t1 = strchr( id, '\t' );
        if ( !t1 )
            continue;
        *t1++ = 0;

        char * description = t1;
        char * t2 = strchr( description, '\t' );
        if ( !t2 )
            continue;
        *t2++ = 0;

        char * reference = t2;

        if ( n < CASE_COUNT )
        {
            g_cases[n].id = strdup( id );
            g_cases[n].description = strdup( description );
            g_cases[n].netcode_reference = strdup( reference );
            n++;
        }
    }

    fclose( f );

    return n;
}

static void free_case_table()
{
    for ( int i = 0; i < CASE_COUNT; i++ )
    {
        free( (void *) g_cases[i].id );
        free( (void *) g_cases[i].description );
        free( (void *) g_cases[i].netcode_reference );
        g_cases[i].id = NULL;
        g_cases[i].description = NULL;
        g_cases[i].netcode_reference = NULL;
    }
}

int main( int argc, const char ** argv )
{
    const char * cases_path = "tools/differential/cases.tsv";

    if ( argc > 1 )
    {
        cases_path = argv[1];
    }

    // netcode_decisions <cases.tsv> [--corpus <corpus_cases.tsv> <corpus file>...]

    int corpus_at = -1;

    for ( int i = 2; i + 1 < argc; i++ )
        if ( strcmp( argv[i], "--corpus" ) == 0 )
        {
            corpus_at = i;
        }

    rocketnet_quiet( true );

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    netcode_log_level( NETCODE_LOG_LEVEL_DEBUG );
    netcode_set_printf_function( netcode_log_sink );

    if ( netcode_init() != NETCODE_OK )
    {
        fprintf( stderr, "netcode_init failed\n" );
        return 1;
    }
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

    if ( parse_case_table( cases_path ) != CASE_COUNT )
    {
        fprintf( stderr, "could not read %d cases from %s\n", CASE_COUNT, cases_path );
        return 1;
    }

    const uint64_t now = ROCKETNET_TEST_CREATE_TS;

    printf( "# case\tnetcode\trocketnet\tverdict\n" );

    for ( int i = 0; i < CASE_COUNT; i++ )
    {
        struct differential_case_t c;
        differential_case_for_id( i, &c );

        struct decision_t rn = rocketnet_decide_case( &c, now );

        const char * netcode = g_cases[i].netcode_reference;
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
        char live_reason[64];
        if ( netcode_decide_case_live( &c, now, live_reason, sizeof( live_reason ) ) >= 0 )
        {
            if ( strcmp( live_reason, g_cases[i].netcode_reference ) != 0 )
            {
                fprintf( stderr, "live netcode disagrees with the reference column on %s\n", g_cases[i].id );
            }

            netcode = live_reason;
        }
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

        char rocketnet[72];

        if ( rn.accept )
        {
            strcpy( rocketnet, "accept" );
        }
        else
        {
            snprintf( rocketnet, sizeof( rocketnet ), "refuse:%s", rn.reason );
        }

        printf( "%s\t%s\t%s\t%s\n", g_cases[i].id, netcode, rocketnet,
                verdict( &rn, netcode ) );
    }

    int status = 0;

    if ( corpus_at > 0 )
    {
        printf( "\n" );
        status = run_corpus( argv[corpus_at + 1], argc - corpus_at - 2, argv + corpus_at + 2 );
    }

    free_case_table();
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    netcode_term();
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    return status;
}
