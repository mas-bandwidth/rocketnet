/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_address.h"
#include "rocketnet_platform.h"
#include "rocketnet_memory.h"
#include "rocketnet_wire.h"

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
    rocketnet_loopback_packet_t * packets;
    bool live;
};

struct rocketnet_loopback_state_t
{
    rocketnet_platform_mutex_t mutex;
    rocketnet_loopback_endpoint_t endpoints[ROCKETNET_LOOPBACK_MAX_ENDPOINTS];
    uint16_t next_port;
};

struct rocketnet_loopback_state_init_t
{
    rocketnet_loopback_state_t state;
    rocketnet_loopback_state_init_t()
    {
        rocketnet_platform_mutex_create( &state.mutex );
        state.next_port = ROCKETNET_LOOPBACK_FIRST_PORT;
    }
};

static rocketnet_loopback_state_t & loopback_state()
{
    static rocketnet_loopback_state_init_t init;
    return init.state;
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
// stack, like a socket with IPV6_V6ONLY off: it serves IPv4 peers too, and
// its datagrams to an IPv4 peer come from the IPv4 loopback on its port, so
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
// dual-stack endpoint bound to the IPv6 wildcard on that port.

static rocketnet_loopback_endpoint_t * loopback_find_destination( rocketnet_loopback_state_t & state, const rocketnet_address_t & to )
{
    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
    {
        rocketnet_loopback_endpoint_t & e = state.endpoints[i];
        if ( e.live && e.bind.port == to.port && e.bind.type == to.type &&
             ( loopback_is_wildcard( e.bind ) || rocketnet_address_equal( &e.bind, &to ) ) )
            return &e;
    }
    if ( to.type == ROCKETNET_ADDRESS_IPV4 )
    {
        for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
        {
            rocketnet_loopback_endpoint_t & e = state.endpoints[i];
            if ( e.live && e.bind.port == to.port && e.bind.type == ROCKETNET_ADDRESS_IPV6 &&
                 loopback_is_wildcard( e.bind ) )
                return &e;
        }
    }
    return NULL;
}

// The next queue slot, or NULL after counting a drop at the fixed ceiling.
// All queue storage is allocated at create; traffic never allocates.

static rocketnet_loopback_packet_t * loopback_queue_tail( rocketnet_loopback_endpoint_t * dst )
{
    if ( dst->count >= ROCKETNET_LOOPBACK_QUEUE_PACKETS )
    {
        ++dst->dropped;
        return NULL;
    }
    return &dst->packets[dst->tail % ROCKETNET_LOOPBACK_QUEUE_PACKETS];
}

size_t rocketnet_loopback_queue_bytes( int packets )
{
    return sizeof( rocketnet_loopback_packet_t ) * (size_t) packets;
}

static void * loopback_create( void * context, rocketnet_address_t * bind )
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
    ep->packets = (rocketnet_loopback_packet_t*) rocketnet_malloc( context,
        rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS ) );
    if ( !ep->packets )
        return NULL;
    ep->context = context;
    ep->bind = *bind;
    ep->dropped = 0;
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->live = true;
    return ep;
}

static void * loopback_server_create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * bind )
{
    return loopback_create( context, bind );
}

static void * loopback_client_create( void * context, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * bind )
{
    return loopback_create( context, bind );
}

static void loopback_destroy( void * transport )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t*) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
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
    rocketnet_loopback_endpoint_t * sender = (rocketnet_loopback_endpoint_t*) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    rocketnet_loopback_endpoint_t * dst = loopback_find_destination( state, *to );
    if ( !dst )
        return;
    rocketnet_loopback_packet_t * packet = loopback_queue_tail( dst );
    if ( !packet )
        return;
    packet->bytes = bytes;
    packet->from = loopback_sender_address( sender->bind, *to );
    memcpy( packet->data, data, (size_t) bytes );
    ++dst->tail;
    ++dst->count;
}

static void loopback_server_send_batch( void * transport, const rocketnet_platform_send_t * packets, int count )
{
    rocketnet_loopback_endpoint_t * sender = (rocketnet_loopback_endpoint_t*) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    for ( int i = 0; i < count; ++i )
    {
        const rocketnet_platform_send_t & packet = packets[i];
        if ( packet.packet_bytes <= 0 || packet.packet_bytes > ROCKETNET_RECEIVE_CEILING )
            continue;
        rocketnet_loopback_endpoint_t * dst = loopback_find_destination( state, *packet.to );
        if ( !dst )
            continue;
        rocketnet_loopback_packet_t * slot = loopback_queue_tail( dst );
        if ( !slot )
            continue;
        slot->bytes = packet.packet_bytes;
        slot->from = loopback_sender_address( sender->bind, *packet.to );
        memcpy( slot->data, packet.packet_data, (size_t) packet.packet_bytes );
        ++dst->tail;
        ++dst->count;
    }
}

static int loopback_server_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t*) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    int received = 0;
    while ( received < count && ep->count > 0 )
    {
        rocketnet_loopback_packet_t * packet = &ep->packets[ep->head % ROCKETNET_LOOPBACK_QUEUE_PACKETS];
        if ( stride <= 0 || packet->bytes <= 0 || packet->bytes > stride )
        {
            ++ep->dropped;
            ++ep->head;
            --ep->count;
            continue;
        }
        from[received] = packet->from;
        bytes[received] = packet->bytes;
        memcpy( data + (size_t) received * stride, packet->data, (size_t) packet->bytes );
        ++ep->head;
        --ep->count;
        ++received;
    }
    return received;
}

static int loopback_client_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t*) transport;
    rocketnet_loopback_state_t & state = loopback_state();
    rocketnet_platform_mutex_guard( &state.mutex );
    if ( ep->count == 0 )
        return 0;
    rocketnet_loopback_packet_t * packet = &ep->packets[ep->head % ROCKETNET_LOOPBACK_QUEUE_PACKETS];
    if ( packet->bytes > capacity )
    {
        ++ep->dropped;
        ++ep->head;
        --ep->count;
        return 0;
    }
    *from = packet->from;
    const int bytes = packet->bytes;
    memcpy( data, packet->data, (size_t) bytes );
    ++ep->head;
    --ep->count;
    return bytes;
}

static const rocketnet_server_transport_io_operations_t loopback_server_operations =
{
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, loopback_server_create, loopback_destroy, loopback_stop, loopback_stopped,
    NULL, NULL, NULL, NULL, NULL, loopback_server_send_batch, loopback_server_receive_batch,
    NULL, NULL, NULL, NULL, NULL, NULL
};

static const rocketnet_client_transport_io_operations_t loopback_client_operations =
{ loopback_client_create, loopback_destroy, loopback_send, loopback_client_receive, NULL, NULL, NULL, NULL, NULL };

extern const rocketnet_transport_backend_t rocketnet_loopback_transport = { &loopback_server_operations, &loopback_client_operations };

uint64_t rocketnet_loopback_endpoint_dropped( void * instance )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t*) instance;
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
