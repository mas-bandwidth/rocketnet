/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop launcher, in the shape of a game's launcher: it runs the server,
    the client, or both.

        example_game_loop [--clients <1..64>] [--seconds <1..600>]

            listen mode: the server and the clients in one process, each on
            its own thread, over the in-memory loopback transport; checks
            that every message sent before the other side's update is taken
            by that update, and prints PASS

        example_game_loop server [--port <n>]

            a dedicated server over UDP on 127.0.0.1, until its clients leave;
            it writes its public key to game_loop.keys for the client

        example_game_loop client [--port <n>] [--seconds <1..600>]

            a client over UDP, reading game_loop.keys as a backend would
            hand it the server's key

    See docs/examples/game_loop.md.
*/

#include "game.h"

#include <stdlib.h>

#define KEY_FILE "game_loop.keys"

static std::atomic<int> quit( 0 );

// MSVC deprecates getenv and fopen: getenv_s and fopen_s there
static bool example_getenv( const char * name, char * buffer, size_t size )
{
#if defined( _MSC_VER )
    size_t length = 0;
    return getenv_s( &length, buffer, size, name ) == 0 && length > 0;
#else
    const char * value = getenv( name );
    if ( !value || strlen( value ) >= size )
        return false;
    memcpy( buffer, value, strlen( value ) + 1 );
    return true;
#endif
}

static FILE * example_fopen( const char * path, const char * mode )
{
#if defined( _MSC_VER )
    FILE * file = NULL;
    return fopen_s( &file, path, mode ) == 0 ? file : NULL;
#else
    return fopen( path, mode );
#endif
}

static void print_cost( const char * label, const cost_t & cost )
{
    printf( "%s network step mean %.1f us, max %.1f us over %d frames\n", label, cost.count ? cost.sum / cost.count : 0.0, cost.max, cost.count );
}

static int listen_mode( int num_clients, double seconds )
{
    static wire_t wire;
    static server_args_t server;
    server.address = "127.0.0.1:0";
    server.transport = "loopback";
    server.max_clients = num_clients;
    server.wire = &wire;
    server.quit = &quit;
    std::thread server_thread( server_main, &server );
    while ( server.ready == 0 )
        rocketnet_sleep( 0.001 );
    if ( server.ready < 0 )
    {
        server_thread.join();
        return 1;
    }

    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) server.port );
    static client_args_t clients[MAX_CLIENTS];
    std::thread * client_threads[MAX_CLIENTS];
    for ( int i = 0; i < num_clients; i++ )
    {
        clients[i].number = i;
        clients[i].server_address = address;
        clients[i].server_public_key = server.public_key;
        clients[i].transport = "loopback";
        clients[i].seconds = seconds;
        clients[i].wire = &wire;
        client_threads[i] = new std::thread( client_main, &clients[i] );
    }
    int result = 0, states = 0;
    for ( int i = 0; i < num_clients; i++ )
    {
        client_threads[i]->join();
        delete client_threads[i];
        result |= clients[i].result;
        states += clients[i].states;
    }
    quit = 1;
    server_thread.join();

    printf( "%d clients and the server on their own threads, transport loopback\n", num_clients );
    print_cost( "server", server.cost );
    print_cost( "client 0", clients[0].cost );
    printf( "%d inputs and %d state updates; %d not taken by the receiver's next update\n", server.inputs, states, wire.late.load() );
    if ( result != 0 || server.result != 0 )
        printf( "FAIL: a client or the server did not exchange messages\n" );
    else if ( server.clients_seen != num_clients )
        printf( "FAIL: %d of %d clients connected\n", server.clients_seen, num_clients );
    else if ( wire.late != 0 )
        printf( "FAIL: %d messages missed the update that followed their send\n", wire.late.load() );
    else
        return 0;
    return 1;
}

static int dedicated_mode( int port )
{
    remove( KEY_FILE );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", port );
    static server_args_t server;
    server.address = address;
    server.transport = NULL;
    server.max_clients = MAX_CLIENTS;
    server.quit = &quit;

    // the server makes its keypair at create: write its public key once it
    // listens, as a backend registers it
    std::thread server_thread( server_main, &server );
    while ( server.ready == 0 )
        rocketnet_sleep( 0.001 );
    if ( server.ready > 0 )
    {
        FILE * file = example_fopen( KEY_FILE, "w" );
        if ( file )
        {
            fprintf( file, "server_public " );
            for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
                fprintf( file, "%02x", server.public_key[i] );
            fprintf( file, "\nserver_ipv4 %s\n", address );
            fclose( file );
            printf( "server wrote %s for the client\n", KEY_FILE );
            fflush( stdout );
        }
    }
    server_thread.join();
    printf( "server took %d inputs from %d clients\n", server.inputs, server.clients_seen );
    print_cost( "server", server.cost );
    return server.ready > 0 ? server.result : 1;
}

// one line of the key file: its label, a space, then the value
static const char * read_line( FILE * file, const char * label, char * line, int size )
{
    if ( !file || !fgets( line, size, file ) )
        return NULL;
    line[strcspn( line, "\r\n" )] = '\0';
    const size_t length = strlen( label );
    return strncmp( line, label, length ) == 0 && line[length] == ' ' ? line + length + 1 : NULL;
}

static int hex_value( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    return -1;
}

static int client_mode( double seconds )
{
    uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char key_line[256], address_line[256];
    FILE * file = example_fopen( KEY_FILE, "r" );
    const char * hex = read_line( file, "server_public", key_line, sizeof( key_line ) );
    const char * address = read_line( file, "server_ipv4", address_line, sizeof( address_line ) );
    if ( file )
        fclose( file );
    bool valid = hex && address && strlen( hex ) == 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES;
    for ( int i = 0; valid && i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        const int high = hex_value( hex[2 * i] ), low = hex_value( hex[2 * i + 1] );
        valid = high >= 0 && low >= 0;
        key[i] = (uint8_t) ( high * 16 + low );
    }
    if ( !valid )
    {
        printf( "FAIL: no %s: run the server first\n", KEY_FILE );
        return 1;
    }

    static client_args_t client;
    client.server_address = address;
    client.server_public_key = key;
    client.seconds = seconds;
    client_main( &client );
    printf( "client took %d state updates\n", client.states );
    print_cost( "client", client.cost );
    return client.result;
}

int main( int argc, char ** argv )
{
    int clients = 1, port = 40000;
    double seconds = 0.0;
    char environment[32];
    if ( example_getenv( "ROCKETNET_EXAMPLE_PORT", environment, sizeof( environment ) ) )
        port = atoi( environment );
    const char * mode = argc > 1 && argv[1][0] != '-' ? argv[1] : "listen";
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--clients" ) == 0 && i + 1 < argc ) clients = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--seconds" ) == 0 && i + 1 < argc ) seconds = atof( argv[++i] );
        else if ( strcmp( argv[i], "--port" ) == 0 && i + 1 < argc ) port = atoi( argv[++i] );
    }
    if ( seconds == 0.0 )
        seconds = strcmp( mode, "client" ) == 0 ? 2.0 : 5.0;
    if ( clients < 1 || clients > MAX_CLIENTS || seconds < 1.0 || seconds > 600.0 || port < 1 || port > 65535 ||
         ( strcmp( mode, "listen" ) != 0 && strcmp( mode, "server" ) != 0 && strcmp( mode, "client" ) != 0 ) )
    {
        printf( "usage: example_game_loop [server|client] [--clients <1..64>] [--seconds <1..600>] [--port <n>]\n" );
        return 1;
    }

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init\n" );
        return 1;
    }
    int result = 1;
    if ( strcmp( mode, "server" ) == 0 )
        result = dedicated_mode( port );
    else if ( strcmp( mode, "client" ) == 0 )
        result = client_mode( seconds );
    else
        result = listen_mode( clients, seconds );
    rocketnet_term();

    // PASS is printed after shutdown so it stays the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
