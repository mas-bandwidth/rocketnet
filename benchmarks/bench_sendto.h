/*
    rocketnet transport bench: the per-packet sendto sender (#81, #100)

    One unconnected UDP socket, one sendto per packet, batches of
    BENCH_BATCH_SIZE at one size from the shared cycle. bench_portable
    and bench_kqueue both send with it, so the two share one sender
    rather than two copies kept in step by hand.
*/

#pragma once

#ifndef ROCKETNET_BENCHMARKS_BENCH_SENDTO_H
#define ROCKETNET_BENCHMARKS_BENCH_SENDTO_H

#include "bench_common.h"

static void bench_sendto_run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
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
        {
            break;
        }

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

#endif // ROCKETNET_BENCHMARKS_BENCH_SENDTO_H
