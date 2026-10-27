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

// The loopback transport backend carries datagrams between endpoints of the same
// process through memory, with no socket and no system network call. A
// process-wide table holds the endpoints; one platform mutex guards it, so the
// client transport's receive thread and the main thread can both call in.

enum
{
    ROCKETNET_LOOPBACK_FIRST_PORT = 49152,
    ROCKETNET_LOOPBACK_LAST_PORT = 65535
};

struct rocketnet_loopback_packet_t
{
    rocketnet_address_t from;
    int bytes;
    uint8_t data[ROCKETNET_RECEIVE_CEILING];
};

struct rocketnet_loopback_endpoint_t
{
    void * context;
    rocketnet_address_t bind;
    uint64_t dropped;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t capacity;                  // a power of two: the queue's datagrams, fixed at create
    rocketnet_loopback_packet_t * packets;
    bool live;
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
        for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
            state.send_failures[i].store( 0 );
    }
};

static rocketnet_loopback_state_t & loopback_state()
{
    static rocketnet_loopback_state_init_t init;
    return init.state;
}

#if defined( ROCKETNET_LOOPBACK_TEST_HOOKS )
static bool loopback_test_send_contention = false;
void rocketnet_loopback_test_send_contention( bool busy ) { loopback_test_send_contention = busy; }
void rocketnet_loopback_test_queue_indices( void * instance, uint32_t * head, uint32_t * tail, uint32_t * capacity )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) instance;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    *head = ep->head;
    *tail = ep->tail;
    *capacity = ep->capacity;
}
#endif

static bool loopback_try_send_lock( rocketnet_loopback_state_t & state )
{
#if defined( ROCKETNET_LOOPBACK_TEST_HOOKS )
    if ( loopback_test_send_contention )
        return false;
#endif
    return rocketnet_platform_mutex_try_acquire( &state.mutex );
}

static void loopback_send_failed( rocketnet_loopback_endpoint_t * sender, uint64_t count )
{
    rocketnet_loopback_state_t & state = loopback_state();
    state.send_failures[sender - state.endpoints].fetch_add( count, std::memory_order_relaxed );
}

static uint64_t loopback_send_failures( void * instance )
{
    rocketnet_loopback_state_t & state = loopback_state();
    return state.send_failures[(rocketnet_loopback_endpoint_t *) instance - state.endpoints].load( std::memory_order_relaxed );
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

// the endpoint a datagram reaches: one bound to this address, or to the
// wildcard of this family, on this port. an IPv4 datagram also reaches a
// dual-stack endpoint bound to the IPv6 wildcard on that port. Create refuses
// a second endpoint of one family on one port, so each is one lookup in
// state.bound.

static rocketnet_loopback_endpoint_t * loopback_find_destination( rocketnet_loopback_state_t & state, const rocketnet_address_t & to )
{
    if ( to.type != ROCKETNET_ADDRESS_IPV4 && to.type != ROCKETNET_ADDRESS_IPV6 )
        return NULL;
    const uint16_t same = *loopback_bound_entry( state, to.type, to.port );
    if ( same )
    {
        rocketnet_loopback_endpoint_t & e = state.endpoints[same - 1];
        if ( e.live && ( loopback_is_wildcard( e.bind ) || rocketnet_address_equal( &e.bind, &to ) ) )
            return &e;
    }
    if ( to.type == ROCKETNET_ADDRESS_IPV4 )
    {
        const uint16_t dual = *loopback_bound_entry( state, ROCKETNET_ADDRESS_IPV6, to.port );
        if ( dual )
        {
            rocketnet_loopback_endpoint_t & e = state.endpoints[dual - 1];
            if ( e.live && loopback_is_wildcard( e.bind ) )
                return &e;
        }
    }
    return NULL;
}

// The next queue slot, or NULL after counting a drop at the fixed ceiling.
// All queue storage is allocated at create; traffic never allocates.
// tla/LoopbackQueue.tla Send: enqueue at tail mod Capacity when count < Capacity;
// a full queue drops and counts.

static rocketnet_loopback_packet_t * loopback_queue_tail( rocketnet_loopback_endpoint_t * dst )
{
    if ( dst->count >= dst->capacity )
    {
        ++dst->dropped;
        return NULL;
    }
    return &dst->packets[dst->tail & ( dst->capacity - 1 )];
}

// Every dequeue goes through here, under the state mutex the caller holds:
// advance head and shrink count, and when the queue empties restart both
// indices at slot zero. A queue that empties therefore writes only the slots
// its deepest backlog reached; FIFO cannot change because an empty queue has
// nothing to reorder. tla/LoopbackQueue.tla Receive and Drop.
static void loopback_queue_pop( rocketnet_loopback_endpoint_t * ep )
{
    ++ep->head;
    --ep->count;
    if ( ep->count == 0 )
        ep->head = ep->tail = 0;
}

size_t rocketnet_loopback_queue_bytes( int packets )
{
    return sizeof( rocketnet_loopback_packet_t ) * (size_t) packets;
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

static void * loopback_create( void * context, rocketnet_address_t * bind, int queue_packets )
{
    if ( bind->type != ROCKETNET_ADDRESS_IPV4 && bind->type != ROCKETNET_ADDRESS_IPV6 )
        return NULL;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
    {
        rocketnet_loopback_endpoint_t & e = state.endpoints[i];
        if ( e.live && e.bind.port == bind->port && e.bind.type == bind->type )
            return NULL;
    }

    if ( bind->port == 0 )
    {
        uint16_t port = state.next_port;
        for ( int tries = 0; tries <= 65535; ++tries )
        {
            bool held = false;
            for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
            {
                if ( state.endpoints[i].live && state.endpoints[i].bind.port == port )
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

    int slot = -1;
    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
    {
        if ( !state.endpoints[i].live )
        {
            slot = i;
            break;
        }
    }
    if ( slot < 0 )
    {
        rocketnet_warn( "loopback transport refused a create: ROCKETNET_LOOPBACK_MAX_ENDPOINTS is %d",
                        (int) ROCKETNET_LOOPBACK_MAX_ENDPOINTS );
        return NULL;
    }

    rocketnet_loopback_endpoint_t * ep = &state.endpoints[slot];
    ep->packets = (rocketnet_loopback_packet_t *) rocketnet_malloc( context,
                                                                    rocketnet_loopback_queue_bytes( queue_packets ) );
    if ( !ep->packets )
        return NULL;
    ep->context = context;
    ep->bind = *bind;
    ep->dropped = 0;
    state.send_failures[slot].store( 0, std::memory_order_relaxed );
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->capacity = (uint32_t) queue_packets;
    ep->live = true;
    *loopback_bound_entry( state, bind->type, bind->port ) = (uint16_t) ( slot + 1 );
    return ep;
}

static void * loopback_server_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    const int max_clients = config && config->transport ? config->transport->max_clients : ROCKETNET_MAX_CLIENTS;
    return loopback_create( context, bind, rocketnet_loopback_server_queue_packets( max_clients ) );
}

static void * loopback_client_create( void * context, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * bind )
{
    return loopback_create( context, bind, ROCKETNET_LOOPBACK_QUEUE_PACKETS );
}

static void loopback_destroy( void * transport )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    uint16_t * bound = loopback_bound_entry( state, ep->bind.type, ep->bind.port );
    if ( *bound == (uint16_t) ( ep - state.endpoints + 1 ) )
        *bound = 0;
    rocketnet_free( ep->context, ep->packets );
    memset( ep, 0, sizeof( *ep ) );
}

static void loopback_stop( void * ) {}

static bool loopback_stopped( void * )
{
    return true;
}

static void loopback_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    if ( bytes <= 0 || bytes > ROCKETNET_RECEIVE_CEILING )
        return;
    rocketnet_loopback_endpoint_t * sender = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    // Ordinary client sends preserve in-process delivery under concurrent
    // receive traffic. Disconnect/stop/check never invoke this callback:
    // the client stages its final burst for the worker or explicit update.
    rocketnet_platform_mutex_acquire( &state.mutex );
    rocketnet_loopback_endpoint_t * dst = loopback_find_destination( state, *to );
    if ( !dst )
    {
        loopback_send_failed( sender, 1 );
        rocketnet_platform_mutex_release( &state.mutex );
        return;
    }
    rocketnet_loopback_packet_t * packet = loopback_queue_tail( dst );
    if ( !packet )
    {
        loopback_send_failed( sender, 1 );
        rocketnet_platform_mutex_release( &state.mutex );
        return;
    }
    packet->bytes = bytes;
    packet->from = loopback_sender_address( sender->bind, *to );
    memcpy( packet->data, data, (size_t) bytes );
    ++dst->tail;
    ++dst->count;
    rocketnet_platform_mutex_release( &state.mutex );
}

static void loopback_server_send_batch_impl( void * transport, const rocketnet_platform_send_t * packets, int count, bool shutdown )
{
    rocketnet_loopback_endpoint_t * sender = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    if ( shutdown && !loopback_try_send_lock( state ) )
    {
        if ( count > 0 )
            loopback_send_failed( sender, (uint64_t) count );
        return;
    }
    if ( !shutdown )
        rocketnet_platform_mutex_acquire( &state.mutex );
    for ( int i = 0; i < count; ++i )
    {
        const rocketnet_platform_send_t & packet = packets[i];
        if ( packet.packet_bytes <= 0 || packet.packet_bytes > ROCKETNET_RECEIVE_CEILING )
            continue;
        rocketnet_loopback_endpoint_t * dst = loopback_find_destination( state, *packet.to );
        if ( !dst )
        {
            loopback_send_failed( sender, 1 );
            continue;
        }
        rocketnet_loopback_packet_t * slot = loopback_queue_tail( dst );
        if ( !slot )
        {
            loopback_send_failed( sender, 1 );
            continue;
        }
        slot->bytes = packet.packet_bytes;
        slot->from = loopback_sender_address( sender->bind, *packet.to );
        memcpy( slot->data, packet.packet_data, (size_t) packet.packet_bytes );
        ++dst->tail;
        ++dst->count;
    }
    rocketnet_platform_mutex_release( &state.mutex );
}

static void loopback_server_send_batch( void * transport, const rocketnet_platform_send_t * packets, int count )
{
    loopback_server_send_batch_impl( transport, packets, count, false );
}

static void loopback_server_send_batch_shutdown( void * transport, const rocketnet_platform_send_t * packets, int count )
{
    loopback_server_send_batch_impl( transport, packets, count, true );
}

static int loopback_server_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    int received = 0;
    while ( received < count && ep->count > 0 )
    {
        rocketnet_loopback_packet_t * packet = &ep->packets[ep->head & ( ep->capacity - 1 )];
        if ( stride <= 0 || packet->bytes <= 0 || packet->bytes > stride )
        {
            ++ep->dropped;
            loopback_queue_pop( ep );    // tla/LoopbackQueue.tla Drop
            continue;
        }
        from[received] = packet->from;
        bytes[received] = packet->bytes;
        memcpy( data + (size_t) received * (size_t) stride, packet->data, (size_t) packet->bytes );
        loopback_queue_pop( ep );        // tla/LoopbackQueue.tla Receive
        ++received;
    }
    return received;
}

static int loopback_client_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    if ( ep->count == 0 )
        return 0;
    rocketnet_loopback_packet_t * packet = &ep->packets[ep->head & ( ep->capacity - 1 )];
    if ( packet->bytes > capacity )
    {
        ++ep->dropped;
        loopback_queue_pop( ep );        // tla/LoopbackQueue.tla Drop
        return 0;
    }
    *from = packet->from;
    const int bytes = packet->bytes;
    memcpy( data, packet->data, (size_t) bytes );
    loopback_queue_pop( ep );            // tla/LoopbackQueue.tla Receive
    return bytes;
}

static const rocketnet_server_transport_io_operations_t loopback_server_operations = {
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, loopback_server_create, loopback_destroy, loopback_stop, loopback_stopped,
    NULL, NULL, NULL, NULL, NULL, loopback_server_send_batch, loopback_server_receive_batch,
    NULL, NULL, NULL, NULL, NULL, loopback_send_failures, loopback_server_send_batch_shutdown
};

// no connect: the loopback router is one in-process table shared by every
// endpoint, with no socket to connect; the client's own source check
// (rocketnet_client_process_packet) is the only filter on this transport
static const rocketnet_client_transport_io_operations_t loopback_client_operations = { loopback_client_create, loopback_destroy, loopback_send, loopback_client_receive, NULL, NULL, NULL, NULL, loopback_send_failures, NULL };

extern const rocketnet_transport_backend_t rocketnet_loopback_transport = { &loopback_server_operations, &loopback_client_operations };

uint64_t rocketnet_loopback_endpoint_dropped( void * instance )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) instance;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    return ep->dropped;
}

int rocketnet_loopback_endpoints_live()
{
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    int live = 0;
    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
        if ( state.endpoints[i].live )
            ++live;
    return live;
}
