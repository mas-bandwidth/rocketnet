/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include "rocketnet_driver_loopback.h"
#include <atomic>
#include <new>
#include "rocketnet_address.h"
#include "rocketnet_platform.h"
#include "rocketnet_memory.h"
#include "rocketnet_wire.h"
#include "rocketnet_client_protocol.h"

#include <string.h>

// The loopback transport backend carries datagrams between endpoints of the same
// process through memory, with no socket and no system network call. The
// endpoints find each other through a hub the application creates
// (rocketnet_loopback_hub_create) and names in each endpoint's config. The hub
// is one block from the caller's allocator: its header, its mutex, its endpoint
// records and every endpoint's queue, all fixed at create. Nothing here is
// per-copy state, so two copies of the library in one process (a server and a
// client in separate shared objects) share one hub. The hub mutex guards it, so
// the client transport's receive thread and the main thread can both call in.

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
    rocketnet_loopback_hub_t * hub;
    rocketnet_address_t bind;
    uint64_t dropped;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t capacity;                  // a power of two: the queue's datagrams, fixed at hub create
    rocketnet_loopback_packet_t * packets;
    bool live;
};

struct rocketnet_loopback_hub_t
{
    rocketnet_loopback_hub_header_t header;     // first: every use checks it before reading anything else
    void * context;
    int max_clients;
    int registered;                             // live endpoints; destroy refuses while any is
    uint16_t next_port;
    rocketnet_platform_mutex_t mutex;
    rocketnet_loopback_endpoint_t * endpoints;  // 1 + max_clients: the server at 0, the clients after it
    std::atomic<uint64_t> * send_failures;      // one per endpoint record, beside it
    // the live endpoint bound on each (family, port), as its record index plus
    // one: create refuses a second endpoint of a family on a port, so a
    // datagram's destination is one lookup, not a walk of the records
    uint16_t bound[2][65536];
};

ROCKETNET_STATIC_ASSERT( ROCKETNET_MAX_CLIENTS + 1 < 65535, "an endpoint's record index plus one fits the bound table" );
ROCKETNET_STATIC_ASSERT( offsetof( rocketnet_loopback_hub_t, header ) == 0, "the hub header is at the start of the hub" );

// the layout id: the sizes and offsets two copies of the library must agree on
// to share a hub. A copy built with another layout refuses the hub
// (rocketnet_loopback_hub_check), so a mismatch is caught at its first use
// instead of read through.
static constexpr uint32_t loopback_layout_hash( const uint64_t * values, int count, uint32_t hash )
{
    return count == 0 ? hash : loopback_layout_hash( values + 1, count - 1, ( hash ^ (uint32_t) ( values[0] ^ ( values[0] >> 32 ) ) ) * 16777619u );
}

static constexpr uint64_t ROCKETNET_LOOPBACK_HUB_LAYOUT_VALUES[] = {
    sizeof( rocketnet_loopback_hub_t ),
    offsetof( rocketnet_loopback_hub_t, mutex ),
    offsetof( rocketnet_loopback_hub_t, bound ),
    sizeof( rocketnet_platform_mutex_t ),
    sizeof( rocketnet_loopback_endpoint_t ),
    offsetof( rocketnet_loopback_endpoint_t, packets ),
    sizeof( rocketnet_loopback_packet_t ),
    sizeof( std::atomic<uint64_t> ),
};

static constexpr uint32_t ROCKETNET_LOOPBACK_HUB_LAYOUT =
    loopback_layout_hash( ROCKETNET_LOOPBACK_HUB_LAYOUT_VALUES, (int) ( sizeof( ROCKETNET_LOOPBACK_HUB_LAYOUT_VALUES ) / sizeof( ROCKETNET_LOOPBACK_HUB_LAYOUT_VALUES[0] ) ), 2166136261u );

// the ephemeral range wraps from its last port to its first
// (proofs/tla/RNLoopbackBindLayout.tla Create, EphemeralInRange: a port + 1
// that wrapped at 65536 would hand out port 0)
static uint16_t loopback_next_port( uint16_t port )
{
    return port == ROCKETNET_LOOPBACK_LAST_PORT ? (uint16_t) ROCKETNET_LOOPBACK_FIRST_PORT : (uint16_t) ( port + 1 );
}

static uint16_t * loopback_bound_entry( rocketnet_loopback_hub_t * hub, int type, uint16_t port )
{
    return &hub->bound[type == ROCKETNET_ADDRESS_IPV6 ? 1 : 0][port];
}

static int loopback_num_endpoints( const rocketnet_loopback_hub_t * hub )
{
    return hub->max_clients + 1;
}

enum rocketnet_status_t rocketnet_loopback_hub_check( const rocketnet_loopback_hub_t * hub )
{
    if ( !hub )
        return ROCKETNET_STATUS_LOOPBACK_HUB_MISSING;

    if ( hub->header.magic != ROCKETNET_LOOPBACK_HUB_MAGIC || hub->header.version != ROCKETNET_VERSION ||
         hub->header.layout != ROCKETNET_LOOPBACK_HUB_LAYOUT )
        return ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH;

    return ROCKETNET_STATUS_OK;
}

static bool loopback_hub_usable( const rocketnet_loopback_hub_t * hub, const char * endpoint )
{
    const enum rocketnet_status_t status = rocketnet_loopback_hub_check( hub );
    if ( status == ROCKETNET_STATUS_LOOPBACK_HUB_MISSING )
        rocketnet_error( "loopback transport refused a %s: its config has no loopback_hub; create one with rocketnet_loopback_hub_create", endpoint );
    else if ( status != ROCKETNET_STATUS_OK )
        rocketnet_error( "loopback transport refused a %s: the loopback hub was made by another build of the library (magic, version or layout differ)", endpoint );

    return status == ROCKETNET_STATUS_OK;
}

#if ROCKETNET_DEV
static bool loopback_test_send_contention = false;

void rocketnet_loopback_test_send_contention( bool busy ) { loopback_test_send_contention = busy; }

void rocketnet_loopback_test_queue_indices( void * instance, uint32_t * head, uint32_t * tail, uint32_t * capacity )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) instance;
    rocketnet_platform_mutex_guard( &ep->hub->mutex );

    *head = ep->head;
    *tail = ep->tail;
    *capacity = ep->capacity;
}
#endif // #if ROCKETNET_DEV

static bool loopback_try_send_lock( rocketnet_loopback_hub_t * hub )
{
#if ROCKETNET_DEV
    if ( loopback_test_send_contention )
        return false;
#endif // #if ROCKETNET_DEV

    return rocketnet_platform_mutex_try_acquire( &hub->mutex );
}

static void loopback_send_failed( rocketnet_loopback_endpoint_t * sender, uint64_t count )
{
    rocketnet_loopback_hub_t * hub = sender->hub;
    hub->send_failures[sender - hub->endpoints].fetch_add( count, std::memory_order_relaxed );
}

static uint64_t loopback_send_failures( void * instance )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) instance;
    return ep->hub->send_failures[ep - ep->hub->endpoints].load( std::memory_order_relaxed );
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

static rocketnet_address_t loopback_sender_address( const rocketnet_address_t & bind, const rocketnet_address_t & to )
{
    // the source address a datagram from this endpoint carries. a wildcard bind
    // sends from its loopback. an endpoint bound to the IPv6 wildcard is dual
    // stack, like a socket with IPV6_V6ONLY off: it serves IPv4 clients too, and
    // its datagrams to an IPv4 client come from the IPv4 loopback on its port, so
    // the IPv4 client sees the reply from the address it sent to.

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

// proofs/tla/RNLoopbackBindLayout.tla LookupIsWalk: the lookup in hub->bound
// returns the endpoint a walk of the live records would
static rocketnet_loopback_endpoint_t * loopback_find_destination( rocketnet_loopback_hub_t * hub, const rocketnet_address_t & to )
{
    // the endpoint a datagram reaches: one bound to this address, or to the
    // wildcard of this family, on this port. an IPv4 datagram also reaches a
    // dual-stack endpoint bound to the IPv6 wildcard on that port. Create refuses
    // a second endpoint of one family on one port, and any IPv4 endpoint on a
    // dual-stack endpoint's port, so each is one lookup in hub->bound and an
    // IPv4 datagram has one destination.

    if ( to.type != ROCKETNET_ADDRESS_IPV4 && to.type != ROCKETNET_ADDRESS_IPV6 )
        return NULL;

    const uint16_t same = *loopback_bound_entry( hub, to.type, to.port );
    if ( same )
    {
        rocketnet_loopback_endpoint_t & e = hub->endpoints[same - 1];
        if ( e.live && ( loopback_is_wildcard( e.bind ) || rocketnet_address_equal( &e.bind, &to ) ) )
            return &e;
    }

    if ( to.type == ROCKETNET_ADDRESS_IPV4 )
    {
        const uint16_t dual = *loopback_bound_entry( hub, ROCKETNET_ADDRESS_IPV6, to.port );
        if ( dual )
        {
            rocketnet_loopback_endpoint_t & e = hub->endpoints[dual - 1];
            if ( e.live && loopback_is_wildcard( e.bind ) )
                return &e;
        }
    }

    return NULL;
}

static rocketnet_loopback_packet_t * loopback_queue_tail( rocketnet_loopback_endpoint_t * dst )
{
    // the next queue slot, or NULL after counting a drop at the fixed ceiling.
    // All queue storage is allocated at hub create; traffic never allocates.
    // proofs/tla/LoopbackQueue.tla Send: enqueue at tail mod Capacity when count < Capacity;
    // a full queue drops and counts. proofs/tla/LoopbackHubLayout.tla Send carries
    // this arithmetic at capacity 3: the mask is the slot mod a power-of-two
    // capacity, and the uint32_t wrap is a multiple of it.

    if ( dst->count >= dst->capacity )
    {
        ++dst->dropped;
        return NULL;
    }

    return &dst->packets[dst->tail & ( dst->capacity - 1 )];
}

static void loopback_queue_pop( rocketnet_loopback_endpoint_t * ep )
{
    // every dequeue goes through here, under the hub mutex the caller holds:
    // advance head and shrink count, and when the queue empties restart both
    // indices at slot zero. A queue that empties therefore writes only the slots
    // its deepest backlog reached; FIFO cannot change because an empty queue has
    // nothing to reorder. proofs/tla/LoopbackQueue.tla Receive and Drop;
    // proofs/tla/LoopbackHubLayout.tla Receive.

    ++ep->head;
    --ep->count;

    if ( ep->count == 0 )
        ep->head = ep->tail = 0;
}

size_t rocketnet_loopback_queue_bytes( int packets )
{
    return sizeof( rocketnet_loopback_packet_t ) * (size_t) packets;
}

int rocketnet_loopback_server_queue_packets( int max_clients )
{
    // the server endpoint's queue, from the plan: every datagram one server
    // update may read, ROCKETNET_RECEIVE_PACKETS_PER_CLIENT for each of the
    // hub's clients, rounded up to a power of two (16384 at 1000 clients),
    // and never less than a client endpoint's. A whole population's disconnect
    // bursts, ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS from each client, fit
    // beside one update's ordinary traffic, and one update reads them all.

    const uint64_t wanted = (uint64_t) ( max_clients > 0 ? max_clients : 1 ) * (uint64_t) ROCKETNET_RECEIVE_PACKETS_PER_CLIENT;
    uint64_t packets = ROCKETNET_LOOPBACK_QUEUE_PACKETS;

    while ( packets < wanted )
        packets *= 2;

    return (int) packets;
}

ROCKETNET_STATIC_ASSERT( ROCKETNET_RECEIVE_PACKETS_PER_CLIENT >= ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS + 2,
                         "a server loopback queue sized from the plan holds every client's disconnect burst beside two packets of ordinary traffic each" );

static uint64_t loopback_align( uint64_t bytes )
{
    return ( bytes + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) / ROCKETNET_ALLOCATION_ALIGNMENT * ROCKETNET_ALLOCATION_ALIGNMENT;
}

uint64_t rocketnet_loopback_hub_bytes( int max_clients )
{
    // the hub's one block: the hub, the endpoint records, their send failure
    // counters, the server's queue and each client's queue, each part aligned

    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
        return 0;

    const uint64_t endpoints = (uint64_t) max_clients + 1;

    return loopback_align( sizeof( rocketnet_loopback_hub_t ) ) +
           loopback_align( endpoints * sizeof( rocketnet_loopback_endpoint_t ) ) +
           loopback_align( endpoints * sizeof( std::atomic<uint64_t> ) ) +
           loopback_align( rocketnet_loopback_queue_bytes( rocketnet_loopback_server_queue_packets( max_clients ) ) ) +
           (uint64_t) max_clients * loopback_align( rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS ) );
}

rocketnet_loopback_hub_t * rocketnet_loopback_hub_create( void * context, int max_clients, enum rocketnet_status_t * status )
{
    // proofs/tla/LoopbackHub.tla Create: one block with every record free. The
    // queues follow the records in it, the server's first, then each
    // client's, the layout proofs/tla/LoopbackHubLayout.tla checks (Base, Addr)

    if ( status )
        *status = ROCKETNET_STATUS_OK;

    if ( !rocketnet_library_initialized() )
    {
        rocketnet_error( "rocketnet_loopback_hub_create: rocketnet_init must be called first" );

        rocketnet_api_misuse_record();

        if ( status )
            *status = ROCKETNET_STATUS_NOT_INITIALIZED;

        return NULL;
    }

    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        rocketnet_error( "rocketnet_loopback_hub_create refused: max_clients is %d, expected 1 .. %d", max_clients, (int) ROCKETNET_MAX_CLIENTS );

        if ( status )
            *status = ROCKETNET_STATUS_INVALID_ARGUMENT;

        return NULL;
    }

    const uint64_t bytes = rocketnet_loopback_hub_bytes( max_clients );
    uint8_t * block = bytes <= (uint64_t) SIZE_MAX ? (uint8_t *) rocketnet_malloc( context, (size_t) bytes ) : NULL;
    if ( !block )
    {
        rocketnet_error( "rocketnet_loopback_hub_create refused: %llu bytes for %d clients", (unsigned long long) bytes, max_clients );

        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

        return NULL;
    }

    const uint64_t endpoints = (uint64_t) max_clients + 1;
    const uint64_t endpoints_offset = loopback_align( sizeof( rocketnet_loopback_hub_t ) );
    const uint64_t failures_offset = endpoints_offset + loopback_align( endpoints * sizeof( rocketnet_loopback_endpoint_t ) );
    const uint64_t queues_offset = failures_offset + loopback_align( endpoints * sizeof( std::atomic<uint64_t> ) );

    // the records are zeroed; the queues are not, so their pages are touched
    // only by traffic

    memset( block, 0, (size_t) queues_offset );

    rocketnet_loopback_hub_t * hub = (rocketnet_loopback_hub_t *) block;
    hub->context = context;
    hub->max_clients = max_clients;
    hub->next_port = ROCKETNET_LOOPBACK_FIRST_PORT;
    hub->endpoints = (rocketnet_loopback_endpoint_t *) ( block + endpoints_offset );
    hub->send_failures = (std::atomic<uint64_t> *) ( block + failures_offset );

    uint8_t * queue = block + queues_offset;
    for ( int i = 0; i < loopback_num_endpoints( hub ); ++i )
    {
        const int packets = i == 0 ? rocketnet_loopback_server_queue_packets( max_clients ) : ROCKETNET_LOOPBACK_QUEUE_PACKETS;
        rocketnet_loopback_endpoint_t & e = hub->endpoints[i];
        e.hub = hub;
        e.capacity = (uint32_t) packets;
        e.packets = (rocketnet_loopback_packet_t *) queue;
        new ( &hub->send_failures[i] ) std::atomic<uint64_t>( 0 );
        queue += loopback_align( rocketnet_loopback_queue_bytes( packets ) );
    }

    if ( !rocketnet_platform_mutex_create( &hub->mutex ) )
    {
        rocketnet_error( "rocketnet_loopback_hub_create refused: the platform refused its mutex" );

        rocketnet_free( context, block );

        if ( status )
            *status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;

        return NULL;
    }

    hub->header.magic = ROCKETNET_LOOPBACK_HUB_MAGIC;
    hub->header.version = ROCKETNET_VERSION;
    hub->header.layout = ROCKETNET_LOOPBACK_HUB_LAYOUT;

    return hub;
}

enum rocketnet_status_t rocketnet_loopback_hub_destroy( rocketnet_loopback_hub_t * hub )
{
    // proofs/tla/LoopbackHub.tla Destroy: refused, and the hub kept, while any
    // endpoint is registered with it

    if ( !hub )
        return ROCKETNET_STATUS_OK;                     // as free(NULL): silent, uncounted

    const enum rocketnet_status_t valid = rocketnet_loopback_hub_check( hub );
    if ( valid != ROCKETNET_STATUS_OK )
    {
        rocketnet_error( "rocketnet_loopback_hub_destroy refused: the hub was made by another build of the library (magic, version or layout differ); destroy it with the library that created it" );

        rocketnet_api_misuse_record();

        return valid;
    }

    rocketnet_platform_mutex_acquire( &hub->mutex );
    const int registered = hub->registered;
    rocketnet_platform_mutex_release( &hub->mutex );

    if ( registered > 0 )
    {
        rocketnet_error( "rocketnet_loopback_hub_destroy refused: %d endpoints are still registered; destroy every server and client that uses the hub first, then destroy the hub", registered );

        rocketnet_api_misuse_record();

        return ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE;
    }

    rocketnet_platform_mutex_destroy( &hub->mutex );
    memset( &hub->header, 0, sizeof( hub->header ) );
    rocketnet_free( hub->context, hub );

    return ROCKETNET_STATUS_OK;
}

// proofs/tla/RNLoopbackBindLayout.tla Create: a held family port refused, the
// record taken, a port 0 bind given the next port neither family holds, and
// the entry written as the record index plus one
static void * loopback_create( rocketnet_loopback_hub_t * hub, rocketnet_address_t * bind, bool server )
{
    // proofs/tla/LoopbackHub.tla RegisterServer and RegisterClient: the server takes
    // record 0, a client the first free record after it; a full hub refuses

    if ( bind->type != ROCKETNET_ADDRESS_IPV4 && bind->type != ROCKETNET_ADDRESS_IPV6 )
        return NULL;

    if ( !loopback_hub_usable( hub, server ? "server" : "client" ) )
        return NULL;

    rocketnet_platform_mutex_guard( &hub->mutex );

    const int num_endpoints = loopback_num_endpoints( hub );

    // one endpoint per family and port, and an endpoint bound to the IPv6
    // wildcard is dual stack (loopback_find_destination), so it shares its port
    // with no IPv4 endpoint: a bind either way round is refused, as a dual-stack
    // socket's bind fails with EADDRINUSE

    for ( int i = 0; i < num_endpoints; ++i )
    {
        rocketnet_loopback_endpoint_t & e = hub->endpoints[i];
        if ( !e.live || e.bind.port != bind->port )
            continue;
        if ( e.bind.type == bind->type )
            return NULL;
        const rocketnet_address_t & ipv6 = e.bind.type == ROCKETNET_ADDRESS_IPV6 ? e.bind : *bind;
        if ( loopback_is_wildcard( ipv6 ) )
        {
            rocketnet_warn( "loopback transport refused a bind: port %d is held by a dual-stack endpoint or by an IPv4 endpoint a dual-stack bind would shadow", (int) bind->port );
            return NULL;
        }
    }

    int slot = -1;
    for ( int i = server ? 0 : 1; i < ( server ? 1 : num_endpoints ); ++i )
    {
        if ( !hub->endpoints[i].live )
        {
            slot = i;
            break;
        }
    }

    if ( slot < 0 )
    {
        if ( server )
            rocketnet_warn( "loopback transport refused a server: its hub already has one" );
        else
            rocketnet_warn( "loopback transport refused a client: its hub holds %d clients", hub->max_clients );

        return NULL;
    }

    if ( bind->port == 0 )
    {
        uint16_t port = hub->next_port;
        for ( int tries = 0; tries <= 65535; ++tries )
        {
            bool held = false;
            for ( int i = 0; i < num_endpoints; ++i )
            {
                if ( hub->endpoints[i].live && hub->endpoints[i].bind.port == port )
                {
                    held = true;
                    break;
                }
            }

            if ( !held )
                break;

            port = loopback_next_port( port );
        }

        bind->port = port;
        hub->next_port = loopback_next_port( port );
    }

    rocketnet_loopback_endpoint_t * ep = &hub->endpoints[slot];
    ep->bind = *bind;
    ep->dropped = 0;
    hub->send_failures[slot].store( 0, std::memory_order_relaxed );
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->live = true;
    ++hub->registered;
    *loopback_bound_entry( hub, bind->type, bind->port ) = (uint16_t) ( slot + 1 );

    return ep;
}

static void * loopback_server_create( void *, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    return loopback_create( config && config->transport ? config->transport->loopback_hub : NULL, bind, true );
}

static void * loopback_client_create( void *, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    return loopback_create( config && config->transport ? config->transport->loopback_hub : NULL, bind, false );
}

// proofs/tla/RNLoopbackBindLayout.tla Destroy: the entry cleared only when it
// names this record (DestroyFindsOwnEntry: under create's refusal it always does)
static void loopback_destroy( void * transport )
{
    // proofs/tla/LoopbackHub.tla Unregister: the record and its queue stay in the
    // hub for the next endpoint; nothing is freed

    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_loopback_hub_t * hub = ep->hub;
    rocketnet_platform_mutex_guard( &hub->mutex );

    uint16_t * bound = loopback_bound_entry( hub, ep->bind.type, ep->bind.port );
    if ( *bound == (uint16_t) ( ep - hub->endpoints + 1 ) )
        *bound = 0;

    memset( &ep->bind, 0, sizeof( ep->bind ) );
    ep->dropped = 0;
    ep->head = 0;
    ep->tail = 0;
    ep->count = 0;
    ep->live = false;
    --hub->registered;
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
    rocketnet_loopback_hub_t * hub = sender->hub;

    // Ordinary client sends preserve in-process delivery under concurrent
    // receive traffic. Disconnect/stop/check never invoke this callback:
    // the client stages its final burst for the worker or explicit update.

    rocketnet_platform_mutex_acquire( &hub->mutex );

    rocketnet_loopback_endpoint_t * dst = loopback_find_destination( hub, *to );
    if ( !dst )
    {
        loopback_send_failed( sender, 1 );
        rocketnet_platform_mutex_release( &hub->mutex );
        return;
    }

    rocketnet_loopback_packet_t * packet = loopback_queue_tail( dst );
    if ( !packet )
    {
        loopback_send_failed( sender, 1 );
        rocketnet_platform_mutex_release( &hub->mutex );
        return;
    }

    packet->bytes = bytes;
    packet->from = loopback_sender_address( sender->bind, *to );
    memcpy( packet->data, data, (size_t) bytes );
    ++dst->tail;
    ++dst->count;

    rocketnet_platform_mutex_release( &hub->mutex );
}

static void loopback_server_send_batch_impl( void * transport, const rocketnet_platform_send_t * packets, int count, bool shutdown )
{
    rocketnet_loopback_endpoint_t * sender = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_loopback_hub_t * hub = sender->hub;

    if ( shutdown && !loopback_try_send_lock( hub ) )
    {
        if ( count > 0 )
            loopback_send_failed( sender, (uint64_t) count );
        return;
    }

    if ( !shutdown )
        rocketnet_platform_mutex_acquire( &hub->mutex );

    for ( int i = 0; i < count; ++i )
    {
        const rocketnet_platform_send_t & packet = packets[i];
        if ( packet.packet_bytes <= 0 || packet.packet_bytes > ROCKETNET_RECEIVE_CEILING )
            continue;

        rocketnet_loopback_endpoint_t * dst = loopback_find_destination( hub, *packet.to );
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

    rocketnet_platform_mutex_release( &hub->mutex );
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
    rocketnet_platform_mutex_guard( &ep->hub->mutex );

    int received = 0;
    while ( received < count && ep->count > 0 )
    {
        rocketnet_loopback_packet_t * packet = &ep->packets[ep->head & ( ep->capacity - 1 )];
        if ( stride <= 0 || packet->bytes <= 0 || packet->bytes > stride )
        {
            ++ep->dropped;
            loopback_queue_pop( ep );    // proofs/tla/LoopbackQueue.tla Drop
            continue;
        }

        from[received] = packet->from;
        bytes[received] = packet->bytes;
        memcpy( data + (size_t) received * (size_t) stride, packet->data, (size_t) packet->bytes );
        loopback_queue_pop( ep );        // proofs/tla/LoopbackQueue.tla Receive
        ++received;
    }

    return received;
}

static int loopback_client_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) transport;
    rocketnet_platform_mutex_guard( &ep->hub->mutex );

    while ( ep->count > 0 )
    {
        rocketnet_loopback_packet_t * packet = &ep->packets[ep->head & ( ep->capacity - 1 )];
        if ( packet->bytes > capacity )
        {
            ++ep->dropped;
            loopback_queue_pop( ep );    // proofs/tla/LoopbackQueue.tla Drop
            continue;
        }

        *from = packet->from;
        const int bytes = packet->bytes;
        memcpy( data, packet->data, (size_t) bytes );
        loopback_queue_pop( ep );        // proofs/tla/LoopbackQueue.tla Receive

        return bytes;
    }

    return 0;
}

static const rocketnet_server_transport_io_operations_t loopback_server_operations = {
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, loopback_server_create, loopback_destroy, loopback_stop, loopback_stopped,
    NULL, NULL, NULL, NULL, NULL, loopback_server_send_batch, loopback_server_receive_batch,
    NULL, NULL, NULL, NULL, NULL, loopback_send_failures, loopback_server_send_batch_shutdown, NULL, NULL
};

// no connect: the loopback router is the hub shared by every endpoint, with
// no socket to connect; the client's own source check
// (rocketnet_client_process_packet) is the only filter on this transport
static const rocketnet_client_transport_io_operations_t loopback_client_operations = { loopback_client_create, loopback_destroy, loopback_send, loopback_client_receive, NULL, NULL, NULL, NULL, loopback_send_failures, NULL, NULL };

extern const rocketnet_transport_backend_t rocketnet_loopback_transport = { &loopback_server_operations, &loopback_client_operations };

#if ROCKETNET_DEV
uint64_t rocketnet_loopback_endpoint_dropped( void * instance )
{
    rocketnet_loopback_endpoint_t * ep = (rocketnet_loopback_endpoint_t *) instance;
    rocketnet_platform_mutex_guard( &ep->hub->mutex );

    return ep->dropped;
}

int rocketnet_loopback_endpoints_live( rocketnet_loopback_hub_t * hub )
{
    rocketnet_platform_mutex_guard( &hub->mutex );

    return hub->registered;
}
#endif // #if ROCKETNET_DEV
