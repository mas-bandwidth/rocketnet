/*
    rocketnet transport bench: shared measurement scaffolding (#81)

    The harnesses are minimal datagram-level sender/receiver pairs at game
    shape: 1000 logical flows, seam-sized batches of 32, a deterministic
    cycle of game-like packet sizes. They measure the transport, not the
    protocol -- no rocketnet machinery is linked.

    Discipline (per the #81 plan): 30 s warm-up discarded, five 60 s
    measured windows, median with min/max spread reported by the runner.
    syscalls are counted by the harness itself (it knows every call it
    makes) and verified externally with strace -c on a separate run.

    WHAT THE ns COLUMNS ARE, AND ARE NOT (#98 item 1). p50_ns and p99_ns
    are BATCH AVERAGES, not per-packet tails: a batch's wall time is
    divided by the packets in it and that one quotient is credited to
    every packet in the batch. A batch that contains one slow packet and
    thirty-one fast ones reports thirty-two medium packets, so the tail
    is smeared and p99 UNDERSTATES a real per-packet tail. Any citation
    of these columns has to say that sentence. pps is unaffected --
    it is a count over a wall-clock window and is immune to where the
    timer sits.

    Only batches whose wall time is a clean measurement feed the
    histogram (#98 item 8): a drain that ends on an empty syscall has
    that empty syscall inside its timed region, which inflates the
    quotient for every packet the batch did carry. Those batches still
    count toward pps and bytes; they do not count toward ns.

    That exclusion can leave the ns columns resting on very little, and
    a small remnant is not a neutral sample -- the batches that fill
    completely are the congested ones, so what survives is a
    congestion-biased subsample. The WINDOW line therefore reports both
    the number of timed batches and the SHARE OF THE WINDOW'S PACKETS
    they cover, and any window whose ns columns cover less than
    BENCH_TIMED_SHARE_FLOOR of its packets prints a line saying so with
    the fraction spelled out. Zero is not the only dishonest case: one
    batch in a million is worse, because it still prints a plausible
    number.

    FAIL LOUD (#98 item 5). Every refusal a harness can meet is either a
    known back-pressure condition, counted and continued, or an abort
    with the call and errno named. A bench that reports pps=0 because
    the kernel refused every send has measured nothing and must not look
    like a measurement. BENCH_FAULT=<name> forces each of those paths so
    the loudness can be watched rather than believed; bench/selftest.sh
    drives them.
*/

#pragma once

#ifndef ROCKETNET_BENCH_BENCH_COMMON_H
#define ROCKETNET_BENCH_BENCH_COMMON_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BENCH_NUM_FLOWS       1000
#define BENCH_BATCH_SIZE      32                    /* the seam batch (#2 ruling) */

/* game-like datagram sizes, cycled per batch (GSO needs equal sizes within a batch) */
static const int bench_sizes[] = { 32, 64, 96, 128, 192, 256, 384, 512, 768, 1200 };
#define BENCH_NUM_SIZES ( (int) ( sizeof(bench_sizes) / sizeof(bench_sizes[0]) ) )
#define BENCH_MAX_PACKET 1200

static inline uint64_t bench_time_ns( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

/*
    Failing loudly.

    bench_fatal prints to both streams and exits nonzero. It is used for
    every condition under which the numbers this harness would go on to
    print are not measurements of anything: a syscall refused for a
    reason that is not back pressure, a submission queue that could not
    be filled, a kernel feature the harness was built to measure turning
    out to be absent.

    The BENCH_FAULT switch is the negative control. Each name forces one
    of those paths on a run that would otherwise be healthy, so the
    abort can be observed. Names:

      recv-errno      the next receive reports a refusal that is not
                      EAGAIN, as a kernel with a broken socket would
      send-refused    every send is treated as refused, as a host
                      without the feature under test would behave
      send-partial    31 of every 32 sends become hard refusals,
                      so the consecutive-refusal limit never trips
      send-short      a batch call reports just one message/segment sent
      send-zero       a batch call reports zero messages/bytes sent
      send-backpressure 31 of every 32 sends report ENOBUFS; the accepted
                      count must exclude them, while validation permits it
      sqe-exhausted   the io_uring submission queue reports empty, the
                      NULL that bench_uring used to dereference

    A fault that produces no abort is a finding about the harness, which
    is the entire reason the switch exists.
*/

static const char * bench_fault_name;

static inline void bench_fault_init( void )
{
    bench_fault_name = getenv( "BENCH_FAULT" );
    if ( bench_fault_name )
    {
        printf( "FAULT ARMED: %s -- synthetic results, never a measurement\n", bench_fault_name );
        fflush( stdout );
    }
}

static inline int bench_fault( const char * name )
{
    return bench_fault_name && strcmp( bench_fault_name, name ) == 0;
}

static inline void bench_fatal( const char * call, const char * detail )
{
    printf( "\nBENCH ABORTED: %s: %s\n", call, detail );
    fflush( stdout );
    fprintf( stderr, "BENCH ABORTED: %s: %s\n", call, detail );
    fflush( stderr );
    exit( 2 );
}

/*
    A receive that returned < 0. EAGAIN/EWOULDBLOCK means the queue is
    empty, which is the ordinary state of a drain loop and is counted.
    Anything else -- ENOTCONN, EBADF, ENOBUFS on the receive side, a
    kernel that does not implement the call -- means the receiver is not
    receiving, and a receiver that is not receiving reports pps=0, which
    reads exactly like a slow transport. It aborts instead.
*/

/* called after EVERY receive that yielded no data, with EAGAIN standing for
   "the queue was simply empty" -- which is what lets the fault switch turn an
   ordinary empty drain into the refusal a broken socket would report */

static inline void bench_receive_refused_or_empty_except( const char * call, int error, const int * tolerated, int tolerated_count )
{
    if ( bench_fault( "recv-errno" ) )
        error = EBADF;
    if ( error == EAGAIN || error == EWOULDBLOCK || error == EINTR )
        return;
    for ( int i = 0; i < tolerated_count; i++ )
    {
        if ( error == tolerated[i] )
            return;
    }
    char detail[192];
    snprintf( detail, sizeof(detail), "refused with errno %d (%s) -- this is not an empty queue, so every number below it would be a fiction",
              error, strerror( error ) );
    bench_fatal( call, detail );
}

static inline void bench_receive_refused_or_empty( const char * call, int error )
{
    bench_receive_refused_or_empty_except( call, error, NULL, 0 );
}

/*
    A send that returned < 0. ENOBUFS and EAGAIN are the socket buffer
    saying slow down: counted, dropped, carry on, exactly as the portable
    control always has. Anything else is the kernel refusing the shape of
    the call -- EINVAL from a host with no UDP_SEGMENT is the case #98
    names -- and a run that meets a long unbroken string of those is not
    measuring a slow transport, it is measuring an absent one.
*/

#define BENCH_SEND_REFUSAL_LIMIT 64

struct bench_send_health_t
{
    uint64_t refused;
    uint64_t back_pressure;
    uint64_t consecutive_refusals;
    uint64_t fault_sends;
    int last_refusal_error;
};

// Fault injection observes both successful and refused calls, so the partial
// failure plant has the same 31-of-32 pattern regardless of the host outcome.
static inline int bench_send_observe( struct bench_send_health_t * health, const char * call, int error )
{
    if ( bench_fault( "send-refused" ) )
        error = EINVAL;
    else if ( bench_fault( "send-partial" ) )
        error = ++health->fault_sends % 32 != 0 ? EINVAL : 0;
    else if ( bench_fault( "send-backpressure" ) )
        error = ++health->fault_sends % 32 != 0 ? ENOBUFS : 0;
    if ( error == 0 )
    {
        health->consecutive_refusals = 0;
        return 1;
    }
    health->refused++;
    if ( error == ENOBUFS || error == EAGAIN || error == EWOULDBLOCK || error == EINTR )
    {
        health->back_pressure++;
        health->consecutive_refusals = 0;
        return 0;
    }
    health->last_refusal_error = error;
    health->consecutive_refusals++;
    if ( health->consecutive_refusals < BENCH_SEND_REFUSAL_LIMIT )
        return 0;
    char detail[224];
    snprintf( detail, sizeof(detail),
              "%llu sends in a row refused with errno %d (%s) -- this host cannot do what this harness measures, and a silent spin here reports pps=0 as if the transport were merely slow",
              (unsigned long long) health->consecutive_refusals, error, strerror( error ) );
    bench_fatal( call, detail );
    return 0;
}

static inline int bench_send_ok( struct bench_send_health_t * health, const char * call )
{
    return bench_send_observe( health, call, 0 );
}

static inline int bench_send_refused( struct bench_send_health_t * health, const char * call, int error )
{
    return bench_send_observe( health, call, error ? error : EIO );
}

// Count accepted messages and every unsent member of a batch. Normal full
// batches take one health update; per-message fault simulation is test-only.
static inline int bench_send_batch_observe( struct bench_send_health_t * health, const char * call,
                                            int accepted, int attempted, int error )
{
    if ( accepted < 0 || accepted > attempted )
        bench_fatal( call, "invalid accepted-message count" );
    int sent = 0;
    if ( !bench_fault_name && accepted > 0 )
    {
        health->consecutive_refusals = 0;
        sent = accepted;
    }
    else
        for ( int i = 0; i < accepted; ++i ) sent += bench_send_ok( health, call );
    for ( int i = accepted; i < attempted; ++i ) sent += bench_send_refused( health, call, error );
    return sent;
}

// Any non-back-pressure refusal invalidates the completed sample, even when
// intervening successes kept the early consecutive-refusal abort from firing.
static inline void bench_send_validate( const struct bench_send_health_t * health )
{
    if ( health->refused == health->back_pressure )
        return;
    char detail[224];
    snprintf( detail, sizeof(detail),
              "refused=%llu back_pressure=%llu last_errno=%d (%s) -- non-back-pressure send failures invalidate this sample",
              (unsigned long long) health->refused, (unsigned long long) health->back_pressure,
              health->last_refusal_error, strerror( health->last_refusal_error ) );
    bench_fatal( "send health", detail );
}

/*
    Histogram of per-packet processing times: 16 ns buckets to ~1.05 ms,
    overflow counted. Allocate max needed, reset per window.
*/

/* the share of a window's packets the ns columns must cover before they are
   reported without a warning. A quarter is a judgement call, not a derived
   constant: below it the surviving batches are too few and too congestion-
   selected for a p99 to mean what a reader will take it to mean.

   What this floor does NOT do, since a threshold invites the reading that
   above it everything is fine: it is a check on QUANTITY while the bias is
   CATEGORICAL. The inclusion rule is "the batch filled" at every share, so a
   window at 26% coverage prints unwarned and is still congestion-selected.
   The floor catches the case where the columns rest on almost nothing; it
   does not make the rest unbiased, and no threshold could. */

#define BENCH_TIMED_SHARE_FLOOR 0.25

#define BENCH_HIST_BUCKETS 65536
#define BENCH_HIST_SHIFT   4                        /* 16 ns per bucket */

struct bench_hist_t
{
    uint64_t counts[BENCH_HIST_BUCKETS];
    uint64_t overflow;
    uint64_t total;
};

static inline void bench_hist_reset( struct bench_hist_t * hist )
{
    memset( hist, 0, sizeof(*hist) );
}

static inline void bench_hist_add( struct bench_hist_t * hist, uint64_t ns, uint64_t count )
{
    uint64_t bucket = ns >> BENCH_HIST_SHIFT;
    if ( bucket >= BENCH_HIST_BUCKETS )
        hist->overflow += count;
    else
        hist->counts[bucket] += count;
    hist->total += count;
}

static inline uint64_t bench_hist_quantile( const struct bench_hist_t * hist, double q )
{
    if ( hist->total == 0 )
        return 0;
    uint64_t target = (uint64_t) ( q * (double) hist->total );
    uint64_t seen = 0;
    for ( int i = 0; i < BENCH_HIST_BUCKETS; i++ )
    {
        seen += hist->counts[i];
        if ( seen > target )
            return ( (uint64_t) i << BENCH_HIST_SHIFT ) + ( 1 << ( BENCH_HIST_SHIFT - 1 ) );
    }
    return (uint64_t) BENCH_HIST_BUCKETS << BENCH_HIST_SHIFT;   /* overflowed: report the cap */
}

/*
    Deterministic payload: flow id, sequence, then a fill pattern the
    receiver folds (one full read pass over every payload byte, so the
    measurement includes touching the data like a real receive path).
*/

static inline void bench_fill_packet( uint8_t * packet, int bytes, uint32_t flow, uint64_t sequence )
{
    packet[0] = (uint8_t) ( flow       );
    packet[1] = (uint8_t) ( flow >> 8  );
    packet[2] = (uint8_t) ( flow >> 16 );
    packet[3] = (uint8_t) ( flow >> 24 );
    for ( int i = 0; i < 8; i++ )
        packet[4+i] = (uint8_t) ( sequence >> ( i * 8 ) );
    for ( int i = 12; i < bytes; i++ )
        packet[i] = (uint8_t) ( i + (int) flow );
}

static inline uint64_t bench_fold_packet( const uint8_t * packet, int bytes )
{
    uint64_t fold = 0;
    for ( int i = 0; i < bytes; i++ )
        fold += packet[i];
    return fold;
}

/*
    Window accounting for receivers. The receiver calls bench_window_tick
    once per batch with the batch's cost; the struct advances warm-up and
    windows off the monotonic clock and prints one line per window:

    WINDOW n secs=S pkts=N pps=R bytes=B p50_ns=X p99_ns=Y syscalls=C
           empty=E timed=T timed_pkts=P fold=F

    "syscalls" is every syscall the harness made in the window (data path
    only), "empty" the subset that returned no data (EAGAIN spins), and
    "timed" the number of batches whose wall time was clean enough to
    feed the ns columns (#98 item 8). p50_ns/p99_ns are batch averages
    over those batches, never per-packet tails -- see the file header.
*/

struct bench_windows_t
{
    double   warmup_seconds;
    double   window_seconds;
    int      num_windows;

    int      window_index;                          /* -1 during warm-up */
    uint64_t window_start_ns;
    uint64_t packets;
    uint64_t bytes;
    uint64_t syscalls;
    uint64_t empty;
    uint64_t timed;                                 /* batches whose ns were clean enough to histogram */
    uint64_t timed_packets;                         /* packets those batches carried */
    uint64_t fold;                                  /* payload fold, defeats dead-code elimination */
    struct bench_hist_t hist;
};

static inline void bench_windows_init( struct bench_windows_t * w, double warmup_seconds, double window_seconds, int num_windows )
{
    memset( w, 0, sizeof(*w) );
    w->warmup_seconds = warmup_seconds;
    w->window_seconds = window_seconds;
    w->num_windows = num_windows;
    w->window_index = -1;
    w->window_start_ns = bench_time_ns();
}

static inline void bench_window_emit( struct bench_windows_t * w, uint64_t now )
{
    double secs = (double) ( now - w->window_start_ns ) / 1e9;
    printf( "WINDOW %d secs=%.3f pkts=%llu pps=%.0f bytes=%llu p50_ns=%llu p99_ns=%llu syscalls=%llu empty=%llu timed=%llu timed_pkts=%llu fold=%llx\n",
            w->window_index, secs,
            (unsigned long long) w->packets,
            (double) w->packets / secs,
            (unsigned long long) w->bytes,
            (unsigned long long) bench_hist_quantile( &w->hist, 0.50 ),
            (unsigned long long) bench_hist_quantile( &w->hist, 0.99 ),
            (unsigned long long) w->syscalls,
            (unsigned long long) w->empty,
            (unsigned long long) w->timed,
            (unsigned long long) w->timed_packets,
            (unsigned long long) w->fold );

    /* ns columns that rest on too little of the window say so. The floor is a
       SHARE of the window's packets, not a nonzero check: one full batch in a
       million still prints a plausible-looking p50 and p99, and because the
       batches that fill completely are the congested ones, a small remnant is
       a congestion-biased subsample rather than a thin but fair one. */

    if ( w->packets > 0 )
    {
        const double share = (double) w->timed_packets / (double) w->packets;
        if ( w->timed == 0 )
            printf( "WINDOW %d NS COLUMNS VOID: %llu packets arrived and NO batch was cleanly timed -- every one ended on an empty syscall. p50_ns/p99_ns above are not measurements. pps and bytes stand.\n",
                    w->window_index, (unsigned long long) w->packets );
        else if ( share < BENCH_TIMED_SHARE_FLOOR )
            printf( "WINDOW %d NS COLUMNS UNREPRESENTATIVE: p50_ns/p99_ns rest on %llu of %llu packets (%.4f%%), in %llu fully drained batches. Fully drained batches are the congested ones, so this is a biased subsample, not a thin fair one. Do not cite the ns columns for this window. pps and bytes stand.\n",
                    w->window_index, (unsigned long long) w->timed_packets,
                    (unsigned long long) w->packets, share * 100.0,
                    (unsigned long long) w->timed );
    }

    fflush( stdout );
}

/* returns 0 while running, 1 when all windows are complete */

/*
    batch_timed says whether batch_ns is a clean measurement of the work
    that produced batch_packets. A drain that stopped because a syscall
    came back empty is NOT clean: that empty syscall sits inside the
    timed region and inflates the quotient for every packet the batch
    did carry (#98 item 8). Such a batch still counts toward pps and
    bytes -- it delivered real packets in real wall-clock time -- and is
    simply not offered to the histogram.
*/

static inline int bench_window_tick( struct bench_windows_t * w, uint64_t batch_ns, int batch_packets, uint64_t batch_bytes, uint64_t batch_syscalls, uint64_t batch_empty, uint64_t batch_fold, int batch_timed )
{
    if ( batch_packets > 0 )
    {
        w->packets += (uint64_t) batch_packets;
        w->bytes += batch_bytes;
        w->fold += batch_fold;
        if ( batch_timed )
        {
            bench_hist_add( &w->hist, batch_ns / (uint64_t) batch_packets, (uint64_t) batch_packets );
            w->timed++;
            w->timed_packets += (uint64_t) batch_packets;
        }
    }
    w->syscalls += batch_syscalls;
    w->empty += batch_empty;

    uint64_t now = bench_time_ns();
    double elapsed = (double) ( now - w->window_start_ns ) / 1e9;
    double limit = ( w->window_index < 0 ) ? w->warmup_seconds : w->window_seconds;
    if ( elapsed < limit )
        return 0;

    if ( w->window_index >= 0 )
        bench_window_emit( w, now );
    else
        { printf( "WARMUP done secs=%.3f pkts=%llu\n", elapsed, (unsigned long long) w->packets ); fflush( stdout ); }

    w->window_index++;
    if ( w->window_index >= w->num_windows )
        return 1;

    w->window_start_ns = now;
    w->packets = 0;
    w->bytes = 0;
    w->syscalls = 0;
    w->empty = 0;
    w->timed = 0;
    w->timed_packets = 0;
    w->fold = 0;
    bench_hist_reset( &w->hist );
    return 0;
}

/*
    Sender options. "drop-half" and "pace=<pps>" are the harness-honesty
    switches: a paced sender injects a KNOWN rate, and the receiver must
    read that rate back or the bench has measured nothing. (A flat-out
    sender that merely skips batches still saturates -- pps does not move,
    only the byte mix does; the pace plant is the one the rate instrument
    cannot fake.) "zc" selects zero-copy send where a harness supports it.
*/

struct bench_sender_options_t
{
    int      drop_half;
    int      zero_copy;
    uint64_t pace_pps;                              /* 0 = flat out */
};

/*
    Receiver poll option (#100): "poll=<usec>" makes the receiver sleep
    that long after an empty drain instead of spinning. The spin loop
    saturates a core by construction, which makes wall CPU useless as a
    per-packet cost instrument; the poll shape is the transport's real
    cadence (drain per tick), so with it /usr/bin/time -l reads back what
    a pump COSTS at a fixed delivered rate.
*/

static inline int bench_parse_poll_usec( int argc, char ** argv, int first )
{
    for ( int i = first; i < argc; i++ )
    {
        if ( strncmp( argv[i], "poll=", 5 ) == 0 )
            return atoi( argv[i] + 5 );
    }
    return 0;
}

static inline struct bench_sender_options_t bench_parse_sender_options( int argc, char ** argv, int first )
{
    struct bench_sender_options_t options;
    memset( &options, 0, sizeof(options) );
    for ( int i = first; i < argc; i++ )
    {
        if ( strcmp( argv[i], "drop-half" ) == 0 )
            options.drop_half = 1;
        else if ( strcmp( argv[i], "zc" ) == 0 )
            options.zero_copy = 1;
        else if ( strncmp( argv[i], "pace=", 5 ) == 0 )
            options.pace_pps = strtoull( argv[i] + 5, NULL, 10 );
    }
    return options;
}

/* spin until the paced sender is allowed its next batch (returns current time) */

static inline uint64_t bench_pace_wait( uint64_t start_ns, uint64_t sent, uint64_t pace_pps )
{
    uint64_t now = bench_time_ns();
    if ( pace_pps == 0 )
        return now;
    for ( ;; )
    {
        uint64_t allowed = (uint64_t) ( (double) ( now - start_ns ) / 1e9 * (double) pace_pps );
        if ( sent < allowed )
            return now;
        now = bench_time_ns();
    }
}

static inline int bench_udp_socket( void )
{
    int fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( fd < 0 )
        { perror( "socket" ); exit( 1 ); }
    int size = 4 * 1024 * 1024;                     /* request big; kernel caps at r/wmem_max -- actual is reported */
    setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size) );
    setsockopt( fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size) );
    return fd;
}

static inline void bench_report_buffers( int fd )
{
    int rcv = 0, snd = 0;
    socklen_t len = sizeof(rcv);
    getsockopt( fd, SOL_SOCKET, SO_RCVBUF, &rcv, &len );
    len = sizeof(snd);
    getsockopt( fd, SOL_SOCKET, SO_SNDBUF, &snd, &len );
    printf( "buffers: SO_RCVBUF=%d SO_SNDBUF=%d\n", rcv, snd );
    fflush( stdout );
}

static inline struct sockaddr_in bench_address( const char * ip, int port )
{
    struct sockaddr_in address;
    memset( &address, 0, sizeof(address) );
    address.sin_family = AF_INET;
    address.sin_port = htons( (uint16_t) port );
    if ( inet_pton( AF_INET, ip, &address.sin_addr ) != 1 )
        bench_fatal( "inet_pton", ip );
    return address;
}

#endif // ROCKETNET_BENCH_BENCH_COMMON_H
