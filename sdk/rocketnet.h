/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

/// The public C API: a client and a server that exchange messages over UDP.
/// This header and a prebuilt RocketNet library are all a program needs: it
/// includes only standard C headers and declares only what a caller calls or
/// reads. Plain C99, callable from C++ through the extern "C" block below.
///
/// The Release-under-load target for the main frame calls is under 0.1 ms
/// (100 microseconds) per call. Create calls may take longer. The client's
/// disconnect and destroy stop its I/O thread and return once that thread has
/// exited; no time bound is promised for them.
///
/// The API's shape: a call that can fail returns enum rocketnet_status_t,
/// except a create, which returns a pointer, NULL when refused; finish
/// message, which returns a uint32_t receipt serial, 0 when refused; start
/// message, rocketnet_client_transport_name and
/// rocketnet_server_transport_name, which return a pointer; and the
/// required_memory calls, which return the byte count, 0 when refused. The
/// stats calls fill a caller struct; update returns void; a pure question
/// returns bool; a call that reports nothing returns void; every set of values
/// is a typed enum named ROCKETNET_<SET>_<VALUE>, and each lookup that names
/// an enum value ends in _name and returns "unknown" for a value outside its
/// enum, never NULL.
///
/// A config struct's first field, config_bytes, is the size of the struct the
/// caller compiled against; the inline default_config calls set it, so a
/// config struct can grow at its end without breaking a program built against
/// an older header.
///
/// Sizes: a buffer size is a size_t (the required_memory results,
/// config_bytes, address_bytes); config fields, message bytes, counts and
/// client indexes are int; a send's destination id is a uint64_t, a client
/// index with ROCKETNET_FLOW_ID_BIT clear.
/// Pure lookups and time calls are thread-safe; client and server instances are not.

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// MSVC /Wall reports C4514 (an unreferenced inline function was removed) at
// the end of every translation unit that does not call each inline below; it
// is informational, and a public header cannot scope it, so it is off.
#if defined( _MSC_VER )
#pragma warning( disable : 4514 )
#endif

/// -----------------------------------------
///                            Version
/// -----------------------------------------

/// ROCKETNET_DEVELOPMENT is 1 when linking a development library
/// (rocketnet_dev): it carries rocketnet_create_dev_connect_token and
/// ROCKETNET_DEV_BACKEND_PUBLIC_KEY below and accepts the published development
/// backend key.

#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 0
#endif /// #if !defined( ROCKETNET_DEVELOPMENT )

#if !ROCKETNET_DEVELOPMENT

    /// @brief Text form of the SDK version: the release version string, or "dev" in a development build.

    #define ROCKETNET_VERSION_FULL                     "1.0.0-preview-1"
    /// @brief Major component of the SDK version; development builds use 255.

    #define ROCKETNET_VERSION_MAJOR                                    1
    /// @brief Minor component of the SDK version; development builds use 255.

    #define ROCKETNET_VERSION_MINOR                                    0
    /// @brief Patch component of the SDK version; development builds use 255.

    #define ROCKETNET_VERSION_PATCH                                    0

#else /// !ROCKETNET_DEVELOPMENT

    #define ROCKETNET_VERSION_FULL                                 "dev"
    #define ROCKETNET_VERSION_MAJOR                                  255
    #define ROCKETNET_VERSION_MINOR                                  255
    #define ROCKETNET_VERSION_PATCH                                  255

#endif /// !ROCKETNET_DEVELOPMENT

/// The version as one comparable integer: major * 10000 + minor * 100 + patch,
/// 10000 for 1.0.0. The prerelease is not in the integer: v1.0.0-preview-1 is
/// 10000 too, and ROCKETNET_VERSION_FULL tells it apart. A development
/// build is 2575755, above every release.
/// rocketnet_version returns the value the library was built with.

#define ROCKETNET_VERSION   ( ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH )

/// -----------------------------------------
///                       Export macro
/// -----------------------------------------

/// Define ROCKETNET_SHARED when linking the shared library (the DLL). A static
/// link defines nothing. ROCKETNET_BUILDING_DLL is the library's own build
/// switch, never a caller's.

#if defined( _WIN32 ) && defined( ROCKETNET_BUILDING_DLL )
    #define ROCKETNET_EXPORT __declspec(dllexport)
#elif defined( _WIN32 ) && defined( ROCKETNET_SHARED )
    #define ROCKETNET_EXPORT __declspec(dllimport)
#elif ( defined( ROCKETNET_SHARED ) || defined( ROCKETNET_BUILDING_DLL ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
    #define ROCKETNET_EXPORT __attribute__((visibility("default")))
#else
    #define ROCKETNET_EXPORT
#endif

/// -----------------------------------------
///                          Constants
/// -----------------------------------------

#define ROCKETNET_MAX_CLIENTS                                       1000

/// A send's destination id is a uint64_t client index, 0 .. max_clients - 1.
/// ROCKETNET_FLOW_ID_BIT is reserved: rocketnet_server_start_message refuses a
/// destination id with it set, as counted API misuse.

#define ROCKETNET_FLOW_ID_BIT                                       ( 1ULL << 63 )

/// The largest packet the library sends: the UDP payload of one datagram on a
/// 1500-byte MTU link over IPv6, the smaller of the two families, so a
/// maximum packet crosses a 1500-MTU path unfragmented on either.

#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 40 - 8 )

/// The largest payload one packet carries: ROCKETNET_MAX_PACKET_BYTES less the
/// packet's own overhead, rounded down to a multiple of eight, so any
/// word-based serializer can write at the payload pointer directly.

/// ( ( 1452 - 26 - 7 ) / 8 ) * 8

#define ROCKETNET_MAX_PACKET_PAYLOAD_BYTES                 1416

/// Unreliable messages larger than this are split across packets and sent reliably.

#define ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES             1411

/// Payloads above ROCKETNET_MAX_PACKET_PAYLOAD_BYTES fragment transparently, and a
/// lost fragment loses the whole payload; the config's max_payload_bytes bounds
/// both what this end fragments and what it reassembles, up to 305,408 bytes.

/// @brief Buffer size in bytes for a connect token string, including the NUL terminator.

/// 1024 base64 characters plus the NUL

#define ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES           1025

/// @brief Application-defined connect-token user data size in bytes.

#define ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES            256
/// @brief Backend signing public key size in bytes.

#define ROCKETNET_BACKEND_PUBLIC_KEY_BYTES                  32
/// @brief Server sealing public key size in bytes: what rocketnet_server_public_key copies out.

#define ROCKETNET_SERVER_PUBLIC_KEY_BYTES                   32
/// @brief The bound in bytes, NUL included, of every address string the API takes or writes ("a.b.c.d:port" or "[v6]:port"); a longer input is refused.

#define ROCKETNET_MAX_ADDRESS_STRING_BYTES                 256

/// @brief The alignment in bytes the SDK asks of a block, and that a custom allocator (rocketnet_allocator) must honour.

#define ROCKETNET_ALLOCATION_ALIGNMENT                     64

/// The channel bound: define it before including rocketnet.h, and build the library with the same value.

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                             32
#endif

/// @brief The default client to server send budget, in kbps, applied when the client config's send_kbps is 0.

#define ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS            1000

/// @brief The default server to client send budget, per client, in kbps, applied when the server config's per_client_send_kbps is 0.

#define ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS            10000

/// send_kbps value: no send budget on this connection.

#define ROCKETNET_SEND_KBPS_UNLIMITED                      0x7fffffff

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable status values returned by a call that can fail, except a create
/// (a pointer, NULL when refused), finish message (a uint32_t receipt serial, 0
/// when refused), start message and the transport_name calls (pointers), and
/// the required_memory calls (the byte count, 0 when refused).
///
/// OK is the sole success value, every failure is a positive code, and the
/// numbering is append-only. A published value and meaning never change; a new
/// code appends immediately before ROCKETNET_STATUS_COUNT.

enum rocketnet_status_t
{
    /// Success: the operation succeeds

    ROCKETNET_STATUS_OK                                 = 0,

    /// Lifecycle and allocator state (rocketnet_init, rocketnet_term, rocketnet_allocator)

    /// Initialization is complete or transitioning: rocketnet_init, or an allocator change, after the library was initialized

    ROCKETNET_STATUS_ALREADY_INITIALIZED                = 1,

    /// The operation needs an initialized library: a call that needs rocketnet_init came before it, or a term with nothing to terminate

    ROCKETNET_STATUS_NOT_INITIALIZED                    = 2,

    /// Term or an allocator change refused: the library still holds allocations

    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = 3,

    /// The platform's socket layer did not start; correct the platform setup before retrying

    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = 4,

    /// The crypto library did not start; initialization does not complete

    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = 5,

    /// Caller arguments and configuration (client, server, message)

    /// An argument, or a combination of arguments, is NULL, out of range, or inconsistent with another

    ROCKETNET_STATUS_INVALID_ARGUMENT                   = 6,

    /// An environment override is malformed: it did not parse

    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = 7,

    /// A config field, from the struct or the environment, is out of its supported range

    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = 8,

    /// The memory the config needs is past the operator's memory budget

    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = 9,

    /// Start refused: the server config's backend_public_key is all zeros

    ROCKETNET_STATUS_MISSING_KEYS                       = 10,

    /// The protocol id is 0, a development id in a release build, or not the expected value

    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = 11,

    /// Start refused: a release build was given the published development backend key

    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = 12,

    /// An address string did not parse or cannot be used

    ROCKETNET_STATUS_INVALID_ADDRESS                    = 13,

    /// The transport candidate list did not parse, or holds no usable candidate

    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = 14,

    /// Client connect token pre-send checks

    /// The connect token fails the client's pre-send checks: it did not parse, or is not for this protocol

    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = 15,

    /// The connect token expired before the first packet was sent; obtain a fresh token before connecting

    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = 16,

    /// Transport candidate list and OS resources

    /// A pinned transport cannot take port 0 (an ephemeral port)

    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = 17,

    /// Every transport in the candidate list refused to start

    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = 18,

    /// An allocation the operation needs failed

    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,

    /// The platform refused a mutex

    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,

    /// The platform refused a thread

    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,

    /// The platform refused a timer

    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,

    /// Object lifecycle (rocketnet_client_connect, rocketnet_server_start) and destinations

    /// Connect refused: the client is connecting or connected, or in a handshake or token failure state (CONNECTION_DENIED .. CONNECT_TOKEN_EXPIRED), which needs rocketnet_client_disconnect first; nothing changed

    ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED            = 23,

    /// Start refused: the server is already running; nothing changed

    ROCKETNET_STATUS_SERVER_NOT_STOPPED                 = 24,

    /// A destination id has the reserved ROCKETNET_FLOW_ID_BIT set; the send is refused

    ROCKETNET_STATUS_FLOW_ID_RESERVED                   = 25,

    /// A transport cannot serve both named families (two interfaces, or a platform that cannot clear IPV6_V6ONLY)

    ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED     = 26,

    /// A config's config_bytes is 0 or too small: fill the config with its default_config call first

    ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED             = 27,

    /// Sentinel: the count of codes, OK included; not an operation result

    ROCKETNET_STATUS_COUNT                              = 28
};

/// -----------------------------------------
///                          Log levels
/// -----------------------------------------

/// @brief The process-wide log levels, least to most verbose.

enum rocketnet_log_level_t
{
    /// As the maximum, only level-NONE lines; quiet mode suppresses every line

    ROCKETNET_LOG_LEVEL_NONE                            = 0,

    /// Errors, the least verbose emitted level

    ROCKETNET_LOG_LEVEL_ERROR                           = 1,

    /// Warnings

    ROCKETNET_LOG_LEVEL_WARN                            = 2,

    /// Information

    ROCKETNET_LOG_LEVEL_INFO                            = 3,

    /// Debugging

    ROCKETNET_LOG_LEVEL_DEBUG                           = 4,

    /// The most verbose level

    ROCKETNET_LOG_LEVEL_SPAM                            = 5
};

/// -----------------------------------------
///                  Address types
/// -----------------------------------------

/// @brief Address family types for endpoints.

enum rocketnet_address_type_t
{
    /// No address family

    ROCKETNET_ADDRESS_NONE                              = 0,

    /// IPv4 address family

    ROCKETNET_ADDRESS_IPV4                              = 1,

    /// IPv6 address family

    ROCKETNET_ADDRESS_IPV6                              = 2
};

/// -----------------------------------------
///                    Message enums
/// -----------------------------------------

/// @brief Per-send reliability: how a message is delivered, ordered and acknowledged.

enum rocketnet_reliability_t
{
    /// Send once without delivery, ordering, sequencing, or receipt. The
    /// receiver can see the message at most once. A lost packet loses it.

    ROCKETNET_RELIABILITY_UNRELIABLE                            = 0,

    /// Send without delivery guarantees and keep only newer channel state.
    /// The receiver delivers a state only when its ordering and sequencing
    /// values are newer; older or out-of-turn states can be discarded.

    ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED                  = 1,

    /// Resend until the peer's transport acknowledges the message. Delivery
    /// is once, but messages on the same channel have no order guarantee. This
    /// mode does not produce a receipt.

    ROCKETNET_RELIABILITY_RELIABLE                              = 2,

    /// Resend until acknowledged and deliver in channel order. The receiver
    /// holds later messages until earlier messages on that channel arrive. This
    /// mode does not produce a receipt.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED                      = 3,

    /// Resend until acknowledged and deliver only the newest channel state.
    /// The receiver can discard an acknowledged stale state; delivery order is
    /// not guaranteed. This mode does not produce a receipt.

    ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED                    = 4,

    /// Send once and report whether the carrying packet is acknowledged. A
    /// larger message is sent as a reliable block and can report ACKED only. A
    /// single-packet message can report ACKED or LOSS.

    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT           = 5,

    /// Resend until acknowledged and report an ACKED receipt. Delivery is
    /// once without channel ordering. A connection failure does not produce a
    /// LOSS receipt.

    ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT             = 6,

    /// Resend until acknowledged, deliver in channel order, and report ACKED.
    /// A connection failure does not produce a LOSS receipt.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT     = 7
};

/// @brief A receipt's outcome, the status field of struct rocketnet_receipt_t.

enum rocketnet_receipt_status_t
{
    /// A zero-filled receipt: no outcome has been written.

    ROCKETNET_RECEIPT_STATUS_NONE                               = 0,

    /// The peer's transport acknowledged the packet. This does not mean the
    /// peer application read the message.

    ROCKETNET_RECEIPT_STATUS_ACKED                              = 1,

    /// An unreliable receipt packet was not acknowledged. The message is not
    /// resent; reliable receipt modes do not produce LOSS.

    ROCKETNET_RECEIPT_STATUS_LOSS                               = 2
};

/// @brief Why a server's client slot's connection ended: what
/// rocketnet_server_disconnect_reason returns. A client
/// reads how its own connection ended from rocketnet_client_state.
///
/// MESSAGE_MEMORY_EXHAUSTED: the connection's region had no room for an
/// operation it must not drop. A server disconnects that
/// client's slot with this reason and resets the slot's region. A connection
/// that ended reads a negative reason, as the client's state does.

enum rocketnet_disconnect_reason_t
{
    /// No connection has ended, or the connection is up

    ROCKETNET_DISCONNECT_REASON_NONE                            =  0,

    /// The two ends' WIRE config fields differ

    ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH                 = -1,

    /// The peer sent what the protocol forbids

    ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION              = -2,

    /// A reliable message went unacknowledged past reliable_message_timeout_ms

    ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT                = -3,

    /// The connection's region had no room for an operation it must not drop

    ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED        = -4,

    /// The server disconnected the client, stopped, or was destroyed

    ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED             = -5
};

/// -----------------------------------------
///                        Client state
/// -----------------------------------------

/// What rocketnet_client_state returns: a connect attempt runs
/// up from DISCONNECTED (0) through the three handshake states to CONNECTED,
/// the highest. A negative state is how the last attempt or connection ended,
/// one state per way it can end, kept until rocketnet_client_disconnect, which
/// returns the client to DISCONNECTED; rocketnet_client_connect from
/// SERVER_DISCONNECTED down to MESSAGE_MEMORY_EXHAUSTED (a connection that
/// ended after it was CONNECTED) ends it first; from CONNECTION_DENIED down
/// to CONNECT_TOKEN_EXPIRED, disconnect first. INVALID is no client state: it is what a NULL client reads as.

enum rocketnet_client_state_t
{
    /// A NULL client: not a state any client is in

    ROCKETNET_CLIENT_STATE_INVALID                          = -13,

    /// The connection's region had no room for an operation it must not drop

    ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED         = -12,

    /// The two ends' WIRE config fields differ

    ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH                  = -11,

    /// The server sent what the protocol forbids

    ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION               = -10,

    /// A reliable message went unacknowledged past reliable_message_timeout_ms

    ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT               = -9,

    /// The server disconnected the client, stopped, or was destroyed

    ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED              = -8,

    /// The connect token expired before the server accepted the client

    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            = -7,

    /// The connect token was refused before the first packet was sent

    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            = -6,

    /// A connection heard nothing from the server for the token's timeout

    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             = -5,

    /// The server never answered the third leg of the handshake

    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    = -4,

    /// The server never answered the connection request

    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     = -3,

    /// The server never answered the cookie request

    ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT         = -2,

    /// The server refused the client

    ROCKETNET_CLIENT_STATE_CONNECTION_DENIED                = -1,

    /// After create, and after rocketnet_client_disconnect

    ROCKETNET_CLIENT_STATE_DISCONNECTED                     =  0,

    /// Handshake, first leg

    ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE                =  1,

    /// Handshake, second leg

    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST       =  2,

    /// Handshake, third leg

    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE      =  3,

    /// Connected: messages flow both ways

    ROCKETNET_CLIENT_STATE_CONNECTED                        =  4
};

/// -----------------------------------------
///                              Stats
/// -----------------------------------------

/// A client's stats, filled by rocketnet_client_stats with the current
/// stats. The counts are monotonic over the client's life, across
/// connections, and start at 0 at create, except api_misuse and
/// thread_priority_refusals, which are process-wide and do not reset. The measured values cover the
/// current connection, rates over the trailing second, and read 0 with no
/// connection.
///
/// The packet_loss value counts gaps in the server's session sequence. The
/// server assigns a sequence only after finish accepts the payload and reserves
/// every wire frame, so an aborted start or refused start or finish creates no
/// gap; a gap after a completed send reflects path loss.

struct rocketnet_client_stats_t
{
    /// Packets: UDP datagrams sent, handshake included

    uint64_t packets_sent;

    /// Packets: UDP datagrams received, handshake included

    uint64_t packets_received;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family

    uint64_t bytes_sent;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family

    uint64_t bytes_received;

    /// Payloads: packets from the server that opened and carried messages

    uint64_t payloads_received;

    /// Records: reliable message records resent on the retransmit timer

    uint64_t messages_resent;

    /// Messages: reliable messages received a second time and dropped

    uint64_t messages_duplicate;

    /// Receipts: sends with a receipt that the server acknowledged

    uint64_t receipts_acked;

    /// Receipts: sends with a receipt that were lost

    uint64_t receipts_lost;

    /// Receipts: outcomes dropped, oldest first, because the receipt queue was full

    uint64_t receipts_dropped;

    /// Messages: unreliable messages dropped because the send budget was spent

    uint64_t budget_drops;

    /// Hand-offs: sends that left with the next hand-off to the send thread

    uint64_t send_deferred;

    /// Messages: sends dropped because the connection was not CONNECTED

    uint64_t not_connected_drops;

    /// Process-wide: API misuses since the process started (the misuse list under Lifecycle and logging)

    uint64_t api_misuse;

    /// Process-wide: priority, affinity and timer period calls the operating system refused since the process started

    uint64_t thread_priority_refusals;

    /// Milliseconds: round trip time

    double rtt;

    /// Milliseconds: inter-arrival jitter

    double jitter;

    /// Percent: received-side loss, from gaps in the server's packet sequence

    double packet_loss;

    /// Packets per second sent

    double packets_sent_per_second;

    /// Packets per second received

    double packets_received_per_second;

    /// Kbps (1000 bit/s) sent

    double bandwidth_sent_kbps;

    /// Kbps (1000 bit/s) received

    double bandwidth_received_kbps;
};

/// A server's stats over all its clients, filled by rocketnet_server_stats
/// with the current stats. The counts are monotonic over the server's life,
/// across starts and stops, and start at 0 at create, except api_misuse and
/// thread_priority_refusals, which are process-wide and do not reset. The rates are the whole
/// socket's over the trailing second, and read 0 while stopped.

struct rocketnet_server_stats_t
{
    /// Packets: UDP datagrams sent

    uint64_t packets_sent;

    /// Packets: UDP datagrams received

    uint64_t packets_received;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family

    uint64_t bytes_sent;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family

    uint64_t bytes_received;

    /// Connections: clients that completed the handshake

    uint64_t clients_connected;

    /// Requests: connect attempts refused because every client slot was taken

    uint64_t clients_denied_full;

    /// Requests: connect attempts dropped by the per-update handshake budget

    uint64_t handshakes_over_budget;

    /// Payloads: packets from clients that opened and carried messages

    uint64_t payloads_received;

    /// Records: reliable message records resent on the retransmit timer

    uint64_t messages_resent;

    /// Messages: reliable messages received a second time and dropped

    uint64_t messages_duplicate;

    /// Receipts: sends with a receipt that a client acknowledged

    uint64_t receipts_acked;

    /// Receipts: sends with a receipt that were lost

    uint64_t receipts_lost;

    /// Receipts: outcomes dropped, oldest first, because a client's receipt queue was full

    uint64_t receipts_dropped;

    /// Messages: unreliable messages dropped because a client's send budget was spent

    uint64_t budget_drops;

    /// Messages: sends dropped because the connection was not CONNECTED

    uint64_t not_connected_drops;

    /// Process-wide: API misuses since the process started (the misuse list under Lifecycle and logging)

    uint64_t api_misuse;

    /// Process-wide: priority, affinity and timer period calls the operating system refused since the process started

    uint64_t thread_priority_refusals;

    /// Packets per second sent

    double packets_sent_per_second;

    /// Packets per second received

    double packets_received_per_second;

    /// Kbps (1000 bit/s) sent

    double bandwidth_sent_kbps;

    /// Kbps (1000 bit/s) received

    double bandwidth_received_kbps;
};

/// One client slot's stats, filled by rocketnet_server_client_stats with
/// the current stats. The values cover the slot's current connection and go
/// back to 0 when a new connection takes the slot; rates cover the trailing
/// second.

struct rocketnet_server_client_stats_t
{
    /// Packets: UDP datagrams sent to this client

    uint64_t packets_sent;

    /// Packets: UDP datagrams received from this client

    uint64_t packets_received;

    /// Bytes: UDP payload plus the IP and UDP headers of this client's family

    uint64_t bytes_sent;

    /// Bytes: UDP payload plus the IP and UDP headers of this client's family

    uint64_t bytes_received;

    /// Milliseconds: round trip time

    double rtt;

    /// Milliseconds: deviation of rtt samples above the minimum

    double jitter;

    /// Percent: packets sent to this client that were never acknowledged

    double packet_loss;

    /// Kbps (1000 bit/s) sent to this client

    double bandwidth_sent_kbps;

    /// Kbps (1000 bit/s) received from this client

    double bandwidth_received_kbps;

    /// Kbps (1000 bit/s) of sent packets this client acknowledged

    double bandwidth_acked_kbps;
};

// The library and its callers are built without -fshort-enums: every public
// enum is the size of int, checked at compile time where the language has a
// static assert.
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

/// -----------------------------------------
///                     Opaque handles
/// -----------------------------------------

struct rocketnet_client_t;
struct rocketnet_server_t;

/// -----------------------------------------
///                Lifecycle and logging
/// -----------------------------------------

/// The library's version as one integer, the value ROCKETNET_VERSION had when
/// the library was built. Compare it with ROCKETNET_VERSION to catch a header
/// and a shared library from different releases. Callable before init.

ROCKETNET_EXPORT uint32_t rocketnet_version( void );

/// @brief Initializes the library and its platform and cryptography state.
/// @param context Opaque allocator context passed to allocation callbacks; NULL is the default.
/// @return ROCKETNET_STATUS_OK; ROCKETNET_STATUS_ALREADY_INITIALIZED when the
/// library is initialized or transitioning; ROCKETNET_STATUS_PLATFORM_INIT_FAILED
/// or ROCKETNET_STATUS_CRYPTO_INIT_FAILED when that layer does not start.
/// Call before creating a client or a server; do not race an init or term transition.
/// Environment: ROCKETNET_LOG_LEVEL overrides the maximum emitted log level before init logs; 0 .. 5 (ROCKETNET_LOG_LEVEL_NONE .. ROCKETNET_LOG_LEVEL_SPAM); a value outside that range is ignored with a warning.
/// @code
/// if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK ) {
///     // handle failure
/// }
/// @endcode

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_init( void * context );

/// @brief Terminates the initialized library after every client and server is destroyed.
///
/// A refused termination logs one error line. Do not call concurrently with SDK work that
/// allocates or depends on platform state.
/// @return ROCKETNET_STATUS_OK; ROCKETNET_STATUS_NOT_INITIALIZED with nothing to
/// terminate, or during a transition; ROCKETNET_STATUS_LIVE_ALLOCATIONS while a
/// client, a server or another allocation is still held.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_term( void );

/// @brief Returns the stable symbolic name of a status code. @param status Status to look up. @return Static name without the ROCKETNET_STATUS_ prefix ("OK", "INVALID_ARGUMENT"), or "unknown" for a value that is not a code, never NULL. Allocation-free and safe before init; returned storage is process-lifetime.

ROCKETNET_EXPORT const char * rocketnet_status_name( enum rocketnet_status_t status );

/// @brief Receives a failed internal assert: the borrowed condition, function
/// and file strings, and the source line that failed. It runs synchronously on
/// the thread that fires the assertion.

typedef void (*rocketnet_assert_function_t)( const char * condition, const char * function, const char * file, int line );

/// @brief Installs the process-wide function for a failed internal assert.
///
/// This setter is the only way to install it; no writable global is public.
/// The library calls the function when it is built with asserts enabled (a
/// debug build); with asserts disabled it is never invoked. The default
/// function logs and traps. Pass NULL to restore the default. Install it
/// before concurrent SDK calls, since the library reads it unsynchronized.
///
/// @param function Function to install, or NULL to restore the default.

ROCKETNET_EXPORT void rocketnet_assert_function( rocketnet_assert_function_t function );

/// @brief Suppresses or resumes all RocketNet log output.
///
/// Quiet state is process-wide and atomically read by logging calls. It does not
/// change the configured log level.
/// @param flag true suppresses output; false resumes output subject to the log level.

ROCKETNET_EXPORT void rocketnet_quiet( bool flag );

/// @brief Sets the process-wide maximum log level.
/// @param level ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM; a value outside the enum is ignored.

ROCKETNET_EXPORT void rocketnet_log_level( enum rocketnet_log_level_t level );

/// @brief Turns the library's network-thread priority calls on or off.
///
/// On (the default), the library sets the priority of the network threads it
/// creates, right after it creates each one: SCHED_RR at
/// sched_get_priority_min( SCHED_RR ) on Linux and macOS, and
/// THREAD_PRIORITY_TIME_CRITICAL on Windows, where it also requests a 1 ms
/// timer period (timeBeginPeriod( 1 )) from rocketnet_init to rocketnet_term.
/// Off, it makes none of these calls. A game that sets its own thread
/// priorities turns it off.
///
/// The setting is process-wide. Set it before rocketnet_init to skip the timer
/// period. Threads created after a change follow the new value; a thread
/// already running keeps what it got.
/// A refusal by the operating system never fails a create or rocketnet_init:
/// the thread runs at its default priority, the first refusal of each call
/// kind logs one warning, and the stats structs'
/// thread_priority_refusals counts them.
/// @param enabled true (the default) makes the calls; false makes none.

ROCKETNET_EXPORT void rocketnet_thread_priority( bool enabled );

/// @brief Returns the display name for a log level.
/// A log function uses it to render a level the way the default sink does. The
/// returned string is static and remains valid for the process lifetime.
/// @param level A log level.
/// @return "none", "error", "warn", "info", "debug" or "spam"; "unknown" for a value outside the enum, never NULL.

ROCKETNET_EXPORT const char * rocketnet_log_level_name( enum rocketnet_log_level_t level );

/// @brief Receives one preformatted log message: the one log sink.
///
/// The function runs synchronously on the logging thread. The message is
/// NUL-terminated (message[length] is always NUL), length is clamped to at most
/// 1023 bytes, and the pointer borrows the logging frame's stack buffer, valid
/// only until the function returns. The function must be thread-safe and must
/// not re-enter rocketnet_log_function.
/// @param context Caller-owned pointer supplied when the function is installed.
/// @param level The level passed by the logging call.
/// @param message Borrowed formatted bytes; valid only during this call.
/// @param length Number of message bytes before the NUL terminator, at most 1023.

typedef void (*rocketnet_log_function_t)( void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

/// @brief Installs the log function, the one log sink.
///
/// Passing NULL restores the default sink, which prints to stdout, and clears
/// its context. Calls already in flight may finish on their previously captured
/// function; log functions must be thread-safe.
/// @param function Function to install, or NULL to restore the default sink.
/// @param context Caller-owned pointer passed back to function; keep it valid until any in-flight call using it returns.

ROCKETNET_EXPORT void rocketnet_log_function( rocketnet_log_function_t function, void * context );

/// API misuse: the stats structs' api_misuse field counts, process-wide since
/// the process started, exactly these: a NULL argument a call refuses (a NULL
/// client's state included, a NULL to either destroy excepted); a create
/// before rocketnet_init; a refused rocketnet_allocator; an update time that
/// is not finite or goes backwards; an unreliable start message of at most
/// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES still open at update or send_packets; a connect from a state other than DISCONNECTED or a
/// connection-ended state; a server start while running, and a server update
/// or send_packets while stopped; a client_index or destination id outside
/// 0 .. max_clients - 1, the reserved ROCKETNET_FLOW_ID_BIT included. A
/// send refused for its own arguments (a second start message while one is
/// pending, a finish message whose bytes_written is out of range, a channel or
/// reliability out of range) is not counted: it returns NULL or 0 and logs. A
/// send on a connection that is not CONNECTED (a client in any other state, a
/// server index below max_clients with no CONNECTED client, a stopped server)
/// is not counted either: it is a normal drop, counted in the
/// not_connected_drops stat.

/// @brief Returns monotonic elapsed time in seconds from an unspecified process-local origin. @return Nondecreasing platform clock value; use the same clock for all time-taking calls.

ROCKETNET_EXPORT double rocketnet_time( void );

/// @brief Suspends the calling thread for a requested duration. @param seconds Duration in seconds; a negative, zero or NaN value returns at once, and a value above 2000 seconds sleeps 2000 seconds. This blocks the calling thread.

ROCKETNET_EXPORT void rocketnet_sleep( double seconds );

/// -----------------------------------------
///                          Allocator
/// -----------------------------------------

/// @brief Replaces the allocator before initialization.
///
/// Configure before init, with no outstanding allocations. Invalid or late
/// replacements are refused: the call returns the refusal's status, logs one
/// line and counts the refusal as API misuse. Callbacks must be
/// thread-safe and return storage aligned for fundamental types. RocketNet adds
/// alignment/ownership metadata, so callbacks receive larger sizes and their
/// original pointer on free.
///
/// @param malloc_function Allocator callback; receives context and requested bytes plus SDK overhead.
/// @param free_function Matching deallocator; receives the original allocation pointer.
/// @return ROCKETNET_STATUS_OK when both callbacks are installed;
/// ROCKETNET_STATUS_INVALID_ARGUMENT for a partial pair (either callback NULL);
/// ROCKETNET_STATUS_ALREADY_INITIALIZED after init or during a transition;
/// ROCKETNET_STATUS_LIVE_ALLOCATIONS while allocations are outstanding.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

/// -----------------------------------------
///                             Config
/// -----------------------------------------

/// The client and the server each take one plain config struct. Fill it with
/// rocketnet_client_default_config or rocketnet_server_default_config, set
/// what the program means, and pass it to create. Precedence at create is
/// DEFAULT < CONFIG STRUCT < ENVIRONMENT: every field except a callback has an
/// environment variable, named next to the field below, so an operator can
/// tune an already-built binary with no rebuild. Every override is logged at
/// create with the variable and the value it displaced. A value that does not parse, or is out of range, refuses the
/// create with one error line; nothing is ever clamped. An environment
/// variable set to the empty string is ignored, with a log line.
///
/// Env encodings: integers are decimal; byte fields are base64 of exactly the
/// field's length; strings are taken as they are.
///
/// Transport is an ORDERED CANDIDATE LIST, comma separated, tried left to right
/// at client connect and server start; the first transport that starts wins, and a list that every
/// candidate refuses fails loudly. "auto" (the default, also NULL or the empty
/// string) is this platform's recommended order: a linux server tries
/// xdp,gso,io_uring,portable (io_uring only in a build configured with
/// liburing), a macOS server msgx,portable, and a Windows server
/// portable,iocp,rio; a client tries msgx,portable on macOS and portable
/// everywhere else. The loopback transport is never in auto; it is chosen by name. A one-element
/// list is a pin: that transport or loud failure. A known name that this
/// platform or this endpoint does not have (xdp for a client) is skipped with
/// one log line, so one config line deploys to mixed machines; a name that is
/// no transport anywhere is a typo and fails loudly, listing the valid names.
/// rocketnet_client_transport_name and rocketnet_server_transport_name return
/// the transport that won.
///
/// The message fields marked WIRE must be identical on both ends; the
/// connection verifies them. Create checks each field's range below and that
/// the message fields agree with each other (max_message_bytes at most
/// receive_message_buffer_bytes, and the like), and logs the rule a refusal broke.
///
/// Create copies every string it is given, so the strings need to live only
/// for the call.
///
/// Config sizes: the first field of each config,
/// config_bytes, is the size of the struct the caller compiled against. The
/// inline rocketnet_client_default_config and rocketnet_server_default_config
/// below set it to sizeof(*config) and then fill every field, so the idiom is
/// two lines:
///
///     struct rocketnet_client_config_t config;
///     rocketnet_client_default_config( &config );
///
/// Every other call that takes a config reads config_bytes first: 0 or too
/// small to hold the field itself (a zeroed config that skipped default_config)
/// is refused with ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED and a log line. A
/// larger size, from a newer header, is read as far as the library knows and
/// the rest is ignored; a smaller size, from an older header, is read as far
/// as it goes and the rest takes the defaults. The library never reads or
/// writes past config_bytes. A NULL config, where a call accepts one, means the
/// defaults.
///
/// A few more environment variables tune what the structs do not expose; the
/// SDK's documentation lists them.

struct rocketnet_client_config_t
{
    /// No env form: sizeof the struct the caller compiled against, set by rocketnet_client_default_config; 0 is refused

    size_t config_bytes;

    /// ROCKETNET_CLIENT_TRANSPORT: the candidate list; NULL is "auto"

    const char * transport;

    /// No env form; NULL is none. Runs on the caller's thread, inside update, disconnect and destroy, once per change of the connection's handshake state; must not re-enter the client.

    void (*state_change_callback)( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state );

    /// No env form: the receive stamp clock; NULL is the platform clock. Runs on the I/O thread, and on the caller's thread during disconnect and destroy.

    double (*time_callback)( void * context );

    /// ROCKETNET_CLIENT_PACKET_FILTER: the per-title packet filter, as the server's

    uint8_t packet_filter[4];

    /// ROCKETNET_CLIENT_STEP_DRIVEN: nonzero (env 1) runs the client I/O step inside update and send_packets on the caller's thread, with no I/O thread; 0 (the default) keeps the I/O thread

    int step_driven;

    /// ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PACKET_PAYLOAD_BYTES .. 305,408; 0 is the default, 16384

    int max_payload_bytes;

    /// ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES: 1 .. 256; 0 is the default, 8

    int max_fragmented_messages;

    /// ROCKETNET_CLIENT_SEND_KBPS: the send budget, kbps; the starting rate of each connection; 0 is the default, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS (1000); ROCKETNET_SEND_KBPS_UNLIMITED is no send budget; 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1 is that budget; negative is refused with ROCKETNET_STATUS_INVALID_ARGUMENT

    int send_kbps;

    /// ROCKETNET_CLIENT_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32

    int num_channels;

    /// ROCKETNET_CLIENT_RELIABLE_MESSAGES_IN_FLIGHT: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024

    int reliable_messages_in_flight;

    /// ROCKETNET_CLIENT_RECEIVE_MESSAGE_BUFFER_BYTES: the receive buffer for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144

    int receive_message_buffer_bytes;

    /// ROCKETNET_CLIENT_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_message_buffer_bytes; 1415 .. 1 << 20; default 262144

    int max_message_bytes;

    /// ROCKETNET_CLIENT_SEND_MESSAGE_BUFFER_BYTES: queued message bytes; 12288 .. 1 << 28; default 800000

    int send_message_buffer_bytes;

    /// ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES: queued messages; 16 .. 1 << 20; default 1024

    int send_queue_messages;

    /// ROCKETNET_CLIENT_PACKETS_IN_FLIGHT: unacknowledged packets tracked; 16 .. 1 << 16; default 256

    int packets_in_flight;

    /// ROCKETNET_CLIENT_PACKETS_PER_UPDATE: packets sent per update; 1 .. 1024; default 256; the send budget is the throttle

    int packets_per_update;

    /// ROCKETNET_CLIENT_UNRELIABLE_MESSAGES_PER_UPDATE: unreliable messages delivered per update; per update; 1 .. 1 << 16; default 1024

    int unreliable_messages_per_update;

    /// ROCKETNET_CLIENT_SEQUENCED_MESSAGES_HELD: sequenced messages held for order; 0 .. 4096; default 16

    int sequenced_messages_held;

    /// ROCKETNET_CLIENT_MAX_MESSAGE_RECEIPTS: receipt outcomes held for rocketnet_client_receipts; a full queue drops its oldest, counted in receipts_dropped; 16 .. 1 << 16; default 256

    int max_message_receipts;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS: the initial resend delay before an rtt sample, milliseconds; 10 .. 60000; default 2000

    int message_resend_delay_initial_ms;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS: optional sampled resend delay floor in milliseconds; 0 disables it; 0 .. 60000; default 0

    int message_resend_delay_min_ms;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS: the maximum resend delay, milliseconds; 10 .. 60000; default 2000

    int message_resend_delay_max_ms;

    /// ROCKETNET_CLIENT_RELIABLE_MESSAGE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000

    int reliable_message_timeout_ms;

    /// Simulated link conditions: development only. A release library refuses nonzero simulated_* fields with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE.

    /// ROCKETNET_CLIENT_SIMULATED_LOSS_PERCENT: development only: outgoing loss 0..100; 0 is off

    int simulated_loss_percent;

    /// ROCKETNET_CLIENT_SIMULATED_LATENCY_MS: development only: outgoing latency 0..60000 milliseconds; 0 is off

    int simulated_latency_ms;

    /// ROCKETNET_CLIENT_SIMULATED_JITTER_MS: development only: outgoing jitter 0..60000 milliseconds; 0 is off

    int simulated_jitter_ms;

    /// ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT: development only: outgoing duplication 0..100; 0 is off

    int simulated_duplicate_percent;
};

/// The server is configured explicitly: rocketnet_server_default_config gives
/// the shape with no backend key and no protocol id, and start refuses it
/// until the backend's public key is supplied (ROCKETNET_STATUS_MISSING_KEYS),
/// because a default key is how every deployment of a protocol ends up sharing
/// one identity. The backend key and the protocol id come from the
/// environment (ROCKETNET_SERVER_BACKEND_PUBLIC_KEY and
/// ROCKETNET_SERVER_PROTOCOL_ID, recommended), from code that sets them into
/// the config after default_config, or both, the environment overriding.
///
/// The server's own X25519 keypair is not configured: create generates a
/// fresh one for each server object and destroy wipes its secret. The flow is
/// create; read rocketnet_server_public_key and, once started, the addresses
/// (rocketnet_server_ipv4_address, rocketnet_server_ipv6_address); the game
/// registers them with its backend over its own authenticated channel; the
/// backend mints connect tokens to that key and those addresses; the client
/// connects with one.
///
/// public_address, when set, displaces the create call's address at start, so the
/// address the transport binds and the address connect tokens are checked
/// against are always the same one. ROCKETNET_SERVER_PUBLIC_ADDRESS and
/// ROCKETNET_SERVER_SECOND_PUBLIC_ADDRESS are consumed together by the first
/// server start in the process, and say so in the log, so a process with several
/// servers is never redirected wholesale by ambient address variables.

struct rocketnet_server_config_t
{
    /// No env form: sizeof the struct the caller compiled against, set by rocketnet_server_default_config; 0 is refused

    size_t config_bytes;

    /// ROCKETNET_SERVER_PROTOCOL_ID: the title's protocol id; connect tokens must carry it

    uint64_t protocol_id;

    /// ROCKETNET_SERVER_MAX_CLIENTS: 1 .. ROCKETNET_MAX_CLIENTS; default 1000

    int max_clients;

    /// ROCKETNET_SERVER_PACKET_FILTER: the per-title packet filter, as the clients'

    uint8_t packet_filter[4];

    /// ROCKETNET_SERVER_BACKEND_PUBLIC_KEY: the backend's public key, the server's one configured key; required, start refuses all zeros (ROCKETNET_STATUS_MISSING_KEYS)

    uint8_t backend_public_key[32];

    /// ROCKETNET_SERVER_NUM_XDP_QUEUES: xdp and gso only; 1 .. 1024; 0 is the transport default

    int num_xdp_queues;

    /// ROCKETNET_SERVER_NUM_OS_CPUS: xdp only, the cores left to the OS; 1 .. 256; 0 is the transport default

    int num_os_cpus;

    /// ROCKETNET_SERVER_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PACKET_PAYLOAD_BYTES .. 305,408; 0 is the default, 16384

    int max_payload_bytes;

    /// ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES: per client; 1 .. 256; 0 is the default, 8

    int max_fragmented_messages;

    /// ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS: 1 .. 3600; default 60

    int max_timeout_seconds;

    /// ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS: 1 .. 60; default 5

    int handshake_deadline_seconds;

    /// ROCKETNET_SERVER_TRANSPORT: the candidate list; NULL is "auto"; every transport, xdp included, serves IPv4, IPv6, or both when second_public_address names the other family on the same port

    const char * transport;

    /// ROCKETNET_SERVER_PUBLIC_ADDRESS: "a.b.c.d:port" or "[v6]:port"; NULL is the create call's address

    const char * public_address;

    /// ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS: each client's send budget, kbps; the starting rate of each client connection; 0 is the default, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS (10000); ROCKETNET_SEND_KBPS_UNLIMITED is no send budget; 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1 is that budget; negative is refused with ROCKETNET_STATUS_INVALID_ARGUMENT

    int per_client_send_kbps;

    /// ROCKETNET_SERVER_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32

    int num_channels;

    /// ROCKETNET_SERVER_RELIABLE_MESSAGES_IN_FLIGHT: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024

    int reliable_messages_in_flight;

    /// ROCKETNET_SERVER_RECEIVE_MESSAGE_BUFFER_BYTES: the receive buffer for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144

    int receive_message_buffer_bytes;

    /// ROCKETNET_SERVER_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_message_buffer_bytes; 1415 .. 1 << 20; default 262144

    int max_message_bytes;

    /// ROCKETNET_SERVER_SEND_MESSAGE_BUFFER_BYTES: queued message bytes; per client; 12288 .. 1 << 28; default 800000

    int send_message_buffer_bytes;

    /// ROCKETNET_SERVER_SEND_QUEUE_MESSAGES: queued messages; per client; 16 .. 1 << 20; default 1024

    int send_queue_messages;

    /// ROCKETNET_SERVER_PACKETS_IN_FLIGHT: unacknowledged packets tracked; per client; 16 .. 1 << 16; default 256

    int packets_in_flight;

    /// ROCKETNET_SERVER_PACKETS_PER_UPDATE: packets sent per update; per client; 1 .. 1024; default 256; the send budget is the throttle

    int packets_per_update;

    /// ROCKETNET_SERVER_UNRELIABLE_MESSAGES_PER_UPDATE: unreliable messages delivered per update; per client per update; 1 .. 1 << 16; default 1024

    int unreliable_messages_per_update;

    /// ROCKETNET_SERVER_SEQUENCED_MESSAGES_HELD: sequenced messages held for order; 0 .. 4096; default 16

    int sequenced_messages_held;

    /// ROCKETNET_SERVER_MAX_MESSAGE_RECEIPTS: receipt outcomes held per client for rocketnet_server_receipts; a full queue drops its oldest, counted in receipts_dropped; per client; 16 .. 1 << 16; default 256

    int max_message_receipts;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS: the initial resend delay before an rtt sample, milliseconds; 10 .. 60000; default 2000

    int message_resend_delay_initial_ms;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS: optional sampled resend delay floor in milliseconds; 0 disables it; 0 .. 60000; default 0

    int message_resend_delay_min_ms;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS: the maximum resend delay, milliseconds; 10 .. 60000; default 2000

    int message_resend_delay_max_ms;

    /// ROCKETNET_SERVER_RELIABLE_MESSAGE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000

    int reliable_message_timeout_ms;

    /// Simulated link conditions: development only. A release library refuses nonzero simulated_* fields with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE.

    /// ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT: development only: outgoing loss 0..100; 0 is off

    int simulated_loss_percent;

    /// ROCKETNET_SERVER_SIMULATED_LATENCY_MS: development only: outgoing latency 0..60000 milliseconds; 0 is off

    int simulated_latency_ms;

    /// ROCKETNET_SERVER_SIMULATED_JITTER_MS: development only: outgoing jitter 0..60000 milliseconds; 0 is off

    int simulated_jitter_ms;

    /// ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT: development only: outgoing duplication 0..100; 0 is off

    int simulated_duplicate_percent;

    /// ROCKETNET_SERVER_SECOND_PUBLIC_ADDRESS: the other family's "a.b.c.d:port" or "[v6]:port" on the same port; NULL or empty serves one family

    const char * second_public_address;
};

/// -----------------------------------------
///                           Messages
/// -----------------------------------------

/// A message is bytes, a pointer and a length. Each client
/// and each server is one block made at create through the allocator -- the
/// object, its connections and one message region per connection -- and
/// allocates nothing after create (the transport underneath allocates its own
/// at client connect and server start). The _required_memory calls give that
/// block's size, so a program that wants every byte up front can hand
/// rocketnet_allocator an allocator over a block of its own.
///
/// Time is the game's clock, a double in seconds, monotonic; pass the same
/// clock to every call that takes one. The library reads no clock of its own
/// on these paths, so a game owns time and a test can hold it still.
///
/// The update order, on the main thread: update (messages in, acks into the
/// channels, received messages into deliveries, then the checks that
/// disconnect), receive, start message, write, finish message
/// (or abort message), then send_packets (each connection's messages, the
/// queued messages included in them, under its send budget, out to the wire).
///
/// Sending: start message, write the message, finish message is the only send
/// path. Start message returns a pointer to room for the message; serialize
/// the message straight into it, and finish message (bytes_written) queues it
/// and returns its receipt serial, nonzero; a NULL start message is a refused
/// send, 0. The pointer is valid
/// until finish message or abort message; abort message gives the room back.
/// One start message is pending per object at a time.
/// Reliability is a rocketnet_reliability_t. An unreliable message larger
/// than ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES is split across packets and
/// sent as its reliable twin, and the receiver sees the twin: UNRELIABLE becomes RELIABLE, UNRELIABLE_SEQUENCED
/// (6 bytes sooner, above 1405) becomes RELIABLE_SEQUENCED on its channel, and
/// UNRELIABLE_WITH_ACK_RECEIPT becomes RELIABLE_WITH_ACK_RECEIPT, keeping its
/// ACKED-only receipt. Every send is
/// bounded by the config's max_message_bytes; a larger one is refused.
///
/// A send the connection's region has no room for ends the connection: the
/// server disconnects that client with the reason
/// ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED, and a client ends in the
/// state ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED. The send budget
/// governs the wire, never the queue: a reliable message waits for budget and
/// is never dropped; an unreliable one over budget is dropped and counted
/// (the budget_drops stat).
///
/// Receiving is pumped: each receive fills the caller's batch,
/// struct rocketnet_message_batch_t (the receive section at the end of this
/// header), from one queue per client, or one per server across all clients.
/// Its data pointers are valid until the next receive or update.

struct rocketnet_message_batch_t;

/// @brief One outcome for a send's nonzero receipt serial.
/// Receipts are copied into caller storage after an update. A full receipt queue
/// drops its oldest entry; copy any result needed beyond the next update.

struct rocketnet_receipt_t
{
    /// @brief Identify the send whose outcome is reported.

    uint32_t serial;

    /// @brief Whether the peer transport acknowledged or missed it: the library
    /// writes ROCKETNET_RECEIPT_STATUS_ACKED or ROCKETNET_RECEIPT_STATUS_LOSS; a
    /// zero-filled receipt is ROCKETNET_RECEIPT_STATUS_NONE.

    enum rocketnet_receipt_status_t status;
};

/// -----------------------------------------
///                      Connect tokens
/// -----------------------------------------

/// Production connect tokens come from the game's backend, made for the
/// server's public key and addresses. The library creates development tokens
/// only (rocketnet_create_dev_connect_token below).

/// -----------------------------------------
///                             Client
/// -----------------------------------------

/// The client connects with a connect token, never an address: an opaque
/// NUL-terminated base64 string, at most ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES
/// with its NUL, that is the client's credential for one server, from the game's backend in production, or from
/// rocketnet_create_dev_connect_token in development.
///
/// The client is reusable: create makes a DISCONNECTED
/// client; rocketnet_client_connect starts a connection with a token;
/// rocketnet_client_disconnect ends it, from any state, and returns the client
/// to DISCONNECTED, ready to connect again with a new token; destroy works
/// from any state. Each connection starts fresh: nothing of an earlier one
/// is delivered or kept except the lifetime stats and, until disconnect or the
/// next connect, the negative state it ended in. The socket is per connection, opened by connect for the address
/// family the token names.

/// The library half of rocketnet_client_default_config: writes the defaults
/// into every field below config->config_bytes, config_bytes itself kept.
/// Returns ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED for
/// a config_bytes too small, nothing written. A NULL config is a counted no-op
/// that returns ROCKETNET_STATUS_INVALID_ARGUMENT. Call the inline below.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config_internal( struct rocketnet_client_config_t * config );

/// Sets config_bytes to the size this header's struct has, then writes the
/// defaults into every field. Returns what rocketnet_client_default_config_internal
/// returns; a NULL config is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.

static inline enum rocketnet_status_t rocketnet_client_default_config( struct rocketnet_client_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );
    return rocketnet_client_default_config_internal( config );
}

/// The bytes of the one block rocketnet_client_create allocates for this
/// config, the environment read as create reads it; 0 for a refused config, a
/// config_bytes too small included. NULL means the defaults.

ROCKETNET_EXPORT size_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );

#if ROCKETNET_DEVELOPMENT
/// The published development backend public key, as an initializer for the
/// server config's backend_public_key:
///
///     static const uint8_t dev_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
///     memcpy( config.backend_public_key, dev_key, sizeof( dev_key ) );
///
/// It is public: a server with it accepts the tokens
/// rocketnet_create_dev_connect_token makes, and a release server refuses it
/// at start (ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE).

#define ROCKETNET_DEV_BACKEND_PUBLIC_KEY { 0xb0, 0xab, 0x03, 0xb3, 0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99, 0x29, 0xa5, 0xb4, 0xbf, \
                                           0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d, 0x08, 0x4a, 0x08, 0xd8, 0x58, 0xbd, 0x24, 0x2c }

/// Creates a development connect token: an opaque NUL-terminated base64
/// string, written into connect_token, a buffer of
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES (NUL included), for
/// rocketnet_client_connect. It is
/// for a development server, one whose config's backend_public_key is the
/// published development backend key; a release server refuses that key at
/// start (ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE), so it never accepts these.
///
/// The inputs: the server's IPv4 address ("1.2.3.4:40000") and its IPv6
/// address ("[::1]:40000"), either NULL for a server that does not serve that
/// family but not both, each shorter than ROCKETNET_MAX_ADDRESS_STRING_BYTES
/// (as rocketnet_server_ipv4_address and rocketnet_server_ipv6_address give
/// them); the server's
/// public key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES, from
/// rocketnet_server_public_key; the token's lifetime, expire_seconds from
/// unix_timestamp; timeout_seconds, how long the connection may go silent; the
/// client's id; the title's protocol_id, as the server's config has it;
/// unix_timestamp, the create time in unix seconds (the call reads no clock);
/// and user_data, ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES long, or NULL for
/// zeros.
///
/// Returns ROCKETNET_STATUS_OK; ROCKETNET_STATUS_INVALID_ADDRESS for an
/// address that does not parse as its own family or is too long;
/// ROCKETNET_STATUS_INVALID_ARGUMENT for both addresses NULL, an
/// expire_seconds or timeout_seconds of zero or below, or a protocol_id of 0;
/// ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the crypto library refuses. Nothing
/// is written on a failure. A NULL server_public_key or connect_token is a
/// counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.

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
#endif /// #if ROCKETNET_DEVELOPMENT

/// A DISCONNECTED client, or NULL with the refusal in status: create is the one
/// check of the config, every field's range, the transport list's parse, the
/// simulated_* fields (nonzero is refused by a production library) and the
/// message rules, and logs the rule a refusal broke. Create reads no clock and
/// opens no socket. It reads
/// every field of the config below its config_bytes, the simulated_* fields
/// included; a config_bytes too small is refused with a log line before any
/// other check. NULL config is the defaults. context is an opaque caller pointer handed to the
/// allocator's callbacks and to the config's state_change_callback and
/// time_callback; keep it valid until destroy. The final nullable status
/// out-parameter holds ROCKETNET_STATUS_OK with a non-NULL result, or the
/// refusal's code with NULL: NOT_INITIALIZED before init,
/// CONFIG_NOT_INITIALIZED for a config_bytes too small, INVALID_ARGUMENT for a
/// negative send_kbps, CONFIG_OUT_OF_RANGE, INVALID_TRANSPORT_LIST or
/// MEMORY_BUDGET_EXCEEDED for the config refusal, CONFIG_ENV_INVALID for an
/// environment override that does not parse, or OUT_OF_MEMORY for a failed
/// allocation.

ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, enum rocketnet_status_t * status );

/// Starts a connection with connect_token, the NUL-terminated base64 string a
/// backend or rocketnet_create_dev_connect_token gave, at most
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES with its NUL. Connect reads no more
/// than that; a string with no NUL within it, or that is not exactly one
/// token's base64 (bad characters, truncated, overlong), returns
/// ROCKETNET_STATUS_INVALID_CONNECT_TOKEN with nothing changed.
/// Only from DISCONNECTED or a state a connection ended in after it was
/// CONNECTED (SERVER_DISCONNECTED down to MESSAGE_MEMORY_EXHAUSTED), which is
/// ended here first. From a handshake or token failure state
/// (CONNECTION_DENIED .. CONNECT_TOKEN_EXPIRED) call rocketnet_client_disconnect
/// first. Any other state returns ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED
/// with nothing changed. A token that
/// fails the pre-send checks returns ROCKETNET_STATUS_INVALID_CONNECT_TOKEN or
/// ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED and leaves the matching error state;
/// a socket or thread the platform refuses returns its status and leaves the
/// client DISCONNECTED. Connect reads no clock: the attempt's clock starts at
/// the next update. A NULL
/// client or connect_token is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT. Returns ROCKETNET_STATUS_OK,
/// ROCKETNET_STATUS_INVALID_ARGUMENT, ROCKETNET_STATUS_NOT_INITIALIZED,
/// ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED, ROCKETNET_STATUS_INVALID_CONNECT_TOKEN,
/// ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED, ROCKETNET_STATUS_CONFIG_ENV_INVALID,
/// ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED,
/// ROCKETNET_STATUS_INVALID_TRANSPORT_LIST, ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED,
/// ROCKETNET_STATUS_OUT_OF_MEMORY, ROCKETNET_STATUS_MUTEX_CREATE_FAILED,
/// or ROCKETNET_STATUS_THREAD_CREATE_FAILED.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const char * connect_token );

/// From any state: a live connection ends as rocketnet_client_disconnect ends
/// it. From SENDING_CONNECTION_RESPONSE or CONNECTED
/// the redundant disconnect packets are sent before the transport closes, so
/// the server frees the slot on receipt rather than at timeout; from
/// DISCONNECTED, an error state, REQUESTING_COOKIE or SENDING_CONNECTION_REQUEST
/// nothing is sent. The client's I/O thread is stopped, and destroy returns
/// once that thread has exited; no time bound is promised. The client's block
/// is wiped and freed. Every pointer from the client is invalid after the
/// call. A NULL client is a silent no-op, as free(NULL) is: no assert and
/// nothing counted.

ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

/// Runs one update: messages in, acks into the channels, received messages into
/// deliveries, then the checks that disconnect; what the update did is read
/// from rocketnet_client_state and rocketnet_client_stats. The update time
/// must be finite and nondecreasing: a NaN, an infinity or a step backwards is
/// refused, counted once as API misuse, and the last good time is kept, so the clock is
/// never poisoned or wedged. A NULL client is a counted no-op; with no
/// connection the update stores no time, so nothing is counted there.

ROCKETNET_EXPORT void rocketnet_client_update( struct rocketnet_client_t * client, double time );

/// Sends this frame's queued messages. A NULL client is a counted no-op.

ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

/// Ends the connection and returns the client to DISCONNECTED. From a
/// handshake state or CONNECTED the disconnect packets are sent first; from
/// a failure state the failure ends. The client's I/O thread is stopped, and
/// disconnect returns once that thread has exited; no time bound is promised. The client stays valid and can connect
/// again. A NULL client is a counted no-op.

ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

/// A NULL client returns ROCKETNET_CLIENT_STATE_INVALID, counted and logged.

ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

/// The name of a state ("connected", "invalid"), or "unknown" for a value that is not one.

ROCKETNET_EXPORT const char * rocketnet_client_state_name( enum rocketnet_client_state_t state );

/// The client's index on the server, 0 .. max_clients - 1, while CONNECTED; -1 in any other state. A NULL client is a counted no-op that returns -1.

ROCKETNET_EXPORT int rocketnet_client_index( const struct rocketnet_client_t * client );

/// The transport that won the candidate list at connect ("portable", "msgx"), NULL while DISCONNECTED. The returned string is static and remains valid for the process lifetime. A NULL client is a counted no-op that returns NULL.

ROCKETNET_EXPORT const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client );

/// The first step of the send: a pointer to room for a message of bytes, or
/// NULL when refused. Serialize the message straight into it:
///
///     uint8_t * p = rocketnet_client_start_message( client, channel, reliability, max_bytes );
///     if ( p )
///     {
///         int bytes = write_player_input( p, max_bytes, &input );    // serialize straight into the packet
///         serial = rocketnet_client_finish_message( client, bytes );
///     }
///
/// Finish accepts any bytes_written from 1 up to the max_bytes started; 0 or
/// more than max_bytes is refused and aborts the start.
///
/// The pointer is valid until finish message or abort message; one start
/// message is pending per object at a time. A message is sent only while the
/// client is CONNECTED (rocketnet_client_state). In every other state the send
/// is dropped: start message returns NULL, logs nothing and adds 1 to the
/// not_connected_drops stat (not API misuse), and finish message returns 0. The drop is
/// normal, never misuse, and nothing is held for a later send. The same
/// if ( out ) that covers the other NULL returns covers it, so game code sends
/// from create on with no state check. A NULL client is a counted no-op that
/// returns NULL.

ROCKETNET_EXPORT uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int bytes );

/// The queued message's receipt serial, nonzero; 0 when no start message is pending (a start dropped because the client was not CONNECTED included: that drop was counted at start, in the not_connected_drops stat) or bytes_written is outside 1 to the bytes started. A refused bytes_written logs and aborts the pending start, so the next start is never blocked; both cases return 0. A NULL client is a counted no-op that returns 0.

ROCKETNET_EXPORT uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written );

/// Gives back the room of the pending start message; with none pending it does nothing. A NULL client is a counted no-op.

ROCKETNET_EXPORT void rocketnet_client_abort_message( struct rocketnet_client_t * client );

/// The client's receive: refills the client's one batch with the next
/// messages of its receive queue, up to ROCKETNET_RECEIVE_BATCH_MESSAGES, every
/// channel's, each naming the server as its sender (client_index 0), and
/// returns it, or NULL when it filled nothing: pump it until NULL. The batch
/// and its data pointers are valid until the next receive call or update on
/// this client. A NULL client is a counted no-op that returns NULL.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_client_receive_messages( struct rocketnet_client_t * client );

/// Copies this update's receipt outcomes into out, which has room for
/// capacity entries, and returns how many it copied, 0 .. capacity. Call after
/// update; the entries stay the same until the next update, and a read does
/// not consume them, so a second call copies the same ones. When capacity is
/// smaller than the outcomes the update produced, the first capacity are
/// copied. A full receipt queue drops its oldest outcome, counted in the
/// receipts_dropped stat. A capacity below 1 returns 0; a NULL client or out
/// is a counted no-op that returns 0.

ROCKETNET_EXPORT int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity );

/// ROCKETNET_STATUS_OK when the budget took the new rate, from now on. 0 is the default rate, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS; a positive value below ROCKETNET_SEND_KBPS_UNLIMITED caps it, and ROCKETNET_SEND_KBPS_UNLIMITED is no send budget; a negative value returns ROCKETNET_STATUS_INVALID_ARGUMENT with the rate unchanged; a client that is not CONNECTED (a handshake state included) returns ROCKETNET_STATUS_INVALID_ARGUMENT with nothing changed and is not counted as misuse: the config's send_kbps sets a connection's starting rate. A NULL client is a counted no-op that returns ROCKETNET_STATUS_INVALID_ARGUMENT.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

/// Fills stats with the client's current stats. A NULL
/// stats is a counted no-op. A NULL client is a counted no-op that fills only
/// the process-wide fields, api_misuse and thread_priority_refusals, and zeroes
/// the rest.

ROCKETNET_EXPORT void rocketnet_client_stats( const struct rocketnet_client_t * client, struct rocketnet_client_stats_t * stats );

/// -----------------------------------------
///                             Server
/// -----------------------------------------

/// Clients are identified by index, an int, 0 .. max_clients - 1, the slot
/// each connection takes; only rocketnet_server_start_message takes its
/// destination as a uint64_t. A client_index outside 0 .. max_clients - 1 is
/// counted API misuse in every per-client call below, which then returns its
/// empty value.
///
/// The server is reusable: create checks the config and
/// makes the server's block but binds nothing, and the server starts stopped.
/// rocketnet_server_start binds the transport and accepts clients;
/// rocketnet_server_stop disconnects every client, resets every slot and
/// releases the transport, and the server can start again. The per-client
/// calls below are valid while running; on a stopped server they read 0.

/// The library half of rocketnet_server_default_config: writes the defaults
/// into every field below config->config_bytes, config_bytes itself kept.
/// Returns ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED for
/// a config_bytes too small, nothing written. A NULL config is a counted no-op
/// that returns ROCKETNET_STATUS_INVALID_ARGUMENT. Call the inline below.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config_internal( struct rocketnet_server_config_t * config );

/// Sets config_bytes to the size this header's struct has, then writes the
/// defaults into every field, with no backend key. Returns what
/// rocketnet_server_default_config_internal returns; a NULL config is a
/// counted no-op that returns ROCKETNET_STATUS_INVALID_ARGUMENT.

static inline enum rocketnet_status_t rocketnet_server_default_config( struct rocketnet_server_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );
    return rocketnet_server_default_config_internal( config );
}

/// The bytes of the one block rocketnet_server_create allocates for this
/// config, at its max_clients, the environment read as create reads it; 0 for
/// a refused config, a config_bytes too small included. NULL means the
/// defaults.

ROCKETNET_EXPORT size_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );

/// A stopped server, or NULL with the refusal in status: create is the one
/// check of the config, every field's range, the transport list's parse, the
/// simulated_* fields (nonzero is refused by a production library) and the
/// message rules, and logs the rule a refusal broke; the backend key is
/// checked at start, which refuses a config without one. Create reads every field of the
/// config below its config_bytes; a config_bytes too small is refused with a
/// log line before any other check. Address is where clients reach the server, "a.b.c.d:port" or
/// "[v6]:port"; a port of 0 takes an ephemeral port (read it back with
/// rocketnet_server_port once started). The server is created stopped and
/// binds nothing. NULL config is the defaults, which start refuses until a
/// backend key is given. context is an opaque caller pointer handed to the allocator's
/// callbacks and to the message and transport create; keep it valid until
/// destroy. The final nullable status out-parameter holds ROCKETNET_STATUS_OK
/// with a non-NULL result, or the refusal's code with NULL: NOT_INITIALIZED
/// before init, CONFIG_NOT_INITIALIZED for a config_bytes too small,
/// INVALID_ARGUMENT for a NULL address or a negative per_client_send_kbps,
/// INVALID_ADDRESS for an over-long address, CONFIG_OUT_OF_RANGE,
/// INVALID_TRANSPORT_LIST or MEMORY_BUDGET_EXCEEDED for the config refusal,
/// CONFIG_ENV_INVALID for an environment override that does not parse, or
/// OUT_OF_MEMORY for a failed allocation.

ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, enum rocketnet_status_t * status );

/// Binds the transport at the address and accepts clients, up to the
/// config's max_clients. Only from stopped: a running server returns
/// ROCKETNET_STATUS_SERVER_NOT_STOPPED with nothing changed. A transport that
/// does not start returns its status and leaves the server stopped:
/// ROCKETNET_STATUS_MISSING_KEYS, DEV_KEYS_IN_RELEASE, INVALID_PROTOCOL_ID,
/// INVALID_ADDRESS, CONFIG_ENV_INVALID, CONFIG_OUT_OF_RANGE,
/// MEMORY_BUDGET_EXCEEDED, INVALID_TRANSPORT_LIST, TRANSPORT_LADDER_EXHAUSTED,
/// TRANSPORT_PORT_ZERO_UNSUPPORTED, TRANSPORT_FAMILIES_UNSUPPORTED,
/// THREAD_CREATE_FAILED, TIMER_CREATE_FAILED, MUTEX_CREATE_FAILED or
/// OUT_OF_MEMORY. A NULL server is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_start( struct rocketnet_server_t * server );

/// Disconnects every client cleanly (the redundant disconnect packets), resets
/// every slot, releases the transport and leaves the server stopped; it can
/// start again. A stopped server's stop is a no-op. A NULL server is a counted
/// no-op.

ROCKETNET_EXPORT void rocketnet_server_stop( struct rocketnet_server_t * server );

/// true running, false stopped. A NULL server is a counted no-op that returns false.

ROCKETNET_EXPORT bool rocketnet_server_running( const struct rocketnet_server_t * server );

/// From either state: a running server is stopped first, as
/// rocketnet_server_stop does, so every connected client is sent the redundant
/// disconnect packets before the transport closes; with no connected client
/// nothing is sent. Then the server's memory, its keypair included, is wiped
/// and freed. Every pointer
/// from the server is invalid after the call. A NULL server is a silent no-op,
/// as free(NULL) is: no assert and nothing counted.

ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

/// Runs one update over every client; what it did is read from the per-client
/// calls (rocketnet_server_client_connected, rocketnet_server_disconnect_reason)
/// and the stats calls. The update time must be finite and nondecreasing: a
/// NaN, an infinity or a step backwards is refused, counted once in
/// API misuse, and the last good time is kept. On a stopped
/// server a counted no-op, storing no time. A NULL server is a counted no-op.

ROCKETNET_EXPORT void rocketnet_server_update( struct rocketnet_server_t * server, double time );

/// Sends this frame's queued messages. On a stopped server a counted no-op. A NULL server is a counted no-op.

ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

/// The port the server bound: the address's port, or the ephemeral port a
/// port of 0 was given; 0 while stopped. A NULL server is a counted no-op that
/// returns 0.

ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

/// Copies the server's X25519 public key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES,
/// into public_key: the key create generated for this server object, which
/// the game registers with its backend and passes to
/// rocketnet_create_dev_connect_token. Valid from create, running or stopped.
/// A NULL server or public_key is a counted no-op that writes nothing.

ROCKETNET_EXPORT void rocketnet_server_public_key( const struct rocketnet_server_t * server, uint8_t * public_key );

/// Writes the server's IPv4 address, as connect tokens name it ("a.b.c.d:port",
/// the bound port filled in), into address, address_bytes long
/// (ROCKETNET_MAX_ADDRESS_STRING_BYTES is always enough). Returns true when the
/// running server serves IPv4; false, with address an empty string, when it
/// does not, while stopped, or for a short buffer (logged). An address_bytes
/// of 0 writes nothing and returns false. A NULL server or address is a
/// counted no-op that returns false.

ROCKETNET_EXPORT bool rocketnet_server_ipv4_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// As rocketnet_server_ipv4_address, for IPv6 ("[v6]:port").

ROCKETNET_EXPORT bool rocketnet_server_ipv6_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// The transport that won the candidate list at start ("xdp", "gso", "io_uring", "msgx", "iocp", "rio", "portable", "loopback"), NULL while stopped. The returned string is static and remains valid for the process lifetime. A NULL server is a counted no-op that returns NULL.

ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

/// The config's max_clients, running or stopped. A NULL server is a counted no-op that returns 0.

ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

/// The clients connected now; 0 while stopped. A NULL server is a counted no-op that returns 0.

ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

/// true when a client holds the slot; false for a free slot or a stopped server. A NULL server, or a client_index outside 0 .. max_clients - 1, is a counted no-op that returns false.

ROCKETNET_EXPORT bool rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index );

/// The client id the connect token of the client in the slot carries; 0 for a free slot or a stopped server (0 is also a valid id a backend may assign, so read rocketnet_server_client_connected first). A NULL server, or a client_index outside 0 .. max_clients - 1, is a counted no-op that returns 0.

ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index );

/// Disconnects one client deliberately and frees its slot; a free slot or a
/// stopped server is a no-op. Why is the game's decision. A NULL server, or a
/// client_index outside 0 .. max_clients - 1, is a counted no-op.

ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

/// Disconnects every connected client as rocketnet_server_disconnect_client
/// does; the server keeps running. A NULL server is a counted no-op.

ROCKETNET_EXPORT void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server );

/// The reason the slot's last connection ended with, until the slot's next connection; NONE while stopped. A NULL server, or a client_index outside 0 .. max_clients - 1, is a counted no-op that returns ROCKETNET_DISCONNECT_REASON_NONE.

ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index );

/// The name of a disconnect reason ("none", "reliable timeout"), or "unknown" for a value that is not one.

ROCKETNET_EXPORT const char * rocketnet_disconnect_reason_name( enum rocketnet_disconnect_reason_t reason );

/// The first step of the send: a pointer to room for a message of bytes, or
/// NULL when refused. Serialize the message straight into it:
///
///     uint8_t * p = rocketnet_server_start_message( server, client_index, channel, reliability, max_bytes );
///     if ( p )
///     {
///         int bytes = write_player_input( p, max_bytes, &input );    // serialize straight into the packet
///         serial = rocketnet_server_finish_message( server, bytes );
///     }
///
/// Finish accepts any bytes_written from 1 up to the max_bytes started; 0 or
/// more than max_bytes is refused and aborts the start.
///
/// destination_id is a client index as a uint64_t. One with the reserved
/// ROCKETNET_FLOW_ID_BIT set, or at or past max_clients, is refused as counted
/// misuse (NULL; ROCKETNET_STATUS_FLOW_ID_RESERVED in the log for the bit). A message is sent only
/// to a client index whose connection is CONNECTED
/// (rocketnet_server_client_connected). A send to an index below max_clients
/// with no CONNECTED client, running server or stopped, is dropped: start
/// message returns NULL, logs nothing and adds 1 to the server's
/// not_connected_drops stat (not API misuse), and finish message returns 0. The drop is
/// normal, never misuse, and nothing is held for a later send; the same
/// if ( out ) covers it. To send to every connected client, loop over the
/// slots. The pointer is valid until finish message or abort message; one
/// start message is pending per server at a time, and finish message and
/// abort message act on it, so they take no destination. A NULL server is a counted
/// no-op that returns NULL.

ROCKETNET_EXPORT uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, enum rocketnet_reliability_t reliability, int bytes );

/// Finishes the pending start message: its receipt serial, nonzero; 0 when no start message is pending (a refused or dropped start included) or bytes_written is outside 1 to the bytes started. A refused bytes_written logs and aborts the pending start, so the next start is never blocked; both cases return 0. A NULL server is a counted no-op that returns 0.

ROCKETNET_EXPORT uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, int bytes_written );

/// Gives back the room of the pending start message; with none pending it does nothing. A NULL server is a counted no-op.

ROCKETNET_EXPORT void rocketnet_server_abort_message( struct rocketnet_server_t * server );

/// The server's receive: refills the server's one batch, which covers every
/// client, with the next messages of its receive queue, up to
/// ROCKETNET_RECEIVE_BATCH_MESSAGES, each naming its sender in client_index[],
/// and returns it, or NULL when it filled nothing (none pending, or the
/// server stopped): pump it until NULL. The batch and its data pointers are
/// valid until the next receive call or update on this server. A NULL server
/// is a counted no-op that returns NULL.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_server_receive_messages( struct rocketnet_server_t * server );

/// As rocketnet_client_receipts, for one client slot: copies this update's
/// receipt outcomes for the client in the slot. A free slot or a stopped
/// server returns 0. A NULL server, or a client_index outside
/// 0 .. max_clients - 1, is a counted no-op that returns 0.

ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity );

/// ROCKETNET_STATUS_OK when the client's budget took the new rate, from now on (the slot must hold a CONNECTED client). 0 is the default rate, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS; a positive value below ROCKETNET_SEND_KBPS_UNLIMITED caps it, and ROCKETNET_SEND_KBPS_UNLIMITED is no send budget; a negative value returns ROCKETNET_STATUS_INVALID_ARGUMENT with the rate unchanged. A slot with no CONNECTED client, or a stopped server, returns ROCKETNET_STATUS_INVALID_ARGUMENT with nothing changed and is not counted as misuse: the config's per_client_send_kbps sets each connection's starting rate. A client_index outside 0 .. max_clients - 1 is counted misuse and returns ROCKETNET_STATUS_INVALID_ARGUMENT. A NULL server is a counted no-op that returns ROCKETNET_STATUS_INVALID_ARGUMENT.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps );

/// Fills stats with the server's current stats. A NULL
/// stats is a counted no-op. A NULL server is a counted no-op that fills only
/// the process-wide fields, api_misuse and thread_priority_refusals, and zeroes
/// the rest.

ROCKETNET_EXPORT void rocketnet_server_stats( const struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats );

/// Fills stats with one client slot's current stats; a
/// free slot or a stopped server zeroes it. A NULL server, or a client_index
/// outside 0 .. max_clients - 1, is counted misuse and zeroes it; a NULL stats
/// is a counted no-op.

ROCKETNET_EXPORT void rocketnet_server_client_stats( const struct rocketnet_server_t * server, int client_index, struct rocketnet_server_client_stats_t * stats );

/// -----------------------------------------
///             Receiving messages
/// -----------------------------------------

/// The send's channel is its ordering channel: RELIABLE_ORDERED,
/// RELIABLE_ORDERED_WITH_ACK_RECEIPT and the two SEQUENCED types are ordered
/// or sequenced within it, and a channel at or past the config's num_channels
/// refuses those sends. UNRELIABLE, RELIABLE, UNRELIABLE_WITH_ACK_RECEIPT and
/// RELIABLE_WITH_ACK_RECEIPT ignore it: any channel value is accepted for
/// them, never refused, and they are delivered on channel 0.
///
/// Receiving is pumped: one queue per client,
/// and one per server across all its clients. Each client and each server
/// holds one struct rocketnet_message_batch_t, made at create; each receive
/// refills it with the queue's next messages, up to its capacity
/// ROCKETNET_RECEIVE_BATCH_MESSAGES, one array per field: each message's
/// sender (on a client, index = 0, the server), the channel it was delivered
/// on, its reliability, its length and a pointer to its bytes in the
/// library's storage, with the count in num_messages, and returns it, or NULL
/// when it filled nothing. Pump it until NULL. There is no per-update cap: what
/// one call does not return waits for the next call in the same update, in
/// order. The queue holds every channel's messages: ordered and sequenced
/// messages keep their order within their channel and sender, and on a server
/// the queue holds the update's messages client by client in index order:
///
///     const struct rocketnet_message_batch_t * batch;
///     while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
///     {
///         for ( int i = 0; i < batch->num_messages; i++ )
///             handle( batch->client_index[i], batch->data[i], batch->bytes[i] );
///     }
///
/// The batch and its data pointers are valid until the next receive call or
/// the next update of the same client or server, whichever comes first; there
/// is no release call. Copy what is kept longer.

/// The most messages one receive call fills.

#define ROCKETNET_RECEIVE_BATCH_MESSAGES                   65536

/// @brief One batch of received messages, one array per field, entry i of
/// every array describing message i, in receive order.

struct rocketnet_message_batch_t
{
    /// @brief The entries filled, 0 when the queue is empty.

    int num_messages;

    /// @brief The sender's index: on a server, the sending client's client_index; on
    /// a client, index = 0, the server, which sends every message a client
    /// receives.

    int client_index[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// @brief The channel the message was delivered on: its ordering channel
    /// for an ordered or sequenced type, 0 for the types that carry none.

    int channel[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// @brief The reliability the message arrived as: a receipt type arrives
    /// as its twin without the receipt (UNRELIABLE, RELIABLE or
    /// RELIABLE_ORDERED), and a promoted unreliable send as its reliable twin.

    enum rocketnet_reliability_t reliability[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// @brief The message's length in bytes.

    int bytes[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// @brief The message's bytes in the library's storage, valid until the next receive call or update.

    const uint8_t * data[ROCKETNET_RECEIVE_BATCH_MESSAGES];
};

#ifdef __cplusplus
}
#endif

#endif /// #ifndef ROCKETNET_H
