/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

/// RocketNet provides a C API for clients and servers exchanging UDP messages.
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
#endif // #if defined( _MSC_VER )

/// ROCKETNET_DEV is 1 by default: a development build. Development builds
/// provide rocketnet_create_dev_connect_token and
/// ROCKETNET_DEV_BACKEND_PUBLIC_KEY, and accept the published development
/// keys and the test packet filter. Production builds
/// define ROCKETNET_DEV 0 (CMake: -DROCKETNET_DEV=OFF), which compiles all of
/// that out and refuses at create the development keys and tokens and the
/// test packet filter. Portable clients and servers are available in both modes.

#if !defined( ROCKETNET_DEV )
#define ROCKETNET_DEV 1
#endif // !defined( ROCKETNET_DEV )

#if !ROCKETNET_DEV

    /// The SDK version as text. Development builds use "dev" and 255 for each
    /// numeric component.

#define ROCKETNET_VERSION_FULL                    "v1.0.0-preview-2"

    /// The major component of the SDK version.

#define ROCKETNET_VERSION_MAJOR                                    1

    /// The minor component of the SDK version.

#define ROCKETNET_VERSION_MINOR                                    0

    /// The patch component of the SDK version.

#define ROCKETNET_VERSION_PATCH                                    0

#else // !ROCKETNET_DEV

    /// The SDK version as text: "dev" in a development build.
#define ROCKETNET_VERSION_FULL                                 "dev"
    /// The major component of the SDK version: 255 in a development build.
#define ROCKETNET_VERSION_MAJOR                                  255
    /// The minor component of the SDK version: 255 in a development build.
#define ROCKETNET_VERSION_MINOR                                  255
    /// The patch component of the SDK version: 255 in a development build.
#define ROCKETNET_VERSION_PATCH                                  255

#endif // !ROCKETNET_DEV

/// Comparable version: major * 10000 + minor * 100 + patch (1.0.0 is 10000).
/// Prerelease labels are omitted; ROCKETNET_VERSION_FULL distinguishes them.
/// Development builds use 2575755. rocketnet_version returns the library's value.

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
    /// Marks each library function for export or import. Empty in a static
    /// build.
#define ROCKETNET_EXPORT
#endif // #if defined( _WIN32 ) && defined( ROCKETNET_BUILDING_DLL )

#ifndef ROCKETNET_MAX_CLIENTS
/// Maximum clients per server: 1000 unless the build defines another value
/// from 512 .. 32768 (-DROCKETNET_MAX_CLIENTS=10000, or the CMake option of
/// the same name). The limits that serve every client derive from it
/// (BUILDING.md, "Maximum clients"). Build the library and every file that
/// includes this header with the same value.
#define ROCKETNET_MAX_CLIENTS                                       1000
#endif // #ifndef ROCKETNET_MAX_CLIENTS

// The bounds are the transports' fixed geometry: below 512 a server's
// per-update receive budget is smaller than its largest send ring allows,
// and above 32768 the io_uring queues cannot serve every client.

#if ROCKETNET_MAX_CLIENTS < 512 || ROCKETNET_MAX_CLIENTS > 32768
#error ROCKETNET_MAX_CLIENTS must be between 512 and 32768
#endif // #if ROCKETNET_MAX_CLIENTS < 512 || ROCKETNET_MAX_CLIENTS > 32768

/// A destination ID is a uint64_t client index. This bit is reserved;
/// rocketnet_server_start_message refuses an ID with it set.

#define ROCKETNET_FLOW_ID_BIT                                       ( 1ULL << 63 )

/// Absolute maximum application message bytes. Configured message limits
/// also require sufficient receive-buffer, reliable-window and send-store capacity.

#define ROCKETNET_MAX_MESSAGE_BYTES                       ( 1 << 20 )

/// Maximum UDP payload bytes per packet.
/// This fits an IPv6 datagram within a 1500-byte MTU.

#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 40 - 8 )

/// Maximum application payload bytes per packet, excluding RocketNet overhead.
/// A multiple of eight.

#define ROCKETNET_MAX_PACKET_PAYLOAD_BYTES                 1416

/// Maximum message bytes that fit in one packet, for UNRELIABLE and
/// UNRELIABLE_WITH_ACK_RECEIPT. UNRELIABLE_SEQUENCED carries a longer header,
/// so its limit is 1405 bytes, or 1404 on a channel of 256 or more.
/// Larger unreliable messages use the matching reliable type.
/// UNRELIABLE becomes RELIABLE.
/// UNRELIABLE_SEQUENCED becomes RELIABLE_SEQUENCED on its channel.
/// UNRELIABLE_WITH_ACK_RECEIPT becomes RELIABLE_WITH_ACK_RECEIPT.

#define ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES             1411

/// Connect-token buffer bytes: 1024 base64 characters plus the terminating NUL.

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

#ifndef ROCKETNET_MAX_CHANNELS
/// Maximum channels: 32 unless the build defines another value from
/// 1 .. 65536 before it includes this header. Build the library and every
/// file that includes this header with the same value.
#define ROCKETNET_MAX_CHANNELS                             32
#endif // #ifndef ROCKETNET_MAX_CHANNELS

/// The default client to server send budget, in kbps.

#define ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS            1000

/// The default server to client send budget per client, in kbps.

#define ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS            10000

/// The send_kbps value for no send budget on a connection.

#define ROCKETNET_SEND_KBPS_UNLIMITED                      0x7fffffff

#ifdef __cplusplus
extern "C"
{
#endif // #ifdef __cplusplus

/// Status-returning calls use these values. OK is zero and the only success;
/// every failure is negative. Test status < 0 for failure.

enum rocketnet_status_t
{
    /// Success.

    ROCKETNET_STATUS_OK = 0,

    /// rocketnet_init came after the library was initialized, or while it is
    /// initializing or terminating. Nothing changed; call rocketnet_term
    /// first to initialize again.

    ROCKETNET_STATUS_ALREADY_INITIALIZED = -1,

    /// A call that needs rocketnet_init came before it, or a term found nothing
    /// to terminate. Call rocketnet_init first.

    ROCKETNET_STATUS_NOT_INITIALIZED = -2,

    /// Term, or init with a new allocator, refused. The library still holds
    /// allocations. Destroy every client and server, then retry.

    ROCKETNET_STATUS_LIVE_ALLOCATIONS = -3,

    /// Init refused: the platform's socket layer did not start. The library
    /// stays uninitialized. Correct the platform setup before retrying.

    ROCKETNET_STATUS_PLATFORM_INIT_FAILED = -4,

    /// The crypto library failed. Init returns it when the library did not
    /// start, and the library stays uninitialized. Server create and
    /// rocketnet_create_dev_connect_token return it when a key or token could
    /// not be made; nothing is created. Retrying will not help unless the
    /// cause is corrected.

    ROCKETNET_STATUS_CRYPTO_INIT_FAILED = -5,

    /// An argument, or a combination of arguments, is NULL, out of range, or
    /// inconsistent with another. Nothing changed. Correct the argument.

    ROCKETNET_STATUS_INVALID_ARGUMENT = -6,

    /// Create refused: an environment override did not parse. The log names
    /// the variable. Correct or unset it.

    ROCKETNET_STATUS_CONFIG_ENV_INVALID = -7,

    /// Create refused: a config field, from the struct or the environment, is
    /// out of its supported range or inconsistent with another field. The log
    /// names the field and the rule it broke. Correct the value.

    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE = -8,

    /// Create refused: the reassembly memory the config asks for is past this
    /// build's limit. Lower max_fragmented_messages, or on a server
    /// max_clients.

    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED = -9,

    /// Start refused. The server config's backend_public_key is all zeros.
    /// Set it, or set ROCKETNET_SERVER_BACKEND_PUBLIC_KEY.

    ROCKETNET_STATUS_MISSING_KEYS = -10,

    /// The protocol id is 0. Server start refuses a server config with
    /// protocol_id 0. Set the title's nonzero protocol id.

    ROCKETNET_STATUS_INVALID_PROTOCOL_ID = -11,

    /// Start refused. A production build (ROCKETNET_DEV=0) was given the
    /// published development backend or server key. Supply production keys.

    ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION = -12,

    /// An address string did not parse or cannot be used. Pass "a.b.c.d:port"
    /// for IPv4 or "[v6]:port" for IPv6, within
    /// ROCKETNET_MAX_ADDRESS_STRING_BYTES.

    ROCKETNET_STATUS_INVALID_ADDRESS = -13,

    /// Create refused: the transport candidate list did not parse. The list
    /// is refused only for an unknown name, an empty element, or "auto"
    /// inside a list. A list whose every name is skipped for this endpoint
    /// on this platform falls back to auto with a warning. The log names
    /// the problem, and an unknown name is logged with the valid names.

    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST = -14,

    /// The connect token fails the client's pre-send checks. It did not parse,
    /// or is not for this protocol. Obtain a new token from the backend.

    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN = -15,

    /// The connect token expired before the first packet was sent. Obtain a
    /// fresh token before connecting.

    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED = -16,

    /// Start refused: the server address has port 0, an ephemeral port, and no
    /// listed transport can report the port it was granted. Give the server a
    /// real port, or list a transport that can.

    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED = -17,

    /// Every transport in the candidate list refused to start, including any
    /// over the ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES or
    /// ROCKETNET_SERVER_MEMORY_BUDGET_BYTES budget. Set the log level to
    /// ROCKETNET_LOG_LEVEL_WARN to see why each one refused.

    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED = -18,

    /// An allocation the operation needs failed. Nothing it allocated stays
    /// live. Free memory or lower the config's sizes, then retry.

    ROCKETNET_STATUS_OUT_OF_MEMORY = -19,

    /// Connect or start refused: the platform refused a mutex. Retry after
    /// freeing system resources.

    ROCKETNET_STATUS_MUTEX_CREATE_FAILED = -20,

    /// Connect or start refused: the platform refused a thread. Retry after
    /// freeing system resources.

    ROCKETNET_STATUS_THREAD_CREATE_FAILED = -21,

    /// Connect or start refused: the platform refused a timer. Retry after
    /// freeing system resources.

    ROCKETNET_STATUS_TIMER_CREATE_FAILED = -22,

    /// Connect refused: an attempt is active, or an earlier attempt's worker
    /// still owns its storage. Call rocketnet_client_disconnect, then connect
    /// again once rocketnet_client_disconnected returns true.

    ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED = -23,

    /// Start refused, nothing changed. The server is running, or a stop has
    /// not finished. Call rocketnet_server_stop, then start again once
    /// rocketnet_server_stopped returns true.

    ROCKETNET_STATUS_SERVER_NOT_STOPPED = -24,

    /// A destination id has the reserved ROCKETNET_FLOW_ID_BIT set. No call
    /// returns it: rocketnet_server_start_message returns NULL and logs this
    /// name. Use a client index.

    ROCKETNET_STATUS_FLOW_ID_RESERVED = -25,

    /// Start refused: every transport that opened cannot serve both named
    /// address families on this machine. Serve one family, or list a
    /// transport that serves both.

    ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED = -26,

    /// A config's config_bytes is 0 or too small. Fill the config with its
    /// default_config call first.

    ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED = -27,

    /// A transport the candidate list names is compiled out of this build:
    /// the xdp server transport in a Linux build configured with
    /// ROCKETNET_XDP=OFF, or without a clang that has the BPF target, libbpf
    /// or libxdp. List a transport this build has, or rebuild with it.

    ROCKETNET_STATUS_TRANSPORT_NOT_BUILT = -28,

    /// Create refused. A production build (ROCKETNET_DEV=0) was given the
    /// published test packet filter. Set your own packet_filter.

    ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION = -29,

    /// Connect refused. A production build (ROCKETNET_DEV=0) was given a
    /// connect token sealed to the published development server key. Use a
    /// token from the production backend.

    ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION = -30,

    /// Connect refused. The connect token names a server address with port 0,
    /// where no server can be reached. Mint the token with the server's bound
    /// port.

    ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO = -31
};

/// The process-wide log levels, least to most verbose.

enum rocketnet_log_level_t
{
    /// As the maximum level, only lines logged at NONE are emitted. Quiet mode
    /// suppresses every line.

    ROCKETNET_LOG_LEVEL_NONE = 0,

    /// Errors, the least verbose emitted level.

    ROCKETNET_LOG_LEVEL_ERROR = 1,

    /// Warnings.

    ROCKETNET_LOG_LEVEL_WARN = 2,

    /// Information.

    ROCKETNET_LOG_LEVEL_INFO = 3,

    /// Debugging.

    ROCKETNET_LOG_LEVEL_DEBUG = 4,

    /// The most verbose level.

    ROCKETNET_LOG_LEVEL_SPAM = 5
};

/// The address family of an endpoint.

enum rocketnet_address_type_t
{
    /// No address family.

    ROCKETNET_ADDRESS_NONE = 0,

    /// IPv4.

    ROCKETNET_ADDRESS_IPV4 = 1,

    /// IPv6.

    ROCKETNET_ADDRESS_IPV6 = 2
};

/// A send's delivery, ordering, and acknowledgement behavior.
/// Ordered and sequenced types require the send's channel below num_channels.
/// Other types accept any channel value, ignore it and use channel 0.

enum rocketnet_reliability_t
{
    /// Sent once without a delivery guarantee, order, or receipt.
    /// Delivered at most once; a lost packet loses the message.

    ROCKETNET_RELIABILITY_UNRELIABLE = 0,

    /// Sent once without a delivery guarantee. The receiver delivers only
    /// newer messages on the channel and drops older messages.

    ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED = 1,

    /// Resent until acknowledged.
    /// Delivered once without channel ordering.

    ROCKETNET_RELIABILITY_RELIABLE = 2,

    /// Resent until acknowledged.
    /// Delivered once in channel order.
    /// The receiver holds later messages until earlier ones arrive.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED = 3,

    /// Resent until acknowledged. The receiver delivers only the
    /// newest message on the channel and can drop a stale one.

    ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED = 4,

    /// As UNRELIABLE, with a receipt. ACKED or LOSS.

    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT = 5,

    /// As RELIABLE, with an ACKED receipt.

    ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT = 6,

    /// As RELIABLE_ORDERED, with an ACKED receipt.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT = 7
};

/// A receipt's outcome, the status field of struct rocketnet_receipt_t.
/// LOSS is negative, like every failure.

enum rocketnet_receipt_status_t
{
    /// The message was not acknowledged, and it is not resent.

    ROCKETNET_RECEIPT_STATUS_LOSS = -1,

    /// A zero-filled receipt. No outcome has been written.

    ROCKETNET_RECEIPT_STATUS_NONE = 0,

    /// The receiver acknowledged the message. This does not mean its
    /// application read it.

    ROCKETNET_RECEIPT_STATUS_ACKED = 1
};

/// Why a server slot's connection ended, returned by
/// rocketnet_server_disconnect_reason. Ended connections have negative reasons.
/// Clients use rocketnet_client_state instead.

enum rocketnet_disconnect_reason_t
{
    /// No connection has ended, or the connection is up.

    ROCKETNET_DISCONNECT_REASON_NONE = 0,

    /// The two ends' configs differ in a field that must match on both ends.

    ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH = -1,

    /// The client sent what the protocol forbids.

    ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION = -2,

    /// A reliable message went unacknowledged past reliable_message_timeout_ms.

    ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT = -3,

    /// The connection's message memory had no room for a message it must not
    /// drop.

    ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED = -4,

    /// The server disconnected the client, stopped, or was destroyed.

    ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED = -5,

    /// A message from this client was still in the receive queue at update.
    /// See rocketnet_server_update.

    ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED = -6
};

/// rocketnet_client_state returns one of these values.
/// A connect attempt moves from DISCONNECTED through the handshake states to
/// CONNECTED. Negative values describe how an attempt or connection ended.

enum rocketnet_client_state_t
{
    /// A message was still in the receive queue at update. See
    /// rocketnet_client_update.

    ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED = -13,

    /// The connection's message memory had no room for a message it must not
    /// drop.

    ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED = -12,

    /// The two ends' configs differ in a field that must match on both ends.

    ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH = -11,

    /// The server sent what the protocol forbids.

    ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION = -10,

    /// A reliable message went unacknowledged past reliable_message_timeout_ms.

    ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT = -9,

    /// The server disconnected the client, stopped, or was destroyed.

    ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED = -8,

    /// The connect token expired before the server accepted the client.

    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED = -7,

    /// The connect token was refused before the first packet was sent.

    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN = -6,

    /// A connection heard nothing from the server for the token's timeout.

    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT = -5,

    /// The server did not answer the third handshake leg in time.

    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT = -4,

    /// The server did not answer the connection request in time.

    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT = -3,

    /// The server did not answer the cookie request in time.

    ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT = -2,

    /// The server refused the client.

    ROCKETNET_CLIENT_STATE_CONNECTION_DENIED = -1,

    /// After create, and after rocketnet_client_disconnect.

    ROCKETNET_CLIENT_STATE_DISCONNECTED = 0,

    /// Handshake, first leg.

    ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE = 1,

    /// Handshake, second leg.

    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST = 2,

    /// Handshake, third leg.

    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE = 3,

    /// Connected. Messages flow both ways.

    ROCKETNET_CLIENT_STATE_CONNECTED = 4
};

/// rocketnet_client_stats fills this struct.
/// Packet, byte, payload, resend, duplicate, receipt and budget counts are
/// the connection that is held. They start at 0 when that connection starts,
/// including a reconnect to the same or a different server, and they read 0
/// when no connection is held. Disconnect clears the held connection, so
/// they read 0 while the old connection is still finishing.
/// messages_dropped_not_connected stays on the
/// client handle. Byte counts include the IP and UDP headers of the
/// connection's address family. Rates cover the trailing second.
/// Connection measurements are 0 without a connection.

struct rocketnet_client_stats_t
{
    /// UDP datagrams sent, handshake included.

    uint64_t packets_sent;

    /// UDP datagrams received, handshake included.

    uint64_t packets_received;

    /// Bytes sent.

    uint64_t bytes_sent;

    /// Bytes received.

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

    double rtt_ms;

    /// Round-trip jitter in milliseconds.
    /// Mean excess of retained positive first-ACK RTT samples over their
    /// minimum. Zero without positive samples; samples do not expire with time.

    double jitter_ms;

    /// Outbound session packets not yet acknowledged, as a percentage of
    /// eligible retained sends. Excludes the newest half-window of sequences;
    /// zero without eligible sends. Delayed or lost ACKs affect this estimate.
    /// The window advances with sent sequences, not elapsed time.

    double packet_loss_percent;

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
/// Per-server counts start at 0 on create and accumulate across starts and stops.
/// Byte counts are as in rocketnet_client_stats_t.
/// Rates cover the whole server over the trailing second.
/// Rates are 0 while the server is stopped.

struct rocketnet_server_stats_t
{
    /// UDP datagrams sent.

    uint64_t packets_sent;

    /// UDP datagrams received.

    uint64_t packets_received;

    /// Bytes sent.

    uint64_t bytes_sent;

    /// Bytes received.

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
/// Values cover the current connection; a new connection resets them to 0.
/// Byte counts, rtt, jitter and loss are as in rocketnet_client_stats_t.
/// Rates cover the trailing second.

struct rocketnet_server_client_stats_t
{
    /// UDP datagrams sent to this client.

    uint64_t packets_sent;

    /// UDP datagrams received from this client.

    uint64_t packets_received;

    /// Bytes sent to this client.

    uint64_t bytes_sent;

    /// Bytes received from this client.

    uint64_t bytes_received;

    /// Round-trip time in milliseconds, the smallest recent sample.

    double rtt_ms;

    /// Round-trip jitter in milliseconds.

    double jitter_ms;

    /// The loss estimate, as a percentage.

    double packet_loss_percent;

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
#endif // #if defined( __clang__ )
static_assert( sizeof( enum rocketnet_status_t ) == sizeof( int ) && sizeof( enum rocketnet_log_level_t ) == sizeof( int ) &&
                   sizeof( enum rocketnet_reliability_t ) == sizeof( int ) && sizeof( enum rocketnet_receipt_status_t ) == sizeof( int ) &&
                   sizeof( enum rocketnet_disconnect_reason_t ) == sizeof( int ) && sizeof( enum rocketnet_client_state_t ) == sizeof( int ),
               "RocketNet's public enums are the size of int: build without -fshort-enums" );
#if defined( __clang__ )
#pragma clang diagnostic pop
#endif // #if defined( __clang__ )
#elif defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 201112L
#if defined( __clang__ )
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wpre-c11-compat"
#endif // #if defined( __clang__ )
_Static_assert( sizeof( enum rocketnet_status_t ) == sizeof( int ) && sizeof( enum rocketnet_log_level_t ) == sizeof( int ) &&
                    sizeof( enum rocketnet_reliability_t ) == sizeof( int ) && sizeof( enum rocketnet_receipt_status_t ) == sizeof( int ) &&
                    sizeof( enum rocketnet_disconnect_reason_t ) == sizeof( int ) && sizeof( enum rocketnet_client_state_t ) == sizeof( int ),
                "RocketNet's public enums are the size of int: build without -fshort-enums" );
#if defined( __clang__ )
#pragma clang diagnostic pop
#endif // #if defined( __clang__ )
#endif // #if defined( __cplusplus ) && __cplusplus >= 201103L

/// An opaque client. rocketnet_client_create makes one and
/// rocketnet_client_destroy frees it. Use each client from one thread at a
/// time.
struct rocketnet_client_t;
/// An opaque server. rocketnet_server_create makes one and
/// rocketnet_server_destroy frees it. Use each server from one thread at a
/// time.
struct rocketnet_server_t;

/// Lifecycle and logging

/// Returns the version this library was built with. Compare it with
/// ROCKETNET_VERSION to detect a header and library mismatch.
/// This function works before init.
///
/// @return The library's ROCKETNET_VERSION value.

ROCKETNET_EXPORT uint32_t rocketnet_version( void );

/// Initializes the library, platform and crypto state. Call before creating
/// clients or servers. Do not race this call with term.
///
/// All allocations go through this allocator, except the memory the operating
/// system must own: AF_XDP UMEM (page-aligned and registered with the kernel),
/// io_uring rings (mapped by the kernel and liburing) and thread stacks
/// (created at the platform's default size). The RIO registered region comes
/// from this allocator: RIO registration takes committed memory at the
/// allocator's ROCKETNET_ALLOCATION_ALIGNMENT (64-byte) alignment and locks
/// the pages it spans, so it needs no page alignment of its own.
/// NULL for both selects the system allocator. Passing only one
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
///
/// @param context Passed to malloc_function and free_function for init's own
/// allocations. May be NULL.
/// @param malloc_function Allocates bytes, or NULL for the system allocator.
/// @param free_function Frees a pointer malloc_function returned, or NULL for
/// the system allocator.
/// @return ROCKETNET_STATUS_OK, or one of the failures above.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_init( void * context, void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) );

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
///
/// @param status The status to name.
/// @return The status name, or "UNKNOWN". Never NULL.

ROCKETNET_EXPORT const char * rocketnet_status_name( enum rocketnet_status_t status );

/// Receives the condition, function and file for a failed library assert,
/// plus its source line. The strings are borrowed. The callback runs on the
/// thread that fires the assert.

typedef void ( *rocketnet_assert_function_t )( const char * condition, const char * function, const char * file, int line );

/// Installs the callback for failed library asserts. NULL restores the
/// default, which logs and traps. The callback runs only in debug builds with
/// asserts enabled. Install before concurrent library calls because reads are
/// not synchronized.
///
/// @param function The callback for failed asserts, or NULL for the default.

ROCKETNET_EXPORT void rocketnet_assert_function( rocketnet_assert_function_t function );

/// Suppresses all logs when true. When false, logs follow the current level.
/// This process-wide setting does not change the log level. Safe to call from
/// any thread, and before init.
///
/// @param flag true to suppress every log line, false to resume them.

ROCKETNET_EXPORT void rocketnet_quiet( bool flag );

/// Sets the process-wide maximum log level. Values must be from
/// ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM. Other values are
/// ignored. The default is ROCKETNET_LOG_LEVEL_INFO. Safe to call from any
/// thread, and before init.
///
/// @param level The most verbose level to emit.

ROCKETNET_EXPORT void rocketnet_log_level( enum rocketnet_log_level_t level );

/// Enables or disables network thread priority changes. Enabled by default.
/// On Windows, init also requests a 1 ms timer period until term. Disable this
/// if the game sets thread priorities itself.
///
/// Set before init to skip the timer request. New threads use the current
/// setting. Running threads keep their existing priority. OS refusal never
/// fails create or init. The thread keeps its default priority. The first
/// refusal of each kind logs one warning. Stats count the refusals. Safe to
/// call from any thread.
///
/// @param enabled true to let the library raise its network threads'
/// priority, false to leave every thread at its default.

ROCKETNET_EXPORT void rocketnet_thread_priority( bool enabled );

/// Returns the log level name, such as "ERROR" or "SPAM". Returns "UNKNOWN"
/// for other values and never returns NULL. The strings last for the process.
/// Use this to format a level like the default log sink.
///
/// @param level The level to name.
/// @return The level name, or "UNKNOWN". Never NULL.

ROCKETNET_EXPORT const char * rocketnet_log_level_name( enum rocketnet_log_level_t level );

/// Receives one formatted log line on the thread that logs it. This may be a
/// network thread, so the callback must be thread-safe. It must not call
/// rocketnet_log_function. context is the installed pointer. message is
/// NUL-terminated and length excludes the NUL. The message is at most 1023
/// bytes and is valid only until the callback returns.

typedef void ( *rocketnet_log_function_t )( void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

/// Installs the log callback and its context. NULL restores the default sink,
/// which prints to stdout, and clears the context. An in-flight call may still
/// use the old callback, so keep its context valid until that call returns.
/// Safe to call from any thread.
///
/// @param function The callback for every log line, or NULL for the default
/// sink.
/// @param context Passed to every call of function. Ignored when function is
/// NULL.

ROCKETNET_EXPORT void rocketnet_log_function( rocketnet_log_function_t function, void * context );

/// API misuse

/// The stats structs' api_misuses field counts these errors.
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
/// A NULL object, NULL required argument or out-of-range client index makes
/// the call do nothing and return zero, false, NULL,
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
/// - A send while not CONNECTED (see the start_message calls).
/// - Update or send_packets on a stopped server does nothing.
/// - A setter for a disconnected client or empty server slot returns OK and
///   does nothing. An invalid value returns INVALID_ARGUMENT.

/// Returns monotonic elapsed seconds from an unspecified origin.

ROCKETNET_EXPORT double rocketnet_time( void );

/// Sleeps for seconds. Negative, zero or NaN returns immediately. Values
/// above 2000 sleep for 2000 seconds.
///
/// @param seconds How long to sleep, in seconds.

ROCKETNET_EXPORT void rocketnet_sleep( double seconds );

/// Config

/// Initialize each config with its default_config function, then set the
/// fields the game needs. Environment values override config values. Every
/// field except callbacks and config_bytes names its environment variable.
/// Overrides are logged with the displaced value. Invalid values refuse
/// create. Values are not clamped. Empty variables are ignored and logged.
/// Create reads the environment once, before it validates the config and
/// sizes memory; connect and start use that resolved config and never read
/// the environment again.
///
/// Integers use decimal. Byte arrays use base64 with exactly the field's
/// length. Strings are used as-is.
///
/// Transport is a comma-separated list tried from left to right at client
/// connect or server start. The first transport that starts wins. "auto" is
/// the default, including NULL or an empty string. Development servers try
/// xdp,gso,io_uring,portable on Linux; msgx,portable on macOS; and
/// portable,iocp,rio on Windows. Production servers try
/// xdp,gso,io_uring,portable on Linux; msgx,portable on macOS; and
/// iocp,rio,portable on Windows, keeping native server transports first.
/// io_uring requires build support. Linux clients try gso,portable; macOS
/// clients try msgx,portable; and Windows clients try iocp,portable.
/// Loopback is selected by name, never by auto. An
/// explicit server pin of "portable" is available in either build.
/// A known name that this build does not offer for this endpoint on this
/// platform (xdp on a client, for example) is skipped with a log.
/// A list with at least one name that is not skipped is a strict pin: only
/// those transports are tried, and if all of them refuse, connect or start
/// fails. A list whose every name is skipped falls back to this endpoint's
/// auto order with a warning, so one fleet-wide list deploys unchanged to
/// mixed machines.
/// Unknown names, empty elements and "auto" inside a list refuse the
/// list; an unknown name reports the valid names.
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
    /// Default is NULL.

    const char * transport;

    /// Called when the handshake state changes. NULL disables the callback. It
    /// runs on the caller thread during update, disconnect and destroy. Do not
    /// call client functions from it. Default is NULL.

    void ( *state_change_callback )( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state );

    /// ROCKETNET_CLIENT_PACKET_FILTER is the title packet filter. It must match
    /// the server's value. Default is the published test packet filter, which
    /// a production build refuses at create.

    uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES];

    /// ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES sets how many fragmented
    /// messages are reassembled at once. Range is 1 .. 256. Zero, the
    /// default, selects 8.

    int max_fragmented_messages;

    /// ROCKETNET_CLIENT_SEND_KBPS sets the starting send budget per
    /// connection, in kbps. Range is 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1.
    /// The unlimited value means no send budget. Zero selects
    /// ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, which is also the default.
    /// Negative values return ROCKETNET_STATUS_INVALID_ARGUMENT.

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
    /// 1048576. Default is 1048576.

    int receive_message_buffer_bytes;

    /// ROCKETNET_CLIENT_MAX_MESSAGE_BYTES sets the largest message. Both ends
    /// must match. It cannot exceed receive_message_buffer_bytes. Range is
    /// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 .. ROCKETNET_MAX_MESSAGE_BYTES.
    /// Default is 1048576. It is the one size limit for every reliability
    /// type: an unreliable message larger than
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES is sent as its reliable twin,
    /// so it is held to max_message_bytes too.

    int max_message_bytes;

    /// ROCKETNET_CLIENT_SEND_MESSAGE_BUFFER_BYTES sets queued message bytes.
    /// Range is 12288 .. 268435456. Default is 4194304.

    int send_message_buffer_bytes;

    /// ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES sets the queued message limit.
    /// Range is 16 .. 1048576. Default is 1024.

    int send_queue_messages;

    /// ROCKETNET_CLIENT_PACKETS_IN_FLIGHT sets the sent map: the
    /// unacknowledged packets tracked for their acks. Range is 64 .. 1048576
    /// and at least reliable_messages_in_flight, so a window sent
    /// one number to a packet never evicts an entry before its ack lands.
    /// Default is 1024.

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
/// ROCKETNET_SERVER_PROTOCOL_ID.
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
    /// tokens must use it. Default is 0, which start refuses with
    /// ROCKETNET_STATUS_INVALID_PROTOCOL_ID.

    uint64_t protocol_id;

    /// ROCKETNET_SERVER_MAX_CLIENTS sets the client limit. Range is 1 ..
    /// ROCKETNET_MAX_CLIENTS. Default is 1000.
    ///
    /// It also sizes the server socket's receive buffer request to hold one
    /// update's inbound traffic: ceil( max_clients x 1000 kbps x 125 x 2 /
    /// 10 Hz ) bytes, at least 1,000,000 and at most INT_MAX (25,000,000 at
    /// 1000 clients). Linux silently clamps it to net.core.rmem_max (about
    /// 208 KB by default) and macOS to kern.ipc.maxsockbuf; the server reads
    /// the grant back and logs one line with it, a warning naming the limit
    /// to raise when the grant is below the request (USAGE.md, "Receive
    /// buffer sizing").

    int max_clients;

    /// ROCKETNET_SERVER_PACKET_FILTER is the title packet filter. It must
    /// match the clients' value. Default is the published test packet filter,
    /// which a production build refuses at create.

    uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES];

    /// ROCKETNET_SERVER_BACKEND_PUBLIC_KEY is the backend's public key. Start
    /// requires a nonzero value or returns ROCKETNET_STATUS_MISSING_KEYS.
    /// Default is all zeros.

    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];

    /// ROCKETNET_SERVER_NUM_XDP_QUEUES applies to xdp, gso and io_uring.
    /// Zero, the default, selects the transport's own. Range is 1 .. 1024.
    /// The Windows iocp transport ignores it and always runs one completion
    /// worker plus one send worker; the Windows rio transport ignores it and
    /// runs one I/O worker.

    int num_xdp_queues;

    /// ROCKETNET_SERVER_NUM_OS_CPUS applies to xdp. It sets cores left for
    /// the operating system. Zero, the default, selects the transport's own.
    /// Range is 1 .. 256.

    int num_os_cpus;

    /// ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES sets how many fragmented
    /// messages are reassembled at once per client. Range is 1 .. 256. Zero,
    /// the default, selects 8.

    int max_fragmented_messages;

    /// ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS sets the maximum timeout. Units
    /// are seconds. Range is 1 .. 3600. Default is 60.

    int max_timeout_seconds;

    /// ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS sets the handshake
    /// deadline. Units are seconds. Range is 1 .. 60. Default is 5.

    int handshake_deadline_seconds;

    /// ROCKETNET_SERVER_TRANSPORT sets the candidate list. NULL means "auto".
    /// Every transport serves IPv4, IPv6, or both. Default is NULL.

    const char * transport;

    /// ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS sets each connection's starting
    /// send budget, in kbps. Range is 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1.
    /// The unlimited value means no send budget. Zero selects
    /// ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, which is also the default.
    /// Negative values return ROCKETNET_STATUS_INVALID_ARGUMENT.

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
    /// 1048576. Default is 1048576.

    int receive_message_buffer_bytes;

    /// ROCKETNET_SERVER_MAX_MESSAGE_BYTES sets the largest message. Both ends
    /// must match. It cannot exceed receive_message_buffer_bytes. Range is
    /// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 .. ROCKETNET_MAX_MESSAGE_BYTES.
    /// Default is 1048576. It is the one size limit for every reliability
    /// type: an unreliable message larger than
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES is sent as its reliable twin,
    /// so it is held to max_message_bytes too.

    int max_message_bytes;

    /// ROCKETNET_SERVER_SEND_MESSAGE_BUFFER_BYTES sets queued message bytes
    /// per client. Range is 12288 .. 268435456. Default is 4194304.

    int send_message_buffer_bytes;

    /// ROCKETNET_SERVER_SEND_QUEUE_MESSAGES sets queued messages per client.
    /// Range is 16 .. 1048576. Default is 1024.

    int send_queue_messages;

    /// ROCKETNET_SERVER_PACKETS_IN_FLIGHT sets the sent map per client: the
    /// unacknowledged packets tracked for their acks. Range is 64 .. 1048576
    /// and at least reliable_messages_in_flight, so a window sent
    /// one number to a packet never evicts an entry before its ack lands.
    /// Default is 1024.

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

    /// ROCKETNET_SERVER_DISCONNECT_PACKETS_PER_UPDATE sets the whole-server
    /// allowance for sealing redundant disconnect-notification datagrams in
    /// one update. Range is 1 .. 1024. Default is 64. A disconnect still
    /// closes its local slot immediately; a value below the 10-copy burst
    /// spreads that burst across updates. If every deferred-burst slot is
    /// occupied, the server keeps the older work and drops the new redundant
    /// notification copies rather than exceeding this allowance.

    int disconnect_packets_per_update;

    /// ROCKETNET_SERVER_HANDSHAKES_PER_UPDATE sets the handshake work budget:
    /// the most connection requests with a valid cookie the server processes
    /// in one update, each one signature verification and, for a valid token,
    /// the key exchange. Range is 1 .. 1024. Default is 16. A request past it
    /// is dropped unanswered and counted in handshakes_dropped_over_budget;
    /// the client sends it again and a later update processes it, so a mass
    /// join or a flood of requests spreads across updates.

    int handshakes_per_update;
};

/// Messages

/// Create allocates the client or server as one block through the configured
/// allocator. Connect and start then allocate transport memory through the
/// same allocator, before any traffic: sockets, worker threads, I/O rings and
/// buffers, and on the server its per-client protocol tables. Nothing in the
/// frame loop below allocates. Disconnect and stop retire the transport; the
/// next connect or start frees it before it allocates the next, and destroy
/// frees it, so a reconnect or restart takes the same bytes again and the
/// peak never grows across cycles. The required_memory calls report that
/// peak: the most the allocator holds over create, connect or start,
/// disconnect or stop, reconnect or restart, and destroy, each allocation
/// with its alignment header, for whichever transport of the config's list
/// is granted (the largest of them). For the planned built-in transports,
/// with unchanged configuration and environment overrides, an allocator over
/// a region of that size never runs out. The development link simulator is
/// included in development builds.
///
/// Not counted, because the operating system owns it: each worker thread's
/// stack (the platform default: pthread's default attribute, or the
/// executable's /STACK reserve, 1 MiB by default, on Windows); on the xdp
/// transport the page-aligned UMEM, 256 MiB per queue (65536 frames of 4096
/// bytes), and the AF_XDP rings; on the
/// io_uring transport its rings and each queue's provided buffer ring; kernel
/// socket buffers. A custom transport's own memory and a
/// ROCKETNET_SERVER_ or ROCKETNET_CLIENT_ environment
/// override changed after the call are not counted either. Connect and start
/// log each candidate's plan at info level ("client memory plan",
/// "server memory plan").
///
/// Pass the same game clock to every update, in seconds.
/// Run each frame on one thread. Update first, receive until NULL, simulate,
/// write messages with start and finish, then send packets.
///
/// Reliable messages wait for send budget. Unreliable messages over budget are
/// dropped and counted in messages_dropped_over_budget.
/// Running out of message memory ends the connection with
/// MESSAGE_MEMORY_EXHAUSTED. Ask first with rocketnet_client_can_send_message
/// or rocketnet_server_can_send_message.

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

/// Each connection starts fresh.
/// The last failure state remains until disconnect or the next connect.
/// Connect opens a socket for the address family in the token. When the token
/// names both families and the host can use both, connect opens a socket for
/// each, the cookie request goes to both servers, and the first cookie commits
/// the connection to the family that answered; the other socket is closed
/// then. The token's lifetime and timeout bound the attempt as before.

/// Fills defaults within config_bytes and leaves config_bytes unchanged.
/// Returns ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED if the size is too small.
/// Use rocketnet_client_default_config to initialize a config.
///
/// Needs no client and no library state. Thread: any thread, one config at a
/// time.
///
/// @param config The config to fill. Its config_bytes must already be set.
/// @return ROCKETNET_STATUS_OK, ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
/// config, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED when config_bytes is
/// smaller than the config_bytes field itself.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config_internal( struct rocketnet_client_config_t * config );

/// Initializes every config field to its default and sets config_bytes.
/// Returns ROCKETNET_STATUS_OK on success.
///
/// Needs no client and no library state. Thread: any thread, one config at a
/// time.
///
/// @param config The config to initialize.
/// @return ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a
/// NULL config.

static inline enum rocketnet_status_t rocketnet_client_default_config( struct rocketnet_client_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );

    return rocketnet_client_default_config_internal( config );
}

/// Fills the light profile within config_bytes and leaves config_bytes
/// unchanged. Returns ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED if the size
/// is too small. Use rocketnet_client_light_config to initialize a config.
///
/// @param config The config to fill. Its config_bytes must already be set.
/// @return ROCKETNET_STATUS_OK, ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
/// config, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED when config_bytes is
/// smaller than the config_bytes field itself.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_light_config_internal( struct rocketnet_client_config_t * config );

/// Initializes every config field to its default, then sets the light-client
/// geometry: messages fit in one packet, one per 100 ms, 10 Hz, with a 256-kbps
/// send budget. Use it in place of rocketnet_client_default_config. It changes
/// no default and adds no field. Returns ROCKETNET_STATUS_OK on success.
///
/// Needs no client and no library state. Thread: any thread, one config at a
/// time.
///
/// @param config The config to initialize. Its config_bytes is set here.
/// @return ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a
/// NULL config.

static inline enum rocketnet_status_t rocketnet_client_light_config( struct rocketnet_client_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );

    return rocketnet_client_light_config_internal( config );
}

/// Returns the most memory the allocator holds for this config over the
/// client's lifecycle: create's block plus the transport each connect
/// allocates (see Messages above). It is at least the block create needs.
/// Applies the same environment overrides as create.
/// NULL config selects defaults. Invalid config returns 0.
///
/// Needs no client. Thread: any thread; it reads only config and the
/// environment.
///
/// @param config The config to size, or NULL for the defaults.
/// @return The most bytes the allocator holds for the client over its
/// lifecycle, or 0 when the config, its config_bytes or an environment
/// override is invalid.

ROCKETNET_EXPORT size_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );

#if ROCKETNET_DEV

/// The public development backend key. Use it to initialize backend_public_key.
///
///     static const uint8_t dev_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] =
///         ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
///     memcpy( config.backend_public_key, dev_key, sizeof( dev_key ) );
///
/// A server using this key accepts development tokens.
/// A production server refuses it with ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION.

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
///
/// Needs no client or server. Thread: any thread; it touches only its
/// arguments.
///
/// @param server_address_ipv4 The server's IPv4 address as "a.b.c.d:port", or
/// NULL.
/// @param server_address_ipv6 The server's IPv6 address as "[v6]:port", or
/// NULL.
/// @param server_public_key ROCKETNET_SERVER_PUBLIC_KEY_BYTES from
/// rocketnet_server_public_key.
/// @param expire_seconds The token lifetime in seconds. Must be positive.
/// @param timeout_seconds The longest silence the connection allows, in
/// seconds. Must be positive.
/// @param client_id The client's id, carried to the server.
/// @param protocol_id The title's protocol id. Must be nonzero.
/// @param unix_timestamp The creation time in Unix seconds.
/// @param user_data ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES carried to the
/// server, or NULL for zeros.
/// @param connect_token Receives the NUL-terminated token string. Holds
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES.
/// @return ROCKETNET_STATUS_OK, or one of the failures above.

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
/// In a development build the settings apply at once to a live connection, on
/// and off, in either direction. Every development connection owns a
/// simulator from the start, so a zero setting costs the memory of one and
/// neither latency nor a copy. Out-of-range values count as API misuse and
/// leave the settings unchanged.
///
/// These environment variables set the values at create.
/// ROCKETNET_DEV_CLIENT_LATENCY_MS
/// ROCKETNET_DEV_CLIENT_JITTER_MS
/// ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT
/// ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT
///
/// Callable in every client state. While DISCONNECTED the settings are kept
/// for the next connect. Thread: not thread-safe; the caller owns the client
/// one thread at a time.
///
/// @param client The client whose outgoing traffic is simulated.
/// @param latency_ms Added delay in milliseconds, 0 .. 60000.
/// @param jitter_ms Most random extra delay in milliseconds, 0 .. 60000.
/// @param packet_loss_percent Percentage of packets dropped, 0 .. 100.
/// @param duplicate_percent Percentage delivered twice, 0 .. 100.

ROCKETNET_EXPORT void rocketnet_client_network_simulator( struct rocketnet_client_t * client, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );

/// Simulates outgoing server packets with the same ranges as the client call.
/// In a development build the settings apply at once to a running server, on
/// and off, in either direction. Every development server owns a simulator
/// from the start, so a zero setting costs the memory of one and neither
/// latency nor a copy. Invalid values count as API misuse and leave the
/// settings unchanged.
/// Use the ROCKETNET_DEV_SERVER_LATENCY_MS, ROCKETNET_DEV_SERVER_JITTER_MS,
/// ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT and
/// ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT environment variables at create.
///
/// Callable running or stopped. While stopped the settings are kept for the
/// next start. Thread: not thread-safe; the caller owns the server one thread
/// at a time.
///
/// @param server The server whose outgoing traffic is simulated.
/// @param latency_ms Added delay in milliseconds, 0 .. 60000.
/// @param jitter_ms Most random extra delay in milliseconds, 0 .. 60000.
/// @param packet_loss_percent Percentage of packets dropped, 0 .. 100.
/// @param duplicate_percent Percentage delivered twice, 0 .. 100.

ROCKETNET_EXPORT void rocketnet_server_network_simulator( struct rocketnet_server_t * server, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );

#endif // ROCKETNET_DEV

/// Creates a DISCONNECTED client without opening a socket.
/// Validates the config and logs any failure. NULL config selects defaults.
/// context is passed to the allocator and state_change_callback.
/// Keep the client and context valid until destroy.
///
/// Returns NULL on failure. If status is supplied, it receives
/// ROCKETNET_STATUS_OK on success or the failure code.
/// Call rocketnet_init before create.
///
/// Thread: any thread. The caller then owns the returned client and uses it
/// from one thread at a time.
///
/// @param context Passed to the allocator and state_change_callback. May be
/// NULL.
/// @param config The config, filled by rocketnet_client_default_config or
/// rocketnet_client_light_config, or NULL for the defaults.
/// @param status Receives the result, or NULL.
/// @return The client, or NULL on failure. The status is
/// ROCKETNET_STATUS_OK, ROCKETNET_STATUS_NOT_INITIALIZED before rocketnet_init,
/// ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED, ROCKETNET_STATUS_CONFIG_ENV_INVALID,
/// ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, ROCKETNET_STATUS_INVALID_ARGUMENT for
/// a negative send_kbps, ROCKETNET_STATUS_INVALID_TRANSPORT_LIST,
/// ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION,
/// ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED or ROCKETNET_STATUS_OUT_OF_MEMORY.

ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, enum rocketnet_status_t * status );

/// Starts a connection using a token string.
/// The string must end with NUL within
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES.
/// Malformed strings return ROCKETNET_STATUS_INVALID_CONNECT_TOKEN without
/// changing the client.
///
/// Connect accepts DISCONNECTED and every negative terminal state. From a
/// terminal state with an old transport it first requests shutdown; while its
/// worker still owns that attempt it returns ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED
/// and retains the terminal state. Poll rocketnet_client_disconnected and retry
/// with the token. After completion connect joins and frees the old attempt,
/// then starts the new one. A connect from a negative state starts a fresh
/// attempt that carries no key, replay window, sequence, cookie or message
/// state of the earlier one. A negative state stays until disconnect or the
/// next connect; explicit disconnect clears it to DISCONNECTED. Active
/// handshake and CONNECTED states are refused.
///
/// Failed token checks set INVALID_CONNECT_TOKEN or CONNECT_TOKEN_EXPIRED.
/// A token naming a server address with port 0 returns
/// ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO and leaves the client DISCONNECTED.
/// A socket or thread failure leaves the client DISCONNECTED.
/// The connection clock starts at the next update.
/// Returns ROCKETNET_STATUS_OK on success or a failure status.
///
/// Thread: not thread-safe; the caller owns the client one thread at a time.
///
/// @param client The client.
/// @param connect_token The NUL-terminated base64 connect token string.
/// @return ROCKETNET_STATUS_OK once the attempt has started, or
/// ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL argument,
/// ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED during a handshake, while
/// CONNECTED or while an old attempt still stops,
/// ROCKETNET_STATUS_INVALID_CONNECT_TOKEN, ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED,
/// ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO,
/// ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION for a development token in a
/// production build, ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED when no
/// transport can open,
/// ROCKETNET_STATUS_OUT_OF_MEMORY, ROCKETNET_STATUS_MUTEX_CREATE_FAILED or
/// ROCKETNET_STATUS_THREAD_CREATE_FAILED.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const char * connect_token );

/// Expects rocketnet_client_disconnected(client) to be true; asserts otherwise.
/// Called early, it logs one error naming the skipped completion poll, records
/// the misuse, requests disconnect and updates the transport a bounded number
/// of times without sleeping. Once disconnected it joins, wipes and frees; if
/// the worker has not exited it logs a second error and keeps the client, which
/// stays owned by that worker. Client pointers become invalid only when destroy frees
/// the client; on the keep path the client and its pointers stay valid.
///
/// Call it once rocketnet_client_disconnected is true, in any client state.
/// Thread: not thread-safe; the caller owns the client one thread at a time.
///
/// @param client The client, or NULL to do nothing.

ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

/// Processes incoming messages and connection timeouts at the supplied time.
/// Read the client state and stats after updating.
///
/// Every message delivered by the previous update must already be taken:
/// loop rocketnet_client_receive_messages until it returns NULL before the
/// next update. A message still in the receive queue at update is API
/// misuse: it asserts in a debug build, counts MESSAGE_NOT_RECEIVED, and the
/// connection ends in ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED.
///
/// Callable in every client state. Before the first connect it does
/// nothing; after disconnect it steps the old attempt's shutdown. Thread: not
/// thread-safe; the caller owns the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and does nothing.
/// @param time The game clock in seconds, the same clock on every call. A
/// time that is not finite or goes backwards is clamped to the last good time
/// and counted as API misuse.

ROCKETNET_EXPORT void rocketnet_client_update( struct rocketnet_client_t * client, double time );

/// Sends queued messages in packets, subject to the send budget.
///
/// Callable in every client state. Before the first connect and after
/// disconnect it does nothing. Thread: not thread-safe; the caller owns the
/// client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and does nothing.

ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

/// Ends the connection and returns the client to DISCONNECTED.
/// Sends disconnect packets when the connection has session keys.
/// Requests worker shutdown without waiting. Poll rocketnet_client_disconnected
/// before reconnecting or destroying.
///
/// Callable in every client state. From a negative state it clears that
/// state to DISCONNECTED. Thread: not thread-safe; the caller owns the client
/// one thread at a time.
///
/// @param client The client. NULL counts as API misuse and does nothing.

ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

/// Returns true after disconnection has completed and workers no longer access
/// client storage. Never waits, joins or frees. Caller-stepped clients progress
/// via update.
///
/// Callable in every client state. It stays false from a connect that
/// returns ROCKETNET_STATUS_OK until disconnect, a negative state included,
/// and then until the workers stop. Thread: not thread-safe; the caller owns
/// the client one thread at a time.
///
/// @param client The client.
/// @return True once the client is disconnected and its workers have
/// stopped. False otherwise, and for a NULL client, which counts as API
/// misuse.
ROCKETNET_EXPORT bool rocketnet_client_disconnected( const struct rocketnet_client_t * client );

/// The client's state.
///
/// Callable in every client state. Thread: not thread-safe; the caller owns
/// the client one thread at a time.
///
/// @param client The client.
/// @return The current state. A NULL client logs an error, counts as API
/// misuse and returns ROCKETNET_CLIENT_STATE_DISCONNECTED.

ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

/// Returns a state name such as "CONNECTED" or "DISCONNECTED".
/// Returns "UNKNOWN" for an invalid value.
///
/// Needs no client. Thread: any thread.
///
/// @param state The state to name.
/// @return A static string, valid for the process lifetime.

ROCKETNET_EXPORT const char * rocketnet_client_state_name( enum rocketnet_client_state_t state );

/// Returns the client's server slot index while CONNECTED.
/// The index ranges from 0 to max_clients - 1. Otherwise returns -1.
///
/// Callable in every client state. Thread: not thread-safe; the caller owns
/// the client one thread at a time.
///
/// @param client The client.
/// @return The slot index while CONNECTED, or -1 in every other state and
/// for a NULL client, which counts as API misuse.

ROCKETNET_EXPORT int rocketnet_client_index( const struct rocketnet_client_t * client );

/// Returns the selected transport name or NULL while DISCONNECTED.
/// The string remains valid for the process lifetime.
///
/// Callable in every client state. Thread: not thread-safe; the caller owns
/// the client one thread at a time.
///
/// @param client The client.
/// @return The transport name, or NULL while DISCONNECTED and for a NULL
/// client, which counts as API misuse.

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
/// and increments messages_dropped_not_connected. Do not call finish after a
/// NULL start. Dropped messages are not queued for a later connection.
///
/// Needs CONNECTED. A message already open or a bad argument is refused with
/// NULL. Any send (reliable, or unreliable that finds no room in the send
/// region) that finds no message memory ends the connection with
/// MESSAGE_MEMORY_EXHAUSTED and returns NULL. Thread: not thread-safe;
/// the caller owns the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and returns NULL.
/// @param channel The channel, below the config's num_channels. The ordered
/// and sequenced modes use it; the others ignore it.
/// @param reliability The reliability mode.
/// @param max_bytes The most bytes the message may hold, from 1 to the
/// config's max_message_bytes.
/// @return Memory for max_bytes of message, or NULL when the message is
/// dropped or refused.

ROCKETNET_EXPORT uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int max_bytes );

/// Queues bytes_written from the open message and returns its nonzero serial.
/// bytes_written must be between 1 and the reserved size.
/// An invalid size logs an error, aborts the message and returns 0.
/// Calling finish without an open message asserts in a debug build and
/// returns 0 in a release build.
///
/// Needs a message opened by rocketnet_client_start_message; in every other
/// state it asserts in a debug build and returns 0 in a release build.
/// Thread: not thread-safe; the caller owns the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and returns 0.
/// @param bytes_written The bytes written into the open message.
/// @return The message's nonzero receipt serial, or 0 when nothing was
/// queued.

ROCKETNET_EXPORT uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written );

/// Cancels the open message and releases its memory.
/// Does nothing if no message is open.
///
/// Callable in every client state. Thread: not thread-safe; the caller owns
/// the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and does nothing.

ROCKETNET_EXPORT void rocketnet_client_abort_message( struct rocketnet_client_t * client );

/// Returns true if rocketnet_client_start_message with the same channel,
/// reliability and max_bytes would be given room now. Returns false if it
/// would not: the message memory is full, or the start would be dropped or
/// refused (not CONNECTED, a message already open, a bad argument).
///
/// Ask before a start you cannot afford to lose. Only start and update take
/// room, so a true answer holds until the next of either. Room returns as the
/// server acknowledges messages.
///
/// Changes no state, counts nothing and allocates nothing. Constant time.
/// A false answer may be pessimistic for a small unreliable message: start
/// can write it straight into a transport frame without message memory.
///
/// Callable in every client state; false in every state but CONNECTED.
/// Thread: not thread-safe; the caller owns the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and returns false.
/// @param channel The channel the start would use.
/// @param reliability The reliability mode the start would use.
/// @param max_bytes The max_bytes the start would reserve.
/// @return True if the start would be given room now.

ROCKETNET_EXPORT bool rocketnet_client_can_send_message( const struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int max_bytes );

/// Returns the next batch of messages from the server, or NULL when the
/// queue is empty. Each message has client_index 0. Call until NULL before the
/// next update (see rocketnet_client_update).
///
/// Callable in every client state; with no connect attempt it returns NULL.
/// Thread: not thread-safe; the caller owns the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and returns NULL.
/// @return The client's batch, valid until the next receive or update, or
/// NULL when no message is waiting.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_client_receive_messages( struct rocketnet_client_t * client );

/// Returns the number of available receipt outcomes and copies up to capacity
/// entries into out. Only min(returned_count, capacity) entries are written.
/// Pass NULL and zero capacity to query the count without copying.
/// A negative capacity, or NULL out with positive capacity, returns 0.
///
/// Use each receipt's serial to identify its send.
/// Only the three _WITH_ACK_RECEIPT reliability modes produce receipts.
/// LOSS comes only from unreliable receipt messages. Reliable ones, including
/// unreliable sends that became reliable, report ACKED only; ending a
/// connection produces no LOSS for them.
/// The outcomes recorded for a connection remain readable after that
/// connection ends, until its next update. A receipt-bearing send, reliable
/// or UNRELIABLE_WITH_ACK_RECEIPT, that is still in flight when its
/// connection ends gets no terminal outcome.
///
/// Read receipts after update. They remain until the next update.
/// Reading does not consume them. Repeated reads copy the same first entries.
/// If the returned count exceeds capacity, use a larger buffer and read again
/// before the next update to get every available receipt.
/// The queue holds max_message_receipts entries; when full it drops its
/// oldest receipt.
///
/// Callable in every client state; with no connect attempt it returns 0.
/// Thread: not thread-safe; the caller owns the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and returns 0.
/// @param out The array to fill, or NULL with zero capacity.
/// @param capacity The entries out holds.
/// @return The number of receipts available, which may exceed capacity.

ROCKETNET_EXPORT int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity );

/// Sets the connection's send budget in kbps and returns ROCKETNET_STATUS_OK.
/// Takes the values the config's send_kbps takes, which sets each
/// connection's starting rate; a negative value returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT and changes nothing.
///
/// Takes effect while CONNECTED. In every other state it returns
/// ROCKETNET_STATUS_OK and changes nothing; each new connection starts at the
/// config's rate. Thread: not thread-safe; the caller owns the client one
/// thread at a time.
///
/// @param client The client.
/// @param kbps The budget in kbps, 0 for the default rate, or
/// ROCKETNET_SEND_KBPS_UNLIMITED for no budget.
/// @return ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a
/// NULL client, a negative kbps or a rate the config's send_kbps refuses.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

/// Fills stats with the client's current stats.
///
/// Callable in every client state. Packet, byte, payload, resend, duplicate,
/// receipt and budget counts cover only the connection that is held and read
/// zero without one, including while an ended connection is retiring.
/// messages_dropped_not_connected remains on the client handle across
/// connections. Rates, round trip time, jitter and loss have the same session
/// boundary and read zero without a held connection. Thread: not thread-safe;
/// the caller owns the client one thread at a time.
///
/// @param client The client. NULL counts as API misuse and fills only
/// api_misuses and thread_priority_refusals.
/// @param stats The stats to fill. NULL counts as API misuse and does
/// nothing.

ROCKETNET_EXPORT void rocketnet_client_stats( const struct rocketnet_client_t * client, struct rocketnet_client_stats_t * stats );

/// Server

/// Clients occupy slots from 0 to max_clients - 1.
/// Only start_message takes the destination index as uint64_t.
/// A per-slot query on a free slot or a stopped server returns 0, false, NULL
/// or ROCKETNET_DISCONNECT_REASON_NONE, and a per-slot action does nothing.

/// Fills defaults within config_bytes and leaves config_bytes unchanged.
/// Returns ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED if the size is too small.
/// Use rocketnet_server_default_config to initialize a config.
///
/// Needs no server and no library state. Thread: any thread, one config at a
/// time.
///
/// @param config The config to fill. Its config_bytes must already be set.
/// @return ROCKETNET_STATUS_OK, ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
/// config, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED when config_bytes is
/// smaller than the config_bytes field itself.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config_internal( struct rocketnet_server_config_t * config );

/// Initializes every config field to its default and sets config_bytes.
/// Leaves backend_public_key empty. Returns ROCKETNET_STATUS_OK on success.
///
/// Needs no server and no library state. Thread: any thread, one config at a
/// time.
///
/// @param config The config to initialize.
/// @return ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a
/// NULL config.

static inline enum rocketnet_status_t rocketnet_server_default_config( struct rocketnet_server_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );

    return rocketnet_server_default_config_internal( config );
}

/// Fills the light profile within config_bytes and leaves config_bytes
/// unchanged. Returns ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED if the size
/// is too small. Use rocketnet_server_light_config to initialize a config.
///
/// @param config The config to fill. Its config_bytes must already be set.
/// @return ROCKETNET_STATUS_OK, ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
/// config, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED when config_bytes is
/// smaller than the config_bytes field itself.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_light_config_internal( struct rocketnet_server_config_t * config );

/// Initializes every config field to its default, then sets the light-client
/// geometry: messages fit in one packet, one per 100 ms to each client, 10 Hz, with a 256-kbps
/// send budget per client. Use it in place of rocketnet_server_default_config.
/// The caller still sets its protocol id and backend key. It changes no
/// default and adds no field. Returns ROCKETNET_STATUS_OK on success.
///
/// Needs no server and no library state. Thread: any thread, one config at a
/// time.
///
/// @param config The config to initialize. Its config_bytes is set here.
/// @return ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a
/// NULL config.

static inline enum rocketnet_status_t rocketnet_server_light_config( struct rocketnet_server_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );

    return rocketnet_server_light_config_internal( config );
}

/// Returns the most memory the allocator holds for this config and
/// max_clients over the server's lifecycle: create's block plus the
/// transport and per-client tables each start allocates (see Messages above).
/// It is at least the block create needs.
/// Applies the same environment overrides as create.
/// NULL config selects defaults. Invalid config returns 0, including the
/// defaults' test filter in a production build. A named transport memory
/// estimate is diagnostic and does not grant that transport at create or start.
///
/// Needs no server. Thread: any thread; it reads only config and the
/// environment.
///
/// @param config The config to size, or NULL for the defaults.
/// @return The most bytes the allocator holds for the server over its
/// lifecycle, or 0 when the config, its config_bytes or an environment
/// override is invalid.

ROCKETNET_EXPORT size_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );

/// Creates a stopped server. No address is bound until start.
/// Validates the config and logs any failure. The backend key is checked at
/// start. NULL config selects defaults, which lack a backend key.
///
/// ipv4_address accepts "a.b.c.d:port". ipv6_address accepts "[v6]:port".
/// Either may be NULL. That family is then not served. Both NULL is refused.
/// With both, the two share one port.
/// The given addresses are the advertised addresses that connect tokens are
/// checked against. Every transport except xdp binds the wildcard address of the family
/// (0.0.0.0 or [::], or [::] for both families) on the given port, so a
/// specific IP is not a bind restriction. xdp attaches to the interface that
/// carries the given address.
/// ROCKETNET_SERVER_IPV4_ADDRESS overrides ipv4_address.
/// ROCKETNET_SERVER_IPV6_ADDRESS overrides ipv6_address.
/// Port 0 requests an ephemeral port. Read it after start with
/// rocketnet_server_port.
///
/// context is passed to allocator callbacks. Returns, status and lifetime
/// follow rocketnet_client_create.
///
/// Thread: any thread. The caller then owns the returned server and uses it
/// from one thread at a time.
///
/// @param context Passed to the allocator callbacks. May be NULL.
/// @param ipv4_address The IPv4 address to serve, as "a.b.c.d:port", or NULL.
/// @param ipv6_address The IPv6 address to serve, as "[v6]:port", or NULL.
/// @param config The config, filled by rocketnet_server_default_config or
/// rocketnet_server_light_config, or NULL for the defaults.
/// @param status Receives the result, or NULL.
/// @return The server, or NULL on failure. The status is ROCKETNET_STATUS_OK,
/// ROCKETNET_STATUS_NOT_INITIALIZED before rocketnet_init,
/// ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED, ROCKETNET_STATUS_CONFIG_ENV_INVALID,
/// ROCKETNET_STATUS_INVALID_ARGUMENT for no address or a negative
/// per_client_send_kbps, ROCKETNET_STATUS_INVALID_ADDRESS,
/// ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, ROCKETNET_STATUS_INVALID_TRANSPORT_LIST,
/// ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION,
/// ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED, ROCKETNET_STATUS_OUT_OF_MEMORY or
/// ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the server key cannot be made.

ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * ipv4_address, const char * ipv6_address, const struct rocketnet_server_config_t * config, enum rocketnet_status_t * status );

/// Binds the transport and accepts up to max_clients clients.
/// Returns ROCKETNET_STATUS_OK on success.
/// A running or still-stopping server returns ROCKETNET_STATUS_SERVER_NOT_STOPPED.
/// Once stopped is true, start synchronously finalizes the old run before binding.
/// Other failures return their status and leave the server stopped.
///
/// Needs a stopped server. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @return ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a
/// NULL server, ROCKETNET_STATUS_SERVER_NOT_STOPPED,
/// ROCKETNET_STATUS_MISSING_KEYS without a backend key,
/// ROCKETNET_STATUS_INVALID_PROTOCOL_ID, ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION,
/// ROCKETNET_STATUS_INVALID_ADDRESS, ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED,
/// ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED,
/// ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED,
/// ROCKETNET_STATUS_TRANSPORT_NOT_BUILT,
/// ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED when no transport can bind,
/// ROCKETNET_STATUS_OUT_OF_MEMORY or ROCKETNET_STATUS_MUTEX_CREATE_FAILED.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_start( struct rocketnet_server_t * server );

/// Requests shutdown and queues best-effort disconnect packets to all clients.
/// Does not wait for workers. Poll rocketnet_server_stopped before restart or
/// destroy. Repeated requests do nothing.
///
/// Needs a running server; stopped or stopping it does nothing. Running is
/// false as soon as it returns. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and does nothing.

ROCKETNET_EXPORT void rocketnet_server_stop( struct rocketnet_server_t * server );

/// Returns true once the stop request has completed. Never waits or joins.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @return True when the server is stopped and its workers have exited,
/// including a server never started. False while running or stopping, and
/// for a NULL server, which counts as API misuse.
ROCKETNET_EXPORT bool rocketnet_server_stopped( const struct rocketnet_server_t * server );

/// true while running, false while stopped.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @return True between a successful start and the next stop. False
/// otherwise, and for a NULL server, which counts as API misuse.

ROCKETNET_EXPORT bool rocketnet_server_running( const struct rocketnet_server_t * server );

/// As rocketnet_client_destroy, with rocketnet_server_stopped and stop in place
/// of disconnected and disconnect. Freeing also wipes the server's keys.
///
/// Call it once rocketnet_server_stopped is true. Thread: not thread-safe;
/// the caller owns the server one thread at a time.
///
/// @param server The server, or NULL to do nothing.

ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

/// Processes incoming messages and connection timeouts for every client.
/// Read the client slot queries and stats after updating.
///
/// Every message delivered by the previous update must already be taken:
/// loop rocketnet_server_receive_messages until it returns NULL before the
/// next update. A message still in the receive queue at update is API
/// misuse: it asserts in a debug build, counts MESSAGE_NOT_RECEIVED, and the
/// sending client's slot is disconnected with
/// ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED.
///
/// Needs a running server; stopped or stopping it does nothing. Thread: not
/// thread-safe; the caller owns the server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and does nothing.
/// @param time The game clock in seconds, the same clock on every call. A
/// time that is not finite or goes backwards is clamped to the last good time
/// and counted as API misuse.

ROCKETNET_EXPORT void rocketnet_server_update( struct rocketnet_server_t * server, double time );

/// Sends each client's queued messages in packets under its send budget.
///
/// Needs a running server; stopped or stopping it does nothing. Thread: not
/// thread-safe; the caller owns the server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and does nothing.

ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

/// Returns the bound port or 0 while stopped.
/// Use this to read the assigned port when the address requested port 0.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @return The bound port, or 0 while stopped and for a NULL server, which
/// counts as API misuse.

ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

/// Copies ROCKETNET_SERVER_PUBLIC_KEY_BYTES into public_key.
/// Create generates this key for the server object.
/// Register it with your backend before issuing connect tokens.
/// The key is available while running or stopped.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and writes nothing.
/// @param public_key The output buffer of ROCKETNET_SERVER_PUBLIC_KEY_BYTES.
/// NULL counts as API misuse and writes nothing.

ROCKETNET_EXPORT void rocketnet_server_public_key( const struct rocketnet_server_t * server, uint8_t * public_key );

/// Writes the server's IPv4 address and bound port as "a.b.c.d:port".
/// Returns true if the running server serves IPv4.
/// Use a buffer of ROCKETNET_MAX_ADDRESS_STRING_BYTES.
///
/// Returns false while stopped, without IPv4, or if address_bytes is too small.
/// Failure writes an empty string if address_bytes is positive.
/// A short buffer logs an error. A zero-sized buffer is left untouched.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns false.
/// @param address The output buffer. NULL counts as API misuse and returns
/// false.
/// @param address_bytes The size of address in bytes.
/// @return True if the address was written.

ROCKETNET_EXPORT bool rocketnet_server_ipv4_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// Like rocketnet_server_ipv4_address, but writes IPv6 as "[v6]:port".
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns false.
/// @param address The output buffer. NULL counts as API misuse and returns
/// false.
/// @param address_bytes The size of address in bytes.
/// @return True if the address was written.

ROCKETNET_EXPORT bool rocketnet_server_ipv6_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// Returns the selected transport name or NULL while stopped.
/// The string remains valid for the process lifetime.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @return The transport name, or NULL while stopped and for a NULL server,
/// which counts as API misuse.

ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

/// The config's max_clients, running or stopped.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @return max_clients, or 0 for a NULL server, which counts as API misuse.

ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

/// Returns the number of connected clients. Returns 0 while stopped.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @return The connected client count, or 0 while stopped and for a NULL
/// server, which counts as API misuse.

ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

/// Returns true if the slot has a connected client.
///
/// Callable running or stopped; false while stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns false.
/// @param client_index The slot, 0 to max_clients - 1. Any other index logs
/// an error and counts as API misuse.
/// @return True if a client is connected in the slot.

ROCKETNET_EXPORT bool rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index );

/// Returns the client ID from the slot's connect token.
/// Zero is also a valid ID. Check rocketnet_server_client_connected first.
///
/// Callable running or stopped; 0 while stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns 0.
/// @param client_index The slot, 0 to max_clients - 1. Any other index logs
/// an error and counts as API misuse.
/// @return The client ID, or 0.

ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index );

/// Copies the user data from the slot's connect token into user_data.
///
/// The bytes are the ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES the backend put
/// in the token, exactly as signed. They are readable from the moment the slot
/// connects, the same moment rocketnet_server_client_id reads the token's
/// client ID, until the slot is released: a disconnect, a timeout, or the
/// server stopping. Release wipes the server's copy, so a later read of the
/// slot writes zeros, and a new client in the slot reads its own token's
/// bytes. The copy written here is the caller's to keep.
///
/// Callable running or stopped. A slot without a connected client, and any
/// slot while stopped, writes zeros and returns false; that is not misuse.
/// Thread: not thread-safe; the caller owns the server one thread at a time.
///
/// @param server The server. NULL counts as API misuse, zeroes user_data and
/// returns false.
/// @param client_index The slot, 0 to max_clients - 1. Any other index logs
/// an error, counts as API misuse, zeroes user_data and returns false.
/// @param user_data The output buffer of
/// ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES. NULL counts as API misuse and
/// writes nothing.
/// @return True if the slot holds a connected client and its user data was
/// copied.

ROCKETNET_EXPORT bool rocketnet_server_client_user_data( const struct rocketnet_server_t * server, int client_index, uint8_t * user_data );

/// Disconnects a client and frees its slot.
///
/// Needs a running server and a connected slot; otherwise it does nothing.
/// The slot reads not connected at once. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and does nothing.
/// @param client_index The slot, 0 to max_clients - 1. Any other index logs
/// an error and counts as API misuse.

ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

/// Disconnects all clients. The server keeps running.
///
/// Needs a running server; stopped it does nothing. A client still in the
/// handshake is left as it is. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and does nothing.

ROCKETNET_EXPORT void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server );

/// Returns why the slot's last connection ended.
/// The reason remains until the slot's next connection.
///
/// Callable running or stopped; ROCKETNET_DISCONNECT_REASON_NONE while
/// stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns
/// ROCKETNET_DISCONNECT_REASON_NONE.
/// @param client_index The slot, 0 to max_clients - 1. Any other index logs
/// an error and counts as API misuse.
/// @return The slot's last disconnect reason.

ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index );

/// Returns a reason name such as "NONE" or "RELIABLE_TIMED_OUT".
/// Returns "UNKNOWN" for an invalid value.
///
/// Needs no server. Thread: any thread.
///
/// @param reason The reason to name.
/// @return A static string, valid for the process lifetime.

ROCKETNET_EXPORT const char * rocketnet_disconnect_reason_name( enum rocketnet_disconnect_reason_t reason );

/// Reserves a message for the client at destination_id.
/// Use it like rocketnet_client_start_message.
///
/// Only one message can be open per server. Finish and abort need no
/// destination.
/// To broadcast, send to each connected client index.
///
/// An index at or above max_clients is refused.
/// A destination with ROCKETNET_FLOW_ID_BIT set is refused and logs
/// ROCKETNET_STATUS_FLOW_ID_RESERVED.
/// Sends to a slot without a connected client are dropped as on the client,
/// including while stopped.
///
/// Needs a running server and a connected client in the slot. A message
/// already open or a bad argument is refused with NULL. Any send (reliable,
/// or unreliable that finds no room in the send region) that finds no
/// message memory disconnects that client with
/// ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED and returns NULL.
/// Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns NULL.
/// @param destination_id The client's slot index, 0 to max_clients - 1. Any
/// other value counts as API misuse and returns NULL.
/// @param channel The channel, below the config's num_channels. The ordered
/// and sequenced modes use it; the others ignore it.
/// @param reliability The reliability mode.
/// @param max_bytes The most bytes the message may hold, from 1 to the
/// config's max_message_bytes.
/// @return Memory for max_bytes of message, or NULL when the message is
/// dropped or refused.

ROCKETNET_EXPORT uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, enum rocketnet_reliability_t reliability, int max_bytes );

/// Finishes the server's open message.
/// Uses the same size, return and abort rules as
/// rocketnet_client_finish_message.
///
/// Needs a message opened by rocketnet_server_start_message on a running
/// server; otherwise it asserts in a debug build and returns 0 in a release
/// build. Thread: not thread-safe; the caller owns the server one thread at
/// a time.
///
/// @param server The server. NULL counts as API misuse and returns 0.
/// @param bytes_written The bytes written into the open message.
/// @return The message's nonzero receipt serial, or 0 when nothing was
/// queued.

ROCKETNET_EXPORT uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, int bytes_written );

/// Cancels the open message and releases its memory.
/// Does nothing if no message is open.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and does nothing.

ROCKETNET_EXPORT void rocketnet_server_abort_message( struct rocketnet_server_t * server );

/// Returns true if rocketnet_server_start_message to client_index with the
/// same channel, reliability and max_bytes would be given room now.
/// Use it like rocketnet_client_can_send_message. Each client has its own
/// message memory.
///
/// Callable running or stopped; false while stopped and for a slot without a
/// connected client. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns false.
/// @param client_index The slot, 0 to max_clients - 1. Any other index counts
/// as API misuse and returns false.
/// @param channel The channel the start would use.
/// @param reliability The reliability mode the start would use.
/// @param max_bytes The max_bytes the start would reserve.
/// @return True if the start would be given room now.

ROCKETNET_EXPORT bool rocketnet_server_can_send_message( const struct rocketnet_server_t * server, int client_index, int channel, enum rocketnet_reliability_t reliability, int max_bytes );

/// Returns the next batch of messages from all clients, or NULL when the
/// queue is empty. Each message names its sender in client_index. Call until
/// NULL before the next update (see rocketnet_server_update). Returns NULL
/// while stopped.
///
/// Callable running or stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns NULL.
/// @return The server's batch, valid until the next receive or update, or
/// NULL when no message is waiting.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_server_receive_messages( struct rocketnet_server_t * server );

/// Copies receipts for the client in this slot.
/// Uses the same rules as rocketnet_client_receipts. For a slot whose
/// connection has ended, the outcomes recorded for it remain readable
/// until that slot's next update; a receipt-bearing send still in flight
/// at the end gets no terminal outcome.
///
/// Callable running or stopped; 0 while stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and returns 0.
/// @param client_index The slot, 0 to max_clients - 1. Any other index logs
/// an error and counts as API misuse.
/// @param out The array to fill, or NULL with zero capacity.
/// @param capacity The entries out holds.
/// @return The number of receipts available for the slot, which may exceed
/// capacity.

ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity );

/// As rocketnet_client_set_send_kbps, for this client, with the values the
/// config's per_client_send_kbps takes.
///
/// Takes effect for a connected client of a running server. Stopped, or for
/// a slot without a connected client, it returns ROCKETNET_STATUS_OK and
/// changes nothing; each new connection starts at the config's rate.
/// Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server.
/// @param client_index The slot, 0 to max_clients - 1.
/// @param kbps The budget in kbps, 0 for the default rate, or
/// ROCKETNET_SEND_KBPS_UNLIMITED for no budget.
/// @return ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a
/// NULL server, a negative kbps, an index outside 0 to max_clients - 1 or a
/// rate the config's per_client_send_kbps refuses.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps );

/// Fills stats with the server's current stats.
///
/// Callable running or stopped. Counts cover the server's whole life, across
/// starts. Rates are zero while stopped. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and fills only
/// api_misuses and thread_priority_refusals.
/// @param stats The stats to fill. NULL counts as API misuse and does
/// nothing.

ROCKETNET_EXPORT void rocketnet_server_stats( const struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats );

/// Copies the current stats for this client slot.
///
/// Callable running or stopped. The stats are zero while stopped and for a
/// slot without a connected client. Thread: not thread-safe; the caller owns the
/// server one thread at a time.
///
/// @param server The server. NULL counts as API misuse and zeroes stats.
/// @param client_index The slot, 0 to max_clients - 1. Any other index logs
/// an error and counts as API misuse.
/// @param stats The stats to fill. NULL counts as API misuse and does
/// nothing.

ROCKETNET_EXPORT void rocketnet_server_client_stats( const struct rocketnet_server_t * server, int client_index, struct rocketnet_server_client_stats_t * stats );

/// Receiving messages

/// The most messages one receive call fills.

#define ROCKETNET_RECEIVE_BATCH_MESSAGES                   65536

/// A batch of received messages. Entry i in each array describes message i.
///
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
    /// flag, and an unreliable message that became reliable arrives as its
    /// reliable type.

    enum rocketnet_reliability_t reliability[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The message's length in bytes.

    int bytes[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The message's bytes, in the library's memory.

    const uint8_t * data[ROCKETNET_RECEIVE_BATCH_MESSAGES];
};

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // ROCKETNET_H
