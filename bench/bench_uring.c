/*
    rocketnet driver bench: io_uring (#81)

    The headline non-root candidate. Batched submission on the send
    side (BENCH_BATCH_SIZE sends per io_uring_enter), multishot receive
    with a provided buffer ring on the receive side -- syscalls per
    packet amortized toward zero on both. SQPOLL is deliberately off:
    the probe reports its availability separately, and the plan starts
    without it.

    Builds against liburing (pinned separately; see Makefile LIBURING).

    bench_uring recv <port> <warmup> <window> <windows>
    bench_uring send <ip> <port> <seconds> [drop-half|zc]

    "zc" switches the sender to IORING_OP_SEND_ZC.
*/

#include "bench_common.h"

#include <liburing.h>

#define URING_ENTRIES        1024
#define URING_BUF_GROUP      1
#define URING_NUM_BUFFERS    4096                   /* power of two, per the ABI */
#define URING_BUFFER_SIZE    2048
#define URING_CQE_BATCH      64

static void resubmit_multishot( struct io_uring * ring, int fd )
{
    struct io_uring_sqe * sqe = io_uring_get_sqe( ring );
    io_uring_prep_recv_multishot( sqe, fd, NULL, 0, 0 );
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BUF_GROUP;
}

static void run_receiver( int port, double warmup, double window, int windows )
{
    int fd = bench_udp_socket();
    struct sockaddr_in address = bench_address( "0.0.0.0", port );
    if ( bind( fd, (struct sockaddr*) &address, sizeof(address) ) < 0 )
        { perror( "bind" ); exit( 1 ); }
    bench_report_buffers( fd );

    struct io_uring ring;
    int result = io_uring_queue_init( URING_ENTRIES, &ring, 0 );
    if ( result < 0 )
        { fprintf( stderr, "io_uring_queue_init: %s\n", strerror(-result) ); exit( 1 ); }

    static uint8_t buffer_memory[URING_NUM_BUFFERS * URING_BUFFER_SIZE];
    int ring_error = 0;
    struct io_uring_buf_ring * buf_ring = io_uring_setup_buf_ring( &ring, URING_NUM_BUFFERS, URING_BUF_GROUP, 0, &ring_error );
    if ( !buf_ring )
        { fprintf( stderr, "io_uring_setup_buf_ring: %s\n", strerror(-ring_error) ); exit( 1 ); }
    for ( int i = 0; i < URING_NUM_BUFFERS; i++ )
        io_uring_buf_ring_add( buf_ring, buffer_memory + i * URING_BUFFER_SIZE, URING_BUFFER_SIZE, (unsigned short) i,
                               io_uring_buf_ring_mask( URING_NUM_BUFFERS ), i );
    io_uring_buf_ring_advance( buf_ring, URING_NUM_BUFFERS );

    resubmit_multishot( &ring, fd );
    io_uring_submit( &ring );

    struct bench_windows_t w;
    bench_windows_init( &w, warmup, window, windows );

    struct io_uring_cqe * cqes[URING_CQE_BATCH];

    for ( ;; )
    {
        uint64_t t0 = bench_time_ns();
        int batch_packets = 0;
        uint64_t batch_bytes = 0;
        uint64_t batch_syscalls = 0;
        uint64_t batch_empty = 0;
        uint64_t batch_fold = 0;

        unsigned count = io_uring_peek_batch_cqe( &ring, cqes, URING_CQE_BATCH );
        if ( count == 0 )
        {
            /* enter the kernel so pending completions can post, then look again */
            io_uring_get_events( &ring );
            batch_syscalls++;
            count = io_uring_peek_batch_cqe( &ring, cqes, URING_CQE_BATCH );
            if ( count == 0 )
                batch_empty++;
        }

        int need_resubmit = 0;
        for ( unsigned i = 0; i < count; i++ )
        {
            struct io_uring_cqe * cqe = cqes[i];
            if ( cqe->res > 0 && ( cqe->flags & IORING_CQE_F_BUFFER ) )
            {
                int buffer_id = (int) ( cqe->flags >> IORING_CQE_BUFFER_SHIFT );
                uint8_t * packet = buffer_memory + buffer_id * URING_BUFFER_SIZE;
                batch_fold += bench_fold_packet( packet, cqe->res );
                batch_bytes += (uint64_t) cqe->res;
                io_uring_buf_ring_add( buf_ring, packet, URING_BUFFER_SIZE, (unsigned short) buffer_id,
                                       io_uring_buf_ring_mask( URING_NUM_BUFFERS ), batch_packets );
                batch_packets++;
            }
            if ( !( cqe->flags & IORING_CQE_F_MORE ) )
                need_resubmit = 1;                  /* multishot terminated (e.g. ENOBUFS): re-arm */
        }
        if ( batch_packets > 0 )
            io_uring_buf_ring_advance( buf_ring, batch_packets );
        io_uring_cq_advance( &ring, count );
        if ( need_resubmit )
        {
            resubmit_multishot( &ring, fd );
            io_uring_submit( &ring );
            batch_syscalls++;
        }

        uint64_t batch_ns = bench_time_ns() - t0;

        if ( bench_window_tick( &w, batch_ns, batch_packets, batch_bytes, batch_syscalls, batch_empty, batch_fold ) )
            break;
    }

    io_uring_queue_exit( &ring );
    close( fd );
}

static void run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
{
    int fd = bench_udp_socket();
    struct sockaddr_in to = bench_address( ip, port );
    if ( connect( fd, (struct sockaddr*) &to, sizeof(to) ) < 0 )
        { perror( "connect" ); exit( 1 ); }

    struct io_uring ring;
    int result = io_uring_queue_init( URING_ENTRIES, &ring, 0 );
    if ( result < 0 )
        { fprintf( stderr, "io_uring_queue_init: %s\n", strerror(-result) ); exit( 1 ); }

    static uint8_t buffers[BENCH_BATCH_SIZE][BENCH_MAX_PACKET];

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
                bench_fill_packet( buffers[i], size, flow, sequence++ );
                flow = ( flow + 1 ) % BENCH_NUM_FLOWS;
                struct io_uring_sqe * sqe = io_uring_get_sqe( &ring );
                if ( options.zero_copy )
                    io_uring_prep_send_zc( sqe, fd, buffers[i], (size_t) size, 0, 0 );
                else
                    io_uring_prep_send( sqe, fd, buffers[i], (size_t) size, 0 );
            }

            /* one enter submits the whole batch and waits for its completions */
            result = io_uring_submit_and_wait( &ring, BENCH_BATCH_SIZE );
            syscalls++;
            if ( result < 0 )
                continue;

            /* reap: for send_zc each send posts a completion and later a notification */
            struct io_uring_cqe * cqe;
            unsigned head;
            unsigned reaped = 0;
            int completed = 0;
            io_uring_for_each_cqe( &ring, head, cqe )
            {
                reaped++;
                if ( !( cqe->flags & IORING_CQE_F_NOTIF ) )
                {
                    completed++;
                    if ( cqe->res > 0 )
                        sent++;
                }
            }
            io_uring_cq_advance( &ring, reaped );

            /* zero-copy: drain outstanding notifications so the ring never fills */
            while ( options.zero_copy && completed < BENCH_BATCH_SIZE )
            {
                if ( io_uring_wait_cqe( &ring, &cqe ) < 0 )
                    break;
                syscalls++;
                if ( !( cqe->flags & IORING_CQE_F_NOTIF ) )
                {
                    completed++;
                    if ( cqe->res > 0 )
                        sent++;
                }
                io_uring_cqe_seen( &ring, cqe );
            }
        }

    }

    double elapsed = (double) ( bench_time_ns() - start ) / 1e9;
    printf( "SENDER pkts=%llu secs=%.3f pps=%.0f syscalls=%llu\n",
            (unsigned long long) sent, elapsed, (double) sent / elapsed, (unsigned long long) syscalls );

    io_uring_queue_exit( &ring );
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
        fprintf( stderr, "usage: bench_uring recv <port> <warmup> <window> <windows>\n"
                         "       bench_uring send <ip> <port> <seconds> [drop-half] [zc] [pace=<pps>]\n" );
        return 1;
    }
    return 0;
}
