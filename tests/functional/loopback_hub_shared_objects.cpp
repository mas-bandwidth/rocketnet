/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    test_loopback_hub_two_shared_objects: the library built into two separate
    shared objects, a server module and a client module, each linking its own
    copy of the development library, loaded into this process through the
    operating system loader and connected through one loopback hub. The hub
    is the only state the two copies share. A functional case: it loads
    shared objects, starts transport threads and runs in real time.

    Every platform: two .so modules loaded with dlopen on Linux and macOS, two
    DLLs loaded with LoadLibrary on Windows.
*/

#include "rocketnet.h"
#include "loopback_hub_module.h"
#include "../plan_rss.h"

#include <stdio.h>
#include <stdlib.h>

#if defined( _WIN32 )
#include <windows.h>
#else // #if defined( _WIN32 )
#include <dlfcn.h>
#endif // #if defined( _WIN32 )

static void loopback_hub_shared_objects_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                    \
do                                                                                                                     \
{                                                                                                                      \
    if ( !(condition) )                                                                                                \
    {                                                                                                                  \
        loopback_hub_shared_objects_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
    }                                                                                                                  \
} while(0)

// The transport pins (ROCKETNET_SERVER_TRANSPORT, ROCKETNET_CLIENT_TRANSPORT)
// override a config's transport, so a pinned suite would run both modules on
// sockets and leave the hub with no endpoint. This case is about the hub, so
// it clears both pins while it runs and puts them back after.

struct loopback_hub_env_pin_t
{
    const char * name;
    bool was_set;
    char saved[256];
};

static void loopback_hub_env_pin_clear( loopback_hub_env_pin_t * pin, const char * name )
{
    pin->name = name;
    pin->was_set = false;
    pin->saved[0] = '\0';
#if defined( _WIN32 )
    size_t required = 0;
    if ( getenv_s( &required, pin->saved, sizeof( pin->saved ), name ) == 0 && required > 0 )
    {
        pin->was_set = true;
    }
    _putenv_s( name, "" );
#else // #if defined( _WIN32 )
    const char * value = getenv( name );
    if ( value )
    {
        snprintf( pin->saved, sizeof( pin->saved ), "%s", value );
        pin->was_set = true;
    }
    unsetenv( name );
#endif // #if defined( _WIN32 )
}

static void loopback_hub_env_pin_restore( const loopback_hub_env_pin_t * pin )
{
#if defined( _WIN32 )
    _putenv_s( pin->name, pin->was_set ? pin->saved : "" );
#else // #if defined( _WIN32 )
    if ( pin->was_set )
    {
        setenv( pin->name, pin->saved, 1 );
    }
    else
    {
        unsetenv( pin->name );
    }
#endif // #if defined( _WIN32 )
}

// the operating system loader: open a module, look up a name, close it

static void * loopback_hub_module_open( const char * path )
{
#if defined( _WIN32 )
    void * module = (void *) LoadLibraryA( path );
    if ( !module )
    {
        printf( "LoadLibrary %s: error %lu\n", path, (unsigned long) GetLastError() );
    }
#else // #if defined( _WIN32 )
    // RTLD_LOCAL: the module's symbols never join the global scope, so each
    // module resolves the library through its own copy
    void * module = dlopen( path, RTLD_NOW | RTLD_LOCAL );
    if ( !module )
    {
        printf( "dlopen %s: %s\n", path, dlerror() );
    }
#endif // #if defined( _WIN32 )
    return module;
}

static void * loopback_hub_module_lookup( void * module, const char * name )
{
#if defined( _WIN32 )
    return (void *) GetProcAddress( (HMODULE) module, name );
#else // #if defined( _WIN32 )
    return dlsym( module, name );
#endif // #if defined( _WIN32 )
}

static bool loopback_hub_module_close( void * module )
{
#if defined( _WIN32 )
    return FreeLibrary( (HMODULE) module ) != 0;
#else // #if defined( _WIN32 )
    return dlclose( module ) == 0;
#endif // #if defined( _WIN32 )
}

static void * loopback_hub_module_symbol( void * module, const char * name )
{
    void * symbol = loopback_hub_module_lookup( module, name );
    if ( !symbol )
    {
        printf( "missing export %s\n", name );
    }
    rocketnet_check( symbol != NULL );
    return symbol;
}

// The modules' own copies of the library allocate with their own default
// allocator, outside the host's plan counter (tests/plan_rss.h), so the case's
// bound is the host's plan plus PLAN_RSS_SLACK_MB. In a Release build the
// modules' untouched planned pages are never resident and fit in that slack.
// Under the MSVC debug CRT every byte their malloc hands out is filled, so
// their whole plan is resident: measured 2026-11-11 on captain (MSVC 2022
// Debug), a 94 MiB peak against a 64 MiB bound. There the modules' planned
// bytes (their server's and client's required memory, the configs the
// modules build) are counted as held outside the allocator for as long as
// the modules run, as the XDP cases count their UMEM; every other build
// counts nothing.
static uint64_t loopback_hub_modules_debug_crt_bytes()
{
#if defined( _MSC_VER ) && defined( _DEBUG )
    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    rocketnet_check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = LOOPBACK_HUB_MODULE_PROTOCOL_ID;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    client_config.transport = "loopback";

    // a loopback config plans only with a hub set: a host hub of the modules'
    // size, for the plan only
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * plan_hub = rocketnet_loopback_hub_create( NULL, 1, &status );
    if ( plan_hub == NULL || status != ROCKETNET_STATUS_OK )
    {
        printf( "loopback hub modules: the plan hub create failed: %s\n", rocketnet_status_name( status ) );
    }
    rocketnet_check( plan_hub != NULL && status == ROCKETNET_STATUS_OK );
    server_config.loopback_hub = plan_hub;
    client_config.loopback_hub = plan_hub;
    const size_t server_bytes = rocketnet_server_required_memory( &server_config );
    const size_t client_bytes = rocketnet_client_required_memory( &client_config );
    rocketnet_check( rocketnet_loopback_hub_destroy( plan_hub ) == ROCKETNET_STATUS_OK );
    rocketnet_check( server_bytes > 0 && client_bytes > 0 );
    printf( "loopback hub modules: debug CRT, counting server %zu + client %zu planned bytes\n", server_bytes, client_bytes );
    return (uint64_t) server_bytes + (uint64_t) client_bytes;
#else
    return 0;
#endif // #if defined( _MSC_VER ) && defined( _DEBUG )
}

void test_loopback_hub_two_shared_objects()
{
    loopback_hub_env_pin_t server_pin, client_pin;
    loopback_hub_env_pin_clear( &server_pin, "ROCKETNET_SERVER_TRANSPORT" );
    loopback_hub_env_pin_clear( &client_pin, "ROCKETNET_CLIENT_TRANSPORT" );

    void * server_module = loopback_hub_module_open( ROCKETNET_LOOPBACK_HUB_SERVER_MODULE_PATH );
    rocketnet_check( server_module != NULL );

    void * client_module = loopback_hub_module_open( ROCKETNET_LOOPBACK_HUB_CLIENT_MODULE_PATH );
    rocketnet_check( client_module != NULL );
    rocketnet_check( client_module != server_module );

    const loopback_hub_server_module_start_t server_start = (loopback_hub_server_module_start_t) loopback_hub_module_symbol( server_module, "loopback_hub_server_module_start" );
    const loopback_hub_server_module_update_t server_update = (loopback_hub_server_module_update_t) loopback_hub_module_symbol( server_module, "loopback_hub_server_module_update" );
    const loopback_hub_server_module_stop_t server_stop = (loopback_hub_server_module_stop_t) loopback_hub_module_symbol( server_module, "loopback_hub_server_module_stop" );
    const loopback_hub_client_module_start_t client_start = (loopback_hub_client_module_start_t) loopback_hub_module_symbol( client_module, "loopback_hub_client_module_start" );
    const loopback_hub_client_module_update_t client_update = (loopback_hub_client_module_update_t) loopback_hub_module_symbol( client_module, "loopback_hub_client_module_update" );
    const loopback_hub_client_module_destroy_hub_t client_destroy_hub = (loopback_hub_client_module_destroy_hub_t) loopback_hub_module_symbol( client_module, "loopback_hub_client_module_destroy_hub" );
    const loopback_hub_client_module_stop_t client_stop = (loopback_hub_client_module_stop_t) loopback_hub_module_symbol( client_module, "loopback_hub_client_module_stop" );

    // the modules export only their entry points: the library inside each is
    // its own, never one shared copy

    rocketnet_check( loopback_hub_module_lookup( server_module, "rocketnet_init" ) == NULL );
    rocketnet_check( loopback_hub_module_lookup( client_module, "rocketnet_init" ) == NULL );

    // each module initializes its own copy: a shared copy would refuse the
    // second init with ALREADY_INITIALIZED

    const uint64_t modules_bytes = loopback_hub_modules_debug_crt_bytes();
    plan_rss_count_outside_allocator( modules_bytes );

    struct rocketnet_loopback_hub_t * hub = NULL;
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_check( server_start( &hub, address, (int) sizeof( address ), public_key ) == 1 );
    rocketnet_check( hub != NULL );
    rocketnet_check( client_start( hub, address, public_key ) == 1 );

    // frames at the caller's time; the transports' I/O threads run in real
    // time, so the host sleeps a frame's millisecond between them

    double time = 1000.0;
    int pings = 0;
    int pong = 0;
    for ( int frame = 0; frame < 5000 && pong == 0; frame++, time += 1.0 / 60.0 )
    {
        pong = client_update( time );
        pings += server_update( time );
        rocketnet_sleep( 0.001 );
    }

    rocketnet_check( pong == 1 );
    rocketnet_check( pings == 1 );

    // the client's copy reads the server copy's hub: same build, so the
    // header check passes, and an endpoint is registered, so destroy refuses
    // and keeps the hub

    rocketnet_check( client_destroy_hub( hub ) == ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE );

    // the client first, then the server, which destroys the hub and terms

    rocketnet_check( client_stop( time ) == 1 );
    rocketnet_check( server_stop() == 1 );
    plan_rss_uncount_outside_allocator( modules_bytes );

    rocketnet_check( loopback_hub_module_close( client_module ) );
    rocketnet_check( loopback_hub_module_close( server_module ) );

    loopback_hub_env_pin_restore( &client_pin );
    loopback_hub_env_pin_restore( &server_pin );
}
