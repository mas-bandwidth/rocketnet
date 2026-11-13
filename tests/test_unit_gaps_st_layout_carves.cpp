/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server transport's one allocation (sdk/rocketnet_server_transport.cpp,
// rocketnet_server_transport_layout and _allocate): the fixed part, then
// every carved array behind it. A carve written over another, or over the
// fixed part, stays inside the same block, where no guard allocator or
// sanitizer sees it, so these cases check the carves by address and size,
// plant guard bytes right after a carve that a flush fills to exactly its
// capacity, and create transports through an allocator that fills fresh
// memory with 0x5A so a carve read before it is written shows up.
//
// The transports run on the in-memory loopback transport (no socket, no
// thread), slots are seated by the unit seam, send_batch is swapped for a
// recorder, and the clock is an argument. The host memory estimate case runs
// in every build; the rest need ROCKETNET_DEV.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_constants.h"
#include "rocketnet_layers.h"
#include "rocketnet_memory.h"
#include "rocketnet_driver.h"
#include "rocketnet_platform.h"
#include "rocketnet_server_protocol.h"
#include "unit_shutdown.h"
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#if defined( _MSC_VER )
#define GAPS_ST_LAYOUT_NOINLINE __declspec( noinline )
#else // #if defined( _MSC_VER )
#define GAPS_ST_LAYOUT_NOINLINE __attribute__( ( noinline ) )
#endif // #if defined( _MSC_VER )

// prints the fault before the check that stops the case, so the red line
// names what went wrong

static bool gaps_st_layout_holds( bool holds, const char * fault )
{
    if ( !holds )
    {
        printf( "fault: %s\n", fault );
        fflush( stdout );
    }
    return holds;
}

// ---------------------------------------------------------------------------
// the host memory estimate on a dirty stack (every build)
// ---------------------------------------------------------------------------

// fills the stack below the caller with 0x5A, where the estimate's own
// locals are about to live

static GAPS_ST_LAYOUT_NOINLINE void gaps_st_layout_dirty_stack()
{
    volatile uint8_t junk[65536];
    for ( size_t i = 0; i < sizeof( junk ); i++ )
    {
        junk[i] = 0x5A;
    }
}

static GAPS_ST_LAYOUT_NOINLINE bool gaps_st_layout_estimate( const char * transport, struct rocketnet_host_memory_estimate_t * estimate )
{
    return rocketnet_server_transport_host_memory_estimate( NULL, transport, estimate );
}

// mutation gap: hunt 2/4866 (the estimate's memset deleted), hunt 2/4909
// (the exclusions copy one past the count). Every field the estimate does not
// set on the portable path is zero, the total is the sum of its parts, and
// every exclusion past the count is NULL, whatever the stack held before.
// The dirty stack reaches the estimate itself (4866); the local exclusion
// list it copies from sits where its own callees ran first, so 4909 is caught
// by the MSan leg (tools/run_msan.sh), which reads the copied slot as
// uninitialized in the NULL check below.

static void test_gaps_st_layout_host_memory_estimate_on_a_dirty_stack()
{
    struct rocketnet_host_memory_estimate_t e;
    memset( &e, 0xA5, sizeof( e ) );

    gaps_st_layout_dirty_stack();
    check( gaps_st_layout_holds( gaps_st_layout_estimate( "portable", &e ), "portable estimate refused: a byte count accumulated onto stack garbage overflowed" ) );

    uint64_t sum = 0;
    for ( uint64_t bytes : { e.allocator_bytes, e.umem_bytes, e.registered_region_bytes, e.socket_send_buffer_bytes,
                             e.socket_receive_buffer_bytes, e.xdp_ring_bytes, e.thread_stack_bytes } )
    {
        sum += bytes;
    }

    check( gaps_st_layout_holds( e.xdp_ring_bytes == 0, "portable estimate: xdp_ring_bytes holds stack garbage, not 0" ) );
    check( gaps_st_layout_holds( e.umem_bytes == 0 && e.registered_region_bytes == 0, "portable estimate: umem or registered bytes not 0" ) );
    check( gaps_st_layout_holds( e.socket_send_buffer_bytes == (uint64_t) e.sockets * ROCKETNET_SOCKET_SEND_BUFFER_SIZE,
                                 "portable estimate: socket send bytes are not sockets * the send buffer (accumulated onto garbage)" ) );
    if ( e.worker_threads == 0 )
    {
        check( gaps_st_layout_holds( e.thread_stack_bytes == 0 && e.thread_stack_bytes_per_thread == 0,
                                     "portable estimate: thread stack bytes not 0 with no worker thread" ) );
    }
    check( gaps_st_layout_holds( e.total_bytes == sum, "portable estimate: total_bytes is not the sum of its parts" ) );
    check( e.num_exclusions > 0 && e.num_exclusions < ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS );
    for ( int i = 0; i < ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS; i++ )
    {
        if ( i < e.num_exclusions )
        {
            check( gaps_st_layout_holds( e.exclusions[i] != NULL, "portable estimate: a counted exclusion is NULL" ) );
        }
        else
        {
            check( gaps_st_layout_holds( e.exclusions[i] == NULL, "portable estimate: an exclusion past num_exclusions is not NULL" ) );
        }
    }
}

REGISTER_UNIT_TEST( test_gaps_st_layout_host_memory_estimate_on_a_dirty_stack );

#if ROCKETNET_DEV

int rocketnet_server_transport_carves_for_test( const struct rocketnet_server_transport_t * server, const char ** names,
                                                uint64_t * offsets, uint64_t * counts, uint64_t * strides, int max,
                                                uint64_t * fixed_bytes );
bool rocketnet_server_transport_seat_clients_for_test( struct rocketnet_server_transport_t * server, int clients );
int rocketnet_server_transport_cookie_reply_source_words_for_test( const struct rocketnet_server_transport_t * server, bool * clear );

#define GAPS_ST_LAYOUT_CARVES 22

// the layout, restated here: each carve in order, and whether a page (4096
// bytes) separates it from what comes before; every carve starts 64-aligned,
// and 8 bytes of slack follow the last

struct gaps_st_layout_expected_t
{
    const char * name;
    bool page_before;
};

static const gaps_st_layout_expected_t gaps_st_layout_expected[GAPS_ST_LAYOUT_CARVES] = {
    { "ring_info", true },
    { "ring_data", true },
    { "ring_sends", true },
    { "ring_send_client_index", false },
    { "ring_accepted", false },
    { "send_to", false },
    { "send_packet_bytes", false },
    { "send_data", false },
    { "send_sends", false },
    { "send_accepted", false },
    { "receive_from", false },
    { "receive_packet_bytes", false },
    { "receive_data", false },
    { "disconnect_bursts", false },
    { "disconnect_burst_keys", false },
    { "cookie_reply_sources", false },
    { "payload_client_index", false },
    { "payload_connection_id", false },
    { "payload_sequence", false },
    { "payload_data", false },
    { "payload_bytes", false },
    { "payload_storage", false },
};

struct gaps_st_layout_carves_t
{
    int count;
    const char * names[GAPS_ST_LAYOUT_CARVES + 1];
    uint64_t offsets[GAPS_ST_LAYOUT_CARVES + 1];
    uint64_t counts[GAPS_ST_LAYOUT_CARVES + 1];
    uint64_t strides[GAPS_ST_LAYOUT_CARVES + 1];
    uint64_t fixed_bytes;
};

static void gaps_st_layout_read_carves( const struct rocketnet_server_transport_t * server, gaps_st_layout_carves_t * carves )
{
    memset( carves, 0, sizeof( *carves ) );
    carves->count = rocketnet_server_transport_carves_for_test( server, carves->names, carves->offsets, carves->counts,
                                                                carves->strides, GAPS_ST_LAYOUT_CARVES + 1, &carves->fixed_bytes );
    check( carves->count == GAPS_ST_LAYOUT_CARVES );
}

static int gaps_st_layout_find( const gaps_st_layout_carves_t * carves, const char * name )
{
    for ( int i = 0; i < carves->count && i < GAPS_ST_LAYOUT_CARVES; i++ )
    {
        if ( strcmp( carves->names[i], name ) == 0 )
        {
            return i;
        }
    }
    check( gaps_st_layout_holds( false, "the carve seam does not name a carve the test expects" ) );
    return 0;
}

static struct rocketnet_server_transport_t * gaps_st_layout_create( int max_clients, struct rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_dev_config( config );
    config->max_clients = max_clients;
    snprintf( config->transport, sizeof( config->transport ), "%s", "loopback" );
    config->loopback_hub = rocketnet_test_loopback_hub();
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", config, sizeof( *config ) );
    check( server != NULL );
    return server;
}

// the allocator that fills every fresh block with 0x5A: a carve the
// transport reads before it writes reads that, never the zero pages a fresh
// system allocation happens to hold. Its blocks are the system allocator's,
// so either frees the other's.

static void * gaps_st_layout_poison_malloc( void * context, size_t bytes )
{
    void * p = rocketnet_default_malloc_function( context, bytes );
    if ( p )
    {
        memset( p, 0x5A, bytes );
    }
    return p;
}

static void gaps_st_layout_poison_free( void * context, void * p )
{
    rocketnet_default_free_function( context, p );
}

static void gaps_st_layout_poison_install()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, gaps_st_layout_poison_malloc, gaps_st_layout_poison_free ) == ROCKETNET_STATUS_OK );
}

static void gaps_st_layout_poison_uninstall()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// ---------------------------------------------------------------------------
// the carves tile the allocation
// ---------------------------------------------------------------------------

// mutation gap: hunt 2/4311, 2/4312, 2/4313, 2/4314, 2/4315, 2/4316, 2/4317,
// 2/4318, 2/4319, 2/4320, 2/4322, 2/4324, 2/4325, 2/4326, 2/4327, 2/4328,
// 2/4329 (a carve deleted: its pointer is the base, over packet_leases at the
// head of the fixed part), hunt 2/4295, 2/4301, 2/4307 (a page gap dropped:
// the ring's arrays move and the plan's bytes shrink).
//
// For a created transport, each carve the transport points at sits where the
// layout puts it: past the fixed part, at the next 64-aligned offset after
// the carve before it (a page further on for the ring's three page-separated
// arrays), inside the block, overlapping no other; and the memory plan's
// transport bytes are exactly the block those carves need.

static void gaps_st_layout_check_carves( int max_clients )
{
    struct rocketnet_server_transport_config_t config;
    struct rocketnet_server_transport_t * server = gaps_st_layout_create( max_clients, &config );
    if ( !server )
    {
        return;
    }

    gaps_st_layout_carves_t carves;
    gaps_st_layout_read_carves( server, &carves );

    const uint64_t ring = (uint64_t) rocketnet_server_send_batch_packets( max_clients );
    const uint64_t send = (uint64_t) rocketnet_server_send_batch_packets( max_clients );
    const uint64_t receive = (uint64_t) rocketnet_server_receive_packets_per_update( max_clients );
    const uint64_t bursts = 2 * (uint64_t) max_clients;
    const uint64_t words = (uint64_t) rocketnet_server_cookie_reply_source_words( max_clients );
    const uint64_t expected_counts[GAPS_ST_LAYOUT_CARVES] = { ring, ring, ring, ring, ring, send, send, send, send, send,
                                                              receive, receive, receive, bursts, bursts, words,
                                                              receive, receive, receive, receive, receive, receive };

    char fault[256];
    uint64_t cursor = carves.fixed_bytes;
    for ( int i = 0; i < GAPS_ST_LAYOUT_CARVES && i < carves.count; i++ )
    {
        snprintf( fault, sizeof( fault ), "max_clients %d: carve %d is named %s, the layout's is %s", max_clients, i,
                  carves.names[i], gaps_st_layout_expected[i].name );
        check( gaps_st_layout_holds( strcmp( carves.names[i], gaps_st_layout_expected[i].name ) == 0, fault ) );

        snprintf( fault, sizeof( fault ), "max_clients %d: carve %s holds %llu elements, the layout's is %llu", max_clients,
                  carves.names[i], (unsigned long long) carves.counts[i], (unsigned long long) expected_counts[i] );
        check( gaps_st_layout_holds( carves.counts[i] == expected_counts[i], fault ) );

        if ( gaps_st_layout_expected[i].page_before )
        {
            cursor = ( ( cursor + 63 ) & ~(uint64_t) 63 ) + 4096;
        }
        const uint64_t expected = ( cursor + 63 ) & ~(uint64_t) 63;

        snprintf( fault, sizeof( fault ),
                  "max_clients %d: carve %s at offset %llu inside the fixed part (%llu bytes): an array over packet_leases",
                  max_clients, carves.names[i], (unsigned long long) carves.offsets[i], (unsigned long long) carves.fixed_bytes );
        check( gaps_st_layout_holds( carves.offsets[i] >= carves.fixed_bytes, fault ) );

        snprintf( fault, sizeof( fault ), "max_clients %d: carve %s at offset %llu, the layout puts it at %llu%s", max_clients,
                  carves.names[i], (unsigned long long) carves.offsets[i], (unsigned long long) expected,
                  gaps_st_layout_expected[i].page_before ? " (a page clear of the carve before)" : "" );
        check( gaps_st_layout_holds( carves.offsets[i] == expected, fault ) );

        cursor = carves.offsets[i] + carves.counts[i] * carves.strides[i];
    }
    const uint64_t block_bytes = cursor + 8;

    // inside the block, and no two overlap

    for ( int i = 0; i < GAPS_ST_LAYOUT_CARVES && i < carves.count; i++ )
    {
        const uint64_t begin = carves.offsets[i];
        const uint64_t end = begin + carves.counts[i] * carves.strides[i];
        snprintf( fault, sizeof( fault ), "max_clients %d: carve %s [%llu, %llu) runs past the block's %llu bytes", max_clients,
                  carves.names[i], (unsigned long long) begin, (unsigned long long) end, (unsigned long long) block_bytes );
        check( gaps_st_layout_holds( begin >= carves.fixed_bytes && end <= block_bytes, fault ) );
        for ( int j = i + 1; j < GAPS_ST_LAYOUT_CARVES && j < carves.count; j++ )
        {
            const uint64_t other_begin = carves.offsets[j];
            const uint64_t other_end = other_begin + carves.counts[j] * carves.strides[j];
            snprintf( fault, sizeof( fault ), "max_clients %d: carves %s and %s overlap", max_clients, carves.names[i], carves.names[j] );
            check( gaps_st_layout_holds( end <= other_begin || other_end <= begin, fault ) );
        }
    }

    // the plan prices exactly that block

    rocketnet_server_memory_plan_t plan;
    check( rocketnet_server_transport_memory_plan( &config, "loopback", plan ) );
    snprintf( fault, sizeof( fault ), "max_clients %d: the plan's transport bytes %llu are not the %llu-byte block the carves need",
              max_clients, (unsigned long long) plan.transport_bytes,
              (unsigned long long) rocketnet_memory_allocations( { block_bytes } ) );
    check( gaps_st_layout_holds( plan.transport_bytes == rocketnet_memory_allocations( { block_bytes } ), fault ) );

    rn_test_server_transport_stop_and_destroy( server );
}

static void test_gaps_st_layout_carves_tile_the_allocation()
{
    gaps_st_layout_check_carves( 4 );
    gaps_st_layout_check_carves( 300 );
    gaps_st_layout_check_carves( 1000 );
}

REGISTER_UNIT_TEST( test_gaps_st_layout_carves_tile_the_allocation );

// ---------------------------------------------------------------------------
// the cookie reply bitmap under a dirty allocator
// ---------------------------------------------------------------------------

// mutation gap: hunt 2/4370 (the bitmap's memset at create deleted), hunt
// 2/4384 (the seam's scan reads one word past the bitmap). Through an
// allocator that fills fresh memory with 0x5A, every word of the bitmap is
// clear after create, and the word after it (padding or the next carve,
// never written at create) is not read.

static void test_gaps_st_layout_cookie_bitmap_clear_under_a_dirty_allocator()
{
    gaps_st_layout_poison_install();

    const int sizes[] = { 4, 64, 1000 };
    for ( int i = 0; i < 3; i++ )
    {
        struct rocketnet_server_transport_config_t config;
        struct rocketnet_server_transport_t * server = gaps_st_layout_create( sizes[i], &config );
        if ( !server )
        {
            break;
        }

        bool clear = false;
        check( rocketnet_server_transport_cookie_reply_source_words_for_test( server, &clear ) == rocketnet_server_cookie_reply_source_words( sizes[i] ) );
        check( gaps_st_layout_holds( clear, "cookie reply bitmap not clear after create under a dirty allocator (a word holds allocator bytes, or the scan read past the bitmap)" ) );

        rn_test_server_transport_stop_and_destroy( server );
    }

    gaps_st_layout_poison_uninstall();
}

REGISTER_UNIT_TEST( test_gaps_st_layout_cookie_bitmap_clear_under_a_dirty_allocator );

// ---------------------------------------------------------------------------
// flushes at exactly capacity, and the ring's refused starts
// ---------------------------------------------------------------------------

#define GAPS_ST_LAYOUT_MAX_SENDS 4096

struct gaps_st_layout_record_t
{
    int calls;
    int count;
    int most_in_one_call;
    int limit;                                          // the most one batch may hold
    bool over_limit;
};

static gaps_st_layout_record_t gaps_st_layout_record;

static void gaps_st_layout_send_batch( void * instance, const rocketnet_platform_send_t * sends, int num_sends )
{
    (void) instance;
    (void) sends;
    gaps_st_layout_record_t * record = &gaps_st_layout_record;
    record->calls++;
    record->count += num_sends;
    if ( num_sends > record->most_in_one_call )
    {
        record->most_in_one_call = num_sends;
    }
    if ( record->limit > 0 && num_sends > record->limit )
    {
        record->over_limit = true;
        printf( "fault: a flush sent %d datagrams, more than the %d slots that were filled (slots past the cap read)\n", num_sends, record->limit );
        fflush( stdout );
    }
}

// swaps the server's send_batch for the recorder; the loopback's table and
// instance come back for the restore

static void gaps_st_layout_record_sends( struct rocketnet_server_transport_t * server, struct rocketnet_server_transport_io_operations_t * operations,
                                         const struct rocketnet_server_transport_io_operations_t ** loopback, void ** loopback_instance, int limit )
{
    memset( &gaps_st_layout_record, 0, sizeof( gaps_st_layout_record ) );
    gaps_st_layout_record.limit = limit;

    const struct rocketnet_server_transport_io_operations_t * swapped = operations;
    void * instance = NULL;
    memset( operations, 0, sizeof( *operations ) );
    rocketnet_server_transport_swap_io_for_test( server, &swapped, &instance );
    *loopback = swapped;
    *loopback_instance = instance;
    *operations = *swapped;
    operations->send_batch = gaps_st_layout_send_batch;
    swapped = operations;
    rocketnet_server_transport_swap_io_for_test( server, &swapped, &instance );
    check( instance == NULL );
}

static void gaps_st_layout_restore( struct rocketnet_server_transport_t * server,
                                    const struct rocketnet_server_transport_io_operations_t * loopback, void * loopback_instance )
{
    const struct rocketnet_server_transport_io_operations_t * swapped = loopback;
    void * instance = loopback_instance;
    rocketnet_server_transport_swap_io_for_test( server, &swapped, &instance );
    rn_test_server_transport_stop_and_destroy( server );
}

// mutation gap: hunt 2/4610 (the link batch marks accepted[] one past the
// sends). One update owes exactly the control buffer's capacity of
// keep-alives, sent as one batch of exactly that many; the byte right after
// send_accepted's capacity (padding or the next carve, inside the block) is
// a planted guard and stays as planted.

static void test_gaps_st_layout_control_flush_at_capacity_leaves_the_next_carve()
{
    const int clients = 512;
    const int capacity = rocketnet_server_send_batch_packets( clients );
    check( capacity == clients );

    struct rocketnet_server_transport_config_t config;
    struct rocketnet_server_transport_t * server = gaps_st_layout_create( clients, &config );
    if ( !server )
    {
        return;
    }
    check( rocketnet_server_transport_seat_clients_for_test( server, clients ) );

    gaps_st_layout_carves_t carves;
    gaps_st_layout_read_carves( server, &carves );
    const int accepted = gaps_st_layout_find( &carves, "send_accepted" );
    check( carves.counts[accepted] == (uint64_t) capacity && carves.strides[accepted] == sizeof( bool ) );
    volatile uint8_t * guard = (uint8_t *) server + carves.offsets[accepted] + (uint64_t) capacity;
    *guard = 0xA5;

    struct rocketnet_server_transport_io_operations_t operations;
    const struct rocketnet_server_transport_io_operations_t * loopback = NULL;
    void * loopback_instance = NULL;
    gaps_st_layout_record_sends( server, &operations, &loopback, &loopback_instance, capacity );

    rocketnet_server_transport_update( server, 1.0 );

    check( gaps_st_layout_record.calls == 1 );
    check( gaps_st_layout_record.count == capacity );
    check( gaps_st_layout_holds( *guard == 0xA5, "the control flush at capacity wrote accepted[capacity], one past send_accepted" ) );

    gaps_st_layout_restore( server, loopback, loopback_instance );
}

REGISTER_UNIT_TEST( test_gaps_st_layout_control_flush_at_capacity_leaves_the_next_carve );

// mutation gap: hunt 2/4610, the payload ring's side. Every ring slot is
// started and finished, the flush sends exactly the ring's size in one batch,
// and the byte after ring_accepted's slots is a guard that stays as planted.

static void test_gaps_st_layout_ring_flush_at_capacity_leaves_the_next_carve()
{
    const int clients = 512;

    struct rocketnet_server_transport_config_t config;
    struct rocketnet_server_transport_t * server = gaps_st_layout_create( clients, &config );
    if ( !server )
    {
        return;
    }
    check( rocketnet_server_transport_seat_clients_for_test( server, 1 ) );

    gaps_st_layout_carves_t carves;
    gaps_st_layout_read_carves( server, &carves );
    const int accepted = gaps_st_layout_find( &carves, "ring_accepted" );
    const int slots = (int) carves.counts[accepted];
    check( rocketnet_server_transport_send_capacity( server ) == slots );
    volatile uint8_t * guard = (uint8_t *) server + carves.offsets[accepted] + (uint64_t) slots;
    *guard = 0xA5;

    struct rocketnet_server_transport_io_operations_t operations;
    const struct rocketnet_server_transport_io_operations_t * loopback = NULL;
    void * loopback_instance = NULL;
    gaps_st_layout_record_sends( server, &operations, &loopback, &loopback_instance, slots );

    for ( int i = 0; i < slots; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * payload = rocketnet_server_transport_start_packet( server, 0, 32, &context );
        check( payload != NULL );
        if ( !payload )
        {
            break;
        }
        memset( payload + context.payload_index, (uint8_t) i, 32 );
        check( rocketnet_server_transport_finish_packet( server, &context, 32 ) > 0 );
    }
    rocketnet_server_transport_send_packets( server );

    check( gaps_st_layout_record.calls == 1 );
    check( gaps_st_layout_record.count == slots );
    check( gaps_st_layout_holds( *guard == 0xA5, "the ring flush at capacity wrote accepted[size], one past ring_accepted" ) );

    gaps_st_layout_restore( server, loopback, loopback_instance );
}

REGISTER_UNIT_TEST( test_gaps_st_layout_ring_flush_at_capacity_leaves_the_next_carve );

// mutation gap: hunt 2/5863 (the flush's clamp of the ring counter to
// send_capacity deleted). Under a dirty allocator, a ring with a send
// capacity of 8 below its 512 slots takes 8 starts and refuses 200 more; the
// counter has moved past the cap, the slots past it were never written, and
// the flush sends exactly the 8 finished frames.

static void test_gaps_st_layout_ring_refused_starts_are_never_flushed()
{
    const int capacity = 8;
    const int refused = 200;

    gaps_st_layout_poison_install();

    rocketnet_server_transport_set_create_send_capacity_for_test( capacity );
    struct rocketnet_server_transport_config_t config;
    struct rocketnet_server_transport_t * server = gaps_st_layout_create( 4, &config );
    if ( server )
    {
        check( rocketnet_server_transport_send_capacity( server ) == capacity );
        check( rocketnet_server_transport_seat_clients_for_test( server, 1 ) );

        gaps_st_layout_carves_t carves;
        gaps_st_layout_read_carves( server, &carves );
        check( carves.counts[gaps_st_layout_find( &carves, "ring_info" )] >= (uint64_t) ( capacity + refused ) );

        struct rocketnet_server_transport_io_operations_t operations;
        const struct rocketnet_server_transport_io_operations_t * loopback = NULL;
        void * loopback_instance = NULL;
        gaps_st_layout_record_sends( server, &operations, &loopback, &loopback_instance, capacity );

        for ( int i = 0; i < capacity; i++ )
        {
            struct rocketnet_packet_context_t context;
            uint8_t * payload = rocketnet_server_transport_start_packet( server, 0, 32, &context );
            check( payload != NULL );
            if ( !payload )
            {
                break;
            }
            memset( payload + context.payload_index, (uint8_t) i, 32 );
            check( rocketnet_server_transport_finish_packet( server, &context, 32 ) > 0 );
        }
        for ( int i = 0; i < refused; i++ )
        {
            struct rocketnet_packet_context_t context;
            check( rocketnet_server_transport_start_packet( server, 0, 32, &context ) == NULL );
        }
        rocketnet_server_transport_send_packets( server );

        check( gaps_st_layout_holds( !gaps_st_layout_record.over_limit && gaps_st_layout_record.count == capacity,
                                     "the ring flush did not send exactly the send capacity's finished frames" ) );

        gaps_st_layout_restore( server, loopback, loopback_instance );
    }

    gaps_st_layout_poison_uninstall();
}

REGISTER_UNIT_TEST( test_gaps_st_layout_ring_refused_starts_are_never_flushed );

#endif // #if ROCKETNET_DEV
