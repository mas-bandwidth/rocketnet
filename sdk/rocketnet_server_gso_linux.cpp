#include "rocketnet_memory.h"
#include <new>
/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// The GSO transport. See rocketnet_server_gso.h for the shape
// and the framing; this file is the machinery.
//
// The receive side is the XDP transport's pattern on portable primitives: one
// socket per queue in an SO_REUSEPORT group, one worker thread per socket,
// each worker draining its own socket with recvmmsg into its own double
// buffer -- zero shared state between queues -- and the main thread flipping
// the buffers once per update under one mutex acquisition per queue. UDP_GRO
// means one delivered buffer can carry many coalesced same-flow segments;
// the worker splits them at the segment stride the kernel reports and stages
// each as its own packet, so above this file a packet is a packet and GRO is
// invisible.
//
// The send side is per queue too, the XDP transport's shape: a destination
// maps to one queue by the XDP hash, and game threads fill that queue's
// main-owned TX bank between flushes with one compare-and-swap claim on the
// queue's count. send_packets only try-publishes each queue's bank into that
// queue's bounded FIFO and wakes its worker; the queue's own worker builds
// coalesced sendmmsg batches and sends them on its own socket, outside the
// ownership mutex. Main makes no send system call. Hot bank metadata and
// destination/size arrays are separate from cold frame payloads.
//
// Every kernel feature is probed at create, and every probe failure selects
// the next tier down rather than an error: no UDP_SEGMENT -> per-packet
// sendmmsg entries; no UDP_GRO -> plain recvmmsg; and a kernel that accepts
// the UDP_SEGMENT probe but refuses a live GSO send to one destination (its
// route: an IPsec policy, say) resends that run plain, remembers the
// destination so its later runs leave plain, and leaves segmentation on for
// every other destination. Segmentation turns off for the transport only
// when ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD destinations refused
// and no segmented send was ever accepted (rocketnet_server_gso.h). That
// degradation resends the remainder plain and drops nothing.

#include "rocketnet_server_gso.h"
#include "rocketnet_driver.h"
#include "rocketnet_server_udp_linux.h"
#include "rocketnet_gso_pin.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"
#include "rocketnet_xdp_frame.h"
#include "rocketnet_receive_prefilter.h"
#include "rocketnet_send_gather.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <string.h>
#include <stdio.h>
#include <atomic>

// the glibc headers on older distros may not carry these; the values are
// kernel ABI and the runtime probe decides whether they work

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

// the queue count the transport reports through send_queues (sdk/rocketnet_driver.h)
static_assert( ROCKETNET_GSO_MAX_QUEUES <= ROCKETNET_SERVER_MAX_SEND_QUEUES, "the gso queues fit the driver's send queue limit" );

// IMPORTANT: The 4096 byte padding walls in these structs are load-bearing,
// exactly as in the xdp transport: each field between walls is touched by a
// different thread, and without the walls those fields would share cache
// lines. Nothing goes wrong if you remove them -- it just gets slow.

// Each slot's info occupies one cache line so adjacent concurrent finishes
// do not share a line. The owning allocation is aligned by rocketnet_malloc.

struct rocketnet_server_gso_send_info_t
{
    rocketnet_address_t to;
    int packet_bytes;
    uint8_t padding[64 - sizeof( rocketnet_address_t ) - sizeof( int )];
};

static_assert( sizeof( rocketnet_server_gso_send_info_t ) == 64, "one send info per cache line" );

struct rocketnet_server_gso_send_buffer_t
{
    uint8_t padding_0[4096];

    std::atomic<int> num_packets{ 0 };

    uint8_t padding_1[4096];

    rocketnet_server_gso_send_info_t info[ROCKETNET_GSO_SEND_QUEUE_SIZE];

    uint8_t padding_2[4096];

    alignas( 16 ) uint8_t data[ROCKETNET_GSO_SEND_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];

    uint8_t padding_3[4096];
};

struct rocketnet_server_gso_recv_buffer_t
{
    uint8_t padding_0[4096];

    uint32_t num_packets = 0;
    uint64_t dropped_size = 0;
    uint64_t dropped_too_large = 0;     // the part of dropped_size above the receive ceiling or truncated
    uint64_t dropped_overflow = 0;
    uint64_t dropped_source = 0;
    uint64_t dropped_prefilter = 0;     // refused by the prefilter for a reason other than size
    uint64_t gro_batches = 0;
    uint64_t gro_cmsg_faults = 0;                   // a GRO cmsg this worker refused
    uint64_t receive_errors = 0;
    rocketnet_address_t from[ROCKETNET_GSO_RECV_QUEUE_SIZE];
    int packet_bytes[ROCKETNET_GSO_RECV_QUEUE_SIZE];
    alignas( 16 ) uint8_t packet_data[ROCKETNET_GSO_RECV_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];

    uint8_t padding_1[4096];
};

// One queue's send side (layer L2 of the gso push): its own staging banks,
// filled by the game threads with one compare-and-swap claim per
// packet, handed to the queue's own worker by main at the frame boundary, and
// sent by that worker on the queue's own socket. A destination always maps to
// one queue (rocketnet_server_gso_send_queue), so every packet to one client
// leaves through one socket in the order it was staged.
// proofs/tla/RNGsoQueueBankLayout.tla (the claim, the id, the slot from a
// pointer) and proofs/tla/RNGsoHandoff.tla with proofs/tla/RNBankFifoLayout.tla
// (the hand-off, per queue).

// the gather's bucket count for a bank (rocketnet_send_gather_scratch_bytes):
// the least power of two at least twice the slots, from buckets up
static constexpr int rocketnet_server_gso_gather_buckets( int buckets )
{
    return buckets >= 2 * ROCKETNET_GSO_SEND_QUEUE_SIZE ? buckets : rocketnet_server_gso_gather_buckets( 2 * buckets );
}

struct rocketnet_server_gso_send_lane_t
{
    uint8_t padding_0[4096];

    // written by main only while no game thread is staging (the game threads' call contract), read
    // by the game threads: the producer bank and its turn-over count, the
    // packet id's high part
    int producer = 0;
    uint64_t turn = 0;

    uint8_t padding_1[4096];

    // Only bank indices cross this mutex. Main retains its producer on
    // contention or a full FIFO; the worker returns a bank only after all its
    // sends finished.
    rocketnet_platform_mutex_t mutex = {};
    // proofs/tla/RNBankFifoLayout.tla Init: this literal is right for three banks only.
    int free_banks[ROCKETNET_GSO_TX_BANKS] = { 1, 2, 0 };
    int free_head = 0;
    int free_count = ROCKETNET_GSO_TX_BANKS - 1;
    int ready[ROCKETNET_GSO_TX_BANKS] = {};
    int ready_head = 0;
    int ready_count = 0;
    // the worker sets it while it holds a bank the socket refused; main's
    // next update wakes the worker to send the rest
    std::atomic<bool> full{ false };

    uint8_t padding_2[4096];

    // the worker's counters, read by main: one writer each, so a load and a
    // store, never a read-modify-write
    std::atomic<uint64_t> counter_packets_sent{ 0 };
    std::atomic<uint64_t> counter_send_syscalls{ 0 };
    std::atomic<uint64_t> counter_send_dropped{ 0 };
    std::atomic<uint64_t> counter_gso_sends{ 0 };

    uint8_t padding_3[4096];

    // worker only: the bank it holds (-1 none), the position in order where
    // its send stopped, its number of positions, the retries after shutdown,
    // and whether it is stop's final bank
    int held = -1;
    int held_slot = 0;
    int held_count = 0;
    int retries_after_quit = 0;
    bool held_final = false;
    struct rocketnet_segment_refused_t segment_refused = {};    // the destinations that refused a segmented send
    bool segment_refusal_warned = false;        // the first refused run warns, later ones are spam

    // worker-only scratch: sendmmsg headers, gather iovecs (up to a full
    // super-datagram per message), destinations and segment cmsgs
    struct mmsghdr headers[ROCKETNET_GSO_SEND_BATCH] = {};
    struct iovec iovecs[ROCKETNET_GSO_SEND_BATCH * ROCKETNET_GSO_MAX_SEGMENTS] = {};
    struct sockaddr_storage names[ROCKETNET_GSO_SEND_BATCH] = {};
    char controls[ROCKETNET_GSO_SEND_BATCH][CMSG_SPACE( sizeof( uint16_t ) )] = {};

    // worker-only gather (sdk/rocketnet_send_gather.h): a taken bank's slots
    // grouped by destination, each destination's in staged order. The flush
    // walks positions in order, so a destination's packets sit together and
    // segment even when other destinations were staged between them.
    // gather_memory holds the gather's scratch: buckets (the least power of
    // two at least twice the slots) plus three ints per slot
    int gather_memory[rocketnet_server_gso_gather_buckets( 1 ) + 3 * ROCKETNET_GSO_SEND_QUEUE_SIZE] = {};
    struct rocketnet_send_gather_scratch_t gather = {};
    int order[ROCKETNET_GSO_SEND_QUEUE_SIZE] = {};
    struct rocketnet_send_gather_run_t runs[ROCKETNET_GSO_SEND_QUEUE_SIZE] = {};

    uint8_t padding_4[4096];

    struct rocketnet_server_gso_send_buffer_t banks[ROCKETNET_GSO_TX_BANKS];
};

struct rocketnet_server_gso_queue_t
{
    uint8_t padding_0[4096];

    int fd = -1;
    int wake_fd = -1;               // eventfd main writes to wake the worker; -1 on the test fixture
    int index = 0;                  // this queue's place in the group: the packet id's low part
    bool ipv6 = false;
    struct rocketnet_server_gso_t * gso = NULL;
    rocketnet_platform_thread_t * thread = NULL;

    // the cpu the worker pinned itself to (rocketnet_gso_pin.h): PENDING until
    // its thread has run the pin, then the cpu or NONE

    std::atomic<int> pin_cpu{ ROCKETNET_GSO_PIN_PENDING };

    // its pin refused and the confinement off cpu 0 failed too: the worker
    // runs on a mask that holds cpu 0 and asked for no real-time scheduling
    // (rocketnet_server_gso_place_worker); stored before pin_cpu

    std::atomic<bool> left_on_core_zero{ false };

    uint8_t padding_1[4096];

    std::atomic<bool> quit{ 0 };
    std::atomic<bool> stopped{ false };

    uint8_t padding_2[4096];

    // Only ownership is protected by receive_mutex. The worker fills one
    // buffer, can publish one ready buffer, and the main thread holds one
    // view buffer. One of ready/free is null at each handoff.
    struct rocketnet_server_gso_recv_buffer_t * worker_buffer = NULL;
    struct rocketnet_server_gso_recv_buffer_t * ready_buffer = NULL;
    struct rocketnet_server_gso_recv_buffer_t * main_buffer = NULL;
    struct rocketnet_server_gso_recv_buffer_t * free_buffer = NULL;
    rocketnet_platform_mutex_t receive_mutex = {};
    uint32_t main_cursor = 0; // retained tail in main_buffer, main thread only
    // Idle EAGAIN calls still count without publishing an empty ready buffer.
    std::atomic<uint64_t> receive_syscalls{ 0 };

    uint8_t padding_3b[4096];

    // the datagrams this queue's worker dropped before banking, by reason
    // (sdk/rocketnet_receive_prefilter.h): the worker is the one writer, main
    // sums every queue's in rocketnet_server_gso_receive_drop_totals
    struct rocketnet_receive_drop_tally_t receive_tally = {};

    uint8_t padding_3[4096];

    // worker-only recvmmsg scratch: the kernel fills these, the worker
    // splits and copies segments into its owned staging buffer

    struct mmsghdr messages[ROCKETNET_GSO_RECV_BATCH] = {};
    struct iovec iovecs[ROCKETNET_GSO_RECV_BATCH] = {};
    struct sockaddr_storage names[ROCKETNET_GSO_RECV_BATCH] = {};
    char controls[ROCKETNET_GSO_RECV_BATCH][CMSG_SPACE( sizeof( int ) )] = {};
    uint8_t scratch[ROCKETNET_GSO_RECV_BATCH][ROCKETNET_GSO_RECV_BUFFER_BYTES]; // filled by the kernel before it is read

    uint8_t padding_4[4096];

    struct rocketnet_server_gso_recv_buffer_t buffers[3];

    uint8_t padding_5[4096];

    struct rocketnet_server_gso_send_lane_t send;
};

struct rocketnet_server_gso_t
{
    void * context;

    int num_queues;
    // the slots a producer bank hands out: ROCKETNET_GSO_SEND_QUEUE_SIZE, and
    // fewer only on the trace check's fixture (rocketnet_server_gso_test_open_slots),
    // so its walk fills a bank in a few steps. proofs/tla/RNGsoQueueBankLayout.tla S.
    int send_slots = ROCKETNET_GSO_SEND_QUEUE_SIZE;
    int receive_start_queue = 0; // the next receive_packets call starts at this queue
    bool stopping = false;
    // main thread: what the queue workers received that main never took,
    // counted once, when stopped first finds every worker exited
    // (RECEIVE_DROP_STOPPED, rocketnet_server_gso_count_stopped)
    bool stop_counted = false;
    uint64_t receive_dropped_stopped = 0;
    std::atomic<int> features{ 0 };
    bool ipv6;

    // some queue's segmented send was accepted: the transport can segment, so
    // no queue's refusals turn the tier off (rocketnet_segment_refused_t's
    // accepted, made transport-wide; any worker sets it, once)
    std::atomic<bool> segment_accepted{ false };

    // finishes rocketnet_server_gso_send_slot refused (a pointer or id that
    // names no slot of its queue's producer, or a finish after stop): the
    // packet was never staged and is counted here. Any game thread adds.
    std::atomic<uint64_t> counter_send_finish_refused{ 0 };

    // the server's packet filter value, copied at create, read by every
    // worker's prefilter; has_filter is false when the transport is driven
    // directly (raw datagrams, no protocol), and then the worker applies the
    // size pass alone
    bool has_filter = false;
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = {};

    struct rocketnet_server_gso_queue_t * queues;

    // shutdown has begun (rocketnet_server_gso_begin_shutdown, or stop): each
    // queue worker's retry of a full socket is bounded from here
    std::atomic<bool> send_shutdown{ false };

    // Send counters live in each queue's lane and are summed on read. Receive
    // totals are main-owned, folded from claimed receive buffers and
    // per-queue syscall atomics.
    uint64_t counter_packets_received;
    uint64_t counter_receive_syscalls;
    uint64_t counter_receive_dropped_size;
    uint64_t counter_receive_dropped_too_large;
    uint64_t counter_receive_dropped_overflow;
    uint64_t counter_receive_dropped_source;
    uint64_t counter_receive_dropped_prefilter;
    uint64_t counter_receive_errors;
    uint64_t counter_gro_batches;
    uint64_t counter_gro_cmsg_faults;

    struct rocketnet_server_socket_process_packets_t process_packets;
    int max_clients;                    // the server's configured client count: sizes the view and the socket receive buffers
    int num_os_cpus = 0;                // the server's num_os_cpus: the workers pin to num_os_cpus + queue; below 1 reads as 1 (the OS keeps cpu 0)
    std::atomic<int> pin_refusals{ 0 }; // pins the workers asked for and the system refused

#if ROCKETNET_DEV
    // the test seam: the next recvmmsg calls on any worker, and the next
    // sendmmsg calls of a flush, answer -1 with the given errno without
    // reaching the kernel
    std::atomic<int> test_fail_receives{ 0 };
    std::atomic<int> test_fail_receive_error{ 0 };
    std::atomic<int> test_fail_sends{ 0 };
    std::atomic<int> test_fail_send_error{ 0 };
    // the send seam: while set, each sendmmsg of a flush is this call, which
    // answers as sendmmsg would
    int ( *test_send )( struct mmsghdr * messages, unsigned int count, void * user ) = NULL;
    void * test_send_user = NULL;
    // the receive seam: while set, each recvmmsg of a drain is this call,
    // which fills the queue's messages and answers as recvmmsg would
    int ( *test_receive )( struct rocketnet_server_gso_queue_t * queue, int vlen, void * user ) = NULL;
    void * test_receive_user = NULL;
    // the claim seam: while set, start_packet calls this between its load of
    // the producer's count and its compare-and-swap, with the queue; and every
    // compare-and-swap that lost is counted, so a walk can interleave a second
    // claimer there and read what the library's own loop did
    void ( *test_claim_hook )( void * user, int queue ) = NULL;
    void * test_claim_user = NULL;
    std::atomic<int> test_claim_losses{ 0 };
#endif // #if ROCKETNET_DEV
};

#if ROCKETNET_DEV

// development library only: the create flags every following create ORs in,
// so a transport-level case can force the plain tiers

static std::atomic<int> rocketnet_server_gso_test_create_flags{ 0 };

void rocketnet_server_gso_test_force_create_flags( int flags )
{
    rocketnet_server_gso_test_create_flags.store( flags );
}

void rocketnet_server_gso_test_fail_receives( struct rocketnet_server_gso_t * gso, int count, int error )
{
    rocketnet_assert( gso );
    gso->test_fail_receive_error.store( error );
    gso->test_fail_receives.store( count > 0 ? count : 0 );
}

void rocketnet_server_gso_test_fail_sends( struct rocketnet_server_gso_t * gso, int count, int error )
{
    rocketnet_assert( gso );
    gso->test_fail_send_error.store( error, std::memory_order_relaxed );
    gso->test_fail_sends.store( count > 0 ? count : 0, std::memory_order_release );
}

// main thread only: true when no queue's send side holds work -- each
// producer is empty, no bank waits in a ready FIFO, and every worker has
// returned every bank it took. A worker counts a bank's sends and drops before
// it returns the bank under its lane mutex, so once this answers true the send
// counters are final for every bank published so far.
bool rocketnet_server_gso_test_send_idle( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    for ( int i = 0; i < gso->num_queues; i++ )
    {
        struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[i].send;
        if ( lane->producer < 0 )
        {
            continue;
        }
        if ( lane->banks[lane->producer].num_packets.load( std::memory_order_relaxed ) != 0 )
        {
            return false;
        }
        rocketnet_platform_mutex_acquire( &lane->mutex );
        const bool idle = lane->ready_count == 0 && lane->free_count == ROCKETNET_GSO_TX_BANKS - 1;
        rocketnet_platform_mutex_release( &lane->mutex );
        if ( !idle )
        {
            return false;
        }
    }

    return true;
}

bool rocketnet_server_gso_test_receive_ready( struct rocketnet_server_gso_t * gso, int queue_index )
{
    rocketnet_assert( gso );
    rocketnet_assert( queue_index >= 0 && queue_index < gso->num_queues );

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[queue_index];
    if ( !rocketnet_platform_mutex_try_acquire( &queue->receive_mutex ) )
    {
        return false;
    }
    const bool ready = queue->ready_buffer && queue->ready_buffer->num_packets > 0;
    rocketnet_platform_mutex_release( &queue->receive_mutex );

    return ready;
}

#endif // #if ROCKETNET_DEV

// the queue a destination's packets leave through: the XDP transport's hash of
// the address bytes and port (rocketnet_server_socket_send_queue in
// sdk/rocketnet_server_socket_linux.cpp, built only with XDP; a unit test
// checks the two agree), so every packet to one client is staged on, and
// sent from, one queue in the order it was staged.
// proofs/tla/RNGsoQueueBankLayout.tla QueueOf.

int rocketnet_server_gso_send_queue( const rocketnet_address_t * to, int num_queues )
{
    rocketnet_assert( to );
    rocketnet_assert( num_queues >= 1 );

    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t * address = reinterpret_cast<const uint8_t *>( &to->data );
    const int address_bytes = to->type == ROCKETNET_ADDRESS_IPV6 ? 16 : 4;
    for ( int i = 0; i < address_bytes; i++ )
    {
        hash ^= address[i];
        hash *= 0x100000001B3ULL;
    }

    hash ^= (uint8_t) ( to->port & 0xFF );
    hash *= 0x100000001B3ULL;
    hash ^= (uint8_t) ( to->port >> 8 );
    hash *= 0x100000001B3ULL;
    return (int) ( hash % (uint64_t) num_queues );
}

// wakes a queue's worker: an eventfd add, which never blocks (the counter
// cannot reach its limit at one add an update). The test fixture has none.
static void rocketnet_server_gso_wake( struct rocketnet_server_gso_queue_t * queue )
{
    if ( queue->wake_fd < 0 )
    {
        return;
    }
    const uint64_t one = 1;
    const ssize_t written = write( queue->wake_fd, &one, sizeof( one ) );
    (void) written;
}

// proofs/tla/RNGsoHandoff.tla TxMainLock and TxPublish, per queue: main's frame
// boundary try-publishes exactly one complete bank of this queue. All fill
// threads have joined; no worker can see the producer until the exchange
// completes. A contended mutex or an empty free FIFO retains the producer:
// main never waits. RNGsoHandoff.tla TxMainWait has no such step here: the
// try-acquire below is the only acquire on main's path, so a held lane mutex
// defers the publish to the next frame instead of blocking. The releases that
// end each exchange are RNGsoHandoff.tla Unlock.
// proofs/tla/RNBankFifoLayout.tla Publish: the free head and the ready tail, each % ROCKETNET_GSO_TX_BANKS.
// proofs/tla/RNGsoQueueBankLayout.tla Flush: the producer turns over only onto a
// free bank, and the turn (the packet id's high part) counts it.
// Returns 1 when a bank was published, 0 when the producer is empty, -1 when
// it was retained.
static int rocketnet_server_gso_publish_send( rocketnet_server_gso_t * gso, struct rocketnet_server_gso_queue_t * queue )
{
    struct rocketnet_server_gso_send_lane_t * lane = &queue->send;
    if ( gso->stopping || lane->producer < 0 )
    {
        return -1;
    }
    if ( lane->banks[lane->producer].num_packets.load( std::memory_order_relaxed ) == 0 )
    {
        return 0;
    }
    if ( !rocketnet_platform_mutex_try_acquire( &lane->mutex ) )
    {
        return -1;
    }
    if ( lane->free_count == 0 )
    {
        rocketnet_platform_mutex_release( &lane->mutex );
        return -1;
    }

    const int old = lane->producer;
    lane->producer = lane->free_banks[lane->free_head];
    lane->free_head = ( lane->free_head + 1 ) % ROCKETNET_GSO_TX_BANKS;
    --lane->free_count;
    lane->ready[( lane->ready_head + lane->ready_count ) % ROCKETNET_GSO_TX_BANKS] = old;
    ++lane->ready_count;
    rocketnet_platform_mutex_release( &lane->mutex );

    lane->turn++;

    // Returned banks were reset by their previous owner before publication.

    rocketnet_assert( lane->banks[lane->producer].num_packets.load( std::memory_order_relaxed ) == 0 );
    return 1;
}

// main: publish every queue's producer and wake each worker that has a bank to
// send, or holds one a full socket refused
static void rocketnet_server_gso_publish_all( rocketnet_server_gso_t * gso )
{
    for ( int i = 0; i < gso->num_queues; i++ )
    {
        struct rocketnet_server_gso_queue_t * queue = &gso->queues[i];
        const int published = rocketnet_server_gso_publish_send( gso, queue );
        if ( published > 0 || queue->send.full.load( std::memory_order_acquire ) )
        {
            rocketnet_server_gso_wake( queue );
        }
    }
}

// proofs/tla/RNGsoHandoff.tla TxWorkerLock, TxTake and TxTakeNone, per queue:
// FIFO ready banks precede the final unpublished producer at stop. Quit is
// observed with ready emptiness under the handoff lock, so an earlier empty
// observation cannot strand a later publication. Stop's release transfers the
// last producer only after main has stopped filling or publishing it.
// proofs/tla/RNBankFifoLayout.tla Take: the ready head advances % ROCKETNET_GSO_TX_BANKS.
static int rocketnet_server_gso_take_send( struct rocketnet_server_gso_queue_t * queue, bool * final )
{
    struct rocketnet_server_gso_send_lane_t * lane = &queue->send;
    *final = false;

    int index = -1;
    rocketnet_platform_mutex_acquire( &lane->mutex );

    if ( lane->ready_count > 0 )
    {
        index = lane->ready[lane->ready_head];
        lane->ready_head = ( lane->ready_head + 1 ) % ROCKETNET_GSO_TX_BANKS;
        --lane->ready_count;
    }
    else if ( queue->quit.load( std::memory_order_acquire ) )
    {
        index = lane->producer;
        lane->producer = -1;
        *final = true;
    }

    rocketnet_platform_mutex_release( &lane->mutex );

    return index;
}

// proofs/tla/RNGsoHandoff.tla TxReturn: a bank rejoins the free FIFO only after
// its synchronous sends finished (the worker cleared it first).
// proofs/tla/RNGsoQueueBankLayout.tla WorkerReturn: the bank's counter is reset
// before it is free, so the producer it next becomes starts at slot 0.
// proofs/tla/RNBankFifoLayout.tla Release: the free tail ( free_head + free_count ) % ROCKETNET_GSO_TX_BANKS.
static void rocketnet_server_gso_return_send( struct rocketnet_server_gso_queue_t * queue, int index )
{
    struct rocketnet_server_gso_send_lane_t * lane = &queue->send;
    lane->banks[index].num_packets.store( 0, std::memory_order_relaxed );
    rocketnet_platform_mutex_acquire( &lane->mutex );
    lane->free_banks[( lane->free_head + lane->free_count ) % ROCKETNET_GSO_TX_BANKS] = index;
    ++lane->free_count;
    rocketnet_platform_mutex_release( &lane->mutex );
}

// a worker-owned lane counter: one writer, so a load and a store, never a
// read-modify-write; main reads it relaxed, and send_idle's lane mutex orders
// the last bank's counts before main sees the bank returned
static void rocketnet_server_gso_lane_add( std::atomic<uint64_t> * counter, uint64_t amount )
{
    counter->store( counter->load( std::memory_order_relaxed ) + amount, std::memory_order_relaxed );
}

// lays a lane's gather scratch over its own memory, once, when its queue is made
static void rocketnet_server_gso_gather_init( struct rocketnet_server_gso_send_lane_t * lane )
{
    rocketnet_assert( rocketnet_send_gather_scratch_bytes( ROCKETNET_GSO_SEND_QUEUE_SIZE ) <= sizeof( lane->gather_memory ) );
    const bool laid_out = rocketnet_send_gather_scratch_init( &lane->gather, lane->gather_memory, ROCKETNET_GSO_SEND_QUEUE_SIZE );
    rocketnet_assert( laid_out );
    (void) laid_out;
}

// groups a taken bank's count slots by destination into lane->order and
// returns the number of positions in it: every slot holding a packet, an
// aborted slot in none. The flush walks positions, not slots.
static int rocketnet_server_gso_gather_bank( rocketnet_server_gso_t * gso, struct rocketnet_server_gso_queue_t * queue,
                                             rocketnet_server_gso_send_buffer_t * bank, int count )
{
    struct rocketnet_server_gso_send_lane_t * lane = &queue->send;
    const bool use_gso = ( gso->features.load( std::memory_order_relaxed ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0;
    const int num_runs = rocketnet_send_gather( &bank->info[0].to, sizeof( bank->info[0] ),
                                                &bank->info[0].packet_bytes, sizeof( bank->info[0] ), count,
                                                use_gso ? ROCKETNET_GSO_MAX_SEGMENTS : 1, ROCKETNET_GSO_MAX_SUPER_BYTES,
                                                &lane->gather, lane->order, lane->runs );
    rocketnet_assert( num_runs >= 0 );
    int positions = 0;
    for ( int i = 0; i < num_runs; i++ )
    {
        positions += lane->runs[i].count;
    }
    return positions;
}

static int rocketnet_server_gso_flush( rocketnet_server_gso_t * gso, struct rocketnet_server_gso_queue_t * queue,
                                       rocketnet_server_gso_send_buffer_t * bank, int first_slot, int num_slots, bool * disable_gso, bool * full );

// proofs/tla/RNGsoHandoff.tla TxSend: send a held bank from *slot on the
// queue's own socket. One live GSO refusal resends that run plain. Refusals by
// the threshold of destinations with none accepted turn segmentation off and
// resend from the refused message.
// proofs/tla/RNGsoHandoff.tla TxSendFull: a full socket stops the bank at its
// first unsent slot and returns false, the rest kept in order in the held bank
// for the next attempt.
static bool rocketnet_server_gso_send_bank( rocketnet_server_gso_t * gso, struct rocketnet_server_gso_queue_t * queue,
                                            rocketnet_server_gso_send_buffer_t * bank, int * slot, int count )
{
    while ( *slot < count )
    {
        bool disable_gso = false;
        bool full = false;
        *slot = rocketnet_server_gso_flush( gso, queue, bank, *slot, count, &disable_gso, &full );
        if ( disable_gso )
        {
            gso->features.fetch_and( ~ROCKETNET_SERVER_GSO_FEATURE_SEGMENT, std::memory_order_relaxed );
        }
        if ( full )
        {
            return false;
        }
    }
    return true;
}

// One send step of a queue's worker (proofs/tla/RNGsoHandoff.tla TxSend, per
// queue): send the bank it holds from where a full socket stopped it, then
// every bank waiting in the queue's FIFO, oldest first, each returned once its
// sends finished. A full socket keeps the bank's unsent tail held, ahead of
// every later bank, and the step returns; the worker tries again when main's
// next update wakes it, the socket reports room, or its wait times out. Once
// shutdown has begun each further attempt counts, and past
// ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT of them the tail is dropped and counted
// so a socket that stays full cannot keep send_idle false (the server asks for
// stop only once it is true) or the queue from stopping. Returns true when
// stop's final bank has been sent: the worker's send side is done.
static bool rocketnet_server_gso_send_step( rocketnet_server_gso_t * gso, struct rocketnet_server_gso_queue_t * queue )
{
    struct rocketnet_server_gso_send_lane_t * lane = &queue->send;

    for ( ;; )
    {
        if ( lane->held < 0 )
        {
            bool final = false;
            const int index = rocketnet_server_gso_take_send( queue, &final );
            if ( index < 0 )
            {
                return final;
            }
            int count = lane->banks[index].num_packets.load( std::memory_order_relaxed );
            if ( count > ROCKETNET_GSO_SEND_QUEUE_SIZE )
            {
                count = ROCKETNET_GSO_SEND_QUEUE_SIZE;
            }
            lane->held = index;
            lane->held_slot = 0;
            lane->held_count = rocketnet_server_gso_gather_bank( gso, queue, &lane->banks[index], count );
            lane->held_final = final;
            lane->retries_after_quit = 0;
        }

        rocketnet_server_gso_send_buffer_t * bank = &lane->banks[lane->held];
        const int before = lane->held_slot;
        if ( !rocketnet_server_gso_send_bank( gso, queue, bank, &lane->held_slot, lane->held_count ) )
        {
            lane->full.store( true, std::memory_order_release );
            if ( !gso->send_shutdown.load( std::memory_order_acquire ) )
            {
                return false;
            }

            // only a socket that stays full loses its tail: an attempt that
            // sent something starts the count again

            if ( lane->held_slot > before )
            {
                lane->retries_after_quit = 0;
                return false;
            }
            lane->retries_after_quit++;
            if ( lane->retries_after_quit <= ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT )
            {
                return false;
            }

            // the tail the socket never took is dropped: count its packets.
            // an aborted slot, or one already counted unroutable and cleared,
            // holds none

            uint64_t dropped = 0;
            for ( int i = lane->held_slot; i < lane->held_count; i++ )
            {
                if ( bank->info[lane->order[i]].packet_bytes > 0 )
                {
                    dropped++;
                }
            }
            rocketnet_server_gso_lane_add( &lane->counter_send_dropped, dropped );
        }
        lane->full.store( false, std::memory_order_release );

        const int index = lane->held;
        const bool final = lane->held_final;
        lane->held = -1;
        if ( final )
        {
            return true;
        }
        rocketnet_server_gso_return_send( queue, index );
    }
}

// ---------------------------------------------------------------------------

// address <-> sockaddr at the boundary, the platform socket's own
// conversions: ports and address words are network order in the sockaddr and
// host order in rocketnet_address_t, and a v4-mapped source on a dual stack
// socket becomes native ipv4 exactly once, here.

static socklen_t rocketnet_server_gso_address_to_sockaddr( const rocketnet_address_t * input, bool socket_ipv6, struct sockaddr_storage * out )
{
    rocketnet_address_t to = *input;

    if ( socket_ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        struct sockaddr_in6 * addr = (struct sockaddr_in6 *) out;
        memset( addr, 0, sizeof( struct sockaddr_in6 ) );
        addr->sin6_family = AF_INET6;

        for ( int i = 0; i < 8; i++ )
        {
            ( (uint16_t *) &addr->sin6_addr )[i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }

        addr->sin6_port = rocketnet_platform_htons( to.port );
        return sizeof( struct sockaddr_in6 );
    }
    else
    {
        if ( to.type != ROCKETNET_ADDRESS_IPV4 )
        {
            return 0;
        }

        rocketnet_platform_address_to_sockaddr_in( &to, (struct sockaddr_in *) out );
        return sizeof( struct sockaddr_in );
    }
}

static bool rocketnet_server_gso_sockaddr_to_address( const struct sockaddr_storage * input, bool socket_ipv6, rocketnet_address_t * out )
{
    if ( input->ss_family == AF_INET6 )
    {
        const struct sockaddr_in6 * addr = (const struct sockaddr_in6 *) input;
        out->type = ROCKETNET_ADDRESS_IPV6;

        for ( int i = 0; i < 8; i++ )
        {
            out->data.ipv6[i] = rocketnet_platform_ntohs( ( (const uint16_t *) &addr->sin6_addr )[i] );
        }

        out->port = rocketnet_platform_ntohs( addr->sin6_port );

        // an ipv6 gso socket is always dual stack (IPV6_V6ONLY is cleared at
        // open), so a mapped source is an ipv4 client: canonicalise it. an
        // ipv4 socket never sees AF_INET6, so a mapped source there drops.

        return rocketnet_address_accept_received_source( out, socket_ipv6 );
    }
    else if ( input->ss_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( (const struct sockaddr_in *) input, out );
        return true;
    }

    return false;
}
// the grant of one buffer in the bytes requested: linux reports double its
// grant for its own bookkeeping, so this halves it. 0 when unreadable
static int rocketnet_server_gso_buffer_granted( int fd, int option )
{
    int granted = 0;
    socklen_t optlen = sizeof( granted );
    if ( getsockopt( fd, SOL_SOCKET, option, &granted, &optlen ) != 0 )
    {
        return 0;
    }
    return granted / 2;
}

// one buffer after the shared open's plain request (rocketnet_server_udp_open):
// the grant read back, and when short the force option once
// (rocketnet_server_gso_buffer_step). A refused force is a fact for the start
// line, kept in force_error
static void rocketnet_server_gso_force_buffer( int fd, bool receive, int requested_bytes, int * granted_bytes, bool * force_tried, int * force_error )
{
    const int option = receive ? SO_RCVBUF : SO_SNDBUF;
    const int force_option = receive ? SO_RCVBUFFORCE : SO_SNDBUFFORCE;

    *force_tried = false;
    *force_error = 0;
    *granted_bytes = rocketnet_server_gso_buffer_granted( fd, option );

    if ( rocketnet_server_gso_buffer_step( requested_bytes, *granted_bytes, false ) == ROCKETNET_SERVER_GSO_BUFFER_FORCE )
    {
        *force_tried = true;
        int size = requested_bytes;
        if ( setsockopt( fd, SOL_SOCKET, force_option, &size, sizeof( size ) ) != 0 )
        {
            *force_error = errno;
        }
        *granted_bytes = rocketnet_server_gso_buffer_granted( fd, option );
    }
}

bool rocketnet_server_gso_buffer_note( char * line, int line_bytes, bool receive, int requested_bytes, int granted_bytes, bool force_tried, int force_error )
{
    rocketnet_assert( line );
    rocketnet_assert( line_bytes > 0 );

    const char * direction = receive ? "receive" : "send";
    const char * force_name = receive ? "SO_RCVBUFFORCE" : "SO_SNDBUFFORCE";
    const char * limit = receive ? "net.core.rmem_max" : "net.core.wmem_max";

    if ( rocketnet_server_gso_buffer_step( requested_bytes, granted_bytes, true ) == ROCKETNET_SERVER_GSO_BUFFER_GRANTED )
    {
        if ( force_tried )
        {
            (void) snprintf( line, (size_t) line_bytes, "gso transport: socket %s buffer asked %d bytes, got %d (past %s with %s)", direction, requested_bytes,
                             granted_bytes, limit, force_name );
        }
        else
        {
            (void) snprintf( line, (size_t) line_bytes, "gso transport: socket %s buffer asked %d bytes, got %d", direction, requested_bytes, granted_bytes );
        }
        return false;
    }

    char force_text[96];
    if ( !force_tried )
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s not tried", force_name );
    }
    else if ( force_error == EPERM )
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s refused: the process lacks CAP_NET_ADMIN", force_name );
    }
    else if ( force_error != 0 )
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s refused: errno %d", force_name, force_error );
    }
    else
    {
        (void) snprintf( force_text, sizeof( force_text ), "%s taken and still short", force_name );
    }

    const char * consequence =
        receive ? "the kernel drops datagrams that arrive past the grant between updates" : "sends wait on a full socket";

    (void) snprintf( line, (size_t) line_bytes, "gso transport: socket %s buffer asked %d bytes, got %d (%s); %s: set sysctl %s=%d", direction, requested_bytes,
                     granted_bytes, force_text, consequence, limit, requested_bytes );
    return true;
}


// ---------------------------------------------------------------------------

// proofs/tla/RNGsoHandoff.tla RxFill: the receive worker drains the socket with
// recvmmsg, staging each segment into its own buffer outside the mutex.
// the receive worker: drain the socket with recvmmsg, split GRO runs at the
// kernel-reported segment stride, stage each segment into the worker buffer.
// A brief ownership lock publishes the completed buffer to the main thread
// and takes a free one. Socket I/O, GRO splitting, copying, and counters run
// outside the lock. While a ready buffer waits, the worker continues filling
// its own buffer; once full, it stops reading until the main thread returns
// a free buffer, retaining the queue's bounded backpressure behavior.

// The GRO stride of one recvmmsg message. A truncated control buffer, or a
// UDP_GRO cmsg whose length does not hold an int, is one fault and the
// message is one datagram. No socket: the caller passes the message header.
int rocketnet_server_gso_gro_segment_bytes( struct msghdr * header, int message_bytes, uint64_t * faults )
{
    int segment_bytes = message_bytes;
    if ( !header )
    {
        return segment_bytes;
    }
    if ( header->msg_flags & MSG_CTRUNC )
    {
        if ( faults )
        {
            *faults += 1;
        }
        return message_bytes;
    }

    for ( struct cmsghdr * cmsg = CMSG_FIRSTHDR( header ); cmsg != NULL; cmsg = CMSG_NXTHDR( header, cmsg ) )
    {
        if ( cmsg->cmsg_len < sizeof( struct cmsghdr ) )
        {
            if ( faults )
            {
                *faults += 1;
            }
            return message_bytes;
        }

        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_GRO )
        {
            if ( cmsg->cmsg_len < CMSG_LEN( sizeof( int ) ) )
            {
                if ( faults )
                {
                    *faults += 1;
                }
                return message_bytes;
            }

            memcpy( &segment_bytes, CMSG_DATA( cmsg ), sizeof( segment_bytes ) );
            break;
        }
    }

    return segment_bytes;
}

// proofs/tla/RNGsoHandoff.tla RxPublish: the worker's filled buffer becomes the
// ready buffer and it takes the free buffer, under the mutex, only when no
// ready buffer waits and a free buffer exists; otherwise nothing moves and
// the worker retains its buffer (the queue's bounded backpressure). The
// defect WriteHeld has no such step: the guard below is what excludes it.
// proofs/tla/RNGsoRecvFill.tla Publish: the worker fills a free buffer from slot 0;
// Done is a drained socket whose last buffer is published.
static bool rocketnet_server_gso_publish_receive( struct rocketnet_server_gso_queue_t * queue )
{
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    rocketnet_platform_mutex_acquire( &queue->receive_mutex );

    const bool published = !queue->ready_buffer && queue->free_buffer;
    if ( published )
    {
        queue->ready_buffer = buffer;
        queue->worker_buffer = queue->free_buffer;
        queue->free_buffer = NULL;
    }

    rocketnet_platform_mutex_release( &queue->receive_mutex );

    return published;
}

// the worker's screen of one datagram before it takes a bank slot. First the
// size pass every transport makes, below the smallest packet a server reads
// or above the receive ceiling, so a size drop reads the same on every
// transport (PREFILTER_TOO_SHORT or _TOO_LARGE and the transport's
// RECEIVE_DROPPED_SIZE); then, when the transport has the packet filter, the
// server's stateless prefilter (sdk/rocketnet_receive_prefilter.h), so
// garbage never evicts a player's packet and is counted exactly as main's
// check sequence would count it.
static enum rocketnet_receive_drop_t rocketnet_server_gso_screen( const struct rocketnet_server_gso_t * gso, const uint8_t * packet_data, int packet_bytes )
{
    if ( packet_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
    {
        return ROCKETNET_RECEIVE_DROP_TOO_SHORT;
    }
    if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
    {
        return ROCKETNET_RECEIVE_DROP_TOO_LARGE;
    }
    if ( gso->has_filter )
    {
        return rocketnet_receive_prefilter( packet_data, packet_bytes, gso->packet_filter );
    }
    return ROCKETNET_RECEIVE_ACCEPT;
}

// one dropped datagram, counted by reason in the queue's tally (the server's
// counters) and in the transport's own counters: size reasons in
// RECEIVE_DROPPED_SIZE, a full bank in RECEIVE_DROPPED_OVERFLOW, the rest in
// RECEIVE_DROPPED_PREFILTER
static void rocketnet_server_gso_count_drop( struct rocketnet_server_gso_queue_t * queue, struct rocketnet_server_gso_recv_buffer_t * buffer,
                                             enum rocketnet_receive_drop_t drop )
{
    rocketnet_receive_drop_tally_count( &queue->receive_tally, drop );
    switch ( drop )
    {
        case ROCKETNET_RECEIVE_DROP_TOO_SHORT:
            buffer->dropped_size++;
            break;
        case ROCKETNET_RECEIVE_DROP_TOO_LARGE:
            buffer->dropped_size++;
            buffer->dropped_too_large++;
            break;
        case ROCKETNET_RECEIVE_DROP_BANK_FULL:
            buffer->dropped_overflow++;
            break;
        case ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED:
            buffer->dropped_source++;
            break;
        default:
            buffer->dropped_prefilter++;
            break;
    }
}

// One drain of the socket into the worker's buffer. Every message recvmmsg
// returns spends one read of a budget of ROCKETNET_GSO_RECV_QUEUE_SIZE,
// accepted or dropped, and so does an interrupted call: the drain ends when
// the budget is spent, the buffer has no room for one more message, the
// socket is empty or a call fails, so no stream of discarded datagrams holds
// the worker away from its publish and its quit test. Each call asks for at
// most the messages whose every segment fits (rocketnet_server_gso_recv_vlen),
// so nothing read is dropped for want of room.
// proofs/tla/RNGsoRecvFill.tla Read: one message of a call, its segments
// staged from the fill index; Vlen is rocketnet_server_gso_recv_vlen, and
// Arrive is the kernel queueing a GRO run on the socket between calls.

static bool rocketnet_server_gso_drain( struct rocketnet_server_gso_queue_t * queue, bool * full )
{
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    const bool gro = ( queue->gso->features.load( std::memory_order_relaxed ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) != 0;

    int reads = 0;
    *full = false;

    while ( reads < ROCKETNET_GSO_RECV_QUEUE_SIZE )
    {
        const int vlen = rocketnet_server_gso_recv_vlen( (int) buffer->num_packets, gro );
        if ( vlen == 0 )
        {
            *full = true;
            break;                              // no room for one more message: the socket keeps the rest
        }

        for ( int i = 0; i < vlen; i++ )
        {
            queue->iovecs[i].iov_base = queue->scratch[i];
            queue->iovecs[i].iov_len = ROCKETNET_GSO_RECV_BUFFER_BYTES;
            memset( &queue->messages[i].msg_hdr, 0, sizeof( struct msghdr ) );
            queue->messages[i].msg_hdr.msg_name = &queue->names[i];
            queue->messages[i].msg_hdr.msg_namelen = sizeof( struct sockaddr_storage );
            queue->messages[i].msg_hdr.msg_iov = &queue->iovecs[i];
            queue->messages[i].msg_hdr.msg_iovlen = 1;
            queue->messages[i].msg_hdr.msg_control = queue->controls[i];
            queue->messages[i].msg_hdr.msg_controllen = sizeof( queue->controls[i] );
        }

        int count;
#if ROCKETNET_DEV
        int pending = queue->gso->test_fail_receives.load();
        if ( pending > 0 && queue->gso->test_fail_receives.compare_exchange_strong( pending, pending - 1 ) )
        {
            count = -1;
            errno = queue->gso->test_fail_receive_error.load();
        }
        else if ( queue->gso->test_receive )
        {
            count = queue->gso->test_receive( queue, vlen, queue->gso->test_receive_user );
        }
        else
#endif
        {
            count = recvmmsg( queue->fd, queue->messages, (unsigned int) vlen, MSG_DONTWAIT, NULL );
        }

        // every recvmmsg counts, empty ones included -- an instrument
        // that hides the misses cannot argue the syscall win. the 1ms
        // poll wakeups are not counted; they are the worker's idle
        // heartbeat, not the receive path.

        queue->receive_syscalls.fetch_add( 1, std::memory_order_relaxed );

        reads += count > 0 ? count : 1;

        // a failed call other than an empty socket is counted; the
        // worker sleeps 1ms and tries again. an interrupted call
        // (EINTR) is retried without counting as an error.

        if ( count < 0 )
        {
            if ( errno == EINTR )
            {
                if ( queue->quit.load( std::memory_order_relaxed ) )
                {
                    break;
                }
                continue;
            }

            if ( errno != EAGAIN && errno != EWOULDBLOCK )
            {
                buffer->receive_errors++;
                return true;
            }

            break;
        }

        if ( count == 0 )
        {
            break;
        }

        for ( int i = 0; i < count; i++ )
        {
            const int message_bytes = (int) queue->messages[i].msg_len;

            // a truncated message would split into a plausible but
            // short final segment -- corruption, not loss -- so it is
            // dropped whole and counted. the buffer holds any UDP
            // payload, so this fires only if a future kernel coalesces
            // past 64KB.

            if ( queue->messages[i].msg_hdr.msg_flags & MSG_TRUNC )
            {
                rocketnet_server_gso_count_drop( queue, buffer, ROCKETNET_RECEIVE_DROP_TOO_LARGE );
                continue;
            }

            // a zero-length datagram is below the smallest packet a
            // server reads: the size pass counts it, as io_uring's does

            if ( message_bytes == 0 )
            {
                rocketnet_server_gso_count_drop( queue, buffer, ROCKETNET_RECEIVE_DROP_TOO_SHORT );
                continue;
            }

            // the GRO stride. a truncated control buffer, or a UDP_GRO
            // cmsg that does not hold an int, is one fault and the
            // message is one datagram.

            uint64_t gro_faults = 0;
            int segment_bytes = rocketnet_server_gso_gro_segment_bytes( &queue->messages[i].msg_hdr, message_bytes, &gro_faults );
            buffer->gro_cmsg_faults += gro_faults;
            if ( segment_bytes <= 0 )
            {
                segment_bytes = message_bytes;
            }

            rocketnet_address_t from;
            if ( !rocketnet_server_gso_sockaddr_to_address( &queue->names[i], queue->ipv6, &from ) )
            {
                // a source family the boundary cannot express is dropped
                // counted, never silently: every datagram of the message
                for ( int offset = 0; offset < message_bytes; offset += segment_bytes )
                {
                    rocketnet_server_gso_count_drop( queue, buffer, ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED );
                }
                continue;
            }

            if ( segment_bytes < message_bytes )
            {
                buffer->gro_batches++;
            }

            for ( int offset = 0; offset < message_bytes; offset += segment_bytes )
            {
                const int remaining = message_bytes - offset;
                const int this_segment = remaining < segment_bytes ? remaining : segment_bytes;

                // the prefilter before banking, once per GRO segment: a
                // datagram main would refuse on its stateless rungs never
                // takes a slot (rocketnet_server_gso_screen)

                const uint8_t * segment_data = queue->scratch[i] + offset;
                const enum rocketnet_receive_drop_t drop = rocketnet_server_gso_screen( queue->gso, segment_data, this_segment );
                if ( drop != ROCKETNET_RECEIVE_ACCEPT )
                {
                    rocketnet_server_gso_count_drop( queue, buffer, drop );
                    continue;
                }

                if ( buffer->num_packets >= ROCKETNET_GSO_RECV_QUEUE_SIZE )
                {
                    rocketnet_server_gso_count_drop( queue, buffer, ROCKETNET_RECEIVE_DROP_BANK_FULL );
                    continue;
                }

                const uint32_t index = buffer->num_packets;
                buffer->from[index] = from;
                buffer->packet_bytes[index] = this_segment;
                memcpy( buffer->packet_data[index], segment_data, (size_t) this_segment );
                buffer->num_packets++;
            }
        }

        if ( count < vlen )
        {
            break;                              // the socket is drained; go back to poll
        }
    }

    return false;
}

// proofs/tla/RNGsoHandoff.tla RxStall and RxSleep: a pass whose buffer is full
// and could not be published, or whose receive failed, returns true and the
// worker sleeps 1ms before it reads again instead of polling a readable socket.

static bool rocketnet_server_gso_worker_step( struct rocketnet_server_gso_queue_t * queue )
{
    bool full = false;
    const bool failed = rocketnet_server_gso_drain( queue, &full );

    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;

    const bool has_results = buffer->num_packets || buffer->dropped_size || buffer->dropped_overflow ||
                             buffer->dropped_source || buffer->dropped_prefilter || buffer->gro_batches || buffer->gro_cmsg_faults ||
                             buffer->receive_errors;
    if ( !has_results )
    {
        return failed;
    }

    const bool published = rocketnet_server_gso_publish_receive( queue );

    return failed || ( full && !published );
}

// drains the queue's eventfd so the next poll waits again; the wake is only a
// prompt to look, the state it points at is read under the hand-off mutex
static void rocketnet_server_gso_clear_wake( struct rocketnet_server_gso_queue_t * queue )
{
    uint64_t value = 0;
    const ssize_t read_bytes = read( queue->wake_fd, &value, sizeof( value ) );
    (void) read_bytes;
}

// the affinity and priority calls a worker makes on its own thread. The
// worker passes the real ones; the development seam
// (rocketnet_server_gso_test_place_worker) passes fakes, so the refusal paths
// run without changing any thread's affinity or priority

struct rocketnet_gso_affinity_calls_t
{
    int ( *get )( cpu_set_t * mask );
    int ( *set )( const cpu_set_t * mask );
    int ( *set_round_robin )( int priority );   // SCHED_RR on the calling thread
};

static int rocketnet_gso_affinity_get_real( cpu_set_t * mask )
{
    return pthread_getaffinity_np( pthread_self(), sizeof( cpu_set_t ), mask );
}

static int rocketnet_gso_affinity_set_real( const cpu_set_t * mask )
{
    return pthread_setaffinity_np( pthread_self(), sizeof( cpu_set_t ), mask );
}

static int rocketnet_gso_set_round_robin_real( int priority )
{
    struct sched_param param;
    param.sched_priority = priority;
    return pthread_setschedparam( pthread_self(), SCHED_RR, &param );
}

static const struct rocketnet_gso_affinity_calls_t rocketnet_gso_affinity_real = { rocketnet_gso_affinity_get_real, rocketnet_gso_affinity_set_real,
                                                                                   rocketnet_gso_set_round_robin_real };

// one warning for a worker left on a mask that holds cpu 0, naming the queue
// and the mask it is left on (read back; "unknown" when it cannot be read)
static void rocketnet_server_gso_warn_left_on_core_zero( int queue_index, const struct rocketnet_gso_affinity_calls_t * calls, const char * why )
{
    char cpus[128];
    cpu_set_t mask;
    CPU_ZERO( &mask );
    if ( calls->get( &mask ) == 0 )
    {
        unsigned char in_mask[CPU_SETSIZE];
        for ( int cpu = 0; cpu < CPU_SETSIZE; cpu++ )
        {
            in_mask[cpu] = CPU_ISSET( (size_t) cpu, &mask ) ? 1 : 0;
        }
        rocketnet_platform_cpu_list_text( in_mask, CPU_SETSIZE, cpus, (int) sizeof( cpus ) );
    }
    else
    {
        snprintf( cpus, sizeof( cpus ), "%s", "unknown" );
    }
    rocketnet_warn( "gso worker %d left on cpus %s, cpu 0 among them: %s; it asks for no real-time scheduling", queue_index, cpus, why );
}

// A refused pin leaves the worker on the mask it inherited, which may include
// cpu 0, so it is confined to the online cpus from num_os_cpus up
// (rocketnet_gso_pin_fallback). true when it is; false when it is not (no
// cpu but 0 is online, or the confinement refused): counted in pin_refusals
// (GSO_PIN_REFUSALS counts the failed confinements after a refused pin too)
// with one warning naming the queue and the mask it is left on
static bool rocketnet_server_gso_confine_refused( struct rocketnet_server_gso_t * gso, int queue_index, int online, const struct rocketnet_gso_affinity_calls_t * calls )
{
    unsigned char allowed[CPU_SETSIZE];
    if ( rocketnet_gso_pin_fallback( allowed, CPU_SETSIZE, online, gso->num_os_cpus ) == 0 )
    {
        gso->pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        rocketnet_server_gso_warn_left_on_core_zero( queue_index, calls, "no cpu but cpu 0 is online" );
        return false;
    }
    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    for ( int cpu = 0; cpu < CPU_SETSIZE; cpu++ )
    {
        if ( allowed[cpu] )
        {
            CPU_SET( (size_t) cpu, &cpuset );
        }
    }
    const int result = calls->set( &cpuset );
    if ( result != 0 )
    {
        gso->pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        char why[192];
        snprintf( why, sizeof( why ), "confining it away from the OS's cpus was refused (%s)", rocketnet_platform_socket_error_text( result ).text );
        rocketnet_server_gso_warn_left_on_core_zero( queue_index, calls, why );
        return false;
    }
    return true;
}

// the worker pins itself the way the XDP transport's thread does
// (rocketnet_server_socket_xdp_thread_function): affinity to cpu num_os_cpus +
// queue. A refusal is a warning naming the queue and the cpu, counted in
// pin_refusals (rocketnet_server_gso_pin_refusals), and the queue's pin_cpu
// stays NONE: a worker is reported pinned only when pthread_setaffinity_np
// said so. num_os_cpus below 1 reads as 1 (the OS keeps cpu 0). true when the
// worker is kept off cpu 0 (pinned, or confined after a refused pin).

static bool rocketnet_server_gso_pin_worker( struct rocketnet_server_gso_queue_t * queue, int online, const struct rocketnet_gso_affinity_calls_t * calls )
{
    struct rocketnet_server_gso_t * gso = queue->gso;

    const int cpu = rocketnet_gso_pin_cpu( queue->index, gso->num_os_cpus );
    const int check = rocketnet_gso_pin_check( cpu, CPU_SETSIZE, online );
    if ( check != ROCKETNET_GSO_PIN_OK )
    {
        gso->pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        if ( check == ROCKETNET_GSO_PIN_OUTSIDE_SET )
        {
            rocketnet_warn( "gso worker %d not pinned: cpu %d is outside 0 .. %d; it runs on the cpus from num_os_cpus up", queue->index, cpu, CPU_SETSIZE - 1 );
        }
        else
        {
            rocketnet_warn( "gso worker %d not pinned: cpu %d is not online (0 .. %d); it runs on the cpus from num_os_cpus up", queue->index, cpu, online - 1 );
        }
        const bool off_core_zero = rocketnet_server_gso_confine_refused( gso, queue->index, online, calls );
        queue->left_on_core_zero.store( !off_core_zero, std::memory_order_relaxed );
        queue->pin_cpu.store( ROCKETNET_GSO_PIN_NONE, std::memory_order_release );
        return off_core_zero;
    }

    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    CPU_SET( (size_t) cpu, &cpuset );
    const int result = calls->set( &cpuset );
    if ( result != 0 )
    {
        gso->pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        rocketnet_warn( "gso worker %d not pinned: pthread_setaffinity_np cpu %d refused (%s); it runs on the cpus from num_os_cpus up", queue->index, cpu, rocketnet_platform_socket_error_text( result ).text );
        const bool off_core_zero = rocketnet_server_gso_confine_refused( gso, queue->index, online, calls );
        queue->left_on_core_zero.store( !off_core_zero, std::memory_order_relaxed );
        queue->pin_cpu.store( ROCKETNET_GSO_PIN_NONE, std::memory_order_release );
        return off_core_zero;
    }

    rocketnet_info( "gso worker %d pinned to cpu %d", queue->index, cpu );
    queue->left_on_core_zero.store( false, std::memory_order_relaxed );
    queue->pin_cpu.store( cpu, std::memory_order_release );
    return true;
}

// The worker's placement at start: the pin, then SCHED_RR at the network
// threads' priority (rocketnet_platform_network_thread_rr_priority) on itself
// when the priority switch is on, and only when it is kept off cpu 0. The
// worker sets its own priority after its pin, so a worker left on cpu 0 never
// asks for real-time scheduling; it is flagged (left_on_core_zero) and
// carries on: a one cpu host still runs.

static void rocketnet_server_gso_place_worker( struct rocketnet_server_gso_queue_t * queue, int online, const struct rocketnet_gso_affinity_calls_t * calls )
{
    const bool off_core_zero = rocketnet_server_gso_pin_worker( queue, online, calls );
    if ( off_core_zero && rocketnet_platform_thread_priority_enabled() )
    {
        const int priority = rocketnet_platform_network_thread_rr_priority();
        const int result = calls->set_round_robin( priority );
        if ( result != 0 )
        {
            char call[64];
            snprintf( call, sizeof( call ), "pthread_setschedparam SCHED_RR %d", priority );
            rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_SCHED, "thread priority", call, strerror( result ), "the thread runs at its default priority" );
        }
    }
}

// The queue's one worker (layer L3 of the gso push): it sends this queue's
// banks on this queue's socket and drains the same socket's receives, so the
// kernel-facing work for a queue stays on one thread and main makes no send
// system call. Each pass sends first (main published this frame's banks), then
// drains, then waits for traffic, main's wake, room on a full socket, or 1 ms,
// so quit is honoured promptly. proofs/tla/RNGsoHandoff.tla, per queue: Tx*
// for the send step, Rx* for the drain; quit stops the receives at once and the
// worker leaves only after stop's final bank is sent (TxWorkerExitIdle).
static void rocketnet_server_gso_worker( rocketnet_platform_thread_t *, void * data )
{
    struct rocketnet_server_gso_queue_t * queue = (struct rocketnet_server_gso_queue_t *) data;

    rocketnet_assert( queue );

    struct rocketnet_server_gso_t * gso = queue->gso;

    rocketnet_server_gso_place_worker( queue, (int) sysconf( _SC_NPROCESSORS_ONLN ), &rocketnet_gso_affinity_real );

    struct pollfd fds[2];
    bool stalled = false;

    for ( ;; )
    {
        if ( rocketnet_server_gso_send_step( gso, queue ) )
        {
            break;
        }

        const bool quit = queue->quit.load( std::memory_order_acquire );
        if ( !quit )
        {
            stalled = rocketnet_server_gso_worker_step( queue );
        }

        // the socket's receives are waited for unless the drain stalled (a
        // full buffer main has not claimed: sleep the 1 ms instead of polling
        // a readable socket) or quit was asked. A bank a full socket refused
        // waits for main's wake or the 1 ms, as ROCKETNET_GSO_SEND_RETRY_SECONDS
        // says, never for POLLOUT: a UDP socket reports room while the device
        // queue behind it still refuses (ENOBUFS), and polling it would spin.

        fds[0].fd = queue->fd;
        fds[0].events = ( !quit && !stalled ) ? POLLIN : 0;
        fds[0].revents = 0;
        fds[1].fd = queue->wake_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        poll( fds, 2, (int) ( ROCKETNET_GSO_SEND_RETRY_SECONDS * 1000.0 ) );

        if ( fds[1].revents & POLLIN )
        {
            rocketnet_server_gso_clear_wake( queue );
        }
    }

    // proofs/tla/RNGsoHandoff.tla RxWorkerExit and TxWorkerExitIdle: last access
    // to queue-owned storage; destroy acquires this before join.

    queue->stopped.store( true, std::memory_order_release );
}

// ---------------------------------------------------------------------------

uint64_t rocketnet_server_gso_allocation_bytes( int queues, int max_clients )
{
    if ( queues < 1 || queues > ROCKETNET_GSO_MAX_QUEUES )
    {
        return 0;
    }
    uint64_t bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_gso_t ),
                                                     (uint64_t) queues * sizeof( rocketnet_server_gso_queue_t ) } );
    const uint64_t view = rocketnet_server_process_packets_allocation_bytes( rocketnet_server_receive_packets_per_update( max_clients ) );
    if ( !bytes || !view || !rocketnet_memory_add( bytes, 1, view ) ||
         !rocketnet_memory_add( bytes, (uint64_t) queues, rocketnet_platform_thread_allocation_bytes() ) )
    {
        return 0;
    }
    return bytes <= SIZE_MAX ? bytes : 0;
}

static void rocketnet_server_gso_abort_create( rocketnet_server_gso_t * gso )
{
    // Create is synchronous and never exposes a partially constructed object.
    rocketnet_server_gso_stop( gso );
    while ( !rocketnet_server_gso_stopped( gso ) )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_server_gso_destroy( gso );
}

struct rocketnet_server_gso_t * rocketnet_server_gso_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int * status,
                                                             const uint8_t * packet_filter )
{
    return rocketnet_server_gso_create_pinned( context, bind_address, num_queues, max_clients, flags, 0, status, packet_filter );
}

int rocketnet_server_gso_worker_cpu( struct rocketnet_server_gso_t * gso, int queue )
{
    rocketnet_assert( gso );
    rocketnet_assert( queue >= 0 && queue < gso->num_queues );
    return gso->queues[queue].pin_cpu.load( std::memory_order_acquire );
}

int rocketnet_server_gso_pin_refusals( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return gso->pin_refusals.load( std::memory_order_relaxed );
}

struct rocketnet_server_gso_t * rocketnet_server_gso_create_pinned( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int num_os_cpus,
                                                                    int * status, const uint8_t * packet_filter )
{
#if ROCKETNET_DEV
    flags |= rocketnet_server_gso_test_create_flags.load();

    // development library only: ROCKETNET_TEST_GSO_PLAIN=1 forces the plain
    // tiers (plain sendmmsg, plain recvmmsg) for a whole process, so a tool
    // such as load pins the gso plain cell without calling the hook above

    char test_plain[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
    if ( rocketnet_platform_getenv( "ROCKETNET_TEST_GSO_PLAIN", test_plain, sizeof( test_plain ) ) > 0 && strcmp( test_plain, "1" ) == 0 )
    {
        flags |= ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO;
    }
#endif // #if ROCKETNET_DEV

    rocketnet_assert( bind_address );
    rocketnet_assert( bind_address->type == ROCKETNET_ADDRESS_IPV4 || bind_address->type == ROCKETNET_ADDRESS_IPV6 );

    if ( num_queues <= 0 )
    {
        num_queues = ROCKETNET_GSO_DEFAULT_QUEUES;
    }

    // refuse, never clamp (the validation law): a queue count past this
    // transport's geometry is a configuration error named here, not a silently
    // different server

    if ( num_queues > ROCKETNET_GSO_MAX_QUEUES )
    {
        rocketnet_error( "gso transport num_queues %d is out of range (1 to %d)", num_queues, ROCKETNET_GSO_MAX_QUEUES );
        return NULL;
    }

    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        rocketnet_error( "gso transport max_clients %d is out of range (1 to %d)", max_clients, ROCKETNET_MAX_CLIENTS );
        return NULL;
    }

    struct rocketnet_server_gso_t * gso = (struct rocketnet_server_gso_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_gso_t ) );
    if ( !gso )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    new ( gso ) rocketnet_server_gso_t();

    gso->context = context;
    gso->num_queues = num_queues;
    // the OS keeps cpu 0 on Linux: a count below 1 is read as 1, here and not
    // only in the builtin driver, so no worker is ever left free to run on cpu 0
    gso->num_os_cpus = num_os_cpus < 1 ? 1 : num_os_cpus;
    gso->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    if ( packet_filter )
    {
        gso->has_filter = true;
        memcpy( gso->packet_filter, packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
    }

    gso->max_clients = max_clients;
    gso->queues = (struct rocketnet_server_gso_queue_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_gso_queue_t ) * (size_t) num_queues );
    if ( !gso->queues || !rocketnet_server_process_packets_create( context, &gso->process_packets, rocketnet_server_receive_packets_per_update( max_clients ) ) )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        if ( gso->queues )
        {
            rocketnet_free( context, gso->queues );
        }
        gso->~rocketnet_server_gso_t();
        rocketnet_free( context, gso );
        return NULL;
    }

    for ( int i = 0; i < num_queues; i++ )
    {
        new ( &gso->queues[i] ) rocketnet_server_gso_queue_t; // default-initialized: the big banks and buffers are not written here, so their pages stay untouched until a packet uses them
        rocketnet_receive_drop_tally_reset( &gso->queues[i].receive_tally );
        rocketnet_server_gso_gather_init( &gso->queues[i].send );
        gso->queues[i].stopped.store( true, std::memory_order_relaxed );
        gso->queues[i].fd = -1;
        gso->queues[i].wake_fd = -1;
        gso->queues[i].index = i;
        gso->queues[i].worker_buffer = &gso->queues[i].buffers[0];
        gso->queues[i].main_buffer = &gso->queues[i].buffers[1];
        gso->queues[i].free_buffer = &gso->queues[i].buffers[2];
    }

    // open the reuseport group. the first socket may discover an ephemeral
    // port; the rest join it.

    uint16_t bound_port = bind_address->port;

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_gso_queue_t * queue = &gso->queues[i];

        queue->ipv6 = gso->ipv6;
        queue->gso = gso;

        queue->fd = rocketnet_server_udp_open( "gso", bind_address, i, max_clients, &bound_port, status );
        if ( queue->fd < 0 )
        {
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }

        // a grant short of the request on any socket is asked again past the
        // sysctl limit with the force option, which the kernel takes from a
        // process with CAP_NET_ADMIN and refuses from any other; the first
        // queue's start lines say what was asked, what was got and, when
        // still short, why and which sysctl to set
        // (rocketnet_server_gso_buffer_note)

        {
            const int send_buffer_size = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
            const int receive_buffer_size = rocketnet_server_transport_receive_buffer_request( "gso", max_clients );
            int granted_send = 0;
            int granted_receive = 0;
            bool send_force_tried = false;
            bool receive_force_tried = false;
            int send_force_error = 0;
            int receive_force_error = 0;
            rocketnet_server_gso_force_buffer( queue->fd, false, send_buffer_size, &granted_send, &send_force_tried, &send_force_error );
            rocketnet_server_gso_force_buffer( queue->fd, true, receive_buffer_size, &granted_receive, &receive_force_tried, &receive_force_error );
            if ( i == 0 )
            {
                char line[512];
                if ( rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), false, send_buffer_size, granted_send, send_force_tried, send_force_error ) )
                {
                    rocketnet_warn( "%s", line );
                }
                else
                {
                    rocketnet_info( "%s", line );
                }
                if ( rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), true, receive_buffer_size, granted_receive, receive_force_tried,
                                                       receive_force_error ) )
                {
                    rocketnet_warn( "%s", line );
                }
                else
                {
                    rocketnet_info( "%s", line );
                }
            }
        }
    }

    bind_address->port = bound_port;
    gso->features |= ROCKETNET_SERVER_GSO_FEATURE_REUSEPORT;

    // the runtime probes, each a real setsockopt against every socket in
    // the group, all or nothing (rocketnet_server_udp_probe_offloads).
    // failure is a tier, not an error.

    int fds[ROCKETNET_GSO_MAX_QUEUES] = {};
    for ( int i = 0; i < num_queues; i++ )
    {
        fds[i] = gso->queues[i].fd;
    }

    bool segment = false;
    bool gro = false;
    rocketnet_server_udp_probe_offloads( fds, num_queues, ( flags & ROCKETNET_SERVER_GSO_DISABLE_SEGMENT ) == 0,
                                         ( flags & ROCKETNET_SERVER_GSO_DISABLE_GRO ) == 0, setsockopt, &segment, &gro );
    if ( segment )
    {
        gso->features |= ROCKETNET_SERVER_GSO_FEATURE_SEGMENT;
    }
    if ( gro )
    {
        gso->features |= ROCKETNET_SERVER_GSO_FEATURE_GRO;
    }

    rocketnet_info( "gso transport: %d queue reuseport group on port %d, UDP_SEGMENT %s, UDP_GRO %s",
                    num_queues, bound_port,
                    ( gso->features & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) ? "yes" : "no (plain sendmmsg)",
                    ( gso->features & ROCKETNET_SERVER_GSO_FEATURE_GRO ) ? "yes" : "no (plain recvmmsg)" );

    // the workers, one per socket, started last so a partial create never
    // has a thread to chase. Each sends and receives on its own socket.

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_gso_queue_t * queue = &gso->queues[i];

        if ( !rocketnet_platform_mutex_create( &queue->receive_mutex ) )
        {
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }
        if ( !rocketnet_platform_mutex_create( &queue->send.mutex ) )
        {
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }
        queue->wake_fd = eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
        if ( queue->wake_fd < 0 )
        {
            rocketnet_error( "gso transport could not create queue %d's wake (errno %d)", i, errno );
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }

        queue->stopped.store( false, std::memory_order_relaxed );
        queue->thread = rocketnet_platform_thread_create( context, rocketnet_server_gso_worker, queue, status );
        if ( !queue->thread )
        {
            queue->stopped.store( true, std::memory_order_relaxed );
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }

        // the worker sets its own priority after its pin
        // (rocketnet_server_gso_place_worker): never on cpu 0
    }

    return gso;
}

// a bank's drop and error tallies folded into the transport's counters and
// cleared: when main claims the bank, or at stop for a bank main never took
static void rocketnet_server_gso_fold_buffer( rocketnet_server_gso_t * gso, rocketnet_server_gso_recv_buffer_t * buffer )
{
    gso->counter_receive_dropped_size += buffer->dropped_size;
    gso->counter_receive_dropped_too_large += buffer->dropped_too_large;
    gso->counter_receive_dropped_overflow += buffer->dropped_overflow;
    gso->counter_receive_dropped_source += buffer->dropped_source;
    gso->counter_receive_dropped_prefilter += buffer->dropped_prefilter;
    gso->counter_receive_errors += buffer->receive_errors;
    gso->counter_gro_cmsg_faults += buffer->gro_cmsg_faults;
    gso->counter_gro_batches += buffer->gro_batches;
    buffer->receive_errors = 0;
    buffer->dropped_size = 0;
    buffer->dropped_too_large = 0;
    buffer->dropped_overflow = 0;
    buffer->dropped_source = 0;
    buffer->dropped_prefilter = 0;
    buffer->gro_batches = 0;
    buffer->gro_cmsg_faults = 0;
}

// proofs/tla/RNGsoHandoff.tla StopCount: main, once, when stopped first
// finds every queue worker exited (its acquire), so main owns every bank.
// What the workers received that main never took (a bank a worker could not
// publish, the ready bank, the unread tail of main's bank) is counted
// RECEIVE_DROP_STOPPED, and each bank's drop tallies are folded: no datagram
// a socket delivered goes uncounted. Main takes nothing after stop
// (rocketnet_server_gso_receive_packets), so nothing is counted twice.
static void rocketnet_server_gso_count_stopped( rocketnet_server_gso_t * gso )
{
    for ( int i = 0; gso->queues && i < gso->num_queues; ++i )
    {
        rocketnet_server_gso_queue_t * queue = &gso->queues[i];

        rocketnet_server_gso_recv_buffer_t * held = queue->main_buffer;
        if ( held && queue->main_cursor < held->num_packets )
        {
            gso->receive_dropped_stopped += held->num_packets - queue->main_cursor;
            queue->main_cursor = held->num_packets;
        }

        rocketnet_server_gso_recv_buffer_t * banks[2] = { queue->worker_buffer, queue->ready_buffer };
        for ( int b = 0; b < 2; ++b )
        {
            if ( !banks[b] )
            {
                continue;
            }
            gso->receive_dropped_stopped += banks[b]->num_packets;
            banks[b]->num_packets = 0;
            rocketnet_server_gso_fold_buffer( gso, banks[b] );
        }
    }
}

// proofs/tla/RNGsoHandoff.tla MainStop, per queue: quit is stored for each
// queue's worker; its final producer passes to that worker with the store.
void rocketnet_server_gso_stop( rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    if ( gso->stopping )
    {
        return;
    }
    gso->stopping = true;

    // Release the final producer after all main-thread writes. The worker
    // drains it after earlier published banks; main never waits or scans it.

    gso->send_shutdown.store( true, std::memory_order_release );
    for ( int i = 0; gso->queues && i < gso->num_queues; ++i )
    {
        gso->queues[i].quit.store( true, std::memory_order_release );
        rocketnet_server_gso_wake( &gso->queues[i] );
    }
}

// proofs/tla/RNGsoHandoff.tla BeginShutdown: the builtin transport's
// begin_shutdown hook: main thread, once, when the
// server starts stopping and before its last disconnect batches. The server
// asks for stop only once send_idle is true, so from here a socket that stays
// full gets its queue worker's bounded retries, as after stop, and the worker
// then leaves the tail and returns the bank instead of holding it forever.
void rocketnet_server_gso_begin_shutdown( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    gso->send_shutdown.store( true, std::memory_order_release );
    for ( int i = 0; gso->queues && i < gso->num_queues; ++i )
    {
        rocketnet_server_gso_wake( &gso->queues[i] );
    }
}


bool rocketnet_server_gso_stopped( rocketnet_server_gso_t * gso )
{
    if ( !gso || !gso->stopping )
    {
        return false;
    }
    for ( int i = 0; gso->queues && i < gso->num_queues; ++i )
    {
        if ( !gso->queues[i].stopped.load( std::memory_order_acquire ) )
        {
            return false;
        }
    }
    if ( !gso->stop_counted )
    {
        gso->stop_counted = true;
        rocketnet_server_gso_count_stopped( gso );
    }
    return true;
}

// proofs/tla/RNGsoHandoff.tla Join and Free: destroy joins every queue's worker before
// freeing any storage, and frees nothing while a worker has not exited
// (StopJoinsBeforeFree); Terminated is the freed state itself.
void rocketnet_server_gso_destroy( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    const bool stopped = rocketnet_server_gso_stopped( gso );
    rocketnet_assert( stopped );
    if ( !stopped )
    {
        return;
    }

    if ( gso->queues )
    {
        for ( int i = 0; i < gso->num_queues; i++ )
        {
            struct rocketnet_server_gso_queue_t * queue = &gso->queues[i];

            if ( queue->thread )
            {
                rocketnet_platform_thread_join( queue->thread );
                rocketnet_platform_thread_destroy( queue->thread );
                queue->thread = NULL;
            }
            if ( queue->receive_mutex.ok )
            {
                rocketnet_platform_mutex_destroy( &queue->receive_mutex );
            }
            if ( queue->send.mutex.ok )
            {
                rocketnet_platform_mutex_destroy( &queue->send.mutex );
            }
            if ( queue->wake_fd >= 0 )
            {
                close( queue->wake_fd );
                queue->wake_fd = -1;
            }
            if ( queue->fd >= 0 )
            {
                close( queue->fd );
                queue->fd = -1;
            }
        }

        for ( int i = 0; i < gso->num_queues; i++ )
        {
            gso->queues[i].~rocketnet_server_gso_queue_t();
        }
        rocketnet_free( gso->context, gso->queues );
        gso->queues = NULL;
    }

    void * allocation_context = gso->context;
    rocketnet_server_process_packets_destroy( allocation_context, &gso->process_packets );
    gso->~rocketnet_server_gso_t();
    rocketnet_free( allocation_context, gso );
}

#if ROCKETNET_DEV
int rocketnet_server_gso_num_queues( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return gso->num_queues;
}
#endif // #if ROCKETNET_DEV

int rocketnet_server_gso_features( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return gso->features;
}

void rocketnet_server_gso_counters( struct rocketnet_server_gso_t * gso, uint64_t * counters )
{
    rocketnet_assert( gso );
    rocketnet_assert( counters );

    // The tallies a worker left in its buffers are folded once, at stop
    // (rocketnet_server_gso_count_stopped, run by the first rocketnet_server_gso_stopped
    // that finds every worker exited), so a datagram dropped after main's last
    // claim is still counted.

    // every row of the gso range (GSO_PACKETS_SENT .. GSO_GRO_CMSG_FAULTS) is
    // written below, and GSO_PIN_REFUSALS, which sits after the other
    // transports' rows; nothing else. The server copies both
    // (rocketnet_server_transport_counters)

    uint64_t packets_sent = 0;
    uint64_t send_syscalls = 0;
    uint64_t send_dropped = 0;
    uint64_t gso_sends = 0;
    for ( int i = 0; gso->queues && i < gso->num_queues; i++ )
    {
        const struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[i].send;
        packets_sent += lane->counter_packets_sent.load( std::memory_order_relaxed );
        send_syscalls += lane->counter_send_syscalls.load( std::memory_order_relaxed );
        send_dropped += lane->counter_send_dropped.load( std::memory_order_relaxed );
        gso_sends += lane->counter_gso_sends.load( std::memory_order_relaxed );
    }

    counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] = packets_sent;
    counters[ROCKETNET_COUNTER_GSO_SEND_SYSCALLS] = send_syscalls;
    counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] = send_dropped;
    counters[ROCKETNET_COUNTER_GSO_GSO_SENDS] = gso_sends;
    counters[ROCKETNET_COUNTER_GSO_PACKETS_RECEIVED] = gso->counter_packets_received;
    counters[ROCKETNET_COUNTER_GSO_RECEIVE_SYSCALLS] = gso->counter_receive_syscalls;
    counters[ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_SIZE] = gso->counter_receive_dropped_size;
    counters[ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_OVERFLOW] = gso->counter_receive_dropped_overflow;
    counters[ROCKETNET_COUNTER_GSO_GRO_BATCHES] = gso->counter_gro_batches;
    counters[ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_SOURCE] = gso->counter_receive_dropped_source;
    counters[ROCKETNET_COUNTER_GSO_RECEIVE_ERRORS] = gso->counter_receive_errors;
    counters[ROCKETNET_COUNTER_GSO_GRO_CMSG_FAULTS] = gso->counter_gro_cmsg_faults;
    counters[ROCKETNET_COUNTER_GSO_PIN_REFUSALS] = (uint64_t) gso->pin_refusals.load( std::memory_order_relaxed );
}

// the segments the workers' prefilter refused for a reason other than size
// (sdk/rocketnet_receive_prefilter.h), folded as main claims each bank.
// Main thread.
uint64_t rocketnet_server_gso_receive_dropped_prefilter( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return gso->counter_receive_dropped_prefilter;
}

// the finishes refused because the id and pointer named no slot of the
// queue's producer, or came after stop: never staged, counted. Any thread.
uint64_t rocketnet_server_gso_send_finish_refused( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return gso->counter_send_finish_refused.load( std::memory_order_relaxed );
}

// the driver's receive_drop_totals: every queue worker's tally summed, by
// reason, cumulative for the transport's life. Main thread.

void rocketnet_server_gso_receive_drop_totals( struct rocketnet_server_gso_t * gso, uint64_t * values )
{
    rocketnet_assert( gso );
    rocketnet_assert( values );
    memset( values, 0, ROCKETNET_RECEIVE_NUM_DROPS * sizeof( uint64_t ) );
    for ( int i = 0; gso->queues && i < gso->num_queues; i++ )
    {
        rocketnet_receive_drop_tally_sum( &gso->queues[i].receive_tally, 1, values );
    }
    values[ROCKETNET_RECEIVE_DROP_STOPPED] += gso->receive_dropped_stopped;
    values[ROCKETNET_RECEIVE_ACCEPT] = 0;
}

// the driver's send queue operations: the queue count, fixed after create,
// and one queue's producer room. Main thread for the room.

int rocketnet_server_gso_send_queues( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return gso->num_queues;
}

int rocketnet_server_gso_send_room_queue( struct rocketnet_server_gso_t * gso, int queue )
{
    rocketnet_assert( gso );
    rocketnet_assert( queue >= 0 && queue < gso->num_queues );
    if ( gso->stopping || queue < 0 || queue >= gso->num_queues )
    {
        return 0;
    }
    const struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[queue].send;
    if ( lane->producer < 0 )
    {
        return 0;
    }
    const int used = lane->banks[lane->producer].num_packets.load( std::memory_order_relaxed );
    return used < 0 || used >= gso->send_slots ? 0 : gso->send_slots - used;
}

// ---------------------------------------------------------------------------
// send
// ---------------------------------------------------------------------------

// proofs/tla/ServerStopSpread.tla Room: the free slots of the fullest queue's
// producer bank. A destination's queue is fixed by its address, so the
// smallest room is what any start is sure to find (the XDP transport's rule).

int rocketnet_server_gso_send_room( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    if ( gso->stopping )
    {
        return 0;
    }
    int room = gso->send_slots;
    for ( int i = 0; i < gso->num_queues; i++ )
    {
        const struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[i].send;
        if ( lane->producer < 0 )
        {
            return 0;
        }
        const int used = lane->banks[lane->producer].num_packets.load( std::memory_order_relaxed );
        const int free = used < 0 || used >= gso->send_slots ? 0 : gso->send_slots - used;
        if ( free < room )
        {
            room = free;
        }
    }
    return room;
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: on every queue, every bank
// back in the free queue and the producer empty. A contended lane answers
// false; main asks again next update.

bool rocketnet_server_gso_send_idle( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    for ( int i = 0; i < gso->num_queues; i++ )
    {
        struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[i].send;
        if ( lane->producer < 0 )
        {
            continue;
        }
        if ( lane->banks[lane->producer].num_packets.load( std::memory_order_relaxed ) != 0 )
        {
            return false;
        }
        if ( !rocketnet_platform_mutex_try_acquire( &lane->mutex ) )
        {
            return false;
        }
        const bool idle = lane->ready_count == 0 && lane->free_count == ROCKETNET_GSO_TX_BANKS - 1;
        rocketnet_platform_mutex_release( &lane->mutex );
        if ( !idle )
        {
            return false;
        }
    }

    return true;
}

// proofs/tla/RNGsoHandoff.tla TxStage and proofs/tla/RNGsoQueueBankLayout.tla
// Claim: a game thread stages one packet into its destination queue's producer
// bank, outside the ownership mutex, with one compare-and-swap on that queue's
// count. The packet id is ( turn * slots + slot ) * queues + queue: unique
// across queues and turn-overs with no shared counter, and finish and abort
// read the queue back from it (IdRule). proofs/tla/RNGsoQueueBankLayout.tla
// Load is the queue, producer and counter read below, and Claim the
// compare-and-swap: one that loses reloads the counter it found (the model's
// reload), and an index at send_slots is refused (ClaimCmp).
uint8_t * rocketnet_server_gso_start_packet( struct rocketnet_server_gso_t * gso, const struct rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( gso );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    if ( gso->stopping )
    {
        return NULL;
    }

    const int q = rocketnet_server_gso_send_queue( to, gso->num_queues );
    struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[q].send;
    if ( lane->producer < 0 )
    {
        return NULL;
    }

    rocketnet_server_gso_send_buffer_t * bank = &lane->banks[lane->producer];
    int slot = bank->num_packets.load( std::memory_order_relaxed );
#if ROCKETNET_DEV
    if ( gso->test_claim_hook )
    {
        gso->test_claim_hook( gso->test_claim_user, q );
    }
#endif // #if ROCKETNET_DEV
    while ( slot >= 0 && slot < gso->send_slots &&
            !bank->num_packets.compare_exchange_weak( slot, slot + 1, std::memory_order_relaxed ) )
    {
#if ROCKETNET_DEV
        gso->test_claim_losses.fetch_add( 1, std::memory_order_relaxed );
#endif // #if ROCKETNET_DEV
    }

    // the same lower bound the xdp transport learned to check: an app that
    // stages forever without flushing wraps the counter negative, and a
    // negative index is an out-of-bounds write, not a dropped packet

    if ( slot >= gso->send_slots || slot < 0 )
    {
        rocketnet_spam( "gso transport send staging full. packet dropped" );
        return NULL;
    }

    bank->info[slot].to = *to;
    bank->info[slot].packet_bytes = 0;

    *packet_id = ( lane->turn * (uint64_t) gso->send_slots + (uint64_t) slot ) * (uint64_t) gso->num_queues + (uint64_t) q;

    return bank->data[slot];
}

#if ROCKETNET_DEV
void rocketnet_server_gso_test_set_claim_hook( struct rocketnet_server_gso_t * gso, void ( *hook )( void * user, int queue ), void * user )
{
    rocketnet_assert( gso );
    gso->test_claim_user = user;
    gso->test_claim_hook = hook;
}

int rocketnet_server_gso_test_claim_losses( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return gso->test_claim_losses.load( std::memory_order_relaxed );
}
#endif // #if ROCKETNET_DEV

// the queue a packet id names: its low part (the id is ( turn * slots + slot ) * queues + queue)
static int rocketnet_server_gso_id_queue( const struct rocketnet_server_gso_t * gso, uint64_t packet_id )
{
    return (int) ( packet_id % (uint64_t) gso->num_queues );
}

// the producer slot packet_data is the root of, on the queue packet_id names,
// or -1. The slot comes from pointer arithmetic against the staging, the xdp
// pattern, with the range checked before the divide: a pointer that is not a
// live slot root fails closed as a lost packet, never an out-of-bounds write.
// proofs/tla/RNGsoQueueBankLayout.tla SlotRule.
static int rocketnet_server_gso_send_slot( struct rocketnet_server_gso_t * gso, uint64_t packet_id, const uint8_t * packet_data,
                                           rocketnet_server_gso_send_buffer_t ** bank_out )
{
    if ( !packet_data || gso->stopping )
    {
        return -1;
    }

    const int q = rocketnet_server_gso_id_queue( gso, packet_id );
    struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[q].send;
    if ( lane->producer < 0 )
    {
        return -1;
    }
    rocketnet_server_gso_send_buffer_t * bank = &lane->banks[lane->producer];

#if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )
    // the trace check's plant (tests/tla_trace_gso.cpp, the tla_trace_plant_gso
    // binary only): the slot divided out of the pointer's signed offset, with
    // C's truncation toward zero, before any test, and no alignment test
    // (proofs/tla/RNGsoQueueBankLayout.tla SlotRule "divide"). A pointer a byte
    // early on slot s lands on slot s - 1, another claim's.
    const intptr_t planted = (intptr_t) ( (uintptr_t) packet_data - (uintptr_t) bank->data[0] ) / (intptr_t) ROCKETNET_FRAME_STRIDE;
    if ( planted < 0 || planted >= gso->send_slots )
    {
        return -1;
    }

    *bank_out = bank;
    return (int) planted;
#else  // #if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )
    const uintptr_t first = (uintptr_t) bank->data[0];
    const uintptr_t address = (uintptr_t) packet_data;
    if ( address < first || address - first >= sizeof( bank->data ) )
    {
        return -1;
    }
    const uintptr_t offset = address - first;
    if ( offset % (uintptr_t) ROCKETNET_FRAME_STRIDE != 0 )
    {
        return -1;
    }
    const int slot = (int) ( offset / (uintptr_t) ROCKETNET_FRAME_STRIDE );
    if ( slot < 0 || slot >= gso->send_slots )
    {
        return -1;
    }

    *bank_out = bank;
    return slot;
#endif // #if defined( ROCKETNET_GSO_SEND_SLOT_PLANT )
}

// proofs/tla/RNGsoQueueBankLayout.tla Finish: the slot the id's queue and the
// pointer name in that queue's producer, or nothing (rocketnet_server_gso_send_slot).
void rocketnet_server_gso_finish_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( gso );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    rocketnet_server_gso_send_buffer_t * bank = NULL;
    const int slot = rocketnet_server_gso_send_slot( gso, packet_id, packet_data, &bank );
    if ( slot < 0 )
    {
        gso->counter_send_finish_refused.fetch_add( 1, std::memory_order_relaxed );
        return;
    }

    bank->info[slot].packet_bytes = packet_bytes;
}

// proofs/tla/RNGsoQueueBankLayout.tla Finish, as an abort: the same slot, or nothing.
void rocketnet_server_gso_abort_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( gso );

    rocketnet_server_gso_send_buffer_t * bank = NULL;
    const int slot = rocketnet_server_gso_send_slot( gso, packet_id, packet_data, &bank );
    if ( slot < 0 )
    {
        return;
    }

    bank->info[slot].packet_bytes = 0;
}

static bool rocketnet_server_gso_segment_refused( int error )
{
    return error == EINVAL || error == EIO || error == EOPNOTSUPP;
}

static int rocketnet_server_gso_invoke_send( struct rocketnet_server_gso_t * gso, int fd, struct mmsghdr * messages, unsigned int count )
{
#if ROCKETNET_DEV
    int pending = gso->test_fail_sends.load( std::memory_order_acquire );
    if ( pending > 0 && gso->test_fail_sends.compare_exchange_strong( pending, pending - 1 ) )
    {
        errno = gso->test_fail_send_error.load( std::memory_order_relaxed );
        return -1;
    }

    if ( gso->test_send )
    {
        return gso->test_send( messages, count, gso->test_send_user );
    }
#else
    (void) gso;
#endif // #if ROCKETNET_DEV

    return sendmmsg( fd, messages, count, 0 );
}

// one plain datagram per packet of a refused segmented run. same destination,
// no UDP_SEGMENT cmsg. a full socket stops at the first unsent packet; a hard
// error drops that packet and the run continues.
static int rocketnet_server_gso_resend_plain( struct rocketnet_server_gso_t * gso, struct rocketnet_server_gso_send_lane_t * lane, int fd, const struct mmsghdr * segmented, int first_slot, int run_packets, bool * full )
{
    struct mmsghdr plain[ROCKETNET_GSO_MAX_SEGMENTS];
    struct iovec iov[ROCKETNET_GSO_MAX_SEGMENTS];

    rocketnet_assert( run_packets > 0 && run_packets <= ROCKETNET_GSO_MAX_SEGMENTS );

    for ( int i = 0; i < run_packets; i++ )
    {
        memset( &plain[i], 0, sizeof( plain[i] ) );
        iov[i] = segmented->msg_hdr.msg_iov[i];
        plain[i].msg_hdr.msg_name = segmented->msg_hdr.msg_name;
        plain[i].msg_hdr.msg_namelen = segmented->msg_hdr.msg_namelen;
        plain[i].msg_hdr.msg_iov = &iov[i];
        plain[i].msg_hdr.msg_iovlen = 1;
    }

    int sent = 0;
    while ( sent < run_packets )
    {
        const int result = rocketnet_server_gso_invoke_send( gso, fd, &plain[sent], (unsigned int) ( run_packets - sent ) );
        if ( result > 0 )
        {
            rocketnet_server_gso_lane_add( &lane->counter_send_syscalls, 1 );
            rocketnet_server_gso_lane_add( &lane->counter_packets_sent, (uint64_t) result );
            sent += result;
            continue;
        }

        rocketnet_server_gso_lane_add( &lane->counter_send_syscalls, 1 );

        if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == ENOMEM || errno == EINTR )
        {
            *full = true;
            return first_slot + sent;
        }

        rocketnet_server_gso_lane_add( &lane->counter_send_dropped, 1 );
        sent++;
    }

    return first_slot + run_packets;
}

// one flush pass: walk the bank's positions in lane->order (its slots grouped
// by destination, rocketnet_server_gso_gather_bank) once, build sendmmsg
// batches, coalescing adjacent same-destination positions into GSO
// super-datagrams where the feature is on and the destination has not refused
// one. first_slot, num_slots and the return are positions in order. returns
// the first unconsumed position. a refused segmented run is resent plain, its
// destination remembered, and the batch continues; when the transport cannot
// segment at all (rocketnet_segment_refused_add) the caller flips the feature
// off and resumes plain from this message. a full socket (*full) resumes from its
// first unsent message later. A slot this pass dropped is cleared, so a
// resumed pass skips it uncounted.

static int rocketnet_server_gso_flush( rocketnet_server_gso_t * gso, struct rocketnet_server_gso_queue_t * queue,
                                       rocketnet_server_gso_send_buffer_t * bank, int first_slot, int num_slots, bool * disable_gso, bool * full )
{
    struct rocketnet_server_gso_send_lane_t * lane = &queue->send;
    const int fd = queue->fd;
    const bool use_gso = ( gso->features & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0;

    int slot = first_slot;

    while ( slot < num_slots )
    {
        // build one sendmmsg batch

        int num_messages = 0;
        int message_first_slot[ROCKETNET_GSO_SEND_BATCH];
        int message_num_packets[ROCKETNET_GSO_SEND_BATCH] = {};
        bool message_is_gso[ROCKETNET_GSO_SEND_BATCH] = {};
        int num_iovecs = 0;
        int batch_end = slot;

        while ( batch_end < num_slots && num_messages < ROCKETNET_GSO_SEND_BATCH )
        {
            struct rocketnet_server_gso_send_info_t * info = &bank->info[lane->order[batch_end]];

            if ( info->packet_bytes <= 0 )
            {
                batch_end++;                        // aborted slot
                continue;
            }

            const int run_first = batch_end;
            const int segment_bytes = info->packet_bytes;
            int run_count = 1;
            int run_bytes = segment_bytes;
            batch_end++;

            if ( use_gso && !rocketnet_segment_refused_contains( &lane->segment_refused, &info->to ) )
            {
                // extend the run, for a destination that has not refused a
                // segmented send: equal sizes to the same destination, plus
                // at most one shorter tail (UDP_SEGMENT semantics: every
                // segment is the stride except possibly the last). a LARGER
                // packet, a different destination, or an aborted slot ends
                // the run (proofs/tla/RNSegmentRefusal.tla Run).

                while ( batch_end < num_slots && run_count < ROCKETNET_GSO_MAX_SEGMENTS )
                {
                    struct rocketnet_server_gso_send_info_t * next = &bank->info[lane->order[batch_end]];

                    if ( next->packet_bytes <= 0 )
                    {
                        break;
                    }
                    if ( next->packet_bytes > segment_bytes )
                    {
                        break;
                    }
                    if ( run_bytes + next->packet_bytes > ROCKETNET_GSO_MAX_SUPER_BYTES )
                    {
                        break;
                    }
                    if ( !rocketnet_address_equal( &next->to, &info->to ) )
                    {
                        break;
                    }

                    run_count++;
                    run_bytes += next->packet_bytes;
                    batch_end++;

                    if ( next->packet_bytes < segment_bytes )
                    {
                        break;                      // the shorter tail closes the run
                    }
                }
            }

            // one message for the run

            struct mmsghdr * message = &lane->headers[num_messages];
            struct msghdr * hdr = &message->msg_hdr;
            memset( hdr, 0, sizeof( struct msghdr ) );

            socklen_t namelen = rocketnet_server_gso_address_to_sockaddr( &info->to, gso->ipv6, &lane->names[num_messages] );
            if ( namelen == 0 )
            {
                // unroutable on this socket family; drop the run counted,
                // cleared so a resumed pass does not count it again
                rocketnet_server_gso_lane_add( &lane->counter_send_dropped, (uint64_t) run_count );
                for ( int i = 0; i < run_count; i++ )
                {
                    bank->info[lane->order[run_first + i]].packet_bytes = 0;
                }
                continue;
            }

            hdr->msg_name = &lane->names[num_messages];
            hdr->msg_namelen = namelen;

            hdr->msg_iov = &lane->iovecs[num_iovecs];
            hdr->msg_iovlen = (size_t) run_count;

            // the run is contiguous positions [run_first, run_first + run_count)
            // with every packet_bytes positive, by construction of the run
            // builder above -- so the fill is bounded by run_count and never
            // walks the arrays. a concurrent mutation of a slot's bytes here
            // would be a fill-contract violation; the loop stays inside the
            // run regardless, fail closed, never out of bounds.

            for ( int i = 0; i < run_count; i++ )
            {
                const int iovec_slot = lane->order[run_first + i];
                lane->iovecs[num_iovecs + i].iov_base = bank->data[iovec_slot] + ROCKETNET_FRAME_WIRE_OFFSET;
                lane->iovecs[num_iovecs + i].iov_len = (size_t) bank->info[iovec_slot].packet_bytes;
            }

            num_iovecs += run_count;

            if ( run_count > 1 )
            {
                memset( lane->controls[num_messages], 0, sizeof( lane->controls[num_messages] ) );
                hdr->msg_control = lane->controls[num_messages];
                hdr->msg_controllen = CMSG_SPACE( sizeof( uint16_t ) );

                struct cmsghdr * cmsg = CMSG_FIRSTHDR( hdr );
                cmsg->cmsg_level = IPPROTO_UDP;
                cmsg->cmsg_type = UDP_SEGMENT;
                cmsg->cmsg_len = CMSG_LEN( sizeof( uint16_t ) );

                uint16_t stride = (uint16_t) segment_bytes;
                memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
            }

            message_first_slot[num_messages] = run_first;
            message_num_packets[num_messages] = run_count;
            message_is_gso[num_messages] = run_count > 1;
            num_messages++;
        }

        if ( num_messages == 0 )
        {
            slot = batch_end;
            continue;
        }

        // the queue's own socket: every socket of the group shares the bound
        // address and port, so replies carry the same 4-tuple whichever
        // socket sent them; a destination's packets always leave through its
        // own queue, so they stay in the order they were staged. The queue's
        // worker owns all batch scratch.

        int sent = 0;
        while ( sent < num_messages )
        {
            const int result = rocketnet_server_gso_invoke_send( gso, fd, &lane->headers[sent], (unsigned int) ( num_messages - sent ) );

            if ( result > 0 )
            {
                rocketnet_server_gso_lane_add( &lane->counter_send_syscalls, 1 );

                for ( int i = sent; i < sent + result; i++ )
                {
                    rocketnet_server_gso_lane_add( &lane->counter_packets_sent, (uint64_t) message_num_packets[i] );
                    if ( message_is_gso[i] )
                    {
                        rocketnet_server_gso_lane_add( &lane->counter_gso_sends, 1 );
                        if ( !lane->segment_refused.accepted )
                        {
                            lane->segment_refused.accepted = true;
                            gso->segment_accepted.store( true, std::memory_order_relaxed );
                        }
                    }
                }

                sent += result;
                continue;
            }

            rocketnet_server_gso_lane_add( &lane->counter_send_syscalls, 1 );

            if ( message_is_gso[sent] && rocketnet_server_gso_segment_refused( errno ) )
            {
                // the kernel accepted the UDP_SEGMENT probe but this
                // destination refused a segmented send. it is that
                // destination's: it is remembered, so its later runs leave
                // plain, and this run is resent plain; other destinations keep
                // segmenting. the tier turns off only when the threshold of
                // destinations refused with none ever accepted (a kernel that
                // cannot segment), and then the bank resends from this message
                // with nothing dropped (proofs/tla/RNSegmentRefusal.tla Refuse).

                const char * reason = strerror( errno );
                const int refused_slot = message_first_slot[sent];

                // another queue's accepted segmented send counts as this
                // queue's: the transport can segment
                if ( gso->segment_accepted.load( std::memory_order_relaxed ) )
                {
                    lane->segment_refused.accepted = true;
                }

                if ( rocketnet_segment_refused_add( &lane->segment_refused, &bank->info[lane->order[refused_slot]].to ) )
                {
                    rocketnet_warn( "gso transport: live GSO send refused (%s). %d destinations refused and none accepted, degrading to plain sendmmsg", reason, lane->segment_refused.destinations );
                    *disable_gso = true;
                    return refused_slot;
                }

                if ( !lane->segment_refusal_warned )
                {
                    lane->segment_refusal_warned = true;
                    rocketnet_warn( "gso transport: live GSO send refused (%s). resending that run plain", reason );
                }
                else
                {
                    rocketnet_spam( "gso transport: live GSO send refused (%s). resending that run plain", reason );
                }
                const int resume = rocketnet_server_gso_resend_plain( gso, lane, fd, &lane->headers[sent], message_first_slot[sent], message_num_packets[sent], full );
                if ( *full )
                {
                    return resume;
                }

                sent++;
                continue;
            }

            // a full socket (or a signal): the socket cannot take more now.
            // nothing is dropped: the flush stops at the first unsent
            // message and the bank keeps the rest, in order, for the next
            // attempt (proofs/tla/RNGsoHandoff.tla TxSend, SentOrHeld)

            if ( errno == EINTR )
            {
                continue;
            }

            if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == ENOMEM )
            {
                rocketnet_spam( "gso transport sendmmsg refused (errno %d). %d messages wait for the next attempt", errno, num_messages - sent );

                *full = true;
                return message_first_slot[sent];
            }

            // nothing went in this call, so the first unsent message is the
            // one the kernel refused (an unroutable destination, say): drop
            // it and send the rest in order, as the portable batch and the
            // client gso do

            rocketnet_server_gso_lane_add( &lane->counter_send_dropped, (uint64_t) message_num_packets[sent] );

            rocketnet_spam( "gso transport sendmmsg refused (errno %d). one message dropped", errno );

            sent++;
        }

        slot = batch_end;
    }

    return num_slots;
}

void rocketnet_server_gso_send_packets( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    // every queue's staging to its worker; a bank a socket refused waits for
    // this update, so its worker is woken to send the rest

    rocketnet_server_gso_publish_all( gso );
}

// ---------------------------------------------------------------------------
// receive
// ---------------------------------------------------------------------------

// proofs/tla/RNGsoHandoff.tla RxClaim and RxConsume: consume a retained bank
// before claiming a newer ready bank. The previous
// public view expires at this receive call, but a partial tail remains owned by
// main and cannot be recycled. Counters are folded exactly once when claimed.
static void rocketnet_server_gso_receive_from_queue( rocketnet_server_gso_t * gso,
                                                     rocketnet_server_gso_queue_t * queue, int budget )
{
    if ( budget <= 0 )
    {
        return;
    }
    if ( queue->main_cursor >= queue->main_buffer->num_packets )
    {
        queue->main_buffer->num_packets = 0;
        queue->main_cursor = 0;
        if ( !rocketnet_platform_mutex_try_acquire( &queue->receive_mutex ) )
        {
            return;
        }
        if ( !queue->ready_buffer )
        {
            rocketnet_platform_mutex_release( &queue->receive_mutex );
            return;
        }

        rocketnet_server_gso_recv_buffer_t * buffer = queue->ready_buffer;
        queue->ready_buffer = NULL;
        queue->free_buffer = queue->main_buffer;
        queue->main_buffer = buffer;
        rocketnet_platform_mutex_release( &queue->receive_mutex );

        gso->counter_packets_received += buffer->num_packets;
        rocketnet_server_gso_fold_buffer( gso, buffer );
    }

    rocketnet_server_gso_recv_buffer_t * buffer = queue->main_buffer;
    while ( queue->main_cursor < buffer->num_packets && budget-- > 0 &&
            gso->process_packets.num_packets < gso->process_packets.capacity )
    {
        const uint32_t source = queue->main_cursor++;
        const int index = gso->process_packets.num_packets++;
        gso->process_packets.from[index] = buffer->from[source];
        gso->process_packets.packet_data[index] = buffer->packet_data[source];
        gso->process_packets.packet_bytes[index] = buffer->packet_bytes[source];
    }
}

void rocketnet_server_gso_receive_packets( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    rocketnet_server_gso_publish_all( gso ); // retry deferred TX even on idle updates
    gso->process_packets.num_packets = 0;

    // proofs/tla/RNGsoHandoff.tla: once stop is asked main claims and
    // consumes nothing (RxClaim and RxConsume); what is still banked is
    // counted RECEIVE_DROP_STOPPED when the workers have exited (StopCount)
    if ( gso->stopping )
    {
        return;
    }

    // a rotating start: queue 0 is not served first on every call. The budget
    // ends on filled_queue and the next call starts after it, the same rule
    // as the xdp drain (rocketnet_xdp_drain_queue). A call that does not fill
    // the budget still advances by one. Two full queues exhaust the per-update
    // budget, so a walk that always starts at 0 never reaches the later ones.
    // proofs/tla/RNGsoRecvWalk.tla Receive and NoQueueStarves.
    const int num_queues = gso->num_queues;
    int filled_queue = -1;
    for ( int step = 0; step < num_queues; step++ )
    {
        const int queue_index = rocketnet_xdp_drain_queue( gso->receive_start_queue, step, num_queues );
        rocketnet_server_gso_queue_t * queue = &gso->queues[queue_index];
        gso->counter_receive_syscalls += queue->receive_syscalls.exchange( 0, std::memory_order_relaxed );

        const int before = gso->process_packets.num_packets;
        rocketnet_server_gso_receive_from_queue( gso, queue, gso->process_packets.capacity - before );
        if ( filled_queue < 0 && before < gso->process_packets.capacity &&
             gso->process_packets.num_packets >= gso->process_packets.capacity )
        {
            filled_queue = queue_index;
        }
    }

    if ( num_queues > 0 )
    {
        const int origin = filled_queue >= 0 ? filled_queue : gso->receive_start_queue;
        gso->receive_start_queue = rocketnet_xdp_drain_queue( origin, 1, num_queues );
    }
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_gso_process_packets( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );
    return &gso->process_packets;
}

#if ROCKETNET_DEV

// The RNGsoHandoff transitions, stepped on the calling thread with a faked
// transport: packet bytes are written straight into the worker's buffer
// (the staged result of a receive pass) and the real publish, claim,
// consume, stage, take, return, stop and destroy functions run in order.
// No socket is opened, no thread is started and no clock is read.

static void rocketnet_server_gso_test_fill( struct rocketnet_server_gso_recv_buffer_t * buffer, uint8_t marker, uint32_t packets )
{
    buffer->num_packets = packets;

    for ( uint32_t i = 0; i < packets; i++ )
    {
        buffer->packet_bytes[i] = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
        memset( buffer->packet_data[i], marker, (size_t) ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }
}

static bool rocketnet_server_gso_test_stage( struct rocketnet_server_gso_t * gso, uint16_t port )
{
    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = port;

    uint64_t packet_id = 0;
    uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
    if ( !packet )
    {
        return false;
    }
    rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );

    return true;
}

// a queue's fixture state: no socket, no wake, no thread
static bool rocketnet_server_gso_test_init_queue( struct rocketnet_server_gso_t * gso, int index )
{
    struct rocketnet_server_gso_queue_t * queue = &gso->queues[index];
    new ( queue ) rocketnet_server_gso_queue_t; // default-initialized, as rocketnet_server_gso_create does
    rocketnet_receive_drop_tally_reset( &queue->receive_tally );
    rocketnet_server_gso_gather_init( &queue->send );
    // the fixture reads every slot of every bank (the trace check compares
    // them before and after a call), so its banks' slot infos start defined;
    // the library itself reads a slot only after it is claimed
    for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
    {
        memset( queue->send.banks[b].info, 0, sizeof( queue->send.banks[b].info ) );
    }
    queue->fd = -1;
    queue->wake_fd = -1;
    queue->index = index;
    queue->gso = gso;
    queue->worker_buffer = &queue->buffers[0];
    queue->main_buffer = &queue->buffers[1];
    queue->free_buffer = &queue->buffers[2];
    queue->stopped.store( true, std::memory_order_relaxed );
    if ( !rocketnet_platform_mutex_create( &queue->receive_mutex ) )
    {
        return false;
    }
    return rocketnet_platform_mutex_create( &queue->send.mutex );
}

static void rocketnet_server_gso_test_fail( struct rocketnet_server_gso_t * gso, int constructed )
{
    for ( int i = 0; i < constructed; i++ )
    {
        if ( gso->queues[i].receive_mutex.ok )
        {
            rocketnet_platform_mutex_destroy( &gso->queues[i].receive_mutex );
        }
        if ( gso->queues[i].send.mutex.ok )
        {
            rocketnet_platform_mutex_destroy( &gso->queues[i].send.mutex );
        }
        gso->queues[i].~rocketnet_server_gso_queue_t();
    }
    rocketnet_free( NULL, gso->queues );
    rocketnet_server_process_packets_destroy( NULL, &gso->process_packets );
    gso->~rocketnet_server_gso_t();
    rocketnet_free( NULL, gso );
}

struct rocketnet_server_gso_t * rocketnet_server_gso_test_open( int num_queues )
{
    if ( num_queues < 1 || num_queues > ROCKETNET_GSO_MAX_QUEUES )
    {
        return NULL;
    }

    void * storage = rocketnet_malloc( NULL, sizeof( struct rocketnet_server_gso_t ) );
    if ( !storage )
    {
        return NULL;
    }
    struct rocketnet_server_gso_t * gso = new ( storage ) rocketnet_server_gso_t();
    gso->context = NULL;
    gso->num_queues = num_queues;
    gso->max_clients = ROCKETNET_DEFAULT_MAX_CLIENTS;
    gso->queues = (struct rocketnet_server_gso_queue_t *) rocketnet_malloc( NULL, sizeof( struct rocketnet_server_gso_queue_t ) * (size_t) num_queues );
    if ( !gso->queues || !rocketnet_server_process_packets_create( NULL, &gso->process_packets, rocketnet_server_receive_packets_per_update( gso->max_clients ) ) )
    {
        if ( gso->queues )
        {
            rocketnet_free( NULL, gso->queues );
        }
        gso->~rocketnet_server_gso_t();
        rocketnet_free( NULL, storage );
        return NULL;
    }

    for ( int i = 0; i < num_queues; i++ )
    {
        if ( !rocketnet_server_gso_test_init_queue( gso, i ) )
        {
            rocketnet_server_gso_test_fail( gso, i + 1 );
            return NULL;
        }
    }

    return gso;
}

// the trace check's fixture (tests/tla_trace_gso.cpp): each producer bank
// hands out send_slots slots, so a walk fills one in a few steps
struct rocketnet_server_gso_t * rocketnet_server_gso_test_open_slots( int num_queues, int send_slots )
{
    if ( send_slots < 1 || send_slots > ROCKETNET_GSO_SEND_QUEUE_SIZE )
    {
        return NULL;
    }
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( num_queues );
    if ( gso )
    {
        gso->send_slots = send_slots;
    }
    return gso;
}

uint64_t rocketnet_server_gso_test_open_bytes( int num_queues )
{
    return (uint64_t) sizeof( struct rocketnet_server_gso_t ) + (uint64_t) num_queues * (uint64_t) sizeof( struct rocketnet_server_gso_queue_t );
}

// one queue's send staging as the lane holds it, read on the thread that
// drives the fixture (no worker runs on it)
bool rocketnet_server_gso_test_send_lane( struct rocketnet_server_gso_t * gso, int queue_index, struct rocketnet_server_gso_test_send_lane_t * lane )
{
    if ( !gso || !lane || queue_index < 0 || queue_index >= gso->num_queues )
    {
        return false;
    }
    const struct rocketnet_server_gso_send_lane_t * send = &gso->queues[queue_index].send;
    lane->slots = gso->send_slots;
    lane->producer = send->producer;
    lane->turn = send->turn;
    lane->free_head = send->free_head;
    lane->free_count = send->free_count;
    lane->ready_head = send->ready_head;
    lane->ready_count = send->ready_count;
    for ( int b = 0; b < ROCKETNET_GSO_TX_BANKS; b++ )
    {
        lane->free_banks[b] = send->free_banks[b];
        lane->ready_banks[b] = send->ready[b];
        lane->num_packets[b] = send->banks[b].num_packets.load( std::memory_order_relaxed );
        lane->first[b] = send->banks[b].data[0];
    }
    lane->bank_bytes = sizeof( send->banks[0].data );
    return true;
}

// what the code decides for a packet id and the pointer start returned: the
// slot (rocketnet_server_gso_send_slot, -1 refused) and the queue the id names
// (the id's low part), so the trace check reads both from the library
int rocketnet_server_gso_test_send_slot( struct rocketnet_server_gso_t * gso, uint64_t packet_id, const uint8_t * packet_data, int * queue_out )
{
    rocketnet_assert( gso );
    rocketnet_assert( queue_out );
    *queue_out = rocketnet_server_gso_id_queue( gso, packet_id );
    rocketnet_server_gso_send_buffer_t * bank = NULL;
    return rocketnet_server_gso_send_slot( gso, packet_id, packet_data, &bank );
}

// the fakes of rocketnet_server_gso_test_place_worker: only the refusals are
// injected; the transport's own placement code counts and records
static int gso_fake_cpus = 4;
static bool gso_fake_refuse_pin = false;
static bool gso_fake_refuse_all = false;
static int gso_fake_round_robin = 0;

static int gso_fake_get( cpu_set_t * mask )
{
    CPU_ZERO( mask );
    for ( int cpu = 0; cpu < gso_fake_cpus && cpu < CPU_SETSIZE; cpu++ )
    {
        CPU_SET( (size_t) cpu, mask );
    }
    return 0;
}

static int gso_fake_set( const cpu_set_t * mask )
{
    if ( gso_fake_refuse_all || ( gso_fake_refuse_pin && CPU_COUNT( mask ) == 1 ) )
    {
        return EPERM;
    }
    return 0;
}

static int gso_fake_set_round_robin( int priority )
{
    (void) priority;
    gso_fake_round_robin++;
    return 0;
}

bool rocketnet_server_gso_test_place_worker( struct rocketnet_server_gso_t * gso, int queue_index, int num_os_cpus, int online, int cpus, bool refuse_pin,
                                             bool refuse_all, bool * left_on_core_zero, int * round_robin_requests )
{
    rocketnet_assert( gso );
    if ( queue_index < 0 || queue_index >= gso->num_queues )
    {
        return false;
    }
    gso->num_os_cpus = num_os_cpus > 1 ? num_os_cpus : 1;
    gso_fake_cpus = cpus;
    gso_fake_refuse_pin = refuse_pin;
    gso_fake_refuse_all = refuse_all;
    gso_fake_round_robin = 0;
    const struct rocketnet_gso_affinity_calls_t fake = { gso_fake_get, gso_fake_set, gso_fake_set_round_robin };

    const bool priority_enabled = rocketnet_platform_thread_priority_enabled();
    rocketnet_platform_set_thread_priority_enabled( true );
    rocketnet_server_gso_place_worker( &gso->queues[queue_index], online, &fake );
    rocketnet_platform_set_thread_priority_enabled( priority_enabled );

    *left_on_core_zero = gso->queues[queue_index].left_on_core_zero.load( std::memory_order_relaxed );
    *round_robin_requests = gso_fake_round_robin;
    return true;
}

int rocketnet_server_gso_test_worker_allows_cpu0( struct rocketnet_server_gso_t * gso, int queue_index )
{
    if ( !gso || queue_index < 0 || queue_index >= gso->num_queues || !gso->queues[queue_index].thread )
    {
        return -1;
    }
    cpu_set_t mask;
    CPU_ZERO( &mask );
    if ( pthread_getaffinity_np( gso->queues[queue_index].thread->handle, sizeof( mask ), &mask ) != 0 )
    {
        return -1;
    }
    return CPU_ISSET( 0, &mask ) ? 1 : 0;
}

int rocketnet_server_gso_test_send_packet_bytes( struct rocketnet_server_gso_t * gso, int queue_index, int bank, int slot )
{
    if ( !gso || queue_index < 0 || queue_index >= gso->num_queues || bank < 0 || bank >= ROCKETNET_GSO_TX_BANKS || slot < 0 ||
         slot >= ROCKETNET_GSO_SEND_QUEUE_SIZE )
    {
        return -1;
    }
    return gso->queues[queue_index].send.banks[bank].info[slot].packet_bytes;
}

int rocketnet_server_gso_test_take_send( struct rocketnet_server_gso_t * gso, int queue_index, bool * final )
{
    rocketnet_assert( gso );
    rocketnet_assert( final );
    rocketnet_assert( queue_index >= 0 && queue_index < gso->num_queues );
    return rocketnet_server_gso_take_send( &gso->queues[queue_index], final );
}

void rocketnet_server_gso_test_return_send( struct rocketnet_server_gso_t * gso, int queue_index, int index )
{
    rocketnet_assert( gso );
    rocketnet_assert( queue_index >= 0 && queue_index < gso->num_queues );
    rocketnet_server_gso_return_send( &gso->queues[queue_index], index );
}

// one queue, no socket, no thread: the fixture every stepped test runs on. Its
// queue reports running until the test says its worker has left.
static struct rocketnet_server_gso_t * rocketnet_server_gso_test_create()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    if ( gso )
    {
        gso->queues[0].stopped.store( false, std::memory_order_relaxed );
    }
    return gso;
}

// the fixture started no threads, so destroy has no handle to join; it
// releases the mutexes and frees the storage
static void rocketnet_server_gso_test_destroy( struct rocketnet_server_gso_t * gso )
{
    rocketnet_server_gso_destroy( gso );
}

// the queue worker's send side, stepped on the calling thread with no wait: a
// step a full socket ended is retried at once, as main's wake would. Returns
// true when stop's final bank was sent, false past the step bound (a worker
// that never finishes).
static bool rocketnet_server_gso_test_run_send( struct rocketnet_server_gso_t * gso, struct rocketnet_server_gso_queue_t * queue )
{
    for ( int step = 0; step < 1000; step++ )
    {
        if ( rocketnet_server_gso_send_step( gso, queue ) )
        {
            return true;
        }
    }
    return false;
}

void rocketnet_server_gso_test_close( struct rocketnet_server_gso_t * gso )
{
    if ( !gso )
    {
        return;
    }

    rocketnet_server_gso_stop( gso );
    for ( int i = 0; i < gso->num_queues; i++ )
    {
        gso->queues[i].stopped.store( true, std::memory_order_release );
    }
    rocketnet_server_gso_destroy( gso );
}

bool rocketnet_server_gso_test_publish_marked( struct rocketnet_server_gso_t * gso, int queue_index, uint8_t marker, uint32_t packets )
{
    if ( !gso || queue_index < 0 || queue_index >= gso->num_queues || packets > (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE )
    {
        return false;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[queue_index];
    rocketnet_server_gso_test_fill( queue->worker_buffer, marker, packets );
    return rocketnet_server_gso_publish_receive( queue );
}

// StopCount: a bank the worker could not publish, the ready bank and the
// unread tail of main's bank, all still banked when stop is asked, are
// counted RECEIVE_DROP_STOPPED, with the banks' drop tallies folded, once
// every worker has exited; main takes nothing after stop, and the count is
// made once.
bool rocketnet_server_gso_test_stop_counts_unreceived()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS];

    bool valid = true;

    // main claims three packets and reads one: a tail of two stays in its bank

    rocketnet_server_gso_test_fill( queue->worker_buffer, (uint8_t) 'a', 3 );
    valid = valid && rocketnet_server_gso_publish_receive( queue );
    gso->process_packets.num_packets = 0;
    rocketnet_server_gso_receive_from_queue( gso, queue, 1 );
    valid = valid && gso->process_packets.num_packets == 1 && queue->main_cursor == 1 && gso->counter_packets_received == 3;

    // two more wait in the ready bank; four, with a size drop, in the
    // worker's bank, which cannot be published behind them

    rocketnet_server_gso_test_fill( queue->worker_buffer, (uint8_t) 'b', 2 );
    valid = valid && rocketnet_server_gso_publish_receive( queue );
    rocketnet_server_gso_test_fill( queue->worker_buffer, (uint8_t) 'c', 4 );
    queue->worker_buffer->dropped_size = 1;
    valid = valid && !rocketnet_server_gso_publish_receive( queue );

    // after stop main takes nothing, and nothing is counted while the
    // worker still runs

    rocketnet_server_gso_stop( gso );
    rocketnet_server_gso_receive_packets( gso );
    valid = valid && gso->process_packets.num_packets == 0 && queue->main_cursor == 1;
    valid = valid && !rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_receive_drop_totals( gso, totals );
    valid = valid && totals[ROCKETNET_RECEIVE_DROP_STOPPED] == 0;

    // the worker exits: the tail, the ready bank and the worker's bank are
    // counted, 2 + 2 + 4, with the worker bank's size drop folded

    queue->stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_receive_drop_totals( gso, totals );
    valid = valid && totals[ROCKETNET_RECEIVE_DROP_STOPPED] == 8 && gso->counter_receive_dropped_size == 1 &&
            gso->counter_packets_received == 3;

    // a second stopped call counts nothing more

    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_receive_drop_totals( gso, totals );
    valid = valid && totals[ROCKETNET_RECEIVE_DROP_STOPPED] == 8;

    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

bool rocketnet_server_gso_test_worker_handoff()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];

    bool valid = true;

    // Receive: the worker publishes a filled buffer and takes the free one.

    rocketnet_server_gso_test_fill( queue->worker_buffer, (uint8_t) 'a', 2 );
    valid = valid && rocketnet_server_gso_publish_receive( queue );
    valid = valid && queue->ready_buffer == &queue->buffers[0] && queue->worker_buffer == &queue->buffers[2] &&
            queue->free_buffer == NULL;

    // While a ready buffer waits, the worker retains its next filled buffer.

    rocketnet_server_gso_test_fill( queue->worker_buffer, (uint8_t) 'b', 1 );
    valid = valid && !rocketnet_server_gso_publish_receive( queue );
    valid = valid && queue->ready_buffer == &queue->buffers[0] && queue->worker_buffer == &queue->buffers[2];

    // Main claims the ready buffer; its previous buffer becomes free.

    rocketnet_server_gso_receive_packets( gso );
    valid = valid && gso->process_packets.num_packets == 2 &&
            gso->process_packets.packet_data[0] == queue->buffers[0].packet_data[0] &&
            queue->main_buffer == &queue->buffers[0] && queue->free_buffer == &queue->buffers[1] &&
            gso->counter_packets_received == 2;

    // A retained tail stays in main's buffer: with one packet consumed, the
    // next receive delivers the tail and claims nothing new.

    queue->main_cursor = 1;
    gso->process_packets.num_packets = 0;
    rocketnet_server_gso_receive_from_queue( gso, queue, 8 );
    valid = valid && gso->process_packets.num_packets == 1 &&
            gso->process_packets.packet_data[0] == queue->buffers[0].packet_data[1] &&
            queue->main_buffer == &queue->buffers[0] && queue->main_cursor == 2;

    // Publishing while main holds that buffer lands in the ready slot only:
    // the held buffer's packets and bytes are untouched.

    valid = valid && rocketnet_server_gso_publish_receive( queue );
    rocketnet_server_gso_test_fill( queue->worker_buffer, (uint8_t) 'c', 1 );
    valid = valid && !rocketnet_server_gso_publish_receive( queue );
    valid = valid && queue->main_buffer == &queue->buffers[0] && queue->buffers[0].num_packets == 2 &&
            queue->buffers[0].packet_data[0][0] == (uint8_t) 'a';

    // Once the held buffer is exhausted, main claims the waiting buffer.

    gso->process_packets.num_packets = 0;
    rocketnet_server_gso_receive_from_queue( gso, queue, 8 );
    valid = valid && gso->process_packets.num_packets == 1 && queue->main_buffer == &queue->buffers[2] &&
            queue->free_buffer == &queue->buffers[0] && gso->counter_packets_received == 3;

    // Transmit: two publishes empty the free FIFO; the third retains the
    // producer bank instead of waiting or overwriting a bank in flight.

    valid = valid && rocketnet_server_gso_test_stage( gso, 40000 );
    rocketnet_server_gso_send_packets( gso );
    valid = valid && rocketnet_server_gso_test_stage( gso, 40000 );
    rocketnet_server_gso_send_packets( gso );
    valid = valid && queue->send.producer == 2 && queue->send.ready_count == 2 && queue->send.free_count == 0 && queue->send.turn == 2;
    valid = valid && rocketnet_server_gso_test_stage( gso, 40000 );
    rocketnet_server_gso_send_packets( gso );
    valid = valid && queue->send.producer == 2 && queue->send.ready_count == 2 && queue->send.turn == 2;

    // The worker takes the published banks oldest first and returns each
    // only after its (here already finished) sends.

    bool final = false;
    int taken = rocketnet_server_gso_take_send( &gso->queues[0], &final );
    valid = valid && taken == 0 && !final;
    rocketnet_server_gso_return_send( &gso->queues[0], taken );
    taken = rocketnet_server_gso_take_send( &gso->queues[0], &final );
    valid = valid && taken == 1 && !final;
    rocketnet_server_gso_return_send( &gso->queues[0], taken );

    // Stop hands the worker the final producer after the ready FIFO; until
    // the queue's worker has exited the transport does not report stopped, so
    // destroy would free nothing.

    rocketnet_server_gso_stop( gso );
    valid = valid && !rocketnet_server_gso_stopped( gso );
    taken = rocketnet_server_gso_take_send( &gso->queues[0], &final );
    valid = valid && taken == 2 && final && queue->send.producer < 0;

    queue->stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

// The drain's read budget, on the fixture with the receive seam: a source
// answering full batches of one kind of discard (a zero-length datagram, a
// refused source, a datagram below the smallest server packet) far past the
// budget. Each drain returns after ROCKETNET_GSO_RECV_QUEUE_SIZE messages,
// every one counted as a drop; then datagrams the size pass accepts are
// staged as before.

enum rocketnet_server_gso_test_kind_t
{
    ROCKETNET_SERVER_GSO_TEST_EMPTY,
    ROCKETNET_SERVER_GSO_TEST_REFUSED,
    ROCKETNET_SERVER_GSO_TEST_SHORT,
    ROCKETNET_SERVER_GSO_TEST_VALID,
    ROCKETNET_SERVER_GSO_TEST_GRO,              // a GRO run of ROCKETNET_GSO_RECV_MAX_SEGMENTS valid segments
    ROCKETNET_SERVER_GSO_TEST_GRO_CTRUNC,       // one valid datagram whose UDP_GRO control the kernel truncated (MSG_CTRUNC)
};

struct rocketnet_server_gso_test_source_t
{
    rocketnet_server_gso_test_kind_t kind;
    int remaining; // messages before the source reports an empty socket
    int delivered;
    int largest_vlen;
};

static int rocketnet_server_gso_test_source_receive( struct rocketnet_server_gso_queue_t * queue, int vlen, void * user )
{
    struct rocketnet_server_gso_test_source_t * source = (struct rocketnet_server_gso_test_source_t *) user;
    if ( source->remaining <= 0 )
    {
        errno = EAGAIN;
        return -1;
    }

    source->largest_vlen = vlen > source->largest_vlen ? vlen : source->largest_vlen;
    const int count = source->remaining < vlen ? source->remaining : vlen;

    for ( int i = 0; i < count; i++ )
    {
        struct msghdr * header = &queue->messages[i].msg_hdr;
        header->msg_controllen = 0;
        header->msg_flags = 0;
        memset( &queue->names[i], 0, sizeof( queue->names[i] ) );

        if ( source->kind == ROCKETNET_SERVER_GSO_TEST_REFUSED )
        {
            // a mapped source on the fixture's ipv4 queue
            struct sockaddr_in6 * in6 = (struct sockaddr_in6 *) &queue->names[i];
            in6->sin6_family = AF_INET6;
            in6->sin6_port = htons( 40000 );
            inet_pton( AF_INET6, "::ffff:1.2.3.4", &in6->sin6_addr );
        }
        else
        {
            struct sockaddr_in * in4 = (struct sockaddr_in *) &queue->names[i];
            in4->sin_family = AF_INET;
            in4->sin_port = htons( 40000 );
            in4->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        }

        int bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
        if ( source->kind == ROCKETNET_SERVER_GSO_TEST_EMPTY )
        {
            bytes = 0;
        }
        else if ( source->kind == ROCKETNET_SERVER_GSO_TEST_SHORT )
        {
            bytes = 1;
        }
        else if ( source->kind == ROCKETNET_SERVER_GSO_TEST_GRO )
        {
            const int segment_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
            bytes = segment_bytes * ROCKETNET_GSO_RECV_MAX_SEGMENTS;
            header->msg_control = queue->controls[i];
            header->msg_controllen = sizeof( queue->controls[i] );
            struct cmsghdr * cmsg = CMSG_FIRSTHDR( header );
            cmsg->cmsg_level = IPPROTO_UDP;
            cmsg->cmsg_type = UDP_GRO;
            cmsg->cmsg_len = CMSG_LEN( sizeof( int ) );
            memcpy( CMSG_DATA( cmsg ), &segment_bytes, sizeof( segment_bytes ) );
        }
        else if ( source->kind == ROCKETNET_SERVER_GSO_TEST_GRO_CTRUNC )
        {
            header->msg_control = queue->controls[i];
            header->msg_controllen = sizeof( queue->controls[i] );
            header->msg_flags = MSG_CTRUNC;
        }
        memset( queue->scratch[i], 'v', (size_t) bytes );
        queue->messages[i].msg_len = (unsigned int) bytes;
    }

    source->remaining -= count;
    source->delivered += count;

    return count;
}

bool rocketnet_server_gso_test_drain_budget()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    gso->test_receive = rocketnet_server_gso_test_source_receive;

    bool valid = true;
    bool full = false;

    const rocketnet_server_gso_test_kind_t discards[] = { ROCKETNET_SERVER_GSO_TEST_EMPTY, ROCKETNET_SERVER_GSO_TEST_REFUSED,
                                                          ROCKETNET_SERVER_GSO_TEST_SHORT };

    for ( rocketnet_server_gso_test_kind_t kind : discards )
    {
        buffer->num_packets = 0;
        buffer->dropped_size = 0;
        buffer->dropped_source = 0;

        struct rocketnet_server_gso_test_source_t source = { kind, 1000000, 0, 0 };
        gso->test_receive_user = &source;
        rocketnet_server_gso_drain( queue, &full );

        const uint64_t dropped = kind == ROCKETNET_SERVER_GSO_TEST_REFUSED ? buffer->dropped_source : buffer->dropped_size;
        valid = valid && source.delivered == ROCKETNET_GSO_RECV_QUEUE_SIZE && buffer->num_packets == 0 &&
                dropped == (uint64_t) ROCKETNET_GSO_RECV_QUEUE_SIZE;
    }

    buffer->num_packets = 0;
    struct rocketnet_server_gso_test_source_t source = { ROCKETNET_SERVER_GSO_TEST_VALID, 3, 0, 0 };
    gso->test_receive_user = &source;
    rocketnet_server_gso_drain( queue, &full );
    valid = valid && source.delivered == 3 && buffer->num_packets == 3 &&
            buffer->packet_bytes[2] == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES && buffer->packet_data[2][0] == (uint8_t) 'v';

    // no worker ran, so stop is complete once both stopped flags are set

    gso->test_receive = NULL;
    gso->test_receive_user = NULL;
    rocketnet_server_gso_stop( gso );
    queue->stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

// The worker's prefilter before banking, on the fixture with the receive
// seam: each datagram the source answers is the one the test wrote. With the
// packet filter set, a keep-alive under the right filter value is banked; one
// under another value, a type only a server sends and a one-byte datagram
// are each dropped before a slot, counted by reason in the queue's tally and
// in the transport's own counters, and the driver's totals read the tally.
struct rocketnet_server_gso_test_datagram_t
{
    const uint8_t * data;
    int bytes;
    int remaining;
};

static int rocketnet_server_gso_test_datagram_receive( struct rocketnet_server_gso_queue_t * queue, int vlen, void * user )
{
    struct rocketnet_server_gso_test_datagram_t * datagram = (struct rocketnet_server_gso_test_datagram_t *) user;
    if ( datagram->remaining <= 0 )
    {
        errno = EAGAIN;
        return -1;
    }

    const int count = datagram->remaining < vlen ? datagram->remaining : vlen;
    for ( int i = 0; i < count; i++ )
    {
        struct msghdr * header = &queue->messages[i].msg_hdr;
        header->msg_controllen = 0;
        header->msg_flags = 0;
        memset( &queue->names[i], 0, sizeof( queue->names[i] ) );
        struct sockaddr_in * in4 = reinterpret_cast<struct sockaddr_in *>( &queue->names[i] );
        in4->sin_family = AF_INET;
        in4->sin_port = htons( 40000 );
        in4->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        memcpy( queue->scratch[i], datagram->data, (size_t) datagram->bytes );
        queue->messages[i].msg_len = (unsigned int) datagram->bytes;
    }

    datagram->remaining -= count;
    return count;
}

bool rocketnet_server_gso_test_drain_prefilter()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x5A, 0x17, 0xC3, 0x08 };
    const uint8_t other[ROCKETNET_FILTER_VALUE_BYTES] = { 0x5A, 0x17, 0xC3, 0x09 };
    gso->has_filter = true;
    memcpy( gso->packet_filter, filter, sizeof( filter ) );

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    gso->test_receive = rocketnet_server_gso_test_datagram_receive;

    uint8_t keep_alive[ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES];
    memset( keep_alive, 'k', sizeof( keep_alive ) );
    uint8_t * p = keep_alive;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, filter );

    uint8_t wrong_value[ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES];
    memset( wrong_value, 'k', sizeof( wrong_value ) );
    p = wrong_value;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, other );

    uint8_t denied[ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES];
    memset( denied, 'd', sizeof( denied ) );
    p = denied;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_DENIED_PACKET, filter );

    const uint8_t one_byte[1] = { 0 };

    bool valid = true;
    bool full = false;

    struct rocketnet_server_gso_test_datagram_t datagram = { keep_alive, (int) sizeof( keep_alive ), 1 };
    gso->test_receive_user = &datagram;
    rocketnet_server_gso_drain( queue, &full );
    valid = valid && buffer->num_packets == 1 && buffer->packet_bytes[0] == (int) sizeof( keep_alive ) &&
            memcmp( buffer->packet_data[0], keep_alive, sizeof( keep_alive ) ) == 0;

    datagram = { wrong_value, (int) sizeof( wrong_value ), 2 };
    rocketnet_server_gso_drain( queue, &full );
    datagram = { denied, (int) sizeof( denied ), 1 };
    rocketnet_server_gso_drain( queue, &full );
    datagram = { one_byte, 1, 1 };
    rocketnet_server_gso_drain( queue, &full );

    valid = valid && buffer->num_packets == 1 && buffer->dropped_prefilter == 3 && buffer->dropped_size == 1;

    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS];
    rocketnet_server_gso_receive_drop_totals( gso, totals );
    for ( int drop = 0; drop < ROCKETNET_RECEIVE_NUM_DROPS; drop++ )
    {
        uint64_t expect = 0;
        if ( drop == ROCKETNET_RECEIVE_DROP_WRONG_VALUE )
        {
            expect = 2;
        }
        else if ( drop == ROCKETNET_RECEIVE_DROP_WRONG_DIRECTION || drop == ROCKETNET_RECEIVE_DROP_TOO_SHORT )
        {
            expect = 1;
        }
        valid = valid && totals[drop] == expect;
    }

    // the transport's own counters fold the buffer once main claims it

    valid = valid && rocketnet_server_gso_publish_receive( queue );
    rocketnet_server_gso_receive_packets( gso );
    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    valid = valid && counters[ROCKETNET_COUNTER_GSO_PACKETS_RECEIVED] == 1 &&
            rocketnet_server_gso_receive_dropped_prefilter( gso ) == 3 &&
            counters[ROCKETNET_COUNTER_GSO_RECEIVE_DROPPED_SIZE] == 1;

    // no worker ran, so stop is complete once the queue's stopped flag is set

    gso->test_receive = NULL;
    gso->test_receive_user = NULL;
    rocketnet_server_gso_stop( gso );
    queue->stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

// The drain reads only what fits, on the fixture with the receive seam: each
// recvmmsg asks for no more messages than the buffer has slots for every
// segment of (ROCKETNET_GSO_RECV_MAX_SEGMENTS each with GRO), and a buffer
// with no room for one more message reads nothing, so a full batch is never
// read into a nearly full buffer and dropped as overflow. The datagrams the
// drain did not read stay with the source, the socket, for the next buffer.

bool rocketnet_server_gso_test_drain_room()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    gso->test_receive = rocketnet_server_gso_test_source_receive;

    bool valid = true;
    bool full = false;

    // GRO on, one slot free: not one message of 64 segments fits

    gso->features.fetch_or( ROCKETNET_SERVER_GSO_FEATURE_GRO, std::memory_order_relaxed );
    buffer->num_packets = ROCKETNET_GSO_RECV_QUEUE_SIZE - 1;
    buffer->dropped_overflow = 0;
    struct rocketnet_server_gso_test_source_t gro = { ROCKETNET_SERVER_GSO_TEST_GRO, ROCKETNET_GSO_RECV_BATCH, 0, 0 };
    gso->test_receive_user = &gro;
    rocketnet_server_gso_drain( queue, &full );
    valid = valid && gro.delivered == 0 && gro.remaining == ROCKETNET_GSO_RECV_BATCH && buffer->dropped_overflow == 0 &&
            buffer->num_packets == ROCKETNET_GSO_RECV_QUEUE_SIZE - 1;

    // room for two runs and part of a third: two are read, every segment
    // staged, and the drain stops with the rest unread

    buffer->num_packets = ROCKETNET_GSO_RECV_QUEUE_SIZE - ( 2 * ROCKETNET_GSO_RECV_MAX_SEGMENTS + 5 );
    rocketnet_server_gso_drain( queue, &full );
    valid = valid && gro.delivered == 2 && gro.largest_vlen == 2 && buffer->dropped_overflow == 0 &&
            buffer->num_packets == ROCKETNET_GSO_RECV_QUEUE_SIZE - 5;

    // an empty buffer asks for a whole batch

    buffer->num_packets = 0;
    gro.largest_vlen = 0;
    rocketnet_server_gso_drain( queue, &full );
    valid = valid && gro.largest_vlen == ROCKETNET_GSO_RECV_BATCH && buffer->dropped_overflow == 0 &&
            buffer->num_packets == (uint32_t) ( ( ROCKETNET_GSO_RECV_BATCH - 2 ) * ROCKETNET_GSO_RECV_MAX_SEGMENTS );

    // GRO off, three slots free: three datagrams are read, the rest wait

    gso->features.fetch_and( ~ROCKETNET_SERVER_GSO_FEATURE_GRO, std::memory_order_relaxed );
    buffer->num_packets = ROCKETNET_GSO_RECV_QUEUE_SIZE - 3;
    struct rocketnet_server_gso_test_source_t plain = { ROCKETNET_SERVER_GSO_TEST_VALID, ROCKETNET_GSO_RECV_BATCH, 0, 0 };
    gso->test_receive_user = &plain;
    rocketnet_server_gso_drain( queue, &full );
    valid = valid && plain.delivered == 3 && plain.largest_vlen == 3 && buffer->dropped_overflow == 0 &&
            buffer->num_packets == ROCKETNET_GSO_RECV_QUEUE_SIZE;

    // no worker ran, so stop is complete once both stopped flags are set

    gso->test_receive = NULL;
    gso->test_receive_user = NULL;
    rocketnet_server_gso_stop( gso );
    queue->stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

// The GRO cmsg faults reach the counters, on the fixture with the receive
// seam: three datagrams whose UDP_GRO control was truncated are each one
// fault and one datagram. The worker step drains and publishes them, main's
// receive folds the bank, and GRO_CMSG_FAULTS reads 3, as io_uring's does.

bool rocketnet_server_gso_test_gro_cmsg_faults()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    gso->features.fetch_or( ROCKETNET_SERVER_GSO_FEATURE_GRO, std::memory_order_relaxed );
    gso->test_receive = rocketnet_server_gso_test_source_receive;

    struct rocketnet_server_gso_test_source_t source = { ROCKETNET_SERVER_GSO_TEST_GRO_CTRUNC, 3, 0, 0 };
    gso->test_receive_user = &source;

    bool valid = true;

    valid = valid && !rocketnet_server_gso_worker_step( queue );
    valid = valid && source.delivered == 3 && queue->ready_buffer != NULL;

    rocketnet_server_gso_receive_from_queue( gso, queue, 8 );

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    valid = valid && counters[ROCKETNET_COUNTER_GSO_PACKETS_RECEIVED] == 3;
    valid = valid && counters[ROCKETNET_COUNTER_GSO_GRO_BATCHES] == 0;
    valid = valid && counters[ROCKETNET_COUNTER_GSO_GRO_CMSG_FAULTS] == 3;

    // no worker ran, so stop is complete once both stopped flags are set

    gso->test_receive = NULL;
    gso->test_receive_user = NULL;
    rocketnet_server_gso_stop( gso );
    queue->stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

bool rocketnet_server_gso_test_receive_backoff()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    gso->test_receive = rocketnet_server_gso_test_source_receive;

    bool valid = true;

    struct rocketnet_server_gso_test_source_t source = { ROCKETNET_SERVER_GSO_TEST_VALID, 1000, 0, 0 };
    gso->test_receive_user = &source;

    // A full worker buffer with a published bank main has not claimed and no
    // free buffer cannot be published: every pass reads nothing, leaves the
    // buffer as it is and asks the worker to sleep.

    queue->ready_buffer = &queue->buffers[2];
    queue->free_buffer = NULL;
    buffer->num_packets = ROCKETNET_GSO_RECV_QUEUE_SIZE;
    buffer->dropped_size = 0;
    buffer->dropped_overflow = 0;
    buffer->dropped_source = 0;
    buffer->gro_batches = 0;
    buffer->gro_cmsg_faults = 0;
    buffer->receive_errors = 0;

    int waits = 0;
    for ( int i = 0; i < 1000; i++ )
    {
        waits += rocketnet_server_gso_worker_step( queue ) ? 1 : 0;
    }

    valid = valid && waits == 1000 && source.delivered == 0 &&
            buffer->num_packets == ROCKETNET_GSO_RECV_QUEUE_SIZE && queue->ready_buffer == &queue->buffers[2];

    // Main's claim frees a buffer: the next pass publishes the full buffer and
    // does not back off, and the pass after it reads the waiting packets.

    rocketnet_server_gso_receive_from_queue( gso, queue, 8 );
    valid = valid && queue->main_buffer == &queue->buffers[2] && queue->free_buffer == &queue->buffers[1];
    valid = valid && !rocketnet_server_gso_worker_step( queue );
    valid = valid && queue->ready_buffer == buffer && queue->worker_buffer == &queue->buffers[1];
    valid = valid && !rocketnet_server_gso_worker_step( queue );
    valid = valid && source.delivered == 1000 && queue->worker_buffer->num_packets == 1000;

    // A failed receive backs off too, though the buffer is not full.

    rocketnet_server_gso_test_fail_receives( gso, 1, EIO );
    valid = valid && rocketnet_server_gso_worker_step( queue );
    valid = valid && queue->worker_buffer->receive_errors == 1;

    gso->test_receive = NULL;
    gso->test_receive_user = NULL;
    rocketnet_server_gso_stop( gso );
    queue->stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

bool rocketnet_server_gso_test_send_refusals()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    bool valid = true;

    // three messages to three destinations. an unroutable destination
    // refuses its own message only: three attempts, one packet dropped each

    for ( int i = 0; i < 3; i++ )
    {
        valid = valid && rocketnet_server_gso_test_stage( gso, (uint16_t) ( 40000 + i ) );
    }
    rocketnet_server_gso_send_packets( gso );

    bool final = false;
    int taken = rocketnet_server_gso_take_send( &gso->queues[0], &final );
    valid = valid && taken >= 0 && !final;

    rocketnet_server_gso_test_fail_sends( gso, 3, EHOSTUNREACH );
    bool disable_gso = false;
    bool full = false;
    struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[0].send;
    valid = valid && rocketnet_server_gso_gather_bank( gso, &gso->queues[0], &lane->banks[taken], 3 ) == 3;
    int next = rocketnet_server_gso_flush( gso, &gso->queues[0], &lane->banks[taken], 0, 3, &disable_gso, &full );
    valid = valid && next == 3 && !disable_gso && !full;
    valid = valid && lane->counter_send_syscalls == 3 && lane->counter_send_dropped == 3 && lane->counter_packets_sent == 0;
    valid = valid && gso->test_fail_sends.load( std::memory_order_relaxed ) == 0;
    rocketnet_server_gso_return_send( &gso->queues[0], taken );

    rocketnet_server_gso_stop( gso );
    rocketnet_server_gso_take_send( &gso->queues[0], &final );
    valid = valid && final;

    gso->queues[0].stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

// a fake socket for the send seam: it takes accept_budget messages, then
// answers EAGAIN as a full socket does, and records each taken message's
// destination port in order
struct rocketnet_server_gso_test_full_socket_t
{
    int accept_budget;
    int calls;
    int num_ports;
    uint16_t ports[256];
};

static int rocketnet_server_gso_test_full_socket_send( struct mmsghdr * messages, unsigned int count, void * user )
{
    struct rocketnet_server_gso_test_full_socket_t * socket = (struct rocketnet_server_gso_test_full_socket_t *) user;
    socket->calls++;
    if ( socket->accept_budget <= 0 )
    {
        errno = EAGAIN;
        return -1;
    }
    int taken = (int) count < socket->accept_budget ? (int) count : socket->accept_budget;
    for ( int i = 0; i < taken; i++ )
    {
        const struct sockaddr_in * name = (const struct sockaddr_in *) messages[i].msg_hdr.msg_name;
        if ( socket->num_ports < 256 )
        {
            socket->ports[socket->num_ports++] = rocketnet_platform_ntohs( name->sin_port );
        }
    }
    socket->accept_budget -= taken;
    return taken;
}

bool rocketnet_server_gso_test_send_full_keeps_rest()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    bool valid = true;

    // 200 plain packets to 200 destinations: two sendmmsg batches (128 and
    // 71 messages). Slot 60 goes to an IPv6 destination this IPv4 socket
    // cannot carry, a run the first pass drops and counts.

    const int count = 200;
    const int unroutable = 60;
    for ( int i = 0; i < count; i++ )
    {
        if ( i != unroutable )
        {
            valid = valid && rocketnet_server_gso_test_stage( gso, (uint16_t) ( 40000 + i ) );
            continue;
        }
        struct rocketnet_address_t to;
        memset( &to, 0, sizeof( to ) );
        to.type = ROCKETNET_ADDRESS_IPV6;
        to.data.ipv6[7] = 1;
        to.port = (uint16_t) ( 40000 + i );
        uint64_t packet_id = 0;
        uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
        valid = valid && packet;
        if ( packet )
        {
            rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
        }
    }
    rocketnet_server_gso_send_packets( gso );

    bool final = false;
    const int taken = rocketnet_server_gso_take_send( &gso->queues[0], &final );
    valid = valid && taken >= 0 && !final;
    if ( !valid )
    {
        rocketnet_server_gso_stop( gso );
        gso->queues[0].stopped.store( true, std::memory_order_release );
        rocketnet_server_gso_test_destroy( gso );
        return false;
    }

    struct rocketnet_server_gso_test_full_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    gso->test_send = rocketnet_server_gso_test_full_socket_send;
    gso->test_send_user = &socket;

    // the socket takes 50 messages, then is full: the flush stops at the
    // first unsent slot, drops nothing, and does not try the second batch

    socket.accept_budget = 50;
    int slot = 0;
    struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[0].send;
    rocketnet_server_gso_send_buffer_t * bank = &lane->banks[taken];
    const int staged = bank->num_packets.load( std::memory_order_relaxed );
    valid = valid && staged == count;
    valid = valid && rocketnet_server_gso_gather_bank( gso, &gso->queues[0], bank, staged ) == staged;
    valid = valid && !rocketnet_server_gso_send_bank( gso, &gso->queues[0], bank, &slot, staged );
    valid = valid && slot == 50 && socket.calls == 2;
    valid = valid && lane->counter_send_syscalls == 2 && lane->counter_packets_sent == 50 && lane->counter_send_dropped == 1;

    // the next attempt sends the rest, in order, and counts the dropped run
    // once

    socket.accept_budget = 1000;
    valid = valid && rocketnet_server_gso_send_bank( gso, &gso->queues[0], bank, &slot, staged );
    valid = valid && slot == count;
    valid = valid && lane->counter_packets_sent == (uint64_t) ( count - 1 ) && lane->counter_send_dropped == 1;
    valid = valid && socket.num_ports == count - 1;
    int expected = 0;
    for ( int i = 0; i < socket.num_ports; i++, expected++ )
    {
        if ( expected == unroutable )
        {
            expected++;
        }
        valid = valid && socket.ports[i] == (uint16_t) ( 40000 + expected );
    }

    gso->test_send = NULL;
    gso->test_send_user = NULL;
    rocketnet_server_gso_return_send( &gso->queues[0], taken );

    rocketnet_server_gso_stop( gso );
    rocketnet_server_gso_take_send( &gso->queues[0], &final );
    valid = valid && final;

    gso->queues[0].stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

void rocketnet_server_gso_test_set_send( struct rocketnet_server_gso_t * gso,
                                         int ( *send )( struct mmsghdr * messages, unsigned int count, void * user ),
                                         void * user )
{
    rocketnet_assert( gso );
    gso->test_send = send;
    gso->test_send_user = user;
}

void rocketnet_server_gso_test_set_features( struct rocketnet_server_gso_t * gso, int features )
{
    rocketnet_assert( gso );
    gso->features.store( features, std::memory_order_relaxed );
}

bool rocketnet_server_gso_test_flush_staged( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    rocketnet_server_gso_send_packets( gso );

    // every queue that published a bank sends it on the calling thread, in
    // queue order
    bool any = false;
    bool sent = true;
    for ( int i = 0; i < gso->num_queues; i++ )
    {
        struct rocketnet_server_gso_queue_t * queue = &gso->queues[i];
        bool final = false;
        const int taken = rocketnet_server_gso_take_send( queue, &final );
        if ( taken < 0 )
        {
            continue;
        }
        if ( final )
        {
            return false;
        }
        any = true;

        rocketnet_server_gso_send_buffer_t * bank = &queue->send.banks[taken];
        int count = bank->num_packets.load( std::memory_order_relaxed );
        if ( count > ROCKETNET_GSO_SEND_QUEUE_SIZE )
        {
            count = ROCKETNET_GSO_SEND_QUEUE_SIZE;
        }

        const int positions = rocketnet_server_gso_gather_bank( gso, queue, bank, count );
        int slot = 0;
        sent = rocketnet_server_gso_send_bank( gso, queue, bank, &slot, positions ) && slot == positions && sent;
        rocketnet_server_gso_return_send( queue, taken );
    }

    return any && sent;
}

// Per-queue staging (proofs/tla/RNGsoQueueBankLayout.tla), on a four-queue
// fixture: each packet is staged on the queue its destination hashes to, its
// id names that queue and slot, finish and abort reach the slot through the
// id, ids stay distinct across a turn-over, and a pointer into another queue's
// staging or between slots is refused. No socket, no thread, no clock.
bool rocketnet_server_gso_test_send_queues()
{
    const int num_queues = 4;
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( num_queues );
    if ( !gso )
    {
        return false;
    }

    bool valid = true;
    int expect[4] = { 0, 0, 0, 0 };
    uint8_t * first_packet[4] = { NULL, NULL, NULL, NULL };
    uint64_t first_id[4] = { 0, 0, 0, 0 };

    for ( int i = 0; i < 64; i++ )
    {
        struct rocketnet_address_t to;
        memset( &to, 0, sizeof( to ) );
        to.type = ROCKETNET_ADDRESS_IPV4;
        to.data.ipv4[0] = 198;
        to.data.ipv4[1] = 51;
        to.data.ipv4[2] = 100;
        to.data.ipv4[3] = 7;
        to.port = (uint16_t) ( 50000 + i );

        const int q = rocketnet_server_gso_send_queue( &to, num_queues );
        valid = valid && q >= 0 && q < num_queues;
        if ( q < 0 || q >= num_queues )
        {
            break;
        }

        uint64_t packet_id = 0;
        uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
        valid = valid && packet;
        if ( !packet )
        {
            break;
        }

        // the id is ( turn * slots + slot ) * queues + queue, turn 0 here
        const rocketnet_server_gso_send_buffer_t * bank = &gso->queues[q].send.banks[gso->queues[q].send.producer];
        valid = valid && packet_id == (uint64_t) expect[q] * (uint64_t) num_queues + (uint64_t) q;
        valid = valid && packet == bank->data[expect[q]];
        valid = valid && rocketnet_address_equal( &bank->info[expect[q]].to, &to );
        if ( !first_packet[q] )
        {
            first_packet[q] = packet;
            first_id[q] = packet_id;
        }
        expect[q]++;
        rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }

    int used = 0;
    for ( int q = 0; q < num_queues; q++ )
    {
        const struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[q].send;
        valid = valid && expect[q] > 0;
        valid = valid && lane->banks[lane->producer].num_packets.load( std::memory_order_relaxed ) == expect[q];
        used = expect[q] > used ? expect[q] : used;
    }
    valid = valid && rocketnet_server_gso_send_room( gso ) == ROCKETNET_GSO_SEND_QUEUE_SIZE - used;

    // a finish naming the wrong queue, or a pointer between slots, is refused:
    // the slot keeps its bytes, and a refused finish is counted
    valid = valid && first_packet[0] != NULL && first_packet[1] != NULL;
    if ( first_packet[0] && first_packet[1] )
    {
        struct rocketnet_server_gso_send_lane_t * lane = &gso->queues[0].send;
        rocketnet_server_gso_finish_packet( gso, first_id[1], first_packet[0], ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        valid = valid && lane->banks[lane->producer].info[0].packet_bytes == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
        valid = valid && gso->counter_send_finish_refused.load( std::memory_order_relaxed ) == 1;
        rocketnet_server_gso_abort_packet( gso, first_id[0], first_packet[0] + 1 );
        valid = valid && lane->banks[lane->producer].info[0].packet_bytes == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
        valid = valid && gso->counter_send_finish_refused.load( std::memory_order_relaxed ) == 1;
    }

    // every queue sends its own bank; the turn moves on, so the next packet
    // on slot 0 of a queue carries a new id
    struct rocketnet_server_gso_test_full_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.accept_budget = 1000;
    rocketnet_server_gso_test_set_send( gso, rocketnet_server_gso_test_full_socket_send, &socket );
    valid = valid && rocketnet_server_gso_test_flush_staged( gso );
    valid = valid && socket.num_ports == 64;

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    valid = valid && counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] == 64;
    for ( int q = 0; q < num_queues; q++ )
    {
        valid = valid && gso->queues[q].send.turn == 1;
        valid = valid && gso->queues[q].send.counter_packets_sent.load( std::memory_order_relaxed ) == (uint64_t) expect[q];
    }
    valid = valid && rocketnet_server_gso_test_send_idle( gso );

    if ( first_packet[0] )
    {
        struct rocketnet_address_t to;
        memset( &to, 0, sizeof( to ) );
        to.type = ROCKETNET_ADDRESS_IPV4;
        to.data.ipv4[0] = 198;
        to.data.ipv4[1] = 51;
        to.data.ipv4[2] = 100;
        to.data.ipv4[3] = 7;
        for ( int i = 0; i < 64; i++ )
        {
            to.port = (uint16_t) ( 50000 + i );
            if ( rocketnet_server_gso_send_queue( &to, num_queues ) == 0 )
            {
                break;
            }
        }
        uint64_t packet_id = 0;
        uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
        valid = valid && packet && packet_id != first_id[0];
        valid = valid && packet_id == (uint64_t) ROCKETNET_GSO_SEND_QUEUE_SIZE * (uint64_t) num_queues;
        rocketnet_server_gso_abort_packet( gso, packet_id, packet );
    }

    rocketnet_server_gso_test_set_send( gso, NULL, NULL );
    rocketnet_server_gso_test_close( gso );
    return valid;
}

// a socket that stays full after stop. the send seam answers EAGAIN, as a
// full socket does, and the stepped worker retries at once instead of
// waiting. the worker must notice quit on that failure and leave,
// within ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT further attempts. one attempt
// past that bound means it retried with the socket still full: the seam then
// accepts the message so this thread is not stuck, and the test fails.
struct rocketnet_server_gso_test_retry_quit_t
{
    struct rocketnet_server_gso_t * gso;
    int calls_after_quit;
    bool retried;
};

static int rocketnet_server_gso_test_retry_quit_send( struct mmsghdr * messages, unsigned int count, void * user )
{
    (void) messages;
    struct rocketnet_server_gso_test_retry_quit_t * retry = (struct rocketnet_server_gso_test_retry_quit_t *) user;
    if ( retry->gso->queues[0].quit.load( std::memory_order_acquire ) )
    {
        retry->calls_after_quit++;
    }
    // the failed attempt itself is not a further retry. past the bound, the
    // socket is still full and the worker came back anyway.
    if ( retry->calls_after_quit > ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 1 )
    {
        retry->retried = true;
        return (int) count;
    }
    errno = EAGAIN;
    return -1;
}

bool rocketnet_server_gso_test_send_retry_quit()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    bool valid = rocketnet_server_gso_test_stage( gso, 40000 );
    rocketnet_server_gso_send_packets( gso );

    struct rocketnet_server_gso_test_retry_quit_t retry;
    memset( &retry, 0, sizeof( retry ) );
    retry.gso = gso;
    gso->test_send = rocketnet_server_gso_test_retry_quit_send;
    gso->test_send_user = &retry;

    rocketnet_server_gso_stop( gso );
    const bool finished = rocketnet_server_gso_test_run_send( gso, &gso->queues[0] );

    valid = valid && !retry.retried && retry.calls_after_quit == ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 1;
    valid = valid && finished;
    gso->queues[0].stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    gso->test_send = NULL;
    gso->test_send_user = NULL;
    rocketnet_server_gso_test_destroy( gso );
    return valid;
}

// a socket that stays full once the server's shutdown has begun, before stop.
// The server asks for stop only when send_idle is true, and send_idle is false
// while the queue's worker holds a bank, so the worker must give up on the full
// socket without quit. Bank A (port 40000) meets a full socket; bank B
// (port 40001) is ready behind it. The worker must leave A after the bounded
// retries and go on to B. A second attempt at A means it retried with the
// socket still full: the seam then stops the transport, which the stop path
// handles, so this thread is not stuck, and the test fails. B is accepted and
// stops the transport, so the worker exits.
struct rocketnet_server_gso_test_shutdown_full_t
{
    struct rocketnet_server_gso_t * gso;
    int calls_a;
    bool retried;
    bool sent_b;
};

static int rocketnet_server_gso_test_shutdown_full_send( struct mmsghdr * messages, unsigned int count, void * user )
{
    struct rocketnet_server_gso_test_shutdown_full_t * full = (struct rocketnet_server_gso_test_shutdown_full_t *) user;
    const struct sockaddr_in * to = (const struct sockaddr_in *) messages[0].msg_hdr.msg_name;
    if ( to && ntohs( to->sin_port ) == 40001 )
    {
        full->sent_b = true;
        rocketnet_server_gso_stop( full->gso );
        return (int) count;
    }

    full->calls_a++;
    if ( full->calls_a > ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 1 )
    {
        full->retried = true;
        rocketnet_server_gso_stop( full->gso );
    }
    errno = EAGAIN;
    return -1;
}

bool rocketnet_server_gso_test_send_shutdown_full()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    bool valid = rocketnet_server_gso_test_stage( gso, 40000 );
    rocketnet_server_gso_send_packets( gso );
    valid = valid && rocketnet_server_gso_test_stage( gso, 40001 );
    rocketnet_server_gso_send_packets( gso );
    valid = valid && gso->queues[0].send.ready_count == 2;

    struct rocketnet_server_gso_test_shutdown_full_t full;
    memset( &full, 0, sizeof( full ) );
    full.gso = gso;
    gso->test_send = rocketnet_server_gso_test_shutdown_full_send;
    gso->test_send_user = &full;

    rocketnet_server_gso_begin_shutdown( gso );
    const bool finished = rocketnet_server_gso_test_run_send( gso, &gso->queues[0] );

    valid = valid && !full.retried && full.calls_a == ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 1 && full.sent_b;
    // bank A's one packet was left: it is counted as dropped, B's is sent
    valid = valid && gso->queues[0].send.counter_send_dropped == 1 && gso->queues[0].send.counter_packets_sent == 1;
    valid = valid && finished;
    gso->queues[0].stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    gso->test_send = NULL;
    gso->test_send_user = NULL;
    rocketnet_server_gso_test_destroy( gso );
    return valid;
}

// the bounded retry after shutdown, from both sides. a bank holds a packet to
// port 40000 and one to port 40002, then a packet to an IPv6 destination this
// IPv4 socket cannot carry (the first pass drops and counts it) and an
// aborted slot: neither is a packet the tail can drop again. the seam accepts
// 40000; 40002 meets a full socket `refusals` times and is then accepted. the
// first refusal also stops the transport, so the worker exits whatever it
// does with the bank.
// a refusal past the bound's last retry means the worker kept retrying: the
// seam then accepts so this thread is not stuck, and the test fails.
struct rocketnet_server_gso_test_bounded_retry_t
{
    struct rocketnet_server_gso_t * gso;
    int refusals;
    int calls_tail;
    bool retried;
};

static int rocketnet_server_gso_test_bounded_retry_send( struct mmsghdr * messages, unsigned int count, void * user )
{
    struct rocketnet_server_gso_test_bounded_retry_t * retry = (struct rocketnet_server_gso_test_bounded_retry_t *) user;

    int accepted = 0;
    while ( accepted < (int) count )
    {
        const struct sockaddr_in * to = (const struct sockaddr_in *) messages[accepted].msg_hdr.msg_name;
        if ( !to || ntohs( to->sin_port ) != 40000 )
        {
            break;
        }
        accepted++;
    }
    if ( accepted > 0 )
    {
        return accepted;
    }

    retry->calls_tail++;
    if ( retry->calls_tail == 1 )
    {
        rocketnet_server_gso_stop( retry->gso );
    }
    // the first refusal ends the attempt that sent 40000: it made progress,
    // so it does not count, and the bound's attempts follow it
    if ( retry->calls_tail > retry->refusals || retry->calls_tail > ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 2 )
    {
        retry->retried = retry->calls_tail > ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 2;
        return (int) count;
    }
    errno = EAGAIN;
    return -1;
}

static bool rocketnet_server_gso_test_bounded_retry( int refusals, uint64_t expect_sent, uint64_t expect_dropped )
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
    {
        return false;
    }

    bool valid = rocketnet_server_gso_test_stage( gso, 40000 );
    valid = valid && rocketnet_server_gso_test_stage( gso, 40002 );

    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV6;
    to.data.ipv6[7] = 1;
    to.port = 40003;
    uint64_t packet_id = 0;
    uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
    valid = valid && packet;
    if ( packet )
    {
        rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }

    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = 40004;
    packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
    valid = valid && packet;
    rocketnet_server_gso_abort_packet( gso, packet_id, packet );

    rocketnet_server_gso_send_packets( gso );
    valid = valid && gso->queues[0].send.ready_count == 1;

    struct rocketnet_server_gso_test_bounded_retry_t retry;
    memset( &retry, 0, sizeof( retry ) );
    retry.gso = gso;
    retry.refusals = refusals;
    gso->test_send = rocketnet_server_gso_test_bounded_retry_send;
    gso->test_send_user = &retry;

    rocketnet_server_gso_begin_shutdown( gso );
    const bool finished = rocketnet_server_gso_test_run_send( gso, &gso->queues[0] );

    const int expect_calls = refusals <= ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 1 ? refusals + 1 : ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 2;
    valid = valid && !retry.retried && retry.calls_tail == expect_calls;
    valid = valid && gso->queues[0].send.counter_packets_sent == expect_sent && gso->queues[0].send.counter_send_dropped == expect_dropped;
    valid = valid && finished;
    gso->queues[0].stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    gso->test_send = NULL;
    gso->test_send_user = NULL;
    rocketnet_server_gso_test_destroy( gso );
    return valid;
}

// the socket refuses the tail fewer times than the bound ends: once (one
// transient EAGAIN), then as many times as the bound allows retries, so the
// last allowed retry is accepted. both routable packets are sent; only the
// unroutable one is dropped.
bool rocketnet_server_gso_test_send_shutdown_within_bound()
{
    bool valid = rocketnet_server_gso_test_bounded_retry( 1, 2, 1 );
    valid = valid && rocketnet_server_gso_test_bounded_retry( ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 1, 2, 1 );
    return valid;
}

// the socket refuses the tail past the bound: the worker stops retrying, the
// one packet it never sent is dropped and counted beside the unroutable one,
// the cleared and aborted slots behind it are not counted, and stop completes
bool rocketnet_server_gso_test_send_shutdown_past_bound()
{
    return rocketnet_server_gso_test_bounded_retry( ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT + 3, 1, 2 );
}

#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

int rocketnet_server_gso_linux_cpp_dummy = 0;
