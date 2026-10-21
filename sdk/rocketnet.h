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
/// The main frame calls take under 0.1 ms each. Create calls may take
/// longer. The client's disconnect and destroy stop its network thread and
/// return once that thread has exited; no time bound is promised for them.
///
/// Sizes: a buffer size is a size_t; config fields, message bytes, counts
/// and client indexes are int; a send's destination id is a uint64_t.
///
/// Pure lookups and time calls are thread-safe; client and server instances
/// are not.

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/// MSVC /Wall reports C4514 (an unreferenced inline function was removed) at
/// the end of every translation unit that does not call each inline below; it
/// is informational, and a public header cannot scope it, so it is off.

#if defined( _MSC_VER )
#pragma warning( disable : 4514 )
#endif

/// -----------------------------------------
///                            Version
/// -----------------------------------------

/// ROCKETNET_DEVELOPMENT is 1 when linking a development library
/// (rocketnet_dev): it carries rocketnet_create_dev_connect_token and
/// ROCKETNET_DEV_BACKEND_PUBLIC_KEY and accepts the published development
/// backend key.

#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 0
#endif // !defined( ROCKETNET_DEVELOPMENT )

#if !ROCKETNET_DEVELOPMENT

    /// The SDK version as text: the release version string, or "dev" in a
    /// development build.

    #define ROCKETNET_VERSION_FULL                     "1.0.0-preview-1"

    /// The major component of the SDK version; development builds use 255.

    #define ROCKETNET_VERSION_MAJOR                                    1

    /// The minor component of the SDK version; development builds use 255.

    #define ROCKETNET_VERSION_MINOR                                    0

    /// The patch component of the SDK version; development builds use 255.

    #define ROCKETNET_VERSION_PATCH                                    0

#else // !ROCKETNET_DEVELOPMENT

    #define ROCKETNET_VERSION_FULL                                 "dev"
    #define ROCKETNET_VERSION_MAJOR                                  255
    #define ROCKETNET_VERSION_MINOR                                  255
    #define ROCKETNET_VERSION_PATCH                                  255

#endif // !ROCKETNET_DEVELOPMENT

/// The version as one comparable integer: major * 10000 + minor * 100 +
/// patch, 10000 for 1.0.0. The prerelease is not in the integer:
/// v1.0.0-preview-1 is 10000 too, and ROCKETNET_VERSION_FULL tells it apart.
/// A development build is 2575755, above every release. rocketnet_version
/// returns the value the library was built with.

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

/// The most clients a server can take.

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

#define ROCKETNET_MAX_PACKET_PAYLOAD_BYTES                 1416

/// The largest message that fits in one packet, the same bound for every
/// reliability type. An unreliable message larger than this is
/// split across packets and sent as its reliable twin, and the receiver sees
/// the twin: UNRELIABLE becomes RELIABLE, UNRELIABLE_SEQUENCED becomes
/// RELIABLE_SEQUENCED on its channel, and UNRELIABLE_WITH_ACK_RECEIPT
/// becomes RELIABLE_WITH_ACK_RECEIPT.

#define ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES             1411

/// The buffer size in bytes for a connect token string: 1024 base64
/// characters plus the NUL.

#define ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES           1025

/// The size in bytes of a connect token's application-defined user data.

#define ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES            256

/// The size in bytes of the backend's signing public key.

#define ROCKETNET_BACKEND_PUBLIC_KEY_BYTES                  32

/// The size in bytes of a server's public key, what
/// rocketnet_server_public_key copies out.

#define ROCKETNET_SERVER_PUBLIC_KEY_BYTES                   32

/// The size in bytes, NUL included, of every address string the API takes or
/// writes ("a.b.c.d:port" or "[v6]:port"); a longer input is refused.

#define ROCKETNET_MAX_ADDRESS_STRING_BYTES                 256

/// The alignment in bytes of the memory the library uses.

#define ROCKETNET_ALLOCATION_ALIGNMENT                     64

/// The channel bound: define it before including rocketnet.h, and build the
/// library with the same value.

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                             32
#endif

/// The default client to server send budget, in kbps, used when the client
/// config's send_kbps is 0.

#define ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS            1000

/// The default server to client send budget per client, in kbps, used when
/// the server config's per_client_send_kbps is 0.

#define ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS            10000

/// The send_kbps value for no send budget on a connection.

#define ROCKETNET_SEND_KBPS_UNLIMITED                      0x7fffffff

#ifdef __cplusplus
extern "C" {
#endif

/// What a call that can fail returns. Calls that return something else: a
/// create returns a pointer, NULL when refused; finish message returns a
/// uint32_t receipt serial, 0 when refused; start message and the
/// transport_name calls return a pointer; the required_memory calls return
/// the byte count, 0 when refused; the stats calls fill a caller struct; a
/// pure question returns bool; a call that reports nothing returns void.
///
/// OK is the one success value, and every failure is a positive code. A
/// value's number and meaning never change.

enum rocketnet_status_t
{
    /// Success.

    ROCKETNET_STATUS_OK                                 = 0,

    /// rocketnet_init, or an allocator change, after the library was
    /// initialized or while it is initializing or terminating.

    ROCKETNET_STATUS_ALREADY_INITIALIZED                = 1,

    /// A call that needs rocketnet_init came before it, or a term found
    /// nothing to terminate.

    ROCKETNET_STATUS_NOT_INITIALIZED                    = 2,

    /// Term or an allocator change refused: the library still holds
    /// allocations.

    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = 3,

    /// The platform's socket layer did not start; correct the platform setup
    /// before retrying.

    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = 4,

    /// The crypto library did not start; initialization does not complete.

    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = 5,

    /// An argument, or a combination of arguments, is NULL, out of range, or
    /// inconsistent with another.

    ROCKETNET_STATUS_INVALID_ARGUMENT                   = 6,

    /// An environment override is malformed: it did not parse.

    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = 7,

    /// A config field, from the struct or the environment, is out of its
    /// supported range.

    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = 8,

    /// The memory the config needs is past the operator's memory budget.

    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = 9,

    /// Start refused: the server config's backend_public_key is all zeros.

    ROCKETNET_STATUS_MISSING_KEYS                       = 10,

    /// The protocol id is 0, or not the expected value.

    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = 11,

    /// Start refused: a release build was given the published development
    /// backend key.

    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = 12,

    /// An address string did not parse or cannot be used.

    ROCKETNET_STATUS_INVALID_ADDRESS                    = 13,

    /// The transport candidate list did not parse, or holds no usable
    /// candidate.

    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = 14,

    /// The connect token fails the client's pre-send checks: it did not
    /// parse, or is not for this protocol.

    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = 15,

    /// The connect token expired before the first packet was sent; obtain a
    /// fresh token before connecting.

    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = 16,

    /// A pinned transport cannot take port 0, an ephemeral port.

    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = 17,

    /// Every transport in the candidate list refused to start.

    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = 18,

    /// An allocation the operation needs failed.

    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,

    /// The platform refused a mutex.

    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,

    /// The platform refused a thread.

    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,

    /// The platform refused a timer.

    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,

    /// Connect refused, nothing changed: the client is connecting or
    /// connected, or in a handshake or token failure state
    /// (CONNECTION_DENIED .. CONNECT_TOKEN_EXPIRED), which needs
    /// rocketnet_client_disconnect first.

    ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED            = 23,

    /// Start refused, nothing changed: the server is already running.

    ROCKETNET_STATUS_SERVER_NOT_STOPPED                 = 24,

    /// A destination id has the reserved ROCKETNET_FLOW_ID_BIT set; the send
    /// is refused.

    ROCKETNET_STATUS_FLOW_ID_RESERVED                   = 25,

    /// A transport cannot serve both named families on this machine.

    ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED     = 26,

    /// A config's config_bytes is 0 or too small: fill the config with its
    /// default_config call first.

    ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED             = 27,

    /// The count of codes, OK included; never an operation's result.

    ROCKETNET_STATUS_COUNT                              = 28
};

/// -----------------------------------------
///                          Log levels
/// -----------------------------------------

/// The process-wide log levels, least to most verbose.

enum rocketnet_log_level_t
{
    /// As the maximum level, only lines logged at NONE are emitted; quiet
    /// mode suppresses every line.

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

/// -----------------------------------------
///                  Address types
/// -----------------------------------------

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

/// -----------------------------------------
///                    Message enums
/// -----------------------------------------

/// A send's reliability: how a message is delivered, ordered and
/// acknowledged. The ordered and sequenced types order within the send's
/// channel, and a channel at or past the config's num_channels refuses them.
/// UNRELIABLE, RELIABLE, UNRELIABLE_WITH_ACK_RECEIPT and
/// RELIABLE_WITH_ACK_RECEIPT ignore the channel: any value is accepted, and
/// they are delivered on channel 0.

enum rocketnet_reliability_t
{
    /// Sent once, with no delivery guarantee, order or receipt. The receiver
    /// sees the message at most once. A lost packet loses it.

    ROCKETNET_RELIABILITY_UNRELIABLE                            = 0,

    /// Sent once, with no delivery guarantee. The receiver delivers a message
    /// only when it is newer than the last one delivered on its channel, and
    /// drops older ones.

    ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED                  = 1,

    /// Resent until the peer acknowledges it, and delivered once, with no
    /// order among the messages on its channel.

    ROCKETNET_RELIABILITY_RELIABLE                              = 2,

    /// Resent until the peer acknowledges it, and delivered once in channel
    /// order: the receiver holds later messages until earlier ones arrive.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED                      = 3,

    /// Resent until the peer acknowledges it. The receiver delivers only the
    /// newest message on the channel and can drop a stale one.

    ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED                    = 4,

    /// As UNRELIABLE, with a receipt: ACKED or LOSS.

    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT           = 5,

    /// As RELIABLE, with an ACKED receipt.

    ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT             = 6,

    /// As RELIABLE_ORDERED, with an ACKED receipt.

    ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT     = 7
};

/// A receipt's outcome, the status field of struct rocketnet_receipt_t.

enum rocketnet_receipt_status_t
{
    /// A zero-filled receipt: no outcome has been written.

    ROCKETNET_RECEIPT_STATUS_NONE                               = 0,

    /// The peer acknowledged the message. This does not mean the peer
    /// application read it.

    ROCKETNET_RECEIPT_STATUS_ACKED                              = 1,

    /// The message was not acknowledged, and it is not resent.

    ROCKETNET_RECEIPT_STATUS_LOSS                               = 2
};

/// Why the connection in a server's client slot ended: what
/// rocketnet_server_disconnect_reason returns. A connection that ended reads
/// a negative reason. A client reads how its own connection ended from
/// rocketnet_client_state.

enum rocketnet_disconnect_reason_t
{
    /// No connection has ended, or the connection is up.

    ROCKETNET_DISCONNECT_REASON_NONE                            =  0,

    /// The two ends' configs differ in a field that must match on both ends.

    ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH                 = -1,

    /// The peer sent what the protocol forbids.

    ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION              = -2,

    /// A reliable message went unacknowledged past
    /// reliable_message_timeout_ms.

    ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT                = -3,

    /// The connection's message memory had no room for a message it must not
    /// drop.

    ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED        = -4,

    /// The server disconnected the client, stopped, or was destroyed.

    ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED             = -5
};

/// -----------------------------------------
///                        Client state
/// -----------------------------------------

/// What rocketnet_client_state returns. A connect attempt runs up from
/// DISCONNECTED (0) through the three handshake states to CONNECTED. A
/// negative state is how the last attempt or connection ended, one state per
/// way it can end, kept until rocketnet_client_disconnect returns the client
/// to DISCONNECTED. From SERVER_DISCONNECTED down to MESSAGE_MEMORY_EXHAUSTED
/// (a connection that ended after it was CONNECTED), rocketnet_client_connect
/// ends it first; from CONNECTION_DENIED down to CONNECT_TOKEN_EXPIRED,
/// disconnect first.

enum rocketnet_client_state_t
{
    /// What a NULL client reads as; no client is ever in this state.

    ROCKETNET_CLIENT_STATE_INVALID                          = -13,

    /// The connection's message memory had no room for a message it must not
    /// drop.

    ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED         = -12,

    /// The two ends' configs differ in a field that must match on both ends.

    ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH                  = -11,

    /// The server sent what the protocol forbids.

    ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION               = -10,

    /// A reliable message went unacknowledged past
    /// reliable_message_timeout_ms.

    ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT               = -9,

    /// The server disconnected the client, stopped, or was destroyed.

    ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED              = -8,

    /// The connect token expired before the server accepted the client.

    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            = -7,

    /// The connect token was refused before the first packet was sent.

    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            = -6,

    /// A connection heard nothing from the server for the token's timeout.

    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             = -5,

    /// The server never answered the third leg of the handshake.

    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    = -4,

    /// The server never answered the connection request.

    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     = -3,

    /// The server never answered the cookie request.

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

    /// Connected: messages flow both ways.

    ROCKETNET_CLIENT_STATE_CONNECTED                        =  4
};

/// -----------------------------------------
///                              Stats
/// -----------------------------------------

/// A client's stats, filled by rocketnet_client_stats. The counts are
/// monotonic over the client's life, across connections, and start at 0 at
/// create, except api_misuses and thread_priority_refusals, which are
/// process-wide and never reset. The measured values cover the current
/// connection, rates over the trailing second, and read 0 with no
/// connection.

struct rocketnet_client_stats_t
{
    /// Packets: UDP datagrams sent, handshake included.

    uint64_t packets_sent;

    /// Packets: UDP datagrams received, handshake included.

    uint64_t packets_received;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_sent;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_received;

    /// Payloads: packets from the server that opened and carried messages.

    uint64_t payloads_received;

    /// Messages: reliable messages resent.

    uint64_t messages_resent;

    /// Messages: reliable messages received a second time and dropped.

    uint64_t messages_duplicate;

    /// Receipts: sends with a receipt that the server acknowledged.

    uint64_t receipts_acked;

    /// Receipts: sends with a receipt that were lost.

    uint64_t receipts_lost;

    /// Receipts: outcomes dropped, oldest first, because the receipt queue
    /// was full.

    uint64_t receipts_dropped;

    /// Messages: unreliable messages dropped because the send budget was
    /// spent.

    uint64_t budget_drops;

    /// Hand-offs: sends that left with the next hand-off to the send thread

    uint64_t send_deferred;

    /// Messages: sends dropped because the connection was not CONNECTED.

    uint64_t not_connected_drops;

    /// Process-wide: API misuses since the process started.

    uint64_t api_misuse;

    /// Process-wide: thread priority and timer period calls the operating
    /// system refused since the process started.

    uint64_t thread_priority_refusals;

    /// Milliseconds: round trip time.

    double rtt;

    /// Milliseconds: receive jitter, a moving average of how much the time
    /// between consecutive arrivals from the server changes from one arrival
    /// to the next.

    double jitter;

    /// Percent: receive-side loss, the share of the server's packet sequence
    /// missing over the trailing second.

    double packet_loss;

    /// Packets per second sent.

    double packets_sent_per_second;

    /// Packets per second received.

    double packets_received_per_second;

    /// Kbps (1000 bit/s) sent.

    double bandwidth_sent_kbps;

    /// Kbps (1000 bit/s) received.

    double bandwidth_received_kbps;
};

/// A server's stats over all its clients, filled by rocketnet_server_stats.
/// The counts are monotonic over the server's life, across starts and stops,
/// and start at 0 at create, except api_misuses and
/// thread_priority_refusals, which are process-wide and never reset. The
/// rates are the whole server's over the trailing second, and read 0 while
/// stopped.

struct rocketnet_server_stats_t
{
    /// Packets: UDP datagrams sent.

    uint64_t packets_sent;

    /// Packets: UDP datagrams received.

    uint64_t packets_received;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_sent;

    /// Bytes: UDP payload plus the IP and UDP headers of the peer's family.

    uint64_t bytes_received;

    /// Connections: clients that completed the handshake.

    uint64_t clients_connected;

    /// Requests: connect attempts refused because every client slot was
    /// taken.

    uint64_t clients_denied_full;

    /// Requests: connect attempts dropped by the per-update handshake budget.

    uint64_t handshakes_over_budget;

    /// Payloads: packets from clients that opened and carried messages.

    uint64_t payloads_received;

    /// Messages: reliable messages resent.

    uint64_t messages_resent;

    /// Messages: reliable messages received a second time and dropped.

    uint64_t messages_duplicate;

    /// Receipts: sends with a receipt that a client acknowledged.

    uint64_t receipts_acked;

    /// Receipts: sends with a receipt that were lost.

    uint64_t receipts_lost;

    /// Receipts: outcomes dropped, oldest first, because a client's receipt
    /// queue was full.

    uint64_t receipts_dropped;

    /// Messages: unreliable messages dropped because a client's send budget
    /// was spent.

    uint64_t budget_drops;

    /// Messages: sends dropped because the connection was not CONNECTED.

    uint64_t not_connected_drops;

    /// Process-wide: API misuses since the process started.

    uint64_t api_misuse;

    /// Process-wide: thread priority and timer period calls the operating
    /// system refused since the process started.

    uint64_t thread_priority_refusals;

    /// Packets per second sent.

    double packets_sent_per_second;

    /// Packets per second received.

    double packets_received_per_second;

    /// Kbps (1000 bit/s) sent.

    double bandwidth_sent_kbps;

    /// Kbps (1000 bit/s) received.

    double bandwidth_received_kbps;
};

/// One client slot's stats, filled by rocketnet_server_client_stats. The
/// values cover the current connection in the client slot and go back to 0
/// when a new connection takes it; rates cover the trailing second.

struct rocketnet_server_client_stats_t
{
    /// Packets: UDP datagrams sent to this client.

    uint64_t packets_sent;

    /// Packets: UDP datagrams received from this client.

    uint64_t packets_received;

    /// Bytes: UDP payload plus the IP and UDP headers of this client's
    /// family.

    uint64_t bytes_sent;

    /// Bytes: UDP payload plus the IP and UDP headers of this client's
    /// family.

    uint64_t bytes_received;

    /// Milliseconds: round trip time, the smallest of the recent samples.

    double rtt;

    /// Milliseconds: round trip jitter, the mean amount by which the recent
    /// round trip samples exceed the smallest of them.

    double jitter;

    /// Percent: send-side loss, the share of recent packets sent to this client
    /// that were never acknowledged.

    double packet_loss;

    /// Kbps (1000 bit/s) sent to this client.

    double bandwidth_sent_kbps;

    /// Kbps (1000 bit/s) received from this client.

    double bandwidth_received_kbps;

    /// Kbps (1000 bit/s) of sent packets this client acknowledged.

    double bandwidth_acked_kbps;
};

/// The library and its callers are built without -fshort-enums: every public
/// enum is the size of int, checked at compile time where the language has a
/// static assert.

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

/// Initializes the library and its platform and crypto state. Call it before
/// creating a client or a server, and do not race it with term. context is
/// an opaque pointer passed to the allocator's callbacks; NULL is the
/// default. Returns ROCKETNET_STATUS_OK; ROCKETNET_STATUS_ALREADY_INITIALIZED
/// when the library is initialized or initializing; or
/// ROCKETNET_STATUS_PLATFORM_INIT_FAILED or ROCKETNET_STATUS_CRYPTO_INIT_FAILED
/// when that layer does not start.
///
/// Environment: ROCKETNET_LOG_LEVEL overrides the maximum log level before
/// init logs anything: 0 .. 5, ROCKETNET_LOG_LEVEL_NONE ..
/// ROCKETNET_LOG_LEVEL_SPAM; a value outside that range is ignored with a
/// warning.
///
///     if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK )
///     {
///         // handle failure
///     }

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_init( void * context );

/// Terminates the library once every client and server is destroyed. Do not
/// call it while other calls into the library are running. A refused term
/// logs one error line. Returns ROCKETNET_STATUS_OK;
/// ROCKETNET_STATUS_NOT_INITIALIZED with nothing to terminate, or while
/// initializing or terminating; or ROCKETNET_STATUS_LIVE_ALLOCATIONS while a
/// client, a server or another allocation is still held.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_term( void );

/// The name of a status code without the ROCKETNET_STATUS_ prefix ("OK",
/// "INVALID_ARGUMENT"), or "UNKNOWN" for a value that is not a code, never
/// NULL. It allocates nothing, works before init, and the string lives for
/// the process.

ROCKETNET_EXPORT const char * rocketnet_status_name( enum rocketnet_status_t status );

/// Receives a failed internal assert: the borrowed condition, function and
/// file strings, and the source line that failed. It runs on the thread that
/// fires the assert.

typedef void (*rocketnet_assert_function_t)( const char * condition, const char * function, const char * file, int line );

/// Installs the process-wide function for a failed internal assert, or
/// restores the default, which logs and traps, for NULL. The library calls it
/// only when built with asserts enabled, a debug build. Install it before
/// other calls into the library run concurrently, since the library reads it
/// unsynchronized.

ROCKETNET_EXPORT void rocketnet_assert_function( rocketnet_assert_function_t function );

/// Suppresses all log output for true, and resumes it, subject to the log
/// level, for false. The setting is process-wide and does not change the
/// log level.

ROCKETNET_EXPORT void rocketnet_quiet( bool flag );

/// Sets the process-wide maximum log level, ROCKETNET_LOG_LEVEL_NONE ..
/// ROCKETNET_LOG_LEVEL_SPAM; a value outside the enum is ignored.

ROCKETNET_EXPORT void rocketnet_log_level( enum rocketnet_log_level_t level );

/// Turns the library's thread priority calls on or off. On, the default, the
/// library raises the priority of each network thread it creates, and on
/// Windows it also requests a 1 ms timer period from rocketnet_init to
/// rocketnet_term. Off, it makes none of these calls. A game that sets its
/// own thread priorities turns it off.
///
/// The setting is process-wide. Set it before rocketnet_init to skip the
/// timer period. Threads created after a change follow the new value; a
/// running thread keeps what it got. A refusal by the operating system never
/// fails a create or rocketnet_init: the thread runs at its default priority,
/// the first refusal of each kind logs one warning, and the stats structs'
/// thread_priority_refusals counts them.

ROCKETNET_EXPORT void rocketnet_thread_priority( bool enabled );

/// The name of a log level ("ERROR", "SPAM"), or "UNKNOWN" for a value outside
/// the enum, never NULL. A log function uses it to render a level the way the
/// default sink does. The string lives for the process.

ROCKETNET_EXPORT const char * rocketnet_log_level_name( enum rocketnet_log_level_t level );

/// Receives one formatted log line: the one log sink. It runs on the thread
/// that logs, which can be a network thread, so it must be thread-safe, and
/// it must not call rocketnet_log_function. context is the pointer given when
/// it was installed. message is NUL-terminated, length is the bytes before
/// the NUL, at most 1023, and message is valid only until the function
/// returns.

typedef void (*rocketnet_log_function_t)( void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

/// Installs the log function, the one log sink, with its context; NULL
/// restores the default sink, which prints to stdout, and clears the context.
/// A log call already running may finish on the previous function, so keep
/// the old context valid until it returns.

ROCKETNET_EXPORT void rocketnet_log_function( rocketnet_log_function_t function, void * context );

/// -----------------------------------------
///                          API misuse
/// -----------------------------------------

/// The stats structs' api_misuses field counts, process-wide since the
/// process started, exactly these: a NULL argument a call needs; a
/// client_index or destination id outside 0 .. max_clients - 1, the reserved
/// ROCKETNET_FLOW_ID_BIT included; a create before rocketnet_init; a refused
/// rocketnet_allocator; an update time that is not finite or goes backwards,
/// after which the last good time is kept; an unreliable start message of at
/// most ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES still open at update or
/// send_packets; a connect from a state other than DISCONNECTED or a
/// connection-ended state; and a server start while running.
///
/// A NULL object or argument, or an out-of-range client_index, is counted as
/// misuse, and the call does nothing and returns its zero value: 0, false,
/// NULL, ROCKETNET_DISCONNECT_REASON_NONE, or
/// ROCKETNET_STATUS_INVALID_ARGUMENT for a status return. Three calls differ:
/// rocketnet_client_state returns ROCKETNET_CLIENT_STATE_INVALID,
/// rocketnet_client_index returns -1, and a stats call with a NULL client or
/// server still fills the process-wide fields and zeroes the rest. A NULL to
/// either destroy does nothing and is not counted, as free(NULL).
///
/// Not misuse: a send refused for its own arguments (a second start message
/// while one is pending, a bytes_written out of range, a channel or
/// reliability out of range) returns NULL or 0 and logs. A send on a
/// connection that is not CONNECTED is a normal drop, counted in the
/// not_connected_drops stat. Update or send_packets on a stopped server does
/// nothing. A setter called on a client that is not connected, or for a
/// server's client slot with no connected client, does nothing and returns
/// ROCKETNET_STATUS_OK; only a bad value returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.

/// Monotonic elapsed time in seconds, from an unspecified origin.

ROCKETNET_EXPORT double rocketnet_time( void );

/// Blocks the calling thread for seconds. A negative, zero or NaN value
/// returns at once, and a value above 2000 sleeps 2000 seconds.

ROCKETNET_EXPORT void rocketnet_sleep( double seconds );

/// -----------------------------------------
///                          Allocator
/// -----------------------------------------

/// Replaces the allocator. Call it before init, with no allocations
/// outstanding. Both callbacks must be thread-safe; malloc_function returns
/// memory aligned for any fundamental type, or NULL, and free_function
/// receives the pointer malloc_function returned. The library aligns what it
/// uses to ROCKETNET_ALLOCATION_ALIGNMENT itself, so a callback is asked for
/// somewhat more than the library uses.
///
/// Returns ROCKETNET_STATUS_OK when both callbacks are installed;
/// ROCKETNET_STATUS_INVALID_ARGUMENT when either is NULL;
/// ROCKETNET_STATUS_ALREADY_INITIALIZED after init or while initializing or
/// terminating; or ROCKETNET_STATUS_LIVE_ALLOCATIONS while allocations are
/// outstanding. A refusal logs one line and is counted as API misuse.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

/// -----------------------------------------
///                             Config
/// -----------------------------------------

/// The client and the server each take one plain config struct. Fill it with
/// rocketnet_client_default_config or rocketnet_server_default_config, set
/// what the program means, and pass it to create. Precedence at create is
/// DEFAULT < CONFIG STRUCT < ENVIRONMENT: every field except a callback has an
/// environment variable, named first in the field's comment, so an operator
/// can tune an already-built binary with no rebuild. Every override is logged
/// at create with the variable and the value it displaced. A value that does
/// not parse, or is out of range, refuses the create with one error line;
/// nothing is ever clamped. An environment variable set to the empty string
/// is ignored, with a log line.
///
/// Environment encodings: integers are decimal; byte fields are base64 of
/// exactly the field's length; strings are taken as they are.
///
/// Transport is an ordered candidate list, comma separated, tried left to
/// right at client connect and server start; the first transport that starts
/// wins, and a list that every candidate refuses fails loudly. "auto" (the
/// default, also NULL or the empty string) is this platform's order: a Linux
/// server tries xdp,gso,io_uring,portable, a macOS server msgx,portable, and
/// a Windows server portable,iocp,rio; a client tries msgx,portable on macOS
/// and portable everywhere else. The loopback transport is never in auto; it
/// is chosen by name. A one-element list is a pin: that transport or loud
/// failure. A known name that this build, platform or endpoint does not have
/// (xdp for a client) is skipped with one log line, so one config line
/// deploys to mixed machines; a name that is no transport anywhere is a typo
/// and fails loudly, listing the valid names.
/// rocketnet_client_transport_name and rocketnet_server_transport_name return
/// the transport that won.
///
/// Fields marked "must match on both ends" are checked by the connection; a
/// mismatch ends it with CONFIG_MISMATCH. Create checks each field's range
/// and that the message fields agree with each other (max_message_bytes at
/// most receive_message_buffer_bytes, and the like), and logs the rule a
/// refusal broke. Create copies every string it is given, so the strings need
/// to live only for the call.
///
/// The first field of each config, config_bytes, is the size of the struct
/// the caller compiled against, so a config struct can grow at its end
/// without breaking a program built against an older header. The inline
/// rocketnet_client_default_config and rocketnet_server_default_config set it
/// to sizeof(*config) and then fill every field, so the idiom is two lines:
///
///     struct rocketnet_client_config_t config;
///     rocketnet_client_default_config( &config );
///
/// Every other call that takes a config reads config_bytes first: 0, or too
/// small to hold the field itself (a zeroed config that skipped
/// default_config), is refused with ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED
/// and a log line. A larger size, from a newer header, is read as far as the
/// library knows and the rest is ignored; a smaller size, from an older
/// header, is read as far as it goes and the rest takes the defaults. The
/// library never reads or writes past config_bytes. A NULL config, where a
/// call accepts one, means the defaults.

struct rocketnet_client_config_t
{
    /// No env form: the size of the struct the caller compiled against, set
    /// by rocketnet_client_default_config; 0 is refused.

    size_t config_bytes;

    /// ROCKETNET_CLIENT_TRANSPORT: the candidate list; NULL is "auto".

    const char * transport;

    /// No env form: called on change of the connection's handshake state;
    /// NULL is none. It runs on the caller's thread, inside update,
    /// disconnect and destroy, and must not call into the client.

    void (*state_change_callback)( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state );

    /// No env form: the receive stamp clock; NULL is the platform clock. Runs on the I/O thread, and on the caller's thread during disconnect and destroy.

    double (*time_callback)( void * context );

    /// ROCKETNET_CLIENT_PACKET_FILTER: the per-title packet filter; it must
    /// match the server's.

    uint8_t packet_filter[4];

    /// ROCKETNET_CLIENT_STEP_DRIVEN: nonzero (env 1) runs the client I/O step inside update and send_packets on the caller's thread, with no I/O thread; 0 (the default) keeps the I/O thread

    int step_driven;

    /// ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES: the largest payload this end
    /// fragments and reassembles; a lost fragment loses the whole payload;
    /// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES .. 305408; 0 is the default, 16384.

    int max_payload_bytes;

    /// ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES: 1 .. 256; 0 is the default,
    /// 8.

    int max_fragmented_messages;

    /// ROCKETNET_CLIENT_SEND_KBPS: the send budget in kbps, the starting rate
    /// of each connection; 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1 is that
    /// budget; ROCKETNET_SEND_KBPS_UNLIMITED is no send budget; 0 is the
    /// default, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS; a negative value is
    /// refused with ROCKETNET_STATUS_INVALID_ARGUMENT.

    int send_kbps;

    /// ROCKETNET_CLIENT_NUM_CHANNELS: the channels messages are sent and
    /// received on; must match on both ends; 1 .. ROCKETNET_MAX_CHANNELS;
    /// default 32.

    int num_channels;

    /// ROCKETNET_CLIENT_RELIABLE_MESSAGES_IN_FLIGHT: reliable messages in
    /// flight; must match on both ends; a power of two; 64 .. 1048576;
    /// default 1024.

    int reliable_messages_in_flight;

    /// ROCKETNET_CLIENT_RECEIVE_MESSAGE_BUFFER_BYTES: the receive buffer for
    /// reassembled messages; must match on both ends; a multiple of 16;
    /// 4096 .. 1048576; default 262144.

    int receive_message_buffer_bytes;

    /// ROCKETNET_CLIENT_MAX_MESSAGE_BYTES: the largest message; must match on
    /// both ends; at most receive_message_buffer_bytes;
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES .. 1048576; default 262144.

    int max_message_bytes;

    /// ROCKETNET_CLIENT_SEND_MESSAGE_BUFFER_BYTES: queued message bytes;
    /// 12288 .. 268435456; default 800000.

    int send_message_buffer_bytes;

    /// ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES: queued messages; 16 .. 1048576;
    /// default 1024.

    int send_queue_messages;

    /// ROCKETNET_CLIENT_PACKETS_IN_FLIGHT: unacknowledged packets tracked;
    /// 16 .. 65536; default 256.

    int packets_in_flight;

    /// ROCKETNET_CLIENT_PACKETS_PER_UPDATE: the most packets sent per update;
    /// 1 .. 1024; default 256; the send budget is the throttle.

    int packets_per_update;

    /// ROCKETNET_CLIENT_UNRELIABLE_MESSAGES_PER_UPDATE: unreliable messages
    /// delivered per update; 1 .. 65536; default 1024.

    int unreliable_messages_per_update;

    /// ROCKETNET_CLIENT_SEQUENCED_MESSAGES_HELD: sequenced messages held for
    /// order; 0 .. 4096; default 16.

    int sequenced_messages_held;

    /// ROCKETNET_CLIENT_MAX_MESSAGE_RECEIPTS: receipt outcomes held for
    /// rocketnet_client_receipts; 16 .. 65536; default 256.

    int max_message_receipts;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS: the resend delay
    /// before the first rtt sample, in milliseconds; 10 .. 60000; default
    /// 2000.

    int message_resend_delay_initial_ms;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS: the smallest resend
    /// delay, in milliseconds; 0 is no floor; 0 .. 60000; default 0.

    int message_resend_delay_min_ms;

    /// ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS: the largest resend
    /// delay, in milliseconds; 10 .. 60000; default 2000.

    int message_resend_delay_max_ms;

    /// ROCKETNET_CLIENT_RELIABLE_MESSAGE_TIMEOUT_MS: a reliable message
    /// unacknowledged this long ends the connection, in milliseconds;
    /// 1000 .. 600000; default 10000.

    int reliable_message_timeout_ms;

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
/// ROCKETNET_SERVER_PROTOCOL_ID), from code that sets them into the config
/// after default_config, or both, the environment overriding.
///
/// The server's own keypair is not configured: create generates a fresh one
/// for each server object and destroy wipes its secret. The flow is create;
/// read rocketnet_server_public_key and, once started, the addresses
/// (rocketnet_server_ipv4_address, rocketnet_server_ipv6_address); the game
/// registers them with its backend over its own authenticated channel; the
/// backend mints connect tokens to that key and those addresses; the client
/// connects with one.
///
/// public_address, when set, displaces the create call's address at start,
/// so the address the transport binds and the address connect tokens are
/// checked against are always the same one. ROCKETNET_SERVER_PUBLIC_ADDRESS
/// and ROCKETNET_SERVER_SECOND_PUBLIC_ADDRESS are consumed together by the
/// first server start in the process, and say so in the log, so a process
/// with several servers is never redirected wholesale by ambient address
/// variables.

struct rocketnet_server_config_t
{
    /// No env form: the size of the struct the caller compiled against, set
    /// by rocketnet_server_default_config; 0 is refused.

    size_t config_bytes;

    /// ROCKETNET_SERVER_PROTOCOL_ID: the title's protocol id; connect tokens
    /// must carry it; 0 is refused.

    uint64_t protocol_id;

    /// ROCKETNET_SERVER_MAX_CLIENTS: 1 .. ROCKETNET_MAX_CLIENTS; default 1000.

    int max_clients;

    /// ROCKETNET_SERVER_PACKET_FILTER: the per-title packet filter; it must
    /// match the clients'.

    uint8_t packet_filter[4];

    /// ROCKETNET_SERVER_BACKEND_PUBLIC_KEY: the backend's public key, the
    /// server's one configured key; required: start refuses all zeros with
    /// ROCKETNET_STATUS_MISSING_KEYS.

    uint8_t backend_public_key[32];

    /// ROCKETNET_SERVER_NUM_XDP_QUEUES: xdp and gso only; 1 .. 1024; 0 is the
    /// transport default.

    int num_xdp_queues;

    /// ROCKETNET_SERVER_NUM_OS_CPUS: xdp only, the cores left to the
    /// operating system; 1 .. 256; 0 is the transport default.

    int num_os_cpus;

    /// ROCKETNET_SERVER_MAX_PAYLOAD_BYTES: the largest payload this end
    /// fragments and reassembles; a lost fragment loses the whole payload;
    /// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES .. 305408; 0 is the default, 16384.

    int max_payload_bytes;

    /// ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES: per client; 1 .. 256; 0 is
    /// the default, 8.

    int max_fragmented_messages;

    /// ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS: 1 .. 3600; default 60.

    int max_timeout_seconds;

    /// ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS: 1 .. 60; default 5.

    int handshake_deadline_seconds;

    /// ROCKETNET_SERVER_TRANSPORT: the candidate list; NULL is "auto". Every
    /// transport serves IPv4, IPv6, or both when second_public_address names
    /// the other family on the same port.

    const char * transport;

    /// ROCKETNET_SERVER_PUBLIC_ADDRESS: "a.b.c.d:port" or "[v6]:port"; NULL
    /// is the create call's address.

    const char * public_address;

    /// ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS: each client's send budget in
    /// kbps, the starting rate of each client connection;
    /// 1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1 is that budget;
    /// ROCKETNET_SEND_KBPS_UNLIMITED is no send budget; 0 is the default,
    /// ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS; a negative value is refused
    /// with ROCKETNET_STATUS_INVALID_ARGUMENT.

    int per_client_send_kbps;

    /// ROCKETNET_SERVER_NUM_CHANNELS: the channels messages are sent and
    /// received on; must match on both ends; 1 .. ROCKETNET_MAX_CHANNELS;
    /// default 32.

    int num_channels;

    /// ROCKETNET_SERVER_RELIABLE_MESSAGES_IN_FLIGHT: reliable messages in
    /// flight; must match on both ends; a power of two; 64 .. 1048576;
    /// default 1024.

    int reliable_messages_in_flight;

    /// ROCKETNET_SERVER_RECEIVE_MESSAGE_BUFFER_BYTES: the receive buffer for
    /// reassembled messages; must match on both ends; a multiple of 16;
    /// 4096 .. 1048576; default 262144.

    int receive_message_buffer_bytes;

    /// ROCKETNET_SERVER_MAX_MESSAGE_BYTES: the largest message; must match on
    /// both ends; at most receive_message_buffer_bytes;
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES .. 1048576; default 262144.

    int max_message_bytes;

    /// ROCKETNET_SERVER_SEND_MESSAGE_BUFFER_BYTES: queued message bytes, per
    /// client; 12288 .. 268435456; default 800000.

    int send_message_buffer_bytes;

    /// ROCKETNET_SERVER_SEND_QUEUE_MESSAGES: queued messages, per client;
    /// 16 .. 1048576; default 1024.

    int send_queue_messages;

    /// ROCKETNET_SERVER_PACKETS_IN_FLIGHT: unacknowledged packets tracked,
    /// per client; 16 .. 65536; default 256.

    int packets_in_flight;

    /// ROCKETNET_SERVER_PACKETS_PER_UPDATE: the most packets sent per update,
    /// per client; 1 .. 1024; default 256; the send budget is the throttle.

    int packets_per_update;

    /// ROCKETNET_SERVER_UNRELIABLE_MESSAGES_PER_UPDATE: unreliable messages
    /// delivered per update, per client; 1 .. 65536; default 1024.

    int unreliable_messages_per_update;

    /// ROCKETNET_SERVER_SEQUENCED_MESSAGES_HELD: sequenced messages held for
    /// order; 0 .. 4096; default 16.

    int sequenced_messages_held;

    /// ROCKETNET_SERVER_MAX_MESSAGE_RECEIPTS: receipt outcomes held for
    /// rocketnet_server_receipts, per client; 16 .. 65536; default 256.

    int max_message_receipts;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS: the resend delay
    /// before the first rtt sample, in milliseconds; 10 .. 60000; default
    /// 2000.

    int message_resend_delay_initial_ms;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS: the smallest resend
    /// delay, in milliseconds; 0 is no floor; 0 .. 60000; default 0.

    int message_resend_delay_min_ms;

    /// ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS: the largest resend
    /// delay, in milliseconds; 10 .. 60000; default 2000.

    int message_resend_delay_max_ms;

    /// ROCKETNET_SERVER_RELIABLE_MESSAGE_TIMEOUT_MS: a reliable message
    /// unacknowledged this long ends the connection, in milliseconds;
    /// 1000 .. 600000; default 10000.

    int reliable_message_timeout_ms;

    /// ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT: development only: outgoing loss 0..100; 0 is off

    int simulated_loss_percent;

    /// ROCKETNET_SERVER_SIMULATED_LATENCY_MS: development only: outgoing latency 0..60000 milliseconds; 0 is off

    int simulated_latency_ms;

    /// ROCKETNET_SERVER_SIMULATED_JITTER_MS: development only: outgoing jitter 0..60000 milliseconds; 0 is off

    int simulated_jitter_ms;

    /// ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT: development only: outgoing duplication 0..100; 0 is off

    int simulated_duplicate_percent;

    /// ROCKETNET_SERVER_SECOND_PUBLIC_ADDRESS: the other family's
    /// "a.b.c.d:port" or "[v6]:port" on the same port; NULL or empty serves
    /// one family.

    const char * second_public_address;
};

/// -----------------------------------------
///                           Messages
/// -----------------------------------------

/// A message is bytes, a pointer and a length.
///
/// A client or a server allocates its memory at create, through the
/// allocator, and allocates nothing after create except what the transport
/// allocates at client connect and server start. The required_memory calls
/// give the bytes create allocates, so a program that wants every byte up
/// front can hand rocketnet_allocator an allocator over memory of its own.
///
/// Time: update takes the game's time, a double in seconds; pass the same
/// clock to every update, so a game owns time and a test can hold it still.
/// The library uses the platform clock only to stamp what it receives.
///
/// Each frame, on one thread: update; receive until it returns NULL; any
/// number of sends, each start message, write, then finish message or abort
/// message; then send_packets, which sends this update's packets under each
/// connection's send budget.
///
/// The send budget governs sending, never the queue: a reliable message waits
/// for budget and is never dropped; an unreliable message over budget is
/// dropped and counted in the budget_drops stat. A send the connection's
/// message memory has no room for ends the connection: the server
/// disconnects that client with the reason
/// ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED, and a client ends in
/// the state ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED.

struct rocketnet_message_batch_t;

/// One send's outcome, as rocketnet_client_receipts and
/// rocketnet_server_receipts copy it out.

struct rocketnet_receipt_t
{
    /// The receipt serial finish message returned for the send.

    uint32_t serial;

    /// ROCKETNET_RECEIPT_STATUS_ACKED or ROCKETNET_RECEIPT_STATUS_LOSS; a
    /// zero-filled receipt is ROCKETNET_RECEIPT_STATUS_NONE.

    enum rocketnet_receipt_status_t status;
};

/// -----------------------------------------
///                      Connect tokens
/// -----------------------------------------

/// A connect token is an opaque NUL-terminated base64 string, at most
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES with its NUL: a client's
/// credential for one server. Production tokens come from the game's
/// backend, made for the server's public key and addresses. The library makes
/// development tokens only, with rocketnet_create_dev_connect_token.

/// -----------------------------------------
///                             Client
/// -----------------------------------------

/// The client connects with a connect token, never an address. The client is
/// reusable: create makes a DISCONNECTED client; rocketnet_client_connect
/// starts a connection with a token; rocketnet_client_disconnect ends it,
/// from any state, and returns the client to DISCONNECTED, ready to connect
/// again with a new token; destroy works from any state. Each connection
/// starts fresh: nothing of an earlier one is delivered or kept except the
/// lifetime stats and, until disconnect or the next connect, the negative
/// state it ended in. The socket is per connection, opened by connect for
/// the address family the token names.

/// The library half of rocketnet_client_default_config: writes the defaults
/// into every field below config->config_bytes, keeping config_bytes. Returns
/// ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED for a
/// config_bytes too small, with nothing written.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config_internal( struct rocketnet_client_config_t * config );

/// Sets config_bytes to the size of this header's struct, then writes the
/// defaults into every field. Returns what
/// rocketnet_client_default_config_internal returns.

static inline enum rocketnet_status_t rocketnet_client_default_config( struct rocketnet_client_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );
    return rocketnet_client_default_config_internal( config );
}

/// The bytes rocketnet_client_create allocates for this config, the
/// environment read as create reads it; 0 for a refused config, a
/// config_bytes too small included. NULL means the defaults.

ROCKETNET_EXPORT size_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );

#if ROCKETNET_DEVELOPMENT

/// The published development backend public key, as an initializer for the
/// server config's backend_public_key:
///
///     static const uint8_t dev_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] =
///         ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
///     memcpy( config.backend_public_key, dev_key, sizeof( dev_key ) );
///
/// It is public: a server with it accepts the tokens
/// rocketnet_create_dev_connect_token makes, and a release server refuses it
/// at start with ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE.

#define ROCKETNET_DEV_BACKEND_PUBLIC_KEY { 0xb0, 0xab, 0x03, 0xb3, 0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99, 0x29, 0xa5, 0xb4, 0xbf, \
                                           0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d, 0x08, 0x4a, 0x08, 0xd8, 0x58, 0xbd, 0x24, 0x2c }

/// Writes a development connect token into connect_token, a buffer of
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES, for rocketnet_client_connect. It
/// is for a development server, one whose backend_public_key is
/// ROCKETNET_DEV_BACKEND_PUBLIC_KEY; a release server refuses that key at
/// start, so it never accepts these.
///
/// The inputs: the server's IPv4 address ("1.2.3.4:40000") and its IPv6
/// address ("[::1]:40000"), as rocketnet_server_ipv4_address and
/// rocketnet_server_ipv6_address give them, either NULL for a server that
/// does not serve that family but not both; server_public_key, the
/// server's key of ROCKETNET_SERVER_PUBLIC_KEY_BYTES from
/// rocketnet_server_public_key; the token's lifetime, expire_seconds from
/// unix_timestamp; timeout_seconds, how long the connection may go silent;
/// the client's id; the title's protocol_id, as the server's config has it;
/// unix_timestamp, the create time in unix seconds; and user_data,
/// ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES long, or NULL for zeros.
///
/// Returns ROCKETNET_STATUS_OK; ROCKETNET_STATUS_INVALID_ADDRESS for an
/// address that does not parse as its own family or is too long;
/// ROCKETNET_STATUS_INVALID_ARGUMENT for both addresses NULL, an
/// expire_seconds or timeout_seconds of zero or below, or a protocol_id of 0;
/// or ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the crypto library refuses.
/// Nothing is written on a failure.

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

#endif // ROCKETNET_DEVELOPMENT

/// Creates a DISCONNECTED client, or returns NULL with the refusal in status.
/// Create is the one check of the config: every field's range, the transport
/// list's parse and the message rules; it logs the rule a refusal broke, and
/// opens no socket. NULL config is the defaults. context is an opaque caller
/// pointer handed to the allocator's callbacks and to state_change_callback;
/// keep it valid until destroy.
///
/// status, when not NULL, receives ROCKETNET_STATUS_OK with a client, or the
/// refusal's code with NULL: NOT_INITIALIZED before init;
/// CONFIG_NOT_INITIALIZED for a config_bytes too small, checked first;
/// INVALID_ARGUMENT for a negative send_kbps; CONFIG_OUT_OF_RANGE,
/// INVALID_TRANSPORT_LIST or MEMORY_BUDGET_EXCEEDED for the config;
/// CONFIG_ENV_INVALID for an environment override that does not parse; or
/// OUT_OF_MEMORY for a failed allocation.

ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, enum rocketnet_status_t * status );

/// Starts a connection with connect_token. Connect reads at most
/// ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES; a string with no NUL within
/// that, or that is not exactly one token, returns
/// ROCKETNET_STATUS_INVALID_CONNECT_TOKEN with nothing changed.
///
/// Connect works from DISCONNECTED, or from a state a connection ended in
/// after it was CONNECTED (SERVER_DISCONNECTED down to
/// MESSAGE_MEMORY_EXHAUSTED), which it ends first. From a handshake or token
/// failure state (CONNECTION_DENIED .. CONNECT_TOKEN_EXPIRED) call
/// rocketnet_client_disconnect first. Any other state returns
/// ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED with nothing changed. A token
/// that fails the pre-send checks returns
/// ROCKETNET_STATUS_INVALID_CONNECT_TOKEN or
/// ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED and leaves the matching state; a
/// socket or thread the platform refuses returns its status and leaves the
/// client DISCONNECTED. The attempt's clock starts at the next update.
///
/// Returns ROCKETNET_STATUS_OK, or INVALID_ARGUMENT, NOT_INITIALIZED,
/// CLIENT_NOT_DISCONNECTED, INVALID_CONNECT_TOKEN, CONNECT_TOKEN_EXPIRED,
/// CONFIG_ENV_INVALID, CONFIG_OUT_OF_RANGE, MEMORY_BUDGET_EXCEEDED,
/// INVALID_TRANSPORT_LIST, TRANSPORT_LADDER_EXHAUSTED, OUT_OF_MEMORY,
/// MUTEX_CREATE_FAILED or THREAD_CREATE_FAILED.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const char * connect_token );

/// Destroys the client from any state. A live connection ends as
/// rocketnet_client_disconnect ends it: from SENDING_CONNECTION_RESPONSE or
/// CONNECTED the redundant disconnect packets are sent before the transport
/// closes, so the server frees the client slot at once rather than at
/// timeout; from any other state nothing is sent. The client's network
/// thread is stopped, and destroy returns once that thread has exited; no
/// time bound is promised. The client's memory is wiped and freed, and every
/// pointer from the client is invalid after the call.

ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

/// Runs one update at time: messages in, received messages queued for
/// receive, then the checks that disconnect. What the update did is read
/// from rocketnet_client_state and rocketnet_client_stats.

ROCKETNET_EXPORT void rocketnet_client_update( struct rocketnet_client_t * client, double time );

/// Sends this update's packets: the queued messages, under the send budget.

ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

/// Ends the connection and returns the client to DISCONNECTED. From a
/// handshake state or CONNECTED the disconnect packets are sent first; from
/// a failure state the failure ends. The client's network thread is stopped,
/// and disconnect returns once that thread has exited; no time bound is
/// promised. The client stays valid and can connect again.

ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

/// The client's state.

ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

/// The name of a client state ("CONNECTED", "INVALID"), or "UNKNOWN" for a
/// value that is not one, never NULL.

ROCKETNET_EXPORT const char * rocketnet_client_state_name( enum rocketnet_client_state_t state );

/// The client's index on the server, 0 .. max_clients - 1, while CONNECTED;
/// -1 in any other state.

ROCKETNET_EXPORT int rocketnet_client_index( const struct rocketnet_client_t * client );

/// The transport that won the candidate list at connect ("portable",
/// "msgx"), or NULL while DISCONNECTED. The string lives for the process.

ROCKETNET_EXPORT const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client );

/// The first step of a send: returns a pointer to room for a message of up
/// to bytes, or NULL when refused. Serialize straight into the message:
///
///     uint8_t * p = rocketnet_client_start_message( client, channel,
///                                                   reliability, max_bytes );
///     if ( p )
///     {
///         int bytes = write_player_input( p, max_bytes, &input );
///         serial = rocketnet_client_finish_message( client, bytes );
///     }
///
/// The pointer is valid until finish message or abort message; one start
/// message is pending per client at a time. A message larger than the
/// config's max_message_bytes is refused. A message is sent only while the
/// client is CONNECTED; in every other state the send is dropped: start
/// message returns NULL, logs nothing and adds 1 to the not_connected_drops
/// stat, and finish message returns 0. Nothing is held for a later send. The
/// same if ( p ) that covers the other NULL returns covers it, so game code
/// sends from create on with no state check.

ROCKETNET_EXPORT uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int bytes );

/// Queues the pending start message with bytes_written bytes, 1 .. the bytes
/// started, and returns its receipt serial, nonzero. Returns 0 when no start
/// message is pending, a dropped start included, or for a bytes_written out
/// of range, which logs and aborts the pending start so the next start is
/// never blocked.

ROCKETNET_EXPORT uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written );

/// Gives back the room of the pending start message; with none pending it
/// does nothing.

ROCKETNET_EXPORT void rocketnet_client_abort_message( struct rocketnet_client_t * client );

/// Refills the client's batch with the next messages from the server, each
/// with client_index 0, and returns it, or NULL when there are none: pump it
/// until NULL.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_client_receive_messages( struct rocketnet_client_t * client );

/// Copies this update's receipt outcomes into out, which has room for
/// capacity entries, and returns how many it copied, 0 .. capacity; a
/// capacity below 1 returns 0.
///
/// Every finished send has a nonzero receipt serial; the three
/// _WITH_ACK_RECEIPT reliability types report an outcome for it. ACKED means
/// the peer acknowledged the message, not that the peer application read it.
/// LOSS means an UNRELIABLE_WITH_ACK_RECEIPT message that fits in one packet
/// was not acknowledged; it is not resent. The reliable receipt types, and an
/// unreliable receipt message larger than
/// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES, report ACKED only, and a
/// connection that ends reports no LOSS for them.
///
/// Call it after update. The outcomes stay the same until the next update,
/// and a read does not consume them, so a second call copies the same ones;
/// when capacity is smaller than the outcomes, the first capacity are copied.
/// The receipt queue holds max_message_receipts outcomes; a full queue drops
/// its oldest, counted in the receipts_dropped stat.

ROCKETNET_EXPORT int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity );

/// Sets the send budget from now on, and returns ROCKETNET_STATUS_OK. 0 is
/// the default rate, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS; a positive
/// value below ROCKETNET_SEND_KBPS_UNLIMITED caps the rate; and
/// ROCKETNET_SEND_KBPS_UNLIMITED is no send budget. A negative value returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT with the rate unchanged. The config's
/// send_kbps sets each connection's starting rate.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

/// Fills stats with the client's current stats.

ROCKETNET_EXPORT void rocketnet_client_stats( const struct rocketnet_client_t * client, struct rocketnet_client_stats_t * stats );

/// -----------------------------------------
///                             Server
/// -----------------------------------------

/// Clients are identified by client_index, an int, 0 .. max_clients - 1, the
/// client slot each connection takes; only rocketnet_server_start_message
/// takes its destination as a uint64_t.
///
/// The server is reusable: create checks the config and allocates the server
/// but binds nothing, and the server starts stopped. rocketnet_server_start
/// binds the transport and accepts clients; rocketnet_server_stop
/// disconnects every client, frees every client slot and releases the
/// transport, and the server can start again. The per-client calls are valid
/// while running; on a stopped server they read 0.

/// The library half of rocketnet_server_default_config: writes the defaults
/// into every field below config->config_bytes, keeping config_bytes. Returns
/// ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED for a
/// config_bytes too small, with nothing written.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config_internal( struct rocketnet_server_config_t * config );

/// Sets config_bytes to the size of this header's struct, then writes the
/// defaults into every field, with no backend key. Returns what
/// rocketnet_server_default_config_internal returns.

static inline enum rocketnet_status_t rocketnet_server_default_config( struct rocketnet_server_config_t * config )
{
    if ( config )
        config->config_bytes = sizeof( *config );
    return rocketnet_server_default_config_internal( config );
}

/// The bytes rocketnet_server_create allocates for this config, at its
/// max_clients, the environment read as create reads it; 0 for a refused
/// config, a config_bytes too small included. NULL means the defaults.

ROCKETNET_EXPORT size_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );

/// Creates a stopped server, or returns NULL with the refusal in status.
/// Create is the one check of the config: every field's range, the transport
/// list's parse and the message rules; it logs the rule a refusal broke. The
/// backend key is checked at start. address is where clients reach the
/// server, "a.b.c.d:port" or "[v6]:port"; a port of 0 takes an ephemeral
/// port, read back with rocketnet_server_port once started. The server binds
/// nothing until start. NULL config is the defaults, which start refuses
/// until a backend key is given. context is an opaque caller pointer handed
/// to the allocator's callbacks; keep it valid until destroy.
///
/// status, when not NULL, receives ROCKETNET_STATUS_OK with a server, or the
/// refusal's code with NULL: NOT_INITIALIZED before init;
/// CONFIG_NOT_INITIALIZED for a config_bytes too small, checked first;
/// INVALID_ARGUMENT for a NULL address or a negative per_client_send_kbps;
/// INVALID_ADDRESS for an over-long address; CONFIG_OUT_OF_RANGE,
/// INVALID_TRANSPORT_LIST or MEMORY_BUDGET_EXCEEDED for the config;
/// CONFIG_ENV_INVALID for an environment override that does not parse; or
/// OUT_OF_MEMORY for a failed allocation.

ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, enum rocketnet_status_t * status );

/// Binds the transport at the address and accepts clients, up to the config's
/// max_clients. A running server returns ROCKETNET_STATUS_SERVER_NOT_STOPPED
/// with nothing changed. A start that fails returns its status and leaves the
/// server stopped: MISSING_KEYS, DEV_KEYS_IN_RELEASE, INVALID_PROTOCOL_ID,
/// INVALID_ADDRESS, CONFIG_ENV_INVALID, CONFIG_OUT_OF_RANGE,
/// MEMORY_BUDGET_EXCEEDED, INVALID_TRANSPORT_LIST, TRANSPORT_LADDER_EXHAUSTED,
/// TRANSPORT_PORT_ZERO_UNSUPPORTED, TRANSPORT_FAMILIES_UNSUPPORTED,
/// THREAD_CREATE_FAILED, TIMER_CREATE_FAILED, MUTEX_CREATE_FAILED or
/// OUT_OF_MEMORY.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_start( struct rocketnet_server_t * server );

/// Disconnects every client cleanly with the redundant disconnect packets,
/// frees every client slot, releases the transport and leaves the server
/// stopped; it can start again. Stopping a stopped server does nothing.

ROCKETNET_EXPORT void rocketnet_server_stop( struct rocketnet_server_t * server );

/// true while running, false while stopped.

ROCKETNET_EXPORT bool rocketnet_server_running( const struct rocketnet_server_t * server );

/// Destroys the server from either state. A running server is stopped first,
/// as rocketnet_server_stop does, so every connected client is sent the
/// redundant disconnect packets before the transport closes. Then the
/// server's memory, its keypair included, is wiped and freed, and every
/// pointer from the server is invalid after the call.

ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

/// Runs one update over every client at time. What it did is read from the
/// per-client calls (rocketnet_server_client_connected,
/// rocketnet_server_disconnect_reason) and the stats calls.

ROCKETNET_EXPORT void rocketnet_server_update( struct rocketnet_server_t * server, double time );

/// Sends this update's packets: each client's queued messages, under its
/// send budget.

ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

/// The port the server bound: the address's port, or the ephemeral port a
/// port of 0 was given; 0 while stopped.

ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

/// Copies the server's public key, ROCKETNET_SERVER_PUBLIC_KEY_BYTES, into
/// public_key: the key create generated for this server object, which the
/// game registers with its backend and passes to
/// rocketnet_create_dev_connect_token. Valid from create, running or stopped.

ROCKETNET_EXPORT void rocketnet_server_public_key( const struct rocketnet_server_t * server, uint8_t * public_key );

/// Writes the server's IPv4 address, as connect tokens name it
/// ("a.b.c.d:port", with the bound port), into address, address_bytes long;
/// ROCKETNET_MAX_ADDRESS_STRING_BYTES is always enough. Returns true when the
/// running server serves IPv4; false, with address an empty string, when it
/// does not, while stopped, or for a short buffer, which logs. An
/// address_bytes of 0 writes nothing and returns false.

ROCKETNET_EXPORT bool rocketnet_server_ipv4_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// As rocketnet_server_ipv4_address, for IPv6 ("[v6]:port").

ROCKETNET_EXPORT bool rocketnet_server_ipv6_address( const struct rocketnet_server_t * server, char * address, size_t address_bytes );

/// The transport that won the candidate list at start ("xdp", "gso",
/// "io_uring", "msgx", "iocp", "rio", "portable", "loopback"), or NULL while
/// stopped. The string lives for the process.

ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

/// The config's max_clients, running or stopped.

ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

/// The clients connected now; 0 while stopped.

ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

/// true when a client holds the client slot; false for a free client slot or
/// a stopped server.

ROCKETNET_EXPORT bool rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index );

/// The client id the connect token of the client in the client slot carries;
/// 0 for a free client slot or a stopped server. 0 is also a valid id a
/// backend may assign, so read rocketnet_server_client_connected first.

ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index );

/// Disconnects one client and frees its client slot; a free client slot or a
/// stopped server does nothing. Why is the game's decision.

ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

/// Disconnects every connected client as rocketnet_server_disconnect_client
/// does; the server keeps running.

ROCKETNET_EXPORT void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server );

/// The reason the last connection in the client slot ended, until the next
/// connection takes it; ROCKETNET_DISCONNECT_REASON_NONE while stopped.

ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index );

/// The name of a disconnect reason ("NONE", "RELIABLE_TIMEOUT"), or "UNKNOWN"
/// for a value that is not one, never NULL.

ROCKETNET_EXPORT const char * rocketnet_disconnect_reason_name( enum rocketnet_disconnect_reason_t reason );

/// As rocketnet_client_start_message, to the client at destination_id, a
/// client index as a uint64_t:
///
///     uint8_t * p = rocketnet_server_start_message( server, client_index,
///                                                   channel, reliability,
///                                                   max_bytes );
///     if ( p )
///     {
///         int bytes = write_world_state( p, max_bytes, &state );
///         serial = rocketnet_server_finish_message( server, bytes );
///     }
///
/// A destination_id with ROCKETNET_FLOW_ID_BIT set, or at or past
/// max_clients, is refused; the log names ROCKETNET_STATUS_FLOW_ID_RESERVED
/// for the bit. A send to a client index with no CONNECTED client, on a
/// running or a stopped server, is dropped as on the client and counted in
/// the server's not_connected_drops stat. One start message is pending per
/// server at a time, so finish message and abort message take no
/// destination. To send to every connected client, loop over the client
/// indexes.

ROCKETNET_EXPORT uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, enum rocketnet_reliability_t reliability, int bytes );

/// As rocketnet_client_finish_message, for the server's pending start
/// message.

ROCKETNET_EXPORT uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, int bytes_written );

/// Gives back the room of the pending start message; with none pending it
/// does nothing.

ROCKETNET_EXPORT void rocketnet_server_abort_message( struct rocketnet_server_t * server );

/// Refills the server's batch with the next messages from every client, each
/// naming its sender in client_index[], and returns it, or NULL when there
/// are none or the server is stopped: pump it until NULL.

ROCKETNET_EXPORT const struct rocketnet_message_batch_t * rocketnet_server_receive_messages( struct rocketnet_server_t * server );

/// As rocketnet_client_receipts, for the client in the client slot. A free
/// client slot or a stopped server returns 0.

ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity );

/// Sets the client's send budget from now on, and returns
/// ROCKETNET_STATUS_OK. 0 is the default rate,
/// ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS; a positive value below
/// ROCKETNET_SEND_KBPS_UNLIMITED caps the rate; and
/// ROCKETNET_SEND_KBPS_UNLIMITED is no send budget. A negative value returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT with the rate unchanged. The config's
/// per_client_send_kbps sets each connection's starting rate.

ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps );

/// Fills stats with the server's current stats.

ROCKETNET_EXPORT void rocketnet_server_stats( const struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats );

/// Fills stats with the current stats of the client in the client slot; a
/// free client slot or a stopped server zeroes it.

ROCKETNET_EXPORT void rocketnet_server_client_stats( const struct rocketnet_server_t * server, int client_index, struct rocketnet_server_client_stats_t * stats );

/// -----------------------------------------
///             Receiving messages
/// -----------------------------------------

/// The most messages one receive call fills.

#define ROCKETNET_RECEIVE_BATCH_MESSAGES                   65536

/// One batch of received messages, one array per field, entry i of every
/// array describing message i, in receive order.
///
/// Receiving is pumped: one queue per client, and one per server across all
/// its clients. Each client and each server holds one batch, made at create;
/// each receive refills it with the queue's next messages, up to
/// ROCKETNET_RECEIVE_BATCH_MESSAGES, and returns it, or NULL when it filled
/// nothing. Pump it until NULL. There is no per-update cap: what one call
/// does not return waits for the next call in the same update, in order.
/// Ordered and sequenced messages keep their order within their channel and
/// sender, and on a server the queue holds the update's messages client by
/// client in index order:
///
///     const struct rocketnet_message_batch_t * batch;
///     while ( ( batch = rocketnet_server_receive_messages( server ) ) )
///     {
///         for ( int i = 0; i < batch->num_messages; i++ )
///             handle( batch->client_index[i], batch->data[i],
///                     batch->bytes[i] );
///     }
///
/// The batch and its data pointers are valid until the next receive call or
/// the next update of the same client or server, whichever comes first;
/// there is no release call. Copy what is kept longer.

struct rocketnet_message_batch_t
{
    /// The entries filled.

    int num_messages;

    /// The sender: on a server, the sending client's client_index; on a
    /// client, 0, the server.

    int client_index[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The channel the message was delivered on: its ordering channel for an
    /// ordered or sequenced type, 0 for the types that carry none.

    int channel[ROCKETNET_RECEIVE_BATCH_MESSAGES];

    /// The reliability the message arrived as: a receipt type arrives as its
    /// twin without the receipt (UNRELIABLE, RELIABLE or RELIABLE_ORDERED),
    /// and an unreliable message larger than
    /// ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES as its reliable twin.

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
