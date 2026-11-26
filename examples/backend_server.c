/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Backend server: the server a backend sends clients to. It takes the
    backend public key from ROCKETNET_SERVER_BACKEND_PUBLIC_KEY (or the
    published development key in a development build) and listens on
    127.0.0.1 at the port from --port <n>, else ROCKETNET_EXAMPLE_PORT, else
    40000. The backend mints each client's token for this server's address and
    public key, so the server registers both with the backend
    (PUT /servers/{id}), prints them, and writes them to backend.env as the
    two environment variables the backends in examples/backend/ read. It
    echoes each client's message, and exits once a client has disconnected
    (--sessions N: once N clients have come and gone).

    The whitelist. Every few seconds the server fetches the client ids the
    backend allows on it (GET /servers/{id}/whitelist), on its own thread
    (whitelist.h), and disconnects any connected client whose client id is
    not on the list: a banned user, or a user the backend has since matched
    to another server. A client is judged by the first list asked for after
    it connected, or by the newest list once it has been connected for
    WHITELIST_GRACE seconds. A failed fetch keeps the last good list, and
    with no list at all no client is allowed past the grace: the server
    fails closed. The backend is at 127.0.0.1:8080 unless --backend
    <ip:port> says otherwise, and the server's id is example-<port> unless
    --server-id <id> says otherwise.

    The server runs a 60 Hz frame. By default (--game-threads 1) every server
    call is made on the main thread, one thread at a time. With
    --game-threads N it runs the way a game server runs it: the main thread
    reads what the last update left and does the world update, then the
    second half of the network update (queue the echoes, send, update in
    three calls) is split off to run on another thread in parallel with the
    main thread, and joined at the start of the next frame.

    Run this server, then a backend, then example_backend_client. See
    examples/backend/README.md.
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates getenv and fopen

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include "whitelist.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV_FILE "backend.env"

#define FRAME_DT        ( 1.0 / 60.0 )
#define MAX_CLIENTS     4
#define WHITELIST_SECONDS   2.0     // how often the server fetches its whitelist
#define WHITELIST_GRACE     ( 3.0 * WHITELIST_SECONDS )    // judged by the newest list after this long, fetched or not
#define MAX_ECHOES      64          // messages echoed in one frame
#define MAX_ECHO_BYTES  256         // a longer message is printed, not echoed

// One frame of the game's state: what the main thread took from the network,
// for the second half of the network update to send back. The server keeps
// two and swaps them at the frame boundary, so the main thread never writes
// the frame the other thread is sending from.

struct echo_frame_t
{
    struct rocketnet_server_t * server;
    int game_threads;   // 1: every call on the main thread; more: the second half is split off
    int max_clients;
    double now;         // the game clock for this frame's update
    struct rn_cli_pool_t * pool;     // the game threads, NULL with one
    int num_messages;
    int client_index[MAX_ECHOES];
    int bytes[MAX_ECHOES];
    uint8_t data[MAX_ECHOES][MAX_ECHO_BYTES];
    int echoed[MAX_ECHOES];     // written by the thread that queued the message, read by the main thread after the join
};

// Queue the echoes for the clients in [begin, end). One thread queues a
// client's messages, so each game thread takes a range of clients. A start
// with no room returns NULL and the echo is skipped.

static void queue_range( void * data, int begin, int end )
{
    struct echo_frame_t * frame = (struct echo_frame_t *) data;

    for ( int i = 0; i < frame->num_messages; i++ )
    {
        const int client = frame->client_index[i];
        if ( client < begin || client >= end )
        {
            continue;
        }

        // game thread start: no can_send_message preflight (that call is a main
        // thread call); a start with no room returns NULL and is skipped below
        uint8_t * p = rocketnet_server_start_message( frame->server, (uint64_t) client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, frame->bytes[i] );
        if ( p )
        {
            memcpy( p, frame->data[i], (size_t) frame->bytes[i] );

            // game threads finish by client index; one thread may use the plain call
            const uint32_t serial = frame->game_threads > 1 ? rocketnet_server_finish_client_message( frame->server, client, frame->bytes[i] ) : rocketnet_server_finish_message( frame->server, frame->bytes[i] );
            frame->echoed[i] = serial != 0;
        }
    }
}

// Apply the acknowledgements of the clients in [begin, end).

static void update_range( void * data, int begin, int end )
{
    struct echo_frame_t * frame = (struct echo_frame_t *) data;

    for ( int i = begin; i < end; i++ )
    {
        (void) rocketnet_server_update_client( frame->server, i );
    }
}

// The second half of the network update: queue this frame's echoes, send,
// then (split off only) update. With one game thread it runs on the main
// thread, after the update the frame began with. With more it is split off
// to run on another thread in parallel with the main thread, which does not
// touch the server until the join; the calls the game threads make are
// joined before the thread's next call. This server has one client slot
// (MAX_CLIENTS), so the ranges below give all the work to one thread; a game
// with many clients spreads them across its game threads.

static void network_half( void * data )
{
    struct echo_frame_t * frame = (struct echo_frame_t *) data;

    rn_cli_pool_run( frame->pool, frame->max_clients, queue_range, frame );
    rocketnet_server_send_packets( frame->server );

    if ( frame->game_threads > 1 )
    {
        rocketnet_server_update_receive( frame->server, frame->now );
        rn_cli_pool_run( frame->pool, frame->max_clients, update_range, frame );
        rocketnet_server_update_finish( frame->server );
    }
}

static int any_echoed( const struct echo_frame_t * frame )
{
    for ( int i = 0; i < frame->num_messages; i++ )
    {
        if ( frame->echoed[i] )
        {
            return 1;
        }
    }

    return 0;
}

// standard base64 with padding, the form the backends read keys in
static void write_base64( const uint8_t * bytes, int count, char * output )
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    for ( int i = 0; i < count; i += 3, output += 4 )
    {
        const uint32_t b = i + 1 < count ? bytes[i + 1] : 0u;
        const uint32_t c = i + 2 < count ? bytes[i + 2] : 0u;
        const uint32_t triple = ( (uint32_t) bytes[i] << 16 ) | ( b << 8 ) | c;
        output[0] = digits[( triple >> 18 ) & 63];
        output[1] = digits[( triple >> 12 ) & 63];
        output[2] = i + 1 < count ? digits[( triple >> 6 ) & 63] : '=';
        output[3] = i + 2 < count ? digits[triple & 63] : '=';
    }

    *output = '\0';
}

int main( int argc, char ** argv )
{
    const char * port = getenv( "ROCKETNET_EXAMPLE_PORT" );

    int game_threads = 1;
    int max_sessions = 1;
    const char * backend_address = "127.0.0.1:8080";
    const char * server_id = NULL;

    for ( int i = 1; i + 1 < argc; i++ )
    {
        if ( strcmp( argv[i], "--port" ) == 0 )
        {
            port = argv[i + 1];
        }
        else if ( strcmp( argv[i], "--backend" ) == 0 )
        {
            backend_address = argv[i + 1];
        }
        else if ( strcmp( argv[i], "--server-id" ) == 0 )
        {
            server_id = argv[i + 1];
        }
        else if ( strcmp( argv[i], "--sessions" ) == 0 )
        {
            char * count_end = NULL;
            const long count = strtol( argv[i + 1], &count_end, 10 );
            if ( argv[i + 1][0] < '0' || argv[i + 1][0] > '9' || *count_end != '\0' || count < 1 || count > 1000 )
            {
                printf( "FAIL: --sessions takes 1 .. 1000, not \"%s\"\n", argv[i + 1] );
                return 1;
            }

            max_sessions = (int) count;
        }
        else if ( strcmp( argv[i], "--game-threads" ) == 0 )
        {
            char * count_end = NULL;
            const long count = strtol( argv[i + 1], &count_end, 10 );
            if ( argv[i + 1][0] < '0' || argv[i + 1][0] > '9' || *count_end != '\0' || count < 1 || count > RN_CLI_MAX_GAME_THREADS )
            {
                printf( "FAIL: --game-threads takes 1 .. %d, not \"%s\"\n", RN_CLI_MAX_GAME_THREADS, argv[i + 1] );
                return 1;
            }

            game_threads = (int) count;
        }
    }

    char * end = NULL;
    const long port_number = port ? strtol( port, &end, 10 ) : 40000;
    if ( port && ( port[0] < '0' || port[0] > '9' || *end != '\0' || port_number < 1 || port_number > 65535 ) )
    {
        printf( "FAIL: the port takes 1 .. 65535, not \"%s\"\n", port );
        return 1;
    }

    char address[32];
    snprintf( address, sizeof( address ), "127.0.0.1:%ld", port_number );

    // the whitelist fetch's settings, fixed before its thread starts

    static struct rn_cli_whitelist_fetch_t fetch;
    memset( &fetch, 0, sizeof( fetch ) );
    if ( strlen( backend_address ) >= sizeof( fetch.backend_address ) || ( server_id && ( strlen( server_id ) == 0 || strlen( server_id ) > 32 || strspn( server_id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_." ) != strlen( server_id ) ) ) )
    {
        printf( "FAIL: --backend takes ip:port and --server-id takes 1 to 32 letters, digits, '-', '_' or '.'\n" );
        return 1;
    }
    snprintf( fetch.backend_address, sizeof( fetch.backend_address ), "%s", backend_address );
    if ( server_id )
    {
        snprintf( fetch.server_id, sizeof( fetch.server_id ), "%s", server_id );
    }
    else
    {
        snprintf( fetch.server_id, sizeof( fetch.server_id ), "example-%ld", port_number );
    }

    // a file left by an earlier run names a key this server does not use

    remove( ENV_FILE );

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the packet filter is the title's, shared with its clients. A deployment's
    // server takes its backend's public key through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY
    // or in config; a production library (ROCKETNET_DEV=0) refuses the
    // development key at start

    const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = EXAMPLE_BACKEND_PACKET_FILTER;
    struct rocketnet_server_config_t config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    config.max_clients = MAX_CLIENTS;
    config.transport = "portable";
    memcpy( config.packet_filter, packet_filter, sizeof( packet_filter ) );
#if ROCKETNET_DEV
    const uint8_t dev_backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( dev_backend_public_key ) );
#endif // #if ROCKETNET_DEV

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &status );
    if ( server && status == ROCKETNET_STATUS_OK )
    {
        status = rocketnet_server_start( server );
    }

    if ( !server || status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( status ) );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    printf( "backend_server: mode=%s transport=%s\n", ROCKETNET_DEV ? "development" : "production", rocketnet_server_transport_name( server ) );

    // a server registers its address and public key with the backend; this
    // one prints them and writes them as the backend's environment

    char served_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char server_public_key_base64[4 * ( ( ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 2 ) / 3 ) + 1];
    if ( !rocketnet_server_ipv4_address( server, served_address, sizeof( served_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    rocketnet_server_public_key( server, server_public_key );
    write_base64( server_public_key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES, server_public_key_base64 );

    FILE * env = fopen( ENV_FILE, "w" );
    if ( !env )
    {
        printf( "FAIL: could not write %s\n", ENV_FILE );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    fprintf( env, "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\nROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", served_address, server_public_key_base64 );
    fclose( env );

    printf( "server listening on %s with protocol id 0x%llx\n", served_address, (unsigned long long) EXAMPLE_PROTOCOL_ID );
    printf( "ROCKETNET_BACKEND_SERVER_ADDRESSES=%s\n", served_address );
    printf( "ROCKETNET_BACKEND_SERVER_PUBLIC_KEY=%s\n", server_public_key_base64 );
    printf( "server wrote %s for the backend\n", ENV_FILE );
    fflush( stdout );

    // register with the backend and fetch the whitelist, on another thread

    snprintf( fetch.registration, sizeof( fetch.registration ), "{\"addresses\":[\"%s\"],\"public_key\":\"%s\"}", served_address, server_public_key_base64 );
    const int sockets_started = rn_cli_http_init();
    rn_cli_lock_init( &fetch.lock );
    struct rn_cli_thread_t fetch_thread;
    memset( &fetch_thread, 0, sizeof( fetch_thread ) );
    if ( !sockets_started || !rn_cli_thread_start( &fetch_thread, rn_cli_whitelist_thread, &fetch ) )
    {
        printf( "FAIL: could not start the whitelist thread\n" );
        rn_cli_lock_destroy( &fetch.lock );
        if ( sockets_started )
        {
            rn_cli_http_term();
        }
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    printf( "server fetches its whitelist from %s every %.1f seconds as %s; a client not on it is kicked after at most %.1f seconds\n", fetch.backend_address, WHITELIST_SECONDS, fetch.server_id, WHITELIST_GRACE );
    fflush( stdout );

    // what the server knows of each slot's connection: its id, so a new
    // connection in the slot is seen, and when it connected

    struct slot_t
    {
        uint64_t connection_id;
        uint64_t client_id;
        double connected_at;
    };

    static struct slot_t slots[MAX_CLIENTS];
    double next_fetch = 0.0;
    int have_list = 0, sessions = 0;

    static struct echo_frame_t frames[2];
    struct echo_frame_t * split_off = NULL;     // the frame the other thread is sending from, until the join
    struct rn_cli_thread_t network;
    memset( &network, 0, sizeof( network ) );
    static struct rn_cli_pool_t pool;     // the stand-in game threads, started once
    if ( game_threads > 1 )
    {
        rn_cli_pool_start( &pool, game_threads );
    }

    int echoed = 0, thread_failed = 0;
    const double start = rocketnet_time();
    double frame_end = start + FRAME_DT;
    for ( uint32_t frame_number = 0; rocketnet_time() - start < 60.0; frame_number++ )
    {
        // 1. join: wait for last frame's second half of the network update.
        // The main thread owns the server again from here

        rn_cli_thread_join( &network );
        if ( split_off )
        {
            echoed |= any_echoed( split_off );
            split_off = NULL;
        }

        // 2. a bit of networking on the main thread: take in the messages the
        // last update left, and copy each into this frame's state, so nothing
        // reads the batch after the next update

        struct echo_frame_t * frame = &frames[frame_number % 2];
        frame->server = server;
        frame->game_threads = game_threads;
        frame->max_clients = MAX_CLIENTS;
        frame->now = 1000.0 + rocketnet_time() - start;
        frame->num_messages = 0;
        frame->pool = game_threads > 1 ? &pool : NULL;
        memset( frame->echoed, 0, sizeof( frame->echoed ) );

        // by default the update comes first, in the order of the integration
        // guide: update, receive, queue, then send last

        if ( game_threads == 1 )
        {
            rocketnet_server_update( server, frame->now );
        }

        // one queue across every client, each message naming its sender

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                const int client_index = batch->client_index[i];
                printf( "server received from client %llx: %.*s\n", (unsigned long long) rocketnet_server_client_id( server, client_index ), batch->bytes[i], (const char *) batch->data[i] );
                if ( frame->num_messages < MAX_ECHOES && batch->bytes[i] <= MAX_ECHO_BYTES )
                {
                    frame->client_index[frame->num_messages] = client_index;
                    frame->bytes[frame->num_messages] = batch->bytes[i];
                    memcpy( frame->data[frame->num_messages], batch->data[i], (size_t) batch->bytes[i] );
                    frame->num_messages++;
                }
            }
        }

        // 3. the world update, on the main thread: a game simulates here.
        // This server keeps its clients to the whitelist, and notes when
        // its clients come and go

        const double game_time = frame->now - 1000.0;
        const int ask = game_time >= next_fetch;
        if ( ask )
        {
            next_fetch = game_time + WHITELIST_SECONDS;
        }
        const struct rn_cli_whitelist_t * whitelist = rn_cli_whitelist_frame( &fetch, game_time, ask, &have_list );

        for ( int i = 0; i < MAX_CLIENTS; i++ )
        {
            struct slot_t * slot = &slots[i];
            const uint64_t connection_id = rocketnet_server_client_connected( server, i ) ? rocketnet_server_client_connection_id( server, i ) : 0;
            if ( connection_id != slot->connection_id )
            {
                if ( slot->connection_id != 0 )
                {
                    printf( "server client %.16llx disconnected from slot %d (reason %s)\n", (unsigned long long) slot->client_id, i, rocketnet_disconnect_reason_name( rocketnet_server_disconnect_reason( server, i ) ) );
                }
                slot->connection_id = connection_id;
                slot->client_id = connection_id ? rocketnet_server_client_id( server, i ) : 0;
                slot->connected_at = game_time;
                if ( connection_id )
                {
                    sessions++;
                    // the address the client's packets come from: for logs
                    // and support only, never for bans (bans go by client id
                    // and the whitelist, docs/BANS-AND-WHITELISTS.md)
                    char client_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
                    rocketnet_server_client_address( server, i, client_address, sizeof( client_address ) );
                    printf( "server client %.16llx connected in slot %d from %s\n", (unsigned long long) slot->client_id, i, client_address );
                }
                fflush( stdout );
            }

            if ( !connection_id || ( whitelist && rn_cli_whitelist_contains( whitelist, slot->client_id ) ) )
            {
                continue;
            }

            // not on the list: judged once a list asked for after it
            // connected has arrived, or once the grace is over, list or none

            const int judged = ( whitelist && whitelist->requested_at > slot->connected_at ) || game_time - slot->connected_at > WHITELIST_GRACE;
            if ( judged )
            {
                printf( "server kicked client %.16llx from slot %d: not on the whitelist%s\n", (unsigned long long) slot->client_id, i, whitelist ? "" : " (no whitelist yet)" );
                fflush( stdout );
                rocketnet_server_disconnect_client( server, i );
            }
        }

        if ( sessions >= max_sessions && rocketnet_server_num_connected_clients( server ) == 0 )
        {
            // the last client's disconnect is printed above, the frame its slot frees
            int all_seen = 1;
            for ( int i = 0; i < MAX_CLIENTS; i++ )
            {
                all_seen &= slots[i].connection_id == 0;
            }
            if ( all_seen )
            {
                break;
            }
        }

        // 4. the second half of the network update. By default it runs here,
        // on the main thread. With game threads it is split off: another
        // thread queues this frame's echoes, sends and updates in parallel
        // with the main thread, which must not touch the server until the
        // join at the start of the next frame

        if ( game_threads == 1 )
        {
            network_half( frame );
            echoed |= any_echoed( frame );
        }
        else if ( !rn_cli_thread_start( &network, network_half, frame ) )
        {
            thread_failed = 1;
            break;
        }
        else
        {
            split_off = frame;
        }

        // 5. the main thread waits for the frame edge; a game's main thread
        // goes on with the rest of its frame here

        const double now = rocketnet_time();
        if ( now < frame_end )
        {
            rocketnet_sleep( frame_end - now );
        }

        frame_end = now > frame_end + FRAME_DT ? now + FRAME_DT : frame_end + FRAME_DT;
    }

    rn_cli_thread_join( &network );
    if ( split_off )
    {
        echoed |= any_echoed( split_off );
    }

    rn_cli_lock_acquire( &fetch.lock );
    fetch.stop = 1;
    rn_cli_lock_release( &fetch.lock );
    rn_cli_thread_join( &fetch_thread );
    rn_cli_lock_destroy( &fetch.lock );
    rn_cli_http_term();

    if ( game_threads > 1 )
    {
        rn_cli_pool_stop( &pool );
    }

    if ( thread_failed )
    {
        printf( "FAIL: could not start a thread\n" );
    }

    rn_cli_server_stop_and_destroy( server );

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    const int passed = sessions >= max_sessions && echoed && !thread_failed;
    printf( passed ? "PASS\n" : "FAIL: a client connected and its message was echoed\n" );
    return passed ? 0 : 1;
}
