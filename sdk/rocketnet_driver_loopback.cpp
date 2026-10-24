/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include <atomic>
#include "rocketnet_address.h"
#include "rocketnet_platform.h"
#include "rocketnet_memory.h"
#include "rocketnet_wire.h"
#include "rocketnet_client_protocol.h"

#include <string.h>

// The loopback transport carries datagrams between endpoints of the same
// process through memory, with no socket, no system network call and no
// thread of its own (tla/RNLoopbackWire.tla). Every endpoint owns an inbox of
// one-datagram slots, allocated at its create: ROCKETNET_LOOPBACK_QUEUE_PACKETS
// for a client, sized from the plan for a server
// (rocketnet_loopback_server_queue_packets). A send runs on the sender's thread (a client's send_packets, a
// server's send_packets), a receive on the receiver's (its update); the two
// are usually different threads in a multiplayer game.
//
// The inbox is an N-buffer handoff in the shape of
// sdk/rocketnet_buffer_handoff.h, its free and ready index queues sized at
// create: every slot is owned by exactly one of the free queue, one sender (reserved, being
// written), the ready queue (filled) and the receiver (taken, being read).
// The inbox mutex covers the index moves alone. A send reserves a free slot
// and appends it to the ready queue in one exchange, copies the datagram in
// outside the mutex, then marks its ready entry filled (release); the receive
// takes the filled prefix of the ready queue (acquire), copies out outside the
// mutex, then frees the slots. A send therefore makes its datagram visible
// before it returns, and the receiver's next update takes it: the same frame
// when the sender ran first. A full inbox drops the datagram and counts it.
//
// The inbox lives with the receiver, so a datagram survives its sender's
// destroy (tla/RNLoopbackWire.tla Leave: a disconnect burst still arrives). A sender pins the receiving
// endpoint before its exchange and checks after the pin that it is still the
// endpoint it routed to; destroy ends liveness under the table mutex and
// frees the inbox only once no pin is held.
//
// The process-wide endpoint table is guarded by one mutex, taken at create,
// destroy and a sender's first send to each address (its route, cached on
// the sender's own thread); the steady state takes only the inbox mutex.
//
// tla/RNLoopbackWire.tla PinAfterCheck and Return are the model's defects,
// which this code never does: no pin follows a liveness read it does not
// repeat, and no send returns before its slot is filled. Done is the model's
// stutter once every thread has finished.

enum
{
    ROCKETNET_LOOPBACK_FIRST_PORT = 49152,
    ROCKETNET_LOOPBACK_LAST_PORT = 65535,
    ROCKETNET_LOOPBACK_TAKE_BATCH = 64,         // slots one receive exchange takes
    ROCKETNET_LOOPBACK_ROUTES = 64              // a sender's route cache, by port; a miss is one table lookup
};

struct rocketnet_loopback_slot_t
{
    rocketnet_address_t from;
    int bytes;
    uint8_t data[ROCKETNET_RECEIVE_CEILING];
};

// a sender's cached route, by the destination's port and family: the table
// index plus one (0: none) and the generation the endpoint was live at; the
// destination's bind is checked against the address at every use
struct rocketnet_loopback_route_t
{
    uint32_t endpoint;
    uint32_t generation;
};

// one endpoint's allocation: its inbox, and its routes as a sender (its own
// thread only). The free stack, the ready ring and the slots follow the
// header in the same block, and create writes the header alone: a slot, and
// its entries in the stack and the ring, are first touched by the datagram
// that first needs them. Slots are handed out fresh only when the free stack
// is empty, freed slots are reused last-freed first, and the ready ring
// starts again at its front whenever it empties, so the slots and entries an
// endpoint touches are the most it ever had in flight at once, however long
// it runs (rocketnet_loopback_endpoint_slots_touched).
struct rocketnet_loopback_storage_t
{
    rocketnet_platform_mutex_t mutex;           // the inbox's index exchanges alone
    uint32_t capacity;                          // slots, a power of two
    uint32_t fresh;                             // slots ever handed out: 0 .. fresh-1 have been used
    uint32_t free_top;                          // the free stack's depth
    uint32_t ready_head, ready_count;           // the ready ring, oldest first
    uint32_t * free_stack;
    std::atomic<uint32_t> * ready;              // a slot index, with ROCKETNET_LOOPBACK_PENDING while its sender writes it
    rocketnet_loopback_slot_t * slots;
    rocketnet_loopback_route_t routes[ROCKETNET_LOOPBACK_ROUTES];
};

#define ROCKETNET_LOOPBACK_PENDING 0x80000000u

static size_t loopback_align( size_t bytes ) { return ( bytes + 63 ) & ~(size_t) 63; }

// tla/RNLoopbackWire.tla Reserve: a free slot, the one freed last, or a
// fresh one when none is free, appended to the ready ring as pending in the
// same exchange; -1 when every slot is in use. *position is its place in the
// ring, which its sender's Fill writes. Inbox mutex held.
static int loopback_reserve( rocketnet_loopback_storage_t * inbox, uint32_t * position )
{
    uint32_t index;
    if ( inbox->free_top > 0 )
        index = inbox->free_stack[--inbox->free_top];
    else if ( inbox->fresh < inbox->capacity )
        index = inbox->fresh++;
    else
        return -1;
    *position = ( inbox->ready_head + inbox->ready_count ) & ( inbox->capacity - 1 );
    inbox->ready[*position].store( index | ROCKETNET_LOOPBACK_PENDING, std::memory_order_relaxed );
    inbox->ready_count++;
    return (int) index;
}

// tla/RNLoopbackWire.tla Take: the oldest ready slot when its sender has
// filled it, or -1. An emptied ring starts again at its front. Inbox mutex
// held.
static int loopback_take_filled( rocketnet_loopback_storage_t * inbox )
{
    if ( inbox->ready_count == 0 )
        return -1;
    const uint32_t entry = inbox->ready[inbox->ready_head].load( std::memory_order_acquire );
    if ( entry & ROCKETNET_LOOPBACK_PENDING )
        return -1;
    inbox->ready_count--;
    inbox->ready_head = inbox->ready_count == 0 ? 0 : ( inbox->ready_head + 1 ) & ( inbox->capacity - 1 );
    return (int) entry;
}

// tla/RNLoopbackWire.tla Release: a slot onto the free stack, so the next
// reserve reuses the slot freed last. Inbox mutex held.
static void loopback_release( rocketnet_loopback_storage_t * inbox, int index )
{
    inbox->free_stack[inbox->free_top++] = (uint32_t) index;
}

struct rocketnet_loopback_endpoint_t
{
    // written at create under the table mutex before live is stored, read
    // by other threads only after they see live (under the table mutex, or
    // after a pin), and constant while live
    void * context;
    rocketnet_address_t bind;
    rocketnet_loopback_storage_t * storage;

    // read across threads
    std::atomic<bool> live;
    std::atomic<uint32_t> generation;           // moves at every create and destroy
    std::atomic<int> pins;                      // senders inside an exchange with this inbox
    std::atomic<uint64_t> dropped;              // datagrams for this endpoint the wire dropped
};

struct rocketnet_loopback_state_t
{
    rocketnet_platform_mutex_t mutex;
    rocketnet_loopback_endpoint_t endpoints[ROCKETNET_LOOPBACK_MAX_ENDPOINTS];
    std::atomic<uint64_t> send_failures[ROCKETNET_LOOPBACK_MAX_ENDPOINTS];
    // the live endpoint bound on each (family, port), as its table index plus
    // one: create refuses a second endpoint of a family on a port, so a
    // datagram's destination is one lookup, not a walk of the table
    uint16_t bound[2][65536];
    uint16_t next_port;
};

ROCKETNET_STATIC_ASSERT( ROCKETNET_LOOPBACK_MAX_ENDPOINTS < 65535, "an endpoint's table index plus one fits the bound table" );

static uint16_t * loopback_bound_entry( rocketnet_loopback_state_t & state, int type, uint16_t port )
{
    return &state.bound[type == ROCKETNET_ADDRESS_IPV6 ? 1 : 0][port];
}

struct rocketnet_loopback_state_init_t
{
    rocketnet_loopback_state_t state;
    rocketnet_loopback_state_init_t()
    {
        rocketnet_platform_mutex_create( &state.mutex );
        state.next_port = ROCKETNET_LOOPBACK_FIRST_PORT;
        memset( state.bound, 0, sizeof( state.bound ) );
        for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
        {
            rocketnet_loopback_endpoint_t & e = state.endpoints[i];
            e.context = NULL;
            memset( &e.bind, 0, sizeof( e.bind ) );
            e.storage = NULL;
            e.live.store( false );
            e.generation.store( 0 );
            e.pins.store( 0 );
            e.dropped.store( 0 );
            state.send_failures[i].store( 0 );
        }
    }
};

static rocketnet_loopback_state_t & loopback_state()
{
    static rocketnet_loopback_state_init_t init;
    return init.state;
}

static int loopback_index( const rocketnet_loopback_endpoint_t * e )
{
    return (int) ( e - loopback_state().endpoints );
}

#if defined(ROCKETNET_LOOPBACK_TEST_HOOKS)
static bool loopback_test_send_contention = false;
void rocketnet_loopback_test_send_contention( bool busy ) { loopback_test_send_contention = busy; }
#endif

// the shutdown send path never waits for a mutex: it drops on contention
static bool loopback_lock( rocketnet_platform_mutex_t * mutex, bool shutdown )
{
    if ( !shutdown )
    {
        rocketnet_platform_mutex_acquire( mutex );
        return true;
    }
#if defined(ROCKETNET_LOOPBACK_TEST_HOOKS)
    if ( loopback_test_send_contention )
        return false;
#endif
    return rocketnet_platform_mutex_try_acquire( mutex );
}

static void loopback_send_failed( rocketnet_loopback_endpoint_t * sender, uint64_t count )
{
    loopback_state().send_failures[loopback_index( sender )].fetch_add( count, std::memory_order_relaxed );
}

static uint64_t loopback_send_failures( void * instance )
{
    return loopback_state().send_failures[loopback_index( (rocketnet_loopback_endpoint_t*) instance )].load( std::memory_order_relaxed );
}

static bool loopback_is_wildcard( const rocketnet_address_t & address )
{
    if ( address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        for ( int i = 0; i < 4; ++i )
            if ( address.data.ipv4[i] )
                return false;
        return true;
    }
    if ( address.type == ROCKETNET_ADDRESS_IPV6 )
    {
        for ( int i = 0; i < 8; ++i )
            if ( address.data.ipv6[i] )
                return false;
        return true;
    }
    return false;
}

// the source address a datagram from this endpoint carries. a wildcard bind
// sends from its loopback. an endpoint bound to the IPv6 wildcard is dual
// stack, like a socket with IPV6_V6ONLY off: it serves IPv4 clients too, and
// its datagrams to an IPv4 client come from the IPv4 loopback on its port, so
// the IPv4 client sees the reply from the address it sent to.

static rocketnet_address_t loopback_sender_address( const rocketnet_address_t & bind, const rocketnet_address_t & to )
{
    rocketnet_address_t from = bind;
    if ( !loopback_is_wildcard( bind ) )
        return from;
    memset( &from.data, 0, sizeof( from.data ) );
    if ( bind.type == ROCKETNET_ADDRESS_IPV4 || to.type == ROCKETNET_ADDRESS_IPV4 )
    {
        from.type = ROCKETNET_ADDRESS_IPV4;
        from.data.ipv4[0] = 127;
        from.data.ipv4[3] = 1;
    }
    else
    {
        from.data.ipv6[7] = 1;
    }
    return from;
}

// true when a datagram to this address reaches an endpoint bound here: bound
// to the address, or to the wildcard of its family, on its port; an IPv4
// datagram also reaches a dual-stack endpoint bound to the IPv6 wildcard

static bool loopback_reaches( const rocketnet_address_t & bind, const rocketnet_address_t & to )
{
    if ( bind.port != to.port )
        return false;
    if ( bind.type == to.type )
        return loopback_is_wildcard( bind ) || rocketnet_address_equal( &bind, &to );
    return to.type == ROCKETNET_ADDRESS_IPV4 && bind.type == ROCKETNET_ADDRESS_IPV6 && loopback_is_wildcard( bind );
}

// the endpoint a datagram reaches: one bound to this address, or to the
// wildcard of this family, on this port; an IPv4 datagram also reaches a
// dual-stack endpoint bound to the IPv6 wildcard on that port. Create refuses
// a second endpoint of one family on one port, so each is one lookup in
// state.bound. Table mutex held.

static rocketnet_loopback_endpoint_t * loopback_find_destination( rocketnet_loopback_state_t & state, const rocketnet_address_t & to )
{
    if ( to.type != ROCKETNET_ADDRESS_IPV4 && to.type != ROCKETNET_ADDRESS_IPV6 )
        return NULL;
    const uint16_t same = *loopback_bound_entry( state, to.type, to.port );
    if ( same && state.endpoints[same - 1].live.load( std::memory_order_relaxed ) && loopback_reaches( state.endpoints[same - 1].bind, to ) )
        return &state.endpoints[same - 1];
    if ( to.type == ROCKETNET_ADDRESS_IPV4 )
    {
        const uint16_t dual = *loopback_bound_entry( state, ROCKETNET_ADDRESS_IPV6, to.port );
        if ( dual && state.endpoints[dual - 1].live.load( std::memory_order_relaxed ) && loopback_reaches( state.endpoints[dual - 1].bind, to ) )
            return &state.endpoints[dual - 1];
    }
    return NULL;
}

size_t rocketnet_loopback_queue_bytes( int packets )
{
    const size_t n = (size_t) packets;
    return loopback_align( sizeof( rocketnet_loopback_storage_t ) ) + loopback_align( n * sizeof( uint32_t ) ) +
           loopback_align( n * sizeof( std::atomic<uint32_t> ) ) + n * sizeof( rocketnet_loopback_slot_t );
}

// The server endpoint's queue, from the plan: every datagram one server
// update may read, ROCKETNET_RECEIVE_PACKETS_PER_CLIENT for each of the
// configured clients, rounded up to a power of two (16384 at 1000 clients),
// and never less than a client endpoint's. A whole population's disconnect
// bursts, ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS from each client, fit
// beside one update's ordinary traffic, and one update reads them all.
int rocketnet_loopback_server_queue_packets( int max_clients )
{
    const uint64_t wanted = (uint64_t) ( max_clients > 0 ? max_clients : 1 ) * (uint64_t) ROCKETNET_RECEIVE_PACKETS_PER_CLIENT;
    uint64_t packets = ROCKETNET_LOOPBACK_QUEUE_PACKETS;
    while ( packets < wanted )
        packets *= 2;
    return (int) packets;
}

ROCKETNET_STATIC_ASSERT( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT >= ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS + 2,
                         "a server loopback queue sized from the plan holds every client's disconnect burst beside two packets of ordinary traffic each" );

// ------------------------------------------------------------------------
// create and destroy

static void * loopback_create( void * context, rocketnet_address_t * bind, int queue_packets, int * status )
{
    if ( bind->type != ROCKETNET_ADDRESS_IPV4 && bind->type != ROCKETNET_ADDRESS_IPV6 )
        return NULL;

    // the storage is allocated before the table is touched, and freed if the
    // table refuses. All of it is allocated here; traffic never allocates.
    rocketnet_assert( queue_packets > 0 && ( queue_packets & ( queue_packets - 1 ) ) == 0 );
    uint8_t * block = (uint8_t*) rocketnet_malloc( context, rocketnet_loopback_queue_bytes( queue_packets ) );
    if ( !block )
        return NULL;
    rocketnet_loopback_storage_t * storage = (rocketnet_loopback_storage_t*) block;
    const size_t n = (size_t) queue_packets;
    uint8_t * p = block + loopback_align( sizeof( rocketnet_loopback_storage_t ) );
    storage->free_stack = (uint32_t*) p;
    p += loopback_align( n * sizeof( uint32_t ) );
    storage->ready = (std::atomic<uint32_t>*) p;
    p += loopback_align( n * sizeof( std::atomic<uint32_t> ) );
    storage->slots = (rocketnet_loopback_slot_t*) p;
    if ( !rocketnet_platform_mutex_create( &storage->mutex ) )
    {
        rocketnet_free( context, storage );
        // a refused mutex ends the create with MUTEX_CREATE_FAILED, as the
        // transport's own mutex does
        if ( status ) *status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;
        return NULL;
    }
    // no slot is held and none has been used: every one is fresh
    storage->capacity = (uint32_t) queue_packets;
    storage->fresh = 0;
    storage->free_top = 0;
    storage->ready_head = 0;
    storage->ready_count = 0;
    memset( storage->routes, 0, sizeof( storage->routes ) );

    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_acquire( &state.mutex );

    bool refused = false;
    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS && !refused; ++i )
    {
        rocketnet_loopback_endpoint_t & e = state.endpoints[i];
        if ( e.live.load( std::memory_order_relaxed ) && e.bind.port == bind->port && e.bind.type == bind->type )
            refused = true;
    }

    if ( !refused && bind->port == 0 )
    {
        uint16_t port = state.next_port;
        for ( int tries = 0; tries <= 65535; ++tries )
        {
            bool held = false;
            for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
            {
                if ( state.endpoints[i].live.load( std::memory_order_relaxed ) && state.endpoints[i].bind.port == port )
                {
                    held = true;
                    break;
                }
            }
            if ( !held )
                break;
            port = port == ROCKETNET_LOOPBACK_LAST_PORT ? (uint16_t) ROCKETNET_LOOPBACK_FIRST_PORT
                                                        : (uint16_t) ( port + 1 );
        }
        bind->port = port;
        state.next_port = port == ROCKETNET_LOOPBACK_LAST_PORT ? (uint16_t) ROCKETNET_LOOPBACK_FIRST_PORT
                                                               : (uint16_t) ( port + 1 );
    }

    // a slot whose destroy still waits for a pin to drop is not free
    rocketnet_loopback_endpoint_t * ep = NULL;
    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS && !refused && !ep; ++i )
        if ( !state.endpoints[i].live.load( std::memory_order_relaxed ) && !state.endpoints[i].storage )
            ep = &state.endpoints[i];
    if ( !refused && !ep )
        rocketnet_warn( "loopback transport refused a create: ROCKETNET_LOOPBACK_MAX_ENDPOINTS is %d",
                        (int) ROCKETNET_LOOPBACK_MAX_ENDPOINTS );
    if ( !ep )
    {
        rocketnet_platform_mutex_release( &state.mutex );
        rocketnet_platform_mutex_destroy( &storage->mutex );
        rocketnet_free( context, storage );
        return NULL;
    }

    ep->context = context;
    ep->bind = *bind;
    ep->storage = storage;
    ep->dropped.store( 0 );
    state.send_failures[loopback_index( ep )].store( 0, std::memory_order_relaxed );
    ep->generation.fetch_add( 1 );
    ep->live.store( true );
    *loopback_bound_entry( state, bind->type, bind->port ) = (uint16_t) ( loopback_index( ep ) + 1 );
    rocketnet_platform_mutex_release( &state.mutex );
    return ep;
}

static void * loopback_server_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    const int max_clients = config && config->transport ? config->transport->max_clients : ROCKETNET_MAX_CLIENTS;
    return loopback_create( context, bind, rocketnet_loopback_server_queue_packets( max_clients ), config ? config->status : NULL );
}

static void * loopback_client_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    return loopback_create( context, bind, ROCKETNET_LOOPBACK_QUEUE_PACKETS, config ? config->status : NULL );
}

// tla/RNLoopbackWire.tla Destroy and Free: liveness ends under the table mutex and
// the generation moves, so every route to this endpoint goes stale; the
// inbox is freed once no sender holds a pin on it. A pin lasts one bounded
// exchange and copy, so the wait is short; the table slot stays taken until
// the free.

static void loopback_destroy( void * transport )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t*) transport;
    rocketnet_loopback_state_t & state = loopback_state();

    rocketnet_platform_mutex_acquire( &state.mutex );
    ep->live.store( false );
    ep->generation.fetch_add( 1 );
    uint16_t * bound = loopback_bound_entry( state, ep->bind.type, ep->bind.port );
    if ( *bound == (uint16_t) ( loopback_index( ep ) + 1 ) )
        *bound = 0;
    rocketnet_platform_mutex_release( &state.mutex );

    while ( ep->pins.load() != 0 )
        rocketnet_platform_sleep( 0.0001 );

    rocketnet_platform_mutex_acquire( &state.mutex );
    void * context = ep->context;
    rocketnet_loopback_storage_t * storage = ep->storage;
    ep->storage = NULL;
    ep->context = NULL;
    rocketnet_platform_mutex_release( &state.mutex );

    rocketnet_platform_mutex_destroy( &storage->mutex );
    rocketnet_free( context, storage );
}

static void loopback_stop( void * ) {}

static bool loopback_stopped( void * )
{
    return true;
}

// ------------------------------------------------------------------------
// send: loopback_deliver, on the sender's thread

// tla/RNLoopbackWire.tla Pin and Verify: the pin comes first, then the check, so a
// destroy either sees the pin or this thread sees the endpoint is gone (both
// sequentially consistent).

static void loopback_pin( rocketnet_loopback_endpoint_t * e ) { e->pins.fetch_add( 1 ); }
static void loopback_unpin( rocketnet_loopback_endpoint_t * e ) { e->pins.fetch_sub( 1 ); }

// The endpoint a datagram to this address reaches, pinned, or NULL: the
// sender's cached route when it still holds, else the table, under its
// mutex, and the route cached. The cache is the sender's own.

static rocketnet_loopback_endpoint_t * loopback_route( rocketnet_loopback_endpoint_t * sender, const rocketnet_address_t & to, bool shutdown )
{
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_loopback_route_t * route = &sender->storage->routes[( to.port + ( to.type == ROCKETNET_ADDRESS_IPV6 ? 1u : 0u ) ) % ROCKETNET_LOOPBACK_ROUTES];
    if ( route->endpoint )
    {
        rocketnet_loopback_endpoint_t * e = &state.endpoints[route->endpoint - 1];
        loopback_pin( e );
        if ( e->live.load() && e->generation.load() == route->generation && loopback_reaches( e->bind, to ) )
            return e;
        loopback_unpin( e );
    }

    if ( !loopback_lock( &state.mutex, shutdown ) )
        return NULL;
    rocketnet_loopback_endpoint_t * e = loopback_find_destination( state, to );
    if ( e )
    {
        // live under the table mutex: its destroy will wait for this pin
        loopback_pin( e );
        route->endpoint = (uint32_t) loopback_index( e ) + 1;
        route->generation = e->generation.load();
    }
    rocketnet_platform_mutex_release( &state.mutex );
    return e;
}

// tla/RNLoopbackWire.tla Reserve and Fill: one exchange reserves a free slot at the
// tail of the ready queue; the datagram is written outside the mutex and the
// slot marked filled before the call returns (SentIsVisible). No free slot,
// or a contended shutdown exchange: false, dropped.

static bool loopback_deliver( rocketnet_loopback_endpoint_t * sender, const rocketnet_address_t & to, const uint8_t * data, int bytes, bool shutdown )
{
    rocketnet_loopback_endpoint_t * dst = loopback_route( sender, to, shutdown );
    if ( !dst )
        return false;
    rocketnet_loopback_storage_t * inbox = dst->storage;
    int index = -1;
    uint32_t position = 0;
    if ( loopback_lock( &inbox->mutex, shutdown ) )
    {
        index = loopback_reserve( inbox, &position );
        rocketnet_platform_mutex_release( &inbox->mutex );
    }
    if ( index < 0 )
    {
        dst->dropped.fetch_add( 1, std::memory_order_relaxed );
        loopback_unpin( dst );
        return false;
    }
    rocketnet_loopback_slot_t * slot = &inbox->slots[index];
    slot->from = loopback_sender_address( sender->bind, to );
    slot->bytes = bytes;
    memcpy( slot->data, data, (size_t) bytes );
    inbox->ready[position].store( (uint32_t) index, std::memory_order_release );
    loopback_unpin( dst );
    return true;
}

static void loopback_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    if ( bytes <= 0 || bytes > ROCKETNET_RECEIVE_CEILING )
        return;
    rocketnet_loopback_endpoint_t * sender = (rocketnet_loopback_endpoint_t*) transport;
    if ( !loopback_deliver( sender, *to, data, bytes, false ) )
        loopback_send_failed( sender, 1 );
}

static void loopback_server_send_batch_impl( void * transport, const rocketnet_platform_send_t * packets, int count, bool shutdown )
{
    rocketnet_loopback_endpoint_t * sender = (rocketnet_loopback_endpoint_t*) transport;
    for ( int i = 0; i < count; ++i )
    {
        const rocketnet_platform_send_t & packet = packets[i];
        if ( packet.packet_bytes <= 0 || packet.packet_bytes > ROCKETNET_RECEIVE_CEILING )
            continue;
        if ( !loopback_deliver( sender, *packet.to, packet.packet_data, packet.packet_bytes, shutdown ) )
            loopback_send_failed( sender, 1 );
    }
}

static void loopback_server_send_batch( void * transport, const rocketnet_platform_send_t * packets, int count )
{
    loopback_server_send_batch_impl( transport, packets, count, false );
}

static void loopback_server_send_batch_shutdown( void * transport, const rocketnet_platform_send_t * packets, int count )
{
    loopback_server_send_batch_impl( transport, packets, count, true );
}

// ------------------------------------------------------------------------
// receive: loopback_take, on the receiver's thread, from its own inbox

// tla/RNLoopbackWire.tla Take, Copy and Release: take the filled prefix of the
// ready queue, up to a batch, copy it out outside the mutex, clear the
// flags, then free the slots in one exchange. A datagram larger than the
// stride is dropped and counted. Returns the datagrams copied; *taken is
// the slots taken.

static int loopback_take( rocketnet_loopback_endpoint_t * receiver, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count, int * taken )
{
    rocketnet_loopback_storage_t * inbox = receiver->storage;
    int indices[ROCKETNET_LOOPBACK_TAKE_BATCH];
    int num_taken = 0;
    const int most = count < ROCKETNET_LOOPBACK_TAKE_BATCH ? count : ROCKETNET_LOOPBACK_TAKE_BATCH;
    rocketnet_platform_mutex_acquire( &inbox->mutex );
    while ( num_taken < most )
    {
        const int index = loopback_take_filled( inbox );
        if ( index < 0 )
            break;
        indices[num_taken++] = index;
    }
    rocketnet_platform_mutex_release( &inbox->mutex );

    int copied = 0;
    for ( int i = 0; i < num_taken; ++i )
    {
        const rocketnet_loopback_slot_t * slot = &inbox->slots[indices[i]];
        if ( stride <= 0 || slot->bytes <= 0 || slot->bytes > stride )
            receiver->dropped.fetch_add( 1, std::memory_order_relaxed );
        else
        {
            from[copied] = slot->from;
            bytes[copied] = slot->bytes;
            memcpy( data + (size_t) copied * (size_t) stride, slot->data, (size_t) slot->bytes );
            ++copied;
        }
    }

    if ( num_taken > 0 )
    {
        rocketnet_platform_mutex_acquire( &inbox->mutex );
        for ( int i = num_taken - 1; i >= 0; --i )
            loopback_release( inbox, indices[i] );
        rocketnet_platform_mutex_release( &inbox->mutex );
    }
    *taken = num_taken;
    return copied;
}

static int loopback_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    rocketnet_loopback_endpoint_t * receiver = (rocketnet_loopback_endpoint_t*) transport;
    const size_t step = (size_t) ( stride > 0 ? stride : 0 );
    int received = 0;
    int taken = ROCKETNET_LOOPBACK_TAKE_BATCH;
    while ( received < count && taken == ROCKETNET_LOOPBACK_TAKE_BATCH )
        received += loopback_take( receiver, from + received, data + (size_t) received * step, stride, bytes + received, count - received, &taken );
    return received;
}

static int loopback_client_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    int bytes = 0;
    return loopback_receive_batch( transport, from, data, capacity, &bytes, 1 ) == 1 ? bytes : 0;
}

static const rocketnet_server_transport_io_operations_t loopback_server_operations =
{
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, loopback_server_create, loopback_destroy, loopback_stop, loopback_stopped,
    NULL, NULL, NULL, NULL, NULL, loopback_server_send_batch, loopback_receive_batch,
    NULL, NULL, NULL, NULL, NULL, loopback_send_failures, loopback_server_send_batch_shutdown
};

// no wait and no wake: the client runs no I/O thread on this transport and
// steps on its caller's thread (rocketnet_client_transport_create). No
// connect: the loopback router is one in-process table shared by every
// endpoint, with no socket to connect; the client's own source check
// (rocketnet_client_process_packet) is the only filter on this transport.
static const rocketnet_client_transport_io_operations_t loopback_client_operations =
{ loopback_client_create, loopback_destroy, loopback_send, loopback_client_receive, NULL, loopback_receive_batch,
  NULL, NULL, loopback_send_failures, NULL };

extern const rocketnet_transport_backend_t rocketnet_loopback_transport = { &loopback_server_operations, &loopback_client_operations };

uint32_t rocketnet_loopback_endpoint_slots_touched( void * instance )
{
    rocketnet_loopback_storage_t * inbox = ( (rocketnet_loopback_endpoint_t*) instance )->storage;
    rocketnet_platform_mutex_guard( &inbox->mutex );
    return inbox->fresh;
}

uint64_t rocketnet_loopback_endpoint_dropped( void * instance )
{
    return ( (rocketnet_loopback_endpoint_t*) instance )->dropped.load( std::memory_order_relaxed );
}

int rocketnet_loopback_endpoints_live()
{
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    int live = 0;
    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
        if ( state.endpoints[i].live.load( std::memory_order_relaxed ) )
            ++live;
    return live;
}
