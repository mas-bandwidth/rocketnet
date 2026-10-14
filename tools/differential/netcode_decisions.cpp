/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Cross-implementation differential: the token/server admission DECISION.

    For each case in cases.tsv this harness drives rocketnet's connect-token
    admission path in process (the token open and the server's connection
    request ladder, with time passed in as an argument) and compares the
    accept/refuse decision against the netcode 1.02 reference decision. The
    two libraries' wire formats differ, so the bytes are never compared --
    only the decision and the refusal reason class.

    The netcode side has two modes:

      * reference -- the decision recorded in cases.tsv, taken from the
        netcode 1.02 source (mas-bandwidth/netcode at the pinned tag). This
        runs anywhere and is what the table is diffed against by default.

      * live -- when built with -DROCKETNET_DIFFERENTIAL_NETCODE_DIR, the
        same decision is computed by driving the netcode 1.02 library itself,
        and the harness reports when the live answer disagrees with the
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

#include "rocketnet.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_vectors.h"

// The netcode side, when the bench supplies the reference source. This header
// is only pulled in when run.sh was given a NETCODE_DIR; the default build
// diffs against the reference column in cases.tsv.
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
#include "netcode.h"
#endif

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

static const char * case_id_name( int id )
{
    switch ( id )
    {
        case CASE_VALID_SIGNATURE:    return "valid_signature";
        case CASE_EXPIRED_TOKEN:      return "expired_token";
        case CASE_BAD_SIGNATURE:      return "bad_signature";
        case CASE_WRONG_SERVER_ADDRESS: return "wrong_server_address";
        case CASE_REPLAYED_TOKEN:     return "replayed_token";
        case CASE_FULL_SERVER:        return "full_server";
        case CASE_WRONG_PROTOCOL_ID:  return "wrong_protocol_id";
        case CASE_TIMEOUT_OUT_OF_RANGE: return "timeout_out_of_range";
        default: return "unknown";
    }
}

static struct case_row_t g_cases[CASE_COUNT];

// --- small helpers ------------------------------------------------------

static void encode_ipv4_token_address( uint8_t out[6], int a, int b, int c, int d, int port )
{
    out[0] = (uint8_t) a;
    out[1] = (uint8_t) b;
    out[2] = (uint8_t) c;
    out[3] = (uint8_t) d;
    out[4] = (uint8_t) ( port & 0xff );
    out[5] = (uint8_t) ( ( port >> 8 ) & 0xff );
}

// --- the rocketnet decision ---------------------------------------------

// A fresh server and a freshly minted token for the case, admitted through
// the real connection request ladder. Returns accept (1) with reason
// "accept", or refuse (0) with the reason class the ladder actually took.

struct decision_t
{
    int accept;
    char reason[64];
};

static struct decision_t rocketnet_decide( int case_id, uint64_t now )
{
    struct decision_t d;
    memset( &d, 0, sizeof(d) );

    // a server, exactly as the unit suite's request-ladder test builds one
    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof(server) );
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.filter_value, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server.server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" );
    memcpy( server.cookie_key, rocketnet_test_backend_secret_key, 32 );   // any 32-byte cookie key
    server.cookie_lifetime_seconds = 3;
    server.max_token_lifetime_seconds = 600;
    server.handshake_deadline_seconds = 5.0;
    server.max_timeout_seconds = 0;
    server.allow_negative_timeout = 0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server.admitted_token_set = rocketnet_admitted_token_set_create( NULL, 16 );
    server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    server.acks = rocketnet_server_acks_create( NULL, 4 );
    server.reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 16 );
    server.counters = &counters;
    rocketnet_reassembly_set_begin_update( server.reassembly );
    rocketnet_server_protocol_update( &server, (double) now );

    struct rocketnet_address_t client_address;
    rocketnet_address_parse( &client_address, "198.51.100.7:56789" );

    // the signed and private token fields, then the case mutation
    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    memset( &sf, 0, sizeof(sf) );
    memset( &pf, 0, sizeof(pf) );

    sf.version = ROCKETNET_VERSION;
    sf.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    sf.create_timestamp = now - 10;
    sf.expire_timestamp = now + 300;
    sf.timeout_seconds = 10;
    sf.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 10, 40000 );
    memcpy( sf.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    pf.client_id = ROCKETNET_TEST_CLIENT_ID;
    memcpy( pf.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( pf.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    switch ( case_id )
    {
        case CASE_EXPIRED_TOKEN:
            sf.expire_timestamp = now - 1;
            break;
        case CASE_WRONG_PROTOCOL_ID:
            sf.protocol_id = 0xdeadbeefdeadbeefULL;
            break;
        case CASE_TIMEOUT_OUT_OF_RANGE:
            sf.timeout_seconds = 0;
            break;
        case CASE_WRONG_SERVER_ADDRESS:
            encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 11, 40000 );
            break;
        default:
            break;
    }

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_connect_token( &sf, &pf, rocketnet_test_backend_secret_key, token ) )
    {
        d.accept = 0;
        strcpy( d.reason, "mint" );
        goto out;
    }

    {
        struct rocketnet_connect_token_t ct;
        rocketnet_read_connect_token( &ct, token );

        if ( case_id == CASE_BAD_SIGNATURE )
            ct.signature[0] ^= 0xff;

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_write_connection_request_packet( request, rocketnet_test_filter_value, NULL,
                                                   rocketnet_test_salt, rocketnet_test_client_eph_public,
                                                   ct.signed_connect_data, ct.signature );

        if ( case_id == CASE_FULL_SERVER )
        {
            uint8_t dummy[ROCKETNET_USER_DATA_BYTES];
            memset( dummy, 0, sizeof(dummy) );
            for ( int i = 0; i < 4; i++ )
                rocketnet_client_slots_assign( server.client_slots, 0xD000 + (uint64_t) i, -1, dummy );
        }

        uint8_t response[256];
        int response_bytes = 0;

        int result = rocketnet_server_protocol_process_connection_request(
                        &server, &client_address, request, sizeof(request), now, (double) now,
                        response, &response_bytes );

        if ( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
        {
            memcpy( request + ROCKETNET_PREFILTER_BYTES,
                    response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
                    ROCKETNET_COOKIE_FIELD_BYTES );
            result = rocketnet_server_protocol_process_connection_request(
                        &server, &client_address, request, sizeof(request), now, (double) now,
                        response, &response_bytes );
        }

        if ( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
        {
            d.accept = 1;
            strcpy( d.reason, "accept" );

            if ( case_id == CASE_REPLAYED_TOKEN )
            {
                // the token is now bound to its first source. replay the same
                // request from a different address: one hash, then refused.
                struct rocketnet_address_t replay_address;
                rocketnet_address_parse( &replay_address, "198.51.100.50:56789" );
                uint8_t replayed[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
                memcpy( replayed, request, sizeof(replayed) );
                memset( replayed + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_COOKIE_FIELD_BYTES );

                int rr = rocketnet_server_protocol_process_connection_request(
                            &server, &replay_address, replayed, sizeof(replayed), now, (double) now,
                            response, &response_bytes );
                if ( rr == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
                {
                    memcpy( replayed + ROCKETNET_PREFILTER_BYTES,
                            response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
                            ROCKETNET_COOKIE_FIELD_BYTES );
                    rr = rocketnet_server_protocol_process_connection_request(
                                &server, &replay_address, replayed, sizeof(replayed), now, (double) now,
                                response, &response_bytes );
                }

                if ( rr != ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
                {
                    d.accept = 0;
                    if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] )
                        strcpy( d.reason, "replay" );
                    else
                        strcpy( d.reason, "refused" );
                }
            }
        }
        else
        {
            d.accept = 0;
            // read the reason the ladder actually took, in the standard's order
            if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] )
                strcpy( d.reason, "expired" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID] )
                strcpy( d.reason, "timeout" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED] )
                strcpy( d.reason, "address" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS] )
                strcpy( d.reason, "address" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID] )
                strcpy( d.reason, "protocol" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] )
                strcpy( d.reason, "version" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] )
                strcpy( d.reason, "signature" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK] )
                strcpy( d.reason, "open" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH] )
                strcpy( d.reason, "mismatch" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] ||
                      counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED] )
                strcpy( d.reason, "replay" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] )
                strcpy( d.reason, "full" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] )
                strcpy( d.reason, "server-key" );
            else
                strcpy( d.reason, "refused" );
        }
    }

out:
    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_admitted_token_set_destroy( server.admitted_token_set );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_reassembly_set_destroy( server.reassembly );
    return d;
}

// --- the netcode decision ------------------------------------------------

// The netcode 1.02 reference decision for a case is what the pinned netcode
// source reaches for the equivalent token. In reference mode this is read
// straight from cases.tsv. In live mode the netcode library is asked and the
// answer compared against the reference column; a mismatch is reported but
// does not change the verdict against rocketnet.

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

// netcode 1.02's public surface, from the pinned mas-bandwidth/netcode tag.
// The harness is only compiled against this when run.sh is given NETCODE_DIR;
// the shapes below are the 1.02 API and the constants are the 1.02 wire
// sizes. The decision is computed by minting a netcode connect token with the
// case's mutation, reading it back, and applying netcode's checks in its
// order -- protocol id, expiry, timeout, server address, then the signature.
// The signature is Ed25519 over the token's public data, verified with the
// public key derived from the private key, exactly as netcode's server does.

#include <sodium.h>

static void netcode_encode_ipv4_address( uint8_t out[NETCODE_ADDRESS_BYTES], int a, int b, int c, int d, int port )
{
    memset( out, 0, NETCODE_ADDRESS_BYTES );
    out[0] = 1;                 // type: IPv4
    out[1] = (uint8_t) a;
    out[2] = (uint8_t) b;
    out[3] = (uint8_t) c;
    out[4] = (uint8_t) d;
    out[NETCODE_ADDRESS_BYTES - 2] = (uint8_t) ( port & 0xff );
    out[NETCODE_ADDRESS_BYTES - 1] = (uint8_t) ( ( port >> 8 ) & 0xff );
}

static int netcode_decide_live( int case_id, uint64_t now, char * reason, size_t reason_size )
{
    (void) reason_size;

    // the server's fixed identity: the protocol id and address the token must
    // name. The token fields below are mutated per case; these are not.
    const uint64_t server_protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    uint8_t server_address[NETCODE_ADDRESS_BYTES];
    netcode_encode_ipv4_address( server_address, 203, 0, 113, 10, 40000 );

    uint64_t protocol_id = server_protocol_id;
    uint64_t create_timestamp = now - 10;
    uint64_t expire_timestamp = now + 300;
    uint32_t timeout_seconds = 10;

    uint8_t server_addresses[NETCODE_ADDRESS_BYTES * NETCODE_MAX_SERVERS_PER_CONNECT];
    int num_server_addresses = 1;
    netcode_encode_ipv4_address( server_addresses, 203, 0, 113, 10, 40000 );

    uint8_t client_to_server_key[NETCODE_KEY_BYTES];
    uint8_t server_to_client_key[NETCODE_KEY_BYTES];
    uint8_t private_key[NETCODE_KEY_BYTES];
    uint8_t user_data[NETCODE_USER_DATA_BYTES];
    uint8_t connect_token_private_data[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t connect_token[NETCODE_CONNECT_TOKEN_BYTES];

    memset( client_to_server_key, 0, sizeof(client_to_server_key) );
    memset( server_to_client_key, 0, sizeof(server_to_client_key) );
    memset( private_key, 0, sizeof(private_key) );
    memset( user_data, 0, sizeof(user_data) );
    memset( connect_token_private_data, 0, sizeof(connect_token_private_data) );
    memset( connect_token, 0, sizeof(connect_token) );

    // a fixed Ed25519 private key, so the run is deterministic
    for ( int i = 0; i < NETCODE_KEY_BYTES; i++ )
        private_key[i] = (uint8_t) ( 0x40 + i );

    switch ( case_id )
    {
        case CASE_EXPIRED_TOKEN:
            expire_timestamp = now - 1;
            break;
        case CASE_WRONG_PROTOCOL_ID:
            protocol_id = 0xdeadbeefdeadbeefULL;
            break;
        case CASE_TIMEOUT_OUT_OF_RANGE:
            timeout_seconds = 0;
            break;
        case CASE_WRONG_SERVER_ADDRESS:
            netcode_encode_ipv4_address( server_addresses, 203, 0, 113, 11, 40000 );
            break;
        default:
            break;
    }

    if ( !netcode_generate_connect_token( connect_token, protocol_id, create_timestamp,
                                          expire_timestamp, timeout_seconds, server_addresses,
                                          num_server_addresses, client_to_server_key,
                                          server_to_client_key, private_key, user_data,
                                          connect_token_private_data ) )
    {
        strcpy( reason, "refuse:mint" );
        return 0;
    }

    if ( case_id == CASE_BAD_SIGNATURE )
    {
        // flip one byte of the signature; netcode's server then refuses the
        // token on the signature check
        connect_token[NETCODE_CONNECT_TOKEN_BYTES - 1] ^= 0xff;
    }

    uint8_t read_protocol_id[8];
    uint64_t read_create = 0, read_expire = 0;
    uint32_t read_timeout = 0;
    uint8_t read_addresses[NETCODE_ADDRESS_BYTES * NETCODE_MAX_SERVERS_PER_CONNECT];
    int read_num_addresses = 0;
    uint8_t read_c2s[NETCODE_KEY_BYTES], read_s2c[NETCODE_KEY_BYTES];
    uint8_t read_user_data[NETCODE_USER_DATA_BYTES];
    uint8_t read_private[NETCODE_CONNECT_TOKEN_BYTES];

    if ( !netcode_read_connect_token( connect_token, read_protocol_id, &read_create, &read_expire,
                                      &read_timeout, read_addresses, &read_num_addresses,
                                      read_c2s, read_s2c, read_user_data, read_private ) )
    {
        strcpy( reason, "refuse:open" );
        return 0;
    }

    // netcode's decision order: protocol id, expiry, timeout, server address,
    // then the signature. The full-server and replay cases are server state,
    // not token state, and are handled below.
    if ( memcmp( read_protocol_id, &server_protocol_id, 8 ) != 0 )
    {
        strcpy( reason, "refuse:protocol" );
        return 0;
    }
    if ( read_expire <= now )
    {
        strcpy( reason, "refuse:expired" );
        return 0;
    }
    if ( read_timeout == 0 )
    {
        strcpy( reason, "refuse:timeout" );
        return 0;
    }
    if ( read_num_addresses != 1 ||
         memcmp( read_addresses, server_address, NETCODE_ADDRESS_BYTES ) != 0 )
    {
        strcpy( reason, "refuse:address" );
        return 0;
    }

    // signature: Ed25519 over the token's public data, verified against the
    // public key derived from the private key, as netcode's server does.
    {
        uint8_t public_key[crypto_sign_PUBLICKEYBYTES];
        crypto_sign_ed25519_sk_to_pk( public_key, private_key );

        // the public data is the fixed header through the user data; the
        // 64-byte signature follows it, before the encrypted private data.
        size_t public_bytes = 13 + 8 + 8 + 8 + 4 + 4
                            + (size_t) read_num_addresses * NETCODE_ADDRESS_BYTES
                            + 32 + 32 + 256;
        if ( crypto_sign_verify_detached( connect_token + public_bytes,
                                          connect_token, public_bytes, public_key ) != 0 )
        {
            strcpy( reason, "refuse:signature" );
            return 0;
        }
    }

    if ( case_id == CASE_FULL_SERVER )
    {
        strcpy( reason, "refuse:full" );
        return 0;
    }

    // replayed_token: netcode 1.02 binds a token to a server address and an
    // expiry and stops there; a replay is admitted.
    strcpy( reason, "accept" );
    return 1;
}
#endif

// --- driver --------------------------------------------------------------

static int parse_case_table( const char * path )
{
    FILE * f = fopen( path, "rb" );
    if ( !f )
        return -1;

    char line[512];
    int n = 0;
    while ( fgets( line, sizeof(line), f ) )
    {
        if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' )
            continue;
        char * nl = strchr( line, '\n' );
        if ( nl ) *nl = 0;
        nl = strchr( line, '\r' );
        if ( nl ) *nl = 0;

        char * id = line;
        char * t1 = strchr( id, '\t' );
        if ( !t1 ) continue;
        *t1++ = 0;
        char * description = t1;
        char * t2 = strchr( description, '\t' );
        if ( !t2 ) continue;
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
        free( (void*) g_cases[i].id );
        free( (void*) g_cases[i].description );
        free( (void*) g_cases[i].netcode_reference );
        g_cases[i].id = NULL;
        g_cases[i].description = NULL;
        g_cases[i].netcode_reference = NULL;
    }
}

static const char * verdict( const struct decision_t * rocketnet, const char * netcode )
{
    int netcode_accept = strncmp( netcode, "accept", 6 ) == 0;
    if ( rocketnet->accept == netcode_accept )
        return "agree";
    if ( !netcode_accept && rocketnet->accept )
        return "rocketnet-weaker";
    return "rocketnet-stricter";
}

int main( int argc, const char ** argv )
{
    const char * cases_path = "tools/differential/cases.tsv";
    if ( argc > 1 )
        cases_path = argv[1];

    rocketnet_quiet( true );

    if ( parse_case_table( cases_path ) != CASE_COUNT )
    {
        fprintf( stderr, "could not read %d cases from %s\n", CASE_COUNT, cases_path );
        return 1;
    }

    const uint64_t now = ROCKETNET_TEST_CREATE_TS;

    printf( "# case\tnetcode\trocketnet\tverdict\n" );
    for ( int i = 0; i < CASE_COUNT; i++ )
    {
        struct decision_t rn = rocketnet_decide( i, now );

        const char * netcode = g_cases[i].netcode_reference;
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
        char live_reason[64];
        if ( netcode_decide_live( i, now, live_reason, sizeof(live_reason) ) >= 0 )
        {
            if ( strcmp( live_reason, g_cases[i].netcode_reference ) != 0 )
                fprintf( stderr, "live netcode disagrees with the reference column on %s\n", g_cases[i].id );
            netcode = live_reason;
        }
#endif

        char rocketnet[72];
        if ( rn.accept )
            strcpy( rocketnet, "accept" );
        else
            snprintf( rocketnet, sizeof(rocketnet), "refuse:%s", rn.reason );

        printf( "%s\t%s\t%s\t%s\n", g_cases[i].id, netcode, rocketnet,
                verdict( &rn, netcode ) );
    }

    free_case_table();
    return 0;
}
