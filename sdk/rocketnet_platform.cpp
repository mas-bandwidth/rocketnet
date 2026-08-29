/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

// linux and the BSDs. one POSIX section: the sockets, threads and mutexes
// are identical; everything linux-only in here (netlink, the wireless
// ioctl, PMTU discovery, the raw clock) sits behind __linux__ with the
// portable shape beside it.

#include "rocketnet_platform.h"
#include "rocketnet_address.h"

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if defined(__linux__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/wireless.h>
#include <alloca.h>
#endif // #if defined(__linux__)

// the BSDs have no CLOCK_MONOTONIC_RAW; CLOCK_MONOTONIC is their
// unadjusted-rate monotonic clock and serves the same reads
#if !defined(CLOCK_MONOTONIC_RAW)
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif // #if !defined(CLOCK_MONOTONIC_RAW)

static int connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

static int get_connection_type();

static double time_start;

bool rocketnet_platform_init()
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    time_start = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    connection_type = get_connection_type();
    return true;
}

void rocketnet_platform_term()
{
    // ...
}

const char * rocketnet_platform_getenv( const char * var )
{
    return getenv( var );
}

// ---------------------------------------------------

int rocketnet_platform_id()
{
    return ROCKETNET_PLATFORM;
}

int rocketnet_platform_connection_type()
{
    return connection_type;
}

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;
    *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
    return inet_ntop( AF_INET6, (void*)address, address_string, socklen_t( address_string_size ) ) != NULL;
}

bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, rocketnet_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof(hints) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                sockaddr_in6 * addr_ipv6 = (sockaddr_in6 *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV6;
                for ( int i = 0; i < 8; ++i )
                {
                    address->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
                }
                address->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                sockaddr_in * addr_ipv4 = (sockaddr_in *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV4;
                address->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
                address->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
                address->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
                address->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
                address->port = rocketnet_platform_ntohs( addr_ipv4->sin_port );
                freeaddrinfo( result );
                return true;
            }
            else
            {
                rocketnet_assert( 0 );
                freeaddrinfo( result );
                return false;
            }
        }
    }

    return false;
}

uint16_t rocketnet_platform_preferred_client_port()
{
    return 0;
}

bool rocketnet_platform_client_dual_stack()
{
    return true;
}

// ---------------------------------------------------

double rocketnet_platform_time()
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    double current = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    return current - time_start;
}

void rocketnet_platform_sleep( double time )
{
    usleep( (int) ( time * 1000000 ) );
}

// ---------------------------------------------------

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket );

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size )
{
    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    rocketnet_platform_socket_t * socket = (rocketnet_platform_socket_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );

    rocketnet_assert( socket );

    socket->context = context;

    // create socket

    socket->type = socket_type;

    socket->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;

    socket->handle = ::socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( socket->handle < 0 )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }

    // enable dual stack for ipv6 sockets

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( socket->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // increase socket send and receive buffer sizes

    if ( setsockopt( socket->handle, SOL_SOCKET, SO_SNDBUF, (char*)( &send_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        return NULL;
    }

    if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &receive_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket receive buffer size" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    // bind to port

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( sockaddr_in6 ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( address->data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( address->port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv6)" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        sockaddr_in socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin_family = AF_INET;
        socket_address.sin_addr.s_addr = ( ( (uint32_t) address->data.ipv4[0] ) )      | 
                                         ( ( (uint32_t) address->data.ipv4[1] ) << 8 )  | 
                                         ( ( (uint32_t) address->data.ipv4[2] ) << 16 ) | 
                                         ( ( (uint32_t) address->data.ipv4[3] ) << 24 );
        socket_address.sin_port = rocketnet_platform_htons( address->port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv4)" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // if bound to port 0 find the actual port we got

    if ( address->port == 0 )
    {
        if ( address->type == ROCKETNET_ADDRESS_IPV6 )
        {
            sockaddr_in6 sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv6)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin6_port );
        }
        else
        {
            sockaddr_in sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv4)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin_port );
        }
    }

    // set non-blocking io and receive timeout

    if ( socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        // non-blocking
        if ( fcntl( socket->handle, F_SETFL, O_NONBLOCK, 1 ) == -1 )
        {
            rocketnet_error( "failed to set socket to non-blocking" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else if ( timeout_seconds > 0.0f )
    {
        // blocking with receive timeout
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (int) ( timeout_seconds * 1000000.0f );
        if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) ) < 0 )
        {
            rocketnet_error( "failed to set socket receive timeout" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        // blocking with no timeout
    }

    // set don't fragment bit

#if defined(__linux__)
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int val = IPV6_PMTUDISC_DO;
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val) );
    }
    else
    {
        int val = IP_PMTUDISC_DO;
        setsockopt( socket->handle, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val) );
    }
#else
    // the BSDs express DF via the RFC 3542 options where they have them
    // (FreeBSD both, OpenBSD and NetBSD IPv6 only); same shape the darwin
    // section uses, and like the linux calls above, refusal is tolerated
    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
    #if defined(IPV6_DONTFRAG)
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_DONTFRAG, &val, sizeof(val) );
    #endif // #if defined(IPV6_DONTFRAG)
    }
    else
    {
    #if defined(IP_DONTFRAG)
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val) );
    #endif // #if defined(IP_DONTFRAG)
    }
#endif // #if defined(__linux__)

    return socket;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    if ( socket->handle != 0 )
    {
        close( socket->handle );
    }
    rocketnet_free( socket->context, socket );
}

void rocketnet_platform_socket_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * to_input, const void * packet_data, int packet_bytes )
{
    rocketnet_assert( socket );
    rocketnet_assert( to_input );
    rocketnet_assert( to_input->type == ROCKETNET_ADDRESS_IPV6 || to_input->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    rocketnet_address_t to = *to_input;

    if ( socket->ipv6 )
    {
        // socket is dual stack ipv4 and ipv6

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( to.port );
        int result = int( sendto( socket->handle, (char*)( packet_data ), packet_bytes, 0, (sockaddr*)( &socket_address ), sizeof(sockaddr_in6) ) );
        if ( result < 0 )
        {
            char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            rocketnet_address_to_string( &to, address_string );
            rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
        }
    }
    else
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            sockaddr_in socket_address;
            memset( &socket_address, 0, sizeof( socket_address ) );
            socket_address.sin_family = AF_INET;
            socket_address.sin_addr.s_addr = ( ( (uint32_t) to.data.ipv4[0] ) )        | 
                                             ( ( (uint32_t) to.data.ipv4[1] ) << 8 )   | 
                                             ( ( (uint32_t) to.data.ipv4[2] ) << 16 )  | 
                                             ( ( (uint32_t) to.data.ipv4[3] ) << 24 );
            socket_address.sin_port = rocketnet_platform_htons( to.port );
            int result = int( sendto( socket->handle, (const char*)( packet_data ), packet_bytes, 0, (sockaddr*)( &socket_address ), sizeof(sockaddr_in) ) );
            if ( result < 0 )
            {
                char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
                rocketnet_address_to_string( &to, address_string );
                rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
            }
        }
        else
        {
            rocketnet_error( "invalid address. could not send packet" );
        }
    }
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( max_packet_size > 0 );

    sockaddr_storage sockaddr_from;
    socklen_t from_length;

    int result;
    for ( ;; )
    {
        from_length = sizeof( sockaddr_from );

        result = int( recvfrom( socket->handle, (char*) packet_data, max_packet_size, socket->type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING ? MSG_DONTWAIT : 0, (sockaddr*) &sockaddr_from, &from_length ) );

        if ( result < 0 )
        {
            if ( errno != EAGAIN && errno != EINTR )
            {
                rocketnet_debug( "recvfrom failed with error %d", errno );
            }
            return 0;
        }

        if ( result == 0 )
        {
            // a zero-length datagram is legal UDP carrying nothing the
            // protocol admits -- and returning 0 here would read as
            // "socket drained", letting byte-less chaff cap an entire
            // drain at one packet (#100's review). skip it, keep reading:
            // both pumps share this semantic.
            continue;
        }

        break;
    }

    if ( sockaddr_from.ss_family == AF_INET6 )
    {
        sockaddr_in6 * addr_ipv6 = (sockaddr_in6*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );

        if ( socket->ipv6 && rocketnet_address_is_ipv4_in_ipv6( from ) )
        {
            rocketnet_address_convert_ipv6_to_ipv4( from );
        }
    }
    else if ( sockaddr_from.ss_family == AF_INET )
    {
        sockaddr_in * addr_ipv4 = (sockaddr_in*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV4;
        from->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
        from->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
        from->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
        from->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
        from->port = rocketnet_platform_ntohs( addr_ipv4->sin_port );
    }
    else
    {
        rocketnet_assert( 0 );
        return 0;
    }
  
    rocketnet_assert( result >= 0 );

    return result;
}

// ---------------------------------------------------

struct thread_shim_data_t
{
    void * context;
    void * real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function;
};

static void* thread_function_shim( void * data )
{
    rocketnet_assert( data );
    thread_shim_data_t * shim_data = (thread_shim_data_t*) data;
    void * context = shim_data->context;
    void * real_thread_data = shim_data->real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function = shim_data->real_thread_function;
    rocketnet_free( context, data );
    real_thread_function( real_thread_data );
    return NULL;
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg )
{
    rocketnet_platform_thread_t * thread = (rocketnet_platform_thread_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_thread_t) );

    rocketnet_assert( thread );

    thread->context = context;

    thread_shim_data_t * shim_data = (thread_shim_data_t*) rocketnet_malloc( context, sizeof(thread_shim_data_t) );
    rocketnet_assert( shim_data );
    if ( !shim_data )
    {
        rocketnet_free( context, thread );
        return NULL;
    }
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;

    if ( pthread_create( &thread->handle, NULL, thread_function_shim, shim_data ) != 0 )
    {
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
        return NULL;
    }

    return thread;
}

void rocketnet_platform_thread_join( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    pthread_join( thread->handle, NULL );
}

void rocketnet_platform_thread_destroy( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    rocketnet_free( thread->context, thread );
}

void rocketnet_platform_client_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you have linux clients, you can adjust the priority of the client thread here.
    struct sched_param param;
    param.sched_priority = sched_get_priority_min( SCHED_RR );
    pthread_setschedparam( thread->handle, SCHED_RR, &param );
}

void rocketnet_platform_server_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you have linux servers, you can adjust the priority of the server thread here.
    struct sched_param param;
    param.sched_priority = sched_get_priority_min( SCHED_RR );
    pthread_setschedparam( thread->handle, SCHED_RR, &param );
}

// ---------------------------------------------------

bool rocketnet_platform_mutex_create( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );

    memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );
    
    pthread_mutexattr_t attr;
    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, 0 );
    int result = pthread_mutex_init( &mutex->handle, &attr );
    pthread_mutexattr_destroy( &attr );

    if ( result != 0 )
        return false;

    mutex->ok = true;

    return true;
}

void rocketnet_platform_mutex_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    pthread_mutex_lock( &mutex->handle );
}

void rocketnet_platform_mutex_release( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    pthread_mutex_unlock( &mutex->handle );
}

void rocketnet_platform_mutex_destroy( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    if ( mutex->ok )
    {
        pthread_mutex_destroy( &mutex->handle );
        memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );
    }
}

// ---------------------------------------------------

extern void * rocketnet_global_context;

template <typename T> struct rocketnet_vector_t
{
    T * data;
    int length;
    int reserved;

    inline rocketnet_vector_t( int reserve_count = 0 )
    {
        rocketnet_assert( reserve_count >= 0 );
        data = 0;
        length = 0;
        reserved = 0;
        if ( reserve_count > 0 )
        {
            reserve( reserve_count );
        }
    }

    inline ~rocketnet_vector_t()
    {
        clear();
    }

    inline void clear() 
    {
        if ( data )
        {
            rocketnet_free( rocketnet_global_context, data );
        }
        data = NULL;
        length = 0;
        reserved = 0;
    }

    inline const T & operator [] ( int i ) const
    {
        rocketnet_assert( data );
        rocketnet_assert( i >= 0 && i < length );
        return *( data + i );
    }

    inline T & operator [] ( int i )
    {
        rocketnet_assert( data );
        rocketnet_assert( i >= 0 && i < length );
        return *( data + i );
    }

    void reserve( int size )
    {
        rocketnet_assert( size >= 0 );
        if ( size > reserved )
        {
            const double VECTOR_GROWTH_FACTOR = 1.5;
            const int VECTOR_INITIAL_RESERVATION = 1;
            unsigned int rocketnet_size = (unsigned int)( pow( VECTOR_GROWTH_FACTOR, int( log( double( size ) ) / log( double( VECTOR_GROWTH_FACTOR ) ) ) + 1 ) );
            if ( !reserved )
            {
                rocketnet_size = rocketnet_size > VECTOR_INITIAL_RESERVATION ? rocketnet_size : VECTOR_INITIAL_RESERVATION;
                data = (T*)( rocketnet_malloc( rocketnet_global_context, rocketnet_size * sizeof(T) ) );
                rocketnet_assert( data );
            }
            else
            {
                T * new_data = (T*)( rocketnet_malloc( rocketnet_global_context, rocketnet_size * sizeof(T) ) );
                rocketnet_assert( data );
                memcpy( new_data, data, reserved * sizeof(T) );
                rocketnet_free( rocketnet_global_context, data );
                data = new_data;
            }
            memset( (void*)( &data[reserved] ), 0, ( rocketnet_size - reserved ) * sizeof(T) );
            reserved = rocketnet_size;
        }
    }

    void resize( int i )
    {
        reserve( i );
        length = i;
    }

    void remove( int i )
    {
        rocketnet_assert( data );
        rocketnet_assert( i >= 0 && i < length );
        if ( i != length - 1 )
        {
            data[i] = data[length - 1];
        }
        length--;
    }

    void remove_ordered( int i )
    {
        rocketnet_assert( data );
        rocketnet_assert( i >= 0 && i < length );
        memmove( &data[i], &data[i + 1], sizeof( T ) * ( length - ( i + 1 ) ) );
        length--;
    }

    T * insert( int i )
    {
        rocketnet_assert( i >= 0 && i <= length );
        resize( length + 1 );
        memmove( &data[i + 1], &data[i], sizeof( T ) * ( length - 1 - i ) );
        return &data[i];
    }

    T * insert( int i, const T & t )
    {
        T * p = insert( i );
        *p = t;
        return p;
    }

    T * add()
    {
        reserve( ++length );
        return &data[length - 1];
    }

    T * add( const T & t )
    {
        T * p = add();
        *p = t;
        return p;
    }
};


// ---------------------------------------------------

#if defined(__linux__)

struct rocketnet_iftable_t
{
    uint32_t if_index;
    int connection_type;
};

struct rocketnet_ifforward4_t
{
    uint32_t if_index;
    uint32_t forward_dest;
    uint32_t forward_mask;
    uint32_t forward_metric;
};

static bool parse_kernel_route( struct nlmsghdr * msg_ptr, rocketnet_ifforward4_t * out )
{
    struct rtmsg * route_entry = (struct rtmsg *)( NLMSG_DATA( msg_ptr ) );

    if ( route_entry->rtm_table != RT_TABLE_MAIN )
        return false;

    out->forward_mask = ( 1 << route_entry->rtm_dst_len ) - 1;

    // read all attributes
    int route_attribute_len = RTM_PAYLOAD( msg_ptr );
    for ( struct rtattr * route_attribute = (struct rtattr *)( RTM_RTA( route_entry ) );
        RTA_OK( route_attribute, route_attribute_len );
        route_attribute = RTA_NEXT( route_attribute, route_attribute_len ) )
    {
        switch ( route_attribute->rta_type )
        {
            case RTA_DST:
            {
                memcpy( &out->forward_dest, RTA_DATA( route_attribute ), sizeof( out->forward_dest ) );
                break;
            }
            case RTA_OIF:
            {
                memcpy( &out->if_index, RTA_DATA( route_attribute ), sizeof( out->if_index ) );
                break;
            }
            case RTA_METRICS:
            {
                memcpy( &out->forward_metric, RTA_DATA( route_attribute ), sizeof( out->forward_metric ) );
                break;
            }
        }
    }

    return true;
}

#define CHECK_BIT( _num, _bit ) ( ( _num ) & ( uint32_t(1) << ( _bit ) ) )

static bool better_match( rocketnet_ifforward4_t * a, rocketnet_ifforward4_t * b, uint32_t ip )
{
    int matching_bits_a = 0;
    int matching_bits_b = 0;

    if ( a )
    {
        for ( int i = 0; i < 32; i++ )
        {
            if ( CHECK_BIT( a->forward_mask, i )
                && CHECK_BIT( a->forward_dest, i ) == CHECK_BIT( ip, i ) )
            {
                matching_bits_a++;
            }
        }
    }

    if ( b )
    {
        for ( int i = 0; i < 32; i++ )
        {
            if ( CHECK_BIT( b->forward_mask, i )
                && CHECK_BIT( b->forward_dest, i ) == CHECK_BIT( ip, i ) )
            {
                matching_bits_b++;
            }
        }
    }
    
    return matching_bits_a > matching_bits_b;
}

static int classify_connection_type( rocketnet_vector_t<rocketnet_ifforward4_t> * route_table, rocketnet_vector_t<rocketnet_iftable_t> * interface_list, const rocketnet_address_t * addr )
{
    uint32_t ip = ( ( (uint32_t) addr->data.ipv4[0] ) )        | 
                  ( ( (uint32_t) addr->data.ipv4[1] ) << 8 )   | 
                  ( ( (uint32_t) addr->data.ipv4[2] ) << 16 )  | 
                  ( ( (uint32_t) addr->data.ipv4[3] ) << 24 );
    rocketnet_ifforward4_t * best_match = NULL;
    for ( int i = 0; i < route_table->length; i++ )
    {
        rocketnet_ifforward4_t * route = &( ( *route_table )[i] );
        if ( better_match( route, best_match, ip ) )
        {
            best_match = route;
        }
    }

    rocketnet_iftable_t * outgoing_interface = NULL;

    if ( best_match )
    {
        for ( int i = 0; i < interface_list->length; i++ )
        {
            rocketnet_iftable_t * iface = &( (* interface_list )[i] );
            if ( iface->if_index == best_match->if_index )
            {
                outgoing_interface = iface;
                break;
            }
        }
    }

    if ( outgoing_interface )
    {
        return outgoing_interface->connection_type;
    }
    else
    {
        return ROCKETNET_CONNECTION_TYPE_WIRED;
    }
}

static int get_connection_type()
{
    rocketnet_vector_t<rocketnet_iftable_t> interface_list;
    rocketnet_vector_t<rocketnet_ifforward4_t> route_table;

    rocketnet_debug( "getting network info" );

    // get interfaces
    {
        struct ifaddrs * ifaddr;

        if ( getifaddrs( &ifaddr ) == -1 )
        {
            rocketnet_warn( "failed to get network interface addresses" );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        int sock = -1;

        if ( ( sock = socket( AF_INET, SOCK_STREAM, 0 ) ) == -1 )
        {
            freeifaddrs( ifaddr );
            rocketnet_warn( "failed to get network interface addresses" );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        for ( struct ifaddrs * i = ifaddr; i != NULL; i = i->ifa_next )
        {
            struct ifreq req;
            memset( &req, 0, sizeof( req ) );
            strncpy( req.ifr_name, i->ifa_name, IFNAMSIZ - 1 );

            if ( ioctl( sock, SIOCGIFINDEX, &req ) == -1 )
            {
                continue;
            }

            uint32_t if_index = req.ifr_ifindex;

            // check for duplicates
            bool duplicate = false;
            for ( int j = 0; j < interface_list.length; j++ )
            {
                if ( interface_list[j].if_index == if_index )
                {
                    duplicate = true;
                    break;
                }
            }
            if ( duplicate )
            {
                continue;
            }

            rocketnet_iftable_t * interface_entry = interface_list.add();
            interface_entry->if_index = if_index;

            struct iwreq pwrq;
            memset( &pwrq, 0, sizeof( pwrq ) );
            strncpy( pwrq.ifr_name, i->ifa_name, IFNAMSIZ - 1 );

            if ( ioctl( sock, SIOCGIWNAME, &pwrq ) == -1 )
            {
                interface_entry->connection_type = ROCKETNET_CONNECTION_TYPE_WIRED;
            }
            else
            {
                interface_entry->connection_type = ROCKETNET_CONNECTION_TYPE_WIFI;
            }
        }

        close( sock );

        freeifaddrs( ifaddr );
    }

    // get kernel route table
    {
        // open netlink socket
        
        pid_t pid = getpid();

        int sock = socket( AF_NETLINK, SOCK_RAW, NETLINK_ROUTE );
        if ( sock == -1 )
        {
            rocketnet_warn( "failed to open netlink socket" );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        struct sockaddr_nl local;
        memset( &local, 0, sizeof( local ) );
        local.nl_family = AF_NETLINK;
        local.nl_pid = pid;
        local.nl_groups = 0;

        if ( bind( sock, (struct sockaddr *)( &local ), sizeof( local ) ) < 0 )
        {
            rocketnet_warn( "failed to bind netlink socket" );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }

        // prepare route table request
        typedef struct nl_req_s nl_req_t;  
        struct nl_req_s
        {
            struct nlmsghdr hdr;
            struct rtgenmsg gen;
        };

        nl_req_t req;
        memset( &req, 0, sizeof( req ) );
        req.hdr.nlmsg_len = NLMSG_LENGTH( sizeof( struct rtgenmsg ) );
        req.hdr.nlmsg_type = RTM_GETROUTE;
        req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP; 
        req.hdr.nlmsg_seq = 1;
        req.hdr.nlmsg_pid = pid;
        req.gen.rtgen_family = AF_INET; 

        struct iovec io;
        io.iov_base = &req;
        io.iov_len = req.hdr.nlmsg_len;

        struct sockaddr_nl kernel;
        memset( &kernel, 0, sizeof( kernel ) );
        kernel.nl_family = AF_NETLINK;
        kernel.nl_groups = 0;

        struct msghdr rtnl_msg;
        memset( &rtnl_msg, 0, sizeof( rtnl_msg ) );
        rtnl_msg.msg_iov = &io;
        rtnl_msg.msg_iovlen = 1;
        rtnl_msg.msg_name = &kernel;
        rtnl_msg.msg_namelen = sizeof( kernel );

        // send request
        sendmsg( sock, (struct msghdr *)( &rtnl_msg ), 0 );

        // read reply

        bool done = false;
        while ( !done )
        {
            struct msghdr rtnl_reply;
            struct iovec io_reply;

            memset( &io_reply, 0, sizeof( io_reply ) );
            memset( &rtnl_reply, 0, sizeof( rtnl_reply ) );

            const size_t IFLIST_REPLY_BUFFER = 8192;
            char reply[IFLIST_REPLY_BUFFER];
            io_reply.iov_base = reply;
            io_reply.iov_len = IFLIST_REPLY_BUFFER;
            rtnl_reply.msg_iov = &io_reply;
            rtnl_reply.msg_iovlen = 1;
            rtnl_reply.msg_name = &kernel;
            rtnl_reply.msg_namelen = sizeof( kernel );

            int len = recvmsg( sock, &rtnl_reply, 0 );
            if ( len )
            {
                for ( struct nlmsghdr * msg_ptr = (struct nlmsghdr *)( reply ); NLMSG_OK( msg_ptr, len ); msg_ptr = NLMSG_NEXT( msg_ptr, len ) )
                {
                    if ( msg_ptr->nlmsg_type == 24 )
                    {
                        rocketnet_ifforward4_t route;
                        if ( parse_kernel_route( msg_ptr, &route ) )
                        {
                            route_table.add( route );
                        }
                    }
                    else if ( msg_ptr->nlmsg_type == 3 )
                    {
                        done = true;
                    }
                }
            }
        }

        close( sock );
    }

    rocketnet_address_t address;
    address.port = 80;
    address.data.ipv4[0] = 8;
    address.data.ipv4[1] = 8;
    address.data.ipv4[2] = 8;
    address.data.ipv4[3] = 8;

    return classify_connection_type( &route_table, &interface_list, &address );
}

#else // #if defined(__linux__)

// the BSDs: no netlink and no wireless-extensions ioctl to classify with.
// UNKNOWN is the honest answer and is already the init fallback everywhere.

static int get_connection_type()
{
    return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
}

#endif // #if defined(__linux__)

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// mac or ios

#include "rocketnet_address.h"

#define __APPLE_USE_RFC_3542

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <dlfcn.h>

extern void * rocketnet_malloc( void * context, size_t bytes );

extern void rocketnet_free( void * context, void * p );

// ---------------------------------------------------

// The darwin batch pump (#100): sendmsg_x/recvmsg_x, Darwin's batch UDP
// syscalls -- private but stable, used by shipping engines, and measured on
// the Studio (see bench/probe_darwin.c) to support the exact server-pump
// shape: unconnected sockets, a per-message destination on send, source
// address and received length filled per message on receive.
//
// The private-API risk is handled by never depending on them: the symbols
// are resolved by dlsym at init (no link-time reference at all -- weaker
// even than a weak import), each socket runs a functional probe at create,
// any refusal at runtime degrades that socket to the portable per-packet
// loop mid-flight without losing a packet, and ROCKETNET_PUMP=portable
// forces the degraded path outright. iOS never resolves them: dlsym of a
// private symbol is an App Store review finding, so the probe is compiled
// out and iOS is portable by construction.
//
// struct msghdr_x is from xnu bsd/sys/socket_private.h, defined locally
// because no public SDK header carries it; the layout is the syscall ABI.

struct rocketnet_msghdr_x_t
{
    void *          msg_name;
    socklen_t       msg_namelen;
    struct iovec *  msg_iov;
    int             msg_iovlen;
    void *          msg_control;
    socklen_t       msg_controllen;
    int             msg_flags;
    size_t          msg_datalen;
};

typedef ssize_t (*rocketnet_sendmsg_x_fn)( int s, const struct rocketnet_msghdr_x_t * msgp, u_int cnt, int flags );
typedef ssize_t (*rocketnet_recvmsg_x_fn)( int s, struct rocketnet_msghdr_x_t * msgp, u_int cnt, int flags );

static rocketnet_sendmsg_x_fn rocketnet_sendmsg_x = NULL;
static rocketnet_recvmsg_x_fn rocketnet_recvmsg_x = NULL;

// the seam batch: how many messages one syscall carries. matches the
// transport's own batch shape; chunking inside the pump keeps the stack
// arrays small and bounded.

#define ROCKETNET_MSGX_BATCH 32

// ---------------------------------------------------

static int connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

static mach_timebase_info_data_t timebase_info;

static uint64_t time_start;

bool rocketnet_platform_init()
{
    mach_timebase_info( &timebase_info );

    time_start = mach_absolute_time();

#if TARGET_OS_OSX
    // resolve the batch pump syscalls once. absence is not an error: every
    // socket that fails this resolution simply runs the portable pump.
    rocketnet_sendmsg_x = (rocketnet_sendmsg_x_fn) dlsym( RTLD_DEFAULT, "sendmsg_x" );
    rocketnet_recvmsg_x = (rocketnet_recvmsg_x_fn) dlsym( RTLD_DEFAULT, "recvmsg_x" );
#endif // #if TARGET_OS_OSX

    connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

    // SystemConfiguration's dynamic store is macOS-only; iOS has the framework
    // but not this key, so the lookup below yields nothing and the sandbox
    // objects to the attempt. iOS keeps the UNKNOWN connection type set above.
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_IOS

    SCDynamicStoreRef dynamic_store = SCDynamicStoreCreate( kCFAllocatorDefault, CFSTR( "FindCurrentInterfaceIpMac" ), NULL, NULL );
    CFPropertyListRef global = SCDynamicStoreCopyValue( dynamic_store, CFSTR( "State:/Network/Global/IPv4" ) );
    if ( global )
    {
        CFStringRef primary_interface_name = (CFStringRef)( CFDictionaryGetValue( (CFDictionaryRef)( global ), CFSTR( "PrimaryInterface" ) ) );
        CFArrayRef interfaces = SCNetworkInterfaceCopyAll();
        CFIndex count = CFArrayGetCount( interfaces );
        for ( CFIndex i = 0; i < count; i++ )
        {   
            SCNetworkInterfaceRef interface = (SCNetworkInterfaceRef)( CFArrayGetValueAtIndex( interfaces, i ) );
            CFStringRef bsd_name = SCNetworkInterfaceGetBSDName( interface );
            if ( CFStringCompare( primary_interface_name, bsd_name, 0 ) == kCFCompareEqualTo )
            {   
                CFStringRef interface_type = SCNetworkInterfaceGetInterfaceType( interface );
                if ( CFStringCompare( interface_type, kSCNetworkInterfaceTypeEthernet, 0 ) == kCFCompareEqualTo )
                {
                    connection_type = ROCKETNET_CONNECTION_TYPE_WIRED;
                }
                else if ( CFStringCompare( interface_type, kSCNetworkInterfaceTypeIEEE80211, 0 ) == kCFCompareEqualTo )
                {
                    connection_type = ROCKETNET_CONNECTION_TYPE_WIFI;
                }
                else if ( CFStringCompare( interface_type, kSCNetworkInterfaceTypeWWAN, 0 ) == kCFCompareEqualTo )
                {
                    connection_type = ROCKETNET_CONNECTION_TYPE_CELLULAR;
                }
                break;
            }
        }
        CFRelease( interfaces );
        CFRelease( global );
    }
    CFRelease( dynamic_store );

#endif // ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_IOS

    return true;
}

void rocketnet_platform_term()
{
    // ...
}

const char * rocketnet_platform_getenv( const char * var )
{
    return getenv( var );
}

// ---------------------------------------------------

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;
    *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
    return inet_ntop( AF_INET6, (void*)address, address_string, socklen_t( address_string_size ) ) != NULL;
}

// ---------------------------------------------------

bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, rocketnet_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof(hints) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                sockaddr_in6 * addr_ipv6 = (sockaddr_in6 *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV6;
                for ( int i = 0; i < 8; ++i )
                {
                    address->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
                }
                address->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                sockaddr_in * addr_ipv4 = (sockaddr_in *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV4;
                address->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
                address->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
                address->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
                address->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
                address->port = rocketnet_platform_ntohs( addr_ipv4->sin_port );
                freeaddrinfo( result );
                return true;
            }
            else
            {
                rocketnet_assert( 0 );
                freeaddrinfo( result );
                return false;
            }
        }
    }

    return false;
}

uint16_t rocketnet_platform_preferred_client_port()
{
    return 0;
}

bool rocketnet_platform_client_dual_stack()
{
    return true;
}

// ---------------------------------------------------

int rocketnet_platform_id()
{
    return ROCKETNET_PLATFORM_MAC;
}

int rocketnet_platform_connection_type()
{
    return connection_type;
}

double rocketnet_platform_time()
{
    uint64_t current = mach_absolute_time();
    return ( (double) ( current - time_start ) ) * ( (double) timebase_info.numer ) / ( (double) timebase_info.denom ) / 1000000000.0;
}

void rocketnet_platform_sleep( double time )
{
    usleep( (int) ( time * 1000000 ) );
}

// ---------------------------------------------------

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket );

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size )
{
    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    rocketnet_platform_socket_t * socket = (rocketnet_platform_socket_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );

    rocketnet_assert( socket );

    socket->context = context;

    socket->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;

    socket->msgx = false;

    socket->msgx_down_until = 0.0;

    // create socket

    socket->handle = ::socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( socket->handle < 0 )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, socket );
        return NULL;
    }

    // setup socket as dual stack ipv4/ipv6

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( socket->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // increase socket send and receive buffer sizes

    if ( setsockopt( socket->handle, SOL_SOCKET, SO_SNDBUF, (char*)( &send_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        return NULL;
    }

    if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &receive_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket receive buffer size" );
        rocketnet_platform_socket_destroy( socket );
        return NULL;
    }

    // bind to port

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( sockaddr_in6 ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( address->data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( address->port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv6)" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        sockaddr_in socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin_family = AF_INET;
        socket_address.sin_addr.s_addr = ( ( (uint32_t) address->data.ipv4[0] ) )      | 
                                         ( ( (uint32_t) address->data.ipv4[1] ) << 8 )  | 
                                         ( ( (uint32_t) address->data.ipv4[2] ) << 16 ) | 
                                         ( ( (uint32_t) address->data.ipv4[3] ) << 24 );
        socket_address.sin_port = rocketnet_platform_htons( address->port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv4)" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // if bound to port 0 find the actual port we got

    if ( address->port == 0 )
    {
        if ( address->type == ROCKETNET_ADDRESS_IPV6 )
        {
            sockaddr_in6 sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv6)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin6_port );
        }
        else
        {
            sockaddr_in sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                rocketnet_error( "failed to get socket port (ipv4)" );
                rocketnet_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = rocketnet_platform_ntohs( sin.sin_port );
        }
    }

    // set non-blocking io and receive timeout

    if ( socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        // non-blocking
        if ( fcntl( socket->handle, F_SETFL, O_NONBLOCK, 1 ) == -1 )
        {
            rocketnet_error( "failed to set socket to non-blocking" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else if ( timeout_seconds > 0.0f )
    {
        // blocking with receive timeout
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (int) ( timeout_seconds * 1000000.0 );
        if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) ) < 0 )
        {
            rocketnet_error( "failed to set socket receive timeout" );
            rocketnet_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        // blocking with no timeout
    }

    // set don't fragment bit

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IPV6, IPV6_DONTFRAG, &val, sizeof(val) );
    }
    else
    {
        int val = 1;
        setsockopt( socket->handle, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val) );
    }

#if TARGET_OS_OSX

    // the batch pump probe: resolved symbols, a non-blocking socket (the
    // pump's spin discipline assumes it), no environment override, and one
    // real recvmsg_x against the just-bound socket -- a live syscall
    // answers EAGAIN (or delivers, if the socket bound a fixed port that
    // already had traffic queued: that consumes one datagram, priced as
    // the UDP loss it is), and anything else (ENOSYS on a kernel that
    // dropped the syscall, EOPNOTSUPP, EINVAL on an ABI drift) leaves the
    // socket on the portable pump. degradation is silent above the seam.

    if ( rocketnet_sendmsg_x && rocketnet_recvmsg_x && socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        const char * pump_override = rocketnet_platform_getenv( "ROCKETNET_PUMP" );
        if ( !pump_override || strcmp( pump_override, "portable" ) != 0 )
        {
            uint8_t probe_data[64];
            struct sockaddr_storage probe_name;
            struct iovec probe_iov;
            probe_iov.iov_base = probe_data;
            probe_iov.iov_len = sizeof(probe_data);
            struct rocketnet_msghdr_x_t probe_msg;
            memset( &probe_msg, 0, sizeof(probe_msg) );
            probe_msg.msg_name = &probe_name;
            probe_msg.msg_namelen = sizeof(probe_name);
            probe_msg.msg_iov = &probe_iov;
            probe_msg.msg_iovlen = 1;

            errno = 0;
            ssize_t probe_result = rocketnet_recvmsg_x( socket->handle, &probe_msg, 1, 0 );
            if ( probe_result >= 0 || errno == EAGAIN || errno == EWOULDBLOCK )
            {
                socket->msgx = true;
            }
            else
            {
                rocketnet_debug( "recvmsg_x probe refused (%s). socket pump is portable", strerror( errno ) );
            }
        }
    }

#endif // #if TARGET_OS_OSX

    return socket;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

    if ( socket->handle != 0 )
    {
        close( socket->handle );
    }
    
    rocketnet_free( socket->context, socket );
}

void rocketnet_platform_socket_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, const void * packet_data, int packet_bytes )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );
    rocketnet_assert( input_to->type == ROCKETNET_ADDRESS_IPV6 || input_to->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    rocketnet_address_t to = *input_to;

    if ( socket->ipv6 )
    {
        // socket is dual stack ipv4 and ipv6

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( to.port );
        int result = int( sendto( socket->handle, (char*)( packet_data ), packet_bytes, 0, (sockaddr*)( &socket_address ), sizeof(sockaddr_in6) ) );
        if ( result < 0 )
        {
            char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            rocketnet_address_to_string( &to, address_string );
            rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
        }
    }
    else
    {
        // socket is ipv4

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            sockaddr_in socket_address;
            memset( &socket_address, 0, sizeof( socket_address ) );
            socket_address.sin_family = AF_INET;
            socket_address.sin_addr.s_addr = ( ( (uint32_t) to.data.ipv4[0] ) )        | 
                                             ( ( (uint32_t) to.data.ipv4[1] ) << 8 )   | 
                                             ( ( (uint32_t) to.data.ipv4[2] ) << 16 )  | 
                                             ( ( (uint32_t) to.data.ipv4[3] ) << 24 );
            socket_address.sin_port = rocketnet_platform_htons( to.port );
            int result = int( sendto( socket->handle, (const char*)( packet_data ), packet_bytes, 0, (sockaddr*)( &socket_address ), sizeof(sockaddr_in) ) );
            if ( result < 0 )
            {
                char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
                rocketnet_address_to_string( &to, address_string );
                rocketnet_debug( "sendto (%s) failed: %s", address_string, strerror( errno ) );
            }
        }
        else
        {
            rocketnet_error( "invalid address. could not send packet" );
        }
    }
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( max_packet_size > 0 );

    sockaddr_storage sockaddr_from;
    socklen_t from_length;

    int result;
    for ( ;; )
    {
        from_length = sizeof( sockaddr_from );

        result = int( recvfrom( socket->handle, (char*) packet_data, max_packet_size, 0, (sockaddr*) &sockaddr_from, &from_length ) );

        if ( result < 0 )
        {
            if ( errno != EAGAIN && errno != EINTR )
            {
                rocketnet_debug( "recvfrom failed with error %d", errno );
            }
            return 0;
        }

        if ( result == 0 )
        {
            // a zero-length datagram is legal UDP carrying nothing the
            // protocol admits -- and returning 0 here would read as
            // "socket drained", letting byte-less chaff cap an entire
            // drain at one packet (#100's review). skip it, keep reading:
            // both pumps share this semantic.
            continue;
        }

        break;
    }

    if ( sockaddr_from.ss_family == AF_INET6 )
    {
        sockaddr_in6 * addr_ipv6 = (sockaddr_in6*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );

        if ( socket->ipv6 && rocketnet_address_is_ipv4_in_ipv6( from ) )
        {
            rocketnet_address_convert_ipv6_to_ipv4( from );
        }
    }
    else if ( sockaddr_from.ss_family == AF_INET )
    {
        sockaddr_in * addr_ipv4 = (sockaddr_in*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV4;
        from->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
        from->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
        from->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
        from->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
        from->port = rocketnet_platform_ntohs( addr_ipv4->sin_port );
    }
    else
    {
        rocketnet_assert( 0 );
        return 0;
    }
  
    rocketnet_assert( result >= 0 );

    return result;
}

// ---------------------------------------------------

// the darwin batch pump (#100). the portable per-packet functions above
// stay the degradation target: every path through these either moves the
// batch through sendmsg_x/recvmsg_x or hands the remainder to the
// per-packet loop, so a packet is never lost to a refusal -- it is sent
// or dropped where the portable pump would have sent or dropped it, with
// one priced exception recorded at the send loop's error path below. on
// iOS the msgx flag is never set and these ARE the portable loop.
//
// a hard refusal (an errno outside UDP's ordinary weather) PAUSES the
// msgx pump on that socket rather than killing it: batch calls run
// portable until the pause expires, and the first batch attempt after
// expiry is itself the re-probe. a transient misclassification therefore
// costs at most one pause window, never the feature (#100's review: a
// permanent degrade hands any single weird errno a kill switch).

#define ROCKETNET_PLATFORM_HAS_BATCH_PUMP 1

#define ROCKETNET_MSGX_RETRY_SECONDS 60.0

// the name reports the LIVE path -- tests and logs treat it as ground
// truth, so a paused pump reads as portable until its pause expires. a
// pure read: expiry itself is cleared by the next batch call's
// rocketnet_msgx_active, never here.

const char * rocketnet_platform_socket_pump( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    if ( !socket->msgx )
        return "portable";
    if ( socket->msgx_down_until > 0.0 && rocketnet_platform_time() < socket->msgx_down_until )
        return "portable";
    return "msgx";
}

// is the msgx pump live for this call? eligibility is decided at create;
// a pause expires here, making the caller's next batch attempt the
// re-probe. main-thread only, like every batch call (the transport's
// phase contract: update and the flushes run on the main thread).

static bool rocketnet_msgx_active( rocketnet_platform_socket_t * socket )
{
    if ( !socket->msgx )
        return false;
    if ( socket->msgx_down_until > 0.0 )
    {
        if ( rocketnet_platform_time() < socket->msgx_down_until )
            return false;
        socket->msgx_down_until = 0.0;
        rocketnet_debug( "msgx pump pause expired. next batch call is the re-probe" );
    }
    return true;
}

static void rocketnet_msgx_pause( rocketnet_platform_socket_t * socket, const char * which, int error )
{
    socket->msgx_down_until = rocketnet_platform_time() + ROCKETNET_MSGX_RETRY_SECONDS;
    rocketnet_warn( "%s refused (%s). msgx pump pauses %g seconds", which, strerror( error ), ROCKETNET_MSGX_RETRY_SECONDS );
}

// UDP's ordinary weather on the send path: errnos the per-packet path
// also produces on a routine bad destination or a full buffer, and which
// therefore must not pause the pump -- the review measured EADDRNOTAVAIL
// from a single forged source port, which under a permanent degrade was
// a one-packet kill switch for the feature.

static bool rocketnet_msgx_send_weather( int error )
{
    return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS || error == ECONNREFUSED ||
           error == EHOSTUNREACH || error == ENETUNREACH || error == EMSGSIZE || error == EADDRNOTAVAIL ||
           error == EPERM || error == EACCES || error == EHOSTDOWN || error == ENETDOWN;
}

// fill a sockaddr for one destination, mirroring send_packet's dual-stack
// conversion. returns the sockaddr length, or 0 for an address the socket
// cannot carry (logged exactly as the per-packet path logs it).

static socklen_t rocketnet_msgx_fill_sockaddr( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, sockaddr_storage * storage )
{
    rocketnet_address_t to = *input_to;

    if ( socket->ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 * socket_address = (sockaddr_in6*) storage;
        memset( socket_address, 0, sizeof(sockaddr_in6) );
        socket_address->sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address->sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address->sin6_port = rocketnet_platform_htons( to.port );
        return sizeof(sockaddr_in6);
    }

    if ( to.type != ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_error( "invalid address. could not send packet" );
        return 0;
    }

    sockaddr_in * socket_address = (sockaddr_in*) storage;
    memset( socket_address, 0, sizeof(sockaddr_in) );
    socket_address->sin_family = AF_INET;
    socket_address->sin_addr.s_addr = ( ( (uint32_t) to.data.ipv4[0] ) )       |
                                      ( ( (uint32_t) to.data.ipv4[1] ) << 8 )  |
                                      ( ( (uint32_t) to.data.ipv4[2] ) << 16 ) |
                                      ( ( (uint32_t) to.data.ipv4[3] ) << 24 );
    socket_address->sin_port = rocketnet_platform_htons( to.port );
    return sizeof(sockaddr_in);
}

// convert one received sockaddr, mirroring receive_packet's mapped-address
// normalization. returns false for a family the transport does not speak.

static bool rocketnet_msgx_read_sockaddr( rocketnet_platform_socket_t * socket, const sockaddr_storage * storage, rocketnet_address_t * from )
{
    if ( storage->ss_family == AF_INET6 )
    {
        const sockaddr_in6 * addr_ipv6 = (const sockaddr_in6*) storage;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (const uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
        if ( socket->ipv6 && rocketnet_address_is_ipv4_in_ipv6( from ) )
        {
            rocketnet_address_convert_ipv6_to_ipv4( from );
        }
        return true;
    }

    if ( storage->ss_family == AF_INET )
    {
        const sockaddr_in * addr_ipv4 = (const sockaddr_in*) storage;
        from->type = ROCKETNET_ADDRESS_IPV4;
        from->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
        from->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
        from->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
        from->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
        from->port = rocketnet_platform_ntohs( addr_ipv4->sin_port );
        return true;
    }

    return false;
}

void rocketnet_platform_socket_send_packets( rocketnet_platform_socket_t * socket, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( sends || num_packets == 0 );

    if ( !rocketnet_msgx_active( socket ) )
    {
        for ( int i = 0; i < num_packets; i++ )
        {
            rocketnet_platform_socket_send_packet( socket, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
        }
        return;
    }

    int base = 0;
    while ( base < num_packets )
    {
        // build one chunk. an address the socket cannot carry is skipped
        // here, already logged by the fill -- the same drop the per-packet
        // path takes. map[] remembers each chunk entry's index in sends,
        // so a mid-chunk degradation can replay the exact unsent tail.

        sockaddr_storage addresses[ROCKETNET_MSGX_BATCH];
        struct iovec iov[ROCKETNET_MSGX_BATCH];
        struct rocketnet_msghdr_x_t msg[ROCKETNET_MSGX_BATCH];
        int map[ROCKETNET_MSGX_BATCH];
        memset( msg, 0, sizeof(msg) );

        int count = 0;
        int scanned = 0;
        while ( base + scanned < num_packets && count < ROCKETNET_MSGX_BATCH )
        {
            const rocketnet_platform_send_t * send = &sends[base + scanned];
            rocketnet_assert( send->to );
            rocketnet_assert( send->packet_data );
            rocketnet_assert( send->packet_bytes > 0 );

            socklen_t sockaddr_bytes = rocketnet_msgx_fill_sockaddr( socket, send->to, &addresses[count] );
            if ( sockaddr_bytes != 0 )
            {
                iov[count].iov_base = (void*) send->packet_data;
                iov[count].iov_len = (size_t) send->packet_bytes;
                msg[count].msg_name = &addresses[count];
                msg[count].msg_namelen = sockaddr_bytes;
                msg[count].msg_iov = &iov[count];
                msg[count].msg_iovlen = 1;
                map[count] = base + scanned;
                count++;
            }
            scanned++;
        }

        int offset = 0;
        while ( offset < count )
        {
            errno = 0;
            ssize_t result = rocketnet_sendmsg_x( socket->handle, msg + offset, (u_int) ( count - offset ), 0 );
            if ( result > 0 )
            {
                offset += (int) result;
                continue;
            }

            if ( errno == EINTR )
                continue;

            // any other -1: the delivered prefix of [offset..count) is
            // INDETERMINATE. xnu can deliver part of a batch and still
            // return -1 with no count -- measured on the Studio: 8 messages
            // with a refused destination at index 5 delivered 0-4 and
            // returned -1 -- so no retry that assumes "nothing was sent"
            // (nor "only the head failed") can be right. the law here: one
            // bounded per-packet pass finishes the chunk. every message in
            // the tail gets exactly one attempt through the per-packet
            // path, which sends or drops it exactly as the portable pump
            // would; a message the kernel had already delivered may go
            // twice. the duplication is priced: at most one duplicate per
            // message per error event (amplification <= 2x, O(count) cost,
            // never O(count^2)), and the protocol's replay protection eats
            // duplicates at the receiver. a weather errno (a routine bad
            // destination, a full buffer) stays on the msgx pump for the
            // next chunk; anything else pauses the pump for
            // ROCKETNET_MSGX_RETRY_SECONDS and this call finishes portable.

            const int error = errno;

            if ( result == 0 && error == 0 )
            {
                // zero messages accepted and no errno: nothing to
                // diagnose, nothing to pause over -- weather. the bounded
                // pass below finishes the chunk (inventing a pause from
                // strerror(0) was #100's review, round two).
                rocketnet_debug( "sendmsg_x accepted nothing. chunk finishes per-packet" );
            }
            else if ( rocketnet_msgx_send_weather( error ) )
            {
                rocketnet_debug( "sendmsg_x error (%s). chunk finishes per-packet", strerror( error ) );
            }
            else
            {
                rocketnet_msgx_pause( socket, "sendmsg_x", error );
            }

            for ( int i = offset; i < count; i++ )
            {
                const rocketnet_platform_send_t * send = &sends[map[i]];
                rocketnet_platform_socket_send_packet( socket, send->to, send->packet_data, send->packet_bytes );
            }
            offset = count;
        }

        base += scanned;

        // a pause landed mid-call: the rest of this batch goes per-packet

        if ( !rocketnet_msgx_active( socket ) )
        {
            for ( int i = base; i < num_packets; i++ )
            {
                rocketnet_platform_socket_send_packet( socket, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
            }
            return;
        }
    }
}

int rocketnet_platform_socket_receive_packets( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_stride > 0 );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( max_packets > 0 );

    int total = 0;

    if ( !rocketnet_msgx_active( socket ) )
        goto per_packet;

    while ( total < max_packets )
    {
        int want = max_packets - total;
        if ( want > ROCKETNET_MSGX_BATCH )
            want = ROCKETNET_MSGX_BATCH;

        const int chunk_base = total;

        sockaddr_storage addresses[ROCKETNET_MSGX_BATCH];
        struct iovec iov[ROCKETNET_MSGX_BATCH];
        struct rocketnet_msghdr_x_t msg[ROCKETNET_MSGX_BATCH];
        memset( msg, 0, sizeof(msg) );
        for ( int i = 0; i < want; i++ )
        {
            iov[i].iov_base = packet_data + (size_t) ( chunk_base + i ) * (size_t) packet_stride;
            iov[i].iov_len = (size_t) packet_stride;
            msg[i].msg_name = &addresses[i];
            msg[i].msg_namelen = sizeof(addresses[i]);
            msg[i].msg_iov = &iov[i];
            msg[i].msg_iovlen = 1;
        }

        errno = 0;
        ssize_t result = rocketnet_recvmsg_x( socket->handle, msg, (u_int) want, 0 );
        if ( result < 0 )
        {
            if ( errno == EINTR )
                continue;
            if ( errno == EAGAIN || errno == EWOULDBLOCK )
                return total;

            // outside ordinary weather: pause the pump and finish this
            // call per-packet -- what earlier chunks drained is already
            // delivered, nothing about this call's contract changes.

            rocketnet_msgx_pause( socket, "recvmsg_x", errno );
            goto per_packet;
        }

        if ( result == 0 )
            return total;

        // walk the chunk in place. a zero-length datagram or a family the
        // transport does not speak (the per-packet path delivers neither)
        // is compacted out: the next kept message's bytes move down to the
        // gap. rare by nature -- attack chaff -- and bounded by the chunk.

        for ( ssize_t i = 0; i < result; i++ )
        {
            int bytes = (int) msg[i].msg_datalen;
            rocketnet_address_t parsed;
            if ( bytes <= 0 || !rocketnet_msgx_read_sockaddr( socket, &addresses[i], &parsed ) )
                continue;
            uint8_t * written = packet_data + (size_t) ( chunk_base + (int) i ) * (size_t) packet_stride;
            uint8_t * target = packet_data + (size_t) total * (size_t) packet_stride;
            if ( target != written )
            {
                memmove( target, written, (size_t) bytes );
            }
            from[total] = parsed;
            packet_bytes[total] = bytes;
            total++;
        }

        if ( result < want )
            return total;
    }

    return total;

per_packet:

    while ( total < max_packets )
    {
        int bytes = rocketnet_platform_socket_receive_packet( socket, &from[total], packet_data + (size_t) total * (size_t) packet_stride, packet_stride );
        if ( bytes == 0 )
            break;
        packet_bytes[total] = bytes;
        total++;
    }
    return total;
}

// ---------------------------------------------------

struct thread_shim_data_t
{
    void * context;
    void * real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function;
};

static void * thread_function_shim( void * data )
{
    rocketnet_assert( data );
    thread_shim_data_t * shim_data = (thread_shim_data_t*) data;
    void * context = shim_data->context;
    void * real_thread_data = shim_data->real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function = shim_data->real_thread_function;
    rocketnet_free( context, data );
    real_thread_function( real_thread_data );
    return NULL;
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg )
{
    rocketnet_platform_thread_t * thread = (rocketnet_platform_thread_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_thread_t) );

    rocketnet_assert( thread );

    thread->context = context;

    thread_shim_data_t * shim_data = (thread_shim_data_t*) rocketnet_malloc( context, sizeof(thread_shim_data_t) );
    rocketnet_assert( shim_data );
    if ( !shim_data )
    {
        rocketnet_free( context, thread );
        return NULL;
    }
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;

    if ( pthread_create( &thread->handle, NULL, thread_function_shim, shim_data ) != 0 )
    {
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
        return NULL;
    }

    return thread;
}

void rocketnet_platform_thread_join( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    pthread_join( thread->handle, NULL );
}

void rocketnet_platform_thread_destroy( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    rocketnet_free( thread->context, thread );
}

void rocketnet_platform_client_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you are developing for mac or ios you can adjust the affinity and priority for the client thread here.
    struct sched_param param;
    param.sched_priority = sched_get_priority_min( SCHED_RR );
    pthread_setschedparam( thread->handle, SCHED_RR, &param );
}

void rocketnet_platform_server_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you are developing for mac or ios you can adjust the affinity and priority for the server thread here.
    struct sched_param param;
    param.sched_priority = sched_get_priority_min( SCHED_RR );
    pthread_setschedparam( thread->handle, SCHED_RR, &param );
}

// ---------------------------------------------------

bool rocketnet_platform_mutex_create( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );

    memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype( &attr, 0 );
    int result = pthread_mutex_init( &mutex->handle, &attr );
    pthread_mutexattr_destroy( &attr );

    if ( result != 0 )
    {
        return false;
    }

    mutex->ok = true;

    return true;
}

void rocketnet_platform_mutex_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    pthread_mutex_lock( &mutex->handle );
}

void rocketnet_platform_mutex_release( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    pthread_mutex_unlock( &mutex->handle );
}

void rocketnet_platform_mutex_destroy( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    if ( mutex->ok )
    {
        pthread_mutex_destroy( &mutex->handle );
        memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );
    }
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// windows

#include "rocketnet_platform.h"
#include "rocketnet_address.h"

#if ROCKETNET_UNREAL_ENGINE
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/PreWindowsApi.h"
#endif // #if ROCKETNET_UNREAL_ENGINE

#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#if !ROCKETNET_UNREAL_ENGINE
#include <windows.h>
#else // #if !ROCKETNET_UNREAL_ENGINE
#include "Windows/MinWindows.h"
#endif // #if !ROCKETNET_UNREAL_ENGINE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <malloc.h>
#include <wininet.h>
#include <iphlpapi.h>
#include <qos2.h>

#pragma comment( lib, "WS2_32.lib" )
#pragma comment( lib, "IPHLPAPI.lib" )
#pragma comment( lib, "Qwave.lib" )

#ifdef SetPort
#undef SetPort
#endif // #ifdef SetPort

extern void * rocketnet_malloc( void * context, size_t bytes );

extern void rocketnet_free( void * context, void * p );

static int get_connection_type();

static int timer_initialized = 0;
static LARGE_INTEGER timer_frequency;
static LARGE_INTEGER timer_start;
static int connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

// init

bool rocketnet_platform_init()
{
    QueryPerformanceFrequency( &timer_frequency );
    QueryPerformanceCounter( &timer_start );

    WSADATA WsaData;
    if ( WSAStartup( MAKEWORD(2,2), &WsaData ) != NO_ERROR )
    {
        rocketnet_error( "WSAStartup failed" );
        return false;
    }

    connection_type = get_connection_type();

    return true;
}

void rocketnet_platform_term()
{
    WSACleanup();
}

int rocketnet_platform_connection_type()
{
    return connection_type;
}

const char * rocketnet_platform_getenv( const char * var )
{
    return getenv( var );
}

// threads

struct thread_shim_data_t
{
    void * context;
    void * real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function;
};

static DWORD WINAPI thread_function_shim( void * data )
{
    rocketnet_assert( data );
    thread_shim_data_t * shim_data = (thread_shim_data_t*) data;
    void * context = shim_data->context;
    void * real_thread_data = shim_data->real_thread_data;
    rocketnet_platform_thread_func_t real_thread_function = shim_data->real_thread_function;
    rocketnet_free( context, data );
    real_thread_function( real_thread_data );
    return 0;
}

rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t thread_function, void * arg )
{
    rocketnet_platform_thread_t * thread = (rocketnet_platform_thread_t*) rocketnet_malloc( context, sizeof( rocketnet_platform_thread_t ) );

    rocketnet_assert( thread );

    thread->context = context;

    thread_shim_data_t * shim_data = (thread_shim_data_t*) rocketnet_malloc( context, sizeof(thread_shim_data_t) );
    rocketnet_assert( shim_data );
    if ( !shim_data )
    {
        rocketnet_free( context, thread );
        return NULL;
    }
    shim_data->context = context;
    shim_data->real_thread_function = thread_function;
    shim_data->real_thread_data = arg;

    thread->handle = CreateThread(NULL, 0, thread_function_shim, shim_data, 0, NULL);

    if ( thread->handle == NULL )
    {
        rocketnet_free( context, thread );
        rocketnet_free( context, shim_data );
        return NULL;
    }

    return thread;
}

void rocketnet_platform_thread_join( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    WaitForSingleObject( thread->handle, INFINITE );
}

void rocketnet_platform_thread_destroy( rocketnet_platform_thread_t * thread )
{
    rocketnet_assert( thread );
    rocketnet_free( thread->context, thread );
}

void rocketnet_platform_client_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If your client runs on windows, you can set the thread priority and affinity of the client thread here
    rocketnet_assert( thread );
    SetThreadPriority( thread->handle, THREAD_PRIORITY_TIME_CRITICAL );
}

void rocketnet_platform_server_thread_priority( rocketnet_platform_thread_t * thread )
{
    // IMPORTANT: If you have a server that runs on windows, you can set the priority and affinity of the server thread here
    rocketnet_assert( thread );
    SetThreadPriority( thread->handle, THREAD_PRIORITY_TIME_CRITICAL );
}

bool rocketnet_platform_mutex_create( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );

    memset( mutex, 0, sizeof(rocketnet_platform_mutex_t) );

    if ( !InitializeCriticalSectionAndSpinCount( (LPCRITICAL_SECTION)&mutex->handle, 0xFF ) )
    {
        return false;
    }

    mutex->ok = true;

    return true;
}

void rocketnet_platform_mutex_acquire( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    EnterCriticalSection( (LPCRITICAL_SECTION)&mutex->handle );
}

void rocketnet_platform_mutex_release( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    rocketnet_assert( mutex->ok );
    LeaveCriticalSection( (LPCRITICAL_SECTION)&mutex->handle );
}

void rocketnet_platform_mutex_destroy( rocketnet_platform_mutex_t * mutex )
{
    rocketnet_assert( mutex );
    if ( mutex->ok )
    {
        DeleteCriticalSection( (LPCRITICAL_SECTION)&mutex->handle );
        memset(mutex, 0, sizeof(rocketnet_platform_mutex_t));
    }
}

// time

void rocketnet_platform_sleep( double time )
{
    const int milliseconds = (int) ( time * 1000 );
    Sleep( milliseconds );
}

double rocketnet_platform_time()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    return ( (double) ( now.QuadPart - timer_start.QuadPart ) ) / ( (double) ( timer_frequency.QuadPart ) );
}

// sockets

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;
    *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
    return inet_ntop( AF_INET6, (void*)address, address_string, address_string_size ) != NULL;
}

bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, rocketnet_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof(hints) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                sockaddr_in6 * addr_ipv6 = (sockaddr_in6 *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV6;
                for ( int i = 0; i < 8; ++i )
                {
                    address->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
                }
                address->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                sockaddr_in * addr_ipv4 = (sockaddr_in *)( result->ai_addr );
                address->type = ROCKETNET_ADDRESS_IPV4;
                address->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
                address->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
                address->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
                address->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
                address->port = rocketnet_platform_ntohs( addr_ipv4->sin_port );
                freeaddrinfo( result );
                return true;
            }
            else
            {
                rocketnet_assert( 0 );
                freeaddrinfo( result );
                return false;
            }
        }
    }

    return false;
}

uint16_t rocketnet_platform_preferred_client_port()
{
    return 0;
}

bool rocketnet_platform_client_dual_stack()
{
    return true;
}

int rocketnet_platform_id()
{
    return ROCKETNET_PLATFORM_WINDOWS;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * );

int rocketnet_set_socket_codepoint( SOCKET socket, QOS_TRAFFIC_TYPE trafficType, QOS_FLOWID flowId, PSOCKADDR addr ) 
{
    QOS_VERSION QosVersion = { 1 , 0 };
    HANDLE qosHandle;
    if ( QOSCreateHandle( &QosVersion, &qosHandle ) == FALSE )
    {
        return GetLastError();
    }
    if ( QOSAddSocketToFlow( qosHandle, socket, addr, trafficType, QOS_NON_ADAPTIVE_FLOW, &flowId ) == FALSE )
    {
        return GetLastError();
    }
    return 0;
}

rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size )
{
    rocketnet_platform_socket_t * s = (rocketnet_platform_socket_t *) rocketnet_malloc( context, sizeof( rocketnet_platform_socket_t ) );

    rocketnet_assert( s );

    s->context = context;

    rocketnet_assert( address );
    rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );

    // create socket

    s->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;

    s->handle = socket( ( address->type == ROCKETNET_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( s->handle == INVALID_SOCKET )
    {
        rocketnet_error( "failed to create socket" );
        rocketnet_free( context, s );
        return NULL;
    }

    // IMPORTANT: tell windows we don't want to receive any connection reset messages
    // for this socket, otherwise recvfrom errors out when client sockets disconnect hard
    // in response to ICMP messages.
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    WSAIoctl( s->handle, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL );

    // set ipv6 sockets as dual stack

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int yes = 0;
        if ( setsockopt( s->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)( &yes ), sizeof( yes ) ) != 0 )
        {
            rocketnet_error( "failed to clear socket ipv6 only" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }

    // increase socket send and receive buffer sizes

    if ( setsockopt( s->handle, SOL_SOCKET, SO_SNDBUF, (char*)( &send_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket send buffer size" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    if ( setsockopt( s->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &receive_buffer_size ), sizeof( int ) ) != 0 )
    {
        rocketnet_error( "failed to set socket receive buffer size" );
        rocketnet_platform_socket_destroy( s );
        return NULL;
    }

    // bind to port

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( sockaddr_in6 ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = rocketnet_platform_htons( address->data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( address->port );

        if ( bind( s->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv6)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }
    else
    {
        sockaddr_in socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin_family = AF_INET;
        socket_address.sin_addr.s_addr = ( ( (uint32_t) address->data.ipv4[0] ) )      | 
                                         ( ( (uint32_t) address->data.ipv4[1] ) << 8 )  | 
                                         ( ( (uint32_t) address->data.ipv4[2] ) << 16 ) | 
                                         ( ( (uint32_t) address->data.ipv4[3] ) << 24 );
        socket_address.sin_port = rocketnet_platform_htons( address->port );

        if ( bind( s->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            rocketnet_error( "failed to bind socket (ipv4)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }

    // if bound to port 0 find the actual port we got

    sockaddr_in sin4;
    sockaddr_in6 sin6;
    sockaddr * addr = NULL;

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        addr = (sockaddr*) &sin6;
        socklen_t len = sizeof( sin6 );
        if ( getsockname( s->handle, addr, &len ) == -1 )
        {
            rocketnet_error( "failed to get socket address (ipv6)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
        address->port = rocketnet_platform_ntohs( sin6.sin6_port );
    }
    else
    {
        addr = (sockaddr*) &sin4;
        socklen_t len = sizeof( sin4 );
        if ( getsockname( s->handle, addr, &len ) == -1 )
        {
            rocketnet_error( "failed to get socket address (ipv4)" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
        address->port = rocketnet_platform_ntohs( sin4.sin_port );
    }

    // set non-blocking io

    if ( socket_type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING )
    {
        DWORD nonBlocking = 1;
        if ( ioctlsocket( s->handle, FIONBIO, &nonBlocking ) != 0 )
        {
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }
    else if ( timeout_seconds > 0.0f )
    {
        // set receive timeout
        DWORD tv = DWORD( timeout_seconds * 1000.0f );
        if ( setsockopt( s->handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)( &tv ), sizeof( tv ) ) < 0 )
        {
            rocketnet_error( "failed to set socket receive timeout" );
            rocketnet_platform_socket_destroy( s );
            return NULL;
        }
    }
    else
    {
        // timeout < 0, socket is blocking with no timeout
    }

    // set don't fragment bit

    if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        int val = 1;
        setsockopt( s->handle, IPPROTO_IPV6, IPV6_DONTFRAG, (const char*) &val, sizeof(val) );
    }
    else
    {
        int val = 1;
        setsockopt( s->handle, IPPROTO_IP, IP_DONTFRAGMENT, (const char*) &val, sizeof(val) );
    }    

    return s;
}

void rocketnet_platform_socket_destroy( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );

    if ( socket->handle != 0 )
    {
        closesocket( socket->handle );
        socket->handle = 0;
    }

    rocketnet_free( socket->context, socket );
}

void rocketnet_platform_socket_send_packet( rocketnet_platform_socket_t * socket, const rocketnet_address_t * input_to, const void * packet_data, int packet_bytes )
{
    rocketnet_assert( socket );
    rocketnet_assert( input_to );
    rocketnet_assert( input_to->type == ROCKETNET_ADDRESS_IPV6 || input_to->type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );

    rocketnet_address_t to = *input_to;

    if ( socket->ipv6 )
    {
        // socket is dual stack ipv4 and ipv6

        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof(socket_address) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; i++ )
        {
            ( (uint16_t*) &socket_address.sin6_addr)[i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        socket_address.sin6_port = rocketnet_platform_htons( to.port );
        int result = sendto( socket->handle, (char*)(packet_data), packet_bytes, 0, (sockaddr*)(&socket_address), sizeof(sockaddr_in6) );
        if ( result < 0 )
        {
            char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            rocketnet_address_to_string( &to, address_string );
            char error_string[256] = { 0 };
            strerror_s( error_string, sizeof(error_string), errno );
            rocketnet_debug( "sendto (%s) failed: %s", address_string, error_string );
        }
    }
    else
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            sockaddr_in socket_address;
            memset( &socket_address, 0, sizeof(socket_address) );
            socket_address.sin_family = AF_INET;
            socket_address.sin_addr.s_addr = (((uint32_t)to.data.ipv4[0])) |
                (((uint32_t)to.data.ipv4[1]) << 8) |
                (((uint32_t)to.data.ipv4[2]) << 16) |
                (((uint32_t)to.data.ipv4[3]) << 24);
            socket_address.sin_port = rocketnet_platform_htons( to.port );
            int result = sendto( socket->handle, (const char*)(packet_data), packet_bytes, 0, (sockaddr*)(&socket_address), sizeof(sockaddr_in) );
            if ( result < 0 )
            {
                char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
                rocketnet_address_to_string( &to, address_string );
                char error_string[256] = { 0 };
                strerror_s( error_string, sizeof(error_string), errno );
                rocketnet_debug( "sendto (%s) failed: %s", address_string, error_string );
            }
        }
        else
        {
            rocketnet_error( "invalid address type. could not send packet" );
        }
    }
}

int rocketnet_platform_socket_receive_packet( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, void * packet_data, int max_packet_size )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( max_packet_size > 0 );

    typedef int socklen_t;

    sockaddr_storage sockaddr_from;
    socklen_t from_length;

    int result;
    for ( ;; )
    {
        from_length = sizeof( sockaddr_from );

        result = recvfrom( socket->handle, (char*) packet_data, max_packet_size, 0, (sockaddr*) &sockaddr_from, &from_length );

        if ( result == SOCKET_ERROR )
        {
            int error = WSAGetLastError();

            if ( error != WSAEWOULDBLOCK && error != WSAETIMEDOUT && error != WSAECONNRESET )
            {
                rocketnet_debug( "recvfrom failed with error %d", error );
            }
            return 0;
        }

        if ( result == 0 )
        {
            // a zero-length datagram is legal UDP carrying nothing the
            // protocol admits -- and returning 0 here would read as
            // "socket drained", letting byte-less chaff cap an entire
            // drain at one packet (#100's review). skip it, keep reading:
            // both pumps share this semantic.
            continue;
        }

        break;
    }

    if ( sockaddr_from.ss_family == AF_INET6 )
    {
        sockaddr_in6 * addr_ipv6 = (sockaddr_in6*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = rocketnet_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = rocketnet_platform_ntohs( addr_ipv6->sin6_port );

        if (socket->ipv6 && rocketnet_address_is_ipv4_in_ipv6( from ) )
        {
            rocketnet_address_convert_ipv6_to_ipv4( from );
        }
    }
    else if ( sockaddr_from.ss_family == AF_INET )
    {
        sockaddr_in * addr_ipv4 = (sockaddr_in*) &sockaddr_from;
        from->type = ROCKETNET_ADDRESS_IPV4;
        from->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
        from->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
        from->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
        from->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
        from->port = rocketnet_platform_ntohs( addr_ipv4->sin_port );
    }
    else
    {
        rocketnet_assert( 0 );
        return 0;
    }
  
    rocketnet_assert( result >= 0 );

    return result;
}

extern void * rocketnet_global_context;

static int get_connection_type()
{
    IP_ADAPTER_ADDRESSES * addresses;
    ULONG buffer_size = 15000;

    do
    {
        addresses = (IP_ADAPTER_ADDRESSES *)( rocketnet_malloc( rocketnet_global_context, buffer_size ) );

        ULONG return_code = GetAdaptersAddresses( AF_INET, 0, NULL, addresses, &buffer_size );

        if ( return_code == NO_ERROR )
        {
            // success!
            break;
        }
        else if ( return_code == ERROR_BUFFER_OVERFLOW )
        {
            rocketnet_free( rocketnet_global_context, addresses );
            continue;
        }
        else
        {
            // error
            rocketnet_free( rocketnet_global_context, addresses );
            return ROCKETNET_CONNECTION_TYPE_UNKNOWN;
        }
    }
    while ( true );

    int result = ROCKETNET_CONNECTION_TYPE_UNKNOWN;
    
    // if there are any adapters at all, default to wired
    if ( addresses )
    {
        result = ROCKETNET_CONNECTION_TYPE_WIRED;
    }

    // if any wifi adapter exists and is connected to a network, assume we're on wifi.
    IP_ADAPTER_ADDRESSES * address = addresses;
    while ( address )
    {
        if ( address->IfType == IF_TYPE_IEEE80211 && address->OperStatus == NET_IF_OPER_STATUS_UP )
        {
            result = ROCKETNET_CONNECTION_TYPE_WIFI;
            break;
        }
        address = address->Next;
    }

    if ( addresses )
    {
        rocketnet_free( rocketnet_global_context, addresses );
    }

    return result;
}

#if ROCKETNET_UNREAL_ENGINE
#include "Windows/PostWindowsApi.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif // #if ROCKETNET_UNREAL_ENGINE

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// ----------------------------------------------------------------

// the batch pump seam's portable implementation (#100): a platform without
// a batch override loops the per-packet calls, so every platform serves the
// seam on day zero (#81's baseline doctrine) and a call site never carries
// an #ifdef. a platform that defines ROCKETNET_PLATFORM_HAS_BATCH_PUMP in
// its own section above supplies all three functions itself.

#if !defined( ROCKETNET_PLATFORM_HAS_BATCH_PUMP )

const char * rocketnet_platform_socket_pump( rocketnet_platform_socket_t * socket )
{
    rocketnet_assert( socket );
    (void) socket;
    return "portable";
}

void rocketnet_platform_socket_send_packets( rocketnet_platform_socket_t * socket, const rocketnet_platform_send_t * sends, int num_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( sends || num_packets == 0 );

    for ( int i = 0; i < num_packets; i++ )
    {
        rocketnet_platform_socket_send_packet( socket, sends[i].to, sends[i].packet_data, sends[i].packet_bytes );
    }
}

int rocketnet_platform_socket_receive_packets( rocketnet_platform_socket_t * socket, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_assert( socket );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_stride > 0 );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( max_packets > 0 );

    int num_packets = 0;
    while ( num_packets < max_packets )
    {
        int bytes = rocketnet_platform_socket_receive_packet( socket, &from[num_packets], packet_data + (size_t) num_packets * (size_t) packet_stride, packet_stride );
        if ( bytes == 0 )
            break;
        packet_bytes[num_packets] = bytes;
        num_packets++;
    }
    return num_packets;
}

#endif // #if !defined( ROCKETNET_PLATFORM_HAS_BATCH_PUMP )
