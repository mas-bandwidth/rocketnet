/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include <string.h>

#define ROCKETNET_DRIVER_LINUX (1u << ROCKETNET_PLATFORM_LINUX)
// the io_uring row exists only in a build configured with liburing; without
// it the row has no platform, so no availability entry and no reference to
// the backend, and a pin of it is skipped by the ladder
#if defined(ROCKETNET_IO_URING) && ROCKETNET_IO_URING
#define ROCKETNET_DRIVER_IO_URING_PLATFORMS ROCKETNET_DRIVER_LINUX
#else
#define ROCKETNET_DRIVER_IO_URING_PLATFORMS 0u
#endif
#define ROCKETNET_DRIVER_MAC (1u << ROCKETNET_PLATFORM_MAC)
#define ROCKETNET_DRIVER_WINDOWS (1u << ROCKETNET_PLATFORM_WINDOWS)
#define ROCKETNET_DRIVER_ALL_PLATFORMS (~0u)
#define ROCKETNET_DRIVER_HERE (1u << ROCKETNET_PLATFORM)

#define ROCKETNET_DRIVER(symbol, name, discovers, operations, platforms, server, client) extern const rocketnet_driver_backend_t operations;
#include "rocketnet_drivers.inc"
#undef ROCKETNET_DRIVER

// Require constant initialization. In particular, unavailable platform rows
// must contain null pointers without emitting references to absent backends.
static constexpr rocketnet_driver_definition_t definitions[] =
{
#define ROCKETNET_DRIVER(symbol, name, discovers, operations, platforms, server, client) \
    { name, discovers, platforms, server, client, (platforms & ROCKETNET_DRIVER_HERE) ? &operations : NULL },
#include "rocketnet_drivers.inc"
#undef ROCKETNET_DRIVER
};
static_assert( sizeof(definitions) / sizeof(definitions[0]) == ROCKETNET_NUM_DRIVERS, "registry completeness" );

const rocketnet_driver_definition_t * rocketnet_driver_definition( int driver )
{
    return driver >= 0 && driver < ROCKETNET_NUM_DRIVERS ? &definitions[driver] : NULL;
}

const char * rocketnet_driver_name( int driver )
{
    const rocketnet_driver_definition_t * definition = rocketnet_driver_definition( driver );
    return definition ? definition->name : "???";
}

bool rocketnet_driver_discovers_bound_port( int driver )
{
    const rocketnet_driver_definition_t * definition = rocketnet_driver_definition( driver );
    return definition && definition->discovers_bound_port;
}

bool rocketnet_driver_pin_only( int driver )
{
    const rocketnet_driver_definition_t * definition = rocketnet_driver_definition( driver );
    return definition && ( definition->server_preference >= ROCKETNET_DRIVER_PIN_ONLY ||
                           definition->client_preference >= ROCKETNET_DRIVER_PIN_ONLY );
}

struct rocketnet_driver_lists_t
{
    int servers[ROCKETNET_NUM_DRIVERS];
    int clients[ROCKETNET_NUM_DRIVERS];
    int num_servers = 0;
    int num_clients = 0;
    // Exact capacity derived from registry strings, including separators.
    char names[1
#define ROCKETNET_DRIVER(symbol, name, discovers, operations, platforms, server, client) + sizeof(name) + 1
#include "rocketnet_drivers.inc"
#undef ROCKETNET_DRIVER
    ] = {};
    char discovering_names[sizeof(names)] = {};

    rocketnet_driver_lists_t()
    {
        for ( int i = 0; i < ROCKETNET_NUM_DRIVERS; ++i )
        {
            if ( i ) strcat( names, ", " );
            strcat( names, definitions[i].name );
            if ( definitions[i].discovers_bound_port )
            {
                if ( discovering_names[0] ) strcat( discovering_names, ", " );
                strcat( discovering_names, definitions[i].name );
            }
            if ( !( definitions[i].platforms & ROCKETNET_DRIVER_HERE ) ) continue;
            if ( definitions[i].server_preference ) insert( servers, num_servers, i, true );
            if ( definitions[i].client_preference ) insert( clients, num_clients, i, false );
        }
    }

    void insert( int * list, int & count, int id, bool server )
    {
        const int rank = server ? definitions[id].server_preference : definitions[id].client_preference;
        int index = count++;
        while ( index > 0 )
        {
            const int previous = list[index - 1];
            const int previous_rank = server ? definitions[previous].server_preference : definitions[previous].client_preference;
            if ( previous_rank <= rank ) break;
            list[index] = previous;
            --index;
        }
        list[index] = id;
    }
};

static const rocketnet_driver_lists_t & rocketnet_driver_lists()
{
    static const rocketnet_driver_lists_t lists;
    return lists;
}

const char * rocketnet_driver_names() { return rocketnet_driver_lists().names; }
const char * rocketnet_driver_discovering_names() { return rocketnet_driver_lists().discovering_names; }
const int * rocketnet_server_driver_availability( int * count )
{
    *count = rocketnet_driver_lists().num_servers;
    return rocketnet_driver_lists().servers;
}
const int * rocketnet_client_driver_availability( int * count )
{
    *count = rocketnet_driver_lists().num_clients;
    return rocketnet_driver_lists().clients;
}

bool rocketnet_server_driver_open( const rocketnet_server_driver_operations_t * ops,
    void * context, const rocketnet_server_driver_config_t * config,
    rocketnet_address_t * bind, rocketnet_server_driver_t * driver )
{
    if ( !driver || driver->instance || !config || !bind || !ops || !ops->create || !ops->destroy || !ops->stop ) return false;
    const bool staged = ( ops->flags & ROCKETNET_DRIVER_STAGED ) != 0;
    if ( staged ? !( ops->start_packet && ops->finish_packet && ops->abort_packet && ops->flush && ops->receive )
                : !( ops->send_batch && ops->receive_batch ) ) return false;
    if ( ( ops->replay_forget || ops->replay_mark || ops->replay_commit ) &&
         !( ops->replay_forget && ops->replay_mark && ops->replay_commit ) ) return false;
    // Refused attempts must not constrain the next candidate's port.
    rocketnet_address_t attempt = *bind;
    void * instance = ops->create( context, config, &attempt );
    if ( !instance ) return false;
    driver->operations = ops;
    driver->instance = instance;
    *bind = attempt;
    return true;
}

void rocketnet_server_driver_close( rocketnet_server_driver_t * driver )
{
    if ( driver->instance ) driver->operations->destroy( driver->instance );
    *driver = {};
}

bool rocketnet_server_driver_stop( rocketnet_server_driver_t * driver )
{
    return !driver->instance || driver->operations->stop( driver->instance );
}

bool rocketnet_client_driver_open( const rocketnet_client_driver_operations_t * ops,
    void * context, const rocketnet_client_driver_config_t * config,
    rocketnet_address_t * bind, rocketnet_client_driver_t * driver )
{
    if ( !driver || driver->instance || !config || !bind || !ops ||
         !ops->create || !ops->destroy || !ops->send || !ops->receive ) return false;
    // The interface requires wait and wake together: a wait with no wake can
    // never be released, a wake with no wait is never observed. docs/CLIENT-IO.md.
    if ( ( ops->wait != NULL ) != ( ops->wake != NULL ) ) return false;
    rocketnet_address_t attempt = *bind;
    void * instance = ops->create( context, config, &attempt );
    if ( !instance ) return false;
    driver->operations = ops;
    driver->instance = instance;
    *bind = attempt;
    return true;
}

void rocketnet_client_driver_close( rocketnet_client_driver_t * driver )
{
    if ( driver->instance ) driver->operations->destroy( driver->instance );
    *driver = {};
}
