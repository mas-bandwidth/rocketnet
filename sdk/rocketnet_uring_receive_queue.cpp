/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_uring_receive_queue.h"
#include "rocketnet_layers.h"
#include "rocketnet_memory.h"
#include "rocketnet_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <new>
#include <string.h>

// The machinery of the per-queue receive; rocketnet_uring_receive_queue.h has
// the shape. The completion handling, the arm and the bank need only the
// kernel's uapi header (the submission entry is prepared field by field, as
// liburing's io_uring_prep_recvmsg_multishot prepares it, and the buffer
// ring's entries are written as io_uring_buf_ring_add writes them), so the
// unit suite and the trace walk drive them on any Linux build; registering
// the buffer ring with a kernel ring needs liburing.

#if defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING && defined( __linux__ )
#include <liburing.h>
#define ROCKETNET_URING_RECEIVE_LIBURING 1
#endif // #if defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING && defined( __linux__ )

#include "rocketnet_uring_codec.h"

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT )
#define ROCKETNET_URING_RECEIVE_BUILT 1
#endif // #if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT )

#if defined( ROCKETNET_URING_RECEIVE_BUILT )

#include "rocketnet_coalesce.h"
#include "rocketnet_platform.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// the control area: a UDP_GRO cmsg (an int) when GRO is on, a SO_RXQ_OVFL
// cmsg (a uint32) when the socket counts its drops

#define ROCKETNET_URING_RECEIVE_GRO_CONTROL_BYTES                CMSG_SPACE( sizeof( int ) )
#define ROCKETNET_URING_RECEIVE_DROPS_CONTROL_BYTES              CMSG_SPACE( sizeof( uint32_t ) )

ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_RECEIVE_GRO_CONTROL_BYTES + ROCKETNET_URING_RECEIVE_DROPS_CONTROL_BYTES <= ROCKETNET_URING_RECEIVE_CONTROL_BYTES,
                         "the receive control area holds a UDP_GRO and a SO_RXQ_OVFL cmsg" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_RECEIVE_GRO_CONTROL_BYTES == ROCKETNET_URING_GRO_CONTROL_BYTES, "the codec's UDP_GRO control area" );

// the largest UDP payload: a coalesced receive is at most this
#define ROCKETNET_URING_RECEIVE_MAX_PAYLOAD                                  65535

static int rocketnet_uring_receive_control_bytes( bool gro, bool socket_drops )
{
    return ( gro ? (int) ROCKETNET_URING_RECEIVE_GRO_CONTROL_BYTES : 0 ) + ( socket_drops ? (int) ROCKETNET_URING_RECEIVE_DROPS_CONTROL_BYTES : 0 );
}

// the bytes a buffer must hold: the multishot header, the name area, the
// control area and a whole datagram, or with GRO a whole coalesced receive

static int rocketnet_uring_receive_min_buffer_bytes( bool gro, bool socket_drops )
{
    return (int) sizeof( struct io_uring_recvmsg_out ) + (int) sizeof( struct sockaddr_storage ) + rocketnet_uring_receive_control_bytes( gro, socket_drops ) +
           ( gro ? ROCKETNET_URING_RECEIVE_MAX_PAYLOAD : ROCKETNET_RECEIVE_CEILING );
}

static bool rocketnet_uring_receive_config_ok( const struct rocketnet_uring_receive_queue_config_t * config, bool log )
{
    if ( config->buffers < 1 || config->buffers > ROCKETNET_URING_RECEIVE_MAX_BUFFERS || ( config->buffers & ( config->buffers - 1 ) ) != 0 )
    {
        if ( log )
        {
            rocketnet_error( "io_uring receive queue %d: %d buffers is not a power of two in 1..%d", config->queue_index, config->buffers, ROCKETNET_URING_RECEIVE_MAX_BUFFERS );
        }
        return false;
    }

    const int min_bytes = rocketnet_uring_receive_min_buffer_bytes( config->gro, config->socket_drops );
    if ( config->buffer_bytes < min_bytes )
    {
        if ( log )
        {
            rocketnet_error( "io_uring receive queue %d: %d buffer bytes is under %d", config->queue_index, config->buffer_bytes, min_bytes );
        }
        return false;
    }

    if ( config->slots < 1 || config->slots > ROCKETNET_URING_RECEIVE_MAX_SLOTS )
    {
        if ( log )
        {
            rocketnet_error( "io_uring receive queue %d: %d slots is outside 1..%d", config->queue_index, config->slots, ROCKETNET_URING_RECEIVE_MAX_SLOTS );
        }
        return false;
    }

    if ( config->queue_index < 0 || config->queue_index > 0xFFFF || config->bgid < 0 || config->bgid > 0xFFFF )
    {
        if ( log )
        {
            rocketnet_error( "io_uring receive queue %d: queue index, buffer group %d out of range", config->queue_index, config->bgid );
        }
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------

// one allocation: the queue, the slot records, the slot frames (16-aligned),
// the provided buffers and, for a queue with no kernel ring, the buffer ring

struct rocketnet_uring_receive_queue_layout_t
{
    uint64_t slot;
    uint64_t slot_data;
    uint64_t buffer;
    uint64_t buf_ring;
    uint64_t bytes;
};

static bool rocketnet_uring_receive_queue_layout( const struct rocketnet_uring_receive_queue_config_t * config, bool fake_ring, rocketnet_uring_receive_queue_layout_t * layout )
{
    rocketnet_carve_t carve = { 0, false };
    rocketnet_carve( carve, 1, sizeof( struct rocketnet_uring_receive_queue_t ) );
    layout->slot = rocketnet_carve( carve, (uint64_t) config->slots, sizeof( struct rocketnet_uring_receive_slot_t ) );
    layout->slot_data = rocketnet_carve( carve, (uint64_t) config->slots, (uint64_t) ROCKETNET_FRAME_STRIDE, 16 );
    layout->buffer = rocketnet_carve( carve, (uint64_t) config->buffers, (uint64_t) config->buffer_bytes );
    layout->buf_ring = fake_ring ? rocketnet_carve( carve, (uint64_t) config->buffers, sizeof( struct io_uring_buf ) ) : 0;
    layout->bytes = carve.bytes;
    return !carve.failed && carve.bytes <= SIZE_MAX;
}

uint64_t rocketnet_uring_receive_queue_allocation_bytes( const struct rocketnet_uring_receive_queue_config_t * config )
{
    if ( !config || !rocketnet_uring_receive_config_ok( config, false ) )
    {
        return 0;
    }

    rocketnet_uring_receive_queue_layout_t layout;
    if ( !rocketnet_uring_receive_queue_layout( config, false, &layout ) )
    {
        return 0;
    }

    return rocketnet_memory_allocations( { layout.bytes } );
}

uint64_t rocketnet_uring_receive_queue_mapped_bytes( int buffers )
{
    if ( buffers < 1 || buffers > ROCKETNET_URING_RECEIVE_MAX_BUFFERS )
    {
        return 0;
    }

    const uint64_t page = (uint64_t) getpagesize();
    return ( (uint64_t) buffers * sizeof( struct io_uring_buf ) + page - 1 ) / page * page;
}

// ---------------------------------------------------------------------------

// the provided buffer ring. Ubuntu 24.04 kernels 6.8.0-136 through 6.8.0-148
// carry a broken backport in io_uring/kbuf.c: the resv check in
// io_register_pbuf_ring and io_unregister_pbuf_ring is inverted, so every
// correct registration (resv all zero) is refused with EINVAL and one with a
// nonzero resv is taken. When the plain form is refused with EINVAL, register
// once more with resv[0] = 1: a correct kernel refuses that too, so the retry
// only succeeds where the check is inverted, and the ring is the same ring
// either way. Unregister then has to carry the same nonzero resv. Either form
// maps the ring's entries from the OS outside the allocator, so setup adds
// those bytes to the direct allocation count and free removes them.

#if defined( ROCKETNET_URING_RECEIVE_LIBURING )

struct io_uring_buf_ring * rocketnet_uring_receive_buf_ring_setup( struct io_uring * ring, unsigned int entries, int bgid, bool * inverted_resv, int * error )
{
    *inverted_resv = false;

    struct io_uring_buf_ring * buf_ring = io_uring_setup_buf_ring( ring, entries, bgid, 0, error );
    if ( buf_ring )
    {
        rocketnet_direct_allocation_add( (uint64_t) entries * sizeof( struct io_uring_buf ) );
    }

    if ( buf_ring || *error != -EINVAL )
    {
        return buf_ring;
    }

    const size_t bytes = (size_t) entries * sizeof( struct io_uring_buf );
    void * memory = mmap( NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0 );
    if ( memory == MAP_FAILED )
    {
        return NULL;                                // keep the EINVAL of the plain form
    }

    struct io_uring_buf_reg reg;
    memset( &reg, 0, sizeof( reg ) );
    reg.ring_addr = (uint64_t) (uintptr_t) memory;
    reg.ring_entries = entries;
    reg.bgid = (uint16_t) bgid;
    reg.resv[0] = 1;

    if ( syscall( __NR_io_uring_register, ring->ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1 ) != 0 )
    {
        munmap( memory, bytes );
        return NULL;                                // a correct kernel: the EINVAL of the plain form stands
    }

    buf_ring = (struct io_uring_buf_ring *) memory;
    io_uring_buf_ring_init( buf_ring );

    rocketnet_direct_allocation_add( (uint64_t) bytes );

    *inverted_resv = true;
    *error = 0;

    return buf_ring;
}

void rocketnet_uring_receive_buf_ring_free( struct io_uring * ring, struct io_uring_buf_ring * buf_ring, unsigned int entries, int bgid, bool inverted_resv )
{
    rocketnet_direct_allocation_remove( (uint64_t) entries * sizeof( struct io_uring_buf ) );

    if ( !inverted_resv )
    {
        io_uring_free_buf_ring( ring, buf_ring, entries, bgid );
        return;
    }

    struct io_uring_buf_reg reg;
    memset( &reg, 0, sizeof( reg ) );
    reg.bgid = (uint16_t) bgid;
    reg.resv[0] = 1;

    syscall( __NR_io_uring_register, ring->ring_fd, IORING_UNREGISTER_PBUF_RING, &reg, 1 );

    munmap( buf_ring, (size_t) entries * sizeof( struct io_uring_buf ) );
}

#else // #if defined( ROCKETNET_URING_RECEIVE_LIBURING )

struct io_uring_buf_ring * rocketnet_uring_receive_buf_ring_setup( struct io_uring * ring, unsigned int entries, int bgid, bool * inverted_resv, int * error )
{
    (void) ring;
    (void) entries;
    (void) bgid;
    *inverted_resv = false;
    *error = -ENOSYS;
    return NULL;
}

void rocketnet_uring_receive_buf_ring_free( struct io_uring * ring, struct io_uring_buf_ring * buf_ring, unsigned int entries, int bgid, bool inverted_resv )
{
    (void) ring;
    (void) buf_ring;
    (void) entries;
    (void) bgid;
    (void) inverted_resv;
}

#endif // #if defined( ROCKETNET_URING_RECEIVE_LIBURING )

// a buffer back to the ring at the tail's entry, then the tail past it (a
// release store: the kernel reads the entry once it sees the tail), as
// io_uring_buf_ring_add and io_uring_buf_ring_advance write them. The
// entry's resv is the ring's tail for entry 0 and is never written here.
// proofs/tla/RNUringQueueReceiveLayout.tla Complete: Entry(brTail) := bid,
// brTail + 1 modulo the 16-bit counter

static void rocketnet_uring_receive_queue_give( struct rocketnet_uring_receive_queue_t * queue, int buffer_id )
{
    struct io_uring_buf * bufs = reinterpret_cast<struct io_uring_buf *>( queue->buf_ring );
    const uint16_t tail = queue->buf_ring->tail;
    struct io_uring_buf * entry = &bufs[tail & ( queue->buffers - 1 )];
    entry->addr = (uint64_t) (uintptr_t) ( queue->buffer + (size_t) buffer_id * (size_t) queue->buffer_bytes );
    entry->len = (uint32_t) queue->buffer_bytes;
    entry->bid = (uint16_t) buffer_id;
    __atomic_store_n( &queue->buf_ring->tail, (uint16_t) ( tail + 1 ), __ATOMIC_RELEASE );
}

// ---------------------------------------------------------------------------

struct rocketnet_uring_receive_queue_t * rocketnet_uring_receive_queue_create( void * context, struct io_uring * ring, const struct rocketnet_uring_receive_queue_config_t * config,
                                                                               bool * out_of_memory )
{
    rocketnet_assert( config );

    if ( out_of_memory )
    {
        *out_of_memory = false;
    }

    if ( !rocketnet_uring_receive_config_ok( config, true ) )
    {
        return NULL;
    }

#if !defined( ROCKETNET_URING_RECEIVE_LIBURING )
    if ( ring )
    {
        rocketnet_error( "io_uring receive queue %d: built without liburing, no buffer ring can be registered", config->queue_index );
        return NULL;
    }
#endif // #if !defined( ROCKETNET_URING_RECEIVE_LIBURING )

    rocketnet_uring_receive_queue_layout_t layout;
    if ( !rocketnet_uring_receive_queue_layout( config, ring == NULL, &layout ) )
    {
        rocketnet_error( "io_uring receive queue %d: the layout overflows", config->queue_index );
        return NULL;
    }

    uint8_t * base = (uint8_t *) rocketnet_malloc( context, (size_t) layout.bytes );
    if ( !base )
    {
        rocketnet_error( "io_uring receive queue %d: could not allocate %" PRIu64 " bytes", config->queue_index, layout.bytes );
        if ( out_of_memory )
        {
            *out_of_memory = true;
        }
        return NULL;
    }

    // the provided buffers and the slot frames are not written here: a page
    // of them is resident only once the kernel or the worker first writes it,
    // and nothing reads a byte that was not written

    memset( base, 0, (size_t) layout.slot_data );
    if ( ring == NULL )
    {
        memset( base + layout.buf_ring, 0, (size_t) config->buffers * sizeof( struct io_uring_buf ) );
    }

    struct rocketnet_uring_receive_queue_t * queue = new ( base ) rocketnet_uring_receive_queue_t();
    queue->context = context;
    queue->ring = ring;
    queue->queue_index = config->queue_index;
    queue->fd = config->fd;
    queue->bgid = config->bgid;
    queue->buffers = config->buffers;
    queue->buffer_bytes = config->buffer_bytes;
    queue->slots = config->slots;
    queue->ipv6 = config->ipv6;
    queue->socket_drops = config->socket_drops;
    queue->has_filter = config->packet_filter != NULL;
    if ( queue->has_filter )
    {
        memcpy( queue->packet_filter, config->packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
    }
    queue->slot = reinterpret_cast<struct rocketnet_uring_receive_slot_t *>( base + layout.slot );
    queue->slot_data = base + layout.slot_data;
    queue->buffer = base + layout.buffer;
    queue->memory = base;
    queue->tail.store( 0, std::memory_order_relaxed );
    queue->head.store( 0, std::memory_order_relaxed );
    queue->head_seen = 0;
    queue->exposed = 0;
    for ( int i = 0; i < ROCKETNET_URING_RECEIVE_NUM_COUNTERS; i++ )
    {
        queue->counters[i].store( 0, std::memory_order_relaxed );
    }
    rocketnet_receive_drop_tally_reset( &queue->tally );

    memset( &queue->receive_message, 0, sizeof( queue->receive_message ) );
    queue->receive_message.msg_namelen = sizeof( struct sockaddr_storage );
    queue->receive_message.msg_controllen = (size_t) rocketnet_uring_receive_control_bytes( config->gro, config->socket_drops );

    if ( ring == NULL )
    {
        queue->buf_ring = reinterpret_cast<struct io_uring_buf_ring *>( base + layout.buf_ring );
    }
    else
    {
        int error = 0;
        queue->buf_ring = rocketnet_uring_receive_buf_ring_setup( ring, (unsigned int) config->buffers, config->bgid, &queue->buf_ring_inverted_resv, &error );
        if ( !queue->buf_ring )
        {
            rocketnet_error( "io_uring receive queue %d: the buffer ring setup failed: %s", config->queue_index, rocketnet_platform_socket_error_text( -error ).text );
            queue->~rocketnet_uring_receive_queue_t();
            rocketnet_free( context, base );
            return NULL;
        }
    }

    // every buffer to the ring: buffer b at entry b, the tail at buffers
    // (proofs/tla/RNUringQueueReceiveLayout.tla Init)

    for ( int b = 0; b < config->buffers; b++ )
    {
        rocketnet_uring_receive_queue_give( queue, b );
    }

    return queue;
}

void rocketnet_uring_receive_queue_destroy( struct rocketnet_uring_receive_queue_t * queue )
{
    if ( !queue )
    {
        return;
    }

    void * context = queue->context;
    void * memory = queue->memory;

    if ( queue->ring )
    {
        rocketnet_uring_receive_buf_ring_free( queue->ring, queue->buf_ring, (unsigned int) queue->buffers, queue->bgid, queue->buf_ring_inverted_resv );
    }

    queue->~rocketnet_uring_receive_queue_t();

    rocketnet_free( context, memory );
}

// ---------------------------------------------------------------------------

// one writer per counter, the worker: no read-modify-write

static void rocketnet_uring_receive_queue_count( struct rocketnet_uring_receive_queue_t * queue, int counter, uint64_t value = 1 )
{
    std::atomic<uint64_t> & c = queue->counters[counter];
    c.store( c.load( std::memory_order_relaxed ) + value, std::memory_order_relaxed );
}

bool rocketnet_uring_receive_queue_needs_arm( const struct rocketnet_uring_receive_queue_t * queue )
{
    rocketnet_assert( queue );
    return !queue->armed && !queue->stopping;
}

bool rocketnet_uring_receive_queue_armed( const struct rocketnet_uring_receive_queue_t * queue )
{
    rocketnet_assert( queue );
    return queue->armed;
}

// proofs/tla/RNUringQueueReceive.tla Stop (proofs/tla/RNUringQueueReceiveLayout.tla
// Stop): the stop is seen by the worker, which cancels the multishot; a
// completion after it is never armed again

void rocketnet_uring_receive_queue_stop( struct rocketnet_uring_receive_queue_t * queue )
{
    rocketnet_assert( queue );
    queue->stopping = true;
}

// proofs/tla/RNUringQueueReceive.tla Arm (proofs/tla/RNUringQueueReceiveLayout.tla Arm):
// the multishot RECVMSG over the queue's buffer group, prepared as
// io_uring_prep_recvmsg_multishot prepares it

bool rocketnet_uring_receive_queue_arm( struct rocketnet_uring_receive_queue_t * queue, struct io_uring_sqe * sqe )
{
    rocketnet_assert( queue );
    rocketnet_assert( rocketnet_uring_receive_queue_needs_arm( queue ) );

    if ( !sqe )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_REARM_FAILED );
        return false;
    }

    memset( sqe, 0, sizeof( *sqe ) );
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = queue->fd;
    sqe->addr = (uint64_t) (uintptr_t) &queue->receive_message;
    sqe->len = 1;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->buf_group = (uint16_t) queue->bgid;
    sqe->user_data = rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue->queue_index, 0 );

    if ( queue->ever_armed )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_REARMS );
    }
    queue->ever_armed = true;
    queue->armed = true;
    return true;
}

// ---------------------------------------------------------------------------

// proofs/tla/RNUringQueueReceive.tla Write and Publish: the slot is written,
// then tail + 1 is published, as two steps.
// proofs/tla/RNUringQueueReceiveLayout.tla BankOne: one accepted datagram to
// position tail, slot tail % slots, while tail - head_seen < slots; head is
// loaded again (acquire: main has finished reading below it) only when
// head_seen says the bank is full. The slot is whole before tail + 1 is
// stored (release: main reads it once it sees the tail). No room: BANK_FULL.

static bool rocketnet_uring_receive_queue_bank( struct rocketnet_uring_receive_queue_t * queue, const struct rocketnet_address_t * from, const uint8_t * data, int bytes )
{
    const uint64_t tail = queue->tail.load( std::memory_order_relaxed );
    const uint64_t slots = (uint64_t) queue->slots;

    if ( tail - queue->head_seen >= slots )
    {
        queue->head_seen = queue->head.load( std::memory_order_acquire );
        if ( tail - queue->head_seen >= slots )
        {
            rocketnet_receive_drop_tally_count( &queue->tally, ROCKETNET_RECEIVE_DROP_BANK_FULL );
            return false;
        }
    }

    const uint64_t index = tail % slots;
    queue->slot[index].from = *from;
    queue->slot[index].packet_bytes = bytes;
    memcpy( queue->slot_data + (size_t) index * (size_t) ROCKETNET_FRAME_STRIDE, data, (size_t) bytes );

    queue->tail.store( tail + 1, std::memory_order_release );
    rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_BANKED );
    return true;
}

// the socket's drop count in a SO_RXQ_OVFL cmsg: the cmsg headers are read
// with memcpy, never cast. false when there is none, or the area is
// malformed (the GRO parse counts that fault).

static bool rocketnet_uring_receive_socket_drops( const uint8_t * control, int control_bytes, uint32_t * drops )
{
    int offset = 0;
    while ( control_bytes - offset >= (int) sizeof( struct cmsghdr ) )
    {
        struct cmsghdr header;
        memcpy( &header, control + offset, sizeof( header ) );
        if ( header.cmsg_len < sizeof( struct cmsghdr ) || header.cmsg_len > (size_t) ( control_bytes - offset ) )
        {
            return false;
        }

        if ( header.cmsg_level == SOL_SOCKET && header.cmsg_type == SO_RXQ_OVFL )
        {
            if ( header.cmsg_len < CMSG_LEN( sizeof( uint32_t ) ) )
            {
                return false;
            }

            memcpy( drops, control + offset + CMSG_LEN( 0 ), sizeof( *drops ) );
            return true;
        }

        offset += (int) CMSG_ALIGN( header.cmsg_len );
    }

    return false;
}

// the datagrams of one provided buffer, each banked or counted by name:
// the transport's refusals in the queue's counters, the prefilter's and the
// full bank's in its tally. Returns the datagrams banked.

static int rocketnet_uring_receive_queue_buffer( struct rocketnet_uring_receive_queue_t * queue, const uint8_t * buffer, int32_t res )
{
    const int msg_namelen = (int) queue->receive_message.msg_namelen;
    const int msg_controllen = (int) queue->receive_message.msg_controllen;

    // the socket's drops before this datagram was queued: counted whatever
    // becomes of the datagram, so long as the header and control area are
    // whole

    if ( queue->socket_drops && res >= (int) sizeof( struct io_uring_recvmsg_out ) + msg_namelen + msg_controllen )
    {
        struct io_uring_recvmsg_out out;
        memcpy( &out, buffer, sizeof( out ) );
        const int control_bytes = out.controllen < (uint32_t) msg_controllen ? (int) out.controllen : msg_controllen;
        uint32_t drops = 0;
        if ( ( out.flags & MSG_CTRUNC ) == 0 &&
             rocketnet_uring_receive_socket_drops( buffer + sizeof( out ) + msg_namelen, control_bytes, &drops ) && drops != queue->socket_drops_seen )
        {
            rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET, (uint32_t) ( drops - queue->socket_drops_seen ) );
            queue->socket_drops_seen = drops;
        }
    }

    const uint8_t * name = NULL;
    const uint8_t * payload = NULL;
    int name_bytes = 0;
    int payload_bytes = 0;
    int segment_bytes = 0;
    uint64_t gro_faults = 0;

    const int verdict = rocketnet_uring_recvmsg_parse( buffer, res, msg_namelen, msg_controllen, &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &gro_faults );

    if ( gro_faults > 0 )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_GRO_CMSG_FAULTS, gro_faults );
    }

    if ( verdict == ROCKETNET_URING_RECV_NAME_TRUNCATED )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE );
        return 0;
    }

    if ( verdict != ROCKETNET_URING_RECV_OK )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE );
        if ( verdict == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED )
        {
            rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE );
        }
        return 0;
    }

    struct sockaddr_storage source;
    memset( &source, 0, sizeof( source ) );
    memcpy( &source, name, (size_t) name_bytes );

    struct rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );
    if ( !rocketnet_uring_sockaddr_to_address( &source, name_bytes, queue->ipv6, &from ) )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE );
        return 0;
    }

    int offsets[ROCKETNET_URING_RECEIVE_MAX_SEGMENTS];
    int lengths[ROCKETNET_URING_RECEIVE_MAX_SEGMENTS];
    int beyond = 0;
    const int segments = rocketnet_coalesce_split_prefix( payload_bytes, segment_bytes, offsets, lengths, ROCKETNET_URING_RECEIVE_MAX_SEGMENTS, &beyond );
    if ( segments == 0 )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE );
        return 0;
    }

    if ( segments > 1 )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_GRO_BATCHES );
    }

    // a local sender's segmented send can coalesce more datagrams than one
    // receive is taken with: each one past them is counted

    if ( beyond > 0 )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_OVERFLOW, (uint64_t) beyond );
    }

    int banked = 0;
    for ( int i = 0; i < segments; i++ )
    {
        const uint8_t * data = payload + offsets[i];
        const int bytes = lengths[i];

        if ( bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || bytes > ROCKETNET_RECEIVE_CEILING )
        {
            rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE );
            if ( bytes > ROCKETNET_RECEIVE_CEILING )
            {
                rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE );
            }
            continue;
        }

        const enum rocketnet_receive_drop_t drop = queue->has_filter ? rocketnet_receive_prefilter( data, bytes, queue->packet_filter ) : ROCKETNET_RECEIVE_ACCEPT;
        if ( drop != ROCKETNET_RECEIVE_ACCEPT )
        {
#if ROCKETNET_DEV
            rocketnet_receive_drop_sample_record( (int) drop, &from, data, bytes );
#endif // #if ROCKETNET_DEV
            rocketnet_receive_drop_tally_count( &queue->tally, drop );
            continue;
        }

        if ( rocketnet_uring_receive_queue_bank( queue, &from, data, bytes ) )
        {
            banked++;
        }
    }

    return banked;
}

// proofs/tla/RNUringQueueReceive.tla Complete (RNUringQueueReceiveLayout
// Complete): a datagram completion's buffer is banked or counted, then given
// back to the ring after its last datagram; a completion without
// IORING_CQE_F_MORE ends the multishot, so the queue needs an arm

int rocketnet_uring_receive_queue_cqe( struct rocketnet_uring_receive_queue_t * queue, int32_t res, uint32_t flags )
{
    rocketnet_assert( queue );

    int banked = 0;

    if ( res >= 0 )
    {
        const int buffer_id = rocketnet_uring_buffer_id( flags );
        if ( buffer_id < 0 || buffer_id >= queue->buffers )
        {
            rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_NO_BUFFER );
        }
        else
        {
            banked = rocketnet_uring_receive_queue_buffer( queue, queue->buffer + (size_t) buffer_id * (size_t) queue->buffer_bytes, res );
            rocketnet_uring_receive_queue_give( queue, buffer_id );
        }
    }
    else if ( res == -ENOBUFS )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_NO_BUFFERS );
    }
    else if ( res != -ECANCELED )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_ERRORS );

        // a kernel that refuses multishot receive ends every arm this way and
        // the queue hears nothing: say so, once
        if ( !queue->logged_refused )
        {
            queue->logged_refused = true;
            rocketnet_error( "io_uring receive queue %d: the kernel refused the multishot receive (errno %d)", queue->queue_index, -res );
        }
    }

    if ( ( flags & IORING_CQE_F_MORE ) == 0 )
    {
        rocketnet_uring_receive_queue_count( queue, ROCKETNET_URING_RECEIVE_COUNTER_MULTISHOT_ENDED );
        queue->armed = false;
    }

    return banked;
}

// ---------------------------------------------------------------------------

// proofs/tla/RNUringQueueReceive.tla Drain (proofs/tla/RNUringQueueReceiveLayout.tla Drain):
// head := the end of the drain before (release: main has finished reading
// those slots), then the positions from there to the tail (acquire), at most
// max, from slot position % slots. ROCKETNET_URING_RECEIVE_QUEUE_PLANT is the
// trace check's plant (proofs/tla/README-TraceCheck.md): only the plant build
// of tla_trace defines it, and its traces must fail TraceConsistent. It
// releases the slots this drain hands main, so the worker may write them
// while main reads.

int rocketnet_uring_receive_queue_drain( struct rocketnet_uring_receive_queue_t * queue, int max, struct rocketnet_address_t * from, int * packet_bytes, uint8_t ** packet_data )
{
    rocketnet_assert( queue );
    rocketnet_assert( max >= 0 );
    rocketnet_assert( max == 0 || ( from && packet_bytes && packet_data ) );

    const uint64_t head = queue->exposed;
    const uint64_t tail = queue->tail.load( std::memory_order_acquire );
    const uint64_t ready = tail - head;
    const int n = ready < (uint64_t) max ? (int) ready : max;

#if defined( ROCKETNET_URING_RECEIVE_QUEUE_PLANT )
    queue->head.store( head + (uint64_t) n, std::memory_order_release );
#else  // #if defined( ROCKETNET_URING_RECEIVE_QUEUE_PLANT )
    queue->head.store( head, std::memory_order_release );
#endif // #if defined( ROCKETNET_URING_RECEIVE_QUEUE_PLANT )

    const uint64_t slots = (uint64_t) queue->slots;
    for ( int i = 0; i < n; i++ )
    {
        const uint64_t index = ( head + (uint64_t) i ) % slots;
        from[i] = queue->slot[index].from;
        packet_bytes[i] = queue->slot[index].packet_bytes;
        packet_data[i] = queue->slot_data + (size_t) index * (size_t) ROCKETNET_FRAME_STRIDE;
    }

    queue->exposed = head + (uint64_t) n;
    return n;
}

uint64_t rocketnet_uring_receive_queue_waiting( const struct rocketnet_uring_receive_queue_t * queue )
{
    rocketnet_assert( queue );
    return queue->tail.load( std::memory_order_acquire ) - queue->exposed;
}

void rocketnet_uring_receive_queue_counters( const struct rocketnet_uring_receive_queue_t * queue, uint64_t * counters )
{
    rocketnet_assert( queue );
    rocketnet_assert( counters );
    for ( int i = 0; i < ROCKETNET_URING_RECEIVE_NUM_COUNTERS; i++ )
    {
        counters[i] = queue->counters[i].load( std::memory_order_relaxed );
    }
}

void rocketnet_uring_receive_queue_drops( const struct rocketnet_uring_receive_queue_t * queue, uint64_t * totals )
{
    rocketnet_assert( queue );
    rocketnet_assert( totals );
    rocketnet_receive_drop_tally_sum( &queue->tally, 1, totals );
}

#if ROCKETNET_DEV

int rocketnet_uring_receive_queue_test_take( const struct rocketnet_uring_receive_queue_t * queue, uint16_t * head )
{
    rocketnet_assert( queue );
    rocketnet_assert( head );
    rocketnet_assert( queue->ring == NULL );

    if ( *head == __atomic_load_n( &queue->buf_ring->tail, __ATOMIC_ACQUIRE ) )
    {
        return -1;
    }

    const struct io_uring_buf * bufs = reinterpret_cast<const struct io_uring_buf *>( queue->buf_ring );
    const int buffer_id = (int) bufs[*head & ( queue->buffers - 1 )].bid;
    *head = (uint16_t) ( *head + 1 );
    return buffer_id;
}

// one cmsg at offset in the control area, when it fits: its header, then
// value_bytes of value

static int rocketnet_uring_receive_test_cmsg( uint8_t * control, int control_bytes, int offset, int level, int type, const void * value, size_t value_bytes )
{
    if ( offset + (int) CMSG_SPACE( value_bytes ) > control_bytes )
    {
        return offset;
    }

    struct cmsghdr header;
    memset( &header, 0, sizeof( header ) );
    header.cmsg_len = CMSG_LEN( value_bytes );
    header.cmsg_level = level;
    header.cmsg_type = type;
    memcpy( control + offset, &header, sizeof( header ) );
    memcpy( control + offset + CMSG_LEN( 0 ), value, value_bytes );
    return offset + (int) CMSG_SPACE( value_bytes );
}

int rocketnet_uring_receive_queue_test_write( struct rocketnet_uring_receive_queue_t * queue, int buffer_id, const void * name, int name_bytes, const uint8_t * payload,
                                              int payload_bytes, int segment_bytes, uint32_t socket_drops, uint32_t out_flags )
{
    rocketnet_assert( queue );
    rocketnet_assert( buffer_id >= 0 && buffer_id < queue->buffers );
    rocketnet_assert( name_bytes >= 0 && payload_bytes >= 0 );

    const int msg_namelen = (int) queue->receive_message.msg_namelen;
    const int msg_controllen = (int) queue->receive_message.msg_controllen;
    const int header_bytes = (int) sizeof( struct io_uring_recvmsg_out ) + msg_namelen + msg_controllen;
    rocketnet_assert( header_bytes + payload_bytes <= queue->buffer_bytes );

    uint8_t * buffer = queue->buffer + (size_t) buffer_id * (size_t) queue->buffer_bytes;
    uint8_t * control = buffer + sizeof( struct io_uring_recvmsg_out ) + msg_namelen;
    memset( buffer, 0, (size_t) header_bytes );

    int control_bytes = 0;
    if ( segment_bytes > 0 )
    {
        const int stride = segment_bytes;
        control_bytes = rocketnet_uring_receive_test_cmsg( control, msg_controllen, control_bytes, IPPROTO_UDP, UDP_GRO, &stride, sizeof( stride ) );
    }
    if ( socket_drops != 0 && queue->socket_drops )
    {
        control_bytes = rocketnet_uring_receive_test_cmsg( control, msg_controllen, control_bytes, SOL_SOCKET, SO_RXQ_OVFL, &socket_drops, sizeof( socket_drops ) );
    }

    const int copied_name = name_bytes < msg_namelen ? name_bytes : msg_namelen;
    if ( copied_name > 0 )
    {
        memcpy( buffer + sizeof( struct io_uring_recvmsg_out ), name, (size_t) copied_name );
    }

    struct io_uring_recvmsg_out out;
    memset( &out, 0, sizeof( out ) );
    out.namelen = (uint32_t) name_bytes;
    out.controllen = (uint32_t) control_bytes;
    out.payloadlen = (uint32_t) payload_bytes;
    out.flags = out_flags;
    memcpy( buffer, &out, sizeof( out ) );

    if ( payload_bytes > 0 )
    {
        memcpy( buffer + header_bytes, payload, (size_t) payload_bytes );
    }

    return header_bytes + payload_bytes;
}

#endif // #if ROCKETNET_DEV

#else // #if defined( ROCKETNET_URING_RECEIVE_BUILT )

// no io_uring uapi here: the functions exist, create refuses

struct rocketnet_uring_receive_queue_t * rocketnet_uring_receive_queue_create( void * context, struct io_uring * ring, const struct rocketnet_uring_receive_queue_config_t * config,
                                                                               bool * out_of_memory )
{
    (void) context;
    (void) ring;
    (void) config;
    if ( out_of_memory )
    {
        *out_of_memory = false;
    }
    rocketnet_error( "io_uring receive queue: not built on this platform" );
    return NULL;
}

void rocketnet_uring_receive_queue_destroy( struct rocketnet_uring_receive_queue_t * queue )
{
    (void) queue;
}

uint64_t rocketnet_uring_receive_queue_allocation_bytes( const struct rocketnet_uring_receive_queue_config_t * config )
{
    (void) config;
    return 0;
}

uint64_t rocketnet_uring_receive_queue_mapped_bytes( int buffers )
{
    (void) buffers;
    return 0;
}

bool rocketnet_uring_receive_queue_needs_arm( const struct rocketnet_uring_receive_queue_t * queue )
{
    (void) queue;
    return false;
}

bool rocketnet_uring_receive_queue_arm( struct rocketnet_uring_receive_queue_t * queue, struct io_uring_sqe * sqe )
{
    (void) queue;
    (void) sqe;
    return false;
}

int rocketnet_uring_receive_queue_cqe( struct rocketnet_uring_receive_queue_t * queue, int32_t res, uint32_t flags )
{
    (void) queue;
    (void) res;
    (void) flags;
    return 0;
}

void rocketnet_uring_receive_queue_stop( struct rocketnet_uring_receive_queue_t * queue )
{
    (void) queue;
}

bool rocketnet_uring_receive_queue_armed( const struct rocketnet_uring_receive_queue_t * queue )
{
    (void) queue;
    return false;
}

int rocketnet_uring_receive_queue_drain( struct rocketnet_uring_receive_queue_t * queue, int max, struct rocketnet_address_t * from, int * packet_bytes, uint8_t ** packet_data )
{
    (void) queue;
    (void) max;
    (void) from;
    (void) packet_bytes;
    (void) packet_data;
    return 0;
}

uint64_t rocketnet_uring_receive_queue_waiting( const struct rocketnet_uring_receive_queue_t * queue )
{
    (void) queue;
    return 0;
}

void rocketnet_uring_receive_queue_counters( const struct rocketnet_uring_receive_queue_t * queue, uint64_t * counters )
{
    (void) queue;
    for ( int i = 0; i < ROCKETNET_URING_RECEIVE_NUM_COUNTERS; i++ )
    {
        counters[i] = 0;
    }
}

void rocketnet_uring_receive_queue_drops( const struct rocketnet_uring_receive_queue_t * queue, uint64_t * totals )
{
    (void) queue;
    (void) totals;
}

struct io_uring_buf_ring * rocketnet_uring_receive_buf_ring_setup( struct io_uring * ring, unsigned int entries, int bgid, bool * inverted_resv, int * error )
{
    (void) ring;
    (void) entries;
    (void) bgid;
    *inverted_resv = false;
    *error = -ENOSYS;
    return NULL;
}

void rocketnet_uring_receive_buf_ring_free( struct io_uring * ring, struct io_uring_buf_ring * buf_ring, unsigned int entries, int bgid, bool inverted_resv )
{
    (void) ring;
    (void) buf_ring;
    (void) entries;
    (void) bgid;
    (void) inverted_resv;
}

#if ROCKETNET_DEV

int rocketnet_uring_receive_queue_test_take( const struct rocketnet_uring_receive_queue_t * queue, uint16_t * head )
{
    (void) queue;
    (void) head;
    return -1;
}

int rocketnet_uring_receive_queue_test_write( struct rocketnet_uring_receive_queue_t * queue, int buffer_id, const void * name, int name_bytes, const uint8_t * payload,
                                              int payload_bytes, int segment_bytes, uint32_t socket_drops, uint32_t out_flags )
{
    (void) queue;
    (void) buffer_id;
    (void) name;
    (void) name_bytes;
    (void) payload;
    (void) payload_bytes;
    (void) segment_bytes;
    (void) socket_drops;
    (void) out_flags;
    return 0;
}

#endif // #if ROCKETNET_DEV

#endif // #if defined( ROCKETNET_URING_RECEIVE_BUILT )
