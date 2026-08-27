/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include "rocketnet.h"

#define ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING       0
#define ROCKETNET_PLATFORM_SOCKET_BLOCKING           1

#define ROCKETNET_MUTEX_BYTES                      256

struct rocketnet_address_t;

typedef void (*rocketnet_platform_thread_func_t)(void*);

// ----------------------------------------------------------------

ROCKETNET_EXPORT_FUNC bool rocketnet_platform_init();

ROCKETNET_EXPORT_FUNC void rocketnet_platform_term();

// ----------------------------------------------------------------

ROCKETNET_EXPORT_FUNC int rocketnet_platform_id();

ROCKETNET_EXPORT_FUNC int rocketnet_platform_connection_type();

ROCKETNET_EXPORT_FUNC double rocketnet_platform_time();

ROCKETNET_EXPORT_FUNC void rocketnet_platform_sleep( double time );

ROCKETNET_EXPORT_FUNC const char * rocketnet_platform_getenv( const char * var );

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_platform_ntohs( uint16_t in );

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_platform_htons( uint16_t in );

ROCKETNET_EXPORT_FUNC bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out );

ROCKETNET_EXPORT_FUNC bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out );

ROCKETNET_EXPORT_FUNC bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size );

ROCKETNET_EXPORT_FUNC bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, struct rocketnet_address_t * address );

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_platform_preferred_client_port();

ROCKETNET_EXPORT_FUNC bool rocketnet_platform_client_dual_stack();

// ----------------------------------------------------------------

ROCKETNET_EXPORT_FUNC struct rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_socket_destroy( struct rocketnet_platform_socket_t * socket );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_socket_send_packet( struct rocketnet_platform_socket_t * socket, const struct rocketnet_address_t * to, const void * packet_data, int packet_bytes );

ROCKETNET_EXPORT_FUNC int rocketnet_platform_socket_receive_packet( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, void * packet_data, int max_packet_size );

// ----------------------------------------------------------------

ROCKETNET_EXPORT_FUNC struct rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t func, void * arg );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_thread_join( struct rocketnet_platform_thread_t * thread );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_thread_destroy( struct rocketnet_platform_thread_t * thread );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_client_thread_priority( struct rocketnet_platform_thread_t * thread );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_server_thread_priority( struct rocketnet_platform_thread_t * thread );

// ----------------------------------------------------------------

ROCKETNET_EXPORT_FUNC bool rocketnet_platform_mutex_create( struct rocketnet_platform_mutex_t * mutex );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_mutex_destroy( struct rocketnet_platform_mutex_t * mutex );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_mutex_acquire( struct rocketnet_platform_mutex_t * mutex );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_mutex_release( struct rocketnet_platform_mutex_t * mutex );

#ifdef __cplusplus

struct rocketnet_platform_mutex_helper_t
{
    rocketnet_platform_mutex_t * mutex;
#if ROCKETNET_SPIKE_TRACKING
    const char * file;
    int line;
    double start_time;
    rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * mutex, const char * file, int line );
#else // #if ROCKETNET_SPIKE_TRACKING
    rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * mutex );
#endif // #if ROCKETNET_SPIKE_TRACKING
    ~rocketnet_platform_mutex_helper_t();
};

#if ROCKETNET_SPIKE_TRACKING
#define rocketnet_platform_mutex_guard( _mutex ) rocketnet_platform_mutex_helper_t __mutex_helper( _mutex, __FILE__, __LINE__ )
#else // #if ROCKETNET_SPIKE_TRACKING
#define rocketnet_platform_mutex_guard( _mutex ) rocketnet_platform_mutex_helper_t __mutex_helper( _mutex )
#endif // #if ROCKETNET_SPIKE_TRACKING

#if ROCKETNET_SPIKE_TRACKING
inline rocketnet_platform_mutex_helper_t::rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * mutex, const char * file, int line ) : mutex(mutex), file(file), line(line), start_time(rocketnet_platform_time())
#else // #if ROCKETNET_SPIKE_TRACKING
inline rocketnet_platform_mutex_helper_t::rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * mutex ) : mutex(mutex)
#endif // #if ROCKETNET_SPIKE_TRACKING
{
    rocketnet_assert( mutex );

#if ROCKETNET_SPIKE_TRACKING
    if ( !mutex->ok )
    {
        rocketnet_error( "bad mutex - %s:%d", file, line );
    }
#endif // #if ROCKETNET_SPIKE_TRACKING

    rocketnet_platform_mutex_acquire( mutex );
}

inline rocketnet_platform_mutex_helper_t::~rocketnet_platform_mutex_helper_t()
{
    rocketnet_assert( mutex );
    rocketnet_platform_mutex_release( mutex );
#if ROCKETNET_SPIKE_TRACKING
    double finish_time = rocketnet_platform_time();
    double mutex_duration = finish_time - start_time;
    if ( mutex_duration > 0.001 )
    {
        rocketnet_warn( "mutex spike %.2f milliseconds at %s:%d", mutex_duration, file, line );
    }
#endif // #if ROCKETNET_SPIKE_TRACKING
    mutex = NULL;
}

#endif // __cplusplus

// ----------------------------------------------------------------

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    // linux

    #include <pthread.h>
    #include <unistd.h>
    #include <sched.h>

    typedef int rocketnet_platform_socket_handle_t;

    struct rocketnet_platform_socket_t
    {
        void * context;
        int type;
        bool ipv6;
        rocketnet_platform_socket_handle_t handle;
    };

    struct rocketnet_platform_thread_t
    {
        void * context;
        pthread_t handle;
    };

    struct rocketnet_platform_mutex_t
    {
        bool ok;
        pthread_mutex_t handle;
    };

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    // macos and ios

    #include <pthread.h>
    #include <unistd.h>

    typedef int rocketnet_platform_socket_handle_t;

    struct rocketnet_platform_socket_t
    {
        void * context;
        bool ipv6;
        rocketnet_platform_socket_handle_t handle;
    };

    struct rocketnet_platform_thread_t
    {
        void * context;
        pthread_t handle;
    };

    struct rocketnet_platform_mutex_t
    {
        bool ok;
        pthread_mutex_t handle;
    };

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    // windows

    #if ROCKETNET_UNREAL_ENGINE
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include "Windows/PreWindowsApi.h"
    #endif // #if ROCKETNET_UNREAL_ENGINE

    #if !ROCKETNET_UNREAL_ENGINE
    #define _WINSOCKAPI_
    #include <windows.h>
    #include <winsock2.h>
    #else // #if !ROCKETNET_UNREAL_ENGINE
    #include "Windows/MinWindows.h"
    #endif // #if !ROCKETNET_UNREAL_ENGINE

    #pragma warning(disable:4996)

    #if _WIN64
    typedef uint64_t rocketnet_platform_socket_handle_t;
    #else
    typedef _W64 unsigned int rocketnet_platform_socket_handle_t;
    #endif

    struct rocketnet_platform_socket_t
    {
        void * context;
        bool ipv6;
        rocketnet_platform_socket_handle_t handle;
    };

    struct rocketnet_platform_thread_t
    {
        void * context;
        HANDLE handle;
    };

    struct rocketnet_platform_mutex_t
    {
        bool ok;
        CRITICAL_SECTION handle;
    };

    #if ROCKETNET_UNREAL_ENGINE
    #include "Windows/PostWindowsApi.h"
    #include "Windows/HideWindowsPlatformTypes.h"
    #endif // #if ROCKETNET_UNREAL_ENGINE

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// ----------------------------------------------------------------

// The fill-phase atomics: the send path lets any number of game threads
// acquire and seal packets between update and the flush, and these two are
// the only shared-word operations that phase needs -- a sequence taken with
// an add, and a timestamp published with a store. Relaxed ordering is
// correct for both: each value is meaningful alone and nothing is ordered
// behind it.

#include <string.h>

#if defined(_MSC_VER)

    #include <intrin.h>

    inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
    {
        return (uint64_t) _InterlockedExchangeAdd64( (volatile long long *) value, (long long) add );
    }

    inline void rocketnet_platform_atomic_store_double( volatile double * value, double store )
    {
        long long bits;
        memcpy( &bits, &store, 8 );
    #if defined(_M_IX86)
        // 32-bit x86 has no _InterlockedExchange64; emulate with a CAS loop
        long long expected = *(volatile long long *) value;
        while ( _InterlockedCompareExchange64( (volatile long long *) value, bits, expected ) != expected )
            expected = *(volatile long long *) value;
    #else // #if defined(_M_IX86)
        _InterlockedExchange64( (volatile long long *) value, bits );
    #endif // #if defined(_M_IX86)
    }

#else // #if defined(_MSC_VER)

    inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
    {
        return __atomic_fetch_add( value, add, __ATOMIC_RELAXED );
    }

    inline void rocketnet_platform_atomic_store_double( volatile double * value, double store )
    {
        uint64_t bits;
        memcpy( &bits, &store, 8 );
        __atomic_store_n( (volatile uint64_t *) value, bits, __ATOMIC_RELAXED );
    }

#endif // #if defined(_MSC_VER)

// ----------------------------------------------------------------
