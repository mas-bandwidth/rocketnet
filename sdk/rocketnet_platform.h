/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include "rocketnet.h"

#define ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING       0
#define ROCKETNET_PLATFORM_SOCKET_BLOCKING           1

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

// pump-request create (#106): the driver ladder's entry into pump
// selection. pump NULL/"auto" behaves exactly like the plain create above
// (probe, honoring ROCKETNET_PUMP); "portable" forces the portable pump;
// "msgx" requires the darwin batch pump or returns NULL loudly.

ROCKETNET_EXPORT_FUNC struct rocketnet_platform_socket_t * rocketnet_platform_socket_create_with_pump( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, const char * pump );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_socket_destroy( struct rocketnet_platform_socket_t * socket );

ROCKETNET_EXPORT_FUNC void rocketnet_platform_socket_send_packet( struct rocketnet_platform_socket_t * socket, const struct rocketnet_address_t * to, const void * packet_data, int packet_bytes );

ROCKETNET_EXPORT_FUNC int rocketnet_platform_socket_receive_packet( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, void * packet_data, int max_packet_size );

// ----------------------------------------------------------------

// The batch pump seam (#100, per #81's matrix): move a batch of packets in
// as few syscalls as the platform allows. Every platform implements these --
// the portable implementation loops the per-packet calls above, so the
// baseline doctrine holds and a call site never carries an #ifdef -- and a
// platform with batch syscalls overrides them behind the same signatures.
// Whether a faster path is live is decided inside the platform layer at
// socket create (runtime probe, environment override ROCKETNET_PUMP=portable),
// and degradation at any point is invisible above this seam.
//
// send_packets sends every entry; per-packet failures are dropped exactly as
// the per-packet call drops them (UDP semantics, counted by the layer above
// through its own accounting). receive_packets drains up to max_packets into
// caller storage -- from[i], packet_bytes[i], and the packet bytes at
// packet_data + i * packet_stride -- and returns the number received; zero
// means drained. packet_stride is the caller's slot size and the per-packet
// receive ceiling.

struct rocketnet_platform_send_t
{
    const struct rocketnet_address_t * to;
    const uint8_t * packet_data;
    int packet_bytes;
};

ROCKETNET_EXPORT_FUNC void rocketnet_platform_socket_send_packets( struct rocketnet_platform_socket_t * socket, const struct rocketnet_platform_send_t * sends, int num_packets );

ROCKETNET_EXPORT_FUNC int rocketnet_platform_socket_receive_packets( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );

// the name of the pump path live behind the seam ("portable", "msgx"), for
// logging and for tests that pin the degradation contract

ROCKETNET_EXPORT_FUNC const char * rocketnet_platform_socket_pump( struct rocketnet_platform_socket_t * socket );

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

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

    // linux and the BSDs: same pure-POSIX shapes

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
        bool msgx;                  // the batched sendmsg_x/recvmsg_x pump is eligible (probed at create, macOS only)
        double msgx_down_until;     // a hard refusal pauses the msgx pump until this time; the next batch call after it is the re-probe
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
// acquire and seal packets between update and the flush, and these are the
// only shared-word operations that phase needs -- a sequence taken with an
// add, a timestamp or counter published with a store, and a high-water mark
// folded with a max. Relaxed ordering is correct for all of them: each
// value is meaningful alone and nothing is ordered behind it.

#include <string.h>

#if defined(_MSC_VER)

    #include <intrin.h>

    inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
    {
        return (uint64_t) _InterlockedExchangeAdd64( (volatile long long *) value, (long long) add );
    }

    inline void rocketnet_platform_atomic_store_u64( volatile uint64_t * value, uint64_t store )
    {
    #if defined(_M_IX86)
        // 32-bit x86 has no _InterlockedExchange64; emulate with a CAS loop
        long long expected = *(volatile long long *) value;
        while ( _InterlockedCompareExchange64( (volatile long long *) value, (long long) store, expected ) != expected )
            expected = *(volatile long long *) value;
    #else // #if defined(_M_IX86)
        _InterlockedExchange64( (volatile long long *) value, (long long) store );
    #endif // #if defined(_M_IX86)
    }

    inline void rocketnet_platform_atomic_store_double( volatile double * value, double store )
    {
        uint64_t bits;
        memcpy( &bits, &store, 8 );
        rocketnet_platform_atomic_store_u64( (volatile uint64_t *) value, bits );
    }

    // folds value = max(value, candidate) with a compare-exchange loop, so
    // concurrent folds from any number of threads leave the largest

    inline void rocketnet_platform_atomic_max_u64( volatile uint64_t * value, uint64_t candidate )
    {
        long long observed = *(volatile long long *) value;
        while ( (uint64_t) observed < candidate )
        {
            long long previous = _InterlockedCompareExchange64( (volatile long long *) value, (long long) candidate, observed );
            if ( previous == observed )
                break;
            observed = previous;
        }
    }

#else // #if defined(_MSC_VER)

    inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
    {
        return __atomic_fetch_add( value, add, __ATOMIC_RELAXED );
    }

    inline void rocketnet_platform_atomic_store_u64( volatile uint64_t * value, uint64_t store )
    {
        __atomic_store_n( value, store, __ATOMIC_RELAXED );
    }

    inline void rocketnet_platform_atomic_store_double( volatile double * value, double store )
    {
        uint64_t bits;
        memcpy( &bits, &store, 8 );
        __atomic_store_n( (volatile uint64_t *) value, bits, __ATOMIC_RELAXED );
    }

    // folds value = max(value, candidate) with a compare-exchange loop, so
    // concurrent folds from any number of threads leave the largest

    inline void rocketnet_platform_atomic_max_u64( volatile uint64_t * value, uint64_t candidate )
    {
        uint64_t observed = __atomic_load_n( value, __ATOMIC_RELAXED );
        while ( observed < candidate &&
                !__atomic_compare_exchange_n( value, &observed, candidate, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED ) )
        {
            // observed reloads on failure; the loop exits once it is at
            // least the candidate
        }
    }

#endif // #if defined(_MSC_VER)

// ----------------------------------------------------------------

// Short critical sections for bookkeeping whose ring slots can alias under a
// concurrent burst. Unlike the independent counters above, these order data.
inline void rocketnet_platform_spin_lock_u64( volatile uint64_t * value )
{
#if defined(_MSC_VER)
    while ( _InterlockedCompareExchange64( (volatile long long*) value, 1, 0 ) != 0 ) {}
#else
    uint64_t expected = 0;
    while ( !__atomic_compare_exchange_n( value, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ) )
        expected = 0;
#endif
}

inline void rocketnet_platform_spin_unlock_u64( volatile uint64_t * value )
{
#if defined(_MSC_VER)
    _InterlockedCompareExchange64( (volatile long long*) value, 0, 1 );
#else
    __atomic_store_n( value, 0, __ATOMIC_RELEASE );
#endif
}

// ----------------------------------------------------------------
//
// The linux connection-type probe reads the kernel route table over netlink.
// Its reply loop is extracted behind the receive call so the unit suite can
// drive it without a socket; a failed or empty receive ends the probe instead
// of spinning. These types and the declaration are linux-only.
//
// ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_route.h"
#include <limits.h>
#include <stdint.h>
#include <sys/socket.h>
#include <linux/netlink.h>

extern void * rocketnet_global_context;

template <typename T> struct rocketnet_vector_t;
// Reads the kernel route table dump over netlink until an NLMSG_DONE message
// ends it; returns true when the dump was read in full, false when a receive
// fails or returns zero. receive is the recvmsg call, passed so tests can
// drive the probe without a socket. get_connection_type maps a false result
// to ROCKETNET_CONNECTION_TYPE_UNKNOWN.
bool rocketnet_platform_read_route_table(
    int sock,
    struct sockaddr_nl * kernel,
    rocketnet_vector_t<rocketnet_ifforward4_t> * route_table,
    ssize_t (*receive)( int, struct msghdr *, int ) );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
