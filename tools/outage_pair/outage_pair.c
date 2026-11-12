/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The outage pair: one server and one client over real UDP sockets, driven
    by scripts/outage-pair-real across two network namespaces whose path it
    darkens with netem. Public API only.

        outage_pair server ADDRESS survive|timeout
        outage_pair client ADDRESS SERVER_KEY_HEX survive SEND_SECONDS
        outage_pair client ADDRESS SERVER_KEY_HEX timeout

    Both sides send, at 60 Hz, one RELIABLE_ORDERED message (a sequence
    number) and one UNRELIABLE message on channel 0, each through
    can_send_message, start_message and finish_message; a send any of the
    three refuses is counted as a refusal, never a failure. The receiver
    checks every reliable message is the next sequence: one already seen is a
    duplicate, one ahead is a gap.

    survive: no end of the connection on either side. The client sends for
    SEND_SECONDS after it connects, then a reliable STOP carrying its count
    of accepted reliable messages; the server answers with its own STOP. Each
    side passes when the other side's STOP arrived in order after exactly
    that many reliable messages, with no duplicate and no gap, and the only
    end is the client's own disconnect after both STOPs (server reason
    CLIENT_DISCONNECTED).

    timeout: the script holds the path dark past the connection timeout. The
    server passes when its one end is TIMED_OUT, the client when its one end
    is CONNECTION_TIMED_OUT; any other end fails.

    Every side prints one "outage_pair <side> PASS ..." or "... FAIL ..."
    line and exits 0 on PASS, 1 on FAIL. Each run is bounded in wall time.

    This is a functional test program, not an example. It is built beside
    client and server under ROCKETNET_BUILD_EXAMPLES.
*/

#if !defined( ROCKETNET_DEV )
#define ROCKETNET_DEV 1
#endif // #if !defined( ROCKETNET_DEV )

#include "../../examples/shutdown.h"
#include "rocketnet.h"
#include "../../examples/dev_keys.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// the development default connect token timeout, the T of the outage cases
#define OUTAGE_PAIR_TIMEOUT_SECONDS 10
#define OUTAGE_PAIR_EXPIRE_SECONDS 30
#define OUTAGE_PAIR_MESSAGE_BYTES 16
#define OUTAGE_PAIR_CHANNEL 0
#define OUTAGE_PAIR_FRAME_SECONDS ( 1.0 / 60.0 )
#define OUTAGE_PAIR_CONNECT_SECONDS 30.0
#define OUTAGE_PAIR_FINISH_SECONDS 60.0

#define OUTAGE_PAIR_KIND_RELIABLE 1
#define OUTAGE_PAIR_KIND_UNRELIABLE 2
#define OUTAGE_PAIR_KIND_STOP 3

struct outage_pair_side_t
{
    uint32_t reliable_sent;
    uint32_t reliable_refused;
    uint32_t unreliable_sent;
    uint32_t unreliable_refused;
    uint32_t stop_refused;
    uint32_t expected;
    uint32_t delivered;
    uint32_t duplicates;
    uint32_t gaps;
    uint32_t malformed;
    uint32_t unreliable_received;
    int stop_sent;
    int stop_received;
    int stop_in_order;
};

static void write_u32( uint8_t * p, uint32_t value )
{
    p[0] = (uint8_t) ( value );
    p[1] = (uint8_t) ( value >> 8 );
    p[2] = (uint8_t) ( value >> 16 );
    p[3] = (uint8_t) ( value >> 24 );
}

static uint32_t read_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

static void fill_message( uint8_t * p, int kind, uint32_t sequence )
{
    memset( p, 0, OUTAGE_PAIR_MESSAGE_BYTES );
    p[0] = (uint8_t) kind;
    write_u32( p + 1, sequence );
}

// one received message, either side
static void receive_one( struct outage_pair_side_t * side, const uint8_t * data, int bytes )
{
    if ( bytes != OUTAGE_PAIR_MESSAGE_BYTES )
    {
        side->malformed++;
        return;
    }

    const int kind = data[0];
    const uint32_t sequence = read_u32( data + 1 );

    if ( kind == OUTAGE_PAIR_KIND_UNRELIABLE )
    {
        side->unreliable_received++;
        return;
    }

    if ( kind != OUTAGE_PAIR_KIND_RELIABLE && kind != OUTAGE_PAIR_KIND_STOP )
    {
        side->malformed++;
        return;
    }

    if ( kind == OUTAGE_PAIR_KIND_STOP )
    {
        // the STOP carries the sender's count of accepted reliable messages:
        // in order, every one of them arrived before it

        side->stop_received++;
        side->stop_in_order = ( sequence == side->expected );
        return;
    }

    if ( side->stop_received )
    {
        side->malformed++;
    }
    else if ( sequence == side->expected )
    {
        side->delivered++;
        side->expected++;
    }
    else if ( sequence < side->expected )
    {
        side->duplicates++;
    }
    else
    {
        side->gaps++;
        side->expected = sequence + 1;
    }
}

static void print_counts( const char * who, const struct outage_pair_side_t * side )
{
    printf( "outage_pair %s counts reliable_sent %u reliable_refused %u unreliable_sent %u unreliable_refused %u stop_refused %u "
            "delivered %u duplicates %u gaps %u malformed %u unreliable_received %u stop_received %d stop_in_order %d\n",
            who, side->reliable_sent, side->reliable_refused, side->unreliable_sent, side->unreliable_refused, side->stop_refused,
            side->delivered, side->duplicates, side->gaps, side->malformed, side->unreliable_received, side->stop_received,
            side->stop_in_order );
}

static int side_delivered_clean( const struct outage_pair_side_t * side )
{
    return side->stop_received == 1 && side->stop_in_order && side->duplicates == 0 && side->gaps == 0 && side->malformed == 0;
}

static double frame_time( double wall_start )
{
    return 1000.0 + ( rocketnet_time() - wall_start );
}

// ---- timeline ----
//
// Every OUTAGE_PAIR_TIMELINE_SECONDS each side prints one line on the wall
// clock the script stamps its outages with (seconds since the epoch), so a
// failure shows, per side, when packets last went out and came in against
// when the path went dark and came back. The script prints these lines only
// when a case fails.

#define OUTAGE_PAIR_TIMELINE_SECONDS 0.1

static double wall_clock_seconds( void )
{
    struct timespec ts;
    if ( timespec_get( &ts, TIME_UTC ) != TIME_UTC )
    {
        return 0.0;
    }
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1.0e9;
}

static void print_timeline( const char * who, double now, uint64_t sent, uint64_t received, const struct rocketnet_conditions_t * conditions )
{
    printf( "outage_timeline %s wall %.3f now %.3f sent %llu received %llu heard %.3f progress %.3f in_flight %llu\n", who,
            wall_clock_seconds(), now, (unsigned long long) sent, (unsigned long long) received, conditions->seconds_since_heard,
            conditions->seconds_since_progress, (unsigned long long) conditions->reliable_in_flight );
    fflush( stdout );
}

// ---- server ----

static int server_send( struct rocketnet_server_t * server, enum rocketnet_reliability_t reliability, int kind, uint32_t sequence )
{
    if ( !rocketnet_server_can_send_message( server, 0, OUTAGE_PAIR_CHANNEL, reliability, OUTAGE_PAIR_MESSAGE_BYTES ) )
    {
        return 0;
    }

    uint8_t * p = rocketnet_server_start_message( server, 0, OUTAGE_PAIR_CHANNEL, reliability, OUTAGE_PAIR_MESSAGE_BYTES );
    if ( !p )
    {
        return 0;
    }

    fill_message( p, kind, sequence );

    return rocketnet_server_finish_message( server, OUTAGE_PAIR_MESSAGE_BYTES ) != 0;
}

static int run_server( const char * address, int expect_timeout )
{
    struct rocketnet_server_config_t config;

    static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = 1;
    config.transport = "portable";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &create_status );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }

    if ( !server )
    {
        printf( "outage_pair server FAIL create: %s\n", rocketnet_status_name( create_status ) );
        return 1;
    }

    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );
    printf( "outage_pair server public key " );

    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        printf( "%02x", server_key[i] );
    }

    printf( "\n" );
    fflush( stdout );

    struct outage_pair_side_t side;
    memset( &side, 0, sizeof( side ) );

    const double wall_start = rocketnet_time();
    double next_timeline = 0.0;
    double connected_at = 0.0;
    int connected = 0;
    enum rocketnet_disconnect_reason_t end_reason = ROCKETNET_DISCONNECT_REASON_NONE;
    const char * first_end = NULL;
    double first_end_at = 0.0;
    const char * failure = NULL;

    while ( 1 )
    {
        const double now = frame_time( wall_start );

        rocketnet_server_update( server, now );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                receive_one( &side, batch->data[i], batch->bytes[i] );
            }
        }

        if ( now >= next_timeline )
        {
            struct rocketnet_server_stats_t stats;
            struct rocketnet_conditions_t conditions;
            rocketnet_server_stats( server, &stats );
            if ( !rocketnet_server_client_conditions( server, 0, &conditions, (int) sizeof( conditions ) ) )
            {
                memset( &conditions, 0, sizeof( conditions ) );
            }
            print_timeline( "server", now, stats.packets_sent, stats.packets_received, &conditions );
            next_timeline = now + OUTAGE_PAIR_TIMELINE_SECONDS;
        }

        const int is_connected = rocketnet_server_client_connected( server, 0 );
        if ( is_connected && !connected )
        {
            connected = 1;
            connected_at = now;
            printf( "outage_pair server client connected\n" );
            fflush( stdout );
        }

        if ( connected && !is_connected )
        {
            // the slot's connection ended: the reason names how

            end_reason = rocketnet_server_disconnect_reason( server, 0 );
            first_end = rocketnet_disconnect_reason_name( end_reason );
            first_end_at = now - connected_at;
            printf( "outage_pair server end %s at +%.2f s after connect\n", first_end, first_end_at );
            fflush( stdout );
            break;
        }

        if ( connected )
        {
            if ( !side.stop_received )
            {
                if ( server_send( server, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, OUTAGE_PAIR_KIND_RELIABLE, side.reliable_sent ) )
                {
                    side.reliable_sent++;
                }
                else
                {
                    side.reliable_refused++;
                }

                if ( server_send( server, ROCKETNET_RELIABILITY_UNRELIABLE, OUTAGE_PAIR_KIND_UNRELIABLE, side.unreliable_sent ) )
                {
                    side.unreliable_sent++;
                }
                else
                {
                    side.unreliable_refused++;
                }
            }
            else if ( !side.stop_sent )
            {
                if ( server_send( server, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, OUTAGE_PAIR_KIND_STOP, side.reliable_sent ) )
                {
                    side.stop_sent = 1;
                }
                else
                {
                    side.stop_refused++;
                }
            }
        }

        rocketnet_server_send_packets( server );

        if ( !connected && now - 1000.0 > OUTAGE_PAIR_CONNECT_SECONDS )
        {
            failure = "no client connected";
            break;
        }

        if ( connected && now - connected_at > OUTAGE_PAIR_FINISH_SECONDS + 120.0 )
        {
            failure = "the run did not finish";
            break;
        }

        rocketnet_sleep( OUTAGE_PAIR_FRAME_SECONDS );
    }

    print_counts( "server", &side );

    if ( !failure && expect_timeout && end_reason != ROCKETNET_DISCONNECT_REASON_TIMED_OUT )
    {
        failure = "the end is not TIMED_OUT";
    }

    if ( !failure && !expect_timeout )
    {
        if ( end_reason != ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED )
        {
            failure = "the connection ended before the client's own disconnect";
        }
        else if ( !side.stop_sent )
        {
            failure = "the client disconnected before the server's STOP was accepted";
        }
        else if ( !side_delivered_clean( &side ) )
        {
            failure = "the client's reliable messages were not delivered exactly once in order";
        }
    }

    if ( failure )
    {
        printf( "outage_pair server FAIL %s (end %s)\n", failure, first_end ? first_end : "none" );
    }
    else
    {
        printf( "outage_pair server PASS end %s at +%.2f s, delivered %u reliable in order, refused %u reliable %u unreliable\n",
                first_end, first_end_at, side.delivered, side.reliable_refused, side.unreliable_refused );
    }

    fflush( stdout );

    rn_cli_server_stop_and_destroy( server );

    return failure ? 1 : 0;
}

// ---- client ----

static int client_send( struct rocketnet_client_t * client, enum rocketnet_reliability_t reliability, int kind, uint32_t sequence )
{
    if ( !rocketnet_client_can_send_message( client, OUTAGE_PAIR_CHANNEL, reliability, OUTAGE_PAIR_MESSAGE_BYTES ) )
    {
        return 0;
    }

    uint8_t * p = rocketnet_client_start_message( client, OUTAGE_PAIR_CHANNEL, reliability, OUTAGE_PAIR_MESSAGE_BYTES );
    if ( !p )
    {
        return 0;
    }

    fill_message( p, kind, sequence );

    return rocketnet_client_finish_message( client, OUTAGE_PAIR_MESSAGE_BYTES ) != 0;
}

static int parse_key( const char * key_hex, uint8_t * server_key )
{
    if ( strlen( key_hex ) != 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES )
    {
        return 0;
    }

    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        int value = 0;
        for ( int j = 0; j < 2; j++ )
        {
            const char c = key_hex[2 * i + j];
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

        server_key[i] = (uint8_t) value;
    }

    return 1;
}

static int run_client( const char * address, const char * key_hex, int expect_timeout, double send_seconds )
{
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !parse_key( key_hex, server_key ) )
    {
        printf( "outage_pair client FAIL the server public key is not 64 hex characters\n" );
        return 1;
    }

    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( rocketnet_create_dev_connect_token( address, NULL, server_key, OUTAGE_PAIR_EXPIRE_SECONDS, OUTAGE_PAIR_TIMEOUT_SECONDS, 1,
                                             EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token ) != ROCKETNET_STATUS_OK )
    {
        printf( "outage_pair client FAIL cannot mint a development connect token\n" );
        return 1;
    }

    struct rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "portable";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &create_status );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_client_stop_and_destroy( client, 1000.0 );
        client = NULL;
    }

    if ( !client )
    {
        printf( "outage_pair client FAIL create: %s\n", rocketnet_status_name( create_status ) );
        return 1;
    }

    printf( "outage_pair client token timeout %d s\n", OUTAGE_PAIR_TIMEOUT_SECONDS );

    struct outage_pair_side_t side;
    memset( &side, 0, sizeof( side ) );

    const double wall_start = rocketnet_time();
    double next_timeline = 0.0;
    double connected_at = 0.0;
    int connected = 0;
    int end_state = 0;
    double end_at = 0.0;
    int done = 0;
    const char * failure = NULL;

    while ( 1 )
    {
        const double now = frame_time( wall_start );

        rocketnet_client_update( client, now );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                receive_one( &side, batch->data[i], batch->bytes[i] );
            }
        }

        if ( now >= next_timeline )
        {
            struct rocketnet_client_stats_t stats;
            struct rocketnet_conditions_t conditions;
            rocketnet_client_stats( client, &stats );
            if ( !rocketnet_client_conditions( client, &conditions, (int) sizeof( conditions ) ) )
            {
                memset( &conditions, 0, sizeof( conditions ) );
            }
            print_timeline( "client", now, stats.packets_sent, stats.packets_received, &conditions );
            next_timeline = now + OUTAGE_PAIR_TIMELINE_SECONDS;
        }

        const int state = rocketnet_client_state( client );
        if ( state < 0 )
        {
            end_state = state;
            end_at = connected ? now - connected_at : 0.0;
            printf( "outage_pair client end %s at +%.2f s after connect\n", rocketnet_client_state_name( (enum rocketnet_client_state_t) state ), end_at );
            fflush( stdout );
            break;
        }

        if ( state == ROCKETNET_CLIENT_STATE_CONNECTED && !connected )
        {
            connected = 1;
            connected_at = now;
            printf( "outage_pair client connected\n" );
            fflush( stdout );
        }

        if ( connected )
        {
            if ( expect_timeout || now - connected_at < send_seconds )
            {
                if ( client_send( client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, OUTAGE_PAIR_KIND_RELIABLE, side.reliable_sent ) )
                {
                    side.reliable_sent++;
                }
                else
                {
                    side.reliable_refused++;
                }

                if ( client_send( client, ROCKETNET_RELIABILITY_UNRELIABLE, OUTAGE_PAIR_KIND_UNRELIABLE, side.unreliable_sent ) )
                {
                    side.unreliable_sent++;
                }
                else
                {
                    side.unreliable_refused++;
                }
            }
            else if ( !side.stop_sent )
            {
                if ( client_send( client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, OUTAGE_PAIR_KIND_STOP, side.reliable_sent ) )
                {
                    side.stop_sent = 1;
                }
                else
                {
                    side.stop_refused++;
                }
            }
            else if ( side.stop_received )
            {
                // both STOPs are through: the client's own disconnect is the one end

                done = 1;
            }
        }

        rocketnet_client_send_packets( client );

        if ( done )
        {
            break;
        }

        if ( !connected && now - 1000.0 > OUTAGE_PAIR_CONNECT_SECONDS )
        {
            failure = "did not connect";
            break;
        }

        if ( connected && now - connected_at > send_seconds + OUTAGE_PAIR_FINISH_SECONDS )
        {
            failure = "the run did not finish";
            break;
        }

        rocketnet_sleep( OUTAGE_PAIR_FRAME_SECONDS );
    }

    print_counts( "client", &side );

    if ( !failure && !connected )
    {
        failure = "did not connect";
    }

    if ( !failure && expect_timeout && end_state != ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT )
    {
        failure = "the end is not CONNECTION_TIMED_OUT";
    }

    if ( !failure && !expect_timeout )
    {
        if ( end_state != 0 )
        {
            failure = "the connection ended";
        }
        else if ( !side_delivered_clean( &side ) )
        {
            failure = "the server's reliable messages were not delivered exactly once in order";
        }
    }

    const char * end_name = end_state ? rocketnet_client_state_name( (enum rocketnet_client_state_t) end_state ) : "none";

    if ( failure )
    {
        printf( "outage_pair client FAIL %s (end %s)\n", failure, end_name );
    }
    else if ( expect_timeout )
    {
        printf( "outage_pair client PASS end %s at +%.2f s, refused %u reliable %u unreliable\n", end_name, end_at,
                side.reliable_refused, side.unreliable_refused );
    }
    else
    {
        printf( "outage_pair client PASS no end, delivered %u reliable in order, refused %u reliable %u unreliable\n",
                side.delivered, side.reliable_refused, side.unreliable_refused );
    }

    fflush( stdout );

    rn_cli_client_stop_and_destroy( client, frame_time( wall_start ) );

    return failure ? 1 : 0;
}

static int usage( void )
{
    printf( "usage: outage_pair server ADDRESS survive|timeout\n" );
    printf( "       outage_pair client ADDRESS SERVER_KEY_HEX survive SEND_SECONDS\n" );
    printf( "       outage_pair client ADDRESS SERVER_KEY_HEX timeout\n" );
    return 2;
}

int main( int argc, char ** argv )
{
    if ( argc < 4 )
    {
        return usage();
    }

    const int is_server = strcmp( argv[1], "server" ) == 0;
    const int is_client = strcmp( argv[1], "client" ) == 0;
    const char * mode = is_server ? argv[3] : ( argc > 4 ? argv[4] : "" );
    const int expect_timeout = strcmp( mode, "timeout" ) == 0;
    const int survive = strcmp( mode, "survive" ) == 0;

    if ( ( !is_server && !is_client ) || ( !expect_timeout && !survive ) )
    {
        return usage();
    }

    double send_seconds = 0.0;
    if ( is_client && survive )
    {
        if ( argc < 6 )
        {
            return usage();
        }

        send_seconds = atof( argv[5] );
        if ( send_seconds <= 0.0 )
        {
            return usage();
        }
    }

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "outage_pair %s FAIL cannot initialize rocketnet\n", argv[1] );
        return 1;
    }

    const int result = is_server ? run_server( argv[2], expect_timeout ) : run_client( argv[2], argv[3], expect_timeout, send_seconds );

    rocketnet_term();

    return result;
}
