/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// linux

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
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <linux/wireless.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <alloca.h>

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
    return ROCKETNET_PLATFORM_LINUX;
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
    socklen_t from_length = sizeof( sockaddr_from );

    int result = int( recvfrom( socket->handle, (char*) packet_data, max_packet_size, socket->type == ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING ? MSG_DONTWAIT : 0, (sockaddr*) &sockaddr_from, &from_length ) );

    if ( result <= 0 )
    {
        if ( errno == EAGAIN || errno == EINTR )
        {
            return 0;
        }

        rocketnet_debug( "recvfrom failed with error %d", errno );
        
        return 0;
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

extern void * rocketnet_malloc( void * context, size_t bytes );

extern void rocketnet_free( void * context, void * p );

// ---------------------------------------------------

static int connection_type = ROCKETNET_CONNECTION_TYPE_UNKNOWN;

static mach_timebase_info_data_t timebase_info;

static uint64_t time_start;

bool rocketnet_platform_init()
{
    mach_timebase_info( &timebase_info );

    time_start = mach_absolute_time();

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
    socklen_t from_length = sizeof( sockaddr_from );

    int result = int( recvfrom( socket->handle, (char*) packet_data, max_packet_size, 0, (sockaddr*) &sockaddr_from, &from_length ) );

    if ( result <= 0 )
    {
        if ( errno == EAGAIN || errno == EINTR )
        {
            return 0;
        }

        rocketnet_debug( "recvfrom failed with error %d", errno );
        
        return 0;
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
    socklen_t from_length = sizeof( sockaddr_from );

    int result = recvfrom( socket->handle, (char*) packet_data, max_packet_size, 0, (sockaddr*) &sockaddr_from, &from_length );

    if ( result == SOCKET_ERROR )
    {
        int error = WSAGetLastError();

        if ( error == WSAEWOULDBLOCK || error == WSAETIMEDOUT || error == WSAECONNRESET )
            return 0;

        rocketnet_debug( "recvfrom failed with error %d", error );

        return 0;
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
