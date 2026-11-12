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
      not-comparable   the case cannot be put to netcode 1.02 (a raw corpus
                        file, or a structured one touching a field netcode
                        does not carry); only rocketnet's decision is checked

    Every row also records the decision rocketnet is expected to reach (the
    last column of cases.tsv and corpus_cases.tsv), and the live decision is
    held to it, so a removed refusal shows even where netcode cannot see it
    (a permitted-stricter row turning agree, a not-comparable row admitted).

    Exit status: 0 only when every row passes; 1 when any row is
    rocketnet-weaker, rocketnet-stricter in a class the README does not
    permit, differs from rocketnet's expected decision, has no reference row
    (not-run), is unreadable, or (live) where live netcode disagrees with the
    reference column; when a reference row names no corpus file; and when no
    row was compared with netcode at all. The last line printed is the tally
    and PASS or FAIL.
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
    const char * rocketnet_expected;     // the decision rocketnet must reach
};

static struct case_row_t g_cases[CASE_COUNT];

// --- the tally ------------------------------------------------------------

struct tally_t
{
    int compared;                // rows judged against a netcode decision
    int agree;
    int stricter_permitted;
    int not_comparable;          // rocketnet's decision checked alone
    int failures;                // every row or file that fails the gate
};

static struct tally_t g_tally;

// one row's checks: the verdict when netcode was asked (netcode non-NULL),
// and rocketnet's decision against the one the row expects. Prints the reason
// for each failure on stderr.

static void tally_row( const char * name, const struct decision_t * rn, const char * rocketnet, const char * netcode, const char * expected )
{
    if ( netcode )
    {
        g_tally.compared++;
        const char * v = verdict( rn, netcode );
        if ( strcmp( v, "agree" ) == 0 )
        {
            g_tally.agree++;
        }
        else if ( strcmp( v, "rocketnet-weaker" ) == 0 )
        {
            fprintf( stderr, "FAIL %s: rocketnet-weaker: netcode 1.02 %s, rocketnet admits\n", name, netcode );
            g_tally.failures++;
        }
        else if ( stricter_class_permitted( rn->reason ) )
        {
            g_tally.stricter_permitted++;
        }
        else
        {
            fprintf( stderr, "FAIL %s: rocketnet-stricter in a class the README does not permit (%s)\n", name, rocketnet );
            g_tally.failures++;
        }
    }
    else
    {
        g_tally.not_comparable++;
    }

    if ( !expected )
    {
        fprintf( stderr, "FAIL %s: no expected rocketnet decision recorded\n", name );
        g_tally.failures++;
    }
    else if ( strcmp( expected, rocketnet ) != 0 )
    {
        fprintf( stderr, "FAIL %s: rocketnet decided %s, the row expects %s\n", name, rocketnet, expected );
        g_tally.failures++;
    }
}

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
    {
        return FIELD_VERSION;
    }
    if ( offset < ROCKETNET_SIGNED_OFFSET_CREATE )
    {
        return FIELD_PROTOCOL;
    }
    if ( offset < ROCKETNET_SIGNED_OFFSET_EXPIRE )
    {
        return FIELD_CREATE;
    }
    if ( offset < ROCKETNET_SIGNED_OFFSET_TIMEOUT )
    {
        return FIELD_EXPIRE;
    }
    if ( offset < ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS )
    {
        return FIELD_TIMEOUT;
    }
    if ( offset < ROCKETNET_SIGNED_OFFSET_SERVER_KEY )
    {
        return FIELD_ADDRESS;
    }
    if ( offset < ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK )
    {
        return FIELD_SERVER_KEY;
    }
    if ( offset < ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES )
    {
        return FIELD_SEALED;
    }
    if ( offset < ROCKETNET_SIGNED_CONNECT_DATA_BYTES )
    {
        return FIELD_SIGNED_PAD;
    }
    if ( offset < ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES )
    {
        return FIELD_SIGNATURE;
    }

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
    {
        return 0;
    }

    uint8_t data[4096];
    const size_t size = fread( data, 1, sizeof( data ), f );

    fclose( f );
    if ( size < 2 )
    {
        return 0;
    }

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

// A raw file is never comparable: its bytes land in netcode's request from
// the version info on, so netcode refuses every one of them as "not a
// netcode packet" (refuse:version) whatever the token holds, and a verdict
// against that constant says nothing about either library's token checks
// (DIFFERENTIAL-JUDGEMENT, 2026-11-12: the valid-token seed read as
// rocketnet-weaker, the other raw rows as empty agreements).

static int corpus_comparable( const struct corpus_case_t * c )
{
    if ( c->raw )
    {
        return 0;
    }

    for ( int field = 0; field < FIELD_COUNT; field++ )
    {
        if ( ( c->touched & ( 1u << field ) ) && !g_field_comparable[field] )
        {
            return 0;
        }
    }
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
            {
                continue;
            }

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
    {
        for ( int i = ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES; i < ROCKETNET_CONNECT_TOKEN_BYTES; i++ )
        {
            token[NC_TOKEN_CLIENT_KEYS + ( i - ( ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES ) )] ^= c->delta[i];
        }
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
            {
                continue;
            }

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

// the corpus reference: file name, the fields the mapping reads, the netcode
// 1.02 decision the live run recorded, and the decision rocketnet must reach
struct corpus_reference_t
{
    char name[128];
    char netcode[64];
    char rocketnet[64];
    int seen;
};

static struct corpus_reference_t g_corpus_reference[256];
static int g_corpus_reference_count = 0;

static void corpus_copy_cell( char * out, size_t out_size, const char * cell )
{
    size_t len = strlen( cell );
    if ( len >= out_size )
    {
        len = out_size - 1;
    }
    memcpy( out, cell, len );
    out[len] = 0;
}

// returns the row count, or -1 when the file cannot be read or a row does not
// have its four columns

static int corpus_read_reference( const char * path )
{
    FILE * f = fopen( path, "rb" );
    if ( !f )
    {
        return -1;
    }

    int status = 0;
    char line[512];
    while ( fgets( line, sizeof( line ), f ) )
    {
        if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' )
        {
            continue;
        }
        line[strcspn( line, "\r\n" )] = 0;

        char * cells[4];
        int n = 0;
        char * p = line;
        while ( n < 4 && p )
        {
            cells[n++] = p;
            char * tab = strchr( p, '\t' );
            if ( tab )
            {
                *tab = 0;
                p = tab + 1;
            }
            else
            {
                p = NULL;
            }
        }

        if ( n != 4 || p || g_corpus_reference_count >= 256 )
        {
            fprintf( stderr, "FAIL %s: a row without exactly four columns, or more than 256 rows\n", path );
            status = -1;
            break;
        }

        struct corpus_reference_t * r = &g_corpus_reference[g_corpus_reference_count++];
        corpus_copy_cell( r->name, sizeof( r->name ), cells[0] );
        corpus_copy_cell( r->netcode, sizeof( r->netcode ), cells[2] );
        corpus_copy_cell( r->rocketnet, sizeof( r->rocketnet ), cells[3] );
        r->seen = 0;
    }

    fclose( f );
    return status == 0 ? g_corpus_reference_count : -1;
}

static struct corpus_reference_t * corpus_reference_for( const char * name )
{
    for ( int i = 0; i < g_corpus_reference_count; i++ )
    {
        if ( strcmp( g_corpus_reference[i].name, name ) == 0 )
        {
            return &g_corpus_reference[i];
        }
    }
    return NULL;
}

static void run_corpus( const char * reference_path, int count, const char ** paths )
{
    if ( !corpus_build_base() )
    {
        fprintf( stderr, "FAIL corpus: could not mint the corpus baseline token\n" );
        g_tally.failures++;
        return;
    }

    if ( corpus_read_reference( reference_path ) < 0 )
    {
        fprintf( stderr, "FAIL corpus: could not read %s\n", reference_path );
        g_tally.failures++;
        return;
    }

    printf( "# corpus\tfields\tnetcode\trocketnet\tverdict\n" );
    for ( int i = 0; i < count; i++ )
    {
        static struct corpus_case_t c;
        if ( !corpus_read( paths[i], &c ) )
        {
            printf( "%s\t-\t-\t-\tunreadable\n", c.name );
            fprintf( stderr, "FAIL %s: unreadable\n", c.name );
            g_tally.failures++;
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

        struct corpus_reference_t * reference = corpus_reference_for( c.name );
        if ( !reference )
        {
            printf( "%s\t%s\tno-reference-row\t%s\tnot-run\n", c.name, fields, rocketnet );
            fprintf( stderr, "FAIL %s: no row in %s (add the file's netcode and rocketnet decisions)\n", c.name, reference_path );
            g_tally.failures++;
            continue;
        }
        reference->seen = 1;

        if ( !corpus_comparable( &c ) )
        {
            printf( "%s\t%s\tnot-comparable\t%s\tnot-comparable\n", c.name, fields, rocketnet );
            tally_row( c.name, &rn, rocketnet, NULL, reference->rocketnet );
            continue;
        }

        const char * netcode = reference->netcode;
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
        static char live[64];
        netcode_decide_corpus( &c, live, sizeof( live ) );
        if ( strcmp( reference->netcode, live ) != 0 )
        {
            fprintf( stderr, "FAIL %s: live netcode disagrees with the corpus reference (%s, reference %s)\n", c.name, live, reference->netcode );
            g_tally.failures++;
        }

        netcode = live;
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

        printf( "%s\t%s\t%s\t%s\t%s\n", c.name, fields, netcode, rocketnet, verdict( &rn, netcode ) );
        tally_row( c.name, &rn, rocketnet, netcode, reference->rocketnet );
    }

    for ( int i = 0; i < g_corpus_reference_count; i++ )
    {
        if ( !g_corpus_reference[i].seen )
        {
            fprintf( stderr, "FAIL %s: a row in %s names no corpus file given\n", g_corpus_reference[i].name, reference_path );
            g_tally.failures++;
        }
    }
}

// --- transport --------------------------------------------------------------


// the case ids, in the order of the enum above: a table row in another order
// would be run as another case, so the order is checked, not assumed

static const char * const g_case_ids[CASE_COUNT] = {
    "valid_signature",
    "expired_token",
    "bad_signature",
    "wrong_server_address",
    "replayed_token",
    "full_server",
    "wrong_protocol_id",
    "timeout_out_of_range",
};

// returns the row count (every row, so an extra row is seen), or -1 when the
// file cannot be read or a row does not have its four columns

static int parse_case_table( const char * path )
{
    FILE * f = fopen( path, "rb" );
    if ( !f )
    {
        return -1;
    }

    char line[512];
    int n = 0;
    while ( fgets( line, sizeof( line ), f ) )
    {
        if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' )
        {
            continue;
        }
        line[strcspn( line, "\r\n" )] = 0;

        char * cells[4];
        int count = 0;
        char * p = line;
        while ( count < 4 && p )
        {
            cells[count++] = p;
            char * tab = strchr( p, '\t' );
            if ( tab )
            {
                *tab = 0;
                p = tab + 1;
            }
            else
            {
                p = NULL;
            }
        }

        if ( count != 4 || p )
        {
            fprintf( stderr, "FAIL %s: a row without exactly four columns\n", path );
            fclose( f );
            return -1;
        }

        if ( n < CASE_COUNT )
        {
            g_cases[n].id = strdup( cells[0] );
            g_cases[n].description = strdup( cells[1] );
            g_cases[n].netcode_reference = strdup( cells[2] );
            g_cases[n].rocketnet_expected = strdup( cells[3] );
        }
        n++;
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
        free( (void *) g_cases[i].rocketnet_expected );
        g_cases[i].id = NULL;
        g_cases[i].description = NULL;
        g_cases[i].netcode_reference = NULL;
        g_cases[i].rocketnet_expected = NULL;
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
    {
        if ( strcmp( argv[i], "--corpus" ) == 0 )
        {
            corpus_at = i;
        }
    }

    rocketnet_quiet( true );

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    netcode_log_level( NETCODE_LOG_LEVEL_DEBUG );
    netcode_set_printf_function( netcode_log_sink );

    if ( netcode_init() != NETCODE_OK )
    {
        fprintf( stderr, "netcode_init failed\n" );
        printf( "differential: FAIL netcode_init\n" );
        return 1;
    }
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

    const int rows = parse_case_table( cases_path );
    if ( rows != CASE_COUNT )
    {
        fprintf( stderr, "could not read exactly %d cases from %s (read %d)\n", CASE_COUNT, cases_path, rows );
        printf( "differential: FAIL case table\n" );
        free_case_table();
        return 1;
    }

    const uint64_t now = ROCKETNET_TEST_CREATE_TS;

    printf( "# case\tnetcode\trocketnet\tverdict\n" );

    for ( int i = 0; i < CASE_COUNT; i++ )
    {
        if ( strcmp( g_cases[i].id, g_case_ids[i] ) != 0 )
        {
            fprintf( stderr, "FAIL %s: row %d of %s, where the harness runs %s\n", g_cases[i].id, i + 1, cases_path, g_case_ids[i] );
            g_tally.failures++;
            continue;
        }

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
                fprintf( stderr, "FAIL %s: live netcode disagrees with the reference column (%s, reference %s)\n", g_cases[i].id, live_reason, g_cases[i].netcode_reference );
                g_tally.failures++;
            }

            netcode = live_reason;
        }
        else
        {
            fprintf( stderr, "FAIL %s: live netcode could not be asked (mint or capture failed)\n", g_cases[i].id );
            g_tally.failures++;
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
        tally_row( g_cases[i].id, &rn, rocketnet, netcode, g_cases[i].rocketnet_expected );
    }

    if ( corpus_at > 0 )
    {
        printf( "\n" );
        run_corpus( argv[corpus_at + 1], argc - corpus_at - 2, argv + corpus_at + 2 );
    }

    if ( g_tally.compared == 0 )
    {
        fprintf( stderr, "FAIL: no row was compared with netcode 1.02\n" );
        g_tally.failures++;
    }

    printf( "\ndifferential: %s compared=%d agree=%d stricter-permitted=%d not-comparable=%d failures=%d\n",
            g_tally.failures == 0 ? "PASS" : "FAIL",
            g_tally.compared, g_tally.agree, g_tally.stricter_permitted, g_tally.not_comparable, g_tally.failures );

    free_case_table();
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    netcode_term();
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    return g_tally.failures == 0 ? 0 : 1;
}
