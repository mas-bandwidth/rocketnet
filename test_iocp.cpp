/* RocketNet. Copyright 2026 Más Bandwidth LLC. */
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_server_iocp.h"
#include "rocketnet_server_rio.h"
#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                  int * repost_calls, int * pending_receives, int * guard_hit );
#endif

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

// ---------------------------------------------------------------------------
// Fault cases: each failure path of the IOCP and RIO drivers, injected
// through the test-only fault seams (ROCKETNET_IOCP_TESTS, ROCKETNET_RIO_TESTS)
// on a live loopback exchange. Each checks that every frame or slot returns,
// the counter moves, and traffic in both directions resumes after the fault.

#define FAULT_BYTES 100
static_assert( FAULT_BYTES >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, "the RIO receive keeps the datagram" );

static int fault_round = 0;

static rocketnet_platform_socket_t * fault_peer( rocketnet_address_t * peer_address )
{
    *peer_address = address("127.0.0.1:0");
    rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, peer_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024);
    CHECK(peer);
    return peer;
}

static void fault_mark( uint8_t * data, int round, int index )
{
    data[0] = 0xA5;
    data[1] = (uint8_t) round;
    data[2] = (uint8_t) ( index >> 8 );
    data[3] = (uint8_t) index;
    for ( int i = 4; i < FAULT_BYTES; ++i ) data[i] = (uint8_t) ( index + i );
}

// the index a marked datagram of this round carries, or -1
static int fault_index( const uint8_t * data, int bytes, int round )
{
    if ( bytes != FAULT_BYTES || data[0] != 0xA5 || data[1] != (uint8_t) round ) return -1;
    const int index = ( data[2] << 8 ) | data[3];
    for ( int i = 4; i < FAULT_BYTES; ++i ) if ( data[i] != (uint8_t) ( index + i ) ) return -1;
    return index;
}

// distinct marked datagrams of this round the peer receives, up to n
static int fault_peer_receive( rocketnet_platform_socket_t * peer, int round, int n )
{
    std::vector<bool> seen( (size_t) n, false );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        rocketnet_address_t from;
        const int bytes = rocketnet_platform_socket_receive_packet(peer, &from, packet, sizeof(packet));
        if ( !bytes ) { rocketnet_platform_sleep(0.001); continue; }
        const int index = fault_index(packet, bytes, round);
        if ( index >= 0 && index < n && !seen[index] ) { seen[index] = true; ++count; }
    }
    return count;
}

static void fault_peer_send( rocketnet_platform_socket_t * peer, const rocketnet_address_t * to, int round, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        uint8_t data[FAULT_BYTES];
        fault_mark(data, round, i);
        rocketnet_platform_socket_send_packet(peer, to, data, FAULT_BYTES);
    }
}

// ---- IOCP ----

static void iocp_stage( rocketnet_server_iocp_t * driver, const rocketnet_address_t * to, int round, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        uint64_t id;
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, to, &id);
        CHECK(frame);
        fault_mark(frame + ROCKETNET_FRAME_WIRE_OFFSET, round, i);
        rocketnet_server_iocp_finish_packet(driver, id, frame, FAULT_BYTES);
    }
}

// waits until sends completed plus sends failed reach total, then joins the
// retirement batch so every retired frame is back in the pool
static void iocp_wait_sends( rocketnet_server_iocp_t * driver, uint64_t total )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    const double deadline = rocketnet_platform_time() + 10.0;
    do
    {
        rocketnet_server_iocp_counters(driver, counters);
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED] >= total ) break;
        rocketnet_platform_sleep(0.001);
    } while ( rocketnet_platform_time() < deadline );
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED] == total);
    rocketnet_server_iocp_test_hold_completions(driver, true);
    rocketnet_server_iocp_test_hold_completions(driver, false);
}

static uint64_t iocp_sends_retired( rocketnet_server_iocp_t * driver )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(driver, counters);
    return counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED];
}

static uint64_t iocp_counter( rocketnet_server_iocp_t * driver, int counter )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(driver, counters);
    return counters[counter];
}

// every frame of the pool is free: a whole generation reserves, each frame once
static void iocp_all_frames_free( rocketnet_server_iocp_t * driver, const rocketnet_address_t * to )
{
    std::set<uint8_t*> frames;
    uint64_t id;
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(driver, to, &id);
        CHECK(frame && frames.insert(frame).second);
        rocketnet_server_iocp_abort_packet(driver, id, frame);
    }
    CHECK(!rocketnet_server_iocp_start_packet(driver, to, &id));
    rocketnet_server_iocp_send_packets(driver);
}

// the receive depth is back to its configured count, with no send in flight
static void iocp_wait_depth( rocketnet_server_iocp_t * driver, int workers )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_server_iocp_pending(driver) != workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER && rocketnet_platform_time() < deadline )
        rocketnet_platform_sleep(0.001);
    CHECK(rocketnet_server_iocp_pending(driver) == workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER);
}

// n datagrams each way, every one delivered: the exchange lives
static void iocp_exchange( rocketnet_server_iocp_t * driver, const rocketnet_address_t * bind,
    rocketnet_platform_socket_t * peer, const rocketnet_address_t * peer_address, int n )
{
    const int round = ++fault_round & 0xFF;
    const uint64_t retired = iocp_sends_retired(driver);
    iocp_stage(driver, peer_address, round, n);
    rocketnet_server_iocp_send_packets(driver);
    CHECK(fault_peer_receive(peer, round, n) == n);
    iocp_wait_sends(driver, retired + (uint64_t) n);
    fault_peer_send(peer, bind, round, n);
    std::vector<bool> seen( (size_t) n, false );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets(driver);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(driver);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index(batch->packet_data[i], batch->packet_bytes[i], round);
            if ( index >= 0 && index < n && !seen[index] ) { seen[index] = true; ++count; }
        }
        rocketnet_platform_sleep(0.001);
    }
    CHECK(count == n);
}

static void iocp_finish( rocketnet_server_iocp_t * driver, rocketnet_platform_socket_t * peer )
{
    wait_stopped(driver);
    rocketnet_server_iocp_destroy(driver);
    rocketnet_platform_socket_destroy(peer);
}

static void test_iocp_send_completion_fails()
{
    // Per-datagram sends: one completion is one frame.
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 1, 0, ROCKETNET_IOCP_DISABLE_USO);
    CHECK(driver);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    iocp_exchange(driver, &bind, peer, &peer_address, 16);
    const int statuses[] = { WSAENOBUFS, ERROR_OPERATION_ABORTED };
    const uint64_t failed_before = iocp_counter(driver, ROCKETNET_IOCP_SEND_FAILED);
    for ( int s = 0; s < 2; ++s )
    {
        const uint64_t retired = iocp_sends_retired(driver);
        rocketnet_server_iocp_test_fail_next(driver, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION, 50, statuses[s]);
        iocp_stage(driver, &peer_address, 0xEE, 50);
        rocketnet_server_iocp_send_packets(driver);
        iocp_wait_sends(driver, retired + 50);
        CHECK(rocketnet_server_iocp_test_faults_left(driver, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION) == 0);
        iocp_all_frames_free(driver, &peer_address);
        iocp_exchange(driver, &bind, peer, &peer_address, 16);
    }
    CHECK(iocp_counter(driver, ROCKETNET_IOCP_SEND_FAILED) - failed_before == 100);
    iocp_wait_depth(driver, 1);
    iocp_finish(driver, peer);

    // A USO run: one failed completion returns every frame of the run.
    bind = address("127.0.0.1:0");
    driver = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(driver);
    peer = fault_peer(&peer_address);
    if ( rocketnet_server_iocp_features(driver) & ROCKETNET_IOCP_FEATURE_USO )
    {
        const uint64_t retired = iocp_sends_retired(driver);
        rocketnet_server_iocp_test_fail_next(driver, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION, 1, WSAENOBUFS);
        iocp_stage(driver, &peer_address, 0xEE, 16);
        rocketnet_server_iocp_send_packets(driver);
        CHECK(iocp_counter(driver, ROCKETNET_IOCP_USO_SENDS) == 1);
        iocp_wait_sends(driver, retired + 16);
        CHECK(iocp_counter(driver, ROCKETNET_IOCP_SEND_FAILED) == 16);
        iocp_all_frames_free(driver, &peer_address);
    }
    iocp_exchange(driver, &bind, peer, &peer_address, 16);
    iocp_finish(driver, peer);
    printf("iocp_send_completion_fails\n");
}

static void test_iocp_send_post_fails()
{
    // A refused post has no completion: the frame returns at the post.
    for ( int uso = 0; uso < 2; ++uso )
    {
        rocketnet_address_t bind = address("127.0.0.1:0");
        rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 1, 0, uso ? 0 : ROCKETNET_IOCP_DISABLE_USO);
        CHECK(driver);
        rocketnet_address_t peer_address;
        rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
        const bool runs = uso && ( rocketnet_server_iocp_features(driver) & ROCKETNET_IOCP_FEATURE_USO );
        if ( uso && !runs )
        {
            iocp_finish(driver, peer);
            continue;
        }
        // per datagram: 100 refused posts of one frame; with USO: 2 refused
        // runs of 16 equal frames
        const int posts = runs ? 2 : 100;
        const int frames = runs ? 32 : 100;
        rocketnet_server_iocp_test_fail_next(driver, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, posts, WSAENOBUFS);
        for ( int r = 0; r < ( runs ? 2 : 1 ); ++r )
        {
            iocp_stage(driver, &peer_address, 0xEE, runs ? 16 : frames);
            rocketnet_server_iocp_send_packets(driver);
        }
        CHECK(rocketnet_server_iocp_test_faults_left(driver, ROCKETNET_IOCP_TEST_FAULT_SEND_POST) == 0);
        CHECK(iocp_counter(driver, ROCKETNET_IOCP_SEND_FAILED) == (uint64_t) frames);
        CHECK(iocp_counter(driver, ROCKETNET_IOCP_SEND_COMPLETED) == 0);
        CHECK(rocketnet_server_iocp_pending(driver) == ROCKETNET_IOCP_RECEIVES_PER_WORKER);
        iocp_all_frames_free(driver, &peer_address);
        iocp_exchange(driver, &bind, peer, &peer_address, 32);
        iocp_finish(driver, peer);
    }
    printf("iocp_send_post_fails\n");
}

static void test_iocp_receive_completion_fails()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 1, 0, ROCKETNET_IOCP_DISABLE_URO);
    CHECK(driver);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    iocp_exchange(driver, &bind, peer, &peer_address, 16);

    // 100 failed receive completions: each is counted dropped and its
    // operation is re-posted, so the depth holds.
    const int statuses[] = { WSAENOBUFS, ERROR_OPERATION_ABORTED };
    for ( int s = 0; s < 2; ++s )
    {
        const uint64_t dropped = iocp_counter(driver, ROCKETNET_IOCP_RECEIVE_DROPPED);
        rocketnet_server_iocp_test_fail_next(driver, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION, 50, statuses[s]);
        fault_peer_send(peer, &bind, 0xEE, 50);
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( iocp_counter(driver, ROCKETNET_IOCP_RECEIVE_DROPPED) - dropped < 50 && rocketnet_platform_time() < deadline )
            rocketnet_platform_sleep(0.001);
        CHECK(iocp_counter(driver, ROCKETNET_IOCP_RECEIVE_DROPPED) - dropped == 50);
        CHECK(rocketnet_server_iocp_test_faults_left(driver, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION) == 0);
        iocp_wait_depth(driver, 1);
        iocp_exchange(driver, &bind, peer, &peer_address, 16);
    }

    // Refused re-posts: each completion delivers its datagram, its re-post is
    // refused and counted, and the retry list re-posts it once the refusals
    // end: the depth recovers to its configured count.
    const uint64_t repost_failed = iocp_counter(driver, ROCKETNET_IOCP_RECEIVE_REPOST_FAILED);
    rocketnet_server_iocp_test_fail_next(driver, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST, 300, WSAENOBUFS);
    const int round = ++fault_round & 0xFF;
    fault_peer_send(peer, &bind, round, 64);
    std::vector<bool> seen( 64, false );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( ( count < 64 || rocketnet_server_iocp_test_faults_left(driver, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST) > 0 ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets(driver);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(driver);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index(batch->packet_data[i], batch->packet_bytes[i], round);
            if ( index >= 0 && index < 64 && !seen[index] ) { seen[index] = true; ++count; }
        }
        rocketnet_platform_sleep(0.001);
    }
    CHECK(count == 64);
    CHECK(rocketnet_server_iocp_test_faults_left(driver, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST) == 0);
    iocp_wait_depth(driver, 1);
    CHECK(iocp_counter(driver, ROCKETNET_IOCP_RECEIVE_REPOST_FAILED) - repost_failed == 300);
    iocp_exchange(driver, &bind, peer, &peer_address, 64);
    iocp_finish(driver, peer);
    printf("iocp_receive_completion_fails\n");
}

static void test_iocp_send_queue_full_recovers()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * driver = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(driver);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    iocp_exchange(driver, &bind, peer, &peer_address, 16);

    // Held completions: the whole queue is staged and posted, and nothing
    // can retire it. start_packet refuses until the completions are read.
    const uint64_t retired = iocp_sends_retired(driver);
    rocketnet_server_iocp_test_hold_completions(driver, true);
    iocp_stage(driver, &peer_address, 0xEE, ROCKETNET_IOCP_SEND_QUEUE_SIZE);
    uint64_t id;
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &id));
    rocketnet_server_iocp_send_packets(driver);
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &id));
    CHECK(rocketnet_server_iocp_pending(driver) > ROCKETNET_IOCP_RECEIVES_PER_WORKER ||
          iocp_counter(driver, ROCKETNET_IOCP_SEND_FAILED) > 0);
    rocketnet_server_iocp_send_packets(driver);
    CHECK(!rocketnet_server_iocp_start_packet(driver, &peer_address, &id));
    rocketnet_server_iocp_test_hold_completions(driver, false);

    // Released: every send drains, every frame is free, the exchange lives.
    iocp_wait_sends(driver, retired + ROCKETNET_IOCP_SEND_QUEUE_SIZE);
    iocp_wait_depth(driver, 1);
    iocp_all_frames_free(driver, &peer_address);
    iocp_exchange(driver, &bind, peer, &peer_address, 64);
    printf("iocp_send_queue_full_recovers [send failed %llu of %d]\n",
        (unsigned long long) iocp_counter(driver, ROCKETNET_IOCP_SEND_FAILED), ROCKETNET_IOCP_SEND_QUEUE_SIZE);
    iocp_finish(driver, peer);
}

// ---- RIO ----

static uint64_t rio_counter( rocketnet_server_rio_t * rio, int counter )
{
    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters(rio, counters);
    return counters[counter];
}

static void rio_stage( rocketnet_server_rio_t * rio, const rocketnet_address_t * to, int round, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        uint64_t id;
        uint8_t * frame = rocketnet_server_rio_start_packet(rio, to, &id);
        CHECK(frame);
        fault_mark(frame + ROCKETNET_FRAME_WIRE_OFFSET, round, i);
        rocketnet_server_rio_finish_packet(rio, id, frame, FAULT_BYTES);
    }
}

// flushes until no send is outstanding and every receive slice is posted
static void rio_settle( rocketnet_server_rio_t * rio )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( ( rocketnet_server_rio_pending(rio) != ROCKETNET_RIO_RECEIVES || rocketnet_server_rio_test_retry_receives(rio) != 0 ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_send_packets(rio);
        rocketnet_server_rio_receive_packets(rio);
        rocketnet_platform_sleep(0.001);
    }
    CHECK(rocketnet_server_rio_pending(rio) == ROCKETNET_RIO_RECEIVES);
    CHECK(rocketnet_server_rio_test_retry_receives(rio) == 0);
}

// every slot of both phases reserves: no phase is held by a send that never
// retires
static void rio_all_slots_free( rocketnet_server_rio_t * rio, const rocketnet_address_t * to )
{
    for ( int cycle = 0; cycle < 2; ++cycle )
    {
        uint64_t id;
        for ( int i = 0; i < ROCKETNET_RIO_SEND_QUEUE_SIZE; ++i )
        {
            uint8_t * frame = rocketnet_server_rio_start_packet(rio, to, &id);
            CHECK(frame);
            rocketnet_server_rio_abort_packet(rio, id, frame);
        }
        CHECK(!rocketnet_server_rio_start_packet(rio, to, &id));
        rocketnet_server_rio_send_packets(rio);
    }
}

static void rio_exchange( rocketnet_server_rio_t * rio, const rocketnet_address_t * bind,
    rocketnet_platform_socket_t * peer, const rocketnet_address_t * peer_address, int n )
{
    const int round = ++fault_round & 0xFF;
    rio_stage(rio, peer_address, round, n);
    rocketnet_server_rio_send_packets(rio);
    CHECK(fault_peer_receive(peer, round, n) == n);
    fault_peer_send(peer, bind, round, n);
    std::vector<bool> seen( (size_t) n, false );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets(rio);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_rio_process_packets(rio);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index(batch->packet_data[i], batch->packet_bytes[i], round);
            if ( index >= 0 && index < n && !seen[index] ) { seen[index] = true; ++count; }
        }
        rocketnet_platform_sleep(0.001);
    }
    CHECK(count == n);
    rio_settle(rio);
}

static rocketnet_server_rio_t * rio_open( rocketnet_address_t * bind )
{
    *bind = address("127.0.0.1:0");
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create(NULL, bind);
    CHECK(rio && bind->port);
    return rio;
}

static void rio_finish( rocketnet_server_rio_t * rio, rocketnet_platform_socket_t * peer )
{
    rocketnet_server_rio_destroy(rio);
    CHECK(rocketnet_server_rio_leaked_regions() == 0);
    rocketnet_platform_socket_destroy(peer);
}

static void test_rio_send_post_fails()
{
    if ( !rocketnet_server_rio_available() ) { printf("rio_send_post_fails [SKIPPED no RIO on this host]\n"); return; }
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open(&bind);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    rio_exchange(rio, &bind, peer, &peer_address, 16);

    // 100 refused RIOSendEx posts of 150: each refused slot is free at once,
    // the other 50 are sent.
    const uint64_t failed = rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED);
    const uint64_t sent = rio_counter(rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT);
    rocketnet_server_rio_test_fail_next(rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST, 100, WSAENOBUFS);
    rio_stage(rio, &peer_address, 0xEE, 150);
    rocketnet_server_rio_send_packets(rio);
    CHECK(rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST) == 0);
    CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED) - failed == 100);
    rio_settle(rio);
    CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT) - sent == 50);
    rio_all_slots_free(rio, &peer_address);
    rio_exchange(rio, &bind, peer, &peer_address, 32);

    // 100 failed send completions: each retires its slot and counts.
    const uint64_t failed2 = rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED);
    rocketnet_server_rio_test_fail_next(rio, ROCKETNET_RIO_TEST_FAULT_SEND_COMPLETION, 100, WSAENOBUFS);
    rio_stage(rio, &peer_address, 0xEE, 100);
    rocketnet_server_rio_send_packets(rio);
    rio_settle(rio);
    CHECK(rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_SEND_COMPLETION) == 0);
    CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED) - failed2 == 100);
    rio_all_slots_free(rio, &peer_address);

    // Refused commits leave the deferred sends for the next flush.
    const int round = ++fault_round & 0xFF;
    rocketnet_server_rio_test_fail_next(rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, 3, WSAENOBUFS);
    rio_stage(rio, &peer_address, round, 10);
    for ( int i = 0; i < 4; ++i ) rocketnet_server_rio_send_packets(rio);
    CHECK(rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_COMMIT) == 0);
    CHECK(fault_peer_receive(peer, round, 10) == 10);
    rio_settle(rio);
    rio_exchange(rio, &bind, peer, &peer_address, 32);
    rio_finish(rio, peer);
    printf("rio_send_post_fails\n");
}

static void test_rio_request_queue_full()
{
    if ( !rocketnet_server_rio_available() ) { printf("rio_request_queue_full [SKIPPED no RIO on this host]\n"); return; }
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open(&bind);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    rio_exchange(rio, &bind, peer, &peer_address, 16);

    // The request queue refuses with WSAENOBUFS, as a full queue does: 200
    // receive re-posts and 200 send posts. Every datagram that completed is
    // delivered, no slice is lost, no slot is held.
    const uint64_t repost_failed = rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED);
    const uint64_t failed = rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED);
    rocketnet_server_rio_test_fail_next(rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST, 200, WSAENOBUFS);
    rocketnet_server_rio_test_fail_next(rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST, 200, WSAENOBUFS);
    const int round = ++fault_round & 0xFF;
    fault_peer_send(peer, &bind, round, 100);
    std::vector<bool> seen( 100, false );
    int count = 0;
    double deadline = rocketnet_platform_time() + 5.0;
    while ( ( count < 100 || rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST) > 0 ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets(rio);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_rio_process_packets(rio);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index(batch->packet_data[i], batch->packet_bytes[i], round);
            if ( index >= 0 && index < 100 && !seen[index] ) { seen[index] = true; ++count; }
        }
        rocketnet_platform_sleep(0.001);
    }
    CHECK(count == 100);
    CHECK(rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST) == 0);
    rio_stage(rio, &peer_address, 0xEE, 300);
    rocketnet_server_rio_send_packets(rio);
    CHECK(rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST) == 0);
    CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED) - failed == 200);
    rio_settle(rio);
    CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED) - repost_failed == 200);
    rio_all_slots_free(rio, &peer_address);

    // Failed receive completions: counted, and the slice is re-posted.
    const uint64_t receive_failed = rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED);
    rocketnet_server_rio_test_fail_next(rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_COMPLETION, 50, WSAENOBUFS);
    fault_peer_send(peer, &bind, 0xEE, 50);
    deadline = rocketnet_platform_time() + 5.0;
    while ( rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED) - receive_failed < 50 && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets(rio);
        rocketnet_platform_sleep(0.001);
    }
    CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED) - receive_failed == 50);
    rio_settle(rio);
    rio_exchange(rio, &bind, peer, &peer_address, 64);
    rio_finish(rio, peer);
    printf("rio_request_queue_full\n");
}

static void test_rio_completion_queue_full()
{
#if defined(_WIN32)
    // Keep the CQ full through a guard beyond two banks. The old unbounded
    // loop reaches the corrupt-CQ sentinel; bounded receive returns at one
    // bank and leaves the still-full queue for the next update.
    uint64_t seam_counters[2 * ROCKETNET_RIO_NUM_COUNTERS] = {};
    int seam_packets[2] = {};
    int seam_reposts = 0;
    int seam_pending = 0;
    int seam_guard_hit = 0;
    CHECK( rocketnet_server_rio_test_receive_overflow( seam_counters, seam_packets, &seam_reposts, &seam_pending, &seam_guard_hit ) );
    CHECK( seam_guard_hit == 0 );
    CHECK( seam_packets[0] == ROCKETNET_RIO_RECEIVES );
    CHECK( seam_packets[1] == ROCKETNET_RIO_RECEIVES );
    CHECK( seam_counters[ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] == ROCKETNET_RIO_RECEIVES );
    CHECK( seam_counters[ROCKETNET_RIO_NUM_COUNTERS + ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] == 2 * ROCKETNET_RIO_RECEIVES );
    CHECK( seam_reposts == 2 * ROCKETNET_RIO_RECEIVES );
    CHECK( seam_pending == ROCKETNET_RIO_RECEIVES );
#endif
    if ( !rocketnet_server_rio_available() ) { printf("rio_completion_queue_full [SKIPPED no RIO on this host]\n"); return; }
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open(&bind);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    rio_exchange(rio, &bind, peer, &peer_address, 16);

    // Unpolled, every slice completes and the receive completion queue fills
    // with ROCKETNET_RIO_RECEIVES completions; the host drops what arrives
    // with no slice posted. The poll that reads the full queue fills the bank
    // to capacity and re-posts slice by slice while a sender keeps datagrams
    // coming, so completions past the bank capacity arrive in the same poll
    // and the overflow is counted. No slice is lost and traffic resumes.
    const uint64_t overflow = rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW);
    int attempts = 0;
    while ( rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW) == overflow && attempts < 10 )
    {
        ++attempts;
        const uint64_t received = rio_counter(rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED);
        fault_peer_send(peer, &bind, 0xEE, ROCKETNET_RIO_RECEIVES + 256);
        rocketnet_platform_sleep(0.1);
        std::atomic<bool> flooding{true};
        std::thread flood([&]() { while ( flooding.load() ) fault_peer_send(peer, &bind, 0xEE, 16); });
        rocketnet_platform_sleep(0.01);
        rocketnet_server_rio_receive_packets(rio);
        flooding.store(false);
        flood.join();
        CHECK(rocketnet_server_rio_process_packets(rio)->num_packets == ROCKETNET_RIO_RECEIVES);
        CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED) - received == ROCKETNET_RIO_RECEIVES);
        rio_settle(rio);
        // drain what the sender left before the next attempt
        for ( int i = 0; i < 20; ++i ) { rocketnet_server_rio_receive_packets(rio); rocketnet_platform_sleep(0.005); }
    }
    const uint64_t dropped = rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW) - overflow;
    printf("rio_completion_queue_full: overflow %llu after %d attempts\n", (unsigned long long) dropped, attempts);
    CHECK(dropped > 0);
    rio_settle(rio);
    rio_exchange(rio, &bind, peer, &peer_address, 64);
    rio_finish(rio, peer);
    printf("rio_completion_queue_full\n");
}

struct fault_case_t { const char * name; void (*run)(); };

static const fault_case_t fault_cases[] = {
    { "iocp_send_completion_fails", test_iocp_send_completion_fails },
    { "iocp_send_post_fails", test_iocp_send_post_fails },
    { "iocp_receive_completion_fails", test_iocp_receive_completion_fails },
    { "iocp_send_queue_full_recovers", test_iocp_send_queue_full_recovers },
    { "rio_completion_queue_full", test_rio_completion_queue_full },
    { "rio_request_queue_full", test_rio_request_queue_full },
    { "rio_send_post_fails", test_rio_send_post_fails },
};

// With no argument every case runs; with names, only those cases run.
int main( int argc, char ** argv )
{
    rocketnet_set_log_level(ROCKETNET_LOG_LEVEL_NONE);
    rocketnet_set_allocator(allocate, release);
    CHECK(rocketnet_init( NULL ) == ROCKETNET_STATUS_OK);
    if ( argc < 2 )
    {
        test_create_cleanup();
        test_completion_owns_frames();
        test_uso_run_owns_frames();
        test_datagrams_and_producers();
        test_receive_boundaries();
        for ( const fault_case_t & c : fault_cases ) c.run();
    }
    for ( int i = 1; i < argc; ++i )
    {
        bool found = false;
        for ( const fault_case_t & c : fault_cases )
            if ( strcmp( argv[i], c.name ) == 0 ) { c.run(); found = true; }
        CHECK(found);
    }
    rocketnet_term();
    CHECK(live_allocations.load() == 0);
    return 0;
}
