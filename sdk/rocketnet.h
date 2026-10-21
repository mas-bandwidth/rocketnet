/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

/// RocketNet provides a C API for clients and servers that exchange UDP
/// messages.
///
/// Main frame calls take less than 0.1 ms each. Create calls may take longer.
/// Client disconnect and destroy wait for the network thread to exit. These
/// waits have no promised time bound.
///
/// Buffer sizes use size_t.
/// Message sizes, counts, and client indexes use int.
/// Destination IDs use uint64_t.
///
/// Lookup and time calls are thread-safe. Client and server objects are not.

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/// Disable the harmless MSVC C4514 warning for unused inline functions.

#if defined( _MSC_VER )
#pragma warning( disable : 4514 )
#endif


/// ROCKETNET_DEVELOPMENT is 1 when linking a development library.
/// Development libraries provide rocketnet_create_dev_connect_token.
/// They also provide ROCKETNET_DEV_BACKEND_PUBLIC_KEY.
/// They accept the published development backend key.

#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 0
#endif // !defined( ROCKETNET_DEVELOPMENT )

#if !ROCKETNET_DEVELOPMENT

    /// The SDK version as text. The release version string, or "dev" in a
    /// development build.

    #define ROCKETNET_VERSION_FULL                    "v1.0.0-preview-1"

    /// The major component of the SDK version. Development builds use 255.

    #define ROCKETNET_VERSION_MAJOR                                    1

    /// The minor component of the SDK version. Development builds use 255.

    #define ROCKETNET_VERSION_MINOR                                    0

    /// The patch component of the SDK version. Development builds use 255.

    #define ROCKETNET_VERSION_PATCH                                    0

#else // !ROCKETNET_DEVELOPMENT

    #define ROCKETNET_VERSION_FULL                                 "dev"
    #define ROCKETNET_VERSION_MAJOR                                  255
    #define ROCKETNET_VERSION_MINOR                                  255
    #define ROCKETNET_VERSION_PATCH                                  255

#endif // !ROCKETNET_DEVELOPMENT

/// The version as one comparable integer.
/// The formula is major * 10000 + minor * 100 + patch.
/// Version 1.0.0 is 10000. Prerelease labels are not included.
/// ROCKETNET_VERSION_FULL distinguishes preview releases.
/// Development builds use 2575755.
/// rocketnet_version returns the library's value.

#define ROCKETNET_VERSION   ( ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH )


/// Define ROCKETNET_SHARED when linking the shared library DLL. Static links
/// define nothing. ROCKETNET_BUILDING_DLL is for library builds.

#if defined( _WIN32 ) && defined( ROCKETNET_BUILDING_DLL )
    #define ROCKETNET_EXPORT __declspec(dllexport)
#elif defined( _WIN32 ) && defined( ROCKETNET_SHARED )
    #define ROCKETNET_EXPORT __declspec(dllimport)
#elif ( defined( ROCKETNET_SHARED ) || defined( ROCKETNET_BUILDING_DLL ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
    #define ROCKETNET_EXPORT __attribute__((visibility("default")))
#else
    #define ROCKETNET_EXPORT
#endif


/// Maximum clients per server.

#define ROCKETNET_MAX_CLIENTS                                       1000

/// A destination ID is a uint64_t client index.
/// Valid client indexes range from 0 through max_clients - 1.
/// ROCKETNET_FLOW_ID_BIT is reserved.
/// rocketnet_server_start_message refuses IDs with this bit set.
/// The refusal counts as API misuse.

#define ROCKETNET_FLOW_ID_BIT                                       ( 1ULL << 63 )

/// Maximum UDP payload bytes per packet.
/// This fits an IPv6 datagram within a 1500-byte MTU.

#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 40 - 8 )

/// Maximum application payload bytes per packet.
/// The value excludes RocketNet packet overhead.
/// The value is a multiple of eight.

#define ROCKETNET_MAX_PACKET_PAYLOAD_BYTES                 1416

/// Maximum message bytes that fit in one packet.
/// The limit is the same for every reliability type.
/// Larger unreliable messages use the matching reliable type.
/// UNRELIABLE becomes RELIABLE.
/// UNRELIABLE_SEQUENCED becomes RELIABLE_SEQUENCED on its channel.
/// UNRELIABLE_WITH_ACK_RECEIPT becomes RELIABLE_WITH_ACK_RECEIPT.

#define ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES             1411

/// Connect-token buffer bytes.
/// The buffer holds 1024 base64 characters and the terminating NUL.

#define ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES           1025

/// Connect-token user-data bytes.

#define ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES            256

/// Packet-filter bytes in each config.

#define ROCKETNET_PACKET_FILTER_BYTES                       4

/// Backend signing public-key bytes.

#define ROCKETNET_BACKEND_PUBLIC_KEY_BYTES                  32

/// Server public-key bytes.
/// rocketnet_server_public_key copies this many bytes.

#define ROCKETNET_SERVER_PUBLIC_KEY_BYTES                   32

/// Maximum address-string bytes including the NUL.
/// IPv4 uses "a.b.c.d:port". IPv6 uses "[v6]:port".
/// Longer input is refused.

#define ROCKETNET_MAX_ADDRESS_STRING_BYTES                 256

/// Required memory alignment in bytes.

#define ROCKETNET_ALLOCATION_ALIGNMENT                     64

/// Maximum channels.
/// Define it before including rocketnet.h.
/// Build the library with the same value.

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                             32
#endif

/// The default client to server send budget, in kbps, used when the client
/// config's send_kbps is 0.

#define ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS            1000

/// The default server to client send budget per client, in kbps, used when the
/// server config's per_client_send_kbps is 0.

#define ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS            10000

/// The send_kbps value for no send budget on a connection.

#define ROCKETNET_SEND_KBPS_UNLIMITED                      0x7fffffff

#ifdef __cplusplus
extern "C" {
#endif

/// Status-returning calls use these values.
/// OK is zero and the only success value.
/// Every failure is negative. Test status < 0 for a failure.
///
/// Create calls return NULL when refused. Finish-message calls return 0 when
/// refused. Required-memory calls return 0 when refused. Stats calls fill a
/// caller-owned struct.

enum rocketnet_status_t
{
    /// Success.

    ROCKETNET_STATUS_OK                                 =  0,

    /// rocketnet_init came after the library was initialized, or while it is
    /// initializing or terminating.

    ROCKETNET_STATUS_ALREADY_INITIALIZED                = -1,

    /// A call that needs rocketnet_init came before it, or a term found nothing
    /// to terminate.

    ROCKETNET_STATUS_NOT_INITIALIZED                    = -2,

    /// Term, or init with a new allocator, refused. The library still holds
    /// allocations.

    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = -3,

    /// The platform's socket layer did not start. Correct the platform setup
    /// before retrying.

    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = -4,

    /// The crypto library did not start. Initialization does not complete.

    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = -5,

    /// An argument, or a combination of arguments, is NULL, out of range, or
    /// inconsistent with another.

    ROCKETNET_STATUS_INVALID_ARGUMENT                   = -6,

    /// An environment override is malformed. It did not parse.

    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = -7,

    /// A config field, from the struct or the environment, is out of its
    /// supported range.

    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = -8,

    /// The memory the config needs is past the operator's memory budget.

    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = -9,

    /// Start refused. The server config's backend_public_key is all zeros.

    ROCKETNET_STATUS_MISSING_KEYS                       = -10,

    /// The protocol id is 0, or not the expected value.

    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = -11,

    /// Start refused. A release build was given the published development
    /// backend key.

    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = -12,

    /// An address string did not parse or cannot be used.

    ROCKETNET_STATUS_INVALID_ADDRESS                    = -13,

    /// The transport candidate list did not parse, or holds no usable
    /// candidate.

    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = -14,

    /// The connect token fails the client's pre-send checks. It did not parse,
    /// or is not for this protocol.

    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = -15,

    /// The connect token expired before the first packet was sent. Obtain a
    /// fresh token before connecting.

    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = -16,

    /// A pinned transport cannot take port 0, an ephemeral port.

    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = -17,

    /// Every transport in the candidate list refused to start.

    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = -18,

    /// An allocation the operation needs failed.

    ROCKETNET_STATUS_OUT_OF_MEMORY                      = -19,

    /// The platform refused a mutex.

    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = -20,

    /// The platform refused a thread.

    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = -21,

    /// The platform refused a timer.

    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = -22,

    /// Connect refused because the client is not disconnected. Nothing changed.
    /// Call rocketnet_client_disconnect before retrying.

    ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED            = -23,

    /// Start refused, nothing changed. The server is already running.

    ROCKETNET_STATUS_SERVER_NOT_STOPPED                 = -24,

    /// A destination id has the reserved ROCKETNET_FLOW_ID_BIT set. The send is
    /// refused.

    ROCKETNET_STATUS_FLOW_ID_RESERVED                   = -25,

    /// A transport cannot serve both named families on this machine.

    ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED     = -26,

    /// A config's config_bytes is 0 or too small. Fill the config with its
    /// default_config call first.

    ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED             = -27
};


/// The process-wide log levels, least to most verbose.

enum rocketnet_log_level_t
{
    /// As the maximum level, only lines logged at NONE are emitted. Quiet mode
    /// suppresses every line.

    ROCKETNET_LOG_LEVEL_NONE                            = 0,

    /// Errors, the least verbose emitted level.

    ROCKETNET_LOG_LEVEL_ERROR                           = 1,

    /// Warnings.

    ROCKETNET_LOG_LEVEL_WARN                            = 2,

    /// Information.

    ROCKETNET_LOG_LEVEL_INFO                            = 3,

    /// Debugging.

    ROCKETNET_LOG_LEVEL_DEBUG                           = 4,

    /// The most verbose level.

    ROCKETNET_LOG_LEVEL_SPAM                            = 5
};


/// The address family of an endpoint.

enum rocketnet_address_type_t
{
    /// No address family.

    ROCKETNET_ADDRESS_NONE                              = 0,

    /// IPv4.

    ROCKETNET_ADDRESS_IPV4                              = 1,

    /// IPv6.

    ROCKETNET_ADDRESS_IPV6                              = 2
};


/// A send's delivery, ordering, and acknowledgement behavior.
/// Ordered and sequenced types use the send's channel.
/// These types require a channel below num_channels.
/// Other types ignore the channel and use channel 0.
/// Any channel value is accepted for those types.

enum rocketnet_reliability_t
{
    /// Sent once without a delivery guarantee, order, or receipt.
    /// The receiver sees the message at most once.
    /// A lost packet loses the message.

    ROCKETNET_RELIABILITY_UNRELIABLE                            = 0,

    /// Sent once without a delivery guarantee.
    /// The receiver delivers only newer messages on the channel.
    /// It drops older messages.

    ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED                  = 1,

    /// Resent until the peer acknowledges it.
    /// Delivered once without channel ordering.

    ROCKETNET_RELIABILITY_RELIABLE                              = 2,

    /// Resent until the peer acknowledges it.
    /// Delivered once in channel order.
    /// The receiver holds later messages until earlier ones arrive.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED                      = 3,

    /// Resent until the peer acknowledges it. The receiver delivers only the
    /// newest message on the channel and can drop a stale one.

    ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED                    = 4,

    /// As UNRELIABLE, with a receipt. ACKED or LOSS.

    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT           = 5,

    /// As RELIABLE, with an ACKED receipt.

    ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT             = 6,

    /// As RELIABLE_ORDERED, with an ACKED receipt.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT     = 7
};

/// A receipt's outcome, the status field of struct rocketnet_receipt_t.

enum rocketnet_receipt_status_t
{
    /// A zero-filled receipt. No outcome has been written.

    ROCKETNET_RECEIPT_STATUS_NONE                               = 0,

    /// The peer acknowledged the message. This does not mean the peer
    /// application read it.

    ROCKETNET_RECEIPT_STATUS_ACKED                              = 1,

    /// The message was not acknowledged, and it is not resent.

    ROCKETNET_RECEIPT_STATUS_LOSS                               = 2
};

/// Why the connection in a server's client slot ended. What
/// rocketnet_server_disconnect_reason returns. A connection that ended reads a
/// negative reason. A client reads how its own connection ended from
/// rocketnet_client_state.

enum rocketnet_disconnect_reason_t
{
    /// No connection has ended, or the connection is up.

    ROCKETNET_DISCONNECT_REASON_NONE                            =  0,

    /// The two ends' configs differ in a field that must match on both ends.

    ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH                 = -1,

    /// The peer sent what the protocol forbids.

    ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION              = -2,

    /// A reliable message went unacknowledged past reliable_message_timeout_ms.

    ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT              = -3,

    /// The connection's message memory had no room for a message it must not
    /// drop.

    ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED        = -4,

    /// The server disconnected the client, stopped, or was destroyed.

    ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED             = -5
};


/// rocketnet_client_state returns one of these values.
/// A connect attempt moves from DISCONNECTED through the handshake states to
/// CONNECTED. Negative values describe how an attempt or connection ended.
/// rocketnet_client_disconnect clears a negative value to DISCONNECTED.
/// rocketnet_client_connect may replace a connection that ended after reaching
/// CONNECTED. Call rocketnet_client_disconnect after other failures.

enum rocketnet_client_state_t
{
    /// The connection's message memory had no room for a message it must not
    /// drop.

    ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED         = -12,

    /// The two ends' configs differ in a field that must match on both ends.

    ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH                  = -11,

    /// The server sent what the protocol forbids.

    ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION               = -10,

    /// A reliable message went unacknowledged past reliable_message_timeout_ms.

    ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT               = -9,

    /// The server disconnected the client, stopped, or was destroyed.

    ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED              = -8,

    /// The connect token expired before the server accepted the client.

    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            = -7,

    /// The connect token was refused before the first packet was sent.

    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            = -6,

    /// A connection heard nothing from the server for the token's timeout.

    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             = -5,

    /// The server did not answer the third handshake leg in time.

    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    = -4,

    /// The server did not answer the connection request in time.

    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     = -3,

    /// The server did not answer the cookie request in time.

    ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT         = -2,

    /// The server refused the client.

    ROCKETNET_CLIENT_STATE_CONNECTION_DENIED                = -1,

    /// After create, and after rocketnet_client_disconnect.

    ROCKETNET_CLIENT_STATE_DISCONNECTED                     =  0,

    /// Handshake, first leg.

    ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE                =  1,

    /// Handshake, second leg.

    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST       =  2,

    /// Handshake, third leg.

    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE      =  3,

    /// Connected. Messages flow both ways.

    ROCKETNET_CLIENT_STATE_CONNECTED                        =  4
};


/// rocketnet_client_stats fills this struct.
/// Per-client counts start at 0 when the client is created.
/// Counts accumulate across connections.
/// api_misuses and thread_priority_refusals are process-wide.
/// Process-wide counters do not reset.
/// Rates cover the trailing second.
/// Connection measurements are 0 without a connection.

struct rocketnet_client_stats_t
{
    /// UDP datagrams sent, handshake included.

    uint64_t packets_sent;

    /// UDP datagrams received, handshake included.

    uint64_t packets_received;

    /// UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_sent;

    /// UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_received;

    /// Packets from the server that opened and carried messages.

    uint64_t payloads_received;

    /// Reliable messages resent.

    uint64_t messages_resent;

    /// Reliable messages received a second time and dropped.

    uint64_t messages_duplicated;

    /// Sends with a receipt that the server acknowledged.

    uint64_t receipts_acked;

    /// Sends with a receipt that were lost.

    uint64_t receipts_lost;

    /// Outcomes dropped oldest first because the receipt queue was full.

    uint64_t receipts_dropped;

    /// Unreliable messages dropped because the send budget was spent.

    uint64_t messages_dropped_over_budget;


    /// Sends dropped because the connection was not CONNECTED.

    uint64_t messages_dropped_not_connected;

    /// Process-wide. API misuses since the process started.

    uint64_t api_misuses;

    /// Process-wide. Thread priority and timer period calls the operating
    /// system refused since the process started.

    uint64_t thread_priority_refusals;

    /// Round-trip time in milliseconds.

    double rtt;

    /// Receive jitter in milliseconds.
    /// Moving average of changes in the interval between packet arrivals.

    double jitter;

    /// Receive-side packet loss as a percentage.
    /// Missing share of the server's packet sequence over the trailing second.

    double packet_loss;

    /// Packets per second sent.

    double packets_sent_per_second;

    /// Packets per second received.

    double packets_received_per_second;

    /// Kilobits per second sent.

    double bandwidth_sent_kbps;

    /// Kilobits per second received.

    double bandwidth_received_kbps;
};

/// rocketnet_server_stats fills this struct for all clients.
/// Per-server counts start at 0 when the server is created.
/// Counts accumulate across starts and stops.
/// api_misuses and thread_priority_refusals are process-wide.
/// Process-wide counters do not reset.
/// Rates cover the whole server over the trailing second.
/// Rates are 0 while the server is stopped.

struct rocketnet_server_stats_t
{
    /// UDP datagrams sent.

    uint64_t packets_sent;

    /// UDP datagrams received.

    uint64_t packets_received;

    /// UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_sent;

    /// UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_received;

    /// Clients that completed the handshake.

    uint64_t connections_accepted;

    /// Connect attempts refused because every client slot was taken.

    uint64_t connections_denied_full;

    /// Connect attempts dropped by the per-update handshake budget.

    uint64_t handshakes_dropped_over_budget;

    /// Packets from clients that opened and carried messages.

    uint64_t payloads_received;

    /// Reliable messages resent.

    uint64_t messages_resent;

    /// Reliable messages received a second time and dropped.

    uint64_t messages_duplicated;

    /// Sends with a receipt that a client acknowledged.

    uint64_t receipts_acked;

    /// Sends with a receipt that were lost.

    uint64_t receipts_lost;

    /// Outcomes dropped oldest first because a client's receipt queue was
    /// full.

    uint64_t receipts_dropped;

    /// Unreliable messages dropped because a client's send budget was spent.

    uint64_t messages_dropped_over_budget;

    /// Sends dropped because the connection was not CONNECTED.

    uint64_t messages_dropped_not_connected;

    /// Process-wide. API misuses since the process started.

    uint64_t api_misuses;

    /// Process-wide. Thread priority and timer period calls the operating
    /// system refused since the process started.

    uint64_t thread_priority_refusals;

    /// Packets per second sent.

    double packets_sent_per_second;

    /// Packets per second received.

    double packets_received_per_second;

    /// Kilobits per second sent.

    double bandwidth_sent_kbps;

    /// Kilobits per second received.

    double bandwidth_received_kbps;
};

/// rocketnet_server_client_stats fills this struct for one client slot.
/// Values cover the current connection in the slot.
/// A new connection resets the values to 0.
/// Rates cover the trailing second.

struct rocketnet_server_client_stats_t
{
    /// UDP datagrams sent to this client.

    uint64_t packets_sent;

    /// UDP datagrams received from this client.

    uint64_t packets_received;

    /// UDP payload plus the IP and UDP headers of this client's family.

    uint64_t bytes_sent;

    /// UDP payload plus the IP and UDP headers of this client's family.

    uint64_t bytes_received;

    /// Round-trip time in milliseconds.
    /// This is the smallest recent sample.

    double rtt;

    /// Round-trip jitter in milliseconds.
    /// This is the mean excess over the smallest recent sample.

    double jitter;

    /// Send-side packet loss as a percentage.
    /// This is the unacknowledged share of recent packets sent to this client.

    double packet_loss;

    /// Kilobits per second sent to this client.

    double bandwidth_sent_kbps;

    /// Kilobits per second received from this client.

    double bandwidth_received_kbps;

    /// Kilobits per second of sent packets this client acknowledged.

    double bandwidth_acked_kbps;
};

/// Every public enum has the same size as int.

#if defined( __cplusplus ) && __cplusplus >= 201103L
#if defined( __clang__ )
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif
static_assert( sizeof( enum rocketnet_status_t ) == sizeof( int ) && sizeof( enum rocketnet_log_level_t ) == sizeof( int ) &&
               sizeof( enum rocketnet_reliability_t ) == sizeof( int ) && sizeof( enum rocketnet_receipt_status_t ) == sizeof( int ) &&
               sizeof( enum rocketnet_disconnect_reason_t ) == sizeof( int ) && sizeof( enum rocketnet_client_state_t ) == sizeof( int ),
               "RocketNet's public enums are the size of int: build without -fshort-enums" );
#if defined( __clang__ )
#pragma clang diagnostic pop
#endif
#elif defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 201112L
#if defined( __clang__ )
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wpre-c11-compat"
#endif
_Static_assert( sizeof( enum rocketnet_status_t ) == sizeof( int ) && sizeof( enum rocketnet_log_level_t ) == sizeof( int ) &&
                sizeof( enum rocketnet_reliability_t ) == sizeof( int ) && sizeof( enum rocketnet_receipt_status_t ) == sizeof( int ) &&
                sizeof( enum rocketnet_disconnect_reason_t ) == sizeof( int ) && sizeof( enum rocketnet_client_state_t ) == sizeof( int ),
                "RocketNet's public enums are the size of int: build without -fshort-enums" );
#if defined( __clang__ )
#pragma clang diagnostic pop
#endif
#endif


struct rocketnet_client_t;
struct rocketnet_server_t;

/// Lifecycle and logging

/// Returns the version this library was built with. Compare it with
/// ROCKETNET_VERSION to detect a header and library mismatch.
/// This function works before init.

ROCKETNET_EXPORT uint32_t rocketnet_version( void );

/// Initializes the library, platform and crypto state. Call before creating
/// clients or servers. Do not race this call with term.
///
/// malloc_function and free_function are the allocator for every library
/// allocation. NULL for both selects the system allocator. Passing only one
/// returns ROCKETNET_STATUS_INVALID_ARGUMENT. Both must be thread-safe.
/// malloc_function returns memory aligned for any fundamental type, or NULL.
/// The library aligns its blocks to ROCKETNET_ALLOCATION_ALIGNMENT itself, so
/// each request is larger than the bytes it uses. free_function receives the
/// pointer malloc_function returned.
///
/// context goes to the allocator calls init makes. Each create call passes
/// its own context to the allocator calls for that object.
///
/// Returns ROCKETNET_STATUS_OK, ROCKETNET_STATUS_INVALID_ARGUMENT,
/// ROCKETNET_STATUS_ALREADY_INITIALIZED,
/// ROCKETNET_STATUS_LIVE_ALLOCATIONS when the allocator changes while
/// allocations are live, ROCKETNET_STATUS_PLATFORM_INIT_FAILED or
/// ROCKETNET_STATUS_CRYPTO_INIT_FAILED.
///
/// ROCKETNET_LOG_LEVEL sets the maximum level before init logs. Values are
/// 0 .. 5, from ROCKETNET_LOG_LEVEL_NONE to ROCKETNET_LOG_LEVEL_SPAM. Other
/// values are ignored with a warning.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_init( void * context, void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

/// Terminates the library after every client and server is destroyed. Do not
/// call while other library calls are running. A refusal logs one error.
///
/// Returns ROCKETNET_STATUS_OK, ROCKETNET_STATUS_NOT_INITIALIZED while not
/// initialized or while initializing or terminating, or
/// ROCKETNET_STATUS_LIVE_ALLOCATIONS while any allocation is still held.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_term( void );

/// Returns a status name without the ROCKETNET_STATUS_ prefix. Returns
/// "UNKNOWN" for other values and never returns NULL. It allocates nothing.
/// It works before init, and its strings last for the process.

ROCKETNET_EXPORT const char * rocketnet_status_name( enum rocketnet_status_t status );

/// Receives the condition, function and file for a failed library assert,
/// plus its source line. The strings are borrowed. The callback runs on the
/// thread that fires the assert.

typedef void (*rocketnet_assert_function_t)( const char * condition, const char * function, const char * file, int line );

/// Installs the callback for failed library asserts. NULL restores the
/// default, which logs and traps. The callback runs only in debug builds with
/// asserts enabled. Install before concurrent library calls because reads are
/// not synchronized.

ROCKETNET_EXPORT void rocketnet_assert_function( rocketnet_assert_function_t function );

/// Suppresses all logs when true. When false, logs follow the current level.
/// This process-wide setting does not change the log level.

ROCKETNET_EXPORT void rocketnet_quiet( bool flag );

/// Sets the process-wide maximum log level. Values must be from
/// ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM. Other values are
/// ignored.

ROCKETNET_EXPORT void rocketnet_log_level( enum rocketnet_log_level_t level );

/// Enables or disables network thread priority changes. Enabled by default.
/// On Windows, init also requests a 1 ms timer period until term. Disable this
/// if the game sets thread priorities itself.
///
/// Set before init to skip the timer request. New threads use the current
/// setting. Running threads keep their existing priority. OS refusal never
/// fails create or init. The thread keeps its default priority. The first
/// refusal of each kind logs one warning. Stats count the refusals.

ROCKETNET_EXPORT void rocketnet_thread_priority( bool enabled );

/// Returns the log level name, such as "ERROR" or "SPAM". Returns "UNKNOWN"
/// for other values and never returns NULL. The strings last for the process.
/// Use this to format a level like the default log sink.

ROCKETNET_EXPORT const char * rocketnet_log_level_name( enum rocketnet_log_level_t level );

/// Receives one formatted log line on the thread that logs it. This may be a
/// network thread, so the callback must be thread-safe. It must not call
/// rocketnet_log_function. context is the installed pointer. message is
/// NUL-terminated and length excludes the NUL. The message is at most 1023
/// bytes and is valid only until the callback returns.

typedef void (*rocketnet_log_function_t)( void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

/// Installs the log callback and its context. NULL restores the default sink,
/// which prints to stdout, and clears the context. An in-flight call may still
/// use the old callback, so keep its context valid until that call returns.

ROCKETNET_EXPORT void rocketnet_log_function( rocketnet_log_function_t function, void * context );

/// API misuse

/// The stats structs' api_misuses field counts these errors process-wide
/// since the process started.
///
/// - A required argument is NULL.
/// - A client index or destination id is outside 0 .. max_clients - 1. This
///   includes a destination id with ROCKETNET_FLOW_ID_BIT set.
/// - Create runs before rocketnet_init, or init refuses its allocator.
/// - Update receives a non-finite time or a time that goes backwards. The last
///   good time is kept.
/// - An unreliable start message of at most
///   ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES stays open at update or
///   send_packets.
/// - Connect runs from a state other than DISCONNECTED or a connection-ended
///   state.
/// - Server start runs while the server is already running.
///
/// A NULL object, NULL required argument or out-of-range client index counts
/// as misuse. The call does nothing and returns zero, false, NULL,
/// ROCKETNET_DISCONNECT_REASON_NONE or
/// ROCKETNET_STATUS_INVALID_ARGUMENT.
///
/// Exceptions follow.
/// - rocketnet_client_state returns ROCKETNET_CLIENT_STATE_DISCONNECTED.
/// - rocketnet_client_index returns -1.
/// - Stats for a NULL client or server still fill process-wide fields and
///   zero the rest.
/// - NULL destroy does nothing and is not counted, like free(NULL).
///
/// These errors are not API misuse.
/// - A send with invalid arguments returns NULL or 0 and logs. Examples are a
///   second start while one is pending, bytes_written outside the valid range,
///   or an invalid channel or reliability value.
/// - A send while not CONNECTED is dropped and counted in
///   messages_dropped_not_connected.
/// - Update or send_packets on a stopped server does nothing.
/// - A setter for a disconnected client or empty server slot returns OK and
///   does nothing. An invalid value returns INVALID_ARGUMENT.

/// Returns monotonic elapsed seconds from an unspecified origin.

ROCKETNET_EXPORT double rocketnet_time( void );

/// Sleeps for seconds. Negative, zero or NaN returns immediately. Values
/// above 2000 sleep for 2000 seconds.

ROCKETNET_EXPORT void rocketnet_sleep( double seconds );

/// Config

/// Initialize each config with its default_config function, then set the
/// fields the game needs. Environment values override config values. Every
/// field except callbacks and config_bytes names its environment variable.
/// Overrides are logged with the displaced value. Invalid values refuse
/// create. Values are not clamped. Empty variables are ignored and logged.
///
/// Integers use decimal. Byte arrays use base64 with exactly the field's
/// length. Strings are used as-is.
///
/// Transport is a comma-separated list tried from left to right at client
/// connect or server start. The first transport that starts wins. "auto" is
/// the default, including NULL or an empty string. Linux servers try
/// xdp,gso,io_uring,portable. io_uring requires build support. macOS servers
/// try msgx,portable. Windows servers try portable,iocp,rio. macOS
/// clients try msgx,portable. Other clients try portable. Loopback is selected
/// by name, never by auto. A one-name list selects only that transport.
/// Unsupported names are skipped with a log.
/// Unknown names refuse the list and report valid names.
///
/// Fields marked "must match on both ends" are checked during connection. A
/// mismatch ends the connection with CONFIG_MISMATCH. Create checks field
/// ranges and related message limits, such as max_message_bytes not exceeding
/// receive_message_buffer_bytes. Create copies strings, so they need only live
/// until create returns.
///
/// config_bytes is the size of the struct used by the caller. It lets newer
/// versions add fields at the end while older programs keep working. The
/// default_config functions set it to sizeof(*config) and fill every field.
///
/// Other config calls check config_bytes first. A zero or too-small size is
/// refused and logged. A larger size is read only as far as this library
/// knows. A smaller size uses defaults for fields beyond the caller's struct.
/// The library never accesses bytes past config_bytes. NULL means defaults
/// where accepted.

struct rocketnet_client_config_t
{
    /// The size of the caller's struct. Set by
    /// rocketnet_client_default_config. Zero is refused.

    size_t config_bytes;

    /// ROCKETNET_CLIENT_TRANSPORT sets the candidate list. NULL means "auto".

    const char * transport;

    /// Called when the handshake state changes. NULL disables the callback. It
    /// runs on the caller thread during update, disconnect and destroy. Do not
    /// call client functions from it.

    void (*state_change_callback)( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state );

    /// ROCKETNET_CLIENT_PACKET_FILTER is the title packet filter. It must match
    /// the server's value.

    uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES];

    /// ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES sets the largest payload this
    /// client fragments and reassembles. Losing one fragment loses the whole
    /// payload. Range is ROCKETNET_MAX_PACKET_PAYLOAD_BYTES .. 305408. Zero
    /// selects the default, 16384.

    int max_payload_bytes;

    /// ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES sets the limit. Range is 1 ..
    /// 256. Zero selects the default, 8.

    int max_fragmented_messages;

    /// ROCKETNET_CLIENT_SEND_KBPS sets the starting send budget per
    /// connection, in kbps. Range is 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1.
    /// The unlimited value means no send budget. Zero selects
    /// ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS. Negative values return
    /// ROCKETNET_STATUS_INVALID_ARGUMENT.

    int send_kbps;

    /// ROCKETNET_CLIENT_NUM_CHANNELS sets the number of channels. Both ends
    /// must use the same value. Range is 1 .. ROCKETNET_MAX_CHANNELS. Default
    /// is 32.

    int num_channels;

    /// ROCKETNET_CLIENT_RELIABLE_MESSAGES_IN_FLIGHT sets the reliable-message
    /// window. Both ends must match. It must be a power of two from 64 ..
    /// 1048576. Default is 1024.

    int reliable_messages_in_flight;

    /// ROCKETNET_CLIENT_RECEIVE_MESSAGE_BUFFER_BYTES sets the reassembly
    /// buffer. Both ends must match. It must be a multiple of 16 from 4096 ..
    /// 1048576. Default is 262144.

    int receive_message_buffer_bytes;

    /// ROCKETNET_CLIENT_MAX_MESSAGE_BYTES sets the largest message. Both ends
    /// must match. It cannot exceed receive_message_buffer_bytes. Range is
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES .. 1048576. Default is 262144.

    int max_message_bytes;

    /// ROCKETNET_CLIENT_SEND_MESSAGE_BUFFER_BYTES sets queued message bytes.
    /// Range is 12288 .. 268435456. Default is 800000.

    int send_message_buffer_bytes;

    /// ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES sets the queued message limit.
    /// Range is 16 .. 1048576. Default is 1024.

    int send_queue_messages;

    /// ROCKETNET_CLIENT_PACKETS_IN_FLIGHT sets the unacknowledged packet
    /// limit. Range is 16 .. 65536. Default is 256.

    int packets_in_flight;

    /// ROCKETNET_CLIENT_PACKETS_PER_UPDATE sets the send limit per update.
    /// Range is 1 .. 1024. Default is 256. The send budget also limits
    /// sending.

    int packets_per_update;

    /// ROCKETNET_CLIENT_UNRELIABLE_MESSAGES_PER_UPDATE sets how many
    /// unreliable messages can be delivered per update. Range is 1 .. 65536.
    /// Default is 1024.

    int unreliable_messages_per_update;

    /// ROCKETNET_CLIENT_SEQUENCED_MESSAGES_HELD sets how many sequenced
    /// messages are held for ordering. Range is 0 .. 4096. Default is 16.

    int sequenced_messages_held;

    /// ROCKETNET_CLIENT_MAX_MESSAGE_RECEIPTS sets the outcomes held for
    /// rocketnet_client_receipts. Range is 16 .. 65536. Default is 256.

    int max_message_receipts;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS sets the resend delay
    /// before the first RTT sample. Units are milliseconds. Range is 10 ..
    /// 60000. Default is 2000.

    int message_resend_delay_initial_ms;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS sets the minimum resend
    /// delay. Units are milliseconds. Zero means no minimum. Range is 0 ..
    /// 60000. Default is 0.

    int message_resend_delay_min_ms;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS sets the maximum resend
    /// delay. Units are milliseconds. Range is 10 .. 60000. Default is 2000.

    int message_resend_delay_max_ms;

    /// ROCKETNET_CLIENT_RELIABLE_MESSAGE_TIMEOUT_MS disconnects if a reliable
    /// message remains unacknowledged this long. Units are milliseconds.
    /// Range is 1000 .. 600000. Default is 10000.

    int reliable_message_timeout_ms;

};

/// The server config starts without a backend public key or protocol id.
/// Supply both in config or with ROCKETNET_SERVER_BACKEND_PUBLIC_KEY and
/// ROCKETNET_SERVER_PROTOCOL_ID. Environment values override config values.
/// Start refuses an all-zero backend key with MISSING_KEYS.
///
/// Each server object generates its own X25519 keypair during create. Destroy
/// wipes its secret key. Read the public key with rocketnet_server_public_key.
/// After start, read the server addresses and register them with the backend
/// over an authenticated channel. The backend issues tokens for that key and
/// those addresses.

struct rocketnet_server_config_t
{
    /// The size of the caller's struct. Set by
    /// rocketnet_server_default_config. Zero is refused.

    size_t config_bytes;

    /// ROCKETNET_SERVER_PROTOCOL_ID is the title's protocol id. Connect
    /// tokens must use it. Zero is refused.

    uint64_t protocol_id;

    /// ROCKETNET_SERVER_MAX_CLIENTS sets the client limit. Range is 1 ..
    /// ROCKETNET_MAX_CLIENTS. Default is 1000.

    int max_clients;

    /// ROCKETNET_SERVER_PACKET_FILTER is the title packet filter. It must
    /// match the clients' value.

    uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES];

    /// ROCKETNET_SERVER_BACKEND_PUBLIC_KEY is the backend's public key. Start
    /// requires a nonzero value or returns ROCKETNET_STATUS_MISSING_KEYS.

    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];

    /// ROCKETNET_SERVER_NUM_XDP_QUEUES applies to xdp and gso. Zero selects
    /// the transport default. Range is 1 .. 1024.

    int num_xdp_queues;

    /// ROCKETNET_SERVER_NUM_OS_CPUS applies to xdp. It sets cores left for
    /// the operating system. Zero selects the transport default. Range is 1
    /// .. 256.

    int num_os_cpus;

    /// ROCKETNET_SERVER_MAX_PAYLOAD_BYTES sets the largest payload this
    /// server fragments and reassembles. Losing one fragment loses the whole
    /// payload. Range is ROCKETNET_MAX_PACKET_PAYLOAD_BYTES .. 305408. Zero
    /// selects the default, 16384.

    int max_payload_bytes;

    /// ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES sets the limit per client.
    /// Range is 1 .. 256. Zero selects the default, 8.

    int max_fragmented_messages;

    /// ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS sets the maximum timeout. Units
    /// are seconds. Range is 1 .. 3600. Default is 60.

    int max_timeout_seconds;

    /// ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS sets the handshake
    /// deadline. Units are seconds. Range is 1 .. 60. Default is 5.

    int handshake_deadline_seconds;

    /// ROCKETNET_SERVER_TRANSPORT sets the candidate list. NULL means "auto".
    /// Every transport serves IPv4, IPv6, or both.

    const char * transport;

    /// ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS sets each connection's starting
    /// send budget, in kbps. Range is 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1.
    /// The unlimited value means no send budget. Zero selects
    /// ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS. Negative values return
    /// ROCKETNET_STATUS_INVALID_ARGUMENT.

    int per_client_send_kbps;

    /// ROCKETNET_SERVER_NUM_CHANNELS sets the number of channels. Both ends
    /// must use the same value. Range is 1 .. ROCKETNET_MAX_CHANNELS. Default
    /// is 32.

    int num_channels;

    /// ROCKETNET_SERVER_RELIABLE_MESSAGES_IN_FLIGHT sets the reliable-message
    /// window. Both ends must match. It must be a power of two from 64 ..
    /// 1048576. Default is 1024.

    int reliable_messages_in_flight;

    /// ROCKETNET_SERVER_RECEIVE_MESSAGE_BUFFER_BYTES sets the reassembly
    /// buffer. Both ends must match. It must be a multiple of 16 from 4096 ..
    /// 1048576. Default is 262144.

    int receive_message_buffer_bytes;

    /// ROCKETNET_SERVER_MAX_MESSAGE_BYTES sets the largest message. Both ends
    /// must match. It cannot exceed receive_message_buffer_bytes. Range is
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES .. 1048576. Default is 262144.

    int max_message_bytes;

    /// ROCKETNET_SERVER_SEND_MESSAGE_BUFFER_BYTES sets queued message bytes
    /// per client. Range is 12288 .. 268435456. Default is 800000.

    int send_message_buffer_bytes;

    /// ROCKETNET_SERVER_SEND_QUEUE_MESSAGES sets queued messages per client.
    /// Range is 16 .. 1048576. Default is 1024.

    int send_queue_messages;

    /// ROCKETNET_SERVER_PACKETS_IN_FLIGHT sets the unacknowledged packet
    /// limit per client. Range is 16 .. 65536. Default is 256.

    int packets_in_flight;

    /// ROCKETNET_SERVER_PACKETS_PER_UPDATE sets the send limit per update and
    /// per client. Range is 1 .. 1024. Default is 256. The send budget also
    /// limits sending.

    int packets_per_update;

    /// ROCKETNET_SERVER_UNRELIABLE_MESSAGES_PER_UPDATE sets how many
    /// unreliable messages can be delivered per update and per client. Range
    /// is 1 .. 65536. Default is 1024.

    int unreliable_messages_per_update;

    /// ROCKETNET_SERVER_SEQUENCED_MESSAGES_HELD sets how many sequenced
    /// messages are held for ordering. Range is 0 .. 4096. Default is 16.

    int sequenced_messages_held;

    /// ROCKETNET_SERVER_MAX_MESSAGE_RECEIPTS sets outcomes held for
    /// rocketnet_server_receipts, per client. Range is 16 .. 65536. Default
    /// is 256.

    int max_message_receipts;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS sets the resend delay
    /// before the first RTT sample. Units are milliseconds. Range is 10 ..
    /// 60000. Default is 2000.

    int message_resend_delay_initial_ms;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS sets the minimum resend
    /// delay. Units are milliseconds. Zero means no minimum. Range is 0 ..
    /// 60000. Default is 0.

    int message_resend_delay_min_ms;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS sets the maximum resend
    /// delay. Units are milliseconds. Range is 10 .. 60000. Default is 2000.

    int message_resend_delay_max_ms;

    /// ROCKETNET_SERVER_RELIABLE_MESSAGE_TIMEOUT_MS disconnects if a reliable
    /// message remains unacknowledged this long. Units are milliseconds.
    /// Range is 1000 .. 600000. Default is 10000.

    int reliable_message_timeout_ms;
};

/// Messages

/// Create allocates the client or server through the configured allocator.
/// Connect and start may allocate transport memory. The required_memory calls
/// report the memory needed by create.
///
/// Pass the same game clock to every update, in seconds.
/// Run each frame on one thread. Update first, receive until NULL, simulate,
/// write messages with start and finish, then send packets.
///
/// Reliable messages wait for send budget. Unreliable messages over budget are
/// dropped and counted in messages_dropped_over_budget.
/// Running out of message memory ends the connection with
/// MESSAGE_MEMORY_EXHAUSTED.

struct rocketnet_message_batch_t;

/// A delivery outcome copied by the client or server receipts call.

struct rocketnet_receipt_t
{
    /// The receipt serial finish message returned for the send.

    uint32_t serial;

    /// ROCKETNET_RECEIPT_STATUS_ACKED or ROCKETNET_RECEIPT_STATUS_LOSS.
    /// Zero means ROCKETNET_RECEIPT_STATUS_NONE.

    enum rocketnet_receipt_status_t status;
};

/// Connect tokens

/// A connect token is an opaque base64 string identifying one server.
/// Its size, including the NUL terminator, is at most
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES.
/// Production tokens come from your backend. Development tokens come from
/// rocketnet_create_dev_connect_token.

/// Client

/// Create makes a DISCONNECTED client. Connect starts a connection using a
/// token. Disconnect returns it to DISCONNECTED so it can connect again.
/// Destroy works from any state.
///
/// Each connection starts fresh. Lifetime stats persist across connections.
/// The last failure state remains until disconnect or the next connect.
/// Connect opens a socket for the address family in the token.

/// Fills defaults within config_bytes and leaves config_bytes unchanged.
/// Returns ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED if the size is too small.
/// Use rocketnet_client_default_config to initialize a config.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config_internal( struct rocketnet_client_config_t * config );

/// Initializes every config field to its default and sets config_bytes.
/// Returns ROCKETNET_STATUS_OK on success.

static inline enum rocketnet_status_t rocketnet_client_default_config( struct rocketnet_client_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );
    return rocketnet_client_default_config_internal( config );
}

/// Returns the memory needed by create for this config.
/// Applies the same environment overrides as create.
/// NULL config selects defaults. Invalid config returns 0.

ROCKETNET_EXPORT size_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );

#if ROCKETNET_DEVELOPMENT

/// The public development backend key. Use it to initialize backend_public_key.
///
///     static const uint8_t dev_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] =
///         ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
///     memcpy( config.backend_public_key, dev_key, sizeof( dev_key ) );
///
/// A server using this key accepts development tokens.
/// A production server refuses it with ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE.

#define ROCKETNET_DEV_BACKEND_PUBLIC_KEY { 0xb0, 0xab, 0x03, 0xb3, 0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99, 0x29, 0xa5, 0xb4, 0xbf, \
                                           0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d, 0x08, 0x4a, 0x08, 0xd8, 0x58, 0xbd, 0x24, 0x2c }

/// Writes a development token for rocketnet_client_connect.
/// The output buffer must hold ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES.
/// The server must use ROCKETNET_DEV_BACKEND_PUBLIC_KEY.
///
/// Supply the addresses returned by the server's address getters.
/// Pass NULL for an unavailable address family. At least one is required.
/// Supply ROCKETNET_SERVER_PUBLIC_KEY_BYTES from rocketnet_server_public_key.
///
/// expire_seconds is the token lifetime from unix_timestamp.
/// timeout_seconds is the maximum time the connection may be silent.
/// Both durations must be positive. unix_timestamp is the creation time
/// in Unix seconds.
/// protocol_id must be nonzero and match the server. client_id identifies the
/// client. user_data holds ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES or is NULL
/// for zeros.
///
/// Returns ROCKETNET_STATUS_OK on success. An invalid or overlong address
/// returns ROCKETNET_STATUS_INVALID_ADDRESS. Invalid arguments return
/// ROCKETNET_STATUS_INVALID_ARGUMENT. A crypto failure returns
/// ROCKETNET_STATUS_CRYPTO_INIT_FAILED. Failure leaves the output unchanged.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_create_dev_connect_token( const char * server_address_ipv4,
                                                                            const char * server_address_ipv6,
                                                                            const uint8_t * server_public_key,
                                                                            int expire_seconds,
                                                                            int timeout_seconds,
                                                                            uint64_t client_id,
                                                                            uint64_t protocol_id,
                                                                            uint64_t unix_timestamp,
                                                                            const uint8_t * user_data,
                                                                            char * connect_token );

/// Simulates latency, jitter, packet loss and duplication on outgoing packets.
/// latency_ms and jitter_ms range from 0 to 60000.
/// packet_loss_percent and duplicate_percent range from 0 to 100.
/// Set all values to zero to disable the simulator.
///
/// Changes apply at the next connect. An active simulator updates immediately.
/// Out-of-range values count as API misuse and leave the settings unchanged.
///
/// These environment variables set the values at create.
/// ROCKETNET_DEV_CLIENT_LATENCY_MS
/// ROCKETNET_DEV_CLIENT_JITTER_MS
/// ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT
/// ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT

ROCKETNET_EXPORT void rocketnet_client_network_simulator( struct rocketnet_client_t * client, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );

/// Simulates outgoing server packets with the same ranges as the client call.
/// Changes apply at the next start. An active simulator updates immediately.
/// Invalid values count as API misuse and leave the settings unchanged.
/// Use the ROCKETNET_DEV_SERVER_LATENCY_MS, ROCKETNET_DEV_SERVER_JITTER_MS,
/// ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT and
/// ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT environment variables at create.

ROCKETNET_EXPORT void rocketnet_server_network_simulator( struct rocketnet_server_t * server, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );


#endif // ROCKETNET_DEVELOPMENT

/// Creates a DISCONNECTED client without opening a socket.
/// Validates the config and logs any failure. NULL config selects defaults.
/// context is passed to the allocator and state_change_callback.
/// Keep context valid until destroy.
///
/// Returns NULL on failure. If status is supplied, it receives
/// ROCKETNET_STATUS_OK on success or the failure code.
/// Call rocketnet_init before create.

ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, enum rocketnet_status_t * status );

/// Starts a connection using a token string.
/// The string must end with NUL within
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES.
/// Malformed strings return ROCKETNET_STATUS_INVALID_CONNECT_TOKEN without
/// changing the client.
///
/// Connect accepts DISCONNECTED and the connection-ended states
/// SERVER_DISCONNECTED through MESSAGE_MEMORY_EXHAUSTED.
/// Call disconnect first after a handshake or token failure.
/// Other states return ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED.
///
/// Failed token checks set INVALID_CONNECT_TOKEN or CONNECT_TOKEN_EXPIRED.
/// A socket or thread failure leaves the client DISCONNECTED.
/// The connection clock starts at the next update.
/// Returns ROCKETNET_STATUS_OK on success or a failure status.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const char * connect_token );

/// Destroys the client from any state.
/// Sends disconnect packets from SENDING_CONNECTION_RESPONSE or CONNECTED.
/// Stops the network thread and waits for it to exit.
/// No time bound is promised.
/// Wipes and frees the client's memory. All client pointers become invalid.

ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

/// Processes incoming messages and connection timeouts at the supplied time.
/// Read the client state and stats after updating.

ROCKETNET_EXPORT void rocketnet_client_update( struct rocketnet_client_t * client, double time );

/// Sends queued messages in packets, subject to the send budget.

ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

/// Ends the connection and returns the client to DISCONNECTED.
/// Sends disconnect packets when the connection has session keys.
/// Stops the network thread and waits for it to exit.
/// No time bound is promised.
/// The client can connect again.

ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

/// The client's state.

ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

/// Returns a state name such as "CONNECTED" or "INVALID".
/// Returns "UNKNOWN" for an invalid value.

ROCKETNET_EXPORT const char * rocketnet_client_state_name( enum rocketnet_client_state_t state );

/// Returns the client's server slot index while CONNECTED.
/// The index ranges from 0 to max_clients - 1. Otherwise returns -1.

ROCKETNET_EXPORT int rocketnet_client_index( const struct rocketnet_client_t * client );

/// Returns the selected transport name or NULL while DISCONNECTED.
/// The string remains valid for the process lifetime.

ROCKETNET_EXPORT const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client );

/// Reserves up to max_bytes for a message. Returns NULL on failure.
/// Write directly into the returned memory, then finish or abort the message.
///
///     uint8_t * p = rocketnet_client_start_message( client, channel,
///                                                 reliability, max_bytes );
///     if ( p )
///     {
///         int bytes = write_player_input( p, max_bytes, &input );
///         serial = rocketnet_client_finish_message( client, bytes );
///     }
///
/// Only one message can be open per client. The pointer remains valid until
/// finish or abort. max_bytes must not exceed the config's max_message_bytes.
///
/// Sends outside CONNECTED are dropped. Start returns NULL without logging
/// and increments messages_dropped_not_connected. Finish then returns 0.
/// Dropped messages are not queued for a later connection.

ROCKETNET_EXPORT uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int max_bytes );

/// Queues bytes_written from the open message and returns its nonzero serial.
/// bytes_written must be between 1 and the reserved size.
/// An invalid size logs an error, aborts the message and returns 0.
/// Returns 0 if no message is open.

ROCKETNET_EXPORT uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written );

/// Cancels the open message and releases its memory.
/// Does nothing if no message is open.

ROCKETNET_EXPORT void rocketnet_client_abort_message( struct rocketnet_client_t * client );

/// Returns the next batch of messages from the server.
/// Each message has client_index 0. Keep calling until NULL is returned.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_client_receive_messages( struct rocketnet_client_t * client );

/// Copies up to capacity receipt outcomes into out and returns the count.
/// Returns 0 if capacity is less than 1.
///
/// Use each receipt's serial to identify its send.
/// Only the three _WITH_ACK_RECEIPT reliability modes produce receipts.
/// ACKED means the peer acknowledged the message, not that its application read
/// it. LOSS means an unreliable receipt message was not acknowledged.
/// That message is not resent.
///
/// Reliable receipts report ACKED only. This also applies to unreliable
/// receipt messages larger than ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES.
/// Ending a connection does not produce LOSS receipts for reliable messages.
///
/// Read receipts after update. They remain until the next update.
/// Reading does not consume them. Repeated reads copy the same first entries.
/// A full queue drops its oldest receipt and increments receipts_dropped.
/// The queue holds max_message_receipts entries.

ROCKETNET_EXPORT int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity );

/// Sets the send budget in kbps and returns ROCKETNET_STATUS_OK.
/// Zero selects ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS.
/// ROCKETNET_SEND_KBPS_UNLIMITED disables the budget.
/// Other positive values cap the rate.
/// A negative value returns ROCKETNET_STATUS_INVALID_ARGUMENT without changing
/// it. The config's send_kbps sets the starting rate for each connection.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

/// Fills stats with the client's current stats.

ROCKETNET_EXPORT void rocketnet_client_stats( const struct rocketnet_client_t * client, struct rocketnet_client_stats_t * stats );

/// Server

/// A server starts stopped. Start binds its transport and accepts clients.
/// Stop disconnects clients and releases the transport. It can then start
/// again.
/// Clients occupy slots from 0 to max_clients - 1.
/// Only start_message takes the destination index as uint64_t.

/// Fills defaults within config_bytes and leaves config_bytes unchanged.
/// Returns ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED if the size is too small.
/// Use rocketnet_server_default_config to initialize a config.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config_internal( struct rocketnet_server_config_t * config );

/// Initializes every config field to its default and sets config_bytes.
/// Leaves backend_public_key empty. Returns ROCKETNET_STATUS_OK on success.

static inline enum rocketnet_status_t rocketnet_server_default_config( struct rocketnet_server_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );
    return rocketnet_server_default_config_internal( config );
}

/// Returns the memory needed by create for this config and max_clients.
/// Applies the same environment overrides as create.
/// NULL config selects defaults. Invalid config returns 0.

ROCKETNET_EXPORT size_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );

/// Creates a stopped server. No address is bound until start.
/// Validates the config and logs any failure. The backend key is checked at
/// start. NULL config selects defaults, which lack a backend key.
///
/// ipv4_address accepts "a.b.c.d:port". ipv6_address accepts "[v6]:port".
/// Either may be NULL. That family is then not served. Both NULL is refused.
/// With both, the two share one port.
/// ROCKETNET_SERVER_IPV4_ADDRESS overrides ipv4_address.
/// ROCKETNET_SERVER_IPV6_ADDRESS overrides ipv6_address.
/// Every server reads both at create. Each override is logged.
/// Port 0 requests an ephemeral port. Read it after start with
/// rocketnet_server_port.
///
/// context is passed to allocator callbacks. Keep it valid until destroy.
/// Returns NULL on failure. If status is supplied, it receives
/// ROCKETNET_STATUS_OK on success or the failure code.
/// Call rocketnet_init before create.

ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * ipv4_address, const char * ipv6_address, const struct rocketnet_server_config_t * config, enum rocketnet_status_t * status );

/// Binds the transport and accepts up to max_clients clients.
/// Returns ROCKETNET_STATUS_OK on success.
/// A running server returns ROCKETNET_STATUS_SERVER_NOT_STOPPED.
/// Other failures return their status and leave the server stopped.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_start( struct rocketnet_server_t * server );

/// Sends disconnect packets to all clients and frees their slots.
/// Releases the transport and leaves the server stopped, ready to start again.
/// Does nothing if already stopped.

ROCKETNET_EXPORT void rocketnet_server_stop( struct rocketnet_server_t * server );

/// true while running, false while stopped.

ROCKETNET_EXPORT bool rocketnet_server_running( const struct rocketnet_server_t * server );

/// Stops a running server before destroying it.
/// Wipes and frees its memory, including the keypair.
/// All pointers from the server become invalid.

ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

/// Processes incoming messages and connection timeouts for every client.
/// Read the client slot queries and stats after updating.

ROCKETNET_EXPORT void rocketnet_server_update( struct rocketnet_server_t * server, double time );

/// Sends each client's queued messages in packets under its send budget.

ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

/// Returns the bound port or 0 while stopped.
/// Use this to read the assigned port when the address requested port 0.

ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

/// Copies ROCKETNET_SERVER_PUBLIC_KEY_BYTES into public_key.
/// Create generates this key for the server object.
/// Register it with your backend before issuing connect tokens.
/// The key is available while running or stopped.

ROCKETNET_EXPORT void rocketnet_server_public_key( const struct rocketnet_server_t * server, uint8_t * public_key );

/// Writes the server's IPv4 address and bound port as "a.b.c.d:port".
/// Returns true if the running server serves IPv4.
/// Use a buffer of ROCKETNET_MAX_ADDRESS_STRING_BYTES.
///
/// Returns false while stopped, without IPv4, or if address_bytes is too small.
/// Failure writes an empty string if address_bytes is positive.
/// A short buffer logs an error. A zero-sized buffer is left untouched.

ROCKETNET_EXPORT bool rocketnet_server_ipv4_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// Like rocketnet_server_ipv4_address, but writes IPv6 as "[v6]:port".

ROCKETNET_EXPORT bool rocketnet_server_ipv6_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// Returns the selected transport name or NULL while stopped.
/// The string remains valid for the process lifetime.

ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

/// The config's max_clients, running or stopped.

ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

/// Returns the number of connected clients. Returns 0 while stopped.

ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

/// Returns true if the slot has a connected client. Returns false while
/// stopped.

ROCKETNET_EXPORT bool rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index );

/// Returns the client ID from the slot's connect token.
/// Returns 0 for a free slot or a stopped server.
/// Zero is also a valid ID. Check rocketnet_server_client_connected first.

ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index );

/// Disconnects a client and frees its slot.
/// Does nothing for a free slot or a stopped server.

ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

/// Disconnects all clients. The server keeps running.

ROCKETNET_EXPORT void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server );

/// Returns why the slot's last connection ended.
/// The reason remains until the slot's next connection.
/// Returns ROCKETNET_DISCONNECT_REASON_NONE while stopped.

ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index );

/// Returns a reason name such as "NONE" or "RELIABLE_TIMED_OUT".
/// Returns "UNKNOWN" for an invalid value.

ROCKETNET_EXPORT const char * rocketnet_disconnect_reason_name( enum rocketnet_disconnect_reason_t reason );

/// Reserves a message for the client at destination_id.
/// Use it like rocketnet_client_start_message.
///
///     uint8_t * p = rocketnet_server_start_message( server, client_index,
///                                                 channel, reliability,
///                                                 max_bytes );
///     if ( p )
///     {
///         int bytes = write_world_state( p, max_bytes, &state );
///         serial = rocketnet_server_finish_message( server, bytes );
///     }
///
/// Only one message can be open per server. Finish and abort need no
/// destination.
/// To broadcast, send to each connected client index.
///
/// An index at or above max_clients is refused.
/// A destination with ROCKETNET_FLOW_ID_BIT set is refused and logs
/// ROCKETNET_STATUS_FLOW_ID_RESERVED.
/// Sends to a slot without a connected client are dropped, including while
/// stopped. Each dropped send increments messages_dropped_not_connected.

ROCKETNET_EXPORT uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, enum rocketnet_reliability_t reliability, int max_bytes );

/// Finishes the server's open message.
/// Uses the same size, return and abort rules as
/// rocketnet_client_finish_message.

ROCKETNET_EXPORT uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, int bytes_written );

/// Cancels the open message and releases its memory.
/// Does nothing if no message is open.

ROCKETNET_EXPORT void rocketnet_server_abort_message( struct rocketnet_server_t * server );

/// Returns the next batch of messages from all clients.
/// Each message names its sender in client_index.
/// Keep calling until NULL is returned. Returns NULL while stopped.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_server_receive_messages( struct rocketnet_server_t * server );

/// Copies receipts for the client in this slot.
/// Uses the same rules as rocketnet_client_receipts.
/// Returns 0 for a free slot or a stopped server.

ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity );

/// Sets this client's send budget in kbps and returns ROCKETNET_STATUS_OK.
/// Zero selects ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS.
/// ROCKETNET_SEND_KBPS_UNLIMITED disables the budget.
/// Other positive values cap the rate.
/// A negative value returns ROCKETNET_STATUS_INVALID_ARGUMENT without changing
/// it. The config's per_client_send_kbps sets each connection's starting rate.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps );

/// Fills stats with the server's current stats.

ROCKETNET_EXPORT void rocketnet_server_stats( const struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats );

/// Copies the current stats for this client slot.
/// Writes zeros for a free slot or a stopped server.

ROCKETNET_EXPORT void rocketnet_server_client_stats( const struct rocketnet_server_t * server, int client_index, struct rocketnet_server_client_stats_t * stats );

/// Receiving messages

/// The most messages one receive call fills.

#define ROCKETNET_RECEIVE_BATCH_MESSAGES                   65536

/// A batch of received messages. Entry i in each array describes message i.
///
/// Receive returns the next batch or NULL when the queue is empty.
/// Call it until NULL to drain all messages from the update.
/// A batch holds up to ROCKETNET_RECEIVE_BATCH_MESSAGES.
/// Each client and server owns one batch allocated at create.
///
/// Ordered and sequenced messages keep their order within each channel and
/// sender. A server returns each client's messages in client index order.
///
///     const struct rocketnet_message_batch_t * batch;
///     while ( ( batch = rocketnet_server_receive_messages( server ) ) )
///     {
///         for ( int i = 0; i < batch->num_messages; i++ )
///             handle( batch->client_index[i], batch->data[i],
///                     batch->bytes[i] );
///     }
///
/// The batch and its data remain valid until the next receive or update on
/// the same endpoint. Copy anything needed longer. There is no release call.

struct rocketnet_message_batch_t
{
    /// The entries filled.

    int num_messages;

    /// The sender's client index when receiving on a server.
    /// Zero when receiving on a client.

    int client_index[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The ordering channel for ordered or sequenced messages.
    /// Zero for other reliability modes.

    int channel[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The received reliability mode. Receipt modes arrive without the receipt
    /// flag. Unreliable messages larger than
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES
    /// arrive as their reliable equivalents.

    enum rocketnet_reliability_t reliability[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The message's length in bytes.

    int bytes[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The message's bytes, in the library's memory.

    const uint8_t * data[ROCKETNET_RECEIVE_BATCH_MESSAGES];
};

#ifdef __cplusplus
}
#endif

#endif // ROCKETNET_H
