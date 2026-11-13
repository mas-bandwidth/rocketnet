/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport's memory plan and operator memory budget, and the
// refused and failed paths of its create (mutation gaps, group budget):
//
//   * the plan is exactly what create asks the caller's allocator for: a
//     step-driven loopback client, and a client of the portable shape that
//     races both families (the caller's step-driven transport allocating
//     what the portable client's I/O allocates), and a NULL config plans the
//     defaults;
//   * the budget: a budget equal to the plan opens, the plan less one byte
//     and a 1-byte budget refuse with TRANSPORT_LADDER_EXHAUSTED before any
//     allocation (status written, a NULL status taken), a budget that
//     refuses one rung opens the next, a pin over budget refuses although a
//     candidate it does not name would fit, and an unparsable
//     ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES is CONFIG_ENV_INVALID;
//   * an exhausted ladder logs exactly the names that refused and were
//     skipped, the stack under the create dirtied first;
//   * every refused create frees exactly what it allocated, with the
//     allocator's own context: each allocation failed in turn (the memory
//     plan seam's blocks, the link simulator's patterns, the family race),
//     and a create refused for its token's family or its development key.
//
// The development library's memory plan seam and step-driven create stand
// in for sockets: no socket, no thread, no sleep, and the clock is 1000.0.
// The environment is set and restored within each case through
// tests/test_env_source.h.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"
#include "rocketnet_memory.h"
#include "rocketnet_platform.h"
#include "rocketnet_driver.h"
#include "rocketnet_connect_token.h"
#if ROCKETNET_DEV
#include "rocketnet_client_io.h"
#endif // #if ROCKETNET_DEV
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// a failed check names the fault before the check reports its line

#define gc_expect( condition, ... )                                                                                   \
    do                                                                                                                \
    {                                                                                                                 \
        if ( !( condition ) )                                                                                         \
        {                                                                                                             \
            printf( "mutation gap fault: " __VA_ARGS__ );                                                             \
            printf( "\n" );                                                                                           \
        }                                                                                                             \
        check( condition );                                                                                           \
    } while ( 0 )

#if defined( _MSC_VER )
#define GC_NOINLINE __declspec( noinline )
#else
#define GC_NOINLINE __attribute__( ( noinline ) )
#endif // #if defined( _MSC_VER )

// ---------------------------------------------------------------------------
// The heap: counts every allocation and free and the bytes asked for, fails
// the fail_at-th call, keeps every live block in a table (a free of a block
// not live is counted, not undefined), and counts every free whose context
// is not the one the allocation was made with.
// ---------------------------------------------------------------------------

#define GC_MAX_BLOCKS 4096

struct gc_block_t
{
    void * base;
    void * user;
    void * context;
};

struct gc_heap_t
{
    int calls;
    int frees;
    int live;
    uint64_t bytes;
    int fail_at;
    int bad_frees;
    int wrong_context_frees;
    gc_block_t blocks[GC_MAX_BLOCKS];
    int num_blocks;
};

static gc_heap_t gc_heap;

static void * gc_allocate( void * context, size_t bytes )
{
    const int call = ++gc_heap.calls;
    if ( gc_heap.fail_at == call || gc_heap.num_blocks >= GC_MAX_BLOCKS )
    {
        return NULL;
    }

    void * base = malloc( bytes + 128 );
    if ( !base )
    {
        return NULL;
    }

    uintptr_t address = ( ( (uintptr_t) base + 63 ) & ~(uintptr_t) 63 ) + 64;
    memset( (void *) address, 0xA5, bytes );
    gc_heap.blocks[gc_heap.num_blocks].base = base;
    gc_heap.blocks[gc_heap.num_blocks].user = (void *) address;
    gc_heap.blocks[gc_heap.num_blocks].context = context;
    gc_heap.num_blocks++;
    gc_heap.live++;
    gc_heap.bytes += bytes;

    return (void *) address;
}

static void gc_release( void * context, void * pointer )
{
    if ( !pointer )
    {
        return;
    }

    for ( int i = 0; i < gc_heap.num_blocks; i++ )
    {
        if ( gc_heap.blocks[i].user != pointer )
        {
            continue;
        }
        if ( gc_heap.blocks[i].context != context )
        {
            gc_heap.wrong_context_frees++;
        }
        free( gc_heap.blocks[i].base );
        gc_heap.blocks[i] = gc_heap.blocks[--gc_heap.num_blocks];
        gc_heap.live--;
        gc_heap.frees++;
        return;
    }

    gc_heap.bad_frees++;
}

static void gc_begin()
{
    memset( &gc_heap, 0, sizeof( gc_heap ) );
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );

    int status = -1;
    check( rocketnet_init_internal( &gc_heap, gc_allocate, gc_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_test_loopback_hub_context( &gc_heap );
}

static void gc_end()
{
    gc_heap.fail_at = 0;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    gc_expect( gc_heap.live == 0, "%d blocks still live after term", gc_heap.live );
    gc_expect( gc_heap.bad_frees == 0, "%d frees of a block that was not live (a double free)", gc_heap.bad_frees );
    gc_expect( gc_heap.wrong_context_frees == 0, "%d frees passed a context other than the allocator's", gc_heap.wrong_context_frees );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// The environment the cases change, held aside and put back
// ---------------------------------------------------------------------------

struct gc_env_saved_t
{
    bool was_set;
    char value[256];
};

static const char * gc_env_names[] = { "ROCKETNET_CLIENT_TRANSPORT", "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES" };
#define GC_ENV_NAMES ( (int) ( sizeof( gc_env_names ) / sizeof( gc_env_names[0] ) ) )

static void gc_env_set_aside( gc_env_saved_t * saved )
{
    for ( int i = 0; i < GC_ENV_NAMES; i++ )
    {
        saved[i].was_set = rocketnet_test_env_getenv( gc_env_names[i], saved[i].value, sizeof( saved[i].value ) ) != 0;
        check( rocketnet_test_env_unsetenv( gc_env_names[i] ) == 0 );
    }
}

static void gc_env_put_back( const gc_env_saved_t * saved )
{
    for ( int i = 0; i < GC_ENV_NAMES; i++ )
    {
        if ( saved[i].was_set )
        {
            check( rocketnet_test_env_setenv( gc_env_names[i], saved[i].value, 1 ) == 0 );
        }
        else
        {
            check( rocketnet_test_env_unsetenv( gc_env_names[i] ) == 0 );
        }
    }
}

// ---------------------------------------------------------------------------
// Tokens: signed data only, which is all create reads. IPv4 127.0.0.1:40000,
// and for the family race IPv6 [::1]:40000 too.
// ---------------------------------------------------------------------------

#define GC_FAMILY_IPV4 1
#define GC_FAMILY_IPV6 2

static void gc_make_token( uint8_t * token, int families, const uint8_t * server_public_key )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    if ( families & GC_FAMILY_IPV4 )
    {
        signed_fields.address_flags |= ROCKETNET_ADDRESS_FLAG_IPV4;
        signed_fields.ipv4_address[0] = 127;
        signed_fields.ipv4_address[3] = 1;
        signed_fields.ipv4_address[4] = 0x40;
        signed_fields.ipv4_address[5] = 0x9c;
    }
    if ( families & GC_FAMILY_IPV6 )
    {
        signed_fields.address_flags |= ROCKETNET_ADDRESS_FLAG_IPV6;
        signed_fields.ipv6_address[14] = 1;
        signed_fields.ipv6_address[16] = 0x40;
        signed_fields.ipv6_address[17] = 0x9c;
    }
    memcpy( signed_fields.server_public_key, server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

// the client defaults with a filter that is not the published test filter,
// which a production library refuses

static void gc_client_config( struct rocketnet_client_transport_config_t * config, const char * transport, int step_driven )
{
    rocketnet_client_transport_default_config( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    snprintf( config->transport, sizeof( config->transport ), "%s", transport );
    config->step_driven = (uint64_t) step_driven;
    config->loopback_hub = rocketnet_test_loopback_hub();
}

#if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// The log: every ERROR line, as the public log callback hands it over
// ---------------------------------------------------------------------------

#define GC_MAX_LINES 16

static char gc_lines[GC_MAX_LINES][1024];
static int gc_num_lines = 0;

static void gc_log_capture( void *, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    if ( level != ROCKETNET_LOG_LEVEL_ERROR || gc_num_lines >= GC_MAX_LINES )
    {
        return;
    }
    const size_t bytes = length < sizeof( gc_lines[0] ) - 1 ? length : sizeof( gc_lines[0] ) - 1;
    memcpy( gc_lines[gc_num_lines], message, bytes );
    gc_lines[gc_num_lines][bytes] = '\0';
    gc_num_lines++;
}

struct gc_log_scope_t
{
    enum rocketnet_log_level_t saved;

    gc_log_scope_t()
    {
        saved = rocketnet_log_level_current();
        gc_num_lines = 0;
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
        rocketnet_log_function( gc_log_capture, NULL );
    }

    ~gc_log_scope_t()
    {
        rocketnet_log_function( NULL, NULL );
        rocketnet_log_level( saved );
    }
};

static const char * gc_line_starting( const char * prefix )
{
    for ( int i = 0; i < gc_num_lines; i++ )
    {
        if ( strncmp( gc_lines[i], prefix, strlen( prefix ) ) == 0 )
        {
            return gc_lines[i];
        }
    }

    return NULL;
}

static void gc_print_lines()
{
    for ( int i = 0; i < gc_num_lines; i++ )
    {
        printf( "  logged: %s\n", gc_lines[i] );
    }
}

// fills the stack the create's frames will take with nonzero bytes and no
// NUL, so a name list read before it is written reads garbage

GC_NOINLINE static void gc_dirty_stack()
{
    volatile uint8_t junk[256 * 1024];
    for ( size_t i = 0; i < sizeof( junk ); i++ )
    {
        junk[i] = 0x5A;
    }
}

// ---------------------------------------------------------------------------
// The caller's step-driven transport, the portable client's shape: each open
// allocates what the portable client's I/O allocates (its socket, its client
// record of three pointers, its wake), so a create's allocations can be
// held against the plan. refuse makes every open a refusal (no allocation,
// no status); every open is counted.
// ---------------------------------------------------------------------------

struct gc_io_t
{
    void * context;
    void * socket;
    void * wake;
};

static bool gc_io_refuse = false;
static int gc_io_opens = 0;

static uint64_t gc_raw( uint64_t allocation_bytes )
{
    return allocation_bytes - rocketnet_allocation_overhead();
}

static void * gc_io_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * )
{
    gc_io_opens++;
    if ( gc_io_refuse )
    {
        return NULL;
    }

    gc_io_t * io = (gc_io_t *) rocketnet_malloc( context, sizeof( gc_io_t ) );
    if ( !io )
    {
        rocketnet_create_status( config->status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    io->context = context;
    io->socket = rocketnet_malloc( context, (size_t) gc_raw( rocketnet_platform_socket_allocation_bytes() ) );
    const uint64_t wake = rocketnet_platform_wake_allocation_bytes();
    io->wake = wake ? rocketnet_malloc( context, (size_t) gc_raw( wake ) ) : NULL;
    if ( !io->socket || ( wake && !io->wake ) )
    {
        rocketnet_free( context, io->socket );
        rocketnet_free( context, io->wake );
        rocketnet_free( context, io );
        rocketnet_create_status( config->status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    return io;
}

static void gc_io_destroy( void * transport )
{
    gc_io_t * io = (gc_io_t *) transport;
    rocketnet_free( io->context, io->wake );
    rocketnet_free( io->context, io->socket );
    rocketnet_free( io->context, io );
}

static void gc_io_send( void *, const rocketnet_address_t *, const uint8_t *, int ) {}
static int gc_io_receive( void *, rocketnet_address_t *, uint8_t *, int ) { return 0; }
static int gc_io_receive_batch( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) { return 0; }

static rocketnet_client_transport_io_operations_t gc_io_operations()
{
    rocketnet_client_transport_io_operations_t operations = {};
    operations.create = gc_io_create;
    operations.destroy = gc_io_destroy;
    operations.send = gc_io_send;
    operations.receive = gc_io_receive;
    operations.receive_batch = gc_io_receive_batch; // a non-blocking step, so a step-driven config takes it
    return operations;
}

// the I/O bytes the portable client plans: its socket, its record, its wake

static uint64_t gc_portable_io_plan()
{
    uint64_t bytes = rocketnet_platform_socket_allocation_bytes() + rocketnet_memory_allocations( { sizeof( gc_io_t ) } );
    bytes += rocketnet_platform_wake_allocation_bytes();
    return bytes;
}

extern void rocketnet_client_transport_set_memory_plan_seam_for_test( bool enable );

struct gc_seam_scope_t
{
    gc_seam_scope_t() { rocketnet_client_transport_set_memory_plan_seam_for_test( true ); }
    ~gc_seam_scope_t() { rocketnet_client_transport_set_memory_plan_seam_for_test( false ); }
};

static double gc_clock_now = 1000.0;
static double gc_clock( void * ) { return gc_clock_now; }

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

// mutation gap: hunt 2/6754 (a NULL config plans uninitialised defaults),
// 6763 (loopback plans a byte it never allocates), 6765 (a threaded
// loopback has no plan), 6783 (the portable plan uses another client's I/O
// record), 6791 (the race plans no second socket), 6796 (a step-driven
// client plans a thread)

static void test_gaps_budget_client_plan_is_what_create_allocates()
{
    gc_env_saved_t saved[GC_ENV_NAMES];
    gc_env_set_aside( saved );
    gc_begin();

    // a step-driven loopback client: its endpoint is in the hub, so the plan
    // is the transport and its simulator, exactly what create allocates

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gc_make_token( token, GC_FAMILY_IPV4, rocketnet_dev_server_public_key );

    struct rocketnet_client_transport_config_t loopback;
    gc_client_config( &loopback, "loopback", 1 );
    loopback.time_callback = gc_clock;
    const uint64_t loopback_plan = rocketnet_client_transport_required_memory( &loopback, "loopback" );
    gc_expect( loopback_plan > 0, "a step-driven loopback client has no memory plan" );

    uint64_t bytes_before = gc_heap.bytes;
    int status = -1;
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_current_internal( &gc_heap, token, &loopback, 1000.0, &status );
    gc_expect( client != NULL, "a create that must succeed was refused: status %s", rocketnet_status_name( (enum rocketnet_status_t) status ) );
    const uint64_t loopback_allocated = gc_heap.bytes - bytes_before;
    rn_test_client_transport_stop_and_destroy( client );
    gc_expect( loopback_plan == loopback_allocated, "a step-driven loopback client plans %llu bytes, its create allocated %llu",
               (unsigned long long) loopback_plan, (unsigned long long) loopback_allocated );

    // a threaded loopback client plans its network thread on top

    struct rocketnet_client_transport_config_t threaded = loopback;
    threaded.step_driven = 0;
    const uint64_t threaded_plan = rocketnet_client_transport_required_memory( &threaded, "loopback" );
    gc_expect( threaded_plan == loopback_plan + rocketnet_platform_thread_allocation_bytes(),
               "a threaded loopback client plans %llu bytes, not the step-driven %llu plus its thread's %llu", (unsigned long long) threaded_plan,
               (unsigned long long) loopback_plan, (unsigned long long) rocketnet_platform_thread_allocation_bytes() );

    // the portable shape, step-driven: one family, then both families raced
    // (the IPv6 server opened, then the IPv4 server on a second socket)

    struct rocketnet_client_transport_config_t portable;
    gc_client_config( &portable, "portable", 1 );
    portable.time_callback = gc_clock;
    const uint64_t portable_plan = rocketnet_client_transport_required_memory( &portable, "portable" );
    gc_expect( portable_plan > loopback_plan, "the portable plan %llu is not above loopback's %llu", (unsigned long long) portable_plan,
               (unsigned long long) loopback_plan );

    const rocketnet_client_transport_io_operations_t operations = gc_io_operations();

    bytes_before = gc_heap.bytes;
    client = rocketnet_client_transport_create_step_driven_internal( &gc_heap, token, &portable, 1000.0, &operations, &gc_heap, &status );
    gc_expect( client != NULL, "a create that must succeed was refused: status %s", rocketnet_status_name( (enum rocketnet_status_t) status ) );
    const uint64_t one_family = gc_heap.bytes - bytes_before;
    rn_test_client_transport_stop_and_destroy( client );

    uint8_t both_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gc_make_token( both_token, GC_FAMILY_IPV4 | GC_FAMILY_IPV6, rocketnet_dev_server_public_key );
    bytes_before = gc_heap.bytes;
    const int opens_before = gc_io_opens;
    client = rocketnet_client_transport_create_step_driven_internal( &gc_heap, both_token, &portable, 1000.0, &operations, &gc_heap, &status );
    gc_expect( client != NULL, "a create that must succeed was refused: status %s", rocketnet_status_name( (enum rocketnet_status_t) status ) );
    const uint64_t both_families = gc_heap.bytes - bytes_before;
    check( gc_io_opens - opens_before == 2 );
    rn_test_client_transport_stop_and_destroy( client );

    // the fake's I/O is the portable client's records, and the race block is
    // what racing allocates beyond its second socket

    const uint64_t io = gc_portable_io_plan();
    const uint64_t one_family_io = one_family - loopback_allocated;
    check( one_family_io == io );
    const uint64_t race_block = both_families - 2 * one_family + loopback_allocated;

    // the memory plan seam allocates the plan's own I/O records for one
    // family (it races nothing): the plan is that, the race block and a
    // second family's records, so a plan that races without the second
    // socket, or plans another client's records, is caught

    uint64_t seam_one_family = 0;
    {
        gc_seam_scope_t seam;
        bytes_before = gc_heap.bytes;
        client = rocketnet_client_transport_create_current_internal( &gc_heap, token, &portable, 1000.0, &status );
        gc_expect( client != NULL, "a create that must succeed was refused: status %s", rocketnet_status_name( (enum rocketnet_status_t) status ) );
        seam_one_family = gc_heap.bytes - bytes_before;
        rn_test_client_transport_stop_and_destroy( client );
    }
    const uint64_t planned_io = seam_one_family - loopback_allocated;
    gc_expect( portable_plan == seam_one_family + race_block + planned_io,
               "the portable client plans %llu bytes, not one family's %llu plus the race block's %llu plus a second family's I/O %llu",
               (unsigned long long) portable_plan, (unsigned long long) seam_one_family, (unsigned long long) race_block, (unsigned long long) planned_io );

    // a threaded portable client plans its network thread; a step-driven one none

    struct rocketnet_client_transport_config_t portable_threaded = portable;
    portable_threaded.step_driven = 0;
    const uint64_t portable_threaded_plan = rocketnet_client_transport_required_memory( &portable_threaded, "portable" );
    gc_expect( portable_threaded_plan == portable_plan + rocketnet_platform_thread_allocation_bytes(),
               "a threaded portable client plans %llu bytes, not the step-driven %llu plus its thread's %llu", (unsigned long long) portable_threaded_plan,
               (unsigned long long) portable_plan, (unsigned long long) rocketnet_platform_thread_allocation_bytes() );

    // a NULL config plans the defaults

    gc_dirty_stack();
    const uint64_t null_plan = rocketnet_client_transport_required_memory( NULL, "portable" );
    struct rocketnet_client_transport_config_t defaults;
    rocketnet_client_transport_default_config( &defaults );
    gc_expect( null_plan == rocketnet_client_transport_required_memory( &defaults, "portable" ), "a NULL config plans %llu bytes, the defaults %llu",
               (unsigned long long) null_plan, (unsigned long long) rocketnet_client_transport_required_memory( &defaults, "portable" ) );

    gc_end();
    gc_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_client_plan_is_what_create_allocates );

// ---------------------------------------------------------------------------
// The budget
// ---------------------------------------------------------------------------

#define GC_BUDGET_EXHAUSTED_PREFIX "client transport ladder exhausted: every candidate's memory plan exceeds the "

static void gc_expect_budget_refusal( const uint8_t * token, const struct rocketnet_client_transport_config_t * config, uint64_t budget, const char * what )
{
    const int calls_before = gc_heap.calls;
    const int live_before = gc_heap.live;

    gc_log_scope_t log;
    int status = ROCKETNET_STATUS_OK;
    gc_dirty_stack();
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_resolved( &gc_heap, token, config, 1000.0, budget, 0, &status );
    gc_expect( client == NULL, "%s: a budget of %llu bytes did not refuse the create (granted %s)", what, (unsigned long long) budget,
               client ? rocketnet_client_transport_io_name( client ) : "-" );
    rn_test_client_transport_stop_and_destroy( client );
    gc_expect( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "%s: the budget refusal's status is %s, not TRANSPORT_LADDER_EXHAUSTED", what,
               rocketnet_status_name( (enum rocketnet_status_t) status ) );
    gc_expect( gc_heap.calls == calls_before, "%s: the budget refusal came after %d allocations, not before the first", what, gc_heap.calls - calls_before );
    gc_expect( gc_heap.live == live_before, "%s: %d blocks leaked", what, gc_heap.live - live_before );

    char expected[256];
    snprintf( expected, sizeof( expected ), GC_BUDGET_EXHAUSTED_PREFIX "%llu byte operator budget", (unsigned long long) budget );
    const char * line = gc_line_starting( GC_BUDGET_EXHAUSTED_PREFIX );
    if ( !line || strcmp( line, expected ) != 0 )
    {
        gc_print_lines();
    }
    gc_expect( line != NULL && strcmp( line, expected ) == 0, "%s: the budget refusal did not log \"%s\"", what, expected );

    gc_dirty_stack();
    client = rocketnet_client_transport_create_resolved( &gc_heap, token, config, 1000.0, budget, 0, NULL );
    gc_expect( client == NULL, "%s: with a NULL status a budget of %llu bytes did not refuse the create", what, (unsigned long long) budget );
    rn_test_client_transport_stop_and_destroy( client );
    gc_expect( gc_heap.calls == calls_before, "%s: with a NULL status the budget refusal came after %d allocations", what, gc_heap.calls - calls_before );
}

// a client transport whose plan is above portable's, or NULL

static const char * gc_transport_over_portable( const struct rocketnet_client_transport_config_t * config )
{
    static const char * names[] = { "gso", "msgx", "iocp" };
    const uint64_t portable = rocketnet_client_transport_required_memory( config, "portable" );
    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );
    for ( size_t i = 0; i < sizeof( names ) / sizeof( names[0] ); i++ )
    {
        bool here = false;
        for ( int j = 0; j < num_available; j++ )
        {
            if ( strcmp( rocketnet_transport_name( available[j] ), names[i] ) == 0 )
            {
                here = true;
            }
        }
        if ( here && rocketnet_client_transport_required_memory( config, names[i] ) > portable )
        {
            return names[i];
        }
    }

    return NULL;
}

// mutation gap: hunt 2/6977, 6979, 6980, 6983, 6984, 6987, 6993, 6994, 6995,
// 6996, 6998, 6999 (the budget's refusal before allocation, its status, its
// boundaries), 6986, 6988 (a budget that refuses one rung opens the next),
// 6978 (a pin over budget refuses although portable, unlisted, fits)

static void test_gaps_budget_client_budget_refuses_before_allocation()
{
    gc_env_saved_t saved[GC_ENV_NAMES];
    gc_env_set_aside( saved );
    gc_begin();
    gc_seam_scope_t seam;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gc_make_token( token, GC_FAMILY_IPV4, rocketnet_dev_server_public_key );

    struct rocketnet_client_transport_config_t config;
    gc_client_config( &config, "portable", 1 );
    config.time_callback = gc_clock;
    const uint64_t plan = rocketnet_client_transport_required_memory( &config, "portable" );
    check( plan > 1 );

    const uint64_t open_budgets[] = { plan, plan + 1, 0 };
    for ( int i = 0; i < 3; i++ )
    {
        int status = -1;
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_resolved( &gc_heap, token, &config, 1000.0, open_budgets[i], 0, &status );
        gc_expect( client != NULL, "a budget of %llu bytes refused a plan of %llu bytes (status %s)", (unsigned long long) open_budgets[i],
                   (unsigned long long) plan, rocketnet_status_name( (enum rocketnet_status_t) status ) );
        gc_expect( status == ROCKETNET_STATUS_OK, "a budget of %llu bytes opened with status %s", (unsigned long long) open_budgets[i],
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        rn_test_client_transport_stop_and_destroy( client );
    }

    gc_expect_budget_refusal( token, &config, plan - 1, "pinned portable, budget the plan less one byte" );
    gc_expect_budget_refusal( token, &config, 1, "pinned portable, a 1-byte budget" );

    // two rungs, the first over budget: the second opens

    struct rocketnet_client_transport_config_t two;
    gc_client_config( &two, "portable, loopback", 1 );
    two.time_callback = gc_clock;
    const uint64_t loopback = rocketnet_client_transport_required_memory( &two, "loopback" );
    check( loopback > 0 && loopback < plan );
    {
        int status = -1;
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_resolved( &gc_heap, token, &two, 1000.0, loopback, 0, &status );
        gc_expect( client != NULL, "\"portable, loopback\" under a budget only loopback fits refused the whole create (status %s)",
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        const char * granted = client ? rocketnet_client_transport_io_name( client ) : "-";
        gc_expect( client != NULL && strcmp( granted, "loopback" ) == 0, "\"portable, loopback\" under a budget only loopback fits granted %s, not loopback", granted );
        rn_test_client_transport_stop_and_destroy( client );
    }

    // the pin over budget, portable (which the pin does not name) fitting

    const char * over = gc_transport_over_portable( &config );
    if ( over )
    {
        struct rocketnet_client_transport_config_t pin;
        gc_client_config( &pin, over, 1 );
        pin.time_callback = gc_clock;
        char what[128];
        snprintf( what, sizeof( what ), "pinned %s over a budget portable fits", over );
        gc_expect_budget_refusal( token, &pin, plan, what );
    }

    gc_end();
    gc_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_client_budget_refuses_before_allocation );

// mutation gap: hunt 2/6972, 6973 (an unparsable budget variable: the status
// written, a NULL status taken)

static void test_gaps_budget_client_budget_env_invalid()
{
    gc_env_saved_t saved[GC_ENV_NAMES];
    gc_env_set_aside( saved );
    gc_begin();
    gc_seam_scope_t seam;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gc_make_token( token, GC_FAMILY_IPV4, rocketnet_dev_server_public_key );

    struct rocketnet_client_transport_config_t config;
    gc_client_config( &config, "portable", 1 );
    config.time_callback = gc_clock;
    const uint64_t plan = rocketnet_client_transport_required_memory( &config, "portable" );

    check( rocketnet_test_env_setenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", "not-a-number", 1 ) == 0 );

    const int calls_before = gc_heap.calls;
    int status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_current_internal( &gc_heap, token, &config, 1000.0, &status );
    gc_expect( client == NULL, "an unparsable ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES did not refuse the create" );
    rn_test_client_transport_stop_and_destroy( client );
    gc_expect( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID, "an unparsable ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES gave status %s, not CONFIG_ENV_INVALID",
               rocketnet_status_name( (enum rocketnet_status_t) status ) );
    gc_expect( gc_heap.calls == calls_before, "an unparsable budget variable was refused after %d allocations", gc_heap.calls - calls_before );

    client = rocketnet_client_transport_create_current_internal( &gc_heap, token, &config, 1000.0, NULL );
    gc_expect( client == NULL, "with a NULL status an unparsable budget variable did not refuse the create" );
    rn_test_client_transport_stop_and_destroy( client );

    char budget[64];
    snprintf( budget, sizeof( budget ), "%llu", (unsigned long long) ( plan - 1 ) );
    check( rocketnet_test_env_setenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", budget, 1 ) == 0 );
    status = ROCKETNET_STATUS_OK;
    client = rocketnet_client_transport_create_current_internal( &gc_heap, token, &config, 1000.0, &status );
    gc_expect( client == NULL && status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES=%s did not refuse a plan of %llu bytes", budget,
               (unsigned long long) plan );
    rn_test_client_transport_stop_and_destroy( client );

    gc_end();
    gc_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_client_budget_env_invalid );

// ---------------------------------------------------------------------------
// The exhausted ladder's lines
// ---------------------------------------------------------------------------

#define GC_EXHAUSTED_PREFIX "client transport ladder exhausted with no grant. "

static const char * gc_unavailable_transport()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return "msgx";
#else
    return "iocp";
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

// mutation gap: hunt 2/7148, 7149 (the skipped names read before set)

static void test_gaps_budget_client_exhausted_ladder_names_exactly()
{
    gc_env_saved_t saved[GC_ENV_NAMES];
    gc_env_set_aside( saved );
    gc_begin();

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gc_make_token( token, GC_FAMILY_IPV4, rocketnet_dev_server_public_key );
    const rocketnet_client_transport_io_operations_t operations = gc_io_operations();

    char skipped_list[64];
    snprintf( skipped_list, sizeof( skipped_list ), "%s, portable", gc_unavailable_transport() );
    char skipped_want[256];
    snprintf( skipped_want, sizeof( skipped_want ), GC_EXHAUSTED_PREFIX "refused: portable. skipped for platform: %s", gc_unavailable_transport() );

    const char * lists[] = { "portable", skipped_list };
    const char * wants[] = { GC_EXHAUSTED_PREFIX "refused: portable. skipped for platform: (none)", skipped_want };

    gc_io_refuse = true;
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_transport_config_t config;
        gc_client_config( &config, lists[i], 1 );
        config.time_callback = gc_clock;

        const int live_before = gc_heap.live;
        gc_log_scope_t log;
        int status = ROCKETNET_STATUS_OK;
        gc_dirty_stack();
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_step_driven_internal( &gc_heap, token, &config, 1000.0, &operations, &gc_heap, &status );
        gc_expect( client == NULL, "\"%s\" with every open refused was granted", lists[i] );
        rn_test_client_transport_stop_and_destroy( client );
        gc_expect( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "\"%s\" exhausted with status %s, not TRANSPORT_LADDER_EXHAUSTED", lists[i],
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        const char * line = gc_line_starting( GC_EXHAUSTED_PREFIX );
        if ( !line || strcmp( line, wants[i] ) != 0 )
        {
            gc_print_lines();
        }
        gc_expect( line != NULL && strcmp( line, wants[i] ) == 0, "\"%s\" exhausted and logged \"%s\", not \"%s\"", lists[i], line ? line : "(nothing)", wants[i] );
        gc_expect( gc_heap.live == live_before, "\"%s\" exhausted and leaked %d blocks", lists[i], gc_heap.live - live_before );
    }
    gc_io_refuse = false;

    gc_end();
    gc_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_client_exhausted_ladder_names_exactly );

// ---------------------------------------------------------------------------
// Refused creates free what they allocated
// ---------------------------------------------------------------------------

// fails each allocation of a create in turn: every one is OUT_OF_MEMORY with
// nothing live and every free made with the allocator's context; returns the
// create's allocation count

typedef struct rocketnet_client_transport_t * ( *gc_create_function_t )( int * status );

static int gc_walk( gc_create_function_t create, const char * what )
{
    const int base_live = gc_heap.live;

    int calls_before = gc_heap.calls;
    int status = -1;
    struct rocketnet_client_transport_t * client = create( &status );
    gc_expect( client != NULL, "a create that must succeed was refused: status %s", rocketnet_status_name( (enum rocketnet_status_t) status ) );
    const int allocations = gc_heap.calls - calls_before;
    rn_test_client_transport_stop_and_destroy( client );
    check( gc_heap.live == base_live );

    for ( int k = 1; k <= allocations; k++ )
    {
        calls_before = gc_heap.calls;
        const int frees_before = gc_heap.frees;
        gc_heap.fail_at = calls_before + k;
        status = -1;
        client = create( &status );
        gc_heap.fail_at = 0;
        gc_expect( client == NULL, "%s: allocation %d of %d failed, yet the create succeeded", what, k, allocations );
        rn_test_client_transport_stop_and_destroy( client );
        gc_expect( status == ROCKETNET_STATUS_OUT_OF_MEMORY, "%s: allocation %d of %d failed: status %s, not OUT_OF_MEMORY", what, k, allocations,
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        gc_expect( gc_heap.live == base_live, "%s: allocation %d of %d failed: %d blocks leaked (%d allocated, %d freed)", what, k, allocations,
                   gc_heap.live - base_live, gc_heap.calls - calls_before - 1, gc_heap.frees - frees_before );
        gc_expect( gc_heap.bad_frees == 0, "%s: allocation %d of %d failed: a block was freed twice", what, k, allocations );
        gc_expect( gc_heap.wrong_context_frees == 0, "%s: allocation %d of %d failed: %d frees passed a context other than the allocator's", what, k,
                   allocations, gc_heap.wrong_context_frees );
    }

    return allocations;
}

static uint8_t gc_walk_token[ROCKETNET_CONNECT_TOKEN_BYTES];
static struct rocketnet_client_transport_config_t gc_walk_config;

static struct rocketnet_client_transport_t * gc_create_internal( int * status )
{
    return rocketnet_client_transport_create_current_internal( &gc_heap, gc_walk_token, &gc_walk_config, 1000.0, status );
}

static struct rocketnet_client_transport_t * gc_create_step_driven( int * status )
{
    static const rocketnet_client_transport_io_operations_t operations = gc_io_operations();
    return rocketnet_client_transport_create_step_driven_internal( &gc_heap, gc_walk_token, &gc_walk_config, 1000.0, &operations, &gc_heap, status );
}

// mutation gap: hunt 2/7097, 7098, 7101, 7103, 7104, 7105, 7106, 7107, 7110
// (the memory plan seam's allocations failed in turn), 7225 (the link
// simulator's patterns failed), 7171, 7174 (the family race's allocations
// failed in turn)

static void test_gaps_budget_client_out_of_memory_frees_everything()
{
    gc_env_saved_t saved[GC_ENV_NAMES];
    gc_env_set_aside( saved );
    gc_begin();

    // the memory plan seam, the reverse window making the simulator's
    // patterns allocate

    {
        gc_seam_scope_t seam;
        gc_make_token( gc_walk_token, GC_FAMILY_IPV4, rocketnet_dev_server_public_key );
        gc_client_config( &gc_walk_config, "portable", 1 );
        gc_walk_config.time_callback = gc_clock;
        gc_walk_config.simulated_reverse_window = 2;
        gc_walk( gc_create_internal, "the memory plan seam" );
    }

    // the family race over the caller's transport

    gc_make_token( gc_walk_token, GC_FAMILY_IPV4 | GC_FAMILY_IPV6, rocketnet_dev_server_public_key );
    gc_client_config( &gc_walk_config, "portable", 1 );
    gc_walk_config.time_callback = gc_clock;
    const int opens_before = gc_io_opens;
    gc_walk( gc_create_step_driven, "the family race" );
    check( gc_io_opens - opens_before > 2 );

    gc_end();
    gc_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_client_out_of_memory_frees_everything );

// mutation gap: hunt 2/7048 (an IPv6-only token on a host without IPv6: the
// transport freed)

static void test_gaps_budget_client_no_ipv6_frees_everything()
{
    gc_env_saved_t saved[GC_ENV_NAMES];
    gc_env_set_aside( saved );
    gc_begin();
    gc_seam_scope_t seam;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gc_make_token( token, GC_FAMILY_IPV6, rocketnet_dev_server_public_key );
    struct rocketnet_client_transport_config_t config;
    gc_client_config( &config, "portable", 1 );
    config.time_callback = gc_clock;

    rocketnet_platform_set_ipv6_socket_available_for_testing( false );
    const int live_before = gc_heap.live;
    const int calls_before = gc_heap.calls;
    int status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_current_internal( &gc_heap, token, &config, 1000.0, &status );
    rocketnet_platform_set_ipv6_socket_available_for_testing( true );
    gc_expect( client == NULL, "an IPv6-only token on a host without IPv6 was granted" );
    rn_test_client_transport_stop_and_destroy( client );
    gc_expect( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "an IPv6-only token on a host without IPv6: status %s, not TRANSPORT_LADDER_EXHAUSTED",
               rocketnet_status_name( (enum rocketnet_status_t) status ) );
    check( gc_heap.calls > calls_before );
    gc_expect( gc_heap.live == live_before, "an IPv6-only token on a host without IPv6 leaked %d blocks", gc_heap.live - live_before );

    gc_end();
    gc_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_client_no_ipv6_frees_everything );

#else // #if ROCKETNET_DEV

// mutation gap: hunt 2/7039 (a production library refuses a token sealed to
// the development server key: the transport and its protocol state freed).
// The production library only: the development library warns and goes on.

static void test_gaps_budget_client_dev_key_refusal_frees_everything()
{
    gc_env_saved_t saved[GC_ENV_NAMES];
    gc_env_set_aside( saved );
    gc_begin();

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gc_make_token( token, GC_FAMILY_IPV4, rocketnet_dev_server_public_key );
    struct rocketnet_client_transport_config_t config;
    gc_client_config( &config, "portable", 0 );

    const int live_before = gc_heap.live;
    const int calls_before = gc_heap.calls;
    int status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_current_internal( &gc_heap, token, &config, 1000.0, &status );
    gc_expect( client == NULL, "a production library took a token sealed to the development server key" );
    rn_test_client_transport_stop_and_destroy( client );
    gc_expect( status == ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION, "a development-key token: status %s, not DEV_CONNECT_TOKEN_IN_PRODUCTION",
               rocketnet_status_name( (enum rocketnet_status_t) status ) );
    check( gc_heap.calls > calls_before );
    gc_expect( gc_heap.live == live_before, "a refused development-key token leaked %d blocks", gc_heap.live - live_before );

    gc_end();
    gc_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_client_dev_key_refusal_frees_everything );

#endif // #if ROCKETNET_DEV
