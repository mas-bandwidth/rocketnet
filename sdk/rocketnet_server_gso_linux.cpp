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
// Game threads fill a main-owned TX bank between flushes. send_packets only
// try-publishes ownership into a bounded FIFO; the dedicated TX worker builds
// coalesced sendmmsg batches and sends outside the ownership mutex. Hot bank
// metadata and destination/size arrays are separate from cold frame payloads.
//
// Every kernel feature is probed at create, and every probe failure selects
// the next tier down rather than an error: no UDP_SEGMENT -> per-packet
// sendmmsg entries; no UDP_GRO -> plain recvmmsg; and a kernel that accepts
// the UDP_SEGMENT probe but refuses a live GSO send (offload edge cases
// exist) flips the feature off and resends that flush's remainder plain --
// the degradation is per-send lossless, not a dropped batch.

#include "rocketnet_server_gso.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
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

static const int ROCKETNET_GSO_TX_BANKS = 3;

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

    uint32_t num_packets;
    uint64_t dropped_size;
    uint64_t dropped_too_large;     // the part of dropped_size above the receive ceiling or truncated
    uint64_t dropped_overflow;
    uint64_t dropped_source;
    uint64_t gro_batches;
    uint64_t gro_cmsg_faults;                   // a GRO cmsg this worker refused
    uint64_t receive_errors;
    rocketnet_address_t from[ROCKETNET_GSO_RECV_QUEUE_SIZE];
    int packet_bytes[ROCKETNET_GSO_RECV_QUEUE_SIZE];
    alignas( 16 ) uint8_t packet_data[ROCKETNET_GSO_RECV_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];

    uint8_t padding_1[4096];
};

struct rocketnet_server_gso_queue_t
{
    uint8_t padding_0[4096];

    int queue_index;
    int fd;
    bool ipv6;
    struct rocketnet_server_gso_t * gso;
    rocketnet_platform_thread_t * thread;

    uint8_t padding_1[4096];

    std::atomic<bool> quit{ 0 };
    std::atomic<bool> stopped{ false };

    uint8_t padding_2[4096];

    // Only ownership is protected by receive_mutex. The worker fills one
    // buffer, can publish one ready buffer, and the main thread holds one
    // view buffer. One of ready/free is null at each handoff.
    struct rocketnet_server_gso_recv_buffer_t * worker_buffer;
    struct rocketnet_server_gso_recv_buffer_t * ready_buffer;
    struct rocketnet_server_gso_recv_buffer_t * main_buffer;
    struct rocketnet_server_gso_recv_buffer_t * free_buffer;
    rocketnet_platform_mutex_t receive_mutex;
    uint32_t main_cursor = 0; // retained tail in main_buffer, main thread only
    // Idle EAGAIN calls still count without publishing an empty ready buffer.
    std::atomic<uint64_t> receive_syscalls{ 0 };

    uint8_t padding_3[4096];

    // worker-only recvmmsg scratch: the kernel fills these, the worker
    // splits and copies segments into its owned staging buffer

    struct mmsghdr messages[ROCKETNET_GSO_RECV_BATCH];
    struct iovec iovecs[ROCKETNET_GSO_RECV_BATCH];
    struct sockaddr_storage names[ROCKETNET_GSO_RECV_BATCH];
    char controls[ROCKETNET_GSO_RECV_BATCH][CMSG_SPACE( sizeof( int ) )];
    uint8_t scratch[ROCKETNET_GSO_RECV_BATCH][ROCKETNET_GSO_RECV_BUFFER_BYTES];

    uint8_t padding_4[4096];

    struct rocketnet_server_gso_recv_buffer_t buffers[3];

    uint8_t padding_5[4096];
};

struct rocketnet_server_gso_t
{
    void * context;

    int num_queues;
    bool stopping = false;
    std::atomic<int> features{ 0 };
    bool ipv6;
    rocketnet_address_t bind_address;

    struct rocketnet_server_gso_queue_t * queues;

    std::atomic<uint64_t> packet_id{ 0 };

    uint64_t send_rotation; // TX worker only: rotates across the group's sockets

    // Only indices cross this mutex. Main retains producer on contention or
    // full FIFO; worker returns a bank only after all synchronous sends finish.
    rocketnet_platform_mutex_t send_mutex = {};
    rocketnet_platform_wake_t * send_wake = NULL;
    rocketnet_platform_thread_t * send_thread = NULL;
    std::atomic<bool> send_quit{ false };
    std::atomic<bool> send_stopped{ false };
    // TX worker sets it while it holds a bank the socket refused; main's
    // next update wakes the worker to send the rest
    std::atomic<bool> send_full{ false };
    int send_producer = 0; // game fill threads joined before main publishes
    int send_free[ROCKETNET_GSO_TX_BANKS] = { 1, 2, 0 };
    int send_free_head = 0;
    int send_free_count = ROCKETNET_GSO_TX_BANKS - 1;
    int send_ready[ROCKETNET_GSO_TX_BANKS] = {};
    int send_ready_head = 0;
    int send_ready_count = 0;
    struct rocketnet_server_gso_send_buffer_t send_banks[ROCKETNET_GSO_TX_BANKS];

    // TX worker-only scratch: sendmmsg headers, gather iovecs (up to a
    // full super-datagram per message), destinations and segment cmsgs

    struct mmsghdr send_headers[ROCKETNET_GSO_SEND_BATCH];
    struct iovec send_iovecs[ROCKETNET_GSO_SEND_BATCH * ROCKETNET_GSO_MAX_SEGMENTS];
    struct sockaddr_storage send_names[ROCKETNET_GSO_SEND_BATCH];
    char send_controls[ROCKETNET_GSO_SEND_BATCH][CMSG_SPACE( sizeof( uint16_t ) )];

    // TX counters cross to main atomically. Receive totals are main-owned,
    // folded from claimed receive buffers and per-queue syscall atomics.
    std::atomic<uint64_t> counter_packets_sent{ 0 };
    std::atomic<uint64_t> counter_send_syscalls{ 0 };
    std::atomic<uint64_t> counter_send_dropped{ 0 };
    std::atomic<uint64_t> counter_gso_sends{ 0 };
    uint64_t counter_packets_received;
    uint64_t counter_receive_syscalls;
    uint64_t counter_receive_dropped_size;
    uint64_t counter_receive_dropped_too_large;
    uint64_t counter_receive_dropped_overflow;
    uint64_t counter_receive_dropped_source;
    uint64_t counter_receive_errors;
    uint64_t counter_gro_batches;
    uint64_t counter_gro_cmsg_faults;           // folded from the receive buffers; not a public counter

    struct rocketnet_server_socket_process_packets_t process_packets;

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

// main thread only: true when the TX path holds no work -- the producer is
// empty, no bank waits in the ready FIFO, and the worker has returned every
// bank it took. The worker counts a bank's sends and drops before it returns
// the bank under send_mutex, so once this answers true the send counters are
// final for every bank published so far.
bool rocketnet_server_gso_test_send_idle( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    if ( gso->send_producer < 0 )
        return true;
    if ( gso->send_banks[gso->send_producer].num_packets.load( std::memory_order_relaxed ) != 0 )
        return false;
    rocketnet_platform_mutex_acquire( &gso->send_mutex );

    const bool idle = gso->send_ready_count == 0 && gso->send_free_count == ROCKETNET_GSO_TX_BANKS - 1;
    rocketnet_platform_mutex_release( &gso->send_mutex );

    return idle;
}

bool rocketnet_server_gso_test_receive_ready( struct rocketnet_server_gso_t * gso, int queue_index )
{
    rocketnet_assert( gso );
    rocketnet_assert( queue_index >= 0 && queue_index < gso->num_queues );

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[queue_index];
    if ( !rocketnet_platform_mutex_try_acquire( &queue->receive_mutex ) )
        return false;
    const bool ready = queue->ready_buffer && queue->ready_buffer->num_packets > 0;
    rocketnet_platform_mutex_release( &queue->receive_mutex );

    return ready;
}

#endif // #if ROCKETNET_DEV

// proofs/tla/RNGsoHandoff.tla TxMainLock and TxPublish: main's frame boundary
// try-publishes exactly one complete bank. All fill threads have joined; no
// worker can see the producer until the exchange completes. A contended
// mutex or an empty free FIFO retains the producer: main never waits.
// RNGsoHandoff.tla TxMainWait has no such step here: the try-acquire above
// is the only acquire on main's path, so a held mutex defers the publish
// to the next frame instead of blocking.
// The releases that end each exchange are RNGsoHandoff.tla Unlock.
static bool rocketnet_server_gso_publish_send( rocketnet_server_gso_t * gso )
{
    if ( gso->stopping || gso->send_producer < 0 )
        return false;
    if ( gso->send_banks[gso->send_producer].num_packets.load( std::memory_order_relaxed ) == 0 )
        return true;
    if ( !rocketnet_platform_mutex_try_acquire( &gso->send_mutex ) )
        return false;
    if ( gso->send_free_count == 0 )
    {
        rocketnet_platform_mutex_release( &gso->send_mutex );
        return false;
    }

    const int old = gso->send_producer;
    gso->send_producer = gso->send_free[gso->send_free_head];
    gso->send_free_head = ( gso->send_free_head + 1 ) % ROCKETNET_GSO_TX_BANKS;
    --gso->send_free_count;
    gso->send_ready[( gso->send_ready_head + gso->send_ready_count ) % ROCKETNET_GSO_TX_BANKS] = old;
    ++gso->send_ready_count;
    rocketnet_platform_mutex_release( &gso->send_mutex );

    // Returned banks were reset by their previous owner before publication.

    rocketnet_assert( gso->send_banks[gso->send_producer].num_packets.load( std::memory_order_relaxed ) == 0 );
    if ( gso->send_wake )
        rocketnet_platform_wake_set( gso->send_wake );
    return true;
}

// proofs/tla/RNGsoHandoff.tla TxWorkerLock, TxTake and TxTakeNone: FIFO ready banks
// precede the final unpublished producer at stop. Quit is
// observed with ready emptiness under the handoff lock, so an earlier empty
// observation cannot strand a later publication. Stop's release transfers the
// last producer only after main has stopped filling or publishing it.
static int rocketnet_server_gso_take_send( rocketnet_server_gso_t * gso, bool * final )
{
    *final = false;

    int index = -1;
    rocketnet_platform_mutex_acquire( &gso->send_mutex );

    if ( gso->send_ready_count > 0 )
    {
        index = gso->send_ready[gso->send_ready_head];
        gso->send_ready_head = ( gso->send_ready_head + 1 ) % ROCKETNET_GSO_TX_BANKS;
        --gso->send_ready_count;
    }
    else if ( gso->send_quit.load( std::memory_order_acquire ) )
    {
        index = gso->send_producer;
        gso->send_producer = -1;
        *final = true;
    }

    rocketnet_platform_mutex_release( &gso->send_mutex );

    return index;
}

// proofs/tla/RNGsoHandoff.tla TxReturn: a bank rejoins the free FIFO only after
// its synchronous sends finished (the worker cleared it first).
static void rocketnet_server_gso_return_send( rocketnet_server_gso_t * gso, int index )
{
    gso->send_banks[index].num_packets.store( 0, std::memory_order_relaxed );
    rocketnet_platform_mutex_acquire( &gso->send_mutex );
    gso->send_free[( gso->send_free_head + gso->send_free_count ) % ROCKETNET_GSO_TX_BANKS] = index;
    ++gso->send_free_count;
    rocketnet_platform_mutex_release( &gso->send_mutex );
}

static int rocketnet_server_gso_flush( rocketnet_server_gso_t * gso,
                                       rocketnet_server_gso_send_buffer_t * bank, int first_slot, int num_slots, bool * disable_gso, bool * full );

// proofs/tla/RNGsoHandoff.tla TxSend: send a held bank from *slot. A live GSO
// refusal turns segmentation off and resends from the refused message.
// proofs/tla/RNGsoHandoff.tla TxSendFull: a full socket stops the bank at its
// first unsent slot and returns false, the rest kept in order in the held bank
// for the next attempt.
static bool rocketnet_server_gso_send_bank( rocketnet_server_gso_t * gso, rocketnet_server_gso_send_buffer_t * bank, int * slot, int count )
{
    while ( *slot < count )
    {
        bool disable_gso = false;
        bool full = false;
        *slot = rocketnet_server_gso_flush( gso, bank, *slot, count, &disable_gso, &full );
        if ( disable_gso )
            gso->features.fetch_and( ~ROCKETNET_SERVER_GSO_FEATURE_SEGMENT, std::memory_order_relaxed );
        if ( full )
            return false;
    }
    return true;
}

// proofs/tla/RNGsoHandoff.tla TxSend: the worker sends each taken bank outside
// the ownership mutex; stop's final bank is sent last and ends the worker.
static void rocketnet_server_gso_send_worker( rocketnet_platform_thread_t *, void * context )
{
    rocketnet_server_gso_t * gso = (rocketnet_server_gso_t *) context;

    for ( ;; )
    {
        bool final = false;
        const int index = rocketnet_server_gso_take_send( gso, &final );
        if ( index < 0 )
        {
            if ( final )
                break;
            rocketnet_platform_wake_wait( gso->send_wake, false, 0.1 );
            continue;
        }

        rocketnet_server_gso_send_buffer_t * bank = &gso->send_banks[index];
        int count = bank->num_packets.load( std::memory_order_relaxed );
        if ( count > ROCKETNET_GSO_SEND_QUEUE_SIZE )
            count = ROCKETNET_GSO_SEND_QUEUE_SIZE;
        // a full socket keeps the bank's unsent tail here, ahead of every
        // later bank; main's next update (or the retry timeout, once main
        // stops updating) wakes the worker to send it

        int slot = 0;
        while ( !rocketnet_server_gso_send_bank( gso, bank, &slot, count ) )
        {
            gso->send_full.store( true, std::memory_order_release );
            rocketnet_platform_wake_wait( gso->send_wake, false, ROCKETNET_GSO_SEND_RETRY_SECONDS );
        }
        gso->send_full.store( false, std::memory_order_release );

        if ( final )
            break;
        rocketnet_server_gso_return_send( gso, index );
    }

    // proofs/tla/RNGsoHandoff.tla TxWorkerExitIdle and the final-bank exit of
    // TxSend: the worker reports stopped only after its drain.

    gso->send_stopped.store( true, std::memory_order_release );
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
            return 0;

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

// ---------------------------------------------------------------------------

// one reuseport socket: family from the bind address, dual stack when ipv6,
// non-blocking, don't-fragment, buffers requested at the configured sizes
// with what the kernel actually granted read back and reported -- the box's
// default rmem/wmem sysctls are tiny, so never assume the request was
// honored.

static int rocketnet_server_gso_open_socket( const rocketnet_address_t * bind_address, int queue_index, uint16_t * bound_port )
{
    const bool ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;

    int fd = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( fd < 0 )
    {
        rocketnet_error( "gso transport: failed to create socket: %s", strerror( errno ) );
        return -1;
    }

    // the first queue binds before it joins a reuseport group, whether its
    // port is explicit or discovered. bound with SO_REUSEPORT, an explicit
    // port another reuseport socket of the same user already holds is joined,
    // not refused, and the kernel then shares that port's datagrams between
    // the two servers; an ephemeral bind may likewise be given a port another
    // reuseport group already holds (measured on Linux x64 gcc: 4 shared
    // ports in 50 x 64 reuseport ephemeral binds, none when the first socket
    // binds without it). Bound first, it takes a port no socket holds or the
    // bind fails with EADDRINUSE as the portable transport's does, and
    // SO_REUSEPORT set after the bind lets this server's other queues join it.

    const bool first_queue = queue_index == 0;

    int one = 1;
    if ( !first_queue && setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) != 0 )
    {
        rocketnet_error( "gso transport: failed to set SO_REUSEPORT: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( ipv6 )
    {
        int zero = 0;
        if ( setsockopt( fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof( zero ) ) != 0 )
        {
            rocketnet_error( "gso transport: failed to clear socket ipv6 only: %s", strerror( errno ) );
            close( fd );
            return -1;
        }
    }

    // request the buffer sizes, then read back what was granted. linux
    // returns the doubled bookkeeping value from getsockopt, so halve it
    // before comparing against the request. a small grant is not an error --
    // it is a fact about the host's sysctls, reported so nobody assumes.
    //
    // the receive request is one update's inbound packets at the full client
    // count (rocketnet_server_transport_receive_buffer_request): the worker
    // stops reading while its buffers wait for main, and SO_REUSEPORT may hash
    // any share of the clients to this one socket

    int send_buffer_size = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    int receive_buffer_size = rocketnet_server_transport_receive_buffer_request( "gso", ROCKETNET_MAX_CLIENTS );

    if ( setsockopt( fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof( int ) ) != 0 )
    {
        rocketnet_error( "gso transport: failed to set socket send buffer size: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size, sizeof( int ) ) != 0 )
    {
        rocketnet_error( "gso transport: failed to set socket receive buffer size: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    int granted_send = 0;
    int granted_receive = 0;
    socklen_t optlen = sizeof( int );
    getsockopt( fd, SOL_SOCKET, SO_SNDBUF, &granted_send, &optlen );
    optlen = sizeof( int );
    getsockopt( fd, SOL_SOCKET, SO_RCVBUF, &granted_receive, &optlen );
    granted_send /= 2;
    granted_receive /= 2;

    if ( queue_index == 0 )
    {
        if ( granted_send < send_buffer_size )
            rocketnet_warn( "gso transport: socket send buffer granted %d of %d requested bytes (raise net.core.wmem_max)",
                            granted_send, send_buffer_size );

        rocketnet_server_receive_buffer_granted( "gso", receive_buffer_size, granted_receive );
        if ( granted_send >= send_buffer_size && granted_receive >= receive_buffer_size )
            rocketnet_info( "gso transport: socket buffers granted: send %d receive %d", granted_send, granted_receive );
    }

    // bind. every queue binds the same address and port; the first queue may
    // discover an ephemeral port and the rest join it, which is why the
    // bound port comes back out.

    rocketnet_address_t addr = *bind_address;
    addr.port = *bound_port != 0 ? *bound_port : bind_address->port;

    struct sockaddr_storage sockaddr;
    socklen_t socklen = rocketnet_server_gso_address_to_sockaddr( &addr, ipv6, &sockaddr );

    if ( socklen == 0 || bind( fd, (struct sockaddr *) &sockaddr, socklen ) < 0 )
    {
        rocketnet_error( "gso transport: failed to bind socket (%s): %s", ipv6 ? "ipv6" : "ipv4", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( first_queue && setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) != 0 )
    {
        rocketnet_error( "gso transport: failed to set SO_REUSEPORT: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( *bound_port == 0 )
    {
        struct sockaddr_storage bound;
        socklen_t bound_length = sizeof( bound );
        if ( getsockname( fd, (struct sockaddr *) &bound, &bound_length ) != 0 )
        {
            rocketnet_error( "gso transport: failed to get bound socket port: %s", strerror( errno ) );
            close( fd );
            return -1;
        }

        rocketnet_address_t bound_address;
        if ( !rocketnet_server_gso_sockaddr_to_address( &bound, ipv6, &bound_address ) )
        {
            rocketnet_error( "gso transport: could not read bound socket address" );
            close( fd );
            return -1;
        }

        *bound_port = bound_address.port;
    }

    if ( fcntl( fd, F_SETFL, fcntl( fd, F_GETFL, 0 ) | O_NONBLOCK ) == -1 )
    {
        rocketnet_error( "gso transport: failed to set socket non-blocking: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( ipv6 )
    {
        int df = 1;
        setsockopt( fd, IPPROTO_IPV6, IPV6_DONTFRAG, &df, sizeof( df ) );
    }
    else
    {
        int pmtud = IP_PMTUDISC_DO;
        setsockopt( fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtud, sizeof( pmtud ) );
    }

    return fd;
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
        return segment_bytes;
    if ( header->msg_flags & MSG_CTRUNC )
    {
        if ( faults )
            *faults += 1;
        return message_bytes;
    }

    for ( struct cmsghdr * cmsg = CMSG_FIRSTHDR( header ); cmsg != NULL; cmsg = CMSG_NXTHDR( header, cmsg ) )
    {
        if ( cmsg->cmsg_len < sizeof( struct cmsghdr ) )
        {
            if ( faults )
                *faults += 1;
            return message_bytes;
        }

        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_GRO )
        {
            if ( cmsg->cmsg_len < CMSG_LEN( sizeof( int ) ) )
            {
                if ( faults )
                    *faults += 1;
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

static void rocketnet_server_gso_drain( struct rocketnet_server_gso_queue_t * queue )
{
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    const bool gro = ( queue->gso->features.load( std::memory_order_relaxed ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) != 0;

    int reads = 0;

    while ( reads < ROCKETNET_GSO_RECV_QUEUE_SIZE )
    {
        const int vlen = rocketnet_server_gso_recv_vlen( (int) buffer->num_packets, gro );
        if ( vlen == 0 )
            break;                              // no room for one more message: the socket keeps the rest

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
            count = queue->gso->test_receive( queue, vlen, queue->gso->test_receive_user );
        else
#endif
            count = recvmmsg( queue->fd, queue->messages, (unsigned int) vlen, MSG_DONTWAIT, NULL );

        // every recvmmsg counts, empty ones included -- an instrument
        // that hides the misses cannot argue the syscall win. the 1ms
        // poll wakeups are not counted; they are the worker's idle
        // heartbeat, not the receive path.

        queue->receive_syscalls.fetch_add( 1, std::memory_order_relaxed );

        reads += count > 0 ? count : 1;

        // a failed call other than an empty socket is counted; the
        // worker goes back to poll and tries again. an interrupted call
        // (EINTR) is retried without counting as an error.

        if ( count < 0 )
        {
            if ( errno == EINTR )
            {
                if ( queue->quit.load( std::memory_order_relaxed ) )
                    break;
                continue;
            }

            if ( errno != EAGAIN && errno != EWOULDBLOCK )
                buffer->receive_errors++;

            break;
        }

        if ( count == 0 )
            break;

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
                buffer->dropped_size++;
                buffer->dropped_too_large++;
                continue;
            }

            // a zero-length datagram is below the smallest packet a
            // server reads: the size pass counts it, as io_uring's does

            if ( message_bytes == 0 )
            {
                buffer->dropped_size++;
                continue;
            }

            rocketnet_address_t from;
            if ( !rocketnet_server_gso_sockaddr_to_address( &queue->names[i], queue->ipv6, &from ) )
            {
                // a source family the boundary cannot express is dropped
                // counted, never silently
                buffer->dropped_source++;
                continue;
            }

            // the GRO stride. a truncated control buffer, or a UDP_GRO
            // cmsg that does not hold an int, is one fault and the
            // message is one datagram.

            uint64_t gro_faults = 0;
            int segment_bytes = rocketnet_server_gso_gro_segment_bytes( &queue->messages[i].msg_hdr, message_bytes, &gro_faults );
            buffer->gro_cmsg_faults += gro_faults;
            if ( segment_bytes <= 0 )
                segment_bytes = message_bytes;

            if ( segment_bytes < message_bytes )
                buffer->gro_batches++;

            for ( int offset = 0; offset < message_bytes; offset += segment_bytes )
            {
                const int remaining = message_bytes - offset;
                const int this_segment = remaining < segment_bytes ? remaining : segment_bytes;

                // the same cheap pass every transport makes before staging:
                // below the smallest packet a server reads, or above the
                // receive ceiling, never reaches the protocol

                if ( this_segment < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || this_segment > ROCKETNET_RECEIVE_CEILING )
                {
                    buffer->dropped_size++;
                    if ( this_segment > ROCKETNET_RECEIVE_CEILING )
                        buffer->dropped_too_large++;
                    continue;
                }

                if ( buffer->num_packets >= ROCKETNET_GSO_RECV_QUEUE_SIZE )
                {
                    buffer->dropped_overflow++;
                    continue;
                }

                const uint32_t index = buffer->num_packets;
                buffer->from[index] = from;
                buffer->packet_bytes[index] = this_segment;
                memcpy( buffer->packet_data[index], queue->scratch[i] + offset, (size_t) this_segment );
                buffer->num_packets++;
            }
        }

        if ( count < vlen )
            break;                              // the socket is drained; go back to poll
    }
}

static void rocketnet_server_gso_worker( rocketnet_platform_thread_t *, void * data )
{
    struct rocketnet_server_gso_queue_t * queue = (struct rocketnet_server_gso_queue_t *) data;

    rocketnet_assert( queue );

    struct pollfd fds[1];
    fds[0].fd = queue->fd;
    fds[0].events = POLLIN;

    while ( !queue->quit.load( std::memory_order_relaxed ) )
    {
        // sleep until traffic, bounded so quit is honored promptly

        poll( fds, 1, 1 );

        rocketnet_server_gso_drain( queue );

        struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;

        const bool has_results = buffer->num_packets || buffer->dropped_size || buffer->dropped_overflow ||
                                 buffer->dropped_source || buffer->gro_batches || buffer->gro_cmsg_faults ||
                                 buffer->receive_errors;
        if ( !has_results )
            continue;

        rocketnet_server_gso_publish_receive( queue );
    }

    // proofs/tla/RNGsoHandoff.tla RxWorkerExit: last access to queue-owned
    // storage; destroy acquires this before join.

    queue->stopped.store( true, std::memory_order_release );
}

// ---------------------------------------------------------------------------

uint64_t rocketnet_server_gso_allocation_bytes( int queues )
{
    if ( queues < 1 || queues > ROCKETNET_GSO_MAX_QUEUES )
        return 0;
    uint64_t bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_gso_t ),
                                                     (uint64_t) queues * sizeof( rocketnet_server_gso_queue_t ) } );
    if ( !bytes || !rocketnet_memory_add( bytes, (uint64_t) queues + 1, rocketnet_platform_thread_allocation_bytes() ) ||
         !rocketnet_memory_add( bytes, 1, rocketnet_platform_wake_allocation_bytes() ) )
        return 0;
    return bytes <= SIZE_MAX ? bytes : 0;
}

static void rocketnet_server_gso_abort_create( rocketnet_server_gso_t * gso )
{
    // Create is synchronous and never exposes a partially constructed object.
    rocketnet_server_gso_stop( gso );
    while ( !rocketnet_server_gso_stopped( gso ) )
        rocketnet_platform_sleep( 0.001 );
    rocketnet_server_gso_destroy( gso );
}

struct rocketnet_server_gso_t * rocketnet_server_gso_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int flags, int * status )
{
#if ROCKETNET_DEV
    flags |= rocketnet_server_gso_test_create_flags.load();

    // development library only: ROCKETNET_TEST_GSO_PLAIN=1 forces the plain
    // tiers (plain sendmmsg, plain recvmmsg) for a whole process, so a tool
    // such as load pins the gso plain cell without calling the hook above

    char test_plain[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
    if ( rocketnet_platform_getenv( "ROCKETNET_TEST_GSO_PLAIN", test_plain, sizeof( test_plain ) ) > 0 && strcmp( test_plain, "1" ) == 0 )
        flags |= ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO;
#endif // #if ROCKETNET_DEV

    rocketnet_assert( bind_address );
    rocketnet_assert( bind_address->type == ROCKETNET_ADDRESS_IPV4 || bind_address->type == ROCKETNET_ADDRESS_IPV6 );

    if ( num_queues <= 0 )
        num_queues = ROCKETNET_GSO_DEFAULT_QUEUES;

    // refuse, never clamp (the validation law): a queue count past this
    // transport's geometry is a configuration error named here, not a silently
    // different server

    if ( num_queues > ROCKETNET_GSO_MAX_QUEUES )
    {
        rocketnet_error( "gso transport num_queues %d is out of range (1 to %d)", num_queues, ROCKETNET_GSO_MAX_QUEUES );
        return NULL;
    }

    struct rocketnet_server_gso_t * gso = (struct rocketnet_server_gso_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_gso_t ) );
    if ( !gso )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    new ( gso ) rocketnet_server_gso_t();
    gso->send_stopped.store( true, std::memory_order_relaxed );

    gso->context = context;
    gso->num_queues = num_queues;
    gso->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    gso->packet_id = 1;

    gso->queues = (struct rocketnet_server_gso_queue_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_gso_queue_t ) * (size_t) num_queues );
    if ( !gso->queues )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        gso->~rocketnet_server_gso_t();
        rocketnet_free( context, gso );
        return NULL;
    }

    for ( int i = 0; i < num_queues; i++ )
    {
        new ( &gso->queues[i] ) rocketnet_server_gso_queue_t();
        gso->queues[i].stopped.store( true, std::memory_order_relaxed );
        gso->queues[i].fd = -1;
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

        queue->queue_index = i;
        queue->ipv6 = gso->ipv6;
        queue->gso = gso;

        queue->fd = rocketnet_server_gso_open_socket( bind_address, i, &bound_port );
        if ( queue->fd < 0 )
        {
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }
    }

    bind_address->port = bound_port;
    gso->bind_address = *bind_address;
    gso->features |= ROCKETNET_SERVER_GSO_FEATURE_REUSEPORT;

    // the runtime probes, each a real setsockopt against a real socket in
    // the group -- the probe's checks as the transport's own selection
    // inputs. failure is a tier, not an error.

    if ( ( flags & ROCKETNET_SERVER_GSO_DISABLE_SEGMENT ) == 0 )
    {
        // transmit rotates across the whole group, so a probe result
        // claimed for the group is established for the group: every socket
        // must accept UDP_SEGMENT -- the same all-or-nothing reasoning as
        // the GRO enable below. unlike GRO there is no partial state to
        // roll back on refusal: the probe writes segment size zero, the
        // disabled value every socket already holds.

        int segment_sockets = 0;

        for ( int i = 0; i < num_queues; i++ )
        {
            int zero = 0;
            if ( setsockopt( gso->queues[i].fd, IPPROTO_UDP, UDP_SEGMENT, &zero, sizeof( zero ) ) == 0 )
                segment_sockets++;
        }

        if ( segment_sockets == num_queues )
        {
            gso->features |= ROCKETNET_SERVER_GSO_FEATURE_SEGMENT;
        }
    }

    if ( ( flags & ROCKETNET_SERVER_GSO_DISABLE_GRO ) == 0 )
    {
        int gro_sockets = 0;

        for ( int i = 0; i < num_queues; i++ )
        {
            int one = 1;
            if ( setsockopt( gso->queues[i].fd, IPPROTO_UDP, UDP_GRO, &one, sizeof( one ) ) == 0 )
                gro_sockets++;
        }

        if ( gro_sockets == num_queues )
        {
            gso->features |= ROCKETNET_SERVER_GSO_FEATURE_GRO;
        }
        else
        {
            // all or nothing: a partial enable would leave socket state the
            // reported tier denies, so the ones that accepted roll back

            for ( int i = 0; i < num_queues; i++ )
            {
                int zero = 0;
                setsockopt( gso->queues[i].fd, IPPROTO_UDP, UDP_GRO, &zero, sizeof( zero ) );
            }
        }
    }

    rocketnet_info( "gso transport: %d queue reuseport group on port %d, UDP_SEGMENT %s, UDP_GRO %s",
                    num_queues, bound_port,
                    ( gso->features & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) ? "yes" : "no (plain sendmmsg)",
                    ( gso->features & ROCKETNET_SERVER_GSO_FEATURE_GRO ) ? "yes" : "no (plain recvmmsg)" );

    // the workers, one per socket, started last so a partial create never
    // has a thread to chase

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_gso_queue_t * queue = &gso->queues[i];

        if ( !rocketnet_platform_mutex_create( &queue->receive_mutex ) )
        {
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }

        queue->stopped.store( false, std::memory_order_relaxed );
        queue->thread = rocketnet_platform_thread_create( context, rocketnet_server_gso_worker, queue, status );
        if ( !queue->thread )
        {
            queue->stopped.store( true, std::memory_order_relaxed );
            rocketnet_platform_mutex_destroy( &queue->receive_mutex );
            rocketnet_server_gso_abort_create( gso );
            return NULL;
        }

        rocketnet_platform_server_thread_priority( queue->thread );
    }

    if ( !rocketnet_platform_mutex_create( &gso->send_mutex ) )
    {
        rocketnet_server_gso_abort_create( gso );
        return NULL;
    }

    gso->send_wake = rocketnet_platform_wake_create( context, NULL, status );
    if ( !gso->send_wake )
    {
        rocketnet_server_gso_abort_create( gso );
        return NULL;
    }

    gso->send_stopped.store( false, std::memory_order_relaxed );
    gso->send_thread = rocketnet_platform_thread_create( context, rocketnet_server_gso_send_worker, gso, status );
    if ( !gso->send_thread )
    {
        gso->send_stopped.store( true, std::memory_order_relaxed );
        rocketnet_server_gso_abort_create( gso );
        return NULL;
    }

    rocketnet_platform_server_thread_priority( gso->send_thread );

    return gso;
}

// proofs/tla/RNGsoHandoff.tla MainStop: quit is stored for both workers; the
// final producer passes to the TX worker with the store.
void rocketnet_server_gso_stop( rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    if ( gso->stopping )
        return;
    gso->stopping = true;

    // Release the final producer after all main-thread writes. The worker
    // drains it after earlier published banks; main never waits or scans it.

    gso->send_quit.store( true, std::memory_order_release );
    if ( gso->send_wake )
        rocketnet_platform_wake_set( gso->send_wake );
    for ( int i = 0; gso->queues && i < gso->num_queues; ++i )
        gso->queues[i].quit.store( true, std::memory_order_release );
}

bool rocketnet_server_gso_stopped( rocketnet_server_gso_t * gso )
{
    if ( !gso || !gso->stopping )
        return false;
    if ( !gso->send_stopped.load( std::memory_order_acquire ) )
        return false;
    for ( int i = 0; gso->queues && i < gso->num_queues; ++i )
        if ( !gso->queues[i].stopped.load( std::memory_order_acquire ) )
            return false;
    return true;
}

// proofs/tla/RNGsoHandoff.tla Join and Free: destroy joins both workers before
// freeing any storage, and frees nothing while a worker has not exited
// (StopJoinsBeforeFree); Terminated is the freed state itself.
void rocketnet_server_gso_destroy( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    const bool stopped = rocketnet_server_gso_stopped( gso );
    rocketnet_assert( stopped );
    if ( !stopped )
        return;

    // The TX worker may use every queue socket. Join before closing any fd.

    if ( gso->send_thread )
    {
        rocketnet_platform_thread_join( gso->send_thread );
        rocketnet_platform_thread_destroy( gso->send_thread );
    }

    if ( gso->send_wake )
        rocketnet_platform_wake_destroy( gso->send_wake );
    if ( gso->send_mutex.ok )
        rocketnet_platform_mutex_destroy( &gso->send_mutex );

    if ( gso->queues )
    {
        for ( int i = 0; i < gso->num_queues; i++ )
        {
            struct rocketnet_server_gso_queue_t * queue = &gso->queues[i];

            if ( queue->thread )
            {
                rocketnet_platform_thread_join( queue->thread );
                rocketnet_platform_thread_destroy( queue->thread );
                rocketnet_platform_mutex_destroy( &queue->receive_mutex );
                queue->thread = NULL;
            }

            if ( queue->fd >= 0 )
            {
                close( queue->fd );
                queue->fd = -1;
            }
        }

        for ( int i = 0; i < gso->num_queues; i++ )
            gso->queues[i].~rocketnet_server_gso_queue_t();
        rocketnet_free( gso->context, gso->queues );
        gso->queues = NULL;
    }

    void * allocation_context = gso->context;
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

    memset( counters, 0, ROCKETNET_SERVER_GSO_NUM_COUNTERS * sizeof( uint64_t ) );

    counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] = gso->counter_packets_sent;
    counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_SYSCALLS] = gso->counter_send_syscalls;
    counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] = gso->counter_send_dropped;
    counters[ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS] = gso->counter_gso_sends;
    counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED] = gso->counter_packets_received;
    counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_SYSCALLS] = gso->counter_receive_syscalls;
    counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE] = gso->counter_receive_dropped_size;
    counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_OVERFLOW] = gso->counter_receive_dropped_overflow;
    counters[ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES] = gso->counter_gro_batches;
    counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SOURCE] = gso->counter_receive_dropped_source;
    counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_ERRORS] = gso->counter_receive_errors;
}

// the builtin transport's receive_drops hook: the size
// pass's drops (sdk/rocketnet_server_gso_linux.cpp:532-586), split at the receive
// ceiling; main thread, as the fold into these totals is (:1209-1210)

void rocketnet_server_gso_receive_drops( struct rocketnet_server_gso_t * gso, uint64_t * too_short, uint64_t * too_large )
{
    rocketnet_assert( gso );
    rocketnet_assert( too_short );
    rocketnet_assert( too_large );
    *too_large = gso->counter_receive_dropped_too_large;
    *too_short = gso->counter_receive_dropped_size - gso->counter_receive_dropped_too_large;
}

// ---------------------------------------------------------------------------
// send
// ---------------------------------------------------------------------------

// proofs/tla/ServerStopSpread.tla Room: the producer bank's free slots

int rocketnet_server_gso_send_room( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    if ( gso->stopping || gso->send_producer < 0 )
        return 0;
    const int used = gso->send_banks[gso->send_producer].num_packets.load( std::memory_order_relaxed );
    return used < 0 || used >= ROCKETNET_GSO_SEND_QUEUE_SIZE ? 0 : ROCKETNET_GSO_SEND_QUEUE_SIZE - used;
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: every bank back in the free
// queue and the producer empty

bool rocketnet_server_gso_send_idle( struct rocketnet_server_gso_t * gso )
{
    rocketnet_assert( gso );

    if ( gso->send_producer < 0 )
        return true;
    if ( gso->send_banks[gso->send_producer].num_packets.load( std::memory_order_relaxed ) != 0 )
        return false;
    if ( !rocketnet_platform_mutex_try_acquire( &gso->send_mutex ) )
        return false;
    const bool idle = gso->send_ready_count == 0 && gso->send_free_count == ROCKETNET_GSO_TX_BANKS - 1;
    rocketnet_platform_mutex_release( &gso->send_mutex );

    return idle;
}

// proofs/tla/RNGsoHandoff.tla TxStage: a fill thread stages one packet into the
// producer bank, outside the ownership mutex.
uint8_t * rocketnet_server_gso_start_packet( struct rocketnet_server_gso_t * gso, const struct rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( gso );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    if ( gso->stopping )
        return NULL;

    *packet_id = gso->packet_id.fetch_add( 1, std::memory_order_relaxed );

    rocketnet_server_gso_send_buffer_t * bank = &gso->send_banks[gso->send_producer];
    int slot = bank->num_packets.load( std::memory_order_relaxed );
    while ( slot >= 0 && slot < ROCKETNET_GSO_SEND_QUEUE_SIZE &&
            !bank->num_packets.compare_exchange_weak( slot, slot + 1, std::memory_order_relaxed ) )
    {
    }

    // the same lower bound the xdp transport learned to check: an app that
    // stages forever without flushing wraps the counter negative, and a
    // negative index is an out-of-bounds write, not a dropped packet

    if ( slot >= ROCKETNET_GSO_SEND_QUEUE_SIZE || slot < 0 )
    {
        rocketnet_spam( "gso transport send staging full. packet dropped" );
        return NULL;
    }

    gso->send_banks[gso->send_producer].info[slot].to = *to;
    gso->send_banks[gso->send_producer].info[slot].packet_bytes = 0;

    return gso->send_banks[gso->send_producer].data[slot];
}

void rocketnet_server_gso_finish_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( gso );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    (void) packet_id;
    if ( gso->stopping || gso->send_producer < 0 )
        return;

    // the slot comes from pointer arithmetic against the staging, the xdp
    // pattern: a pointer that is not a live slot root lands out of range and
    // fails closed as a lost packet, never an out-of-bounds write

    const uintptr_t first = (uintptr_t) gso->send_banks[gso->send_producer].data[0];
    const uintptr_t address = (uintptr_t) packet_data;
    if ( address < first || address - first >= sizeof( gso->send_banks[gso->send_producer].data ) )
        return;
    const uintptr_t offset = address - first;

    const int slot = (int) ( offset / ROCKETNET_FRAME_STRIDE );

    if ( slot < 0 || slot >= ROCKETNET_GSO_SEND_QUEUE_SIZE || gso->send_banks[gso->send_producer].data[slot] != packet_data )
        return;

    gso->send_banks[gso->send_producer].info[slot].packet_bytes = packet_bytes;
}

void rocketnet_server_gso_abort_packet( struct rocketnet_server_gso_t * gso, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( gso );

    (void) packet_id;

    if ( !packet_data || gso->stopping || gso->send_producer < 0 )
        return;

    const uintptr_t first = (uintptr_t) gso->send_banks[gso->send_producer].data[0];
    const uintptr_t address = (uintptr_t) packet_data;
    if ( address < first || address - first >= sizeof( gso->send_banks[gso->send_producer].data ) )
        return;
    const uintptr_t offset = address - first;

    const int slot = (int) ( offset / ROCKETNET_FRAME_STRIDE );

    if ( slot < 0 || slot >= ROCKETNET_GSO_SEND_QUEUE_SIZE || gso->send_banks[gso->send_producer].data[slot] != packet_data )
        return;

    gso->send_banks[gso->send_producer].info[slot].packet_bytes = 0;
}

// one flush pass: walk the staged slots once, build sendmmsg batches,
// coalescing adjacent same-destination runs into GSO super-datagrams where
// the feature is on. returns the first unconsumed slot, so a live GSO refusal
// can flip the feature off and resume plain from exactly where it stopped,
// and a full socket (*full) can resume from its first unsent message later.
// A slot this pass dropped is cleared, so a resumed pass skips it uncounted.

static int rocketnet_server_gso_flush( struct rocketnet_server_gso_t * gso, rocketnet_server_gso_send_buffer_t * bank, int first_slot, int num_slots, bool * disable_gso, bool * full )
{
    const bool use_gso = ( gso->features & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0;

    int slot = first_slot;

    while ( slot < num_slots )
    {
        // build one sendmmsg batch

        int num_messages = 0;
        int message_first_slot[ROCKETNET_GSO_SEND_BATCH];
        int message_num_packets[ROCKETNET_GSO_SEND_BATCH];
        bool message_is_gso[ROCKETNET_GSO_SEND_BATCH];
        int num_iovecs = 0;
        int batch_end = slot;

        while ( batch_end < num_slots && num_messages < ROCKETNET_GSO_SEND_BATCH )
        {
            // close the batch while a full run still fits the iovec array:
            // the run builder below may take up to MAX_SEGMENTS entries

            if ( num_iovecs + ROCKETNET_GSO_MAX_SEGMENTS > ROCKETNET_GSO_SEND_BATCH * ROCKETNET_GSO_MAX_SEGMENTS )
                break;

            struct rocketnet_server_gso_send_info_t * info = &bank->info[batch_end];

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

            if ( use_gso )
            {
                // extend the run: equal sizes to the same destination, plus
                // at most one shorter tail (UDP_SEGMENT semantics: every
                // segment is the stride except possibly the last). a LARGER
                // packet, a different destination, or an aborted slot ends
                // the run.

                while ( batch_end < num_slots &&
                        run_count < ROCKETNET_GSO_MAX_SEGMENTS &&
                        num_iovecs + run_count < ROCKETNET_GSO_SEND_BATCH * ROCKETNET_GSO_MAX_SEGMENTS )
                {
                    struct rocketnet_server_gso_send_info_t * next = &bank->info[batch_end];

                    if ( next->packet_bytes <= 0 )
                        break;
                    if ( next->packet_bytes > segment_bytes )
                        break;
                    if ( run_bytes + next->packet_bytes > ROCKETNET_GSO_MAX_SUPER_BYTES )
                        break;
                    if ( !rocketnet_address_equal( &next->to, &info->to ) )
                        break;

                    run_count++;
                    run_bytes += next->packet_bytes;
                    batch_end++;

                    if ( next->packet_bytes < segment_bytes )
                        break;                      // the shorter tail closes the run
                }
            }

            // one message for the run

            struct mmsghdr * message = &gso->send_headers[num_messages];
            struct msghdr * hdr = &message->msg_hdr;
            memset( hdr, 0, sizeof( struct msghdr ) );

            socklen_t namelen = rocketnet_server_gso_address_to_sockaddr( &info->to, gso->ipv6, &gso->send_names[num_messages] );
            if ( namelen == 0 )
            {
                // unroutable on this socket family; drop the run counted,
                // cleared so a resumed pass does not count it again
                gso->counter_send_dropped += (uint64_t) run_count;
                for ( int i = 0; i < run_count; i++ )
                    bank->info[run_first + i].packet_bytes = 0;
                continue;
            }

            hdr->msg_name = &gso->send_names[num_messages];
            hdr->msg_namelen = namelen;

            hdr->msg_iov = &gso->send_iovecs[num_iovecs];
            hdr->msg_iovlen = (size_t) run_count;

            // the run is contiguous slots [run_first, run_first + run_count)
            // with every packet_bytes positive, by construction of the run
            // builder above -- so the fill is bounded by run_count and never
            // walks the arrays. a concurrent mutation of a slot's bytes here
            // would be a fill-contract violation; the loop stays inside the
            // run regardless, fail closed, never out of bounds.

            for ( int i = 0; i < run_count; i++ )
            {
                const int iovec_slot = run_first + i;
                gso->send_iovecs[num_iovecs + i].iov_base = bank->data[iovec_slot] + ROCKETNET_FRAME_WIRE_OFFSET;
                gso->send_iovecs[num_iovecs + i].iov_len = (size_t) bank->info[iovec_slot].packet_bytes;
            }

            num_iovecs += run_count;

            if ( run_count > 1 )
            {
                memset( gso->send_controls[num_messages], 0, sizeof( gso->send_controls[num_messages] ) );
                hdr->msg_control = gso->send_controls[num_messages];
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

        // transmit rotates across the group's sockets, one per batch: every
        // socket shares the same bound address and port, so replies carry
        // the same 4-tuple whichever socket sent them, and the group's send
        // buffer headroom adds up instead of funneling through one socket.
        // The dedicated TX worker owns this rotation and all batch scratch.

        const int fd = gso->queues[gso->send_rotation++ % (uint64_t) gso->num_queues].fd;

        int sent = 0;
        while ( sent < num_messages )
        {
            int result;
#if ROCKETNET_DEV
            int pending = gso->test_fail_sends.load( std::memory_order_acquire );
            if ( pending > 0 && gso->test_fail_sends.compare_exchange_strong( pending, pending - 1 ) )
            {
                result = -1;
                errno = gso->test_fail_send_error.load( std::memory_order_relaxed );
            }
            else if ( gso->test_send )
                result = gso->test_send( &gso->send_headers[sent], (unsigned int) ( num_messages - sent ), gso->test_send_user );
            else
#endif
                result = sendmmsg( fd, &gso->send_headers[sent], (unsigned int) ( num_messages - sent ), 0 );

            if ( result > 0 )
            {
                gso->counter_send_syscalls++;

                for ( int i = sent; i < sent + result; i++ )
                {
                    gso->counter_packets_sent += (uint64_t) message_num_packets[i];
                    if ( message_is_gso[i] )
                        gso->counter_gso_sends++;
                }

                sent += result;
                continue;
            }

            gso->counter_send_syscalls++;

            if ( message_is_gso[sent] && ( errno == EINVAL || errno == EIO || errno == EOPNOTSUPP ) )
            {
                // the kernel accepted the UDP_SEGMENT probe but refused a
                // live GSO send. flip the tier and resend everything from
                // this message's first slot as plain packets -- degradation
                // is lossless, not a dropped batch.

                rocketnet_warn( "gso transport: live GSO send refused (%s). degrading to plain sendmmsg", strerror( errno ) );
                *disable_gso = true;
                return message_first_slot[sent];
            }

            // a full socket (or a signal): the socket cannot take more now.
            // nothing is dropped: the flush stops at the first unsent
            // message and the bank keeps the rest, in order, for the next
            // attempt (proofs/tla/RNGsoHandoff.tla TxSend, SentOrHeld)

            if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == ENOMEM || errno == EINTR )
            {
                rocketnet_spam( "gso transport sendmmsg refused (errno %d). %d messages wait for the next attempt", errno, num_messages - sent );

                *full = true;
                return message_first_slot[sent];
            }

            // nothing went in this call, so the first unsent message is the
            // one the kernel refused (an unroutable destination, say): drop
            // it and send the rest in order, as the portable batch and the
            // client gso do

            gso->counter_send_dropped += (uint64_t) message_num_packets[sent];

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
    rocketnet_server_gso_publish_send( gso );

    // a bank the socket refused waits for this update: wake the worker to
    // send the rest

    if ( gso->send_full.load( std::memory_order_acquire ) && gso->send_wake )
        rocketnet_platform_wake_set( gso->send_wake );
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
        return;
    if ( queue->main_cursor >= queue->main_buffer->num_packets )
    {
        queue->main_buffer->num_packets = 0;
        queue->main_cursor = 0;
        if ( !rocketnet_platform_mutex_try_acquire( &queue->receive_mutex ) )
            return;
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
        gso->counter_receive_dropped_size += buffer->dropped_size;
        gso->counter_receive_dropped_too_large += buffer->dropped_too_large;
        gso->counter_receive_dropped_overflow += buffer->dropped_overflow;
        gso->counter_receive_dropped_source += buffer->dropped_source;
        gso->counter_receive_errors += buffer->receive_errors;
        gso->counter_gro_batches += buffer->gro_batches;
        gso->counter_gro_cmsg_faults += buffer->gro_cmsg_faults;
        buffer->receive_errors = 0;
        buffer->dropped_size = 0;
        buffer->dropped_too_large = 0;
        buffer->dropped_overflow = 0;
        buffer->dropped_source = 0;
        buffer->gro_batches = 0;
        buffer->gro_cmsg_faults = 0;
    }

    rocketnet_server_gso_recv_buffer_t * buffer = queue->main_buffer;
    while ( queue->main_cursor < buffer->num_packets && budget-- > 0 &&
            gso->process_packets.num_packets < ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE )
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

    rocketnet_server_gso_publish_send( gso ); // retry deferred TX even on idle updates
    gso->process_packets.num_packets = 0;
    for ( int queue_index = 0; queue_index < gso->num_queues; ++queue_index )
    {
        rocketnet_server_gso_queue_t * queue = &gso->queues[queue_index];
        gso->counter_receive_syscalls += queue->receive_syscalls.exchange( 0, std::memory_order_relaxed );
        rocketnet_server_gso_receive_from_queue( gso, queue,
                                                 ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE - gso->process_packets.num_packets );
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
        return false;
    rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );

    return true;
}

// one queue, no socket, no thread: the fixture every stepped test runs on
static struct rocketnet_server_gso_t * rocketnet_server_gso_test_create()
{
    void * storage = rocketnet_malloc( NULL, sizeof( struct rocketnet_server_gso_t ) );
    if ( !storage )
        return NULL;
    struct rocketnet_server_gso_t * gso = new ( storage ) rocketnet_server_gso_t();
    gso->context = NULL;
    gso->num_queues = 1;
    gso->queues = (struct rocketnet_server_gso_queue_t *) rocketnet_malloc( NULL, sizeof( struct rocketnet_server_gso_queue_t ) );
    if ( !gso->queues )
    {
        gso->~rocketnet_server_gso_t();
        rocketnet_free( NULL, storage );
        return NULL;
    }

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    new ( queue ) rocketnet_server_gso_queue_t();
    queue->queue_index = 0;
    queue->fd = -1;
    queue->gso = gso;
    queue->worker_buffer = &queue->buffers[0];
    queue->main_buffer = &queue->buffers[1];
    queue->free_buffer = &queue->buffers[2];

    bool valid = rocketnet_platform_mutex_create( &queue->receive_mutex ) &&
                 rocketnet_platform_mutex_create( &gso->send_mutex );
    if ( !valid )
    {
        if ( queue->receive_mutex.ok )
            rocketnet_platform_mutex_destroy( &queue->receive_mutex );
        if ( gso->send_mutex.ok )
            rocketnet_platform_mutex_destroy( &gso->send_mutex );
        queue->~rocketnet_server_gso_queue_t();
        rocketnet_free( NULL, gso->queues );
        gso->~rocketnet_server_gso_t();
        rocketnet_free( NULL, storage );
        return NULL;
    }

    return gso;
}

// the fixture started no threads, so destroy has no handle to join; it
// releases the receive mutex itself, then destroy frees the storage
static void rocketnet_server_gso_test_destroy( struct rocketnet_server_gso_t * gso )
{
    rocketnet_platform_mutex_destroy( &gso->queues[0].receive_mutex );
    rocketnet_server_gso_destroy( gso );
}

bool rocketnet_server_gso_test_worker_handoff()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
        return false;

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
    valid = valid && gso->send_producer == 2 && gso->send_ready_count == 2 && gso->send_free_count == 0;
    valid = valid && rocketnet_server_gso_test_stage( gso, 40000 );
    rocketnet_server_gso_send_packets( gso );
    valid = valid && gso->send_producer == 2 && gso->send_ready_count == 2;

    // The worker takes the published banks oldest first and returns each
    // only after its (here already finished) sends.

    bool final = false;
    int taken = rocketnet_server_gso_take_send( gso, &final );
    valid = valid && taken == 0 && !final;
    rocketnet_server_gso_return_send( gso, taken );
    taken = rocketnet_server_gso_take_send( gso, &final );
    valid = valid && taken == 1 && !final;
    rocketnet_server_gso_return_send( gso, taken );

    // Stop hands the worker the final producer after the ready FIFO; until
    // both workers have exited the transport does not report stopped, so
    // destroy would free nothing.

    rocketnet_server_gso_stop( gso );
    valid = valid && !rocketnet_server_gso_stopped( gso );
    taken = rocketnet_server_gso_take_send( gso, &final );
    valid = valid && taken == 2 && final && gso->send_producer < 0;

    queue->stopped.store( true, std::memory_order_release );
    gso->send_stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

// A refused sendmmsg, stepped on this thread through the send-failure seam
// with no socket and no thread: a full socket drops the rest of the batch,
// any other errno drops the one refused message and sends the rest in
// order, as the portable batch and the client gso do.
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
            bytes = 0;
        else if ( source->kind == ROCKETNET_SERVER_GSO_TEST_SHORT )
            bytes = 1;
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
        return false;

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    gso->test_receive = rocketnet_server_gso_test_source_receive;

    bool valid = true;

    const rocketnet_server_gso_test_kind_t discards[] = { ROCKETNET_SERVER_GSO_TEST_EMPTY, ROCKETNET_SERVER_GSO_TEST_REFUSED,
                                                          ROCKETNET_SERVER_GSO_TEST_SHORT };

    for ( rocketnet_server_gso_test_kind_t kind : discards )
    {
        buffer->num_packets = 0;
        buffer->dropped_size = 0;
        buffer->dropped_source = 0;

        struct rocketnet_server_gso_test_source_t source = { kind, 1000000, 0, 0 };
        gso->test_receive_user = &source;
        rocketnet_server_gso_drain( queue );

        const uint64_t dropped = kind == ROCKETNET_SERVER_GSO_TEST_REFUSED ? buffer->dropped_source : buffer->dropped_size;
        valid = valid && source.delivered == ROCKETNET_GSO_RECV_QUEUE_SIZE && buffer->num_packets == 0 &&
                dropped == (uint64_t) ROCKETNET_GSO_RECV_QUEUE_SIZE;
    }

    buffer->num_packets = 0;
    struct rocketnet_server_gso_test_source_t source = { ROCKETNET_SERVER_GSO_TEST_VALID, 3, 0, 0 };
    gso->test_receive_user = &source;
    rocketnet_server_gso_drain( queue );
    valid = valid && source.delivered == 3 && buffer->num_packets == 3 &&
            buffer->packet_bytes[2] == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES && buffer->packet_data[2][0] == (uint8_t) 'v';

    // no worker ran, so stop is complete once both stopped flags are set

    gso->test_receive = NULL;
    gso->test_receive_user = NULL;
    rocketnet_server_gso_stop( gso );
    queue->stopped.store( true, std::memory_order_release );
    gso->send_stopped.store( true, std::memory_order_release );
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
        return false;

    struct rocketnet_server_gso_queue_t * queue = &gso->queues[0];
    struct rocketnet_server_gso_recv_buffer_t * buffer = queue->worker_buffer;
    gso->test_receive = rocketnet_server_gso_test_source_receive;

    bool valid = true;

    // GRO on, one slot free: not one message of 64 segments fits

    gso->features.fetch_or( ROCKETNET_SERVER_GSO_FEATURE_GRO, std::memory_order_relaxed );
    buffer->num_packets = ROCKETNET_GSO_RECV_QUEUE_SIZE - 1;
    buffer->dropped_overflow = 0;
    struct rocketnet_server_gso_test_source_t gro = { ROCKETNET_SERVER_GSO_TEST_GRO, ROCKETNET_GSO_RECV_BATCH, 0, 0 };
    gso->test_receive_user = &gro;
    rocketnet_server_gso_drain( queue );
    valid = valid && gro.delivered == 0 && gro.remaining == ROCKETNET_GSO_RECV_BATCH && buffer->dropped_overflow == 0 &&
            buffer->num_packets == ROCKETNET_GSO_RECV_QUEUE_SIZE - 1;

    // room for two runs and part of a third: two are read, every segment
    // staged, and the drain stops with the rest unread

    buffer->num_packets = ROCKETNET_GSO_RECV_QUEUE_SIZE - ( 2 * ROCKETNET_GSO_RECV_MAX_SEGMENTS + 5 );
    rocketnet_server_gso_drain( queue );
    valid = valid && gro.delivered == 2 && gro.largest_vlen == 2 && buffer->dropped_overflow == 0 &&
            buffer->num_packets == ROCKETNET_GSO_RECV_QUEUE_SIZE - 5;

    // an empty buffer asks for a whole batch

    buffer->num_packets = 0;
    gro.largest_vlen = 0;
    rocketnet_server_gso_drain( queue );
    valid = valid && gro.largest_vlen == ROCKETNET_GSO_RECV_BATCH && buffer->dropped_overflow == 0 &&
            buffer->num_packets == (uint32_t) ( ( ROCKETNET_GSO_RECV_BATCH - 2 ) * ROCKETNET_GSO_RECV_MAX_SEGMENTS );

    // GRO off, three slots free: three datagrams are read, the rest wait

    gso->features.fetch_and( ~ROCKETNET_SERVER_GSO_FEATURE_GRO, std::memory_order_relaxed );
    buffer->num_packets = ROCKETNET_GSO_RECV_QUEUE_SIZE - 3;
    struct rocketnet_server_gso_test_source_t plain = { ROCKETNET_SERVER_GSO_TEST_VALID, ROCKETNET_GSO_RECV_BATCH, 0, 0 };
    gso->test_receive_user = &plain;
    rocketnet_server_gso_drain( queue );
    valid = valid && plain.delivered == 3 && plain.largest_vlen == 3 && buffer->dropped_overflow == 0 &&
            buffer->num_packets == ROCKETNET_GSO_RECV_QUEUE_SIZE;

    // no worker ran, so stop is complete once both stopped flags are set

    gso->test_receive = NULL;
    gso->test_receive_user = NULL;
    rocketnet_server_gso_stop( gso );
    queue->stopped.store( true, std::memory_order_release );
    gso->send_stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );
    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

bool rocketnet_server_gso_test_send_refusals()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
        return false;

    bool valid = true;

    // three messages to three destinations. an unroutable destination
    // refuses its own message only: three attempts, one packet dropped each

    for ( int i = 0; i < 3; i++ )
        valid = valid && rocketnet_server_gso_test_stage( gso, (uint16_t) ( 40000 + i ) );
    rocketnet_server_gso_send_packets( gso );

    bool final = false;
    int taken = rocketnet_server_gso_take_send( gso, &final );
    valid = valid && taken >= 0 && !final;

    rocketnet_server_gso_test_fail_sends( gso, 3, EHOSTUNREACH );
    bool disable_gso = false;
    bool full = false;
    int next = rocketnet_server_gso_flush( gso, &gso->send_banks[taken], 0, 3, &disable_gso, &full );
    valid = valid && next == 3 && !disable_gso && !full;
    valid = valid && gso->counter_send_syscalls == 3 && gso->counter_send_dropped == 3 && gso->counter_packets_sent == 0;
    valid = valid && gso->test_fail_sends.load( std::memory_order_relaxed ) == 0;
    rocketnet_server_gso_return_send( gso, taken );

    rocketnet_server_gso_stop( gso );
    rocketnet_server_gso_take_send( gso, &final );
    valid = valid && final;

    gso->queues[0].stopped.store( true, std::memory_order_release );
    gso->send_stopped.store( true, std::memory_order_release );
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
            socket->ports[socket->num_ports++] = rocketnet_platform_ntohs( name->sin_port );
    }
    socket->accept_budget -= taken;
    return taken;
}

bool rocketnet_server_gso_test_send_full_keeps_rest()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_create();
    if ( !gso )
        return false;

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
            rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }
    rocketnet_server_gso_send_packets( gso );

    bool final = false;
    const int taken = rocketnet_server_gso_take_send( gso, &final );
    valid = valid && taken >= 0 && !final;
    if ( !valid )
    {
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
    rocketnet_server_gso_send_buffer_t * bank = &gso->send_banks[taken];
    const int staged = bank->num_packets.load( std::memory_order_relaxed );
    valid = valid && staged == count;
    valid = valid && !rocketnet_server_gso_send_bank( gso, bank, &slot, staged );
    valid = valid && slot == 50 && socket.calls == 2;
    valid = valid && gso->counter_send_syscalls == 2 && gso->counter_packets_sent == 50 && gso->counter_send_dropped == 1;

    // the next attempt sends the rest, in order, and counts the dropped run
    // once

    socket.accept_budget = 1000;
    valid = valid && rocketnet_server_gso_send_bank( gso, bank, &slot, staged );
    valid = valid && slot == count;
    valid = valid && gso->counter_packets_sent == (uint64_t) ( count - 1 ) && gso->counter_send_dropped == 1;
    valid = valid && socket.num_ports == count - 1;
    int expected = 0;
    for ( int i = 0; i < socket.num_ports; i++, expected++ )
    {
        if ( expected == unroutable )
            expected++;
        valid = valid && socket.ports[i] == (uint16_t) ( 40000 + expected );
    }

    gso->test_send = NULL;
    gso->test_send_user = NULL;
    rocketnet_server_gso_return_send( gso, taken );

    rocketnet_server_gso_stop( gso );
    rocketnet_server_gso_take_send( gso, &final );
    valid = valid && final;

    gso->queues[0].stopped.store( true, std::memory_order_release );
    gso->send_stopped.store( true, std::memory_order_release );
    valid = valid && rocketnet_server_gso_stopped( gso );

    rocketnet_server_gso_test_destroy( gso );

    return valid;
}

#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

int rocketnet_server_gso_linux_cpp_dummy = 0;
