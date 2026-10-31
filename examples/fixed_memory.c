/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Fixed memory: size a server and a client before creating them, then hold
    each one to exactly that many bytes. rocketnet_server_required_memory and
    rocketnet_client_required_memory read the configs the objects will be
    created with; the allocator installed through rocketnet_init refuses any
    request past an object's budget. The whole lifecycle runs over the
    in-memory loopback transport: create, start and connect, a few frames
    with messages, disconnect, reconnect, stop and destroy, with no refusal
    and a peak at or under the size asked for. Then the same server and
    client with one byte less than the peak they reached: start and connect
    refuse with ROCKETNET_STATUS_OUT_OF_MEMORY. Run: ./bin/example_fixed_memory
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// The budget allocator. A game puts its own heap (tlsf or similar) over one
// region per object of exactly the size required_memory returned: that size
// already includes slack for the heap's block headers, alignment and
// fragmentation, so the region needs no margin of its own. This one does not
// implement a heap. It takes each block from malloc, charges the block and
// its own 16-byte header (the cost a heap's block header has) to the
// object's budget, and refuses past it.
//
// Each create passes its context to every allocation for that object, so the
// budget is the context. Allocations with no budget (init's and the hub's,
// context NULL) are not charged. The client below is step-driven, so every
// call comes from this one thread and the counts need no lock; a game's heap
// must be thread-safe, since network threads also call the allocator.
// ---------------------------------------------------------------------------

#define HEADER_BYTES 16

struct budget_t
{
    size_t budget;
    size_t live;
    size_t peak;
    int refusals;
};

static void * budget_malloc( void * context, size_t bytes )
{
    struct budget_t * b = (struct budget_t *) context;
    const size_t charge = bytes + HEADER_BYTES;
    if ( b && ( charge > b->budget || b->live > b->budget - charge ) )
    {
        b->refusals++;
        return NULL;
    }

    unsigned char * block = (unsigned char *) malloc( charge );
    if ( !block )
        return NULL;

    memcpy( block, &charge, sizeof( charge ) );
    if ( b )
    {
        b->live += charge;
        if ( b->live > b->peak )
            b->peak = b->live;
    }

    return block + HEADER_BYTES;
}

static void budget_free( void * context, void * p )
{
    if ( !p )
        return;

    unsigned char * block = (unsigned char *) p - HEADER_BYTES;
    size_t charge;
    memcpy( &charge, block, sizeof( charge ) );
    if ( context )
        ( (struct budget_t *) context )->live -= charge;

    free( block );
}

static void budget_reset( struct budget_t * b, size_t budget )
{
    memset( b, 0, sizeof( *b ) );
    b->budget = budget;
}

// ---------------------------------------------------------------------------

static double now = 1000.0;
static uint64_t next_client_id = 1;

static enum rocketnet_status_t connect_client( struct rocketnet_server_t * server, struct rocketnet_client_t * client )
{
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !rocketnet_server_ipv4_address( server, address, sizeof( address ) ) )
        return ROCKETNET_STATUS_INVALID_ADDRESS;
    rocketnet_server_public_key( server, server_public_key );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( address, NULL, server_public_key, 30, 10, next_client_id++, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, token );
    if ( token_status != ROCKETNET_STATUS_OK )
        return token_status;

    return rocketnet_client_connect( client, token );
}

// the frame loop: update, drain, send one message each way, send packets

static int run_frames( struct rocketnet_server_t * server, struct rocketnet_client_t * client )
{
    int sent = 0, received = 0;

    for ( int frame = 0; frame < 1000 && received < 4; frame++ )
    {
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            received += batch->num_messages;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
            received += batch->num_messages;

        // unreliable: a message with no room this frame is skipped, and the next frame sends another

        const int index = rocketnet_client_index( client );
        if ( index >= 0 && rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 4 ) )
        {
            uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 4 );
            if ( p )
            {
                memcpy( p, "ping", 4 );
                sent += rocketnet_client_finish_message( client, 4 ) != 0;
            }
        }

        if ( index >= 0 && rocketnet_server_can_send_message( server, index, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 4 ) )
        {
            uint8_t * p = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 4 );
            if ( p )
            {
                memcpy( p, "pong", 4 );
                sent += rocketnet_server_finish_message( server, 4 ) != 0;
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        now += 1.0 / 60.0;
    }

    return sent >= 4 && received >= 4;
}

static int fail( const char * what, enum rocketnet_status_t status )
{
    printf( "FAIL: %s: %s\n", what, rocketnet_status_name( status ) );
    return 1;
}

int main( void )
{
    enum rocketnet_status_t status = rocketnet_init( NULL, budget_malloc, budget_free );
    if ( status != ROCKETNET_STATUS_OK )
        return fail( "init", status );

    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 2, &status );
    if ( !hub )
        return fail( "loopback hub create", status );

    // the configs the objects are created with are the configs they are sized with

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    if ( ( status = rocketnet_server_default_config( &server_config ) ) != ROCKETNET_STATUS_OK )
        return fail( "server default config", status );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    server_config.max_clients = 2;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;

    struct rocketnet_client_config_t client_config;
    if ( ( status = rocketnet_client_default_config( &client_config ) ) != ROCKETNET_STATUS_OK )
        return fail( "client default config", status );
    client_config.transport = "loopback";
    client_config.loopback_hub = hub;
    client_config.step_driven = true;

    const size_t server_required = rocketnet_server_required_memory( &server_config );
    const size_t client_required = rocketnet_client_required_memory( &client_config );
    if ( server_required == 0 || client_required == 0 )
    {
        printf( "FAIL: required memory: a config is invalid\n" );
        return 1;
    }

    struct budget_t server_budget, client_budget;
    budget_reset( &server_budget, server_required );
    budget_reset( &client_budget, client_required );

    // the whole lifecycle inside the budgets

    struct rocketnet_server_t * server = rocketnet_server_create( &server_budget, "127.0.0.1:0", NULL, &server_config, &status );
    if ( !server )
        return fail( "server create", status );
    if ( ( status = rocketnet_server_start( server ) ) != ROCKETNET_STATUS_OK )
        return fail( "server start", status );

    struct rocketnet_client_t * client = rocketnet_client_create( &client_budget, &client_config, &status );
    if ( !client )
        return fail( "client create", status );

    for ( int connection = 0; connection < 2; connection++ )
    {
        if ( ( status = connect_client( server, client ) ) != ROCKETNET_STATUS_OK )
            return fail( "client connect", status );
        if ( !run_frames( server, client ) )
            return fail( "messages", ROCKETNET_STATUS_OK );

        // disconnect, then reconnect: the next connect frees the old attempt before it allocates

        rocketnet_client_disconnect( client );
        rn_cli_client_wait_disconnected( client, now );
    }

    rocketnet_client_destroy( client );
    rn_cli_server_stop_and_destroy( server );

    printf( "server: required %zu bytes, peak %zu, refusals %d\n", server_required, server_budget.peak, server_budget.refusals );
    printf( "client: required %zu bytes, peak %zu, refusals %d\n", client_required, client_budget.peak, client_budget.refusals );

    if ( server_budget.refusals || client_budget.refusals || server_budget.peak > server_required || client_budget.peak > client_required ||
         server_budget.live || client_budget.live )
    {
        printf( "FAIL: the lifecycle did not fit the required memory\n" );
        return 1;
    }

    // the negative control: one byte under the peak each object reached, start
    // and connect refuse with OUT_OF_MEMORY and leave nothing behind

    const size_t server_peak = server_budget.peak;
    const size_t client_peak = client_budget.peak;

    budget_reset( &server_budget, server_peak - 1 );
    server = rocketnet_server_create( &server_budget, "127.0.0.1:0", NULL, &server_config, &status );
    if ( server )
        status = rocketnet_server_start( server );
    printf( "server with %zu bytes: %s\n", server_peak - 1, rocketnet_status_name( status ) );
    if ( status != ROCKETNET_STATUS_OUT_OF_MEMORY )
        return fail( "server under budget", status );
    rn_cli_server_stop_and_destroy( server );

    budget_reset( &server_budget, server_required );
    server = rocketnet_server_create( &server_budget, "127.0.0.1:0", NULL, &server_config, &status );
    if ( !server || ( status = rocketnet_server_start( server ) ) != ROCKETNET_STATUS_OK )
        return fail( "server start", status );

    budget_reset( &client_budget, client_peak - 1 );
    client = rocketnet_client_create( &client_budget, &client_config, &status );
    if ( client )
        status = connect_client( server, client );
    printf( "client with %zu bytes: %s\n", client_peak - 1, rocketnet_status_name( status ) );
    if ( status != ROCKETNET_STATUS_OUT_OF_MEMORY )
        return fail( "client under budget", status );
    if ( client )
        rocketnet_client_destroy( client );
    rn_cli_server_stop_and_destroy( server );

    if ( server_budget.live || client_budget.live )
    {
        printf( "FAIL: a refused start or connect left memory behind\n" );
        return 1;
    }

    rn_cli_loopback_hub_destroy( hub );
    if ( ( status = rocketnet_term() ) != ROCKETNET_STATUS_OK )
        return fail( "term", status );

    printf( "PASS\n" );

    return 0;
}
