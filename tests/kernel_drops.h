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
// Linux: the per-socket "drops" column of /proc/self/net/udp and udp6, summed
// over the sockets this process holds (/proc/self/fd): only the case's own
// sockets count. A machine-wide counter does not do here: on a loaded Linux
// bench (vision, 2026-11-12) Udp InErrors rose by 309 and by 11171 during one
// case, which would admit a resend for a loss the library made.
// macOS: net.inet.udp.stats udps_fullsock ("dropped due to full socket
// buffers"), the only drop count macOS keeps, and it is machine-wide: another
// process's drops raise it too, so on a loaded Mac the gate can admit a resend
// the case's own datagrams did not need. The printed line names the delta
// every time. Measured on the studio, 2026-11-12: a 2000-datagram burst to a
// 127.0.0.1 socket lost 1149, 1947 and 1987 datagrams at three receive buffer
// sizes, and the counter rose by exactly 1149, 1947 and 1987; idle, it did not
// move between reads.
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

#include <dirent.h>
#include <unistd.h>

#define ROCKETNET_KERNEL_DROPS_MAX_SOCKETS 4096

// the inodes of this process's open sockets, from /proc/self/fd

static inline int rocketnet_kernel_drops_own_sockets( unsigned long * inodes, int capacity )
{
    DIR * dir = opendir( "/proc/self/fd" );
    if ( !dir )
    {
        return 0;
    }

    int count = 0;
    struct dirent * entry;
    while ( ( entry = readdir( dir ) ) != NULL && count < capacity )
    {
        char path[300];
        char target[64];
        snprintf( path, sizeof( path ), "/proc/self/fd/%s", entry->d_name );
        const ssize_t bytes = readlink( path, target, sizeof( target ) - 1 );
        if ( bytes <= 0 )
        {
            continue;
        }
        target[bytes] = '\0';
        unsigned long inode = 0;
        if ( sscanf( target, "socket:[%lu]", &inode ) == 1 )
        {
            inodes[count++] = inode;
        }
    }

    closedir( dir );
    return count;
}

// the "drops" column of /proc/self/net/udp or udp6, summed over the rows whose
// socket this process holds

static inline uint64_t rocketnet_kernel_drops_table( const char * path, const unsigned long * inodes, int num_inodes )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
    {
        return 0;
    }

    uint64_t result = 0;
    char line[512];
    while ( fgets( line, sizeof( line ), file ) )
    {
        unsigned long inode = 0;
        unsigned long long drops = 0;
        if ( sscanf( line, " %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %*s %*s %llu", &inode, &drops ) != 2 )
        {
            continue;
        }
        for ( int i = 0; i < num_inodes; i++ )
        {
            if ( inodes[i] == inode )
            {
                result += drops;
                break;
            }
        }
    }

    fclose( file );
    return result;
}

#endif // #if defined( __linux__ )

// the kernel's count of UDP datagrams it dropped on receive (per platform above)

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
    static unsigned long inodes[ROCKETNET_KERNEL_DROPS_MAX_SOCKETS];
    const int num_inodes = rocketnet_kernel_drops_own_sockets( inodes, ROCKETNET_KERNEL_DROPS_MAX_SOCKETS );
    return rocketnet_kernel_drops_table( "/proc/self/net/udp", inodes, num_inodes ) + rocketnet_kernel_drops_table( "/proc/self/net/udp6", inodes, num_inodes );
#else
    return 0;
#endif
}

// what the count covers, printed with every gate decision

#if defined( __APPLE__ )
#define ROCKETNET_LOSS_GATE_SCOPE \
    "WEAK: a machine-wide count (macOS keeps no per-socket drop count), so another process's drops can permit a resend the case did not need"
#elif defined( __linux__ )
#define ROCKETNET_LOSS_GATE_SCOPE "the drops of this process's own sockets"
#else
#define ROCKETNET_LOSS_GATE_SCOPE "no drop count is read on this platform"
#endif

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
                "the loss is not the kernel's [%s]\n",
                gate->name, missing, (unsigned long long) drops, (unsigned long long) gate->drops_used, ROCKETNET_LOSS_GATE_SCOPE );
        fflush( stdout );
        return false;
    }

    if ( gate->resends >= ROCKETNET_LOSS_GATE_RESENDS )
    {
        printf( "%s: %d datagrams still missing after %d resends (kernel receive drops %llu since the case began) [%s]\n", gate->name, missing,
                gate->resends, (unsigned long long) drops, ROCKETNET_LOSS_GATE_SCOPE );
        fflush( stdout );
        return false;
    }

    gate->resends++;
    gate->drops_used += (uint64_t) missing;
    printf( "%s: %d datagrams missing at the deadline; the kernel counted %llu receive drops since the case began: resend %d of %d [%s]\n",
            gate->name, missing, (unsigned long long) drops, gate->resends, ROCKETNET_LOSS_GATE_RESENDS, ROCKETNET_LOSS_GATE_SCOPE );
    fflush( stdout );
    return true;
}

#endif // #ifndef ROCKETNET_TESTS_KERNEL_DROPS_H
