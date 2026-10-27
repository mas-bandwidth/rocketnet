/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include <string.h>

#define ROCKETNET_TRANSPORT_LINUX (1u << ROCKETNET_PLATFORM_LINUX)
// the io_uring row exists only in a build configured with liburing; without
// it the row has no platform, so no availability entry and no reference to
// the backend, and a pin of it is skipped by the ladder
#if defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING
#define ROCKETNET_TRANSPORT_IO_URING_PLATFORMS ROCKETNET_TRANSPORT_LINUX
#else
#define ROCKETNET_TRANSPORT_IO_URING_PLATFORMS 0u
#endif
#define ROCKETNET_TRANSPORT_MAC (1u << ROCKETNET_PLATFORM_MAC)
#define ROCKETNET_TRANSPORT_WINDOWS (1u << ROCKETNET_PLATFORM_WINDOWS)
#define ROCKETNET_TRANSPORT_ALL_PLATFORMS (~0u)
#define ROCKETNET_TRANSPORT_HERE (1u << ROCKETNET_PLATFORM)

#define ROCKETNET_TRANSPORT( symbol, name, discovers, operations, platforms, server, client ) extern const rocketnet_transport_backend_t operations;
#include "rocketnet_drivers.inc"
#undef ROCKETNET_TRANSPORT

// Require constant initialization. In particular, unavailable platform rows
// must contain null pointers without emitting references to absent backends.
static constexpr rocketnet_transport_definition_t definitions[] = {
#define ROCKETNET_TRANSPORT( symbol, name, discovers, operations, platforms, server, client ) \
    { name, discovers, platforms, server, client, (platforms & ROCKETNET_TRANSPORT_HERE) ? &operations : NULL },
#include "rocketnet_drivers.inc"
#undef ROCKETNET_TRANSPORT
};
static_assert( sizeof( definitions ) / sizeof( definitions[0] ) == ROCKETNET_NUM_TRANSPORTS, "registry completeness" );

const rocketnet_transport_definition_t * rocketnet_transport_definition( int transport )
{
    return transport >= 0 && transport < ROCKETNET_NUM_TRANSPORTS ? &definitions[transport] : NULL;
}

const char * rocketnet_transport_name( int transport )
{
    const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( transport );
    return definition ? definition->name : "???";
}

bool rocketnet_transport_discovers_bound_port( int transport )
{
    const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( transport );
    return definition && definition->discovers_bound_port;
}

bool rocketnet_transport_pin_only( int transport, bool server )
{
    const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( transport );
    if ( !definition )
        return false;
    return ( server ? definition->server_preference : definition->client_preference ) >= ROCKETNET_TRANSPORT_PIN_ONLY;
}

bool rocketnet_transport_pin_only( int transport )
{
    return rocketnet_transport_pin_only( transport, true );
}

struct rocketnet_transport_lists_t
{
    int servers[ROCKETNET_NUM_TRANSPORTS];
    int clients[ROCKETNET_NUM_TRANSPORTS];
    int num_servers = 0;
    int num_clients = 0;
    // Exact capacity derived from registry strings, including separators.
    char names[1
#define ROCKETNET_TRANSPORT( symbol, name, discovers, operations, platforms, server, client ) + sizeof(name) + 1
#include "rocketnet_drivers.inc"
#undef ROCKETNET_TRANSPORT
    ] = {};
    char discovering_names[sizeof( names )] = {};

    rocketnet_transport_lists_t()
    {
        for ( int i = 0; i < ROCKETNET_NUM_TRANSPORTS; ++i )
        {
            if ( i )
                append( names, ", " );
            append( names, definitions[i].name );
            if ( definitions[i].discovers_bound_port )
            {
                if ( discovering_names[0] )
                    append( discovering_names, ", " );
                append( discovering_names, definitions[i].name );
            }
            if ( !( definitions[i].platforms & ROCKETNET_TRANSPORT_HERE ) )
                continue;
            if ( definitions[i].server_preference )
                insert( servers, num_servers, i, true );
            if ( definitions[i].client_preference )
                insert( clients, num_clients, i, false );
        }
    }

    // Both lists are sized exactly from the registry, so an append always fits.
    static void append( char ( &buffer )[sizeof( names )], const char * text )
    {
        const size_t used = strlen( buffer );
        const size_t bytes = strlen( text );
        if ( used + bytes >= sizeof( buffer ) )
            return;
        memcpy( buffer + used, text, bytes + 1 );
    }

    void insert( int * list, int & count, int id, bool server )
    {
        const int rank = server ? definitions[id].server_preference : definitions[id].client_preference;
        int index = count++;
        while ( index > 0 )
        {
            const int previous = list[index - 1];
            const int previous_rank = server ? definitions[previous].server_preference : definitions[previous].client_preference;
            if ( previous_rank <= rank )
                break;
            list[index] = previous;
            --index;
        }
        list[index] = id;
    }
};

static const rocketnet_transport_lists_t & rocketnet_transport_lists()
{
    static const rocketnet_transport_lists_t lists;
    return lists;
}

const char * rocketnet_transport_names() { return rocketnet_transport_lists().names; }
const char * rocketnet_transport_discovering_names() { return rocketnet_transport_lists().discovering_names; }
const int * rocketnet_server_transport_io_availability( int * count )
{
    *count = rocketnet_transport_lists().num_servers;
    return rocketnet_transport_lists().servers;
}
const int * rocketnet_client_transport_io_availability( int * count )
{
    *count = rocketnet_transport_lists().num_clients;
    return rocketnet_transport_lists().clients;
}

// a create that returned NULL after an allocation on this thread was refused
// failed for want of memory: report OUT_OF_MEMORY through the config's status
static void rocketnet_transport_report_out_of_memory( int * status, uint64_t refusals_before )
{
    if ( status && rocketnet_allocation_refusals() != refusals_before )
        *status = ROCKETNET_STATUS_OUT_OF_MEMORY;
}

bool rocketnet_server_transport_io_open( const rocketnet_server_transport_io_operations_t * ops,
                                         void * context, const rocketnet_server_transport_io_config_t * config,
                                         rocketnet_address_t * bind, rocketnet_server_transport_io_t * transport )
{
    if ( !transport || transport->instance || !config || !bind || !ops || !ops->create || !ops->destroy || !ops->stop || !ops->stopped )
        return false;
    const bool staged = ( ops->flags & ROCKETNET_TRANSPORT_STAGED ) != 0;
    if ( staged ? !( ops->start_packet && ops->finish_packet && ops->abort_packet && ops->flush && ops->receive )
                : !( ops->send_batch && ops->receive_batch ) )
        return false;
    if ( ( ops->replay_forget || ops->replay_mark || ops->replay_commit ) &&
         !( ops->replay_forget && ops->replay_mark && ops->replay_commit ) )
        return false;
    // Refused attempts must not constrain the next candidate's port.
    rocketnet_address_t attempt = *bind;
    const uint64_t refusals = rocketnet_allocation_refusals();
    void * instance = ops->create( context, config, &attempt );
    if ( !instance )
    {
        rocketnet_transport_report_out_of_memory( config->status, refusals );
        return false;
    }
    transport->operations = ops;
    transport->instance = instance;
    *bind = attempt;
    return true;
}

void rocketnet_server_transport_io_close( rocketnet_server_transport_io_t * transport )
{
    if ( transport->instance )
    {
        const bool stopped = transport->operations->stopped( transport->instance );
        rocketnet_assert( stopped );
        if ( !stopped )
            return;
        transport->operations->destroy( transport->instance );
    }
    *transport = {};
}

void rocketnet_server_transport_io_stop( rocketnet_server_transport_io_t * transport )
{
    if ( transport->instance )
        transport->operations->stop( transport->instance );
}

bool rocketnet_server_transport_io_stopped( const rocketnet_server_transport_io_t * transport )
{
    return !transport->instance || transport->operations->stopped( transport->instance );
}

bool rocketnet_client_transport_io_open( const rocketnet_client_transport_io_operations_t * ops,
                                         void * context, const rocketnet_client_transport_io_config_t * config,
                                         rocketnet_address_t * bind, rocketnet_client_transport_io_t * transport )
{
    if ( !transport || transport->instance || !config || !bind || !ops ||
         !ops->create || !ops->destroy || !ops->send || !ops->receive )
        return false;
    // The interface requires wait and wake together: a wait with no wake can
    // never be released, a wake with no wait is never observed. docs/CLIENT-IO.md.
    if ( ( ops->wait != NULL ) != ( ops->wake != NULL ) )
        return false;
    rocketnet_address_t attempt = *bind;
    const uint64_t refusals = rocketnet_allocation_refusals();
    void * instance = ops->create( context, config, &attempt );
    if ( !instance )
    {
        rocketnet_transport_report_out_of_memory( config->status, refusals );
        return false;
    }
    transport->operations = ops;
    transport->instance = instance;
    *bind = attempt;
    return true;
}

void rocketnet_client_transport_io_close( rocketnet_client_transport_io_t * transport )
{
    if ( transport->instance )
        transport->operations->destroy( transport->instance );
    *transport = {};
}
