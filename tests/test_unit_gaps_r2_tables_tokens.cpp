/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Connect token writers and the used-token history at the refusals and the
// edges no other case reached (mutation gaps, round 2, r2-tables). No
// sockets and no threads; token timestamps and table times are arguments
// from 1000, except where a case says why it probes a time at or below zero.
// Each failing check prints the fault first.

#include "test_unit_registry.h"
#include "loopback_hub.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_address.h"
#include "rocketnet_base64.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV
static void gaps_r2_tables_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}
#endif // #if ROCKETNET_DEV

static bool gaps_r2_tables_all( const uint8_t * bytes, size_t count, uint8_t value )
{
    for ( size_t i = 0; i < count; ++i )
    {
        if ( bytes[i] != value )
        {
            return false;
        }
    }
    return true;
}

struct gaps_r2_tables_keys_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
};

static void gaps_r2_tables_keys( gaps_r2_tables_keys_t * keys )
{
    check( rocketnet_generate_backend_keypair( keys->backend_public, keys->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( keys->server_public, keys->server_secret ) == ROCKETNET_STATUS_OK );
}

static enum rocketnet_status_t gaps_r2_tables_write( const gaps_r2_tables_keys_t * keys, const char * const * addresses, int count, uint8_t * token )
{
    return rocketnet_write_production_connect_token( keys->backend_secret, keys->server_public, 0x1234ULL, addresses, count, 7, 1000, 2000, 10,
                                                     NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES );
}

// ---------------------------------------------------------------------------
// the production writer's address list

// mutation gap: hunt 2/208
//
// A token carries at most one IPv4 and one IPv6 address. Two IPv6 addresses
// are refused; without the refusal the second overwrites the first's field and
// the token names one server where the caller listed two.

static void test_gaps_r2_tables_token_refuses_two_ipv6_addresses()
{
    gaps_r2_tables_keys_t keys;
    gaps_r2_tables_keys( &keys );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0xA5, sizeof( token ) );
    const char * two_ipv6[] = { "[::1]:40000", "[fe80::1]:40000" };
    const enum rocketnet_status_t status = gaps_r2_tables_write( &keys, two_ipv6, 2, token );
    if ( status != ROCKETNET_STATUS_INVALID_ARGUMENT )
    {
        printf( "fault: a production token listing two IPv6 addresses was not refused (status %d): the second overwrote the first\n", (int) status );
    }
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( gaps_r2_tables_all( token, sizeof( token ), 0xA5 ) );

    // one of each family is a token
    const char * one_each[] = { "127.0.0.1:40000", "[::1]:40000" };
    check( gaps_r2_tables_write( &keys, one_each, 2, token ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_token_refuses_two_ipv6_addresses );

// ---------------------------------------------------------------------------
// the used-token history: a binding or claim recorded with no address, and
// destroy of nothing

static struct rocketnet_address_t gaps_r2_tables_address( const char * text )
{
    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, text ) );
    return address;
}

static void gaps_r2_tables_token_id( uint8_t * identifier, uint32_t ordinal )
{
    memset( identifier, 0x3C, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    memcpy( identifier, &ordinal, sizeof( ordinal ) );
}

// mutation gap: hunt 2/796
// mutation gap: hunt 2/797
//
// A binding or claim recorded with no address (the table takes NULL and
// stores the cleared address) is bound to no address: every other address is
// refused, an address whose bytes are all 1 included (1.1.1.1:257 is what a
// store filled with byte 1 reads as), and a slot reused from an earlier
// holder does not keep that holder's address.

static void test_gaps_r2_tables_token_null_address_binds_to_no_address()
{
    const struct rocketnet_address_t ones = gaps_r2_tables_address( "1.1.1.1:257" );
    const struct rocketnet_address_t holder = gaps_r2_tables_address( "198.51.100.7:50000" );
    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];

    // bindings: a fresh slot, then a slot reused from an expired binding
    {
        struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 1 );
        check( history );

        gaps_r2_tables_token_id( id, 1 );
        rocketnet_used_token_history_record( history, id, NULL, 5000, 1000 );
        const bool ones_refused = rocketnet_used_token_history_check( history, id, &ones, 1000 ) == 0;
        if ( !ones_refused )
        {
            printf( "fault: a binding recorded with no address admitted 1.1.1.1:257: the stored address is not the cleared one\n" );
        }
        check( ones_refused );
        rocketnet_used_token_history_destroy( history );

        history = rocketnet_used_token_history_create( NULL, 1 );
        check( history );
        gaps_r2_tables_token_id( id, 2 );
        rocketnet_used_token_history_record( history, id, &holder, 1001, 1000 );
        gaps_r2_tables_token_id( id, 3 );
        rocketnet_used_token_history_record( history, id, NULL, 5000, 1002 );
        const bool holder_refused = rocketnet_used_token_history_check( history, id, &holder, 1002 ) == 0;
        if ( !holder_refused )
        {
            printf( "fault: a binding recorded with no address in a reused slot admitted the slot's previous holder: its address was kept\n" );
        }
        check( holder_refused );
        rocketnet_used_token_history_destroy( history );
    }

    // claims: a fresh slot, then a slot freed by a release and claimed again
    {
        struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 1 );
        check( history );

        gaps_r2_tables_token_id( id, 4 );
        check( rocketnet_used_token_history_claim_record( history, id, NULL, 1005.0, 1000.0 ) == 1 );
        const bool ones_refused = rocketnet_used_token_history_claim_check( history, id, &ones, 1000.0 ) == 0;
        if ( !ones_refused )
        {
            printf( "fault: a claim recorded with no address admitted 1.1.1.1:257: the stored address is not the cleared one\n" );
        }
        check( ones_refused );
        rocketnet_used_token_history_destroy( history );

        history = rocketnet_used_token_history_create( NULL, 1 );
        check( history );
        gaps_r2_tables_token_id( id, 5 );
        check( rocketnet_used_token_history_claim_record( history, id, &holder, 1005.0, 1000.0 ) == 1 );
        rocketnet_used_token_history_claim_release( history, id, &holder );
        gaps_r2_tables_token_id( id, 6 );
        check( rocketnet_used_token_history_claim_record( history, id, NULL, 1005.0, 1000.0 ) == 1 );
        const bool holder_refused = rocketnet_used_token_history_claim_check( history, id, &holder, 1000.0 ) == 0;
        if ( !holder_refused )
        {
            printf( "fault: a claim recorded with no address in a reused slot admitted the slot's previous holder: its address was kept\n" );
        }
        check( holder_refused );
        rocketnet_used_token_history_destroy( history );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_token_null_address_binds_to_no_address );

// mutation gap: hunt 2/909

static void test_gaps_r2_tables_token_history_destroy_null()
{
    rocketnet_used_token_history_destroy( NULL );
    check( true );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_token_history_destroy_null );

// mutation gap: hunt 2/944
//
// A claim check of a token the table has never seen admits every address, at
// every time. A check that tests its miss against the wrong sentinel goes on to
// read claim_deadline[-1] and address[-1]: the bytes rocketnet_malloc keeps
// before each array, its header, whose free-function pointer reads as a tiny
// positive double. At any time above zero that "deadline" has passed and the
// answer is still 1, so this probe uses times at and below zero, where the
// stale read is taken as a live claim held by a cleared address and the
// unseen token is refused. The table takes any time of its clock domain.

static void test_gaps_r2_tables_token_claim_unseen_admitted_at_every_time()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 4 );
    check( history );

    const struct rocketnet_address_t address = gaps_r2_tables_address( "198.51.100.7:50000" );
    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    gaps_r2_tables_token_id( id, 9 );

    const double times[] = { 1000.0, 0.0, -1.0 };
    for ( int i = 0; i < 3; ++i )
    {
        const bool admitted = rocketnet_used_token_history_claim_check( history, id, &address, times[i] ) == 1;
        if ( !admitted )
        {
            printf( "fault: a claim check of a token never claimed refused its address at time %.1f: the miss read the deadline before the array\n",
                    times[i] );
        }
        check( admitted );
    }

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_token_claim_unseen_admitted_at_every_time );

#if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// the claim table's free-stack peek reads nothing before the stack
//
// mutation gap: hunt 2/970
// mutation gap: hunt 2/971
//
// A claim that takes the oldest expired slot, with no free slot, peeks the
// free stack only while it holds an entry. A peek at free_count 0 reads
// free_slots[-1]: the last four bytes of rocketnet_malloc's header before the
// stack, the high half of the installed free function's address. When those
// bytes equal the slot being taken, the claim pops a stack that is empty
// (free_count goes to -1) and never retires the slot it reuses. This case
// installs its own allocator, so it knows that value, sizes the table so the
// value is a slot, and walks the expired claims through every slot.

static void * gaps_r2_tables_malloc( void * context, size_t bytes )
{
    (void) context;
    return malloc( bytes );
}

static void gaps_r2_tables_free( void * context, void * p )
{
    (void) context;
    free( p );
}

static void gaps_r2_tables_install( void * ( *allocate )( void *, size_t ), void ( *release )( void *, void * ) )
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( NULL, allocate, release, &status ) );
    check( status == ROCKETNET_STATUS_OK );
}

static void gaps_r2_tables_uninstall()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static void test_gaps_r2_tables_token_claim_reuse_reads_nothing_before_the_free_stack()
{
    // the int just before each block rocketnet_malloc hands out: the last
    // bytes of its header, which end with the free function's pointer
    void ( *release )( void *, void * ) = gaps_r2_tables_free;
    int before = 0;
    memcpy( &before, (const uint8_t *) &release + sizeof( release ) - sizeof( before ), sizeof( before ) );
    if ( before < 0 || before >= 65536 )
    {
        printf( "note: the int before each block is %d, not a slot of a table this case builds; nothing probed\n", before );
        return;
    }

    gaps_r2_tables_install( gaps_r2_tables_malloc, gaps_r2_tables_free );

    const int capacity = before + 1;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history );

    const struct rocketnet_address_t address = gaps_r2_tables_address( "198.51.100.7:50000" );
    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < capacity; ++i )
    {
        gaps_r2_tables_token_id( id, (uint32_t) i );
        check( rocketnet_used_token_history_claim_record( history, id, &address, 1005.0, 1000.0 ) == 1 );
    }
    check( rocketnet_used_token_history_claim_free_count( history ) == 0 );

    // every claim has expired: each new claim takes the oldest, one slot after
    // another, until every slot was taken once more
    rocketnet_assert_function( gaps_r2_tables_quiet_assert );
    int bad_at = -1;
    int free_count = 0;
    for ( int i = 0; i < capacity && bad_at < 0; ++i )
    {
        gaps_r2_tables_token_id( id, (uint32_t) ( capacity + i ) );
        rocketnet_used_token_history_claim_record( history, id, &address, 1010.0, 1006.0 );
        free_count = rocketnet_used_token_history_claim_free_count( history );
        if ( free_count != 0 )
        {
            bad_at = i;
        }
    }
    rocketnet_assert_function( NULL );
    if ( bad_at >= 0 )
    {
        printf( "fault: reusing an expired claim popped an empty free stack (free count %d after reuse %d of %d): the peek read free_slots[-1]\n",
                free_count, bad_at, capacity );
    }
    check( bad_at < 0 );

    rocketnet_used_token_history_destroy( history );
    gaps_r2_tables_uninstall();
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_token_claim_reuse_reads_nothing_before_the_free_stack );

// ---------------------------------------------------------------------------
// the DEV writers

static void gaps_r2_tables_signed_fields( struct rocketnet_signed_connect_data_t * signed_fields, const uint8_t * server_public )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = 0x1234ULL;
    signed_fields->create_timestamp = 1000;
    signed_fields->expire_timestamp = 2000;
    signed_fields->timeout_seconds = 10;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields->ipv4_address[0] = 127;
    signed_fields->ipv4_address[3] = 1;
    signed_fields->ipv4_address[4] = 0x40;
    signed_fields->ipv4_address[5] = 0x9c;
    memcpy( signed_fields->server_public_key, server_public, ROCKETNET_KEY_BYTES );
}

// mutation gap: hunt 2/125
// mutation gap: hunt 2/126
//
// The deterministic writer seals only under the ephemeral key it is given.
// Without one it writes nothing and reports 0; it never falls back to the
// production seal, whose ephemeral key is drawn at random.

static void test_gaps_r2_tables_deterministic_writer_needs_its_ephemeral_key()
{
    gaps_r2_tables_keys_t keys;
    gaps_r2_tables_keys( &keys );

    struct rocketnet_signed_connect_data_t signed_fields;
    gaps_r2_tables_signed_fields( &signed_fields, keys.server_public );
    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 7;
    memset( private_fields.client_to_server_key, 0x11, sizeof( private_fields.client_to_server_key ) );
    memset( private_fields.server_to_client_key, 0x22, sizeof( private_fields.server_to_client_key ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0xA5, sizeof( token ) );
    const int written = rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields, keys.backend_secret, NULL, token );
    if ( written != 0 )
    {
        printf( "fault: the deterministic writer reported a token written with no ephemeral key\n" );
    }
    check( written == 0 );
    const bool untouched = gaps_r2_tables_all( token, sizeof( token ), 0xA5 );
    if ( !untouched )
    {
        printf( "fault: the deterministic writer wrote a token with no ephemeral key: it fell back to a random seal\n" );
    }
    check( untouched );

    // with the key, the same fields are a token
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];
    memset( ephemeral, 0x5C, sizeof( ephemeral ) );
    check( rocketnet_write_connect_token_deterministic( &signed_fields, &private_fields, keys.backend_secret, ephemeral, token ) == 1 );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_deterministic_writer_needs_its_ephemeral_key );

// mutation gap: hunt 2/218
//
// When the crypto source cannot be acquired (it stands for a sodium_init
// failure; the hangs DEV seam rocketnet_crypto_fail_acquire_for_test), the
// production writer refuses with CRYPTO_INIT_FAILED, writes nothing and
// releases nothing it did not hold.

static void test_gaps_r2_tables_production_token_refused_without_the_crypto_source()
{
    gaps_r2_tables_keys_t keys;
    gaps_r2_tables_keys( &keys );
    const char * addresses[] = { "127.0.0.1:40000" };

    const int holders = rocketnet_crypto_holders_for_test();
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0xA5, sizeof( token ) );

    rocketnet_crypto_fail_acquire_for_test( true );
    const enum rocketnet_status_t status = gaps_r2_tables_write( &keys, addresses, 1, token );
    const int holders_after = rocketnet_crypto_holders_for_test();
    rocketnet_crypto_fail_acquire_for_test( false );

    if ( status != ROCKETNET_STATUS_CRYPTO_INIT_FAILED )
    {
        printf( "fault: a production token was minted (status %d) though the crypto source could not be acquired\n", (int) status );
    }
    check( status == ROCKETNET_STATUS_CRYPTO_INIT_FAILED );
    if ( holders_after != holders )
    {
        printf( "fault: a refused production token changed the crypto holder count from %d to %d: it released a hold it never took\n", holders,
                holders_after );
    }
    check( holders_after == holders );
    check( gaps_r2_tables_all( token, sizeof( token ), 0xA5 ) );

    // with the source back, the token is written and the count is back
    check( gaps_r2_tables_write( &keys, addresses, 1, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_crypto_holders_for_test() == holders );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_production_token_refused_without_the_crypto_source );

// mutation gap: hunt 2/601
//
// A test token with no server address is refused as INVALID_ARGUMENT at once:
// nothing past the address check runs, so a short buffer is not reported too.

static int gaps_r2_tables_buffer_errors = 0;

static void gaps_r2_tables_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( level == ROCKETNET_LOG_LEVEL_ERROR && strstr( message, "test connect token refused" ) )
    {
        ++gaps_r2_tables_buffer_errors;
    }
}

static void test_gaps_r2_tables_test_token_without_address_stops_at_the_address()
{
    const rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    gaps_r2_tables_buffer_errors = 0;
    rocketnet_log_function( gaps_r2_tables_log, NULL );
    rocketnet_assert_function( gaps_r2_tables_quiet_assert );

    uint8_t token[8];
    memset( token, 0xA5, sizeof( token ) );
    const enum rocketnet_status_t status = rocketnet_write_test_connect_token( NULL, 7, 1000, token, sizeof( token ) );

    rocketnet_assert_function( NULL );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );

    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    if ( gaps_r2_tables_buffer_errors != 0 )
    {
        printf( "fault: a test token with no address went on past the address check and reported its short buffer\n" );
    }
    check( gaps_r2_tables_buffer_errors == 0 );
    check( gaps_r2_tables_all( token, sizeof( token ), 0xA5 ) );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_test_token_without_address_stops_at_the_address );

// mutation gap: hunt 2/619
//
// The development token carries the caller's user data in its private block.

static void test_gaps_r2_tables_dev_token_carries_user_data()
{
    gaps_r2_tables_keys_t keys;
    gaps_r2_tables_keys( &keys );

    uint8_t user_data[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    for ( int i = 0; i < (int) sizeof( user_data ); ++i )
    {
        user_data[i] = (uint8_t) ( i * 7 + 3 );
    }

    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_create_dev_connect_token( "127.0.0.1:40000", NULL, keys.server_public, 30, 10, 7, 0x1234ULL, 1000, user_data, token_string ) ==
           ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_base64_decode_data( token_string, token, sizeof( token ) ) == ROCKETNET_CONNECT_TOKEN_BYTES );
    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, token );
    struct rocketnet_connect_token_private_t opened;
    check( rocketnet_open_connect_token_private( &opened, parsed.signed_connect_data, keys.server_public, keys.server_secret ) == 1 );

    const bool carried = memcmp( opened.user_data, user_data, sizeof( user_data ) ) == 0;
    if ( !carried )
    {
        printf( "fault: the development token dropped the caller's user data: its private block does not carry it\n" );
    }
    check( carried );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_dev_token_carries_user_data );

#endif // #if ROCKETNET_DEV
