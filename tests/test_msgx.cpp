/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Exercise the actual Darwin pump with controlled syscall refusal. Function
// pointer substitution is confined to this executable; SDK sources are unchanged.
// The msgx seam (the pause-expiry race hook) is ROCKETNET_DEV code.
#include "../sdk/rocketnet_platform.cpp"
#include <stdio.h>
#include <stdlib.h>

#define CHECK( x ) do { if (!(x)) { fprintf(stderr, "msgx check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)
static rocketnet_sendmsg_x_fn real_send;
static rocketnet_recvmsg_x_fn real_receive;
static int send_calls, receive_calls;
static bool refuse_send, refuse_receive, interrupt_send;

static ssize_t controlled_send( int s, const rocketnet_msghdr_x_t * messages, u_int count, int flags )
{
    ++send_calls;

    if ( interrupt_send )
    {
        errno = EINTR;
        return -1;
    }

    if ( refuse_send )
    {
        errno = ENOTSUP;
        return -1;
    }

    return real_send( s, messages, count, flags );
}
static ssize_t controlled_receive( int s, rocketnet_msghdr_x_t * messages, u_int count, int flags )
{
    ++receive_calls;

    if ( refuse_receive )
    {
        errno = ENOTSUP;
        return -1;
    }

    return real_receive( s, messages, count, flags );
}

static void drain( rocketnet_platform_socket_t * socket, const rocketnet_address_t & address, int expected )
{
    bool seen[80] = {};
    int received = 0;
    double deadline = rocketnet_platform_time() + 5.0;
    while ( received < expected && rocketnet_platform_time() < deadline )
    {
        rocketnet_address_t from[80];
        uint8_t data[80][32];
        int bytes[80];
        int count = rocketnet_platform_socket_receive_packets( socket, from, &data[0][0], 32, bytes, 80 );
        for ( int i = 0; i < count; ++i )
        {
            CHECK( bytes[i] == 32 && data[i][0] < expected );
            CHECK( !seen[data[i][0]] && rocketnet_address_equal( &from[i], &address ) );
            seen[data[i][0]] = true;
            for ( int j = 1; j < 32; ++j )
            {
                CHECK( data[i][j] == uint8_t( data[i][0] + j ) );
            }
            ++received;
        }

        if ( !count )
        {
            rocketnet_platform_sleep( 0.001 );
        }
    }

    CHECK( received == expected );
}

// Send failures by errno: pure counting on a zeroed table, no socket and no
// clock. The first failure of each call and errno kind reports first, every
// later one only counts, and the format names each nonzero count once.

static void test_msgx_send_errnos_count()
{
    rocketnet_platform_send_errnos_t errnos;
    memset( &errnos, 0, sizeof( errnos ) );
    CHECK( rocketnet_platform_send_errno_kind( 0 ) == ROCKETNET_PLATFORM_SEND_ERRNO_NONE );
    CHECK( rocketnet_platform_send_errno_kind( EAGAIN ) == ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN );
    CHECK( rocketnet_platform_send_errno_kind( EWOULDBLOCK ) == ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN );
    CHECK( rocketnet_platform_send_errno_kind( ENOBUFS ) == ROCKETNET_PLATFORM_SEND_ERRNO_ENOBUFS );
    CHECK( rocketnet_platform_send_errno_kind( EACCES ) == ROCKETNET_PLATFORM_SEND_ERRNO_EPERM );
    CHECK( rocketnet_platform_send_errno_kind( ENOTSUP ) == ROCKETNET_PLATFORM_SEND_ERRNO_OTHER );
    CHECK( strcmp( rocketnet_platform_send_errno_kind_name( ROCKETNET_PLATFORM_SEND_ERRNO_ENOBUFS ), "ENOBUFS" ) == 0 );
    CHECK( strcmp( rocketnet_platform_send_errno_kind_name( ROCKETNET_PLATFORM_SEND_ERRNO_KINDS ), "unknown" ) == 0 );
    CHECK( strcmp( rocketnet_platform_send_call_name( ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X ), "sendmsg_x" ) == 0 );

    char text[256];
    CHECK( rocketnet_platform_send_errnos_format( &errnos, text, sizeof( text ) ) == 0 && text[0] == '\0' );

    // first EAGAIN on sendmsg_x reports first; the next 999 only count

    CHECK( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, EAGAIN ) );
    for ( int i = 0; i < 999; ++i )
    {
        CHECK( !rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, EWOULDBLOCK ) );
    }

    // the same errno on another call is its own first

    CHECK( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_REPLAY, EAGAIN ) );
    CHECK( rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ENOTSUP ) );
    CHECK( !rocketnet_platform_send_errnos_count( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, 100000 ) );
    CHECK( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 1000 );
    CHECK( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_REPLAY, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 1 );
    CHECK( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ROCKETNET_PLATFORM_SEND_ERRNO_OTHER ) == 2 );
    CHECK( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALL_SENDTO, ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN ) == 0 );
    CHECK( rocketnet_platform_send_errnos_read( &errnos, ROCKETNET_PLATFORM_SEND_CALLS, 0 ) == 0 );
    CHECK( errnos.other_error == (uint64_t) ENOTSUP ); // the first other errno, kept

    CHECK( rocketnet_platform_send_errnos_format( &errnos, text, sizeof( text ) ) == 3 );
    CHECK( strcmp( text, "sendto other=2 sendmsg_x EAGAIN=1000 replay sendto EAGAIN=1" ) == 0 );

    // a short buffer keeps what fit and stays terminated

    char small[20];
    CHECK( rocketnet_platform_send_errnos_format( &errnos, small, sizeof( small ) ) == 1 );
    CHECK( strcmp( small, "sendto other=2" ) == 0 );
    printf( "test_msgx_send_errnos_count\n" );
}

int main()
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_forbid_xdp_commit();
    test_msgx_send_errnos_count();

    // the TX and RX workers share one msgx socket: a re-probe clears only the
    // expired pause it read, so a fresh pause set by the other worker wins

    CHECK( rocketnet_msgx_test_pause_expiry_race() );
    real_send = rocketnet_sendmsg_x;
    real_receive = rocketnet_recvmsg_x;
    CHECK( real_send && real_receive );

    rocketnet_address_t address;
    CHECK( rocketnet_address_parse( &address, "127.0.0.1:0" ) );

    auto * socket = rocketnet_platform_socket_create_with_pump( NULL, &address,
                                                                ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024 * 1024, 1024 * 1024, "msgx" );
    CHECK( socket && strcmp( rocketnet_platform_socket_pump( socket ), "msgx" ) == 0 );
    rocketnet_sendmsg_x = controlled_send;
    rocketnet_recvmsg_x = controlled_receive;

    uint8_t data[80][32];
    rocketnet_platform_send_t sends[80];

    for ( int i = 0; i < 80; ++i )
    {
        for ( int j = 0; j < 32; ++j )
        {
            data[i][j] = uint8_t( i + j );
        }
        sends[i] = { &address, data[i], 32 };
    }

    // more than two msgx chunks: one unsupported syscall pauses the pump;
    // the complete unsent tail falls back and later chunks stay portable

    refuse_send = true;

    double before = rocketnet_platform_time();
    rocketnet_platform_socket_send_packets( socket, sends, 80 );
    CHECK( send_calls == 1 && socket->msgx_down_until >= before + ROCKETNET_MSGX_RETRY_SECONDS );
    CHECK( strcmp( rocketnet_platform_socket_pump( socket ), "portable" ) == 0 );
    drain( socket, address, 80 );
    CHECK( receive_calls == 0 );
    rocketnet_platform_socket_send_packets( socket, sends, 1 );
    drain( socket, address, 1 );
    CHECK( send_calls == 1 && receive_calls == 0 );

    // Advance only the stored deadline, not wall clock or the production
    // retry interval. The next real batch call must perform the re-probe.

    refuse_send = false;
    socket->msgx_down_until = rocketnet_platform_time() - 0.001;
    rocketnet_platform_socket_send_packets( socket, sends, 1 );
    CHECK( send_calls == 2 && socket->msgx_down_until == 0.0 );
    drain( socket, address, 1 );
    CHECK( strcmp( rocketnet_platform_socket_pump( socket ), "msgx" ) == 0 );
    printf( "test_msgx_transport_send_pause_fallback_reprobe\n" );

    rocketnet_platform_socket_send_packets( socket, sends, 4 );

    int receive_before = receive_calls;
    refuse_receive = true;
    before = rocketnet_platform_time();
    drain( socket, address, 4 );
    CHECK( receive_calls == receive_before + 1 && socket->msgx_down_until >= before + ROCKETNET_MSGX_RETRY_SECONDS );
    CHECK( strcmp( rocketnet_platform_socket_pump( socket ), "portable" ) == 0 );
    refuse_receive = false;
    rocketnet_platform_socket_send_packets( socket, sends, 1 );
    socket->msgx_down_until = rocketnet_platform_time() - 0.001;
    drain( socket, address, 1 );
    CHECK( receive_calls > receive_before + 1 && socket->msgx_down_until == 0.0 );
    CHECK( strcmp( rocketnet_platform_socket_pump( socket ), "msgx" ) == 0 );
    printf( "test_msgx_transport_receive_pause_fallback_reprobe\n" );

    // A persistently interrupted batch must make one attempt and finish via
    // the per-packet path; this assertion does not wait for packet delivery.
    // The refused batch counts once in send_failures (its delivered prefix
    // is indeterminate); its replay, which sends everything, counts nothing.

    interrupt_send = true;

    const int calls_before_interrupt = send_calls;
    const uint64_t failures_before_interrupt = rocketnet_platform_socket_send_failures( socket );
    rocketnet_platform_socket_send_packets( socket, sends, 4 );
    CHECK( send_calls == calls_before_interrupt + 1 );
    CHECK( strcmp( rocketnet_platform_socket_pump( socket ), "msgx" ) == 0 );
    interrupt_send = false;
    drain( socket, address, 4 );
    CHECK( rocketnet_platform_socket_send_failures( socket ) == failures_before_interrupt + 1 );
    printf( "test_msgx_transport_send_eintr_bounded\n" );

    rocketnet_sendmsg_x = real_send;
    rocketnet_recvmsg_x = real_receive;
    rocketnet_platform_socket_destroy( socket );
    rocketnet_term();
}
