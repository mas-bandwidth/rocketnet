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
        loopback_hub_shared_objects_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
} while(0)

// the operating system loader: open a module, look up a name, close it

static void * loopback_hub_module_open( const char * path )
{
#if defined( _WIN32 )
    void * module = (void *) LoadLibraryA( path );
    if ( !module )
        printf( "LoadLibrary %s: error %lu\n", path, (unsigned long) GetLastError() );
#else // #if defined( _WIN32 )
    // RTLD_LOCAL: the module's symbols never join the global scope, so each
    // module resolves the library through its own copy
    void * module = dlopen( path, RTLD_NOW | RTLD_LOCAL );
    if ( !module )
        printf( "dlopen %s: %s\n", path, dlerror() );
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
        printf( "missing export %s\n", name );
    rocketnet_check( symbol != NULL );
    return symbol;
}

void test_loopback_hub_two_shared_objects()
{
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

    rocketnet_check( loopback_hub_module_close( client_module ) );
    rocketnet_check( loopback_hub_module_close( server_module ) );
}
