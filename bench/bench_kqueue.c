/*
    rocketnet driver bench: kqueue readiness + nonblocking recvfrom (#100)

    Darwin candidate 2, the portable-plus baseline: kevent readiness
    gating a drain loop of per-packet recvfroms. The probe measured
    SO_REUSEPORT fan-out absent on Darwin (100% to the last-bound
    socket), so the multi-worker variant is refuted before benching --
    this is the single-socket shape, and what it can show over the
    portable control is at most saturation behavior, since every packet
    still costs one syscall. The sender is the portable sender.

    bench_kqueue recv <port> <warmup> <window> <windows>
    bench_kqueue send <ip> <port> <seconds> [drop-half] [pace=<pps>]
*/

#include "bench_common.h"

#include <fcntl.h>
#include <sys/event.h>

static void run_receiver( int port, double warmup, double window, int windows )
{
    int fd = bench_udp_socket();
    struct sockaddr_in address = bench_address( "0.0.0.0", port );
    if ( bind( fd, (struct sockaddr*) &address, sizeof(address) ) < 0 )
        { perror( "bind" ); exit( 1 ); }
    fcntl( fd, F_SETFL, O_NONBLOCK );
    bench_report_buffers( fd );

    int kq = kqueue();
    if ( kq < 0 )
        { perror( "kqueue" ); exit( 1 ); }
    struct kevent change;
    EV_SET( &change, fd, EVFILT_READ, EV_ADD, 0, 0, NULL );
    if ( kevent( kq, &change, 1, NULL, 0, NULL ) < 0 )
        { perror( "kevent EV_ADD" ); exit( 1 ); }

    struct bench_windows_t w;
    bench_windows_init( &w, warmup, window, windows );

    uint8_t packet[BENCH_MAX_PACKET];

    for ( ;; )
    {
        /* one kevent wait (1 ms cap so windows advance under silence),
           then a drain of up to BENCH_BATCH_SIZE recvfroms */

        uint64_t batch_syscalls = 0;
        uint64_t batch_empty = 0;

        struct kevent event;
        struct timespec timeout = { 0, 1000000 };
        int n = kevent( kq, NULL, 0, &event, 1, &timeout );
        batch_syscalls++;
        if ( n == 0 )
            batch_empty++;

        uint64_t t0 = bench_time_ns();
        int batch_packets = 0;
        uint64_t batch_bytes = 0;
        uint64_t batch_fold = 0;
        if ( n > 0 )
        {
            for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
            {
                struct sockaddr_in from;
                socklen_t from_length = sizeof(from);
                ssize_t bytes = recvfrom( fd, packet, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*) &from, &from_length );
                batch_syscalls++;
                if ( bytes < 0 )
                {
                    batch_empty++;
                    break;
                }
                batch_fold += bench_fold_packet( packet, (int) bytes );
                batch_packets++;
                batch_bytes += (uint64_t) bytes;
            }
        }
        uint64_t batch_ns = bench_time_ns() - t0;

        if ( bench_window_tick( &w, batch_ns, batch_packets, batch_bytes, batch_syscalls, batch_empty, batch_fold ) )
            break;
    }

    close( kq );
    close( fd );
}

static void run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
{
    int fd = bench_udp_socket();
    struct sockaddr_in to = bench_address( ip, port );

    uint8_t packet[BENCH_MAX_PACKET];
    uint64_t sent = 0;
    uint64_t syscalls = 0;
    uint64_t batch_index = 0;
    uint64_t sequence = 0;
    uint32_t flow = 0;

    uint64_t start = bench_time_ns();
    uint64_t deadline = start + (uint64_t) ( seconds * 1e9 );

    for ( ;; )
    {
        int size = bench_sizes[ batch_index % BENCH_NUM_SIZES ];
        int skip = options.drop_half && ( batch_index & 1 );
        batch_index++;

        if ( bench_pace_wait( start, sent, options.pace_pps ) >= deadline )
            break;

        if ( !skip )
        {
            for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
            {
                bench_fill_packet( packet, size, flow, sequence++ );
                flow = ( flow + 1 ) % BENCH_NUM_FLOWS;
                if ( sendto( fd, packet, (size_t) size, 0, (struct sockaddr*) &to, sizeof(to) ) < 0 )
                {
                    syscalls++;
                    continue;
                }
                syscalls++;
                sent++;
            }
        }
    }

    double elapsed = (double) ( bench_time_ns() - start ) / 1e9;
    printf( "SENDER pkts=%llu secs=%.3f pps=%.0f syscalls=%llu\n",
            (unsigned long long) sent, elapsed, (double) sent / elapsed, (unsigned long long) syscalls );
    close( fd );
}

int main( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    if ( argc >= 6 && strcmp( argv[1], "recv" ) == 0 )
        run_receiver( atoi( argv[2] ), atof( argv[3] ), atof( argv[4] ), atoi( argv[5] ) );
    else if ( argc >= 5 && strcmp( argv[1], "send" ) == 0 )
        run_sender( argv[2], atoi( argv[3] ), atof( argv[4] ), bench_parse_sender_options( argc, argv, 5 ) );
    else
    {
        fprintf( stderr, "usage: bench_kqueue recv <port> <warmup> <window> <windows>\n"
                         "       bench_kqueue send <ip> <port> <seconds> [drop-half] [pace=<pps>]\n" );
        return 1;
    }
    return 0;
}
