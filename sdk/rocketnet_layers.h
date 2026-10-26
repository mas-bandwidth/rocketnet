/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The SDK's layers under the public client and server: the transports, the
// message objects, frame sync, the counter blocks, the logging emitters and
// the allocator helpers. Not for callers and not exported: the library's own
// sources, its tests and its tools include it through rocketnet_internal.h.
// rocketnet.h is the public API.

#ifndef ROCKETNET_LAYERS_H
#define ROCKETNET_LAYERS_H

#include "rocketnet.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Config revision sizes (docs/ABI-EVOLUTION.md): the end of a revision's last
/// field rounded up to that revision's alignment. The public configs have one
/// revision each, the whole struct; a config_bytes the library does not know
/// is refused.
#define ROCKETNET_CONFIG_REVISION_BYTES( end, align ) ( ( ( end ) + ( align ) - 1 ) / ( align ) * ( align ) )
#define ROCKETNET_CLIENT_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_client_config_t, reliable_message_timeout_ms ) + sizeof( int ), sizeof( void * ) )
#define ROCKETNET_SERVER_CONFIG_BYTES_1 ROCKETNET_CONFIG_REVISION_BYTES( offsetof( struct rocketnet_server_config_t, reliable_message_timeout_ms ) + sizeof( int ), sizeof( void * ) )

/// Receive bounds match the sender's datagram and unfragmented payload limits.
/// Internal; fragments retain their separate, unrounded stride bound.
#define ROCKETNET_RECEIVE_PACKET_BYTES                     ROCKETNET_MAX_PACKET_BYTES
#define ROCKETNET_RECEIVE_PAYLOAD_BYTES                    ROCKETNET_MAX_PACKET_PAYLOAD_BYTES

/// The largest payload the transport sends fragmented and reassembles. Not
/// a config field: the message layer sends at most one packet's payload per
/// transport payload (larger messages ride its own reliable fragments), so
/// this bounds only the transport layer's own callers. It sizes the
/// reassembly arena, max_clients x slots x this, so it stays at the size the
/// 1000-client plan was budgeted for.
#define ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES              16384

/// The defaults the public header states as numbers in each field's comment.
#define ROCKETNET_DEFAULT_REASSEMBLY_SLOTS                 8

/// The largest message one packet carries, written in place: the payload
/// capacity less the one form byte.
#define ROCKETNET_MESSAGE_SINGLE_BYTES                     ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 )

/// The standard's numbering of the ways a connection ends (standard/MESSAGES.md),
/// the one place it maps from the public enum, which is dense and negative.
static inline int rocketnet_disconnect_reason_standard_code( int reason )
{
    switch ( reason )
    {
        case ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH:           return 1;
        case ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION:        return 3;
        case ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT:          return 4;
        case ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED:  return 5;
        case ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED:       return 6;
        case ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED:      return 7;
        default:                                                    return 0;
    }
}

/// What the message layer's update returns. The public update returns void.
enum rocketnet_update_result_t
{
    ROCKETNET_UPDATE_OK                                         =  0,  ///< The update ran
    ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED                       = -1   ///< A received message view was still held at update
};

/// The process-wide log level in force: a relaxed atomic read for the SDK's
/// own tests, which save and restore it. Not exported.
enum rocketnet_log_level_t rocketnet_log_level_current( void );

/// The process-wide reads the public stats structs carry (api_misuses,
/// thread_priority_refusals) and the thread priority setting, for the SDK's
/// own code and tests. Not exported.
uint64_t rocketnet_api_misuse_count( void );
uint64_t rocketnet_get_thread_priority_refusals( void );
bool rocketnet_get_thread_priority( void );

/** @brief Capacity in bytes of transport address strings, including the terminating NUL. */
#define ROCKETNET_MAX_ADDRESS_STRING_LENGTH                256

/** @brief Address-family value for no address family. */
#define ROCKETNET_ADDRESS_NONE                             0
/** @brief Address-family value for IPv4. */
#define ROCKETNET_ADDRESS_IPV4                             1
/** @brief Address-family value for IPv6. */
#define ROCKETNET_ADDRESS_IPV6                             2

/** @brief Sets the storage capacity for a transport transport candidate list.
 *
 * The value includes the terminating NUL byte; a configured string must fit
 * within this many bytes.
 */
#define ROCKETNET_TRANSPORT_STRING_LENGTH                     64

/** @brief The connect token expired: its expire time was not after the current Unix time when the connect began, or its lifetime (expire minus create) ran out before the handshake reached CONNECTED. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED         -7
/** @brief The connect token failed the client's checks at connect: no or unknown address flags, a create time after its expire time, or a wrong format version. The client does not verify the token's signature; the server does. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN         -6
/** @brief The established connection has timed out. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT          -5
/** @brief The server did not answer the connection response in time. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT -4
/** @brief The server did not answer the connection request in time. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT  -3
/** @brief The server did not answer the cookie request in time. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT      -2
/** @brief The server denied the connection. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED             -1
/** @brief The client is disconnected or has not started connecting. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED                   0
/** @brief The client is requesting a connection cookie. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE              1
/** @brief The client is sending its connection request. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST     2
/** @brief The client is answering the server's challenge. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE    3
/** @brief The client connection is established. */
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED                      4

/** @brief Server transport state while its receive and update work is active. */
#define ROCKETNET_SERVER_TRANSPORT_RUNNING                              0
/** @brief Server transport state while shutdown is in progress. */
#define ROCKETNET_SERVER_TRANSPORT_STOPPING                             1
/** @brief Server transport state after shutdown completes. */
#define ROCKETNET_SERVER_TRANSPORT_STOPPED                              2

/** @brief Maximum number of explanatory exclusions in a host-memory estimate. */
#define ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS                            8

#define ROCKETNET_PRIORITY_IMMEDIATE                                    0
#define ROCKETNET_PRIORITY_HIGH                                         1
#define ROCKETNET_PRIORITY_MEDIUM                                       2
#define ROCKETNET_PRIORITY_LOW                                          3

// the block stride, a wire constant

#define ROCKETNET_MESSAGE_FRAGMENT_BYTES                             1152

// The reliable resend schedule adapts to the measured round trip, with no
// per-resend back-off. The timeout uses 2*srtt + 4*rttvar + 30 ms and is
// capped at 2 s. Validation reserves four sends of the configured initial resend delay.

#define ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS                        4

// What a clean run shows of a counter. Every counter has exactly one class.
//   refusal    reads 0 on a clean path at any round trip (a resource
//              exhaustion is a refusal)
//   expected   moves on a clean path by design (success outcomes, lifecycle
//              and side counters)
//   path_loss  moves only when the path loses or reorders packets
//   arrival    a RECEIVED counter: a packet of the type arrived
/** @brief Classifies counters by the change expected on clean and lossy paths. */
enum
{
    ROCKETNET_COUNTER_CLASS_REFUSAL = 0,
    ROCKETNET_COUNTER_CLASS_EXPECTED,
    ROCKETNET_COUNTER_CLASS_PATH_LOSS,
    ROCKETNET_COUNTER_CLASS_ARRIVAL,
    /** @brief Sentinel and number of counter classifications. */
    ROCKETNET_NUM_COUNTER_CLASSES
};

/** Opaque client endpoint created and destroyed by the transport API. */
struct rocketnet_client_transport_t;
/** Opaque server endpoint created and destroyed by the transport API. */
struct rocketnet_server_transport_t;
struct rocketnet_message_client_t;
struct rocketnet_message_server_t;
/** @brief Holds state for one frame synchronizer.
 *
 * The type is opaque. Create it with rocketnet_frame_sync_create, use it from
 * its owning loop thread, and release it with rocketnet_frame_sync_destroy.
 */
struct rocketnet_frame_sync_t;
struct rocketnet_counters_t;            // read through rocketnet_counter_value
struct rocketnet_packet_context_t;      // defined below, with the send surface

// printf format checking for the log functions below: every call's format
// and arguments are checked by GCC and Clang at the call site.
#if defined( __GNUC__ ) || defined( __clang__ )
#define ROCKETNET_PRINTF_FORMAT( format_index, first_argument ) __attribute__(( format( printf, format_index, first_argument ) ))
#else
#define ROCKETNET_PRINTF_FORMAT( format_index, first_argument )
#endif

/**
 * @brief Formats and emits a message at the supplied log level.
 *
 * Quiet mode and the configured level filter the message before formatting.
 * Formatting uses printf rules; output is truncated to 1023 bytes. Unknown
 * levels are passed to the sink, and the default sink labels them "???".
 * @param level A log level, normally 0 through 5.
 * @param format NUL-terminated printf format; NULL is a counted no-op.
 * The remaining arguments must match the conversion specifiers in format.
 */
ROCKETNET_PRINTF_FORMAT( 2, 3 ) void rocketnet_printf( int level, const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_INFO. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
ROCKETNET_PRINTF_FORMAT( 1, 2 ) void rocketnet_info( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_WARN. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
ROCKETNET_PRINTF_FORMAT( 1, 2 ) void rocketnet_warn( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_ERROR. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
ROCKETNET_PRINTF_FORMAT( 1, 2 ) void rocketnet_error( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_DEBUG. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
ROCKETNET_PRINTF_FORMAT( 1, 2 ) void rocketnet_debug( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_SPAM. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
ROCKETNET_PRINTF_FORMAT( 1, 2 ) void rocketnet_spam( const char * format, ... );

/** @brief Formats a user identifier as lowercase hexadecimal. @param user_id Identifier to format. @param buffer Caller-owned output buffer; NULL is a counted no-op returning NULL. @param buffer_size Buffer capacity in bytes; zero writes no bytes. @return buffer, or NULL when buffer is NULL. A nonempty buffer is NUL-terminated and may contain a truncated string. */
const char * rocketnet_user_id_string( uint64_t user_id, char * buffer, size_t buffer_size );

/** @brief Returns a cryptographically generated float in the inclusive range [0, 1]. @return Random value. */
float rocketnet_random_float( void );

/** @brief Returns a cryptographically generated 8-bit value. @return Value in [0, 255]. */
uint8_t rocketnet_random_uint8( void );

/** @brief Returns a cryptographically generated 32-bit value. @return Value in [0, UINT32_MAX]. */
uint32_t rocketnet_random_uint32( void );

/** @brief Returns a cryptographically generated 64-bit value. @return Value in [0, UINT64_MAX]. */
uint64_t rocketnet_random_uint64( void );

/** @brief Fills a buffer with cryptographically generated bytes. @param data Writable destination; NULL is a counted no-op that writes nothing. @param bytes Number of bytes to write. The caller owns the buffer. */
void rocketnet_random_bytes( uint8_t * data, size_t bytes );

/** @brief Copies a NUL-terminated string into a bounded caller buffer. @param dest Writable destination; NULL is a counted no-op. @param source NUL-terminated source; NULL is a counted no-op. @param dest_size Destination capacity in bytes; zero writes nothing. @return Bytes copied, excluding NUL; returns 0 for null pointers or zero capacity. For nonzero capacity, output is always terminated; source and destination must not overlap. */
size_t rocketnet_copy_string( char * dest, const char * source, size_t dest_size );

/**
 * @brief Allocates SDK-counted storage aligned to ROCKETNET_ALLOCATION_ALIGNMENT (64 bytes).
 * @param context Opaque allocator context; NULL is allowed.
 * @param bytes Requested byte count.
 * @return Aligned pointer, or NULL on overflow or allocation failure. Free only
 * through rocketnet_free; the original allocator and context are retained for release.
 */
void * rocketnet_malloc( void * context, size_t bytes );

/** @brief Releases storage returned by rocketnet_malloc. @param context Ignored; the allocation's original context and deallocator are retained. @param p Pointer to release; NULL is a no-op. Do not use p after this call. */
void rocketnet_free( void * context, void * p );

// -----------------------------------------
//                   transport config
// -----------------------------------------

// Each transport's config is a plain struct. Precedence at create is
// DEFAULT < CONFIG STRUCT < ENVIRONMENT: the app ships what it means in code,
// and an operator standing at an already-built binary overrides any field with
// an environment variable, no rebuild -- the ROCKETNET_ prefix plus the
// endpoint and field name uppercased (cfg.transport -> ROCKETNET_SERVER_TRANSPORT or
// ROCKETNET_CLIENT_TRANSPORT). Every env override is logged loudly at create with
// the variable name and the value it displaced (secret fields log redacted).
// Validation refuses loudly at create on any out-of-range value -- never
// clamps, never silently corrects.
//
// Env encodings: integers are decimal; byte fields are base64 of exactly
// the field's length (keygen prints keys in base64); the transport field is
// the ordered candidate list below. An env var set to the empty string is
// ignored as if unset, with a log line. The pointer fields (the client's
// callbacks) have no env form.
//
// The transport field: an ORDERED CANDIDATE LIST, comma separated, tried left
// to right at create -- first grant wins, and a list exhausted by REFUSAL
// fails loudly. "auto" (the default, also the empty string) expands to
// this platform's recommended order for the endpoint: linux server
// xdp,gso,portable; macOS msgx,portable; everywhere else portable
// (clients: portable everywhere, plus msgx on macOS). A one-element list
// is a pin: that transport or loud failure, never a silent fall. A name in
// the known transport set (xdp, gso, msgx, portable) but absent on this
// platform -- or not applicable to this endpoint, like xdp for a client --
// is IGNORED with one soft log line, so one fleet-wide config line deploys
// unmodified to heterogeneous boxes; a list whose EVERY name is ignored
// names no transport for this endpoint at all and falls to the endpoint's own
// auto order, loudly, rather than bricking an endpoint the line was not
// addressing. A name in no transport set anywhere is a typo and fails
// loudly at parse, listing the valid names.
//
// Sized configs (docs/ABI-EVOLUTION.md). Each config struct has sized entry
// points beside its unsized ones: the caller passes sizeof the struct it was
// compiled against. A struct only grows at its end; a known size reads the
// caller's prefix and defaults the rest, and an unknown size is refused with a
// log line and nothing is read or written. The unsized entry points read and
// write exactly revision 1.

// -----------------------------------------
//                   client transport
// -----------------------------------------

/** @brief Return the stable text name for a client transport state.
 * @param state [in] One of the ROCKETNET_CLIENT_TRANSPORT_STATE_* values.
 * @return A static string naming a known state. An unknown value asserts in assertion builds and returns `???`; do not free the result.
 */
const char * rocketnet_client_transport_state_name( int state );

// The client connects with a token, never an address: the token names the
// server, carries the keys, and is the credential -- minted by a backend in
// production, or locally by rocketnet_write_test_connect_token against the
// internal test keys. The config carries what the token does not:
// the per-title filter value, the fragmentation shape -- the largest
// payload this client sends and reassembles, and the per-connection
// reassembly slot count, zero meaning the defaults above; the client's
// delivered mirror sizes itself to the client's own drain bound, about
// 8.4MB at the default maximum -- the transport candidate list, and an
// optional state-change callback. NULL config is the development posture:
// the published test filter, the defaults, no callback.

/** @brief Settings and callbacks for one client transport.
 * @details Members use the defaults and ranges documented below. A NULL config to create selects the default test filter, table defaults, and no callbacks. At creation, defaults are overlaid by this struct and then by nonempty environment overrides; invalid values are refused. The caller owns this value and may modify it before creation.
 * @par Threading
 * Do not mutate the config concurrently with a create call.
 */
struct rocketnet_client_transport_config_t
{
    /** @brief Per-title 4-byte packet filter; clients with a different value cannot connect. */
    uint8_t packet_filter[4];
    /** @brief Concurrent fragmented payloads per connection; zero selects the default, otherwise 1 through 256. */
    int max_fragmented_messages;                       // 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    /** @brief Ordered transport candidate list; defaults to auto. */
    char transport[ROCKETNET_TRANSPORT_STRING_LENGTH];    // the candidate list; default "auto"
    /** @brief Optional state transition callback; NULL disables it. It receives the create context and previous and current states. */
    void (*state_change_callback)( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state );
    /** @brief Optional monotonic receive timestamp clock in seconds; NULL selects the platform clock. */
    double (*time_callback)( void * context );      // the receive stamp clock, read once at create; NULL is the platform clock
    uint64_t game_clock_epoch;                      // unix seconds at the game clock's zero for the token check; 0 reads the wall clock
    uint64_t step_driven;                           // 1: the caller's update and send_packets run the I/O step, no I/O thread; 0: today's thread
    int simulated_loss_percent;                     // development only: simulated loss 0..100, 0 off
    int simulated_latency_ms;                       // development only: simulated latency ms, 0 off
    int simulated_jitter_ms;                        // development only: simulated jitter ms, 0 off
    int simulated_duplicate_percent;                // development only: simulated duplicate 0..100, 0 off
    int simulated_drop_every_nth;                   // development only: drop every Nth outgoing datagram (2 .. 1000000), counted from the first send; 0 off
    int simulated_reverse_window;                   // development only: reverse each window of W outgoing datagrams (2 .. 64), flushed whole at the end of each send_packets; 0 off
};

// a NULL config is a counted no-op.
/** @brief Fill a client transport config with its defaults.
 * @param[out] config Receives the defaults; NULL is a counted no-op.
 * @details This function only writes caller-owned storage and does not require library initialization. The caller may edit the result before create. Do not mutate it concurrently with a create call.
 */
void rocketnet_client_transport_default_config( struct rocketnet_client_transport_config_t * config );

// Allocation-free constructor memory plan for one built-in transport name.
// Includes SDK allocator requests (alignment/ownership overhead). Excludes OS
// stacks, kernel/libc resources and application memory; this is not a host RAM
// limit. Uses the supplied numeric config exactly (NULL means defaults); no
// environment overlay, key validation, socket probe or budget refusal. Zero
// means invalid geometry, overflow, unknown/custom transport, or transport not built
// for this platform. Query each candidate separately: "auto" and transport lists
// are not concrete plans.
/** @brief Measure SDK allocator bytes for one client transport transport.
 * @param[in] config Numeric settings to measure, or NULL for defaults. Environment overrides are not applied.
 * @param[in] transport One concrete built-in transport name; `auto` and comma-separated candidate lists are not plans.
 * @return The planned SDK allocator bytes, including its alignment and ownership overhead, or 0 for invalid geometry, overflow, an unknown/custom transport, or a transport not built for this platform.
 * @details This is allocation-free and does not validate keys, inspect the environment, probe a socket, or enforce a host memory budget. It excludes OS stacks, kernel, libc, and application memory. Query each candidate separately before choosing a transport. The result is a plan, not a reservation or grant.
 */
uint64_t rocketnet_client_transport_required_memory( const struct rocketnet_client_transport_config_t * config, const char * transport );

// the defaults for a caller whose struct is config_bytes long, a known size of
// the struct. 1 written; 0 refused, nothing written. a NULL config is a
// counted no-op that returns 0.
/** @brief Write defaults into a known client config revision.
 * @param[out] config Receives the defaults; NULL is a counted no-op and returns 0.
 * @param[in] config_bytes Size, in bytes, of the caller's config revision.
 * @return 1 when config_bytes is a known revision and the bytes are written; 0 when it is unknown or config is NULL. An unknown size leaves config untouched.
 * @details A known prefix receives defaults for that revision only. This function is allocation-free and may be called before initialization. The caller owns config.
 */
int rocketnet_client_transport_default_config_sized( struct rocketnet_client_transport_config_t * config, size_t config_bytes );


// time is the game's clock, a double in seconds, monotonic; pass the same
// clock to every call that takes one. The library never reads a clock of
// its own on these paths, so a game owns time and a test can hold it still.

// a NULL connect_token is a counted no-op that returns NULL; a NULL config is
// the default shape.
/** @brief Create a client transport using the standard NULL-or-pointer result.
 * @param context [in] Caller-provided allocator context passed to allocations for this transport; NULL is allowed if supported by the configured allocator.
 * @param connect_token [in] Non-NULL complete token of ROCKETNET_CONNECT_TOKEN_BYTES; NULL is a counted no-op and returns NULL.
 * @param config [in] Optional full configuration; NULL selects defaults.
 * @param time [in] Finite monotonic time in seconds.
 * @return A caller-owned transport or NULL on any creation failure. Use create_ex to obtain the specific refusal status.
 */
struct rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time );

// The _ex form: same contract, and when status is not NULL it is written with
// ROCKETNET_STATUS_OK on success and the section-2.1 code of the refusal on
// every failure exit. The non-_ex form is a thin wrapper passing NULL.
/** @brief Create a client transport and report the exact refusal status.
 * @param context [in] Caller-provided allocator context passed to allocations for this transport; NULL is allowed if supported by the configured allocator.
 * @param connect_token [in] Non-NULL complete token buffer of ROCKETNET_CONNECT_TOKEN_BYTES.
 * @param config [in] Optional full config; NULL selects defaults.
 * @param time [in] Finite, monotonic time in seconds for the initial update clock.
 * @param status [out] Optional status destination; receives OK on success or the specific failure code on every failure.
 * @return A caller-owned transport on success, or NULL. Failures report INVALID_ARGUMENT, NOT_INITIALIZED, CONFIG_ENV_INVALID, CONFIG_OUT_OF_RANGE, MEMORY_BUDGET_EXCEEDED, INVALID_TRANSPORT_LIST, INVALID_CONNECT_TOKEN, CONNECT_TOKEN_EXPIRED, TRANSPORT_LADDER_EXHAUSTED, OUT_OF_MEMORY, MUTEX_CREATE_FAILED, or THREAD_CREATE_FAILED. A transport transport whose open fails an allocation ends the create with OUT_OF_MEMORY and the ladder tries no later candidate (sdk/rocketnet_client_transport.cpp:882); nothing the create allocated stays live. Call rocketnet_init first and correct the reported cause before retrying.
 * @code
 * // token, context, config, time, and status are supplied by the game.
 * rocketnet_init( NULL, NULL, NULL );
 * struct rocketnet_client_transport_t *client = rocketnet_client_transport_create_ex(context, token, config, time, &status);
 * if (client) { rocketnet_client_transport_update(client, time); rocketnet_client_transport_destroy(client); }
 * rocketnet_term();
 * @endcode
 */
struct rocketnet_client_transport_t * rocketnet_client_transport_create_ex( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status );

// create with a config config_bytes long: a known size is read over the
// defaults and created as above; an unknown size is refused with a log line and
// returns NULL before any other check. a NULL config is the default shape,
// whatever config_bytes says.
/** @brief Create a client transport from a versioned config prefix.
 * @param[in] context Opaque pointer passed to callbacks; may be NULL.
 * @param[in] connect_token Server-issued connection token; NULL is a counted misuse and returns NULL.
 * @param[in] config Config prefix to read over defaults; NULL selects defaults regardless of config_bytes.
 * @param[in] config_bytes Size in bytes of config; non-NULL configs require a known revision.
 * @param[in] time Monotonic game-clock time in seconds, using the same clock as later timed calls.
 * @return The transport on success, or NULL when a size is unknown or transport creation is refused.
 * @details The library must be initialized first. An unknown config size is refused before other argument checks. A known-size config follows the ordinary create path, including environment overrides and validation. Destroy the returned object with rocketnet_client_transport_destroy; the object owns its internal allocations.
 * @code
 * struct rocketnet_client_transport_config_t config;
 * rocketnet_client_transport_default_config_sized(&config, sizeof(config));
 * struct rocketnet_client_transport_t * client =
 *     rocketnet_client_transport_create_sized(context, token, &config, sizeof(config), now);
 * @endcode
 */
struct rocketnet_client_transport_t * rocketnet_client_transport_create_sized( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, size_t config_bytes, double time );

/** @brief Request transport shutdown without waiting for worker completion.
 * Call disconnect first to stage a graceful disconnect burst. No producers may
 * access send storage after stop. A caller-stepped transport progresses shutdown
 * through update; a threaded transport progresses on its worker.
 */
void rocketnet_client_transport_stop( struct rocketnet_client_transport_t * client_transport );
/** @brief Check worker quiescence without joining or freeing resources. */
bool rocketnet_client_transport_stopped( const struct rocketnet_client_transport_t * client_transport );
/** @brief Join completed workers and release transport resources synchronously.
 * Requires stopped to be true. Premature calls assert and retain the object.
 */
void rocketnet_client_transport_destroy( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op.
/** @brief Advance client networking and publish the next receive and acknowledgement batches.
 * @param client_transport [in,out] Created transport; NULL is a counted no-op.
 * @param time [in] Finite monotonic time in seconds, using the same game clock as other timed transport calls.
 * @return Nothing. Call on the main thread, outside concurrent packet-fill phases.
 */
void rocketnet_client_transport_update( struct rocketnet_client_transport_t * client_transport, double time );

// a NULL client_transport or packet_data is a counted no-op.
/**
 * @brief Queues and flushes one client payload.
 *
 * @param client_transport Client transport; NULL is a counted no-op.
 * @param packet_data Bytes to copy; NULL is a counted no-op.
 * @param packet_bytes Payload length in bytes, from 1 through configured maximum.
 */
void rocketnet_client_transport_send_packet( struct rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes );

// The copying cursor: pops the next delivered payload into packet_data,
// returning its length, or zero when the update's batch is drained.
// packet_capacity is the caller's buffer size: a payload larger than it --
// possible only for reassembled fragmented payloads, which reach the
// configured maximum -- is refused whole and counted on
// RECEIVE_CURSOR_TOO_SMALL, never truncated, and the cursor moves on to
// the next payload that fits. Size the buffer by
// rocketnet_client_transport_max_payload_bytes to never refuse.

// a NULL client_transport or packet_data is a counted no-op that returns 0.
/**
 * @brief Copies the next delivered client payload into a caller buffer.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning zero.
 * @param packet_data Destination buffer; NULL is a counted no-op returning zero.
 * @param packet_capacity Destination capacity in bytes.
 * @return Copied byte count, or zero when drained or no payload fits; oversize payloads are skipped whole and counted.
 */
int rocketnet_client_transport_receive_packet( struct rocketnet_client_transport_t * client_transport, uint8_t * packet_data, int packet_capacity );

// the maximum payload this transport sends and reassembles --
// ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES -- so a caller can size
// its receive buffers without carrying the config around

// a NULL client_transport is a counted no-op that returns 0.
/**
 * @brief Returns the configured client payload limit.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning zero.
 * @return Maximum send and reassembly payload size in bytes, or zero for NULL.
 */
int rocketnet_client_transport_max_payload_bytes( struct rocketnet_client_transport_t * client_transport );

// the batch behind the copying cursor above: by count and index, and as
// parallel arrays in one call. valid until the next update. reassembled
// fragmented payloads take their place here in arrival position, at the
// moment they complete, up to the configured maximum payload size.

// a NULL client_transport is a counted no-op that returns 0.
/**
 * @brief Returns the number of payloads delivered by the last client update.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning zero.
 * @return Payload count; the batch remains valid until the next update.
 */
int rocketnet_client_transport_num_payloads( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport or payload_bytes is a counted no-op that returns NULL.
/**
 * @brief Returns one payload from the last client update.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning NULL.
 * @param payload_index Zero-based index in the latest batch.
 * @param payload_bytes Receives payload length; NULL is a counted no-op returning NULL.
 * @return Borrowed view valid until next update, or NULL for invalid input or index.
 */
uint8_t * rocketnet_client_transport_payload( struct rocketnet_client_transport_t * client_transport, int payload_index, int * payload_bytes );

// a NULL client_transport, payload_data or payload_bytes is a counted no-op that returns 0.
/**
 * @brief Returns parallel views of payloads from the last client update.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning zero.
 * @param payload_data Receives array of payload pointers; NULL is a counted no-op returning zero.
 * @param payload_bytes Receives parallel lengths in bytes; NULL is a counted no-op returning zero.
 * @return Payload count; arrays are borrowed and valid until next update.
 */
int rocketnet_client_transport_payloads( struct rocketnet_client_transport_t * client_transport, uint8_t * const ** payload_data, const int ** payload_bytes );

// the client's acquire/commit send family: the same shape, layout and
// threading contract as the server's, minus the client index. NOTE:
// rocketnet_client_transport_send_packet (the convenience above) flushes the ring
// inline and is therefore main-thread only, never during an open fill --
// it is the simple form, not a fill-phase participant.

// a NULL client_transport or context is a counted no-op that returns NULL.
/**
 * @brief Reserves a client send frame and returns its writable payload start.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning NULL.
 * @param request_bytes Requested capacity in bytes, from 1 through configured maximum.
 * @param context Receives lease metadata; NULL is a counted no-op returning NULL.
 * @return Writable payload pointer, or NULL when reservation fails.
 */
uint8_t * rocketnet_client_transport_start_packet( struct rocketnet_client_transport_t * client_transport, int request_bytes, struct rocketnet_packet_context_t * context );

// Validates an active packet lease and returns its payload view. NULL/zero on
// stale, consumed, modified or wrong-owner context. The view expires at finish,
// abort, update, flush or destroy; callers must join before those operations.
// a NULL transport, context or capacity is a counted no-op that returns NULL.
/**
 * @brief Validates a client packet lease and returns its writable view.
 *
 * @param transport Client transport that created the lease; NULL is a counted no-op.
 * @param context Unmodified active lease from start_packet; NULL is a counted no-op.
 * @param capacity Receives capacity in bytes; NULL is a counted no-op.
 * @return Borrowed payload pointer, or NULL for invalid lease; view expires at finish, abort, update, flush, or destroy.
 */
uint8_t * rocketnet_client_transport_packet_payload( struct rocketnet_client_transport_t * transport, const struct rocketnet_packet_context_t * context, int * capacity );

// a NULL client_transport or context is a counted no-op that returns 0.
// An unstarted, consumed, stale, modified or wrong-owner context asserts in a
// debug build and returns 0 in a release build.
/**
 * @brief Seals and stages bytes written into a client packet lease.
 *
 * @param client_transport Client transport that created the lease; NULL is a counted no-op.
 * @param context Active, unmodified lease from start_packet; this call consumes it. Invalid or stale leases assert in a debug build and return zero in a release build.
 * @param payload_bytes Initialized byte count, from 1 through reported capacity.
 * @return Sealed wire size, or zero when refused or dropped.
 */
int rocketnet_client_transport_finish_packet( struct rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context, int payload_bytes );

// a NULL client_transport or context is a counted no-op.
/**
 * @brief Releases an unused client packet lease.
 *
 * @param client_transport Client transport that created the lease; NULL is a counted no-op.
 * @param context Active, unmodified lease to release; NULL is a counted no-op.
 * @return No value. Invalid or stale leases are refused and counted.
 */
void rocketnet_client_transport_abort_packet( struct rocketnet_client_transport_t * client_transport, struct rocketnet_packet_context_t * context );

// a NULL client_transport is a counted no-op.
/**
 * @brief Flushes client packets staged since the previous flush.
 *
 * @param client_transport Client transport; NULL is a counted no-op.
 * @return No value. Call on the main thread after fill threads join.
 */
void rocketnet_client_transport_send_packets( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op that returns 0.
/** @brief Read the client's current connection state.
 * @param client_transport [in] Created client transport; NULL is a counted no-op.
 * @return A ROCKETNET_CLIENT_TRANSPORT_STATE_* value. NULL returns DISCONNECTED. Read on the main thread outside concurrent update work.
 */
int rocketnet_client_transport_state( struct rocketnet_client_transport_t * client_transport );

// the granted transport, request-vs-granted style: the name of the candidate
// that won the ladder at create ("portable", "msgx"), never the requested
// list. any performance claim attaches to what this returns.

// a NULL client_transport is a counted no-op that returns NULL.
/**
 * @brief Returns the client transport granted at creation.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning NULL.
 * @return Transport-owned granted transport name, valid until destroy, or NULL.
 */
const char * rocketnet_client_transport_io_name( struct rocketnet_client_transport_t * client_transport );
bool rocketnet_client_transport_network_simulator( struct rocketnet_client_transport_t * client_transport, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );
bool rocketnet_server_transport_network_simulator( struct rocketnet_server_transport_t * server_transport, int latency_ms, int jitter_ms, int packet_loss_percent, int duplicate_percent );
/** @brief Makes the next `sends` link sends through an active simulator take the lock-miss path, for a unit test. */
void rocketnet_server_transport_link_busy_for_test( struct rocketnet_server_transport_t * server_transport, int sends );

// a NULL client_transport is a counted no-op that returns 0.
/** @brief Read the server-assigned index of the connected client.
 * @param client_transport [in] Created client transport; NULL is a counted no-op.
 * @return The assigned index when connected, -1 before a connection is assigned, and 0 for NULL. Read on the main thread outside update work.
 */
int rocketnet_client_transport_client_index( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op.
/** @brief Disconnect the client now and return it to DISCONNECTED.
 * When the session keys exist (SENDING_CONNECTION_RESPONSE or CONNECTED), seals
 * a redundant burst of disconnect packets; earlier states simply reset. The
 * connection is reset and the state change is reported to the state-change
 * callback within this call. Without an I/O thread, and outside update, the
 * burst is sent within this call; otherwise it is handed to the I/O thread,
 * retried by the next update when the other send ring is busy, and handed off
 * by destroy at the latest.
 * @param client_transport [in,out] Created transport; NULL is a counted no-op.
 * @return Nothing. Call on the main thread, never during a packet-fill phase.
 */
void rocketnet_client_transport_disconnect( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op that returns NULL.
const struct rocketnet_counters_t * rocketnet_client_transport_counters( struct rocketnet_client_transport_t * client_transport );

// The client's connection quality: rtt and jitter in milliseconds, packet
// loss as a percentage, and the last second's packet rates and bandwidth
// both ways. The rates come from elapsed-time buckets (about 4 ms boundary
// resolution) over the trailing second, so the cost per update is flat.
// rtt seeds from the handshake's round trips and is continuously refreshed
// by the acks; jitter is inter-arrival jitter over the session sequence;
// packet loss is receive-side, from gaps in the session sequence. These
// numbers feed time synchronization. The cumulative totals count every wire
// datagram and byte over the client's current connection, handshake included.

struct rocketnet_client_transport_stats_t
{
    float rtt;                                      // round trip time (milliseconds)
    float jitter;                                   // inter-arrival jitter (milliseconds)
    float packet_loss;                              // percent
    int packets_sent_per_second;
    int packets_received_per_second;
    int bandwidth_sent_kbps;
    int bandwidth_received_kbps;
    uint64_t packets_sent;                          // wire datagrams sent on this connection
    uint64_t packets_received;                      // wire datagrams received on this connection
    uint64_t bytes_sent;                            // wire bytes sent on this connection
    uint64_t bytes_received;                        // wire bytes received on this connection
};

// a NULL client_transport or stats is a counted no-op.
void rocketnet_client_transport_stats( struct rocketnet_client_transport_t * client_transport, struct rocketnet_client_transport_stats_t * stats );

// The client's delivery notification batch: the packet ids newly
// acknowledged by the last update, valid until the next update. The packet
// id is the one handed out in rocketnet_packet_context_t at start_packet.
// Overflow past the batch's capacity is deferred to a later update rather
// than lost -- the CLIENT_ACKED_BATCH_FULL counter counts each deferral.

// a NULL client_transport or packet_ids is a counted no-op that returns 0.
/**
 * @brief Returns packet ids acknowledged by the last client update.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning zero.
 * @param packet_ids Receives the transport-owned array; NULL is a counted no-op returning zero.
 * @return Number of ids; array remains valid until next update.
 */
int rocketnet_client_transport_acked_packets( struct rocketnet_client_transport_t * client_transport, const uint64_t ** packet_ids );

// -----------------------------------------
//                   server transport
// -----------------------------------------

// A packet source: type is ROCKETNET_ADDRESS_IPV4
// or ROCKETNET_ADDRESS_IPV6; an IPv4 address is data.ipv4[0..3] in dotted
// order (a.b.c.d), an IPv6 address is data.ipv6[0..7], the eight groups in
// host byte order; port is in host byte order.

/** @brief Internet endpoint used by the transports and address helpers.
 * @details IPv4 bytes are in network order; IPv6 groups and the port are in host order. Callers synchronize concurrent access to a shared object.
 */
struct rocketnet_address_t
{
    /** @brief Address bytes or groups selected by type. */
    union { uint32_t ip; uint8_t ipv4[4]; uint16_t ipv6[8]; } data;
    /** @brief Port from 0 through 65535; zero means no explicit port. */
    uint16_t port;
    /** @brief ROCKETNET_ADDRESS_NONE, ROCKETNET_ADDRESS_IPV4, or ROCKETNET_ADDRESS_IPV6. */
    uint8_t type;
};

// The server is configured explicitly. rocketnet_server_transport_default_config gives
// the shape with no keys -- create refuses it until keys are supplied
// (through the struct or through the env surface), because default keys are
// how every deployment of a protocol ends up sharing one identity.
// rocketnet_server_transport_dev_config fills the published
// development keys and test filter for local work, loudly. NULL config is
// the default shape. num_xdp_queues is the staged transports' queue count -- XDP
// queues on the xdp transport, reuseport sockets on the gso transport -- kept under
// its historic name for ABI stability with the language bindings; num_os_cpus
// configures the XDP transport's core layout. zero means the transport default, and
// the portable transport ignores both. The tighter num_xdp_queues bounds are per
// transport and refused loudly at create, never clamped: the xdp transport takes any
// count from 1, the gso transport refuses past 32.
//
// The fragmentation shape rides ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES and
// max_fragmented_messages: the largest payload this server sends and
// reassembles, and the per-client reassembly slot count. zero slots means
// the default. budget arithmetic, all spent once at create, approximately --
// slot records ride along: the reassembly arena is max_clients x slots x
// (max payload + 1413 staging) per the standard; the delivered mirror of
// slots x max payload per client -- the per-update home of completed
// payloads; and the ack endpoints' fragment aggregate rings, about 48KB per
// client slot.
//
// public_address, empty by default, displaces the create call's address
// parameter when set, so the address the transport ladder verifies, the
// address the transport binds and the address connect tokens validate
// against are always the same one. It has no environment override.
//
// The handshake deadline is fixed at the challenge, never refreshed, and
// discharged on connect.

/** @brief Settings used to create a server transport.
 * @details Members document defaults and ranges. Creation applies defaults, this struct, then nonempty environment overrides; invalid values are refused. The caller owns the config and its buffers.
 * @par Threading
 * Do not mutate the config concurrently with a create call.
 */
struct rocketnet_server_transport_config_t
{
    /** @brief Title protocol identifier carried by connect tokens. */
    uint64_t protocol_id;
    /** @brief Maximum simultaneous clients; 1 through ROCKETNET_MAX_CLIENTS, default 1000. */
    int max_clients;                                // 1 .. ROCKETNET_MAX_CLIENTS; default 1000
    /** @brief Per-title 4-byte packet filter; clients with a different value cannot connect. */
    uint8_t packet_filter[4];
    /** @brief 32-byte key used to verify connect-token signatures. */
    uint8_t backend_public_key[32];
    /** @brief 32-byte public key used to seal server-bound token data. */
    uint8_t server_public_key[32];
    /** @brief 32-byte server secret key; override logs redact it. */
    uint8_t server_secret_key[32];                  // secret: logged redacted
    /** @brief Requested XDP/GSO queue count; zero selects the transport default, otherwise 1 through 1024. */
    int num_xdp_queues;                             // 1 .. 1024; 0 is the transport default
    /** @brief Cores reserved for the operating system by XDP; zero selects the transport default, otherwise 1 through 256. */
    int num_os_cpus;                                // 1 .. 256; 0 is the transport default
    /** @brief Concurrent fragmented payloads per client; zero selects the default, otherwise 1 through 256. */
    int max_fragmented_messages;                       // 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    /** @brief Ordered transport candidate list; defaults to auto. */
    char transport[ROCKETNET_TRANSPORT_STRING_LENGTH];    // the candidate list; default "auto"
    /** @brief NUL-terminated advertised address; an empty string uses the create address. */
    char public_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    /** @brief Maximum client inactivity in seconds; 1 through 3600, default 60. */
    int max_timeout_seconds;                        // 1 .. 3600; default 60
    /** @brief Fixed handshake deadline in seconds; 1 through 60, default 5. */
    int handshake_deadline_seconds;                 // 1 .. 60; default 5
    uint64_t game_clock_epoch;                      // unix seconds at the game clock's zero for the token check; 0 reads the wall clock
    int simulated_loss_percent;                     // development only: simulated loss 0..100, 0 off
    int simulated_latency_ms;                       // development only: simulated latency ms, 0 off
    int simulated_jitter_ms;                        // development only: simulated jitter ms, 0 off
    int simulated_duplicate_percent;                // development only: simulated duplicate 0..100, 0 off
    int simulated_drop_every_nth;                   // development only: drop every Nth outgoing datagram (2 .. 1000000), counted from the first send; 0 off
    int simulated_reverse_window;                   // development only: reverse each window of W outgoing datagrams (2 .. 64), flushed whole at the end of each send_packets; 0 off
    /** @brief The other family's advertised address on the same port; empty serves one family. */
    char second_public_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
};

// a NULL config is a counted no-op.
/** @brief Fill a server transport config with its defaults.
 * @param[out] config Receives the defaults; NULL is a counted no-op.
 * @details This writes caller-owned storage and does not require initialization. Edit the result before creating a server; do not mutate it concurrently with creation.
 */
void rocketnet_server_transport_default_config( struct rocketnet_server_transport_config_t * config );

// the defaults for a caller whose struct is config_bytes long, a known size of
// the struct. 1 written; 0 refused, nothing written. a NULL config is a
// counted no-op that returns 0.
/** @brief Write defaults into a known server config revision.
 * @param[out] config Receives the defaults; NULL is a counted no-op and returns 0.
 * @param[in] config_bytes Size, in bytes, of the caller's config revision.
 * @return 1 when config_bytes is a known revision and the bytes are written; 0 when it is unknown or config is NULL. An unknown size leaves config untouched.
 * @details A known prefix receives defaults for that revision only. This is allocation-free and may be called before initialization; the caller owns config.
 */
int rocketnet_server_transport_default_config_sized( struct rocketnet_server_transport_config_t * config, size_t config_bytes );

// a NULL config is a counted no-op.
/** @brief Fill a server config with the development key set.
 * @param[out] config Receives the defaults and published development keys; NULL is a counted no-op.
 * @details This helper selects the built-in development posture. It does not create a transport. Use only in builds that enable development support; the caller owns the resulting config and must supply production keys for production servers.
 */
void rocketnet_server_transport_dev_config( struct rocketnet_server_transport_config_t * config );

// Allocation-free constructor memory plan for one built-in transport name.
// Includes SDK allocator requests (alignment/ownership overhead and transient
// worker shims) plus XDP UMEM. Excludes OS stacks, kernel/libbpf/libxdp resources,
// loader/libc bookkeeping and application memory; this is not a host RAM limit.
// Uses the supplied numeric config exactly (NULL means defaults); no environment
// overlay, key validation, socket probe or budget refusal. Zero means invalid
// geometry, overflow, unknown/custom transport, or transport not built for this platform.
// Query each candidate separately: "auto" and transport lists are not concrete plans.
/** @brief Measure SDK allocator bytes for one server transport transport.
 * @param[in] config Numeric server settings to measure, or NULL for defaults. Environment overrides are not applied.
 * @param[in] transport One concrete built-in transport name; `auto` and candidate lists are not plans.
 * @return The planned request bytes (SDK allocator plus XDP UMEM or RIO registered storage where applicable), or 0 for invalid geometry, overflow, an unknown/custom transport, or a transport not built for this platform.
 * @details This is allocation-free and does not validate keys, inspect environment overrides, probe sockets, or enforce a host memory budget. It excludes OS stacks, kernel/libbpf/libxdp resources, libc bookkeeping, and application memory. It measures one transport candidate; the result is not a reservation or grant.
 */
uint64_t rocketnet_server_transport_required_memory( const struct rocketnet_server_transport_config_t * config, const char * transport );

// Allocation-free whole-host memory estimate for one built-in transport name: the
// required_memory plan plus the kernel-facing memory the transport requests. Each
// component is a request, never a grant: allocator_bytes is the SDK allocator
// plan (required_memory minus UMEM and RIO registered storage); umem_bytes is XDP UMEM, the frame memory,
// equal to the plan's; the socket buffer fields sum the SO_SNDBUF and SO_RCVBUF
// sizes asked of every socket the transport opens; xdp_ring_bytes is the AF_XDP
// RX and TX descriptor rings asked of every XDP socket; thread_stack_bytes is
// worker_threads x thread_stack_bytes_per_thread, the stack each transport thread
// is created with (the platform default: the transport passes no stack size), as
// reserved address space. registered_region_bytes is the RIO VirtualAlloc
// request, outside the SDK allocator. total_bytes is the sum of the seven byte components
// and is at least required_memory. exclusions names, in static strings, what
// the library cannot know and does not count. Same config and transport rules as
// required_memory: NULL config means defaults; false (and a zeroed estimate)
// means no plan. A NULL estimate is a counted misuse that returns false.

/** @brief Component breakdown of a requested host-memory estimate.
 * @details Each byte value is a requested amount, not a kernel grant or a host-memory cap. The exclusions array points to process-lifetime strings.
 */
struct rocketnet_host_memory_estimate_t
{
    /** @brief SDK allocator bytes excluding XDP UMEM. */
    uint64_t allocator_bytes;
    /** @brief Requested XDP UMEM bytes. */
    uint64_t umem_bytes;
    /** @brief Requested RIO VirtualAlloc registered-region bytes. */
    uint64_t registered_region_bytes;
    /** @brief Sum of requested socket send-buffer bytes. */
    uint64_t socket_send_buffer_bytes;
    /** @brief Sum of requested socket receive-buffer bytes. */
    uint64_t socket_receive_buffer_bytes;
    /** @brief Requested AF_XDP RX and TX descriptor-ring bytes. */
    uint64_t xdp_ring_bytes;
    /** @brief Total reserved address space for worker thread stacks. */
    uint64_t thread_stack_bytes;
    /** @brief Platform-default stack bytes attributed to each worker thread. */
    uint64_t thread_stack_bytes_per_thread;
    /** @brief Sum of all byte components. */
    uint64_t total_bytes;
    /** @brief Number of sockets included in the estimate. */
    int sockets;
    /** @brief Number of worker threads included in the estimate. */
    int worker_threads;
    /** @brief Number of process-lifetime strings in exclusions. */
    int num_exclusions;
    /** @brief Process-lifetime descriptions of memory excluded from the estimate. */
    const char * exclusions[ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS];
};

/** @brief Estimate memory requests made by one server transport transport.
 * @param[in] config Numeric server settings, or NULL for defaults; environment overrides are not applied.
 * @param[in] transport One concrete built-in transport name; NULL or an unsupported transport makes the call fail.
 * @param[out] estimate Receives the estimate; NULL is counted misuse and returns false. On failure the struct remains zeroed.
 * @return true with a populated estimate, or false when the transport/config has no valid plan, arithmetic overflows, or estimate is NULL.
 * @details This allocation-free estimate separates SDK allocator bytes, UMEM, RIO registered storage, requested socket buffers or XDP rings, and worker stack address space. They are requests, not grants. Exclusions point to static strings whose storage remains valid for the process lifetime. This is not a host RAM limit.
 */
bool rocketnet_server_transport_host_memory_estimate( const struct rocketnet_server_transport_config_t * config, const char * transport, struct rocketnet_host_memory_estimate_t * estimate );

// a NULL public_address_string is a counted no-op that returns NULL; a NULL
// config is the default shape.
/** @brief Create a server transport using the standard NULL-or-pointer result.
 * @param context [in] Caller-provided allocator context passed to allocations for this transport; NULL is allowed if supported by the configured allocator.
 * @param public_address_string [in] Non-NULL numeric address; port zero requests dynamic binding when supported by the selected transport.
 * @param config [in] Optional full configuration; NULL selects defaults and does not supply production keys.
 * @return A caller-owned transport or NULL on any creation failure. Use create_ex to obtain the specific refusal status.
 */
struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config );

// As rocketnet_server_transport_create, with the exact section-2.1 code
// written to *status on every return path when status is not NULL
// (ROCKETNET_STATUS_OK on success, a non-zero ROCKETNET_STATUS_* on failure).
// A failure logs one rocketnet_error line, except the link-simulator
// OUT_OF_MEMORY and MUTEX_CREATE_FAILED exits, which return without logging;
// the non-_ex form passes NULL.
/** @brief Create a server transport and report the exact refusal status.
 * @param context [in] Caller-provided allocator context passed to allocations for this transport; NULL is allowed if supported by the configured allocator.
 * @param public_address_string [in] Non-NULL numeric bind/public address with optional port; port zero requests a dynamically assigned port when the selected transport supports it.
 * @param config [in] Optional full server configuration; NULL selects defaults, which require application keys.
 * @param time [in] Finite monotonic initial time in seconds.
 * @param status [out] Optional status destination; receives OK or the specific refusal code.
 * @return A caller-owned server transport on success, or NULL. Failures report INVALID_ARGUMENT, NOT_INITIALIZED, CONFIG_ENV_INVALID, CONFIG_OUT_OF_RANGE, MEMORY_BUDGET_EXCEEDED, MISSING_KEYS, INVALID_PROTOCOL_ID, DEV_KEYS_IN_PRODUCTION, TEST_PACKET_FILTER_IN_PRODUCTION, INVALID_TRANSPORT_LIST, INVALID_ADDRESS, TRANSPORT_PORT_ZERO_UNSUPPORTED, TRANSPORT_FAMILIES_UNSUPPORTED, TRANSPORT_LADDER_EXHAUSTED, OUT_OF_MEMORY, MUTEX_CREATE_FAILED, or THREAD_CREATE_FAILED. TRANSPORT_FAMILIES_UNSUPPORTED is the status when every candidate that opened refused because it cannot serve both named families. A transport whose open fails an allocation ends the create with OUT_OF_MEMORY and the ladder tries no later candidate; nothing the create allocated stays live. Correct the reported cause before retrying. Every failure logs an error line except two: when a simulated loss, latency, jitter, or duplicate setting is non-zero, a failed link-simulator allocation (OUT_OF_MEMORY) or a failed link-simulator mutex creation (MUTEX_CREATE_FAILED) returns without logging.
 * @code
 * // context, address, config, status, time, and dt are supplied by the game.
 * rocketnet_init( NULL, NULL, NULL );
 * struct rocketnet_server_transport_t *server = rocketnet_server_transport_create_ex(context, address, config, time, &status);
 * if (server) { rocketnet_server_transport_stop(server); while (rocketnet_server_transport_state(server) != ROCKETNET_SERVER_TRANSPORT_STOPPED) { time += dt; rocketnet_server_transport_update(server, time); } rocketnet_server_transport_destroy(server); }
 * rocketnet_term();
 * @endcode
 */
struct rocketnet_server_transport_t * rocketnet_server_transport_create_ex( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config, double time, int * status );

// create with a config config_bytes long: a known size is read over the
// defaults and created as above; an unknown size is refused with a log line and
// returns NULL before any other check. a NULL config is the default shape,
// whatever config_bytes says.
/** @brief Create a server transport from a versioned config prefix.
 * @param[in] context Opaque pointer passed to callbacks; may be NULL.
 * @param[in] public_address_string Public bind/advertise address; NULL is counted misuse and returns NULL.
 * @param[in] config Config prefix to read over defaults; NULL selects defaults regardless of config_bytes.
 * @param[in] config_bytes Size in bytes of config; non-NULL configs require a known revision.
 * @return The transport on success, or NULL when the size is unknown or creation is refused.
 * @details The library must be initialized first. Unknown sizes are refused before other argument checks. A known-size config follows ordinary server creation, including environment overrides and validation. Stop and destroy the returned object according to the transport lifecycle; it owns its internal allocations.
 */
struct rocketnet_server_transport_t * rocketnet_server_transport_create_sized( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config, size_t config_bytes );

// a NULL server_transport is a counted no-op.
/** @brief Release a stopped server transport and its owned resources.
 * @param server_transport [in] Transport returned by a create function; NULL is a counted no-op.
 * @return Nothing. Call stop, then poll stopped before destroying.
 */
void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport );      // stop and wait for stopped first

// a NULL server_transport is a counted no-op.
/** @brief Request an orderly server stop.
 * @param server_transport [in,out] Created server transport; NULL is a counted no-op.
 * @return Nothing. Poll stopped until true, then destroy.
 */
void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport );
bool rocketnet_server_transport_stopped( const struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport is a counted no-op that returns 0.
/** @brief Read the server lifecycle state.
 * @param server_transport [in] Created server transport; NULL is a counted no-op.
 * @return RUNNING, STOPPING, or STOPPED; NULL returns RUNNING's numeric value (zero). Read on the main thread outside update work.
 */
int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport );

// The port the transport actually bound. A public address string carrying
// port zero asks the kernel for an ephemeral port at create; the granted
// port is adopted into the advertised address -- so connect tokens minted
// for it validate -- and this reads it back. Tests and single-box tools
// build their token address from it instead of hardcoding a port;
// production servers know their public address already.
//
// Port-zero discovery is a per-transport property: a transport whose create binds
// the port it is given and discovers nothing -- the XDP transport -- refuses a
// port-zero address at create, loudly, before it binds anything. An "auto"
// ladder falls past it to a transport that does discover, and only a pin on a
// non-discovering transport refuses the create outright. The portable, msgx and
// GSO transports discover; the XDP transport does not. So this returns either the
// port the public address carried, or the port a discovering transport was
// granted.

// a NULL server_transport is a counted no-op that returns 0.
/** @brief Return the port the server transport bound.
 * @param server_transport [in] Created server transport; NULL is a counted no-op.
 * @return The bound port in host byte order; zero for NULL. A configured port of zero is supported only by transports that can discover the bound port.
 */
uint16_t rocketnet_server_transport_port( struct rocketnet_server_transport_t * server_transport );

struct rocketnet_address_t;

// The address the running transport advertises for one family
// (ROCKETNET_ADDRESS_IPV4 or ROCKETNET_ADDRESS_IPV6), the bound port filled
// in: the one connect tokens name. false, *address untouched, when the
// transport does not serve that family or is not running.
bool rocketnet_server_transport_advertised_address( struct rocketnet_server_transport_t * server_transport, int family, struct rocketnet_address_t * address );

// the granted transport, request-vs-granted style: the name of the candidate
// that won the ladder at create ("xdp", "gso", "msgx", "portable"), never
// the requested list. the 1000s-of-players claims attach to the granted
// tier this returns, not the requested one -- one log line at create names
// the same grant and why every earlier candidate refused.

// a NULL server_transport is a counted no-op that returns NULL.
/**
 * @brief Returns the server transport granted at creation.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning NULL.
 * @return Transport-owned granted transport name, valid until destroy, or NULL.
 */
const char * rocketnet_server_transport_io_name( struct rocketnet_server_transport_t * server_transport );

// the commit barrier for host processes that must never commit an XDP data
// plane (test harnesses, smokes): installed once, the xdp transport's create
// refuses before its first act; the non-mutating pre-flight stays
// reachable. a no-op on platforms without the xdp transport.

/**
 * @brief Prevents XDP transports from committing a data plane in this process.
 *
 * @return No value. Install before XDP server creation; XDP create refuses before commit while preflight remains available.
 */
void rocketnet_forbid_xdp_commit( void );

// a NULL server_transport is a counted no-op.
/** @brief Advance server networking and publish this update's payload and acknowledgement batches.
 * @param server_transport [in,out] Created transport; NULL is a counted no-op.
 * @param time [in] Finite monotonic time in seconds, from the same application clock used for other timed calls.
 * @return Nothing. Call on the main thread, outside concurrent packet-fill phases.
 */
void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport, double time );

// Unit tests only: supplies token-validation Unix time so update reads no wall clock.
void rocketnet_server_transport_set_test_timestamp( struct rocketnet_server_transport_t * server_transport, uint64_t timestamp );

// The payloads delivered by the last update, valid until the next update.
// Iterated by index so the storage behind them stays the library's own --
// its layout and its capacity are not part of this surface. Reassembled
// fragmented payloads take their place in this batch in arrival position,
// at the moment they complete, up to the configured maximum payload size
// -- size any buffer you copy into by that configuration, not by the
// wire's ceiling.
//
/**
 * @brief Returns the number of payloads delivered by the last server update.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning zero.
 * @return Payload count; batch remains valid until next update.
 */
//     int num_payloads = rocketnet_server_transport_num_payloads( server );
//     for ( int i = 0; i < num_payloads; i++ )
//     {
//         int client_index, payload_bytes;
/**
 * @brief Returns one payload delivered by the last server update.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning NULL.
 * @param payload_index Zero-based index in the latest batch.
 * @param client_index Receives associated slot index; NULL is a counted no-op returning NULL.
 * @param payload_bytes Receives payload length; NULL is a counted no-op returning NULL.
 * @return Borrowed view valid until next update, or NULL for invalid input or index.
 */
//         uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &payload_bytes );
//         ...
//     }

// a NULL server_transport is a counted no-op that returns 0.
int rocketnet_server_transport_num_payloads( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport, client_index or payload_bytes is a counted no-op that returns NULL.
uint8_t * rocketnet_server_transport_payload( struct rocketnet_server_transport_t * server_transport, int payload_index, int * client_index, int * payload_bytes );

// The whole receive batch as parallel arrays in one call, valid until the
// next update -- the zero-per-item form of the accessors above, for
// bindings and tight loops. Returns the number of payloads.

// a NULL server_transport, client_index, payload_data or payload_bytes is a counted no-op that returns 0.
/**
 * @brief Returns parallel views of payloads delivered by the last server update.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning zero.
 * @param client_index Receives slot indices parallel to payload arrays.
 * @param payload_data Receives pointers to payload bytes.
 * @param payload_bytes Receives lengths in bytes.
 * @return Payload count; arrays are borrowed and valid until next update.
 */
int rocketnet_server_transport_payloads( struct rocketnet_server_transport_t * server_transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes );

// The connection id of each payload in the batch, parallel to the client
// index array of rocketnet_server_transport_payloads and valid until the next
// update: the connection the payload was received for, which differs from
// rocketnet_server_transport_client_connection_id when the slot was released
// and reassigned later in the same update. Returns the number of payloads.

// a NULL server_transport or connection_id is a counted no-op that returns 0.
/**
 * @brief Returns connection ids parallel to the last server payload batch.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning zero.
 * @param connection_id Receives parallel id array; NULL is a counted no-op returning zero.
 * @return Number of ids; array is valid until next update.
 */
int rocketnet_server_transport_payload_connection_ids( struct rocketnet_server_transport_t * server_transport, const uint64_t ** connection_id );

// The send surface is acquire, write, commit: start_packet hands back a
// 16-aligned frame; the payload is written at frame + payload_index --
// itself 16-aligned, capacity a multiple of eight, so any word-based
// serializer roots there directly -- and finish_packet seals it in place
// on the calling thread and stages it. Nothing is copied at any step; the
// packet is encrypted where it was written. send_packets flushes
// everything staged.
//
// request_bytes declares the capacity the caller needs, and the cost is
// declared with it. A request at or under ROCKETNET_MAX_PACKET_PAYLOAD_BYTES is
// a real wire frame: written in place, sealed in place, zero copies. A
// request above it -- up to the configured maximum payload -- routes to
// the transport's arena, and finish_packet splits the written payload
// into type 9 fragments, each an ordinary sealed session packet, the
// copy spent only where splitting makes it unavoidable; an over-capacity
// request whose write turns out to fit one packet collapses to a single
// ordinary payload packet with one copy. Either way the context's one
// packet_id is the payload's: a fragmented payload's id surfaces in the
// acked batch when every fragment's wire packet is acknowledged. A lost
// fragment loses the whole payload -- the transport never resends.
//
// The context's payload_capacity reports the frame's real capacity -- the
// wire capacity on the wire route, the configured maximum on the arena
// route -- never the request itself: a caller may write up to it, and
// finish_packet routes on what was actually written.
//
// Threading contract: update and send_packets run on the main thread, and
// so do disconnect, disconnect_client and stop -- never while a fill is
// open. start_packet, finish_packet and abort_packet may run on any number
// of game threads strictly between an update and the following
// send_packets, and the handoff between fill and flush must be a
// synchronizing operation (a join or a barrier), not a wall-clock wait.
// Call send_packets after each joined fill phase. There is no required frame,
// update, send or ACK rate. When staged packets actually leave is per
// transport, and a caller owns only the guarantee: everything staged before
// send_packets is sent by it, and nothing is guaranteed sent without it.
// Code against the guarantee, not a transport's eagerness. Calling
// send_packets again without newly staged packets is a no-op. The fields of the
// context are read-only to the caller. The transport privately owns and checks
// all metadata; a copy can be consumed once, and stale, modified or wrong-owner
// contexts are refused before dereferencing a frame. packet_id is an opaque,
// process-unique lease identifier. Active payload views expire at
// finish/abort or the next update/flush; raw pointers cannot be revoked.
//
// finish_packet returns the sealed wire size, or zero when the packet was
// dropped instead (payload out of range, the connection gone, or a wire
// frame it could not reserve). The server assigns its session sequence only
// after finish accepts the payload and reserves every wire frame, so a
// refused or dropped finish takes no sequence and leaves no gap in the
// client's receive-side loss stat. USAGE.md carries the worked arithmetic.

/** @brief Describes a reserved packet payload lease.
 *
 * The transport fills this output structure at start_packet. Treat its fields
 * as read-only and pass the same, unmodified object to packet_payload, finish
 * or abort. Its frame and payload view are borrowed and expire when the lease
 * is consumed or the transport is updated, flushed or destroyed.
 */
struct rocketnet_packet_context_t
{
    /** @brief Reserved frame, borrowed for the active lease. */
    uint8_t * frame;
    /** @brief Byte offset from frame to the payload start. */
    int payload_index;
    /** @brief Writable payload capacity in bytes. */
    int payload_capacity;
    /** @brief Opaque id used to match this payload with an ACK. */
    uint64_t packet_id;
    /** @brief First wire sequence, populated by server finish_packet or client start_packet. */
    uint64_t sequence;
    /** @brief Server slot index for a server lease; -1 for a client lease. */
    int client_index;
};

// a NULL server_transport or context is a counted no-op that returns NULL.
/**
 * @brief Reserves a send frame for one connected server client.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning NULL.
 * @param client_index Connected client slot index.
 * @param request_bytes Requested capacity in bytes, from 1 through configured maximum.
 * @param context Receives lease metadata; NULL is a counted no-op returning NULL.
 * @return Writable payload pointer, or NULL when slot or reservation is invalid. Fill calls may run concurrently between update and the following flush.
 */
uint8_t * rocketnet_server_transport_start_packet( struct rocketnet_server_transport_t * server_transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context );

// Validates an active packet lease and returns its payload view. NULL/zero on
// stale, consumed, modified or wrong-owner context. The view expires at finish,
// abort, update, flush or destroy; callers must join before those operations.
// a NULL transport, context or capacity is a counted no-op that returns NULL.
/**
 * @brief Validates a server packet lease and returns its writable view.
 *
 * @param transport Server transport that created the lease; NULL is a counted no-op.
 * @param context Unmodified active lease from start_packet; NULL is a counted no-op.
 * @param capacity Receives capacity in bytes; NULL is a counted no-op.
 * @return Borrowed payload pointer, or NULL for invalid lease; view expires at finish, abort, update, flush, or destroy.
 */
uint8_t * rocketnet_server_transport_packet_payload( struct rocketnet_server_transport_t * transport, const struct rocketnet_packet_context_t * context, int * capacity );

// a NULL server_transport or context is a counted no-op that returns 0.
// An unstarted, consumed, stale, modified or wrong-owner context asserts in a
// debug build and returns 0 in a release build.
/**
 * @brief Seals and stages bytes written into a server packet lease.
 *
 * @param server_transport Server transport that created the lease; NULL is a counted no-op.
 * @param context Active, unmodified lease from start_packet; this call consumes it. Invalid or stale leases assert in a debug build and return zero in a release build.
 * @param payload_bytes Initialized byte count, from 1 through reported capacity.
 * @return Sealed wire size, or zero when refused or dropped.
 */
int rocketnet_server_transport_finish_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context, int payload_bytes );

// a NULL server_transport or context is a counted no-op.
/**
 * @brief Releases an unused server packet lease.
 *
 * @param server_transport Server transport that created the lease; NULL is a counted no-op.
 * @param context Active, unmodified lease to release; NULL is a counted no-op.
 * @return No value. Invalid or stale leases are refused and counted.
 */
void rocketnet_server_transport_abort_packet( struct rocketnet_server_transport_t * server_transport, struct rocketnet_packet_context_t * context );

// a NULL server_transport is a counted no-op.
/**
 * @brief Flushes server packets staged since the previous flush.
 *
 * @param server_transport Server transport; NULL is a counted no-op.
 * @return No value. Call on main thread after fill threads join.
 */
void rocketnet_server_transport_send_packets( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport or payload_data is a counted no-op.
/**
 * @brief Copies and stages one payload for a connected server client.
 *
 * @param server_transport Server transport; NULL is a counted no-op.
 * @param client_index Connected client slot index.
 * @param payload_data Payload bytes to copy; NULL is a counted no-op.
 * @param payload_bytes Length in bytes, from 1 through configured maximum.
 * @return No value. Flush staged packets with send_packets.
 */
void rocketnet_server_transport_send_packet( struct rocketnet_server_transport_t * server_transport, int client_index, const uint8_t * payload_data, int payload_bytes );

// Disconnects one connected client deliberately: the redundant disconnect
// burst is staged and the slot frees immediately. Buffered transports may send
// full batches during this call; remaining packets leave on the next flush.
// Why is the game's decision; this is only the mechanism. Main thread only, never
// while a fill phase is open -- it tears down the connection state the
// fill relies on.

// a NULL server_transport is a counted no-op.
/** @brief Disconnect a connected client and release its slot.
 * The slot frees within this call. The redundant disconnect burst is queued for
 * the client's address and leaves on the next rocketnet_server_transport_send_packets
 * or rocketnet_server_transport_update, whichever comes first; a buffered transport
 * may send a full batch during this call. When send staging is full the rest of
 * the burst is dropped, and the slot is still released.
 * @param server_transport [in,out] Created server transport; NULL is a counted no-op.
 * @param client_index [in] Slot index from 0 through max_clients - 1. A free or out-of-range slot is ignored.
 * @return Nothing. Main thread only, never while a fill phase is open.
 */
void rocketnet_server_transport_disconnect_client( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns 0.
/** @brief Count connected client slots.
 * @param server_transport [in] Created server transport; NULL is a counted no-op.
 * @return Number of connected slots, from 0 through configured max_clients. Read on the main thread outside update work.
 */
int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport is a counted no-op that returns 0.
/** @brief Check whether a slot currently holds a connected client.
 * @param server_transport [in] Created server transport; NULL is a counted no-op.
 * @param client_index [in] Slot index from 0 through max_clients - 1.
 * @return 1 when connected, 0 when free or out of range; NULL transport returns 0. Read on the main thread outside update work.
 */
int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns 0.
/** @brief Read the client identifier currently assigned to a slot.
 * @param server_transport [in] Created server transport; NULL is a counted no-op.
 * @param client_index [in] Slot index from 0 through max_clients - 1.
 * @return The connected client's identifier, or zero for a free or invalid slot. Read on the main thread outside update work.
 */
uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index );

// The connection that holds the slot now: nonzero and unique for each
// connection over the transport's life, 0 while the slot is free (and for an
// index out of range). A reconnect into the same slot, even inside one
// update, has a new id; compare it with the per-entry ids of the payload and
// acked batches to tell whose an entry is.

// a NULL server_transport is a counted no-op that returns 0.
/** @brief Read the unique identifier of the connection currently holding a slot.
 * @param server_transport [in] Created server transport; NULL is a counted no-op.
 * @param client_index [in] Slot index from 0 through max_clients - 1.
 * @return A nonzero identifier for a live connection, or zero for a free or invalid slot. A reconnect receives a new identifier. Read on the main thread outside update work.
 */
uint64_t rocketnet_server_transport_client_connection_id( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns NULL.
const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport );

// the maximum payload this transport sends and reassembles --
// ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES -- so a caller can size
// the buffers it copies payloads into

// a NULL server_transport is a counted no-op that returns 0.
/**
 * @brief Returns the configured server payload limit.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning zero.
 * @return Maximum send and reassembly payload size in bytes, or zero for NULL.
 */
int rocketnet_server_transport_max_payload_bytes( struct rocketnet_server_transport_t * server_transport );

// The server's whole-socket rates: packets and bandwidth per second, both
// directions, over the trailing second, from time-based buckets (about 4 ms
// boundary resolution) at bounded cost: no per-client sweep, no extra pass
// over any buffer. At 1000 clients the update pays the same fixed bucket sum
// it pays at one. These numbers feed time synchronization. The cumulative
// totals count every wire datagram and byte over the server transport's
// lifetime.

struct rocketnet_server_transport_stats_t
{
    uint64_t packets_sent_per_second;
    uint64_t packets_received_per_second;
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
    uint64_t packets_sent;                          // wire datagrams sent over the server's lifetime
    uint64_t packets_received;                      // wire datagrams received over the server's lifetime
    uint64_t bytes_sent;                            // wire bytes sent over the server's lifetime
    uint64_t bytes_received;                        // wire bytes received over the server's lifetime
};

// a NULL server_transport or stats is a counted no-op.
void rocketnet_server_transport_stats( struct rocketnet_server_transport_t * server_transport, struct rocketnet_server_transport_stats_t * stats );

// Per-client connection quality, per slot: rtt and jitter in milliseconds,
// packet loss as the percentage of this end's sent packets never
// acknowledged, and the trailing bandwidth sent, received and acknowledged.
// Fed by the ack blocks every session packet carries; recomputed by modulo
// phase, at most 20 configured slots visited per update, every slot covered
// once per phase cycle. The cumulative totals count every wire
// datagram and byte over the slot's current connection, zeroed when a new
// connection takes the slot.

struct rocketnet_server_transport_client_stats_t
{
    float rtt;                                      // round trip time (milliseconds)
    float jitter;                                   // deviation of rtt samples above the minimum (milliseconds)
    float packet_loss;                              // percent of sent packets never acknowledged
    float bandwidth_sent_kbps;
    float bandwidth_received_kbps;
    float bandwidth_acked_kbps;
    uint64_t packets_sent;                          // wire datagrams sent on the slot's current connection
    uint64_t packets_received;                      // wire datagrams received on the slot's current connection
    uint64_t bytes_sent;                            // wire bytes sent on the slot's current connection
    uint64_t bytes_received;                        // wire bytes received on the slot's current connection
};

// a NULL server_transport or stats is a counted no-op.
void rocketnet_server_transport_client_stats( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_server_transport_client_stats_t * stats );

// The delivery notification primitive: the batch of packet ids newly
// acknowledged by the last update, as parallel arrays with the client index
// of each, valid until the next update. The packet id is the one handed out
// in rocketnet_packet_context_t at start_packet -- the join key back to
// whatever the game sent. The transport tells; it never resends. Keep-alive,
// disconnect and standalone-ack sequences have no packet id and never
// appear here. When more packets are acknowledged in one update than the
// batch holds, the overflow is deferred to a later update rather than lost
// -- the ACKED_BATCH_FULL counter counts each deferral.

// a NULL server_transport, client_index or packet_ids is a counted no-op that returns 0.
/**
 * @brief Returns packet ids acknowledged by the last server update.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning zero.
 * @param client_index Receives parallel slot indices; NULL is a counted no-op returning zero.
 * @param packet_ids Receives opaque packet ids; NULL is a counted no-op returning zero.
 * @return Number of ids; both arrays are valid until next update. Overflow is deferred and counted by ROCKETNET_COUNTER_ACKED_BATCH_FULL.
 */
int rocketnet_server_transport_acked_packets( struct rocketnet_server_transport_t * server_transport, const int ** client_index, const uint64_t ** packet_ids );

// The connection id of each acked packet id, parallel to the client index
// array of rocketnet_server_transport_acked_packets and valid until the next
// update: the connection whose ack block acknowledged it. Returns the number
// of acked packet ids.

// a NULL server_transport or connection_id is a counted no-op that returns 0.
/**
 * @brief Returns connection ids parallel to the last acknowledged packet batch.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning zero.
 * @param connection_id Receives parallel id array; NULL is a counted no-op returning zero.
 * @return Number of ids; array is valid until next update.
 */
int rocketnet_server_transport_acked_connection_ids( struct rocketnet_server_transport_t * server_transport, const uint64_t ** connection_id );

// The payload send capacity: how many payload frames one update may start.
// Create sets the default, at least the configured client count; the first
// call is the unit seam that makes the next create read `packets` instead
// (0 restores the default). The available query is what the message layer's
// send pass reads before it gives a connection its turn.
/** @brief Sets the send capacity the next server transport create reads, for a unit test. */
void rocketnet_server_transport_set_create_send_capacity_for_test( int packets );
/** @brief Returns the transport's payload send capacity. */
int rocketnet_server_transport_send_capacity( const struct rocketnet_server_transport_t * server_transport );
/** @brief Returns the payload frames that may still start this update. */
int rocketnet_server_transport_send_capacity_available( const struct rocketnet_server_transport_t * server_transport );

// -----------------------------------------
//                         frame sync
// -----------------------------------------

// Pegs a headless server loop to real time: an absolute deadline per frame,
// the platform sleep that honors it, thread priority so the wakeups are
// sharp, and a capped adaptive spin guard for the last microseconds.
// Optional -- examples may still just sleep. Create at a frame rate, then
// call wait once per loop iteration; it returns at the frame boundary.
// Dropped frames are detected, counted and resynchronized, never chased.
// Priority is taken on the waiting thread at the first wait; where the
// platform refuses it the primitive degrades to plain absolute sleep --
// never an unprivileged spin -- and the stats say so.

/**
 * @brief Creates a real-time frame synchronizer.
 *
 * @param context Allocator context; NULL uses the default context.
 * @param frame_rate Finite frames per second, greater than zero and no more than 1000.
 * @return Caller-owned synchronizer, or NULL if RocketNet is not initialized, the rate is invalid, allocation fails, or the platform timer cannot be created.
 */
struct rocketnet_frame_sync_t * rocketnet_frame_sync_create( void * context, double frame_rate );

// As rocketnet_frame_sync_create, with the section-2.1 code written to *status
// on every return path when status is not NULL (F1 INVALID_ARGUMENT,
// F2 OUT_OF_MEMORY, F3 TIMER_CREATE_FAILED). A failure logs one error line.
/**
 * @brief Creates a frame synchronizer and reports its error status.
 *
 * @param context Allocator context; NULL uses the default context.
 * @param frame_rate Finite frames per second, greater than zero and no more than 1000.
 * @param status Receives ROCKETNET_STATUS_OK or the specific refusal code; NULL omits status reporting.
 * @return Caller-owned synchronizer, or NULL on refusal. Status receives ROCKETNET_STATUS_OK, NOT_INITIALIZED, INVALID_ARGUMENT, OUT_OF_MEMORY, or TIMER_CREATE_FAILED if the platform timer cannot be created.
 */
struct rocketnet_frame_sync_t * rocketnet_frame_sync_create_ex( void * context, double frame_rate, int * status );

// a NULL frame_sync is a counted no-op.
/**
 * @brief Releases a frame synchronizer.
 *
 * @param frame_sync Synchronizer to release; NULL is a counted no-op.
 * @return No value. Stop using the object before destruction.
 */
void rocketnet_frame_sync_destroy( struct rocketnet_frame_sync_t * frame_sync );

// a NULL frame_sync is a counted no-op.
/**
 * @brief Waits until the next frame deadline.
 *
 * @param frame_sync Synchronizer; NULL is a counted no-op.
 * @return No value. Call once per loop iteration from its owning thread.
 */
void rocketnet_frame_sync_wait( struct rocketnet_frame_sync_t * frame_sync );

struct rocketnet_frame_sync_stats_t
{
    uint64_t frame;                                 // frames completed
    uint64_t dropped_frames;                        // frames lost to overruns, total
    int priority_held;                              // platform thread priority obtained
    int spinning;                                   // spin guard active (requires priority)
    double guard_seconds;                           // the current adaptive guard
    double wakeup_error_seconds;                    // the last wake's lateness past its deadline
};

// a NULL frame_sync or stats is a counted no-op.
void rocketnet_frame_sync_stats( struct rocketnet_frame_sync_t * frame_sync, struct rocketnet_frame_sync_stats_t * stats );

// -----------------------------------------
//                           messages
// -----------------------------------------

// The message layer above the transports (docs/CHANNELS.md): the message
// config, the sizing of one connection's region, callable before any
// connect, and the client and server message objects.

// One outcome for a send's nonzero serial. The list returned after an update
// is valid until the next update; a full receipt ring drops its oldest entry.
struct rocketnet_message_receipt_t
{
    uint32_t serial;
    int32_t status;             // ROCKETNET_RECEIPT_STATUS_ACKED or ROCKETNET_RECEIPT_STATUS_LOSS
};

// The message config. Fields marked WIRE must be identical on both ends; the
// connection verifies them. Validation applies the ranges here and the cross
// rules of docs/CHANNELS.md section 14, and refuses, never clamps.

/** @brief Memory, wire, and timing settings for the message layer.
 * @details Member comments state defaults and accepted ranges. Fields marked WIRE must agree between client and server. The caller owns this value; validation and sizing do not allocate.
 */
struct rocketnet_message_config_t
{
    /** @brief Message channels; WIRE, 1 through ROCKETNET_MAX_CHANNELS, default 32. */
    int num_channels;                   // WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    /** @brief Reliable receive window; WIRE, power of two from 64 through 1<<20, default 1024. */
    int reliable_messages_in_flight;                // WIRE, power of two; 64 .. 1 << 20; default 1024
    /** @brief Receive ring bytes; WIRE, multiple of 16 from 4096 through 1<<20, default 1048576. */
    int receive_message_buffer_bytes;             // WIRE, multiple of 16; 4096 .. 1 << 20; default 1048576
    /** @brief Maximum message bytes; WIRE, at most receive_message_buffer_bytes and from ROCKETNET_MESSAGE_SINGLE_BYTES through 1<<20, default 1048576. */
    int max_message_bytes;              // WIRE, <= receive_message_buffer_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. ROCKETNET_MAX_MESSAGE_BYTES; default 1048576
    /** @brief Per-connection queued payload bytes; 12288 through 1<<28, default 4194304. */
    int send_message_buffer_bytes;              // 12288 .. 1 << 28; default 4194304
    /** @brief Queued message count; 16 through 1<<20, default 1024. */
    int send_queue_messages;            // 16 .. 1 << 20; default 1024
    /** @brief Tracked unacknowledged packets; 64 through 1<<20 and at least reliable_messages_in_flight, default 1024. */
    int packets_in_flight;              // 64 .. 1 << 20, >= reliable_messages_in_flight; default 1024
    /** @brief Packets sent per connection per update, in-place unreliable messages and the send pass together; 1 through 1024, default 256. */
    int packets_per_update;         // 1 .. 1024; default 256
    /** @brief Unreliable deliveries per connection per update; 1 through 1<<16, default 256. */
    int unreliable_messages_per_update;          // per connection per update; 1 .. 1 << 16; default 256
    /** @brief Messages held for sequenced delivery; 0 through 4096, default 16. */
    int sequenced_messages_held;                // 0 .. 4096; default 16
    /** @brief Receipt history entries; 16 through 1<<16, default 256; a full ring drops its oldest. */
    int max_message_receipts;                   // 16 .. 1 << 16; default 256
    /** @brief Initial resend delay in milliseconds; 10 through 60000, default 2000. */
    int message_resend_delay_initial_ms;                 // 10 .. 60000; default 2000
    /** @brief Optional sampled resend delay floor in milliseconds; 0 disables it. 0 through 60000, default 0. */
    int message_resend_delay_min_ms;                     // optional resend delay floor; 0 disables; 0 .. 60000; default 0
    /** @brief Maximum resend delay in milliseconds; 10 through 60000, default 2000. */
    int message_resend_delay_max_ms;                     // 10 .. 60000; default 2000
    /** @brief Reliable-message lifetime in milliseconds; 1000 through 600000, default 10000. */
    int reliable_message_timeout_ms;            // 1000 .. 600000; default 10000
    /** @brief Default client send budget in kilobits per second; zero or less is uncapped, positive values may not exceed ROCKETNET_SEND_BUDGET_MAX_KBPS. */
    int client_send_kbps;                // client default budget; 0 or lower uncapped; <= ROCKETNET_SEND_BUDGET_MAX_KBPS
    /** @brief Default per-client server send budget in kilobits per second; zero or less is uncapped, positive values may not exceed ROCKETNET_SEND_BUDGET_MAX_KBPS. */
    int server_send_kbps;                // server per-client default budget; 0 or lower uncapped; <= ROCKETNET_SEND_BUDGET_MAX_KBPS
};

// a NULL config is a counted no-op.
/** @brief Fill a message config with its defaults.
 * @param[out] config Receives the defaults; NULL is a counted no-op.
 * @details This writes caller-owned storage and is safe before initialization. Edit the config before validating or creating message objects; do not mutate it concurrently with those calls.
 */
void rocketnet_message_default_config( struct rocketnet_message_config_t * config );

// the defaults for a caller whose struct is config_bytes long, a known size of
// the struct. 1 written; 0 refused, nothing written. a NULL config is a
// counted no-op that returns 0.
/** @brief Write defaults into a known message config revision.
 * @param[out] config Receives the defaults; NULL is a counted no-op and returns 0.
 * @param[in] config_bytes Size, in bytes, of the caller's config revision.
 * @return 1 when config_bytes is a known revision and defaults are written; 0 when unknown or config is NULL. An unknown size leaves config untouched.
 * @details Known prefixes receive defaults for that revision. This operation is allocation-free and safe before initialization; the caller owns config.
 */
int rocketnet_message_default_config_sized( struct rocketnet_message_config_t * config, size_t config_bytes );

// 1 when the config is accepted, else 0 with a log line naming the rule.
// NULL means the defaults. Allocation-free; safe before init.
/** @brief Check a message config without allocating memory.
 * @param[in] config Settings to check, or NULL to check the defaults.
 * @return 1 when every field and cross-field rule is accepted; 0 when a rule is refused, with an error log naming the rule.
 * @details This function may run before initialization and does not create a connection. It checks field ranges and message-layer relationships such as ring capacity, retransmission timing, and wire constraints. It never clamps values.
 */
int rocketnet_message_validate_config( const struct rocketnet_message_config_t * config );

// As rocketnet_message_validate_config, with the section-2.1 code written to
// *status on every return path when status is not NULL (CONFIG_OUT_OF_RANGE on
// refusal). A refusal logs one error line.
/** @brief Validate message settings and return the refusal status.
 * @param[in] config Settings to check, or NULL to check the defaults.
 * @param[out] status Optional status output; receives ROCKETNET_STATUS_OK on success or ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE on refusal.
 * @return 1 when accepted, otherwise 0; a refusal logs one error line naming the rule.
 * @details Allocation-free and safe before initialization. The caller owns status and config; NULL status is allowed.
 */
int rocketnet_message_validate_config_ex( const struct rocketnet_message_config_t * config, int * status );

// The exact bytes of one connection's region (docs/CHANNELS.md 14): the
// formula plus the 64-byte alignment of each part. 0 for a refused config;
// NULL means the defaults. Allocation-free; callable before init or connect.
// A server holds one region per client slot, a client one.
/** @brief Measure the aligned memory region for one message connection.
 * @param[in] config Message settings, or NULL for defaults.
 * @return Exact region bytes including 64-byte alignment padding, or 0 when the config is refused.
 * @details This formula-only query allocates nothing and is callable before initialization or connection creation. A server needs one region per client slot; a client needs one. The result is not memory allocated by the SDK.
 */
uint64_t rocketnet_message_connection_bytes( const struct rocketnet_message_config_t * config );

// The formula, each term at this config, the padding, the region and the
// total for num_connections regions, as text into buffer. Returns the full
// length (as snprintf: a short buffer is truncated and terminated, and NULL
// with 0 measures), or 0 for a refused config. Allocation-free.
/** @brief Format the message connection memory formula and totals.
 * @param[in] config Message settings, or NULL for defaults.
 * @param[in] num_connections Number of regions to total; negative values are treated as zero.
 * @param[out] buffer Output text, or NULL to measure when buffer_size is zero. A positive capacity receives a NUL-terminated, possibly truncated string.
 * @param[in] buffer_size Buffer capacity in bytes; negative values are treated as zero.
 * @return Full formatted character count excluding the terminator, as snprintf does, or 0 when config validation refuses the layout.
 * @details Allocation-free and callable before initialization. If the buffer is short, inspect the returned length to detect truncation. The caller owns the buffer.
 */
int rocketnet_message_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections, char * buffer, int buffer_size );

// The same lines, logged at info level. Returns the region bytes, or 0 refused.
/** @brief Log the connection memory breakdown at info level.
 * @param[in] config Message settings, or NULL for defaults.
 * @param[in] num_connections Number of regions to display; negative values are treated as zero.
 * @return Bytes in one connection region, or 0 when config validation refuses the layout.
 * @details This emits one info log line per breakdown line and allocates no region. Call it when info logging is appropriate; use rocketnet_message_connection_sizing for caller-owned text instead.
 */
uint64_t rocketnet_message_print_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections );

// The client and server message objects over the transports (docs/CHANNELS.md
// 10 to 12): a message is bytes, a pointer and a length. Each object is
// one block made at create -- the object, its connections and one region per
// connection (the client's one, one per server slot) -- and allocates nothing
// after create. memory is optional: NULL makes the one allocation, and a
// caller block of at least *_required_memory bytes, 64-aligned, makes none
// (the transport still allocates its own at its create).
//
// The update order, on the main thread: update (the transport's update, the
// transport's acked packet ids into the channels -- the only ack there is --
// then the received payloads into deliveries, then the exhaustion and
// error checks that disconnect), receive and release, start/write/finish,
// then send_packets (each connection's packets under its send budget into
// frames from the transport's start_packet, then the transport's
// send_packets).
//
// Sending mirrors the transport's start_packet and finish_packet: start
// returns a pointer into the connection's region; write the message there
// once, in place; finish(bytes_written) queues it and returns its receipt
// serial, nonzero; abort gives the room back. One start is
// pending per object at a time. The enabled types are ROCKETNET_RELIABILITY_UNRELIABLE,
// ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
// ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
// ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, and ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
// a type a caller's own mask leaves out is refused (counted
// MESSAGE_SEND_TYPE_NOT_ENABLED). Plain
// UNRELIABLE uses records packed together in a frame, up to 1411 bytes per message. UNRELIABLE_SEQUENCED
// can use the transport's fragments for one larger unreliable RECORDS payload.
// Receipt-bearing UNRELIABLE uses the code-0 wire form at single-packet size and promotes a larger
// message to a reliable code-2 block with an ACKED-only receipt. Both are
// received on channel 0. The reliable types and promoted blocks are bounded
// by the config's max_message_bytes.
//
// A start the connection's region has no room for ends the connection:
// the server disconnects that client, the client disconnects itself, with the
// reason ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED, and start returns
// NULL. The send budget governs the wire, never the queue: a reliable message
// waits for budget and is never dropped; an unreliable one over budget is
// dropped and counted (MESSAGE_UNRELIABLE_OVER_BUDGET).
//
// Receiving: receive hands out the next message of the update from the one
// receive queue (every channel's, and on a server every slot's; the queue is
// rn_message_object_next in sdk/rocketnet_message_server.cpp) as a const view
// and its length, valid until the next update. Every received
// message is released before the next update; update with one still held
// asserts in a debug build and returns ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED in
// a release build. Copy what is kept past the update.

// ---- client ----

// the bytes a caller block for a client object needs; 0 for a refused config
/** @brief Measure caller-provided memory for one message client.
 * @param[in] message_config Message settings, or NULL for defaults.
 * @return Required bytes for one client object, or 0 for a refused config or arithmetic overflow.
 * @details This query is allocation-free. The create call requires at least this many bytes in caller-provided memory aligned to 64 bytes; the caller retains ownership of that region.
 */
uint64_t rocketnet_message_client_required_memory( const struct rocketnet_message_config_t * message_config );

// a NULL connect_token is a counted no-op that returns NULL. NULL configs are the defaults.
// create reads no clock: the connect attempt starts at the first update, at whatever time the game passes it, and every time comes in through update.
struct rocketnet_message_client_t * rocketnet_message_client_create( void * context, const uint8_t * connect_token,
                                                                                    const struct rocketnet_client_transport_config_t * transport_config,
                                                                                    const struct rocketnet_message_config_t * message_config,
                                                                                    void * memory, uint64_t memory_bytes, uint32_t send_kbps );

// As rocketnet_message_client_create, with the section-2.1 code written to
// *status on every return path when status is not NULL: the transport's own
// code (INVALID_CONNECT_TOKEN, CONNECT_TOKEN_EXPIRED, a socket or thread
// failure), CONFIG_OUT_OF_RANGE for a refused message config or budget,
// OUT_OF_MEMORY when the object's block is refused.
// reserve_unlimited is for the public zero-budget setting: keep its runtime
// ceiling while omitting a finite create-time payload reserve.
struct rocketnet_message_client_t * rocketnet_message_client_create_ex( void * context, const uint8_t * connect_token,
                                                                        const struct rocketnet_client_transport_config_t * transport_config,
                                                                        const struct rocketnet_message_config_t * message_config,
                                                                        void * memory, uint64_t memory_bytes, uint32_t send_kbps, int * status, int reserve_unlimited );

// create with configs transport_config_bytes and message_config_bytes long: each
// known size is read over its defaults and created as above; an unknown size is
// refused with a log line and returns NULL before any other check. a NULL config
// is its defaults, whatever its size says.
/** @brief Create a message client using versioned transport and message configs.
 * @param[in] context Opaque callback context; may be NULL.
 * @param[in] connect_token Server-issued connection token; NULL is counted misuse and returns NULL.
 * @param[in] transport_config Client transport config prefix, or NULL for defaults.
 * @param[in] transport_config_bytes Size of that prefix in bytes; ignored when its config is NULL, otherwise must be a known revision.
 * @param[in] message_config Message config prefix, or NULL for defaults.
 * @param[in] message_config_bytes Size of that prefix in bytes; ignored when its config is NULL, otherwise must be a known revision.
 * @param[in] memory Caller-owned region, at least the required size and 64-byte aligned; NULL selects SDK allocation.
 * @param[in] memory_bytes Size of memory in bytes; must cover the required region when memory is supplied.
 * @param[in] send_kbps Per-client wire budget in kilobits per second: takes message_config client_send_kbps (the built-in default applies only when message_config is NULL; 0 or lower in that field disables the budget; a positive send_kbps argument overrides it for this create; above ROCKETNET_SEND_BUDGET_MAX_KBPS refuses; the environment override applies over both).
 * @return The client on success, or NULL for unknown config sizes, invalid arguments/configuration, an invalid budget, or insufficient memory.
 * @details The library must be initialized first. Unknown non-NULL config sizes are refused before create checks. The object owns its transport; destroy the client to release it. A supplied memory region remains caller-owned and must outlive the client.
 */
struct rocketnet_message_client_t * rocketnet_message_client_create_sized( void * context, const uint8_t * connect_token,
                                                                                          const struct rocketnet_client_transport_config_t * transport_config, size_t transport_config_bytes,
                                                                                          const struct rocketnet_message_config_t * message_config, size_t message_config_bytes,
                                                                                          void * memory, uint64_t memory_bytes, uint32_t send_kbps );

// a NULL client is a counted no-op.
void rocketnet_message_client_destroy( struct rocketnet_message_client_t * client );

// the client's transport, for its stats and state; never its batches. a NULL client is a counted no-op that returns NULL.
struct rocketnet_client_transport_t * rocketnet_message_client_transport( struct rocketnet_message_client_t * client );

// ROCKETNET_CLIENT_TRANSPORT_STATE_*. a NULL client is a counted no-op that returns DISCONNECTED.
int rocketnet_message_client_state( struct rocketnet_message_client_t * client );

// ROCKETNET_UPDATE_OK, or ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. a NULL client is a counted no-op that returns ROCKETNET_UPDATE_OK.
int rocketnet_message_client_update( struct rocketnet_message_client_t * client, double time );

// a NULL client is a counted no-op.
/**
 * @brief Flushes packets staged through a message client.
 *
 * @param client Message client whose transport is flushed.
 * @return No value; delegates to the underlying transport.
 */
void rocketnet_message_client_send_packets( struct rocketnet_message_client_t * client );

// a NULL client is a counted no-op that returns NULL.
uint8_t * rocketnet_message_client_start( struct rocketnet_message_client_t * client, int channel, int reliability, int bytes );

// the receipt serial, nonzero; asserts in a debug build and returns 0 in a
// release build when no start is pending; returns 0 and aborts the pending
// start when bytes_written is outside 1 to the bytes started. a NULL client is
// a counted no-op that returns 0.
uint32_t rocketnet_message_client_finish( struct rocketnet_message_client_t * client, int bytes_written );

// a NULL client is a counted no-op.
void rocketnet_message_client_abort( struct rocketnet_message_client_t * client );

// The convenience over start and finish. It copies data into the region
// record: start, write in place and finish do not. a NULL client or data is a counted no-op that returns 0.
uint32_t rocketnet_message_client_send( struct rocketnet_message_client_t * client, int channel, int reliability, const uint8_t * data, int bytes );

// 1 with the next message of the update from the one receive queue, every
// channel's in delivery order (sdk/rocketnet_message_server.cpp,
// rn_message_object_next), the channel it was delivered on in *channel (NULL:
// not reported); 0 when there is none. a NULL client, data or bytes is a
// counted no-op that returns 0.
int rocketnet_message_client_receive( struct rocketnet_message_client_t * client, int * channel, const uint8_t ** data, int * bytes );

// The pumped batch form of the same queue: fills messages with up to
// ROCKETNET_RECEIVE_BATCH_MESSAGES next messages and returns how many; views
// valid until the next receive or update, which releases them. a NULL client
// or messages is a counted no-op that returns 0.
int rocketnet_message_client_receive_messages( struct rocketnet_message_client_t * client, struct rocketnet_message_batch_t * messages );

// Copies this update's receipt list into out, up to capacity entries; returns
// the available count. NULL out with zero capacity queries without copying.
// Repeated calls see the same list until the next update. Negative capacity
// or NULL out with positive capacity returns zero.
int rocketnet_message_client_receipts( struct rocketnet_message_client_t * client, struct rocketnet_message_receipt_t * out, int capacity );

// a NULL client or data is a counted no-op; a pointer that is not a held view is a counted misuse.
void rocketnet_message_client_release( struct rocketnet_message_client_t * client, const uint8_t * data );

// 1 when the budget took the new rate, from now on. a NULL client is a counted no-op that returns 0.
int rocketnet_message_client_set_send_kbps( struct rocketnet_message_client_t * client, int32_t kbps );

// ROCKETNET_DISCONNECT_REASON_* the client ended its connection with, NONE while it has not. a NULL client is a counted no-op that returns NONE.
int rocketnet_message_client_disconnect_reason( struct rocketnet_message_client_t * client );

// the message counters. a NULL client is a counted no-op that returns NULL.
const struct rocketnet_counters_t * rocketnet_message_client_counters( struct rocketnet_message_client_t * client );

// ---- server ----

// the bytes a caller block for a server object needs, at the transport config's max_clients; 0 for a refused config
/** @brief Measure caller-provided memory for a message server and its client slots.
 * @param[in] transport_config Server transport settings, or NULL for defaults.
 * @param[in] message_config Message settings, or NULL for defaults.
 * @return Required bytes for the server object and configured client slots, or 0 for refused settings or arithmetic overflow.
 * @details This query is allocation-free. Create requires at least this many bytes in caller-provided memory aligned to 64 bytes; the caller retains ownership of that region.
 */
uint64_t rocketnet_message_server_required_memory( const struct rocketnet_server_transport_config_t * transport_config, const struct rocketnet_message_config_t * message_config );

// a NULL address is a counted no-op that returns NULL. NULL configs are the defaults.
struct rocketnet_message_server_t * rocketnet_message_server_create( void * context, const char * address,
                                                                                    const struct rocketnet_server_transport_config_t * transport_config,
                                                                                    const struct rocketnet_message_config_t * message_config,
                                                                                    void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps );

// As rocketnet_message_server_create, with the section-2.1 code written to
// *status on every return path when status is not NULL: the transport's own
// code for a transport that does not start (rocketnet_server_transport_create_ex),
// CONFIG_OUT_OF_RANGE for a refused message config or budget, OUT_OF_MEMORY
// when the object's block is refused.
// reserve_unlimited preserves the public unlimited-budget runtime ceiling
// without sizing a finite payload reserve for that sentinel.
struct rocketnet_message_server_t * rocketnet_message_server_create_ex( void * context, const char * address,
                                                                        const struct rocketnet_server_transport_config_t * transport_config,
                                                                        const struct rocketnet_message_config_t * message_config,
                                                                        void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps, int * status, int reserve_unlimited );

// create with configs transport_config_bytes and message_config_bytes long: each
// known size is read over its defaults and created as above; an unknown size is
// refused with a log line and returns NULL before any other check. a NULL config
// is its defaults, whatever its size says.
/** @brief Create a message server using versioned transport and message configs.
 * @param[in] context Opaque callback context; may be NULL.
 * @param[in] address Public bind/advertise address; NULL is counted misuse and returns NULL.
 * @param[in] transport_config Server transport config prefix, or NULL for defaults.
 * @param[in] transport_config_bytes Size of that prefix in bytes; ignored when its config is NULL, otherwise must be a known revision.
 * @param[in] message_config Message config prefix, or NULL for defaults.
 * @param[in] message_config_bytes Size of that prefix in bytes; ignored when its config is NULL, otherwise must be a known revision.
 * @param[in] memory Caller-owned region, at least the required size and 64-byte aligned; NULL selects SDK allocation.
 * @param[in] memory_bytes Size of memory in bytes; must cover the required region when memory is supplied.
 * @param[in] per_client_send_kbps Per-client wire budget in kilobits per second: takes message_config server_send_kbps (the built-in default applies only when message_config is NULL; 0 or lower in that field disables the budget; a positive per_client_send_kbps argument overrides it for this create; above ROCKETNET_SEND_BUDGET_MAX_KBPS refuses; the environment override applies over both).
 * @return The server on success, or NULL for unknown config sizes, invalid arguments/configuration, an invalid budget, or insufficient memory.
 * @details The library must be initialized first. Unknown non-NULL config sizes are refused before create checks. Destroy the server to release its owned transport. Supplied memory remains caller-owned and must outlive the server.
 */
struct rocketnet_message_server_t * rocketnet_message_server_create_sized( void * context, const char * address,
                                                                                          const struct rocketnet_server_transport_config_t * transport_config, size_t transport_config_bytes,
                                                                                          const struct rocketnet_message_config_t * message_config, size_t message_config_bytes,
                                                                                          void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps );

// a NULL server is a counted no-op.
void rocketnet_message_server_destroy( struct rocketnet_message_server_t * server );

// the server's transport, for its port, stats and stop; never its batches. a NULL server is a counted no-op that returns NULL.
struct rocketnet_server_transport_t * rocketnet_message_server_transport( struct rocketnet_message_server_t * server );

// ROCKETNET_UPDATE_OK, or ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. a NULL server is a counted no-op that returns ROCKETNET_UPDATE_OK.
int rocketnet_message_server_update( struct rocketnet_message_server_t * server, double time );

// a NULL server is a counted no-op.
/**
 * @brief Flushes packets staged through a message server.
 *
 * @param server Message server whose transport is flushed.
 * @return No value; delegates to the underlying transport.
 */
void rocketnet_message_server_send_packets( struct rocketnet_message_server_t * server );

// a NULL server is a counted no-op that returns NULL.
uint8_t * rocketnet_message_server_start( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, int bytes );

// the receipt serial, nonzero; asserts in a debug build and returns 0 in a
// release build when no start is pending on client_index; returns 0 and aborts
// the pending start when bytes_written is outside 1 to the bytes started. a
// NULL server is a counted no-op that returns 0.
uint32_t rocketnet_message_server_finish( struct rocketnet_message_server_t * server, int client_index, int bytes_written );

// a NULL server is a counted no-op.
void rocketnet_message_server_abort( struct rocketnet_message_server_t * server, int client_index );

// The convenience over start and finish; it copies data into the region record. a NULL server or data is a counted no-op that returns 0.
uint32_t rocketnet_message_server_send( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, const uint8_t * data, int bytes );

// Sends data to every connected slot exactly as rocketnet_message_server_send
// would, one copy per connection: each connection's region holds its own copy
// (docs/CHANNELS.md section 11.4, Broadcast). Returns the number of slots it
// was queued for; a slot that refuses (exhausted, not connected) is skipped.
// a NULL server or data is a counted no-op that returns 0.
int rocketnet_message_server_broadcast( struct rocketnet_message_server_t * server, int channel, int reliability, const uint8_t * data, int bytes );

// 1 with the next message of the update from the one receive queue across
// every client slot, its sender's slot in *client_index and the channel it
// was delivered on in *channel (either NULL: not reported); 0 when there is
// none. a NULL server, data or bytes is a counted no-op that returns 0.
int rocketnet_message_server_receive( struct rocketnet_message_server_t * server, int * client_index, int * channel, const uint8_t ** data, int * bytes );

// The pumped batch form of the same queue, each entry naming its sender's
// slot. a NULL server or messages is a counted no-op that returns 0.
int rocketnet_message_server_receive_messages( struct rocketnet_message_server_t * server, struct rocketnet_message_batch_t * messages );

// As rocketnet_message_client_receipts for one connected client slot. Invalid inputs return zero.
int rocketnet_message_server_receipts( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_message_receipt_t * out, int capacity );

// a NULL server or data is a counted no-op; a pointer that is not a held view is a counted misuse.
void rocketnet_message_server_release( struct rocketnet_message_server_t * server, const uint8_t * data );

// a NULL server is a counted no-op that returns 0.
int rocketnet_message_server_set_send_kbps( struct rocketnet_message_server_t * server, int client_index, int32_t kbps );

// ROCKETNET_DISCONNECT_REASON_* the slot's last connection ended with, until the slot's next connection. a NULL server is a counted no-op that returns NONE.
int rocketnet_message_server_disconnect_reason( struct rocketnet_message_server_t * server, int client_index );

// a NULL server is a counted no-op that returns 0.
int rocketnet_message_server_client_connected( struct rocketnet_message_server_t * server, int client_index );

// a NULL server is a counted no-op that returns NULL.
const struct rocketnet_counters_t * rocketnet_message_server_counters( struct rocketnet_message_server_t * server );

// -----------------------------------------
//                           counters
// -----------------------------------------

// Every transport and message object keeps a block of monotonic counters,
// one per distinct reason a packet is refused, arrival counted separately
// from outcome. Read them by index through these calls: the count, the
// names and the classes are the library's, so a caller never assumes the
// block's length. Read on the main thread outside update/fill.

int rocketnet_num_counters( void );

// a NULL counters is a counted no-op that returns 0.
uint64_t rocketnet_counter_value( const struct rocketnet_counters_t * counters, int counter );

const char * rocketnet_counter_name( int counter );

// The class of a counter, one of the ROCKETNET_COUNTER_CLASS_ values, or -1 for
// an index outside the counter list. Allocation-free; callable before init.
int rocketnet_counter_class( int counter );

// "refusal", "expected", "path_loss" or "arrival"; NULL for a value that is not a class.
const char * rocketnet_counter_class_name( int counter_class );

// 1 when every counter family balances: for each RECEIVED counter, its value
// equals the sum of the outcomes that name it as their family. Otherwise 0,
// with the first unbalanced family (in counter order) written to report as
// "<RECEIVED name> received=<n> outcomes=<m>"; report may be NULL. Read the
// block at a quiescent point, on the thread that owns it: after an update
// returns, when every packet counted as arrived has taken its outcome. A NULL
// counters is a counted no-op that returns 0.
int rocketnet_counters_conserved( const struct rocketnet_counters_t * counters, char * report, int report_bytes );

// The SDK's own tests and tools read one stat in an expression: each returns
// the public stats call's struct by value.
static inline struct rocketnet_client_stats_t rn_client_stats_of( const struct rocketnet_client_t * client )
{
    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( client, &stats );
    return stats;
}

static inline struct rocketnet_server_stats_t rn_server_stats_of( const struct rocketnet_server_t * server )
{
    struct rocketnet_server_stats_t stats;
    rocketnet_server_stats( server, &stats );
    return stats;
}

static inline struct rocketnet_server_client_stats_t rn_server_client_stats_of( const struct rocketnet_server_t * server, int client_index )
{
    struct rocketnet_server_client_stats_t stats;
    rocketnet_server_client_stats( server, client_index, &stats );
    return stats;
}

#if ROCKETNET_DEV

// -----------------------------------------
//                  delivery capture (dev only)
// -----------------------------------------

// One connection's delivery state, read for diagnosis by the load tools
// (load_split.c, tools/load_split_capture.h). Development builds only: the
// production library has neither the structs' readers nor their symbols, and
// nothing here is public API. Read on the main thread outside update/fill;
// every value is a plain copy of state the SDK already keeps, and a read
// changes nothing. An empty slot, a disconnected client or a NULL argument
// reads as all zeros with connected 0.
//
// Sequences are transport session sequences: received is the highest the
// replay window has opened from the other end, sent is the highest this end
// has sealed, acked is the highest of this end's sends the other end has
// acknowledged. Ages are seconds at each layer's last update, on that layer's
// own clock (a client transport's clock starts at its first update; the
// message layer reads game time); -1 means never or none. The reliable fields
// are the ordered path's window (docs/CHANNELS.md 6.2, 6.4): unacked =
// next - U, and the oldest unacknowledged age is that of U's first send.

struct rocketnet_delivery_capture_t
{
    int connected;
    int server_mapping_known;              // server slot and remote endpoint are populated
    int server_slot;
    char remote_endpoint[ROCKETNET_MAX_ADDRESS_STRING_LENGTH]; // numeric address and UDP port
    int keep_alive_acked;                   // the other end has acknowledged one of this end's keep-alives
    double last_receive_age_seconds;        // since the last accepted session packet; -1 never
    uint64_t highest_received_sequence;
    uint64_t highest_sent_sequence;
    uint64_t largest_acked_sequence;
    uint64_t reliable_unacked;              // message numbers sent and not yet acknowledged
    uint64_t reliable_resend_queued;        // numbers linked in the resend FIFOs
    double reliable_oldest_unacked_age_seconds;    // since U's first send; -1 when nothing is outstanding
    double srtt_seconds;
    double resend_delay_seconds;
    int window_stalled;                     // the sender's window is full
    int budget_reliable_waiting;            // this update a reliable item did not fit the send budget
    double budget_tokens;                   // bytes the send budget allows now; 0 when uncapped
};

// One server slot. client_index is the public slot index.
void rocketnet_server_delivery_capture( struct rocketnet_server_t * server, int client_index, struct rocketnet_delivery_capture_t * capture );

// The client's current connection.
void rocketnet_client_delivery_capture( struct rocketnet_client_t * client, struct rocketnet_delivery_capture_t * capture );

// The transport and message counter blocks behind a public server or client,
// NULL when it holds none. Read through rocketnet_counter_value.
const struct rocketnet_counters_t * rocketnet_server_transport_counter_block( struct rocketnet_server_t * server );
const struct rocketnet_counters_t * rocketnet_server_message_counter_block( struct rocketnet_server_t * server );
const struct rocketnet_counters_t * rocketnet_client_transport_counter_block( struct rocketnet_client_t * client );
const struct rocketnet_counters_t * rocketnet_client_message_counter_block( struct rocketnet_client_t * client );

// The layers' halves of the reads above: the transports fill the connection,
// sequence and receive fields, the message objects the reliable and budget
// fields. Each writes only its own fields.
void rocketnet_server_transport_delivery_capture( struct rocketnet_server_transport_t * server_transport, int client_index, struct rocketnet_delivery_capture_t * capture );
void rocketnet_client_transport_delivery_capture( struct rocketnet_client_transport_t * client_transport, struct rocketnet_delivery_capture_t * capture );
void rocketnet_message_server_delivery_capture( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_delivery_capture_t * capture );
void rocketnet_message_client_delivery_capture( struct rocketnet_message_client_t * client, struct rocketnet_delivery_capture_t * capture );

#endif // #if ROCKETNET_DEV


void rocketnet_client_transport_defer_payload_acks( struct rocketnet_client_transport_t * transport );
void rocketnet_client_transport_accept_payload( struct rocketnet_client_transport_t * transport, int k );

void rocketnet_server_transport_defer_payload_acks( struct rocketnet_server_transport_t * transport );
void rocketnet_server_transport_accept_payload( struct rocketnet_server_transport_t * transport, int k );

#ifdef __cplusplus
}
#endif

#endif // #ifndef ROCKETNET_LAYERS_H
