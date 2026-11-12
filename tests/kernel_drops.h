/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_TESTS_KERNEL_DROPS_H
#define ROCKETNET_TESTS_KERNEL_DROPS_H

// Loopback does not promise delivery. A burst that overruns a receiving
// socket's buffer is dropped by the kernel, on macOS more readily than on
// Linux, and more on a loaded machine. A functional case that counts every
// datagram the library sent, exactly, then fails with nothing wrong in the
// library.
//
// Waiting longer does not help (a dropped datagram never arrives), and a
// resend from the test must never hide a datagram the LIBRARY lost. So the
// resend is gated on the kernel's own word: a case short of `missing`
// datagrams after ROCKETNET_LOSS_GATE_WAIT seconds may send them again only
// when the kernel's receive-drop counter rose by at least `missing` since the
// case began (less what earlier resends of the case already used), and at
// most ROCKETNET_LOSS_GATE_RESENDS times. Every decision is printed. A loss
// the kernel did not count is the library's, and the case fails as it always
// did.
//
//   struct rocketnet_loss_gate_t gate;
//   rocketnet_loss_gate_begin( &gate, "case name" );
//   ... send, receive for up to ROCKETNET_LOSS_GATE_WAIT seconds ...
//   check( rocketnet_loss_gate_resend( &gate, missing ) );   // then send the missing ones again
//
// The counters are machine-wide: another process's drops raise them too, so
// on a loaded machine the gate can admit a resend the case's own datagrams did
// not need. The printed line names the counter delta every time.
//
// macOS: net.inet.udp.stats udps_fullsock ("dropped due to full socket
// buffers"). Measured on the studio, 2026-11-12: a 2000-datagram burst to a
// 127.0.0.1 socket lost 1149, 1947 and 1987 datagrams at three receive buffer
// sizes, and the counter rose by exactly 1149, 1947 and 1987.
// Linux: Udp InErrors (/proc/net/snmp) and Udp6InErrors (/proc/net/snmp6),
// which include RcvbufErrors, plus the per-CPU backlog drops of
// /proc/net/softnet_stat (the loopback device's input queue).
// Windows: no counter is read; the gate never admits a resend there.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined( __APPLE__ )
#include <sys/types.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <netinet/ip_var.h>
#include <netinet/udp_var.h>
#endif // #if defined( __APPLE__ )

#define ROCKETNET_LOSS_GATE_WAIT 2.0        // seconds a case waits for what it sent before it asks the gate
#define ROCKETNET_LOSS_GATE_RESENDS 3       // resends a case may make before it fails

#if defined( __linux__ )

static inline uint64_t rocketnet_kernel_drops_snmp( const char * path, const char * prefix, const char * name )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
    {
        return 0;
    }

    uint64_t result = 0;
    char header[2048] = { 0 };
    char line[2048];
    const size_t prefix_bytes = strlen( prefix );
    while ( fgets( line, sizeof( line ), file ) )
    {
        if ( prefix_bytes == 0 )
        {
            // /proc/net/snmp6: one "name value" pair per line
            char field[128];
            unsigned long long value;
            if ( sscanf( line, "%127s %llu", field, &value ) == 2 && strcmp( field, name ) == 0 )
            {
                result = value;
                break;
            }
            continue;
        }

        if ( strncmp( line, prefix, prefix_bytes ) != 0 )
        {
            continue;
        }

        if ( header[0] == 0 )
        {
            snprintf( header, sizeof( header ), "%s", line );
            continue;
        }

        // the second line of the prefix holds the values, in the header's order
        char * header_save = NULL;
        char * value_save = NULL;
        char * h = strtok_r( header + prefix_bytes, " \n", &header_save );
        char * v = strtok_r( line + prefix_bytes, " \n", &value_save );
        while ( h && v )
        {
            if ( strcmp( h, name ) == 0 )
            {
                result = strtoull( v, NULL, 10 );
            }
            h = strtok_r( NULL, " \n", &header_save );
            v = strtok_r( NULL, " \n", &value_save );
        }
        break;
    }

    fclose( file );
    return result;
}

static inline uint64_t rocketnet_kernel_drops_backlog()
{
    FILE * file = fopen( "/proc/net/softnet_stat", "r" );
    if ( !file )
    {
        return 0;
    }

    uint64_t result = 0;
    char line[1024];
    while ( fgets( line, sizeof( line ), file ) )
    {
        unsigned int processed = 0;
        unsigned int dropped = 0;
        if ( sscanf( line, "%x %x", &processed, &dropped ) == 2 )
        {
            result += dropped;
        }
    }

    fclose( file );
    return result;
}

#endif // #if defined( __linux__ )

// the kernel's machine-wide count of UDP datagrams it dropped on receive

static inline uint64_t rocketnet_kernel_udp_receive_drops()
{
#if defined( __APPLE__ )
    struct udpstat stats;
    size_t bytes = sizeof( stats );
    memset( &stats, 0, sizeof( stats ) );
    if ( sysctlbyname( "net.inet.udp.stats", &stats, &bytes, NULL, 0 ) != 0 )
    {
        return 0;
    }
    return (uint64_t) stats.udps_fullsock;
#elif defined( __linux__ )
    return rocketnet_kernel_drops_snmp( "/proc/net/snmp", "Udp: ", "InErrors" ) + rocketnet_kernel_drops_snmp( "/proc/net/snmp6", "", "Udp6InErrors" ) +
           rocketnet_kernel_drops_backlog();
#else
    return 0;
#endif
}

struct rocketnet_loss_gate_t
{
    const char * name;
    uint64_t drops_at_start;
    uint64_t drops_used;
    int resends;
};

static inline void rocketnet_loss_gate_begin( struct rocketnet_loss_gate_t * gate, const char * name )
{
    gate->name = name;
    gate->drops_at_start = rocketnet_kernel_udp_receive_drops();
    gate->drops_used = 0;
    gate->resends = 0;
}

// a case is short of `missing` datagrams after its wait: true when the kernel
// counted at least that many receive drops not yet used by an earlier resend,
// and the case has resends left. The caller sends the missing ones again.

static inline bool rocketnet_loss_gate_resend( struct rocketnet_loss_gate_t * gate, int missing )
{
    const uint64_t now = rocketnet_kernel_udp_receive_drops();
    const uint64_t drops = now >= gate->drops_at_start ? now - gate->drops_at_start : 0;
    const uint64_t unused = drops >= gate->drops_used ? drops - gate->drops_used : 0;

    if ( missing <= 0 )
    {
        printf( "%s: loss gate asked with %d missing\n", gate->name, missing );
        fflush( stdout );
        return false;
    }

    if ( unused < (uint64_t) missing )
    {
        printf( "%s: %d datagrams missing at the deadline, and the kernel counted %llu receive drops since the case began (%llu used by earlier resends): "
                "the loss is not the kernel's\n",
                gate->name, missing, (unsigned long long) drops, (unsigned long long) gate->drops_used );
        fflush( stdout );
        return false;
    }

    if ( gate->resends >= ROCKETNET_LOSS_GATE_RESENDS )
    {
        printf( "%s: %d datagrams still missing after %d resends (kernel receive drops %llu since the case began)\n", gate->name, missing,
                gate->resends, (unsigned long long) drops );
        fflush( stdout );
        return false;
    }

    gate->resends++;
    gate->drops_used += (uint64_t) missing;
    printf( "%s: %d datagrams missing at the deadline; the kernel counted %llu receive drops since the case began: resend %d of %d\n", gate->name,
            missing, (unsigned long long) drops, gate->resends, ROCKETNET_LOSS_GATE_RESENDS );
    fflush( stdout );
    return true;
}

#endif // #ifndef ROCKETNET_TESTS_KERNEL_DROPS_H
