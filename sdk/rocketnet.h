/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public C API: a client and a server that exchange messages over UDP.
// This header and a prebuilt RocketNet library are all a program needs: it
// includes only standard C headers and declares only what a caller calls or
// reads. Plain C99, callable from C++ through the extern "C" block below.

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ROCKETNET_CONFIG_REVISION_BYTES( end, align ) ( ( ( end ) + ( align ) - 1 ) / ( align ) * ( align ) )

// -----------------------------------------
//                     version and ABI
// -----------------------------------------

// ROCKETNET_DEVELOPMENT is 1 when linking a development library
// (rocketnet_dev): it carries the development token minting below and accepts
// the published development keys.

#if !defined( ROCKETNET_DEVELOPMENT )
#define ROCKETNET_DEVELOPMENT 0
#endif // #if !defined( ROCKETNET_DEVELOPMENT )

#if !ROCKETNET_DEVELOPMENT

    /** @brief Text form of the SDK version: the release version string, or "dev" in a development build. */
    #define ROCKETNET_VERSION_FULL                               "1.0.0"
    /** @brief Major component of the SDK version; development builds use 255. */
    #define ROCKETNET_VERSION_MAJOR                                    1
    /** @brief Minor component of the SDK version; development builds use 255. */
    #define ROCKETNET_VERSION_MINOR                                    0
    /** @brief Patch component of the SDK version; development builds use 255. */
    #define ROCKETNET_VERSION_PATCH                                    0

#else // !ROCKETNET_DEVELOPMENT

    #define ROCKETNET_VERSION_FULL                                 "dev"
    #define ROCKETNET_VERSION_MAJOR                                  255
    #define ROCKETNET_VERSION_MINOR                                  255
    #define ROCKETNET_VERSION_PATCH                                  255

#endif // !ROCKETNET_DEVELOPMENT

// The version as one comparable integer: major * 10000 + minor * 100 + patch,
// 10000 for 1.0.0. A development build is 2575755, above every release.
// rocketnet_version returns the value the library was built with.
#define ROCKETNET_VERSION   ( ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH )

// the earlier names of the three parts
#define ROCKETNET_VERSION_MAJOR_INT                 ROCKETNET_VERSION_MAJOR
#define ROCKETNET_VERSION_MINOR_INT                 ROCKETNET_VERSION_MINOR
#define ROCKETNET_VERSION_PATCH_INT                 ROCKETNET_VERSION_PATCH

/**
 * @brief Identifies the public C struct and calling-convention revision.
 *
 * This revision is independent of the wire protocol version. Existing public
 * layouts remain unchanged. Increment it for an incompatible public layout or
 * calling-contract change; append-only additions that preserve old layouts
 * keep the revision unchanged.
 */
#define ROCKETNET_ABI_VERSION 2

// -----------------------------------------
//                       export macro
// -----------------------------------------

// Define ROCKETNET_SHARED when linking the shared library (the DLL). A static
// link defines nothing. ROCKETNET_BUILDING_DLL is the library's own build
// switch, never a caller's.

#if defined( _WIN32 ) && defined( ROCKETNET_BUILDING_DLL )
    #define ROCKETNET_EXPORT __declspec(dllexport)
#elif defined( _WIN32 ) && defined( ROCKETNET_SHARED )
    #define ROCKETNET_EXPORT __declspec(dllimport)
#elif ( defined( ROCKETNET_SHARED ) || defined( ROCKETNET_BUILDING_DLL ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
    #define ROCKETNET_EXPORT __attribute__((visibility("default")))
#else
    #define ROCKETNET_EXPORT
#endif

// -----------------------------------------
//                          constants
// -----------------------------------------

#define ROCKETNET_MAX_CLIENTS                                       1000

// The in-memory loopback transport holds this many endpoints in one process:
// enough for one server and ROCKETNET_MAX_CLIENTS clients. It is chosen only
// by name ("loopback"), for tests and examples; auto never selects it.

#define ROCKETNET_LOOPBACK_MAX_ENDPOINTS          ( ROCKETNET_MAX_CLIENTS + 1 )

// The packet size: the UDP payload of one datagram on a 1500-byte MTU link,
// the number every other size limit derives from (STANDARD.md _Packet
// sizes_). Over IPv4 it is 1500 - 20 - 8 = 1472 bytes, the receive ceiling;
// over IPv6 it is 1500 - 40 - 8 = 1452 bytes, the send budget, so a maximum
// packet crosses a 1500-MTU path unfragmented on either family.

#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 20 - 8 )
#define ROCKETNET_MAX_PACKET_BYTES_IPV6                ( 1500 - 40 - 8 )

// The send-side payload capacity: the send budget (ROCKETNET_MAX_PACKET_BYTES_IPV6,
// 1452) less the 26-byte fixed overhead and the 7-byte minimum ack block
// leaves 1419; the capacity is the largest multiple of eight inside that,
// 1416, so any word-based serializer can root at the payload pointer
// directly. The receive side accepts payloads up to the receive ceiling
// (ROCKETNET_MAX_PACKET_BYTES, 1472) less the same overhead, 1439 bytes,
// whatever this end's own budget, so receive buffers size by the receive
// constant.

#define ROCKETNET_MAX_PAYLOAD_BYTES                        1416    // ( ( 1452 - 26 - 7 ) / 8 ) * 8
#define ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES                1439    // 1472 - 26 - 7

// Payloads above the wire capacity fragment transparently, and a lost
// fragment loses the whole payload. The config's max_payload_bytes bounds
// both what this end fragments and what it reassembles; storage for both is
// allocated once at create, to the maximum. The hard ceiling is 256
// fragments at the full stride: 305,408 bytes.

/** @brief Default maximum payload in bytes. */
#define ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES                16384
/** @brief Default number of simultaneous fragmented-payload reassembly slots per connection. */
#define ROCKETNET_DEFAULT_REASSEMBLY_SLOTS                 8

/** @brief Required connect-token buffer size in bytes. */
#define ROCKETNET_CONNECT_TOKEN_BYTES                      768
/** @brief Application-defined connect-token user data size in bytes. */
#define ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES            256
/** @brief Backend signing public key size in bytes. */
#define ROCKETNET_BACKEND_PUBLIC_KEY_BYTES                  32
/** @brief Backend signing secret key size in bytes. */
#define ROCKETNET_BACKEND_SECRET_KEY_BYTES                  64
/** @brief Backend signing seed size in bytes. */
#define ROCKETNET_BACKEND_SEED_BYTES                        32
/** @brief Server sealing public key size in bytes. */
#define ROCKETNET_SERVER_PUBLIC_KEY_BYTES                   32
/** @brief Server sealing secret key size in bytes. */
#define ROCKETNET_SERVER_SECRET_KEY_BYTES                   32

/** @brief Alignment in bytes that a caller memory block for a client or a server needs, and that SDK allocations return. */
#define ROCKETNET_ALLOCATION_ALIGNMENT                     64

/** @brief Allows only level-NONE messages when used as the maximum; quiet mode suppresses all messages. */
#define ROCKETNET_LOG_LEVEL_NONE                                       0
/** @brief Error log level, the least verbose emitted level. */
#define ROCKETNET_LOG_LEVEL_ERROR                                      1
/** @brief Warning log level. */
#define ROCKETNET_LOG_LEVEL_WARN                                       2
/** @brief Informational log level. */
#define ROCKETNET_LOG_LEVEL_INFO                                       3
/** @brief Debug log level. */
#define ROCKETNET_LOG_LEVEL_DEBUG                                      4
/** @brief Most verbose log level. */
#define ROCKETNET_LOG_LEVEL_SPAM                                       5

// messages (docs/CHANNELS.md): the channel bound, per-send reliability --
// RakNet's values and numbering -- and receipt outcomes

#ifndef ROCKETNET_MAX_CHANNELS
/** @brief Maximum channel count supported by the public message API; the default configuration uses 32. */
#define ROCKETNET_MAX_CHANNELS                                         32
#endif // #ifndef ROCKETNET_MAX_CHANNELS

/** @brief Send once without delivery, ordering, sequencing, or receipt.
 * The receiver can see the message at most once. A lost packet loses it.
 */
#define ROCKETNET_UNRELIABLE                                            0
/** @brief Send without delivery guarantees and keep only newer channel state.
 * The receiver delivers a state only when its ordering and sequencing values
 * are newer; older or out-of-turn states can be discarded.
 */
#define ROCKETNET_UNRELIABLE_SEQUENCED                                  1
/** @brief Resend until the peer's transport acknowledges the message.
 * Delivery is once, but messages on the same channel have no order guarantee.
 * This mode does not produce a receipt.
 */
#define ROCKETNET_RELIABLE                                              2
/** @brief Resend until acknowledged and deliver in channel order.
 * The receiver holds later messages until earlier messages on that channel
 * arrive. This mode does not produce a receipt.
 */
#define ROCKETNET_RELIABLE_ORDERED                                      3
/** @brief Resend until acknowledged and deliver only the newest channel state.
 * The receiver can discard an acknowledged stale state; delivery order is
 * not guaranteed. This mode does not produce a receipt.
 */
#define ROCKETNET_RELIABLE_SEQUENCED                                    4
/** @brief Send once and report whether the carrying packet is acknowledged.
 * A larger message is sent as a reliable block and can report ACKED only.
 * A single-packet message can report ACKED or LOSS.
 */
#define ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT                           5
/** @brief Resend until acknowledged and report an ACKED receipt.
 * Delivery is once without channel ordering. A connection failure does not
 * produce a LOSS receipt.
 */
#define ROCKETNET_RELIABLE_WITH_ACK_RECEIPT                             6
/** @brief Resend until acknowledged, deliver in channel order, and report ACKED.
 * A connection failure does not produce a LOSS receipt.
 */
#define ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT                     7

/** @brief Receipt status meaning the peer's transport acknowledged the packet.
 * This does not mean the peer application read the message.
 */
#define ROCKETNET_RECEIPT_ACKED                                         1
/** @brief Receipt status meaning an unreliable receipt packet was not acknowledged.
 * The message is not resent; reliable receipt modes do not produce LOSS.
 */
#define ROCKETNET_RECEIPT_LOSS                                          2

// Why a connection ended: what rocketnet_client_disconnect_reason and
// rocketnet_server_disconnect_reason return
// (docs/CHANNELS.md 12). MESSAGE_MEMORY_EXHAUSTED: the connection's region had
// no room for an operation it must not drop (docs/CHANNELS.md 4). A server
// disconnects that client's slot with this reason and resets the slot's
// region; a client disconnects itself.

/** @brief No message-connection error has occurred. */
#define ROCKETNET_MESSAGE_ERROR_NONE                                    0
/** @brief The peer message configuration does not match. */
#define ROCKETNET_MESSAGE_ERROR_CONFIG_MISMATCH                         1
/** @brief Reserved; the library does not currently return this reason (a configuration that never arrives ends with ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT). */
#define ROCKETNET_MESSAGE_ERROR_CONFIG_TIMEOUT                          2
/** @brief The peer sent message data that violates the protocol. */
#define ROCKETNET_MESSAGE_ERROR_PROTOCOL_VIOLATION                      3
/** @brief A reliable message, or the peer's message configuration, is not acknowledged or received within reliable_timeout_ms. */
#define ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT                        4
/** @brief The message region cannot retain a required operation, so the connection ends. */
#define ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED                5

// the largest message one packet carries, written in place with zero copies:
// the payload capacity less the one form byte

/** @brief Maximum byte count for a message written into one wire payload. */
#define ROCKETNET_MESSAGE_SINGLE_BYTES          ( ROCKETNET_MAX_PAYLOAD_BYTES - 1 )      // 1415

// Send budgets are kilobits per second of wire bytes (1 kbps = 1000 bit/s),
// per connection: the defaults of the config's send_kbps and
// per_client_send_kbps.

#ifndef ROCKETNET_DEFAULT_CLIENT_SEND_KBPS
/** @brief Default client send budget in decimal kilobits per second per connection. */
#define ROCKETNET_DEFAULT_CLIENT_SEND_KBPS                  1000
#endif
#ifndef ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS
/** @brief Default server send budget in decimal kilobits per second for each client. */
#define ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS       10000
#endif
#ifndef ROCKETNET_SEND_BUDGET_MAX_KBPS
/** @brief Maximum supported send budget in decimal kilobits per second per connection. */
#define ROCKETNET_SEND_BUDGET_MAX_KBPS                      10000000    // 10 Gbit/s per connection
#endif

/** @brief Successful client or server message update status. */
#define ROCKETNET_MESSAGE_OK                                0
/** @brief Update result indicating that a received message view remains unreleased. */
#define ROCKETNET_ERROR_MESSAGE_NOT_RELEASED                -1

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stable status values returned by the _ex lifecycle calls and the creates.
 *
 * Stable status codes (docs/ERRORS-AND-LOGGING.md 2.1): OK is the sole
 * success value, every failure is a positive code, and the numbering is
 * append-only. A published value and meaning never change; a new code appends
 * immediately before ROCKETNET_NUM_STATUS_CODES.
 */
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,   ///< success: the operation succeeds

    // lifecycle and allocator state (rocketnet_init, rocketnet_term, rocketnet_allocator)
    ROCKETNET_STATUS_ALREADY_INITIALIZED                = 1,   ///< initialization is complete or transitioning: rocketnet_init, or an allocator change, after the library was initialized
    ROCKETNET_STATUS_NOT_INITIALIZED                    = 2,   ///< the operation needs an initialized library: a call that needs rocketnet_init came before it, or a term with nothing to terminate
    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = 3,   ///< term or an allocator change refused: the library still holds allocations
    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = 4,   ///< the platform's socket layer did not start; correct the platform setup before retrying
    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = 5,   ///< the crypto library did not start; initialization does not complete

    // caller arguments and configuration (client, server, message)
    ROCKETNET_STATUS_INVALID_ARGUMENT                   = 6,   ///< an argument, or a combination of arguments, is NULL, out of range, or inconsistent with another
    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = 7,   ///< an environment override is malformed: it did not parse
    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = 8,   ///< a config field, from the struct or the environment, is out of its supported range
    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = 9,   ///< the memory the config needs is past the operator's memory budget
    ROCKETNET_STATUS_MISSING_KEYS                       = 10,   ///< required key material is absent: a server config has no keys
    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = 11,   ///< the protocol id is 0, a development id in a release build, or not the expected value
    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = 12,   ///< the published development keys in a release build
    ROCKETNET_STATUS_INVALID_ADDRESS                    = 13,   ///< an address string did not parse or cannot be used
    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = 14,   ///< the transport candidate list did not parse, or holds no usable candidate

    // client connect token pre-send checks
    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = 15,   ///< the connect token fails the client's pre-send checks: it did not parse, or is not for this protocol
    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = 16,   ///< the connect token expired before the first packet was sent; obtain a fresh token before connecting

    // transport candidate list and OS resources
    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = 17,   ///< a pinned transport cannot take port 0 (an ephemeral port)
    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = 18,   ///< every transport in the candidate list refused to start
    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,   ///< an allocation the operation needs failed
    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,   ///< the platform refused a mutex
    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,   ///< the platform refused a thread
    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,   ///< the platform refused a timer

    ROCKETNET_NUM_STATUS_CODES                          = 23     ///< sentinel: the count of codes, OK included; not an operation result
};

// -----------------------------------------
//                        client state
// -----------------------------------------

// What rocketnet_client_state returns. A connect attempt runs up from
// DISCONNECTED through the three handshake states to CONNECTED; every
// negative value is a way the attempt or the connection ended, and the
// client stays in it until it is destroyed.

/** @brief Client connection and handshake states returned by rocketnet_client_state. */
enum rocketnet_client_state_t
{
    /** @brief The connect token expired before the server accepted the client. */
    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            = -7,   // the connect token expired before the server accepted the client
    /** @brief The connect token was refused before the first packet was sent. */
    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            = -6,   // the connect token was refused before the first packet was sent
    /** @brief A connection heard nothing from the server for the token's timeout. */
    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             = -5,   // a connection heard nothing from the server for the token's timeout
    /** @brief The server never answered the third leg of the handshake. */
    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    = -4,   // the server never answered the third leg of the handshake
    /** @brief The server never answered the connection request. */
    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     = -3,   // the server never answered the connection request
    /** @brief The server never answered the cookie request. */
    ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT         = -2,   // the server never answered the cookie request
    /** @brief The server refused the client. */
    ROCKETNET_CLIENT_STATE_CONNECTION_DENIED                = -1,   // the server refused the client
    /** @brief Before the first update, and after rocketnet_client_disconnect. */
    ROCKETNET_CLIENT_STATE_DISCONNECTED                     =  0,   // before the first update, and after rocketnet_client_disconnect
    /** @brief Handshake, first leg. */
    ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE                =  1,   // handshake, first leg
    /** @brief Handshake, second leg. */
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST       =  2,   // handshake, second leg
    /** @brief Handshake, third leg. */
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE      =  3,   // handshake, third leg
    /** @brief Connected: messages flow both ways. */
    ROCKETNET_CLIENT_STATE_CONNECTED                        =  4    // connected: messages flow both ways
};

// -----------------------------------------
//                     query enums
// -----------------------------------------

// What a client counts, read with rocketnet_client_counter. Every value is
// monotonic over the client's life and starts at 0 at create.

/** @brief Monotonic client counter selectors for rocketnet_client_counter. */
enum rocketnet_client_counter_t
{
    /** @brief Packets: UDP datagrams sent, handshake included. */
    ROCKETNET_CLIENT_COUNTER_PACKETS_SENT,          // packets: UDP datagrams sent, handshake included
    /** @brief Packets: UDP datagrams received, handshake included. */
    ROCKETNET_CLIENT_COUNTER_PACKETS_RECEIVED,      // packets: UDP datagrams received, handshake included
    /** @brief Bytes: UDP payload bytes sent. */
    ROCKETNET_CLIENT_COUNTER_BYTES_SENT,            // bytes: UDP payload bytes sent
    /** @brief Bytes: UDP payload bytes received. */
    ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED,        // bytes: UDP payload bytes received
    /** @brief Payloads: packets from the server that opened and carried messages. */
    ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED,     // payloads: packets from the server that opened and carried messages
    /** @brief Records: reliable message records resent on the retransmit timer. */
    ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT,       // records: reliable message records resent on the retransmit timer
    /** @brief Messages: reliable messages received a second time and dropped. */
    ROCKETNET_CLIENT_COUNTER_MESSAGES_DUPLICATE,    // messages: reliable messages received a second time and dropped
    /** @brief Receipts: sends with a receipt that the server acknowledged. */
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED,        // receipts: sends with a receipt that the server acknowledged
    /** @brief Receipts: sends with a receipt that were lost. */
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST,         // receipts: sends with a receipt that were lost
    /** @brief Messages: unreliable messages dropped because the send budget was spent. */
    ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS,          // messages: unreliable messages dropped because the send budget was spent
    /** @brief Hand-offs: sends that left with the next hand-off to the send thread. */
    ROCKETNET_CLIENT_COUNTER_SEND_DEFERRED,         // hand-offs: sends that left with the next hand-off to the send thread
    /** @brief Messages: received messages still held at an update, which takes them back and returns ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. */
    ROCKETNET_CLIENT_COUNTER_MESSAGES_NOT_RELEASED, // messages: received messages still held when an update was refused
    /** @brief Number of selectors; do not pass this sentinel to a query. */
    ROCKETNET_CLIENT_COUNTER_COUNT
};

// The client's connection quality, read with rocketnet_client_stat. Rates
// cover the trailing second.

/** @brief Client connection-quality selectors for rocketnet_client_stat. */
enum rocketnet_client_stat_t
{
    /** @brief Milliseconds: round trip time. */
    ROCKETNET_CLIENT_STAT_RTT,                      // milliseconds: round trip time
    /** @brief Milliseconds: inter-arrival jitter. */
    ROCKETNET_CLIENT_STAT_JITTER,                   // milliseconds: inter-arrival jitter
    /** @brief Percent: received-side loss, from gaps in the server's packet sequence. */
    ROCKETNET_CLIENT_STAT_PACKET_LOSS,              // percent: received-side loss, from gaps in the server's packet sequence
    /** @brief Packets per second sent. */
    ROCKETNET_CLIENT_STAT_PACKETS_SENT_PER_SECOND,  // packets per second sent
    /** @brief Packets per second received. */
    ROCKETNET_CLIENT_STAT_PACKETS_RECEIVED_PER_SECOND, // packets per second received
    /** @brief Kbps (1000 bit/s) sent. */
    ROCKETNET_CLIENT_STAT_BANDWIDTH_SENT,           // kbps (1000 bit/s) sent
    /** @brief Kbps (1000 bit/s) received. */
    ROCKETNET_CLIENT_STAT_BANDWIDTH_RECEIVED,       // kbps (1000 bit/s) received
    /** @brief Number of selectors; do not pass this sentinel to a query. */
    ROCKETNET_CLIENT_STAT_COUNT
};

// What a server counts over all its clients, read with
// rocketnet_server_counter. Every value is monotonic over the server's life
// and starts at 0 at create.

/** @brief Monotonic server-wide counter selectors for rocketnet_server_counter. */
enum rocketnet_server_counter_t
{
    /** @brief Packets: UDP datagrams sent. */
    ROCKETNET_SERVER_COUNTER_PACKETS_SENT,          // packets: UDP datagrams sent
    /** @brief Packets: UDP datagrams received. */
    ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED,      // packets: UDP datagrams received
    /** @brief Bytes: UDP payload bytes sent. */
    ROCKETNET_SERVER_COUNTER_BYTES_SENT,            // bytes: UDP payload bytes sent
    /** @brief Bytes: UDP payload bytes received. */
    ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED,        // bytes: UDP payload bytes received
    /** @brief Connections: clients that completed the handshake. */
    ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED,     // connections: clients that completed the handshake
    /** @brief Requests: connect attempts refused because every client slot was taken. */
    ROCKETNET_SERVER_COUNTER_CLIENTS_DENIED_FULL,   // requests: connect attempts refused because every client slot was taken
    /** @brief Requests: connect attempts dropped by the per-update handshake budget. */
    ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET,// requests: connect attempts dropped by the per-update handshake budget
    /** @brief Payloads: packets from clients that opened and carried messages. */
    ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED,     // payloads: packets from clients that opened and carried messages
    /** @brief Records: reliable message records resent on the retransmit timer. */
    ROCKETNET_SERVER_COUNTER_MESSAGES_RESENT,       // records: reliable message records resent on the retransmit timer
    /** @brief Messages: reliable messages received a second time and dropped. */
    ROCKETNET_SERVER_COUNTER_MESSAGES_DUPLICATE,    // messages: reliable messages received a second time and dropped
    /** @brief Receipts: sends with a receipt that a client acknowledged. */
    ROCKETNET_SERVER_COUNTER_RECEIPTS_ACKED,        // receipts: sends with a receipt that a client acknowledged
    /** @brief Receipts: sends with a receipt that were lost. */
    ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST,         // receipts: sends with a receipt that were lost
    /** @brief Messages: unreliable messages dropped because a client's send budget was spent. */
    ROCKETNET_SERVER_COUNTER_BUDGET_DROPS,          // messages: unreliable messages dropped because a client's send budget was spent
    /** @brief Messages: received messages still held at an update, which takes them back and returns ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. */
    ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED, // messages: received messages still held when an update was refused
    /** @brief Number of selectors; do not pass this sentinel to a query. */
    ROCKETNET_SERVER_COUNTER_COUNT
};

// The server's whole-socket rates over the trailing second, read with
// rocketnet_server_stat.

/** @brief Server-wide rate selectors for rocketnet_server_stat. */
enum rocketnet_server_stat_t
{
    /** @brief Packets per second sent. */
    ROCKETNET_SERVER_STAT_PACKETS_SENT_PER_SECOND,  // packets per second sent
    /** @brief Packets per second received. */
    ROCKETNET_SERVER_STAT_PACKETS_RECEIVED_PER_SECOND, // packets per second received
    /** @brief Kbps (1000 bit/s) sent. */
    ROCKETNET_SERVER_STAT_BANDWIDTH_SENT,           // kbps (1000 bit/s) sent
    /** @brief Kbps (1000 bit/s) received. */
    ROCKETNET_SERVER_STAT_BANDWIDTH_RECEIVED,       // kbps (1000 bit/s) received
    /** @brief Number of selectors; do not pass this sentinel to a query. */
    ROCKETNET_SERVER_STAT_COUNT
};

// What a server counts for one client slot, read with
// rocketnet_server_client_counter. The values cover the slot's current
// connection and go back to 0 when a new connection takes the slot.

/** @brief Per-client-slot counter selectors for rocketnet_server_client_counter. */
enum rocketnet_server_client_counter_t
{
    /** @brief Packets: UDP datagrams sent to this client. */
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT,       // packets: UDP datagrams sent to this client
    /** @brief Packets: UDP datagrams received from this client. */
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_RECEIVED,   // packets: UDP datagrams received from this client
    /** @brief Bytes: UDP payload bytes sent to this client. */
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT,         // bytes: UDP payload bytes sent to this client
    /** @brief Bytes: UDP payload bytes received from this client. */
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED,     // bytes: UDP payload bytes received from this client
    /** @brief Number of selectors; do not pass this sentinel to a query. */
    ROCKETNET_SERVER_CLIENT_COUNTER_COUNT
};

// One client slot's connection quality, read with
// rocketnet_server_client_stat. Rates cover the trailing second.

/** @brief Per-client-slot connection-quality selectors for rocketnet_server_client_stat. */
enum rocketnet_server_client_stat_t
{
    /** @brief Milliseconds: round trip time. */
    ROCKETNET_SERVER_CLIENT_STAT_RTT,                   // milliseconds: round trip time
    /** @brief Milliseconds: deviation of rtt samples above the minimum. */
    ROCKETNET_SERVER_CLIENT_STAT_JITTER,                // milliseconds: deviation of rtt samples above the minimum
    /** @brief Percent: packets sent to this client that were never acknowledged. */
    ROCKETNET_SERVER_CLIENT_STAT_PACKET_LOSS,           // percent: packets sent to this client that were never acknowledged
    /** @brief Kbps (1000 bit/s) sent to this client. */
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_SENT,        // kbps (1000 bit/s) sent to this client
    /** @brief Kbps (1000 bit/s) received from this client. */
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_RECEIVED,    // kbps (1000 bit/s) received from this client
    /** @brief Kbps (1000 bit/s) of sent packets this client acknowledged. */
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_ACKED,       // kbps (1000 bit/s) of sent packets this client acknowledged
    /** @brief Number of selectors; do not pass this sentinel to a query. */
    ROCKETNET_SERVER_CLIENT_STAT_COUNT
};

// -----------------------------------------
//                     opaque handles
// -----------------------------------------

/**
 * @brief Opaque client object created with rocketnet_client_create and destroyed with rocketnet_client_destroy.
 * Call update and send_packets on the main thread. Queue messages between them;
 * join any sending work before send_packets. Do not overlap update with held
 * receive views from the prior update. A caller-provided memory block remains
 * caller-owned and must outlive the client.
 */
struct rocketnet_client_t;
/**
 * @brief Opaque server object created with rocketnet_server_create and destroyed with rocketnet_server_destroy.
 * Call update and send_packets on the main thread. Queue messages between them;
 * join any sending work before send_packets. Do not overlap update with held
 * receive views from the prior update. A caller-provided memory block remains
 * caller-owned and must outlive the server.
 */
struct rocketnet_server_t;

// -----------------------------------------
//                lifecycle and logging
// -----------------------------------------

// The library's version as one integer, the value ROCKETNET_VERSION had when
// the library was built. Compare it with ROCKETNET_VERSION to catch a header
// and a shared library from different releases. Callable before init.
ROCKETNET_EXPORT uint32_t rocketnet_version( void );

// The ABI queries are safe before init, allocate nothing and write no
// caller-owned struct. Type names are the public C struct names. Unknown
// types/fields return SIZE_MAX for size and offset, and -1 for field count.
// They describe the plain structs in this header (the two configs and the
// receipt), not the opaque client and server.

/** @brief Returns the SDK ABI revision. @return ROCKETNET_ABI_VERSION; safe before init and allocation-free. */
ROCKETNET_EXPORT uint32_t rocketnet_abi_version( void );

/**
 * @brief Returns the byte size of a public struct known to the ABI table.
 *
 * This query is allocation-free and safe before initialization. It describes
 * the plain structs in this header, not the opaque handles.
 *
 * @param type NUL-terminated public C struct name; NULL is a counted no-op.
 * @return The struct size, or SIZE_MAX for NULL or an unknown name.
 */
ROCKETNET_EXPORT size_t rocketnet_abi_type_size( const char * type );

/**
 * @brief Returns the number of ABI-described fields in a known public struct.
 * @param type NUL-terminated public C struct name; NULL is a counted no-op.
 * @return The field count, or -1 for NULL or an unknown name.
 */
ROCKETNET_EXPORT int rocketnet_abi_field_count( const char * type );

/**
 * @brief Returns the byte offset of a named field in a known public struct.
 * @param type NUL-terminated public C struct name; NULL is a counted no-op.
 * @param field NUL-terminated field name; NULL is a counted no-op.
 * @return The field offset in bytes, or SIZE_MAX if either name is NULL or the pair is unknown.
 */
ROCKETNET_EXPORT size_t rocketnet_abi_field_offset( const char * type, const char * field );

/**
 * @brief Returns the byte size of a named field in a known public struct.
 * @param type NUL-terminated public C struct name; NULL is a counted no-op.
 * @param field NUL-terminated field name; NULL is a counted no-op.
 * @return The field size in bytes, or SIZE_MAX if either name is NULL or the pair is unknown.
 */
ROCKETNET_EXPORT size_t rocketnet_abi_field_size( const char * type, const char * field );

/**
 * @brief Initializes the library and its platform and cryptography state.
 * @param context Opaque allocator context passed to allocation callbacks; NULL is the default.
 * @return true on success; false if already initialized, platform setup fails, or cryptography setup fails.
 * Call before creating a client or a server; do not race an init or term transition.
 * @code
 * if (!rocketnet_init(NULL)) {
 *     // handle failure
 * }
 * @endcode
 */
ROCKETNET_EXPORT bool rocketnet_init( void * context );

/**
 * @brief Initializes RocketNet and optionally returns the exact status code.
 *
 * This has the same lifecycle and logging behavior as rocketnet_init, which is
 * this call with a NULL status. It writes the exact section-2.1 code to status
 * on every return path when status is non-NULL; a failure logs one error line.
 * No client or server may be created until initialization succeeds.
 *
 * @param context Opaque allocator context retained for platform allocations; NULL is allowed.
 * @param status Optional writable int receiving a ROCKETNET_STATUS_* value; NULL skips the write.
 * @return true on success; false on an existing transition or platform/crypto failure.
 * @code
 * int status;
 * if (!rocketnet_init_ex(NULL, &status)) {
 *     fprintf(stderr, "init failed: %s\n", rocketnet_status_string(status));
 * }
 * @endcode
 */
ROCKETNET_EXPORT bool rocketnet_init_ex( void * context, int * status );

/**
 * @brief Terminates the initialized library after every client and server is destroyed.
 *
 * This wrapper discards the status returned by rocketnet_term_ex. A refused
 * termination logs one error line. Do not call concurrently with SDK work that
 * allocates or depends on platform state.
 */
ROCKETNET_EXPORT void rocketnet_term( void );

/**
 * @brief Terminates the library and returns why termination did or did not occur.
 *
 * Call after every client and server is destroyed and every SDK allocation is
 * released. A failure logs one error line. Do not race termination with other
 * SDK work.
 *
 * @return The section-2.1 code: ROCKETNET_STATUS_OK on termination,
 * ROCKETNET_STATUS_NOT_INITIALIZED if the library is not initialized (nothing
 * to terminate) or is transitioning, or ROCKETNET_STATUS_LIVE_ALLOCATIONS while
 * SDK allocations remain and the term refuses.
 */
ROCKETNET_EXPORT int rocketnet_term_ex( void );

/** @brief Returns the total number of status codes, including OK. @return Count of defined values; allocation-free and safe before init. */
ROCKETNET_EXPORT int rocketnet_num_status_codes( void );

/** @brief Returns the stable symbolic name of a status code. @param status Status integer to look up. @return Static name without the ROCKETNET_STATUS_ prefix for 0 .. count - 1, or NULL (without asserting) when status is negative or greater than the last defined code. Allocation-free and safe before init; returned storage is process-lifetime. */
ROCKETNET_EXPORT const char * rocketnet_status_string( int status );

/**
 * @brief Process-wide callback for a failed internal assert.
 *
 * The library calls it when it is built with asserts enabled (a debug build);
 * with asserts disabled it is never invoked. The default callback logs and
 * traps. rocketnet_assert_function replaces it, and NULL restores the default;
 * set it that way before concurrent SDK calls, since direct writes are not
 * synchronized. The strings are borrowed for the synchronous callback and the
 * line is the source line that failed.
 */
extern ROCKETNET_EXPORT void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line );

/**
 * @brief Installs the process-wide callback for enabled assertion failures.
 *
 * Pass NULL to restore the default callback. The callback receives borrowed
 * condition, function and file strings plus the source line, and runs
 * synchronously on the thread that fires the assertion. Install it before
 * concurrent SDK calls; this setter does not synchronize direct pointer reads.
 *
 * @param function Callback to install, or NULL to restore the default.
 */
ROCKETNET_EXPORT void rocketnet_assert_function( void (*function)( const char * condition, const char * function, const char * file, int line ) );

/**
 * @brief Suppresses or resumes all RocketNet log output.
 *
 * Quiet state is process-wide and atomically read by logging calls. It does not
 * change the configured log level.
 * @param flag true suppresses output; false resumes output subject to the log level.
 */
ROCKETNET_EXPORT void rocketnet_quiet( bool flag );

/**
 * @brief Sets the process-wide maximum log level.
 * @param level An integer from 0 (none) through 5 (spam); other values are ignored.
 */
ROCKETNET_EXPORT void rocketnet_log_level( int level );

/**
 * @brief Reads the current process-wide log level.
 * The level currently in force, so a caller that moves the level for a bounded
 * stretch can restore what it found rather than guessing. This relaxed atomic
 * read is diagnostic only; it is not a synchronization operation.
 * @return A value from ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM.
 */
ROCKETNET_EXPORT int rocketnet_get_log_level( void );

/**
 * @brief Returns the display name for a log level.
 * A replacement log function uses it to render a level the way the default one
 * does. The returned string is static and remains valid for the process lifetime.
 * @param level A log level value.
 * @return "error", "warning", "info", "debug" or "spam" for levels 1 through 5; "???" for none or an unknown value.
 */
ROCKETNET_EXPORT const char * rocketnet_log_level_string( int level );

/**
 * @brief Installs the legacy varargs logging function.
 *
 * The last registration across this function and rocketnet_set_log_callback
 * wins. Passing NULL restores the default sink. The function is called
 * synchronously on each logging thread with an already formatted message via
 * the format string "%s"; it must be thread-safe.
 * @param function Callback to install, or NULL to restore the default sink.
 */
ROCKETNET_EXPORT void rocketnet_log_function( void (*function)( int level, const char * format, ... ) );

/**
 * @brief Receives one preformatted log message through the FFI-safe sink (docs/ERRORS-AND-LOGGING.md 3.2).
 *
 * The callback runs synchronously on the logging thread. The message is
 * NUL-terminated (message[length] is always NUL), length is clamped to at most
 * 1023 bytes, and the pointer borrows the logging frame's stack buffer, valid
 * only until the callback returns. The callback must be thread-safe and must
 * not re-enter rocketnet_set_log_callback or rocketnet_log_function.
 * @param context Caller-owned pointer supplied when the callback is installed.
 * @param level The level passed by the logging call.
 * @param message Borrowed formatted bytes; valid only during this callback.
 * @param length Number of message bytes before the NUL terminator, at most 1023.
 */
typedef void (*rocketnet_log_callback_t)( void * context, int level, const char * message, size_t length );

/**
 * @brief Installs the preformatted logging callback.
 *
 * The last registration across this function and rocketnet_log_function wins.
 * Passing NULL restores the default sink and clears its context. Calls already
 * in flight may finish on their previously captured callback; callbacks must
 * be thread-safe.
 * @param callback Callback to install, or NULL to restore the default sink.
 * @param context Caller-owned pointer passed back to callback; keep it valid until any in-flight callback using it returns.
 */
ROCKETNET_EXPORT void rocketnet_set_log_callback( rocketnet_log_callback_t callback, void * context );

/**
 * @brief Reads the process-wide count of refused API calls.
 * Counts every refused API misuse since the process started: a create before
 * rocketnet_init, a late or partial rocketnet_allocator, and any other call the
 * SDK refuses.
 * @return Relaxed atomic diagnostic count; this read does not synchronize SDK work.
 */
ROCKETNET_EXPORT uint64_t rocketnet_api_misuse_count( void );

/** @brief Returns the protocol identifier derived from the selected release version. @return Hashed release version, or 0 in a development build. Safe before init. */
ROCKETNET_EXPORT uint64_t rocketnet_protocol_version( void );

/** @brief Returns monotonic elapsed time in seconds from an unspecified process-local origin. @return Nondecreasing platform clock value; use the same clock for all time-taking calls. */
ROCKETNET_EXPORT double rocketnet_time( void );

/** @brief Suspends the calling thread for a requested duration. @param seconds Duration in seconds; provide a finite nonnegative value representable by the platform. This blocks the calling thread. */
ROCKETNET_EXPORT void rocketnet_sleep( double seconds );

// -----------------------------------------
//                          allocator
// -----------------------------------------

/**
 * @brief Replaces the allocator before initialization.
 *
 * Configure before init, with no outstanding allocations. Invalid or late
 * replacements are refused: the call returns false, logs one line and counts
 * the refusal in rocketnet_api_misuse_count. Refused after init, while
 * transitioning, with live allocations, or with a partial set (either callback
 * NULL). Callbacks must be thread-safe and return storage aligned for
 * fundamental types. RocketNet adds alignment/ownership metadata, so callbacks
 * receive larger sizes and their original pointer on free.
 *
 * @param malloc_function Allocator callback; NULL is refused (a counted no-op that returns false). Receives context and requested bytes plus SDK overhead.
 * @param free_function Matching deallocator; NULL is refused (a counted no-op that returns false) and receives the original allocation pointer.
 * @return true when both callbacks are installed; false for a partial pair, initialized library, transition, or live allocations.
 */
ROCKETNET_EXPORT bool rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

/** @brief Replaces allocator callbacks and returns the refusal reason. @param malloc_function Non-NULL allocator callback. @param free_function Non-NULL matching deallocator. @return The section-2.1 code: ROCKETNET_STATUS_OK, INVALID_ARGUMENT for a partial pair, ALREADY_INITIALIZED after init or during the transition, or LIVE_ALLOCATIONS with outstanding allocations. A failure logs one error line. Install before init with no live allocations. */
ROCKETNET_EXPORT int rocketnet_allocator_ex( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

// -----------------------------------------
//                             config
// -----------------------------------------

// The client and the server each take one plain config struct. Fill it with
// rocketnet_client_default_config or rocketnet_server_default_config, set
// what the program means, and pass it to create. Precedence at create is
// DEFAULT < CONFIG STRUCT < ENVIRONMENT: every field except a callback has an
// environment variable, named next to the field below, so an operator can
// tune an already-built binary with no rebuild. Every override is logged at
// create with the variable and the value it displaced (the secret key logs
// redacted). A value that does not parse, or is out of range, refuses the
// create with one error line; nothing is ever clamped. An environment
// variable set to the empty string is ignored, with a log line.
//
// Env encodings: integers are decimal; byte fields are base64 of exactly the
// field's length (keygen prints keys in base64); strings are taken as they
// are.
//
// transport is an ORDERED CANDIDATE LIST, comma separated, tried left to right
// at create; the first transport that starts wins, and a list that every
// candidate refuses fails loudly. "auto" (the default, also NULL or the empty
// string) is this platform's recommended order: a linux server tries
// xdp,gso,portable, a macOS server msgx,portable, and everything else
// portable (clients: portable everywhere, plus msgx on macOS). A one-element
// list is a pin: that transport or loud failure. A known name that this
// platform or this endpoint does not have (xdp for a client) is skipped with
// one log line, so one config line deploys to mixed machines; a name that is
// no transport anywhere is a typo and fails loudly, listing the valid names.
// rocketnet_client_transport_name and rocketnet_server_transport_name return
// the transport that won.
//
// The message fields marked WIRE must be identical on both ends; the
// connection verifies them. Validation applies the ranges below and the cross
// rules of docs/CHANNELS.md section 14.
//
// create copies every string it is given, so the strings need to live only
// for the call.
//
// Sized configs (docs/ABI-EVOLUTION.md): the _sized entry points take
// sizeof the struct the caller was compiled against, so a struct can grow at
// its end without breaking a program built against an older header. For a
// non-NULL config, an unknown size is refused with a log line and nothing is
// read or written.

/** @brief Client transport and message settings, copied at rocketnet_client_create. */
struct rocketnet_client_config_t
{
    /** @brief The per-title packet filter, as the server's. Environment override: ROCKETNET_CLIENT_FILTER_VALUE. */
    uint8_t filter_value[4];                        // ROCKETNET_CLIENT_FILTER_VALUE: the per-title packet filter, as the server's
    /** @brief Maximum transport payload bytes, ROCKETNET_MAX_PAYLOAD_BYTES through 305408; 0 selects ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES. Environment override: ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES. */
    int max_payload_bytes;                          // ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    /** @brief Accepted range: 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS. Environment override: ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS. */
    int num_reassembly_slots;                       // ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS: 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    /** @brief The candidate list; NULL is "auto". Environment override: ROCKETNET_CLIENT_TRANSPORT. */
    const char * transport;                         // ROCKETNET_CLIENT_TRANSPORT: the candidate list; NULL is "auto"
    /** @brief Callback when the client state changes; NULL disables it. No environment override. */
    void (*state_change_callback)( void * context, int previous_state, int current_state );   // no env form; NULL is none
    /** @brief Receive timestamp clock read at create; NULL uses the platform clock. No environment override. */
    double (*time_callback)( void * context );      // no env form: the receive stamp clock, read once at create; NULL is the platform clock
    uint64_t step_driven;                           // ROCKETNET_CLIENT_STEP_DRIVEN: 1 runs the client I/O step inside update and send_packets on the caller's thread, with no I/O thread; 0 (the default) keeps today's thread
    /** @brief The send budget, kbps; default 1000; 0 or less disables the budget in this direction (client to server), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused. Environment override: ROCKETNET_CLIENT_SEND_KBPS. */
    int send_kbps;                                  // ROCKETNET_CLIENT_SEND_KBPS: the send budget, kbps; default 1000; 0 or less disables the budget in this direction (client to server), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused
    /** @brief The channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32. Environment override: ROCKETNET_CLIENT_NUM_CHANNELS. */
    int num_channels;                               // ROCKETNET_CLIENT_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    /** @brief Reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024. Environment override: ROCKETNET_CLIENT_WINDOW_MESSAGES. */
    int window_messages;                            // ROCKETNET_CLIENT_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    /** @brief The receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144. Environment override: ROCKETNET_CLIENT_RECEIVE_RING_BYTES. */
    int receive_ring_bytes;                         // ROCKETNET_CLIENT_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    /** @brief The largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144. Environment override: ROCKETNET_CLIENT_MAX_MESSAGE_BYTES. */
    int max_message_bytes;                          // ROCKETNET_CLIENT_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    /** @brief Queued message bytes; 12288 .. 1 << 28; default 800000. Environment override: ROCKETNET_CLIENT_SEND_BUFFER_BYTES. */
    int send_buffer_bytes;                          // ROCKETNET_CLIENT_SEND_BUFFER_BYTES: queued message bytes; 12288 .. 1 << 28; default 800000
    /** @brief Queued messages; 16 .. 1 << 20; default 1024. Environment override: ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES. */
    int send_queue_messages;                        // ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES: queued messages; 16 .. 1 << 20; default 1024
    /** @brief Unacknowledged packets tracked; 16 .. 1 << 16; default 256. Environment override: ROCKETNET_CLIENT_PACKETS_IN_FLIGHT. */
    int packets_in_flight;                          // ROCKETNET_CLIENT_PACKETS_IN_FLIGHT: unacknowledged packets tracked; 16 .. 1 << 16; default 256
    /** @brief Packets send_packets sends per update; 1 .. 256; default 4 Environment override: ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE. */
    int max_packets_per_update;                     // ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE: packets send_packets sends per update; 1 .. 256; default 4
    /** @brief Unreliable messages delivered per update; per update; 1 .. 1 << 16; default 256. Environment override: ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES. */
    int unreliable_deliveries;                      // ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per update; 1 .. 1 << 16; default 256
    /** @brief Sequenced messages held for order; 0 .. 4096; default 16. Environment override: ROCKETNET_CLIENT_SEQUENCED_HOLDS. */
    int sequenced_holds;                            // ROCKETNET_CLIENT_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    /** @brief Maximum queued client send receipts, 16 through 65536 (default 256).
     * A full ring drops its oldest receipt; ROCKETNET_CLIENT_RECEIPT_RING can override it.
     */
    int receipt_ring;                               // ROCKETNET_CLIENT_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    /** @brief The retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100. Environment override: ROCKETNET_CLIENT_RTO_INITIAL_MS. */
    int rto_initial_ms;                             // ROCKETNET_CLIENT_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100
    /** @brief The least retransmit timeout, milliseconds; 1 .. 60000; default 10. Environment override: ROCKETNET_CLIENT_RTO_MIN_MS. */
    int rto_min_ms;                                 // ROCKETNET_CLIENT_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10
    /** @brief The most retransmit timeout, milliseconds; 10 .. 60000; default 250. Environment override: ROCKETNET_CLIENT_RTO_MAX_MS. */
    int rto_max_ms;                                 // ROCKETNET_CLIENT_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250
    /** @brief A reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000. Environment override: ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS. */
    int reliable_timeout_ms;                        // ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
    int simulated_loss_percent;                     // development only: outgoing loss 0..100; 0 is off
    int simulated_latency_ms;                       // development only: outgoing latency in milliseconds; 0 is off
    int simulated_jitter_ms;                        // development only: outgoing jitter in milliseconds; 0 is off
    int simulated_duplicate_percent;                // development only: outgoing duplication 0..100; 0 is off
};

#define ROCKETNET_CLIENT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_config_t, reliable_timeout_ms ) + sizeof( int ), sizeof( void * ) )
#define ROCKETNET_CLIENT_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( void * ) )
#define ROCKETNET_CLIENT_CONFIG_KNOWN_BYTES ROCKETNET_CLIENT_CONFIG_BYTES_1, ROCKETNET_CLIENT_CONFIG_BYTES_2

// The server is configured explicitly: rocketnet_server_default_config gives
// the shape with no keys, and create refuses it until keys are supplied
// (through the struct or the environment), because default keys are how
// every deployment of a protocol ends up sharing one identity.
// rocketnet_server_dev_config fills the published development keys and test
// filter for local work, loudly.
//
// public_address, when set, displaces the create call's address, so the
// address the transport binds and the address connect tokens are checked
// against are always the same one. ROCKETNET_SERVER_PUBLIC_ADDRESS applies to
// the first server create in the process only, and says so in the log, so a
// process with several servers is never redirected wholesale by one variable.

/** @brief Server transport and message settings, copied at rocketnet_server_create. */
struct rocketnet_server_config_t
{
    /** @brief The title's protocol id; connect tokens must carry it. Environment override: ROCKETNET_SERVER_PROTOCOL_ID. */
    uint64_t protocol_id;                           // ROCKETNET_SERVER_PROTOCOL_ID: the title's protocol id; connect tokens must carry it
    /** @brief Accepted range: 1 .. ROCKETNET_MAX_CLIENTS; default 1000. Environment override: ROCKETNET_SERVER_MAX_CLIENTS. */
    int max_clients;                                // ROCKETNET_SERVER_MAX_CLIENTS: 1 .. ROCKETNET_MAX_CLIENTS; default 1000
    /** @brief The per-title packet filter, as the clients'. Environment override: ROCKETNET_SERVER_FILTER_VALUE. */
    uint8_t filter_value[4];                        // ROCKETNET_SERVER_FILTER_VALUE: the per-title packet filter, as the clients'
    /** @brief Verifies connect token signatures Environment override: ROCKETNET_SERVER_BACKEND_PUBLIC_KEY. */
    uint8_t backend_public_key[32];                 // ROCKETNET_SERVER_BACKEND_PUBLIC_KEY: verifies connect token signatures
    /** @brief The server public key used to seal tokens. Environment override: ROCKETNET_SERVER_SERVER_PUBLIC_KEY. */
    uint8_t server_public_key[32];                  // ROCKETNET_SERVER_SERVER_PUBLIC_KEY
    /** @brief Secret, logged redacted. Environment override: ROCKETNET_SERVER_SERVER_SECRET_KEY. */
    uint8_t server_secret_key[32];                  // ROCKETNET_SERVER_SERVER_SECRET_KEY: secret, logged redacted
    /** @brief Xdp and gso only; 1 .. 1024; 0 is the transport default. Environment override: ROCKETNET_SERVER_NUM_XDP_QUEUES. */
    int num_xdp_queues;                             // ROCKETNET_SERVER_NUM_XDP_QUEUES: xdp and gso only; 1 .. 1024; 0 is the transport default
    /** @brief Xdp only, the cores left to the OS; 1 .. 256; 0 is the transport default. Environment override: ROCKETNET_SERVER_NUM_OS_CPUS. */
    int num_os_cpus;                                // ROCKETNET_SERVER_NUM_OS_CPUS: xdp only, the cores left to the OS; 1 .. 256; 0 is the transport default
    /** @brief Maximum transport payload bytes, ROCKETNET_MAX_PAYLOAD_BYTES through 305408; 0 selects ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES. Environment override: ROCKETNET_SERVER_MAX_PAYLOAD_BYTES. */
    int max_payload_bytes;                          // ROCKETNET_SERVER_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    /** @brief Per client; 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS. Environment override: ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS. */
    int num_reassembly_slots;                       // ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS: per client; 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    /** @brief Accepted range: 1 .. 3600; default 60. Environment override: ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS. */
    int max_timeout_seconds;                        // ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS: 1 .. 3600; default 60
    /** @brief Accepted range: 1 .. 60; default 5. Environment override: ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS. */
    int handshake_deadline_seconds;                 // ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS: 1 .. 60; default 5
    /** @brief The candidate list; NULL is "auto". Environment override: ROCKETNET_SERVER_TRANSPORT. */
    const char * transport;                         // ROCKETNET_SERVER_TRANSPORT: the candidate list; NULL is "auto"
    /** @brief "a.b.c.d:port" or "[v6]:port"; NULL is the create call's address. Environment override: ROCKETNET_SERVER_PUBLIC_ADDRESS. */
    const char * public_address;                    // ROCKETNET_SERVER_PUBLIC_ADDRESS: "a.b.c.d:port" or "[v6]:port"; NULL is the create call's address
    /** @brief Each client's send budget, kbps; default 10000; 0 or less disables the budget in this direction (server to each client), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused. Environment override: ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS. */
    int per_client_send_kbps;                       // ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS: each client's send budget, kbps; default 10000; 0 or less disables the budget in this direction (server to each client), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused
    /** @brief The channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32. Environment override: ROCKETNET_SERVER_NUM_CHANNELS. */
    int num_channels;                               // ROCKETNET_SERVER_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    /** @brief Reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024. Environment override: ROCKETNET_SERVER_WINDOW_MESSAGES. */
    int window_messages;                            // ROCKETNET_SERVER_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    /** @brief The receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144. Environment override: ROCKETNET_SERVER_RECEIVE_RING_BYTES. */
    int receive_ring_bytes;                         // ROCKETNET_SERVER_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    /** @brief The largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144. Environment override: ROCKETNET_SERVER_MAX_MESSAGE_BYTES. */
    int max_message_bytes;                          // ROCKETNET_SERVER_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    /** @brief Queued message bytes; per client; 12288 .. 1 << 28; default 800000. Environment override: ROCKETNET_SERVER_SEND_BUFFER_BYTES. */
    int send_buffer_bytes;                          // ROCKETNET_SERVER_SEND_BUFFER_BYTES: queued message bytes; per client; 12288 .. 1 << 28; default 800000
    /** @brief Queued messages; per client; 16 .. 1 << 20; default 1024. Environment override: ROCKETNET_SERVER_SEND_QUEUE_MESSAGES. */
    int send_queue_messages;                        // ROCKETNET_SERVER_SEND_QUEUE_MESSAGES: queued messages; per client; 16 .. 1 << 20; default 1024
    /** @brief Unacknowledged packets tracked; per client; 16 .. 1 << 16; default 256. Environment override: ROCKETNET_SERVER_PACKETS_IN_FLIGHT. */
    int packets_in_flight;                          // ROCKETNET_SERVER_PACKETS_IN_FLIGHT: unacknowledged packets tracked; per client; 16 .. 1 << 16; default 256
    /** @brief Packets send_packets sends per update; per client; 1 .. 256; default 4 Environment override: ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE. */
    int max_packets_per_update;                     // ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE: packets send_packets sends per update; per client; 1 .. 256; default 4
    /** @brief Unreliable messages delivered per update; per client per update; 1 .. 1 << 16; default 256. Environment override: ROCKETNET_SERVER_UNRELIABLE_DELIVERIES. */
    int unreliable_deliveries;                      // ROCKETNET_SERVER_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per client per update; 1 .. 1 << 16; default 256
    /** @brief Sequenced messages held for order; 0 .. 4096; default 16. Environment override: ROCKETNET_SERVER_SEQUENCED_HOLDS. */
    int sequenced_holds;                            // ROCKETNET_SERVER_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    /** @brief Maximum queued receipts per server client, 16 through 65536 (default 256).
     * A full ring drops its oldest receipt; ROCKETNET_SERVER_RECEIPT_RING can override it.
     */
    int receipt_ring;                               // ROCKETNET_SERVER_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    /** @brief The retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100. Environment override: ROCKETNET_SERVER_RTO_INITIAL_MS. */
    int rto_initial_ms;                             // ROCKETNET_SERVER_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100
    /** @brief The least retransmit timeout, milliseconds; 1 .. 60000; default 10. Environment override: ROCKETNET_SERVER_RTO_MIN_MS. */
    int rto_min_ms;                                 // ROCKETNET_SERVER_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10
    /** @brief The most retransmit timeout, milliseconds; 10 .. 60000; default 250. Environment override: ROCKETNET_SERVER_RTO_MAX_MS. */
    int rto_max_ms;                                 // ROCKETNET_SERVER_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250
    /** @brief A reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000. Environment override: ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS. */
    int reliable_timeout_ms;                        // ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
    int simulated_loss_percent;                     // development only: outgoing loss 0..100; 0 is off
    int simulated_latency_ms;                       // development only: outgoing latency in milliseconds; 0 is off
    int simulated_jitter_ms;                        // development only: outgoing jitter in milliseconds; 0 is off
    int simulated_duplicate_percent;                // development only: outgoing duplication 0..100; 0 is off
};

#define ROCKETNET_SERVER_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_config_t, reliable_timeout_ms ) + sizeof( int ), sizeof( void * ) )
#define ROCKETNET_SERVER_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( void * ) )
#define ROCKETNET_SERVER_CONFIG_KNOWN_BYTES ROCKETNET_SERVER_CONFIG_BYTES_1, ROCKETNET_SERVER_CONFIG_BYTES_2

// -----------------------------------------
//                           messages
// -----------------------------------------

// A message is bytes, a pointer and a length (docs/CHANNELS.md). Each client
// and each server is one block made at create -- the object, its connections
// and one message region per connection -- and allocates nothing after
// create. memory is optional: NULL makes the one allocation, and a caller
// block of at least the _required_memory bytes, aligned to
// ROCKETNET_ALLOCATION_ALIGNMENT, makes none (the transport underneath still
// allocates its own at create).
//
// time is the game's clock, a double in seconds, monotonic; pass the same
// clock to every call that takes one. The library reads no clock of its own
// on these paths, so a game owns time and a test can hold it still.
//
// The update order, on the main thread: update (packets in, acks into the
// channels, received messages into deliveries, then the checks that
// disconnect), receive and release, send (or start, write, finish), then
// send_packets (each connection's packets, under its send budget, out to the
// wire).
//
// Sending: start, write in place, finish is the only send path. start returns
// a pointer into the connection's region for the message, write the message
// there once (copy it in when the caller already holds a buffer), and
// finish(bytes_written) queues it and returns its receipt serial, nonzero; a
// NULL start is a refused send, 0. The pointer is valid until finish or abort;
// abort gives the room back. One start is pending per object at a time.
// reliability is one of ROCKETNET_UNRELIABLE,
// ROCKETNET_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABLE,
// ROCKETNET_RELIABLE_ORDERED, ROCKETNET_RELIABLE_SEQUENCED,
// ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT, ROCKETNET_RELIABLE_WITH_ACK_RECEIPT
// and ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT. An unreliable send too
// big for one packet's record is promoted to its reliable twin, as RakNet's
// ReliabilityLayer does, and counted (MESSAGE_PROMOTED_UNRELIABLE,
// MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED,
// MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT); the receiver sees the twin's
// wire code. Plain UNRELIABLE is one record in one packet up to 1411 bytes
// and promotes to RELIABLE above that.
// UNRELIABLE_SEQUENCED is one record up to 1405 bytes (1404 when the channel
// is WIDE) and promotes to RELIABLE_SEQUENCED past that, keeping its channel.
// UNRELIABLE_WITH_ACK_RECEIPT is the code-0 wire form up to 1411 bytes and
// promotes to RELIABLE_WITH_ACK_RECEIPT semantics (the code-2 block path)
// above that, keeping its ACKED-only receipt. The reliable types and promoted
// sends are bounded by the config's max_message_bytes; a send above it is
// refused in every unreliable mode, with no second record-length refusal.
//
// A send the connection's region has no room for ends the connection: the
// server disconnects that client, the client disconnects itself, with the
// reason ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED. The send budget
// governs the wire, never the queue: a reliable message waits for budget and
// is never dropped; an unreliable one over budget is dropped and counted
// (the BUDGET_DROPS counters).
//
// Receiving: receive hands out the next message of the update on a channel
// as a const view and its length, valid until the next update. Release every
// received message before the next update; update with one still held
// asserts in a debug build and returns ROCKETNET_ERROR_MESSAGE_NOT_RELEASED
// in a release build. Copy what is kept past the update.

/** @brief One outcome for a send's nonzero receipt serial.
 * Receipts are copied into caller storage after an update. A full receipt ring
 * drops its oldest entry; copy any result needed beyond the next update.
 */
struct rocketnet_receipt_t
{
    /** @brief Identify the send whose outcome is reported. */
    uint32_t serial;
    /** @brief State whether the peer transport acknowledged or missed it. */
    uint32_t status;            // ROCKETNET_RECEIPT_ACKED or ROCKETNET_RECEIPT_LOSS
};

// -----------------------------------------
//                      connect tokens
// -----------------------------------------

// The backend's Ed25519 signing keypair: public_key is
// ROCKETNET_BACKEND_PUBLIC_KEY_BYTES long and secret_key is
// ROCKETNET_BACKEND_SECRET_KEY_BYTES long. The backend keeps the secret key
// and gives the public key to every server. Returns ROCKETNET_STATUS_OK, or
// ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL pointer, or
// ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the crypto library refuses.
ROCKETNET_EXPORT int rocketnet_generate_backend_keypair( uint8_t * public_key, uint8_t * secret_key );

// Derives the backend's Ed25519 signing keypair from a caller seed: seed is
// ROCKETNET_BACKEND_SEED_BYTES long, and public_key and secret_key are as
// above. The same seed always gives the same keypair. Returns
// ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
// pointer, or ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the crypto library
// refuses. Secret keys are never logged.
ROCKETNET_EXPORT int rocketnet_generate_backend_keypair_from_seed( const uint8_t * seed, uint8_t * public_key, uint8_t * secret_key );

// The server's X25519 sealing keypair: public_key and secret_key are each
// ROCKETNET_SERVER_PUBLIC_KEY_BYTES and ROCKETNET_SERVER_SECRET_KEY_BYTES
// long. The server keeps the secret key; the backend seals tokens to the
// public key. Returns the same codes as the backend keypair call.
ROCKETNET_EXPORT int rocketnet_generate_server_keypair( uint8_t * public_key, uint8_t * secret_key );

// Mints a production connect token from the backend's secret key and the
// server's public key. The token carries client_id, protocol_id, the server
// addresses, create_timestamp, expire_timestamp, timeout_seconds and
// user_data, and fresh session keys. server_address_strings holds
// server_address_count entries (1 or 2), at most one of each family; user_data
// is ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES long, or NULL for all zeros. The
// output is ROCKETNET_CONNECT_TOKEN_BYTES long and connect_token_bytes is its
// size; a short buffer is refused and nothing is written. Reads no clock.
// Returns ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT,
// ROCKETNET_STATUS_INVALID_PROTOCOL_ID, ROCKETNET_STATUS_INVALID_ADDRESS or
// ROCKETNET_STATUS_CRYPTO_INIT_FAILED. Secret keys are never logged.
ROCKETNET_EXPORT int rocketnet_write_production_connect_token( const uint8_t * backend_secret_key,
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

// -----------------------------------------
//                             client
// -----------------------------------------

// The client connects with a connect token, never an address: the token names
// the server, carries the keys, and is the credential -- minted by a backend
// in production, or locally by rocketnet_write_dev_connect_token against the
// published development keys.

// The unsized form writes the revision-1 prefix. Use the sized form with
// sizeof(*config) to initialize fields added after revision 1.
// a NULL config is a counted no-op.
/**
 * @brief Fill a client config with library defaults.
 * @param config Writable output config; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_default_config( struct rocketnet_client_config_t * config );

// the defaults for a caller whose struct is config_bytes long. 1 written; 0
// refused, nothing written. a NULL config is a counted no-op that returns 0.
/**
 * @brief Fill a client config of a known ABI size with defaults.
 * @param config Writable output config; NULL is refused.
 * @param config_bytes Caller config size in bytes; unknown sizes are refused.
 * @return 1 if written, or 0 if refused without writing.
 */
ROCKETNET_EXPORT int rocketnet_client_default_config_sized( struct rocketnet_client_config_t * config, size_t config_bytes );

// 1 when create would accept the config's own values, else 0 with a log line
// naming the rule. The environment is not read. NULL means the defaults.
/**
 * @brief Check client config values without reading environment overrides.
 * @param config Input config; NULL selects defaults. Environment overrides are not read.
 * @return 1 if valid, otherwise 0 with a logged rule.
 */
ROCKETNET_EXPORT int rocketnet_client_validate_config( const struct rocketnet_client_config_t * config );
// Validates a known-size client config; an unknown non-NULL size logs and returns 0.
ROCKETNET_EXPORT int rocketnet_client_validate_config_sized( const struct rocketnet_client_config_t * config, size_t config_bytes );

// the bytes a caller memory block for this client needs; 0 for a refused
// config. NULL means the defaults.
/**
 * @brief Measure the aligned client message block required by a config.
 * @param config Input config; NULL selects defaults.
 * @return Required bytes, or 0 for an invalid config.
 */
ROCKETNET_EXPORT uint64_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );
// Queries a known-size client config; an unknown non-NULL size logs and returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_client_required_memory_sized( const struct rocketnet_client_config_t * config, size_t config_bytes );

#if ROCKETNET_DEVELOPMENT
// Mints a development connect token for a server at this address, signed with
// the published development backend key and sealed to the published development
// server key. Returns 1, or 0 for an address that does not parse.
// a NULL server_address_string or connect_token is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_write_dev_connect_token( const char * server_address_string, uint64_t client_id, uint64_t unix_timestamp, uint8_t * connect_token );
#endif // #if ROCKETNET_DEVELOPMENT

// connect_token is ROCKETNET_CONNECT_TOKEN_BYTES long. create reads no clock:
// the connect attempt starts at the first update. NULL config is the
// defaults. a NULL connect_token is a counted no-op that returns NULL.
/**
 * @brief Create a client from a connect token and optional config.
 * @param context Allocator and callback context supplied at create.
 * @param connect_token Input connect token of ROCKETNET_CONNECT_TOKEN_BYTES; NULL is refused.
 * @param config Input config copied at create; NULL selects defaults.
 * @param memory Optional caller-owned aligned block; NULL requests library allocation.
 * @param memory_bytes Size of a supplied memory block in bytes; ignored when memory is NULL.
 * @return Owned client handle, or NULL on refusal; destroy it with rocketnet_client_destroy.
 */
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, void * memory, uint64_t memory_bytes );

// create with a config config_bytes long; if config is non-NULL, an unknown
// size is refused with a log line before any other check.
/**
 * @brief Create a client with an ABI-sized config.
 * @param context Allocator and callback context supplied at create.
 * @param connect_token Input connect token of ROCKETNET_CONNECT_TOKEN_BYTES; NULL is refused.
 * @param config Input config copied at create; NULL selects defaults.
 * @param config_bytes Caller config size in bytes; checked only when config is non-NULL, and unknown sizes are refused.
 * @param memory Optional caller-owned aligned block; NULL requests library allocation.
 * @param memory_bytes Size of a supplied memory block in bytes; ignored when memory is NULL.
 * @return Owned client handle, or NULL on refusal; destroy it with rocketnet_client_destroy.
 */
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create_sized( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes );

// a NULL client is a counted no-op.
/**
 * @brief Destroy a client and free its owned resources without sending a disconnect packet.
 * Hands off committed payloads, retires keys, stops I/O, and frees state; it sends no disconnect packet, so the server frees the slot by timeout. Call rocketnet_client_disconnect before rocketnet_client_destroy for a clean disconnect.
 * @param client Client handle returned by create; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL client is a counted no-op that returns ROCKETNET_MESSAGE_OK.
/**
 * @brief Pump the client transport and deliver received messages at the start of a frame.
 * Call before receive and release, game simulation, and queuing this frame's outgoing messages. Release all views from the previous update first.
 * @param client Client created by rocketnet_client_create; NULL is a counted no-op.
 * @param time Monotonic game time in seconds, shared with other time-taking calls.
 * @return ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED if a received view remains held; NULL returns OK.
 */
ROCKETNET_EXPORT int rocketnet_client_update( struct rocketnet_client_t * client, double time );

// a NULL client is a counted no-op.
/**
 * @brief Send queued client messages in packets within the connection's send budget.
 * Call last in each frame, after update, receive and release, simulation, and
 * start/finish of all outgoing messages. Calling early leaves messages queued
 * afterward until the next frame and adds one frame of latency.
 * Synchronize any message-writing threads before this main-thread call.
 * @param client Client created by rocketnet_client_create; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

// Ends the connection: the disconnect is sent and the state becomes DISCONNECTED. a NULL client is a counted no-op.
/**
 * @brief Disconnect a client deliberately.
 * @param client Client handle returned by create; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

// a NULL client is a counted no-op that returns ROCKETNET_CLIENT_STATE_DISCONNECTED.
/**
 * @brief Read the client connection or handshake state.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @return A rocketnet_client_state_t value; NULL reports DISCONNECTED.
 */
ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

// the name of a state ("connected"), or "???" for a value that is not one.
/**
 * @brief Get the static text name of a client state.
 * @param state Client state enum value; an unknown value returns the static string ???.
 * @return Static text for a known state, or ???; do not free it.
 */
ROCKETNET_EXPORT const char * rocketnet_client_state_name( int state );

// ROCKETNET_MESSAGE_ERROR_* the client ended its connection with, NONE while it has not. a NULL client is a counted no-op that returns NONE.
/**
 * @brief Read the message-layer reason that ended the client connection.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @return A ROCKETNET_MESSAGE_ERROR_* value, or NONE before a message error ends the connection.
 */
ROCKETNET_EXPORT int rocketnet_client_disconnect_reason( const struct rocketnet_client_t * client );

// the client's slot on the server, valid while connected. a NULL client is a counted no-op that returns 0.
/**
 * @brief Read the server slot assigned to this connected client.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @return Server slot while connected; check state to distinguish a valid slot from disconnection.
 */
ROCKETNET_EXPORT int rocketnet_client_index( const struct rocketnet_client_t * client );

// the transport that won the candidate list at create ("portable", "msgx"). a NULL client is a counted no-op that returns NULL.
/**
 * @brief Identify the selected client transport.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @return Borrowed name valid while the client exists, or NULL for a NULL handle.
 */
ROCKETNET_EXPORT const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client );

// the first step of the send: a pointer into the connection's region for a
// message of bytes, or NULL when refused. Write the message there once, or
// copy it in when the caller already holds a buffer:
//
//     uint8_t * out = rocketnet_client_start( client, channel, reliability, bytes );
//     if ( out )
//     {
//         memcpy( out, data, bytes );
//         serial = rocketnet_client_finish( client, bytes );
//     }
//
// The pointer is valid until finish or abort; one start is pending per object
// at a time. a NULL client is a counted no-op that returns NULL.
/**
 * @brief Reserve writable client message bytes without copying.
 * Only one start may be pending per object; write the bytes, then finish or abort.
 * @code
 * uint8_t * out = rocketnet_client_start( client, channel, reliability, bytes );
 * if ( out )
 * {
 *     memcpy( out, data, bytes );
 *     serial = rocketnet_client_finish( client, bytes );
 * }
 * @endcode
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @param channel Channel index from 0 through configured num_channels - 1.
 * @param reliability One enabled ROCKETNET_UNRELIABLE or ROCKETNET_RELIABLE family value.
 * @param bytes Positive message byte count within the selected reliability type’s configured limit.
 * @return Writable region until finish or abort, or NULL on refusal; never free it.
 */
ROCKETNET_EXPORT uint8_t * rocketnet_client_start( struct rocketnet_client_t * client, int channel, int reliability, int bytes );

// the queued message's receipt serial, nonzero; 0 when no start is pending or bytes_written is outside 1 to the bytes started. a NULL client is a counted no-op that returns 0.
/**
 * @brief Queue bytes written into a pending client reservation.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @param bytes_written Byte count from 1 through the reserved size; an invalid count leaves the reservation pending.
 * @return Nonzero send serial on success, or 0 for a missing reservation or invalid count.
 */
ROCKETNET_EXPORT uint32_t rocketnet_client_finish( struct rocketnet_client_t * client, int bytes_written );

// a NULL client is a counted no-op.
/**
 * @brief Cancel a pending client message reservation.
 * @param client Client handle returned by create; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_abort( struct rocketnet_client_t * client );

// 1 with the next message on channel, 0 when there is none. a NULL client, data or bytes is a counted no-op that returns 0.
/**
 * @brief Take the next message delivered on a client channel.
 * Release every successful view before the next update; copy bytes kept longer.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @param channel Channel index from 0 through configured num_channels - 1.
 * @param data Writable output pointer for a borrowed const view; NULL is refused.
 * @param bytes Writable output pointer for message length in bytes; NULL is refused.
 * @return 1 with a borrowed message view, or 0 when none is available or an argument is invalid.
 */
ROCKETNET_EXPORT int rocketnet_client_receive( struct rocketnet_client_t * client, int channel, const uint8_t ** data, int * bytes );

// a NULL client or data is a counted no-op; a pointer that is not a held view is a counted misuse.
/**
 * @brief Release a borrowed client message view.
 * Release each successful view exactly once before the next update; do not free it.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @param data Borrowed view returned by a successful receive on this client; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_release( struct rocketnet_client_t * client, const uint8_t * data );

/** @brief Copy this update's client receipt outcomes into caller storage.
 * Call after update and before the next update, without a concurrent update.
 * Repeated calls copy the same visible entries; the client owns the internal
 * list, and the caller owns the output array.
 * @param client Client object; NULL returns 0.
 * @param out Writable array with room for capacity entries; NULL returns 0.
 * @param capacity Array length in entries; values below 1 return 0.
 * @return Entries copied, from 0 through capacity. Zero means none are visible
 * or an input is invalid; it is not an error status.
 */
ROCKETNET_EXPORT int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity );

// 1 when the budget took the new rate, from now on. 0 or less disables the budget in this direction, client to server (NOT RECOMMENDED); a positive value up to ROCKETNET_SEND_BUDGET_MAX_KBPS caps it; above the maximum returns 0 with the rate unchanged. a NULL client is a counted no-op that returns 0.
/**
 * @brief Change the client-to-server wire send budget.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @param kbps Decimal kilobits per second; nonpositive disables this direction’s budget, and values above ROCKETNET_SEND_BUDGET_MAX_KBPS are refused.
 * @return 1 if accepted for subsequent sends, or 0 if refused with the old rate retained.
 */
ROCKETNET_EXPORT int rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

// a counter by its enum; 0 for a value outside the enum. a NULL client is a counted no-op that returns 0.
/**
 * @brief Read a monotonic client counter.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @param counter Counter enum selector; invalid values return 0.
 * @return Counter value in the enum’s units; 0 for NULL or an invalid selector.
 */
ROCKETNET_EXPORT uint64_t rocketnet_client_counter( const struct rocketnet_client_t * client, enum rocketnet_client_counter_t counter );

// a stat by its enum; 0 for a value outside the enum. a NULL client is a counted no-op that returns 0.
/**
 * @brief Read a client connection-quality measurement.
 * @param client Client handle returned by create; NULL is a counted no-op.
 * @param stat Stat enum selector; invalid values return 0.
 * @return Measurement in the enum’s units; 0 for NULL or an invalid selector.
 */
ROCKETNET_EXPORT double rocketnet_client_stat( const struct rocketnet_client_t * client, enum rocketnet_client_stat_t stat );

// -----------------------------------------
//                             server
// -----------------------------------------

// Clients are identified by index, 0 .. max_clients - 1, the slot each
// connection takes.

// The unsized form writes the revision-1 prefix. Use the sized form with
// sizeof(*config) to initialize fields added after revision 1.
// a NULL config is a counted no-op.
/**
 * @brief Fill a server config with defaults and no production keys.
 * @param config Writable output config; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_default_config( struct rocketnet_server_config_t * config );

// the defaults for a caller whose struct is config_bytes long. 1 written; 0
// refused, nothing written. a NULL config is a counted no-op that returns 0.
/**
 * @brief Fill a server config of a known ABI size with defaults.
 * @param config Writable output config; NULL is refused.
 * @param config_bytes Caller config size in bytes; unknown sizes are refused.
 * @return 1 if written, or 0 if refused without writing.
 */
ROCKETNET_EXPORT int rocketnet_server_default_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes );

// the defaults with the published development keys and test filter. The
// unsized form writes the revision-1 prefix; use the sized form for current fields.
// a NULL config is a counted no-op.
/**
 * @brief Fill a server config with published development keys and test filter.
 * @param config Writable output config; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_dev_config( struct rocketnet_server_config_t * config );
// Writes development defaults for a known server config size; unknown sizes log and are untouched.
ROCKETNET_EXPORT int rocketnet_server_dev_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes );

// 1 when create would accept the config's own values, else 0 with a log line
// naming the rule. The environment is not read and the keys are not checked.
// NULL means the defaults.
/**
 * @brief Check server config values without reading environment overrides.
 * @param config Input config; NULL selects defaults without production keys. Environment overrides are not read.
 * @return 1 if valid, otherwise 0 with a logged rule.
 */
ROCKETNET_EXPORT int rocketnet_server_validate_config( const struct rocketnet_server_config_t * config );
// Validates a known-size server config; an unknown non-NULL size logs and returns 0.
ROCKETNET_EXPORT int rocketnet_server_validate_config_sized( const struct rocketnet_server_config_t * config, size_t config_bytes );

// the bytes a caller memory block for this server needs, at the config's
// max_clients; 0 for a refused config. NULL means the defaults.
/**
 * @brief Measure the aligned server message block for all configured slots.
 * @param config Input config; NULL selects defaults.
 * @return Required bytes, or 0 for an invalid config.
 */
ROCKETNET_EXPORT uint64_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );
// Queries a known-size server config; an unknown non-NULL size logs and returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_required_memory_sized( const struct rocketnet_server_config_t * config, size_t config_bytes );

// address is where clients reach the server, "a.b.c.d:port" or "[v6]:port"; a
// port of 0 takes an ephemeral port (read it back with rocketnet_server_port).
// NULL config is the defaults, which create refuses until keys are given. a
// NULL address is a counted no-op that returns NULL.
/**
 * @brief Create a server bound to an address with configured keys and message limits.
 * @param context Allocator and callback context supplied at create.
 * @param address Server bind address as a.b.c.d:port or [v6]:port; NULL is refused and port 0 requests an ephemeral port.
 * @param config Input config copied at create; NULL selects defaults, which lack production keys.
 * @param memory Optional caller-owned aligned block; NULL requests library allocation.
 * @param memory_bytes Size of a supplied memory block in bytes; ignored when memory is NULL.
 * @return Owned server handle, or NULL on refusal; destroy it with rocketnet_server_destroy.
 */
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, void * memory, uint64_t memory_bytes );

// create with a config config_bytes long; if config is non-NULL, an unknown
// size is refused with a log line before any other check.
/**
 * @brief Create a server with an ABI-sized config.
 * @param context Allocator and callback context supplied at create.
 * @param address Server bind address as a.b.c.d:port or [v6]:port; NULL is refused and port 0 requests an ephemeral port.
 * @param config Input config copied at create; NULL selects defaults, which lack production keys.
 * @param config_bytes Caller config size in bytes; checked only when config is non-NULL, and unknown sizes are refused.
 * @param memory Optional caller-owned aligned block; NULL requests library allocation.
 * @param memory_bytes Size of a supplied memory block in bytes; ignored when memory is NULL.
 * @return Owned server handle, or NULL on refusal; destroy it with rocketnet_server_destroy.
 */
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create_sized( void * context, const char * address, const struct rocketnet_server_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes );

// Frees server state and sockets without sending disconnect packets; clients reach their timeout. a NULL server is a counted no-op.
/**
 * @brief Destroy a server and free its owned state and sockets without sending disconnect packets.
 * Frees server state and sockets and sends no disconnect packet; connected clients reach their timeout.
 * @param server Server handle returned by create; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL server is a counted no-op that returns ROCKETNET_MESSAGE_OK.
/**
 * @brief Pump the server transport and deliver received messages at the start of a frame.
 * Call before receive and release, game simulation, and queuing this frame's outgoing messages. Release all views from the previous update first.
 * @param server Server created by rocketnet_server_create; NULL is a counted no-op.
 * @param time Monotonic game time in seconds, shared with other time-taking calls.
 * @return ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED if a received view remains held; NULL returns OK.
 */
ROCKETNET_EXPORT int rocketnet_server_update( struct rocketnet_server_t * server, double time );

// a NULL server is a counted no-op.
/**
 * @brief Send queued server messages to clients within each connection's send budget.
 * Call last in each frame, after update, receive and release, simulation, and
 * start/finish of all outgoing messages. Calling early leaves messages queued
 * afterward until the next frame and adds one frame of latency.
 * Synchronize any message-writing threads before this main-thread call.
 * @param server Server created by rocketnet_server_create; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

// The port the server bound: the address's port, or the ephemeral port a
// port of 0 was given. Build a token address from it in tests and tools. a
// NULL server is a counted no-op that returns 0.
/**
 * @brief Read the actual port bound by the server.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @return Bound port, including the chosen ephemeral port, or 0 for NULL.
 */
ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

// the transport that won the candidate list at create ("xdp", "gso", "msgx", "portable"). a NULL server is a counted no-op that returns NULL.
/**
 * @brief Identify the selected server transport.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @return Borrowed name valid while the server exists, or NULL for a NULL handle.
 */
ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

// a NULL server is a counted no-op that returns 0.
/**
 * @brief Read the configured number of server client slots.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @return Maximum simultaneous client count, or 0 for NULL.
 */
ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

// a NULL server is a counted no-op that returns 0.
/**
 * @brief Count currently connected server client slots.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @return Number of connected clients, or 0 for NULL.
 */
ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

// 1 when a client holds the slot. a NULL server is a counted no-op that returns 0.
/**
 * @brief Check whether a server slot holds a connected client.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @return 1 for a connected slot, otherwise 0.
 */
ROCKETNET_EXPORT int rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index );

// the client id from the connect token of the client in the slot, 0 for a free slot. a NULL server is a counted no-op that returns 0.
/**
 * @brief Read the connect-token identity of a client slot.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @return Client ID, or 0 for a free or invalid slot.
 */
ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index );

// Disconnects one client deliberately and frees its slot. Why is the game's
// decision. a NULL server is a counted no-op.
/**
 * @brief Disconnect one server client and free its slot.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; an invalid slot does nothing.
 */
ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

// ROCKETNET_MESSAGE_ERROR_* the slot's last connection ended with, until the slot's next connection. a NULL server is a counted no-op that returns NONE.
/**
 * @brief Read the last message-layer disconnect reason for a server slot.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @return A ROCKETNET_MESSAGE_ERROR_* value retained until that slot’s next connection, or NONE.
 */
ROCKETNET_EXPORT int rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index );

// the first step of the send: a pointer into the connection's region for a
// message of bytes, or NULL when refused. Write the message there once, or
// copy it in when the caller already holds a buffer:
//
//     uint8_t * out = rocketnet_server_start( server, client_index, channel, reliability, bytes );
//     if ( out )
//     {
//         memcpy( out, data, bytes );
//         serial = rocketnet_server_finish( server, client_index, bytes );
//     }
//
// The pointer is valid until finish or abort; one start is pending per object
// at a time. a NULL server is a counted no-op that returns NULL.
/**
 * @brief Reserve writable message bytes for one connected server client.
 * Only one start may be pending per object; write the bytes, then finish or abort.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @param channel Channel index from 0 through configured num_channels - 1.
 * @param reliability One enabled ROCKETNET_UNRELIABLE or ROCKETNET_RELIABLE family value.
 * @param bytes Positive message byte count within the selected reliability type’s configured limit.
 * @return Writable region until finish or abort, or NULL on refusal; never free it.
 */
ROCKETNET_EXPORT uint8_t * rocketnet_server_start( struct rocketnet_server_t * server, int client_index, int channel, int reliability, int bytes );

// the queued message's receipt serial, nonzero; 0 when no start is pending, bytes_written is outside 1 to the bytes started, or client_index is not a connected client. a NULL server is a counted no-op that returns 0.
/**
 * @brief Queue bytes written into a pending server reservation.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @param bytes_written Byte count from 1 through the reserved size; an invalid count leaves the reservation pending.
 * @return Nonzero send serial on success, or 0 for a missing reservation, wrong slot, or invalid count.
 */
ROCKETNET_EXPORT uint32_t rocketnet_server_finish( struct rocketnet_server_t * server, int client_index, int bytes_written );

// a NULL server is a counted no-op.
/**
 * @brief Cancel a pending server message reservation.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; an invalid slot does nothing.
 */
ROCKETNET_EXPORT void rocketnet_server_abort( struct rocketnet_server_t * server, int client_index );

// Takes the caller's data and copies it to every connected client, one copy
// per connection. Returns the number of clients it was queued for; a client
// that refuses is skipped. a NULL server or data is a counted no-op that
// returns 0.
/**
 * @brief Copy and queue a message for each connected client that accepts it.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param channel Channel index from 0 through configured num_channels - 1.
 * @param reliability One enabled ROCKETNET_UNRELIABLE or ROCKETNET_RELIABLE family value.
 * @param data Input message bytes copied into each accepting client region; NULL is refused.
 * @param bytes Positive input byte count within the selected reliability type’s configured limit.
 * @return Number of connected slots queued; a refusing slot is skipped.
 */
ROCKETNET_EXPORT int rocketnet_server_broadcast( struct rocketnet_server_t * server, int channel, int reliability, const uint8_t * data, int bytes );

// 1 with the next message on channel, 0 when there is none. a NULL server, data or bytes is a counted no-op that returns 0.
/**
 * @brief Take the next message for a server client on a channel.
 * Release every successful view before the next update; copy bytes kept longer.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @param channel Channel index from 0 through configured num_channels - 1.
 * @param data Writable output pointer for a borrowed const view; NULL is refused.
 * @param bytes Writable output pointer for message length in bytes; NULL is refused.
 * @return 1 with a borrowed message view, or 0 when none is available or an argument is invalid.
 */
ROCKETNET_EXPORT int rocketnet_server_receive( struct rocketnet_server_t * server, int client_index, int channel, const uint8_t ** data, int * bytes );

// a NULL server or data is a counted no-op; a pointer that is not a held view is a counted misuse.
/**
 * @brief Release a borrowed server message view.
 * Release each successful view exactly once before the next update; do not free it.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; an invalid slot does nothing.
 * @param data Borrowed view returned by a successful receive for this slot; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_release( struct rocketnet_server_t * server, int client_index, const uint8_t * data );

/** @brief Copy one connected server client's receipt outcomes.
 * Call after update and before the next update, without a concurrent update.
 * Repeated calls copy the same visible entries; the server owns the internal
 * list, and the caller owns the output array.
 * @param server Server object; NULL returns 0.
 * @param client_index Slot index in [0, configured max_clients); an invalid or
 * disconnected slot returns 0.
 * @param out Writable array with room for capacity entries; NULL returns 0.
 * @param capacity Array length in entries; values below 1 return 0.
 * @return Entries copied, from 0 through capacity. Zero means none are visible
 * or an input is invalid; it is not an error status.
 */
ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity );

// 1 when the client's budget took the new rate, from now on. 0 or less disables the budget in this direction, server to that client (NOT RECOMMENDED); a positive value up to ROCKETNET_SEND_BUDGET_MAX_KBPS caps it; above the maximum returns 0 with the rate unchanged. a NULL server is a counted no-op that returns 0.
/**
 * @brief Change the server-to-client wire send budget for one slot.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @param kbps Decimal kilobits per second; nonpositive disables this direction’s budget, and values above ROCKETNET_SEND_BUDGET_MAX_KBPS are refused.
 * @return 1 if accepted for subsequent sends, or 0 if refused with the old rate retained.
 */
ROCKETNET_EXPORT int rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps );

// a counter by its enum; 0 for a value outside the enum. a NULL server is a counted no-op that returns 0.
/**
 * @brief Read a monotonic server-wide counter.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param counter Counter enum selector; invalid values return 0.
 * @return Counter in the enum’s units; 0 for NULL or invalid selector.
 */
ROCKETNET_EXPORT uint64_t rocketnet_server_counter( const struct rocketnet_server_t * server, enum rocketnet_server_counter_t counter );

// a stat by its enum; 0 for a value outside the enum. a NULL server is a counted no-op that returns 0.
/**
 * @brief Read a server-wide rate measurement.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param stat Stat enum selector; invalid values return 0.
 * @return Rate in the enum’s units over the trailing second; 0 for NULL or invalid selector.
 */
ROCKETNET_EXPORT double rocketnet_server_stat( const struct rocketnet_server_t * server, enum rocketnet_server_stat_t stat );

// one client slot's counter by its enum; 0 for a free slot or a value outside the enum. a NULL server is a counted no-op that returns 0.
/**
 * @brief Read a counter for one server client slot.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @param counter Counter enum selector; invalid values return 0.
 * @return Slot counter; 0 for free or invalid slot or selector.
 */
ROCKETNET_EXPORT uint64_t rocketnet_server_client_counter( const struct rocketnet_server_t * server, int client_index, enum rocketnet_server_client_counter_t counter );

// one client slot's stat by its enum; 0 for a free slot or a value outside the enum. a NULL server is a counted no-op that returns 0.
/**
 * @brief Read a connection-quality measurement for one server client slot.
 * @param server Server handle returned by create; NULL is a counted no-op.
 * @param client_index Server slot index from 0 through max_clients - 1; invalid slots are refused or return 0.
 * @param stat Stat enum selector; invalid values return 0.
 * @return Slot measurement; 0 for free or invalid slot or selector.
 */
ROCKETNET_EXPORT double rocketnet_server_client_stat( const struct rocketnet_server_t * server, int client_index, enum rocketnet_server_client_stat_t stat );

#ifdef __cplusplus
}
#endif

#endif // #ifndef ROCKETNET_H
