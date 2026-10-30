/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#include "native_shutdown.h"
#include "rocketnet_internal.h"
#include "portable_crt.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_packet_lease.h"
#include "rocketnet_stats_window.h"
#include "rocketnet_pending_frames.h"
#include "rocketnet_memory.h"
#include "rocketnet_fragment.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet_frame_sync.h"
#if ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#include "rocketnet_constants.h"
#endif // #if ROCKETNET_XDP
#include <atomic>
#include <initializer_list>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Windows has no setenv or unsetenv: the same shim as tests/functional/harness.inc
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static int setenv( const char * name, const char * value, int overwrite )
{
    char existing[4096];
    if ( !overwrite && rocketnet_test_getenv( name, existing, sizeof( existing ) ) )
        return 0;
    return _putenv_s( name, value );
}

static int unsetenv( const char * name ) { return _putenv_s( name, "" ); }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#define check( c ) do { if (!(c)) { fprintf(stderr, "lifecycle check failed: %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct allocator_state
{
    std::atomic<int> live{ 0 };
    std::atomic<int> calls{ 0 };
    std::atomic<int> fail_at{ 0 };
};

static void * allocate( void * context, size_t bytes )
{
    auto & state = *(allocator_state *) context;

    const int call = ++state.calls;
    if ( state.fail_at == call )
        return NULL;

    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;

    // valid fundamental alignment, deliberately not cache-line alignment

    static_assert( alignof( max_align_t ) <= 16, "test allocator needs a wider alignment offset" );

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memset( (void *) address, 0xA5, bytes );
    ++state.live;

    return (void *) address;
}

static void release( void * context, void * pointer )
{
    auto & state = *(allocator_state *) context;

    void * base = NULL;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    check( --state.live >= 0 );
    free( base );
}

static void * refuse_allocation( void *, size_t ) { return NULL; }
static void refuse_free( void *, void * ) { check( false ); }
static void thread_body( void * ) {}

// The platform thread and shim share one custom-allocator-backed record.
static const int expected_platform_thread_allocations = 1;

struct ack_writer
{
    rocketnet_ack_endpoint_t * endpoint;
    std::atomic<bool> * go;
    uint64_t sequence;
    bool accepted;
};

static void ack_thread( void * data )
{
    auto * writer = (ack_writer *) data;
    while ( !writer->go->load( std::memory_order_acquire ) )
    {
    }

    writer->accepted = rocketnet_ack_endpoint_record_sent( writer->endpoint, writer->sequence, 100, writer->sequence, 1, 0, 10.0 );
}

static void check_ack_retention( allocator_state & state )
{
    auto * endpoint = (rocketnet_ack_endpoint_t *) rocketnet_malloc( &state, sizeof( rocketnet_ack_endpoint_t ) );
    check( endpoint );
    rocketnet_ack_endpoint_reset( endpoint );

    const uint64_t depth = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    uint64_t * ids = (uint64_t *) rocketnet_malloc( &state, depth * sizeof( uint64_t ) );
    check( ids );

    // Identical, fast, slow and irregular caller time. No return traffic until
    // the complete retained burst has arrived, then one ACK describes it all.

    for ( int cadence = 0; cadence < 4; ++cadence )
    {
        rocketnet_ack_endpoint_reset( endpoint );

        rocketnet_replay_protection_t replay{};
        double last_time = 0.0;

        for ( uint64_t sequence = 1; sequence <= depth; ++sequence )
        {
            last_time += cadence == 0 ? 0.0 : cadence == 1 ? 0.000001
                                          : cadence == 2   ? 0.1
                                                           : ( sequence % 7 == 0 ? 0.3 : 0.0001 );
            check( rocketnet_ack_endpoint_record_sent( endpoint, sequence, 100, sequence, 1, 0, last_time ) );
            rocketnet_replay_advance( &replay, sequence );
        }

        uint8_t data[ROCKETNET_ACK_BLOCK_MAX_BYTES];
        rocketnet_ack_block_t block{};
        int bytes = rocketnet_ack_block_build( data, sizeof( data ), &replay );
        check( rocketnet_ack_block_read( &block, data, bytes, depth ) == bytes );
        check( block.first_range_length == depth );

        int count = 0;
        uint64_t unknown = 0, full = 0;
        check( rocketnet_ack_endpoint_process_block( endpoint, &block, last_time + 2.0,
                                                     ids, (int) depth, &count, &unknown, &full ) == (int) depth );
        check( count == (int) depth && unknown == 0 && full == 0 );
        for ( uint64_t i = 0; i < depth; ++i )
        {
            check( ids[i] == depth - i );
        }
        check( rocketnet_ack_endpoint_process_block( endpoint, &block, last_time + 3.0,
                                                     ids, (int) depth, &count, &unknown, &full ) == 0 );
    }

    rocketnet_free( &state, ids );

    rocketnet_ack_endpoint_reset( endpoint );
    for ( uint64_t sequence = 1; sequence <= depth; ++sequence )
    {
        check( rocketnet_ack_endpoint_record_sent( endpoint, sequence, 100, sequence, 1, 0, 10.0 ) );
    }

    const uint64_t next = depth + 1;

    // Overflow is advisory, not a refusal or time-based wait. The newer
    // sequence is recorded immediately even with zero elapsed time.

    check( !rocketnet_ack_endpoint_record_sent( endpoint, next, 100, next, 1, 0, 10.0 ) );
    check( endpoint->sent[1].sequence == next && endpoint->sent[1].packet_id == next );
    check( !rocketnet_ack_endpoint_record_sent( endpoint, 1, 100, 1, 1, 0, 12.0 ) );
    check( endpoint->sent[1].sequence == next );

    rocketnet_ack_endpoint_reset( endpoint );

    std::atomic<bool> go{ false };
    ack_writer writers[2] = { { endpoint, &go, 1, false }, { endpoint, &go, next, false } };
    auto * first = rocketnet_platform_thread_create( &state, ack_thread, &writers[0] );
    auto * second = rocketnet_platform_thread_create( &state, ack_thread, &writers[1] );
    check( first && second );
    go.store( true, std::memory_order_release );
    rocketnet_platform_thread_join( first );
    rocketnet_platform_thread_join( second );
    rocketnet_platform_thread_destroy( first );
    rocketnet_platform_thread_destroy( second );
    check( writers[0].accepted != writers[1].accepted );

    const uint64_t winner = next;
    check( endpoint->sent[1].sequence == winner && endpoint->sent[1].packet_id == winner );
    rocketnet_free( &state, endpoint );

    auto * acks = rocketnet_server_acks_create( &state, 2 );
    check( acks && acks->acked_capacity == 2 * ROCKETNET_SENT_PACKET_BUFFER_SIZE );
    rocketnet_server_acks_destroy( acks );
}

struct lease_consumer
{
    rocketnet_packet_leases_t<8> * leases;
    rocketnet_packet_context_t context;
    std::atomic<bool> * go;
    bool consumed;
};

static void lease_thread( void * data )
{
    auto * consumer = (lease_consumer *) data;
    while ( !consumer->go->load( std::memory_order_acquire ) )
    {
    }

    rocketnet_packet_context_t trusted{};
    uint64_t backend = 0;
    consumer->consumed = consumer->leases->take( consumer->context, trusted, backend );
}

static void check_packet_leases( allocator_state & state )
{
    rocketnet_packet_leases_t<8> leases{}, other{};
    uint8_t frame[128]{};
    rocketnet_packet_context_t context{};
    context.frame = frame;
    context.packet_id = 77;
    context.sequence = 100;
    context.client_index = 2;
    context.payload_index = 32;
    context.payload_capacity = 64;
    check( leases.insert( context ) );

    rocketnet_packet_context_t original = context, trusted{};
    uint64_t backend = 0;
    check( !other.take( context, trusted, backend ) );

    for ( int field = 0; field < 6; ++field )
    {
        auto changed = original;

        switch ( field )
        {
            case 0: changed.frame++; break;
            case 1: changed.packet_id++; break;
            case 2: changed.sequence++; break;
            case 3: changed.client_index++; break;
            case 4: changed.payload_index = -1; break;
            case 5: changed.payload_capacity = INT32_MAX; break;
        }

        check( !leases.take( changed, trusted, backend ) );
    }

    std::atomic<bool> go{ false };
    lease_consumer consumers[2] = { { &leases, original, &go, false }, { &leases, original, &go, false } };
    auto * first = rocketnet_platform_thread_create( &state, lease_thread, &consumers[0] );
    auto * second = rocketnet_platform_thread_create( &state, lease_thread, &consumers[1] );
    check( first && second );
    go.store( true, std::memory_order_release );
    rocketnet_platform_thread_join( first );
    rocketnet_platform_thread_join( second );
    rocketnet_platform_thread_destroy( first );
    rocketnet_platform_thread_destroy( second );
    check( consumers[0].consumed != consumers[1].consumed );
    check( !leases.take( original, trusted, backend ) );

    // backends may hand an aborted frame out again in the same fill

    context = original;
    context.packet_id = 88;
    check( leases.insert( context ) && context.packet_id != original.packet_id );
    check( !leases.take( original, trusted, backend ) );
    check( leases.take( context, trusted, backend ) && backend == 88 );
    leases.reset();
    check( !leases.take( context, trusted, backend ) );
    context = original;
    context.packet_id = 99;
    check( leases.insert( context ) );
    check( !leases.take( original, trusted, backend ) );
    check( leases.take( context, trusted, backend ) && backend == 99 );

    // capacity is explicit and exhaustion leaves the caller's metadata alone

    leases.reset();

    for ( int i = 0; i < 4; ++i )
    {
        context = original;
        check( leases.insert( context ) );
    }

    context = original;
    check( !leases.insert( context ) && context.packet_id == original.packet_id );
}

static void check_stats_cadence()
{
    struct bucket
    {
        double time;
        uint64_t frame;
        uint64_t packets;
    };

    for ( int updates : { 16, 256, 2000, 10000 } )
    {
        bucket buckets[256]{};
        int index = 0;

        for ( int i = 0; i < updates; ++i )
        {
            index = rocketnet_stats_window_advance( buckets, index, (uint64_t) i + 1, 0.9 * i / updates );
            buckets[index].packets++;
        }

        uint64_t total = 0;
        for ( const auto & b : buckets )
            if ( b.frame && b.time + 1.0 >= 0.9 )
                total += b.packets;
        check( total == (uint64_t) updates );

        for ( int i = 0; i < 10000; ++i )
        {
            index = rocketnet_stats_window_advance( buckets, index, (uint64_t) updates + (uint64_t) i + 1, 0.9 );
            buckets[index].packets++;
        }

        total = 0;
        for ( const auto & b : buckets )
            if ( b.frame && b.time + 1.0 >= 0.9 )
                total += b.packets;
        check( total == (uint64_t) updates + 10000 );
        index = rocketnet_stats_window_advance( buckets, index, 20001, 3.0 );
        total = 0;
        for ( const auto & b : buckets )
            if ( b.frame && b.time + 1.0 >= 3.0 )
                total += b.packets;
        check( total == 0 );
    }
}

static void check_frame_conservation()
{
    rocketnet_pending_frames_t<8> pending;
    uint64_t first[] = { 11, 12, 13, 14 };
    uint64_t second[] = { 15, 16, 17, 18 };
    check( pending.append( first, 4 ) );

    uint64_t submitted[8]{};
    int written = 0, published = 0;
    auto write = [&]( uint32_t index, uint64_t frame )
    {
        check( index < 8 && !submitted[index] );
        submitted[index] = frame;
        ++written;
    };
    auto submit = [&]( uint32_t count )
    { check(written == (int)count); published += (int)count; };

    // the refusing reserve reads its zero through a volatile, so the optimiser
    // cannot prove flush's code after the refusal unreachable (MSVC C4702)

    volatile uint32_t refused = 0;
    for ( int i = 0; i < 10; ++i )
        check( !pending.flush( [&]( uint32_t, uint32_t * )
                               { return (uint32_t) refused; }, write, submit ) );
    check( pending.count == 4 && written == 0 && published == 0 );
    check( pending.append( second, 4 ) && pending.available() == 0 );
    check( !pending.append( first, 1 ) && pending.count == 8 );
    check( pending.flush( []( uint32_t count, uint32_t * index )
                          { *index = 0; return count; }, write, submit ) );
    check( pending.count == 0 && published == 8 );
    for ( int i = 0; i < 8; ++i )
    {
        check( submitted[i] == (uint64_t) ( 11 + i ) );
    }

    // an empty buffer must not reserve: count the calls, rather than fail
    // inside the lambda, so flush's code after the reserve stays reachable

    int empty_reserves = 0;
    check( pending.flush( [&]( uint32_t, uint32_t * )
                          { ++empty_reserves; return (uint32_t) refused; }, write, submit ) );
    check( empty_reserves == 0 );
    check( published == 8 ); // no frame submitted twice after successful retry
}

static void check_memory_geometry( allocator_state & state )
{
    uint64_t total = UINT64_MAX - 3;
    check( !rocketnet_memory_add( total, 2, 2 ) && total == UINT64_MAX - 3 );
    check( rocketnet_memory_add( total, 1, 3 ) && total == UINT64_MAX );

    const uint64_t small_geometry_bytes = rocketnet_reassembly_memory_required( 4, 8, 16384, 32 );
    const uint64_t large_geometry_bytes = rocketnet_reassembly_memory_required( 1000, 256, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 256000 );
    check( small_geometry_bytes > 32 * 16384 && large_geometry_bytes > UINT64_C( 100 ) * 1024 * 1024 * 1024 );
    check( !rocketnet_reassembly_memory_required( -1, 8, 16384, 32 ) );

    const int before = state.calls;

    if ( large_geometry_bytes > (uint64_t) ROCKETNET_REASSEMBLY_MEMORY_MIB * 1024 * 1024 )
    {
        check( !rocketnet_reassembly_set_create( &state, 1000, 256, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 256000 ) );
        check( state.calls == before ); // refused before any allocation
    }
#if ROCKETNET_XDP
    check( !rocketnet_xdp_umem_bytes( 0 ) );
    check( rocketnet_xdp_umem_bytes( 7 ) == (uint64_t) 7 * ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE );
    check( !rocketnet_server_socket_memory_required( 0 ) );
    check( rocketnet_server_socket_memory_required( 7 ) > rocketnet_xdp_umem_bytes( 7 ) );

    // Twenty-two queues exceed 4 GiB at the current UMEM geometry (192 MiB
    // per queue). This plans the allocation size without allocating it.

    check( rocketnet_xdp_umem_bytes( 22 ) > UINT32_MAX );
    check( rocketnet_server_socket_memory_required( 22 ) > UINT32_MAX );
    check( rocketnet_server_socket_memory_required( 1024 ) > rocketnet_server_socket_memory_required( 7 ) );
    check( state.calls == before );
#endif // #if ROCKETNET_XDP
}

template <typename T, typename CreateFn, typename DestroyFn>
static int sweep_component_create( allocator_state & state, CreateFn create_fn, DestroyFn destroy_fn, bool hold_live_check = true )
{
    const int baseline = state.live;

    for ( int n = 1;; ++n )
    {
        check( n <= 64 );

        const int start_calls = state.calls;
        state.fail_at = start_calls + n;

        T * instance = create_fn();
        state.fail_at = 0;

        const int made = state.calls - start_calls;

        if ( !instance )
        {
            check( made >= n );
            check( state.live == baseline );
        }
        else
        {
            const int allocations = n - 1;
            check( made == allocations );
            if ( hold_live_check )
            {
                check( state.live == baseline + allocations );
            }
            destroy_fn( instance );
            check( state.live == baseline );
            return allocations;
        }
    }
}

static void check_token_history_create_sweep( allocator_state & state )
{
    const int baseline = state.live;

    // rocketnet_used_token_history_create: 17 allocations (fail N=1..17,
    // succeed N=18). One history holder, seven allocations for the
    // response-binding table, and nine for the claim table, whose deadline
    // and two insertion-order ring arrays replace the binding's expiry.

    check( sweep_component_create<rocketnet_used_token_history_t>(
               state,
               [&]()
               { return rocketnet_used_token_history_create( &state, 16 ); },
               []( rocketnet_used_token_history_t * h )
               { rocketnet_used_token_history_destroy( h ); } ) == 17 );
    check( state.live == baseline );
    printf( "token history allocation sweep OK: 17 allocations\n" );
}

static void check_component_create_sweeps( allocator_state & state )
{
    const int baseline = state.live;

    // 1. rocketnet_encryption_map_create: 7 allocations (fail N=1..7, succeed N=8).

    check( sweep_component_create<rocketnet_encryption_map_t>(
               state,
               [&]()
               { return rocketnet_encryption_map_create( &state, 16 ); },
               []( rocketnet_encryption_map_t * m )
               { rocketnet_encryption_map_destroy( m ); } ) == 7 );

    // 4. rocketnet_client_slots_create: 9 allocations (fail N=1..9, succeed N=10).

    check( sweep_component_create<rocketnet_client_slots_t>(
               state,
               [&]()
               { return rocketnet_client_slots_create( &state, 16 ); },
               []( rocketnet_client_slots_t * s )
               { rocketnet_client_slots_destroy( s ); } ) == 9 );

    // 5. rocketnet_reassembly_set_create: 4 allocations (fail N=1..4, succeed N=5).

    check( sweep_component_create<rocketnet_reassembly_set_t>(
               state,
               [&]()
               { return rocketnet_reassembly_set_create( &state, 4, 8, 16384, 32 ); },
               []( rocketnet_reassembly_set_t * r )
               { rocketnet_reassembly_set_destroy( r ); } ) == 4 );

    // 6. rocketnet_send_arena_create: 2 allocations (fail N=1..2, succeed N=3).

    check( sweep_component_create<rocketnet_send_arena_t>(
               state,
               [&]()
               { return rocketnet_send_arena_create( &state, 16, 1280 ); },
               []( rocketnet_send_arena_t * a )
               { rocketnet_send_arena_destroy( a ); } ) == 2 );

    // 7. rocketnet_server_acks_create: 4 allocations (fail N=1..4, succeed N=5).
    {
        const int before = state.calls;
        check( !rocketnet_server_acks_create( &state, INT32_MAX / ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1 ) );
        check( state.calls == before && state.live == baseline );
    }

    check( sweep_component_create<rocketnet_server_acks_t>(
               state,
               [&]()
               { return rocketnet_server_acks_create( &state, 16 ); },
               []( rocketnet_server_acks_t * a )
               { rocketnet_server_acks_destroy( a ); } ) == 4 );

    // 8. rocketnet_network_simulator_create: 12 allocations (fail N=1..12, succeed N=13).
    // A production build (ROCKETNET_DEV=0) has no simulator (sdk/rocketnet_test_seams.h).
#if ROCKETNET_DEV
    check( sweep_component_create<rocketnet_network_simulator_t>(
               state,
               [&]()
               { return rocketnet_network_simulator_create( &state, 64, 1 ); },
               []( rocketnet_network_simulator_t * s )
               { rocketnet_network_simulator_destroy( s ); } ) == 12 );
#endif // #if ROCKETNET_DEV

    // 9. rn_arena_create (sdk/rocketnet_allocator.h): 1 allocation (fail N=1, succeed N=2).
    // Refused configs and region counts allocate nothing; in-region carve/send/store allocate nothing.

    rocketnet_counters_t counters{};
    rocketnet_message_config_t msg_config{};
    rocketnet_message_default_config( &msg_config );

    {
        const int before = state.calls;
        check( !rn_arena_create( &state, &msg_config, 0, &counters ) );
        check( !rn_arena_create( &state, &msg_config, ( 1 << 20 ) + 1, &counters ) );

        rocketnet_message_config_t bad_config = msg_config;
        bad_config.num_channels = 0;
        check( !rn_arena_create( &state, &bad_config, 1, &counters ) );
        check( state.calls == before && state.live == baseline );
    }

    check( sweep_component_create<rn_arena_t>(
               state,
               [&]()
               { return rn_arena_create( &state, NULL, 1, &counters ); },
               []( rn_arena_t * a )
               { rn_arena_destroy( a ); } ) == 1 );
    check( sweep_component_create<rn_arena_t>(
               state,
               [&]()
               { return rn_arena_create( &state, &msg_config, 2, &counters ); },
               [&]( rn_arena_t * a )
               {
                   const int before = state.calls;
                   state.fail_at = before + 1;
                   for ( int i = 0; rn_arena_region( a, i ) != NULL; ++i )
                   {
                       rn_region_t * r = rn_arena_region( a, i );
                       check( r != NULL );
                       rn_region_carve( r );
                       uint32_t d = rn_region_send_alloc( r, 256 );
                       check( d != RN_DESCRIPTOR_NONE );
                       rn_region_send_free( r, d );
                       uint32_t c = rn_send_store_alloc( &r->store, 512 );
                       check( c != RN_CHUNK_NONE );
                       check( rn_send_store_free_checked( &r->store, c ) );
                   }
                   state.fail_at = 0;
                   check( state.calls == before );
                   rn_arena_destroy( a );
               } ) == 1 );

    // 10. rocketnet_frame_sync_create: 1 allocation (fail N=1, succeed N=2).
    {
        const int before = state.calls;
        check( !rocketnet_frame_sync_create( &state, 0.0 ) );
        check( !rocketnet_frame_sync_create( &state, -60.0 ) );
        check( !rocketnet_frame_sync_create( &state, ROCKETNET_FRAME_SYNC_MAX_FRAME_RATE + 1.0 ) );
        check( state.calls == before && state.live == baseline );
    }

    check( sweep_component_create<rocketnet_frame_sync_t>(
               state,
               [&]()
               { return rocketnet_frame_sync_create( &state, 60.0 ); },
               []( rocketnet_frame_sync_t * f )
               { rocketnet_frame_sync_destroy( f ); } ) == 1 );

    // 11. The platform thread and its shim share an allocator-backed record.

    check( sweep_component_create<rocketnet_platform_thread_t>(
               state,
               [&]()
               { return rocketnet_platform_thread_create( &state, thread_body, NULL ); },
               []( rocketnet_platform_thread_t * t )
               {
                   rocketnet_platform_thread_join( t );
                   rocketnet_platform_thread_destroy( t );
               },
               false ) == expected_platform_thread_allocations );

    check( state.live == baseline );
}

// The operator memory budgets (#155): the server and client tables' memory
// budget field is overlaid by ROCKETNET_SERVER_/CLIENT_MEMORY_BUDGET_BYTES.
// A budget below a candidate's default plan refuses the create before any
// allocation (the counting allocator's call count is unchanged); a budget
// above the plan lets the create succeed. The env shim is the platform's
// real environment, read through rocketnet_platform_getenv.

// a valid connect token with future timestamps, minted with the production
// test backend secret (the library's non-development build has no dev minting)

static void write_valid_token( uint8_t * token, uint64_t create, uint64_t expire )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    memset( &private_fields, 0, sizeof( private_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = 1;
    signed_fields.create_timestamp = create;
    signed_fields.expire_timestamp = expire;
    signed_fields.timeout_seconds = 60;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;

    // a real port, little-endian 40000: the client connects its socket to the
    // server the token names, and darwin refuses a connect to port 0

    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;

    // not the published development server key, which a production build
    // (ROCKETNET_DEV=0) refuses at connect

    memset( signed_fields.server_public_key, 0x43, 32 );
    private_fields.client_id = 1;
    private_fields.protocol_id = 1;
    private_fields.expire_timestamp = expire;
    memset( private_fields.client_to_server_key, 0x11, 32 );
    memset( private_fields.server_to_client_key, 0x22, 32 );
    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token ) );
}

static void make_valid_token( uint8_t * token )
{
    const uint64_t now = (uint64_t) time( NULL );
    write_valid_token( token, now - 100, now + 100000 );
}

static void check_game_clock_client_create( allocator_state & state )
{
    const uint64_t epoch = 1700000000ULL;
    const uint64_t create = epoch + 1000;
    const uint64_t expire = create + 10;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    write_valid_token( token, create, expire );

    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    memset( config.packet_filter, 0x5A, sizeof( config.packet_filter ) );
    config.token_epoch_unix_seconds = epoch;

    rocketnet_client_transport_t * client = rocketnet_client_transport_create_ex( &state, token, &config, 1000.0, NULL );
    check( client != NULL );
    rn_native_client_transport_stop_and_destroy( client );
}

static void check_server_memory_budget( allocator_state & state )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    const char * transport = "gso";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    const char * transport = "msgx";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const char * transport = "iocp";
#else
#error "server memory budget lifecycle test needs a production server transport"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    // non-development keys and filter: a production build (ROCKETNET_DEV=0)
    // refuses the published development keys and test packet filter, so any
    // other values pass

    memset( config.packet_filter, 0x5A, sizeof( config.packet_filter ) );
    memset( config.server_public_key, 0xAB, 32 );
    memset( config.server_secret_key, 0xCD, 32 );
    memset( config.backend_public_key, 0xEF, 32 );
    config.protocol_id = 1;
    snprintf( config.transport, sizeof( config.transport ), "%s", transport );

    // The plan below is the production transport's, so the create must run
    // that transport. CI runs this binary with ROCKETNET_SERVER_TRANSPORT=portable,
    // and the env layer displaces the struct's transport: the production
    // library then refuses the portable pin, which made the budget refusal
    // pass for the wrong reason and the budget success fail. Hold the env
    // pin aside for this check and put it back afterwards.

    char pinned_transport[64];
    const bool transport_pinned = rocketnet_test_getenv( "ROCKETNET_SERVER_TRANSPORT", pinned_transport, sizeof( pinned_transport ) ) != 0;
    unsetenv( "ROCKETNET_SERVER_TRANSPORT" );

    const uint64_t plan = rocketnet_server_transport_required_memory( &config, transport );
    check( plan > 0 );

    char budget[64];
    snprintf( budget, sizeof( budget ), "%llu", (unsigned long long) ( plan - 1 ) );
    setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", budget, 1 );

    const int calls_before = state.calls;
    const int live_before = state.live;
    check( rocketnet_server_transport_create( &state, "127.0.0.1:0", &config ) == NULL );
    check( state.calls == calls_before );       // refused before any allocation
    check( state.live == live_before );
    unsetenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" );

    snprintf( budget, sizeof( budget ), "%llu", (unsigned long long) ( plan + 1 ) );
    setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", budget, 1 );

    rocketnet_server_transport_t * server = rocketnet_server_transport_create( &state, "127.0.0.1:0", &config );
    check( server != NULL );
    rn_native_server_transport_stop_and_destroy( server );
    unsetenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" );
    if ( transport_pinned )
    {
        setenv( "ROCKETNET_SERVER_TRANSPORT", pinned_transport, 1 );
    }
}

static void check_client_memory_budget( allocator_state & state )
{
    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );

    // not the published test packet filter, which a production build refuses

    memset( config.packet_filter, 0x5A, sizeof( config.packet_filter ) );

    const uint64_t plan = rocketnet_client_transport_required_memory( &config, "portable" );
    check( plan > 0 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    make_valid_token( token );

    char budget[64];
    snprintf( budget, sizeof( budget ), "%llu", (unsigned long long) ( plan - 1 ) );
    setenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", budget, 1 );

    const int calls_before = state.calls;
    const int live_before = state.live;
    check( rocketnet_client_transport_create( &state, token, &config, 0.0 ) == NULL );
    check( state.calls == calls_before );       // refused before any allocation
    check( state.live == live_before );
    unsetenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES" );

    snprintf( budget, sizeof( budget ), "%llu", (unsigned long long) ( plan + 1 ) );
    setenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", budget, 1 );

    rocketnet_client_transport_t * client = rocketnet_client_transport_create( &state, token, &config, 0.0 );
    check( client != NULL );
    rn_native_client_transport_stop_and_destroy( client );
    unsetenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES" );
}

int main()
{
    allocator_state state;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_allocator_ex( allocate, release );
    check( rocketnet_malloc( &state, SIZE_MAX ) == NULL );
    check( state.calls == 0 );

    allocator_state token_history_state;
    check_token_history_create_sweep( token_history_state );

    rocketnet_server_transport_config_t memory_config;
    rocketnet_server_transport_default_config( &memory_config );
    check( rocketnet_server_transport_required_memory( NULL, "portable" ) > 0 );

    const uint64_t default_memory = rocketnet_server_transport_required_memory( &memory_config, "portable" );
    check( default_memory > 0 );
    check( rocketnet_server_transport_required_memory( &memory_config, "auto" ) == 0 );
    check( rocketnet_server_transport_required_memory( &memory_config, "custom" ) == 0 );
    check( rocketnet_server_transport_required_memory( &memory_config, NULL ) == 0 );
    memory_config.max_clients = ROCKETNET_MAX_CLIENTS + 1;
    check( rocketnet_server_transport_required_memory( &memory_config, "portable" ) == 0 );
    memory_config.max_clients = ROCKETNET_MAX_CLIENTS;
    memory_config.max_fragmented_messages = ROCKETNET_MAX_FRAGMENTS;

    // the whole-host estimate is a plan too: it allocates nothing before init

    rocketnet_host_memory_estimate_t host_estimate;
    check( rocketnet_server_transport_host_memory_estimate( NULL, "portable", &host_estimate ) );
    check( host_estimate.total_bytes >= rocketnet_server_transport_required_memory( NULL, "portable" ) );
    check( rocketnet_server_transport_host_memory_estimate( &memory_config, "portable", &host_estimate ) );

    // The client transport's plan: defaults plan a nonzero size, the same config
    // plans the same size again, unknown/NULL/auto transports and an overflowing
    // geometry all refuse to 0, and planning allocates nothing. The payload is
    // fixed at ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES (no config field). A
    // create-time counting-allocator match needs a live socket, so it is left
    // to benchmarks/transport_memory.cpp.

    rocketnet_client_transport_config_t client_memory_config;
    rocketnet_client_transport_default_config( &client_memory_config );
    check( rocketnet_client_transport_required_memory( NULL, "portable" ) > 0 );

    const uint64_t client_default_memory = rocketnet_client_transport_required_memory( &client_memory_config, "portable" );
    check( client_default_memory > 0 );
    check( rocketnet_client_transport_required_memory( &client_memory_config, "auto" ) == 0 );
    check( rocketnet_client_transport_required_memory( &client_memory_config, "custom" ) == 0 );
    check( rocketnet_client_transport_required_memory( &client_memory_config, NULL ) == 0 );
    check( rocketnet_client_transport_required_memory( &client_memory_config, "portable" ) == client_default_memory );
    client_memory_config.max_fragmented_messages = ROCKETNET_MAX_FRAGMENTS + 1;
    check( rocketnet_client_transport_required_memory( &client_memory_config, "portable" ) == 0 );
    client_memory_config.max_fragmented_messages = 0;
    check( state.calls == 0 );

    for ( size_t size = 0; size < 256; ++size )
    {
        void * p = rocketnet_malloc( &state, size );
        check( p && (uintptr_t) p % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
        memset( p, 0, size );
        rocketnet_free( &state, p );
    }

    void * p = rocketnet_malloc( &state, 1 );
    rocketnet_allocator_ex( refuse_allocation, refuse_free ); // live allocation prevents replacement
    rocketnet_free( &state, p );
    check( state.live == 0 );
    check( rocketnet_init( &state, allocate, release ) == ROCKETNET_STATUS_OK );
    check( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK );
    rocketnet_forbid_xdp_commit();
    rocketnet_allocator_ex( refuse_allocation, refuse_free ); // initialized library prevents replacement
    p = rocketnet_malloc( &state, 16 );
    check( p );
    rocketnet_term(); // live allocation prevents premature shutdown
    check( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK );
    rocketnet_free( &state, p );

    const int baseline = state.live;

    check_ack_retention( state );
    check_packet_leases( state );
    check_stats_cadence();
    check_frame_conservation();
    check_memory_geometry( state );
    check_component_create_sweeps( state );
    check_server_memory_budget( state );
    check_client_memory_budget( state );
    check_game_clock_client_create( state );
    check( state.live == baseline );

    for ( int failure = 1; failure <= expected_platform_thread_allocations; ++failure )
    {
        state.fail_at = state.calls + failure;
        check( !rocketnet_platform_thread_create( &state, thread_body, NULL ) );
        state.fail_at = 0;
        check( state.live == baseline );
    }

    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( &state, thread_body, NULL );
    check( thread );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    check( state.live == baseline );

    rocketnet_term();
    rocketnet_term();
    check( state.live == 0 );
    rocketnet_allocator_ex( refuse_allocation, refuse_free );
    check( rocketnet_malloc( NULL, 16 ) == NULL );
    check( rocketnet_init( &state, allocate, release ) == ROCKETNET_STATUS_OK );
    rocketnet_term();
    check( state.live == 0 );
    printf( "lifecycle OK: aligned allocator, component create sweeps, thread refusal and init/term\n" );

    return 0;
}
