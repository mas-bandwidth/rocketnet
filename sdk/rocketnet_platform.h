/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_PLATFORM_H
#define ROCKETNET_SDK_ROCKETNET_PLATFORM_H

#include "rocketnet_internal.h"
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <sys/time.h>
#else
#include <winsock2.h>
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#if defined( _MSC_VER )
#include <intrin.h>
#endif // #if defined( _MSC_VER )

// Portable 64-bit leading/trailing zero counts. The argument must be
// nonzero; callers guard the zero case. MSVC has no __builtin_clzll, so it
// uses the bit-scan intrinsics.
static inline int rocketnet_platform_clz64( uint64_t value )
{
#if defined( _MSC_VER )
    unsigned long index;
    _BitScanReverse64( &index, value );
    return 63 - (int) index;
#else
    return __builtin_clzll( value );
#endif // #if defined( _MSC_VER )
}

#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS && defined( __cplusplus )
// Private static-development-library fixture interface. Never compiled into a
// production or shared library. RIO IDs are resources, not Windows HANDLEs.
enum rocketnet_lifecycle_resource_kind_t
{
    ROCKETNET_LIFECYCLE_THREAD = 0,
    ROCKETNET_LIFECYCLE_EVENT,
    ROCKETNET_LIFECYCLE_SOCKET,
    ROCKETNET_LIFECYCLE_IOCP,
    ROCKETNET_LIFECYCLE_TIMER,
    ROCKETNET_LIFECYCLE_RIO_BUFFER,
    ROCKETNET_LIFECYCLE_RIO_CQ,
    ROCKETNET_LIFECYCLE_RIO_RQ,
    ROCKETNET_LIFECYCLE_RESOURCE_KINDS
};

#define ROCKETNET_LIFECYCLE_MAX_LIVE 512
struct rocketnet_lifecycle_resource_t
{
    uint64_t generation;
    uintptr_t raw;
    const char * site;
    unsigned kind;
};
struct rocketnet_lifecycle_resources_t
{
    uint64_t created[ROCKETNET_LIFECYCLE_RESOURCE_KINDS];
    uint64_t closed[ROCKETNET_LIFECYCLE_RESOURCE_KINDS];
    uint64_t latest_generation;
    unsigned live_count;
    unsigned errors;
    rocketnet_lifecycle_resource_t live[ROCKETNET_LIFECYCLE_MAX_LIVE];
};

void rocketnet_lifecycle_track_begin();
void rocketnet_lifecycle_track_end();
uint64_t rocketnet_lifecycle_track_open( unsigned kind, uintptr_t raw, const char * site );
bool rocketnet_lifecycle_track_close_begin( unsigned kind, uintptr_t raw, uint64_t generation );
void rocketnet_lifecycle_track_close_end( uint64_t generation, bool success );
bool rocketnet_lifecycle_track_read( rocketnet_lifecycle_resources_t * result );
#endif

#define ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING       0
#define ROCKETNET_PLATFORM_SOCKET_BLOCKING           1

struct rocketnet_address_t;
struct sockaddr_in;
struct sockaddr_in6;
struct sockaddr_storage;

struct rocketnet_platform_thread_t;

// a thread's function receives its own record, for thread_wait and thread_sleep
typedef void ( *rocketnet_platform_thread_func_t )( struct rocketnet_platform_thread_t * thread, void * arg );

// ----------------------------------------------------------------

#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus

// false only when Windows Winsock startup fails
bool rocketnet_platform_init( void );

void rocketnet_platform_term( void );

// ----------------------------------------------------------------

int rocketnet_platform_id( void );

// seconds since rocketnet_platform_init
double rocketnet_platform_time( void );

// the connect token clock's epoch in Unix seconds: configured when nonzero,
// otherwise the wall clock read once now, less the caller's time
uint64_t rocketnet_platform_token_epoch( uint64_t configured, double time );

// Windows treats non-positive and NaN as a yield; POSIX is usleep
void rocketnet_platform_sleep( double time );

struct timeval rocketnet_platform_timeval_from_seconds( float seconds );

// Whole milliseconds rounded up and clamped to 1 .. 0xFFFFFFFE.
uint32_t rocketnet_platform_timeout_milliseconds( float seconds );

struct rocketnet_platform_sleep_split_t
{
    uint32_t seconds;
    uint32_t microseconds;
};

struct rocketnet_platform_sleep_split_t rocketnet_platform_sleep_split( double time );

// 1 set and copied, 0 unset, -1 value too long for size; buffer is left empty unless 1
int rocketnet_platform_getenv( const char * var, char * buffer, size_t size );

#define ROCKETNET_PLATFORM_GETENV_MAX_BYTES 4096

#if ROCKETNET_DEV
/** Installs the unit suite's empty in-memory environment. */
void rocketnet_platform_test_environment_install( void );

/** Sets one variable in the installed in-memory environment. */
void rocketnet_platform_test_environment_set( const char * name, const char * value );

/** Removes one variable from the installed in-memory environment. */
void rocketnet_platform_test_environment_unset( const char * name );

/** Removes every variable while keeping the in-memory environment installed. */
void rocketnet_platform_test_environment_reset( void );
#endif // #if ROCKETNET_DEV

uint16_t rocketnet_platform_ntohs( uint16_t in );

uint16_t rocketnet_platform_htons( uint16_t in );

// the address bytes stay in network order; the port is converted
void rocketnet_platform_address_to_sockaddr_in( const struct rocketnet_address_t * address, struct sockaddr_in * out );

// the address bytes stay in network order; the port is converted to host order
void rocketnet_platform_sockaddr_in_to_address( const struct sockaddr_in * in, struct rocketnet_address_t * address );

// the IPv6 pair: eight host-order words become sixteen network-order bytes,
// the port is converted; the destination is zeroed first.
void rocketnet_platform_address_to_sockaddr_in6( const struct rocketnet_address_t * address, struct sockaddr_in6 * out );
void rocketnet_platform_sockaddr_in6_to_address( const struct sockaddr_in6 * in, struct rocketnet_address_t * address );

// An address as the sockaddr of its own family, written into zeroed
// storage. Returns the sockaddr's byte count, 0 for an address that is
// neither IPv4 nor IPv6.
int rocketnet_platform_sockaddr_from_address( const struct rocketnet_address_t * address, struct sockaddr_storage * storage );

// A destination as the socket's family carries it: a dual-stack IPv6 socket
// sends IPv4 as its v4-mapped form, an IPv4 socket carries IPv4 only.
// Returns the byte count, 0 for an address the socket cannot carry.
int rocketnet_platform_sockaddr_for( bool ipv6, const struct rocketnet_address_t * to, struct sockaddr_storage * storage );

// A received source: false for a family the transport does not speak, or a
// source the socket must drop (rocketnet_address_accept_received_source).
bool rocketnet_platform_sockaddr_to_address( const struct sockaddr_storage * storage, bool dual_stack, struct rocketnet_address_t * address );

// address_out is written only on success
bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out );

// the words hold raw network-order bytes; run each through ntohs for a RocketNet IPv6 group
bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out );

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size );

#if ROCKETNET_DEV
bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, struct rocketnet_address_t * address );
#endif // #if ROCKETNET_DEV

// zero lets the socket choose an ephemeral port
uint16_t rocketnet_platform_preferred_client_port( void );

// false where IPv6 sockets are IPv6-only (OpenBSD)
bool rocketnet_platform_client_dual_stack( void );

// a host with IPv6 disabled refuses socket( AF_INET6 ); the client picks its bind family from this
bool rocketnet_platform_client_ipv6_available( void );

#if ROCKETNET_DEV
// false behaves as a host with no IPv6 stack
void rocketnet_platform_set_ipv6_socket_available_for_testing( bool available );

#endif // #if ROCKETNET_DEV

// ROCKETNET_ADDRESS_NONE when the server is IPv6 and this host has no IPv6 stack
int rocketnet_platform_client_bind_family( int server_address_type, bool ipv6_available, bool dual_stack );

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

// ----------------------------------------------------------------

#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus

// port zero is written back as the assigned port; release with socket_destroy.
// The socket, wake and thread creates write OUT_OF_MEMORY through status when
// their allocation is refused (rocketnet_create_status); NULL: not asked.
struct rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, int * status = NULL );

// pump is NULL or "auto", "portable", or "msgx" (macOS only); NULL back when it is unavailable
struct rocketnet_platform_socket_t * rocketnet_platform_socket_create_with_pump( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, const char * pump, int * status = NULL );

void rocketnet_platform_socket_destroy( struct rocketnet_platform_socket_t * socket );

// closes the handle and keeps the memory for socket_destroy on the owning thread; false only when the Windows lifecycle tracker refused
bool rocketnet_platform_socket_close( struct rocketnet_platform_socket_t * socket );

// send errors are logged or dropped
void rocketnet_platform_socket_send_packet( struct rocketnet_platform_socket_t * socket, const struct rocketnet_address_t * to, const void * packet_data, int packet_bytes );

// the kernel drops datagrams from other sources and sends name no destination, so send only to this endpoint
bool rocketnet_platform_socket_connect( struct rocketnet_platform_socket_t * socket, const struct rocketnet_address_t * to );

// a result above max_packet_size marks an oversize datagram; only its prefix is stored
int rocketnet_platform_socket_receive_packet( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, void * packet_data, int max_packet_size );

// ----------------------------------------------------------------

// The batch pump seam: move a batch of packets in
// as few syscalls as the platform allows. Every platform implements these --
// the portable implementation loops the per-packet calls above, so the
// baseline doctrine holds and a call site never carries an #ifdef -- and a
// platform with batch syscalls overrides them behind the same signatures.
// Whether a faster path is live is decided inside the platform layer at
// socket create (runtime probe or an explicit transport pump request),
// and degradation at any point is invisible above this seam.
//
// send_packets sends every entry; per-packet failures are dropped exactly as
// the per-packet call drops them (UDP semantics, counted by the layer above
// through its own accounting). receive_packets drains up to max_packets into
// caller storage -- from[i], packet_bytes[i], and the packet bytes at
// packet_data + i * packet_stride -- and returns the number received; zero
// means drained. packet_stride is the caller's slot size and the per-packet
// receive ceiling.

// the address and bytes stay caller-owned until socket_send_packets returns
struct rocketnet_platform_send_t
{
    const struct rocketnet_address_t * to;
    const uint8_t * packet_data;
    int packet_bytes;
};

// each datagram follows send_packet's error behavior; failures are dropped
void rocketnet_platform_socket_send_packets( struct rocketnet_platform_socket_t * socket, const struct rocketnet_platform_send_t * sends, int num_packets );

// a length above packet_stride marks an oversize datagram; only its prefix is stored
int rocketnet_platform_socket_receive_packets( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// Winsock reports an ICMP port, host or network unreachable that answers an
// earlier send as an error on the next receive of a UDP socket: WSAECONNRESET
// (SIO_UDP_CONNRESET) or WSAENETRESET (SIO_UDP_NETRESET), and an overlapped or
// registered receive completes with the matching unreachable status. A client
// that dies without closing its socket makes a server see these on every
// send to it. Every rocketnet socket turns both reports off at create, and
// every receive path skips these statuses, counts them and keeps draining.

// turns SIO_UDP_CONNRESET and SIO_UDP_NETRESET off on a UDP socket (a SOCKET
// passed as uintptr_t). Each WSAIoctl result is checked; a refusal is logged
// once per process and never fails the create. Returns true when both are off.
bool rocketnet_platform_socket_disable_udp_resets( uintptr_t handle );

// true for a Winsock or Win32 status an ICMP error report produces on a UDP
// receive: WSAECONNRESET, WSAENETRESET, WSAECONNREFUSED, WSAEHOSTUNREACH,
// WSAENETUNREACH, ERROR_PORT_UNREACHABLE, ERROR_HOST_UNREACHABLE,
// ERROR_NETWORK_UNREACHABLE and ERROR_CONNECTION_REFUSED
bool rocketnet_platform_receive_status_is_reset( int status );

// the process-wide count of reset statuses the receive paths skipped: zero
// while the reports stay off
void rocketnet_platform_count_receive_reset( void );
uint64_t rocketnet_platform_receive_resets( void );

// the process-wide count of zero-length datagrams the per-packet receive
// (rocketnet_platform_socket_receive_packet) skipped: its 0 return means the
// socket is drained, so a zero-length datagram is read past and counted here.
// The batch receive hands one to the transport at 0 bytes instead, where the
// prefilter counts it.
uint64_t rocketnet_platform_receive_empties( void );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// counts each dropped datagram and each refused sendmsg_x chunk once; the replay's refusals count in send_replay_failures
uint64_t rocketnet_platform_socket_send_failures( struct rocketnet_platform_socket_t * socket );

#if ROCKETNET_DEV
// the replay may resend packets the kernel had delivered, so a refusal here may have reached the wire
uint64_t rocketnet_platform_socket_send_replay_failures( struct rocketnet_platform_socket_t * socket );
#endif // #if ROCKETNET_DEV

// Send failures by errno. Each failed send call is counted by the call that
// failed and the errno it returned, and the first failure of each call and
// errno is logged once at info level; later ones are only counted, so a
// lasting refusal costs no log line per packet. The socket logs the counts
// when it is destroyed. Pure counting: no socket or clock is read here.

#define ROCKETNET_PLATFORM_SEND_CALL_SENDTO             0   // an ordinary per-packet send
#define ROCKETNET_PLATFORM_SEND_CALL_SENDMSG_X          1   // a refused sendmsg_x chunk, counted once per chunk
#define ROCKETNET_PLATFORM_SEND_CALL_REPLAY             2   // a refused per-packet replay send of a refused sendmsg_x chunk
#define ROCKETNET_PLATFORM_SEND_CALLS                   3

#define ROCKETNET_PLATFORM_SEND_ERRNO_NONE              0   // the call failed with no errno (sendmsg_x accepted nothing)
#define ROCKETNET_PLATFORM_SEND_ERRNO_EAGAIN            1   // EAGAIN or EWOULDBLOCK: the socket send buffer is full
#define ROCKETNET_PLATFORM_SEND_ERRNO_ENOBUFS           2
#define ROCKETNET_PLATFORM_SEND_ERRNO_EHOSTUNREACH      3
#define ROCKETNET_PLATFORM_SEND_ERRNO_ENETUNREACH       4
#define ROCKETNET_PLATFORM_SEND_ERRNO_ECONNREFUSED      5
#define ROCKETNET_PLATFORM_SEND_ERRNO_EHOSTDOWN         6
#define ROCKETNET_PLATFORM_SEND_ERRNO_ENETDOWN          7
#define ROCKETNET_PLATFORM_SEND_ERRNO_EMSGSIZE          8
#define ROCKETNET_PLATFORM_SEND_ERRNO_EADDRNOTAVAIL     9
#define ROCKETNET_PLATFORM_SEND_ERRNO_EPERM             10  // EPERM or EACCES
#define ROCKETNET_PLATFORM_SEND_ERRNO_EINTR             11
#define ROCKETNET_PLATFORM_SEND_ERRNO_OTHER             12
#define ROCKETNET_PLATFORM_SEND_ERRNO_KINDS             13

struct rocketnet_platform_send_errnos_t
{
    uint64_t counts[ROCKETNET_PLATFORM_SEND_CALLS][ROCKETNET_PLATFORM_SEND_ERRNO_KINDS]; // atomic, monotonic
    uint64_t other_error;       // the errno of the first OTHER failure, so the log can name it; atomic
};

int rocketnet_platform_send_errno_kind( int error );

const char * rocketnet_platform_send_errno_kind_name( int kind );

const char * rocketnet_platform_send_call_name( int call );

// true for the first failure of this call and errno kind, so the caller logs it once
bool rocketnet_platform_send_errnos_count( struct rocketnet_platform_send_errnos_t * errnos, int call, int error );

uint64_t rocketnet_platform_send_errnos_read( struct rocketnet_platform_send_errnos_t * errnos, int call, int kind );

int rocketnet_platform_send_errnos_format( struct rocketnet_platform_send_errnos_t * errnos, char * buffer, size_t size );

#if ROCKETNET_DEV && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
// test only: the next count sends on the socket fail with error without reaching the kernel
void rocketnet_platform_socket_fail_next_sends( struct rocketnet_platform_socket_t * socket, int count, int error );

// test only: lets a functional case find the socket a transport created
int rocketnet_platform_sockets_for_testing( struct rocketnet_platform_socket_t ** sockets, int max_sockets );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_DEV
struct sockaddr_storage;
// test only: writes one datagram and its source, returns its size; -1 means drained
typedef int ( *rocketnet_platform_receive_hook_t )( void * user, void * packet_data, int max_packet_size, struct sockaddr_storage * from );

// test only: replaces the kernel read on this socket; NULL clears it
void rocketnet_platform_socket_set_receive_hook( struct rocketnet_platform_socket_t * socket, rocketnet_platform_receive_hook_t hook, void * user );
#endif // #if ROCKETNET_DEV

// the name of the pump path live behind the seam ("portable", "msgx"), for
// logging and for tests that pin the degradation contract

// names the path in use, such as "portable" or "msgx"; a degraded path shows here
const char * rocketnet_platform_socket_pump( struct rocketnet_platform_socket_t * socket );

#if ROCKETNET_DEV
// Test seams, compiled only into the development library (TRANSPORTS.md, ICMP
// errors). inject_receive_errno makes the next recvmsg or recvmsg_x on any
// socket return -1 with that errno instead of reading, leaving the queue
// untouched; it is defined where the darwin pumps are. The server transport's
// platform socket is its portable or msgx transport's socket, NULL for any
// other transport.
void rocketnet_platform_inject_receive_errno_for_testing( int error );
// the repeating form: the next count receives each return error (the
// one-shot call is a count of 1), and the remaining count of those
// injections not yet taken by a receive.
void rocketnet_platform_inject_receive_errno_repeat_for_testing( int error, int count );
int rocketnet_platform_receive_errno_remaining_for_testing( void );
struct rocketnet_server_transport_t;
struct rocketnet_platform_socket_t * rocketnet_server_transport_platform_socket_for_testing( struct rocketnet_server_transport_t * server_transport );
#endif // #if ROCKETNET_DEV

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

// ----------------------------------------------------------------

// The Linux client's batch send/receive (#client-io-linux-mmsg): sendmmsg
// and recvmmsg on the I/O thread behind the same signatures as the seam
// above. Internal, no export, defined only where __linux__ is set.

#if defined( __linux__ )
// Linux sendmmsg path; behaves as socket_send_packets
void rocketnet_platform_socket_send_packets_mmsg( struct rocketnet_platform_socket_t * socket, const struct rocketnet_platform_send_t * sends, int num_packets );
// Linux recvmmsg path; same layout and oversize rule as socket_receive_packets, and zero-length datagrams make no entry
int rocketnet_platform_socket_receive_packets_mmsg( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );
#endif // #if defined( __linux__ )

// The client I/O wake (the design's The wake section): a level-triggered wake
// beside a socket. wait blocks until the wake is set, the socket is readable
// (only when readable is true) or timeout_seconds pass, consumes a set wake,
// and returns true when woken or readable; with readable false the socket's
// readiness is ignored, so a full receive buffer waits on the wake alone. set may run on any thread; a set made while no wait
// runs makes the next wait return at once. Platforms that define
// ROCKETNET_PLATFORM_HAS_WAKE implement socket-backed waits. BSD supports
// event-only pipe wakes without changing its socket-backed client path.
// Linux builds the eventfd wake; macOS and iOS build the kqueue wake
// (docs/CLIENT-IO.md "The wake"), so the msgx
// client runs on the I/O thread there. Windows builds the event wake,
// WSAEventSelect FD_READ beside a manual-reset event (docs/CLIENT-IO.md
// "The wake"), so the portable client runs on the I/O
// thread there.
#if ( ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( __linux__ ) ) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#define ROCKETNET_PLATFORM_HAS_WAKE 1
#endif
#if defined( ROCKETNET_PLATFORM_HAS_WAKE ) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
#define ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE 1
#endif // #if defined( ROCKETNET_PLATFORM_HAS_WAKE ) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
// level-triggered; a socket given to wake_create must outlive the wake
struct rocketnet_platform_wake_t;
// socket may be NULL for an event-only wake; NULL back when the platform has no wakes
struct rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, struct rocketnet_platform_socket_t * socket, int * status = NULL );
// zero when wakes are unavailable
uint64_t rocketnet_platform_wake_allocation_bytes( void );
// destroy before the associated socket
void rocketnet_platform_wake_destroy( struct rocketnet_platform_wake_t * wake );
// closes the handles on the waiting thread and keeps the memory for wake_destroy; false only when the Windows lifecycle tracker refused
bool rocketnet_platform_wake_close( struct rocketnet_platform_wake_t * wake );
// readable also releases on socket readability; an event-only wake returns false at once for readable
bool rocketnet_platform_wake_wait( struct rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds );
// any thread may signal; a signal made before a wait stays pending until a wait consumes it
void rocketnet_platform_wake_set( struct rocketnet_platform_wake_t * wake );

// ----------------------------------------------------------------

#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus

// join before thread_destroy. The record exists before the thread starts and
// is the function's first argument.
struct rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t func, void * arg, int * status = NULL );

// call once, from another thread
void rocketnet_platform_thread_join( struct rocketnet_platform_thread_t * thread );

void rocketnet_platform_thread_destroy( struct rocketnet_platform_thread_t * thread );

// called on the thread that thread_create started, with that thread's own
// record: returns after time, or once thread_wake has been called for it.
// Windows sleeps and its wake is a no-op.
void rocketnet_platform_thread_wait( struct rocketnet_platform_thread_t * thread, double time );

// called on the thread that thread_create started, with that thread's own
// record: rocketnet_platform_sleep on the thread's own timer (Windows)
void rocketnet_platform_thread_sleep( struct rocketnet_platform_thread_t * thread, double time );

// store the quit flag first, then wake, then join
void rocketnet_platform_thread_wake( struct rocketnet_platform_thread_t * thread );

// a refused request is counted and logged, never returned
void rocketnet_platform_client_thread_priority( struct rocketnet_platform_thread_t * thread );

// a refused request is counted and logged, never returned
void rocketnet_platform_server_thread_priority( struct rocketnet_platform_thread_t * thread );

/**
 * @brief The operating-system calls behind the network-thread priority.
 *
 * Each platform fills the table with the real calls; a unit test replaces it
 * with a recording fake through rocketnet_platform_set_priority_calls_for_testing.
 * An entry a platform does not use is a stub that does nothing.
 */
struct rocketnet_platform_mutex_t;

#if ROCKETNET_DEV
struct rocketnet_platform_priority_calls_t
{
    /** @brief POSIX: pthread_setschedparam on the thread with the policy and priority; 0 or an errno. */
    int ( *set_sched )( struct rocketnet_platform_thread_t * thread, int policy, int priority );
    /** @brief Windows: SetThreadPriority on the thread; nonzero on success, as the call returns. */
    int ( *set_thread_priority )( struct rocketnet_platform_thread_t * thread, int priority );
    /** @brief Windows: timeBeginPeriod; 0 (TIMERR_NOERROR) on success. */
    unsigned int ( *time_begin_period )( unsigned int period );
    /** @brief Windows: timeEndPeriod; 0 (TIMERR_NOERROR) on success. */
    unsigned int ( *time_end_period )( unsigned int period );
    /** @brief Windows: QueryPerformanceFrequency into the LARGE_INTEGER the argument points at. */
    void ( *timer_frequency )( void * frequency );
    /** @brief Platform mutex create on the embedded handle; 0 on success, an errno or nonzero on failure. */
    int ( *mutex_init )( struct rocketnet_platform_mutex_t * mutex );
    /** @brief Platform startup (Windows WSAStartup); 0 on success, nonzero on failure. */
    int ( *platform_start )( void );
    /** @brief Windows: CreateWaitableTimerExW; NULL on failure. Frame sync requests CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x00000002) and refuses creation without falling back to a coarse timer when unavailable. Off Windows the handle is unused and the entry returns a non-NULL placeholder only for the high-resolution flag, so NULL stays the failure answer on every platform. */
    void * ( *create_waitable_timer )( unsigned flags );
    /** @brief The OS thread start for a thread record (POSIX pthread_create, Windows CreateThread), writing the thread's handle; 0 on success, nonzero when the OS refuses the thread. */
    int ( *thread_start )( struct rocketnet_platform_thread_t * thread, void * record );
};

/**
 * @brief The real call table of this platform.
 * @return The table the library uses when no test has replaced it; never NULL.
 */
const struct rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls_real( void );

/**
 * @brief Replaces the priority call table, for unit tests only.
 * @param calls The table to use; NULL restores the real table.
 * @return None.
 */
void rocketnet_platform_set_priority_calls_for_testing( const struct rocketnet_platform_priority_calls_t * calls );

/**
 * @brief The priority call table currently in effect (the test override, or the real table).
 * @return The table in effect; never NULL.
 */
const struct rocketnet_platform_priority_calls_t * rocketnet_platform_priority_calls_current( void );

/**
 * @brief Forgets which call kinds have logged their one refusal line, for unit tests only.
 * @return None.
 */
void rocketnet_platform_thread_priority_reset_logged_for_testing( void );

#endif // #if ROCKETNET_DEV

/** @brief The call kinds a refusal is counted and logged under, one line each. */
enum
{
    ROCKETNET_PLATFORM_PRIORITY_CALL_SET_SCHED = 0,
    ROCKETNET_PLATFORM_PRIORITY_CALL_SET_AFFINITY,
    ROCKETNET_PLATFORM_PRIORITY_CALL_SET_THREAD_PRIORITY,
    ROCKETNET_PLATFORM_PRIORITY_CALL_TIME_BEGIN_PERIOD,
    ROCKETNET_PLATFORM_PRIORITY_CALL_TIME_END_PERIOD,
    ROCKETNET_PLATFORM_PRIORITY_CALL_COUNT
};

/**
 * @brief Sets whether the library makes its priority and timer period calls.
 * @param enabled true (the default) makes them; false makes none.
 * @return None.
 */
void rocketnet_platform_set_thread_priority_enabled( bool enabled );

/**
 * @brief Reads whether the library makes its priority and timer period calls.
 * @return true when enabled.
 */
bool rocketnet_platform_thread_priority_enabled( void );

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
/**
 * @brief The SCHED_RR priority the client and server network threads request.
 *
 * Above every ordinary thread: sched_get_priority_min( SCHED_RR ) on Linux
 * (1), sched_get_priority_max( SCHED_RR ) on macOS (47; its minimum, 15, is
 * below the default 31), elsewhere the minimum raised to a default thread's
 * priority where that is higher.
 * @return The priority.
 */
int rocketnet_platform_network_thread_rr_priority( void );
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

/**
 * @brief Counts one refused priority, affinity or timer period call.
 *
 * Adds one to the process-wide refusal count and, the first time for the call
 * kind only, logs one warning: "<prefix>: <call> refused (<reason>); <effect>".
 * Safe from any thread.
 * @param kind One of the ROCKETNET_PLATFORM_PRIORITY_CALL_* kinds.
 * @param prefix "thread priority", "thread affinity" or "timer period".
 * @param call The call and its arguments, for example "pthread_setschedparam SCHED_RR 1".
 * @param reason Why the operating system refused it.
 * @param effect What runs instead, for example "the thread runs at its default priority".
 * @return None.
 */
void rocketnet_platform_thread_priority_refused( int kind, const char * prefix, const char * call, const char * reason, const char * effect );

/**
 * @brief The process-wide count of refused priority, affinity and timer period calls.
 * @return The count since the process started.
 */
uint64_t rocketnet_platform_thread_priority_refusals( void );

// ----------------------------------------------------------------

struct rocketnet_platform_mutex_t;

bool rocketnet_platform_mutex_create( struct rocketnet_platform_mutex_t * mutex );

// no thread may hold or wait on the mutex
void rocketnet_platform_mutex_destroy( struct rocketnet_platform_mutex_t * mutex );

// not recursive: a second acquire on one thread deadlocks
void rocketnet_platform_mutex_acquire( struct rocketnet_platform_mutex_t * mutex );

// Acquires only when immediately available; never waits for the owner.
bool rocketnet_platform_mutex_try_acquire( struct rocketnet_platform_mutex_t * mutex );

void rocketnet_platform_mutex_release( struct rocketnet_platform_mutex_t * mutex );

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#ifdef __cplusplus

// scope guard: locks on construction, unlocks on destruction
struct rocketnet_platform_mutex_helper_t
{
    rocketnet_platform_mutex_t * mutex;
    rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * held_mutex );
    ~rocketnet_platform_mutex_helper_t();
};

#define rocketnet_platform_mutex_guard( _mutex ) rocketnet_platform_mutex_helper_t __mutex_helper( _mutex )

inline rocketnet_platform_mutex_helper_t::rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * held_mutex ) : mutex( held_mutex )
{
    rocketnet_assert( mutex );

    rocketnet_platform_mutex_acquire( mutex );
}

inline rocketnet_platform_mutex_helper_t::~rocketnet_platform_mutex_helper_t()
{
    rocketnet_assert( mutex );

    rocketnet_platform_mutex_release( mutex );
    mutex = NULL;
}

#endif // #ifdef __cplusplus

// ----------------------------------------------------------------

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS // POSIX

#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#if defined( __APPLE__ )
#include <mach/mach_types.h>
#endif // #if defined( __APPLE__ )

typedef int rocketnet_platform_socket_handle_t;

struct rocketnet_platform_socket_t
{
    void * context;
    int type;
    bool ipv6;
    bool connected;                 // connected to its one remote endpoint (rocketnet_platform_socket_connect); sends name no destination
    rocketnet_platform_socket_handle_t handle;
    uint64_t send_failures;         // datagrams a failed send dropped, plus one per refused sendmsg_x chunk; atomic, monotonic
    uint64_t send_replay_failures;  // refused sends of the per-packet replay of a refused sendmsg_x chunk; atomic, monotonic
#if defined( __APPLE__ )
    bool msgx;                      // the sendmsg_x/recvmsg_x pump is eligible (macOS only); the first batch call probes it
    double msgx_down_until;         // a hard refusal pauses the msgx pump until this time; the next batch call after it is the re-probe
    struct rocketnet_platform_send_errnos_t send_errnos; // every failed send call above, by call and errno
#endif // #if defined( __APPLE__ )
};

bool rocketnet_platform_mmsg_read_sockaddr( struct rocketnet_platform_socket_t * socket, const struct sockaddr_storage * storage, struct rocketnet_address_t * from );

struct rocketnet_platform_thread_t
{
    void * context;
    pthread_t handle;
#if defined( __APPLE__ )
    mach_port_t wake_semaphore;     // thread_wait waits on it, thread_wake signals it
#else // #if defined( __APPLE__ )
    pthread_mutex_t wake_mutex;
    pthread_cond_t wake_cond;
#endif // #if defined( __APPLE__ )
    int woken;                      // set once by thread_wake, never cleared
};

struct rocketnet_platform_mutex_t
{
    bool ok;
    pthread_mutex_t handle;
};

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS // #if POSIX

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

#if _WIN64
typedef uint64_t rocketnet_platform_socket_handle_t;
#else
typedef _W64 unsigned int rocketnet_platform_socket_handle_t;
#endif // #if _WIN64

struct rocketnet_platform_socket_t
{
    void * context;
    bool ipv6;
    bool connected;             // connected to its one remote endpoint (rocketnet_platform_socket_connect); sends name no destination
    rocketnet_platform_socket_handle_t handle;
    uint64_t send_failures;     // datagrams a failed send dropped; atomic, monotonic
    uint64_t send_replay_failures; // refused sends of a sendmsg_x replay; zero here; atomic, monotonic
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    uint64_t lifecycle_generation;
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
};

struct rocketnet_platform_thread_t
{
    void * context;
    HANDLE handle;
    HANDLE sleep_timer;             // thread_sleep blocks on it; NULL where the timer cannot be created
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    uint64_t lifecycle_generation;
    uint64_t sleep_timer_generation;
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
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

static inline bool rocketnet_platform_socket_handle_is_valid( rocketnet_platform_socket_handle_t handle )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return handle != (rocketnet_platform_socket_handle_t) INVALID_SOCKET;
#else
    return handle >= 0;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

// ----------------------------------------------------------------

// The fill-phase atomics: the send path lets any number of game threads
// acquire and seal packets between update and the flush, and these are the
// only shared-word operations that phase needs -- a sequence taken with an
// add, a timestamp or counter published with a store, and a high-water mark
// folded with a max. Relaxed ordering is correct for all of them: each
// value is meaningful alone and nothing is ordered behind it.

#include <string.h>

#if defined( _MSC_VER )

#include <intrin.h>

    // returns the previous value
inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
{
    return (uint64_t) _InterlockedExchangeAdd64( (volatile long long *) value, (long long) add );
}

    // relaxed ordering
inline uint64_t rocketnet_platform_atomic_load_u64( volatile uint64_t * value )
{
#if defined( _M_IX86 )
        // 32-bit x86 has no single-instruction 64-bit load; CAS with the
        // observed value (a self-write) retries until it reads consistent.
    long long observed = *(volatile long long *) value;
    while ( _InterlockedCompareExchange64( (volatile long long *) value, observed, observed ) != observed )
        observed = *(volatile long long *) value;
    return (uint64_t) observed;
#else
    return (uint64_t) _InterlockedCompareExchange64( (volatile long long *) value, 0, 0 );
#endif // #if defined( _M_IX86 )
}

    // relaxed ordering
inline void rocketnet_platform_atomic_store_u64( volatile uint64_t * value, uint64_t store )
{
#if defined( _M_IX86 )
        // 32-bit x86 has no _InterlockedExchange64; emulate with a CAS loop
    long long expected = *(volatile long long *) value;
    while ( _InterlockedCompareExchange64( (volatile long long *) value, (long long) store, expected ) != expected )
        expected = *(volatile long long *) value;
#else // #if defined(_M_IX86)
    _InterlockedExchange64( (volatile long long *) value, (long long) store );
#endif // #if defined(_M_IX86)
}

    // relaxed ordering
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

    // returns the previous value
inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
{
    return __atomic_fetch_add( value, add, __ATOMIC_RELAXED );
}

    // relaxed ordering
inline uint64_t rocketnet_platform_atomic_load_u64( volatile uint64_t * value )
{
    return __atomic_load_n( value, __ATOMIC_RELAXED );
}

    // relaxed ordering
inline void rocketnet_platform_atomic_store_u64( volatile uint64_t * value, uint64_t store )
{
    __atomic_store_n( value, store, __ATOMIC_RELAXED );
}

    // relaxed ordering
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
// spins until the word goes from zero to one
inline void rocketnet_platform_spin_lock_u64( volatile uint64_t * value )
{
#if defined( _MSC_VER )
    while ( _InterlockedCompareExchange64( (volatile long long *) value, 1, 0 ) != 0 )
    {
    }
#else
    uint64_t expected = 0;
    while ( !__atomic_compare_exchange_n( value, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ) )
        expected = 0;
#endif // #if defined( _MSC_VER )
}

inline void rocketnet_platform_spin_unlock_u64( volatile uint64_t * value )
{
#if defined( _MSC_VER )
    _InterlockedCompareExchange64( (volatile long long *) value, 0, 1 );
#else
    __atomic_store_n( value, 0, __ATOMIC_RELEASE );
#endif // #if defined( _MSC_VER )
}

#endif // ROCKETNET_SDK_ROCKETNET_PLATFORM_H
