/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

/// The public C API: a client and a server that exchange messages over UDP.
/// This header and a prebuilt RocketNet library are all a program needs: it
/// includes only standard C headers and declares only what a caller calls or
/// reads. Plain C99, callable from C++ through the extern "C" block below.
///
/// Main API calls must never block or wait; the Release-under-load target is
/// under 0.1 ms (100 microseconds) per call and generally not noticeable on a
/// profiler compared with the rest of the game. Client and server create calls
/// may be somewhat slower but must still be fast without a noticeable stall;
/// these are contract targets, with load-run measurement due after features land.
///
/// The API's shape: a call that can fail returns enum rocketnet_status_t,
/// except a create, which returns a pointer, NULL when refused; finish
/// message, which returns a uint32_t receipt serial, 0 when refused; start
/// message, rocketnet_client_transport_name and
/// rocketnet_server_transport_name, which return a pointer; the
/// required_memory calls, which return the byte count, 0 when refused;
/// rocketnet_abi_type_size, rocketnet_abi_field_offset and
/// rocketnet_abi_field_size, which return SIZE_MAX for an unknown type or
/// field; and rocketnet_abi_field_count, which returns -1 for an unknown type.
/// Counter reads return uint64_t and stat reads return double; update returns
/// enum rocketnet_update_result_t; a pure question returns bool; a call that
/// reports nothing returns void; every set of values is a typed enum named
/// ROCKETNET_<SET>_<VALUE>, and each lookup that names an enum value ends in
/// _name and returns "unknown" for a value outside its enum, never NULL.
///
/// Sizes: a caller memory block or buffer handed to a call is sized in size_t
/// (memory_bytes, the required_memory results, config_bytes,
/// connect_token_bytes); config fields, message bytes and counts are int;
/// client indexes and destination ids are uint64_t.
/// Pure lookups, ABI queries, and time calls are thread-safe; client and server instances are not.

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ROCKETNET_CONFIG_REVISION_BYTES( end, align ) ( ( ( end ) + ( align ) - 1 ) / ( align ) * ( align ) )

/// -----------------------------------------
///                     Version and ABI
/// -----------------------------------------

/// ROCKETNET_DEVELOPMENT is 1 when linking a development library
/// (rocketnet_dev): it carries the development token minting below and accepts
/// the published development keys.

#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 0
#endif /// #if !defined( ROCKETNET_DEVELOPMENT )

#if !ROCKETNET_DEVELOPMENT

    /// @brief Text form of the SDK version: the release version string, or "dev" in a development build.
    #define ROCKETNET_VERSION_FULL                               "1.0.0"
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
/// 10000 for 1.0.0. A development build is 2575755, above every release.
/// rocketnet_version returns the value the library was built with.
#define ROCKETNET_VERSION   ( ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH )

/// @brief Identifies the public C struct and calling-convention revision.
///
/// This revision is independent of the wire protocol version. Revision 1 is
/// the layout and calling contract of the first published API, this header.
/// From here on, increment it for an incompatible public layout or
/// calling-contract change; append-only additions that preserve old layouts
/// keep the revision unchanged.
#define ROCKETNET_ABI_VERSION 1

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

/// Destination ids are uint64_t. Today every destination is a client index,
/// 0 .. max_clients - 1. An id with ROCKETNET_FLOW_ID_BIT set is a flow id,
/// reserved for a future server-side flow feature (clients subscribe and
/// unsubscribe; the server sends to a flow by id with the same start message
/// and finish message), and never a client index. Until flows exist every function taking
/// a destination id refuses one with the bit set, and counts it.

#define ROCKETNET_FLOW_ID_BIT                                       ( 1ULL << 63 )

/// The in-memory loopback transport holds this many endpoints in one process:
/// enough for one server and ROCKETNET_MAX_CLIENTS clients. It is chosen only
/// by name ("loopback"), for tests and examples; auto never selects it.

#define ROCKETNET_LOOPBACK_MAX_ENDPOINTS          ( ROCKETNET_MAX_CLIENTS + 1 )

/// The packet size: the UDP payload of one datagram on a 1500-byte MTU link,
/// the number every other size limit derives from (STANDARD.md _Packet
/// sizes_). Over IPv4 it is 1500 - 20 - 8 = 1472 bytes, the receive ceiling;
/// over IPv6 it is 1500 - 40 - 8 = 1452 bytes, the send budget, so a maximum
/// packet crosses a 1500-MTU path unfragmented on either family.

#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 20 - 8 )
#define ROCKETNET_MAX_PACKET_BYTES_IPV6                ( 1500 - 40 - 8 )

/// The send-side payload capacity: the send budget (ROCKETNET_MAX_PACKET_BYTES_IPV6,
/// 1452) less the 26-byte fixed overhead and the 7-byte minimum ack block
/// leaves 1419; the capacity is the largest multiple of eight inside that,
/// 1416, so any word-based serializer can root at the payload pointer
/// directly. The receive side accepts payloads up to the receive ceiling
/// (ROCKETNET_MAX_PACKET_BYTES, 1472) less the same overhead, 1439 bytes,
/// whatever this end's own budget, so receive buffers size by the receive
/// constant.

#define ROCKETNET_MAX_PAYLOAD_BYTES                        1416    ///< ( ( 1452 - 26 - 7 ) / 8 ) * 8
#define ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES                1439    ///< 1472 - 26 - 7

/// Payloads above the wire capacity fragment transparently, and a lost
/// fragment loses the whole payload. The config's max_payload_bytes bounds
/// both what this end fragments and what it reassembles; storage for both is
/// allocated once at create, to the maximum. The hard ceiling is 256
/// fragments at the full stride: 305,408 bytes.

/// @brief Default maximum payload in bytes.
#define ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES                16384
/// @brief Default number of simultaneous fragmented-payload reassembly slots per connection.
#define ROCKETNET_DEFAULT_REASSEMBLY_SLOTS                 8

/// @brief Required connect-token buffer size in bytes.
#define ROCKETNET_CONNECT_TOKEN_BYTES                      768
/// @brief Application-defined connect-token user data size in bytes.
#define ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES            256
/// @brief Backend signing public key size in bytes.
#define ROCKETNET_BACKEND_PUBLIC_KEY_BYTES                  32
/// @brief Backend signing secret key size in bytes.
#define ROCKETNET_BACKEND_SECRET_KEY_BYTES                  64
/// @brief Backend signing seed size in bytes.
#define ROCKETNET_BACKEND_SEED_BYTES                        32
/// @brief Server sealing public key size in bytes.
#define ROCKETNET_SERVER_PUBLIC_KEY_BYTES                   32
/// @brief Server sealing secret key size in bytes.
#define ROCKETNET_SERVER_SECRET_KEY_BYTES                   32

/// @brief Alignment in bytes that a caller memory block for a client or a server needs, and that SDK allocations return.
#define ROCKETNET_ALLOCATION_ALIGNMENT                     64

/// Messages (docs/CHANNELS.md): the channel bound

/// The internal wide-channel test builds the whole SDK with a matching override.
/// Consumers of the prebuilt library always use its fixed channel bound.
#if !defined( ROCKETNET_INTERNAL_WIDE_BUILD )
#undef ROCKETNET_MAX_CHANNELS
#endif
#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                                         32
#endif

/// The largest message one packet carries, written in place with zero copies:
/// the payload capacity less the one form byte. Unreliable messages above
/// the 1411-byte in-place limit promote to reliable.

#define ROCKETNET_MESSAGE_SINGLE_BYTES          ( ROCKETNET_MAX_PAYLOAD_BYTES - 1 )      ///< 1415; above the 1411 in-place limit

/// Send budgets are kilobits per second of wire bytes (1 kbps = 1000 bit/s),
/// per connection: the defaults of the config's send_kbps and
/// per_client_send_kbps, each connection's starting rate.

/// @brief Default client send budget in decimal kilobits per second per connection.
#define ROCKETNET_DEFAULT_CLIENT_SEND_KBPS                  1000

/// @brief Default server send budget in decimal kilobits per second for each client.
#define ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS       10000

/// @brief Maximum supported send budget in decimal kilobits per second per connection.
#define ROCKETNET_SEND_BUDGET_MAX_KBPS                      10000000    ///< 10 Gbit/s per connection

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable status values returned by a call that can fail, except a create
/// (a pointer, NULL when refused), finish message (a uint32_t receipt serial, 0
/// when refused), start message and the transport_name calls (pointers), the
/// required_memory calls (the byte count, 0 when refused),
/// rocketnet_abi_type_size, rocketnet_abi_field_offset and
/// rocketnet_abi_field_size (SIZE_MAX for an unknown type or field), and
/// rocketnet_abi_field_count (-1 for an unknown type).
///
/// OK is the sole success value, every failure is a positive code, and the
/// numbering is append-only. A published value and meaning never change; a new
/// code appends immediately before ROCKETNET_STATUS_COUNT.
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,   ///< Success: the operation succeeds

    /// Lifecycle and allocator state (rocketnet_init, rocketnet_term, rocketnet_set_allocator)
    ROCKETNET_STATUS_ALREADY_INITIALIZED                = 1,   ///< Initialization is complete or transitioning: rocketnet_init, or an allocator change, after the library was initialized
    ROCKETNET_STATUS_NOT_INITIALIZED                    = 2,   ///< The operation needs an initialized library: a call that needs rocketnet_init came before it, or a term with nothing to terminate
    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = 3,   ///< Term or an allocator change refused: the library still holds allocations
    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = 4,   ///< The platform's socket layer did not start; correct the platform setup before retrying
    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = 5,   ///< The crypto library did not start; initialization does not complete

    /// Caller arguments and configuration (client, server, message)
    ROCKETNET_STATUS_INVALID_ARGUMENT                   = 6,   ///< An argument, or a combination of arguments, is NULL, out of range, or inconsistent with another
    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = 7,   ///< An environment override is malformed: it did not parse
    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = 8,   ///< A config field, from the struct or the environment, is out of its supported range
    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = 9,   ///< The memory the config needs is past the operator's memory budget
    ROCKETNET_STATUS_MISSING_KEYS                       = 10,   ///< Required key material is absent: a server config has no keys
    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = 11,   ///< The protocol id is 0, a development id in a release build, or not the expected value
    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = 12,   ///< The published development keys in a release build
    ROCKETNET_STATUS_INVALID_ADDRESS                    = 13,   ///< An address string did not parse or cannot be used
    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = 14,   ///< The transport candidate list did not parse, or holds no usable candidate

    /// Client connect token pre-send checks
    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = 15,   ///< The connect token fails the client's pre-send checks: it did not parse, or is not for this protocol
    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = 16,   ///< The connect token expired before the first packet was sent; obtain a fresh token before connecting

    /// Transport candidate list and OS resources
    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = 17,   ///< A pinned transport cannot take port 0 (an ephemeral port)
    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = 18,   ///< Every transport in the candidate list refused to start
    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,   ///< An allocation the operation needs failed
    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,   ///< The platform refused a mutex
    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,   ///< The platform refused a thread
    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,   ///< The platform refused a timer

    /// Object lifecycle (rocketnet_client_connect, rocketnet_server_start) and destinations
    ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED            = 23,   ///< Connect refused: the client is not DISCONNECTED; call rocketnet_client_disconnect first; nothing changed
    ROCKETNET_STATUS_SERVER_NOT_STOPPED                 = 24,   ///< Start refused: the server is already running; nothing changed
    ROCKETNET_STATUS_FLOW_ID_RESERVED                   = 25,   ///< A destination id has ROCKETNET_FLOW_ID_BIT set: flow ids are reserved and refused until flows exist
    ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED     = 26,   ///< A transport cannot serve both named families (two interfaces, or a platform that cannot clear IPV6_V6ONLY)

    ROCKETNET_STATUS_COUNT                              = 27     ///< Sentinel: the count of codes, OK included; not an operation result
};

/// -----------------------------------------
///                          Log levels
/// -----------------------------------------

/// @brief The process-wide log levels, least to most verbose.
enum rocketnet_log_level_t
{
    ROCKETNET_LOG_LEVEL_NONE                            = 0,   ///< As the maximum, only level-NONE lines; quiet mode suppresses every line
    ROCKETNET_LOG_LEVEL_ERROR                           = 1,   ///< Errors, the least verbose emitted level
    ROCKETNET_LOG_LEVEL_WARN                            = 2,   ///< Warnings
    ROCKETNET_LOG_LEVEL_INFO                            = 3,   ///< Information
    ROCKETNET_LOG_LEVEL_DEBUG                           = 4,   ///< Debugging
    ROCKETNET_LOG_LEVEL_SPAM                            = 5    ///< The most verbose level
};

/// -----------------------------------------
///                  Address types
/// -----------------------------------------

/// @brief Address family types for endpoints.
enum rocketnet_address_type_t
{
    ROCKETNET_ADDRESS_NONE                              = 0,   ///< No address family
    ROCKETNET_ADDRESS_IPV4                              = 1,   ///< IPv4 address family
    ROCKETNET_ADDRESS_IPV6                              = 2    ///< IPv6 address family
};

/// -----------------------------------------
///                    Message enums
/// -----------------------------------------

/// @brief Per-send reliability, RakNet's values and numbering (docs/CHANNELS.md).
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

/// @brief Why a connection ended: what rocketnet_client_disconnect_reason and
/// rocketnet_server_disconnect_reason return (docs/CHANNELS.md 12).
///
/// MESSAGE_MEMORY_EXHAUSTED: the connection's region had no room for an
/// operation it must not drop (docs/CHANNELS.md 4). A server disconnects that
/// client's slot with this reason and resets the slot's region; a client
/// disconnects itself. Value 2 is reserved and is not a reason.
enum rocketnet_disconnect_reason_t
{
    ROCKETNET_DISCONNECT_REASON_NONE                            = 0,   ///< No connection has ended, or the connection is up
    ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH                 = 1,   ///< The two ends' WIRE config fields differ
    // 2 is reserved and is not a reason.
    ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION              = 3,   ///< The peer sent what the protocol forbids
    ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT                = 4,   ///< A reliable message went unacknowledged past reliable_timeout_ms
    ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED        = 5,   ///< The connection's region had no room for an operation it must not drop
    ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED             = 6    ///< The client accepted the server's disconnect: the server disconnected the client, stopped, or was destroyed
};

/// @brief What rocketnet_client_update and rocketnet_server_update return.
enum rocketnet_update_result_t
{
    ROCKETNET_UPDATE_OK                                         =  0,  ///< The update ran
    ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED                       = -1   ///< Refused: a received message view is still held
};

/// -----------------------------------------
///                        Client state
/// -----------------------------------------

/// What rocketnet_client_state returns. A connect attempt runs up from
/// DISCONNECTED through the three handshake states to CONNECTED. A negative
/// state is how the last attempt or connection ended, kept until
/// rocketnet_client_disconnect, which returns the client to DISCONNECTED so it
/// can connect again. INVALID is no client state: it is what a NULL client
/// reads as.

enum rocketnet_client_state_t
{
    ROCKETNET_CLIENT_STATE_INVALID                          = -100, ///< A NULL client: not a state any client is in
    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            = -7,   ///< The connect token expired before the server accepted the client
    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            = -6,   ///< The connect token was refused before the first packet was sent
    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             = -5,   ///< A connection heard nothing from the server for the token's timeout
    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    = -4,   ///< The server never answered the third leg of the handshake
    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     = -3,   ///< The server never answered the connection request
    ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT         = -2,   ///< The server never answered the cookie request
    ROCKETNET_CLIENT_STATE_CONNECTION_DENIED                = -1,   ///< The server refused the client
    ROCKETNET_CLIENT_STATE_DISCONNECTED                     =  0,   ///< After create, and after rocketnet_client_disconnect
    ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE                =  1,   ///< Handshake, first leg
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST       =  2,   ///< Handshake, second leg
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE      =  3,   ///< Handshake, third leg
    ROCKETNET_CLIENT_STATE_CONNECTED                        =  4    ///< Connected: messages flow both ways
};

/// -----------------------------------------
///                     Query enums
/// -----------------------------------------

/// What a client counts, read with rocketnet_client_counter. Every value is
/// monotonic over the client's life, across connections, and starts at 0 at
/// create.

enum rocketnet_client_counter_t
{
    ROCKETNET_CLIENT_COUNTER_PACKETS_SENT,          ///< Packets: UDP datagrams sent, handshake included
    ROCKETNET_CLIENT_COUNTER_PACKETS_RECEIVED,      ///< Packets: UDP datagrams received, handshake included
    ROCKETNET_CLIENT_COUNTER_BYTES_SENT,            ///< Bytes: UDP payload plus the IP and UDP headers of the peer's family
    ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED,        ///< Bytes: UDP payload plus the IP and UDP headers of the peer's family
    ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED,     ///< Payloads: packets from the server that opened and carried messages
    ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT,       ///< Records: reliable message records resent on the retransmit timer
    ROCKETNET_CLIENT_COUNTER_MESSAGES_DUPLICATE,    ///< Messages: reliable messages received a second time and dropped
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED,        ///< Receipts: sends with a receipt that the server acknowledged
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST,         ///< Receipts: sends with a receipt that were lost
    ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS,          ///< Messages: unreliable messages dropped because the send budget was spent
    ROCKETNET_CLIENT_COUNTER_SEND_DEFERRED,         ///< Hand-offs: sends that left with the next hand-off to the send thread
    ROCKETNET_CLIENT_COUNTER_MESSAGES_NOT_RELEASED, ///< Messages: received messages still held when an update was refused
    ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS,   ///< Messages: sends dropped because the connection was not CONNECTED
    ROCKETNET_CLIENT_COUNTER_COUNT
};

/// The client's connection quality, read with rocketnet_client_stat. Rates
/// cover the trailing second.
///
/// The loss stat counts gaps in the server's session sequence. The server
/// assigns a sequence only after finish accepts the payload and reserves every
/// wire frame, so an aborted start or refused start or finish creates no gap;
/// a gap after a completed send reflects path loss.

enum rocketnet_client_stat_t
{
    ROCKETNET_CLIENT_STAT_RTT,                      ///< Milliseconds: round trip time
    ROCKETNET_CLIENT_STAT_JITTER,                   ///< Milliseconds: inter-arrival jitter
    ROCKETNET_CLIENT_STAT_PACKET_LOSS,              ///< Percent: received-side loss, from gaps in the server's packet sequence
    ROCKETNET_CLIENT_STAT_PACKETS_SENT_PER_SECOND,  ///< Packets per second sent
    ROCKETNET_CLIENT_STAT_PACKETS_RECEIVED_PER_SECOND, ///< Packets per second received
    ROCKETNET_CLIENT_STAT_BANDWIDTH_SENT,           ///< Kbps (1000 bit/s) sent
    ROCKETNET_CLIENT_STAT_BANDWIDTH_RECEIVED,       ///< Kbps (1000 bit/s) received
    ROCKETNET_CLIENT_STAT_COUNT
};

/// What a server counts over all its clients, read with
/// rocketnet_server_counter. Every value is monotonic over the server's life,
/// across starts and stops, and starts at 0 at create.

enum rocketnet_server_counter_t
{
    ROCKETNET_SERVER_COUNTER_PACKETS_SENT,          ///< Packets: UDP datagrams sent
    ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED,      ///< Packets: UDP datagrams received
    ROCKETNET_SERVER_COUNTER_BYTES_SENT,            ///< Bytes: UDP payload plus the IP and UDP headers of the peer's family
    ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED,        ///< Bytes: UDP payload plus the IP and UDP headers of the peer's family
    ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED,     ///< Connections: clients that completed the handshake
    ROCKETNET_SERVER_COUNTER_CLIENTS_DENIED_FULL,   ///< Requests: connect attempts refused because every client slot was taken
    ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET,///< Requests: connect attempts dropped by the per-update handshake budget
    ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED,     ///< Payloads: packets from clients that opened and carried messages
    ROCKETNET_SERVER_COUNTER_MESSAGES_RESENT,       ///< Records: reliable message records resent on the retransmit timer
    ROCKETNET_SERVER_COUNTER_MESSAGES_DUPLICATE,    ///< Messages: reliable messages received a second time and dropped
    ROCKETNET_SERVER_COUNTER_RECEIPTS_ACKED,        ///< Receipts: sends with a receipt that a client acknowledged
    ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST,         ///< Receipts: sends with a receipt that were lost
    ROCKETNET_SERVER_COUNTER_BUDGET_DROPS,          ///< Messages: unreliable messages dropped because a client's send budget was spent
    ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED, ///< Messages: received messages still held when an update was refused
    ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS,   ///< Messages: sends dropped because the connection was not CONNECTED
    ROCKETNET_SERVER_COUNTER_COUNT
};

/// The server's whole-socket rates over the trailing second, read with
/// rocketnet_server_stat.

enum rocketnet_server_stat_t
{
    ROCKETNET_SERVER_STAT_PACKETS_SENT_PER_SECOND,  ///< Packets per second sent
    ROCKETNET_SERVER_STAT_PACKETS_RECEIVED_PER_SECOND, ///< Packets per second received
    ROCKETNET_SERVER_STAT_BANDWIDTH_SENT,           ///< Kbps (1000 bit/s) sent
    ROCKETNET_SERVER_STAT_BANDWIDTH_RECEIVED,       ///< Kbps (1000 bit/s) received
    ROCKETNET_SERVER_STAT_COUNT
};

/// What a server counts for one client slot, read with
/// rocketnet_server_client_counter. The values cover the slot's current
/// connection and go back to 0 when a new connection takes the slot.

enum rocketnet_server_client_counter_t
{
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT,       ///< Packets: UDP datagrams sent to this client
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_RECEIVED,   ///< Packets: UDP datagrams received from this client
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT,         ///< Bytes: UDP payload plus the IP and UDP headers of this client's family
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED,     ///< Bytes: UDP payload plus the IP and UDP headers of this client's family
    ROCKETNET_SERVER_CLIENT_COUNTER_COUNT
};

/// One client slot's connection quality, read with
/// rocketnet_server_client_stat. Rates cover the trailing second.

enum rocketnet_server_client_stat_t
{
    ROCKETNET_SERVER_CLIENT_STAT_RTT,                   ///< Milliseconds: round trip time
    ROCKETNET_SERVER_CLIENT_STAT_JITTER,                ///< Milliseconds: deviation of rtt samples above the minimum
    ROCKETNET_SERVER_CLIENT_STAT_PACKET_LOSS,           ///< Percent: packets sent to this client that were never acknowledged
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_SENT,        ///< Kbps (1000 bit/s) sent to this client
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_RECEIVED,    ///< Kbps (1000 bit/s) received from this client
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_ACKED,       ///< Kbps (1000 bit/s) of sent packets this client acknowledged
    ROCKETNET_SERVER_CLIENT_STAT_COUNT
};

// The library and its callers are built without -fshort-enums. Each public
// enum is the size of int. A build that shortens one fails here.
struct rocketnet_public_enums_are_int
{
    char status[sizeof(enum rocketnet_status_t) == sizeof(int) ? 1 : -1];
    char log_level[sizeof(enum rocketnet_log_level_t) == sizeof(int) ? 1 : -1];
    char reliability[sizeof(enum rocketnet_reliability_t) == sizeof(int) ? 1 : -1];
    char receipt_status[sizeof(enum rocketnet_receipt_status_t) == sizeof(int) ? 1 : -1];
    char disconnect_reason[sizeof(enum rocketnet_disconnect_reason_t) == sizeof(int) ? 1 : -1];
    char update_result[sizeof(enum rocketnet_update_result_t) == sizeof(int) ? 1 : -1];
    char client_state[sizeof(enum rocketnet_client_state_t) == sizeof(int) ? 1 : -1];
    char client_counter[sizeof(enum rocketnet_client_counter_t) == sizeof(int) ? 1 : -1];
    char client_stat[sizeof(enum rocketnet_client_stat_t) == sizeof(int) ? 1 : -1];
    char server_counter[sizeof(enum rocketnet_server_counter_t) == sizeof(int) ? 1 : -1];
    char server_stat[sizeof(enum rocketnet_server_stat_t) == sizeof(int) ? 1 : -1];
    char server_client_counter[sizeof(enum rocketnet_server_client_counter_t) == sizeof(int) ? 1 : -1];
    char server_client_stat[sizeof(enum rocketnet_server_client_stat_t) == sizeof(int) ? 1 : -1];
};

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

/// The ABI queries are safe before init, allocate nothing and write no
/// caller-owned struct. Type names are the C names of the types described below. Unknown
/// types/fields return SIZE_MAX for size and offset, and -1 for field count.
/// They describe the plain structs in this header (the two configs and the
/// receipt), the address type enum, and the internal transport/layer structs
/// the sized-config paths resolve; not the opaque client and server.

/// @brief Returns the SDK ABI revision. @return ROCKETNET_ABI_VERSION; safe before init and allocation-free.
ROCKETNET_EXPORT uint32_t rocketnet_abi_version( void );

/// @brief Returns the byte size of a type known to the ABI table.
///
/// This query is allocation-free and safe before initialization. It describes
/// the two public configs, receipt, address type enum, and internal
/// transport/layer structs described above, not the opaque handles.
///
/// @param type NUL-terminated C type name known to the ABI table; NULL is a counted no-op.
/// @return The type size, or SIZE_MAX for NULL or an unknown name.
ROCKETNET_EXPORT size_t rocketnet_abi_type_size( const char * type );

/// @brief Returns the number of ABI-described fields in a known type.
/// @param type NUL-terminated C type name known to the ABI table; NULL is a counted no-op.
/// @return The field count (zero for the address type enum), or -1 for NULL or an unknown name.
ROCKETNET_EXPORT int rocketnet_abi_field_count( const char * type );

/// @brief Returns the byte offset of a named field in a known type.
/// @param type NUL-terminated C type name known to the ABI table; NULL is a counted no-op.
/// @param field NUL-terminated field name; NULL is a counted no-op.
/// @return The field offset in bytes, or SIZE_MAX if either name is NULL or the pair is unknown.
ROCKETNET_EXPORT size_t rocketnet_abi_field_offset( const char * type, const char * field );

/// @brief Returns the byte size of a named field in a known type.
/// @param type NUL-terminated C type name known to the ABI table; NULL is a counted no-op.
/// @param field NUL-terminated field name; NULL is a counted no-op.
/// @return The field size in bytes, or SIZE_MAX if either name is NULL or the pair is unknown.
ROCKETNET_EXPORT size_t rocketnet_abi_field_size( const char * type, const char * field );

/// @brief Initializes the library and its platform and cryptography state.
/// @param context Opaque allocator context passed to allocation callbacks; NULL is the default.
/// @return ROCKETNET_STATUS_OK; ROCKETNET_STATUS_ALREADY_INITIALIZED when the
/// library is initialized or transitioning; ROCKETNET_STATUS_PLATFORM_INIT_FAILED
/// or ROCKETNET_STATUS_CRYPTO_INIT_FAILED when that layer does not start.
/// Call before creating a client or a server; do not race an init or term transition.
/// Environment: ROCKETNET_LOG_LEVEL overrides the maximum emitted log level before init logs; 0 .. 5 (ROCKETNET_LOG_LEVEL_NONE .. ROCKETNET_LOG_LEVEL_SPAM); a value outside that range is ignored with a warning (sdk/rocketnet.cpp:715).
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

/// @brief Returns the total number of status codes, including OK. @return Count of defined values; allocation-free and safe before init.
ROCKETNET_EXPORT int rocketnet_num_status_codes( void );

/// @brief Returns the stable symbolic name of a status code. @param status Status to look up. @return Static name without the ROCKETNET_STATUS_ prefix ("OK", "INVALID_ARGUMENT"), or "unknown" for a value that is not a code, never NULL. Allocation-free and safe before init; returned storage is process-lifetime.
ROCKETNET_EXPORT const char * rocketnet_status_name( enum rocketnet_status_t status );

/// @brief Receives a failed internal assert: the borrowed condition, function
/// and file strings, and the source line that failed. It runs synchronously on
/// the thread that fires the assertion.
typedef void (*rocketnet_assert_callback_t)( const char * condition, const char * function, const char * file, int line );

/// @brief Installs the process-wide callback for a failed internal assert.
///
/// This setter is the only way to install it; no writable global is public.
/// The library calls the callback when it is built with asserts enabled (a
/// debug build); with asserts disabled it is never invoked. The default
/// callback logs and traps. Pass NULL to restore the default. Install it
/// before concurrent SDK calls, since the library reads it unsynchronized.
///
/// @param callback Callback to install, or NULL to restore the default.
ROCKETNET_EXPORT void rocketnet_set_assert_callback( rocketnet_assert_callback_t callback );

/// @brief Suppresses or resumes all RocketNet log output.
///
/// Quiet state is process-wide and atomically read by logging calls. It does not
/// change the configured log level.
/// @param flag true suppresses output; false resumes output subject to the log level.
ROCKETNET_EXPORT void rocketnet_set_quiet( bool flag );

/// @brief Sets the process-wide maximum log level.
/// @param level ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM; a value outside the enum is ignored.
ROCKETNET_EXPORT void rocketnet_set_log_level( enum rocketnet_log_level_t level );

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
/// already running keeps what it got. docs/CLIENT-IO.md and DRIVERS.md list the
/// threads.
/// @param enabled true (the default) makes the calls; false makes none.
ROCKETNET_EXPORT void rocketnet_set_thread_priority( bool enabled );

/// @brief Reads whether the library's network-thread priority calls are on.
/// @return true (the default) when on; false after rocketnet_set_thread_priority( false ).
ROCKETNET_EXPORT bool rocketnet_get_thread_priority( void );

/// @brief Counts the priority, affinity and timer period calls the operating system refused.
///
/// Process-wide, since the process started. A refusal never fails a create or
/// rocketnet_init: the thread runs at its default priority. The first refusal
/// of each call kind logs one warning (docs/ERRORS-AND-LOGGING.md).
/// @return The number of refused calls.
ROCKETNET_EXPORT uint64_t rocketnet_get_thread_priority_refusals( void );

/// @brief Reads the current process-wide log level.
/// The level currently in force, so a caller that moves the level for a bounded
/// stretch can restore what it found rather than guessing. This relaxed atomic
/// read is diagnostic only; it is not a synchronization operation.
/// @return A value from ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM.
ROCKETNET_EXPORT enum rocketnet_log_level_t rocketnet_get_log_level( void );

/// @brief Returns the display name for a log level.
/// A log callback uses it to render a level the way the default sink does. The
/// returned string is static and remains valid for the process lifetime.
/// @param level A log level.
/// @return "none", "error", "warn", "info", "debug" or "spam"; "unknown" for a value outside the enum, never NULL.
ROCKETNET_EXPORT const char * rocketnet_log_level_name( enum rocketnet_log_level_t level );

/// @brief Receives one preformatted log message: the one log sink.
///
/// The callback runs synchronously on the logging thread. The message is
/// NUL-terminated (message[length] is always NUL), length is clamped to at most
/// 1023 bytes, and the pointer borrows the logging frame's stack buffer, valid
/// only until the callback returns. The callback must be thread-safe and must
/// not re-enter rocketnet_set_log_callback.
/// @param context Caller-owned pointer supplied when the callback is installed.
/// @param level The level passed by the logging call.
/// @param message Borrowed formatted bytes; valid only during this callback.
/// @param length Number of message bytes before the NUL terminator, at most 1023.
typedef void (*rocketnet_log_callback_t)( void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

/// @brief Installs the log callback, the one log sink.
///
/// Passing NULL restores the default sink, which prints to stdout, and clears
/// its context. Calls already in flight may finish on their previously captured
/// callback; callbacks must be thread-safe.
/// @param callback Callback to install, or NULL to restore the default sink.
/// @param context Caller-owned pointer passed back to callback; keep it valid until any in-flight callback using it returns.
ROCKETNET_EXPORT void rocketnet_set_log_callback( rocketnet_log_callback_t callback, void * context );

/// @brief Reads the process-wide count of API misuses since the process started.
/// It counts exactly these: a NULL argument a call refuses (a NULL client's
/// state included); a create before rocketnet_init; a refused
/// rocketnet_set_allocator; an update time that is not finite or goes
/// backwards; a release of a pointer that is not a held view, a release on a
/// client with no connection, or a server release to an index at or past
/// max_clients or on a stopped server; an in-place start message still open at
/// update or send_packets; a connect from a state other than DISCONNECTED; a
/// server start while running, and a server update or send_packets while
/// stopped; a server start message or finish message to an index at or past
/// max_clients; and a destination id with ROCKETNET_FLOW_ID_BIT set. A send
/// refused for its own arguments (a second start message while one is pending,
/// a finish message whose bytes_written is out of range, a channel or
/// reliability out of range) is not counted here: it returns NULL or 0 and
/// logs. A send on a connection that is not CONNECTED (a client in any other
/// state, a server index below max_clients with no CONNECTED client, a stopped
/// server) is not counted here either: it is a normal drop, counted in
/// ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS or
/// ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS.
/// @return Relaxed atomic diagnostic count; this read does not synchronize SDK work.
ROCKETNET_EXPORT uint64_t rocketnet_api_misuse_count( void );

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
/// line and counts the refusal in rocketnet_api_misuse_count. Callbacks must be
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
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_set_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );


/// -----------------------------------------
///                             Config
/// -----------------------------------------

/// The client and the server each take one plain config struct. Fill it with
/// rocketnet_client_default_config or rocketnet_server_default_config, set
/// what the program means, and pass it to create. Precedence at create is
/// DEFAULT < CONFIG STRUCT < ENVIRONMENT: every field except a callback has an
/// environment variable, named next to the field below, so an operator can
/// tune an already-built binary with no rebuild. Every override is logged at
/// create with the variable and the value it displaced (the secret key logs
/// redacted). A value that does not parse, or is out of range, refuses the
/// create with one error line; nothing is ever clamped. An environment
/// variable set to the empty string is ignored, with a log line.
///
/// Env encodings: integers are decimal; byte fields are base64 of exactly the
/// field's length (keygen prints keys in base64); strings are taken as they
/// are.
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
/// connection verifies them. Validation applies the ranges below and the cross
/// rules of docs/CHANNELS.md section 14.
///
/// Create copies every string it is given, so the strings need to live only
/// for the call.
///
/// Sized configs (docs/ABI-EVOLUTION.md): the _sized entry points take
/// sizeof the struct the caller was compiled against, so a struct can grow at
/// its end without breaking a program built against an older header. Each
/// public client and server config has one revision, the whole struct
/// (ROCKETNET_CLIENT_CONFIG_BYTES_1 and ROCKETNET_SERVER_CONFIG_BYTES_1);
/// the unsized entry points read and write all of it. An unknown size is
/// refused with a log line and nothing is read or written
/// (rn_public_config_size_known in sdk/rocketnet_api.cpp).
///
/// The transport environment passes, which fill the transport config the
/// public struct does not expose, read these variables beside the fields the
/// public structs name below:
///
/// Environment: ROCKETNET_CLIENT_GAME_CLOCK_EPOCH sets game_clock_epoch, unix seconds at the game clock's zero for the token check; 0 reads the wall clock (sdk/rocketnet_internal.h:432).
/// Environment: ROCKETNET_SERVER_GAME_CLOCK_EPOCH sets game_clock_epoch, unix seconds at the game clock's zero for the token check; 0 reads the wall clock (sdk/rocketnet_internal.h:509).
/// Environment: ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH sets simulated_drop_every_nth, development only: drop every Nth outgoing datagram, 2 .. 1000000; 0 is off (sdk/rocketnet_internal.h:441).
/// Environment: ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH sets simulated_drop_every_nth, development only: drop every Nth outgoing datagram, 2 .. 1000000; 0 is off (sdk/rocketnet_internal.h:514).
/// Environment: ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW sets simulated_reverse_window, development only: reverse each window of W outgoing datagrams, 2 .. 64; 0 is off (sdk/rocketnet_internal.h:442).
/// Environment: ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW sets simulated_reverse_window, development only: reverse each window of W outgoing datagrams, 2 .. 64; 0 is off (sdk/rocketnet_internal.h:515).
/// Environment: ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES sets the operator memory budget, allocator bytes the client's pre-allocation plan must fit; uint64, 0 is no budget (sdk/rocketnet_client_transport.cpp:653).
/// Environment: ROCKETNET_SERVER_MEMORY_BUDGET_BYTES sets the operator memory budget, allocator bytes a server candidate's pre-allocation plan must fit; uint64, 0 is no budget (sdk/rocketnet_server_transport.cpp:1082).
///
/// The platform passes read these process-wide variables, which are not
/// config fields:
///
/// Environment: ROCKETNET_PUMP=portable forces the portable datagram pump and refuses the msgx pump; any other value allows the batch pump decision.
/// Environment: ROCKETNET_GATEWAY_ETHERNET_ADDRESS overrides gateway MAC discovery, xx:xx:xx:xx:xx:xx (linux); a malformed value refuses the socket (sdk/rocketnet_server_socket_linux.cpp:488).

struct rocketnet_client_config_t
{
    uint8_t filter_value[4];                        ///< ROCKETNET_CLIENT_FILTER_VALUE: the per-title packet filter, as the server's
    int max_payload_bytes;                          ///< ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    int num_reassembly_slots;                       ///< ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS: 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    bool step_driven;                               ///< ROCKETNET_CLIENT_STEP_DRIVEN: true (env 1) runs the client I/O step inside update and send_packets on the caller's thread, with no I/O thread; false (env 0, the default) keeps the I/O thread
    const char * transport;                         ///< ROCKETNET_CLIENT_TRANSPORT: the candidate list; NULL is "auto"
    void (*state_change_callback)( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state );   ///< No env form; NULL is none. Fires inside disconnect and destroy and must not re-enter the client.
    double (*time_callback)( void * context );      ///< No env form: the receive stamp clock, read once at create; NULL is the platform clock. Runs on the I/O thread.
    int send_kbps;                                  ///< ROCKETNET_CLIENT_SEND_KBPS: the send budget, kbps; default 1000; the starting rate of each connection; 0 or less sets the budget to its ceiling, ROCKETNET_SEND_BUDGET_MAX_KBPS (10 Gbit/s), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused
    int num_channels;                               ///< ROCKETNET_CLIENT_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    int window_messages;                            ///< ROCKETNET_CLIENT_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    int receive_ring_bytes;                         ///< ROCKETNET_CLIENT_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    int max_message_bytes;                          ///< ROCKETNET_CLIENT_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    int send_buffer_bytes;                          ///< ROCKETNET_CLIENT_SEND_BUFFER_BYTES: queued message bytes; 12288 .. 1 << 28; default 800000
    int send_queue_messages;                        ///< ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES: queued messages; 16 .. 1 << 20; default 1024
    int packets_in_flight;                          ///< ROCKETNET_CLIENT_PACKETS_IN_FLIGHT: unacknowledged packets tracked; 16 .. 1 << 16; default 256
    int max_packets_per_update;                     ///< ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE: packets sent per update, in-place unreliable finish messages and send_packets together; 1 .. 1024; default 256; the send budget is the throttle
    int unreliable_deliveries;                      ///< ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per update; 1 .. 1 << 16; default 256
    int sequenced_holds;                            ///< ROCKETNET_CLIENT_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    /// @brief Maximum queued client send receipts, 16 through 65536 (default 256).
    /// A full ring drops its oldest receipt; ROCKETNET_CLIENT_RECEIPT_RING can override it.
    int receipt_ring;                               ///< ROCKETNET_CLIENT_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    int rto_initial_ms;                             ///< ROCKETNET_CLIENT_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 2000
    int rto_min_ms;                                 ///< ROCKETNET_CLIENT_RTO_MIN_MS: optional sampled RTO floor in milliseconds; 0 disables it; 0 .. 60000; default 0
    int rto_max_ms;                                 ///< ROCKETNET_CLIENT_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 2000
    int reliable_timeout_ms;                        ///< ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
    /// Simulated link conditions: development only. A release library refuses nonzero simulated_* fields with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE.
    int simulated_loss_percent;                     ///< ROCKETNET_CLIENT_SIMULATED_LOSS_PERCENT: development only: outgoing loss 0..100; 0 is off (sdk/rocketnet_internal.h:435)
    int simulated_latency_ms;                       ///< ROCKETNET_CLIENT_SIMULATED_LATENCY_MS: development only: outgoing latency 0..60000 milliseconds; 0 is off (sdk/rocketnet_internal.h:436)
    int simulated_jitter_ms;                        ///< ROCKETNET_CLIENT_SIMULATED_JITTER_MS: development only: outgoing jitter 0..60000 milliseconds; 0 is off (sdk/rocketnet_internal.h:437)
    int simulated_duplicate_percent;                ///< ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT: development only: outgoing duplication 0..100; 0 is off (sdk/rocketnet_internal.h:438)
};

/// Revision 1, the one revision: the whole struct, through simulated_duplicate_percent.
#define ROCKETNET_CLIENT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( void * ) )

/// The server is configured explicitly: rocketnet_server_default_config gives
/// the shape with no keys, and start refuses it until keys are supplied
/// (through the struct or the environment), because default keys are how
/// every deployment of a protocol ends up sharing one identity.
/// rocketnet_server_dev_config fills the published development keys and test
/// filter for local work, loudly.
///
/// public_address, when set, displaces the create call's address at start, so the
/// address the transport binds and the address connect tokens are checked
/// against are always the same one. ROCKETNET_SERVER_PUBLIC_ADDRESS and
/// ROCKETNET_SERVER_SECOND_PUBLIC_ADDRESS are consumed together by the first
/// server start in the process, and say so in the log, so a process with several
/// servers is never redirected wholesale by ambient address variables.

struct rocketnet_server_config_t
{
    uint64_t protocol_id;                           ///< ROCKETNET_SERVER_PROTOCOL_ID: the title's protocol id; connect tokens must carry it
    int max_clients;                                ///< ROCKETNET_SERVER_MAX_CLIENTS: 1 .. ROCKETNET_MAX_CLIENTS; default 1000
    uint8_t filter_value[4];                        ///< ROCKETNET_SERVER_FILTER_VALUE: the per-title packet filter, as the clients'
    uint8_t backend_public_key[32];                 ///< ROCKETNET_SERVER_BACKEND_PUBLIC_KEY: verifies connect token signatures
    uint8_t server_public_key[32];                  ///< ROCKETNET_SERVER_SERVER_PUBLIC_KEY: title server public key
    uint8_t server_secret_key[32];                  ///< ROCKETNET_SERVER_SERVER_SECRET_KEY: secret, logged redacted
    int num_xdp_queues;                             ///< ROCKETNET_SERVER_NUM_XDP_QUEUES: xdp and gso only; 1 .. 1024; 0 is the transport default
    int num_os_cpus;                                ///< ROCKETNET_SERVER_NUM_OS_CPUS: xdp only, the cores left to the OS; 1 .. 256; 0 is the transport default
    int max_payload_bytes;                          ///< ROCKETNET_SERVER_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    int num_reassembly_slots;                       ///< ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS: per client; 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    int max_timeout_seconds;                        ///< ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS: 1 .. 3600; default 60
    int handshake_deadline_seconds;                 ///< ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS: 1 .. 60; default 5
    const char * transport;                         ///< ROCKETNET_SERVER_TRANSPORT: the candidate list; NULL is "auto"; every transport, xdp included, serves IPv4, IPv6, or both when second_public_address names the other family on the same port
    const char * public_address;                    ///< ROCKETNET_SERVER_PUBLIC_ADDRESS: "a.b.c.d:port" or "[v6]:port"; NULL is the create call's address
    int per_client_send_kbps;                       ///< ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS: each client's send budget, kbps; default 10000; the starting rate of each client connection; 0 or less sets the budget to its ceiling, ROCKETNET_SEND_BUDGET_MAX_KBPS (10 Gbit/s), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused
    int num_channels;                               ///< ROCKETNET_SERVER_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    int window_messages;                            ///< ROCKETNET_SERVER_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    int receive_ring_bytes;                         ///< ROCKETNET_SERVER_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    int max_message_bytes;                          ///< ROCKETNET_SERVER_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    int send_buffer_bytes;                          ///< ROCKETNET_SERVER_SEND_BUFFER_BYTES: queued message bytes; per client; 12288 .. 1 << 28; default 800000
    int send_queue_messages;                        ///< ROCKETNET_SERVER_SEND_QUEUE_MESSAGES: queued messages; per client; 16 .. 1 << 20; default 1024
    int packets_in_flight;                          ///< ROCKETNET_SERVER_PACKETS_IN_FLIGHT: unacknowledged packets tracked; per client; 16 .. 1 << 16; default 256
    int max_packets_per_update;                     ///< ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE: packets sent per update, in-place unreliable finish messages and send_packets together; per client; 1 .. 1024; default 256; the send budget is the throttle
    int unreliable_deliveries;                      ///< ROCKETNET_SERVER_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per client per update; 1 .. 1 << 16; default 256
    int sequenced_holds;                            ///< ROCKETNET_SERVER_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    /// @brief Maximum queued receipts per server client, 16 through 65536 (default 256).
    /// A full ring drops its oldest receipt; ROCKETNET_SERVER_RECEIPT_RING can override it.
    int receipt_ring;                               ///< ROCKETNET_SERVER_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    int rto_initial_ms;                             ///< ROCKETNET_SERVER_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 2000
    int rto_min_ms;                                 ///< ROCKETNET_SERVER_RTO_MIN_MS: optional sampled RTO floor in milliseconds; 0 disables it; 0 .. 60000; default 0
    int rto_max_ms;                                 ///< ROCKETNET_SERVER_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 2000
    int reliable_timeout_ms;                        ///< ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
    /// Simulated link conditions: development only. A release library refuses nonzero simulated_* fields with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE.
    int simulated_loss_percent;                     ///< ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT: development only: outgoing loss 0..100; 0 is off (sdk/rocketnet_internal.h:510)
    int simulated_latency_ms;                       ///< ROCKETNET_SERVER_SIMULATED_LATENCY_MS: development only: outgoing latency 0..60000 milliseconds; 0 is off (sdk/rocketnet_internal.h:511)
    int simulated_jitter_ms;                        ///< ROCKETNET_SERVER_SIMULATED_JITTER_MS: development only: outgoing jitter 0..60000 milliseconds; 0 is off (sdk/rocketnet_internal.h:512)
    int simulated_duplicate_percent;                ///< ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT: development only: outgoing duplication 0..100; 0 is off (sdk/rocketnet_internal.h:513)
    const char * second_public_address;             ///< ROCKETNET_SERVER_SECOND_PUBLIC_ADDRESS: the other family's "a.b.c.d:port" or "[v6]:port" on the same port; NULL or empty serves one family
};

/// Revision 1, the one revision: the whole struct, through second_public_address.
#define ROCKETNET_SERVER_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_config_t, second_public_address ) + sizeof( const char * ), sizeof( void * ) )

/// -----------------------------------------
///                           Messages
/// -----------------------------------------

/// A message is bytes, a pointer and a length (docs/CHANNELS.md). Each client
/// and each server is one block made at create -- the object, its connections
/// and one message region per connection -- and allocates nothing after
/// create. Memory is optional: NULL makes the one allocation, and a caller
/// block of at least the _required_memory bytes, aligned to
/// ROCKETNET_ALLOCATION_ALIGNMENT, makes none (the transport underneath still
/// allocates its own at client connect and server start).
///
/// Time is the game's clock, a double in seconds, monotonic; pass the same
/// clock to every call that takes one. The library reads no clock of its own
/// on these paths, so a game owns time and a test can hold it still.
///
/// The update order, on the main thread: update (messages in, acks into the
/// channels, received messages into deliveries, then the checks that
/// disconnect), receive and release, start message, write, finish message
/// (or abort message), then send_packets (each connection's messages, the
/// queued messages included in them, under its send budget, out to the wire).
///
/// Sending: start message, write in place, finish message is the only send
/// path. Start message returns a pointer into the connection's region for the
/// message, write the message there once (copy it in when the caller already
/// holds a buffer), and
/// finish message (bytes_written) queues it and returns its receipt serial,
/// nonzero; a NULL start message is a refused send, 0. The pointer is valid
/// until finish message or abort message; abort message gives the room back.
/// One start message is pending per object at a time.
/// Reliability is a rocketnet_reliability_t. An unreliable send too
/// big for one packet's record is promoted to its reliable twin, as RakNet's
/// ReliabilityLayer does, and counted in the internal counter set, not the
/// public counter enums (MESSAGE_PROMOTED_UNRELIABLE,
/// MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED,
/// MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT); the receiver sees the twin's
/// wire code. Plain UNRELIABLE is one record in one packet up to 1411 bytes
/// and promotes to RELIABLE above that.
/// UNRELIABLE_SEQUENCED is one record up to 1405 bytes and promotes to
/// RELIABLE_SEQUENCED past that, keeping its channel.
/// UNRELIABLE_WITH_ACK_RECEIPT is the code-0 wire form up to 1411 bytes and
/// promotes to RELIABLE_WITH_ACK_RECEIPT semantics (the code-2 block path)
/// above that, keeping its ACKED-only receipt. The reliable types and promoted
/// sends are bounded by the config's max_message_bytes; a send above it is
/// refused in every unreliable mode, with no second record-length refusal.
///
/// A send the connection's region has no room for ends the connection: the
/// server disconnects that client, the client disconnects itself, with the
/// reason ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED. The send budget
/// governs the wire, never the queue: a reliable message waits for budget and
/// is never dropped; an unreliable one over budget is dropped and counted
/// (the BUDGET_DROPS counters).
///
/// Receiving: receive hands out the next message of the update on a channel
/// as a const view and its length, valid until the next update. Release every
/// received message before the next update; update with one still held
/// asserts in a debug build and returns ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED
/// in a release build. Copy what is kept past the update.

/// @brief One outcome for a send's nonzero receipt serial.
/// Receipts are copied into caller storage after an update. A full receipt ring
/// drops its oldest entry; copy any result needed beyond the next update.
struct rocketnet_receipt_t
{
    /// @brief Identify the send whose outcome is reported.
    uint32_t serial;
    /// @brief State whether the peer transport acknowledged or missed it: a
    /// rocketnet_receipt_status_t value held in a fixed 32-bit field. A
    /// zero-filled receipt is ROCKETNET_RECEIPT_STATUS_NONE. The library writes
    /// ROCKETNET_RECEIPT_STATUS_ACKED or ROCKETNET_RECEIPT_STATUS_LOSS.
    uint32_t status;
};

/// -----------------------------------------
///                      Connect tokens
/// -----------------------------------------

/// The backend's Ed25519 signing keypair: public_key is
/// ROCKETNET_BACKEND_PUBLIC_KEY_BYTES long and secret_key is
/// ROCKETNET_BACKEND_SECRET_KEY_BYTES long. The backend keeps the secret key
/// and gives the public key to every server. Returns ROCKETNET_STATUS_OK, or
/// ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL pointer, or
/// ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the crypto library refuses.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_generate_backend_keypair( uint8_t * public_key, uint8_t * secret_key );

/// Derives the backend's Ed25519 signing keypair from a caller seed: seed is
/// ROCKETNET_BACKEND_SEED_BYTES long, and public_key and secret_key are as
/// above. The same seed always gives the same keypair. Returns
/// ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
/// pointer, or ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the crypto library
/// refuses. Secret keys are never logged.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_generate_backend_keypair_from_seed( const uint8_t * seed, uint8_t * public_key, uint8_t * secret_key );

/// The server's X25519 sealing keypair: public_key and secret_key are each
/// ROCKETNET_SERVER_PUBLIC_KEY_BYTES and ROCKETNET_SERVER_SECRET_KEY_BYTES
/// long. The server keeps the secret key; the backend seals tokens to the
/// public key. Returns the same codes as the backend keypair call.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_generate_server_keypair( uint8_t * public_key, uint8_t * secret_key );

/// Mints a production connect token from the backend's secret key and the
/// server's public key. The token carries client_id, protocol_id, the server
/// addresses, create_timestamp, expire_timestamp, timeout_seconds and
/// user_data, and fresh session keys. server_address_strings holds
/// server_address_count entries (1 or 2), at most one of each family. With
/// both families the client selects the IPv6 address on a host that can open
/// an IPv6 socket and the IPv4 address on one that cannot. The server checks
/// only the token address for the family the request arrived on; that address
/// must be one the server advertises. user_data
/// is ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES long, or NULL for all zeros. The
/// output is ROCKETNET_CONNECT_TOKEN_BYTES long and connect_token_bytes is its
/// size; a short buffer is refused with ROCKETNET_STATUS_INVALID_ARGUMENT and
/// nothing is written. A timeout_seconds of zero or below, or an
/// expire_timestamp at or before create_timestamp, is refused with
/// ROCKETNET_STATUS_INVALID_ARGUMENT and nothing is written. Reads no clock.
/// Returns ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT,
/// ROCKETNET_STATUS_INVALID_PROTOCOL_ID, ROCKETNET_STATUS_INVALID_ADDRESS or
/// ROCKETNET_STATUS_CRYPTO_INIT_FAILED. Secret keys are never logged.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_write_production_connect_token( const uint8_t * backend_secret_key,
                                                                                  const uint8_t * server_public_key,
                                                                                  uint64_t protocol_id,
                                                                                  const char * const * server_address_strings,
                                                                                  int server_address_count,
                                                                                  uint64_t client_id,
                                                                                  uint64_t create_timestamp,
                                                                                  uint64_t expire_timestamp,
                                                                                  int32_t timeout_seconds,
                                                                                  const uint8_t * user_data,
                                                                                  uint8_t * connect_token,
                                                                                  size_t connect_token_bytes );

/// -----------------------------------------
///                             Client
/// -----------------------------------------

/// The client connects with a connect token, never an address: the token names
/// the server, carries the keys, and is the credential -- minted by a backend
/// in production, or locally by rocketnet_write_dev_connect_token against the
/// published development keys.
///
/// The client is reusable, as netcode's is: create makes a DISCONNECTED
/// client; rocketnet_client_connect starts a connection with a token;
/// rocketnet_client_disconnect ends it, from any state, and returns the client
/// to DISCONNECTED, ready to connect again with a new token; destroy works
/// from any state. Each connection starts fresh: nothing of an earlier one
/// is delivered or kept except the lifetime counters and the last disconnect
/// reason. The socket is per connection, opened by connect for the address
/// family the token names.

/// Writes the defaults into every field of the struct, ROCKETNET_CLIENT_CONFIG_BYTES_1
/// bytes, the same bytes as the sized form at sizeof(*config)
/// (rocketnet_client_default_config in sdk/rocketnet_api.cpp).
/// ROCKETNET_STATUS_OK; a NULL config is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config( struct rocketnet_client_config_t * config );

/// The defaults for a caller whose struct is config_bytes long.
/// ROCKETNET_STATUS_OK written; ROCKETNET_STATUS_INVALID_ARGUMENT for an
/// unknown size, nothing written. A NULL config is a counted no-op that
/// returns ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config_sized( struct rocketnet_client_config_t * config, size_t config_bytes );

/// ROCKETNET_STATUS_OK when create would accept the config's own values, else
/// ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE with a log line naming the rule. It
/// runs the one check create runs: every field's range, the transport list's
/// parse, the simulated_* fields (nonzero is refused by a production library),
/// and the message rules. The environment is not read here, though create and
/// required_memory read it. Safe before init. NULL means the defaults.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_validate_config( const struct rocketnet_client_config_t * config );
/// Validates a known-size client config; an unknown non-NULL size logs and
/// returns ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_validate_config_sized( const struct rocketnet_client_config_t * config, size_t config_bytes );

/// The bytes a caller memory block for this client needs; 0 for a refused
/// config. NULL means the defaults.
ROCKETNET_EXPORT size_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );
/// Queries a known-size client config; an unknown non-NULL size logs and returns 0.
ROCKETNET_EXPORT size_t rocketnet_client_required_memory_sized( const struct rocketnet_client_config_t * config, size_t config_bytes );

#if ROCKETNET_DEVELOPMENT
/// Mints a development connect token for a server at this address, signed with
/// the published development backend key and sealed to the published development
/// server key, into connect_token, connect_token_bytes long (at least
/// ROCKETNET_CONNECT_TOKEN_BYTES). Returns ROCKETNET_STATUS_OK;
/// ROCKETNET_STATUS_INVALID_ADDRESS for an address that does not parse;
/// ROCKETNET_STATUS_INVALID_ARGUMENT for a short buffer, nothing written. A NULL
/// server_address_string or connect_token is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_write_dev_connect_token( const char * server_address_string, uint64_t client_id, uint64_t unix_timestamp, uint8_t * connect_token, size_t connect_token_bytes );
#endif /// #if ROCKETNET_DEVELOPMENT

/// A DISCONNECTED client. Create reads no clock and opens no socket. It reads
/// every field of the config, the simulated_* fields included, as the sized
/// form at sizeof(*config) (rocketnet_client_create in sdk/rocketnet_api.cpp).
/// NULL config is the defaults. context is an opaque caller pointer handed to the
/// allocator's callbacks and to the config's state_change_callback and
/// time_callback; keep it valid until destroy. Returns NULL on a config
/// validate_config refuses, on a refused memory block, and before
/// rocketnet_init (logged, and counted in rocketnet_api_misuse_count).
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, void * memory, size_t memory_bytes );

/// Create with a config config_bytes long; an unknown size is refused with a
/// log line and returns NULL before any other check. context and its lifetime
/// are as rocketnet_client_create states.
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create_sized( void * context, const struct rocketnet_client_config_t * config, size_t config_bytes, void * memory, size_t memory_bytes );

/// Starts a connection with connect_token, ROCKETNET_CONNECT_TOKEN_BYTES long.
/// Only from DISCONNECTED: any other state returns
/// ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED with nothing changed. A token that
/// fails the pre-send checks returns ROCKETNET_STATUS_INVALID_CONNECT_TOKEN or
/// ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED and leaves the matching error state;
/// a socket or thread the platform refuses returns its status and leaves the
/// client DISCONNECTED. Connect reads no clock: the attempt's clock starts at
/// the next update. A connection the server ended (DISCONNECTED, its object
/// still held) is ended here first, its reason kept. A NULL
/// client or connect_token is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT. Returns ROCKETNET_STATUS_OK,
/// ROCKETNET_STATUS_INVALID_ARGUMENT, ROCKETNET_STATUS_NOT_INITIALIZED,
/// ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED, ROCKETNET_STATUS_INVALID_CONNECT_TOKEN,
/// ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED, ROCKETNET_STATUS_CONFIG_ENV_INVALID,
/// ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED,
/// ROCKETNET_STATUS_INVALID_TRANSPORT_LIST, ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED,
/// ROCKETNET_STATUS_OUT_OF_MEMORY, ROCKETNET_STATUS_MUTEX_CREATE_FAILED,
/// or ROCKETNET_STATUS_THREAD_CREATE_FAILED.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const uint8_t * connect_token );

/// From any state: a live connection ends as rocketnet_client_disconnect ends
/// it. Once the session keys exist (SENDING_CONNECTION_RESPONSE or CONNECTED)
/// the redundant disconnect packets are sent before the transport closes, so
/// the server frees the slot on receipt rather than at timeout; from
/// DISCONNECTED, an error state, REQUESTING_COOKIE or SENDING_CONNECTION_REQUEST
/// nothing is sent. The client is freed, and a caller memory block is cleared
/// and given back to the caller. Every pointer from the client is invalid after
/// the call. A NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

/// ROCKETNET_UPDATE_OK, or ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. The update time
/// must be finite and nondecreasing: a NaN, an infinity or a step backwards is
/// refused, counted once in rocketnet_api_misuse_count, and the last good time
/// is kept, so the clock is never poisoned or wedged. A NULL client is a
/// counted no-op that returns ROCKETNET_UPDATE_OK; with no connection the update
/// stores no time, so nothing is counted there.
ROCKETNET_EXPORT enum rocketnet_update_result_t rocketnet_client_update( struct rocketnet_client_t * client, double time );

/// Sends this frame's queued messages. A NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

/// Ends the connection and returns the client to DISCONNECTED. From a
/// handshake state or CONNECTED the disconnect packets are sent first; from
/// an error state the error ends. The client stays valid and can connect
/// again. A NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

/// A NULL client returns ROCKETNET_CLIENT_STATE_INVALID, counted and logged.
ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

/// The name of a state ("connected", "invalid"), or "unknown" for a value that is not one.
ROCKETNET_EXPORT const char * rocketnet_client_state_name( enum rocketnet_client_state_t state );

/// The reason the last connection ended with, kept until the next connection is established: ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED when the client accepted the server's disconnect (the server disconnected the client, stopped, or was destroyed); NONE while connected, after a deliberate disconnect and after a connection timeout. A NULL client is a counted no-op that returns ROCKETNET_DISCONNECT_REASON_NONE.
ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_client_disconnect_reason( const struct rocketnet_client_t * client );

/// The name of a disconnect reason ("none", "reliable timeout"), or "unknown" for a value that is not one.
ROCKETNET_EXPORT const char * rocketnet_disconnect_reason_name( enum rocketnet_disconnect_reason_t reason );

/// The client's index on the server while CONNECTED. A client in any other state returns 0, which is not a server slot it holds. A NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_client_index( const struct rocketnet_client_t * client );

/// The transport that won the candidate list at connect ("portable", "msgx"), NULL while DISCONNECTED. The returned string is static and remains valid for the process lifetime. A NULL client is a counted no-op that returns NULL.
ROCKETNET_EXPORT const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client );

/// The first step of the send: a pointer into the connection's region for a
/// message of bytes, or, for an unreliable message whose declared bytes fit one
/// packet with every header, into the transport packet buffer itself
/// (docs/OUTGOING-BUILD.md; written in place, with no copy between the message
/// layer and the transport), or NULL when refused. Write the message there once,
/// or copy it in when the caller already holds a buffer:
///
///     uint8_t * out = rocketnet_client_start_message( client, channel, reliability, bytes );
///     if ( out )
///     {
///         memcpy( out, data, bytes );
///         serial = rocketnet_client_finish_message( client, bytes );
///     }
///
/// The pointer is valid until finish message or abort message; one start
/// message is pending per object at a time. A message is sent only while the
/// client is CONNECTED (rocketnet_client_state). In every other state the send
/// is dropped: start message returns NULL, logs nothing and adds 1 to
/// ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS (not
/// rocketnet_api_misuse_count), and finish message returns 0. The drop is
/// normal, never misuse, and nothing is held for a later send. The same
/// if ( out ) that covers the other NULL returns covers it, so game code sends
/// from create on with no state check. A NULL client is a counted no-op that
/// returns NULL.
ROCKETNET_EXPORT uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int bytes );

/// The queued message's receipt serial, nonzero; 0 when no start message is pending (a start dropped because the client was not CONNECTED included: that drop was counted at start, in ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS) or bytes_written is outside 1 to the bytes started. For a message written in place, finish message completes and stages that packet buffer, with no copy. A NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written );

/// A NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_abort_message( struct rocketnet_client_t * client );

/// 1 with the next message on channel, 0 when there is none. A NULL client, data or bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_client_receive_message( struct rocketnet_client_t * client, int channel, const uint8_t ** data, int * bytes );

/// A NULL client or data is a counted no-op; a pointer that is not a held view is a counted misuse.
ROCKETNET_EXPORT void rocketnet_client_release_message( struct rocketnet_client_t * client, const uint8_t * data );

/// @brief Copy this update's client receipt outcomes into caller storage.
/// Call after update and before the next update, without a concurrent update.
/// Repeated calls copy the same visible entries; the client owns the internal
/// list, and the caller owns the output array.
/// @param client Client object; NULL returns 0.
/// @param out Writable array with room for capacity entries; NULL returns 0.
/// @param capacity Array length in entries; values below 1 return 0.
/// @return Entries copied, from 0 through capacity. Zero means none are visible
/// or an input is invalid; it is not an error status.
ROCKETNET_EXPORT int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity );

/// ROCKETNET_STATUS_OK when the budget took the new rate, from now on. 0 or less sets the budget to its ceiling, ROCKETNET_SEND_BUDGET_MAX_KBPS (10 Gbit/s), NOT RECOMMENDED; a positive value up to ROCKETNET_SEND_BUDGET_MAX_KBPS caps it; above the maximum returns ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE with the rate unchanged; a client that is not CONNECTED (a handshake state included) returns ROCKETNET_STATUS_INVALID_ARGUMENT with nothing changed and is not counted as misuse: the config's send_kbps sets a connection's starting rate. A NULL client is a counted no-op that returns ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

/// A counter by its enum; 0 for a value outside the enum. A NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_client_counter( const struct rocketnet_client_t * client, enum rocketnet_client_counter_t counter );

/// A stat by its enum; 0 for a value outside the enum. A NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_client_stat( const struct rocketnet_client_t * client, enum rocketnet_client_stat_t stat );

/// -----------------------------------------
///                             Server
/// -----------------------------------------

/// Clients are identified by index, 0 .. max_clients - 1, the slot each
/// connection takes, as a uint64_t (ROCKETNET_FLOW_ID_BIT is never set in a
/// client index).
///
/// The server is reusable, as netcode's is: create validates the config and
/// makes the memory block but binds nothing, and the server starts stopped.
/// rocketnet_server_start binds the transport and accepts clients;
/// rocketnet_server_stop disconnects every client, resets every slot and
/// releases the transport, and the server can start again. The per-client
/// calls below are valid while running; on a stopped server they read 0.

/// Writes the defaults into every field of the struct, ROCKETNET_SERVER_CONFIG_BYTES_1
/// bytes, the same bytes as the sized form at sizeof(*config)
/// (rocketnet_server_default_config in sdk/rocketnet_api.cpp).
/// ROCKETNET_STATUS_OK; a NULL config is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config( struct rocketnet_server_config_t * config );

/// The defaults for a caller whose struct is config_bytes long.
/// ROCKETNET_STATUS_OK written; ROCKETNET_STATUS_INVALID_ARGUMENT for an
/// unknown size, nothing written. A NULL config is a counted no-op that
/// returns ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes );

/// The defaults with the published development keys and test filter, written
/// into every field of the struct, as the sized form at sizeof(*config)
/// (rocketnet_server_dev_config in sdk/rocketnet_api.cpp).
/// ROCKETNET_STATUS_OK; a NULL config is a counted no-op that returns
/// ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_dev_config( struct rocketnet_server_config_t * config );
/// Writes development defaults for a known server config size; an unknown size
/// logs, leaves the config untouched and returns ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_dev_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes );

/// ROCKETNET_STATUS_OK when create would accept the config's own values, else
/// ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE with a log line naming the rule. It
/// runs the one check create runs: every field's range, the transport list's
/// parse, the simulated_* fields (nonzero is refused by a production library),
/// and the message rules. The environment is not read here, though create and
/// required_memory read it, and the keys are not checked (start refuses a
/// config with none). Safe before init. NULL means the defaults.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_validate_config( const struct rocketnet_server_config_t * config );
/// Validates a known-size server config; an unknown non-NULL size logs and
/// returns ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_validate_config_sized( const struct rocketnet_server_config_t * config, size_t config_bytes );

/// The bytes a caller memory block for this server needs, at the config's
/// max_clients; 0 for a refused config. NULL means the defaults.
ROCKETNET_EXPORT size_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );
/// Queries a known-size server config; an unknown non-NULL size logs and returns 0.
ROCKETNET_EXPORT size_t rocketnet_server_required_memory_sized( const struct rocketnet_server_config_t * config, size_t config_bytes );

/// Create reads every field of the config, the simulated_* fields included, as
/// the sized form at sizeof(*config)
/// (rocketnet_server_create in sdk/rocketnet_api.cpp).
/// Address is where clients reach the server, "a.b.c.d:port" or "[v6]:port"; a
/// port of 0 takes an ephemeral port (read it back with rocketnet_server_port
/// once started). The server is created stopped and binds nothing. NULL config
/// is the defaults, which start refuses until keys are given. context is an
/// opaque caller pointer handed to the allocator's callbacks and to the message
/// and transport create; keep it valid until destroy. Returns NULL on a config validate_config
/// refuses, on a refused memory block, and before rocketnet_init (logged, and
/// counted in rocketnet_api_misuse_count). A NULL address is a counted no-op
/// that returns NULL.
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, void * memory, size_t memory_bytes );

/// Create with a config config_bytes long; an unknown size is refused with a
/// log line and returns NULL before any other check. context and its lifetime
/// are as rocketnet_server_create states.
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create_sized( void * context, const char * address, const struct rocketnet_server_config_t * config, size_t config_bytes, void * memory, size_t memory_bytes );

/// Binds the transport at the address and accepts clients, up to the
/// config's max_clients. Only from stopped: a running server returns
/// ROCKETNET_STATUS_SERVER_NOT_STOPPED with nothing changed. A transport that
/// does not start returns its status (ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED,
/// TRANSPORT_PORT_ZERO_UNSUPPORTED, TRANSPORT_FAMILIES_UNSUPPORTED, MISSING_KEYS, THREAD_CREATE_FAILED,
/// TIMER_CREATE_FAILED, MUTEX_CREATE_FAILED, OUT_OF_MEMORY, ...) and leaves the
/// server stopped. A NULL server is a counted no-op that returns
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
/// nothing is sent. Then the server's memory, keys included, is wiped and
/// freed, and a caller memory block is given back to the caller. Every pointer
/// from the server is invalid after the call. A NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

/// ROCKETNET_UPDATE_OK, or ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. The update time
/// must be finite and nondecreasing: a NaN, an infinity or a step backwards is
/// refused, counted once in rocketnet_api_misuse_count, and the last good time
/// is kept. On a stopped server a counted no-op, storing no time. A NULL server
/// is a counted no-op that returns ROCKETNET_UPDATE_OK.
ROCKETNET_EXPORT enum rocketnet_update_result_t rocketnet_server_update( struct rocketnet_server_t * server, double time );

/// Sends this frame's queued messages. On a stopped server a counted no-op. A NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

/// The port the server bound: the address's port, or the ephemeral port a
/// port of 0 was given; 0 while stopped. Build a token address from it in
/// tests and tools. A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

/// The transport that won the candidate list at start ("xdp", "gso", "io_uring", "msgx", "iocp", "rio", "portable", "loopback"), NULL while stopped. The returned string is static and remains valid for the process lifetime. A NULL server is a counted no-op that returns NULL.
ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

/// A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

/// A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

/// true when a client holds the slot. A NULL server is a counted no-op that returns false.
ROCKETNET_EXPORT bool rocketnet_server_client_connected( const struct rocketnet_server_t * server, uint64_t client_index );

/// The client id from the connect token of the client in the slot, 0 for a free slot. A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, uint64_t client_index );

/// Disconnects one client deliberately and frees its slot. Why is the game's
/// decision. A NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, uint64_t client_index );

/// Disconnects every connected client as rocketnet_server_disconnect_client
/// does; the server keeps running. A NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server );

/// The reason the slot's last connection ended with, until the slot's next connection. A NULL server is a counted no-op that returns ROCKETNET_DISCONNECT_REASON_NONE.
ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, uint64_t client_index );

/// The first step of the send: a pointer into the connection's region for a
/// message of bytes, or, for an unreliable message whose declared bytes fit one
/// packet with every header, into the transport packet buffer itself
/// (docs/OUTGOING-BUILD.md; written in place, with no copy between the message
/// layer and the transport), or NULL when refused. Write the message there once,
/// or copy it in when the caller already holds a buffer:
///
///     uint8_t * out = rocketnet_server_start_message( server, destination_id, channel, reliability, bytes );
///     if ( out )
///     {
///         memcpy( out, data, bytes );
///         serial = rocketnet_server_finish_message( server, destination_id, bytes );
///     }
///
/// destination_id is a client index today. An id with ROCKETNET_FLOW_ID_BIT
/// set is a flow id, reserved for a future flow feature and refused for now
/// (NULL, 0 or nothing, counted; ROCKETNET_STATUS_FLOW_ID_RESERVED in the log).
/// An index at or past max_clients is a counted misuse. A message is sent only
/// to a client index whose connection is CONNECTED
/// (rocketnet_server_client_connected). A send to an index below max_clients
/// with no CONNECTED client, running server or stopped, is dropped: start
/// message returns NULL, logs nothing and adds 1 to
/// ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS (not
/// rocketnet_api_misuse_count), and finish message returns 0. The drop is
/// normal, never misuse, and nothing is held for a later send; the same
/// if ( out ) covers it. To send to every connected client, loop over the
/// slots. The pointer is valid until finish message or abort message; one
/// start message is pending per object at a time. A NULL server is a counted
/// no-op that returns NULL.
ROCKETNET_EXPORT uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, enum rocketnet_reliability_t reliability, int bytes );

/// The queued message's receipt serial, nonzero; 0 when no start message is pending, bytes_written is outside 1 to the bytes started, destination_id is a flow id or an index at or past max_clients (counted misuse), or the server is stopped or the slot is not CONNECTED (a normal drop, counted nowhere). For a message written in place, finish message completes and stages that packet buffer, with no copy. A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, uint64_t destination_id, int bytes_written );

/// A NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_abort_message( struct rocketnet_server_t * server, uint64_t destination_id );

/// 1 with the next message on channel, 0 when there is none. A NULL server, data or bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_receive_message( struct rocketnet_server_t * server, uint64_t client_index, int channel, const uint8_t ** data, int * bytes );

/// A NULL server or data is a counted no-op; a pointer that is not a held view is a counted misuse.
ROCKETNET_EXPORT void rocketnet_server_release_message( struct rocketnet_server_t * server, uint64_t client_index, const uint8_t * data );

/// @brief Copy one connected server client's receipt outcomes.
/// Call after update and before the next update, without a concurrent update.
/// Repeated calls copy the same visible entries; the server owns the internal
/// list, and the caller owns the output array.
/// @param server Server object; NULL returns 0.
/// @param client_index Client index in [0, configured max_clients); an invalid or
/// disconnected slot, or a stopped server, returns 0.
/// @param out Writable array with room for capacity entries; NULL returns 0.
/// @param capacity Array length in entries; values below 1 return 0.
/// @return Entries copied, from 0 through capacity. Zero means none are visible
/// or an input is invalid; it is not an error status.
ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, uint64_t client_index, struct rocketnet_receipt_t * out, int capacity );

/// ROCKETNET_STATUS_OK when the client's budget took the new rate, from now on (the slot must hold a CONNECTED client). 0 or less sets the budget to its ceiling, ROCKETNET_SEND_BUDGET_MAX_KBPS (10 Gbit/s), NOT RECOMMENDED; a positive value up to ROCKETNET_SEND_BUDGET_MAX_KBPS caps it; above the maximum returns ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE with the rate unchanged. An index at or past max_clients, a slot below max_clients with no CONNECTED client, or a stopped server returns ROCKETNET_STATUS_INVALID_ARGUMENT with nothing changed; a slot that is not CONNECTED is not counted as misuse: the config's per_client_send_kbps sets each connection's starting rate. A flow id returns ROCKETNET_STATUS_FLOW_ID_RESERVED. A NULL server is a counted no-op that returns ROCKETNET_STATUS_INVALID_ARGUMENT.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, uint64_t client_index, int kbps );

/// A counter by its enum; 0 for a value outside the enum. A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_counter( const struct rocketnet_server_t * server, enum rocketnet_server_counter_t counter );

/// A stat by its enum; 0 for a value outside the enum. A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_server_stat( const struct rocketnet_server_t * server, enum rocketnet_server_stat_t stat );

/// One client slot's counter by its enum; 0 for a free slot or a value outside the enum. A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_client_counter( const struct rocketnet_server_t * server, uint64_t client_index, enum rocketnet_server_client_counter_t counter );

/// One client slot's stat by its enum; 0 for a free slot or a value outside the enum. A NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_server_client_stat( const struct rocketnet_server_t * server, uint64_t client_index, enum rocketnet_server_client_stat_t stat );

#ifdef __cplusplus
}
#endif

#endif /// #ifndef ROCKETNET_H
