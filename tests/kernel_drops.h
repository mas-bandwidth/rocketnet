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
// macOS: only the datagrams this process's own sockets sent and none of them
// received, from the kernel's per-socket packet counts (below: a loss under
// machine load that no drop counter of macOS sees). A full receive buffer is
// NOT excused on macOS, because xnu keeps no per-socket count of it:
// udp_input and udp_append add a datagram to the socket's receive count
// before sbappendaddr, so a full-buffer drop reads as received, and the one
// counter of it, net.inet.udp.stats udps_fullsock, is machine-wide. Another
// process's drops would raise it and admit a resend that hides a library
// loss, the reason the Linux path above refuses Udp InErrors. So a case that
// overruns a receiving socket's buffer on macOS fails; it sizes the burst or
// the buffer so it does not. (Measured on the studio, 2026-11-12: a
// 2000-datagram burst to a 127.0.0.1 socket lost 1149, 1947 and 1987
// datagrams at three receive buffer sizes, every one a full-buffer drop.)
// Windows: no counter is read; the gate never admits a resend there.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined( __APPLE__ )
#include <sys/types.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
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

#if defined( __APPLE__ )

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// macOS keeps per-socket packet counts: net.inet.udp.pcblist_n gives each of
// this process's UDP sockets (another process's are not listed to it) a
// record XSO_INPCB (xi_inpp, the socket's identity, at byte 8) followed by
// XSO_STATS (struct data_stats { rxpackets, rxbytes, txpackets, txbytes }
// for each of SO_TC_STATS_MAX = 4 traffic classes, from byte 8), each
// record rounded up to 8 bytes after a leading struct xinpgen. These are
// xnu's netstat layouts (bsd/netinet/in_pcb.h, bsd/sys/socketvar.h), not in
// the SDK's headers.
//
// txpackets counts a datagram at udp_output, rxpackets when udp_input hands
// it to the socket. A loopback datagram one of these sockets sent that none
// of them received was lost between the two, where no other counter of the
// kernel's sees it: measured on the studio, 2026-11-12, under six
// functional_production suites at once, a 300-datagram burst lost 272
// datagrams of 6000 with udps_fullsock and droptap's "DLIL burst limit
// exceeded" unmoved, and sent minus received by these counts was exactly
// 272. A datagram the library read and then lost is in both counts, so it
// never permits a resend.
//
// rxpackets advanced by 2 per datagram on the studio (macOS 27.2), so the
// receive count per datagram is measured once per process, on a private
// pair of sockets; a measurement that is not 1 or 2 turns the count off.

#define ROCKETNET_KERNEL_OWN_MAX_SOCKETS 128
#define ROCKETNET_KERNEL_XSO_STATS 0x008
#define ROCKETNET_KERNEL_XSO_INPCB 0x010

struct rocketnet_kernel_own_socket_t
{
    uint64_t id;
    uint64_t rx;
    uint64_t tx;
    uint16_t port; // local port, network order
};

// this process's UDP sockets and their packet counts; -1 when the table could not be read

static inline int rocketnet_kernel_own_sockets( struct rocketnet_kernel_own_socket_t * sockets, int capacity )
{
    size_t bytes = 0;
    if ( sysctlbyname( "net.inet.udp.pcblist_n", NULL, &bytes, NULL, 0 ) != 0 )
    {
        return -1;
    }
    bytes += 65536;
    char * table = (char *) malloc( bytes );
    if ( !table )
    {
        return -1;
    }
    if ( sysctlbyname( "net.inet.udp.pcblist_n", table, &bytes, NULL, 0 ) != 0 || bytes < 8 )
    {
        free( table );
        return -1;
    }

    int count = 0;
    uint64_t id = 0;
    uint16_t port = 0;
    uint32_t header = 0;
    memcpy( &header, table, sizeof( header ) );
    size_t offset = ( (size_t) header + 7 ) & ~(size_t) 7;
    while ( offset + 8 <= bytes )
    {
        uint32_t length = 0;
        uint32_t kind = 0;
        memcpy( &length, table + offset, sizeof( length ) );
        memcpy( &kind, table + offset + 4, sizeof( kind ) );
        if ( length < 8 || offset + length > bytes )
        {
            break;
        }
        if ( kind == ROCKETNET_KERNEL_XSO_INPCB && length >= 20 )
        {
            memcpy( &id, table + offset + 8, sizeof( id ) );
            memcpy( &port, table + offset + 18, sizeof( port ) ); // inp_lport
        }
        else if ( kind == ROCKETNET_KERNEL_XSO_STATS && length >= 8 + 4 * 32 && count < capacity )
        {
            uint64_t rx = 0;
            uint64_t tx = 0;
            for ( int traffic_class = 0; traffic_class < 4; traffic_class++ )
            {
                uint64_t stats[4];
                memcpy( stats, table + offset + 8 + traffic_class * 32, sizeof( stats ) );
                rx += stats[0];
                tx += stats[2];
            }
            sockets[count].id = id;
            sockets[count].rx = rx;
            sockets[count].tx = tx;
            sockets[count].port = port;
            count++;
        }
        offset += ( (size_t) length + 7 ) & ~(size_t) 7;
    }

    free( table );
    return count;
}

// the receive count one datagram adds, measured once on a private pair; 0 when unknown

static inline int rocketnet_kernel_rx_per_datagram()
{
    static int measured = -1;
    if ( measured >= 0 )
    {
        return measured;
    }
    measured = 0;

    const int sender = socket( AF_INET, SOCK_DGRAM, 0 );
    const int receiver = socket( AF_INET, SOCK_DGRAM, 0 );
    struct sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    socklen_t address_bytes = sizeof( address );
    if ( sender >= 0 && receiver >= 0 && bind( receiver, (struct sockaddr *) &address, sizeof( address ) ) == 0 &&
         getsockname( receiver, (struct sockaddr *) &address, &address_bytes ) == 0 && fcntl( receiver, F_SETFL, O_NONBLOCK ) == 0 )
    {
        struct rocketnet_kernel_own_socket_t before[ROCKETNET_KERNEL_OWN_MAX_SOCKETS];
        const int num_before = rocketnet_kernel_own_sockets( before, ROCKETNET_KERNEL_OWN_MAX_SOCKETS );
        int received = 0;
        for ( int attempt = 0; attempt < 3 && received == 0; attempt++ )
        {
            const uint8_t byte = 0x5a;
            if ( sendto( sender, &byte, 1, 0, (const struct sockaddr *) &address, sizeof( address ) ) != 1 )
            {
                continue;
            }
            for ( int wait = 0; wait < 1000 && received == 0; wait++ )
            {
                uint8_t buffer[16];
                if ( recv( receiver, buffer, sizeof( buffer ), 0 ) == 1 )
                {
                    received = 1;
                }
                else
                {
                    usleep( 1000 );
                }
            }
        }

        struct rocketnet_kernel_own_socket_t after[ROCKETNET_KERNEL_OWN_MAX_SOCKETS];
        const int num_after = rocketnet_kernel_own_sockets( after, ROCKETNET_KERNEL_OWN_MAX_SOCKETS );
        if ( received && num_before >= 0 && num_after >= 0 )
        {
            // the receiver is found by its port: the process's other sockets may be receiving too
            int moved = 0;
            uint64_t delta = 0;
            for ( int i = 0; i < num_after; i++ )
            {
                if ( after[i].port != address.sin_port )
                {
                    continue;
                }
                uint64_t base = 0;
                for ( int j = 0; j < num_before; j++ )
                {
                    if ( before[j].id == after[i].id )
                    {
                        base = before[j].rx;
                        break;
                    }
                }
                moved++;
                delta = after[i].rx - base;
            }
            if ( moved == 1 && ( delta == 1 || delta == 2 ) )
            {
                measured = (int) delta;
            }
        }
    }

    if ( sender >= 0 )
    {
        close( sender );
    }
    if ( receiver >= 0 )
    {
        close( receiver );
    }
    return measured;
}

// datagrams this process's sockets sent since `before` that none of them received:
// sockets open now count from their count in `before`, or from 0 if opened since;
// sockets closed since are left out. -1 when the counts cannot be read.

static inline int64_t rocketnet_kernel_own_lost_since( const struct rocketnet_kernel_own_socket_t * before, int num_before )
{
    const int rx_per_datagram = rocketnet_kernel_rx_per_datagram();
    if ( rx_per_datagram == 0 || num_before < 0 )
    {
        return -1;
    }

    struct rocketnet_kernel_own_socket_t now[ROCKETNET_KERNEL_OWN_MAX_SOCKETS];
    const int num_now = rocketnet_kernel_own_sockets( now, ROCKETNET_KERNEL_OWN_MAX_SOCKETS );
    if ( num_now < 0 )
    {
        return -1;
    }

    int64_t sent = 0;
    int64_t received = 0;
    for ( int i = 0; i < num_now; i++ )
    {
        uint64_t base_rx = 0;
        uint64_t base_tx = 0;
        for ( int j = 0; j < num_before; j++ )
        {
            if ( before[j].id == now[i].id )
            {
                base_rx = before[j].rx;
                base_tx = before[j].tx;
                break;
            }
        }
        sent += (int64_t) ( now[i].tx - base_tx );
        received += (int64_t) ( now[i].rx - base_rx );
    }

    const int64_t lost = sent - received / rx_per_datagram;
    return lost > 0 ? lost : 0;
}

#endif // #if defined( __APPLE__ )

// the kernel's count of UDP datagrams it dropped on receive at this process's
// own sockets (per platform above; macOS has none, its own-socket loss is
// rocketnet_kernel_own_lost_since)

static inline uint64_t rocketnet_kernel_udp_receive_drops()
{
#if defined( __APPLE__ )
    return 0;
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
#define ROCKETNET_LOSS_GATE_SCOPE                                                                   \
    "the datagrams this process's own sockets sent and none of them received (a full receive " \
    "buffer is not excused on macOS: its only count, udps_fullsock, is machine-wide)"
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
#if defined( __APPLE__ )
    struct rocketnet_kernel_own_socket_t own_at_start[ROCKETNET_KERNEL_OWN_MAX_SOCKETS];
    int num_own_at_start;
#endif // #if defined( __APPLE__ )
};

static inline void rocketnet_loss_gate_begin( struct rocketnet_loss_gate_t * gate, const char * name )
{
    gate->name = name;
    gate->drops_at_start = rocketnet_kernel_udp_receive_drops();
    gate->drops_used = 0;
    gate->resends = 0;
#if defined( __APPLE__ )
    rocketnet_kernel_rx_per_datagram();
    gate->num_own_at_start = rocketnet_kernel_own_sockets( gate->own_at_start, ROCKETNET_KERNEL_OWN_MAX_SOCKETS );
#endif // #if defined( __APPLE__ )
}

// the kernel's count of this case's lost datagrams since the gate began

static inline uint64_t rocketnet_loss_gate_drops( const struct rocketnet_loss_gate_t * gate )
{
    const uint64_t now = rocketnet_kernel_udp_receive_drops();
    uint64_t drops = now >= gate->drops_at_start ? now - gate->drops_at_start : 0;
#if defined( __APPLE__ )
    const int64_t own_lost = rocketnet_kernel_own_lost_since( gate->own_at_start, gate->num_own_at_start );
    if ( own_lost > 0 )
    {
        drops += (uint64_t) own_lost;
    }
#endif // #if defined( __APPLE__ )
    return drops;
}

// a case is short of `missing` datagrams after its wait: true when the kernel
// counted at least that many receive drops not yet used by an earlier resend,
// and the case has resends left. The caller sends the missing ones again.

static inline bool rocketnet_loss_gate_resend( struct rocketnet_loss_gate_t * gate, int missing )
{
    const uint64_t drops = rocketnet_loss_gate_drops( gate );
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

#endif // ROCKETNET_TESTS_KERNEL_DROPS_H
