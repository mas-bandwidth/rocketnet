/* RocketNet. Copyright 2026 Más Bandwidth LLC. */
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_server_iocp.h"
#include "rocketnet_server_rio.h"
#include <ws2tcpip.h>
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

static void wait_stopped( rocketnet_server_iocp_t * transport )
{
    rocketnet_server_iocp_stop( transport );
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( !rocketnet_server_iocp_stopped(transport) && rocketnet_platform_time() < deadline )
        rocketnet_platform_sleep(0.001);
    CHECK( rocketnet_server_iocp_stopped(transport) );
    CHECK( rocketnet_server_iocp_pending(transport) == 0 );
}

static void test_create_cleanup()
{
    const int baseline = live_allocations.load();
    rocketnet_address_t bind = address("127.0.0.1:0");
    allocations.store(0);
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 2);
    CHECK(transport && bind.port != 0);
    const int count = allocations.load();
    wait_stopped(transport);
    rocketnet_server_iocp_destroy(transport);
    CHECK(live_allocations.load() == baseline);

    DWORD handle_baseline;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handle_baseline));
    for ( int i = 0; i < count; ++i )
    {
        bind = address("127.0.0.1:0");
        allocations.store(0);
        fail_allocation = i;
        transport = rocketnet_server_iocp_create(NULL, &bind, 2);
        fail_allocation = -1;
        CHECK(!transport);
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
    printf("test_iocp_transport_create_cleanup\n");
}

static void test_completion_owns_frames()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(transport);
    rocketnet_address_t peer_address = address("127.0.0.1:0");
    rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, &peer_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024);
    CHECK(peer);

    rocketnet_address_t invalid = {};
    uint64_t refused_id;
    CHECK(!rocketnet_server_iocp_start_packet(transport, &invalid, &refused_id));
    // Address refusal must leave a whole generation available below.

    // Exhaust and abort a complete phase. Every frame must be reclaimable.
    std::set<uint8_t*> frames;
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        uint64_t id;
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && ((uintptr_t)frame % 16) == 0 && frames.insert(frame).second);
        rocketnet_server_iocp_abort_packet(transport, id, frame);
    }
    uint64_t id;
    CHECK(!rocketnet_server_iocp_start_packet(transport, &peer_address, &id));
    rocketnet_server_iocp_send_packets(transport);

    // Deterministically hold retirement while the OS completes sends. These
    // buffers must remain unavailable across flushes until completion is read.
    rocketnet_server_iocp_test_hold_completions(transport, true);
    std::set<uint8_t*> pending;
    for ( int i = 0; i < 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && pending.insert(frame).second);
        memset(frame + ROCKETNET_FRAME_WIRE_OFFSET, i, 32);
        rocketnet_server_iocp_finish_packet(transport, id, frame, 32);
    }
    rocketnet_server_iocp_send_packets(transport);
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(transport, counters);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE - 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && !pending.count(frame));
        rocketnet_server_iocp_abort_packet(transport, id, frame);
    }
    CHECK(!rocketnet_server_iocp_start_packet(transport, &peer_address, &id));
    rocketnet_server_iocp_test_hold_completions(transport, false);
    const double deadline = rocketnet_platform_time() + 5.0;
    do
    {
        rocketnet_server_iocp_counters(transport, counters);
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] == 32 ) break;
        rocketnet_platform_sleep(0.001);
    } while ( rocketnet_platform_time() < deadline );
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] == 32);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    // Counters are diagnostic snapshots, not completion fences. Join the
    // retirement batch before checking that every frame is back in the pool.
    rocketnet_server_iocp_test_hold_completions(transport, true);
    rocketnet_server_iocp_test_hold_completions(transport, false);
    // The failed pop above must not burn a staging slot. As the 32 in-flight
    // frames return, all 32 must be reservable in this SAME fill generation.
    for ( int i = 0; i < 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && pending.erase(frame) == 1);
        rocketnet_server_iocp_abort_packet(transport, id, frame);
    }
    CHECK(pending.empty());
    rocketnet_server_iocp_send_packets(transport);
    frames.clear();
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && frames.insert(frame).second);
        rocketnet_server_iocp_abort_packet(transport, id, frame);
    }
    rocketnet_server_iocp_send_packets(transport);
    wait_stopped(transport);
    rocketnet_server_iocp_destroy(transport);
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_transport_completion_owns_frames\n");
}

static void test_uso_run_owns_frames()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(transport);
    if ( !(rocketnet_server_iocp_features(transport) & ROCKETNET_IOCP_FEATURE_USO) )
    {
        wait_stopped(transport);
        rocketnet_server_iocp_destroy(transport);
        return;
    }
    rocketnet_address_t peer_address = address("127.0.0.1:0");
    rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, &peer_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024);
    CHECK(peer);

    // One run of equal-size frames to one peer is one segmented post. Hold
    // retirement: no frame of the run may come back before that completion.
    const int run = 16;
    rocketnet_server_iocp_test_hold_completions(transport, true);
    std::set<uint8_t*> pending;
    uint64_t id;
    for ( int i = 0; i < run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && pending.insert(frame).second);
        memset(frame + ROCKETNET_FRAME_WIRE_OFFSET, i, 200);
        rocketnet_server_iocp_finish_packet(transport, id, frame, 200);
    }
    rocketnet_server_iocp_send_packets(transport);
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(transport, counters);
    CHECK(counters[ROCKETNET_IOCP_USO_SENDS] == 1);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] == 0);
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE - run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && !pending.count(frame));
        rocketnet_server_iocp_abort_packet(transport, id, frame);
    }
    CHECK(!rocketnet_server_iocp_start_packet(transport, &peer_address, &id));
    rocketnet_server_iocp_test_hold_completions(transport, false);
    const double deadline = rocketnet_platform_time() + 5.0;
    do
    {
        rocketnet_server_iocp_counters(transport, counters);
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] == (uint64_t) run ) break;
        rocketnet_platform_sleep(0.001);
    } while ( rocketnet_platform_time() < deadline );
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] == (uint64_t) run);
    CHECK(counters[ROCKETNET_IOCP_SEND_FAILED] == 0);
    rocketnet_server_iocp_test_hold_completions(transport, true);
    rocketnet_server_iocp_test_hold_completions(transport, false);
    // The one completion returned every frame of the run, and only those.
    for ( int i = 0; i < run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
        CHECK(frame && pending.erase(frame) == 1);
        rocketnet_server_iocp_abort_packet(transport, id, frame);
    }
    CHECK(pending.empty());
    rocketnet_server_iocp_send_packets(transport);
    wait_stopped(transport);
    rocketnet_server_iocp_destroy(transport);
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_transport_uso_run_owns_frames\n");
}

static void test_datagrams_and_producers()
{
    rocketnet_address_t bind = address("[::]:0");
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 2);
    CHECK(transport && bind.port);
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
        rocketnet_server_iocp_receive_packets(transport);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(transport);
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
    CHECK(rocketnet_server_iocp_test_receive_view_published(transport, received));
    uint64_t before[ROCKETNET_IOCP_NUM_COUNTERS], after[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(transport, before);
    uint8_t extra[32]; memset(extra, 0x44, sizeof(extra));
    for ( int i = 0; i < 100; ++i ) rocketnet_platform_socket_send_packet(peer, &destination, extra, sizeof(extra));
    const double receive_deadline = rocketnet_platform_time() + 5.0;
    do
    {
        rocketnet_server_iocp_counters(transport, after);
        if (after[ROCKETNET_IOCP_RECEIVE_COMPLETED] > before[ROCKETNET_IOCP_RECEIVE_COMPLETED]) break;
        rocketnet_platform_sleep(0.001);
    } while (rocketnet_platform_time() < receive_deadline);
    CHECK(after[ROCKETNET_IOCP_RECEIVE_COMPLETED] > before[ROCKETNET_IOCP_RECEIVE_COMPLETED]);
    CHECK(rocketnet_server_iocp_test_receive_view_published(transport, received));
    CHECK(memcmp(received, marker, sizeof(marker)) == 0); // View lifetime until next receive.

    std::vector<std::thread> producers;
    for ( int t = 0; t < 4; ++t )
        producers.emplace_back([=]() {
            for ( int i = 0; i < 64; ++i )
            {
                uint64_t id;
                uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &peer_address, &id);
                CHECK(frame);
                memset(frame + ROCKETNET_FRAME_WIRE_OFFSET, t * 64 + i, 32);
                rocketnet_server_iocp_finish_packet(transport, id, frame, 32);
            }
        });
    for ( auto & thread : producers ) thread.join();
    rocketnet_server_iocp_send_packets(transport);
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
    rocketnet_server_iocp_test_hold_completions(transport, true);
    std::atomic<bool> sending{true};
    std::thread incoming([&]() {
        while (sending.load())
            rocketnet_platform_socket_send_packet(peer, &destination, extra, sizeof(extra));
    });
    std::atomic<bool> stop_returned{false};
    std::thread stopping([&]() {
        rocketnet_server_iocp_stop(transport);
        stop_returned.store(true);
    });
    const double stop_deadline = rocketnet_platform_time() + 5.0;
    while (!rocketnet_server_iocp_test_stopping(transport) && rocketnet_platform_time() < stop_deadline)
        rocketnet_platform_sleep(0.001);
    CHECK(rocketnet_server_iocp_test_stopping(transport));
    CHECK(!stop_returned.load());
    rocketnet_server_iocp_test_hold_completions(transport, false);
    stopping.join();
    CHECK(stop_returned.load());
    wait_stopped(transport);
    sending.store(false);
    incoming.join();
    uint64_t stopped_id;
    CHECK(!rocketnet_server_iocp_start_packet(transport, &peer_address, &stopped_id));
    rocketnet_server_iocp_stop(transport);
    rocketnet_server_iocp_destroy(transport);
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_transport_datagrams_and_producers\n");
}

static void test_receive_boundaries()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(transport);
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
        rocketnet_server_iocp_receive_packets(transport);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(transport);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            CHECK(batch->packet_bytes[i] == 4 && memcmp(batch->packet_data[i], "good", 4) == 0);
            ++received;
        }
        rocketnet_server_iocp_counters(transport, counters);
        if ( received == 1 && counters[ROCKETNET_IOCP_RECEIVE_DROPPED] == 2 &&
             rocketnet_server_iocp_pending(transport) == ROCKETNET_IOCP_RECEIVES_PER_WORKER ) break;
        rocketnet_platform_sleep(0.001);
    }
    CHECK(received == 1 && counters[ROCKETNET_IOCP_RECEIVE_DROPPED] == 2);
    CHECK(rocketnet_server_iocp_pending(transport) == ROCKETNET_IOCP_RECEIVES_PER_WORKER);
    wait_stopped(transport); // Cancellation with idle, outstanding receives too.
    rocketnet_server_iocp_destroy(transport);
    closesocket(peer);
    printf("test_iocp_transport_receive_boundaries\n");
}

// ---------------------------------------------------------------------------
// Fault cases: each failure path of the IOCP and RIO transports, injected
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

static void iocp_stage( rocketnet_server_iocp_t * transport, const rocketnet_address_t * to, int round, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        uint64_t id;
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, to, &id);
        CHECK(frame);
        fault_mark(frame + ROCKETNET_FRAME_WIRE_OFFSET, round, i);
        rocketnet_server_iocp_finish_packet(transport, id, frame, FAULT_BYTES);
    }
}

// waits until sends completed plus sends failed reach total, then joins the
// retirement batch so every retired frame is back in the pool
static void iocp_wait_sends( rocketnet_server_iocp_t * transport, uint64_t total )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    const double deadline = rocketnet_platform_time() + 10.0;
    do
    {
        rocketnet_server_iocp_counters(transport, counters);
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED] >= total ) break;
        rocketnet_platform_sleep(0.001);
    } while ( rocketnet_platform_time() < deadline );
    CHECK(counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED] == total);
    rocketnet_server_iocp_test_hold_completions(transport, true);
    rocketnet_server_iocp_test_hold_completions(transport, false);
}

static uint64_t iocp_sends_retired( rocketnet_server_iocp_t * transport )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(transport, counters);
    return counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED];
}

static uint64_t iocp_counter( rocketnet_server_iocp_t * transport, int counter )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters(transport, counters);
    return counters[counter];
}

// every frame of the pool is free: a whole generation reserves, each frame once
static void iocp_all_frames_free( rocketnet_server_iocp_t * transport, const rocketnet_address_t * to )
{
    std::set<uint8_t*> frames;
    uint64_t id;
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet(transport, to, &id);
        CHECK(frame && frames.insert(frame).second);
        rocketnet_server_iocp_abort_packet(transport, id, frame);
    }
    CHECK(!rocketnet_server_iocp_start_packet(transport, to, &id));
    rocketnet_server_iocp_send_packets(transport);
}

// the receive depth is back to its configured count, with no send in flight
static void iocp_wait_depth( rocketnet_server_iocp_t * transport, int workers )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_server_iocp_pending(transport) != workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER && rocketnet_platform_time() < deadline )
        rocketnet_platform_sleep(0.001);
    CHECK(rocketnet_server_iocp_pending(transport) == workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER);
}

// n datagrams each way, every one delivered: the exchange lives
static void iocp_exchange( rocketnet_server_iocp_t * transport, const rocketnet_address_t * bind,
    rocketnet_platform_socket_t * peer, const rocketnet_address_t * peer_address, int n )
{
    const int round = ++fault_round & 0xFF;
    const uint64_t retired = iocp_sends_retired(transport);
    iocp_stage(transport, peer_address, round, n);
    rocketnet_server_iocp_send_packets(transport);
    CHECK(fault_peer_receive(peer, round, n) == n);
    iocp_wait_sends(transport, retired + (uint64_t) n);
    fault_peer_send(peer, bind, round, n);
    std::vector<bool> seen( (size_t) n, false );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets(transport);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(transport);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index(batch->packet_data[i], batch->packet_bytes[i], round);
            if ( index >= 0 && index < n && !seen[index] ) { seen[index] = true; ++count; }
        }
        rocketnet_platform_sleep(0.001);
    }
    CHECK(count == n);
}

static void iocp_finish( rocketnet_server_iocp_t * transport, rocketnet_platform_socket_t * peer )
{
    wait_stopped(transport);
    rocketnet_server_iocp_destroy(transport);
    rocketnet_platform_socket_destroy(peer);
}

static void test_iocp_send_completion_fails()
{
    // Per-datagram sends: one completion is one frame.
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 1, 0, ROCKETNET_IOCP_DISABLE_USO);
    CHECK(transport);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    iocp_exchange(transport, &bind, peer, &peer_address, 16);
    const int statuses[] = { WSAENOBUFS, ERROR_OPERATION_ABORTED };
    const uint64_t failed_before = iocp_counter(transport, ROCKETNET_IOCP_SEND_FAILED);
    for ( int s = 0; s < 2; ++s )
    {
        const uint64_t retired = iocp_sends_retired(transport);
        rocketnet_server_iocp_test_fail_next(transport, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION, 50, statuses[s]);
        iocp_stage(transport, &peer_address, 0xEE, 50);
        rocketnet_server_iocp_send_packets(transport);
        iocp_wait_sends(transport, retired + 50);
        CHECK(rocketnet_server_iocp_test_faults_left(transport, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION) == 0);
        iocp_all_frames_free(transport, &peer_address);
        iocp_exchange(transport, &bind, peer, &peer_address, 16);
    }
    CHECK(iocp_counter(transport, ROCKETNET_IOCP_SEND_FAILED) - failed_before == 100);
    iocp_wait_depth(transport, 1);
    iocp_finish(transport, peer);

    // A USO run: one failed completion returns every frame of the run.
    bind = address("127.0.0.1:0");
    transport = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(transport);
    peer = fault_peer(&peer_address);
    if ( rocketnet_server_iocp_features(transport) & ROCKETNET_IOCP_FEATURE_USO )
    {
        const uint64_t retired = iocp_sends_retired(transport);
        rocketnet_server_iocp_test_fail_next(transport, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION, 1, WSAENOBUFS);
        iocp_stage(transport, &peer_address, 0xEE, 16);
        rocketnet_server_iocp_send_packets(transport);
        CHECK(iocp_counter(transport, ROCKETNET_IOCP_USO_SENDS) == 1);
        iocp_wait_sends(transport, retired + 16);
        CHECK(iocp_counter(transport, ROCKETNET_IOCP_SEND_FAILED) == 16);
        iocp_all_frames_free(transport, &peer_address);
    }
    iocp_exchange(transport, &bind, peer, &peer_address, 16);
    iocp_finish(transport, peer);
    printf("test_iocp_transport_send_completion_fails\n");
}

static void test_iocp_send_post_fails()
{
    // A refused post has no completion: the frame returns at the post.
    for ( int uso = 0; uso < 2; ++uso )
    {
        rocketnet_address_t bind = address("127.0.0.1:0");
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 1, 0, uso ? 0 : ROCKETNET_IOCP_DISABLE_USO);
        CHECK(transport);
        rocketnet_address_t peer_address;
        rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
        const bool runs = uso && ( rocketnet_server_iocp_features(transport) & ROCKETNET_IOCP_FEATURE_USO );
        if ( uso && !runs )
        {
            iocp_finish(transport, peer);
            continue;
        }
        // per datagram: 100 refused posts of one frame; with USO: 2 refused
        // runs of 16 equal frames
        const int posts = runs ? 2 : 100;
        const int frames = runs ? 32 : 100;
        rocketnet_server_iocp_test_fail_next(transport, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, posts, WSAENOBUFS);
        for ( int r = 0; r < ( runs ? 2 : 1 ); ++r )
        {
            iocp_stage(transport, &peer_address, 0xEE, runs ? 16 : frames);
            rocketnet_server_iocp_send_packets(transport);
        }
        CHECK(rocketnet_server_iocp_test_faults_left(transport, ROCKETNET_IOCP_TEST_FAULT_SEND_POST) == 0);
        CHECK(iocp_counter(transport, ROCKETNET_IOCP_SEND_FAILED) == (uint64_t) frames);
        CHECK(iocp_counter(transport, ROCKETNET_IOCP_SEND_COMPLETED) == 0);
        CHECK(rocketnet_server_iocp_pending(transport) == ROCKETNET_IOCP_RECEIVES_PER_WORKER);
        iocp_all_frames_free(transport, &peer_address);
        iocp_exchange(transport, &bind, peer, &peer_address, 32);
        iocp_finish(transport, peer);
    }
    printf("test_iocp_transport_send_post_fails\n");
}

static void test_iocp_receive_completion_fails()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 1, 0, ROCKETNET_IOCP_DISABLE_URO);
    CHECK(transport);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    iocp_exchange(transport, &bind, peer, &peer_address, 16);

    // 100 failed receive completions: each is counted dropped and its
    // operation is re-posted, so the depth holds.
    const int statuses[] = { WSAENOBUFS, ERROR_OPERATION_ABORTED };
    for ( int s = 0; s < 2; ++s )
    {
        const uint64_t dropped = iocp_counter(transport, ROCKETNET_IOCP_RECEIVE_DROPPED);
        rocketnet_server_iocp_test_fail_next(transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION, 50, statuses[s]);
        fault_peer_send(peer, &bind, 0xEE, 50);
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( iocp_counter(transport, ROCKETNET_IOCP_RECEIVE_DROPPED) - dropped < 50 && rocketnet_platform_time() < deadline )
            rocketnet_platform_sleep(0.001);
        CHECK(iocp_counter(transport, ROCKETNET_IOCP_RECEIVE_DROPPED) - dropped == 50);
        CHECK(rocketnet_server_iocp_test_faults_left(transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION) == 0);
        iocp_wait_depth(transport, 1);
        iocp_exchange(transport, &bind, peer, &peer_address, 16);
    }

    // Refused re-posts: each completion delivers its datagram, its re-post is
    // refused and counted, and the retry list re-posts it once the refusals
    // end: the depth recovers to its configured count.
    const uint64_t repost_failed = iocp_counter(transport, ROCKETNET_IOCP_RECEIVE_REPOST_FAILED);
    rocketnet_server_iocp_test_fail_next(transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST, 300, WSAENOBUFS);
    const int round = ++fault_round & 0xFF;
    fault_peer_send(peer, &bind, round, 64);
    std::vector<bool> seen( 64, false );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( ( count < 64 || rocketnet_server_iocp_test_faults_left(transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST) > 0 ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets(transport);
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets(transport);
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index(batch->packet_data[i], batch->packet_bytes[i], round);
            if ( index >= 0 && index < 64 && !seen[index] ) { seen[index] = true; ++count; }
        }
        rocketnet_platform_sleep(0.001);
    }
    CHECK(count == 64);
    CHECK(rocketnet_server_iocp_test_faults_left(transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST) == 0);
    iocp_wait_depth(transport, 1);
    CHECK(iocp_counter(transport, ROCKETNET_IOCP_RECEIVE_REPOST_FAILED) - repost_failed == 300);
    iocp_exchange(transport, &bind, peer, &peer_address, 64);
    iocp_finish(transport, peer);
    printf("test_iocp_transport_receive_completion_fails\n");
}

static void test_iocp_send_queue_full_recovers()
{
    rocketnet_address_t bind = address("127.0.0.1:0");
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 1);
    CHECK(transport);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    iocp_exchange(transport, &bind, peer, &peer_address, 16);

    // Held completions: the whole queue is staged and posted, and nothing
    // can retire it. start_packet refuses until the completions are read.
    const uint64_t retired = iocp_sends_retired(transport);
    rocketnet_server_iocp_test_hold_completions(transport, true);
    iocp_stage(transport, &peer_address, 0xEE, ROCKETNET_IOCP_SEND_QUEUE_SIZE);
    uint64_t id;
    CHECK(!rocketnet_server_iocp_start_packet(transport, &peer_address, &id));
    rocketnet_server_iocp_send_packets(transport);
    CHECK(!rocketnet_server_iocp_start_packet(transport, &peer_address, &id));
    CHECK(rocketnet_server_iocp_pending(transport) > ROCKETNET_IOCP_RECEIVES_PER_WORKER ||
          iocp_counter(transport, ROCKETNET_IOCP_SEND_FAILED) > 0);
    rocketnet_server_iocp_send_packets(transport);
    CHECK(!rocketnet_server_iocp_start_packet(transport, &peer_address, &id));
    rocketnet_server_iocp_test_hold_completions(transport, false);

    // Released: every send drains, every frame is free, the exchange lives.
    iocp_wait_sends(transport, retired + ROCKETNET_IOCP_SEND_QUEUE_SIZE);
    iocp_wait_depth(transport, 1);
    iocp_all_frames_free(transport, &peer_address);
    iocp_exchange(transport, &bind, peer, &peer_address, 64);
    printf("test_iocp_transport_send_queue_full_recovers\n");
    iocp_finish(transport, peer);
}

// A watchdog over one call: the process exits 1 naming the call when it
// runs past its bound, so a destroy that never returns is a failure, never a
// hang.
struct case_watchdog_t
{
    std::atomic<bool> done{false};
    std::thread thread;
    case_watchdog_t( const char * name, double seconds )
    {
        thread = std::thread( [this, name, seconds]() {
            const double deadline = rocketnet_platform_time() + seconds;
            while ( !done.load() )
            {
                if ( rocketnet_platform_time() > deadline )
                {
                    fprintf( stderr, "IOCP check failed: %s ran past its %.0f s watchdog (%s:%d)\n", name, seconds, __FILE__, __LINE__ );
                    fflush( stderr );
                    _Exit( 1 );
                }
                rocketnet_platform_sleep( 0.01 );
            }
        } );
    }
    ~case_watchdog_t() { done.store( true ); thread.join(); }
};

// Destroy with I/O in flight: rocketnet_server_iocp_stop cancels with
// CancelIoEx( socket, NULL ), which cancels every thread's outstanding I/O on
// the handle, then wakes each worker; destroy joins the workers and only then
// closes the socket. 100 idle servers are created and destroyed, then 100
// more after datagrams completed, so at least one receive was re-posted by a
// worker thread rather than the destroying one. Each destroy runs under its
// own 10 s watchdog.
static void iocp_destroy_watched( rocketnet_server_iocp_t * transport, double * slowest )
{
    case_watchdog_t watchdog( "rocketnet_server_iocp_destroy", 10.0 );
    const double start = rocketnet_platform_time();
    rocketnet_server_iocp_destroy(transport);
    const double elapsed = rocketnet_platform_time() - start;
    if ( elapsed > *slowest ) *slowest = elapsed;
}

static void test_iocp_destroy_never_hangs()
{
    double slowest = 0.0;
    for ( int i = 0; i < 100; ++i )
    {
        rocketnet_address_t bind = address("127.0.0.1:0");
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 4);
        CHECK(transport);
        iocp_destroy_watched(transport, &slowest);
    }
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    for ( int i = 0; i < 100; ++i )
    {
        rocketnet_address_t bind = address("127.0.0.1:0");
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create(NULL, &bind, 4);
        CHECK(transport);
        fault_peer_send(peer, &bind, 0xEE, 4 * ROCKETNET_IOCP_RECEIVES_PER_WORKER);
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( iocp_counter(transport, ROCKETNET_IOCP_RECEIVE_COMPLETED) == 0 && rocketnet_platform_time() < deadline )
            rocketnet_platform_sleep(0.001);
        CHECK(iocp_counter(transport, ROCKETNET_IOCP_RECEIVE_COMPLETED) > 0);
        iocp_destroy_watched(transport, &slowest);
    }
    rocketnet_platform_socket_destroy(peer);
    printf("test_iocp_transport_destroy_never_hangs\n");
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
    CHECK(rocketnet_server_rio_available());
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
    printf("test_rio_transport_send_post_fails\n");
}

static void test_rio_request_queue_full()
{
    CHECK(rocketnet_server_rio_available());
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
    printf("test_rio_transport_request_queue_full\n");
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
    CHECK(rocketnet_server_rio_available());
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open(&bind);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    rio_exchange(rio, &bind, peer, &peer_address, 16);

    // Unpolled, every slice completes and the receive completion queue fills
    // with ROCKETNET_RIO_RECEIVES completions; the host drops what arrives
    // with no slice posted. The poll that reads the full queue fills the bank
    // to capacity and re-posts slice by slice while a sender keeps datagrams
    // coming. rocketnet_server_rio_receive_packets dequeues at most one bank
    // per call, so completions past the bank stay queued for the next call:
    // no overflow is counted, and the next poll delivers the tail.
    const uint64_t overflow = rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW);
    int attempts = 0;
    int tail = 0;
    while ( tail == 0 && attempts < 10 )
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
        // completions the flood queued during the bounded poll are the tail
        rocketnet_server_rio_receive_packets(rio);
        tail = rocketnet_server_rio_process_packets(rio)->num_packets;
        CHECK(tail <= ROCKETNET_RIO_RECEIVES);
        CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW) == overflow);
        rio_settle(rio);
        // drain what the sender left before the next attempt
        for ( int i = 0; i < 20; ++i ) { rocketnet_server_rio_receive_packets(rio); rocketnet_platform_sleep(0.005); }
    }
    CHECK(tail > 0);
    CHECK(rio_counter(rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW) == overflow);
    rio_settle(rio);
    rio_exchange(rio, &bind, peer, &peer_address, 64);
    rio_finish(rio, peer);
    printf("test_rio_transport_completion_queue_full\n");
}

// Sends a refused RIO_MSG_COMMIT_ONLY left deferred at destroy are
// committed before rocketnet_server_rio_destroy closes the socket: the
// destroy drains well inside ROCKETNET_RIO_DRAIN_SECONDS and leaks no
// region. The close may abort committed sends, so delivery is printed, not
// checked.
static void test_rio_destroy_commits_deferred_sends()
{
    CHECK(rocketnet_server_rio_available());
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open(&bind);
    rocketnet_address_t peer_address;
    rocketnet_platform_socket_t * peer = fault_peer(&peer_address);
    rio_exchange(rio, &bind, peer, &peer_address, 16);

    const int round = ++fault_round & 0xFF;
    rocketnet_server_rio_test_fail_next(rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, 1, WSAENOBUFS);
    rio_stage(rio, &peer_address, round, 10);
    rocketnet_server_rio_send_packets(rio);
    CHECK(rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_COMMIT) == 0);
    CHECK(rocketnet_server_rio_pending(rio) == ROCKETNET_RIO_RECEIVES + 10);

    const double start = rocketnet_platform_time();
    rocketnet_server_rio_destroy(rio);
    const double elapsed = rocketnet_platform_time() - start;
    CHECK(rocketnet_server_rio_leaked_regions() == 0);
    CHECK(elapsed < 1.0);
    fault_peer_receive(peer, round, 10);
    rocketnet_platform_socket_destroy(peer);
    printf("test_rio_transport_destroy_commits_deferred_sends\n");
}

// ---------------------------------------------------------------------------
// Windows socket rows: the bound port is the socket's alone, and a refused
// send is counted on the socket and logged with the Winsock error.

// a raw socket that asks to share the port with SO_REUSEADDR, as another
// local process would, on the address given; true when Windows lets it bind
static bool port_thief_binds( const rocketnet_address_t * owner, SOCKET * thief )
{
    *thief = socket( owner->type == ROCKETNET_ADDRESS_IPV6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK(*thief != INVALID_SOCKET);
    BOOL reuse = TRUE;
    CHECK(setsockopt(*thief, SOL_SOCKET, SO_REUSEADDR, (const char*) &reuse, sizeof(reuse)) == 0);
    u_long non_blocking = 1;
    CHECK(ioctlsocket(*thief, FIONBIO, &non_blocking) == 0);
    sockaddr_storage storage;
    memset(&storage, 0, sizeof(storage));
    int bytes;
    if ( owner->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 * a = (sockaddr_in6*) &storage;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(owner->port);
        for ( int i = 0; i < 8; ++i )
        {
            a->sin6_addr.s6_addr[i * 2] = (uint8_t) ( owner->data.ipv6[i] >> 8 );
            a->sin6_addr.s6_addr[i * 2 + 1] = (uint8_t) owner->data.ipv6[i];
        }
        bytes = sizeof(sockaddr_in6);
    }
    else
    {
        sockaddr_in * a = (sockaddr_in*) &storage;
        a->sin_family = AF_INET;
        a->sin_port = htons(owner->port);
        memcpy(&a->sin_addr, owner->data.ipv4, 4);
        bytes = sizeof(sockaddr_in);
    }
    return bind(*thief, (sockaddr*) &storage, bytes) == 0;
}

// datagrams a peer sends to the owner's port that the thief receives instead
static int port_thief_steals( const rocketnet_address_t * owner, SOCKET thief )
{
    rocketnet_address_t peer_address = address(owner->type == ROCKETNET_ADDRESS_IPV6 ? "[::1]:0" : "127.0.0.1:0");
    rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, &peer_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024);
    CHECK(peer);
    uint8_t data[FAULT_BYTES];
    for ( int i = 0; i < 8; ++i )
    {
        fault_mark(data, 0x5A, i);
        rocketnet_platform_socket_send_packet(peer, owner, data, FAULT_BYTES);
    }
    int stolen = 0;
    const double deadline = rocketnet_platform_time() + 1.0;
    while ( stolen < 8 && rocketnet_platform_time() < deadline )
    {
        char packet[ROCKETNET_RECEIVE_CEILING];
        if ( recv(thief, packet, sizeof(packet), 0) > 0 ) ++stolen; else rocketnet_platform_sleep(0.001);
    }
    rocketnet_platform_socket_destroy(peer);
    return stolen;
}

// a server bound to the wildcard address owns its port: a second socket
// that binds the more specific loopback address on that port with
// SO_REUSEADDR would take every datagram sent there, and is refused.
// Portable, IOCP and RIO; IPv4 and dual-stack IPv6.
static void test_windows_port_exclusive()
{
    const char * binds[] = { "0.0.0.0:0", "[::]:0" };
    const char * targets[] = { "127.0.0.1:0", "[::1]:0" };
    const char * transports[] = { "portable", "iocp", "rio" };
    for ( int b = 0; b < 2; ++b )
    {
        for ( int d = 0; d < 3; ++d )
        {
            rocketnet_address_t owner = address(binds[b]);
            rocketnet_platform_socket_t * portable = NULL;
            rocketnet_server_iocp_t * iocp = NULL;
            rocketnet_server_rio_t * rio = NULL;
            if ( d == 0 ) { portable = rocketnet_platform_socket_create(NULL, &owner, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024); CHECK(portable); }
            if ( d == 1 ) { iocp = rocketnet_server_iocp_create(NULL, &owner, 1); CHECK(iocp); }
            if ( d == 2 ) { rio = rocketnet_server_rio_create(NULL, &owner); CHECK(rio); }
            CHECK(owner.port != 0);
            rocketnet_address_t target = address(targets[b]);
            target.port = owner.port;
            SOCKET thief;
            const bool bound = port_thief_binds(&target, &thief);
            const int stolen = bound ? port_thief_steals(&target, thief) : 0;
            (void) stolen;
            closesocket(thief);
            if ( portable ) rocketnet_platform_socket_destroy(portable);
            if ( iocp ) { wait_stopped(iocp); rocketnet_server_iocp_destroy(iocp); }
            if ( rio ) { rocketnet_server_rio_destroy(rio); CHECK(rocketnet_server_rio_leaked_regions() == 0); }
            CHECK(!bound);
        }
    }
    printf("test_windows_port_exclusive\n");
}

static std::atomic<int> send_failed_logs{0};

static void send_failed_log( void *, rocketnet_log_level_t, const char * message, size_t )
{
    // WSAEMSGSIZE is 10040: the Winsock error, never errno's text
    if ( strstr(message, "sendto (") && strstr(message, "WSA error 10040") ) send_failed_logs.fetch_add(1);
}

// a sendto that answers SOCKET_ERROR (an oversize datagram: WSAEMSGSIZE) is
// counted on the socket and logged with WSAGetLastError(); a good send is not
// counted. An IPv4 socket and an IPv6 socket: both sendto arms.
static void test_windows_send_failures_counted()
{
    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };
    const char * peers[] = { "127.0.0.1:0", "[::1]:0" };
    std::vector<uint8_t> oversize( 70000, 0x5A );
    for ( int c = 0; c < 2; ++c )
    {
        rocketnet_address_t bind = address(binds[c]);
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create(NULL, &bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024);
        CHECK(socket);
        rocketnet_address_t peer_address = address(peers[c]);
        rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create(NULL, &peer_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024);
        CHECK(peer);
        CHECK(rocketnet_platform_socket_send_failures(socket) == 0);
        uint8_t data[FAULT_BYTES];
        fault_mark(data, 0x5B, 0);
        rocketnet_platform_socket_send_packet(socket, &peer_address, data, FAULT_BYTES);
        CHECK(rocketnet_platform_socket_send_failures(socket) == 0);
        send_failed_logs.store(0);
        rocketnet_log_function(send_failed_log, NULL);
        rocketnet_log_level(ROCKETNET_LOG_LEVEL_DEBUG);
        for ( int i = 0; i < 3; ++i )
            rocketnet_platform_socket_send_packet(socket, &peer_address, oversize.data(), (int) oversize.size());
        rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
        rocketnet_log_function(NULL, NULL);
        CHECK(rocketnet_platform_socket_send_failures(socket) == 3);
        CHECK(send_failed_logs.load() == 3);
        rocketnet_platform_socket_send_packet(socket, &peer_address, data, FAULT_BYTES);
        CHECK(rocketnet_platform_socket_send_failures(socket) == 3);
        rocketnet_platform_socket_destroy(peer);
        rocketnet_platform_socket_destroy(socket);
    }
    printf("test_windows_send_failures_counted\n");
}

struct fault_case_t { const char * name; void (*run)(); };

static const fault_case_t fault_cases[] = {
    { "test_iocp_transport_send_completion_fails", test_iocp_send_completion_fails },
    { "test_iocp_transport_send_post_fails", test_iocp_send_post_fails },
    { "test_iocp_transport_receive_completion_fails", test_iocp_receive_completion_fails },
    { "test_iocp_transport_send_queue_full_recovers", test_iocp_send_queue_full_recovers },
    { "test_iocp_transport_destroy_never_hangs", test_iocp_destroy_never_hangs },
    { "test_rio_transport_completion_queue_full", test_rio_completion_queue_full },
    { "test_rio_transport_request_queue_full", test_rio_request_queue_full },
    { "test_rio_transport_send_post_fails", test_rio_send_post_fails },
    { "test_rio_transport_destroy_commits_deferred_sends", test_rio_destroy_commits_deferred_sends },
    { "test_windows_port_exclusive", test_windows_port_exclusive },
    { "test_windows_send_failures_counted", test_windows_send_failures_counted },
};

// With no argument every case runs; with names, only those cases run.
int main( int argc, char ** argv )
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    CHECK(rocketnet_init( NULL, allocate, release ) == ROCKETNET_STATUS_OK);
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
