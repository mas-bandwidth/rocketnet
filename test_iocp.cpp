/* RocketNet. Copyright 2026 Más Bandwidth LLC. */
#include "rocketnet.h"
#include "rocketnet_platform.h"
#include "rocketnet_server_iocp.h"
#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "IOCP check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

static std::atomic<int> live_allocations{0};
static std::atomic<int> allocations{0};
static int fail_allocation = -1;

static void * allocate( void *, size_t bytes )
{
    const int index = allocations.fetch_add( 1 );
    if ( index == fail_allocation ) return NULL;
    void * p = malloc( bytes );
    if ( p ) { memset(p, 0xB5, bytes); live_allocations.fetch_add(1); }
    return p;
}

static void release( void *, void * p )
{
    if ( p ) { live_allocations.fetch_sub(1); free(p); }
}

static rocketnet_address_t address( const char * text )
{
    rocketnet_address_t result;
    CHECK( rocketnet_address_parse( &result, text ) );
    return result;
}

static void wait_stopped( rocketnet_server_iocp_t * driver )
{
    rocketnet_server_iocp_stop( driver );
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( !rocketnet_server_iocp_stopped(driver) && rocketnet_platform_time() < deadline )
        rocketnet_platform_sleep(0.001);
    CHECK( rocketnet_server_iocp_stopped(driver) );
    CHECK( rocketnet_server_iocp_pending(driver) == 0 );
}

static void test_create_cleanup()
{
    const int baseline = live_allocations.load();
    rocketnet_address_t bind = address("127.0.0.1:0");
    allocations.store(0);
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 2);
    CHECK(driver && bind.port != 0);
    const int count = allocations.load();
    wait_stopped(driver);
    rocketnet_server_iocp_destroy(driver);
    CHECK(live_allocations.load() == baseline);

    DWORD handle_baseline;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handle_baseline));
    for ( int i = 0; i < count; ++i )
    {
        bind = address("127.0.0.1:0");
        allocations.store(0);
        fail_allocation = i;
        driver = rocketnet_server_iocp_create(NULL, &bind, 2);
        fail_allocation = -1;
        CHECK(!driver);
        CHECK(live_allocations.load() == baseline);
    }
    for ( int repeat = 0; repeat < 3; ++repeat )
        for ( int stage = 1; stage <= 4; ++stage )
        {
            bind = address("127.0.0.1:0");
            CHECK(!rocketnet_server_iocp_create(NULL, &bind, 2, stage));
            CHECK(live_allocations.load() == baseline);
        }
    DWORD handles;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
    CHECK(handles <= handle_baseline + 2); // Repeated failure must not leak handles.
    CHECK(!rocketnet_server_iocp_create(NULL, &bind, ROCKETNET_IOCP_MAX_WORKERS + 1));
    CHECK(!rocketnet_server_iocp_create(NULL, &bind, -1));
    printf("test_iocp_create_cleanup\n");
}

static void test_completion_owns_frames()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(driver);
    rocketnet_address_t peer_address = address("127.0.0.1:0");
    rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, &peer_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024);
    CHECK(peer);

    // Exhaust and abort a complete phase. Every frame must be reclaimable.
    std::set<uint8_t*> frames;
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        uint64_t id;
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && ((uintptr_t)frame % 16) == 0 && frames.insert(frame).second);
        rocketnet_server_iocp_abort_packet(driver, id, frame);
    }
    uint64_t id;
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &id));
    rocketnet_server_iocp_send_packets(driver);

    // Deterministically hold retirement while the OS completes sends. These
    // buffers must remain unavailable across flushes until completion is read.
    rocketnet_server_iocp_test_hold_completions(driver, true);
    std::set<uint8_t*> pending;
    for ( int i = 0; i < 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && pending.insert(frame).second);
        memset(frame + ROCKETNET_FRAME_WIRE_OFFSET, i, 32);
        rocketnet_server_iocp_finish_packet(driver, id, frame, 32);
    }
    rocketnet_server_iocp_send_packets(driver);
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(driver, counters);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE - 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && !pending.count(frame));
        rocketnet_server_iocp_abort_packet(driver, id, frame);
    }
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &id));
    rocketnet_server_iocp_send_packets(driver);
    rocketnet_server_iocp_test_hold_completions(driver, false);
    const double deadline = rocketnet_platform_time() + 5.0;
    do
    {
        rocketnet_server_iocp_counters(driver, counters);
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] == 32 ) break;
        rocketnet_platform_sleep(0.001);
    } while ( rocketnet_platform_time() < deadline );
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] == 32);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    frames.clear();
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && frames.insert(frame).second);
        rocketnet_server_iocp_abort_packet(driver, id, frame);
    }
    rocketnet_server_iocp_send_packets(driver);
    wait_stopped(driver);
    rocketnet_server_iocp_destroy(driver);
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_completion_owns_frames\n");
}

static void test_datagrams_and_producers()
{
    rocketnet_address_t bind = address("[::]:0");
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 2);
    CHECK(driver && bind.port);
    rocketnet_address_t destination = address("127.0.0.1:0");
    destination.port = bind.port;
    rocketnet_address_t peer_address = address("127.0.0.1:0");
    rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, &peer_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024);
    CHECK(peer);
    const uint8_t marker[] = { 9, 8, 7, 6 };
    rocketnet_platform_socket_send_packet(peer, &destination, marker, sizeof(marker));
    uint8_t * received = NULL;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline && !received )
    {
        rocketnet_server_iocp_receive_packets(driver);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(driver);
        if ( batch->num_packets )
        {
            CHECK(batch->num_packets == 1);
            CHECK(batch->from[0].type == ROCKETNET_ADDRESS_IPV4 && batch->from[0].port == peer_address.port);
            CHECK(batch->packet_bytes[0] == sizeof(marker) && memcmp(batch->packet_data[0], marker, sizeof(marker)) == 0);
            received = batch->packet_data[0];
        }
        rocketnet_platform_sleep(0.001);
    }
    CHECK(received);
    uint8_t extra[32]; memset(extra, 0x44, sizeof(extra));
    for ( int i = 0; i < 100; ++i ) rocketnet_platform_socket_send_packet(peer, &destination, extra, sizeof(extra));
    rocketnet_platform_sleep(0.05);
    CHECK(memcmp(received, marker, sizeof(marker)) == 0); // View lifetime until next receive.

    std::vector<std::thread> producers;
    for ( int t = 0; t < 4; ++t )
        producers.emplace_back([=]() {
            for ( int i = 0; i < 64; ++i )
            {
                uint64_t id;
                uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
                CHECK(frame);
                memset(frame + ROCKETNET_FRAME_WIRE_OFFSET, t * 64 + i, 32);
                rocketnet_server_iocp_finish_packet(driver, id, frame, 32);
            }
        });
    for ( auto & thread : producers ) thread.join();
    rocketnet_server_iocp_send_packets(driver);
    bool seen[256] = {};
    int count = 0;
    const double send_deadline = rocketnet_platform_time() + 5.0;
    while ( count < 256 && rocketnet_platform_time() < send_deadline )
    {
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        rocketnet_address_t from;
        const int bytes = rocketnet_platform_socket_receive_packet(peer, &from, packet, sizeof(packet));
        if ( !bytes ) { rocketnet_platform_sleep(0.001); continue; }
        CHECK(bytes == 32 && from.port == bind.port && !seen[packet[0]]);
        for ( int i = 1; i < 32; ++i ) CHECK(packet[i] == packet[0]);
        seen[packet[0]] = true;
        ++count;
    }
    CHECK(count == 256);
    // Shutdown with receives still posted and no further inbound traffic.
    wait_stopped(driver);
    uint64_t stopped_id;
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &stopped_id));
    rocketnet_server_iocp_stop(driver);
    rocketnet_server_iocp_destroy(driver);
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_datagrams_and_producers\n");
}

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    rocketnet_allocator(allocate, release);
    CHECK(rocketnet_init());
    test_create_cleanup();
    test_completion_owns_frames();
    test_datagrams_and_producers();
    rocketnet_term();
    CHECK(live_allocations.load() == 0);
    return 0;
}
