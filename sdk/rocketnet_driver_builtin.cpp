/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include "rocketnet_platform.h"
#include "rocketnet_memory.h"
#include <new>
#include <atomic>
#include <string.h>

// Adapters preserve the existing native backend entry points. New backends
// can define their operations directly in their own implementation file.
// A client socket asks for ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE; a server
// socket asks for rocketnet_server_receive_buffer_request at its max_clients.
static void * portable_socket_create( void * context, const char * name, rocketnet_address_t * bind, int receive_buffer_bytes )
{
    return rocketnet_platform_socket_create_with_pump( context, bind,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE,
        receive_buffer_bytes, name );
}
// The buffered interface borrows caller packets for the duration of send_batch.
// This adapter copies them into one of three owned banks before returning. The
// TX and RX workers own only their respective banks; main never calls socket
// I/O. Portable and msgx use the same ownership, with pump selection at create.
static const int PORTABLE_TX_BANKS = 3;
static const int PORTABLE_RX_BANKS = 3;
// A TX bank holds ROCKETNET_SEND_PACKETS_PER_CLIENT packets per configured
// client, and never fewer than ROCKETNET_SERVER_MAX_SEND_PACKETS, so a burst
// queues behind the worker instead of finding every bank in flight. The banks
// are one allocation sized at create from max_clients, as the RX banks are.
static int portable_tx_capacity( const rocketnet_server_transport_config_t * config )
{
    const int max_clients = config && config->max_clients > 0 ? config->max_clients : ROCKETNET_DEFAULT_MAX_CLIENTS;
    const int capacity = ROCKETNET_SEND_PACKETS_PER_CLIENT * max_clients;
    return capacity > ROCKETNET_SERVER_MAX_SEND_PACKETS ? capacity : ROCKETNET_SERVER_MAX_SEND_PACKETS;
}
static uint64_t portable_tx_storage_bytes( int capacity )
{
    return (uint64_t) PORTABLE_TX_BANKS * (uint64_t) capacity *
        ( sizeof( rocketnet_address_t ) + sizeof( int ) + ROCKETNET_RECEIVE_CEILING + sizeof( rocketnet_platform_send_t ) );
}
// An RX bank holds one update's receive budget for the configured clients
// (16 per client), so main can take a full update from one bank and the worker
// can fill the next meanwhile. The banks are one allocation sized at create
// from max_clients: a 4-client server does not carry the 70 MB a 1000-client
// server needs.
static int portable_rx_capacity( const rocketnet_server_transport_config_t * config )
{
    const int max_clients = config && config->max_clients > 0 ? config->max_clients : ROCKETNET_DEFAULT_MAX_CLIENTS;
    return ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * max_clients;
}
static uint64_t portable_rx_storage_bytes( int capacity )
{
    return (uint64_t) PORTABLE_RX_BANKS * (uint64_t) capacity *
        ( sizeof( rocketnet_address_t ) + sizeof( int ) + ROCKETNET_RECEIVE_CEILING );
}
// Every packet the send handoff drops is counted by the path that dropped it,
// apart from the socket's own send failures; SEND_FAILED reads the sum, and
// the transport logs each path's count when it closes.
static const int PORTABLE_TX_DROP_NO_FREE_BANK = 0;  // the producer bank is full and every other bank is in flight
static const int PORTABLE_TX_DROP_HANDOFF_BUSY = 1;  // the producer bank is full and the handoff lock was held
static const int PORTABLE_TX_DROP_STOPPING = 2;      // sent after stop, or unpublished at stop
static const int PORTABLE_TX_DROP_INVALID = 3;       // no address, no data, or a size outside the datagram ceiling
static const int PORTABLE_TX_DROP_PATHS = 4;
static const char * const portable_tx_drop_path_name[PORTABLE_TX_DROP_PATHS] =
    { "no-free-bank", "handoff-busy", "stopping", "invalid-packet" };
struct portable_tx_bank_t
{
    int count = 0;
    rocketnet_address_t * to = NULL;                    // tx_capacity entries in tx_storage
    int * bytes = NULL;
    uint8_t (*data)[ROCKETNET_RECEIVE_CEILING] = NULL;
    rocketnet_platform_send_t * sends = NULL;           // worker-owned batch descriptors
};
struct portable_rx_bank_t
{
    int count = 0;
    rocketnet_address_t * from = NULL;                  // rx_capacity entries in rx_storage
    int * bytes = NULL;
    uint8_t (*data)[ROCKETNET_RECEIVE_CEILING] = NULL;
};
struct portable_server_t
{
    void * context = NULL;
    rocketnet_platform_socket_t * socket = NULL;
    rocketnet_platform_wake_t * wake = NULL;
    rocketnet_platform_thread_t * thread = NULL;
    rocketnet_platform_mutex_t mutex = {};
    rocketnet_platform_wake_t * rx_wake = NULL;
    rocketnet_platform_thread_t * rx_thread = NULL;
    rocketnet_platform_mutex_t rx_mutex = {};
    std::atomic<bool> stopping{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> rx_exited{false};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> dropped_by_path[PORTABLE_TX_DROP_PATHS] = {}; // the paths of dropped, by PORTABLE_TX_DROP_*
    int publish_refusal = PORTABLE_TX_DROP_HANDOFF_BUSY; // main only: why the last publish try failed
#if defined(ROCKETNET_PORTABLE_TX_TEST_HOOKS)
    bool stop_publish_saw_quit = false;
    bool force_publish_busy = false;
    bool force_rx_acquire_busy = false;
    void (*after_worker_empty)( portable_server_t * ) = NULL;
    bool skip_socket_send = false;
    std::atomic<int> test_sent_banks{0};
    std::atomic<int> test_rx_full_waits{0};
#endif
    int producer = 0; // main thread only
    int free_index[PORTABLE_TX_BANKS] = {1, 2, 0};
    int free_head = 0;
    int free_count = PORTABLE_TX_BANKS - 1;
    int ready_index[PORTABLE_TX_BANKS] = {};
    int ready_head = 0;
    int ready_count = 0;
    portable_tx_bank_t bank[PORTABLE_TX_BANKS];
    int tx_capacity = 0;
    uint8_t * tx_storage = NULL; // the three TX banks' arrays, one allocation
    int rx_producer = 0; // RX worker only
    int rx_consumer = -1; // main only, retained until completely consumed
    int rx_cursor = 0; // main only
    int rx_free_index[PORTABLE_RX_BANKS] = {1, 2, 0};
    int rx_free_head = 0;
    int rx_free_count = PORTABLE_RX_BANKS - 1;
    int rx_ready_index[PORTABLE_RX_BANKS] = {};
    int rx_ready_head = 0;
    int rx_ready_count = 0;
    int rx_capacity = 0;
    uint8_t * rx_storage = NULL; // the three banks' arrays, one allocation
    portable_rx_bank_t rx_bank[PORTABLE_RX_BANKS];
};

// Points the three banks into one allocation of capacity datagrams each:
// the data arrays first (aligned as the allocation is), then the addresses,
// then the byte counts.
static bool portable_rx_storage_create( portable_server_t * server, int capacity )
{
    const uint64_t bytes = portable_rx_storage_bytes( capacity );
    if ( capacity <= 0 || bytes > (uint64_t) SIZE_MAX ) return false;
    server->rx_storage = (uint8_t*) rocketnet_malloc( server->context, (size_t) bytes );
    if ( !server->rx_storage ) return false;
    server->rx_capacity = capacity;
    uint8_t * p = server->rx_storage;
    for ( int i = 0; i < PORTABLE_RX_BANKS; ++i )
    {
        server->rx_bank[i].data = (uint8_t (*)[ROCKETNET_RECEIVE_CEILING]) p;
        p += (size_t) capacity * ROCKETNET_RECEIVE_CEILING;
    }
    for ( int i = 0; i < PORTABLE_RX_BANKS; ++i )
    {
        server->rx_bank[i].from = (rocketnet_address_t*) p;
        p += (size_t) capacity * sizeof( rocketnet_address_t );
    }
    for ( int i = 0; i < PORTABLE_RX_BANKS; ++i )
    {
        server->rx_bank[i].bytes = (int*) p;
        p += (size_t) capacity * sizeof( int );
    }
    return true;
}

// Points the three TX banks into one allocation of capacity packets each: the
// worker's send descriptors first (pointer aligned, as the allocation is),
// then the data arrays, the addresses and the byte counts, as the RX banks.
static bool portable_tx_storage_create( portable_server_t * server, int capacity )
{
    const uint64_t bytes = portable_tx_storage_bytes( capacity );
    if ( capacity <= 0 || bytes > (uint64_t) SIZE_MAX ) return false;
    server->tx_storage = (uint8_t*) rocketnet_malloc( server->context, (size_t) bytes );
    if ( !server->tx_storage ) return false;
    server->tx_capacity = capacity;
    uint8_t * p = server->tx_storage;
    for ( int i = 0; i < PORTABLE_TX_BANKS; ++i )
    {
        server->bank[i].sends = (rocketnet_platform_send_t*) p;
        p += (size_t) capacity * sizeof( rocketnet_platform_send_t );
    }
    for ( int i = 0; i < PORTABLE_TX_BANKS; ++i )
    {
        server->bank[i].data = (uint8_t (*)[ROCKETNET_RECEIVE_CEILING]) p;
        p += (size_t) capacity * ROCKETNET_RECEIVE_CEILING;
    }
    for ( int i = 0; i < PORTABLE_TX_BANKS; ++i )
    {
        server->bank[i].to = (rocketnet_address_t*) p;
        p += (size_t) capacity * sizeof( rocketnet_address_t );
    }
    for ( int i = 0; i < PORTABLE_TX_BANKS; ++i )
    {
        server->bank[i].bytes = (int*) p;
        p += (size_t) capacity * sizeof( int );
    }
    return true;
}

// Main only: counts packets dropped on one path into SEND_FAILED and into
// that path's count. The first drop on each path logs once; later ones are
// only counted, and the transport logs every path's count when it closes.
static void portable_tx_drop( portable_server_t * server, int path, uint64_t packets )
{
    if ( packets == 0 ) return;
    server->dropped.fetch_add( packets, std::memory_order_relaxed );
    if ( server->dropped_by_path[path].fetch_add( packets, std::memory_order_relaxed ) == 0 )
        rocketnet_info( "server send handoff dropped %llu packets (%s): first of its kind; later ones are counted and logged when the transport closes",
                        (unsigned long long) packets, portable_tx_drop_path_name[path] );
}

// The only shared ownership transition. No copy, allocation or socket call
// occurs with the mutex held. A failed try retains the main-owned producer
// and records why in publish_refusal.
static bool portable_tx_publish( portable_server_t * server )
{
    if ( server->bank[server->producer].count == 0 ) return true;
    server->publish_refusal = PORTABLE_TX_DROP_HANDOFF_BUSY;
#if defined(ROCKETNET_PORTABLE_TX_TEST_HOOKS)
    if ( server->stopping.load( std::memory_order_relaxed ) && server->quit.load( std::memory_order_relaxed ) )
        server->stop_publish_saw_quit = true;
    if ( server->force_publish_busy ) return false;
#endif
    if ( !rocketnet_platform_mutex_try_acquire( &server->mutex ) ) return false;
    if ( server->free_count == 0 )
    {
        rocketnet_platform_mutex_release( &server->mutex );
        server->publish_refusal = PORTABLE_TX_DROP_NO_FREE_BANK;
        return false;
    }
    const int old = server->producer;
    server->producer = server->free_index[server->free_head];
    server->free_head = ( server->free_head + 1 ) % PORTABLE_TX_BANKS;
    server->free_count--;
    server->ready_index[( server->ready_head + server->ready_count ) % PORTABLE_TX_BANKS] = old;
    server->ready_count++;
    rocketnet_platform_mutex_release( &server->mutex );
    server->bank[server->producer].count = 0;
    rocketnet_platform_wake_set( server->wake );
    return true;
}

#if defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
static void portable_tx_worker( void * arg )
{
    portable_server_t * server = (portable_server_t*) arg;
    for ( ;; )
    {
        rocketnet_platform_mutex_acquire( &server->mutex );
        int index = -1;
        bool exit_empty = false;
        if ( server->ready_count > 0 )
        {
            index = server->ready_index[server->ready_head];
            server->ready_head = ( server->ready_head + 1 ) % PORTABLE_TX_BANKS;
            server->ready_count--;
        }
        else
            exit_empty = server->quit.load( std::memory_order_acquire );
        rocketnet_platform_mutex_release( &server->mutex );
        if ( index < 0 )
        {
#if defined(ROCKETNET_PORTABLE_TX_TEST_HOOKS)
            if ( server->after_worker_empty ) server->after_worker_empty( server );
#endif
            // Empty and quit were observed together under the handoff lock.
            // A stop publication after an earlier empty observation must win.
            if ( exit_empty ) break;
            rocketnet_platform_wake_wait( server->wake, false, 0.1 );
            continue;
        }
        portable_tx_bank_t * bank = &server->bank[index];
#if defined(ROCKETNET_PORTABLE_TX_TEST_HOOKS)
        if ( server->skip_socket_send )
            server->test_sent_banks.fetch_add( 1, std::memory_order_relaxed );
        else
#endif
        {
            for ( int i = 0; i < bank->count; ++i )
                bank->sends[i] = { &bank->to[i], bank->data[i], bank->bytes[i] };
            rocketnet_platform_socket_send_packets( server->socket, bank->sends, bank->count );
        }
        bank->count = 0;
        rocketnet_platform_mutex_acquire( &server->mutex );
        server->free_index[( server->free_head + server->free_count ) % PORTABLE_TX_BANKS] = index;
        server->free_count++;
        rocketnet_platform_mutex_release( &server->mutex );
    }
    server->exited.store( true, std::memory_order_release );
}
#endif

// The RX handoff (tla/RNPortableRx.tla). The worker owns one producer bank
// and reads the socket into it only when it is empty; it publishes a filled
// producer onto the ready FIFO in exchange for a free bank. Main owns at most
// one consumer bank and a cursor, and returns the consumer only when it is
// completely copied. Only queue indices change under rx_mutex: the socket
// read, the copies and the producer reset run outside it, and main only ever
// tries the mutex.

// tla/RNPortableRx.tla WPublishEmpty, WLock, WSwap and WReset: true when
// the producer is empty or was queued for a free bank. Without a free bank
// the worker keeps its full producer (HoldOnFull) and stops reading the
// socket.
static bool portable_rx_publish( portable_server_t * server )
{
    if ( server->rx_bank[server->rx_producer].count == 0 ) return true;
    rocketnet_platform_mutex_acquire( &server->rx_mutex );
    if ( server->rx_free_count == 0 )
    {
        rocketnet_platform_mutex_release( &server->rx_mutex );
        return false;
    }
    const int old = server->rx_producer;
    server->rx_producer = server->rx_free_index[server->rx_free_head];
    server->rx_free_head = ( server->rx_free_head + 1 ) % PORTABLE_RX_BANKS;
    server->rx_free_count--;
    server->rx_ready_index[( server->rx_ready_head + server->rx_ready_count ) % PORTABLE_RX_BANKS] = old;
    server->rx_ready_count++;
    rocketnet_platform_mutex_release( &server->rx_mutex );
    server->rx_bank[server->rx_producer].count = 0; // RNPortableRx WReset
    return true;
}

// tla/RNPortableRx.tla MTry, MTryFail, MSwap and MWake: true while main
// owns a consumer with datagrams left to copy. A completely copied consumer
// is returned and the oldest ready bank taken under one try; a failed try
// keeps the consumer and its cursor and ends the call. Main wakes the worker only when it returns a bank to an empty
// free FIFO: only then can the worker be waiting for one (WakeOnReturn).
static bool portable_rx_acquire( portable_server_t * server )
{
    if ( server->rx_consumer >= 0 && server->rx_cursor < server->rx_bank[server->rx_consumer].count )
        return true;
#if defined(ROCKETNET_PORTABLE_TX_TEST_HOOKS)
    if ( server->force_rx_acquire_busy ) return false;
#endif
    if ( !rocketnet_platform_mutex_try_acquire( &server->rx_mutex ) ) return false;
    const bool freed = server->rx_consumer >= 0 && server->rx_free_count == 0;
    if ( server->rx_consumer >= 0 )
    {
        server->rx_free_index[( server->rx_free_head + server->rx_free_count ) % PORTABLE_RX_BANKS] = server->rx_consumer;
        server->rx_free_count++;
        server->rx_consumer = -1;
    }
    if ( server->rx_ready_count > 0 )
    {
        server->rx_consumer = server->rx_ready_index[server->rx_ready_head];
        server->rx_ready_head = ( server->rx_ready_head + 1 ) % PORTABLE_RX_BANKS;
        server->rx_ready_count--;
    }
    rocketnet_platform_mutex_release( &server->rx_mutex );
    server->rx_cursor = 0;
    if ( freed ) rocketnet_platform_wake_set( server->rx_wake );
    return server->rx_consumer >= 0;
}

enum portable_rx_step_t
{
    PORTABLE_RX_EXIT,
    PORTABLE_RX_AGAIN,
    PORTABLE_RX_WAIT_FULL,
    PORTABLE_RX_WAIT_IDLE,
};

// One pass of the RX worker loop, without its waits (tla/RNPortableRx.tla WTop,
// publish and WRead). Quit is read at the top of every pass, so a stop that
// sets quit before its wake (QuitBeforeWake) always ends the worker.
static portable_rx_step_t portable_rx_step( portable_server_t * server )
{
    if ( server->quit.load( std::memory_order_acquire ) )
        return PORTABLE_RX_EXIT;
    if ( !portable_rx_publish( server ) )
    {
#if defined(ROCKETNET_PORTABLE_TX_TEST_HOOKS)
        server->test_rx_full_waits.fetch_add( 1, std::memory_order_relaxed );
#endif
        return PORTABLE_RX_WAIT_FULL;
    }
    portable_rx_bank_t * bank = &server->rx_bank[server->rx_producer];
    bank->count = rocketnet_platform_socket_receive_packets( server->socket,
        bank->from, &bank->data[0][0], ROCKETNET_RECEIVE_CEILING, bank->bytes, server->rx_capacity );
    if ( bank->count < 0 ) bank->count = 0;
    return bank->count > 0 ? PORTABLE_RX_AGAIN : PORTABLE_RX_WAIT_IDLE;
}

#if defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
// tla/RNPortableRx.tla WWaitFull and WWaitIdle: the worker waits on its wake
// alone when every bank is full, and on the wake or socket readability
// after an empty read.
static void portable_rx_worker( void * arg )
{
    portable_server_t * server = (portable_server_t*) arg;
    for ( ;; )
    {
        const portable_rx_step_t step = portable_rx_step( server );
        if ( step == PORTABLE_RX_EXIT ) break;
        if ( step == PORTABLE_RX_WAIT_FULL )
        {
            // Full banks: readability cannot help. A bank returned to an empty
            // free FIFO or stop signals this wake; the bounded timeout also
            // tolerates a lost hint.
            rocketnet_platform_wake_wait( server->rx_wake, false, 0.1 );
        }
        else if ( step == PORTABLE_RX_WAIT_IDLE )
        {
#if defined(ROCKETNET_PLATFORM_HAS_WAKE)
            rocketnet_platform_wake_wait( server->rx_wake, true, 0.1 );
#else
            // BSD has event-only wakes; preserve its supported portable transport.
            rocketnet_platform_wake_wait( server->rx_wake, false, 0.001 );
#endif
        }
    }
    server->rx_exited.store( true, std::memory_order_release );
}
#endif

static void portable_server_cleanup( portable_server_t * server )
{
    // Also handles RX creation failure after TX has already started.
    server->quit.store( true, std::memory_order_release );
    if ( server->wake ) rocketnet_platform_wake_set( server->wake );
    if ( server->rx_wake ) rocketnet_platform_wake_set( server->rx_wake );
    if ( server->thread )
    {
        rocketnet_platform_thread_join( server->thread );
        rocketnet_platform_thread_destroy( server->thread );
    }
    if ( server->rx_thread )
    {
        rocketnet_platform_thread_join( server->rx_thread );
        rocketnet_platform_thread_destroy( server->rx_thread );
    }
    if ( server->rx_wake ) rocketnet_platform_wake_destroy( server->rx_wake );
    if ( server->rx_mutex.ok ) rocketnet_platform_mutex_destroy( &server->rx_mutex );
    if ( server->wake ) rocketnet_platform_wake_destroy( server->wake );
    if ( server->mutex.ok ) rocketnet_platform_mutex_destroy( &server->mutex );
    if ( server->socket )
    {
        // every drop path once, zeros included, beside the socket's own failures
        rocketnet_info( "server send handoff drops: %s %llu, %s %llu, %s %llu, %s %llu packets; socket send failures %llu",
            portable_tx_drop_path_name[PORTABLE_TX_DROP_NO_FREE_BANK], (unsigned long long) server->dropped_by_path[PORTABLE_TX_DROP_NO_FREE_BANK].load(),
            portable_tx_drop_path_name[PORTABLE_TX_DROP_HANDOFF_BUSY], (unsigned long long) server->dropped_by_path[PORTABLE_TX_DROP_HANDOFF_BUSY].load(),
            portable_tx_drop_path_name[PORTABLE_TX_DROP_STOPPING], (unsigned long long) server->dropped_by_path[PORTABLE_TX_DROP_STOPPING].load(),
            portable_tx_drop_path_name[PORTABLE_TX_DROP_INVALID], (unsigned long long) server->dropped_by_path[PORTABLE_TX_DROP_INVALID].load(),
            (unsigned long long) rocketnet_platform_socket_send_failures( server->socket ) );
        rocketnet_platform_socket_destroy( server->socket );
    }
    void * context = server->context;
    // the receive banks hold datagrams, never a secret: freed as they are,
    // so destroy touches no bank page the run did not (tla/RNDestroyWipe.tla)
    if ( server->rx_storage )
        rocketnet_free( context, server->rx_storage );
    if ( server->tx_storage )
        rocketnet_free( context, server->tx_storage );
    server->~portable_server_t();
    rocketnet_free( context, server );
}

static void * portable_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
#if !defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    (void) context; (void) config; (void) bind;
    rocketnet_warn( "server I/O workers require an event-only wake on this platform" );
    return NULL;
#else
#if defined( __OpenBSD__ )
    if ( config->transport && config->transport->second_public_address[0] != '\0' )
    {
        if ( config->status )
            *config->status = ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED;
        rocketnet_error( "server transport portable cannot serve both families on this platform: IPV6_V6ONLY cannot be cleared" );
        return NULL;
    }
#else
    (void) config;
#endif
    portable_server_t * server = (portable_server_t*) rocketnet_malloc( context, sizeof( portable_server_t ) );
    if ( !server ) return NULL;
    new (server) portable_server_t();
    server->context = context;
    if ( !portable_rx_storage_create( server, portable_rx_capacity( config->transport ) ) ||
         !portable_tx_storage_create( server, portable_tx_capacity( config->transport ) ) )
    {
        portable_server_cleanup( server );
        return NULL;
    }
    // one update's inbound packets at max_clients: the RX worker stops
    // reading while every bank waits for main (RNPortableRx HoldOnFull), so
    // the kernel holds whatever arrives until the next update
    const int max_clients = config->transport && config->transport->max_clients > 0 ? config->transport->max_clients : ROCKETNET_DEFAULT_MAX_CLIENTS;
    const int receive_buffer_bytes = rocketnet_server_receive_buffer_request( max_clients );
    server->socket = (rocketnet_platform_socket_t*) portable_socket_create( context, config->name, bind, receive_buffer_bytes );
    if ( !server->socket )
    {
        portable_server_cleanup( server );
        return NULL;
    }
    rocketnet_server_receive_buffer_granted( config->name, receive_buffer_bytes,
        rocketnet_platform_socket_receive_buffer_bytes( server->socket ) );
    if ( !rocketnet_platform_mutex_create( &server->mutex ) ||
         !rocketnet_platform_mutex_create( &server->rx_mutex ) )
    {
        if ( config->status ) *config->status = ROCKETNET_STATUS_MUTEX_CREATE_FAILED;
        portable_server_cleanup( server );
        return NULL;
    }
    server->wake = rocketnet_platform_wake_create( context, NULL );
#if defined(ROCKETNET_PLATFORM_HAS_WAKE)
    server->rx_wake = rocketnet_platform_wake_create( context, server->socket );
#else
    server->rx_wake = rocketnet_platform_wake_create( context, NULL );
#endif
    if ( !server->wake || !server->rx_wake )
    {
        portable_server_cleanup( server );
        return NULL;
    }
    server->thread = rocketnet_platform_thread_create( context, portable_tx_worker, server );
    if ( !server->thread )
    {
        if ( config->status ) *config->status = ROCKETNET_STATUS_THREAD_CREATE_FAILED;
        portable_server_cleanup( server );
        return NULL;
    }
    server->rx_thread = rocketnet_platform_thread_create( context, portable_rx_worker, server );
    if ( !server->rx_thread )
    {
        if ( config->status ) *config->status = ROCKETNET_STATUS_THREAD_CREATE_FAILED;
        portable_server_cleanup( server );
        return NULL;
    }
    return server;
#endif
}
static void portable_destroy( void * transport ) { portable_server_cleanup( (portable_server_t*) transport ); }
#if !defined( ROCKETNET_PLATFORM_HAS_WAKE )
// The synchronous portable client below is the only user of the bare socket.
static void portable_socket_destroy( void * transport ) { rocketnet_platform_socket_destroy( (rocketnet_platform_socket_t*) transport ); }
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && !ROCKETNET_XDP
// The disabled xdp transport below is the only synchronous server transport.
static void synchronous_stop( void * ) {}
static bool synchronous_stopped( void * ) { return true; }
#endif
static void portable_send( void * transport, const rocketnet_platform_send_t * packets, int count )
{
    portable_server_t * server = (portable_server_t*) transport;
    for ( int i = 0; i < count; ++i )
    {
        if ( server->stopping.load( std::memory_order_acquire ) )
        {
            portable_tx_drop( server, PORTABLE_TX_DROP_STOPPING, (uint64_t)( count - i ) );
            return;
        }
        portable_tx_bank_t * bank = &server->bank[server->producer];
        if ( bank->count == server->tx_capacity && !portable_tx_publish( server ) )
        {
            portable_tx_drop( server, server->publish_refusal, (uint64_t)( count - i ) );
            return;
        }
        bank = &server->bank[server->producer];
        const rocketnet_platform_send_t * packet = &packets[i];
        if ( !packet->to || !packet->packet_data || packet->packet_bytes <= 0 || packet->packet_bytes > ROCKETNET_RECEIVE_CEILING )
        {
            portable_tx_drop( server, PORTABLE_TX_DROP_INVALID, 1 );
            continue;
        }
        const int slot = bank->count;
        bank->to[slot] = *packet->to;
        memcpy( bank->data[slot], packet->packet_data, (size_t) packet->packet_bytes );
        bank->bytes[slot] = packet->packet_bytes;
        bank->count++;
    }
    portable_tx_publish( server );
}
// tla/RNPortableRx.tla MCall, MCopy and MEnd: a call copies datagrams from
// the consumer at its cursor within the caller's count and returns when the
// count is spent or no consumer is left; a stopping server returns nothing.
static int portable_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    portable_server_t * server = (portable_server_t*) transport;
    portable_tx_publish( server ); // retry a deferred batch even on an idle update
    if ( count <= 0 || stride <= 0 || server->stopping.load( std::memory_order_acquire ) ) return 0;
    int received = 0;
    while ( received < count && portable_rx_acquire( server ) )
    {
        const portable_rx_bank_t * bank = &server->rx_bank[server->rx_consumer];
        int n = bank->count - server->rx_cursor;
        if ( n > count - received ) n = count - received;
        for ( int i = 0; i < n; ++i )
        {
            const int source = server->rx_cursor + i;
            const int target = received + i;
            from[target] = bank->from[source];
            bytes[target] = bank->bytes[source]; // preserve the oversize marker
            int copy = bank->bytes[source];
            if ( copy > ROCKETNET_RECEIVE_CEILING ) copy = ROCKETNET_RECEIVE_CEILING;
            if ( copy > stride ) copy = stride;
            if ( copy > 0 ) memcpy( data + (size_t) target * (size_t) stride, bank->data[source], (size_t) copy );
        }
        server->rx_cursor += n;
        received += n;
    }
    return received;
}
// tla/RNPortableRx.tla MStop, MStop1 and MStop2: stopping, then quit, then
// the RX wake (QuitBeforeWake).
static void portable_stop( void * transport )
{
    portable_server_t * server = (portable_server_t*) transport;
    if ( server->stopping.exchange( true, std::memory_order_acq_rel ) ) return;
    // The worker never reads the main-owned unpublished producer. If the
    // final try cannot publish it, account and drop it before completion.
    if ( !portable_tx_publish( server ) )
    {
        portable_tx_drop( server, PORTABLE_TX_DROP_STOPPING, (uint64_t)server->bank[server->producer].count );
        server->bank[server->producer].count = 0;
    }
    // Publish or drop the main-owned bank before allowing an empty-ready
    // worker to exit. Releasing quit before publication would lose this batch.
    server->quit.store( true, std::memory_order_release );
    rocketnet_platform_wake_set( server->wake );
    if ( server->rx_wake ) rocketnet_platform_wake_set( server->rx_wake );
}
// tla/RNPortableRx.tla MPollStopped: main polls between frames, and nothing
// changes until the workers have exited.
static bool portable_stopped( void * transport )
{
    portable_server_t * server = (portable_server_t*) transport;
    return server->exited.load( std::memory_order_acquire ) && server->rx_exited.load( std::memory_order_acquire );
}
static uint64_t portable_send_failures( void * transport )
{
    portable_server_t * server = (portable_server_t*) transport;
    return rocketnet_platform_socket_send_failures( server->socket ) + server->dropped.load( std::memory_order_relaxed );
}
#if !defined( ROCKETNET_PLATFORM_HAS_WAKE )
static uint64_t portable_socket_send_failures( void * transport )
{ return rocketnet_platform_socket_send_failures( (rocketnet_platform_socket_t*) transport ); }
#endif
#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
// development library only. Sends leave the main thread when the worker takes
// a published bank, so a functional case that injects socket send errors
// waits for this before it clears the injection: true when nothing is
// unpublished and every published bank has been sent and returned free.
bool rocketnet_portable_tx_test_drained( void * transport )
{
    portable_server_t * server = (portable_server_t*) transport;
    if ( server->bank[server->producer].count != 0 )
        return false;
    rocketnet_platform_mutex_acquire( &server->mutex );
    const bool drained = server->free_count == PORTABLE_TX_BANKS - 1;
    rocketnet_platform_mutex_release( &server->mutex );
    return drained;
}
#endif
#if ROCKETNET_DEV
// development library only: the platform socket under a portable or msgx
// server, for the functional cases that read its pump or send raw datagrams
// from the server's own address. The transport instance is the
// portable_server_t, never the socket itself.
rocketnet_platform_socket_t * rocketnet_portable_server_test_socket( void * transport )
{
    return ( (portable_server_t*) transport )->socket;
}
#endif
#if defined(ROCKETNET_PORTABLE_TX_TEST_HOOKS)
// No socket or worker: each path is forced at an exact ownership transition.
bool rocketnet_portable_tx_test_handoff()
{
#if !defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    return true;
#else
    // Exercise the same cleanup entry with each partially constructed owner.
    // Sanitizer/allocator gates can detect a leaked wake or bank allocation.
    for ( int stage = 0; stage < 3; ++stage )
    {
        void * partial_storage = rocketnet_malloc( NULL, sizeof( portable_server_t ) );
        if ( !partial_storage ) return false;
        portable_server_t * partial = new (partial_storage) portable_server_t();
        if ( stage >= 1 && !rocketnet_platform_mutex_create( &partial->mutex ) )
        {
            portable_server_cleanup( partial );
            return false;
        }
        if ( stage >= 2 )
        {
            partial->wake = rocketnet_platform_wake_create( NULL, NULL );
            if ( !partial->wake )
            {
                portable_server_cleanup( partial );
                return false;
            }
        }
        portable_server_cleanup( partial );
    }
    void * storage = rocketnet_malloc( NULL, sizeof( portable_server_t ) );
    if ( !storage ) return false;
    portable_server_t * server = new (storage) portable_server_t();
    if ( !portable_tx_storage_create( server, ROCKETNET_SERVER_MAX_SEND_PACKETS ) ||
         !rocketnet_platform_mutex_create( &server->mutex ) )
    {
        portable_server_cleanup( server );
        return false;
    }
    server->wake = rocketnet_platform_wake_create( NULL, NULL );
    if ( !server->wake )
    {
        portable_server_cleanup( server );
        return false;
    }
    rocketnet_address_t to = {};
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.port = 1234;
    uint8_t bytes[4] = { 1, 2, 3, 4 };
    rocketnet_platform_send_t send = { &to, bytes, 4 };
    portable_send( server, &send, 1 );
    bytes[0] = 99;
    to.port = 5678;
    bool valid = server->ready_count == 1 && server->bank[0].count == 1 &&
        server->bank[0].data[0][0] == 1 && server->bank[0].to[0].port == 1234;
    // A contended publish retains the complete producer; a later try moves
    // it, and the worker may inspect bank 0 independently of caller storage.
    server->force_publish_busy = true;
    portable_send( server, &send, 1 );
    valid &= server->producer == 1 && server->bank[1].count == 1;
    server->force_publish_busy = false;
    valid &= portable_tx_publish( server ) && server->ready_count == 2;
    // Stop must expose the last bank before quit permits an empty-ready exit.
    portable_send( server, &send, 1 );
    valid &= server->bank[2].count == 1 && server->ready_count == 2;
    for ( int i = 1; i < server->tx_capacity; ++i ) portable_send( server, &send, 1 );
    portable_send( server, &send, 1 ); // full producer and no free bank: one drop
    valid &= server->bank[2].count == server->tx_capacity && server->dropped.load() == 1;
    portable_stop( server );
    valid &= !server->stop_publish_saw_quit && server->quit.load() &&
        server->ready_count == 2 && server->bank[2].count == 0 &&
        server->dropped.load() == (uint64_t) server->tx_capacity + 1;
    portable_server_cleanup( server );

    // Exact former race: worker observes empty, pauses after releasing the
    // handoff mutex, stop publishes the last bank and sets quit, then worker
    // resumes its exit decision. A post-unlock quit read would strand bank 0.
    void * race_storage = rocketnet_malloc( NULL, sizeof( portable_server_t ) );
    if ( !race_storage ) return false;
    portable_server_t * race = new (race_storage) portable_server_t();
    if ( !rocketnet_platform_mutex_create( &race->mutex ) )
    {
        portable_server_cleanup( race );
        return false;
    }
    race->wake = rocketnet_platform_wake_create( NULL, NULL );
    if ( !race->wake )
    {
        portable_server_cleanup( race );
        return false;
    }
    race->bank[0].count = 1;
    race->skip_socket_send = true;
    race->after_worker_empty = []( portable_server_t * owner )
    {
        owner->after_worker_empty = NULL;
        portable_stop( owner );
    };
    race->thread = rocketnet_platform_thread_create( NULL, portable_tx_worker, race );
    if ( !race->thread )
    {
        portable_server_cleanup( race );
        return false;
    }
    rocketnet_platform_thread_join( race->thread );
    valid &= race->exited.load( std::memory_order_acquire ) && race->test_sent_banks.load() == 1 &&
        !race->stop_publish_saw_quit && race->dropped.load() == 0;
    rocketnet_platform_thread_destroy( race->thread );
    race->thread = NULL;
    portable_server_cleanup( race );
    return valid;
#endif
}

// A TX owner of the given bank capacity with no worker and no socket: the
// caller plays the worker by taking ready banks itself.
static portable_server_t * portable_tx_test_owner( int capacity )
{
    void * storage = rocketnet_malloc( NULL, sizeof( portable_server_t ) );
    if ( !storage ) return NULL;
    portable_server_t * server = new (storage) portable_server_t();
    if ( !portable_tx_storage_create( server, capacity ) ||
         !rocketnet_platform_mutex_create( &server->mutex ) )
    {
        portable_server_cleanup( server );
        return NULL;
    }
    server->wake = rocketnet_platform_wake_create( NULL, NULL );
    if ( !server->wake )
    {
        portable_server_cleanup( server );
        return NULL;
    }
    return server;
}

// The burst the bank count could not hold: the worker is busy with one bank
// and another waits, while main sends updates of 1100 packets. Returns the
// packets dropped for want of a free bank.
static uint64_t portable_tx_test_burst( portable_server_t * server, int updates )
{
    rocketnet_address_t to = {};
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.port = 1234;
    uint8_t bytes[4] = { 1, 2, 3, 4 };
    rocketnet_platform_send_t sends[1100];
    for ( int i = 0; i < 1100; ++i ) sends[i] = { &to, bytes, 4 };
    // the worker takes the first published bank and keeps it in flight
    portable_send( server, sends, 1 );
    server->ready_head = ( server->ready_head + 1 ) % PORTABLE_TX_BANKS;
    server->ready_count--;
    for ( int update = 0; update < updates; ++update )
        portable_send( server, sends, 1100 );
    return server->dropped.load();
}

// Sizing: one bank holds ROCKETNET_SEND_PACKETS_PER_CLIENT per configured
// client, never fewer than ROCKETNET_SERVER_MAX_SEND_PACKETS, and the memory
// plan counts the allocation. A burst of three updates of 1100 packets behind
// a busy worker drops at the former fixed 1000-packet banks and drops nothing
// at the banks sized for 1000 clients.
bool rocketnet_portable_tx_test_sizing()
{
#if !defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    return true;
#else
    rocketnet_server_transport_config_t config = {};
    bool valid = portable_tx_capacity( NULL ) == ROCKETNET_SEND_PACKETS_PER_CLIENT * ROCKETNET_DEFAULT_MAX_CLIENTS;
    config.max_clients = 1;
    valid &= portable_tx_capacity( &config ) == ROCKETNET_SERVER_MAX_SEND_PACKETS;
    config.max_clients = ROCKETNET_SERVER_MAX_SEND_PACKETS / ROCKETNET_SEND_PACKETS_PER_CLIENT + 1;
    valid &= portable_tx_capacity( &config ) == ROCKETNET_SEND_PACKETS_PER_CLIENT * config.max_clients;
    config.max_clients = 1000;
    const int sized = portable_tx_capacity( &config );
    valid &= sized == ROCKETNET_SEND_PACKETS_PER_CLIENT * 1000;
    valid &= portable_tx_storage_bytes( sized ) == (uint64_t) PORTABLE_TX_BANKS * (uint64_t) sized *
        ( sizeof( rocketnet_address_t ) + sizeof( int ) + ROCKETNET_RECEIVE_CEILING + sizeof( rocketnet_platform_send_t ) );
    valid &= portable_tx_storage_bytes( 0 ) == 0;

    portable_server_t * fixed = portable_tx_test_owner( ROCKETNET_SERVER_MAX_SEND_PACKETS );
    if ( !fixed ) return false;
    valid &= portable_tx_test_burst( fixed, 3 ) > 0;
    portable_server_cleanup( fixed );

    portable_server_t * server = portable_tx_test_owner( sized );
    if ( !server ) return false;
    valid &= portable_tx_test_burst( server, 3 ) == 0;
    // every packet is queued: one in the worker's bank, the rest behind it
    int queued = 0;
    for ( int i = 0; i < PORTABLE_TX_BANKS; ++i ) queued += server->bank[i].count;
    valid &= queued == 1 + 3 * 1100;
    portable_server_cleanup( server );
    return valid;
#endif
}

// Drop paths: each way the handoff drops a packet is counted on its own path,
// and SEND_FAILED's handoff part is their sum. No worker and no socket: the
// test takes a ready bank by hand to keep it in flight.
bool rocketnet_portable_tx_test_drop_paths()
{
#if !defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    return true;
#else
    portable_server_t * server = portable_tx_test_owner( ROCKETNET_SERVER_MAX_SEND_PACKETS );
    if ( !server ) return false;
    rocketnet_address_t to = {};
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.port = 1234;
    uint8_t bytes[4] = { 1, 2, 3, 4 };
    const rocketnet_platform_send_t send = { &to, bytes, 4 };
    const rocketnet_platform_send_t empty = { &to, bytes, 0 };
    portable_send( server, &empty, 1 );
    bool valid = server->dropped_by_path[PORTABLE_TX_DROP_INVALID].load() == 1;
    // one bank in flight, one ready, and a full producer: no free bank
    portable_send( server, &send, 1 );
    server->ready_head = ( server->ready_head + 1 ) % PORTABLE_TX_BANKS;
    server->ready_count--;
    portable_send( server, &send, 1 );
    for ( int i = 0; i < server->tx_capacity; ++i ) portable_send( server, &send, 1 );
    valid &= server->bank[server->producer].count == server->tx_capacity && server->dropped.load() == 1;
    portable_send( server, &send, 1 );
    portable_send( server, &send, 1 );
    valid &= server->dropped_by_path[PORTABLE_TX_DROP_NO_FREE_BANK].load() == 2;
    // the worker returns a bank, but the handoff lock is held: handoff busy
    server->free_index[( server->free_head + server->free_count ) % PORTABLE_TX_BANKS] = 0;
    server->free_count++;
    server->force_publish_busy = true;
    portable_send( server, &send, 1 );
    valid &= server->dropped_by_path[PORTABLE_TX_DROP_HANDOFF_BUSY].load() == 1;
    server->force_publish_busy = false;
    // stop publishes the full producer into the free bank; later sends drop
    portable_stop( server );
    valid &= server->bank[server->producer].count == 0 && server->dropped_by_path[PORTABLE_TX_DROP_STOPPING].load() == 0;
    rocketnet_platform_send_t sends[5] = { send, send, send, send, send };
    portable_send( server, sends, 5 );
    valid &= server->dropped_by_path[PORTABLE_TX_DROP_STOPPING].load() == 5;
    uint64_t sum = 0;
    for ( int path = 0; path < PORTABLE_TX_DROP_PATHS; ++path ) sum += server->dropped_by_path[path].load();
    valid &= sum == 1 + 2 + 1 + 5 && server->dropped.load() == sum;
    portable_server_cleanup( server );
    return valid;
#endif
}

static portable_server_t * portable_rx_test_owner()
{
    void * storage = rocketnet_malloc( NULL, sizeof( portable_server_t ) );
    if ( !storage ) return NULL;
    portable_server_t * server = new (storage) portable_server_t();
    if ( !portable_rx_storage_create( server, 1024 ) ||
         !rocketnet_platform_mutex_create( &server->mutex ) ||
         !rocketnet_platform_mutex_create( &server->rx_mutex ) )
    {
        portable_server_cleanup( server );
        return NULL;
    }
    server->wake = rocketnet_platform_wake_create( NULL, NULL );
    server->rx_wake = rocketnet_platform_wake_create( NULL, NULL );
    if ( !server->wake || !server->rx_wake )
    {
        portable_server_cleanup( server );
        return NULL;
    }
    return server;
}

// Exercise the actual ownership/copy functions without sockets or scheduling.
// The separate worker case below proves full-queue stop and partial creation.
bool rocketnet_portable_rx_test_handoff()
{
#if !defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    return true;
#else
    portable_server_t * server = portable_rx_test_owner();
    if ( !server ) return false;
    auto fill = []( portable_rx_bank_t & bank, int count, int first )
    {
        bank.count = count;
        for ( int i = 0; i < count; ++i )
        {
            bank.from[i] = {};
            bank.from[i].type = ROCKETNET_ADDRESS_IPV4;
            bank.from[i].port = (uint16_t)( first + i );
            bank.bytes[i] = 4;
            memset( bank.data[i], (first + i) & 255, 4 );
        }
    };
    fill( server->rx_bank[0], 3, 100 );
    bool valid = portable_rx_publish( server );
    rocketnet_address_t from[4] = {};
    int bytes[4] = {};
    uint8_t data[4][8] = {};
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 1 &&
        from[0].port == 100 && data[0][0] == 100 && server->rx_cursor == 1;
    fill( server->rx_bank[1], 2, 200 );
    valid &= portable_rx_publish( server );
    fill( server->rx_bank[2], 1, 300 );
    valid &= !portable_rx_publish( server ) && server->rx_producer == 2 &&
        server->rx_bank[2].count == 1 && server->rx_bank[0].data[1][0] == 101;
    // Contention never hides the caller-owned partial bank. Once it is empty,
    // a failed exchange retains the bank and cursor and does not overwrite it.
    server->force_rx_acquire_busy = true;
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 4 ) == 2 &&
        from[0].port == 101 && from[1].port == 102 && server->rx_consumer == 0 && server->rx_cursor == 3;
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 0 &&
        server->rx_consumer == 0 && server->rx_cursor == 3 && server->rx_ready_count == 1;
    server->force_rx_acquire_busy = false;
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 2 ) == 2 &&
        from[0].port == 200 && from[1].port == 201;
    // Returning the first consumed bank unblocks the retained worker tail.
    valid &= portable_rx_publish( server ) && server->rx_producer == 0 && server->rx_bank[0].count == 0;
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 1 && from[0].port == 300;
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 0;
    fill( server->rx_bank[server->rx_producer], 1, 400 );
    portable_rx_bank_t & oversized = server->rx_bank[server->rx_producer];
    oversized.bytes[0] = ROCKETNET_RECEIVE_CEILING + 1;
    memset( oversized.data[0], 0x5a, ROCKETNET_RECEIVE_CEILING );
    valid &= portable_rx_publish( server );
    memset( data, 0xcc, sizeof(data) );
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 1 &&
        bytes[0] == ROCKETNET_RECEIVE_CEILING + 1 && data[0][7] == 0x5a && data[1][0] == 0xcc;
    fill( server->rx_bank[server->rx_producer], server->rx_capacity, 1000 );
    valid &= portable_rx_publish( server );
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 1 &&
        server->rx_cursor == 1 && server->rx_bank[server->rx_consumer].count == server->rx_capacity;
    portable_stop( server );
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 0 &&
        server->rx_cursor == 1; // stop cannot recycle an exposed tail
    portable_server_cleanup( server );
    return valid;
#endif
}

// The worker pass, stepped on this thread with no socket reads reached and
// no clock: a full FIFO holds the producer (WAIT_FULL, nothing overwritten);
// main copying its consumer out returns it to the empty free FIFO and signals
// the wake; the next pass publishes the held producer; stop sets quit before
// its wake, so the pass after a stop exits however the wait ended.
bool rocketnet_portable_rx_test_worker_steps()
{
#if !defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    return true;
#else
    portable_server_t * server = portable_rx_test_owner();
    if ( !server ) return false;
    // Bank 0 is main's partly copied consumer, bank 1 is ready, bank 2 is
    // the worker's full producer and nothing is free.
    server->rx_consumer = 0;
    server->rx_cursor = 0;
    server->rx_bank[0].count = 1;
    server->rx_bank[0].from[0].port = 10;
    server->rx_bank[0].bytes[0] = 4;
    server->rx_ready_index[0] = 1;
    server->rx_ready_head = 0;
    server->rx_ready_count = 1;
    server->rx_bank[1].count = 1;
    server->rx_bank[1].from[0].port = 11;
    server->rx_bank[1].bytes[0] = 4;
    server->rx_producer = 2;
    server->rx_bank[2].count = 1;
    server->rx_bank[2].from[0].port = 12;
    server->rx_bank[2].bytes[0] = 4;
    server->rx_free_count = 0;
    bool valid = portable_rx_step( server ) == PORTABLE_RX_WAIT_FULL &&
        server->rx_producer == 2 && server->rx_bank[2].count == 1 && server->rx_bank[2].from[0].port == 12;
    // A pending wake would end the full wait at once; there is none yet.
    valid &= !rocketnet_platform_wake_wait( server->rx_wake, false, 0.0 );
    rocketnet_address_t from[2] = {};
    int bytes[2] = {};
    uint8_t data[2][8] = {};
    // Copying the consumer and the ready bank returns bank 0 to the empty free
    // FIFO (the wake) and takes bank 1; bank 1 is returned in turn.
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 2 ) == 2 && from[0].port == 10 && from[1].port == 11;
    valid &= rocketnet_platform_wake_wait( server->rx_wake, false, 0.0 );
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 1 ) == 0 &&
        server->rx_consumer == -1 && server->rx_free_count == 2;
    // A bank returned to a non-empty free FIFO needs no wake.
    valid &= !rocketnet_platform_wake_wait( server->rx_wake, false, 0.0 );
    // The held producer is published now, and main receives it.
    valid &= portable_rx_publish( server ) && server->rx_ready_count == 1 && server->rx_bank[server->rx_producer].count == 0;
    valid &= portable_receive( server, from, &data[0][0], 8, bytes, 2 ) == 1 && from[0].port == 12;
    // Stop: quit, then the wake. Both waits end and the next pass exits.
    portable_stop( server );
    valid &= server->quit.load() && rocketnet_platform_wake_wait( server->rx_wake, false, 0.0 );
    valid &= portable_rx_step( server ) == PORTABLE_RX_EXIT;
    portable_server_cleanup( server );
    return valid;
#endif
}
#endif
static const rocketnet_server_transport_io_operations_t portable_server_operations =
{
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, portable_create, portable_destroy, portable_stop, portable_stopped,
    NULL, NULL, NULL, NULL, NULL, portable_send, portable_receive,
    NULL, NULL, NULL, NULL, NULL, portable_send_failures, portable_send
};
static const rocketnet_server_transport_io_operations_t msgx_server_operations =
{
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, portable_create, portable_destroy, portable_stop, portable_stopped,
    NULL, NULL, NULL, NULL, NULL, portable_send, portable_receive,
    NULL, NULL, NULL, NULL, NULL, portable_send_failures, portable_send
};
#if defined( ROCKETNET_PLATFORM_HAS_WAKE )
// The portable client on a platform with the client I/O wake: an eight-callback
// table over the socket and its wake, so the transport runs the I/O thread of
// docs/CLIENT-IO.md (RNClientIo) instead of the BASE receive thread.
struct portable_io_client_t
{
    rocketnet_platform_socket_t * socket;
    rocketnet_platform_wake_t * wake;
    void * context;
};
// RNClientIo initial / Stop and destroy order (docs/CLIENT-IO.md): create owns
// the socket and the wake beside it; any failure releases what it opened.
static void * portable_io_client_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) rocketnet_malloc( context, sizeof( *client ) );
    if ( !client )
        return NULL;
    client->socket = (rocketnet_platform_socket_t*) portable_socket_create( context, config->name, bind, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    if ( !client->socket )
    {
        rocketnet_free( context, client );
        return NULL;
    }
    client->wake = rocketnet_platform_wake_create( context, client->socket );
    if ( !client->wake )
    {
        rocketnet_platform_socket_destroy( client->socket );
        rocketnet_free( context, client );
        return NULL;
    }
    client->context = context;
    return client;
}
// RNClientIo Stop and destroy order (docs/CLIENT-IO.md): the transport joins
// the I/O thread before closing the transport, so the wake goes, then the socket.
static void portable_io_client_destroy( void * transport )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) transport;
    if ( !client )
        return;
    rocketnet_platform_wake_destroy( client->wake );
    rocketnet_platform_socket_destroy( client->socket );
    rocketnet_free( client->context, client );
}
// RNClientIo IoSend / IoReceive (docs/CLIENT-IO.md The I/O step): the
// per-packet platform functions on the instance's socket.
static void portable_io_client_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) transport; rocketnet_platform_socket_send_packet( client->socket, to, data, bytes ); }
static int portable_io_client_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) transport; return rocketnet_platform_socket_receive_packet( client->socket, from, data, capacity ); }
// RNClientIo IoSend / IoReceive batches (docs/CLIENT-IO.md The I/O step): the
// batch pump seam; on Linux it is sendmmsg/recvmmsg (#client-io-linux-mmsg),
// otherwise the seam's portable loop. Still off the main thread.
static void portable_io_client_send_batch( void * transport, const rocketnet_platform_send_t * sends, int num_packets )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) transport;
#if defined(__linux__)
    rocketnet_platform_socket_send_packets_mmsg( client->socket, sends, num_packets );
#else
    rocketnet_platform_socket_send_packets( client->socket, sends, num_packets );
#endif
}
static int portable_io_client_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) transport;
#if defined(__linux__)
    return rocketnet_platform_socket_receive_packets_mmsg( client->socket, from, packet_data, packet_stride, packet_bytes, max_packets );
#else
    return rocketnet_platform_socket_receive_packets( client->socket, from, packet_data, packet_stride, packet_bytes, max_packets );
#endif
}
// RNClientIo IoWait / waitReadable and the wake (docs/CLIENT-IO.md The wake):
// readable passes through, so with it false the socket is not polled and a
// full receive buffer waits on the eventfd alone.
static bool portable_io_client_wait( void * transport, bool readable, double timeout_seconds )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) transport; return rocketnet_platform_wake_wait( client->wake, readable, timeout_seconds ); }
static void portable_io_client_wake( void * transport )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) transport; rocketnet_platform_wake_set( client->wake ); }
static uint64_t portable_io_client_send_failures( void * transport )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) transport; return rocketnet_platform_socket_send_failures( client->socket ); }
// the client trusts only the server: the socket is connected to it, so the
// kernel drops datagrams from any other source (TRANSPORT.md t11)
static bool portable_io_client_connect( void * transport, const rocketnet_address_t * server_address )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) transport; return rocketnet_platform_socket_connect( client->socket, server_address ); }
static const rocketnet_client_transport_io_operations_t portable_client_operations =
{
    portable_io_client_create, portable_io_client_destroy, portable_io_client_send, portable_io_client_receive,
    portable_io_client_send_batch, portable_io_client_receive_batch, portable_io_client_wait, portable_io_client_wake,
    portable_io_client_send_failures, portable_io_client_connect
};
#else // #if defined( ROCKETNET_PLATFORM_HAS_WAKE )
static void * portable_client_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{ return portable_socket_create( context, config->name, bind, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE ); }
static void portable_client_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{ rocketnet_platform_socket_send_packet( (rocketnet_platform_socket_t*) transport, to, data, bytes ); }
static int portable_client_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{ return rocketnet_platform_socket_receive_packet( (rocketnet_platform_socket_t*) transport, from, data, capacity ); }
#if defined(__linux__)
static void portable_client_send_batch( void * transport, const rocketnet_platform_send_t * sends, int count )
{ rocketnet_platform_socket_send_packets_mmsg( (rocketnet_platform_socket_t*) transport, sends, count ); }
static int portable_client_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{ return rocketnet_platform_socket_receive_packets_mmsg( (rocketnet_platform_socket_t*) transport, from, data, stride, bytes, count ); }
#endif
// the client trusts only the server: the socket is connected to it, so the
// kernel drops datagrams from any other source (TRANSPORT.md t11)
static bool portable_client_connect( void * transport, const rocketnet_address_t * server_address )
{ return rocketnet_platform_socket_connect( (rocketnet_platform_socket_t*) transport, server_address ); }
static const rocketnet_client_transport_io_operations_t portable_client_operations =
{ portable_client_create, portable_socket_destroy, portable_client_send, portable_client_receive,
#if defined(__linux__)
  portable_client_send_batch, portable_client_receive_batch,
#else
  NULL, NULL,
#endif
  NULL, NULL, portable_socket_send_failures, portable_client_connect
};
#endif // #if defined( ROCKETNET_PLATFORM_HAS_WAKE )
extern const rocketnet_transport_backend_t rocketnet_portable_transport = { &portable_server_operations, &portable_client_operations };
extern const rocketnet_transport_backend_t rocketnet_msgx_transport = { &msgx_server_operations, &portable_client_operations };

#define ROCKETNET_STAGED_ADAPTER(name, type, prefix) \
    static uint8_t * name##_start( void * transport, const rocketnet_address_t * to, uint64_t * id ) \
    { return prefix##_start_packet( (type*) transport, to, id ); } \
    static void name##_finish( void * transport, uint64_t id, uint8_t * frame, int bytes ) \
    { prefix##_finish_packet( (type*) transport, id, frame, bytes ); } \
    static void name##_abort( void * transport, uint64_t id, uint8_t * frame ) \
    { prefix##_abort_packet( (type*) transport, id, frame ); } \
    static void name##_flush( void * transport ) { prefix##_send_packets( (type*) transport ); } \
    static rocketnet_server_socket_process_packets_t * name##_receive( void * transport ) \
    { prefix##_receive_packets( (type*) transport ); return prefix##_process_packets( (type*) transport ); }

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include "rocketnet_server_gso.h"
#include "rocketnet_client_gso.h"
#if ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#endif // #if ROCKETNET_XDP

// the native transports' drop counts behind send_failures and receive_drops,
// defined at sdk/rocketnet_server_gso_linux.cpp:869 and sdk/rocketnet_server_socket_linux.cpp:2598
void rocketnet_server_gso_receive_drops( struct rocketnet_server_gso_t * gso, uint64_t * too_short, uint64_t * too_large );
#if ROCKETNET_XDP
uint64_t rocketnet_server_socket_send_failures( struct rocketnet_server_socket_t * server_socket );
#endif // #if ROCKETNET_XDP

static void * gso_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    if ( config->transport->num_xdp_queues > ROCKETNET_GSO_MAX_QUEUES )
    {
        rocketnet_warn( "gso transport refused: num_xdp_queues %d is past the gso transport's %d queue geometry",
            config->transport->num_xdp_queues, ROCKETNET_GSO_MAX_QUEUES );
        return NULL;
    }
    return rocketnet_server_gso_create( context, bind, config->transport->num_xdp_queues, 0 );
}
static void gso_destroy( void * transport ) { rocketnet_server_gso_destroy( (rocketnet_server_gso_t*) transport ); }
static void gso_stop( void * transport ) { rocketnet_server_gso_stop( (rocketnet_server_gso_t*) transport ); }
static bool gso_stopped( void * transport ) { return rocketnet_server_gso_stopped( (rocketnet_server_gso_t*) transport ); }
ROCKETNET_STAGED_ADAPTER(gso, rocketnet_server_gso_t, rocketnet_server_gso)
// SEND_DROPPED: the datagrams a refused sendmmsg dropped
static uint64_t gso_send_failures( void * transport )
{
    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( (rocketnet_server_gso_t*) transport, counters );
    return counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED];
}
static void gso_receive_drops( void * transport, uint64_t * too_short, uint64_t * too_large )
{ rocketnet_server_gso_receive_drops( (rocketnet_server_gso_t*) transport, too_short, too_large ); }
static const rocketnet_server_transport_io_operations_t gso_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    gso_create, gso_destroy, gso_stop, gso_stopped,
    gso_start, gso_finish, gso_abort, gso_flush, gso_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, gso_send_failures, NULL
};
// the client side: sendmmsg with UDP_SEGMENT, recvmmsg (sdk/rocketnet_client_gso_linux.cpp)
extern const rocketnet_transport_backend_t rocketnet_gso_transport = { &gso_operations, &rocketnet_client_gso_operations };

#include "rocketnet_server_uring.h"
void rocketnet_server_uring_receive_drops( struct rocketnet_server_uring_t * uring, uint64_t * too_short, uint64_t * too_large ); // sdk/rocketnet_server_uring_linux.cpp:1223
static void * io_uring_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    if ( config->transport->num_xdp_queues > ROCKETNET_URING_MAX_QUEUES )
    {
        rocketnet_warn( "io_uring transport refused: num_xdp_queues %d is past the io_uring transport's %d queue geometry",
            config->transport->num_xdp_queues, ROCKETNET_URING_MAX_QUEUES );
        return NULL;
    }
    return rocketnet_server_uring_create( context, bind, config->transport->num_xdp_queues );
}
static void io_uring_destroy( void * transport ) { rocketnet_server_uring_destroy( (rocketnet_server_uring_t*) transport ); }
static void io_uring_stop( void * transport ) { rocketnet_server_uring_stop( (rocketnet_server_uring_t*) transport ); }
static bool io_uring_stopped( void * transport ) { return rocketnet_server_uring_stopped( (rocketnet_server_uring_t*) transport ); }
ROCKETNET_STAGED_ADAPTER(io_uring, rocketnet_server_uring_t, rocketnet_server_uring)
// SEND_DROPPED: send CQEs with a negative result, and destinations the
// socket family cannot carry
static uint64_t io_uring_send_failures( void * transport )
{
    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( (rocketnet_server_uring_t*) transport, counters );
    return counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED];
}
static void io_uring_receive_drops( void * transport, uint64_t * too_short, uint64_t * too_large )
{ rocketnet_server_uring_receive_drops( (rocketnet_server_uring_t*) transport, too_short, too_large ); }
static const rocketnet_server_transport_io_operations_t io_uring_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    io_uring_create, io_uring_destroy, io_uring_stop, io_uring_stopped,
    io_uring_start, io_uring_finish, io_uring_abort, io_uring_flush, io_uring_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, io_uring_send_failures, NULL
};
extern const rocketnet_transport_backend_t rocketnet_io_uring_transport = { &io_uring_operations, NULL };

#if ROCKETNET_XDP
static void * xdp_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * )
{
    const int queues = config->transport->num_xdp_queues > 0 ? config->transport->num_xdp_queues : ROCKETNET_XDP_DEFAULT_QUEUES;
    const int cpus = config->transport->num_os_cpus > 0 ? config->transport->num_os_cpus : 1;
    // The backend retains its existing verify-and-bind preflight and commit
    // barrier. Registration never bypasses either or provisions the machine.
    const char * second = NULL;
    if ( config->transport && config->transport->second_public_address[0] != '\0' )
        second = config->transport->second_public_address;
    return rocketnet_server_socket_create( context, config->public_address, config->transport->packet_filter, queues, cpus, second, config->status );
}
static void xdp_stop( void * transport )
{
    rocketnet_server_socket_stop( (rocketnet_server_socket_t*) transport );
}
static bool xdp_stopped( void * transport )
{
    return rocketnet_server_socket_stopped( (rocketnet_server_socket_t*) transport );
}
static void xdp_destroy( void * transport )
{
    rocketnet_server_socket_destroy( (rocketnet_server_socket_t*) transport );
}
ROCKETNET_STAGED_ADAPTER(xdp, rocketnet_server_socket_t, rocketnet_server_socket)
static void xdp_prefilter_counters( void * transport, uint64_t * values )
{
    rocketnet_server_socket_xdp_counters( (rocketnet_server_socket_t*) transport, values );
}
static bool xdp_replay_forget( void * transport, const rocketnet_address_t * address )
{
    return rocketnet_server_socket_replay_forget( (rocketnet_server_socket_t*) transport, address );
}
static bool xdp_replay_mark( void * transport, const rocketnet_address_t * address, uint32_t high_water )
{
    return rocketnet_server_socket_replay_mark( (rocketnet_server_socket_t*) transport, address, high_water );
}
static void xdp_replay_commit( void * transport )
{
    rocketnet_server_socket_replay_commit( (rocketnet_server_socket_t*) transport );
}
static void xdp_client_accepted( void * transport, int view_index )
{
    rocketnet_server_socket_client_accepted( (rocketnet_server_socket_t*) transport, view_index );
}
static uint64_t xdp_send_failures( void * transport )
{
    return rocketnet_server_socket_send_failures( (rocketnet_server_socket_t*) transport );
}
static const rocketnet_server_transport_io_operations_t xdp_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FINISH_FINAL | ROCKETNET_TRANSPORT_BINDS_PUBLIC_ADDRESS,
    xdp_create, xdp_destroy, xdp_stop, xdp_stopped,
    xdp_start, xdp_finish, xdp_abort, xdp_flush, xdp_receive, NULL, NULL, xdp_prefilter_counters,
    xdp_replay_forget, xdp_replay_mark, xdp_replay_commit, xdp_client_accepted, xdp_send_failures, NULL
};
#else
// Keep a refusing rung: removing this known row would make an all-skipped
// pin expand to auto under the fleet-wide list policy. No socket is opened.
// XDP is compiled out (CMakeLists.txt: ROCKETNET_XDP=OFF, or no clang with the
// BPF target, libbpf or an AF_XDP provider at configure), so a list naming xdp refuses it
// with TRANSPORT_NOT_BUILT and auto falls past it to the next transport.
static void * xdp_disabled_create( void *, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * )
{
    if ( config->status )
        *config->status = ROCKETNET_STATUS_TRANSPORT_NOT_BUILT;
    rocketnet_error( "xdp create refused: XDP is compiled out of this build (ROCKETNET_XDP=OFF, or no clang with the BPF target, libbpf or an AF_XDP provider at configure)" );
    return NULL;
}
static void xdp_disabled_destroy( void * ) {}
static void xdp_disabled_send( void *, const rocketnet_platform_send_t *, int ) {}
static int xdp_disabled_receive( void *, rocketnet_address_t *, uint8_t *, int, int *, int ) { return 0; }
static const rocketnet_server_transport_io_operations_t xdp_operations =
{
    0, xdp_disabled_create, xdp_disabled_destroy, synchronous_stop, synchronous_stopped,
    NULL, NULL, NULL, NULL, NULL, xdp_disabled_send, xdp_disabled_receive,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
#endif // ROCKETNET_XDP
extern const rocketnet_transport_backend_t rocketnet_xdp_transport = { &xdp_operations, NULL };
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include "rocketnet_server_iocp.h"
#include "rocketnet_client_iocp.h"
static void * iocp_create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * bind )
{
    // rn-iocp-one-completion-worker: iocp always runs one completion worker plus one send worker.
    return rocketnet_server_iocp_create( context, bind, ROCKETNET_IOCP_DEFAULT_WORKERS );
}
static void iocp_destroy( void * transport ) { rocketnet_server_iocp_destroy( (rocketnet_server_iocp_t*) transport ); }
static void iocp_stop( void * transport )
{
    rocketnet_server_iocp_stop( (rocketnet_server_iocp_t*) transport );
}
static bool iocp_stopped( void * transport )
{
    return rocketnet_server_iocp_stopped( (rocketnet_server_iocp_t*) transport );
}
ROCKETNET_STAGED_ADAPTER(iocp, rocketnet_server_iocp_t, rocketnet_server_iocp)
// SEND_FAILED: the transport's own count of datagrams a refused post or a failed
// send completion dropped (ROCKETNET_IOCP_SEND_FAILED)
static uint64_t iocp_send_failures( void * transport )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( (rocketnet_server_iocp_t*) transport, counters );
    return counters[ROCKETNET_IOCP_SEND_FAILED];
}
static void iocp_receive_drops( void * transport, uint64_t * too_short, uint64_t * too_large )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( (rocketnet_server_iocp_t*) transport, counters );
    *too_short = counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT];
    *too_large = counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE];
}
static const rocketnet_server_transport_io_operations_t iocp_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    iocp_create, iocp_destroy, iocp_stop, iocp_stopped,
    iocp_start, iocp_finish, iocp_abort, iocp_flush, iocp_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, iocp_send_failures, NULL
};
// the client column is the iocp client (sdk/rocketnet_client_iocp_windows.cpp):
// its own completion port, never this server's
extern const rocketnet_transport_backend_t rocketnet_iocp_transport = { &iocp_operations, &rocketnet_client_iocp_operations };

#include "rocketnet_server_rio.h"
static void * rio_create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * bind )
{ return rocketnet_server_rio_create( context, bind, 0 ); }
static void rio_destroy( void * transport ) { rocketnet_server_rio_destroy( (rocketnet_server_rio_t*) transport ); }
static void rio_stop( void * transport )
{
    rocketnet_server_rio_stop( (rocketnet_server_rio_t*) transport );
}
static bool rio_stopped( void * transport )
{
    return rocketnet_server_rio_stopped( (rocketnet_server_rio_t*) transport );
}
ROCKETNET_STAGED_ADAPTER(rio, rocketnet_server_rio_t, rocketnet_server_rio)
// SEND_FAILED: the transport's own count of datagrams a refused post or a failed
// send completion dropped (ROCKETNET_RIO_COUNTER_SEND_FAILED)
static uint64_t rio_send_failures( void * transport )
{
    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( (rocketnet_server_rio_t*) transport, counters );
    return counters[ROCKETNET_RIO_COUNTER_SEND_FAILED];
}
static void rio_receive_drops( void * transport, uint64_t * too_short, uint64_t * too_large )
{
    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( (rocketnet_server_rio_t*) transport, counters );
    *too_short = counters[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT];
    *too_large = counters[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE];
}
static const rocketnet_server_transport_io_operations_t rio_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    rio_create, rio_destroy, rio_stop, rio_stopped,
    rio_start, rio_finish, rio_abort, rio_flush, rio_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, rio_send_failures, NULL
};
extern const rocketnet_transport_backend_t rocketnet_rio_transport = { &rio_operations, NULL };
#endif
#undef ROCKETNET_STAGED_ADAPTER

// Existing public compatibility hook; the backend policy stays in this layer.
void rocketnet_forbid_xdp_commit()
{
#if ROCKETNET_XDP
    rocketnet_server_socket_forbid_commit();
#endif
}

// Built-in resource policy belongs beside the native adapters, never in the
// transport. Custom transports may report their own plan without changing create.
bool rocketnet_builtin_server_memory_plan(const rocketnet_server_transport_config_t * config,
    const char * transport, rocketnet_server_memory_plan_t & plan)
{
    (void) config; // max_clients sizes the portable RX and TX banks; also read by the Linux and Windows transports below
    if (strcmp(transport, "portable") == 0
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
        || strcmp(transport, "msgx") == 0
#endif
       )
    {
#if !defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
        return false;
#else
        uint64_t bytes = rocketnet_memory_allocations({sizeof(portable_server_t),
            portable_rx_storage_bytes( portable_rx_capacity( config ) ),
            portable_tx_storage_bytes( portable_tx_capacity( config ) )});
        if ( !bytes || !rocketnet_memory_add( bytes, 1, rocketnet_platform_socket_allocation_bytes() ) ||
             !rocketnet_memory_add( bytes, 2, rocketnet_platform_wake_allocation_bytes() ) ||
             !rocketnet_memory_add( bytes, 2, rocketnet_platform_thread_allocation_bytes() ) )
            return false;
        plan.transport_io_bytes = bytes;
        plan.worker_threads = 2;
        plan.sockets = 1;
#endif
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    else if (strcmp(transport, "gso") == 0)
    {
        const int queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : ROCKETNET_GSO_DEFAULT_QUEUES;
        plan.transport_io_bytes = rocketnet_server_gso_allocation_bytes(queues);
        plan.worker_threads = queues + 1; plan.sockets = queues;
    }
    else if (strcmp(transport, "io_uring") == 0)
    {
        const int queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : ROCKETNET_URING_DEFAULT_QUEUES;
        plan.transport_io_bytes = rocketnet_server_uring_allocation_bytes(queues);
        plan.worker_threads = 0; plan.sockets = queues;
    }
#if ROCKETNET_XDP
    else if (strcmp(transport, "xdp") == 0)
    {
        const int queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : ROCKETNET_XDP_DEFAULT_QUEUES;
        plan.transport_io_bytes = rocketnet_server_xdp_allocation_bytes(queues);
        plan.umem_bytes = rocketnet_xdp_umem_bytes(queues);
        plan.worker_threads = queues; plan.sockets = queues;
    }
#endif
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    else if (strcmp(transport, "iocp") == 0)
    {
        // rn-iocp-one-completion-worker: iocp always plans one completion worker plus one send worker.
        plan.transport_io_bytes = rocketnet_server_iocp_allocation_bytes(ROCKETNET_IOCP_DEFAULT_WORKERS);
        plan.worker_threads = ROCKETNET_IOCP_DEFAULT_WORKERS + 1; plan.sockets = 1;
    }
    else if (strcmp(transport, "rio") == 0)
    {
        plan.transport_io_bytes = rocketnet_server_rio_allocation_bytes();
        plan.registered_region_bytes = rocketnet_server_rio_registered_region_bytes();
        if ( !plan.registered_region_bytes ) return false;
        plan.worker_threads = 1; plan.sockets = 1;
    }
#endif
    else return false; // A custom transport's resource policy remains native to it.
    return plan.transport_io_bytes != 0;
}

// Builtin-only receive-drop hook. Match the operations table, not a configured
// name, so custom transports retain their own counter policy without a cast.
bool rocketnet_server_transport_io_receive_drops( const rocketnet_server_transport_io_operations_t * operations,
                                           void * transport, uint64_t * too_short, uint64_t * too_large )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    if ( operations == &gso_operations )
        gso_receive_drops( transport, too_short, too_large );
    else if ( operations == &io_uring_operations )
        io_uring_receive_drops( transport, too_short, too_large );
    else
        return false;
    return true;
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    // Match the transport-counter promise in rocketnet_server_transport.cpp.
    if ( operations == &iocp_operations )
        iocp_receive_drops( transport, too_short, too_large );
    else if ( operations == &rio_operations )
        rio_receive_drops( transport, too_short, too_large );
    else
        return false;
    return true;
#else
    (void) operations; (void) transport; (void) too_short; (void) too_large;
    return false;
#endif
}
