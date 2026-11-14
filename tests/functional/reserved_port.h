/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_FUNCTIONAL_RESERVED_PORT_H
#define ROCKETNET_TESTS_FUNCTIONAL_RESERVED_PORT_H

// A port for a case that must bind the same port number twice: destroy a
// transport, then bind its port again to show its sockets are gone, or bind
// an explicit port and hold it while another bind of it is refused.
//
// A port the kernel chose (a bind of port 0) is not the case's to keep. Once
// the first transport releases it, any case running in parallel that binds
// port 0 can be given that port, and the second bind fails with EADDRINUSE
// for a reason that has nothing to do with the transport. The suite runs its
// cases in parallel, so this happened: test_io_uring_transport_server_probe
// failed in CI with "io_uring transport: failed to bind socket (ipv4):
// Address already in use" on the second bind.
//
// So the port comes from below the kernel's ephemeral range
// (/proc/sys/net/ipv4/ip_local_port_range), where no bind of port 0 is ever
// given a port. Only a socket that names the port can take it. Each caller
// names its own slot, and each slot is a separate part of the band, so two
// cases never scan the same ports. In a slot the scan starts at an offset
// taken from the process id, so two suites on one host start at different
// ports. A port is used when a probe bind of it, on every address, succeeds.
//
// The band is 30000 up to the ephemeral range. 1025-29999 belongs to the
// restart_same_port cells (posix_reserved_restart_port in
// posix_lifecycle_stress.cpp), which split it between them in the same way.
//
// The result is 0 when the host leaves no band below its ephemeral range, or
// when no port in the slot can be bound. The caller checks for 0.

#if defined( __linux__ )

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

enum rocketnet_test_reserved_port_slot_t
{
    ROCKETNET_TEST_PORT_URING_PROBE = 0,
    ROCKETNET_TEST_PORT_URING_STALL,
    ROCKETNET_TEST_PORT_GSO_EXPLICIT,
    ROCKETNET_TEST_PORT_GSO_HELD,
    ROCKETNET_TEST_PORT_URING_EXPLICIT,
    ROCKETNET_TEST_PORT_URING_HELD,
    ROCKETNET_TEST_PORT_CLOSED_SEND,    // a port no socket binds: a connected send to it draws ICMP port unreachable
    ROCKETNET_TEST_PORT_SLOTS
};

static inline uint16_t rocketnet_test_reserved_port( int slot )
{
    const unsigned first_port = 30000;

    FILE * port_range = fopen( "/proc/sys/net/ipv4/ip_local_port_range", "r" );
    if ( !port_range )
    {
        printf( "  reserved port: /proc/sys/net/ipv4/ip_local_port_range is not readable\n" );
        return 0;
    }

    unsigned ephemeral_first = 0;
    unsigned ephemeral_last = 0;
    const int range_read = fscanf( port_range, "%u %u", &ephemeral_first, &ephemeral_last );
    fclose( port_range );
    if ( range_read != 2 || ephemeral_first <= first_port + ROCKETNET_TEST_PORT_SLOTS )
    {
        printf( "  reserved port: no band between %u and the ephemeral range (starts at %u)\n", first_port, ephemeral_first );
        return 0;
    }

    if ( slot < 0 || slot >= ROCKETNET_TEST_PORT_SLOTS )
    {
        return 0;
    }

    const unsigned band = ephemeral_first - first_port;
    const unsigned slice = band / ROCKETNET_TEST_PORT_SLOTS;
    const unsigned base = first_port + (unsigned) slot * slice;
    const unsigned start = (unsigned) getpid() % slice;

    for ( unsigned i = 0; i < slice; i++ )
    {
        const unsigned port = base + ( start + i ) % slice;

        const int fd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
        if ( fd < 0 )
        {
            printf( "  reserved port: socket failed: %s\n", strerror( errno ) );
            return 0;
        }

        struct sockaddr_in address;
        memset( &address, 0, sizeof( address ) );
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl( INADDR_ANY );
        address.sin_port = htons( (uint16_t) port );

        const int result = bind( fd, (struct sockaddr *) &address, sizeof( address ) );
        close( fd );
        if ( result == 0 )
        {
            return (uint16_t) port;
        }
    }

    printf( "  reserved port: no free port in slot %d (%u-%u)\n", slot, base, base + slice - 1 );
    return 0;
}

#endif // #if defined( __linux__ )

#endif // ROCKETNET_TESTS_FUNCTIONAL_RESERVED_PORT_H
