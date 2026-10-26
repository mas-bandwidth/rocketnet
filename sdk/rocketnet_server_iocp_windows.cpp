/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "rocketnet_server_iocp.h"
#include "rocketnet_platform.h"
#include "rocketnet_coalesce.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <atomic>
#include <new>
#include <stddef.h>
#include <string.h>

struct rocketnet_iocp_ref_t
{
    uint64_t owner_epoch;
    uint64_t generation;
    uint32_t index;
};

enum rocketnet_iocp_operation_state_t
{
    ROCKETNET_IOCP_OPERATION_IDLE,
    ROCKETNET_IOCP_OPERATION_FREE,
    ROCKETNET_IOCP_OPERATION_STAGED,
    ROCKETNET_IOCP_OPERATION_SEND_POSTED,
    ROCKETNET_IOCP_OPERATION_RECEIVE_POSTED,
    ROCKETNET_IOCP_OPERATION_RETRY,
    ROCKETNET_IOCP_OPERATION_RETIRED
};

// Every cross-thread work list is a bounded MPMC ring of full operation
// references. A cell's sequence publishes its ref with release/acquire; the
// monotonically claimed enqueue/dequeue positions prevent ABA at a physical
// cell. free_sends has caller consumers and TX/completion producers;
// retry_receives has completion-worker producers and consumers. A successful
// operation touches one cell, but contention may cause CAS retries: it is
// lock-free bounded storage, not a hard constant-time claim.
template <size_t Capacity>
struct rocketnet_iocp_ref_queue_t
{
    static_assert( Capacity > 0 && ( Capacity & ( Capacity - 1 ) ) == 0, "an IOCP reference queue has power-of-two capacity" );
    struct cell_t
    {
        std::atomic<uint64_t> sequence{0};
        rocketnet_iocp_ref_t ref = {};
    };
    cell_t cells[Capacity];
    std::atomic<uint64_t> enqueue{0};
    std::atomic<uint64_t> dequeue{0};
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
    std::atomic<uint64_t> pushes{0};
    std::atomic<uint64_t> pops{0};
    std::atomic<uint64_t> probes{0};
#endif
};

template <size_t Capacity>
static void rocketnet_iocp_queue_initialize( rocketnet_iocp_ref_queue_t<Capacity> * queue )
{
    queue->enqueue.store( 0, std::memory_order_relaxed );
    queue->dequeue.store( 0, std::memory_order_relaxed );
    for ( uint64_t i = 0; i < Capacity; ++i )
        queue->cells[i].sequence.store( i, std::memory_order_relaxed );
}

template <size_t Capacity>
static bool rocketnet_iocp_queue_push( rocketnet_iocp_ref_queue_t<Capacity> * queue, const rocketnet_iocp_ref_t & ref )
{
    uint64_t position = queue->enqueue.load( std::memory_order_relaxed );
    typename rocketnet_iocp_ref_queue_t<Capacity>::cell_t * cell;
    for ( ;; )
    {
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
        queue->probes.fetch_add( 1, std::memory_order_relaxed );
#endif
        cell = &queue->cells[position & ( Capacity - 1 )];
        const uint64_t sequence = cell->sequence.load( std::memory_order_acquire );
        const int64_t difference = (int64_t) ( sequence - position );
        if ( difference == 0 )
        {
            if ( queue->enqueue.compare_exchange_weak( position, position + 1, std::memory_order_relaxed ) ) break;
        }
        else if ( difference < 0 )
            return false;
        else
            position = queue->enqueue.load( std::memory_order_relaxed );
    }
    cell->ref = ref;
    cell->sequence.store( position + 1, std::memory_order_release );
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
    queue->pushes.fetch_add( 1, std::memory_order_relaxed );
#endif
    return true;
}

template <size_t Capacity>
static bool rocketnet_iocp_queue_pop( rocketnet_iocp_ref_queue_t<Capacity> * queue, rocketnet_iocp_ref_t * ref )
{
    uint64_t position = queue->dequeue.load( std::memory_order_relaxed );
    typename rocketnet_iocp_ref_queue_t<Capacity>::cell_t * cell;
    for ( ;; )
    {
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
        queue->probes.fetch_add( 1, std::memory_order_relaxed );
#endif
        cell = &queue->cells[position & ( Capacity - 1 )];
        const uint64_t sequence = cell->sequence.load( std::memory_order_acquire );
        const int64_t difference = (int64_t) ( sequence - ( position + 1 ) );
        if ( difference == 0 )
        {
            if ( queue->dequeue.compare_exchange_weak( position, position + 1, std::memory_order_relaxed ) ) break;
        }
        else if ( difference < 0 )
            return false;
        else
            position = queue->dequeue.load( std::memory_order_relaxed );
    }
    *ref = cell->ref;
    cell->sequence.store( position + Capacity, std::memory_order_release );
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
    queue->pops.fetch_add( 1, std::memory_order_relaxed );
#endif
    return true;
}

// Pop only an enqueue position that existed before stop was captured. A
// failed operation may enqueue a new reference concurrently, but its position
// is at or beyond stop and therefore cannot be consumed again in this pass.
template <size_t Capacity>
static bool rocketnet_iocp_queue_pop_before( rocketnet_iocp_ref_queue_t<Capacity> * queue,
                                              uint64_t stop, rocketnet_iocp_ref_t * ref )
{
    uint64_t position = queue->dequeue.load( std::memory_order_relaxed );
    typename rocketnet_iocp_ref_queue_t<Capacity>::cell_t * cell;
    for ( ;; )
    {
        if ( (int64_t) ( stop - position ) <= 0 ) return false;
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
        queue->probes.fetch_add( 1, std::memory_order_relaxed );
#endif
        cell = &queue->cells[position & ( Capacity - 1 )];
        const uint64_t sequence = cell->sequence.load( std::memory_order_acquire );
        const int64_t difference = (int64_t) ( sequence - ( position + 1 ) );
        if ( difference == 0 )
        {
            if ( queue->dequeue.compare_exchange_weak( position, position + 1, std::memory_order_relaxed ) ) break;
        }
        else if ( difference < 0 )
            return false;
        else
            position = queue->dequeue.load( std::memory_order_relaxed );
    }
    *ref = cell->ref;
    cell->sequence.store( position + Capacity, std::memory_order_release );
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
    queue->pops.fetch_add( 1, std::memory_order_relaxed );
#endif
    return true;
}

template <size_t Capacity>
static size_t rocketnet_iocp_queue_count( rocketnet_iocp_ref_queue_t<Capacity> * queue )
{
    const uint64_t enqueued = queue->enqueue.load( std::memory_order_acquire );
    const uint64_t dequeued = queue->dequeue.load( std::memory_order_acquire );
    const uint64_t count = enqueued - dequeued;
    return (size_t) ( count < Capacity ? count : Capacity );
}

// The 16-aligned members lead; an operation is a fixed record identified by
// index, owner epoch and generation rather than by an intrusive list node.
struct rocketnet_iocp_operation_t
{
    OVERLAPPED overlapped;
    alignas(16) char control[64];
    alignas(16) uint8_t frame[ROCKETNET_FRAME_STRIDE];
    WSABUF buffer;
    sockaddr_storage address;
    int address_bytes;
    DWORD flags;
    int index;
    bool send;
    int packet_bytes;
    uint64_t owner_epoch;
    uint64_t generation;
    rocketnet_iocp_ref_t completion_ref;
    int state;
    // A send post carries run_count frames and run_bytes bytes: one frame on
    // the per-datagram path, a USO run otherwise. A USO root names a flat
    // generation-tagged run sequence by ticket. Every member stays owned
    // until that one completion retires the exact recorded references.
    rocketnet_address_t to;
    int run_count;
    int run_bytes;
    uint64_t run_ticket;
    // WSASendMsg for a USO run, WSARecvMsg for a URO receive. uro_data is the
    // receive's coalesced buffer, NULL on the per-datagram receive path.
    WSAMSG message;
    uint8_t * uro_data;
};

static_assert( offsetof( rocketnet_iocp_operation_t, overlapped ) == 0, "completion identifies its operation" );
static_assert( alignof( rocketnet_iocp_operation_t ) % MEMORY_ALLOCATION_ALIGNMENT == 0 && alignof( rocketnet_iocp_operation_t ) >= 16, "an operation keeps its allocation alignment" );
static_assert( offsetof( rocketnet_iocp_operation_t, control ) % 16 == 0, "control keeps native ancillary-data alignment" );
static_assert( offsetof( rocketnet_iocp_operation_t, frame ) == offsetof( rocketnet_iocp_operation_t, control ) + 64, "frame follows control with no implicit padding" );
static_assert( ROCKETNET_RECEIVE_CEILING + 1 <= ROCKETNET_FRAME_STRIDE, "receive detects oversize datagrams" );

// the bytes of a receive buffer's members after data, which pack with no
// padding between them
#define ROCKETNET_IOCP_RECEIVE_BUFFER_TAIL_BYTES ( sizeof( int ) + ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE * ( sizeof( rocketnet_address_t ) + sizeof( int ) ) )

static_assert( ROCKETNET_IOCP_RECEIVE_BUFFER_TAIL_BYTES % 16 != 0, "a receive buffer's tail padding is a nonzero array" );

// data leads, so its 16-alignment needs no padding before it, and the rest of
// the last 16 bytes is written out, so the compiler adds no tail padding
// (MSVC C4324)
struct rocketnet_iocp_receive_buffer_t
{
    alignas(16) uint8_t data[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];
    int num_packets;
    rocketnet_address_t from[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE];
    int packet_bytes[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE];
    uint8_t padding[16 - ROCKETNET_IOCP_RECEIVE_BUFFER_TAIL_BYTES % 16];
};

// the bytes of a worker's members, which pack with no padding between them
#define ROCKETNET_IOCP_RECEIVE_BANKS 4
#define ROCKETNET_IOCP_WORKER_MEMBER_BYTES ( ROCKETNET_IOCP_RECEIVE_BANKS * sizeof( rocketnet_iocp_receive_buffer_t ) + sizeof( rocketnet_server_iocp_t * ) + \
                                             sizeof( rocketnet_platform_thread_t * ) + sizeof( rocketnet_platform_mutex_t ) + 17 * sizeof( int ) )

static_assert( ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64 != 0, "a worker's tail padding is a nonzero array" );

struct alignas(64) rocketnet_iocp_worker_t
{
    rocketnet_iocp_receive_buffer_t buffers[ROCKETNET_IOCP_RECEIVE_BANKS];
    rocketnet_server_iocp_t * owner;
    rocketnet_platform_thread_t * thread;
    // Only ownership indices are exchanged under this mutex. Completion
    // handling, packet copies, send retirement and reposts run outside it.
    rocketnet_platform_mutex_t mutex;
    int free_banks[ROCKETNET_IOCP_RECEIVE_BANKS];
    int ready_banks[ROCKETNET_IOCP_RECEIVE_BANKS];
    int exposed_banks[ROCKETNET_IOCP_RECEIVE_BANKS];
    int free_count = ROCKETNET_IOCP_RECEIVE_BANKS - 1;
    int ready_head = 0;
    int ready_count = 0;
    int fill_index = 0;
    int exposed_count = 0;
    // the rest of the last cache line, explicit so the compiler adds no tail padding
    uint8_t padding[64 - ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64];
};

static_assert( sizeof( rocketnet_iocp_worker_t ) == ROCKETNET_IOCP_WORKER_MEMBER_BYTES + 64 - ROCKETNET_IOCP_WORKER_MEMBER_BYTES % 64 &&
               sizeof( rocketnet_iocp_worker_t ) % 64 == 0, "a worker has no implicit padding" );

// These compact command banks contain only operation indices. The cold frame
// pool remains completion-owned until Windows retires each overlapped send.
// Main owns one bank, TX owns at most one, and ready banks are FIFO.
#define ROCKETNET_IOCP_SEND_BANKS 3
struct rocketnet_iocp_send_bank_t
{
    int count = 0;
    rocketnet_iocp_ref_t staged[ROCKETNET_IOCP_SEND_QUEUE_SIZE];
};

struct rocketnet_iocp_run_slot_t
{
    rocketnet_iocp_ref_t ref = {};
    uint64_t owner_epoch = 0;
    uint64_t ticket = 0;
    bool occupied = false;
};

struct rocketnet_server_iocp_t
{
    rocketnet_iocp_ref_queue_t<ROCKETNET_IOCP_SEND_QUEUE_SIZE> free_sends;
    rocketnet_iocp_ref_queue_t<ROCKETNET_IOCP_MAX_WORKERS * ROCKETNET_IOCP_RECEIVES_PER_WORKER> retry_receives;
    // The TX worker is the only run producer. Completion workers retire runs.
    // Tickets select fixed physical slots; the mutex publishes the whole run
    // and prevents an out-of-order completion from aliasing a reused slot.
    rocketnet_platform_mutex_t run_mutex = {};
    rocketnet_iocp_run_slot_t run_refs[ROCKETNET_IOCP_SEND_QUEUE_SIZE] = {};
    uint64_t next_run_ticket = 1;
    uint64_t owner_epoch = 0;
    std::atomic<uint64_t> num_staged{0};
    rocketnet_iocp_send_bank_t send_banks[ROCKETNET_IOCP_SEND_BANKS];
    rocketnet_platform_mutex_t send_mutex = {};
    rocketnet_platform_wake_t * send_wake = NULL;
    rocketnet_platform_thread_t * send_thread = NULL;
    std::atomic<bool> send_exited{true}; // also true before a worker is created
    int send_producer = 0; // caller-owned until stop publishes stopping
    int send_free[ROCKETNET_IOCP_SEND_BANKS] = {1, 2, 0};
    int send_free_count = ROCKETNET_IOCP_SEND_BANKS - 1;
    int send_ready[ROCKETNET_IOCP_SEND_BANKS] = {};
    int send_ready_head = 0;
    int send_ready_count = 0;
    void * context;
    SOCKET socket = INVALID_SOCKET;
    HANDLE port;
#if defined(ROCKETNET_WINDOWS_LIFECYCLE_TRACK)
    uint64_t socket_generation = 0;
    uint64_t port_generation = 0;
#endif
    bool ipv6;
    int num_workers;
    rocketnet_iocp_worker_t * workers;
    rocketnet_iocp_operation_t * sends;
    rocketnet_iocp_operation_t * receives;
    std::atomic<bool> stopping{false};
    std::atomic<int> pending{0};
    std::atomic<int> active_workers{0};
    std::atomic<uint64_t> counters[ROCKETNET_IOCP_NUM_COUNTERS]{};
    rocketnet_server_socket_process_packets_t process;
    std::atomic<int> features{0};
    // The socket's UDP_SEND_MSG_SIZE now. Only the TX worker writes it.
    int uso_stride = 0;
    LPFN_WSARECVMSG receive_message = NULL;
    uint8_t * uro_buffers = NULL;
    // The flush's view of the staged frames, slot by slot, for run grouping:
    // bytes (zero for an aborted or empty slot) and destination.
    int run_bytes[ROCKETNET_IOCP_SEND_QUEUE_SIZE];
    rocketnet_address_t run_to[ROCKETNET_IOCP_SEND_QUEUE_SIZE];
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
    std::atomic<int> test_faults[ROCKETNET_IOCP_TEST_NUM_FAULTS]{};
    std::atomic<int> test_fault_status[ROCKETNET_IOCP_TEST_NUM_FAULTS]{};
    std::atomic<bool> test_hold_tx{false};
    std::atomic<int> test_active_tx_batches{0};
    std::atomic<int> test_pending_tx_banks{0};
    std::atomic<bool> test_hold_completions{false};
    std::atomic<int> test_active_completion_batches{0};
#endif
};

static_assert( ROCKETNET_IOCP_SEND_QUEUE_SIZE <= UINT16_MAX, "packet handles reserve sixteen bits for the operation index" );

static std::atomic<uint64_t> rocketnet_iocp_next_owner_epoch{1};

// The fault seam (rocketnet_server_iocp_test_fail_next) exists only in the
// test executable. Elsewhere the check is the constant false, so the
// production paths carry no test branch.
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
static bool rocketnet_iocp_test_fault( rocketnet_server_iocp_t * iocp, int fault, int * status )
{
    int left = iocp->test_faults[fault].load( std::memory_order_acquire );
    while ( left > 0 )
    {
        if ( iocp->test_faults[fault].compare_exchange_weak( left, left - 1, std::memory_order_acq_rel ) )
        {
            *status = iocp->test_fault_status[fault].load( std::memory_order_relaxed );
            return true;
        }
    }
    return false;
}
#define ROCKETNET_IOCP_TEST_FAULT( iocp, fault, status ) rocketnet_iocp_test_fault( iocp, fault, status )
#else
#define ROCKETNET_IOCP_TEST_FAULT( iocp, fault, status ) false
#endif

static rocketnet_iocp_ref_t rocketnet_iocp_operation_ref( const rocketnet_iocp_operation_t * op )
{
    rocketnet_iocp_ref_t ref;
    ref.owner_epoch = op->owner_epoch;
    ref.generation = op->generation;
    ref.index = (uint32_t) op->index;
    return ref;
}

static rocketnet_iocp_operation_t * rocketnet_iocp_send_from_ref( rocketnet_server_iocp_t * iocp,
                                                                   const rocketnet_iocp_ref_t & ref,
                                                                   int state )
{
    if ( ref.owner_epoch != iocp->owner_epoch || ref.index >= ROCKETNET_IOCP_SEND_QUEUE_SIZE )
        return NULL;
    rocketnet_iocp_operation_t * op = &iocp->sends[ref.index];
    return op->owner_epoch == ref.owner_epoch && op->generation == ref.generation && op->state == state ? op : NULL;
}

static rocketnet_iocp_operation_t * rocketnet_iocp_receive_from_ref( rocketnet_server_iocp_t * iocp,
                                                                      const rocketnet_iocp_ref_t & ref,
                                                                      int state )
{
    const uint32_t capacity = (uint32_t) ( iocp->num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    if ( ref.owner_epoch != iocp->owner_epoch || ref.index >= capacity )
        return NULL;
    rocketnet_iocp_operation_t * op = &iocp->receives[ref.index];
    return op->owner_epoch == ref.owner_epoch && op->generation == ref.generation && op->state == state ? op : NULL;
}

static bool rocketnet_iocp_release_send( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t & ref, int state )
{
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, state );
    if ( !op ) return false;
    op->state = ROCKETNET_IOCP_OPERATION_FREE;
    const bool pushed = rocketnet_iocp_queue_push( &iocp->free_sends, ref );
    rocketnet_assert( pushed );
    if ( !pushed ) op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
    return pushed;
}

static bool rocketnet_iocp_acquire_send( rocketnet_server_iocp_t * iocp, rocketnet_iocp_ref_t * ref )
{
    rocketnet_iocp_ref_t free_ref;
    while ( rocketnet_iocp_queue_pop( &iocp->free_sends, &free_ref ) )
    {
        rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, free_ref, ROCKETNET_IOCP_OPERATION_FREE );
        if ( !op ) continue;
        // Sixteen packet-id bits hold the index; the rest hold this lease's
        // generation. Refuse the physically unreachable wrap rather than let
        // an ancient packet id name a new lease.
        if ( op->generation == ( UINT64_MAX >> 16 ) )
        {
            op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
            continue;
        }
        ++op->generation;
        op->state = ROCKETNET_IOCP_OPERATION_STAGED;
        *ref = rocketnet_iocp_operation_ref( op );
        return true;
    }
    return false;
}

static uint64_t rocketnet_iocp_packet_id( const rocketnet_iocp_ref_t & ref )
{
    return ( ref.generation << 16 ) | ref.index;
}

static bool rocketnet_iocp_packet_ref( rocketnet_server_iocp_t * iocp, uint64_t packet_id,
                                       rocketnet_iocp_ref_t * ref )
{
    ref->owner_epoch = iocp->owner_epoch;
    ref->generation = packet_id >> 16;
    ref->index = (uint32_t) ( packet_id & UINT16_MAX );
    return ref->generation != 0 && ref->index < ROCKETNET_IOCP_SEND_QUEUE_SIZE;
}

static bool rocketnet_iocp_allocate_run( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t * refs,
                                         int count, uint64_t * first_ticket )
{
    rocketnet_assert( count > 1 && count <= ROCKETNET_IOCP_USO_MAX_SEGMENTS );
    rocketnet_platform_mutex_acquire( &iocp->run_mutex );
    const uint64_t first = iocp->next_run_ticket;
    bool available = first != 0 && first <= UINT64_MAX - (uint64_t) count;
    for ( int i = 0; available && i < count; ++i )
    {
        const size_t index = (size_t) ( ( first + (uint64_t) i - 1 ) % ROCKETNET_IOCP_SEND_QUEUE_SIZE );
        available = !iocp->run_refs[index].occupied;
    }
    if ( available )
    {
        for ( int i = 0; i < count; ++i )
        {
            const uint64_t ticket = first + (uint64_t) i;
            rocketnet_iocp_run_slot_t * slot = &iocp->run_refs[( ticket - 1 ) % ROCKETNET_IOCP_SEND_QUEUE_SIZE];
            slot->ref = refs[i];
            slot->owner_epoch = iocp->owner_epoch;
            slot->ticket = ticket;
            slot->occupied = true;
        }
        iocp->next_run_ticket = first + (uint64_t) count;
        *first_ticket = first;
    }
    rocketnet_platform_mutex_release( &iocp->run_mutex );
    return available;
}

static bool rocketnet_iocp_take_run( rocketnet_server_iocp_t * iocp, uint64_t first, int count,
                                     rocketnet_iocp_ref_t * refs )
{
    if ( first == 0 || count < 1 || count > ROCKETNET_IOCP_USO_MAX_SEGMENTS ) return false;
    rocketnet_platform_mutex_acquire( &iocp->run_mutex );
    bool valid = true;
    for ( int i = 0; valid && i < count; ++i )
    {
        const uint64_t ticket = first + (uint64_t) i;
        const rocketnet_iocp_run_slot_t * slot = &iocp->run_refs[( ticket - 1 ) % ROCKETNET_IOCP_SEND_QUEUE_SIZE];
        valid = slot->occupied && slot->owner_epoch == iocp->owner_epoch && slot->ticket == ticket;
    }
    if ( valid )
    {
        for ( int i = 0; i < count; ++i )
        {
            const uint64_t ticket = first + (uint64_t) i;
            rocketnet_iocp_run_slot_t * slot = &iocp->run_refs[( ticket - 1 ) % ROCKETNET_IOCP_SEND_QUEUE_SIZE];
            refs[i] = slot->ref;
            slot->occupied = false;
        }
    }
    rocketnet_platform_mutex_release( &iocp->run_mutex );
    return valid;
}

static bool rocketnet_iocp_retire_send( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t & root )
{
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, root, ROCKETNET_IOCP_OPERATION_SEND_POSTED );
    if ( !op ) return false;
    const int run_count = op->run_count;
    const uint64_t run_ticket = op->run_ticket;
    if ( run_count <= 1 )
        return rocketnet_iocp_release_send( iocp, root, ROCKETNET_IOCP_OPERATION_SEND_POSTED );

    rocketnet_iocp_ref_t refs[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
    if ( !rocketnet_iocp_take_run( iocp, run_ticket, run_count, refs ) )
        return false;
    bool retired = true;
    // Releasing refs[0] publishes the root back to free_sends, so no member
    // of op may be read after the first iteration: another caller may acquire
    // and restage it immediately.
    for ( int i = 0; i < run_count; ++i )
        retired &= rocketnet_iocp_release_send( iocp, refs[i], ROCKETNET_IOCP_OPERATION_SEND_POSTED );
    return retired;
}

static int rocketnet_iocp_address_write( const rocketnet_address_t * input, bool ipv6, sockaddr_storage * storage )
{
    rocketnet_address_t address = *input;
    memset( storage, 0, sizeof(*storage) );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
        rocketnet_address_convert_ipv4_to_ipv6( &address );
    if ( ipv6 && address.type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 * out = (sockaddr_in6*) storage;
        out->sin6_family = AF_INET6;
        out->sin6_port = htons( address.port );
        for ( int i = 0; i < 8; ++i )
        {
            out->sin6_addr.s6_addr[i * 2] = (uint8_t) ( address.data.ipv6[i] >> 8 );
            out->sin6_addr.s6_addr[i * 2 + 1] = (uint8_t) address.data.ipv6[i];
        }
        return sizeof(sockaddr_in6);
    }
    if ( !ipv6 && address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        sockaddr_in * out = (sockaddr_in*) storage;
        out->sin_family = AF_INET;
        out->sin_port = htons( address.port );
        memcpy( &out->sin_addr, address.data.ipv4, 4 );
        return sizeof(sockaddr_in);
    }
    return 0;
}

// a source from its sockaddr; an IPv4-mapped IPv6 source passes the one
// receive rule, rocketnet_address_accept_received_source: canonicalised to
// IPv4 on the dual-stack socket, refused on any other
static bool rocketnet_iocp_address_read( const sockaddr_storage * storage, int bytes, bool dual_stack, rocketnet_address_t * address )
{
    memset( address, 0, sizeof(*address) );
    if ( storage->ss_family == AF_INET && bytes >= (int) sizeof(sockaddr_in) )
    {
        const sockaddr_in * from = (const sockaddr_in*) storage;
        address->type = ROCKETNET_ADDRESS_IPV4;
        address->port = ntohs( from->sin_port );
        memcpy( address->data.ipv4, &from->sin_addr, 4 );
        return true;
    }
    if ( storage->ss_family == AF_INET6 && bytes >= (int) sizeof(sockaddr_in6) )
    {
        const sockaddr_in6 * from = (const sockaddr_in6*) storage;
        address->type = ROCKETNET_ADDRESS_IPV6;
        address->port = ntohs( from->sin6_port );
        for ( int i = 0; i < 8; ++i )
            address->data.ipv6[i] = (uint16_t) ( ( from->sin6_addr.s6_addr[i * 2] << 8 ) | from->sin6_addr.s6_addr[i * 2 + 1] );
        return rocketnet_address_accept_received_source( address, dual_stack );
    }
    return false;
}

static bool rocketnet_iocp_post_receive( rocketnet_server_iocp_t * iocp, rocketnet_iocp_operation_t * op )
{
    // Only a completion worker or the creating thread posts a receive; no
    // publication mutex is held. A worker observing stop reissues cancellation
    // after its last potential repost:
    // a late repost is cancelled by that worker's next pass. Stack addresses/flags must not back pending I/O.
    if ( iocp->stopping.load( std::memory_order_acquire ) )
    {
        op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
        return false;
    }
    rocketnet_assert( op->state == ROCKETNET_IOCP_OPERATION_IDLE || op->state == ROCKETNET_IOCP_OPERATION_RETRY );
    if ( op->generation == UINT64_MAX )
    {
        op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
        return false;
    }
    ++op->generation;
    op->completion_ref = rocketnet_iocp_operation_ref( op );
    op->state = ROCKETNET_IOCP_OPERATION_RECEIVE_POSTED;
    memset( &op->overlapped, 0, sizeof(op->overlapped) );
    op->address_bytes = sizeof(op->address);
    op->flags = 0;
    int result;
    int error = 0;
    if ( ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST, &error ) )
    {
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        result = SOCKET_ERROR;
    }
    else if ( op->uro_data )
    {
        // URO: a buffer of the coalesced maximum and a control area for
        // UDP_COALESCED_INFO, the stride of all but the last datagram.
        op->buffer.buf = (char*) op->uro_data;
        op->buffer.len = ROCKETNET_IOCP_URO_BUFFER_BYTES;
        memset( &op->message, 0, sizeof(op->message) );
        op->message.name = (LPSOCKADDR) &op->address;
        op->message.namelen = sizeof(op->address);
        op->message.lpBuffers = &op->buffer;
        op->message.dwBufferCount = 1;
        op->message.Control.buf = op->control;
        op->message.Control.len = sizeof(op->control);
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        result = iocp->receive_message( iocp->socket, &op->message, NULL, &op->overlapped, NULL );
    }
    else
    {
        op->buffer.buf = (char*) op->frame;
        op->buffer.len = ROCKETNET_RECEIVE_CEILING + 1;
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        result = WSARecvFrom( iocp->socket, &op->buffer, 1, NULL, &op->flags,
            (sockaddr*) &op->address, &op->address_bytes, &op->overlapped, NULL );
    }
    if ( result != 0 && error == 0 )
        error = WSAGetLastError();
    if ( result == 0 || error == WSA_IO_PENDING )
        return true; // Immediate success also produces exactly one IOCP event.
    iocp->pending.fetch_sub( 1, std::memory_order_release );
    // a post refused with an ICMP report of an earlier send is counted with
    // the reset statuses, then waits on the retry list like every refused post
    // (windows-udp-connreset audit, iocp post)
    if ( rocketnet_platform_receive_status_is_reset( error ) )
        rocketnet_platform_count_receive_reset();
    iocp->counters[ROCKETNET_IOCP_RECEIVE_REPOST_FAILED].fetch_add( 1, std::memory_order_relaxed );
    op->state = ROCKETNET_IOCP_OPERATION_RETRY;
    const bool queued = rocketnet_iocp_queue_push( &iocp->retry_receives, op->completion_ref );
    rocketnet_assert( queued );
    if ( !queued ) op->state = ROCKETNET_IOCP_OPERATION_RETIRED;
    return false;
}

// The UDP_COALESCED_INFO stride of a received message's control area, or 0
// when it has none. A UDP_COALESCED_INFO header whose cmsg_len is shorter
// than WSA_CMSG_LEN( sizeof( DWORD ) ) carries no whole DWORD and yields 0:
// only bytes the header claims are read. Not static: the unit suite calls it
// over a built WSAMSG (test_windows_coalesced_cmsg_short_length_is_not_read).
DWORD rocketnet_iocp_coalesced_stride( WSAMSG * message )
{
    for ( WSACMSGHDR * cmsg = WSA_CMSG_FIRSTHDR( message ); cmsg; cmsg = WSA_CMSG_NXTHDR( message, cmsg ) )
    {
        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_COALESCED_INFO )
        {
            DWORD stride = 0;
            if ( cmsg->cmsg_len >= WSA_CMSG_LEN( sizeof( stride ) ) )
                memcpy( &stride, WSA_CMSG_DATA( cmsg ), sizeof( stride ) );
            return stride;
        }
    }
    return 0;
}

// Publishes one URO completion: the datagrams of one source, each the
// UDP_COALESCED_INFO stride except a shorter last one, split by the shared
// coalescing rule and each checked against the bounds a single datagram is.
static void rocketnet_iocp_publish_uro( rocketnet_server_iocp_t * iocp, rocketnet_iocp_receive_buffer_t * buffer,
    rocketnet_iocp_operation_t * op, bool success, int receive_error, DWORD bytes )
{
    rocketnet_address_t from;
    if ( !success || bytes == 0 || ( op->message.dwFlags & MSG_TRUNC ) ||
         !rocketnet_iocp_address_read( &op->address, op->message.namelen, iocp->ipv6, &from ) )
    {
        iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( 1, std::memory_order_relaxed );
        if ( success && bytes == 0 )
            iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT].fetch_add( 1, std::memory_order_relaxed );
        else if ( receive_error == WSAEMSGSIZE || ( success && ( op->message.dwFlags & MSG_TRUNC ) ) )
            iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE].fetch_add( 1, std::memory_order_relaxed );
        return;
    }
    const DWORD stride = rocketnet_iocp_coalesced_stride( &op->message );
    const int message_bytes = (int) bytes;
    const int segment_bytes = stride > 0 && stride < bytes ? (int) stride : message_bytes;
    // Split window by window: each window but the last is a whole number of
    // strides, so the windows together are the split of the whole message.
    const int window_limit = segment_bytes * ROCKETNET_IOCP_URO_MAX_SEGMENTS;
    int offsets[ROCKETNET_IOCP_URO_MAX_SEGMENTS];
    int lengths[ROCKETNET_IOCP_URO_MAX_SEGMENTS];
    int segments = 0;
    for ( int base = 0; base < message_bytes; )
    {
        const int remaining = message_bytes - base;
        const int window = remaining < window_limit ? remaining : window_limit;
        const int count = rocketnet_coalesce_split( window, segment_bytes, offsets, lengths, ROCKETNET_IOCP_URO_MAX_SEGMENTS );
        for ( int i = 0; i < count; ++i )
        {
            ++segments;
            const int length = lengths[i];
            if ( length > 0 && length <= ROCKETNET_RECEIVE_CEILING && buffer->num_packets < ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE )
            {
                const int index = buffer->num_packets++;
                buffer->from[index] = from;
                buffer->packet_bytes[index] = length;
                memcpy( buffer->data[index], op->uro_data + base + offsets[i], (size_t) length );
                iocp->counters[ROCKETNET_IOCP_RECEIVE_COMPLETED].fetch_add( 1, std::memory_order_relaxed );
            }
            else
            {
                iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( 1, std::memory_order_relaxed );
                if ( length <= 0 )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT].fetch_add( 1, std::memory_order_relaxed );
                else if ( length > ROCKETNET_RECEIVE_CEILING )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE].fetch_add( 1, std::memory_order_relaxed );
            }
        }
        base += window;
    }
    if ( segments > 1 )
        iocp->counters[ROCKETNET_IOCP_URO_BATCHES].fetch_add( 1, std::memory_order_relaxed );
}

// A worker never gives its fill bank away until a different free bank is
// available. READY banks are FIFO; EXPOSED belongs to the main thread until
// its next receive call. This mutex protects only the index exchange.
static void rocketnet_iocp_publish_receive_bank( rocketnet_iocp_worker_t * worker )
{
    if ( worker->buffers[worker->fill_index].num_packets == 0 )
        return;
    bool published = false;
    rocketnet_platform_mutex_acquire( &worker->mutex );
    if ( worker->free_count > 0 )
    {
        const int tail = ( worker->ready_head + worker->ready_count ) % ROCKETNET_IOCP_RECEIVE_BANKS;
        rocketnet_assert( worker->ready_count < ROCKETNET_IOCP_RECEIVE_BANKS );
        worker->ready_banks[tail] = worker->fill_index;
        worker->ready_count++;
        worker->fill_index = worker->free_banks[--worker->free_count];
        published = true;
    }
    rocketnet_platform_mutex_release( &worker->mutex );
    if ( published ) worker->buffers[worker->fill_index].num_packets = 0;
}

static void rocketnet_iocp_worker( void * argument )
{
    rocketnet_iocp_worker_t * worker = (rocketnet_iocp_worker_t*) argument;
    rocketnet_server_iocp_t * iocp = worker->owner;
    for ( ;; )
    {
        // RNBufferHandoff WorkerExit sends every bank before receive cancellation.
        if ( iocp->stopping.load( std::memory_order_acquire ) &&
             iocp->send_exited.load( std::memory_order_acquire ) )
        {
            // Every worker repeats cancellation after its last possible repost.
            // Cancel only receives, so the final posted sends can complete.
            if ( iocp->socket != INVALID_SOCKET )
                for ( int i = 0; i < iocp->num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
                    if ( !CancelIoEx( (HANDLE) iocp->socket, &iocp->receives[i].overlapped ) &&
                         GetLastError() != ERROR_NOT_FOUND )
                        rocketnet_error( "IOCP cancellation failed: %lu", GetLastError() );
            if ( iocp->pending.load( std::memory_order_acquire ) == 0 ) break;
        }

        OVERLAPPED_ENTRY entries[64];
        ULONG count = 0;
        const DWORD timeout = rocketnet_iocp_queue_count( &iocp->retry_receives ) ? 1 : 50;
        const BOOL ok = GetQueuedCompletionStatusEx( iocp->port, entries, 64, &count, timeout, FALSE );
        if ( !ok )
        {
            count = 0;
            if ( GetLastError() != WAIT_TIMEOUT )
            {
                rocketnet_error( "IOCP completion dequeue failed: %lu", GetLastError() );
                rocketnet_platform_sleep( 0.001 );
            }
        }
        if ( count )
            iocp->counters[ROCKETNET_IOCP_COMPLETION_BATCHES].fetch_add( 1, std::memory_order_relaxed );

#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
        for ( ;; )
        {
            while ( iocp->test_hold_completions.load( std::memory_order_acquire ) )
                rocketnet_platform_sleep( 0.001 );
            iocp->test_active_completion_batches.fetch_add( 1, std::memory_order_acq_rel );
            if ( !iocp->test_hold_completions.load( std::memory_order_acquire ) )
                break;
            iocp->test_active_completion_batches.fetch_sub( 1, std::memory_order_release );
        }
#endif
        rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[worker->fill_index];
        for ( ULONG i = 0; i < count; ++i )
        {
            if ( !entries[i].lpOverlapped )
                continue; // A shutdown wakeup, not an I/O operation.
            rocketnet_iocp_operation_t * op = (rocketnet_iocp_operation_t*) entries[i].lpOverlapped;
            ULONG_PTR status = entries[i].Internal;
            int injected = 0;
            if ( ROCKETNET_IOCP_TEST_FAULT( iocp, op->send ? ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION : ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION, &injected ) )
                status = (ULONG_PTR) injected;
            const bool success = status == 0;
            const DWORD bytes = entries[i].dwNumberOfBytesTransferred;
            if ( op->send )
            {
                const rocketnet_iocp_ref_t completed = op->completion_ref;
                if ( !rocketnet_iocp_send_from_ref( iocp, completed, ROCKETNET_IOCP_OPERATION_SEND_POSTED ) )
                    continue;
                const int counter = success && bytes == (DWORD) op->run_bytes ? ROCKETNET_IOCP_SEND_COMPLETED : ROCKETNET_IOCP_SEND_FAILED;
                iocp->counters[counter].fetch_add( (uint64_t) op->run_count, std::memory_order_relaxed );
                iocp->pending.fetch_sub( 1, std::memory_order_release );
                const bool retired = rocketnet_iocp_retire_send( iocp, completed );
                rocketnet_assert( retired );
                if ( !retired ) rocketnet_error( "IOCP send completion carried a stale operation generation" );
                continue;
            }

            const rocketnet_iocp_ref_t completed = op->completion_ref;
            if ( !rocketnet_iocp_receive_from_ref( iocp, completed, ROCKETNET_IOCP_OPERATION_RECEIVE_POSTED ) )
                continue;

            rocketnet_address_t from;
            const bool running = !iocp->stopping.load( std::memory_order_acquire );
            int receive_error = 0;
            if ( !success && running && entries[i].Internal != 0 )
            {
                // a receive completed with an ICMP report of an earlier send
                // (port, host or network unreachable): counted, dropped and
                // reposted below like every failed receive, never the end of
                // the worker (windows-udp-connreset audit, iocp and URO completion)
                DWORD transferred = 0;
                DWORD completion_flags = 0;
                if ( !WSAGetOverlappedResult( iocp->socket, &op->overlapped, &transferred, FALSE, &completion_flags ) )
                {
                    receive_error = WSAGetLastError();
                    if ( rocketnet_platform_receive_status_is_reset( receive_error ) )
                        rocketnet_platform_count_receive_reset();
                }
            }
            if ( op->uro_data )
            {
                if ( running )
                    rocketnet_iocp_publish_uro( iocp, buffer, op, success, receive_error, bytes );
            }
            else if ( running && success && bytes > 0 && bytes <= ROCKETNET_RECEIVE_CEILING &&
                 buffer->num_packets < ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE &&
                 rocketnet_iocp_address_read( &op->address, op->address_bytes, iocp->ipv6, &from ) )
            {
                const int index = buffer->num_packets++;
                buffer->from[index] = from;
                buffer->packet_bytes[index] = (int) bytes;
                memcpy( buffer->data[index], op->frame, bytes );
                iocp->counters[ROCKETNET_IOCP_RECEIVE_COMPLETED].fetch_add( 1, std::memory_order_relaxed );
            }
            else if ( running )
            {
                iocp->counters[ROCKETNET_IOCP_RECEIVE_DROPPED].fetch_add( 1, std::memory_order_relaxed );
                if ( success && bytes == 0 )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT].fetch_add( 1, std::memory_order_relaxed );
                else if ( receive_error == WSAEMSGSIZE || ( success && bytes > ROCKETNET_RECEIVE_CEILING ) )
                    iocp->counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE].fetch_add( 1, std::memory_order_relaxed );
            }

            iocp->pending.fetch_sub( 1, std::memory_order_release );
            op->state = ROCKETNET_IOCP_OPERATION_IDLE;
            rocketnet_iocp_post_receive( iocp, op );
        }

        // A transient posting failure must not permanently shrink the receive
        // pool. Capture the enqueue ticket before the first pop: an immediate
        // refusal requeues at a later ticket and cannot loop in this pass.
        const uint64_t retry_stop = iocp->retry_receives.enqueue.load( std::memory_order_acquire );
        for ( ;; )
        {
            rocketnet_iocp_ref_t retry;
            if ( !rocketnet_iocp_queue_pop_before( &iocp->retry_receives, retry_stop, &retry ) ) break;
            rocketnet_iocp_operation_t * op = rocketnet_iocp_receive_from_ref( iocp, retry, ROCKETNET_IOCP_OPERATION_RETRY );
            if ( !op ) continue;
            rocketnet_iocp_post_receive( iocp, op );
        }
        if ( !iocp->stopping.load( std::memory_order_acquire ) )
            rocketnet_iocp_publish_receive_bank( worker );
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
        iocp->test_active_completion_batches.fetch_sub( 1, std::memory_order_release );
#endif
    }
    iocp->active_workers.fetch_sub( 1, std::memory_order_release );
}

void rocketnet_server_iocp_stop( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp || iocp->stopping.load( std::memory_order_acquire ) ) return;
    // tla/RNBufferHandoff.tla MainStop transfers the final producer with quit.
    // Fill threads have joined before stop. The release transfers the final
    // unpublished producer to TX, without scanning it or acquiring a mutex.
    uint64_t count = iocp->num_staged.load( std::memory_order_relaxed );
    if ( count > ROCKETNET_IOCP_SEND_QUEUE_SIZE ) count = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
    iocp->send_banks[iocp->send_producer].count = (int) count;
    iocp->stopping.store( true, std::memory_order_release );
    if ( iocp->send_wake ) rocketnet_platform_wake_set( iocp->send_wake );
    // Cancellation runs on the workers after their current pass. This request
    // never acquires a worker mutex or waits for an overlapped operation.
    if ( iocp->port )
        for ( int i = 0; i < iocp->num_workers; ++i )
            PostQueuedCompletionStatus( iocp->port, 0, 0, NULL );
}

bool rocketnet_server_iocp_stopped( rocketnet_server_iocp_t * iocp )
{
    return iocp->stopping.load( std::memory_order_acquire ) &&
           iocp->send_exited.load( std::memory_order_acquire ) &&
           iocp->pending.load( std::memory_order_acquire ) == 0 &&
           iocp->active_workers.load( std::memory_order_acquire ) == 0;
}

void rocketnet_server_iocp_destroy( rocketnet_server_iocp_t * iocp )
{
    if ( !iocp ) return;
    const bool stopped = rocketnet_server_iocp_stopped( iocp );
    rocketnet_assert( stopped );
    if ( !stopped ) return;
    if ( iocp->send_thread )
    {
        rocketnet_platform_thread_join( iocp->send_thread );
        rocketnet_platform_thread_destroy( iocp->send_thread );
    }
    if ( iocp->send_wake ) rocketnet_platform_wake_destroy( iocp->send_wake );
    if ( iocp->send_mutex.ok ) rocketnet_platform_mutex_destroy( &iocp->send_mutex );
    if ( iocp->workers )
        for ( int i = 0; i < iocp->num_workers; ++i )
        {
            rocketnet_iocp_worker_t * worker = &iocp->workers[i];
            if ( worker->thread )
            {
                rocketnet_platform_thread_join( worker->thread );
                rocketnet_platform_thread_destroy( worker->thread );
            }
            if ( worker->mutex.ok ) rocketnet_platform_mutex_destroy( &worker->mutex );
            worker->~rocketnet_iocp_worker_t();
        }
    if ( iocp->run_mutex.ok ) rocketnet_platform_mutex_destroy( &iocp->run_mutex );
    rocketnet_assert( iocp->pending.load() == 0 );
    if ( iocp->socket != INVALID_SOCKET )
    {
#if defined(ROCKETNET_WINDOWS_LIFECYCLE_TRACK)
        if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) iocp->socket, iocp->socket_generation ) )
        {
            const int closed = closesocket( iocp->socket );
            rocketnet_lifecycle_track_close_end( iocp->socket_generation, closed == 0 );
        }
#else
        closesocket( iocp->socket );
#endif
    }
    if ( iocp->port )
    {
#if defined(ROCKETNET_WINDOWS_LIFECYCLE_TRACK)
        if ( rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_IOCP, (uintptr_t) iocp->port, iocp->port_generation ) )
        {
            const BOOL closed = CloseHandle( iocp->port );
            rocketnet_lifecycle_track_close_end( iocp->port_generation, closed != 0 );
        }
#else
        CloseHandle( iocp->port );
#endif
    }
    if ( iocp->sends )
        for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i ) iocp->sends[i].~rocketnet_iocp_operation_t();
    if ( iocp->receives )
        for ( int i = 0; i < iocp->num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i ) iocp->receives[i].~rocketnet_iocp_operation_t();
    void * context = iocp->context;
    rocketnet_free( context, iocp->workers );
    rocketnet_free( context, iocp->sends );
    rocketnet_free( context, iocp->receives );
    rocketnet_free( context, iocp->uro_buffers );
    iocp->~rocketnet_server_iocp_t();
    rocketnet_free( context, iocp );
}

uint64_t rocketnet_server_iocp_allocation_bytes(int workers)
{
    if (workers < 1 || workers > ROCKETNET_IOCP_MAX_WORKERS) return 0;
    uint64_t bytes = rocketnet_memory_allocations({sizeof(rocketnet_server_iocp_t),
        (uint64_t)workers * sizeof(rocketnet_iocp_worker_t),
        (uint64_t)ROCKETNET_IOCP_SEND_QUEUE_SIZE * sizeof(rocketnet_iocp_operation_t),
        (uint64_t)workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * sizeof(rocketnet_iocp_operation_t),
        (uint64_t)workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * ROCKETNET_IOCP_URO_BUFFER_BYTES});
    if (!bytes || !rocketnet_memory_add(bytes, (uint64_t)workers + 1, rocketnet_platform_thread_allocation_bytes()) ||
        !rocketnet_memory_add(bytes, 1, rocketnet_platform_wake_allocation_bytes())) return 0;
    return bytes <= SIZE_MAX ? bytes : 0;
}

static void rocketnet_iocp_send_worker( void * argument );

#if ROCKETNET_DEV
static std::atomic<int> rocketnet_iocp_test_forced_flags{0};

void rocketnet_server_iocp_test_force_flags( int flags )
{
    rocketnet_iocp_test_forced_flags.store( flags, std::memory_order_relaxed );
}
#endif

rocketnet_server_iocp_t * rocketnet_server_iocp_create( void * context, rocketnet_address_t * bind_address, int num_workers
#if ROCKETNET_DEV
    , int fail_stage, int flags
#endif
    )
{
#if ROCKETNET_DEV
    flags |= rocketnet_iocp_test_forced_flags.load( std::memory_order_relaxed );
#endif
    if ( !bind_address || ( bind_address->type != ROCKETNET_ADDRESS_IPV4 && bind_address->type != ROCKETNET_ADDRESS_IPV6 ) ) return NULL;
    if ( num_workers == 0 ) num_workers = ROCKETNET_IOCP_DEFAULT_WORKERS;
    if ( num_workers < 1 || num_workers > ROCKETNET_IOCP_MAX_WORKERS )
    {
        rocketnet_warn( "iocp transport refused: worker count %d is outside 1..%d", num_workers, ROCKETNET_IOCP_MAX_WORKERS );
        return NULL;
    }
    void * storage = rocketnet_malloc( context, sizeof(rocketnet_server_iocp_t) );
    if ( !storage ) return NULL;
    rocketnet_server_iocp_t * iocp = new (storage) rocketnet_server_iocp_t();
    iocp->context = context;
    iocp->num_workers = num_workers;
    iocp->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    iocp->owner_epoch = rocketnet_iocp_next_owner_epoch.fetch_add( 1, std::memory_order_relaxed );
    rocketnet_iocp_queue_initialize( &iocp->free_sends );
    rocketnet_iocp_queue_initialize( &iocp->retry_receives );

    sockaddr_storage address;
    int address_bytes = rocketnet_iocp_address_write( bind_address, iocp->ipv6, &address );
    // one update's inbound packets at the full client count: the iocp create
    // does not carry max_clients, so it sizes for ROCKETNET_MAX_CLIENTS (a
    // request; the stack charges only what it holds)
    const int receive_buffer_bytes = rocketnet_server_receive_buffer_request( ROCKETNET_MAX_CLIENTS );
    int buffer_bytes = receive_buffer_bytes;
    int granted_receive_bytes = 0;
    int granted_length = sizeof( granted_receive_bytes );
    DWORD returned_bytes = 0;
    BOOL enabled = TRUE;
    DWORD v6_only = FALSE;
    if ( iocp->owner_epoch == 0 || !rocketnet_platform_mutex_create( &iocp->run_mutex ) ||
         !rocketnet_platform_mutex_create( &iocp->send_mutex ) )
        goto failed;

    iocp->workers = (rocketnet_iocp_worker_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_worker_t) * (size_t) num_workers );
    if ( !iocp->workers ) goto failed;
    for ( int i = 0; i < num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = new (&iocp->workers[i]) rocketnet_iocp_worker_t();
        for ( int bank = 1; bank < ROCKETNET_IOCP_RECEIVE_BANKS; ++bank )
            worker->free_banks[bank - 1] = bank;
    }

    iocp->sends = (rocketnet_iocp_operation_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_operation_t) * ROCKETNET_IOCP_SEND_QUEUE_SIZE );
    if ( !iocp->sends ) goto failed;
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
    {
        rocketnet_iocp_operation_t * op = new (&iocp->sends[i]) rocketnet_iocp_operation_t();
        op->index = i;
        op->send = true;
        op->owner_epoch = iocp->owner_epoch;
        op->state = ROCKETNET_IOCP_OPERATION_FREE;
    }
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
        if ( !rocketnet_iocp_queue_push( &iocp->free_sends, rocketnet_iocp_operation_ref( &iocp->sends[i] ) ) ) goto failed;
    iocp->receives = (rocketnet_iocp_operation_t*) rocketnet_malloc( context, sizeof(rocketnet_iocp_operation_t) * (size_t) num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    if ( !iocp->receives ) goto failed;
    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
    {
        rocketnet_iocp_operation_t * op = new (&iocp->receives[i]) rocketnet_iocp_operation_t();
        op->index = i;
        op->owner_epoch = iocp->owner_epoch;
        op->state = ROCKETNET_IOCP_OPERATION_IDLE;
    }

    iocp->socket = WSASocketW( iocp->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED );
#if defined(ROCKETNET_WINDOWS_LIFECYCLE_TRACK)
    if ( iocp->socket != INVALID_SOCKET )
        iocp->socket_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_SOCKET, (uintptr_t) iocp->socket, "iocp socket" );
#endif
    if ( iocp->socket == INVALID_SOCKET
#if ROCKETNET_DEV
         || fail_stage == 1
#endif
       ) goto failed;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &enabled, sizeof(enabled) ) == SOCKET_ERROR ) goto failed;
    if ( iocp->ipv6 && setsockopt( iocp->socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &v6_only, sizeof(v6_only) ) == SOCKET_ERROR ) goto failed;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_RCVBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    if ( getsockopt( iocp->socket, SOL_SOCKET, SO_RCVBUF, (char*) &granted_receive_bytes, &granted_length ) == SOCKET_ERROR ) granted_receive_bytes = 0;
    rocketnet_server_receive_buffer_granted( "iocp", receive_buffer_bytes, granted_receive_bytes );
    buffer_bytes = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    if ( setsockopt( iocp->socket, SOL_SOCKET, SO_SNDBUF, (const char*) &buffer_bytes, sizeof(buffer_bytes) ) == SOCKET_ERROR ) goto failed;
    // Match the portable socket's don't-fragment rule, tolerating option refusal.
    setsockopt( iocp->socket, iocp->ipv6 ? IPPROTO_IPV6 : IPPROTO_IP,
                iocp->ipv6 ? IPV6_DONTFRAG : IP_DONTFRAGMENT, (const char*) &enabled, sizeof(enabled) );
    // windows-udp-connreset audit, iocp socket: both ICMP reset reports off;
    // a refusal is logged once and the receive paths skip the statuses
    rocketnet_platform_socket_disable_udp_resets( (uintptr_t) iocp->socket );
    if ( bind( iocp->socket, (sockaddr*) &address, address_bytes ) == SOCKET_ERROR ) goto failed;
    address_bytes = sizeof(address);
    if ( getsockname( iocp->socket, (sockaddr*) &address, &address_bytes ) == SOCKET_ERROR ) goto failed;

    // The probes run on the bound socket; a refusal is a tier, not an error.
    // UDP_SEND_MSG_SIZE (IPPROTO_UDP socket options, Winsock reference,
    // learn.microsoft.com): a DWORD; non-zero makes the stack break each sent
    // buffer into messages of that size, the last one possibly shorter; zero,
    // the default, is no segmentation. The probe writes zero.
#if ROCKETNET_DEV
    if ( ( flags & ROCKETNET_IOCP_DISABLE_USO ) == 0 )
#endif
    {
        DWORD zero = 0;
        if ( setsockopt( iocp->socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (const char*) &zero, sizeof(zero) ) != SOCKET_ERROR )
            iocp->features.fetch_or( ROCKETNET_IOCP_FEATURE_USO, std::memory_order_relaxed );
    }
    // UDP_RECV_MAX_COALESCED_SIZE (same page): a DWORD; non-zero lets the stack
    // coalesce datagrams of one source address and port into one message of
    // at most that many bytes, all the same size but a shorter last one; the
    // stride reaches the application only through a receive with control
    // data (WSARecvMsg) as the DWORD control message UDP_COALESCED_INFO.
#if ROCKETNET_DEV
    if ( ( flags & ROCKETNET_IOCP_DISABLE_URO ) == 0 )
#endif
    {
        GUID guid = WSAID_WSARECVMSG;
        DWORD maximum = ROCKETNET_IOCP_URO_BUFFER_BYTES;
        if ( WSAIoctl( iocp->socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &iocp->receive_message,
                       sizeof(iocp->receive_message), &returned_bytes, NULL, NULL ) != SOCKET_ERROR && iocp->receive_message &&
             setsockopt( iocp->socket, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, (const char*) &maximum, sizeof(maximum) ) != SOCKET_ERROR )
        {
            iocp->uro_buffers = (uint8_t*) rocketnet_malloc( context, (size_t) num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER * ROCKETNET_IOCP_URO_BUFFER_BYTES );
            if ( !iocp->uro_buffers ) goto failed;
            iocp->features.fetch_or( ROCKETNET_IOCP_FEATURE_URO, std::memory_order_relaxed );
        }
    }

    iocp->port = CreateIoCompletionPort( (HANDLE) iocp->socket, NULL, 0, (DWORD) num_workers );
#if defined(ROCKETNET_WINDOWS_LIFECYCLE_TRACK)
    if ( iocp->port )
        iocp->port_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_IOCP, (uintptr_t) iocp->port, "iocp completion port" );
#endif
    if ( !iocp->port
#if ROCKETNET_DEV
         || fail_stage == 2
#endif
       ) goto failed;
    iocp->send_wake = rocketnet_platform_wake_create( context, NULL );
    if ( !iocp->send_wake ) goto failed;
    iocp->send_exited.store( false, std::memory_order_relaxed );
    iocp->send_thread = rocketnet_platform_thread_create( context, rocketnet_iocp_send_worker, iocp );
    if ( !iocp->send_thread )
    {
        iocp->send_exited.store( true, std::memory_order_relaxed );
        goto failed;
    }
    rocketnet_platform_server_thread_priority( iocp->send_thread );
    for ( int i = 0; i < num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[i];
        worker->owner = iocp;
        if ( !rocketnet_platform_mutex_create( &worker->mutex ) ) goto failed;
        iocp->active_workers.fetch_add( 1, std::memory_order_relaxed );
        worker->thread = rocketnet_platform_thread_create( context, rocketnet_iocp_worker, worker );
        if ( !worker->thread )
        {
            iocp->active_workers.fetch_sub( 1, std::memory_order_relaxed );
            goto failed;
        }
        // each worker takes the server thread priority from this thread,
        // right after its create, as the GSO queue threads do
        rocketnet_platform_server_thread_priority( worker->thread );
#if ROCKETNET_DEV
        if ( fail_stage == 3 ) goto failed;
#endif
    }
    for ( int i = 0; i < num_workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
    {
        if ( iocp->uro_buffers )
            iocp->receives[i].uro_data = iocp->uro_buffers + (size_t) i * ROCKETNET_IOCP_URO_BUFFER_BYTES;
        if ( !rocketnet_iocp_post_receive( iocp, &iocp->receives[i] ) ) goto failed;
#if ROCKETNET_DEV
        if ( fail_stage == 4 ) goto failed;
#endif
    }
    if ( !rocketnet_iocp_address_read( &address, address_bytes, iocp->ipv6, bind_address ) ) goto failed;
    rocketnet_info( "iocp transport: %d completion workers and one TX worker on port %d, USO %s, URO %s", num_workers, bind_address->port,
        ( iocp->features.load() & ROCKETNET_IOCP_FEATURE_USO ) ? "yes" : "no (WSASendTo per datagram)",
        ( iocp->features.load() & ROCKETNET_IOCP_FEATURE_URO ) ? "yes" : "no (WSARecvFrom per datagram)" );
    return iocp;

failed:
#if ROCKETNET_DEV
    rocketnet_warn( "iocp transport create failed (Winsock %d, Win32 %lu, test stage %d)", WSAGetLastError(), GetLastError(), fail_stage );
#else
    rocketnet_warn( "iocp transport create failed (Winsock %d, Win32 %lu)", WSAGetLastError(), GetLastError() );
#endif
    rocketnet_server_iocp_stop( iocp );
    while ( !rocketnet_server_iocp_stopped( iocp ) ) rocketnet_platform_sleep( 0.001 );
    rocketnet_server_iocp_destroy( iocp );
    return NULL;
}

uint8_t * rocketnet_server_iocp_start_packet( rocketnet_server_iocp_t * iocp, const rocketnet_address_t * to, uint64_t * packet_id )
{
    if ( iocp->stopping.load( std::memory_order_acquire ) ) return NULL;
    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_acquire_send( iocp, &ref ) ) return NULL;
    rocketnet_iocp_operation_t * op = &iocp->sends[ref.index];
    op->address_bytes = rocketnet_iocp_address_write( to, iocp->ipv6, &op->address );
    if ( !op->address_bytes )
    {
        rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
        return NULL;
    }
    // Reserve a staging slot only after acquiring a usable frame. A failed
    // pop/address must not consume capacity while completions return frames
    // during this same fill phase. Every accepted slot owns one pool entry.
    const uint64_t slot = iocp->num_staged.fetch_add( 1, std::memory_order_relaxed );
    if ( slot >= ROCKETNET_IOCP_SEND_QUEUE_SIZE )
    {
        rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
        return NULL;
    }
    op->packet_bytes = 0;
    op->to = *to;
    iocp->send_banks[iocp->send_producer].staged[slot] = ref;
    *packet_id = rocketnet_iocp_packet_id( ref );
    return op->frame;
}

void rocketnet_server_iocp_finish_packet( rocketnet_server_iocp_t * iocp, uint64_t packet_id, uint8_t * frame, int packet_bytes )
{
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );
    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_packet_ref( iocp, packet_id, &ref ) ) return;
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    rocketnet_assert( op && frame == op->frame );
    // Release callers may not use a frame or generation the transport did not issue.
    if ( !op || frame != op->frame ) return;
    (void) frame;
    op->packet_bytes = packet_bytes;
}

void rocketnet_server_iocp_abort_packet( rocketnet_server_iocp_t * iocp, uint64_t packet_id, uint8_t * frame )
{
    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_packet_ref( iocp, packet_id, &ref ) ) return;
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    rocketnet_assert( op && frame == op->frame );
    // Release callers may not use a frame or generation the transport did not issue.
    if ( !op || frame != op->frame ) return;
    (void) frame;
    op->packet_bytes = 0;
}

// Sets the socket's UDP_SEND_MSG_SIZE when it differs. Only the TX worker calls it.
static bool rocketnet_iocp_set_stride( rocketnet_server_iocp_t * iocp, int stride )
{
    if ( iocp->uso_stride == stride )
        return true;
    DWORD value = (DWORD) stride;
    if ( setsockopt( iocp->socket, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (const char*) &value, sizeof(value) ) == SOCKET_ERROR )
        return false;
    iocp->uso_stride = stride;
    return true;
}

// The per-datagram send: one frame, one WSASendTo.
static void rocketnet_iocp_post_send( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t & ref )
{
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    if ( !op ) return;
    op->run_count = 1;
    op->run_bytes = op->packet_bytes;
    op->run_ticket = 0;
    // A frame longer than the stride a USO run left on the socket would be
    // broken into segments: clear the stride first.
    if ( iocp->uso_stride != 0 && op->packet_bytes > iocp->uso_stride && !rocketnet_iocp_set_stride( iocp, 0 ) )
    {
        iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( 1, std::memory_order_relaxed );
        rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
        return;
    }
    memset( &op->overlapped, 0, sizeof(op->overlapped) );
    op->buffer.buf = (char*) op->frame + ROCKETNET_FRAME_WIRE_OFFSET;
    op->buffer.len = (ULONG) op->packet_bytes;
    op->completion_ref = ref;
    op->state = ROCKETNET_IOCP_OPERATION_SEND_POSTED;
    iocp->pending.fetch_add( 1, std::memory_order_relaxed );
    int error = 0;
    int result = SOCKET_ERROR;
    if ( !ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, &error ) )
    {
        result = WSASendTo( iocp->socket, &op->buffer, 1, NULL, 0,
            (sockaddr*) &op->address, op->address_bytes, &op->overlapped, NULL );
        if ( result == SOCKET_ERROR )
            error = WSAGetLastError();
    }
    if ( result == SOCKET_ERROR && error != WSA_IO_PENDING )
    {
        // An unsuccessful submission has no completion. All accepted
        // submissions, including immediate success, retire in the worker.
        iocp->pending.fetch_sub( 1, std::memory_order_release );
        iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( 1, std::memory_order_relaxed );
        rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_SEND_POSTED );
    }
}

// A USO run: one overlapped WSASendMsg with one WSABUF per frame, the socket's
// UDP_SEND_MSG_SIZE set to the run's stride before the post. The Winsock
// reference documents the segment size as a socket option only, not as a
// per-send control message, so the stride is the option and a run is one
// stride (the shared run rule's shorter tail is the documented shorter last
// segment). WSASendMsg captures the WSABUF array before it returns (WSASendMsg
// reference), so the array lives on this stack; the WSAMSG lives in the run's
// first frame. Returns false when the host refused USO live: the feature is
// cleared, counted, and the caller sends the run frame by frame.
static bool rocketnet_iocp_post_send_run( rocketnet_server_iocp_t * iocp, const rocketnet_iocp_ref_t * staged, int run )
{
    rocketnet_iocp_operation_t * first = rocketnet_iocp_send_from_ref( iocp, staged[0], ROCKETNET_IOCP_OPERATION_STAGED );
    if ( !first ) return false;
    WSABUF buffers[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
    int bytes = 0;
    for ( int k = 0; k < run; ++k )
    {
        rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, staged[k], ROCKETNET_IOCP_OPERATION_STAGED );
        if ( !op ) return false;
        buffers[k].buf = (char*) op->frame + ROCKETNET_FRAME_WIRE_OFFSET;
        buffers[k].len = (ULONG) op->packet_bytes;
        bytes += op->packet_bytes;
    }
    uint64_t run_ticket = 0;
    if ( !rocketnet_iocp_allocate_run( iocp, staged, run, &run_ticket ) )
        return false; // bounded run storage is busy; send frames individually
    for ( int k = 0; k < run; ++k )
    {
        rocketnet_iocp_operation_t * op = &iocp->sends[staged[k].index];
        op->state = ROCKETNET_IOCP_OPERATION_SEND_POSTED;
    }
    first->run_count = run;
    first->run_bytes = bytes;
    first->run_ticket = run_ticket;
    first->completion_ref = staged[0];
    int error = 0;
    if ( rocketnet_iocp_set_stride( iocp, first->packet_bytes ) )
    {
        memset( &first->overlapped, 0, sizeof(first->overlapped) );
        memset( &first->message, 0, sizeof(first->message) );
        first->message.name = (LPSOCKADDR) &first->address;
        first->message.namelen = first->address_bytes;
        first->message.lpBuffers = buffers;
        first->message.dwBufferCount = (ULONG) run;
        iocp->pending.fetch_add( 1, std::memory_order_relaxed );
        int result = SOCKET_ERROR;
        if ( !ROCKETNET_IOCP_TEST_FAULT( iocp, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, &error ) )
        {
            result = WSASendMsg( iocp->socket, &first->message, 0, NULL, &first->overlapped, NULL );
            if ( result == 0 )
            {
                iocp->counters[ROCKETNET_IOCP_USO_SENDS].fetch_add( 1, std::memory_order_relaxed );
                return true;
            }
            error = WSAGetLastError();
        }
        if ( error == WSA_IO_PENDING )
        {
            iocp->counters[ROCKETNET_IOCP_USO_SENDS].fetch_add( 1, std::memory_order_relaxed );
            return true;
        }
        // An unsuccessful submission has no completion.
        iocp->pending.fetch_sub( 1, std::memory_order_release );
    }
    else
        error = WSAEINVAL;                          // the host refused the stride itself

    if ( error == WSAEINVAL || error == WSAEOPNOTSUPP )
    {
        // As the GSO transport does for a refused live UDP_SEGMENT send:
        // degrade the tier and resend losslessly, never drop the run.
        rocketnet_warn( "iocp transport: live USO send refused (Winsock %d). degrading to WSASendTo per datagram", error );
        iocp->features.fetch_and( ~ROCKETNET_IOCP_FEATURE_USO, std::memory_order_relaxed );
        iocp->counters[ROCKETNET_IOCP_USO_FALLBACKS].fetch_add( 1, std::memory_order_relaxed );
        rocketnet_iocp_set_stride( iocp, 0 );
        rocketnet_iocp_ref_t refs[ROCKETNET_IOCP_USO_MAX_SEGMENTS];
        const bool released = rocketnet_iocp_take_run( iocp, run_ticket, run, refs );
        rocketnet_assert( released );
        for ( int k = 0; released && k < run; ++k )
            iocp->sends[refs[k].index].state = ROCKETNET_IOCP_OPERATION_STAGED;
        return false;
    }
    iocp->counters[ROCKETNET_IOCP_SEND_FAILED].fetch_add( (uint64_t) run, std::memory_order_relaxed );
    const bool retired = rocketnet_iocp_retire_send( iocp, staged[0] );
    rocketnet_assert( retired );
    if ( !retired ) rocketnet_error( "IOCP refused USO post carried a stale run generation" );
    return true;
}

static void rocketnet_iocp_send_bank( rocketnet_server_iocp_t * iocp, rocketnet_iocp_send_bank_t * bank )
{
    // tla/RNBufferHandoff.tla Consume and WorkerExit keep sending after quit.
    const uint64_t count = (uint64_t) bank->count;
    if ( iocp->features.load( std::memory_order_relaxed ) & ROCKETNET_IOCP_FEATURE_USO )
    {
        for ( uint64_t i = 0; i < count; ++i )
        {
            iocp->run_bytes[i] = iocp->sends[bank->staged[i].index].packet_bytes;
            iocp->run_to[i] = iocp->sends[bank->staged[i].index].to;
        }
    }
    for ( uint64_t i = 0; i < count; ++i )
    {
        const rocketnet_iocp_ref_t ref = bank->staged[i];
        rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
        if ( !op ) continue;
        if ( op->packet_bytes == 0 )
        {
            rocketnet_iocp_release_send( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
            continue;
        }
        if ( iocp->features.load( std::memory_order_relaxed ) & ROCKETNET_IOCP_FEATURE_USO )
        {
            const int run = rocketnet_coalesce_send_run( &iocp->run_bytes[i], &iocp->run_to[i], (int) ( count - i ),
                ROCKETNET_IOCP_USO_MAX_SEGMENTS, ROCKETNET_IOCP_USO_MAX_SUPER_BYTES );
            if ( run > 1 && rocketnet_iocp_post_send_run( iocp, &bank->staged[i], run ) )
            {
                i += (uint64_t) ( run - 1 );
                continue;
            }
        }
        rocketnet_iocp_post_send( iocp, ref );
    }
    bank->count = 0;
}


// Publish an already finished producer with a constant-time ownership swap.
// A failed try leaves every frame in the caller-owned bank for the next call.
void rocketnet_server_iocp_send_packets( rocketnet_server_iocp_t * iocp )
{
    if ( iocp->stopping.load( std::memory_order_acquire ) ) return;
    uint64_t count = iocp->num_staged.load( std::memory_order_relaxed );
    if ( count == 0 ) return;
    if ( count > ROCKETNET_IOCP_SEND_QUEUE_SIZE ) count = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
    if ( !rocketnet_platform_mutex_try_acquire( &iocp->send_mutex ) ) return;
    if ( iocp->send_free_count == 0 )
    {
        rocketnet_platform_mutex_release( &iocp->send_mutex );
        return;
    }
    const int producer = iocp->send_producer;
    iocp->send_banks[producer].count = (int) count;
    iocp->send_producer = iocp->send_free[--iocp->send_free_count];
    iocp->send_ready[( iocp->send_ready_head + iocp->send_ready_count ) % ROCKETNET_IOCP_SEND_BANKS] = producer;
    iocp->send_ready_count++;
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
    iocp->test_pending_tx_banks.fetch_add( 1, std::memory_order_release );
#endif
    rocketnet_platform_mutex_release( &iocp->send_mutex );
    iocp->num_staged.store( 0, std::memory_order_relaxed );
    rocketnet_platform_wake_set( iocp->send_wake );
}

static void rocketnet_iocp_send_worker( void * argument )
{
    // tla/RNBufferHandoff.tla WorkerExit drains ready banks and the final producer.
    rocketnet_server_iocp_t * iocp = (rocketnet_server_iocp_t*) argument;
    for ( ;; )
    {
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
        for ( ;; )
        {
            while ( iocp->test_hold_tx.load( std::memory_order_acquire ) &&
                    !iocp->stopping.load( std::memory_order_acquire ) )
                rocketnet_platform_sleep( 0.001 );
            iocp->test_active_tx_batches.fetch_add( 1, std::memory_order_acq_rel );
            if ( !iocp->test_hold_tx.load( std::memory_order_acquire ) ||
                  iocp->stopping.load( std::memory_order_acquire ) ) break;
            iocp->test_active_tx_batches.fetch_sub( 1, std::memory_order_release );
        }
#endif
        rocketnet_platform_mutex_acquire( &iocp->send_mutex );
        int index = -1;
        if ( iocp->send_ready_count )
        {
            index = iocp->send_ready[iocp->send_ready_head];
            iocp->send_ready_head = ( iocp->send_ready_head + 1 ) % ROCKETNET_IOCP_SEND_BANKS;
            iocp->send_ready_count--;
        }
        const bool finish = index < 0 && iocp->stopping.load( std::memory_order_acquire );
        rocketnet_platform_mutex_release( &iocp->send_mutex );
        if ( index >= 0 )
        {
            rocketnet_iocp_send_bank( iocp, &iocp->send_banks[index] );
            rocketnet_platform_mutex_acquire( &iocp->send_mutex );
            iocp->send_free[iocp->send_free_count++] = index;
            rocketnet_platform_mutex_release( &iocp->send_mutex );
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
            iocp->test_pending_tx_banks.fetch_sub( 1, std::memory_order_release );
#endif
        }
        else if ( finish )
        {
            // Stop's release transferred the last unpublished producer. It
            // contains no kernel-owned sends; post those frames outside the
            // mutex before advertising that no later post is possible.
            rocketnet_iocp_send_bank( iocp, &iocp->send_banks[iocp->send_producer] );
        }
#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
        iocp->test_active_tx_batches.fetch_sub( 1, std::memory_order_release );
#endif
        if ( finish ) break;
        if ( index < 0 ) rocketnet_platform_wake_wait( iocp->send_wake, false, 0.05 );
    }
    iocp->send_exited.store( true, std::memory_order_release );
    for ( int i = 0; i < iocp->num_workers; ++i )
        PostQueuedCompletionStatus( iocp->port, 0, 0, NULL );
}

void rocketnet_server_iocp_receive_packets( rocketnet_server_iocp_t * iocp )
{
    iocp->process.num_packets = 0;
    for ( int q = 0; q < iocp->num_workers; ++q )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[q];
        // Never wait behind a worker or expose the bank it is filling.
        if ( !rocketnet_platform_mutex_try_acquire( &worker->mutex ) )
            continue;
        for ( int j = 0; j < worker->exposed_count; ++j )
        {
            rocketnet_assert( worker->free_count < ROCKETNET_IOCP_RECEIVE_BANKS );
            worker->free_banks[worker->free_count++] = worker->exposed_banks[j];
        }
        worker->exposed_count = 0;
        int write_index = iocp->process.num_packets;
        while ( worker->ready_count > 0 )
        {
            const int candidate = worker->ready_banks[worker->ready_head];
            const int remaining = ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS - iocp->process.num_packets;
            if ( worker->buffers[candidate].num_packets > remaining )
                break; // preserve the oldest complete bank for the next call
            worker->ready_head = ( worker->ready_head + 1 ) % ROCKETNET_IOCP_RECEIVE_BANKS;
            worker->ready_count--;
            worker->exposed_banks[worker->exposed_count++] = candidate;
            iocp->process.num_packets += worker->buffers[candidate].num_packets;
        }
        const int num_exposed = worker->exposed_count;
        int exposed[ROCKETNET_IOCP_RECEIVE_BANKS];
        for ( int j = 0; j < num_exposed; ++j )
            exposed[j] = worker->exposed_banks[j];
        // Reserve process capacity under this lock, then fill those slots
        // outside it. No other caller uses process concurrently.
        rocketnet_platform_mutex_release( &worker->mutex );
        for ( int j = 0; j < num_exposed; ++j )
        {
            rocketnet_iocp_receive_buffer_t * buffer = &worker->buffers[exposed[j]];
            for ( int i = 0; i < buffer->num_packets; ++i )
            {
                const int index = write_index++;
                iocp->process.from[index] = buffer->from[i];
                iocp->process.packet_bytes[index] = buffer->packet_bytes[i];
                iocp->process.packet_data[index] = buffer->data[i];
            }
        }
    }
}

rocketnet_server_socket_process_packets_t * rocketnet_server_iocp_process_packets( rocketnet_server_iocp_t * iocp )
{
    return &iocp->process;
}

void rocketnet_server_iocp_counters( rocketnet_server_iocp_t * iocp, uint64_t * counters )
{
    for ( int i = 0; i < ROCKETNET_IOCP_NUM_COUNTERS; ++i ) counters[i] = iocp->counters[i].load( std::memory_order_relaxed );
}

int rocketnet_server_iocp_pending( rocketnet_server_iocp_t * iocp )
{
    return iocp->pending.load( std::memory_order_acquire );
}

int rocketnet_server_iocp_features( rocketnet_server_iocp_t * iocp )
{
    return iocp->features.load( std::memory_order_relaxed );
}

#if ROCKETNET_DEV && defined(ROCKETNET_IOCP_TESTS)
uintptr_t rocketnet_server_iocp_test_socket( rocketnet_server_iocp_t * iocp )
{
    return (uintptr_t) iocp->socket;
}

void rocketnet_server_iocp_test_hold_tx( rocketnet_server_iocp_t * iocp, bool hold )
{
    iocp->test_hold_tx.store( hold, std::memory_order_release );
    if ( iocp->send_wake ) rocketnet_platform_wake_set( iocp->send_wake );
    if ( hold )
        while ( iocp->test_active_tx_batches.load( std::memory_order_acquire ) != 0 )
            rocketnet_platform_sleep( 0.001 );
}

void rocketnet_server_iocp_test_hold_tx_publication( rocketnet_server_iocp_t * iocp, bool hold )
{
    if ( hold ) rocketnet_platform_mutex_acquire( &iocp->send_mutex );
    else rocketnet_platform_mutex_release( &iocp->send_mutex );
}

bool rocketnet_server_iocp_test_tx_idle( rocketnet_server_iocp_t * iocp )
{
    // Idle: the producer is published (nothing staged) and every published
    // bank is serviced.
    return iocp->num_staged.load( std::memory_order_relaxed ) == 0 &&
           iocp->test_pending_tx_banks.load( std::memory_order_acquire ) == 0;
}

int rocketnet_server_iocp_test_ready_tx_banks( rocketnet_server_iocp_t * iocp )
{
    rocketnet_platform_mutex_acquire( &iocp->send_mutex );
    const int count = iocp->send_ready_count;
    rocketnet_platform_mutex_release( &iocp->send_mutex );
    return count;
}

// Only the dedicated test executable compiles this helper. The gate pauses
// completion work without making production work run under a publication
// mutex. A hold returns after all earlier batches have retired.
void rocketnet_server_iocp_test_hold_completions( rocketnet_server_iocp_t * iocp, bool hold )
{
    iocp->test_hold_completions.store( hold, std::memory_order_release );
    if ( hold )
        while ( iocp->test_active_completion_batches.load( std::memory_order_acquire ) != 0 )
            rocketnet_platform_sleep( 0.001 );
}

void rocketnet_server_iocp_test_hold_publication( rocketnet_server_iocp_t * iocp, bool hold )
{
    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        if ( hold ) rocketnet_platform_mutex_acquire( &iocp->workers[i].mutex );
        else rocketnet_platform_mutex_release( &iocp->workers[i].mutex );
    }
}

int rocketnet_server_iocp_test_ready_banks( rocketnet_server_iocp_t * iocp, int worker_index )
{
    rocketnet_assert( worker_index >= 0 && worker_index < iocp->num_workers );
    rocketnet_iocp_worker_t * worker = &iocp->workers[worker_index];
    rocketnet_platform_mutex_acquire( &worker->mutex );
    const int count = worker->ready_count;
    rocketnet_platform_mutex_release( &worker->mutex );
    return count;
}

bool rocketnet_server_iocp_test_receive_view_published( rocketnet_server_iocp_t * iocp, const uint8_t * view )
{
    bool published = false;
    const uintptr_t pointer = (uintptr_t) view;
    for ( int i = 0; i < iocp->num_workers; ++i )
    {
        rocketnet_iocp_worker_t * worker = &iocp->workers[i];
        rocketnet_platform_mutex_acquire( &worker->mutex );
        for ( int j = 0; j < ROCKETNET_IOCP_RECEIVE_BANKS; ++j )
        {
            const uintptr_t begin = (uintptr_t) worker->buffers[j].data;
            if ( pointer >= begin && pointer < begin + sizeof(worker->buffers[j].data) )
                for ( int k = 0; k < worker->exposed_count; ++k )
                    published |= j == worker->exposed_banks[k];
        }
        rocketnet_platform_mutex_release( &worker->mutex );
    }
    return published;
}

bool rocketnet_server_iocp_test_stopping( rocketnet_server_iocp_t * iocp )
{
    return iocp->stopping.load( std::memory_order_acquire );
}

int rocketnet_server_iocp_test_packet_bytes( rocketnet_server_iocp_t * iocp, uint64_t packet_id )
{
    rocketnet_iocp_ref_t ref;
    if ( !rocketnet_iocp_packet_ref( iocp, packet_id, &ref ) ) return -1;
    rocketnet_iocp_operation_t * op = rocketnet_iocp_send_from_ref( iocp, ref, ROCKETNET_IOCP_OPERATION_STAGED );
    return op ? op->packet_bytes : -1;
}

#if ROCKETNET_DEV
void rocketnet_server_iocp_test_queue_stats( rocketnet_server_iocp_t * iocp,
    rocketnet_server_iocp_test_queue_stats_t * stats )
{
    memset( stats, 0, sizeof(*stats) );
    stats->free_pushes = iocp->free_sends.pushes.load( std::memory_order_relaxed );
    stats->free_pops = iocp->free_sends.pops.load( std::memory_order_relaxed );
    stats->retry_pushes = iocp->retry_receives.pushes.load( std::memory_order_relaxed );
    stats->retry_pops = iocp->retry_receives.pops.load( std::memory_order_relaxed );
    stats->queue_probes = iocp->free_sends.probes.load( std::memory_order_relaxed ) +
                          iocp->retry_receives.probes.load( std::memory_order_relaxed );
    stats->free_count = (int) rocketnet_iocp_queue_count( &iocp->free_sends );
    stats->retry_count = (int) rocketnet_iocp_queue_count( &iocp->retry_receives );
    rocketnet_platform_mutex_acquire( &iocp->run_mutex );
    for ( int i = 0; i < ROCKETNET_IOCP_SEND_QUEUE_SIZE; ++i )
        stats->live_run_refs += iocp->run_refs[i].occupied ? 1 : 0;
    rocketnet_platform_mutex_release( &iocp->run_mutex );
}
#endif

void rocketnet_server_iocp_test_fail_next( rocketnet_server_iocp_t * iocp, int fault, int count, int status )
{
    rocketnet_assert( fault >= 0 && fault < ROCKETNET_IOCP_TEST_NUM_FAULTS && count >= 0 );
    iocp->test_fault_status[fault].store( status, std::memory_order_relaxed );
    iocp->test_faults[fault].store( count, std::memory_order_release );
}

int rocketnet_server_iocp_test_faults_left( rocketnet_server_iocp_t * iocp, int fault )
{
    return iocp->test_faults[fault].load( std::memory_order_acquire );
}
#endif

#endif // ROCKETNET_PLATFORM_WINDOWS

int rocketnet_server_iocp_windows_cpp_dummy = 0;
