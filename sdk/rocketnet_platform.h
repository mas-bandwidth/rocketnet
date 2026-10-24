/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include "rocketnet_internal.h"
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <sys/time.h>
#else
#include <winsock2.h>
#endif

#define ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING       0
#define ROCKETNET_PLATFORM_SOCKET_BLOCKING           1

/**
 * @brief Names the SDK's IP address type used by the socket, conversion and
 * resolve helpers.
 *
 * The full type is defined in the internal layers header; this header only
 * forward-declares it so the functions below can take and return it.
 */
struct rocketnet_address_t;
/**
 * @brief Names the IPv4 socket-address type used by the conversion helpers.
 *
 * This is the system socket type; this header only forward-declares it.
 */
struct sockaddr_in;

/**
 * @brief Describes a function that runs as a platform thread.
 *
 * The function runs with the argument supplied to thread_create and returns
 * normally to finish the thread.
 * @param arg User argument; NULL is passed through unchanged.
 * @return None.
 */
typedef void (*rocketnet_platform_thread_func_t)(void*);

// ----------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the platform layer before other platform calls.
 *
 * Call once before creating transports, sockets, threads, or mutexes. POSIX
 * implementations return true after recording the time origin; Windows can
 * return false when Winsock initialization fails.
 * @return true when initialization succeeds; false when platform startup fails.
 */
bool rocketnet_platform_init( void );

/**
 * @brief Releases platform-wide resources after all users have stopped.
 *
 * Call after transports and platform objects are destroyed and worker threads
 * are joined. The function has no status result.
 * @return None.
 */
void rocketnet_platform_term( void );

// ----------------------------------------------------------------

/**
 * @brief Returns the compile-time platform identifier.
 *
 * The result is one of the ROCKETNET_PLATFORM_* constants used by this build.
 * @return The platform identifier; this call has no failure result.
 */
int rocketnet_platform_id( void );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
/**
 * @brief Compares a dynamic-store primary interface name with a BSD name.
 *
 * Both inputs must be Core Foundation strings. A missing or differently typed
 * value is not a match.
 * @return true when both names are Core Foundation strings and compare equal.
 */
bool rocketnet_platform_mac_primary_interface_matches( const void * primary_interface_name, const void * bsd_name );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

/**
 * @brief Returns the connection type detected during platform initialization.
 *
 * Call after rocketnet_platform_init. A failed or unavailable route probe is
 * reported as ROCKETNET_CONNECTION_TYPE_UNKNOWN.
 * @return The detected connection type, or UNKNOWN when it cannot be detected.
 */
int rocketnet_platform_connection_type( void );

/**
 * @brief Reads elapsed monotonic time in seconds.
 *
 * Initialize the platform first. The value is relative to the platform's
 * initialization time and is suitable for measuring durations.
 * @return Elapsed seconds; no error status is reported.
 */
double rocketnet_platform_time( void );

/**
 * @brief Pauses the calling thread for the requested duration.
 *
 * @param time Requested seconds. Windows treats non-positive and NaN values
 * as yield-only requests and rounds positive values up to its timer unit. POSIX
 * passes the value converted to microseconds to usleep; invalid or
 * unrepresentable values are not reported by this void API.
 * @return None.
 */
void rocketnet_platform_sleep( double time );

struct timeval rocketnet_platform_timeval_from_seconds( float seconds );

struct rocketnet_platform_sleep_split_t
{
    uint32_t seconds;
    uint32_t microseconds;
};

struct rocketnet_platform_sleep_split_t rocketnet_platform_sleep_split( double time );

/**
 * @brief Copies a process environment variable's value into caller storage.
 *
 * @param var NUL-terminated variable name; must not be NULL.
 * @param buffer Receives the NUL-terminated value; left empty unless 1 is returned.
 * @param size Bytes of buffer, at least 1. ROCKETNET_PLATFORM_GETENV_MAX_BYTES holds any value the SDK reads.
 * @return 1 when the variable is set and its value was copied, 0 when it is
 * unset, -1 when it is set but its value and terminator need more than size bytes.
 */
int rocketnet_platform_getenv( const char * var, char * buffer, size_t size );

#define ROCKETNET_PLATFORM_GETENV_MAX_BYTES 4096

/**
 * @brief Converts a 16-bit value from network byte order to host byte order.
 *
 * @param in Value in network byte order.
 * @return The converted host-order value.
 */
uint16_t rocketnet_platform_ntohs( uint16_t in );

/**
 * @brief Converts a 16-bit value from host byte order to network byte order.
 *
 * @param in Value in host byte order.
 * @return The converted network-order value.
 */
uint16_t rocketnet_platform_htons( uint16_t in );

/**
 * @brief Copies an IPv4 address into a system socket address.
 *
 * @param address Source IPv4 address; must be non-NULL and have IPv4 type.
 * Its address bytes are already in network order and its port is in host order.
 * @param out Caller-owned destination structure; must be non-NULL. The port is
 * converted to network byte order.
 * @return None. The destination remains owned by the caller.
 */
void rocketnet_platform_address_to_sockaddr_in( const struct rocketnet_address_t * address, struct sockaddr_in * out );

/**
 * @brief Copies a system IPv4 socket address into a RocketNet address.
 *
 * @param in Source socket address; must be non-NULL.
 * @param address Caller-owned destination; must be non-NULL. Address bytes
 * remain in network order and the port is converted to host byte order.
 * @return None. The destination remains owned by the caller.
 */
void rocketnet_platform_sockaddr_in_to_address( const struct sockaddr_in * in, struct rocketnet_address_t * address );

/**
 * @brief Parses a dotted-decimal IPv4 address.
 *
 * @param address_string NUL-terminated address text; must be non-NULL.
 * @param address_out Caller-owned output word; must be non-NULL and is written
 * only when parsing succeeds. Its four in-memory bytes are in network order.
 * @return true for a valid IPv4 address; false when the text is invalid.
 */
bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out );

/**
 * @brief Parses an IPv6 address into its 16 network-order bytes.
 *
 * The bytes are written through eight 16-bit words as storage; the numeric
 * word values are not host-order address groups. Convert each word with
 * rocketnet_platform_ntohs before using it as a RocketNet IPv6 group.
 * @param address_string NUL-terminated IPv6 text; must be non-NULL.
 * @param address_out Caller-owned storage for eight 16-bit words; must be
 * non-NULL.
 * @return true for a valid IPv6 address; false when parsing fails.
 */
bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out );

/**
 * @brief Formats 16 network-order IPv6 bytes as text.
 *
 * @param address Caller-owned storage containing eight words with the same
 * network-order byte layout produced by inet_pton6; must be non-NULL.
 * @param address_string Caller-owned output buffer; must be non-NULL.
 * @param address_string_size Buffer capacity in bytes, including space for the
 * terminator; must be large enough for an IPv6 string.
 * @return true when formatting succeeds; false when the buffer is too small or
 * the system formatter rejects the arguments.
 */
bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size );

/**
 * @brief Resolves a host name and service to one IPv4 or IPv6 address.
 *
 * @param hostname NUL-terminated host name accepted by the system resolver.
 * @param port NUL-terminated service or port accepted by the system resolver.
 * @param address Caller-owned output address; must be non-NULL and is written
 * on success.
 * @return true when the first supported result is IPv4 or IPv6; false when
 * resolution fails or returns an unsupported address family.
 */
bool rocketnet_platform_hostname_resolve( const char * hostname, const char * port, struct rocketnet_address_t * address );

/**
 * @brief Returns the platform's preferred local client port.
 *
 * The current implementations return zero, asking socket creation to choose an
 * ephemeral port.
 * @return Preferred port, or zero for an ephemeral port.
 */
uint16_t rocketnet_platform_preferred_client_port( void );

/**
 * @brief Reports whether an IPv6 client socket can also use IPv4-mapped addresses.
 *
 * Call after platform initialization. Some platforms, including OpenBSD,
 * report false because they keep IPv6 sockets IPv6-only.
 * @return true when dual-stack client sockets are supported; otherwise false.
 */
bool rocketnet_platform_client_dual_stack( void );

/**
 * @brief Reports whether this host can create an IPv6 UDP socket.
 *
 * A host with IPv6 disabled (for example ipv6.disable=1 or a container with
 * no IPv6 stack) refuses socket(AF_INET6) with EAFNOSUPPORT. The client reads
 * this before choosing its bind family so an IPv4 server stays reachable.
 * @return true when an IPv6 socket can be created; otherwise false.
 */
bool rocketnet_platform_client_ipv6_available( void );

/**
 * @brief Overrides the IPv6 availability probe for tests.
 *
 * Off unless a test sets it. With false, socket creation for an IPv6 address
 * fails as it would on a host with no IPv6 stack, so the client bind-family
 * choice is exercised on any build host.
 * @param available true to probe the host normally; false to report no IPv6.
 * @return None.
 */
void rocketnet_platform_set_ipv6_socket_available_for_testing( bool available );

/**
 * @brief Chooses the client bind family from the server family and platform.
 *
 * @param server_address_type ROCKETNET_ADDRESS_IPV4 or ROCKETNET_ADDRESS_IPV6.
 * @param ipv6_available Whether an IPv6 socket can be created on this host.
 * @param dual_stack Whether IPv6 sockets also carry IPv4-mapped addresses.
 * @return IPv6 when the server needs it or an IPv4 server can use dual stack;
 * IPv4 when only IPv4 is possible; ROCKETNET_ADDRESS_NONE when the server is
 * IPv6 and this host has no IPv6 stack.
 */
int rocketnet_platform_client_bind_family( int server_address_type, bool ipv6_available, bool dual_stack );

#ifdef __cplusplus
}
#endif

// ----------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates and binds a UDP socket.
 *
 * Call after platform initialization. The address must be IPv4 or IPv6; port
 * zero asks the OS for a port, which is written back to address. The returned
 * object and native socket are owned by the caller and released with
 * socket_destroy.
 * @param context Allocator context passed to RocketNet allocation; NULL uses
 * the default allocator.
 * @param address Writable bind address, modified with the assigned port when its
 * port is zero; must be non-NULL and IPv4 or IPv6.
 * @param socket_type ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING or
 * ROCKETNET_PLATFORM_SOCKET_BLOCKING.
 * @param timeout_seconds Receive timeout in seconds for blocking sockets;
 * positive values request an OS timeout and non-positive values request no
 * timeout. A value rejected by the OS makes creation fail.
 * @param send_buffer_size Requested send-buffer bytes; use a non-negative value.
 * @param receive_buffer_size Requested receive-buffer bytes; use a non-negative value.
 * @return An owned socket, or NULL if allocation, socket setup, buffer setup,
 * bind, or requested mode setup fails.
 */
struct rocketnet_platform_socket_t * rocketnet_platform_socket_create( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size );

// pump-request create (#106): the transport ladder's entry into pump
// selection. pump NULL/"auto" behaves exactly like the plain create above
// (runtime probe); "portable" forces the portable pump;
// "msgx" requires the darwin batch pump or returns NULL loudly.

/**
 * @brief Creates a UDP socket and requests a named packet pump.
 *
 * The address, socket mode, timeout, and buffer arguments follow
 * socket_create. NULL or "auto" uses runtime selection and the
 * platform choice; "portable" requests the portable implementation; "msgx"
 * requires the macOS batch pump.
 * @param context Allocator context; NULL uses the default allocator.
 * @param address Writable bind address, modified with the assigned port when
 * zero; must be non-NULL and IPv4 or IPv6.
 * @param socket_type NON_BLOCKING or BLOCKING.
 * @param timeout_seconds Blocking receive timeout in seconds; positive values
 * request an OS timeout, non-positive values disable it, and a rejected value
 * makes creation fail.
 * @param send_buffer_size Requested send-buffer bytes; use a non-negative value.
 * @param receive_buffer_size Requested receive-buffer bytes; use a non-negative value.
 * @param pump NULL, "auto", "portable", or "msgx"; other values are not
 * documented as supported.
 * @return An owned socket, or NULL when socket setup fails or the requested
 * pump is unavailable.
 */
struct rocketnet_platform_socket_t * rocketnet_platform_socket_create_with_pump( void * context, struct rocketnet_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size, const char * pump );

/**
 * @brief Closes and frees a socket returned by socket_create.
 *
 * Call once after all sends, receives, and wake objects using the socket stop.
 * @param socket Caller-owned socket; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_socket_destroy( struct rocketnet_platform_socket_t * socket );

/**
 * @brief Sends one UDP datagram.
 *
 * @param socket Live socket returned by socket_create; must be non-NULL.
 * @param to IPv4 or IPv6 destination; must be non-NULL.
 * @param packet_data Caller-owned bytes; must be non-NULL and remain readable
 * for the call.
 * @param packet_bytes Datagram length in bytes; must be positive.
 * @return None. Send errors are logged or dropped by the platform layer.
 */
void rocketnet_platform_socket_send_packet( struct rocketnet_platform_socket_t * socket, const struct rocketnet_address_t * to, const void * packet_data, int packet_bytes );

/**
 * @brief Connects a UDP socket to its one remote endpoint.
 *
 * The kernel then drops every datagram from any other source before it is
 * queued, and every send on the socket goes to the remote endpoint: the destination a
 * send names is not passed to the kernel, so the caller sends only to the
 * remote endpoint. A client socket connects once, right after it is created, to the
 * server its connect token selects; a server socket is never connected. An
 * ICMP error for the remote endpoint may then reach a receive, which skips it.
 * @param socket Live socket returned by socket_create; must be non-NULL.
 * @param to IPv4 or IPv6 remote endpoint the socket can carry; must be non-NULL.
 * @return True when connected; false when the kernel refused or the socket
 * cannot carry the address, with the refusal logged.
 */
bool rocketnet_platform_socket_connect( struct rocketnet_platform_socket_t * socket, const struct rocketnet_address_t * to );

/**
 * @brief Receives one UDP datagram into caller storage.
 *
 * @param socket Live socket returned by socket_create; must be non-NULL.
 * @param from Caller-owned output address; must be non-NULL.
 * @param packet_data Caller-owned output buffer; must be non-NULL.
 * @param max_packet_size Buffer capacity in bytes; must be positive.
 * @return Received datagram length in bytes, or zero when no usable packet is
 * available. If the datagram exceeds max_packet_size, only its prefix fits in
 * packet_data and the returned value exceeds the capacity (Windows can report
 * max_packet_size + 1 as an oversize marker). Treat that value as an oversize
 * signal and do not read beyond the buffer. Receive errors have no separate
 * status code.
 */
int rocketnet_platform_socket_receive_packet( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, void * packet_data, int max_packet_size );

// ----------------------------------------------------------------

// The batch pump seam (#100, per #81's matrix): move a batch of packets in
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

/**
 * @brief Describes one datagram in a batch send.
 *
 * The pointed-to address and packet bytes remain caller-owned and must stay
 * valid until socket_send_packets returns. The platform may use a native batch
 * path or loop over single-packet sends; socket_pump reports the live path,
 * including a fallback after a native path degrades.
 */
struct rocketnet_platform_send_t
{
    /** @brief Destination address for this datagram. */
    const struct rocketnet_address_t * to;
    /** @brief Caller-owned bytes to send; valid for the duration of the call. */
    const uint8_t * packet_data;
    /** @brief Datagram length in bytes; the implementation requires a positive value. */
    int packet_bytes;
};

/**
 * @brief Sends a caller-owned batch of UDP datagrams.
 *
 * The owning transport controls thread affinity. This function does not
 * synchronize socket destruction with a concurrent call. Each datagram follows
 * the send_packet error behavior and failures are dropped.
 * @param socket Live socket; must be non-NULL.
 * @param sends Array of num_packets entries; must be non-NULL when count is
 * positive and remain valid until return.
 * @param num_packets Number of entries; must not be negative.
 * @return None.
 */
void rocketnet_platform_socket_send_packets( struct rocketnet_platform_socket_t * socket, const struct rocketnet_platform_send_t * sends, int num_packets );

/**
 * @brief Drains up to a fixed number of datagrams into caller-owned arrays.
 *
 * @param socket Live socket; must be non-NULL.
 * @param from Output array of max_packets source addresses.
 * @param packet_data Output storage with packet_stride bytes per slot.
 * @param packet_stride Slot size and maximum stored prefix in bytes; positive.
 * @param packet_bytes Output array of max_packets datagram lengths. A length
 * may exceed packet_stride when a datagram is oversized; only its prefix is
 * stored in the corresponding packet_data slot.
 * @param max_packets Capacity of all output arrays; positive.
 * @return Number of received datagrams, from zero through max_packets. A
 * returned zero means no usable datagram was received.
 */
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

/**
 * @brief Counts the datagrams this socket's sends dropped.
 *
 * Every sendto, sendmmsg and per-packet tail of a sendmsg_x batch that
 * answers -1 (SOCKET_ERROR on Windows) drops its datagram and increments the
 * count, including the test seam's injected errors. Monotonic for the
 * socket's life.
 * @param socket Live socket; must be non-NULL.
 * @return Number of dropped datagrams since the socket was created.
 */
uint64_t rocketnet_platform_socket_send_failures( struct rocketnet_platform_socket_t * socket );

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
/**
 * @brief Test-only seam: makes the next sends on one socket report an error.
 *
 * Compiled into the development library only. The next count calls to sendto
 * and sendmsg_x on the socket return -1 with error set, without reaching the
 * kernel. A non-positive count clears the seam.
 * @param socket Live socket; must be non-NULL.
 * @param count Number of send calls to fail.
 * @param error errno value each failed call reports.
 * @return None.
 */
void rocketnet_platform_socket_fail_next_sends( struct rocketnet_platform_socket_t * socket, int count, int error );

/**
 * @brief Test-only: the live sockets, oldest first.
 *
 * Development builds only. Lets a functional case find the socket a
 * transport created.
 * @param sockets Caller-owned output array.
 * @param max_sockets Capacity of sockets.
 * @return Number of sockets written.
 */
int rocketnet_platform_sockets_for_testing( struct rocketnet_platform_socket_t ** sockets, int max_sockets );
#endif // #if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS) && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#if defined(ROCKETNET_RECEIVE_TEST_HOOKS)
struct sockaddr_storage;
/**
 * @brief Test-only receive hook: one datagram per call.
 *
 * Writes the datagram into packet_data (at most max_packet_size bytes) and its
 * source into from, and returns the datagram size; -1 reads as "socket drained".
 */
typedef int (*rocketnet_platform_receive_hook_t)( void * user, void * packet_data, int max_packet_size, struct sockaddr_storage * from );

/**
 * @brief Test-only seam: feeds one socket's single-packet receive from a hook.
 *
 * Compiled into the unit suite's hook library only. While set, each read in
 * rocketnet_platform_socket_receive_packet on this socket calls the hook in
 * place of the kernel; other sockets read the kernel. A NULL hook clears it.
 * @param socket Socket the hook applies to.
 * @param hook Datagram source, or NULL to clear.
 * @param user Passed to the hook.
 * @return None.
 */
void rocketnet_platform_socket_set_receive_hook( struct rocketnet_platform_socket_t * socket, rocketnet_platform_receive_hook_t hook, void * user );
#endif // #if defined(ROCKETNET_RECEIVE_TEST_HOOKS)

// the name of the pump path live behind the seam ("portable", "msgx"), for
// logging and for tests that pin the degradation contract

/**
 * @brief Returns the packet-pump path currently selected by a socket.
 *
 * @param socket Live socket; must be non-NULL.
 * @return Borrowed static text naming the currently selected path, such as
 * "portable" or "msgx"; a degraded path is visible here. Do not free it.
 */
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
#endif

// ----------------------------------------------------------------

// The Linux client's batch send/receive (#client-io-linux-mmsg): sendmmsg
// and recvmmsg on the I/O thread behind the same signatures as the seam
// above. Internal, no export, defined only where __linux__ is set.

#if defined(__linux__)
/**
 * @brief Sends a batch with Linux sendmmsg on the caller's I/O thread.
 *
 * This internal Linux entry point has the same arguments and dropped-error
 * behavior as socket_send_packets.
 * @param socket Live socket.
 * @param sends Caller-owned batch array.
 * @param num_packets Number of entries.
 * @return None.
 */
void rocketnet_platform_socket_send_packets_mmsg( struct rocketnet_platform_socket_t * socket, const struct rocketnet_platform_send_t * sends, int num_packets );
/**
 * @brief Receives a batch with Linux recvmmsg on the caller's I/O thread.
 *
 * This internal Linux entry point follows socket_receive_packets layout: an
 * oversized datagram is reported with its true size in packet_bytes, greater
 * than packet_stride, and only its prefix is stored. Zero-length datagrams
 * produce no output entry.
 * @param socket Live socket.
 * @param from Output source-address array.
 * @param packet_data Output storage with packet_stride bytes per slot.
 * @param packet_stride Capacity of each packet slot in bytes.
 * @param packet_bytes Output packet-length array.
 * @param max_packets Capacity of each output array.
 * @return Number of datagrams received, or zero when none are available.
 */
int rocketnet_platform_socket_receive_packets_mmsg( struct rocketnet_platform_socket_t * socket, struct rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );
#endif

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
#if ( ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined(__linux__) ) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#define ROCKETNET_PLATFORM_HAS_WAKE 1
#endif
#if defined(ROCKETNET_PLATFORM_HAS_WAKE) || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
#define ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE 1
#endif
/**
 * @brief Opaque reusable level-triggered wake object, optionally associated with a socket.
 *
 * Create it with wake_create and release it with wake_destroy. The socket must
 * outlive the wake object when provided.
 */
struct rocketnet_platform_wake_t;
/**
 * @brief Creates a wake object, optionally associated with a live socket.
 *
 * @param context Allocator context; NULL uses the default allocator.
 * @param socket Optional live socket that remains open until the wake is destroyed.
 * Pass NULL for an event-only wake. BSD supports this event-only form but
 * continues to refuse socket-backed wakes.
 * @return Owned wake object, or NULL if this platform does not implement wakes
 * or allocation/native wake creation fails.
 */
struct rocketnet_platform_wake_t * rocketnet_platform_wake_create( void * context, struct rocketnet_platform_socket_t * socket );
/** @brief Allocator bytes reserved by one wake object; zero when wakes are unavailable. */
uint64_t rocketnet_platform_wake_allocation_bytes( void );
/**
 * @brief Destroys a wake object and releases its native wake handle.
 *
 * Destroy it before destroying its associated socket, if any.
 * @param wake Caller-owned wake; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_wake_destroy( struct rocketnet_platform_wake_t * wake );
/**
 * @brief Waits for a wake signal, optional socket readability, or timeout.
 *
 * @param wake Live wake object; must be non-NULL.
 * @param readable When true, socket readability also releases the wait; when
 * false, only a wake signal or timeout does. With an event-only wake,
 * readable=true returns false immediately without consuming a pending signal.
 * @param timeout_seconds Timeout in seconds. Non-positive values do not wait
 * on POSIX; Windows converts the value for its native wait API. Avoid values
 * outside the range supported by the target OS.
 * @return true when the wake or requested readability releases the wait;
 * false on timeout or when the native wait reports no event.
 */
bool rocketnet_platform_wake_wait( struct rocketnet_platform_wake_t * wake, bool readable, double timeout_seconds );
/**
 * @brief Signals a wake object so a waiting thread can resume.
 *
 * Any thread may signal it. A signal made before a wait remains pending until
 * consumed by a later wait.
 * @param wake Live wake object; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_wake_set( struct rocketnet_platform_wake_t * wake );

// ----------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts a platform thread running func(arg).
 *
 * @param context Allocator context; NULL uses the default allocator.
 * @param func Thread function; must be non-NULL.
 * @param arg User pointer passed unchanged; may be NULL.
 * @return Owned thread handle, or NULL if allocation or native thread creation
 * fails. Join the thread before destroying its handle.
 */
struct rocketnet_platform_thread_t * rocketnet_platform_thread_create( void * context, rocketnet_platform_thread_func_t func, void * arg );

/**
 * @brief Waits until a created platform thread has returned.
 *
 * Call once for a live thread, from a different thread than the one being
 * joined.
 * @param thread Live thread handle; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_thread_join( struct rocketnet_platform_thread_t * thread );

/**
 * @brief Frees a thread handle after its thread has been joined.
 *
 * @param thread Joined thread handle; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_thread_destroy( struct rocketnet_platform_thread_t * thread );

/**
 * @brief Waits on the calling thread's wake for at most the given duration.
 *
 * Returns when the duration passes or when rocketnet_platform_thread_wake has
 * been called for the calling thread; once woken, every later wait returns at
 * once. The wake is an explicit signal, so a woken thread does not depend on
 * its wait's timeout firing. POSIX waits on a condition variable, macOS and
 * iOS on a mach semaphore (the primitive usleep waits on there, so the
 * per-wait cost matches usleep). Windows sleeps for the duration and its wake
 * is a no-op, so a Windows thread observes a quit flag within one wait. Called
 * on a thread that thread_create did not start, it sleeps for the duration.
 * @param time Requested seconds; must be positive.
 * @return None.
 */
void rocketnet_platform_thread_wait( double time );

/**
 * @brief Wakes a live thread's current thread_wait and every later one.
 *
 * Any thread may call it, before or during the target's wait. A thread that
 * polls a quit flag between waits stores the flag first, then wakes, then
 * joins.
 * @param thread Live thread handle; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_thread_wake( struct rocketnet_platform_thread_t * thread );

/**
 * @brief Requests the platform's client-thread scheduling priority.
 *
 * Call on a live thread. Operating-system permissions can prevent the request;
 * the API does not return that status.
 * @param thread Live thread handle; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_client_thread_priority( struct rocketnet_platform_thread_t * thread );

/**
 * @brief Requests the platform's server-thread scheduling priority.
 *
 * Call on a live thread. Operating-system permissions can prevent the request;
 * the API does not return that status.
 * @param thread Live thread handle; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_server_thread_priority( struct rocketnet_platform_thread_t * thread );

/**
 * @brief The operating-system calls behind the network-thread priority.
 *
 * Each platform fills the table with the real calls; a unit test replaces it
 * with a recording fake through rocketnet_platform_set_priority_calls_for_testing.
 * An entry a platform does not use is a stub that does nothing.
 */
struct rocketnet_platform_mutex_t;

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
    /** @brief Windows: CreateWaitableTimerExW; NULL on failure. Off Windows the handle is unused and the entry returns a non-NULL placeholder, so NULL stays the failure answer on every platform. */
    void* ( *create_waitable_timer )( unsigned flags );
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

/**
 * @brief Opaque platform mutex storage.
 *
 * Create the object before locking it; destroy it after all users stop.
 */
struct rocketnet_platform_mutex_t;

/**
 * @brief Initializes caller-provided storage as a mutex.
 *
 * @param mutex Caller-owned storage; must be non-NULL and not already
 * initialized.
 * @return true when native mutex creation succeeds; false when it fails.
 */
bool rocketnet_platform_mutex_create( struct rocketnet_platform_mutex_t * mutex );

/**
 * @brief Destroys an initialized mutex and clears its storage.
 *
 * No thread may hold or wait on the mutex during destruction.
 * @param mutex Initialized mutex storage; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_mutex_destroy( struct rocketnet_platform_mutex_t * mutex );

/**
 * @brief Locks an initialized mutex, waiting until it becomes available.
 *
 * Do not acquire the same non-recursive mutex twice on one thread.
 * @param mutex Successfully initialized mutex; must be non-NULL.
 * @return None.
 */
void rocketnet_platform_mutex_acquire( struct rocketnet_platform_mutex_t * mutex );

// Acquires only when immediately available; never waits for the owner.
bool rocketnet_platform_mutex_try_acquire( struct rocketnet_platform_mutex_t * mutex );

/**
 * @brief Unlocks a mutex held by the calling thread.
 *
 * @param mutex Successfully initialized mutex held by this thread.
 * @return None.
 */
void rocketnet_platform_mutex_release( struct rocketnet_platform_mutex_t * mutex );

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

/**
 * @brief Locks a mutex on construction and unlocks it on destruction.
 *
 * This C++ scope guard releases the mutex when its local scope ends.
 */
struct rocketnet_platform_mutex_helper_t
{
    /** @brief Mutex locked for this helper's lifetime. */
    rocketnet_platform_mutex_t * mutex;
#if ROCKETNET_SPIKE_TRACKING
    const char * file;
    int line;
    double start_time;
    rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * held_mutex, const char * file, int line );
#else // #if ROCKETNET_SPIKE_TRACKING
    rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * held_mutex );
#endif // #if ROCKETNET_SPIKE_TRACKING
    ~rocketnet_platform_mutex_helper_t();
};

#if ROCKETNET_SPIKE_TRACKING
#define rocketnet_platform_mutex_guard( _mutex ) rocketnet_platform_mutex_helper_t __mutex_helper( _mutex, __FILE__, __LINE__ )
#else // #if ROCKETNET_SPIKE_TRACKING
#define rocketnet_platform_mutex_guard( _mutex ) rocketnet_platform_mutex_helper_t __mutex_helper( _mutex )
#endif // #if ROCKETNET_SPIKE_TRACKING

#if ROCKETNET_SPIKE_TRACKING
inline rocketnet_platform_mutex_helper_t::rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * held_mutex, const char * file, int line ) : mutex(held_mutex), file(file), line(line), start_time(rocketnet_platform_time())
#else // #if ROCKETNET_SPIKE_TRACKING
inline rocketnet_platform_mutex_helper_t::rocketnet_platform_mutex_helper_t( rocketnet_platform_mutex_t * held_mutex ) : mutex(held_mutex)
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

    /**
 * @brief Stores a native POSIX socket descriptor.
 */
    typedef int rocketnet_platform_socket_handle_t;

    /** @brief Holds the platform socket and its allocator context. */
    struct rocketnet_platform_socket_t
    {
        /** @brief Allocator context used to free this socket. */
        void * context;
        /** @brief Blocking or non-blocking mode selected at creation. */
        int type;
        /** @brief True when the socket was created for an IPv6 bind address. */
        bool ipv6;
        /** @brief True once rocketnet_platform_socket_connect connected it to its one remote endpoint. */
        bool connected;
        /** @brief Native socket descriptor owned by this object. */
        rocketnet_platform_socket_handle_t handle;
        /** @brief Datagrams a failed send dropped; atomic, monotonic. */
        uint64_t send_failures;
    };

struct sockaddr_storage;
bool rocketnet_platform_mmsg_read_sockaddr( struct rocketnet_platform_socket_t * socket, const struct sockaddr_storage * storage, struct rocketnet_address_t * from );

    /** @brief Holds a native thread and its allocator context. */
    struct rocketnet_platform_thread_t
    {
        /** @brief Allocator context used to free the thread handle. */
        void * context;
        /** @brief Native thread handle. */
        pthread_t handle;
        /** @brief Guards woken for thread_wait and thread_wake. */
        pthread_mutex_t wake_mutex;
        /** @brief Signalled by thread_wake; waited on by thread_wait. */
        pthread_cond_t wake_cond;
        /** @brief Set once by thread_wake; never cleared. */
        bool woken;
    };

    /** @brief Stores a POSIX mutex and whether its initialization succeeded. */
    struct rocketnet_platform_mutex_t
    {
        /** @brief True only after native mutex initialization succeeds. */
        bool ok;
        /** @brief Native non-recursive mutex handle. */
        pthread_mutex_t handle;
    };

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    // macos and ios

    #include <pthread.h>
    #include <unistd.h>
    #include <mach/mach_types.h>

    typedef int rocketnet_platform_socket_handle_t;

    struct rocketnet_platform_socket_t
    {
        void * context;
        bool ipv6;
        bool msgx;                  // the batched sendmsg_x/recvmsg_x pump is eligible (first batch call probes support, macOS only)
        bool connected;             // connected to its one remote endpoint (rocketnet_platform_socket_connect); sends name no destination
        double msgx_down_until;     // a hard refusal pauses the msgx pump until this time; the next batch call after it is the re-probe
        rocketnet_platform_socket_handle_t handle;
        uint64_t send_failures;     // datagrams a failed send dropped; atomic, monotonic
    };

struct sockaddr_storage;
bool rocketnet_msgx_read_sockaddr( struct rocketnet_platform_socket_t * socket, const struct sockaddr_storage * storage, struct rocketnet_address_t * from );

    struct rocketnet_platform_thread_t
    {
        void * context;
        pthread_t handle;
        mach_port_t wake_semaphore;     // a mach semaphore: thread_wait waits on it, thread_wake signals it
        int woken;                      // set once by thread_wake (atomic store), never cleared
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

    #if _WIN64
    typedef uint64_t rocketnet_platform_socket_handle_t;
    #else
    typedef _W64 unsigned int rocketnet_platform_socket_handle_t;
    #endif

    struct rocketnet_platform_socket_t
    {
        void * context;
        bool ipv6;
        bool connected;             // connected to its one remote endpoint (rocketnet_platform_socket_connect); sends name no destination
        rocketnet_platform_socket_handle_t handle;
        uint64_t send_failures;     // datagrams a failed send dropped; atomic, monotonic
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

static inline bool rocketnet_platform_socket_handle_is_valid( rocketnet_platform_socket_handle_t handle )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return handle != (rocketnet_platform_socket_handle_t) INVALID_SOCKET;
#else
    return handle >= 0;
#endif
}

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

    /**
     * @brief Atomically adds an unsigned value and returns the previous value.
     *
     * @param value Aligned, initialized shared word; must be non-NULL.
     * @param add Amount added, including zero.
     * @return The value observed before the addition.
     */
    inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
    {
        return (uint64_t) _InterlockedExchangeAdd64( (volatile long long *) value, (long long) add );
    }

    /**
     * @brief Atomically reads an unsigned 64-bit shared word.
     *
     * @param value Aligned, initialized shared word; must be non-NULL.
     * @return The value observed at read time. The read has relaxed ordering.
     */
    inline uint64_t rocketnet_platform_atomic_load_u64( volatile uint64_t * value )
    {
    #if defined(_M_IX86)
        // 32-bit x86 has no single-instruction 64-bit load; CAS with the
        // observed value (a self-write) retries until it reads consistent.
        long long observed = *(volatile long long *) value;
        while ( _InterlockedCompareExchange64( (volatile long long *) value, observed, observed ) != observed )
            observed = *(volatile long long *) value;
        return (uint64_t) observed;
    #else
        return (uint64_t) _InterlockedCompareExchange64( (volatile long long *) value, 0, 0 );
    #endif
    }

    /**
     * @brief Atomically publishes an unsigned 64-bit value.
     *
     * @param value Aligned shared word; must be non-NULL.
     * @param store Value to publish.
     * @return None. The operation has relaxed ordering.
     */
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

    /**
     * @brief Atomically publishes the bit pattern of a double value.
     *
     * @param value Aligned shared double; must be non-NULL.
     * @param store Value to publish.
     * @return None. The operation has relaxed ordering.
     */
    inline void rocketnet_platform_atomic_store_double( volatile double * value, double store )
    {
        uint64_t bits;
        memcpy( &bits, &store, 8 );
        rocketnet_platform_atomic_store_u64( (volatile uint64_t *) value, bits );
    }

    // folds value = max(value, candidate) with a compare-exchange loop, so
    // concurrent folds from any number of threads leave the largest

    /**
     * @brief Atomically raises a shared value to at least candidate.
     *
     * @param value Aligned, initialized shared word; must be non-NULL.
     * @param candidate Candidate high-water value.
     * @return None.
     */
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

    /**
     * @brief Atomically adds an unsigned value and returns the previous value.
     * @param value Aligned, initialized shared word; non-NULL.
     * @param add Amount added, including zero.
     * @return Value observed before the addition.
     */
    inline uint64_t rocketnet_platform_atomic_add_u64( volatile uint64_t * value, uint64_t add )
    {
        return __atomic_fetch_add( value, add, __ATOMIC_RELAXED );
    }

    /**
     * @brief Atomically reads an unsigned 64-bit shared word.
     *
     * @param value Aligned, initialized shared word; non-NULL.
     * @return The value observed at read time. The read has relaxed ordering.
     */
    inline uint64_t rocketnet_platform_atomic_load_u64( volatile uint64_t * value )
    {
        return __atomic_load_n( value, __ATOMIC_RELAXED );
    }

    /**
     * @brief Atomically publishes an unsigned 64-bit value.
     * @param value Aligned shared word; non-NULL.
     * @param store Value to publish.
     * @return None. This operation has relaxed ordering.
     */
    inline void rocketnet_platform_atomic_store_u64( volatile uint64_t * value, uint64_t store )
    {
        __atomic_store_n( value, store, __ATOMIC_RELAXED );
    }

    /**
     * @brief Atomically publishes the bit pattern of a double.
     * @param value Aligned shared double; non-NULL.
     * @param store Value to publish.
     * @return None. This operation has relaxed ordering.
     */
    inline void rocketnet_platform_atomic_store_double( volatile double * value, double store )
    {
        uint64_t bits;
        memcpy( &bits, &store, 8 );
        __atomic_store_n( (volatile uint64_t *) value, bits, __ATOMIC_RELAXED );
    }

    // folds value = max(value, candidate) with a compare-exchange loop, so
    // concurrent folds from any number of threads leave the largest

    /**
     * @brief Atomically raises a shared value to at least candidate.
     * @param value Aligned, initialized shared word; non-NULL.
     * @param candidate Candidate high-water value.
     * @return None.
     */
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
/**
 * @brief Spins until the shared lock word changes from zero to one.
 *
 * @param value Aligned lock word initialized to zero; must be non-NULL.
 * @return None. The call blocks by spinning until the lock is acquired.
 */
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

/**
 * @brief Releases a shared lock word acquired by spin_lock_u64.
 *
 * @param value Lock word held by the caller; must be non-NULL.
 * @return None.
 */
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
// of spinning. These internal C++ types and the declaration are linux-only.
//
// ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined(__cplusplus)

#include "rocketnet_route.h"
#include <limits.h>
#include <stdint.h>
#include <sys/socket.h>
#include <linux/netlink.h>

/**
 * @brief Holds the process-wide allocator context used by platform helpers.
 *
 * Set from the initialization context before platform discovery and cleared
 * during failed initialization or termination. It is an internal borrowed
 * pointer and is not an allocation owned by this variable.
 */
extern void * rocketnet_global_context;

/**
 * @brief Forward-declares the internal vector type used by the route probe.
 *
 * The definition is private to the implementation.
 */
template <typename T> struct rocketnet_vector_t;
// Reads the kernel route table dump over netlink until an NLMSG_DONE message
// ends it; returns true when the dump was read in full, false when a receive
// fails or returns zero. receive is the recvmsg call, passed so tests can
// drive the probe without a socket. get_connection_type maps a false result
// to ROCKETNET_CONNECTION_TYPE_UNKNOWN.
/**
 * @brief Reads a Linux route dump through a caller-supplied receive function.
 *
 * This internal C++ helper consumes messages until NLMSG_DONE and supports
 * deterministic tests without opening a socket.
 * @param sock Netlink socket descriptor.
 * @param kernel Kernel netlink address.
 * @param route_table Caller-owned output vector.
 * @param receive Receive function used to obtain each message.
 * @return true when the complete dump is read; false when receiving fails or
 * ends before completion.
 */
bool rocketnet_platform_read_route_table(
    int sock,
    struct sockaddr_nl * kernel,
    rocketnet_vector_t<rocketnet_ifforward4_t> * route_table,
    ssize_t (*receive)( int, struct msghdr *, int ) );

#endif // linux internal C++ route probe
