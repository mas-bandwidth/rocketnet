/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Linux server transports' shared socket steps: one socket of a
// reuseport group, and the UDP_SEGMENT and UDP_GRO probes over the group
// (sdk/rocketnet_server_udp_linux.h). The gso and io_uring transports call
// both at create.

#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_server_udp_linux.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

// UDP_SEGMENT is linux 4.18, UDP_GRO linux 5.0: older userspace headers
// lack the constants even when the running kernel has the features. the
// values are kernel ABI and the runtime probe decides whether they work

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

static void rocketnet_server_udp_set_socket_option( void * user, int level, int option_name, int value )
{
    const int fd = *(const int *) user;
    setsockopt( fd, level, option_name, &value, sizeof( value ) );
}

int rocketnet_server_udp_open( const char * transport, const rocketnet_address_t * bind_address, int queue_index,
                               int max_clients, uint16_t * bound_port )
{
    rocketnet_assert( transport );
    rocketnet_assert( bind_address );
    rocketnet_assert( bound_port );

    const bool ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;

    int fd = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP );
    if ( fd < 0 )
    {
        rocketnet_error( "%s transport: failed to create socket: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
        return -1;
    }

    // the first queue binds before it joins a reuseport group, whether its
    // port is explicit or discovered. bound with SO_REUSEPORT, an explicit
    // port another reuseport socket of the same user already holds is joined,
    // not refused, and the kernel then shares that port's datagrams between
    // the two servers; an ephemeral bind may likewise be given a port another
    // reuseport group already holds (measured on Linux x64 gcc: 4 shared
    // ports in 50 x 64 reuseport ephemeral binds, none when the first socket
    // binds without it). Bound first, it takes a port no socket holds or the
    // bind fails with EADDRINUSE as the portable transport's does, and
    // SO_REUSEPORT set after the bind lets this server's other queues join it.

    const bool first_queue = queue_index == 0;

    int one = 1;
    if ( !first_queue && setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) != 0 )
    {
        rocketnet_error( "%s transport: failed to set SO_REUSEPORT: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
        close( fd );
        return -1;
    }

    if ( ipv6 )
    {
        int zero = 0;
        if ( setsockopt( fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof( zero ) ) != 0 )
        {
            rocketnet_error( "%s transport: failed to clear socket ipv6 only: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
            close( fd );
            return -1;
        }
    }

    // request the buffer sizes, then read back what was granted. linux
    // returns the doubled bookkeeping value from getsockopt, so halve it
    // before comparing against the request. a small grant is not an error --
    // it is a fact about the host's sysctls, reported so nobody assumes.
    //
    // the receive request is one update's inbound packets at the full client
    // count (rocketnet_server_transport_receive_buffer_request; a request,
    // and the kernel charges only what it holds): SO_REUSEPORT may hash any
    // share of the clients to this one socket

    int send_buffer_size = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    int receive_buffer_size = rocketnet_server_transport_receive_buffer_request( transport, max_clients );

    if ( setsockopt( fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof( int ) ) != 0 )
    {
        rocketnet_error( "%s transport: failed to set socket send buffer size: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
        close( fd );
        return -1;
    }

    if ( setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size, sizeof( int ) ) != 0 )
    {
        rocketnet_error( "%s transport: failed to set socket receive buffer size: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
        close( fd );
        return -1;
    }

    int granted_send = 0;
    int granted_receive = 0;
    socklen_t optlen = sizeof( int );
    getsockopt( fd, SOL_SOCKET, SO_SNDBUF, &granted_send, &optlen );
    optlen = sizeof( int );
    getsockopt( fd, SOL_SOCKET, SO_RCVBUF, &granted_receive, &optlen );
    granted_send /= 2;
    granted_receive /= 2;

    if ( first_queue )
    {
        if ( granted_send < send_buffer_size )
        {
            rocketnet_warn( "%s transport: socket send buffer granted %d of %d requested bytes (raise net.core.wmem_max)",
                            transport, granted_send, send_buffer_size );
        }

        rocketnet_server_receive_buffer_granted( transport, receive_buffer_size, granted_receive );
        if ( granted_send >= send_buffer_size && granted_receive >= receive_buffer_size )
        {
            rocketnet_info( "%s transport: socket buffers granted: send %d receive %d", transport, granted_send, granted_receive );
        }
    }

    // bind. every queue binds the same address and port; the first queue may
    // discover an ephemeral port and the rest join it, which is why the
    // bound port comes back out.

    rocketnet_address_t addr = *bind_address;
    addr.port = *bound_port != 0 ? *bound_port : bind_address->port;

    struct sockaddr_storage sockaddr;
    const int socklen = rocketnet_platform_sockaddr_for( ipv6, &addr, &sockaddr );

    if ( socklen == 0 || bind( fd, (struct sockaddr *) &sockaddr, (socklen_t) socklen ) < 0 )
    {
        rocketnet_error( "%s transport: failed to bind socket (%s): %s", transport, ipv6 ? "ipv6" : "ipv4", rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
        close( fd );
        return -1;
    }

    if ( first_queue && setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) != 0 )
    {
        rocketnet_error( "%s transport: failed to set SO_REUSEPORT: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
        close( fd );
        return -1;
    }

    if ( *bound_port == 0 )
    {
        struct sockaddr_storage bound;
        memset( &bound, 0, sizeof( bound ) );
        socklen_t bound_length = sizeof( bound );
        if ( getsockname( fd, (struct sockaddr *) &bound, &bound_length ) != 0 )
        {
            rocketnet_error( "%s transport: failed to get bound socket port: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
            close( fd );
            return -1;
        }

        rocketnet_address_t bound_address;
        if ( !rocketnet_platform_sockaddr_to_address( &bound, ipv6, &bound_address ) )
        {
            rocketnet_error( "%s transport: could not read bound socket address", transport );
            close( fd );
            return -1;
        }

        *bound_port = bound_address.port;
    }

    if ( fcntl( fd, F_SETFL, fcntl( fd, F_GETFL, 0 ) | O_NONBLOCK ) == -1 )
    {
        rocketnet_error( "%s transport: failed to set socket non-blocking: %s", transport, rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
        close( fd );
        return -1;
    }

    rocketnet_platform_socket_apply_dont_fragment( ipv6, rocketnet_server_udp_set_socket_option, &fd );

    return fd;
}

void rocketnet_server_udp_probe_offloads( const int * fds, int num_fds, bool probe_segment, bool probe_gro,
                                          rocketnet_server_udp_setsockopt_t set, bool * segment, bool * gro )
{
    rocketnet_assert( fds );
    rocketnet_assert( num_fds > 0 );
    rocketnet_assert( set );
    rocketnet_assert( segment );
    rocketnet_assert( gro );

    *segment = false;
    *gro = false;

    if ( probe_segment )
    {
        int segment_sockets = 0;

        for ( int i = 0; i < num_fds; i++ )
        {
            int zero = 0;
            if ( set( fds[i], IPPROTO_UDP, UDP_SEGMENT, &zero, sizeof( zero ) ) == 0 )
            {
                segment_sockets++;
            }
        }

        *segment = segment_sockets == num_fds;
    }

    if ( probe_gro )
    {
        int gro_sockets = 0;

        for ( int i = 0; i < num_fds; i++ )
        {
            int one = 1;
            if ( set( fds[i], IPPROTO_UDP, UDP_GRO, &one, sizeof( one ) ) == 0 )
            {
                gro_sockets++;
            }
        }

        if ( gro_sockets == num_fds )
        {
            *gro = true;
        }
        else
        {
            // all or nothing: a partial enable would leave socket state the
            // reported tier denies, so the ones that accepted roll back

            for ( int i = 0; i < num_fds; i++ )
            {
                int zero = 0;
                set( fds[i], IPPROTO_UDP, UDP_GRO, &zero, sizeof( zero ) );
            }
        }
    }
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
