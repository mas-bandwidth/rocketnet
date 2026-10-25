/*
    rocketnet transport bench: UDP GSO / GRO (#81)

    First fast path, least gated. The sender hands the kernel one
    contiguous buffer of BENCH_BATCH_SIZE equal-size segments with a
    UDP_SEGMENT cmsg -- one syscall per batch. The receiver enables
    UDP_GRO and drains with recvmmsg; coalesced buffers arrive with a
    UDP_GRO cmsg carrying the segment size, and the harness splits and
    touches every segment so the per-packet cost is honest.

    Note carried from the plan: GSO wants contiguous same-size segments
    in one buffer. This harness builds them in place (one fill pass, no
    gather copy); whether the transport seam's send descriptors can do
    the same without an extra copy is a seam-tax suspect for the
    behind-the-seam column, counted there, not here.

    On a host without UDP_SEGMENT the sender's sendmsg refuses every
    batch with EINVAL. Before #98 it spun on that silently and the run
    reported pps=0, which reads exactly like a very slow transport; it now
    aborts with the errno named, because a GSO-less host has nothing for
    this harness to measure. The receiver's UDP_GRO setsockopt has always
    aborted, and its recvmmsg now aborts on any errno that is not an
    empty queue.

    bench_gso recv <port> <warmup> <window> <windows>
    bench_gso send <ip> <port> <seconds> [drop-half]

    BENCH_FAULT=recv-errno / send-refused force those aborts on a
    healthy host so the loudness can be watched (bench/selftest.sh).
*/

#include "bench_common.h"

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif

#define GSO_RECV_VLEN     8
#define GSO_RECV_BUFFER   65536

static void run_receiver( int port, double warmup, double window, int windows )
{
    int fd = bench_udp_socket();
    int one = 1;
    if ( setsockopt( fd, IPPROTO_UDP, UDP_GRO, &one, sizeof( one ) ) < 0 )
    {
        perror( "setsockopt UDP_GRO" );
        exit( 1 );
    }
    struct sockaddr_in address = bench_address( "0.0.0.0", port );
    if ( bind( fd, (struct sockaddr *) &address, sizeof( address ) ) < 0 )
    {
        perror( "bind" );
        exit( 1 );
    }
    bench_report_buffers( fd );

    static uint8_t buffers[GSO_RECV_VLEN][GSO_RECV_BUFFER];
    struct mmsghdr messages[GSO_RECV_VLEN];
    struct iovec iovecs[GSO_RECV_VLEN];
    static char controls[GSO_RECV_VLEN][CMSG_SPACE( sizeof( int ) )];

    struct bench_windows_t w;
    bench_windows_init( &w, warmup, window, windows );

    for ( ;; )
    {
        for ( int i = 0; i < GSO_RECV_VLEN; i++ )
        {
            iovecs[i].iov_base = buffers[i];
            iovecs[i].iov_len = GSO_RECV_BUFFER;
            memset( &messages[i].msg_hdr, 0, sizeof( struct msghdr ) );
            messages[i].msg_hdr.msg_iov = &iovecs[i];
            messages[i].msg_hdr.msg_iovlen = 1;
            messages[i].msg_hdr.msg_control = controls[i];
            messages[i].msg_hdr.msg_controllen = sizeof( controls[i] );
        }

        uint64_t t0 = bench_time_ns();
        int batch_packets = 0;
        uint64_t batch_bytes = 0;
        uint64_t batch_empty = 0;
        uint64_t batch_fold = 0;

        int count = recvmmsg( fd, messages, GSO_RECV_VLEN, MSG_DONTWAIT, NULL );
        if ( count < 0 )
        {
            bench_receive_refused_or_empty( "recvmmsg", errno );
            batch_empty = 1;
            count = 0;
        }

        for ( int i = 0; i < count; i++ )
        {
            int bytes = (int) messages[i].msg_len;
            int segment_size = bytes;
            for ( struct cmsghdr * cmsg = CMSG_FIRSTHDR( &messages[i].msg_hdr ); cmsg; cmsg = CMSG_NXTHDR( &messages[i].msg_hdr, cmsg ) )
            {
                if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_GRO )
                {
                    memcpy( &segment_size, CMSG_DATA( cmsg ), sizeof( int ) );
                    break;
                }
            }
            if ( segment_size <= 0 )
                segment_size = bytes;

            /* split the coalesced buffer and touch every segment */
            for ( int offset = 0; offset < bytes; offset += segment_size )
            {
                int this_segment = bytes - offset < segment_size ? bytes - offset : segment_size;
                batch_fold += bench_fold_packet( buffers[i] + offset, this_segment );
                batch_packets++;
                batch_bytes += (uint64_t) this_segment;
            }
        }

        uint64_t batch_ns = bench_time_ns() - t0;

        /* one recvmmsg is the whole batch: an empty return carries no
           packets, so no empty syscall is ever inside a timed region that
           produced any (#98 item 8) */
        if ( bench_window_tick( &w, batch_ns, batch_packets, batch_bytes, 1 /* one recvmmsg */, batch_empty, batch_fold, 1 ) )
            break;
    }

    close( fd );
}

static void run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
{
    int fd = bench_udp_socket();
    struct sockaddr_in to = bench_address( ip, port );

    static uint8_t buffer[BENCH_BATCH_SIZE * BENCH_MAX_PACKET];
    char control[CMSG_SPACE( sizeof( uint16_t ) )];

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
            /* one contiguous run of equal-size segments, filled in place */
            for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
            {
                bench_fill_packet( buffer + i * size, size, flow, sequence++ );
                flow = ( flow + 1 ) % BENCH_NUM_FLOWS;
            }

            struct iovec iov = { buffer, (size_t) ( BENCH_BATCH_SIZE * size ) };
            struct msghdr msg;
            memset( &msg, 0, sizeof( msg ) );
            msg.msg_name = &to;
            msg.msg_namelen = sizeof( to );
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof( control );
            struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
            cmsg->cmsg_level = IPPROTO_UDP;
            cmsg->cmsg_type = UDP_SEGMENT;
            cmsg->cmsg_len = CMSG_LEN( sizeof( uint16_t ) );
            uint16_t segment_size = (uint16_t) size;
            memcpy( CMSG_DATA( cmsg ), &segment_size, sizeof( segment_size ) );

            ssize_t result = sendmsg( fd, &msg, 0 );
            const int error = errno;
            syscalls++;
            if ( bench_fault( "send-short" ) )
                result = size;
            if ( bench_fault( "send-zero" ) )
                result = 0;
            // sendmsg returns bytes. Only whole accepted segments count;
            // any partial segment and the remaining messages are refusals.
            sent += bench_send_batch_observe( &health, "sendmsg (UDP_SEGMENT)",
                                              result >= 0 ? (int) ( result / size ) : 0, BENCH_BATCH_SIZE,
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
    if ( argc >= 6 && strcmp( argv[1], "recv" ) == 0 )
        run_receiver( atoi( argv[2] ), atof( argv[3] ), atof( argv[4] ), atoi( argv[5] ) );
    else if ( argc >= 5 && strcmp( argv[1], "send" ) == 0 )
        run_sender( argv[2], atoi( argv[3] ), atof( argv[4] ), bench_parse_sender_options( argc, argv, 5 ) );
    else
    {
        fprintf( stderr, "usage: bench_gso recv <port> <warmup> <window> <windows>\n"
                         "       bench_gso send <ip> <port> <seconds> [drop-half] [pace=<pps>]\n" );
        return 1;
    }
    return 0;
}
