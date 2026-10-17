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

    rocketnet_address_t invalid = {};
    uint64_t refused_id;
    CHECK(!rocketnet_server_iocp_start_packet(driver, &invalid, &refused_id));
    // Address refusal must leave a whole generation available below.

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
    // Counters are diagnostic snapshots, not completion fences. Join the
    // retirement batch before checking that every frame is back in the pool.
    rocketnet_server_iocp_test_hold_completions(driver, true);
    rocketnet_server_iocp_test_hold_completions(driver, false);
    // The failed pop above must not burn a staging slot. As the 32 in-flight
    // frames return, all 32 must be reservable in this SAME fill generation.
    for ( int i = 0; i < 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && pending.erase(frame) == 1);
        rocketnet_server_iocp_abort_packet(driver, id, frame);
    }
    CHECK(pending.empty());
    rocketnet_server_iocp_send_packets(driver);
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

static void test_uso_run_owns_frames()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(driver);
    if ( !(rocketnet_server_iocp_features(driver) & ROCKETNET_IOCP_FEATURE_USO) )
    {
        printf("test_iocp_uso_run_owns_frames [SKIPPED this host has no USO feature (UDP_SEND_MSG_SIZE refused)]\n");
        wait_stopped(driver);
        rocketnet_server_iocp_destroy(driver);
        return;
    }
    rocketnet_address_t peer_address = address("127.0.0.1:0");
    rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, &peer_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024);
    CHECK(peer);

    // One run of equal-size frames to one peer is one segmented post. Hold
    // retirement: no frame of the run may come back before that completion.
    const int run = 16;
    rocketnet_server_iocp_test_hold_completions(driver, true);
    std::set<uint8_t*> pending;
    uint64_t id;
    for ( int i = 0; i < run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && pending.insert(frame).second);
        memset(frame + ROCKETNET_FRAME_WIRE_OFFSET, i, 200);
        rocketnet_server_iocp_finish_packet(driver, id, frame, 200);
    }
    rocketnet_server_iocp_send_packets(driver);
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(driver, counters);
    CHECK(counters[ROCKETNET_IOCP_USO_SENDS] == 1);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] == 0);
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE - run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && !pending.count(frame));
        rocketnet_server_iocp_abort_packet(driver, id, frame);
    }
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &id));
    rocketnet_server_iocp_test_hold_completions(driver, false);
    const double deadline = rocketnet_platform_time() + 5.0;
    do
    {
        rocketnet_server_iocp_counters(driver, counters);
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] == (uint64_t) run ) break;
        rocketnet_platform_sleep(0.001);
    } while ( rocketnet_platform_time() < deadline );
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] == (uint64_t) run);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    rocketnet_server_iocp_test_hold_completions(driver, true);
    rocketnet_server_iocp_test_hold_completions(driver, false);
    // The one completion returned every frame of the run, and only those.
    for ( int i = 0; i < run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, &peer_address, &id);
        CHECK(frame && pending.erase(frame) == 1);
        rocketnet_server_iocp_abort_packet(driver, id, frame);
    }
    CHECK(pending.empty());
    rocketnet_server_iocp_send_packets(driver);
    wait_stopped(driver);
    rocketnet_server_iocp_destroy(driver);
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_uso_run_owns_frames\n");
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
    CHECK(rocketnet_server_iocp_test_receive_view_published(driver, received));
    uint64_t before[ROCKETNET_IOCP_NUM_COUNTERS], after[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(driver, before);
    uint8_t extra[32]; memset(extra, 0x44, sizeof(extra));
    for ( int i = 0; i < 100; ++i ) rocketnet_platform_socket_send_packet(peer, &destination, extra, sizeof(extra));
    const double receive_deadline = rocketnet_platform_time() + 5.0;
    do
    {
        rocketnet_server_iocp_counters(driver, after);
        if (after[ROCKETNET_IOCP_RECEIVE_COMPLETED] > before[ROCKETNET_IOCP_RECEIVE_COMPLETED]) break;
        rocketnet_platform_sleep(0.001);
    } while (rocketnet_platform_time() < receive_deadline);
    CHECK(after[ROCKETNET_IOCP_RECEIVE_COMPLETED] > before[ROCKETNET_IOCP_RECEIVE_COMPLETED]);
    CHECK(rocketnet_server_iocp_test_receive_view_published(driver, received));
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
    // Force stop to join the same mutex used by completion/repost. The stop
    // thread sets stopping while the publication locks are held here; only
    // releasing them can let cancellation proceed. No scheduler warm-up sleep.
    rocketnet_server_iocp_test_hold_completions(driver, true);
    std::atomic<bool> sending{true};
    std::thread incoming([&]() {
        while (sending.load())
            rocketnet_platform_socket_send_packet(peer, &destination, extra, sizeof(extra));
    });
    std::atomic<bool> stop_returned{false};
    std::thread stopping([&]() {
        rocketnet_server_iocp_stop(driver);
        stop_returned.store(true);
    });
    const double stop_deadline = rocketnet_platform_time() + 5.0;
    while (!rocketnet_server_iocp_test_stopping(driver) && rocketnet_platform_time() < stop_deadline)
        rocketnet_platform_sleep(0.001);
    CHECK(rocketnet_server_iocp_test_stopping(driver));
    CHECK(!stop_returned.load());
    rocketnet_server_iocp_test_hold_completions(driver, false);
    stopping.join();
    CHECK(stop_returned.load());
    wait_stopped(driver);
    sending.store(false);
    incoming.join();
    uint64_t stopped_id;
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &stopped_id));
    rocketnet_server_iocp_stop(driver);
    rocketnet_server_iocp_destroy(driver);
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_datagrams_and_producers\n");
}

static void test_receive_boundaries()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(driver);
    SOCKET peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(peer != INVALID_SOCKET);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(bind.port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char oversized[ROCKETNET_RECEIVE_CEILING + 64] = {};
    CHECK(sendto(peer, oversized, 0, 0, (sockaddr*)&to, sizeof(to)) == 0);
    CHECK(sendto(peer, oversized, sizeof(oversized), 0, (sockaddr*)&to, sizeof(to)) == sizeof(oversized));
    CHECK(sendto(peer, "good", 4, 0, (sockaddr*)&to, sizeof(to)) == 4);
    int received = 0;
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS] = {};
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets(driver);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(driver);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            CHECK(batch->packet_bytes[i] == 4 && memcmp(batch->packet_data[i], "good", 4) == 0);
            ++received;
        }
        rocketnet_server_iocp_counters(driver, counters);
        if ( received == 1 && counters[ROCKETNET_IOCP_RECEIVE_DROPPED] == 2 &&
             rocketnet_server_iocp_pending(driver) == ROCKETNET_IOCP_RECEIVES_PER_WORKER ) break;
        rocketnet_platform_sleep(0.001);
    }
    CHECK(received == 1 && counters[ROCKETNET_IOCP_RECEIVE_DROPPED] == 2);
    CHECK(rocketnet_server_iocp_pending(driver) == ROCKETNET_IOCP_RECEIVES_PER_WORKER);
    wait_stopped(driver); // Cancellation with idle, outstanding receives too.
    rocketnet_server_iocp_destroy(driver);
    closesocket(peer);
    printf("test_iocp_receive_boundaries\n");
}

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    rocketnet_allocator(allocate, release);
    CHECK(rocketnet_init());
    test_create_cleanup();
    test_completion_owns_frames();
    test_uso_run_owns_frames();
    test_datagrams_and_producers();
    test_receive_boundaries();
    rocketnet_term();
    CHECK(live_allocations.load() == 0);
    return 0;
}
