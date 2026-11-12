/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The cross-endian interoperability check: one side of a real connection,
    run as a server on one byte order and as clients on the other
    (.github/workflows/nightly-bigendian-system.yml runs it both ways between
    the s390x guest and the x86-64 runner). Every wire field the library
    writes crosses between a little-endian and a big-endian end, so a field
    written in host order fails here even when it has no byte vector.

        bigendian_interop server <a.b.c.d:port> <clients>
        bigendian_interop client <a.b.c.d:port> <server public key hex> <clients>

    The server serves the address it is given (the address in the tokens),
    prints its public key, and waits for <clients> clients. Each client
    connects with a development token minted for that address, then sends,
    one per frame, UNRELIABLE_MESSAGES unreliable and RELIABLE_MESSAGES
    reliable-ordered messages and one LARGE_MESSAGE_BYTES reliable message,
    which the library sends in fragments. The server checks every byte of
    every message, that the sender's index in the message is the index it
    received from, and that the client id in it is the one in the client's
    token; it echoes each message back on the same reliability, and the
    client checks every byte of the echoes. Each client then disconnects; the
    server exits when every client has come and gone.

    It passes (exit 0) when every reliable message and its echo arrived
    intact, at least one unreliable message and one unreliable echo arrived
    per client, and nothing arrived damaged. The multi-byte fields inside
    the messages are written byte by byte, so the payload means the same on
    either end.

    A functional check of real sockets, not a unit test, and not an example.
    It is built under ROCKETNET_BUILD_EXAMPLES beside happy_path.
*/

#if !defined( ROCKETNET_DEV )
#define ROCKETNET_DEV 1
#endif // #if !defined( ROCKETNET_DEV )

#include "../../examples/shutdown.h"
#include "rocketnet.h"
#include "../../examples/dev_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INTEROP_MAX_CLIENTS 8
#define UNRELIABLE_MESSAGES 16
#define RELIABLE_MESSAGES 16
#define SMALL_MESSAGE_BYTES 200
#define LARGE_MESSAGE_BYTES ( 48 * 1024 )
#define HEADER_BYTES 18
#define DEADLINE_SECONDS 600.0

enum interop_kind_t
{
    INTEROP_UNRELIABLE = 1,
    INTEROP_RELIABLE = 2,
    INTEROP_LARGE = 3,
};

static void interop_write_u32( uint8_t * p, uint32_t value )
{
    p[0] = (uint8_t) ( value & 0xff );
    p[1] = (uint8_t) ( ( value >> 8 ) & 0xff );
    p[2] = (uint8_t) ( ( value >> 16 ) & 0xff );
    p[3] = (uint8_t) ( ( value >> 24 ) & 0xff );
}

static uint32_t interop_read_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

static uint64_t interop_read_u64( const uint8_t * p )
{
    return (uint64_t) interop_read_u32( p ) | ( (uint64_t) interop_read_u32( p + 4 ) << 32 );
}

static uint8_t interop_body_byte( int i, int kind, int sender, uint32_t number )
{
    return (uint8_t) ( (uint32_t) i * 131u + number * 7u + (uint32_t) sender * 29u + (uint32_t) kind + ( (uint32_t) i >> 8 ) );
}

// kind u8, sender index u8, number u32, length u32, client id u64, then the body

static void interop_fill( uint8_t * p, int bytes, int kind, int sender, uint32_t number, uint64_t client_id )
{
    p[0] = (uint8_t) kind;
    p[1] = (uint8_t) sender;
    interop_write_u32( p + 2, number );
    interop_write_u32( p + 6, (uint32_t) bytes );
    interop_write_u32( p + 10, (uint32_t) ( client_id & 0xffffffffu ) );
    interop_write_u32( p + 14, (uint32_t) ( client_id >> 32 ) );
    for ( int i = HEADER_BYTES; i < bytes; i++ )
    {
        p[i] = interop_body_byte( i, kind, sender, number );
    }
}

// 1 when the message is whole and its header is consistent with its length

static int interop_check( const uint8_t * p, int bytes, int * kind, int * sender, uint32_t * number, uint64_t * client_id )
{
    if ( bytes < HEADER_BYTES || interop_read_u32( p + 6 ) != (uint32_t) bytes )
    {
        return 0;
    }
    *kind = p[0];
    *sender = p[1];
    *number = interop_read_u32( p + 2 );
    *client_id = interop_read_u64( p + 10 );
    if ( *kind < INTEROP_UNRELIABLE || *kind > INTEROP_LARGE )
    {
        return 0;
    }
    if ( ( *kind == INTEROP_LARGE ) != ( bytes == LARGE_MESSAGE_BYTES ) )
    {
        return 0;
    }
    for ( int i = HEADER_BYTES; i < bytes; i++ )
    {
        if ( p[i] != interop_body_byte( i, *kind, *sender, *number ) )
        {
            return 0;
        }
    }
    return 1;
}

static enum rocketnet_reliability_t interop_reliability( int kind )
{
    return kind == INTEROP_UNRELIABLE ? ROCKETNET_RELIABILITY_UNRELIABLE : ROCKETNET_RELIABILITY_RELIABLE_ORDERED;
}

static int interop_parse_clients( const char * text )
{
    const int clients = atoi( text );
    if ( clients < 1 || clients > INTEROP_MAX_CLIENTS )
    {
        printf( "error: clients takes 1 .. %d, not \"%s\"\n", INTEROP_MAX_CLIENTS, text );
        return 0;
    }
    return clients;
}

struct interop_record_t
{
    int seen;
    uint64_t client_id;
    int unreliable;
    int reliable;
    int large;
};

static int interop_server( const char * address, int clients )
{
    struct rocketnet_server_config_t config;
    static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = clients;

    printf( "server required memory %llu bytes\n", (unsigned long long) rocketnet_server_required_memory( &config ) );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &create_status );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    if ( !server )
    {
        printf( "error: failed to create server: %s\n", rocketnet_status_name( create_status ) );
        return 1;
    }

    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );
    printf( "server public key " );
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        printf( "%02x", server_key[i] );
    }
    printf( "\n" );
    printf( "server started on %s, transport %s\n", address, rocketnet_server_transport_name( server ) );
    fflush( stdout );

    struct interop_record_t records[INTEROP_MAX_CLIENTS];
    memset( records, 0, sizeof( records ) );
    int seen = 0;
    int bad = 0;
    int result = 1;
    const double wall_start = rocketnet_time();

    while ( 1 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );

        rocketnet_server_update( server, now );

        for ( int index = 0; index < clients; index++ )
        {
            if ( rocketnet_server_client_connected( server, index ) && !records[index].seen )
            {
                records[index].seen = 1;
                records[index].client_id = rocketnet_server_client_id( server, index );
                seen++;
                printf( "client %d connected, client id %llu\n", index, (unsigned long long) records[index].client_id );
                fflush( stdout );
            }
        }

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                const int from = batch->client_index[i];
                int kind = 0;
                int sender = 0;
                uint32_t number = 0;
                uint64_t client_id = 0;
                if ( from < 0 || from >= clients ||
                     !interop_check( batch->data[i], batch->bytes[i], &kind, &sender, &number, &client_id ) ||
                     sender != from || client_id != records[from].client_id )
                {
                    printf( "BAD message from client %d: %d bytes, sender %d, number %u, client id %llu\n", from,
                            batch->bytes[i], sender, (unsigned) number, (unsigned long long) client_id );
                    fflush( stdout );
                    bad++;
                    continue;
                }

                if ( kind == INTEROP_UNRELIABLE )
                {
                    records[from].unreliable++;
                }
                else if ( kind == INTEROP_RELIABLE )
                {
                    records[from].reliable++;
                }
                else
                {
                    records[from].large++;
                }

                // the echo: the same bytes back on the same reliability

                uint8_t * echo = rocketnet_server_start_message( server, (uint64_t) from, 0, interop_reliability( kind ), batch->bytes[i] );
                if ( echo )
                {
                    memcpy( echo, batch->data[i], (size_t) batch->bytes[i] );
                    rocketnet_server_finish_message( server, batch->bytes[i] );
                }
                else if ( kind != INTEROP_UNRELIABLE )
                {
                    printf( "BAD: the echo of a reliable message to client %d was refused\n", from );
                    bad++;
                }
            }
        }

        if ( seen == clients && rocketnet_server_num_connected_clients( server ) == 0 )
        {
            result = 0;
            for ( int index = 0; index < clients; index++ )
            {
                const int ok = records[index].reliable == RELIABLE_MESSAGES && records[index].large == 1 && records[index].unreliable >= 1;
                printf( "client %d: %d of %d reliable, %d of 1 large, %d of %d unreliable: %s\n", index, records[index].reliable,
                        RELIABLE_MESSAGES, records[index].large, records[index].unreliable, UNRELIABLE_MESSAGES, ok ? "ok" : "FAIL" );
                if ( !ok )
                {
                    result = 1;
                }
            }
            break;
        }

        rocketnet_server_send_packets( server );

        if ( rocketnet_time() - wall_start > DEADLINE_SECONDS )
        {
            printf( "error: server timed out with %d of %d clients seen\n", seen, clients );
            break;
        }

        rocketnet_sleep( 0.005 );
    }

    rn_cli_server_stop_and_destroy( server );

    if ( bad )
    {
        printf( "server: %d damaged messages\n", bad );
        result = 1;
    }
    printf( "server %s\n", result == 0 ? "PASS" : "FAIL" );
    fflush( stdout );
    return result;
}

static int interop_parse_key( const char * hex, uint8_t * key )
{
    if ( strlen( hex ) != 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES )
    {
        return 0;
    }
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        int value = 0;
        for ( int j = 0; j < 2; j++ )
        {
            const char c = hex[2 * i + j];
            int nibble = -1;
            if ( c >= '0' && c <= '9' )
            {
                nibble = c - '0';
            }
            else if ( c >= 'a' && c <= 'f' )
            {
                nibble = c - 'a' + 10;
            }
            else if ( c >= 'A' && c <= 'F' )
            {
                nibble = c - 'A' + 10;
            }
            if ( nibble < 0 )
            {
                return 0;
            }
            value = value * 16 + nibble;
        }
        key[i] = (uint8_t) value;
    }
    return 1;
}

struct interop_client_t
{
    struct rocketnet_client_t * client;
    uint64_t client_id;
    int index;
    int connected;
    int sent_unreliable;
    int sent_reliable;
    int sent_large;
    int echo_unreliable;
    int echo_reliable;
    int echo_large;
    double all_sent_at;
    int done;
};

static int interop_send( struct interop_client_t * c, int kind, uint32_t number, int bytes )
{
    uint8_t * payload = rocketnet_client_start_message( c->client, 0, interop_reliability( kind ), bytes );
    if ( !payload )
    {
        return 0;
    }
    interop_fill( payload, bytes, kind, c->index, number, c->client_id );
    return rocketnet_client_finish_message( c->client, bytes ) != 0;
}

static int interop_clients( const char * address, const char * key_hex, int clients )
{
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !interop_parse_key( key_hex, server_key ) )
    {
        printf( "error: expected the server public key as %d hex characters\n", 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES );
        return 1;
    }

    struct interop_client_t c[INTEROP_MAX_CLIENTS];
    memset( c, 0, sizeof( c ) );
    int result = 0;
    int bad = 0;

    for ( int k = 0; k < clients; k++ )
    {
        // client ids with every byte distinct, so a field read in the wrong order cannot match

        c[k].client_id = UINT64_C( 0x0102030405060708 ) + (uint64_t) k * UINT64_C( 0x1010101010101010 );
        c[k].index = -1;

        char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        if ( rocketnet_create_dev_connect_token( address, NULL, server_key, 300, 60, c[k].client_id, EXAMPLE_PROTOCOL_ID,
                                                 (uint64_t) time( NULL ), NULL, connect_token ) != ROCKETNET_STATUS_OK )
        {
            printf( "error: failed to create dev connect token\n" );
            result = 1;
            break;
        }

        struct rocketnet_client_config_t config;
        rocketnet_client_default_config( &config );
        enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
        c[k].client = rocketnet_client_create( NULL, &config, &create_status );
        if ( c[k].client && rocketnet_client_connect( c[k].client, connect_token ) != ROCKETNET_STATUS_OK )
        {
            rn_cli_client_stop_and_destroy( c[k].client, 1000.0 );
            c[k].client = NULL;
        }
        if ( !c[k].client )
        {
            printf( "error: failed to create client %d: %s\n", k, rocketnet_status_name( create_status ) );
            result = 1;
            break;
        }
    }

    const double wall_start = rocketnet_time();
    int remaining = clients;

    while ( result == 0 && remaining > 0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        if ( rocketnet_time() - wall_start > DEADLINE_SECONDS )
        {
            printf( "error: clients timed out\n" );
            result = 1;
            break;
        }

        for ( int k = 0; k < clients; k++ )
        {
            struct interop_client_t * x = &c[k];
            if ( x->done )
            {
                continue;
            }

            rocketnet_client_update( x->client, now );

            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( x->client ) ) != NULL )
            {
                for ( int i = 0; i < batch->num_messages; i++ )
                {
                    int kind = 0;
                    int sender = 0;
                    uint32_t number = 0;
                    uint64_t client_id = 0;
                    if ( !interop_check( batch->data[i], batch->bytes[i], &kind, &sender, &number, &client_id ) ||
                         sender != x->index || client_id != x->client_id )
                    {
                        printf( "BAD echo to client %d: %d bytes\n", k, batch->bytes[i] );
                        bad++;
                        continue;
                    }
                    if ( kind == INTEROP_UNRELIABLE )
                    {
                        x->echo_unreliable++;
                    }
                    else if ( kind == INTEROP_RELIABLE )
                    {
                        x->echo_reliable++;
                    }
                    else
                    {
                        x->echo_large++;
                    }
                }
            }

            const int state = rocketnet_client_state( x->client );
            if ( state < 0 )
            {
                printf( "error: client %d failed: %s\n", k, rocketnet_client_state_name( state ) );
                result = 1;
                break;
            }

            if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                if ( !x->connected )
                {
                    x->connected = 1;
                    x->index = rocketnet_client_index( x->client );
                    printf( "client %d connected as index %d\n", k, x->index );
                    fflush( stdout );
                }

                // one message of each kind per frame until all are sent

                if ( x->sent_unreliable < UNRELIABLE_MESSAGES )
                {
                    if ( interop_send( x, INTEROP_UNRELIABLE, (uint32_t) x->sent_unreliable, SMALL_MESSAGE_BYTES ) )
                    {
                        x->sent_unreliable++;
                    }
                }
                if ( x->sent_reliable < RELIABLE_MESSAGES )
                {
                    if ( interop_send( x, INTEROP_RELIABLE, (uint32_t) x->sent_reliable, SMALL_MESSAGE_BYTES ) )
                    {
                        x->sent_reliable++;
                    }
                }
                else if ( !x->sent_large )
                {
                    if ( interop_send( x, INTEROP_LARGE, 0, LARGE_MESSAGE_BYTES ) )
                    {
                        x->sent_large = 1;
                        x->all_sent_at = now;
                    }
                }

                // done once every reliable echo is back and at least one
                // unreliable echo, with a second after the last send for
                // stragglers

                if ( x->sent_large && x->echo_reliable == RELIABLE_MESSAGES && x->echo_large == 1 &&
                     x->echo_unreliable >= 1 && now - x->all_sent_at >= 1.0 )
                {
                    printf( "client %d: %d of %d reliable echoes, %d of 1 large, %d of %d unreliable: ok\n", k, x->echo_reliable,
                            RELIABLE_MESSAGES, x->echo_large, x->echo_unreliable, UNRELIABLE_MESSAGES );
                    fflush( stdout );
                    rocketnet_client_disconnect( x->client );
                    rn_cli_client_wait_disconnected( x->client, now );
                    x->done = 1;
                    remaining--;
                    continue;
                }
            }

            rocketnet_client_send_packets( x->client );
        }

        rocketnet_sleep( 0.005 );
    }

    for ( int k = 0; k < clients; k++ )
    {
        if ( c[k].client )
        {
            if ( !c[k].done )
            {
                printf( "client %d: %d of %d reliable echoes, %d of 1 large, %d of %d unreliable: FAIL\n", k, c[k].echo_reliable,
                        RELIABLE_MESSAGES, c[k].echo_large, c[k].echo_unreliable, UNRELIABLE_MESSAGES );
                result = 1;
            }
            rn_cli_client_stop_and_destroy( c[k].client, 1000.0 + ( rocketnet_time() - wall_start ) );
        }
    }

    if ( bad )
    {
        printf( "clients: %d damaged echoes\n", bad );
        result = 1;
    }
    printf( "clients %s\n", result == 0 ? "PASS" : "FAIL" );
    fflush( stdout );
    return result;
}

int main( int argc, char ** argv )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    int result = 2;
    if ( argc == 4 && strcmp( argv[1], "server" ) == 0 )
    {
        const int clients = interop_parse_clients( argv[3] );
        result = clients ? interop_server( argv[2], clients ) : 2;
    }
    else if ( argc == 5 && strcmp( argv[1], "client" ) == 0 )
    {
        const int clients = interop_parse_clients( argv[4] );
        result = clients ? interop_clients( argv[2], argv[3], clients ) : 2;
    }
    else
    {
        printf( "usage: %s server <a.b.c.d:port> <clients>\n", argv[0] );
        printf( "       %s client <a.b.c.d:port> <server public key hex> <clients>\n", argv[0] );
    }

    rocketnet_term();
    return result;
}
