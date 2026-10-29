/*
    rocketnet transport bench: io_uring (#81)

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

    THREE QUALIFICATIONS THIS HARNESS'S NUMBERS CARRY (#98 items 2-4),
    written here so a table quoting them cannot lose them:

    1. The sender uses a CONNECTED socket. That is structural, not a
       choice: io_uring_prep_send takes no destination address, so the
       address has to be bound into the socket. The portable control and
       the GSO harness both use unconnected sendto/sendmsg with a
       per-call address, and a connected UDP socket skips the per-send
       route lookup the others pay. So io_uring's ratio against those
       harnesses is an UPPER BOUND for io_uring, not a like-for-like
       comparison, and the gap is in io_uring's favour.

    2. SEND_ZC over loopback does not measure what SEND_ZC costs on a
       NIC. The zero-copy notification completion is posted when the
       kernel is done with the buffer, and on lo that is coupled to the
       RECEIVER draining it rather than to a transmit completing. The
       "notification overhead is unchanged on a real NIC" reading of the
       #81 SEND_ZC row overreaches for exactly that reason; the copy
       saved changes on a NIC and so does when the notification lands.

    3. The zero-copy syscall self-count is qualitative. liburing peeks
       the completion queue before entering the kernel, so an
       io_uring_wait_cqe often returns without a syscall at all -- this
       harness counts every one of them as an enter, which OVERSTATES
       the ZC row's syscall column. Treat that column as an ordering,
       and take the real counts from strace -c (benchmarks/PROTOCOL.md).

    BENCH_FAULT=sqe-exhausted / recv-errno / send-refused force the
    abort paths so their loudness can be watched (benchmarks/selftest.sh).
*/

#include "bench_common.h"

#include <liburing.h>

#define URING_ENTRIES        1024
#define URING_BUF_GROUP      1
#define URING_NUM_BUFFERS    4096                   /* power of two, per the ABI */
#define URING_BUFFER_SIZE    2048
#define URING_CQE_BATCH      64

/* the submission queue is finite: io_uring_get_sqe returns NULL when it is
   full, and dereferencing that NULL is a segfault where a message belongs
   (#98 item 5) */

static struct io_uring_sqe * bench_get_sqe_or_die( struct io_uring * ring, const char * what )
{
    struct io_uring_sqe * sqe = bench_fault( "sqe-exhausted" ) ? NULL : io_uring_get_sqe( ring );
    if ( !sqe )
    {
        char detail[192];
        snprintf( detail, sizeof( detail ),
                  "the submission queue had no free entry for %s -- the harness has more in flight than URING_ENTRIES (%d) allows, and every number after this point would be wrong",
                  what, URING_ENTRIES );
        bench_fatal( "io_uring_get_sqe", detail );
    }

    return sqe;
}

static void resubmit_multishot( struct io_uring * ring, int fd )
{
    struct io_uring_sqe * sqe = bench_get_sqe_or_die( ring, "the multishot receive" );
    io_uring_prep_recv_multishot( sqe, fd, NULL, 0, 0 );
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BUF_GROUP;
}

/* the buffer ring running dry: expected under load, and the re-arm handles it */
static const int uring_recv_tolerated[] = { ENOBUFS };

static void run_receiver( int port, double warmup, double window, int windows )
{
    int fd = bench_udp_socket();
    struct sockaddr_in address = bench_address( "0.0.0.0", port );
    if ( bind( fd, (struct sockaddr *) &address, sizeof( address ) ) < 0 )
    {
        bench_fatal( "bind", strerror( errno ) );
    }

    bench_report_buffers( fd );

    struct io_uring ring;
    int result = io_uring_queue_init( URING_ENTRIES, &ring, 0 );
    if ( result < 0 )
    {
        bench_fatal( "io_uring_queue_init", strerror( -result ) );
    }

    static uint8_t buffer_memory[URING_NUM_BUFFERS * URING_BUFFER_SIZE];
    int ring_error = 0;
    struct io_uring_buf_ring * buf_ring = io_uring_setup_buf_ring( &ring, URING_NUM_BUFFERS, URING_BUF_GROUP, 0, &ring_error );
    if ( !buf_ring )
    {
        /* a kernel that refuses the provided-buffer ring leaves the multishot
           receiver with nothing to receive into: this run measured nothing,
           and says so rather than exiting as if it had merely failed */
        char detail[256];
        snprintf( detail, sizeof( detail ), "%s (errno %d) -- this host cannot run the io_uring multishot receiver, so nothing was measured",
                  strerror( -ring_error ), -ring_error );
        bench_fatal( "io_uring_setup_buf_ring", detail );
    }

    for ( int i = 0; i < URING_NUM_BUFFERS; i++ )
        io_uring_buf_ring_add( buf_ring, buffer_memory + i * URING_BUFFER_SIZE, URING_BUFFER_SIZE, (unsigned short) i,
                               io_uring_buf_ring_mask( URING_NUM_BUFFERS ), i );
    io_uring_buf_ring_advance( buf_ring, URING_NUM_BUFFERS );

    resubmit_multishot( &ring, fd );

    int submit_result = io_uring_submit( &ring );
    if ( submit_result < 0 )
    {
        bench_fatal( "io_uring_submit", strerror( -submit_result ) );
    }

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
            // enter the kernel so pending completions can post, then look again
            io_uring_get_events( &ring );
            batch_syscalls++;
            count = io_uring_peek_batch_cqe( &ring, cqes, URING_CQE_BATCH );
            if ( count == 0 )
            {
                /* an empty completion queue is the ordinary idle state, and it
                   is where the shared rule is reached on every drain so the
                   fault switch has a healthy run to break (#98 item 5) */
                bench_receive_refused_or_empty( "io_uring multishot recv", EAGAIN );
                batch_empty++;
            }
        }

        int need_resubmit = 0;

        for ( unsigned i = 0; i < count; i++ )
        {
            struct io_uring_cqe * cqe = cqes[i];
            if ( cqe->res < 0 )
            {
                /* the multishot receive itself failed. ENOBUFS is the buffer
                   ring running dry, which the re-arm below handles and which
                   is ordinary under load; anything else is a receiver that has
                   stopped receiving, and used to be stepped straight over. */
                bench_receive_refused_or_empty_except( "io_uring multishot recv", -cqe->res,
                                                       uring_recv_tolerated,
                                                       (int) ( sizeof( uring_recv_tolerated ) / sizeof( uring_recv_tolerated[0] ) ) );
            }
            else if ( cqe->res > 0 && ( cqe->flags & IORING_CQE_F_BUFFER ) )
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
        {
            io_uring_buf_ring_advance( buf_ring, batch_packets );
        }

        io_uring_cq_advance( &ring, count );

        if ( need_resubmit )
        {
            resubmit_multishot( &ring, fd );

            int submit_result = io_uring_submit( &ring );
            if ( submit_result < 0 )
            {
                bench_fatal( "io_uring_submit", strerror( -submit_result ) );
            }

            batch_syscalls++;
        }

        uint64_t batch_ns = bench_time_ns() - t0;

        /* the completion reap is the batch: an empty peek carries no packets,
           so no empty enter is ever inside a timed region that produced any
           (#98 item 8) */
        if ( bench_window_tick( &w, batch_ns, batch_packets, batch_bytes, batch_syscalls, batch_empty, batch_fold, 1 ) )
            break;
    }

    io_uring_queue_exit( &ring );
    close( fd );
}

static void run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
{
    int fd = bench_udp_socket();
    struct sockaddr_in to = bench_address( ip, port );

    /* CONNECTED, structurally: io_uring_prep_send carries no destination
       address, so it has to live in the socket. The control and GSO senders
       are unconnected and pay a route lookup per send that this one does
       not, which makes every io_uring ratio against them an upper bound
       rather than like for like (#98 item 2). */

    if ( connect( fd, (struct sockaddr *) &to, sizeof( to ) ) < 0 )
    {
        bench_fatal( "connect", strerror( errno ) );
    }

    struct io_uring ring;
    int result = io_uring_queue_init( URING_ENTRIES, &ring, 0 );
    if ( result < 0 )
    {
        bench_fatal( "io_uring_queue_init", strerror( -result ) );
    }

    static uint8_t buffers[BENCH_BATCH_SIZE][BENCH_MAX_PACKET];

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
                bench_fill_packet( buffers[i], size, flow, sequence++ );
                flow = ( flow + 1 ) % BENCH_NUM_FLOWS;

                struct io_uring_sqe * sqe = bench_get_sqe_or_die( &ring, "a batched send" );

                if ( options.zero_copy )
                {
                    io_uring_prep_send_zc( sqe, fd, buffers[i], (size_t) size, 0, 0 );
                }
                else
                {
                    io_uring_prep_send( sqe, fd, buffers[i], (size_t) size, 0 );
                }
            }

            // one enter submits the whole batch and waits for its completions
            result = io_uring_submit_and_wait( &ring, BENCH_BATCH_SIZE );
            syscalls++;

            if ( result < 0 )
            {
                /* a refused submission used to be stepped over in silence,
                   so a ring that never accepted anything reported pps=0 */
                bench_send_batch_observe( &health, "io_uring_submit_and_wait", 0, BENCH_BATCH_SIZE, -result );
                continue;
            }

            /* NOT bench_send_ok here. A submission the ring ACCEPTED is not a
               packet the kernel SENT, and calling it here reset the refusal
               counter once per batch while the reap below could only raise it
               32 times against a limit of 64 -- so on a host where every send
               completion failed, the counter peaked at 32 and the abort could
               never fire. The run spun forever and printed pps=0, which is
               the condition #98 item 5 filed, in the code written to fix it.
               Only a completion that actually sent something clears the
               count, which is what the other harnesses have always meant by
               a send succeeding. */

            // reap: for send_zc each send posts a completion and later a notification
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
                    const int completed_bytes = bench_fault( "send-short" ) ? size - 1 : bench_fault( "send-zero" ) ? 0
                                                                                                                    : cqe->res;

                    if ( completed_bytes == size )
                    {
                        sent += bench_send_ok( &health, "io_uring send completion" );
                    }
                    else
                    {
                        sent += bench_send_refused( &health, "io_uring send completion", completed_bytes < 0 ? -completed_bytes : EIO );
                    }
                }
            }
            io_uring_cq_advance( &ring, reaped );

            /* zero-copy: drain outstanding notifications so the ring never
               fills. syscalls++ here OVERSTATES: liburing peeks the queue
               first, so many of these waits return without entering the
               kernel at all (#98 item 4). */
            while ( options.zero_copy && completed < BENCH_BATCH_SIZE )
            {
                if ( io_uring_wait_cqe( &ring, &cqe ) < 0 )
                    break;
                syscalls++;

                if ( !( cqe->flags & IORING_CQE_F_NOTIF ) )
                {
                    completed++;
                    const int completed_bytes = bench_fault( "send-short" ) ? size - 1 : bench_fault( "send-zero" ) ? 0
                                                                                                                    : cqe->res;

                    if ( completed_bytes == size )
                    {
                        sent += bench_send_ok( &health, "io_uring send completion (zc)" );
                    }
                    else
                    {
                        /* the zero-copy drain discarded this outright: a
                           SEND_ZC row could report pps=0 with every send
                           refused and never say a word */
                        sent += bench_send_refused( &health, "io_uring send completion (zc)", completed_bytes < 0 ? -completed_bytes : EIO );
                    }
                }

                io_uring_cqe_seen( &ring, cqe );
            }
        }
    }

    double elapsed = (double) ( bench_time_ns() - start ) / 1e9;

    bench_send_validate( &health );
    printf( "SENDER pkts=%llu secs=%.3f pps=%.0f syscalls=%llu back_pressure=%llu refused=%llu\n",
            (unsigned long long) sent, elapsed, (double) sent / elapsed,
            (unsigned long long) syscalls, (unsigned long long) health.back_pressure, (unsigned long long) health.refused );

    io_uring_queue_exit( &ring );
    close( fd );
}

int main( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    bench_fault_init();

    if ( argc >= 6 && strcmp( argv[1], "recv" ) == 0 )
    {
        run_receiver( atoi( argv[2] ), atof( argv[3] ), atof( argv[4] ), atoi( argv[5] ) );
    }
    else if ( argc >= 5 && strcmp( argv[1], "send" ) == 0 )
    {
        run_sender( argv[2], atoi( argv[3] ), atof( argv[4] ), bench_parse_sender_options( argc, argv, 5 ) );
    }
    else
    {
        fprintf( stderr, "usage: bench_uring recv <port> <warmup> <window> <windows>\n"
                         "       bench_uring send <ip> <port> <seconds> [drop-half] [zc] [pace=<pps>]\n" );
        return 1;
    }

    return 0;
}
