/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server transport's memory plan and operator memory budget, and the
// refused and failed paths of its create (mutation gaps, group budget):
//
//   * the plan of a loopback server is exactly what create asks the
//     caller's allocator for, at one client and at four, and the plan's
//     parts add up (the xdp plan's UMEM counted once, the ladder's
//     allocator bytes those of the one candidate it names);
//   * the budget: a budget equal to the plan opens, the plan less one byte
//     and a 1-byte budget refuse with TRANSPORT_LADDER_EXHAUSTED before any
//     allocation (status written, a NULL status taken), a budget that
//     refuses one rung opens the next, and a pin over budget refuses even
//     when an unlisted candidate would fit;
//   * an unparsable ROCKETNET_SERVER_MEMORY_BUDGET_BYTES is CONFIG_ENV_INVALID
//     before any allocation, with a status pointer and with NULL;
//   * an exhausted ladder logs exactly the names that refused and were
//     skipped, and its status is the one its refusals earn, with the stack
//     under the create dirtied first so a name list read before it is set
//     shows;
//   * every refused create frees exactly what it allocated: an allocation
//     failed in turn, the link simulator's mutex refused in turn, and a
//     destroy destroys every mutex it created before the block holding it is
//     freed.
//
// The transport is the in-memory loopback (and candidates the budget
// refuses, which are never opened): no socket, no thread, no sleep, and the
// clock is 1000.0. The environment is set and restored within each case
// through tests/test_env_source.h.

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
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// a failed check names the fault before the check reports its line

#define gb_expect( condition, ... )                                                                                   \
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
#define GB_NOINLINE __declspec( noinline )
#else
#define GB_NOINLINE __attribute__( ( noinline ) )
#endif // #if defined( _MSC_VER )

// ---------------------------------------------------------------------------
// The heap: counts every allocation and free, the bytes asked for, the
// blocks live, and fails the fail_at-th call. Every live block is in a table,
// so a free of a block not live (a double free) is counted rather than
// undefined. A block freed while it still holds a created platform mutex
// (one the fake mutex create made, never destroyed) is counted too.
// ---------------------------------------------------------------------------

#define GB_MAX_BLOCKS 4096
#define GB_MAX_MUTEXES 64

struct gb_block_t
{
    void * base;
    void * user;
    size_t bytes;
};

struct gb_heap_t
{
    int calls;
    int frees;
    int live;
    uint64_t bytes;
    int fail_at;
    int bad_frees;
    int undestroyed_mutexes;
    gb_block_t blocks[GB_MAX_BLOCKS];
    int num_blocks;
};

static gb_heap_t gb_heap;

static void * gb_allocate( void *, size_t bytes )
{
    const int call = ++gb_heap.calls;
    if ( gb_heap.fail_at == call )
    {
        return NULL;
    }
    if ( gb_heap.num_blocks >= GB_MAX_BLOCKS )
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
    gb_heap.blocks[gb_heap.num_blocks].base = base;
    gb_heap.blocks[gb_heap.num_blocks].user = (void *) address;
    gb_heap.blocks[gb_heap.num_blocks].bytes = bytes;
    gb_heap.num_blocks++;
    gb_heap.live++;
    gb_heap.bytes += bytes;

    return (void *) address;
}

// the fake mutex create's record: every mutex it created, until destroyed

static rocketnet_platform_mutex_t * gb_mutexes[GB_MAX_MUTEXES];
static int gb_num_mutexes = 0;

static void gb_release( void *, void * pointer )
{
    if ( !pointer )
    {
        return;
    }

    for ( int i = 0; i < gb_heap.num_blocks; i++ )
    {
        if ( gb_heap.blocks[i].user != pointer )
        {
            continue;
        }

        // a mutex created in this block and still marked created was never destroyed

        const uint8_t * first = (const uint8_t *) pointer;
        const uint8_t * last = first + gb_heap.blocks[i].bytes;
        for ( int m = 0; m < gb_num_mutexes; m++ )
        {
            const uint8_t * at = (const uint8_t *) gb_mutexes[m];
            if ( at >= first && at < last )
            {
                if ( gb_mutexes[m]->ok )
                {
                    gb_heap.undestroyed_mutexes++;
                }
                gb_mutexes[m] = gb_mutexes[--gb_num_mutexes];
                m--;
            }
        }

        free( gb_heap.blocks[i].base );
        gb_heap.blocks[i] = gb_heap.blocks[--gb_heap.num_blocks];
        gb_heap.live--;
        gb_heap.frees++;
        return;
    }

    gb_heap.bad_frees++;
}

static void gb_begin()
{
    memset( &gb_heap, 0, sizeof( gb_heap ) );
    gb_num_mutexes = 0;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );

    int status = -1;
    check( rocketnet_init_internal( &gb_heap, gb_allocate, gb_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_test_loopback_hub_context( &gb_heap );
}

static void gb_end()
{
    gb_heap.fail_at = 0;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    gb_expect( gb_heap.live == 0, "%d blocks still live after term", gb_heap.live );
    gb_expect( gb_heap.bad_frees == 0, "%d frees of a block that was not live (a double free)", gb_heap.bad_frees );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// The environment the cases change, held aside and put back
// ---------------------------------------------------------------------------

struct gb_env_saved_t
{
    bool was_set;
    char value[256];
};

static const char * gb_env_names[] = { "ROCKETNET_SERVER_TRANSPORT", "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "ROCKETNET_SERVER_MAX_CLIENTS" };
#define GB_ENV_NAMES ( (int) ( sizeof( gb_env_names ) / sizeof( gb_env_names[0] ) ) )

static void gb_env_set_aside( gb_env_saved_t * saved )
{
    for ( int i = 0; i < GB_ENV_NAMES; i++ )
    {
        saved[i].was_set = rocketnet_test_env_getenv( gb_env_names[i], saved[i].value, sizeof( saved[i].value ) ) != 0;
        check( rocketnet_test_env_unsetenv( gb_env_names[i] ) == 0 );
    }
}

static void gb_env_put_back( const gb_env_saved_t * saved )
{
    for ( int i = 0; i < GB_ENV_NAMES; i++ )
    {
        if ( saved[i].was_set )
        {
            check( rocketnet_test_env_setenv( gb_env_names[i], saved[i].value, 1 ) == 0 );
        }
        else
        {
            check( rocketnet_test_env_unsetenv( gb_env_names[i] ) == 0 );
        }
    }
}

// ---------------------------------------------------------------------------
// The log: every ERROR line, as the public log callback hands it over
// ---------------------------------------------------------------------------

#define GB_MAX_LINES 16

static char gb_lines[GB_MAX_LINES][1024];
static int gb_num_lines = 0;

static void gb_log_capture( void *, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    if ( level != ROCKETNET_LOG_LEVEL_ERROR || gb_num_lines >= GB_MAX_LINES )
    {
        return;
    }
    const size_t bytes = length < sizeof( gb_lines[0] ) - 1 ? length : sizeof( gb_lines[0] ) - 1;
    memcpy( gb_lines[gb_num_lines], message, bytes );
    gb_lines[gb_num_lines][bytes] = '\0';
    gb_num_lines++;
}

struct gb_log_scope_t
{
    enum rocketnet_log_level_t saved;

    gb_log_scope_t()
    {
        saved = rocketnet_log_level_current();
        gb_num_lines = 0;
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
        rocketnet_log_function( gb_log_capture, NULL );
    }

    ~gb_log_scope_t()
    {
        rocketnet_log_function( NULL, NULL );
        rocketnet_log_level( saved );
    }
};

// the captured line that starts with prefix, or NULL

static const char * gb_line_starting( const char * prefix )
{
    for ( int i = 0; i < gb_num_lines; i++ )
    {
        if ( strncmp( gb_lines[i], prefix, strlen( prefix ) ) == 0 )
        {
            return gb_lines[i];
        }
    }

    return NULL;
}

static void gb_print_lines()
{
    for ( int i = 0; i < gb_num_lines; i++ )
    {
        printf( "  logged: %s\n", gb_lines[i] );
    }
}

// fills the stack the create's frames will take with nonzero bytes and no
// NUL, so a name list or flag array read before it is written reads garbage,
// not the zeros a fresh stack happens to hold

GB_NOINLINE static void gb_dirty_stack()
{
    volatile uint8_t junk[256 * 1024];
    for ( size_t i = 0; i < sizeof( junk ); i++ )
    {
        junk[i] = 0x5A;
    }
}

// ---------------------------------------------------------------------------
// Configs
// ---------------------------------------------------------------------------

// non-development keys and filter, so the production library takes the
// config as the development library does

static void gb_server_config( struct rocketnet_server_transport_config_t * config, const char * transport, int max_clients )
{
    rocketnet_server_transport_default_config_current( config );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    config->protocol_id = 1;
    config->max_clients = max_clients;
    snprintf( config->transport, sizeof( config->transport ), "%s", transport );
    config->loopback_hub = rocketnet_test_loopback_hub();
}

// a built-in transport this platform does not have: its name is skipped
// for platform, never opened

static const char * gb_unavailable_transport()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return "msgx";
#else
    return "iocp";
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

// a transport with a plan larger than portable's, or NULL: the pin a budget
// refuses while portable, a candidate the pin does not name, would fit

static const char * gb_transport_over_portable( const struct rocketnet_server_transport_config_t * config )
{
    static const char * names[] = { "gso", "msgx", "iocp", "rio", "io_uring", "xdp" };
    const uint64_t portable = rocketnet_server_transport_required_memory( config, "portable" );
    if ( portable == 0 )
    {
        return NULL;
    }
    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
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
        if ( here && rocketnet_server_transport_required_memory( config, names[i] ) > portable )
        {
            return names[i];
        }
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

// mutation gap: hunt 2/4782 (a 1-client server has no plan), hunt 2/4843
// (a config's plan is the defaults'), hunt 2/4851 (the ladder's allocator
// bytes plan a candidate past the list), hunt 2/4828 (xdp's UMEM counted twice)

static void test_gaps_budget_server_plan_is_what_create_allocates()
{
    gb_env_saved_t saved[GB_ENV_NAMES];
    gb_env_set_aside( saved );
    gb_begin();

    const int populations[] = { 1, 4 };
    for ( int p = 0; p < 2; p++ )
    {
        const int clients = populations[p];
        struct rocketnet_server_transport_config_t config;
        gb_server_config( &config, "loopback", clients );

        const uint64_t required = rocketnet_server_transport_required_memory( &config, "loopback" );
        gb_expect( required > 0, "a %d-client loopback server has no memory plan (required memory 0)", clients );

        const int hub_live = gb_heap.live;
        const uint64_t bytes_before = gb_heap.bytes;
        const int calls_before = gb_heap.calls;
        int status = -1;
        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, &status );
        check( server != NULL );
        check( status == ROCKETNET_STATUS_OK );
        const uint64_t allocated = gb_heap.bytes - bytes_before;
        gb_expect( required == allocated, "%d clients: the plan says %llu bytes, create allocated %llu in %d calls",
                   clients, (unsigned long long) required, (unsigned long long) allocated, gb_heap.calls - calls_before );

        rn_test_server_transport_stop_and_destroy( server );
        gb_expect( gb_heap.live == hub_live, "%d blocks leaked by a create and destroy", gb_heap.live - hub_live );

        // the parts: allocator bytes plus UMEM (none here) is the total, and
        // the ladder of one candidate plans that candidate alone

        rocketnet_server_memory_plan_t plan;
        check( rocketnet_server_transport_memory_plan( &config, "loopback", plan ) );
        gb_expect( plan.total_bytes == plan.allocator_bytes + plan.umem_bytes, "%d clients: the plan total %llu is not its allocator %llu plus UMEM %llu bytes",
                   clients, (unsigned long long) plan.total_bytes, (unsigned long long) plan.allocator_bytes, (unsigned long long) plan.umem_bytes );
        const uint64_t ladder = rocketnet_server_transport_ladder_allocator_bytes( &config );
        gb_expect( ladder == plan.allocator_bytes, "%d clients: the ladder \"loopback\" plans %llu allocator bytes, its one candidate %llu",
                   clients, (unsigned long long) ladder, (unsigned long long) plan.allocator_bytes );
    }

    // the plan is the config's, not the defaults': the defaults' population
    // is not one client, so their plans differ

    struct rocketnet_server_transport_config_t defaults;
    rocketnet_server_transport_default_config( &defaults );
    check( defaults.max_clients != 1 );
    struct rocketnet_server_transport_config_t one;
    gb_server_config( &one, "loopback", 1 );
    const uint64_t one_plan = rocketnet_server_transport_required_memory( &one, "loopback" );
    const uint64_t default_plan = rocketnet_server_transport_required_memory( NULL, "loopback" );
    gb_expect( default_plan == rocketnet_server_transport_required_memory( &defaults, "loopback" ), "required memory of a NULL config is not the defaults' plan" );
    gb_expect( one_plan != default_plan, "required memory for a 1-client config is the default config's (%llu bytes)", (unsigned long long) default_plan );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_XDP ) && ROCKETNET_XDP
    // the xdp plan's UMEM is page-aligned memory outside the allocator,
    // counted once in the total

    struct rocketnet_server_transport_config_t xdp;
    gb_server_config( &xdp, "xdp", 4 );
    rocketnet_server_memory_plan_t xdp_plan;
    check( rocketnet_server_transport_memory_plan( &xdp, "xdp", xdp_plan ) );
    gb_expect( xdp_plan.umem_bytes == rocketnet_xdp_umem_bytes( ROCKETNET_XDP_DEFAULT_QUEUES ), "the xdp plan's UMEM is %llu bytes, not %llu",
               (unsigned long long) xdp_plan.umem_bytes, (unsigned long long) rocketnet_xdp_umem_bytes( ROCKETNET_XDP_DEFAULT_QUEUES ) );
    gb_expect( xdp_plan.total_bytes == xdp_plan.allocator_bytes + xdp_plan.umem_bytes, "the xdp plan total %llu is not its allocator %llu plus UMEM %llu bytes (UMEM counted twice?)",
               (unsigned long long) xdp_plan.total_bytes, (unsigned long long) xdp_plan.allocator_bytes, (unsigned long long) xdp_plan.umem_bytes );
    gb_expect( rocketnet_server_transport_required_memory( &xdp, "xdp" ) == xdp_plan.total_bytes, "xdp required memory is not the plan total" );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_XDP ) && ROCKETNET_XDP

    gb_end();
    gb_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_server_plan_is_what_create_allocates );

// ---------------------------------------------------------------------------
// The budget
// ---------------------------------------------------------------------------

#define GB_BUDGET_EXHAUSTED_PREFIX "server transport ladder exhausted: every candidate's memory plan exceeds the "

// a create under budget that the budget refuses: NULL, the status, the
// error naming the budget, and not one allocation

static void gb_expect_budget_refusal( const struct rocketnet_server_transport_config_t * config, uint64_t budget, const char * what )
{
    const int calls_before = gb_heap.calls;
    const int live_before = gb_heap.live;

    gb_log_scope_t log;
    int status = ROCKETNET_STATUS_OK;
    gb_dirty_stack();
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_resolved( &gb_heap, "127.0.0.1:0", config, budget, &status );
    gb_expect( server == NULL, "%s: a budget of %llu bytes did not refuse the create (granted %s)", what, (unsigned long long) budget,
               server ? rocketnet_server_transport_io_name( server ) : "-" );
    rn_test_server_transport_stop_and_destroy( server );
    gb_expect( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "%s: the budget refusal's status is %s, not TRANSPORT_LADDER_EXHAUSTED", what,
               rocketnet_status_name( (enum rocketnet_status_t) status ) );
    gb_expect( gb_heap.calls == calls_before, "%s: the budget refusal came after %d allocations, not before the first", what, gb_heap.calls - calls_before );
    gb_expect( gb_heap.live == live_before, "%s: %d blocks leaked", what, gb_heap.live - live_before );

    char expected[256];
    snprintf( expected, sizeof( expected ), GB_BUDGET_EXHAUSTED_PREFIX "%llu byte operator budget", (unsigned long long) budget );
    const char * line = gb_line_starting( GB_BUDGET_EXHAUSTED_PREFIX );
    if ( !line || strcmp( line, expected ) != 0 )
    {
        gb_print_lines();
    }
    gb_expect( line != NULL && strcmp( line, expected ) == 0, "%s: the budget refusal did not log \"%s\"", what, expected );

    // a NULL status is taken: the same refusal, nothing written through it

    gb_dirty_stack();
    server = rocketnet_server_transport_create_resolved( &gb_heap, "127.0.0.1:0", config, budget, NULL );
    gb_expect( server == NULL, "%s: with a NULL status a budget of %llu bytes did not refuse the create", what, (unsigned long long) budget );
    rn_test_server_transport_stop_and_destroy( server );
    gb_expect( gb_heap.calls == calls_before, "%s: with a NULL status the budget refusal came after %d allocations", what, gb_heap.calls - calls_before );
}

// mutation gap: hunt 2/4991, 4993, 4994, 4997, 4998, 5001, 5008, 5009, 5012,
// 5013 (the budget's refusal before allocation, its status, its boundaries),
// hunt 2/5000, 5002 (a budget that refuses one rung opens the next), hunt
// 2/4992 (a pin over budget refuses although portable, unlisted, fits)

static void test_gaps_budget_server_budget_refuses_before_allocation()
{
    gb_env_saved_t saved[GB_ENV_NAMES];
    gb_env_set_aside( saved );
    gb_begin();

    struct rocketnet_server_transport_config_t config;
    gb_server_config( &config, "loopback", 4 );
    const uint64_t plan = rocketnet_server_transport_required_memory( &config, "loopback" );
    check( plan > 1 );

    // a budget exactly the plan opens; no budget (0) opens

    const uint64_t open_budgets[] = { plan, plan + 1, 0 };
    for ( int i = 0; i < 3; i++ )
    {
        int status = -1;
        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_resolved( &gb_heap, "127.0.0.1:0", &config, open_budgets[i], &status );
        gb_expect( server != NULL, "a budget of %llu bytes refused a plan of %llu bytes (status %s)", (unsigned long long) open_budgets[i],
                   (unsigned long long) plan, rocketnet_status_name( (enum rocketnet_status_t) status ) );
        gb_expect( status == ROCKETNET_STATUS_OK, "a budget of %llu bytes opened with status %s", (unsigned long long) open_budgets[i],
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        rn_test_server_transport_stop_and_destroy( server );
    }

    // the plan less one byte, and one byte: refused before the first allocation

    gb_expect_budget_refusal( &config, plan - 1, "pinned loopback, budget the plan less one byte" );
    gb_expect_budget_refusal( &config, 1, "pinned loopback, a 1-byte budget" );

    // two rungs, the first over budget: the second opens

    const uint64_t portable = rocketnet_server_transport_required_memory( &config, "portable" );
    if ( portable > plan )
    {
        struct rocketnet_server_transport_config_t two;
        gb_server_config( &two, "portable, loopback", 4 );

        gb_log_scope_t log;
        int status = -1;
        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_resolved( &gb_heap, "127.0.0.1:0", &two, plan, &status );
        gb_expect( server != NULL, "\"portable, loopback\" under a budget only loopback fits refused the whole create (status %s)",
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        const char * granted = server ? rocketnet_server_transport_io_name( server ) : "-";
        gb_expect( server != NULL && strcmp( granted, "loopback" ) == 0, "\"portable, loopback\" under a budget only loopback fits granted %s, not loopback", granted );
        rn_test_server_transport_stop_and_destroy( server );

        // the pin over budget, a candidate the pin does not name fitting

        const char * over = gb_transport_over_portable( &config );
        if ( over )
        {
            struct rocketnet_server_transport_config_t pin;
            gb_server_config( &pin, over, 4 );
            char what[128];
            snprintf( what, sizeof( what ), "pinned %s over a budget portable fits", over );
            gb_expect_budget_refusal( &pin, portable, what );
        }
    }

    gb_end();
    gb_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_server_budget_refuses_before_allocation );

// mutation gap: hunt 2/4986, 4987 (an unparsable budget variable: the status
// written, a NULL status taken)

static void test_gaps_budget_server_budget_env_invalid()
{
    gb_env_saved_t saved[GB_ENV_NAMES];
    gb_env_set_aside( saved );
    gb_begin();

    struct rocketnet_server_transport_config_t config;
    gb_server_config( &config, "loopback", 4 );
    const uint64_t plan = rocketnet_server_transport_required_memory( &config, "loopback" );

    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "not-a-number", 1 ) == 0 );

    const int calls_before = gb_heap.calls;
    int status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, &status );
    gb_expect( server == NULL, "an unparsable ROCKETNET_SERVER_MEMORY_BUDGET_BYTES did not refuse the create" );
    rn_test_server_transport_stop_and_destroy( server );
    gb_expect( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID, "an unparsable ROCKETNET_SERVER_MEMORY_BUDGET_BYTES gave status %s, not CONFIG_ENV_INVALID",
               rocketnet_status_name( (enum rocketnet_status_t) status ) );
    gb_expect( gb_heap.calls == calls_before, "an unparsable budget variable was refused after %d allocations", gb_heap.calls - calls_before );

    server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, NULL );
    gb_expect( server == NULL, "with a NULL status an unparsable budget variable did not refuse the create" );
    rn_test_server_transport_stop_and_destroy( server );

    // the variable parsed: the plan less one byte refuses, the plan opens

    char budget[64];
    snprintf( budget, sizeof( budget ), "%llu", (unsigned long long) ( plan - 1 ) );
    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", budget, 1 ) == 0 );
    status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, &status );
    gb_expect( server == NULL && status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES=%s did not refuse a plan of %llu bytes", budget,
               (unsigned long long) plan );
    rn_test_server_transport_stop_and_destroy( server );

    snprintf( budget, sizeof( budget ), "%llu", (unsigned long long) plan );
    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", budget, 1 ) == 0 );
    server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, &status );
    gb_expect( server != NULL, "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES=%s refused a plan of exactly that", budget );
    rn_test_server_transport_stop_and_destroy( server );

    gb_end();
    gb_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_server_budget_env_invalid );

// ---------------------------------------------------------------------------
// The exhausted ladder's lines
// ---------------------------------------------------------------------------

#define GB_EXHAUSTED_PREFIX "server transport ladder exhausted with no grant. "
#define GB_PORT_ZERO_PREFIX "the public address carries port zero"

// mutation gap: hunt 2/5029, 5030 (refused names read before set), 5032
// (refused flags read before set), 5035, 5036 (port-zero names read before
// set), 5087, 5088 (skipped names read before set), 5092 (the port-zero test
// reads past the name set)

static void test_gaps_budget_server_exhausted_ladder_names_exactly()
{
    gb_env_saved_t saved[GB_ENV_NAMES];
    gb_env_set_aside( saved );
    gb_begin();

    // the hub holds one server: a second loopback server is refused at its
    // open, a refusal that is not for want of memory, so the ladder exhausts

    struct rocketnet_server_transport_config_t first_config;
    gb_server_config( &first_config, "loopback", 4 );
    struct rocketnet_server_transport_t * first = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &first_config, 1000.0, NULL );
    check( first != NULL );

    const uint64_t plan = rocketnet_server_transport_required_memory( &first_config, "loopback" );
    const bool portable_refusable = rocketnet_server_transport_required_memory( &first_config, "portable" ) > plan;

    // a skipped name, a budget-refused rung (when portable has a plan above
    // loopback's) and a refused open

    char list[ROCKETNET_TRANSPORT_STRING_LENGTH];
    snprintf( list, sizeof( list ), "%s, %s", gb_unavailable_transport(), portable_refusable ? "portable, loopback" : "loopback" );

    char expected[256];
    snprintf( expected, sizeof( expected ), GB_EXHAUSTED_PREFIX "refused: %s. skipped for platform: %s", portable_refusable ? "portable, loopback" : "loopback",
              gb_unavailable_transport() );

    struct rocketnet_server_transport_config_t config;
    gb_server_config( &config, list, 4 );

    // twice: the second create runs on the stack the first left, so a list
    // or flag array read before it is set holds the first create's names

    const int live_before = gb_heap.live;
    for ( int attempt = 1; attempt <= 2; attempt++ )
    {
        gb_log_scope_t log;
        int status = ROCKETNET_STATUS_OK;
        if ( attempt == 1 )
        {
            gb_dirty_stack();
        }
        struct rocketnet_server_transport_t * second = rocketnet_server_transport_create_resolved( &gb_heap, "127.0.0.1:0", &config, portable_refusable ? plan : 0, &status );
        gb_expect( second == NULL, "create %d: a second server on the hub was granted %s", attempt, second ? rocketnet_server_transport_io_name( second ) : "-" );
        rn_test_server_transport_stop_and_destroy( second );

        const char * line = gb_line_starting( GB_EXHAUSTED_PREFIX );
        if ( !line || strcmp( line, expected ) != 0 || gb_line_starting( GB_PORT_ZERO_PREFIX ) )
        {
            gb_print_lines();
        }
        gb_expect( line != NULL && strcmp( line, expected ) == 0, "create %d: the exhausted ladder logged \"%s\", not \"%s\"", attempt, line ? line : "(nothing)", expected );
        gb_expect( gb_line_starting( GB_PORT_ZERO_PREFIX ) == NULL, "create %d: no rung refused for port zero, yet the port-zero error was logged", attempt );
        gb_expect( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "create %d: the exhausted ladder's status is %s, not TRANSPORT_LADDER_EXHAUSTED", attempt,
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        gb_expect( gb_heap.live == live_before, "create %d: the refused create leaked %d blocks", attempt, gb_heap.live - live_before );
    }

    // nothing skipped, nothing budget-refused: "(none)" for the skipped list

    struct rocketnet_server_transport_config_t pin;
    gb_server_config( &pin, "loopback", 4 );
    {
        gb_log_scope_t log;
        int status = ROCKETNET_STATUS_OK;
        gb_dirty_stack();
        struct rocketnet_server_transport_t * second = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &pin, 1000.0, &status );
        check( second == NULL );
        const char * line = gb_line_starting( GB_EXHAUSTED_PREFIX );
        const char * want = GB_EXHAUSTED_PREFIX "refused: loopback. skipped for platform: (none)";
        if ( !line || strcmp( line, want ) != 0 || gb_line_starting( GB_PORT_ZERO_PREFIX ) )
        {
            gb_print_lines();
        }
        gb_expect( line != NULL && strcmp( line, want ) == 0, "the exhausted pin logged \"%s\", not \"%s\"", line ? line : "(nothing)", want );
        gb_expect( gb_line_starting( GB_PORT_ZERO_PREFIX ) == NULL, "a pinned loopback refusal logged the port-zero error" );
        gb_expect( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "the exhausted pin's status is %s, not TRANSPORT_LADDER_EXHAUSTED",
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
    }

    rn_test_server_transport_stop_and_destroy( first );

    gb_end();
    gb_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_server_exhausted_ladder_names_exactly );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_XDP ) && ROCKETNET_XDP

// mutation gap: hunt 2/5038 (the port-zero flags read before set): xdp binds
// the port it is given and discovers none, so a port-zero pin on it is
// refused before it opens: the status says so, and the error names it. The
// create runs twice, the second on the stack the first left: a name list or
// a flag array read before it is set holds the first create's values there
// (xdp already seen), whatever the build's frame layout.

static void test_gaps_budget_server_port_zero_pin_names_the_rung()
{
    gb_env_saved_t saved[GB_ENV_NAMES];
    gb_env_set_aside( saved );
    gb_begin();

    struct rocketnet_server_transport_config_t config;
    gb_server_config( &config, "xdp", 4 );

    const int live_before = gb_heap.live;
    for ( int attempt = 1; attempt <= 2; attempt++ )
    {
        gb_log_scope_t log;
        int status = ROCKETNET_STATUS_OK;
        if ( attempt == 1 )
        {
            gb_dirty_stack();
        }
        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, &status );
        gb_expect( server == NULL, "create %d: a port-zero xdp pin was granted", attempt );
        rn_test_server_transport_stop_and_destroy( server );
        gb_expect( status == ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED, "create %d: a port-zero xdp pin's status is %s, not TRANSPORT_PORT_ZERO_UNSUPPORTED", attempt,
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );

        const char * exhausted = gb_line_starting( GB_EXHAUSTED_PREFIX );
        const char * want = GB_EXHAUSTED_PREFIX "refused: xdp. skipped for platform: (none)";
        const char * port_zero = gb_line_starting( GB_PORT_ZERO_PREFIX );
        const char * names = GB_PORT_ZERO_PREFIX ", which asks the kernel for an ephemeral port: xdp cannot discover";
        if ( !exhausted || strcmp( exhausted, want ) != 0 || !port_zero || strncmp( port_zero, names, strlen( names ) ) != 0 )
        {
            gb_print_lines();
        }
        gb_expect( exhausted != NULL && strcmp( exhausted, want ) == 0, "create %d: the port-zero pin logged \"%s\", not \"%s\"", attempt, exhausted ? exhausted : "(nothing)",
                   want );
        gb_expect( port_zero != NULL && strncmp( port_zero, names, strlen( names ) ) == 0, "create %d: the port-zero error does not name xdp: \"%s\"", attempt,
                   port_zero ? port_zero : "(nothing)" );
        gb_expect( gb_heap.live == live_before, "create %d: the refused create leaked %d blocks", attempt, gb_heap.live - live_before );
    }

    gb_end();
    gb_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_server_port_zero_pin_names_the_rung );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_XDP ) && ROCKETNET_XDP

// ---------------------------------------------------------------------------
// Refused creates free what they allocated
// ---------------------------------------------------------------------------

#if ROCKETNET_DEV

// the fake mutex create: every mutex created is recorded, and the
// refuse_at-th create is refused

static int gb_mutex_creates = 0;
static int gb_mutex_refuse_at = 0;
static const struct rocketnet_platform_priority_calls_t * gb_real_calls = NULL;

static int gb_mutex_init( struct rocketnet_platform_mutex_t * mutex )
{
    const int call = ++gb_mutex_creates;
    if ( call == gb_mutex_refuse_at )
    {
        memset( mutex, 0, sizeof( *mutex ) );
        return 1;
    }

    const int result = gb_real_calls->mutex_init( mutex );
    if ( result == 0 && gb_num_mutexes < GB_MAX_MUTEXES )
    {
        gb_mutexes[gb_num_mutexes++] = mutex;
    }

    return result;
}

struct gb_mutex_scope_t
{
    struct rocketnet_platform_priority_calls_t fake;

    gb_mutex_scope_t()
    {
        gb_real_calls = rocketnet_platform_priority_calls_current();
        fake = *gb_real_calls;
        fake.mutex_init = gb_mutex_init;
        gb_mutex_creates = 0;
        gb_mutex_refuse_at = 0;
        rocketnet_platform_set_priority_calls_for_testing( &fake );
    }

    ~gb_mutex_scope_t()
    {
        rocketnet_platform_set_priority_calls_for_testing( gb_real_calls );
    }
};

// mutation gap: hunt 2/5192 (the simulator patterns' allocation failed: the
// transport, its I/O and its simulator freed); the walk fails each allocation
// of a development create in turn, the reverse window making the patterns
// allocate

static void test_gaps_budget_server_out_of_memory_frees_everything()
{
    gb_env_saved_t saved[GB_ENV_NAMES];
    gb_env_set_aside( saved );
    gb_begin();

    struct rocketnet_server_transport_config_t config;
    gb_server_config( &config, "loopback", 4 );
    config.simulated_reverse_window = 2;

    const int hub_live = gb_heap.live;

    // the allocations of the whole create, counted on a create that succeeds

    int calls_before = gb_heap.calls;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, NULL );
    check( server != NULL );
    const int allocations = gb_heap.calls - calls_before;
    rn_test_server_transport_stop_and_destroy( server );
    check( gb_heap.live == hub_live );

    for ( int k = 1; k <= allocations; k++ )
    {
        const int frees_before = gb_heap.frees;
        calls_before = gb_heap.calls;
        gb_heap.fail_at = calls_before + k;
        int status = -1;
        server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, &status );
        gb_heap.fail_at = 0;
        gb_expect( server == NULL, "allocation %d of %d failed, yet the create succeeded", k, allocations );
        rn_test_server_transport_stop_and_destroy( server );
        gb_expect( status == ROCKETNET_STATUS_OUT_OF_MEMORY, "allocation %d of %d failed: status %s, not OUT_OF_MEMORY", k, allocations,
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        gb_expect( gb_heap.live == hub_live, "allocation %d of %d failed: %d blocks leaked (%d allocated, %d freed)", k, allocations, gb_heap.live - hub_live,
                   gb_heap.calls - calls_before - 1, gb_heap.frees - frees_before );
        gb_expect( gb_heap.bad_frees == 0, "allocation %d of %d failed: a block was freed twice", k, allocations );
    }

    gb_end();
    gb_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_server_out_of_memory_frees_everything );

// mutation gap: hunt 2/5187, 5188, 5189 (the link simulator's mutex refused:
// the simulator freed once, the transport freed, no mutex destroyed that was
// never created), hunt 2/5330 (destroy destroys the simulator's mutex before
// the block holding it is freed)

static void test_gaps_budget_server_mutex_refused_frees_everything()
{
    gb_env_saved_t saved[GB_ENV_NAMES];
    gb_env_set_aside( saved );
    gb_begin();

    struct rocketnet_server_transport_config_t config;
    gb_server_config( &config, "loopback", 4 );
    const int hub_live = gb_heap.live;

    gb_mutex_scope_t mutexes;

    // the mutexes of the whole create, counted on a create that succeeds,
    // and destroyed, every one, before their blocks are freed

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, NULL );
    check( server != NULL );
    const int creates = gb_mutex_creates;
    check( creates > 0 );
    rn_test_server_transport_stop_and_destroy( server );
    gb_expect( gb_heap.undestroyed_mutexes == 0, "destroy freed %d created mutexes without destroying them", gb_heap.undestroyed_mutexes );
    check( gb_heap.live == hub_live );

    for ( int k = 1; k <= creates; k++ )
    {
        gb_mutex_creates = 0;
        gb_mutex_refuse_at = k;
        const int calls_before = gb_heap.calls;
        const int frees_before = gb_heap.frees;
        int status = -1;
        server = rocketnet_server_transport_create_current_internal( &gb_heap, "127.0.0.1:0", &config, 1000.0, &status );
        gb_mutex_refuse_at = 0;
        gb_expect( server == NULL, "mutex create %d of %d refused, yet the create succeeded", k, creates );
        rn_test_server_transport_stop_and_destroy( server );
        gb_expect( status == ROCKETNET_STATUS_MUTEX_CREATE_FAILED, "mutex create %d of %d refused: status %s, not MUTEX_CREATE_FAILED", k, creates,
                   rocketnet_status_name( (enum rocketnet_status_t) status ) );
        gb_expect( gb_heap.bad_frees == 0, "mutex create %d of %d refused: %d frees of a block not live (the simulator freed twice?)", k, creates, gb_heap.bad_frees );
        gb_expect( gb_heap.live == hub_live, "mutex create %d of %d refused: %d blocks leaked (%d allocated, %d freed)", k, creates, gb_heap.live - hub_live,
                   gb_heap.calls - calls_before, gb_heap.frees - frees_before );
        gb_expect( gb_heap.undestroyed_mutexes == 0, "mutex create %d of %d refused: %d created mutexes freed undestroyed", k, creates, gb_heap.undestroyed_mutexes );
    }

    gb_end();
    gb_env_put_back( saved );
}

REGISTER_UNIT_TEST( test_gaps_budget_server_mutex_refused_frees_everything );

#if !defined( ROCKETNET_CONFORMANCE_DEFAULTS_ONLY )
extern int rocketnet_server_transport_test_staged_simulator_flush();

// mutation gap: hunt 2/6290 (the staged-flush fixture destroys the mutex it
// created before freeing the transport that holds it)

static void test_gaps_budget_server_staged_fixture_destroys_its_mutex()
{
    gb_begin();
    gb_mutex_scope_t mutexes;

    const int live_before = gb_heap.live;
    check( rocketnet_server_transport_test_staged_simulator_flush() == 1 );
    gb_expect( gb_mutex_creates > 0, "the staged-flush fixture created no mutex" );
    gb_expect( gb_heap.undestroyed_mutexes == 0, "the staged-flush fixture freed %d created mutexes without destroying them", gb_heap.undestroyed_mutexes );
    gb_expect( gb_heap.live == live_before, "the staged-flush fixture leaked %d blocks", gb_heap.live - live_before );

    gb_end();
}

REGISTER_UNIT_TEST( test_gaps_budget_server_staged_fixture_destroys_its_mutex );
#endif // #if !defined( ROCKETNET_CONFORMANCE_DEFAULTS_ONLY )

#endif // #if ROCKETNET_DEV
