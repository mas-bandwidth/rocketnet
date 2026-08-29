/*
    rocketnet driver bench: shared measurement scaffolding (#81)

    The harnesses are minimal datagram-level sender/receiver pairs at game
    shape: 1000 logical flows, seam-sized batches of 32, a deterministic
    cycle of game-like packet sizes. They measure the driver, not the
    protocol -- no rocketnet machinery is linked.

    Discipline (per the #81 plan): 30 s warm-up discarded, five 60 s
    measured windows, median with min/max spread reported by the runner.
    Per-packet processing time is the batch wall time divided by the
    packets in the batch, accumulated into a histogram per window;
    syscalls are counted by the harness itself (it knows every call it
    makes) and verified externally with strace -c on a separate run.
*/

#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
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
    Histogram of per-packet processing times: 16 ns buckets to ~1.05 ms,
    overflow counted. Allocate max needed, reset per window.
*/

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

    WINDOW n secs=S pkts=N pps=R bytes=B p50_ns=X p99_ns=Y syscalls=C empty=E

    "syscalls" is every syscall the harness made in the window (data path
    only), "empty" the subset that returned no data (EAGAIN spins).
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
    printf( "WINDOW %d secs=%.3f pkts=%llu pps=%.0f bytes=%llu p50_ns=%llu p99_ns=%llu syscalls=%llu empty=%llu fold=%llx\n",
            w->window_index, secs,
            (unsigned long long) w->packets,
            (double) w->packets / secs,
            (unsigned long long) w->bytes,
            (unsigned long long) bench_hist_quantile( &w->hist, 0.50 ),
            (unsigned long long) bench_hist_quantile( &w->hist, 0.99 ),
            (unsigned long long) w->syscalls,
            (unsigned long long) w->empty,
            (unsigned long long) w->fold );
    fflush( stdout );
}

/* returns 0 while running, 1 when all windows are complete */

static inline int bench_window_tick( struct bench_windows_t * w, uint64_t batch_ns, int batch_packets, uint64_t batch_bytes, uint64_t batch_syscalls, uint64_t batch_empty, uint64_t batch_fold )
{
    if ( batch_packets > 0 )
    {
        w->packets += (uint64_t) batch_packets;
        w->bytes += batch_bytes;
        w->fold += batch_fold;
        bench_hist_add( &w->hist, batch_ns / (uint64_t) batch_packets, (uint64_t) batch_packets );
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
    inet_pton( AF_INET, ip, &address.sin_addr );
    return address;
}

#endif /* BENCH_COMMON_H */
