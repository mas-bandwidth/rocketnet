/*
    rocketnet transport bench: sendmsg_x/recvmsg_x batching (#100)

    Darwin candidate 1: the private-but-stable batch UDP syscalls, one
    syscall per seam batch of 32 in each direction. The send side uses the
    server-pump shape the probe validated -- unconnected socket, a
    per-message msg_name on every message -- and the receive side asks for
    a full batch with msg_name fill and reads msg_datalen back per packet.

    bench_msgx recv <port> <warmup> <window> <windows>
    bench_msgx send <ip> <port> <seconds> [drop-half] [pace=<pps>]

    Darwin only. The harness refuses to run if the syscalls are absent --
    the transport degrades, a measurement instrument reports.
*/

#include "bench_common.h"

#include <dlfcn.h>
#include <fcntl.h>

/* struct msghdr_x from xnu bsd/sys/socket_private.h, defined locally: the
   layout is the syscall ABI */

struct msghdr_x
{
    void * msg_name;
    socklen_t msg_namelen;
    struct iovec * msg_iov;
    int msg_iovlen;
    void * msg_control;
    socklen_t msg_controllen;
    int msg_flags;
    size_t msg_datalen;
};

typedef ssize_t ( *sendmsg_x_fn )( int s, const struct msghdr_x * msgp, u_int cnt, int flags );
typedef ssize_t ( *recvmsg_x_fn )( int s, struct msghdr_x * msgp, u_int cnt, int flags );

static sendmsg_x_fn msgx_send;
static recvmsg_x_fn msgx_recv;

static void resolve_msgx_or_die( void )
{
    msgx_send = (sendmsg_x_fn) dlsym( RTLD_DEFAULT, "sendmsg_x" );
    msgx_recv = (recvmsg_x_fn) dlsym( RTLD_DEFAULT, "recvmsg_x" );
    if ( !msgx_send || !msgx_recv )
    {
        fprintf( stderr, "sendmsg_x/recvmsg_x not present on this host: nothing to measure\n" );
        exit( 1 );
    }
}

static void run_receiver( int port, double warmup, double window, int windows, int poll_usec )
{
    int fd = bench_udp_socket();
    struct sockaddr_in address = bench_address( "0.0.0.0", port );
    if ( bind( fd, (struct sockaddr *) &address, sizeof( address ) ) < 0 )
    {
        perror( "bind" );
        exit( 1 );
    }
    fcntl( fd, F_SETFL, O_NONBLOCK );
    bench_report_buffers( fd );

    struct bench_windows_t w;
    bench_windows_init( &w, warmup, window, windows );

    static uint8_t payload[BENCH_BATCH_SIZE][BENCH_MAX_PACKET];
    struct sockaddr_in from[BENCH_BATCH_SIZE];
    struct iovec iov[BENCH_BATCH_SIZE];
    struct msghdr_x msg[BENCH_BATCH_SIZE];

    for ( ;; )
    {
        /* one recvmsg_x per batch: the whole point of the candidate */

        memset( msg, 0, sizeof( msg ) );
        for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
        {
            iov[i].iov_base = payload[i];
            iov[i].iov_len = BENCH_MAX_PACKET;
            msg[i].msg_name = &from[i];
            msg[i].msg_namelen = sizeof( from[i] );
            msg[i].msg_iov = &iov[i];
            msg[i].msg_iovlen = 1;
        }

        uint64_t t0 = bench_time_ns();
        int batch_packets = 0;
        uint64_t batch_bytes = 0;
        uint64_t batch_empty = 0;
        uint64_t batch_fold = 0;

        ssize_t received = msgx_recv( fd, msg, BENCH_BATCH_SIZE, 0 );
        if ( received < 0 )
        {
            bench_receive_refused_or_empty( "recvmsg_x", errno );
            batch_empty = 1;
        }
        else
        {
            for ( ssize_t i = 0; i < received; i++ )
            {
                int bytes = (int) msg[i].msg_datalen;
                batch_fold += bench_fold_packet( payload[i], bytes );
                batch_bytes += (uint64_t) bytes;
            }
            batch_packets = (int) received;
        }
        uint64_t batch_ns = bench_time_ns() - t0;

        /* one recvmsg_x is the whole batch: the empty case carries no
           packets at all, so no empty syscall is ever inside a timed
           region that produced any (#98 item 8) */
        if ( bench_window_tick( &w, batch_ns, batch_packets, batch_bytes, 1, batch_empty, batch_fold, 1 ) )
            break;

        if ( poll_usec > 0 && batch_packets == 0 )
            usleep( (useconds_t) poll_usec );
    }

    close( fd );
}

static void run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
{
    int fd = bench_udp_socket();
    struct sockaddr_in to = bench_address( ip, port );

    static uint8_t payload[BENCH_BATCH_SIZE][BENCH_MAX_PACKET];
    struct iovec iov[BENCH_BATCH_SIZE];
    struct msghdr_x msg[BENCH_BATCH_SIZE];

    struct bench_send_health_t health;
    memset( &health, 0, sizeof( health ) );
    uint64_t sent = 0;
    uint64_t syscalls = 0;
    uint64_t batch_index = 0;
    uint64_t sequence = 0;
    uint32_t flow = 0;

    uint64_t start = bench_time_ns();
    uint64_t deadline = start + (uint64_t) ( seconds * 1e9 );

    for ( ;; )
    {
        int size = bench_sizes[batch_index % BENCH_NUM_SIZES];
        int skip = options.drop_half && ( batch_index & 1 );
        batch_index++;

        if ( bench_pace_wait( start, sent, options.pace_pps ) >= deadline )
            break;

        if ( !skip )
        {
            /* the server-pump send shape: per-message msg_name on an
               unconnected socket, one sendmsg_x per batch */

            memset( msg, 0, sizeof( msg ) );
            for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
            {
                bench_fill_packet( payload[i], size, flow, sequence++ );
                flow = ( flow + 1 ) % BENCH_NUM_FLOWS;
                iov[i].iov_base = payload[i];
                iov[i].iov_len = (size_t) size;
                msg[i].msg_name = &to;
                msg[i].msg_namelen = sizeof( to );
                msg[i].msg_iov = &iov[i];
                msg[i].msg_iovlen = 1;
            }

            ssize_t result = msgx_send( fd, msg, BENCH_BATCH_SIZE, 0 );
            const int error = errno;
            syscalls++;
            if ( bench_fault( "send-short" ) )
                result = 1;
            if ( bench_fault( "send-zero" ) )
                result = 0;
            sent += bench_send_batch_observe( &health, "sendmsg_x",
                                              result >= 0 ? (int) result : 0, BENCH_BATCH_SIZE,
                                              result < 0 ? error : EIO );
        }
    }

    double elapsed = (double) ( bench_time_ns() - start ) / 1e9;
    bench_send_validate( &health );
    printf( "SENDER pkts=%llu secs=%.3f pps=%.0f syscalls=%llu back_pressure=%llu refused=%llu\n",
            (unsigned long long) sent, elapsed, (double) sent / elapsed,
            (unsigned long long) syscalls, (unsigned long long) health.back_pressure, (unsigned long long) health.refused );
    close( fd );
}

int main( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    bench_fault_init();
    resolve_msgx_or_die();
    if ( argc >= 6 && strcmp( argv[1], "recv" ) == 0 )
        run_receiver( atoi( argv[2] ), atof( argv[3] ), atof( argv[4] ), atoi( argv[5] ), bench_parse_poll_usec( argc, argv, 6 ) );
    else if ( argc >= 5 && strcmp( argv[1], "send" ) == 0 )
        run_sender( argv[2], atoi( argv[3] ), atof( argv[4] ), bench_parse_sender_options( argc, argv, 5 ) );
    else
    {
        fprintf( stderr, "usage: bench_msgx recv <port> <warmup> <window> <windows> [poll=<usec>]\n"
                         "       bench_msgx send <ip> <port> <seconds> [drop-half] [pace=<pps>]\n" );
        return 1;
    }
    return 0;
}
