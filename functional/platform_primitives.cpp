/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional cases that the unit suite must not run: real threads, real
    frame pacing and wall-clock reads belong here, never in ./bin/test.
    Each test is one process, and starts a real platform primitive.
*/

#include "rocketnet_internal.h"
#include "rocketnet_ack.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_frame_sync.h"
#include "rocketnet_test_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

static void rocketnet_platform_primitives_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_platform_primitives_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
    }                                                                                                           \
} while(0)

static bool threads_work = false;

static void test_thread_function(void*)
{
    threads_work = true;
}

void rocketnet_test_platform_thread()
{
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, test_thread_function, NULL );
    rocketnet_check( thread );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    rocketnet_check( threads_work );
}

void rocketnet_test_frame_sync_wait()
{
    // paced for real, briefly: 25 frames at 250hz is ~100ms of wall time.
    // the assertion is pacing and accounting, never precision -- precision
    // was the issue #56 measurement campaign's job, and CI runners are noisy.

    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, 250.0 );
    rocketnet_check( frame_sync );

    const double start = rocketnet_platform_time();
    for ( int i = 0; i < 25; i++ )
    {
        rocketnet_frame_sync_wait( frame_sync );
    }
    const double elapsed = rocketnet_platform_time() - start;

    struct rocketnet_frame_sync_stats_t stats;
    rocketnet_frame_sync_stats( frame_sync, &stats );
    rocketnet_check( stats.frame == 25 );
    rocketnet_check( elapsed > 0.06 );                              // it actually paced: a free run finishes in microseconds
    // The suite watchdog catches a hang; scheduler latency is not a failure.
    rocketnet_check( !stats.spinning || stats.priority_held );      // never an unprivileged spin
    rocketnet_check( stats.guard_seconds <= ( 1.0 / 250.0 ) / ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR + 1e-9 );

    rocketnet_frame_sync_destroy( frame_sync );
}

static void rocketnet_test_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                               struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof(*signed_fields) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof(*private_fields) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

struct rocketnet_mint_thread_result_t
{
    bool ok;
    int mints;
};

// one thread's independent run: mint into its own buffer, then verify and open
// every token against the test keys and its own private fields
static void rocketnet_test_connect_token_mint_thread( struct rocketnet_signed_connect_data_t signed_fields,
                                                      struct rocketnet_connect_token_private_t private_fields,
                                                      uint64_t base_client_id,
                                                      int iterations,
                                                      struct rocketnet_mint_thread_result_t * result )
{
    result->ok = true;
    result->mints = 0;

    for ( int i = 0; i < iterations; i++ )
    {
        struct rocketnet_connect_token_private_t fields = private_fields;
        fields.client_id = base_client_id + (uint64_t) i;

        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        if ( !rocketnet_write_connect_token( &signed_fields, &fields, rocketnet_test_backend_secret_key, token_data ) )
        {
            result->ok = false;
            return;
        }

        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );
        if ( !rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, rocketnet_test_backend_public_key ) )
        {
            result->ok = false;
            return;
        }

        struct rocketnet_connect_token_private_t opened;
        if ( !rocketnet_open_connect_token_private( &opened, token.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) )
        {
            result->ok = false;
            return;
        }

        if ( opened.client_id != fields.client_id )
        {
            result->ok = false;
            return;
        }

        if ( memcmp( opened.client_to_server_key, fields.client_to_server_key, ROCKETNET_KEY_BYTES ) != 0 ||
             memcmp( opened.server_to_client_key, fields.server_to_client_key, ROCKETNET_KEY_BYTES ) != 0 )
        {
            result->ok = false;
            return;
        }

        result->mints++;
    }
}

void rocketnet_test_connect_token_mint_concurrent()
{
    // two threads mint independent tokens at once, each with its own client
    // ids and output buffer, and each opens only its own tokens: minting must
    // touch no state outside its own call
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );

    const int iterations = 300;
    struct rocketnet_mint_thread_result_t results[2];

    std::thread thread_a( rocketnet_test_connect_token_mint_thread, signed_fields, private_fields,
                          ROCKETNET_TEST_CLIENT_ID + 1, iterations, &results[0] );
    std::thread thread_b( rocketnet_test_connect_token_mint_thread, signed_fields, private_fields,
                          ROCKETNET_TEST_CLIENT_ID + 0x100000, iterations, &results[1] );
    thread_a.join();
    thread_b.join();

    rocketnet_check( results[0].ok );
    rocketnet_check( results[1].ok );
    rocketnet_check( results[0].mints == iterations );
    rocketnet_check( results[1].mints == iterations );
}

// two independent ack endpoints -- one per connection, as a client and a
// server in one process each own theirs -- apply ack blocks at once on two
// threads. the ack walk must touch no state outside its own endpoint: under
// a thread sanitizer any shared write here is a reported race.

struct rocketnet_ack_thread_result_t
{
    bool ok;
    int acked;
};

static void rocketnet_test_ack_endpoint_thread( int rounds, struct rocketnet_ack_thread_result_t * result )
{
    result->ok = false;
    result->acked = 0;

    struct rocketnet_ack_endpoint_t * endpoint = (struct rocketnet_ack_endpoint_t*)
        rocketnet_malloc( NULL, sizeof(struct rocketnet_ack_endpoint_t) );
    if ( !endpoint )
        return;
    rocketnet_ack_endpoint_reset( endpoint );

    uint64_t acked[64];
    uint64_t unknown = 0;
    uint64_t batch_full = 0;
    double time = 1000.0;
    uint64_t sequence = 100;

    bool ok = true;
    for ( int round = 0; round < rounds; round++ )
    {
        // send 16 packets, then apply one block that acknowledges all of them

        const uint64_t first = sequence;
        for ( int i = 0; i < 16; i++, sequence++ )
            rocketnet_ack_endpoint_record_sent( endpoint, sequence, 100, sequence, 1, 0, time );

        struct rocketnet_ack_block_t block;
        memset( &block, 0, sizeof(block) );
        block.largest = sequence - 1;
        block.first_range_length = (uint32_t) ( sequence - first );

        time += 0.01;
        int acked_count = 0;
        const int newly_acked = rocketnet_ack_endpoint_process_block( endpoint, &block, time, acked, 64, &acked_count, &unknown, &batch_full );
        if ( newly_acked != 16 || acked_count != 16 )
            ok = false;
        result->acked += newly_acked;
    }

    rocketnet_free( NULL, endpoint );
    result->ok = ok && unknown == 0 && batch_full == 0;
}

void rocketnet_test_ack_endpoints_independent_across_threads()
{
    const int rounds = 2000;
    struct rocketnet_ack_thread_result_t results[2];

    std::thread thread_a( rocketnet_test_ack_endpoint_thread, rounds, &results[0] );
    std::thread thread_b( rocketnet_test_ack_endpoint_thread, rounds, &results[1] );
    thread_a.join();
    thread_b.join();

    rocketnet_check( results[0].ok );
    rocketnet_check( results[1].ok );
    rocketnet_check( results[0].acked == rounds * 16 );
    rocketnet_check( results[1].acked == rounds * 16 );
}
