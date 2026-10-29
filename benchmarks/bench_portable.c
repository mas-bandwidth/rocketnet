/*
    rocketnet transport bench: portable sendto/recvfrom (#81)

    The negative control. One unconnected UDP socket per side, one
    syscall per packet by construction, everywhere-buildable. Every
    other candidate must beat this by enough to earn its complexity,
    and this number calibrates the machine between sittings.

    bench_portable recv <port> <warmup> <window> <windows>
    bench_portable send <ip> <port> <seconds> [drop-half]

    "drop-half" is the harness-honesty switch: the sender skips every
    other batch, and the receiver's pps must fall by half or the bench
    has measured nothing.

    BENCH_FAULT=recv-errno / send-refused force the two abort paths, so
    the loudness is watched rather than assumed (benchmarks/selftest.sh).
*/

#include "bench_common.h"

static void run_receiver( int port, double warmup, double window, int windows, int poll_usec )
{
    int fd = bench_udp_socket();
    struct sockaddr_in address = bench_address( "0.0.0.0", port );
    if ( bind( fd, (struct sockaddr *) &address, sizeof( address ) ) < 0 )
    {
        perror( "bind" );
        exit( 1 );
    }

    bench_report_buffers( fd );

    struct bench_windows_t w;
    bench_windows_init( &w, warmup, window, windows );

    uint8_t packet[BENCH_MAX_PACKET];

    for ( ;; )
    {
        /*
            One batch of up to BENCH_BATCH_SIZE nonblocking recvfroms.
            Empty recvs (EAGAIN) are counted but contribute no packets:
            the p99 covers packet-carrying syscalls, saturated by design.
        */
        uint64_t t0 = bench_time_ns();
        int batch_packets = 0;
        uint64_t batch_bytes = 0;
        uint64_t batch_syscalls = 0;
        uint64_t batch_empty = 0;
        uint64_t batch_fold = 0;
        int batch_timed = 1;

        for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
        {
            struct sockaddr_in from;
            socklen_t from_length = sizeof( from );
            ssize_t bytes = recvfrom( fd, packet, sizeof( packet ), MSG_DONTWAIT, (struct sockaddr *) &from, &from_length );

            batch_syscalls++;
            if ( bytes < 0 )
            {
                /* only an empty queue ends a drain; any other errno means
                   this receiver is not receiving and must not go on to
                   print a rate (#98 item 5) */
                bench_receive_refused_or_empty( "recvfrom", errno );
                batch_empty++;
                /* this batch's wall time now contains that empty syscall,
                   so it is not a clean per-packet measurement (#98 item 8) */
                batch_timed = 0;
                break;
            }

            batch_fold += bench_fold_packet( packet, (int) bytes );
            batch_packets++;
            batch_bytes += (uint64_t) bytes;
        }

        uint64_t batch_ns = bench_time_ns() - t0;

        if ( bench_window_tick( &w, batch_ns, batch_packets, batch_bytes, batch_syscalls, batch_empty, batch_fold, batch_timed ) )
            break;

        if ( poll_usec > 0 && batch_packets == 0 )
        {
            usleep( (useconds_t) poll_usec );
        }
    }

    close( fd );
}

static void run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
{
    int fd = bench_udp_socket();
    struct sockaddr_in to = bench_address( ip, port );

    uint8_t packet[BENCH_MAX_PACKET];
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
            for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
            {
                bench_fill_packet( packet, size, flow, sequence++ );
                flow = ( flow + 1 ) % BENCH_NUM_FLOWS;

                ssize_t result = sendto( fd, packet, (size_t) size, 0, (struct sockaddr *) &to, sizeof( to ) );

                if ( bench_fault( "send-short" ) )
                {
                    result = size - 1;
                }

                if ( bench_fault( "send-zero" ) )
                {
                    result = 0;
                }

                if ( result != size )
                {
                    /* ENOBUFS et al is back pressure: count the syscall, drop
                       the packet, keep going. A long unbroken run of any other
                       refusal is a host that cannot do this, and aborts. */
                    syscalls++;
                    sent += bench_send_refused( &health, "sendto", result < 0 ? errno : EIO );
                    continue;
                }

                syscalls++;
                sent += bench_send_ok( &health, "sendto" );
            }
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

    if ( argc >= 6 && strcmp( argv[1], "recv" ) == 0 )
    {
        run_receiver( atoi( argv[2] ), atof( argv[3] ), atof( argv[4] ), atoi( argv[5] ), bench_parse_poll_usec( argc, argv, 6 ) );
    }
    else if ( argc >= 5 && strcmp( argv[1], "send" ) == 0 )
    {
        run_sender( argv[2], atoi( argv[3] ), atof( argv[4] ), bench_parse_sender_options( argc, argv, 5 ) );
    }
    else
    {
        fprintf( stderr, "usage: bench_portable recv <port> <warmup> <window> <windows> [poll=<usec>]\n"
                         "       bench_portable send <ip> <port> <seconds> [drop-half] [pace=<pps>]\n" );
        return 1;
    }

    return 0;
}
