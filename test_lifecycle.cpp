/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#include "rocketnet.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_packet_lease.h"
#include "rocketnet_stats_window.h"
#include "rocketnet_pending_frames.h"
#include "rocketnet_send_handoff.h"
#include "rocketnet_memory.h"
#include "rocketnet_fragment.h"
#if ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#include "rocketnet_constants.h"
#endif
#include <atomic>
#include <initializer_list>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define check(c) do { if (!(c)) { fprintf(stderr, "lifecycle check failed: %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct allocator_state
{
    std::atomic<int> live{0};
    std::atomic<int> calls{0};
    std::atomic<int> fail_at{0};
};

static void * allocate( void * context, size_t bytes )
{
    auto & state = *(allocator_state*) context;
    const int call = ++state.calls;
    if ( state.fail_at == call )
        return NULL;
    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;
    // Valid fundamental alignment, deliberately not cache-line alignment.
    static_assert( alignof(max_align_t) <= 16, "test allocator needs a wider alignment offset" );
    uintptr_t address = ( ( (uintptr_t) base + sizeof(void*) + 63 ) & ~(uintptr_t)63 ) + 16;
    memcpy( (void*) ( address - sizeof(void*) ), &base, sizeof(base) );
    memset( (void*) address, 0xA5, bytes );
    ++state.live;
    return (void*) address;
}

static void release( void * context, void * pointer )
{
    auto & state = *(allocator_state*) context;
    void * base = NULL;
    memcpy( &base, (uint8_t*) pointer - sizeof(void*), sizeof(base) );
    check( --state.live >= 0 );
    free( base );
}

static void * refuse_allocation( void *, size_t ) { return NULL; }
static void refuse_free( void *, void * ) { check(false); }
static void thread_body( void * ) {}

struct ack_writer
{
    rocketnet_ack_endpoint_t * endpoint;
    std::atomic<bool> * go;
    uint64_t sequence;
    bool accepted;
};

static void ack_thread( void * data )
{
    auto * writer = (ack_writer*) data;
    while ( !writer->go->load(std::memory_order_acquire) ) {}
    writer->accepted = rocketnet_ack_endpoint_record_sent( writer->endpoint, writer->sequence, 100, writer->sequence, 1, 0, 10.0 );
}

static void check_ack_retention( allocator_state & state )
{
    auto * endpoint = (rocketnet_ack_endpoint_t*) rocketnet_malloc( &state, sizeof(rocketnet_ack_endpoint_t) );
    check( endpoint );
    rocketnet_ack_endpoint_reset( endpoint );
    const uint64_t depth = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    uint64_t * ids = (uint64_t*) rocketnet_malloc(&state, depth * sizeof(uint64_t));
    check(ids);
    // Identical, fast, slow and irregular caller time. No return traffic until
    // the complete retained burst has arrived, then one ACK describes it all.
    for ( int cadence = 0; cadence < 4; ++cadence )
    {
        rocketnet_ack_endpoint_reset(endpoint);
        rocketnet_replay_protection_t replay{};
        double last_time = 0.0;
        for ( uint64_t sequence = 1; sequence <= depth; ++sequence )
        {
            last_time += cadence == 0 ? 0.0 : cadence == 1 ? 0.000001 :
                         cadence == 2 ? 0.1 : (sequence % 7 == 0 ? 0.3 : 0.0001);
            check(rocketnet_ack_endpoint_record_sent(endpoint, sequence, 100, sequence, 1, 0, last_time));
            rocketnet_replay_advance(&replay, sequence);
        }
        uint8_t data[ROCKETNET_ACK_BLOCK_MAX_BYTES];
        rocketnet_ack_block_t block{};
        int bytes = rocketnet_ack_block_build(data, sizeof(data), &replay);
        check(rocketnet_ack_block_read(&block, data, bytes, depth) == bytes);
        check(block.first_range_length == depth);
        int count = 0;
        uint64_t unknown = 0, full = 0;
        check(rocketnet_ack_endpoint_process_block(endpoint, &block, last_time + 2.0,
              ids, (int)depth, &count, &unknown, &full) == (int)depth);
        check(count == (int)depth && unknown == 0 && full == 0);
        for ( uint64_t i = 0; i < depth; ++i ) check(ids[i] == depth - i);
        check(rocketnet_ack_endpoint_process_block(endpoint, &block, last_time + 3.0,
              ids, (int)depth, &count, &unknown, &full) == 0);
    }
    rocketnet_free(&state, ids);

    rocketnet_ack_endpoint_reset(endpoint);
    for ( uint64_t sequence = 1; sequence <= depth; ++sequence )
        check(rocketnet_ack_endpoint_record_sent(endpoint, sequence, 100, sequence, 1, 0, 10.0));
    const uint64_t next = depth + 1;
    // Overflow is advisory, not a refusal or time-based wait. The newer
    // sequence is recorded immediately even with zero elapsed time.
    check(!rocketnet_ack_endpoint_record_sent(endpoint, next, 100, next, 1, 0, 10.0));
    check(endpoint->sent[1].sequence == next && endpoint->sent[1].packet_id == next);
    check(!rocketnet_ack_endpoint_record_sent(endpoint, 1, 100, 1, 1, 0, 12.0));
    check(endpoint->sent[1].sequence == next);

    rocketnet_ack_endpoint_reset( endpoint );
    std::atomic<bool> go{false};
    ack_writer writers[2] = {{ endpoint, &go, 1, false }, { endpoint, &go, next, false }};
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

static void lease_thread(void * data)
{
    auto * consumer = (lease_consumer*)data;
    while (!consumer->go->load(std::memory_order_acquire)) {}
    rocketnet_packet_context_t trusted{};
    uint64_t backend = 0;
    consumer->consumed = consumer->leases->take(consumer->context, trusted, backend);
}

static void check_packet_leases(allocator_state & state)
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
    check(leases.insert(context));
    rocketnet_packet_context_t original = context, trusted{};
    uint64_t backend = 0;
    check(!other.take(context, trusted, backend));
    for (int field = 0; field < 6; ++field)
    {
        auto changed = original;
        switch(field)
        {
            case 0: changed.frame++; break;
            case 1: changed.packet_id++; break;
            case 2: changed.sequence++; break;
            case 3: changed.client_index++; break;
            case 4: changed.payload_index = -1; break;
            case 5: changed.payload_capacity = INT32_MAX; break;
        }
        check(!leases.take(changed, trusted, backend));
    }
    std::atomic<bool> go{false};
    lease_consumer consumers[2] = {{&leases, original, &go, false}, {&leases, original, &go, false}};
    auto * first = rocketnet_platform_thread_create(&state, lease_thread, &consumers[0]);
    auto * second = rocketnet_platform_thread_create(&state, lease_thread, &consumers[1]);
    check(first && second);
    go.store(true, std::memory_order_release);
    rocketnet_platform_thread_join(first);
    rocketnet_platform_thread_join(second);
    rocketnet_platform_thread_destroy(first);
    rocketnet_platform_thread_destroy(second);
    check(consumers[0].consumed != consumers[1].consumed);
    check(!leases.take(original, trusted, backend));
    // Backends may hand an aborted frame out again in the same fill.
    context = original;
    context.packet_id = 88;
    check(leases.insert(context) && context.packet_id != original.packet_id);
    check(!leases.take(original, trusted, backend));
    check(leases.take(context, trusted, backend) && backend == 88);
    leases.reset();
    check(!leases.take(context, trusted, backend));
    context = original;
    context.packet_id = 99;
    check(leases.insert(context));
    check(!leases.take(original, trusted, backend));
    check(leases.take(context, trusted, backend) && backend == 99);
    // Capacity is explicit and exhaustion leaves the caller's metadata alone.
    leases.reset();
    for (int i = 0; i < 4; ++i) { context = original; check(leases.insert(context)); }
    context = original;
    check(!leases.insert(context) && context.packet_id == original.packet_id);
}

static void check_stats_cadence()
{
    struct bucket { double time; uint64_t frame; uint64_t packets; };
    for (int updates : {16, 256, 2000, 10000})
    {
        bucket buckets[256]{};
        int index = 0;
        for (int i = 0; i < updates; ++i)
        {
            index = rocketnet_stats_window_advance(buckets, index, (uint64_t)i + 1, 0.9 * i / updates);
            buckets[index].packets++;
        }
        uint64_t total = 0;
        for (const auto & b : buckets) if (b.frame && b.time + 1.0 >= 0.9) total += b.packets;
        check(total == (uint64_t)updates);
        for (int i = 0; i < 10000; ++i)
        {
            index = rocketnet_stats_window_advance(buckets, index, (uint64_t)updates + i + 1, 0.9);
            buckets[index].packets++;
        }
        total = 0;
        for (const auto & b : buckets) if (b.frame && b.time + 1.0 >= 0.9) total += b.packets;
        check(total == (uint64_t)updates + 10000);
        index = rocketnet_stats_window_advance(buckets, index, 20001, 3.0);
        total = 0;
        for (const auto & b : buckets) if (b.frame && b.time + 1.0 >= 3.0) total += b.packets;
        check(total == 0);
    }
}

static void check_frame_conservation()
{
    rocketnet_pending_frames_t<8> pending;
    uint64_t first[] = {11, 12, 13, 14};
    uint64_t second[] = {15, 16, 17, 18};
    check(pending.append(first, 4));
    uint64_t submitted[8]{};
    int written = 0, published = 0;
    auto write = [&](uint32_t index, uint64_t frame) {
        check(index < 8 && !submitted[index]);
        submitted[index] = frame;
        ++written;
    };
    auto submit = [&](uint32_t count) { check(written == (int)count); published += count; };
    for (int i = 0; i < 10; ++i)
        check(!pending.flush([](uint32_t, uint32_t *) { return 0U; }, write, submit));
    check(pending.count == 4 && written == 0 && published == 0);
    check(pending.append(second, 4) && pending.available() == 0);
    check(!pending.append(first, 1) && pending.count == 8);
    check(pending.flush([](uint32_t count, uint32_t * index) { *index = 0; return count; }, write, submit));
    check(pending.count == 0 && published == 8);
    for (int i = 0; i < 8; ++i) check(submitted[i] == (uint64_t)(11 + i));
    check(pending.flush([](uint32_t, uint32_t *) { check(false); return 0U; }, write, submit));
    check(published == 8); // no frame submitted twice after successful retry
}

static void check_send_handoff()
{
    // Reproduce #162's ordering: publish 100 packets, copy only the first
    // 32, then publish another generation while the first still has a tail.
    rocketnet_send_handoff_t<2> queue;
    int packets[2][100] = {};
    int copied[2] = {};
    int first = queue.acquire();
    check(first >= 0 && queue.front() == -1);
    for (int i = 0; i < 100; ++i) packets[first][i] = i + 1;
    queue.publish();
    int second = queue.acquire();
    check(second >= 0 && second != first);
    for (int i = 0; i < 100; ++i) packets[second][i] = i + 101;
    copied[first] = 32;
    queue.publish();
    for (int i = 0; i < 10000; ++i)
        check(queue.acquire() == -1 && queue.front() == first);
    int expected = 33;
    while (queue.front() >= 0)
    {
        const int index = queue.front();
        // A worker can pause between any of its 32-packet batches. A producer
        // cannot reset either queued buffer, however often it asks for storage.
        for (int batch = 0; batch < 32 && copied[index] < 100; ++batch)
            check(packets[index][copied[index]++] == expected++);
        if (copied[index] == 100)
        {
            queue.retire();
            if (index == first)
            {
                check(queue.front() == second);
                check(queue.acquire() == first);
                memset(packets[first], 0, sizeof(packets[first]));
            }
        }
    }
    check(expected == 201 && copied[first] == 100 && copied[second] == 100);
    // Exercise repeated wrap/reuse with the next producer always reserved.
    for (int i = 0; i < 10000; ++i)
    {
        int filling = queue.acquire();
        check(filling >= 0);
        queue.publish();
        check(queue.front() == filling);
        queue.retire();
    }
}

static void check_memory_geometry(allocator_state & state)
{
    uint64_t total = UINT64_MAX - 3;
    check(!rocketnet_memory_add(total, 2, 2) && total == UINT64_MAX - 3);
    check(rocketnet_memory_add(total, 1, 3) && total == UINT64_MAX);
    const uint64_t small_geometry_bytes = rocketnet_reassembly_memory_required(4, 8, 16384, 32);
    const uint64_t large_geometry_bytes = rocketnet_reassembly_memory_required(1000, 256, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 256000);
    check(small_geometry_bytes > 32 * 16384 && large_geometry_bytes > UINT64_C(100) * 1024 * 1024 * 1024);
    check(!rocketnet_reassembly_memory_required(-1, 8, 16384, 32));
    const int before = state.calls;
    if (large_geometry_bytes > (uint64_t)ROCKETNET_REASSEMBLY_MEMORY_MIB * 1024 * 1024)
    {
        check(!rocketnet_reassembly_set_create(&state, 1000, 256, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, 256000));
        rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config(&config);
        config.max_clients = 1000;
        config.num_reassembly_slots = 256;
        config.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES;
        strcpy(config.driver, "portable");
        check(!rocketnet_server_transport_create(&state, "127.0.0.1", &config, 0.0));
        check(state.calls == before); // refused before any allocation or socket
    }
#if ROCKETNET_XDP
    check(!rocketnet_xdp_umem_bytes(0));
    check(rocketnet_xdp_umem_bytes(7) == (uint64_t)7 * ROCKETNET_XDP_NUM_FRAMES * ROCKETNET_XDP_FRAME_SIZE);
    check(!rocketnet_server_socket_memory_required(0));
    check(rocketnet_server_socket_memory_required(7) > rocketnet_xdp_umem_bytes(7));
    check(rocketnet_server_socket_memory_required(7) > UINT64_C(3) * 1024 * 1024 * 1024);
    const uint64_t xdp_huge = rocketnet_server_socket_memory_required(1024);
    if (xdp_huge > (uint64_t)ROCKETNET_XDP_MEMORY_MIB * 1024 * 1024)
        check(!rocketnet_server_socket_preflight("127.0.0.1", 1024));
    check(state.calls == before);
#endif
}

int main()
{
    allocator_state state;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    rocketnet_allocator( allocate, release );
    check( rocketnet_malloc( &state, SIZE_MAX ) == NULL );
    check( state.calls == 0 );
    rocketnet_server_transport_config_t memory_config;
    rocketnet_server_transport_default_config(&memory_config);
    check(rocketnet_server_transport_required_memory(NULL, "portable") > 0);
    const uint64_t default_memory = rocketnet_server_transport_required_memory(&memory_config, "portable");
    check(default_memory > 0);
    check(rocketnet_server_transport_required_memory(&memory_config, "auto") == 0);
    check(rocketnet_server_transport_required_memory(&memory_config, "custom") == 0);
    check(rocketnet_server_transport_required_memory(&memory_config, NULL) == 0);
    memory_config.max_clients = ROCKETNET_MAX_CLIENTS + 1;
    check(rocketnet_server_transport_required_memory(&memory_config, "portable") == 0);
    memory_config.max_clients = ROCKETNET_MAX_CLIENTS;
    memory_config.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES;
    memory_config.num_reassembly_slots = ROCKETNET_MAX_FRAGMENTS;
    // Planning a geometry larger than the component budget never allocates it.
    check(rocketnet_server_transport_required_memory(&memory_config, "portable") > default_memory);
    check( state.calls == 0 );
    for ( size_t size = 0; size < 256; ++size )
    {
        void * p = rocketnet_malloc( &state, size );
        check( p && (uintptr_t)p % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
        memset( p, 0, size );
        rocketnet_free( &state, p );
    }
    void * p = rocketnet_malloc( &state, 1 );
    rocketnet_allocator( refuse_allocation, refuse_free ); // live allocation prevents replacement
    rocketnet_free( &state, p );
    check( state.live == 0 );
    check( rocketnet_init( &state ) );
    check( !rocketnet_init( NULL ) );
    rocketnet_forbid_xdp_commit();
    rocketnet_allocator( refuse_allocation, refuse_free ); // initialized library prevents replacement
    p = rocketnet_malloc( &state, 16 );
    check( p );
    rocketnet_term(); // live allocation prevents premature shutdown
    check( !rocketnet_init( NULL ) );
    rocketnet_free( &state, p );
    const int baseline = state.live;

    check_ack_retention( state );
    check_packet_leases( state );
    check_stats_cadence();
    check_frame_conservation();
    check_send_handoff();
    check_memory_geometry(state);
    check( state.live == baseline );

    rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "127.0.0.1" ) );
    state.fail_at = state.calls + 1;
    check( !rocketnet_platform_socket_create( &state, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 65536, 65536 ) );
    state.fail_at = 0;
    check( state.live == baseline );

    for ( int failure = 1; failure <= 2; ++failure )
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

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_dev_connect_token( "127.0.0.1:40000", 1, (uint64_t) time(NULL), token ) );
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    strcpy( client_config.driver, "portable" );
    int client_success = 0;
    for ( int failure = 1; failure <= 64; ++failure )
    {
        state.fail_at = state.calls + failure;
        auto * client = rocketnet_client_transport_create( &state, token, &client_config, 0.0 );
        state.fail_at = 0;
        if ( client )
        {
            check( (uintptr_t)client % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
            rocketnet_client_transport_destroy( client );
            client_success = failure;
        }
        check( state.live == baseline );
        if ( client_success ) break;
    }
    check( client_success );

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;
    strcpy( server_config.driver, "portable" );
    int server_success = 0;
    for ( int failure = 1; failure <= 64; ++failure )
    {
        state.fail_at = state.calls + failure;
        auto * server = rocketnet_server_transport_create( &state, "127.0.0.1", &server_config, 0.0 );
        state.fail_at = 0;
        if ( server )
        {
            check( (uintptr_t)server % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
            rocketnet_server_transport_destroy( server );
            server_success = failure;
        }
        check( state.live == baseline );
        if ( server_success ) break;
    }
    check( server_success );
    rocketnet_term();
    rocketnet_term();
    check( state.live == 0 );
    rocketnet_allocator( refuse_allocation, refuse_free );
    check( rocketnet_malloc( NULL, 16 ) == NULL );
    rocketnet_allocator( allocate, release );
    check( rocketnet_init( &state ) );
    rocketnet_term();
    check( state.live == 0 );
    printf( "lifecycle OK: client %d, server %d allocation failure stages; aligned allocator, thread/socket refusal and init/term\n", client_success - 1, server_success - 1 );
    return 0;
}
