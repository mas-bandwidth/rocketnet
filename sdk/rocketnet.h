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

    #define ROCKETNET_VERSION_FULL                               "1.0.0"
    #define ROCKETNET_VERSION_MAJOR                                    1
    #define ROCKETNET_VERSION_MINOR                                    0
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

// SDK ABI revision, independent of the wire version. Existing public layouts
// remain unchanged. Increment on any incompatible layout/calling-contract change.
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

#define ROCKETNET_MAX_CLIENTS                                       1000

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

#define ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES                16384
#define ROCKETNET_DEFAULT_REASSEMBLY_SLOTS                 8

#define ROCKETNET_CONNECT_TOKEN_BYTES                      768

// the alignment a caller memory block for a client or a server needs
#define ROCKETNET_ALLOCATION_ALIGNMENT                     64

#define ROCKETNET_LOG_LEVEL_NONE                                       0
#define ROCKETNET_LOG_LEVEL_ERROR                                      1
#define ROCKETNET_LOG_LEVEL_WARN                                       2
#define ROCKETNET_LOG_LEVEL_INFO                                       3
#define ROCKETNET_LOG_LEVEL_DEBUG                                      4
#define ROCKETNET_LOG_LEVEL_SPAM                                       5

// messages (docs/CHANNELS.md): the channel bound, per-send reliability --
// RakNet's values and numbering -- and receipt outcomes

#ifndef ROCKETNET_MAX_CHANNELS
#define ROCKETNET_MAX_CHANNELS                                         32
#endif // #ifndef ROCKETNET_MAX_CHANNELS

#define ROCKETNET_UNRELIABLE                                            0
#define ROCKETNET_UNRELIABLE_SEQUENCED                                  1
#define ROCKETNET_RELIABLE                                              2
#define ROCKETNET_RELIABLE_ORDERED                                      3
#define ROCKETNET_RELIABLE_SEQUENCED                                    4
#define ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT                           5
#define ROCKETNET_RELIABLE_WITH_ACK_RECEIPT                             6
#define ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT                     7

#define ROCKETNET_RECEIPT_ACKED                                         1
#define ROCKETNET_RECEIPT_LOSS                                          2

// Why a connection ended: what rocketnet_client_disconnect_reason and
// rocketnet_server_disconnect_reason return
// (docs/CHANNELS.md 12). MESSAGE_MEMORY_EXHAUSTED: the connection's region had
// no room for an operation it must not drop (docs/CHANNELS.md 4). A server
// disconnects that client's slot with this reason and resets the slot's
// region; a client disconnects itself.

#define ROCKETNET_MESSAGE_ERROR_NONE                                    0
#define ROCKETNET_MESSAGE_ERROR_CONFIG_MISMATCH                         1
#define ROCKETNET_MESSAGE_ERROR_CONFIG_TIMEOUT                          2
#define ROCKETNET_MESSAGE_ERROR_PROTOCOL_VIOLATION                      3
#define ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT                        4
#define ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED                5

// the largest message one packet carries, written in place with zero copies:
// the payload capacity less the one form byte

#define ROCKETNET_MESSAGE_SINGLE_BYTES          ( ROCKETNET_MAX_PAYLOAD_BYTES - 1 )      // 1415

// Send budgets are kilobits per second of wire bytes (1 kbps = 1000 bit/s),
// per connection: the defaults of the config's send_kbps and
// per_client_send_kbps.

#ifndef ROCKETNET_DEFAULT_CLIENT_SEND_KBPS
#define ROCKETNET_DEFAULT_CLIENT_SEND_KBPS                  1000
#endif
#ifndef ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS
#define ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS       10000
#endif

#define ROCKETNET_MESSAGE_OK                                0
#define ROCKETNET_ERROR_MESSAGE_NOT_RELEASED                -1

#ifdef __cplusplus
extern "C" {
#endif

// Stable status codes (docs/ERRORS-AND-LOGGING.md 2.1): OK is the sole success
// value, every failure is a positive code, and the numbering is append-only.
// A published value and meaning never change; a new code appends immediately
// before ROCKETNET_NUM_STATUS_CODES.
enum rocketnet_status_t
{
    ROCKETNET_STATUS_OK                                 = 0,   // success

    // lifecycle and allocator state (rocketnet_init, rocketnet_term, rocketnet_allocator)
    ROCKETNET_STATUS_ALREADY_INITIALIZED                = 1,   // rocketnet_init, or an allocator change, after the library was initialized
    ROCKETNET_STATUS_NOT_INITIALIZED                    = 2,   // a call that needs rocketnet_init came before it, or a term with nothing to terminate
    ROCKETNET_STATUS_LIVE_ALLOCATIONS                   = 3,   // term or an allocator change refused: the library still holds allocations
    ROCKETNET_STATUS_PLATFORM_INIT_FAILED               = 4,   // the platform's socket layer did not start
    ROCKETNET_STATUS_CRYPTO_INIT_FAILED                 = 5,   // the crypto library did not start

    // caller arguments and configuration (client, server, message)
    ROCKETNET_STATUS_INVALID_ARGUMENT                   = 6,   // an argument is NULL, out of range, or inconsistent with another
    ROCKETNET_STATUS_CONFIG_ENV_INVALID                 = 7,   // an environment override did not parse
    ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE                = 8,   // a config field, from the struct or the environment, is out of its range
    ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED             = 9,   // the memory the config needs is past the operator's memory budget
    ROCKETNET_STATUS_MISSING_KEYS                       = 10,   // a server config has no keys
    ROCKETNET_STATUS_INVALID_PROTOCOL_ID                = 11,   // the protocol id is 0, or a development id in a release build
    ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE                = 12,   // the published development keys in a release build
    ROCKETNET_STATUS_INVALID_ADDRESS                    = 13,   // an address string did not parse
    ROCKETNET_STATUS_INVALID_TRANSPORT_LIST             = 14,   // the transport candidate list did not parse

    // client connect token pre-send checks
    ROCKETNET_STATUS_INVALID_CONNECT_TOKEN              = 15,   // the connect token did not parse, or is not for this protocol
    ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED              = 16,   // the connect token expired before the first packet was sent

    // transport candidate list and OS resources
    ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED    = 17,   // a pinned transport cannot take port 0 (an ephemeral port)
    ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED         = 18,   // every transport in the candidate list refused to start
    ROCKETNET_STATUS_OUT_OF_MEMORY                      = 19,   // an allocation failed
    ROCKETNET_STATUS_MUTEX_CREATE_FAILED                = 20,   // the platform refused a mutex
    ROCKETNET_STATUS_THREAD_CREATE_FAILED               = 21,   // the platform refused a thread
    ROCKETNET_STATUS_TIMER_CREATE_FAILED                = 22,   // the platform refused a timer

    ROCKETNET_NUM_STATUS_CODES                          = 23     // the count of codes, OK included
};

// -----------------------------------------
//                        client state
// -----------------------------------------

// What rocketnet_client_state returns. A connect attempt runs up from
// DISCONNECTED through the three handshake states to CONNECTED; every
// negative value is a way the attempt or the connection ended, and the
// client stays in it until it is destroyed.

enum rocketnet_client_state_t
{
    ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            = -7,   // the connect token expired before the server accepted the client
    ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            = -6,   // the connect token was refused before the first packet was sent
    ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             = -5,   // a connection heard nothing from the server for the token's timeout
    ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    = -4,   // the server never answered the third leg of the handshake
    ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     = -3,   // the server never answered the connection request
    ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT         = -2,   // the server never answered the cookie request
    ROCKETNET_CLIENT_STATE_CONNECTION_DENIED                = -1,   // the server refused the client
    ROCKETNET_CLIENT_STATE_DISCONNECTED                     =  0,   // before the first update, and after rocketnet_client_disconnect
    ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE                =  1,   // handshake, first leg
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST       =  2,   // handshake, second leg
    ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE      =  3,   // handshake, third leg
    ROCKETNET_CLIENT_STATE_CONNECTED                        =  4    // connected: messages flow both ways
};

// -----------------------------------------
//                     query enums
// -----------------------------------------

// What a client counts, read with rocketnet_client_counter. Every value is
// monotonic over the client's life and starts at 0 at create.

enum rocketnet_client_counter_t
{
    ROCKETNET_CLIENT_COUNTER_PACKETS_SENT,          // packets: UDP datagrams sent, handshake included
    ROCKETNET_CLIENT_COUNTER_PACKETS_RECEIVED,      // packets: UDP datagrams received, handshake included
    ROCKETNET_CLIENT_COUNTER_BYTES_SENT,            // bytes: UDP payload bytes sent
    ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED,        // bytes: UDP payload bytes received
    ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED,     // payloads: packets from the server that opened and carried messages
    ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT,       // records: reliable message records resent on the retransmit timer
    ROCKETNET_CLIENT_COUNTER_MESSAGES_DUPLICATE,    // messages: reliable messages received a second time and dropped
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED,        // receipts: sends with a receipt that the server acknowledged
    ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST,         // receipts: sends with a receipt that were lost
    ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS,          // messages: unreliable messages dropped because the send budget was spent
    ROCKETNET_CLIENT_COUNTER_SEND_DEFERRED,         // hand-offs: sends that left with the next hand-off to the send thread
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
// rocketnet_server_counter. Every value is monotonic over the server's life
// and starts at 0 at create.

enum rocketnet_server_counter_t
{
    ROCKETNET_SERVER_COUNTER_PACKETS_SENT,          // packets: UDP datagrams sent
    ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED,      // packets: UDP datagrams received
    ROCKETNET_SERVER_COUNTER_BYTES_SENT,            // bytes: UDP payload bytes sent
    ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED,        // bytes: UDP payload bytes received
    ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED,     // connections: clients that completed the handshake
    ROCKETNET_SERVER_COUNTER_CLIENTS_DENIED_FULL,   // requests: connect attempts refused because every client slot was taken
    ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET,// requests: connect attempts dropped by the per-update handshake budget
    ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED,     // payloads: packets from clients that opened and carried messages
    ROCKETNET_SERVER_COUNTER_MESSAGES_RESENT,       // records: reliable message records resent on the retransmit timer
    ROCKETNET_SERVER_COUNTER_MESSAGES_DUPLICATE,    // messages: reliable messages received a second time and dropped
    ROCKETNET_SERVER_COUNTER_RECEIPTS_ACKED,        // receipts: sends with a receipt that a client acknowledged
    ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST,         // receipts: sends with a receipt that were lost
    ROCKETNET_SERVER_COUNTER_BUDGET_DROPS,          // messages: unreliable messages dropped because a client's send budget was spent
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

enum rocketnet_server_client_counter_t
{
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT,       // packets: UDP datagrams sent to this client
    ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_RECEIVED,   // packets: UDP datagrams received from this client
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT,         // bytes: UDP payload bytes sent to this client
    ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED,     // bytes: UDP payload bytes received from this client
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

struct rocketnet_client_t;
struct rocketnet_server_t;

// -----------------------------------------
//                lifecycle and logging
// -----------------------------------------

// The library's version as one integer, the value ROCKETNET_VERSION had when
// the library was built. Compare it with ROCKETNET_VERSION to catch a header
// and a shared library from different releases. Callable before init.
ROCKETNET_EXPORT uint32_t rocketnet_version( void );

// Safe before init; no allocation and no caller-owned struct writes. Type names
// are the public C struct names. Unknown types/fields return SIZE_MAX for size
// and offset, and -1 for field count. This describes the plain structs in this header (the two configs and the
// receipt), not the opaque client and server.
ROCKETNET_EXPORT uint32_t rocketnet_abi_version( void );
// a NULL type is a counted no-op that returns SIZE_MAX.
ROCKETNET_EXPORT size_t rocketnet_abi_type_size( const char * type );
// a NULL type is a counted no-op that returns -1.
ROCKETNET_EXPORT int rocketnet_abi_field_count( const char * type );
// a NULL type or field is a counted no-op that returns SIZE_MAX.
ROCKETNET_EXPORT size_t rocketnet_abi_field_offset( const char * type, const char * field );
// a NULL type or field is a counted no-op that returns SIZE_MAX.
ROCKETNET_EXPORT size_t rocketnet_abi_field_size( const char * type, const char * field );

// context is the allocator context, NULL for the default.
ROCKETNET_EXPORT bool rocketnet_init( void * context );

// As rocketnet_init, with the exact section-2.1 code written to *status on
// every return path when status is not NULL. A failure also logs one
// rocketnet_error line. The non-_ex form passes NULL and keeps its result.
ROCKETNET_EXPORT bool rocketnet_init_ex( void * context, int * status );

ROCKETNET_EXPORT void rocketnet_term( void );

// As rocketnet_term, returning the section-2.1 code: OK when the library
// terminated, NOT_INITIALIZED when there was nothing to terminate, and
// LIVE_ALLOCATIONS when the term refused. A failure logs one error line.
ROCKETNET_EXPORT int rocketnet_term_ex( void );

// The count includes OK. Allocation-free; callable before init.
ROCKETNET_EXPORT int rocketnet_num_status_codes( void );

// The identifier without the ROCKETNET_STATUS_ prefix for 0 .. count - 1, and
// NULL (without asserting) for an out-of-range value. Allocation-free.
ROCKETNET_EXPORT const char * rocketnet_status_string( int status );

// The library's assert handler, called on a failed internal assert in a debug
// build. rocketnet_assert_function replaces it; NULL restores the default.
extern ROCKETNET_EXPORT void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line );

ROCKETNET_EXPORT void rocketnet_assert_function( void (*function)( const char * condition, const char * function, const char * file, int line ) );

ROCKETNET_EXPORT void rocketnet_quiet( bool flag );

ROCKETNET_EXPORT void rocketnet_log_level( int level );

// the level currently in force, so a caller that moves the level for a
// bounded stretch can restore what it found rather than guessing

ROCKETNET_EXPORT int rocketnet_get_log_level( void );

// the name of a level, so a replacement log function renders it the way the
// default one does

ROCKETNET_EXPORT const char * rocketnet_log_level_string( int level );

ROCKETNET_EXPORT void rocketnet_log_function( void (*function)( int level, const char * format, ... ) );

// a preformatted, FFI-safe log callback (docs/ERRORS-AND-LOGGING.md 3.2). the
// callback receives the already-formatted bytes and their exact length; message[length]
// is always NUL-terminated and length is clamped to 1023. message borrows the
// logging frame's stack buffer and is valid only for the synchronous duration of the
// call. the callback must be thread-safe and must not call rocketnet_set_log_callback
// or rocketnet_log_function re-entrantly.
typedef void (*rocketnet_log_callback_t)( void * context, int level, const char * message, size_t length );

ROCKETNET_EXPORT void rocketnet_set_log_callback( rocketnet_log_callback_t callback, void * context );

// Counts every refused API misuse since the process started: a create before
// rocketnet_init, a late or partial rocketnet_allocator, and any other call the
// SDK refuses. A relaxed read; a diagnostic, not a synchronization point.
ROCKETNET_EXPORT uint64_t rocketnet_api_misuse_count( void );

ROCKETNET_EXPORT uint64_t rocketnet_protocol_version( void );

ROCKETNET_EXPORT double rocketnet_time( void );

ROCKETNET_EXPORT void rocketnet_sleep( double seconds );

// -----------------------------------------
//                          allocator
// -----------------------------------------

// Configure before init, with no outstanding allocations. Invalid or late
// replacements are refused: the call returns false, logs one line and counts
// the refusal in rocketnet_api_misuse_count. Refused after init, while
// transitioning, with live allocations, or with a partial set (either callback
// NULL). Returns true when both callbacks were taken. Callbacks must be
// thread-safe and return storage aligned for fundamental types. RocketNet adds
// alignment/ownership metadata, so callbacks receive larger sizes and their
// original pointer on free.
// a NULL malloc_function or free_function is a counted no-op that returns false.
ROCKETNET_EXPORT bool rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

// As rocketnet_allocator, returning the section-2.1 code: INVALID_ARGUMENT for
// a partial set, ALREADY_INITIALIZED after init or during the transition, and
// LIVE_ALLOCATIONS with outstanding allocations. A failure logs one error line.
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
// its end without breaking a program built against an older header. An
// unknown size is refused with a log line and nothing is read or written.

struct rocketnet_client_config_t
{
    uint8_t filter_value[4];                        // ROCKETNET_CLIENT_FILTER_VALUE: the per-title packet filter, as the server's
    int max_payload_bytes;                          // ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    int num_reassembly_slots;                       // ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS: 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    const char * transport;                         // ROCKETNET_CLIENT_TRANSPORT: the candidate list; NULL is "auto"
    void (*state_change_callback)( void * context, int previous_state, int current_state );   // no env form; NULL is none
    double (*time_callback)( void * context );      // no env form: the receive stamp clock, read once at create; NULL is the platform clock
    int send_kbps;                                  // ROCKETNET_CLIENT_SEND_KBPS: the send budget, kbps; default 1000; 0 or less sets the budget to its 10 Gbit/s ceiling, in effect none, NOT RECOMMENDED
    int num_channels;                               // ROCKETNET_CLIENT_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    int window_messages;                            // ROCKETNET_CLIENT_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    int receive_ring_bytes;                         // ROCKETNET_CLIENT_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    int max_message_bytes;                          // ROCKETNET_CLIENT_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    int send_buffer_bytes;                          // ROCKETNET_CLIENT_SEND_BUFFER_BYTES: queued message bytes; 12288 .. 1 << 28; default 800000
    int send_queue_messages;                        // ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES: queued messages; 16 .. 1 << 20; default 1024
    int packets_in_flight;                          // ROCKETNET_CLIENT_PACKETS_IN_FLIGHT: unacknowledged packets tracked; 16 .. 1 << 16; default 256
    int max_packets_per_update;                     // ROCKETNET_CLIENT_MAX_PACKETS_PER_UPDATE: packets send_packets sends per update; 1 .. 256; default 4
    int unreliable_deliveries;                      // ROCKETNET_CLIENT_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per update; 1 .. 1 << 16; default 256
    int sequenced_holds;                            // ROCKETNET_CLIENT_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    int receipt_ring;                               // ROCKETNET_CLIENT_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    int rto_initial_ms;                             // ROCKETNET_CLIENT_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100
    int rto_min_ms;                                 // ROCKETNET_CLIENT_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10
    int rto_max_ms;                                 // ROCKETNET_CLIENT_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250
    int reliable_timeout_ms;                        // ROCKETNET_CLIENT_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
};

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

struct rocketnet_server_config_t
{
    uint64_t protocol_id;                           // ROCKETNET_SERVER_PROTOCOL_ID: the title's protocol id; connect tokens must carry it
    int max_clients;                                // ROCKETNET_SERVER_MAX_CLIENTS: 1 .. ROCKETNET_MAX_CLIENTS; default 1000
    uint8_t filter_value[4];                        // ROCKETNET_SERVER_FILTER_VALUE: the per-title packet filter, as the clients'
    uint8_t backend_public_key[32];                 // ROCKETNET_SERVER_BACKEND_PUBLIC_KEY: verifies connect token signatures
    uint8_t server_public_key[32];                  // ROCKETNET_SERVER_SERVER_PUBLIC_KEY
    uint8_t server_secret_key[32];                  // ROCKETNET_SERVER_SERVER_SECRET_KEY: secret, logged redacted
    int num_xdp_queues;                             // ROCKETNET_SERVER_NUM_XDP_QUEUES: xdp and gso only; 1 .. 1024; 0 is the transport default
    int num_os_cpus;                                // ROCKETNET_SERVER_NUM_OS_CPUS: xdp only, the cores left to the OS; 1 .. 256; 0 is the transport default
    int max_payload_bytes;                          // ROCKETNET_SERVER_MAX_PAYLOAD_BYTES: ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    int num_reassembly_slots;                       // ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS: per client; 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    int max_timeout_seconds;                        // ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS: 1 .. 3600; default 60
    int handshake_deadline_seconds;                 // ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS: 1 .. 60; default 5
    const char * transport;                         // ROCKETNET_SERVER_TRANSPORT: the candidate list; NULL is "auto"
    const char * public_address;                    // ROCKETNET_SERVER_PUBLIC_ADDRESS: "a.b.c.d:port" or "[v6]:port"; NULL is the create call's address
    int per_client_send_kbps;                       // ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS: each client's send budget, kbps; default 10000; 0 or less sets the budget to its 10 Gbit/s ceiling, in effect none, NOT RECOMMENDED
    int num_channels;                               // ROCKETNET_SERVER_NUM_CHANNELS: the channels messages are sent and received on; WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    int window_messages;                            // ROCKETNET_SERVER_WINDOW_MESSAGES: reliable messages in flight per channel set; WIRE, power of two; 64 .. 1 << 20; default 1024
    int receive_ring_bytes;                         // ROCKETNET_SERVER_RECEIVE_RING_BYTES: the receive ring for reassembled messages; WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    int max_message_bytes;                          // ROCKETNET_SERVER_MAX_MESSAGE_BYTES: the largest message; WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    int send_buffer_bytes;                          // ROCKETNET_SERVER_SEND_BUFFER_BYTES: queued message bytes; per client; 12288 .. 1 << 28; default 800000
    int send_queue_messages;                        // ROCKETNET_SERVER_SEND_QUEUE_MESSAGES: queued messages; per client; 16 .. 1 << 20; default 1024
    int packets_in_flight;                          // ROCKETNET_SERVER_PACKETS_IN_FLIGHT: unacknowledged packets tracked; per client; 16 .. 1 << 16; default 256
    int max_packets_per_update;                     // ROCKETNET_SERVER_MAX_PACKETS_PER_UPDATE: packets send_packets sends per update; per client; 1 .. 256; default 4
    int unreliable_deliveries;                      // ROCKETNET_SERVER_UNRELIABLE_DELIVERIES: unreliable messages delivered per update; per client per update; 1 .. 1 << 16; default 256
    int sequenced_holds;                            // ROCKETNET_SERVER_SEQUENCED_HOLDS: sequenced messages held for order; 0 .. 4096; default 16
    int receipt_ring;                               // ROCKETNET_SERVER_RECEIPT_RING: send receipts kept until read; a full ring drops its oldest; 16 .. 1 << 16; default 256
    int rto_initial_ms;                             // ROCKETNET_SERVER_RTO_INITIAL_MS: the retransmit timeout before an rtt sample, milliseconds; 10 .. 60000; default 100
    int rto_min_ms;                                 // ROCKETNET_SERVER_RTO_MIN_MS: the least retransmit timeout, milliseconds; 1 .. 60000; default 10
    int rto_max_ms;                                 // ROCKETNET_SERVER_RTO_MAX_MS: the most retransmit timeout, milliseconds; 10 .. 60000; default 250
    int reliable_timeout_ms;                        // ROCKETNET_SERVER_RELIABLE_TIMEOUT_MS: a reliable message unacknowledged this long ends the connection, milliseconds; 1000 .. 600000; default 10000
};

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
// Sending: send copies the message in and returns its receipt serial,
// nonzero, or 0 when it was refused. start, write in place, finish is the
// zero-copy form of the same: start returns a pointer into the connection's
// region, write the message there once, and finish(bytes_written) queues it
// and returns its serial; abort gives the room back. One start is pending
// per object at a time. reliability is one of ROCKETNET_UNRELIABLE,
// ROCKETNET_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABLE,
// ROCKETNET_RELIABLE_ORDERED, ROCKETNET_RELIABLE_SEQUENCED,
// ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT, ROCKETNET_RELIABLE_WITH_ACK_RECEIPT
// and ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT. Plain UNRELIABLE is one
// message in one packet, up to ROCKETNET_MESSAGE_SINGLE_BYTES; the other
// types are bounded by the config's max_message_bytes.
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

// One outcome for a send's nonzero serial. The list after an update is valid
// until the next update; a full receipt ring drops its oldest entry.
struct rocketnet_receipt_t
{
    uint32_t serial;
    uint32_t status;            // ROCKETNET_RECEIPT_ACKED or ROCKETNET_RECEIPT_LOSS
};

// -----------------------------------------
//                             client
// -----------------------------------------

// The client connects with a connect token, never an address: the token names
// the server, carries the keys, and is the credential -- minted by a backend
// in production, or locally by rocketnet_write_dev_connect_token against the
// published development keys.

// a NULL config is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_default_config( struct rocketnet_client_config_t * config );

// the defaults for a caller whose struct is config_bytes long. 1 written; 0
// refused, nothing written. a NULL config is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_client_default_config_sized( struct rocketnet_client_config_t * config, size_t config_bytes );

// 1 when create would accept the config's own values, else 0 with a log line
// naming the rule. The environment is not read. NULL means the defaults.
ROCKETNET_EXPORT int rocketnet_client_validate_config( const struct rocketnet_client_config_t * config );

// the bytes a caller memory block for this client needs; 0 for a refused
// config. NULL means the defaults.
ROCKETNET_EXPORT uint64_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config );

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
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, void * memory, uint64_t memory_bytes );

// create with a config config_bytes long; an unknown size is refused with a
// log line and returns NULL before any other check.
ROCKETNET_EXPORT struct rocketnet_client_t * rocketnet_client_create_sized( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes );

// a NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_destroy( struct rocketnet_client_t * client );

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL client is a counted no-op that returns ROCKETNET_MESSAGE_OK.
ROCKETNET_EXPORT int rocketnet_client_update( struct rocketnet_client_t * client, double time );

// a NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_send_packets( struct rocketnet_client_t * client );

// Ends the connection: the disconnect is sent and the state becomes DISCONNECTED. a NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_disconnect( struct rocketnet_client_t * client );

// a NULL client is a counted no-op that returns ROCKETNET_CLIENT_STATE_DISCONNECTED.
ROCKETNET_EXPORT enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client );

// the name of a state ("connected"), or "???" for a value that is not one.
ROCKETNET_EXPORT const char * rocketnet_client_state_name( int state );

// ROCKETNET_MESSAGE_ERROR_* the client ended its connection with, NONE while it has not. a NULL client is a counted no-op that returns NONE.
ROCKETNET_EXPORT int rocketnet_client_disconnect_reason( const struct rocketnet_client_t * client );

// the client's slot on the server, valid while connected. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_client_index( const struct rocketnet_client_t * client );

// the transport that won the candidate list at create ("portable", "msgx"). a NULL client is a counted no-op that returns NULL.
ROCKETNET_EXPORT const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client );

// a NULL client is a counted no-op that returns NULL.
ROCKETNET_EXPORT uint8_t * rocketnet_client_start( struct rocketnet_client_t * client, int channel, int reliability, int bytes );

// the receipt serial, nonzero; 0 when no start is pending or bytes_written is outside 1 to the bytes started. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT uint32_t rocketnet_client_finish( struct rocketnet_client_t * client, int bytes_written );

// a NULL client is a counted no-op.
ROCKETNET_EXPORT void rocketnet_client_abort( struct rocketnet_client_t * client );

// a NULL client or data is a counted no-op that returns 0.
ROCKETNET_EXPORT uint32_t rocketnet_client_send( struct rocketnet_client_t * client, int channel, int reliability, const uint8_t * data, int bytes );

// 1 with the next message on channel, 0 when there is none. a NULL client, data or bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_client_receive( struct rocketnet_client_t * client, int channel, const uint8_t ** data, int * bytes );

// a NULL client or data is a counted no-op; a pointer that is not a held view is a counted misuse.
ROCKETNET_EXPORT void rocketnet_client_release( struct rocketnet_client_t * client, const uint8_t * data );

// Copies this update's receipts into out, up to capacity entries; returns the
// copied count. Repeated calls see the same list until the next update. a
// NULL client or out is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity );

// 1 when the budget took the new rate, from now on; 0 or less sets the budget to its 10 Gbit/s ceiling, in effect none (NOT RECOMMENDED). a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps );

// a counter by its enum; 0 for a value outside the enum. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_client_counter( const struct rocketnet_client_t * client, enum rocketnet_client_counter_t counter );

// a stat by its enum; 0 for a value outside the enum. a NULL client is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_client_stat( const struct rocketnet_client_t * client, enum rocketnet_client_stat_t stat );

// -----------------------------------------
//                             server
// -----------------------------------------

// Clients are identified by index, 0 .. max_clients - 1, the slot each
// connection takes.

// a NULL config is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_default_config( struct rocketnet_server_config_t * config );

// the defaults for a caller whose struct is config_bytes long. 1 written; 0
// refused, nothing written. a NULL config is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_default_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes );

// the defaults with the published development keys and test filter. a NULL config is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_dev_config( struct rocketnet_server_config_t * config );

// 1 when create would accept the config's own values, else 0 with a log line
// naming the rule. The environment is not read and the keys are not checked.
// NULL means the defaults.
ROCKETNET_EXPORT int rocketnet_server_validate_config( const struct rocketnet_server_config_t * config );

// the bytes a caller memory block for this server needs, at the config's
// max_clients; 0 for a refused config. NULL means the defaults.
ROCKETNET_EXPORT uint64_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config );

// address is where clients reach the server, "a.b.c.d:port" or "[v6]:port"; a
// port of 0 takes an ephemeral port (read it back with rocketnet_server_port).
// NULL config is the defaults, which create refuses until keys are given. a
// NULL address is a counted no-op that returns NULL.
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, void * memory, uint64_t memory_bytes );

// create with a config config_bytes long; an unknown size is refused with a
// log line and returns NULL before any other check.
ROCKETNET_EXPORT struct rocketnet_server_t * rocketnet_server_create_sized( void * context, const char * address, const struct rocketnet_server_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes );

// Disconnects every client and frees the server. a NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_destroy( struct rocketnet_server_t * server );

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL server is a counted no-op that returns ROCKETNET_MESSAGE_OK.
ROCKETNET_EXPORT int rocketnet_server_update( struct rocketnet_server_t * server, double time );

// a NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_send_packets( struct rocketnet_server_t * server );

// The port the server bound: the address's port, or the ephemeral port a
// port of 0 was given. Build a token address from it in tests and tools. a
// NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint16_t rocketnet_server_port( const struct rocketnet_server_t * server );

// the transport that won the candidate list at create ("xdp", "gso", "msgx", "portable"). a NULL server is a counted no-op that returns NULL.
ROCKETNET_EXPORT const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_max_clients( const struct rocketnet_server_t * server );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server );

// 1 when a client holds the slot. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index );

// the client id from the connect token of the client in the slot, 0 for a free slot. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index );

// Disconnects one client deliberately and frees its slot. Why is the game's
// decision. a NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

// ROCKETNET_MESSAGE_ERROR_* the slot's last connection ended with, until the slot's next connection. a NULL server is a counted no-op that returns NONE.
ROCKETNET_EXPORT int rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index );

// a NULL server is a counted no-op that returns NULL.
ROCKETNET_EXPORT uint8_t * rocketnet_server_start( struct rocketnet_server_t * server, int client_index, int channel, int reliability, int bytes );

// a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint32_t rocketnet_server_finish( struct rocketnet_server_t * server, int client_index, int bytes_written );

// a NULL server is a counted no-op.
ROCKETNET_EXPORT void rocketnet_server_abort( struct rocketnet_server_t * server, int client_index );

// a NULL server or data is a counted no-op that returns 0.
ROCKETNET_EXPORT uint32_t rocketnet_server_send( struct rocketnet_server_t * server, int client_index, int channel, int reliability, const uint8_t * data, int bytes );

// Sends data to every connected client exactly as rocketnet_server_send
// would, one copy per connection. Returns the number of clients it was queued
// for; a client that refuses is skipped. a NULL server or data is a counted
// no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_broadcast( struct rocketnet_server_t * server, int channel, int reliability, const uint8_t * data, int bytes );

// 1 with the next message on channel, 0 when there is none. a NULL server, data or bytes is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_receive( struct rocketnet_server_t * server, int client_index, int channel, const uint8_t ** data, int * bytes );

// a NULL server or data is a counted no-op; a pointer that is not a held view is a counted misuse.
ROCKETNET_EXPORT void rocketnet_server_release( struct rocketnet_server_t * server, int client_index, const uint8_t * data );

// As rocketnet_client_receipts, for one connected client. Invalid inputs return 0.
ROCKETNET_EXPORT int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity );

// 1 when the client's budget took the new rate, from now on; 0 or less sets the budget to its 10 Gbit/s ceiling, in effect none (NOT RECOMMENDED). a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT int rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps );

// a counter by its enum; 0 for a value outside the enum. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_counter( const struct rocketnet_server_t * server, enum rocketnet_server_counter_t counter );

// a stat by its enum; 0 for a value outside the enum. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_server_stat( const struct rocketnet_server_t * server, enum rocketnet_server_stat_t stat );

// one client slot's counter by its enum; 0 for a free slot or a value outside the enum. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT uint64_t rocketnet_server_client_counter( const struct rocketnet_server_t * server, int client_index, enum rocketnet_server_client_counter_t counter );

// one client slot's stat by its enum; 0 for a free slot or a value outside the enum. a NULL server is a counted no-op that returns 0.
ROCKETNET_EXPORT double rocketnet_server_client_stat( const struct rocketnet_server_t * server, int client_index, enum rocketnet_server_client_stat_t stat );

#ifdef __cplusplus
}
#endif

#endif // #ifndef ROCKETNET_H
