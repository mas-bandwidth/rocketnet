/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public C API: a client and a server that exchange messages over UDP.
// This header and a prebuilt RocketNet library are all a program needs: it
// includes only standard C headers and declares only what a caller calls or
// reads. Plain C99, callable from C++ through the extern "C" block below.
//
// The API's shape: every call that can fail returns enum rocketnet_status_t;
// a pure question returns bool; every set of values is a typed enum named
// ROCKETNET_<SET>_<VALUE>, and each lookup that names an enum value ends in
// _name and returns "unknown" for a value outside its enum, never NULL.
//
// Sizes: a caller memory block or buffer handed to a call is sized in size_t
// (memory_bytes, the required_memory results, config_bytes,
// connect_token_bytes); config fields, message bytes and counts are int;
// client indexes and destination ids are uint64_t.

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

/**
 * @brief Identifies the public C struct and calling-convention revision.
 *
 * This revision is independent of the wire protocol version. Revision 1 is
 * the layout and calling contract of the first published API, this header.
 * From here on, increment it for an incompatible public layout or
 * calling-contract change; append-only additions that preserve old layouts
 * keep the revision unchanged.
 */
#define ROCKETNET_ABI_VERSION 1

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

/** @brief Maximum number of simultaneous clients accepted by a server. */
#define ROCKETNET_MAX_CLIENTS                                       1000

// Destination ids are uint64_t. Today every destination is a client index,
// 0 .. max_clients - 1. An id with ROCKETNET_FLOW_ID_BIT set is a flow id,
// reserved for a future server-side flow feature (clients subscribe and
// unsubscribe; the server sends to a flow by id with the same start message
// and finish message), and never a client index. Until flows exist every function taking
// a destination id refuses one with the bit set, and counts it.

#define ROCKETNET_FLOW_ID_BIT                                       ( 1ULL << 63 )

// The in-memory loopback transport holds this many endpoints in one process:
// enough for one server and ROCKETNET_MAX_CLIENTS clients. It is chosen only
// by name ("loopback"), for tests and examples; auto never selects it.

#define ROCKETNET_LOOPBACK_MAX_ENDPOINTS          ( ROCKETNET_MAX_CLIENTS + 1 )

// The packet size: the UDP payload of one datagram on a 1500-byte MTU link,
// the number every other size limit derives from (STANDARD.md _Packet
// sizes_). Over IPv4 it is 1500 - 20 - 8 = 1472 bytes, the receive ceiling;
// over IPv6 it is 1500 - 40 - 8 = 1452 bytes, the send budget, so a maximum
// packet crosses a 1500-MTU path unfragmented on either family.

/** @brief Maximum UDP payload received over IPv4, in bytes, on a 1500-byte MTU path. */
#define ROCKETNET_MAX_PACKET_BYTES                     ( 1500 - 20 - 8 )
/** @brief Maximum UDP payload sent over IPv6, in bytes, on a 1500-byte MTU path. */
#define ROCKETNET_MAX_PACKET_BYTES_IPV6                ( 1500 - 40 - 8 )

// The send-side payload capacity: the send budget (ROCKETNET_MAX_PACKET_BYTES_IPV6,
// 1452) less the 26-byte fixed overhead and the 7-byte minimum ack block
// leaves 1419; the capacity is the largest multiple of eight inside that,
// 1416, so any word-based serializer can root at the payload pointer
// directly. The receive side accepts payloads up to the receive ceiling
// (ROCKETNET_MAX_PACKET_BYTES, 1472) less the same overhead, 1439 bytes,
// whatever this end's own budget, so receive buffers size by the receive
// constant.

/** @brief Maximum application payload sent without fragmentation, in bytes. */
#define ROCKETNET_MAX_PAYLOAD_BYTES                        1416    // ( ( 1452 - 26 - 7 ) / 8 ) * 8
/** @brief Maximum unfragmented application payload accepted on receive, in bytes. */
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

// messages (docs/CHANNELS.md): the channel bound

#ifndef ROCKETNET_MAX_CHANNELS
/** @brief Maximum number of message channels an endpoint configuration may select; defaults to 32 when not overridden at compile time. */
#define ROCKETNET_MAX_CHANNELS                                         32
#endif // #ifndef ROCKETNET_MAX_CHANNELS


/**
 * @brief The payload room for the message one packet carries: ROCKETNET_MAX_PAYLOAD_BYTES less the one form byte, 1415 bytes.
 *
 * It is the least value either config's max_message_bytes accepts. The
 * message's record header takes room beside it, so the largest
 * single-packet message is smaller: 1411 bytes for
 * ROCKETNET_RELIABILITY_UNRELIABLE and ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
 * 1405 for ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED. A larger unreliable
 * message is promoted to its reliable twin (the messages section below).
 */
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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stable status values returned by every call that can fail.
 *
 * OK is the sole success value, every failure is a positive code, and the
 * numbering is append-only. A published value and meaning never change; a new
 * code appends immediately before ROCKETNET_NUM_STATUS_CODES.
 */
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,   ///< success: the operation succeeds

    // lifecycle and allocator state (rocketnet_init, rocketnet_term, rocketnet_set_allocator)
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

    // object lifecycle (rocketnet_client_connect, rocketnet_server_start) and destinations
    ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED            = 23,   ///< connect refused: the client is not DISCONNECTED; call rocketnet_client_disconnect first; nothing changed
    ROCKETNET_STATUS_SERVER_NOT_STOPPED                 = 24,   ///< start refused: the server is already running; nothing changed
    ROCKETNET_STATUS_FLOW_ID_RESERVED                   = 25,   ///< a destination id has ROCKETNET_FLOW_ID_BIT set: flow ids are reserved and refused until flows exist

    ROCKETNET_NUM_STATUS_CODES                          = 26     ///< sentinel: the count of codes, OK included; not an operation result
};

// -----------------------------------------
//                          log levels
// -----------------------------------------

/** @brief The process-wide log levels, least to most verbose. */
enum rocketnet_log_level_t
{
    ROCKETNET_LOG_LEVEL_NONE                            = 0,   ///< as the maximum, only level-NONE lines; quiet mode suppresses every line
    ROCKETNET_LOG_LEVEL_ERROR                           = 1,   ///< errors, the least verbose emitted level
    ROCKETNET_LOG_LEVEL_WARN                            = 2,   ///< warnings
    ROCKETNET_LOG_LEVEL_INFO                            = 3,   ///< information
    ROCKETNET_LOG_LEVEL_DEBUG                           = 4,   ///< debugging
    ROCKETNET_LOG_LEVEL_SPAM                            = 5    ///< the most verbose level
};

// -----------------------------------------
//                    message enums
// -----------------------------------------

/**
 * @brief Per-send reliability, RakNet's values and numbering (docs/CHANNELS.md).
 */
enum rocketnet_reliability_t
{
    /** Send once without delivery, ordering, sequencing, or receipt. The
     * receiver can see the message at most once. A lost packet loses it. */
    ROCKETNET_RELIABILITY_UNRELIABLE                            = 0,
    /** Send without delivery guarantees and keep only newer channel state.
     * The receiver delivers a state only when its ordering and sequencing
     * values are newer; older or out-of-turn states can be discarded. */
    ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED                  = 1,
    /** Resend until the peer's transport acknowledges the message. Delivery
     * is once, but messages on the same channel have no order guarantee. This
     * mode does not produce a receipt. */
    ROCKETNET_RELIABILITY_RELIABLE                              = 2,
    /** Resend until acknowledged and deliver in channel order. The receiver
     * holds later messages until earlier messages on that channel arrive. This
     * mode does not produce a receipt. */
    ROCKETNET_RELIABILITY_RELIABLE_ORDERED                      = 3,
    /** Resend until acknowledged and deliver only the newest channel state.
     * The receiver can discard an acknowledged stale state; delivery order is
     * not guaranteed. This mode does not produce a receipt. */
    ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED                    = 4,
    /** Send once and report whether the carrying packet is acknowledged. A
     * larger message is sent as a reliable block and can report ACKED only. A
     * single-packet message can report ACKED or LOSS. */
    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT           = 5,
    /** Resend until acknowledged and report an ACKED receipt. Delivery is
     * once without channel ordering. A connection failure does not produce a
     * LOSS receipt. */
    ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT             = 6,
    /** Resend until acknowledged, deliver in channel order, and report ACKED.
     * A connection failure does not produce a LOSS receipt. */
    ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT     = 7
};

/** @brief A receipt's outcome, the status field of struct rocketnet_receipt_t. */
enum rocketnet_receipt_status_t
{
    /** The peer's transport acknowledged the packet. This does not mean the
     * peer application read the message. */
    ROCKETNET_RECEIPT_STATUS_ACKED                              = 1,
    /** An unreliable receipt packet was not acknowledged. The message is not
     * resent; reliable receipt modes do not produce LOSS. */
    ROCKETNET_RECEIPT_STATUS_LOSS                               = 2
};

/**
 * @brief Why a connection ended: what rocketnet_client_disconnect_reason and
 * rocketnet_server_disconnect_reason return (docs/CHANNELS.md 12).
 *
 * MESSAGE_MEMORY_EXHAUSTED: the connection's region had no room for an
 * operation it must not drop (docs/CHANNELS.md 4). A server disconnects that
 * client's slot with this reason and resets the slot's region; a client
 * disconnects itself.
 */
enum rocketnet_disconnect_reason_t
{
    ROCKETNET_DISCONNECT_REASON_NONE                            = 0,   ///< no connection has ended, or the connection is up
    ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH                 = 1,   ///< the two ends' WIRE config fields differ
    ROCKETNET_DISCONNECT_REASON_CONFIG_TIMEOUT                  = 2,   ///< the config exchange did not finish in time
    ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION              = 3,   ///< the peer sent what the protocol forbids
    ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT                = 4,   ///< a reliable message went unacknowledged past reliable_timeout_ms
    ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED        = 5,   ///< the connection's region had no room for an operation it must not drop
    ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED            = 6    ///< the server ended the connection: it disconnected the client, stopped or was destroyed
};

/** @brief What rocketnet_client_update and rocketnet_server_update return. */
enum rocketnet_update_result_t
{
    ROCKETNET_UPDATE_OK                                         =  0,  ///< the update ran
    ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED                       = -1   ///< refused: a received message view is still held
};

// -----------------------------------------
//                        client state
// -----------------------------------------

// What rocketnet_client_state returns. A connect attempt runs up from
// DISCONNECTED through the three handshake states to CONNECTED. A negative
// state is how the last attempt or connection ended, kept until
// rocketnet_client_disconnect, which returns the client to DISCONNECTED so it
// can connect again. INVALID is no client state: it is what a NULL client
// reads as.

enum rocketnet_client_state_t
{
    ROCKETNET_CLIENT_STATE_INVALID                          = -100, // a NULL client: not a state any client is in
    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            = -7,   // the connect token expired before the server accepted the client
    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            = -6,   // the connect token was refused before the first packet was sent
    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             = -5,   // a connection heard nothing from the server for the token's timeout
    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    = -4,   // the server never answered the third leg of the handshake
    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     = -3,   // the server never answered the connection request
    ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT         = -2,   // the server never answered the cookie request
    ROCKETNET_CLIENT_STATE_CONNECTION_DENIED                = -1,   // the server refused the client
    ROCKETNET_CLIENT_STATE_DISCONNECTED                     =  0,   // after create, and after rocketnet_client_disconnect
    ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE                =  1,   // handshake, first leg
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST       =  2,   // handshake, second leg
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE      =  3,   // handshake, third leg
    ROCKETNET_CLIENT_STATE_CONNECTED                        =  4    // connected: messages flow both ways
};

// -----------------------------------------
//                     query enums
// -----------------------------------------

// What a client counts, read with rocketnet_client_counter. Every value is
// monotonic over the client's life, across connections, and starts at 0 at
// create.

/**
 * @brief The client's cumulative counters, read one at a time with rocketnet_client_counter.
 *
 * Every value is monotonic over the client's life and starts at 0 at create.
 * The datagram and byte values are the client's wire totals; the others sum the
 * matching cumulative protocol counters of the client's transport and message
 * layers.
 */
enum rocketnet_client_counter_t
{
    /** @brief UDP datagrams the client sent, handshake included. Unit: datagrams. */
    ROCKETNET_CLIENT_COUNTER_PACKETS_SENT,          // packets: UDP datagrams sent, handshake included
    /** @brief UDP datagrams the client received, handshake included. Unit: datagrams. */
    ROCKETNET_CLIENT_COUNTER_PACKETS_RECEIVED,      // packets: UDP datagrams received, handshake included
    /** @brief Bytes the client sent, handshake included: each datagram's UDP payload plus its IP and UDP headers (28 bytes for IPv4, 48 for IPv6, from the server address's family). Unit: bytes. */
    ROCKETNET_CLIENT_COUNTER_BYTES_SENT,            // bytes: UDP payload plus the IP and UDP headers of the peer's family
    /** @brief Bytes the client received, handshake included: each datagram's UDP payload plus its IP and UDP headers (28 bytes for IPv4, 48 for IPv6, from the server address's family). Unit: bytes. */
    ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED,        // bytes: UDP payload plus the IP and UDP headers of the peer's family
    /** @brief Datagrams from the server that opened and carried messages. Unit: payloads. */
    ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED,     // payloads: packets from the server that opened and carried messages
    /** @brief Reliable message records the client resent on its retransmit timer. Unit: records. */
    ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT,       // records: reliable message records resent on the retransmit timer
    /** @brief Reliable messages the client received a second time and dropped. Unit: messages. */
    ROCKETNET_CLIENT_COUNTER_MESSAGES_DUPLICATE,    // messages: reliable messages received a second time and dropped
    /** @brief Client sends with a receipt that the server acknowledged. Unit: receipts. */
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED,        // receipts: sends with a receipt that the server acknowledged
    /** @brief Client sends with a receipt that were reported lost. Unit: receipts. */
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST,         // receipts: sends with a receipt that were lost
    /** @brief Unreliable messages the client dropped because its send budget was spent. Unit: messages. */
    ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS,          // messages: unreliable messages dropped because the send budget was spent
    /** @brief Sends that left with the next hand-off to the send thread instead of at once. Unit: hand-offs. */
    ROCKETNET_CLIENT_COUNTER_SEND_DEFERRED,         // hand-offs: sends that left with the next hand-off to the send thread
    /** @brief Received messages still held, not released, when a client update began; that update adds the number held, still runs, and returns ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. Unit: messages. */
    ROCKETNET_CLIENT_COUNTER_MESSAGES_NOT_RELEASED, // messages: received messages still held when an update was refused
    /** @brief The number of client counters; not a counter, and rocketnet_client_counter returns 0 for it. */
    ROCKETNET_CLIENT_COUNTER_COUNT
};

// The client's connection quality, read with rocketnet_client_stat. Rates
// cover the trailing second.

enum rocketnet_client_stat_t
{
    ROCKETNET_CLIENT_STAT_RTT,                      // milliseconds: round trip time
    ROCKETNET_CLIENT_STAT_JITTER,                   // milliseconds: inter-arrival jitter
    ROCKETNET_CLIENT_STAT_PACKET_LOSS,              // percent: received-side loss, from gaps in the server's packet sequence
    ROCKETNET_CLIENT_STAT_PACKETS_SENT_PER_SECOND,  // packets per second sent
    ROCKETNET_CLIENT_STAT_PACKETS_RECEIVED_PER_SECOND, // packets per second received
    ROCKETNET_CLIENT_STAT_BANDWIDTH_SENT,           // kbps (1000 bit/s) sent
    ROCKETNET_CLIENT_STAT_BANDWIDTH_RECEIVED,       // kbps (1000 bit/s) received
    ROCKETNET_CLIENT_STAT_COUNT
};

// What a server counts over all its clients, read with
// rocketnet_server_counter. Every value is monotonic over the server's life,
// across starts and stops, and starts at 0 at create.

/**
 * @brief The server's cumulative counters over all its clients, read one at a time with rocketnet_server_counter.
 *
 * Every value is monotonic over the server's life and starts at 0 at create.
 * The datagram and byte values are the server's whole-socket wire totals; the
 * others sum the matching cumulative protocol counters of the server's
 * transport and message layers.
 */
enum rocketnet_server_counter_t
{
    /** @brief UDP datagrams the server sent to all clients. Unit: datagrams. */
    ROCKETNET_SERVER_COUNTER_PACKETS_SENT,          // packets: UDP datagrams sent
    /** @brief UDP datagrams the server received. Unit: datagrams. */
    ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED,      // packets: UDP datagrams received
    /** @brief Bytes the server sent to all clients: each datagram's UDP payload plus its IP and UDP headers (28 bytes for IPv4, 48 for IPv6, from each datagram's peer family). Unit: bytes. */
    ROCKETNET_SERVER_COUNTER_BYTES_SENT,            // bytes: UDP payload plus the IP and UDP headers of the peer's family
    /** @brief Bytes the server received: each datagram's UDP payload plus its IP and UDP headers (28 bytes for IPv4, 48 for IPv6, from each datagram's peer family). Unit: bytes. */
    ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED,        // bytes: UDP payload plus the IP and UDP headers of the peer's family
    /** @brief Clients that completed the handshake. Unit: connections. */
    ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED,     // connections: clients that completed the handshake
    /** @brief Connection requests and responses refused because every client slot was taken. Unit: requests. */
    ROCKETNET_SERVER_COUNTER_CLIENTS_DENIED_FULL,   // requests: connect attempts refused because every client slot was taken
    /** @brief Connection requests dropped by the per-update handshake work budget. Unit: requests. */
    ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET,// requests: connect attempts dropped by the per-update handshake budget
    /** @brief Datagrams from clients that opened and carried messages. Unit: payloads. */
    ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED,     // payloads: packets from clients that opened and carried messages
    /** @brief Reliable message records the server resent on its retransmit timer. Unit: records. */
    ROCKETNET_SERVER_COUNTER_MESSAGES_RESENT,       // records: reliable message records resent on the retransmit timer
    /** @brief Reliable messages the server received a second time and dropped. Unit: messages. */
    ROCKETNET_SERVER_COUNTER_MESSAGES_DUPLICATE,    // messages: reliable messages received a second time and dropped
    /** @brief Server sends with a receipt that a client acknowledged. Unit: receipts. */
    ROCKETNET_SERVER_COUNTER_RECEIPTS_ACKED,        // receipts: sends with a receipt that a client acknowledged
    /** @brief Server sends with a receipt that were reported lost. Unit: receipts. */
    ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST,         // receipts: sends with a receipt that were lost
    /** @brief Unreliable messages dropped because a client's send budget was spent. Unit: messages. */
    ROCKETNET_SERVER_COUNTER_BUDGET_DROPS,          // messages: unreliable messages dropped because a client's send budget was spent
    /** @brief Received messages still held, not released, when a server update began; that update adds the number held, still runs, and returns ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. Unit: messages. */
    ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED, // messages: received messages still held when an update was refused
    /** @brief The number of server counters; not a counter, and rocketnet_server_counter returns 0 for it. */
    ROCKETNET_SERVER_COUNTER_COUNT
};

// The server's whole-socket rates over the trailing second, read with
// rocketnet_server_stat.

enum rocketnet_server_stat_t
{
    ROCKETNET_SERVER_STAT_PACKETS_SENT_PER_SECOND,  // packets per second sent
    ROCKETNET_SERVER_STAT_PACKETS_RECEIVED_PER_SECOND, // packets per second received
    ROCKETNET_SERVER_STAT_BANDWIDTH_SENT,           // kbps (1000 bit/s) sent
    ROCKETNET_SERVER_STAT_BANDWIDTH_RECEIVED,       // kbps (1000 bit/s) received
    ROCKETNET_SERVER_STAT_COUNT
};

// What a server counts for one client slot, read with
// rocketnet_server_client_counter. The values cover the slot's current
// connection and go back to 0 when a new connection takes the slot.

/**
 * @brief One client slot's counters, read one at a time with rocketnet_server_client_counter.
 *
 * The values cover the slot's current connection and start again at 0 when a
 * new connection takes the slot.
 */
enum rocketnet_server_client_counter_t
{
    /** @brief UDP datagrams the server sent to this client on its current connection. Unit: datagrams. */
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT,       // packets: UDP datagrams sent to this client
    /** @brief UDP datagrams the server received from this client on its current connection. Unit: datagrams. */
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_RECEIVED,   // packets: UDP datagrams received from this client
    /** @brief Bytes the server sent to this client on its current connection: each datagram's UDP payload plus its IP and UDP headers (28 bytes for IPv4, 48 for IPv6, from this client's family). Unit: bytes. */
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT,         // bytes: UDP payload plus the IP and UDP headers of this client's family
    /** @brief Bytes the server received from this client on its current connection: each datagram's UDP payload plus its IP and UDP headers (28 bytes for IPv4, 48 for IPv6, from this client's family). Unit: bytes. */
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED,     // bytes: UDP payload plus the IP and UDP headers of this client's family
    /** @brief The number of per-client counters; not a counter, and rocketnet_server_client_counter returns 0 for it. */
    ROCKETNET_SERVER_CLIENT_COUNTER_COUNT
};

// One client slot's connection quality, read with
// rocketnet_server_client_stat. Rates cover the trailing second.

enum rocketnet_server_client_stat_t
{
    ROCKETNET_SERVER_CLIENT_STAT_RTT,                   // milliseconds: round trip time
    ROCKETNET_SERVER_CLIENT_STAT_JITTER,                // milliseconds: deviation of rtt samples above the minimum
    ROCKETNET_SERVER_CLIENT_STAT_PACKET_LOSS,           // percent: packets sent to this client that were never acknowledged
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_SENT,        // kbps (1000 bit/s) sent to this client
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_RECEIVED,    // kbps (1000 bit/s) received from this client
    ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_ACKED,       // kbps (1000 bit/s) of sent packets this client acknowledged
    ROCKETNET_SERVER_CLIENT_STAT_COUNT
};

// -----------------------------------------
//                     opaque handles
// -----------------------------------------

/**
 * @brief A client: one connection to a server at a time, made from a connect token.
 *
 * rocketnet_client_create makes a DISCONNECTED client,
 * rocketnet_client_connect starts a connection, rocketnet_client_disconnect
 * ends it, and rocketnet_client_destroy frees the client from any state.
 * The client allocates nothing after create except the transport's own
 * resources at connect. A caller memory block passed to create stays the
 * caller's and must outlive the client.
 *
 * Every frame runs in this order: update first, which takes in what
 * arrived; then receive and release every message; then the game
 * simulation; then start message and finish message for this frame's
 * sends; then send_packets last, so this frame's messages leave this
 * frame. Calling send_packets before the frame's messages are queued leaves
 * them waiting a whole frame, and receiving before update hands out only
 * what the previous update took in, a frame late.
 *
 * The client holds no lock: call its functions from one thread at a time.
 *
 * @code
 * struct rocketnet_client_t * client = rocketnet_client_create( NULL, NULL, NULL, 0 );
 * if ( client && rocketnet_client_connect( client, connect_token ) == ROCKETNET_STATUS_OK )
 * {
 *     while ( playing )
 *     {
 *         rocketnet_client_update( client, game_time );     // first: take in what arrived
 *         const uint8_t * data;
 *         int bytes;
 *         while ( rocketnet_client_receive_message( client, channel, &data, &bytes ) )
 *         {
 *             handle_message( data, bytes );
 *             rocketnet_client_release_message( client, data );
 *         }
 *         simulate_frame();
 *         uint8_t * out = rocketnet_client_start_message( client, channel, ROCKETNET_RELIABILITY_UNRELIABLE, input_bytes );
 *         if ( out )
 *         {
 *             memcpy( out, input, input_bytes );
 *             rocketnet_client_finish_message( client, input_bytes );
 *         }
 *         rocketnet_client_send_packets( client );          // last, so this frame's messages leave this frame
 *     }
 *     rocketnet_client_disconnect( client );
 * }
 * if ( client )
 *     rocketnet_client_destroy( client );
 * @endcode
 */
struct rocketnet_client_t;
/**
 * @brief A server: up to max_clients client connections, each in a slot named by its client index.
 *
 * rocketnet_server_create makes a stopped server that binds nothing,
 * rocketnet_server_start binds the transport and accepts clients,
 * rocketnet_server_stop disconnects every client and releases the
 * transport, and rocketnet_server_destroy frees the server from either
 * state. A caller memory block passed to create stays the caller's and must
 * outlive the server.
 *
 * Every frame runs in this order: update first, then receive and release
 * every message from every client, then the game simulation, then start
 * message and finish message for this frame's sends, then send_packets
 * last, so this frame's messages leave this frame. Calling send_packets
 * before the frame's messages are queued leaves them waiting a whole frame.
 *
 * The server holds no lock: call its functions from one thread at a time.
 *
 * @code
 * // config: rocketnet_server_default_config with the title's keys set
 * struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:40000", &config, NULL, 0 );
 * if ( server && rocketnet_server_start( server ) == ROCKETNET_STATUS_OK )
 * {
 *     const int max_clients = rocketnet_server_max_clients( server );
 *     while ( running )
 *     {
 *         rocketnet_server_update( server, game_time );     // first: take in what arrived
 *         for ( int i = 0; i < max_clients; i++ )
 *         {
 *             const uint8_t * data;
 *             int bytes;
 *             while ( rocketnet_server_receive_message( server, i, channel, &data, &bytes ) )
 *             {
 *                 handle_message( i, data, bytes );
 *                 rocketnet_server_release_message( server, i, data );
 *             }
 *         }
 *         simulate_frame();
 *         for ( int i = 0; i < max_clients; i++ )
 *         {
 *             if ( !rocketnet_server_client_connected( server, i ) )
 *                 continue;
 *             uint8_t * out = rocketnet_server_start_message( server, i, channel, ROCKETNET_RELIABILITY_UNRELIABLE, state_bytes );
 *             if ( out )
 *             {
 *                 write_state( i, out );
 *                 rocketnet_server_finish_message( server, i, state_bytes );
 *             }
 *         }
 *         rocketnet_server_send_packets( server );          // last, so this frame's messages leave this frame
 *     }
 * }
 * if ( server )
 *     rocketnet_server_destroy( server );
 * @endcode
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
 * @return ROCKETNET_STATUS_OK; ROCKETNET_STATUS_ALREADY_INITIALIZED when the
 * library is initialized or transitioning; ROCKETNET_STATUS_PLATFORM_INIT_FAILED
 * or ROCKETNET_STATUS_CRYPTO_INIT_FAILED when that layer does not start.
 * Call before creating a client or a server; do not race an init or term transition.
 * @code
 * if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK ) {
 *     // handle failure
 * }
 * @endcode
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_init( void * context );

/**
 * @brief Terminates the initialized library after every client and server is destroyed.
 *
 * A refused termination logs one error line. Do not call concurrently with SDK work that
 * allocates or depends on platform state.
 * @return ROCKETNET_STATUS_OK; ROCKETNET_STATUS_NOT_INITIALIZED with nothing to
 * terminate, or during a transition; ROCKETNET_STATUS_LIVE_ALLOCATIONS while a
 * client, a server or another allocation is still held.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_term( void );

/** @brief Returns the total number of status codes, including OK. @return Count of defined values; allocation-free and safe before init. */
ROCKETNET_EXPORT int rocketnet_num_status_codes( void );

/** @brief Returns the stable symbolic name of a status code. @param status Status to look up. @return Static name without the ROCKETNET_STATUS_ prefix ("OK", "INVALID_ARGUMENT"), or "unknown" for a value that is not a code, never NULL. Allocation-free and safe before init; returned storage is process-lifetime. */
ROCKETNET_EXPORT const char * rocketnet_status_name( enum rocketnet_status_t status );

/**
 * @brief Receives a failed internal assert: the borrowed condition, function
 * and file strings, and the source line that failed. It runs synchronously on
 * the thread that fires the assertion.
 */
typedef void (*rocketnet_assert_callback_t)( const char * condition, const char * function, const char * file, int line );

/**
 * @brief Process-wide callback for a failed internal assert.
 *
 * The library calls it when it is built with asserts enabled (a debug build);
 * with asserts disabled it is never invoked. The default callback logs and
 * traps. rocketnet_set_assert_callback replaces it, and NULL restores the
 * default; set it that way before concurrent SDK calls, since direct writes
 * are not synchronized.
 */
extern ROCKETNET_EXPORT void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line );

/**
 * @brief Installs the process-wide callback for enabled assertion failures.
 *
 * Pass NULL to restore the default callback. Install it before concurrent SDK
 * calls; this setter does not synchronize direct pointer reads.
 *
 * @param callback Callback to install, or NULL to restore the default.
 */
ROCKETNET_EXPORT void rocketnet_set_assert_callback( rocketnet_assert_callback_t callback );

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
 * @param level ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM; a value outside the enum is ignored.
 */
ROCKETNET_EXPORT void rocketnet_set_log_level( enum rocketnet_log_level_t level );

/**
 * @brief Reads the current process-wide log level.
 * The level currently in force, so a caller that moves the level for a bounded
 * stretch can restore what it found rather than guessing. This relaxed atomic
 * read is diagnostic only; it is not a synchronization operation.
 * @return A value from ROCKETNET_LOG_LEVEL_NONE through ROCKETNET_LOG_LEVEL_SPAM.
 */
ROCKETNET_EXPORT enum rocketnet_log_level_t rocketnet_get_log_level( void );

/**
 * @brief Returns the display name for a log level.
 * A log callback uses it to render a level the way the default sink does. The
 * returned string is static and remains valid for the process lifetime.
 * @param level A log level.
 * @return "none", "error", "warning", "info", "debug" or "spam"; "unknown" for a value outside the enum, never NULL.
 */
ROCKETNET_EXPORT const char * rocketnet_log_level_name( enum rocketnet_log_level_t level );

/**
 * @brief Receives one preformatted log message: the one log sink.
 *
 * The callback runs synchronously on the logging thread. The message is
 * NUL-terminated (message[length] is always NUL), length is clamped to at most
 * 1023 bytes, and the pointer borrows the logging frame's stack buffer, valid
 * only until the callback returns. The callback must be thread-safe and must
 * not re-enter rocketnet_set_log_callback.
 * @param context Caller-owned pointer supplied when the callback is installed.
 * @param level The level passed by the logging call.
 * @param message Borrowed formatted bytes; valid only during this callback.
 * @param length Number of message bytes before the NUL terminator, at most 1023.
 */
typedef void (*rocketnet_log_callback_t)( void * context, enum rocketnet_log_level_t level, const char * message, size_t length );

/**
 * @brief Installs the log callback, the one log sink.
 *
 * Passing NULL restores the default sink, which prints to stdout, and clears
 * its context. Calls already in flight may finish on their previously captured
 * callback; callbacks must be thread-safe.
 * @param callback Callback to install, or NULL to restore the default sink.
 * @param context Caller-owned pointer passed back to callback; keep it valid until any in-flight callback using it returns.
 */
ROCKETNET_EXPORT void rocketnet_set_log_callback( rocketnet_log_callback_t callback, void * context );

/**
 * @brief Reads the process-wide count of API misuses since the process started.
 * It counts exactly these: a NULL argument a call refuses (a NULL client's
 * state included); a create before rocketnet_init; a refused
 * rocketnet_set_allocator; an update time that is not finite or goes
 * backwards; a release of a pointer that is not a held view, or a release on
 * a client with no connection; an in-place start message still open at update
 * or send_packets; a connect from a state other than DISCONNECTED; a start
 * message or finish message on a client that is not CONNECTED; and a destination id with
 * ROCKETNET_FLOW_ID_BIT set. A send refused for its own arguments (a second
 * start message while one is pending, a finish message whose bytes_written
 * is out of range, a
 * channel or reliability out of range, a server slot with no client) is not
 * counted here: it returns NULL or 0 and logs.
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
 * replacements are refused: the call returns the refusal's status, logs one
 * line and counts the refusal in rocketnet_api_misuse_count. Callbacks must be
 * thread-safe and return storage aligned for fundamental types. RocketNet adds
 * alignment/ownership metadata, so callbacks receive larger sizes and their
 * original pointer on free.
 *
 * @param malloc_function Allocator callback; receives context and requested bytes plus SDK overhead.
 * @param free_function Matching deallocator; receives the original allocation pointer.
 * @return ROCKETNET_STATUS_OK when both callbacks are installed;
 * ROCKETNET_STATUS_INVALID_ARGUMENT for a partial pair (either callback NULL);
 * ROCKETNET_STATUS_ALREADY_INITIALIZED after init or during a transition;
 * ROCKETNET_STATUS_LIVE_ALLOCATIONS while allocations are outstanding.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_set_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );


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
// at client connect and server start; the first transport that starts wins, and a list that every
// candidate refuses fails loudly. "auto" (the default, also NULL or the empty
// string) is this platform's recommended order: a linux server tries
// xdp,gso,io_uring,portable (io_uring only in a build configured with
// liburing), a macOS server msgx,portable, and a Windows server
// portable,iocp,rio; a client tries msgx,portable on macOS and portable
// everywhere else. loopback is never in auto; it is chosen by name. A one-element
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
// its end without breaking a program built against an older header. An
// unknown size is refused with a log line and nothing is read or written.

/**
 * @brief Complete client creation configuration.
 *
 * Initialize with rocketnet_client_default_config(), change the fields the
 * application owns, then pass the structure to rocketnet_client_create().
 * Creation copies string values. Fields marked WIRE must match the server.
 * Memory sizing and creation apply the environment overrides of the message
 * and send budget fields; rocketnet_client_connect() applies the overrides of
 * the transport fields (filter_value, max_payload_bytes, num_reassembly_slots,
 * step_driven, transport and the simulated_* fields) and checks them.
 * rocketnet_client_validate_config() reads no environment.
 */
struct rocketnet_client_config_t
{
    /** @brief ROCKETNET_CLIENT_FILTER_VALUE: the per-title packet filter, as the server's */
    uint8_t filter_value[4];                        // ROCKETNET_CLIENT_FILTER_VALUE: the per-title packet filter, as the server's
    /** @brief ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES */
    int max_payload_bytes;                          // ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    /** @brief ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS: 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS */
    int num_reassembly_slots;                       // ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS: 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    /**
     * @brief ROCKETNET_CLIENT_STEP_DRIVEN: true runs the client I/O step on the caller's thread; default false.
     *
     * With true, rocketnet_client_update() and rocketnet_client_send_packets()
     * run the I/O step on the calling thread and no I/O thread is started.
     * With false the client runs its own I/O thread. The environment form
     * takes 0 or 1. An environment value above 1, or true with a transport
     * that has no non-blocking I/O step, refuses rocketnet_client_connect()
     * with ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE and one error line.
     */
    bool step_driven;                               // ROCKETNET_CLIENT_STEP_DRIVEN: true (env 1) runs the client I/O step inside update and send_packets on the caller's thread, with no I/O thread; false (env 0, the default) keeps the I/O thread
    /** @brief ROCKETNET_CLIENT_TRANSPORT: the candidate list, tried in order at rocketnet_client_connect(); NULL is "auto" */
    const char * transport;                         // ROCKETNET_CLIENT_TRANSPORT: the candidate list; NULL is "auto"
    /**
     * @brief Callback invoked when the client connection state changes; no environment form and NULL disables it.
     * @param context The context passed to rocketnet_client_create().
     * @param previous_state State before the transition.
     * @param current_state State after the transition.
     */
    void (*state_change_callback)( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state );   // no env form; NULL is none
    /**
     * @brief Receive timestamp clock selected once at creation; no environment form and NULL selects the platform clock.
     * @param context The context passed to rocketnet_client_create().
     * @return Current time in seconds from a monotonic clock.
     */
    double (*time_callback)( void * context );      // no env form: the receive stamp clock, read once at create; NULL is the platform clock
    /** @brief ROCKETNET_CLIENT_SEND_KBPS: the send budget, kbps; default 1000; 0 or less disables the budget in this direction (client to server), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused */
    int send_kbps;                                  // ROCKETNET_CLIENT_SEND_KBPS: the send budget, kbps; default 1000; 0 or less disables the budget in this direction (client to server), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused
    /** @brief ROCKETNET_CLIENT_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32 */
    int num_channels;                               // ROCKETNET_CLIENT_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    /** @brief ROCKETNET_CLIENT_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024 */
    int window_messages;                            // ROCKETNET_CLIENT_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    /** @brief ROCKETNET_CLIENT_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144 */
    int receive_ring_bytes;                         // ROCKETNET_CLIENT_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    /** @brief ROCKETNET_CLIENT_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144 */
    int max_message_bytes;                          // ROCKETNET_CLIENT_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    /** @brief ROCKETNET_CLIENT_SEND_BUFFER_BYTES: queued message bytes; 12288 .. 1 << 28; default 800000 */
    int send_buffer_bytes;                          // ROCKETNET_CLIENT_SEND_BUFFER_BYTES: queued message bytes; 12288 .. 1 << 28; default 800000
    /** @brief ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES: queued messages; 16 .. 1 << 20; default 1024 */
    int send_queue_messages;                        // ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES: queued messages; 16 .. 1 << 20; default 1024
    /** @brief ROCKETNET_CLIENT_PACKETS_IN_FLIGHT: unacknowledged packets tracked, the packets_in_flight window; 16 .. 1 << 16; default 256 */
    int packets_in_flight;                          // ROCKETNET_CLIENT_PACKETS_IN_FLIGHT: unacknowledged packets tracked; 16 .. 1 << 16; default 256
    /**
     * @brief ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE: max_packets_per_update, the most packets the client sends in one update; 1 .. 1024; default 256.
     *
     * In-place unreliable messages sent at finish and what
     * rocketnet_client_send_packets() writes count together against this one
     * allowance per update. While reliable work is due, an in-place message
     * never takes the last of the allowance, so that work is sent in the
     * same update. The send budget, not this field, is the throttle.
     */
    int max_packets_per_update;                     // ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE: packets sent per update, in-place unreliable finish messages and send_packets together; 1 .. 1024; default 256; the send budget is the throttle
    /** @brief ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per update; 1 .. 1 << 16; default 256 */
    int unreliable_deliveries;                      // ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per update; 1 .. 1 << 16; default 256
    /** @brief ROCKETNET_CLIENT_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16 */
    int sequenced_holds;                            // ROCKETNET_CLIENT_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    /** @brief Maximum queued client send receipts, 16 through 65536 (default 256).
     * A full ring drops its oldest receipt; ROCKETNET_CLIENT_RECEIPT_RING can override it.
     */
    int receipt_ring;                               // ROCKETNET_CLIENT_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    /** @brief ROCKETNET_CLIENT_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100 */
    int rto_initial_ms;                             // ROCKETNET_CLIENT_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100
    /** @brief ROCKETNET_CLIENT_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10 */
    int rto_min_ms;                                 // ROCKETNET_CLIENT_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10
    /** @brief ROCKETNET_CLIENT_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250 */
    int rto_max_ms;                                 // ROCKETNET_CLIENT_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250
    /** @brief ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000 */
    int reliable_timeout_ms;                        // ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
    /**
     * @brief Development only: the link simulator's outgoing loss, percent; 0 .. 100; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_client_connect()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_client_validate_config_sized() does not make that check.
     */
    int simulated_loss_percent;                     // development only: outgoing loss 0..100; 0 is off
    /**
     * @brief Development only: the link simulator's added outgoing latency, milliseconds; 0 .. 60000; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_client_connect()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_client_validate_config_sized() does not make that check.
     */
    int simulated_latency_ms;                       // development only: outgoing latency in milliseconds; 0 is off
    /**
     * @brief Development only: the link simulator's added outgoing jitter, milliseconds; 0 .. 60000; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_client_connect()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_client_validate_config_sized() does not make that check.
     */
    int simulated_jitter_ms;                        // development only: outgoing jitter in milliseconds; 0 is off
    /**
     * @brief Development only: the link simulator's outgoing duplication, percent; 0 .. 100; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_client_connect()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_client_validate_config_sized() does not make that check.
     */
    int simulated_duplicate_percent;                // development only: outgoing duplication 0..100; 0 is off
};

#define ROCKETNET_CLIENT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_config_t, reliable_timeout_ms ) + sizeof( int ), sizeof( void * ) )
#define ROCKETNET_CLIENT_CONFIG_BYTES_2 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_config_t, simulated_duplicate_percent ) + sizeof( int ), sizeof( void * ) )
#define ROCKETNET_CLIENT_CONFIG_KNOWN_BYTES ROCKETNET_CLIENT_CONFIG_BYTES_1, ROCKETNET_CLIENT_CONFIG_BYTES_2

// The server is configured explicitly: rocketnet_server_default_config gives
// the shape with no keys, and start refuses it until keys are supplied
// (through the struct or the environment), because default keys are how
// every deployment of a protocol ends up sharing one identity.
// rocketnet_server_dev_config fills the published development keys and test
// filter for local work, loudly.
//
// public_address, when set, displaces the create call's address at start, so the
// address the transport binds and the address connect tokens are checked
// against are always the same one. ROCKETNET_SERVER_PUBLIC_ADDRESS applies to
// the first server start in the process only, and says so in the log, so a
// process with several servers is never redirected wholesale by one variable.

/**
 * @brief Complete server creation configuration.
 *
 * Initialize with rocketnet_server_default_config() for production or
 * rocketnet_server_dev_config() for local development, then set deployment
 * identity and policy before creation. Creation copies string values. Fields
 * marked WIRE must match every client. Memory sizing and creation apply the
 * environment overrides of max_clients, the message fields and the send
 * budget; rocketnet_server_start() applies the overrides of the other
 * transport fields, the keys included, and checks them.
 * rocketnet_server_validate_config() reads no environment.
 */
struct rocketnet_server_config_t
{
    /** @brief ROCKETNET_SERVER_PROTOCOL_ID: the title's protocol id; connect tokens must carry it */
    uint64_t protocol_id;                           // ROCKETNET_SERVER_PROTOCOL_ID: the title's protocol id; connect tokens must carry it
    /** @brief ROCKETNET_SERVER_MAX_CLIENTS: 1 .. ROCKETNET_MAX_CLIENTS; default 1000 */
    int max_clients;                                // ROCKETNET_SERVER_MAX_CLIENTS: 1 .. ROCKETNET_MAX_CLIENTS; default 1000
    /** @brief ROCKETNET_SERVER_FILTER_VALUE: the per-title packet filter, as the clients' */
    uint8_t filter_value[4];                        // ROCKETNET_SERVER_FILTER_VALUE: the per-title packet filter, as the clients'
    /** @brief ROCKETNET_SERVER_BACKEND_PUBLIC_KEY: verifies connect token signatures */
    uint8_t backend_public_key[32];                 // ROCKETNET_SERVER_BACKEND_PUBLIC_KEY: verifies connect token signatures
    /** @brief ROCKETNET_SERVER_SERVER_PUBLIC_KEY: public key used to seal server-bound token data. */
    uint8_t server_public_key[32];                  // ROCKETNET_SERVER_SERVER_PUBLIC_KEY
    /** @brief ROCKETNET_SERVER_SERVER_SECRET_KEY: secret, logged redacted */
    uint8_t server_secret_key[32];                  // ROCKETNET_SERVER_SERVER_SECRET_KEY: secret, logged redacted
    /** @brief ROCKETNET_SERVER_NUM_XDP_QUEUES: xdp and gso only; 1 .. 1024; 0 is the transport default */
    int num_xdp_queues;                             // ROCKETNET_SERVER_NUM_XDP_QUEUES: xdp and gso only; 1 .. 1024; 0 is the transport default
    /** @brief ROCKETNET_SERVER_NUM_OS_CPUS: xdp only, the cores left to the OS; 1 .. 256; 0 is the transport default */
    int num_os_cpus;                                // ROCKETNET_SERVER_NUM_OS_CPUS: xdp only, the cores left to the OS; 1 .. 256; 0 is the transport default
    /** @brief ROCKETNET_SERVER_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES */
    int max_payload_bytes;                          // ROCKETNET_SERVER_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    /** @brief ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS: per client; 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS */
    int num_reassembly_slots;                       // ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS: per client; 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    /** @brief ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS: 1 .. 3600; default 60 */
    int max_timeout_seconds;                        // ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS: 1 .. 3600; default 60
    /** @brief ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS: 1 .. 60; default 5 */
    int handshake_deadline_seconds;                 // ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS: 1 .. 60; default 5
    /** @brief ROCKETNET_SERVER_TRANSPORT: the candidate list, tried in order at rocketnet_server_start(); NULL is "auto"; every transport, xdp included, serves IPv4 and IPv6, one family per server (the family of its public address) */
    const char * transport;                         // ROCKETNET_SERVER_TRANSPORT: the candidate list; NULL is "auto"; every transport, xdp included, serves IPv4 and IPv6, one family per server (the family of its public address)
    /** @brief ROCKETNET_SERVER_PUBLIC_ADDRESS: "a.b.c.d:port" or "[v6]:port"; NULL is the create call's address */
    const char * public_address;                    // ROCKETNET_SERVER_PUBLIC_ADDRESS: "a.b.c.d:port" or "[v6]:port"; NULL is the create call's address
    /** @brief ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS: each client's send budget, kbps; default 10000; 0 or less disables the budget in this direction (server to each client), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused */
    int per_client_send_kbps;                       // ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS: each client's send budget, kbps; default 10000; 0 or less disables the budget in this direction (server to each client), NOT RECOMMENDED; above ROCKETNET_SEND_BUDGET_MAX_KBPS is refused
    /** @brief ROCKETNET_SERVER_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32 */
    int num_channels;                               // ROCKETNET_SERVER_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    /** @brief ROCKETNET_SERVER_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024 */
    int window_messages;                            // ROCKETNET_SERVER_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    /** @brief ROCKETNET_SERVER_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144 */
    int receive_ring_bytes;                         // ROCKETNET_SERVER_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    /** @brief ROCKETNET_SERVER_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144 */
    int max_message_bytes;                          // ROCKETNET_SERVER_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    /** @brief ROCKETNET_SERVER_SEND_BUFFER_BYTES: queued message bytes; per client; 12288 .. 1 << 28; default 800000 */
    int send_buffer_bytes;                          // ROCKETNET_SERVER_SEND_BUFFER_BYTES: queued message bytes; per client; 12288 .. 1 << 28; default 800000
    /** @brief ROCKETNET_SERVER_SEND_QUEUE_MESSAGES: queued messages; per client; 16 .. 1 << 20; default 1024 */
    int send_queue_messages;                        // ROCKETNET_SERVER_SEND_QUEUE_MESSAGES: queued messages; per client; 16 .. 1 << 20; default 1024
    /** @brief ROCKETNET_SERVER_PACKETS_IN_FLIGHT: unacknowledged packets tracked, the packets_in_flight window; per client; 16 .. 1 << 16; default 256 */
    int packets_in_flight;                          // ROCKETNET_SERVER_PACKETS_IN_FLIGHT: unacknowledged packets tracked; per client; 16 .. 1 << 16; default 256
    /**
     * @brief ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE: max_packets_per_update, the most packets sent to each client in one update; 1 .. 1024; default 256.
     *
     * Per client: in-place unreliable messages sent at finish and what
     * rocketnet_server_send_packets() writes count together against this one
     * allowance per update. While reliable work is due, an in-place message
     * never takes the last of the allowance, so that work is sent in the
     * same update. The send budget, not this field, is the throttle.
     */
    int max_packets_per_update;                     // ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE: packets sent per update, in-place unreliable finish messages and send_packets together; per client; 1 .. 1024; default 256; the send budget is the throttle
    /** @brief ROCKETNET_SERVER_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per client per update; 1 .. 1 << 16; default 256 */
    int unreliable_deliveries;                      // ROCKETNET_SERVER_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per client per update; 1 .. 1 << 16; default 256
    /** @brief ROCKETNET_SERVER_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16 */
    int sequenced_holds;                            // ROCKETNET_SERVER_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    /** @brief Maximum queued receipts per server client, 16 through 65536 (default 256).
     * A full ring drops its oldest receipt; ROCKETNET_SERVER_RECEIPT_RING can override it.
     */
    int receipt_ring;                               // ROCKETNET_SERVER_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    /** @brief ROCKETNET_SERVER_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100 */
    int rto_initial_ms;                             // ROCKETNET_SERVER_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100
    /** @brief ROCKETNET_SERVER_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10 */
    int rto_min_ms;                                 // ROCKETNET_SERVER_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10
    /** @brief ROCKETNET_SERVER_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250 */
    int rto_max_ms;                                 // ROCKETNET_SERVER_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250
    /** @brief ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000 */
    int reliable_timeout_ms;                        // ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
    /**
     * @brief Development only: the link simulator's outgoing loss, percent; 0 .. 100; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_server_start()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_server_validate_config_sized() does not make that check.
     */
    int simulated_loss_percent;                     // development only: outgoing loss 0..100; 0 is off
    /**
     * @brief Development only: the link simulator's added outgoing latency, milliseconds; 0 .. 60000; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_server_start()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_server_validate_config_sized() does not make that check.
     */
    int simulated_latency_ms;                       // development only: outgoing latency in milliseconds; 0 is off
    /**
     * @brief Development only: the link simulator's added outgoing jitter, milliseconds; 0 .. 60000; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_server_start()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_server_validate_config_sized() does not make that check.
     */
    int simulated_jitter_ms;                        // development only: outgoing jitter in milliseconds; 0 is off
    /**
     * @brief Development only: the link simulator's outgoing duplication, percent; 0 .. 100; default 0, which is off.
     *
     * A revision-2 field: only a _sized call given the revision-2 size reads or
     * writes it. The unsized validate, required_memory and create calls take it
     * at its default, 0, and the unsized
     * default calls leave it untouched. A library built without development
     * support refuses a nonzero value at rocketnet_server_start()
     * (ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, one error line naming the field);
     * rocketnet_server_validate_config_sized() does not make that check.
     */
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
// allocates its own at client connect and server start).
//
// time is the game's clock, a double in seconds, monotonic; pass the same
// clock to every call that takes one. The library reads no clock of its own
// on these paths, so a game owns time and a test can hold it still.
//
// The update order, on the main thread: update (messages in, acks into the
// channels, received messages into deliveries, then the checks that
// disconnect), receive and release, start message, write, finish message
// (or abort message), then send_packets (each connection's messages, the
// queued messages included in them, under its send budget, out to the wire).
//
// Sending: start message, write in place, finish message is the only send
// path. start message returns a pointer into the connection's region for the
// message, write the message there once (copy it in when the caller already
// holds a buffer), and
// finish message (bytes_written) queues it and returns its receipt serial,
// nonzero; a NULL start message is a refused send, 0. The pointer is valid
// until finish message or abort message; abort message gives the room back.
// One start message is pending per object at a time.
// reliability is a rocketnet_reliability_t. An unreliable send too
// big for one packet's record is promoted to its reliable twin, as RakNet's
// ReliabilityLayer does, and counted in the internal counter set, not the
// public counter enums (MESSAGE_PROMOTED_UNRELIABLE,
// MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED,
// MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT); the receiver sees the twin's
// wire code. Plain UNRELIABLE is one record in one packet up to 1411 bytes
// and promotes to RELIABLE above that.
// UNRELIABLE_SEQUENCED is one record up to 1405 bytes and promotes to
// RELIABLE_SEQUENCED past that, keeping its channel (1404 on a channel of 256
// or more, which exists only in a build that defines ROCKETNET_MAX_CHANNELS
// above 256; the default is 32).
// UNRELIABLE_WITH_ACK_RECEIPT is the code-0 wire form up to 1411 bytes and
// promotes to RELIABLE_WITH_ACK_RECEIPT semantics (the code-2 block path)
// above that, keeping its ACKED-only receipt. The reliable types and promoted
// sends are bounded by the config's max_message_bytes; a send above it is
// refused in every unreliable mode, with no second record-length refusal.
//
// A send the connection's region has no room for ends the connection: the
// server disconnects that client, the client disconnects itself, with the
// reason ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED. The send budget
// governs the wire, never the queue: a reliable message waits for budget and
// is never dropped; an unreliable one over budget is dropped and counted
// (the BUDGET_DROPS counters).
//
// Receiving: receive hands out the next message of the update on a channel
// as a const view and its length, valid until the next update. Release every
// received message before the next update; update with one still held
// asserts in a debug build and returns ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED
// in a release build. Copy what is kept past the update.

/** @brief One outcome for a send's nonzero receipt serial.
 * Receipts are copied into caller storage after an update. A full receipt ring
 * drops its oldest entry; copy any result needed beyond the next update.
 */
struct rocketnet_receipt_t
{
    /** @brief Identify the send whose outcome is reported. */
    uint32_t serial;
    /** @brief State whether the peer transport acknowledged or missed it: a
     * rocketnet_receipt_status_t value, ROCKETNET_RECEIPT_STATUS_ACKED or
     * ROCKETNET_RECEIPT_STATUS_LOSS, held in a fixed 32-bit field. */
    uint32_t status;
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
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_generate_backend_keypair( uint8_t * public_key, uint8_t * secret_key );

// Derives the backend's Ed25519 signing keypair from a caller seed: seed is
// ROCKETNET_BACKEND_SEED_BYTES long, and public_key and secret_key are as
// above. The same seed always gives the same keypair. Returns
// ROCKETNET_STATUS_OK, or ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
// pointer, or ROCKETNET_STATUS_CRYPTO_INIT_FAILED when the crypto library
// refuses. Secret keys are never logged.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_generate_backend_keypair_from_seed( const uint8_t * seed, uint8_t * public_key, uint8_t * secret_key );

// The server's X25519 sealing keypair: public_key and secret_key are each
// ROCKETNET_SERVER_PUBLIC_KEY_BYTES and ROCKETNET_SERVER_SECRET_KEY_BYTES
// long. The server keeps the secret key; the backend seals tokens to the
// public key. Returns the same codes as the backend keypair call.
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_generate_server_keypair( uint8_t * public_key, uint8_t * secret_key );

/** @brief Mint a production connect token for one client and one or two server addresses.
 *
 * Signs the token with the backend's secret key and seals its private part,
 * with fresh random session keys, to the server's public key. The token
 * carries client_id, protocol_id, the server addresses, create_timestamp,
 * expire_timestamp, timeout_seconds and user_data. The call reads no clock;
 * the backend chooses both timestamps. Secret keys are never logged.
 *
 * @param backend_secret_key [in] ROCKETNET_BACKEND_SECRET_KEY_BYTES-byte signing secret held by the backend; non-NULL.
 * @param server_public_key [in] ROCKETNET_SERVER_PUBLIC_KEY_BYTES-byte sealing public key of the server; non-NULL.
 * @param protocol_id [in] Nonzero application protocol identifier.
 * @param server_address_strings [in] Array of server_address_count numeric address strings, each non-NULL; non-NULL.
 * @param server_address_count [in] 1 or 2, with at most one address of each IP family.
 * @param client_id [in] Application client identifier carried in the sealed part of the token.
 * @param create_timestamp [in] Unix creation time in seconds.
 * @param expire_timestamp [in] Unix expiry time in seconds.
 * @param timeout_seconds [in] Connection timeout carried in the token, in seconds.
 * @param user_data [in] ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES bytes, or NULL for all zeros.
 * @param connect_token [out] Caller-owned output buffer; non-NULL.
 * @param connect_token_bytes [in] Size of connect_token in bytes, at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return ROCKETNET_STATUS_OK with ROCKETNET_CONNECT_TOKEN_BYTES written;
 * ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL pointer argument (other than
 * user_data) or address string, a count other than 1 or 2, two addresses of
 * one family, or a buffer shorter than ROCKETNET_CONNECT_TOKEN_BYTES;
 * ROCKETNET_STATUS_INVALID_PROTOCOL_ID for a zero protocol_id;
 * ROCKETNET_STATUS_INVALID_ADDRESS for an address string that does not parse;
 * ROCKETNET_STATUS_CRYPTO_INIT_FAILED when sealing or signing fails. Every
 * refusal before the crypto step writes nothing; after
 * ROCKETNET_STATUS_CRYPTO_INIT_FAILED the buffer is not a token. Correct the
 * arguments and call again. A NULL pointer is also a counted misuse: it
 * increments the API misuse count and fires an assert in a debug build.
 */
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

// -----------------------------------------
//                             client
// -----------------------------------------

// The client connects with a connect token, never an address: the token names
// the server, carries the keys, and is the credential -- minted by a backend
// in production, or locally by rocketnet_write_dev_connect_token against the
// published development keys.
//
// The client is reusable, as netcode's is: create makes a DISCONNECTED
// client; rocketnet_client_connect starts a connection with a token;
// rocketnet_client_disconnect ends it, from any state, and returns the client
// to DISCONNECTED, ready to connect again with a new token; destroy works
// from any state. Each connection starts fresh: nothing of an earlier one
// is delivered or kept except the lifetime counters and the last disconnect
// reason. The socket is per connection, opened by connect for the address
// family the token names.

/**
 * @brief Writes the revision-1 client defaults into config.
 * @param config Destination structure; NULL is a counted misuse that writes nothing.
 * @return ROCKETNET_STATUS_OK after writing, or ROCKETNET_STATUS_INVALID_ARGUMENT
 * for a NULL config.
 * @details Writes only the revision-1 prefix of the structure; the fields added
 * after revision 1 (the simulated_* link simulator fields) are left as they
 * were. Use rocketnet_client_default_config_sized() with sizeof(*config) to
 * initialize every field of the current structure.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config( struct rocketnet_client_config_t * config );

/**
 * @brief Writes client defaults only for a recognized ABI structure size.
 * @param config Destination structure; NULL is a counted misuse that writes nothing.
 * @param config_bytes Size in bytes of the caller's client config structure, normally sizeof(*config).
 * @return ROCKETNET_STATUS_OK after writing the first config_bytes of the
 * defaults; ROCKETNET_STATUS_INVALID_ARGUMENT, with nothing written, for a NULL
 * config or an unknown size (one error line naming the current size).
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_default_config_sized( struct rocketnet_client_config_t * config, size_t config_bytes );

/**
 * @brief Validates the values stored in a client configuration.
 * @param config Configuration to validate, or NULL to validate the defaults.
 * @return ROCKETNET_STATUS_OK when the stored values pass every range and
 * cross-field rule; ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE after one error line
 * naming the violated rule.
 * @details Reads only the revision-1 prefix of config and takes the later
 * fields at their defaults; use rocketnet_client_validate_config_sized() to
 * check them. Environment variables are not read.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_validate_config( const struct rocketnet_client_config_t * config );
/**
 * @brief Validates a client configuration of a known structure size.
 * @param config Configuration to validate, or NULL to validate the defaults.
 * @param config_bytes Size in bytes of the caller's structure, normally
 * sizeof(*config); ignored when config is NULL.
 * @return ROCKETNET_STATUS_OK when the values pass every range and cross-field
 * rule; ROCKETNET_STATUS_INVALID_ARGUMENT after one error line for an unknown
 * size; ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE after one error line for a
 * violated rule.
 * @details Reads the first config_bytes of config and takes every later field
 * at its default. Environment variables are not read. A library built without
 * development support refuses nonzero simulated_* fields at
 * rocketnet_client_connect(); this call does not make that check.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_validate_config_sized( const struct rocketnet_client_config_t * config, size_t config_bytes );

/**
 * @brief Calculates the caller-owned memory a client needs.
 * @param config Configuration to size, or NULL for defaults. The environment
 * overrides of the message and send budget fields are applied.
 * @return Required bytes, or 0 when the configuration or one of those
 * environment overrides is refused.
 * @details Reads only the revision-1 prefix of config and takes the later
 * fields at their defaults. Pass a block of at least this many bytes, aligned
 * to ROCKETNET_ALLOCATION_ALIGNMENT, to rocketnet_client_create(), or pass NULL
 * memory to let the library allocate. The transport fields are checked at
 * rocketnet_client_connect(), not here.
 */
ROCKETNET_EXPORT size_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );
/**
 * @brief Calculates the caller-owned memory a client needs, for a configuration of a known structure size.
 * @param config Configuration to size, or NULL for defaults. The environment
 * overrides of the message and send budget fields are applied.
 * @param config_bytes Size in bytes of the caller's structure, normally
 * sizeof(*config); ignored when config is NULL.
 * @return Required bytes, or 0 after one error line for an unknown size, a
 * refused value or a refused environment override.
 * @details Reads the first config_bytes of config and takes every later field
 * at its default. The result is the memory_bytes that
 * rocketnet_client_create_sized() needs for the same config and config_bytes.
 */
ROCKETNET_EXPORT size_t rocketnet_client_required_memory_sized( const struct rocketnet_client_config_t * config, size_t config_bytes );

#if ROCKETNET_DEVELOPMENT
/** @brief Mint a development connect token for one numeric server address.
 *
 * Signs the token with the published development backend key and seals it to
 * the published development server key, with ROCKETNET_DEV_PROTOCOL_ID, a
 * token that expires ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS after
 * unix_timestamp, a ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS timeout,
 * fresh random session keys and all-zero user data. Declared only in a
 * development build (ROCKETNET_DEVELOPMENT); a release library does not
 * export it. The published keys secure nothing: never use these tokens in
 * production.
 *
 * @param server_address_string [in] Numeric IPv4 or IPv6 address with port; non-NULL.
 * @param client_id [in] Application client identifier carried in the token.
 * @param unix_timestamp [in] Current Unix time in seconds, supplied by the caller; the call reads no clock.
 * @param connect_token [out] Caller-owned output buffer; non-NULL.
 * @param connect_token_bytes [in] Size of connect_token in bytes, at least ROCKETNET_CONNECT_TOKEN_BYTES.
 * @return ROCKETNET_STATUS_OK with ROCKETNET_CONNECT_TOKEN_BYTES written;
 * ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL server_address_string or
 * connect_token, or for a buffer shorter than ROCKETNET_CONNECT_TOKEN_BYTES
 * (logged, nothing written); ROCKETNET_STATUS_INVALID_ADDRESS for an address
 * that does not parse as IPv4 or IPv6; ROCKETNET_STATUS_CRYPTO_INIT_FAILED
 * when sealing or signing fails, after which the buffer is not a token. A
 * NULL pointer is also a counted misuse: it increments the API misuse count
 * and fires an assert in a debug build.
 *
 * @code
 * uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
 * if ( rocketnet_write_dev_connect_token( "127.0.0.1:40000", client_id, now_unix_seconds,
 *                                         token, sizeof( token ) ) == ROCKETNET_STATUS_OK )
 *     rocketnet_client_connect( client, token );
 * @endcode
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_write_dev_connect_token( const char * server_address_string, uint64_t client_id, uint64_t unix_timestamp, uint8_t * connect_token, size_t connect_token_bytes );
#endif // #if ROCKETNET_DEVELOPMENT

/**
 * @brief Create a DISCONNECTED client; connect it with rocketnet_client_connect.
 *
 * create resolves the config (the struct, then the environment overrides),
 * makes the one memory block, reads no clock and opens no socket.
 *
 * @param context Caller pointer passed back to the allocator and the callbacks; may be NULL.
 * @param config The config, read for this call only; NULL is the defaults.
 * @param memory NULL to allocate the block, or a caller block of at least
 * rocketnet_client_required_memory bytes aligned to
 * ROCKETNET_ALLOCATION_ALIGNMENT, which then stays the caller's.
 * @param memory_bytes The caller block's size in bytes; ignored when memory is NULL.
 * @return The client, or NULL when the config or an environment override is
 * refused, the caller block is short or misaligned, or the allocation fails;
 * each refusal logs a line.
 */
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config, void * memory, size_t memory_bytes );

/**
 * @brief Creates a disconnected client while checking the caller's configuration ABI size.
 * @param context Opaque value supplied to callbacks and allocator operations.
 * @param config Configuration to copy, or NULL for defaults.
 * @param config_bytes Size in bytes of config, normally sizeof(*config); ignored when config is NULL and otherwise must be a known size.
 * @param memory Optional caller-owned block aligned to ROCKETNET_ALLOCATION_ALIGNMENT, or NULL to let the library allocate.
 * @param memory_bytes Available bytes in memory; size it with rocketnet_client_required_memory_sized() given the same config and config_bytes.
 * @return A new client in the DISCONNECTED state, or NULL on refusal. An
 * unknown config size is refused with one error line before any other check; a
 * refused message or send budget value, a short or misaligned memory block, or
 * a failed allocation also returns NULL. rocketnet_client_connect() starts a
 * connection and checks the transport fields; rocketnet_client_destroy() frees
 * the client.
 *
 * @code
 * struct rocketnet_client_config_t config;
 * rocketnet_client_default_config_sized( &config, sizeof( config ) );
 * config.simulated_latency_ms = 50;
 * struct rocketnet_client_t * client = rocketnet_client_create_sized( NULL, &config, sizeof( config ), NULL, 0 );
 * @endcode
 */
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create_sized( void * context, const struct rocketnet_client_config_t * config, size_t config_bytes, void * memory, size_t memory_bytes );

/**
 * @brief Start a connection with a connect token, from DISCONNECTED only.
 *
 * Call rocketnet_init first: the transport refuses a connect before it. The
 * socket opens here, for the address family the token names. connect reads
 * no clock: the attempt's clock starts at the next update. A connection the
 * server ended (DISCONNECTED, its object still held) is ended here first,
 * its reason kept.
 *
 * @param client The client.
 * @param connect_token The token, ROCKETNET_CONNECT_TOKEN_BYTES long, read for this call only.
 * @return ROCKETNET_STATUS_OK when the attempt starts;
 * ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED from any other state, nothing
 * changed (call rocketnet_client_disconnect first);
 * ROCKETNET_STATUS_INVALID_CONNECT_TOKEN or ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED
 * when the token fails the pre-send checks, leaving the matching error
 * state (get a fresh token); the platform's status when a socket or thread
 * is refused, leaving the client DISCONNECTED; ROCKETNET_STATUS_INVALID_ARGUMENT
 * for a NULL client or token, a counted no-op.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_connect( struct rocketnet_client_t * client, const uint8_t * connect_token );

/**
 * @brief Destroy a client from any state.
 *
 * A live connection ends as rocketnet_client_disconnect ends it, then the
 * client is freed; a caller memory block is cleared and given back to the
 * caller. Once the session keys exist (SENDING_CONNECTION_RESPONSE or
 * CONNECTED) the redundant disconnect packets are sent before the transport
 * closes, so the server frees the slot on receipt rather than at timeout;
 * from DISCONNECTED, an error state, REQUESTING_COOKIE or
 * SENDING_CONNECTION_REQUEST nothing is sent. Every pointer from the client
 * is invalid after the call.
 *
 * @param client The client; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

/**
 * @brief Run the client's frame step first in every frame: pump the transport and take in what arrived.
 *
 * update takes in what the transport received, applies acks, makes this frame's received
 * messages ready for rocketnet_client_receive_message, and runs the checks
 * that end a connection. The previous update's received message views end
 * here, and an in-place start message still open is aborted, logged and
 * counted in rocketnet_api_misuse_count. A client with no connection
 * returns ROCKETNET_UPDATE_OK and does nothing.
 *
 * @param client The client.
 * @param time The game clock in seconds, the same monotonic clock every call
 * takes; a NaN or a time earlier than the last update's is replaced by the
 * last update's time.
 * @return ROCKETNET_UPDATE_OK; ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED when a
 * received message from the previous update is still held: the update still
 * runs and takes the views back, the count goes into
 * ROCKETNET_CLIENT_COUNTER_MESSAGES_NOT_RELEASED, and a debug build asserts;
 * release every message before the next update. A NULL client is a counted
 * no-op that returns ROCKETNET_UPDATE_OK.
 */
ROCKETNET_EXPORT enum rocketnet_update_result_t rocketnet_client_update( struct rocketnet_client_t * client, double time );

/**
 * @brief Send this frame's queued messages; call it last in the frame.
 *
 * Call it after update, receive and release, the simulation and every start
 * message and finish message of the frame, so this frame's messages leave
 * this frame; called earlier, they wait a whole frame. It writes the
 * connection's queued messages out to the wire under the send budget: a
 * reliable message waits for budget and is never
 * dropped; an unreliable one over budget is dropped and counted in
 * ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS. An in-place start message still
 * open is aborted, logged and counted in rocketnet_api_misuse_count. A
 * client with no connection does nothing.
 *
 * @param client The client; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

/**
 * @brief End the connection, if any, and return the client to DISCONNECTED.
 *
 * From a handshake state or CONNECTED the disconnect packets are sent
 * first; from an error state the error state ends. The connection's
 * counters move into the client's lifetime counters and its received
 * message views end. The client stays valid and can connect again with a
 * new token.
 *
 * @param client The client; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

/**
 * @brief Read the client's state.
 * @param client The client.
 * @return The current rocketnet_client_state_t: DISCONNECTED, a handshake
 * state, CONNECTED, or the negative error state the last attempt ended in,
 * kept until rocketnet_client_disconnect. A NULL client returns
 * ROCKETNET_CLIENT_STATE_INVALID, counted and logged.
 */
ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

// the name of a state ("connected", "invalid"), or "unknown" for a value that is not one.
ROCKETNET_EXPORT const char * rocketnet_client_state_name( enum rocketnet_client_state_t state );

/**
 * @brief Read why the last connection ended.
 * @param client The client.
 * @return The reason the last connection ended with, kept until the next
 * connection is established; ROCKETNET_DISCONNECT_REASON_NONE while
 * connected, after a deliberate disconnect, and for a NULL client (a
 * counted no-op); ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED
 * when the server ended the connection.
 */
ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_client_disconnect_reason( const struct rocketnet_client_t * client );

// the name of a disconnect reason ("none", "reliable timeout"), or "unknown" for a value that is not one.
ROCKETNET_EXPORT const char * rocketnet_disconnect_reason_name( enum rocketnet_disconnect_reason_t reason );

// the client's index on the server, valid while connected. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_client_index( const struct rocketnet_client_t * client );

// the transport that won the candidate list at connect ("portable", "msgx"), NULL while DISCONNECTED. a NULL client is a counted no-op that returns NULL.
ROCKETNET_EXPORT const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client );

/**
 * @brief Start a message: get the room to write it in, then finish it or abort it.
 *
 * start message returns a pointer into the connection's region for a
 * message of bytes, or, for an unreliable message whose declared bytes fit
 * one packet with every header, into the transport packet buffer itself
 * (docs/OUTGOING-BUILD.md): the message is written in place, with no copy
 * between the message layer and the transport. Write the message there
 * once, or copy it in when the caller already holds a buffer. The pointer
 * is valid until rocketnet_client_finish_message or
 * rocketnet_client_abort_message; one start message is pending per client
 * at a time. Messages flow only while CONNECTED.
 *
 * An unreliable message too big for one packet's record is promoted to its
 * reliable twin (the messages section above). A start the connection's
 * region has no room for ends the connection with
 * ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED.
 *
 * @code
 * uint8_t * out = rocketnet_client_start_message( client, channel, reliability, bytes );
 * if ( out )
 * {
 *     memcpy( out, data, bytes );
 *     serial = rocketnet_client_finish_message( client, bytes );
 * }
 * @endcode
 *
 * @param client The client.
 * @param channel The channel, 0 to the config's num_channels - 1.
 * @param reliability How the message is delivered.
 * @param bytes The most bytes the message holds, 1 to the config's max_message_bytes.
 * @return The room, or NULL when refused: a client not CONNECTED (counted
 * in rocketnet_api_misuse_count), a start already pending, a channel,
 * reliability or size out of range, or no room in the region. A NULL
 * client is a counted no-op that returns NULL.
 */
ROCKETNET_EXPORT uint8_t * rocketnet_client_start_message( struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int bytes );

/**
 * @brief Finish the pending start message: queue it, and get its receipt serial.
 *
 * The message joins its channel's queue in order and leaves at the next
 * rocketnet_client_send_packets. A message written in place has its
 * packet buffer completed and staged here, with no copy; each such finish
 * counts against the config's max_packets_per_update.
 *
 * @param client The client.
 * @param bytes_written The bytes written at the start message's pointer, 1 to the bytes started.
 * @return The receipt serial, nonzero; a receipt reliability reports it
 * through rocketnet_client_receipts. 0 when no start message is pending,
 * bytes_written is out of range (the start stays pending: finish again or
 * abort), or the client is not CONNECTED (counted). A NULL client is a
 * counted no-op that returns 0.
 */
ROCKETNET_EXPORT uint32_t rocketnet_client_finish_message( struct rocketnet_client_t * client, int bytes_written );

/**
 * @brief Abort the pending start message and give its room back.
 *
 * A message written in place gives its packet buffer back to the transport.
 * With no start message pending, or no connection, it does nothing.
 *
 * @param client The client; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_abort_message( struct rocketnet_client_t * client );

/**
 * @brief Take the next received message on a channel, after update.
 *
 * Each call hands out the next message the last update took in on the
 * channel, as a const view and its length. The view is valid until the
 * next update (or disconnect or destroy); release it with
 * rocketnet_client_release_message before the next update, and copy what
 * is kept past it.
 *
 * @param client The client.
 * @param channel The channel, 0 to the config's num_channels - 1.
 * @param data Out: the message's bytes, owned by the client.
 * @param bytes Out: the message's length in bytes.
 * @return 1 with the next message; 0 when there is none, the channel is out
 * of range, or the client has no connection. On 0, do not read data or
 * bytes. A NULL client, data or bytes is a counted no-op that returns 0.
 */
ROCKETNET_EXPORT int rocketnet_client_receive_message( struct rocketnet_client_t * client, int channel, const uint8_t ** data, int * bytes );

/**
 * @brief Release a received message view; release every one before the next update.
 * @param client The client.
 * @param data The data pointer rocketnet_client_receive_message handed out.
 * A pointer that is not a held view, or a release on a client with no
 * connection, is counted in rocketnet_api_misuse_count. A NULL client or
 * data is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_client_release_message( struct rocketnet_client_t * client, const uint8_t * data );

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

/**
 * @brief Set the connection's send budget, client to server, from now on.
 * @param client The client.
 * @param kbps The rate in kilobits per second (1 kbps = 1000 bit/s), up to
 * ROCKETNET_SEND_BUDGET_MAX_KBPS. 0 or less sets the ceiling,
 * ROCKETNET_SEND_BUDGET_MAX_KBPS, which in practice lifts the budget (not
 * recommended).
 * @return ROCKETNET_STATUS_OK when the budget took the rate;
 * ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE above the ceiling, rate unchanged;
 * ROCKETNET_STATUS_INVALID_ARGUMENT for a client with no connection, and
 * for a NULL client (a counted no-op).
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

// a counter by its enum; 0 for a value outside the enum. a NULL client is a counted no-op that returns 0.
/**
 * @brief Read one of the client's cumulative counters.
 *
 * Call it on the thread that updates the client, between updates; never call
 * it alongside rocketnet_client_update on another thread, since the read
 * refreshes the client's acknowledgement statistics.
 * @param client The created client. NULL is an API misuse: it is counted in
 * rocketnet_api_misuse_count, fires an assert in a debug build, and returns 0.
 * @param counter A value of rocketnet_client_counter_t below
 * ROCKETNET_CLIENT_COUNTER_COUNT; any other value returns 0.
 * @return The counter's value since create, or 0 for a NULL client or a value
 * outside the enum. A 0 is never an error status.
 * @code
 * uint64_t resent = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT );
 * uint64_t sent = rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_PACKETS_SENT );
 * @endcode
 */
ROCKETNET_EXPORT uint64_t rocketnet_client_counter( const struct rocketnet_client_t * client, enum rocketnet_client_counter_t counter );

// a stat by its enum; 0 for a value outside the enum. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_client_stat( const struct rocketnet_client_t * client, enum rocketnet_client_stat_t stat );

// -----------------------------------------
//                             server
// -----------------------------------------

// Clients are identified by index, 0 .. max_clients - 1, the slot each
// connection takes, as a uint64_t (ROCKETNET_FLOW_ID_BIT is never set in a
// client index).
//
// The server is reusable, as netcode's is: create validates the config and
// makes the memory block but binds nothing, and the server starts stopped.
// rocketnet_server_start binds the transport and accepts clients;
// rocketnet_server_stop disconnects every client, resets every slot and
// releases the transport, and the server can start again. The per-client
// calls below are valid while running; on a stopped server they read 0.

/**
 * @brief Writes the revision-1 production server defaults, with zero-valued key material.
 * @param config Destination structure; NULL is a counted misuse that writes nothing.
 * @return ROCKETNET_STATUS_OK after writing, or ROCKETNET_STATUS_INVALID_ARGUMENT
 * for a NULL config.
 * @details rocketnet_server_start() refuses these defaults
 * (ROCKETNET_STATUS_MISSING_KEYS) until deployment keys are supplied through
 * the structure or the environment. Writes only the revision-1 prefix of the
 * structure; the fields added after revision 1 (the simulated_* link simulator
 * fields) are left as they were. Use rocketnet_server_default_config_sized()
 * with sizeof(*config) to initialize every field of the current structure.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config( struct rocketnet_server_config_t * config );

/**
 * @brief Writes server defaults only for a recognized ABI structure size.
 * @param config Destination structure; NULL is a counted misuse that writes nothing.
 * @param config_bytes Size in bytes of the caller's server config structure, normally sizeof(*config).
 * @return ROCKETNET_STATUS_OK after writing the first config_bytes of the
 * defaults; ROCKETNET_STATUS_INVALID_ARGUMENT, with nothing written, for a NULL
 * config or an unknown size (one error line naming the current size).
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_default_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes );

/**
 * @brief Writes the revision-1 defaults with the published development keys and test filter value.
 * @param config Destination structure; NULL is a counted misuse that writes nothing.
 * @return ROCKETNET_STATUS_OK after writing, or ROCKETNET_STATUS_INVALID_ARGUMENT
 * for a NULL config.
 * @details Writes only the revision-1 prefix of the structure; use
 * rocketnet_server_dev_config_sized() with sizeof(*config) for the fields
 * added after revision 1.
 * @warning Use this identity only for local development; all callers share the published secrets.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_dev_config( struct rocketnet_server_config_t * config );
/**
 * @brief Writes the development defaults for a known server structure size.
 * @param config Destination structure; NULL is a counted misuse that writes nothing.
 * @param config_bytes Size in bytes of the caller's structure, normally sizeof(*config).
 * @return ROCKETNET_STATUS_OK after writing the first config_bytes of the
 * development defaults; ROCKETNET_STATUS_INVALID_ARGUMENT, with nothing
 * written, for a NULL config or an unknown size (one error line).
 * @warning Use this identity only for local development; all callers share the published secrets.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_dev_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes );

/**
 * @brief Validates the values stored in a server configuration.
 * @param config Configuration to validate, or NULL to validate the defaults.
 * @return ROCKETNET_STATUS_OK when the stored range and cross-field rules
 * pass; ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE after one error line naming the
 * violated rule.
 * @details Reads only the revision-1 prefix of config and takes the later
 * fields at their defaults; use rocketnet_server_validate_config_sized() to
 * check them. Environment variables are not read and the keys are not checked.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_validate_config( const struct rocketnet_server_config_t * config );
/**
 * @brief Validates a server configuration of a known structure size.
 * @param config Configuration to validate, or NULL to validate the defaults.
 * @param config_bytes Size in bytes of the caller's structure, normally
 * sizeof(*config); ignored when config is NULL.
 * @return ROCKETNET_STATUS_OK when the values pass every range and cross-field
 * rule; ROCKETNET_STATUS_INVALID_ARGUMENT after one error line for an unknown
 * size; ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE after one error line for a
 * violated rule.
 * @details Reads the first config_bytes of config and takes every later field
 * at its default. Environment variables are not read and the keys are not
 * checked. A library built without development support refuses nonzero
 * simulated_* fields at rocketnet_server_start(); this call does not make that
 * check.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_validate_config_sized( const struct rocketnet_server_config_t * config, size_t config_bytes );

/**
 * @brief Calculates the caller-owned memory a server and all its client slots need.
 * @param config Configuration to size, or NULL for defaults. The environment
 * overrides of max_clients, the message fields and the send budget are applied.
 * @return Required bytes, or 0 when the configuration or one of those
 * environment overrides is refused.
 * @details Reads only the revision-1 prefix of config and takes the later
 * fields at their defaults. Pass a block of at least this many bytes, aligned
 * to ROCKETNET_ALLOCATION_ALIGNMENT, to rocketnet_server_create(), or pass NULL
 * memory to let the library allocate. The keys and the other transport fields
 * are checked at rocketnet_server_start(), not here.
 */
ROCKETNET_EXPORT size_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );
/**
 * @brief Calculates the caller-owned memory a server needs, for a configuration of a known structure size.
 * @param config Configuration to size, or NULL for defaults. The environment
 * overrides of max_clients, the message fields and the send budget are applied.
 * @param config_bytes Size in bytes of the caller's structure, normally
 * sizeof(*config); ignored when config is NULL.
 * @return Required bytes, or 0 after one error line for an unknown size, a
 * refused value or a refused environment override.
 * @details Reads the first config_bytes of config and takes every later field
 * at its default. The result is the memory_bytes that
 * rocketnet_server_create_sized() needs for the same config and config_bytes.
 */
ROCKETNET_EXPORT size_t rocketnet_server_required_memory_sized( const struct rocketnet_server_config_t * config, size_t config_bytes );

/**
 * @brief Create a stopped server; start it with rocketnet_server_start.
 *
 * create resolves the config (the struct, then the environment overrides),
 * copies the address and makes the one memory block; it binds nothing. The
 * default config has no keys, and start refuses it until keys are given.
 *
 * @param context Caller pointer passed back to the allocator and the callbacks; may be NULL.
 * @param address Where clients reach the server, "a.b.c.d:port" or
 * "[v6]:port", copied; a port of 0 takes an ephemeral port at start (read it
 * with rocketnet_server_port).
 * @param config The config, read for this call only; NULL is the defaults.
 * @param memory NULL to allocate the block, or a caller block of at least
 * rocketnet_server_required_memory bytes aligned to
 * ROCKETNET_ALLOCATION_ALIGNMENT, which then stays the caller's.
 * @param memory_bytes The caller block's size in bytes; ignored when memory is NULL.
 * @return The server, or NULL when the address is too long, the config or
 * an environment override is refused, the caller block is short or
 * misaligned, or the allocation fails; each refusal logs a line. A NULL
 * address is a counted no-op that returns NULL.
 */
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, void * memory, size_t memory_bytes );

/**
 * @brief Creates a stopped server while checking the caller's configuration ABI size.
 * @param context Opaque value supplied to callbacks and allocator operations.
 * @param address Bind and public address in a.b.c.d:port or [v6]:port form,
 * copied; must not be NULL and must be at most 255 characters. Port zero
 * asks rocketnet_server_start()
 * for an ephemeral port, which some transports refuse
 * (ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED).
 * @param config Configuration to copy, or NULL for defaults.
 * @param config_bytes Size in bytes of config, normally sizeof(*config); ignored when config is NULL and otherwise must be a known size.
 * @param memory Optional caller-owned block aligned to ROCKETNET_ALLOCATION_ALIGNMENT, or NULL to let the library allocate.
 * @param memory_bytes Available bytes in memory; size it with rocketnet_server_required_memory_sized() given the same config and config_bytes.
 * @return A new stopped server, or NULL on refusal. An unknown config size is
 * refused with one error line before any other check; a NULL or too long
 * address, a refused max_clients, message or send budget value, a short or
 * misaligned memory block, or a failed allocation also returns NULL. The keys
 * and the other transport fields are checked by rocketnet_server_start(), which
 * binds the address; rocketnet_server_destroy() frees the server.
 */
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create_sized( void * context, const char * address, const struct rocketnet_server_config_t * config, size_t config_bytes, void * memory, size_t memory_bytes );

/**
 * @brief Bind the transport at the server's address and accept clients, from stopped only.
 *
 * Call rocketnet_init first: the transport refuses a start before it.
 * Clients are accepted up to the config's max_clients.
 *
 * @param server The server.
 * @return ROCKETNET_STATUS_OK when running;
 * ROCKETNET_STATUS_SERVER_NOT_STOPPED for a running server, nothing
 * changed; the transport's status when it does not start
 * (ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED,
 * TRANSPORT_PORT_ZERO_UNSUPPORTED, MISSING_KEYS, THREAD_CREATE_FAILED,
 * TIMER_CREATE_FAILED, MUTEX_CREATE_FAILED, OUT_OF_MEMORY and others),
 * leaving the server stopped; ROCKETNET_STATUS_INVALID_ARGUMENT for a NULL
 * server, a counted no-op.
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_start( struct rocketnet_server_t * server );

/**
 * @brief Stop a running server: disconnect every client and release the transport.
 *
 * Every client is disconnected cleanly (the disconnect packets, sent before
 * the transport goes), every slot is reset, and the run's counters move
 * into the server's lifetime counters. Received message views end. The
 * server is stopped and can start again; stopping a stopped server does
 * nothing.
 *
 * @param server The server; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_stop( struct rocketnet_server_t * server );

/**
 * @brief Read whether the server is running.
 * @param server The server.
 * @return true after a start that succeeded and before stop; false while
 * stopped, and for a NULL server (a counted no-op).
 */
ROCKETNET_EXPORT bool rocketnet_server_running( const struct rocketnet_server_t * server );

/**
 * @brief Destroy a server from either state.
 *
 * A running server is stopped first, as rocketnet_server_stop does: every
 * connected client is sent the redundant disconnect packets before the
 * transport closes, and with no connected client nothing is sent. Then the
 * server's memory, keys included, is wiped and freed, and a caller memory
 * block is given back to the caller. Every pointer from the server is
 * invalid after the call.
 *
 * @param server The server; NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

/**
 * @brief Run the server's frame step first in every frame: pump the transport and take in what arrived.
 *
 * update takes in what the transport received, binds and frees slots, applies acks, makes this
 * frame's received messages ready for rocketnet_server_receive_message, and
 * runs the checks that disconnect a client. The previous update's received
 * message views end here, and an in-place start message still open is
 * aborted, logged and counted in rocketnet_api_misuse_count.
 *
 * @param server The server.
 * @param time The game clock in seconds, the same monotonic clock every call
 * takes; a NaN or a time earlier than the last update's is replaced by the
 * last update's time.
 * @return ROCKETNET_UPDATE_OK; ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED when a
 * received message from the previous update is still held: the update still
 * runs and takes the views back, the count goes into
 * ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED, and a debug build asserts;
 * release every message before the next update. A stopped server and a
 * NULL server are counted no-ops that return ROCKETNET_UPDATE_OK.
 */
ROCKETNET_EXPORT enum rocketnet_update_result_t rocketnet_server_update( struct rocketnet_server_t * server, double time );

/**
 * @brief Send this frame's queued messages to every client; call it last in the frame.
 *
 * Call it after update, receive and release, the simulation and every start
 * message and finish message of the frame, so this frame's messages leave
 * this frame; called earlier, they wait a whole frame. It writes each
 * connected client's queued messages out to the wire under that client's
 * send budget, with the staged disconnect packets: a reliable
 * message waits for budget and is never dropped; an unreliable one over
 * budget is dropped and counted in ROCKETNET_SERVER_COUNTER_BUDGET_DROPS. An
 * in-place start message still open is aborted, logged and counted in
 * rocketnet_api_misuse_count.
 *
 * @param server The server; a stopped server and NULL are counted no-ops.
 */
ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

// The port the server bound: the address's port, or the ephemeral port a
// port of 0 was given; 0 while stopped. Build a token address from it in
// tests and tools. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

// the transport that won the candidate list at start ("xdp", "gso", "io_uring", "msgx", "iocp", "rio", "portable", "loopback"), NULL while stopped. a NULL server is a counted no-op that returns NULL.
ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

/**
 * @brief Read whether a client holds a slot.
 * @param server The server.
 * @param client_index The slot, 0 to max_clients - 1.
 * @return true while the slot's connection is up; false for a free slot, an
 * index out of range, a stopped server, and a NULL server (a counted
 * no-op).
 */
ROCKETNET_EXPORT bool rocketnet_server_client_connected( const struct rocketnet_server_t * server, uint64_t client_index );

// the client id from the connect token of the client in the slot, 0 for a free slot. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, uint64_t client_index );

/**
 * @brief Disconnect one client deliberately and free its slot.
 *
 * The disconnect packets are staged and leave on the next
 * rocketnet_server_send_packets. Why is the game's decision. A free slot,
 * an index out of range, or a stopped server does nothing.
 *
 * @param server The server; NULL is a counted no-op.
 * @param client_index The slot, 0 to max_clients - 1.
 */
ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, uint64_t client_index );

/**
 * @brief Disconnect every connected client as rocketnet_server_disconnect_client does; the server keeps running.
 * @param server The server; a stopped server does nothing, and NULL is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_disconnect_all_clients( struct rocketnet_server_t * server );

/**
 * @brief Read why a slot's last connection ended.
 * @param server The server.
 * @param client_index The slot, 0 to max_clients - 1.
 * @return The reason the slot's last connection ended with, until the slot's
 * next connection; ROCKETNET_DISCONNECT_REASON_NONE for an index out of
 * range, a stopped server, and a NULL server (a counted no-op).
 */
ROCKETNET_EXPORT enum rocketnet_disconnect_reason_t rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, uint64_t client_index );

/**
 * @brief Start a message to one client: get the room to write it in, then finish it or abort it.
 *
 * start message returns a pointer into the connection's region for a
 * message of bytes, or, for an unreliable message whose declared bytes fit
 * one packet with every header, into the transport packet buffer itself
 * (docs/OUTGOING-BUILD.md): the message is written in place, with no copy
 * between the message layer and the transport. Write the message there
 * once, or copy it in when the caller already holds a buffer. The pointer
 * is valid until rocketnet_server_finish_message or
 * rocketnet_server_abort_message; one start message is pending per server
 * at a time, whatever the destination. To send to every connected client,
 * loop over the slots.
 *
 * An unreliable message too big for one packet's record is promoted to its
 * reliable twin (the messages section above). A start the connection's
 * region has no room for disconnects that client with
 * ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED.
 *
 * @code
 * uint8_t * out = rocketnet_server_start_message( server, destination_id, channel, reliability, bytes );
 * if ( out )
 * {
 *     memcpy( out, data, bytes );
 *     serial = rocketnet_server_finish_message( server, destination_id, bytes );
 * }
 * @endcode
 *
 * @param server The server.
 * @param destination_id A client index today, 0 to max_clients - 1. An id
 * with ROCKETNET_FLOW_ID_BIT set is a flow id, reserved for a future flow
 * feature and refused for now (counted; ROCKETNET_STATUS_FLOW_ID_RESERVED
 * in the log).
 * @param channel The channel, 0 to the config's num_channels - 1.
 * @param reliability How the message is delivered.
 * @param bytes The most bytes the message holds, 1 to the config's max_message_bytes.
 * @return The room, or NULL when refused: a flow id, an index out of range
 * or a stopped server (both counted in rocketnet_api_misuse_count), a slot
 * with no connection, a start already pending, a channel, reliability or
 * size out of range, or no room in the region. A NULL server is a counted
 * no-op that returns NULL.
 */
ROCKETNET_EXPORT uint8_t * rocketnet_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, enum rocketnet_reliability_t reliability, int bytes );

/**
 * @brief Finish the pending start message to a client: queue it, and get its receipt serial.
 *
 * The message joins its channel's queue in order and leaves at the next
 * rocketnet_server_send_packets. A message written in place has its
 * packet buffer completed and staged here, with no copy; each such finish
 * counts against the config's max_packets_per_update for the client.
 *
 * @param server The server.
 * @param destination_id The client index the start message named.
 * @param bytes_written The bytes written at the start message's pointer, 1 to the bytes started.
 * @return The receipt serial, nonzero; a receipt reliability reports it
 * through rocketnet_server_receipts. 0 when no start message is pending for
 * destination_id, bytes_written is out of range (the start stays pending:
 * finish again or abort), destination_id is a flow id or out of range, or
 * the server is stopped (counted). A NULL server is a counted no-op that
 * returns 0.
 */
ROCKETNET_EXPORT uint32_t rocketnet_server_finish_message( struct rocketnet_server_t * server, uint64_t destination_id, int bytes_written );

/**
 * @brief Abort the pending start message to a client and give its room back.
 *
 * A message written in place gives its packet buffer back to the
 * transport. With no start message pending for destination_id, an index
 * out of range, or a stopped server, it does nothing; a flow id is counted.
 *
 * @param server The server; NULL is a counted no-op.
 * @param destination_id The client index the start message named.
 */
ROCKETNET_EXPORT void rocketnet_server_abort_message( struct rocketnet_server_t * server, uint64_t destination_id );

/**
 * @brief Take the next received message from a client on a channel, after update.
 *
 * Each call hands out the next message the last update took in from the
 * client on the channel, as a const view and its length. The view is valid
 * until the next update (or stop or destroy); release it with
 * rocketnet_server_release_message before the next update, and copy what
 * is kept past it.
 *
 * @param server The server.
 * @param client_index The slot, 0 to max_clients - 1.
 * @param channel The channel, 0 to the config's num_channels - 1.
 * @param data Out: the message's bytes, owned by the server.
 * @param bytes Out: the message's length in bytes.
 * @return 1 with the next message; 0 when there is none, the slot is free
 * or out of range, the channel is out of range, or the server is stopped.
 * On 0, do not read data or bytes. A NULL server, data or bytes is a
 * counted no-op that returns 0.
 */
ROCKETNET_EXPORT int rocketnet_server_receive_message( struct rocketnet_server_t * server, uint64_t client_index, int channel, const uint8_t ** data, int * bytes );

/**
 * @brief Release a received message view; release every one before the next update.
 * @param server The server.
 * @param client_index The slot the message came from.
 * @param data The data pointer rocketnet_server_receive_message handed out.
 * A pointer that is not a held view of that slot, an index out of range or
 * a stopped server is counted in rocketnet_api_misuse_count. A NULL server
 * or data is a counted no-op.
 */
ROCKETNET_EXPORT void rocketnet_server_release_message( struct rocketnet_server_t * server, uint64_t client_index, const uint8_t * data );

/** @brief Copy one connected server client's receipt outcomes.
 * Call after update and before the next update, without a concurrent update.
 * Repeated calls copy the same visible entries; the server owns the internal
 * list, and the caller owns the output array.
 * @param server Server object; NULL returns 0.
 * @param client_index Client index in [0, configured max_clients); an invalid or
 * disconnected slot, or a stopped server, returns 0.
 * @param out Writable array with room for capacity entries; NULL returns 0.
 * @param capacity Array length in entries; values below 1 return 0.
 * @return Entries copied, from 0 through capacity. Zero means none are visible
 * or an input is invalid; it is not an error status.
 */
ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, uint64_t client_index, struct rocketnet_receipt_t * out, int capacity );

/**
 * @brief Set one client's send budget, server to that client, from now on.
 * @param server The server.
 * @param client_index The slot, 0 to max_clients - 1.
 * @param kbps The rate in kilobits per second (1 kbps = 1000 bit/s), up to
 * ROCKETNET_SEND_BUDGET_MAX_KBPS. 0 or less sets the ceiling,
 * ROCKETNET_SEND_BUDGET_MAX_KBPS, which in practice lifts the budget (not
 * recommended).
 * @return ROCKETNET_STATUS_OK when the slot's budget took the rate (a slot in
 * range is accepted whether or not a client holds it);
 * ROCKETNET_STATUS_FLOW_ID_RESERVED for a flow id (counted);
 * ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE above the ceiling, rate unchanged;
 * ROCKETNET_STATUS_INVALID_ARGUMENT for an index out of range, a stopped
 * server, and a NULL server (a counted no-op).
 */
ROCKETNET_EXPORT enum rocketnet_status_t rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, uint64_t client_index, int kbps );

// a counter by its enum; 0 for a value outside the enum. a NULL server is a counted no-op that returns 0.
/**
 * @brief Read one of the server's cumulative counters over all its clients.
 *
 * Call it on the thread that updates the server, between updates; never call
 * it alongside rocketnet_server_update on another thread.
 * @param server The created server. NULL is an API misuse: it is counted in
 * rocketnet_api_misuse_count, fires an assert in a debug build, and returns 0.
 * @param counter A value of rocketnet_server_counter_t below
 * ROCKETNET_SERVER_COUNTER_COUNT; any other value returns 0.
 * @return The counter's value since create, or 0 for a NULL server or a value
 * outside the enum. A 0 is never an error status.
 */
ROCKETNET_EXPORT uint64_t rocketnet_server_counter( const struct rocketnet_server_t * server, enum rocketnet_server_counter_t counter );

// a stat by its enum; 0 for a value outside the enum. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_server_stat( const struct rocketnet_server_t * server, enum rocketnet_server_stat_t stat );

// one client slot's counter by its enum; 0 for a free slot or a value outside the enum. a NULL server is a counted no-op that returns 0.
/**
 * @brief Read one counter of one connected client slot.
 *
 * Call it on the thread that updates the server, between updates; never call
 * it alongside rocketnet_server_update on another thread.
 * @param server The created server. NULL is an API misuse: it is counted in
 * rocketnet_api_misuse_count, fires an assert in a debug build, and returns 0.
 * @param client_index The slot, in [0, max_clients). A slot outside that range,
 * a slot with no connected client, or any slot while the server is stopped
 * returns 0.
 * @param counter A value of rocketnet_server_client_counter_t below
 * ROCKETNET_SERVER_CLIENT_COUNTER_COUNT; any other value returns 0.
 * @return The counter's value over the slot's current connection, or 0 for a
 * NULL server, a stopped server, a free or out-of-range slot, or a value
 * outside the enum. A 0 is never an error status.
 */
ROCKETNET_EXPORT uint64_t rocketnet_server_client_counter( const struct rocketnet_server_t * server, uint64_t client_index, enum rocketnet_server_client_counter_t counter );

// one client slot's stat by its enum; 0 for a free slot or a value outside the enum. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_server_client_stat( const struct rocketnet_server_t * server, uint64_t client_index, enum rocketnet_server_client_stat_t stat );

#ifdef __cplusplus
}
#endif

#endif // #ifndef ROCKETNET_H
