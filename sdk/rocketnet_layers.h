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

#define ROCKETNET_MAX_ADDRESS_STRING_LENGTH                256

/** @brief Address-family value for no address family. */
#define ROCKETNET_ADDRESS_NONE                             0
/** @brief Address-family value for IPv4. */
#define ROCKETNET_ADDRESS_IPV4                             1
/** @brief Address-family value for IPv6. */
#define ROCKETNET_ADDRESS_IPV6                             2

/** @brief Sets the storage capacity for a transport driver candidate list.
 *
 * The value includes the terminating NUL byte; a configured string must fit
 * within this many bytes.
 */
#define ROCKETNET_DRIVER_STRING_LENGTH                     64

#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED         -7
#define ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN         -6
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT          -5
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT -4
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT  -3
#define ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT      -2
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED             -1
#define ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED                   0
#define ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE              1
#define ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST     2
#define ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE    3
#define ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED                      4

/** @brief Server transport state while its receive and update work is active. */
#define ROCKETNET_SERVER_TRANSPORT_RUNNING                              0
/** @brief Server transport state while shutdown is in progress. */
#define ROCKETNET_SERVER_TRANSPORT_STOPPING                             1
/** @brief Server transport state after shutdown completes. */
#define ROCKETNET_SERVER_TRANSPORT_STOPPED                              2

// what an admission callback returns (rocketnet_server_admission_callback_t)
/** @brief Admission callback result that allows an unmapped source packet to proceed. */
#define ROCKETNET_SERVER_ADMISSION_ADMIT                                0
/** @brief Admission callback result that drops an unmapped source packet. */
#define ROCKETNET_SERVER_ADMISSION_DROP                                 1

/** @brief Maximum number of explanatory exclusions in a host-memory estimate. */
#define ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS                            8

#define ROCKETNET_PRIORITY_IMMEDIATE                                    0
#define ROCKETNET_PRIORITY_HIGH                                         1
#define ROCKETNET_PRIORITY_MEDIUM                                       2
#define ROCKETNET_PRIORITY_LOW                                          3

// the block stride, a wire constant

#define ROCKETNET_MESSAGE_FRAGMENT_BYTES                             1152

// The reliable resend schedule (docs/CHANNELS.md 6.4). Every resend of a
// record waits min( rto, ROCKETNET_MESSAGE_RESEND_INTERVAL_MS ), with no
// back-off, and rto = clamp( srtt x ROCKETNET_MESSAGE_RTO_SCALE_PERCENT / 100
// + 2 rttvar, rto_min_ms, rto_max_ms ). Validation refuses reliable_timeout_ms
// below ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS times
// max( ROCKETNET_MESSAGE_RESEND_INTERVAL_MS, rto_initial_ms ), the sends that
// must fit before reliable_timeout gives up on a record.

#define ROCKETNET_MESSAGE_RESEND_INTERVAL_MS                         100
#define ROCKETNET_MESSAGE_RTO_SCALE_PERCENT                          125
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

struct rocketnet_client_transport_t;
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
void rocketnet_printf( int level, const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_INFO. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
void rocketnet_info( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_WARN. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
void rocketnet_warn( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_ERROR. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
void rocketnet_error( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_DEBUG. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
void rocketnet_debug( const char * format, ... );

/** @brief Emits a message at ROCKETNET_LOG_LEVEL_SPAM. @param format Printf format; NULL is a counted no-op. The remaining arguments must match format specifiers. */
void rocketnet_spam( const char * format, ... );

/** @brief Formats a user identifier as lowercase hexadecimal. @param user_id Identifier to format. @param buffer Caller-owned output buffer; NULL is a counted no-op returning NULL. @param buffer_size Buffer capacity in bytes; zero writes no bytes. @return buffer, or NULL when buffer is NULL. A nonempty buffer is NUL-terminated and may contain a truncated string. */
const char * rocketnet_user_id_string( uint64_t user_id, char * buffer, size_t buffer_size );

/** @brief Returns a cryptographically generated float in the inclusive range [0, 1]. @return Random value. */
float rocketnet_random_float( void );

/** @brief Returns a cryptographically generated 8-bit value. @return Value in [0, 255]. */
uint8_t rocketnet_random_uint8( void );

/** @brief Returns a cryptographically generated 16-bit value. @return Value in [0, 65535]. */
uint16_t rocketnet_random_uint16( void );

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

/** @brief Clears a buffer before releasing it through rocketnet_free. @param context Opaque allocator context; allocation metadata selects the original deallocator. @param p Pointer to release; NULL is a no-op. @param p_size Number of bytes to clear. The caller ensures p_size does not exceed the allocation. */
void rocketnet_clear_and_free( void * context, void * p, size_t p_size );

// -----------------------------------------
//                   transport config
// -----------------------------------------

// Each transport's config is a plain struct. Precedence at create is
// DEFAULT < CONFIG STRUCT < ENVIRONMENT: the app ships what it means in code,
// and an operator standing at an already-built binary overrides any field with
// an environment variable, no rebuild -- the ROCKETNET_ prefix plus the
// endpoint and field name uppercased (cfg.driver -> ROCKETNET_SERVER_TRANSPORT or
// ROCKETNET_CLIENT_TRANSPORT). Every env override is logged loudly at create with
// the variable name and the value it displaced (secret fields log redacted).
// Validation refuses loudly at create on any out-of-range value -- never
// clamps, never silently corrects.
//
// Env encodings: integers are decimal; byte fields are base64 of exactly
// the field's length (keygen prints keys in base64); the driver field is
// the ordered candidate list below. An env var set to the empty string is
// ignored as if unset, with a log line. The pointer fields (the client's
// callbacks) have no env form.
//
// The driver field: an ORDERED CANDIDATE LIST, comma separated, tried left
// to right at create -- first grant wins, and a list exhausted by REFUSAL
// fails loudly. "auto" (the default, also the empty string) expands to
// this platform's recommended order for the endpoint: linux server
// xdp,gso,portable; macOS msgx,portable; everywhere else portable
// (clients: portable everywhere, plus msgx on macOS). A one-element list
// is a pin: that driver or loud failure, never a silent fall. A name in
// the known transport set (xdp, gso, msgx, portable) but absent on this
// platform -- or not applicable to this endpoint, like xdp for a client --
// is IGNORED with one soft log line, so one fleet-wide config line deploys
// unmodified to heterogeneous boxes; a list whose EVERY name is ignored
// names no driver for this endpoint at all and falls to the endpoint's own
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

const char * rocketnet_client_transport_state_name( int state );

// The client connects with a token, never an address: the token names the
// server, carries the keys, and is the credential -- minted by a backend in
// production, or locally by rocketnet_write_dev_connect_token against the
// published development keys. The config carries what the token does not:
// the per-title filter value, the fragmentation shape -- the largest
// payload this client sends and reassembles, and the per-connection
// reassembly slot count, zero meaning the defaults above; the client's
// delivered mirror sizes itself to the client's own drain bound, about
// 8.4MB at the default maximum -- the driver candidate list, and an
// optional state-change callback. NULL config is the development posture:
// the published test filter, the defaults, no callback.

struct rocketnet_client_transport_config_t
{
    uint8_t filter_value[4];
    int max_payload_bytes;                          // ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    int num_reassembly_slots;                       // 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    char driver[ROCKETNET_DRIVER_STRING_LENGTH];    // the candidate list; default "auto"
    void (*state_change_callback)( void * context, int previous_state, int current_state );
    double (*time_callback)( void * context );      // the receive stamp clock, read once at create; NULL is the platform clock
    uint64_t game_clock_epoch;                      // unix seconds at the game clock's zero for the token check; 0 reads the wall clock
    uint64_t step_driven;                           // 1: the caller's update and send_packets run the I/O step, no I/O thread; 0: today's thread
};

// a NULL config is a counted no-op.
void rocketnet_client_transport_default_config( struct rocketnet_client_transport_config_t * config );

// Allocation-free constructor memory plan for one built-in driver name.
// Includes SDK allocator requests (alignment/ownership overhead). Excludes OS
// stacks, kernel/libc resources and application memory; this is not a host RAM
// limit. Uses the supplied numeric config exactly (NULL means defaults); no
// environment overlay, key validation, socket probe or budget refusal. Zero
// means invalid geometry, overflow, unknown/custom driver, or driver not built
// for this platform. Query each candidate separately: "auto" and driver lists
// are not concrete plans.
uint64_t rocketnet_client_transport_required_memory( const struct rocketnet_client_transport_config_t * config, const char * driver );

// the defaults for a caller whose struct is config_bytes long, a known size of
// the struct. 1 written; 0 refused, nothing written. a NULL config is a
// counted no-op that returns 0.
int rocketnet_client_transport_default_config_sized( struct rocketnet_client_transport_config_t * config, size_t config_bytes );


// time is the game's clock, a double in seconds, monotonic; pass the same
// clock to every call that takes one. The library never reads a clock of
// its own on these paths, so a game owns time and a test can hold it still.

// a NULL connect_token is a counted no-op that returns NULL; a NULL config is
// the default shape.
struct rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time );

// The _ex form: same contract, and when status is not NULL it is written with
// ROCKETNET_STATUS_OK on success and the section-2.1 code of the refusal on
// every failure exit. The non-_ex form is a thin wrapper passing NULL.
struct rocketnet_client_transport_t * rocketnet_client_transport_create_ex( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status );

// create with a config config_bytes long: a known size is read over the
// defaults and created as above; an unknown size is refused with a log line and
// returns NULL before any other check. a NULL config is the default shape,
// whatever config_bytes says.
struct rocketnet_client_transport_t * rocketnet_client_transport_create_sized( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, size_t config_bytes, double time );

// a NULL client_transport is a counted no-op.
void rocketnet_client_transport_destroy( struct rocketnet_client_transport_t * client_transport );      // disconnect and pump a few updates first

// a NULL client_transport is a counted no-op.
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

// the configured maximum payload this transport sends and reassembles --
// the config's max_payload_bytes after defaulting -- so a caller can size
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
/**
 * @brief Seals and stages bytes written into a client packet lease.
 *
 * @param client_transport Client transport that created the lease; NULL is a counted no-op.
 * @param context Active, unmodified lease; this call consumes it.
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
int rocketnet_client_transport_state( struct rocketnet_client_transport_t * client_transport );

// the granted driver, request-vs-granted style: the name of the candidate
// that won the ladder at create ("portable", "msgx"), never the requested
// list. any performance claim attaches to what this returns.

// a NULL client_transport is a counted no-op that returns NULL.
/**
 * @brief Returns the client driver granted at creation.
 *
 * @param client_transport Client transport; NULL is a counted no-op returning NULL.
 * @return Transport-owned granted driver name, valid until destroy, or NULL.
 */
const char * rocketnet_client_transport_driver( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op that returns 0.
int rocketnet_client_transport_client_index( struct rocketnet_client_transport_t * client_transport );

// a NULL client_transport is a counted no-op.
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

// The admission gate: a callback the application installs on a server
// transport, consulted for every received packet whose source has no
// encryption mapping -- traffic that has not cleared the connect handshake --
// before it reaches the protocol's process step. Return
// ROCKETNET_SERVER_ADMISSION_ADMIT to process the packet, or
// ROCKETNET_SERVER_ADMISSION_DROP to drop it before any crypto work runs.
// A mapped source (an in-flight handshake or an established connection)
// bypasses the gate: it has already cleared the connect ladder, and this gate
// is about admission, not session policing. The callback runs on the receive
// thread and must be fast and thread-safe.

// The packet source an admission callback reads: type is ROCKETNET_ADDRESS_IPV4
// or ROCKETNET_ADDRESS_IPV6; an IPv4 address is data.ipv4[0..3] in dotted
// order (a.b.c.d), an IPv6 address is data.ipv6[0..7], the eight groups in
// host byte order; port is in host byte order.

struct rocketnet_address_t
{
    union { uint32_t ip; uint8_t ipv4[4]; uint16_t ipv6[8]; } data;
    uint16_t port;
    uint8_t type;
};

/** @brief Filters packets from sources without an encryption mapping. @param context Application context supplied at registration. @param from Borrowed source address, valid during the callback. @param packet_data Borrowed packet bytes, valid during the callback. @param packet_bytes Packet length in bytes. @return ROCKETNET_SERVER_ADMISSION_ADMIT or ROCKETNET_SERVER_ADMISSION_DROP. Runs on the receive thread and must be fast and thread-safe; mapped sources bypass it. */
typedef int (*rocketnet_server_admission_callback_t)( void * context, const struct rocketnet_address_t * from, const uint8_t * packet_data, int packet_bytes );

// The server is configured explicitly. rocketnet_server_transport_default_config gives
// the shape with no keys -- create refuses it until keys are supplied
// (through the struct or through the env surface), because default keys are
// how every deployment of a protocol ends up sharing one identity.
// rocketnet_server_transport_dev_config fills the published
// development keys and test filter for local work, loudly. NULL config is
// the default shape. num_xdp_queues is the staged drivers' queue count -- XDP
// queues on the xdp driver, reuseport sockets on the gso driver -- kept under
// its historic name for ABI stability with the language bindings; num_os_cpus
// configures the XDP driver's core layout. zero means the driver default, and
// the portable driver ignores both. The tighter num_xdp_queues bounds are per
// driver and refused loudly at create, never clamped: the xdp driver takes any
// count from 1, the gso driver refuses past 32.
//
// The fragmentation shape rides max_payload_bytes and
// num_reassembly_slots: the largest payload this server sends and
// reassembles, and the per-client reassembly slot count. zero means the
// defaults. budget arithmetic, all spent once at create, approximately --
// slot records ride along: the reassembly arena is max_clients x slots x
// (max payload + 1433 staging) per the standard; the delivered mirror of
// slots x max payload per client -- the per-update home of completed
// payloads; and the ack endpoints' fragment aggregate rings, about 48KB per
// client slot.
//
// public_address, empty by default, displaces the create call's address
// parameter when set -- struct or ROCKETNET_PUBLIC_ADDRESS (deprecated alias:
// ROCKETNET_SERVER_PUBLIC_ADDRESS) -- so the address the driver ladder
// verifies, the address the driver binds, and the address connect tokens
// validate against are always the same one. the ENV form is scoped to the
// first server create in the process and then consumed, loudly: the
// deployed binary's one server gets its redirect, and a multi-transport
// process is never wholesale redirected by one ambient variable. the
// struct form is per create.
//
// The handshake deadline is fixed at the challenge, never refreshed, and
// discharged on connect.

struct rocketnet_server_transport_config_t
{
    uint64_t protocol_id;
    int max_clients;                                // 1 .. ROCKETNET_MAX_CLIENTS; default 1000
    uint8_t filter_value[4];
    uint8_t backend_public_key[32];
    uint8_t server_public_key[32];
    uint8_t server_secret_key[32];                  // secret: logged redacted
    int num_xdp_queues;                             // 1 .. 1024; 0 is the driver default
    int num_os_cpus;                                // 1 .. 256; 0 is the driver default
    int max_payload_bytes;                          // ROCKETNET_MAX_PAYLOAD_BYTES .. 305,408; 0 is ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES
    int num_reassembly_slots;                       // 1 .. 256; 0 is ROCKETNET_DEFAULT_REASSEMBLY_SLOTS
    char driver[ROCKETNET_DRIVER_STRING_LENGTH];    // the candidate list; default "auto"
    char public_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    int max_timeout_seconds;                        // 1 .. 3600; default 60
    int handshake_deadline_seconds;                 // 1 .. 60; default 5
    uint64_t game_clock_epoch;                      // unix seconds at the game clock's zero for the token check; 0 reads the wall clock
};

// a NULL config is a counted no-op.
void rocketnet_server_transport_default_config( struct rocketnet_server_transport_config_t * config );

// the defaults for a caller whose struct is config_bytes long, a known size of
// the struct. 1 written; 0 refused, nothing written. a NULL config is a
// counted no-op that returns 0.
int rocketnet_server_transport_default_config_sized( struct rocketnet_server_transport_config_t * config, size_t config_bytes );

// a NULL config is a counted no-op.
void rocketnet_server_transport_dev_config( struct rocketnet_server_transport_config_t * config );

// Allocation-free constructor memory plan for one built-in driver name.
// Includes SDK allocator requests (alignment/ownership overhead and transient
// worker shims) plus XDP UMEM. Excludes OS stacks, kernel/libbpf/libxdp resources,
// loader/libc bookkeeping and application memory; this is not a host RAM limit.
// Uses the supplied numeric config exactly (NULL means defaults); no environment
// overlay, key validation, socket probe or budget refusal. Zero means invalid
// geometry, overflow, unknown/custom driver, or driver not built for this platform.
// Query each candidate separately: "auto" and driver lists are not concrete plans.
uint64_t rocketnet_server_transport_required_memory( const struct rocketnet_server_transport_config_t * config, const char * driver );

// Allocation-free whole-host memory estimate for one built-in driver name: the
// required_memory plan plus the kernel-facing memory the driver requests. Each
// component is a request, never a grant: allocator_bytes is the SDK allocator
// plan (required_memory minus UMEM); umem_bytes is XDP UMEM, the frame memory,
// equal to the plan's; the socket buffer fields sum the SO_SNDBUF and SO_RCVBUF
// sizes asked of every socket the driver opens; xdp_ring_bytes is the AF_XDP
// RX and TX descriptor rings asked of every XDP socket; thread_stack_bytes is
// worker_threads x thread_stack_bytes_per_thread, the stack each driver thread
// is created with (the platform default: the driver passes no stack size), as
// reserved address space. total_bytes is the sum of the six byte components
// and is at least required_memory. exclusions names, in static strings, what
// the library cannot know and does not count. Same config and driver rules as
// required_memory: NULL config means defaults; false (and a zeroed estimate)
// means no plan. A NULL estimate is a counted misuse that returns false.

struct rocketnet_host_memory_estimate_t
{
    uint64_t allocator_bytes;
    uint64_t umem_bytes;
    uint64_t socket_send_buffer_bytes;
    uint64_t socket_receive_buffer_bytes;
    uint64_t xdp_ring_bytes;
    uint64_t thread_stack_bytes;
    uint64_t thread_stack_bytes_per_thread;
    uint64_t total_bytes;
    int sockets;
    int worker_threads;
    int num_exclusions;
    const char * exclusions[ROCKETNET_HOST_MEMORY_MAX_EXCLUSIONS];
};

bool rocketnet_server_transport_host_memory_estimate( const struct rocketnet_server_transport_config_t * config, const char * driver, struct rocketnet_host_memory_estimate_t * estimate );

// a NULL public_address_string is a counted no-op that returns NULL; a NULL
// config is the default shape.
struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config );

// As rocketnet_server_transport_create, with the exact section-2.1 code
// written to *status on every return path when status is not NULL
// (ROCKETNET_STATUS_OK on success, a non-zero ROCKETNET_STATUS_* on failure).
// A failure also logs one rocketnet_error line; the non-_ex form passes NULL.
struct rocketnet_server_transport_t * rocketnet_server_transport_create_ex( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config, double time, int * status );

// create with a config config_bytes long: a known size is read over the
// defaults and created as above; an unknown size is refused with a log line and
// returns NULL before any other check. a NULL config is the default shape,
// whatever config_bytes says.
struct rocketnet_server_transport_t * rocketnet_server_transport_create_sized( void * context, const char * public_address_string, const struct rocketnet_server_transport_config_t * config, size_t config_bytes );

// a NULL server_transport is a counted no-op.
void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport );      // stop and wait for stopped first

// a NULL server_transport is a counted no-op.
void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport is a counted no-op that returns 0.
int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport );

// The port the transport actually bound. A public address string carrying
// port zero asks the kernel for an ephemeral port at create; the granted
// port is adopted into the advertised address -- so connect tokens minted
// for it validate -- and this reads it back. Tests and single-box tools
// build their token address from it instead of hardcoding a port;
// production servers know their public address already.
//
// Port-zero discovery is a per-driver property: a driver whose create binds
// the port it is given and discovers nothing -- the XDP driver -- refuses a
// port-zero address at create, loudly, before it binds anything. An "auto"
// ladder falls past it to a driver that does discover, and only a pin on a
// non-discovering driver refuses the create outright. The portable, msgx and
// GSO drivers discover; the XDP driver does not. So this returns either the
// port the public address carried, or the port a discovering driver was
// granted.

// a NULL server_transport is a counted no-op that returns 0.
uint16_t rocketnet_server_transport_port( struct rocketnet_server_transport_t * server_transport );

// the granted driver, request-vs-granted style: the name of the candidate
// that won the ladder at create ("xdp", "gso", "msgx", "portable"), never
// the requested list. the 1000s-of-players claims attach to the granted
// tier this returns, not the requested one -- one log line at create names
// the same grant and why every earlier candidate refused.

// a NULL server_transport is a counted no-op that returns NULL.
/**
 * @brief Returns the server driver granted at creation.
 *
 * @param server_transport Server transport; NULL is a counted no-op returning NULL.
 * @return Transport-owned granted driver name, valid until destroy, or NULL.
 */
const char * rocketnet_server_transport_driver( struct rocketnet_server_transport_t * server_transport );

// the commit barrier for host processes that must never commit an XDP data
// plane (test harnesses, smokes): installed once, the xdp driver's create
// refuses before its first act; the non-mutating pre-flight stays
// reachable. a no-op on platforms without the xdp driver.

/**
 * @brief Prevents XDP drivers from committing a data plane in this process.
 *
 * @return No value. Install before XDP server creation; XDP create refuses before commit while preflight remains available.
 */
void rocketnet_forbid_xdp_commit( void );

// a NULL server_transport is a counted no-op.
void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport, double time );

// Unit tests only: supplies token-validation Unix time so update reads no wall clock.
void rocketnet_server_transport_set_test_timestamp( struct rocketnet_server_transport_t * server_transport, uint64_t timestamp );

// The admission gate, set per transport (see the callback type above). NULL
// callback -- the default -- admits every source: the connect ladder alone
// decides. May be set or cleared at any time between updates.

// a NULL server_transport is a counted no-op.
void rocketnet_server_transport_set_admission_callback( struct rocketnet_server_transport_t * server_transport, rocketnet_server_admission_callback_t callback, void * context );

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
// declared with it. A request at or under ROCKETNET_MAX_PAYLOAD_BYTES is
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
// driver, and a caller owns only the guarantee: everything staged before
// send_packets is sent by it, and nothing is guaranteed sent without it.
// Code against the guarantee, not a driver's eagerness. Calling
// send_packets again without newly staged packets is a no-op. The fields of the
// context are read-only to the caller. The transport privately owns and checks
// all metadata; a copy can be consumed once, and stale, modified or wrong-owner
// contexts are refused before dereferencing a frame. packet_id is an opaque,
// process-unique lease identifier. Active payload views expire at
// finish/abort or the next update/flush; raw pointers cannot be revoked.
//
// finish_packet returns the sealed wire size, or zero when the packet was
// dropped instead (payload out of range, the connection gone, or a wire
// frame it could not reserve) -- the sequences already taken are burned
// either way, which the receiver's replay window tolerates. An unfragmented
// payload burns one sequence; a fragmented one burns up to its fragment
// count (14 at the default 16384-byte maximum payload), so a failed
// fragmented send moves the receive-side loss figure by up to that count.
// USAGE.md carries the worked arithmetic.

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
    /** @brief Session sequence reserved for the first wire packet. */
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
/**
 * @brief Seals and stages bytes written into a server packet lease.
 *
 * @param server_transport Server transport that created the lease; NULL is a counted no-op.
 * @param context Active, unmodified lease; this call consumes it.
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
// burst is staged and the slot frees immediately. Buffered drivers may send
// full batches during this call; remaining packets leave on the next flush.
// Why is the game's decision; this is only the mechanism. Main thread only, never
// while a fill phase is open -- it tears down the connection state the
// fill relies on.

// a NULL server_transport is a counted no-op.
void rocketnet_server_transport_disconnect_client( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns 0.
int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport );

// a NULL server_transport is a counted no-op that returns 0.
int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns 0.
uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index );

// The connection that holds the slot now: nonzero and unique for each
// connection over the transport's life, 0 while the slot is free (and for an
// index out of range). A reconnect into the same slot, even inside one
// update, has a new id; compare it with the per-entry ids of the payload and
// acked batches to tell whose an entry is.

// a NULL server_transport is a counted no-op that returns 0.
uint64_t rocketnet_server_transport_client_connection_id( struct rocketnet_server_transport_t * server_transport, int client_index );

// a NULL server_transport is a counted no-op that returns NULL.
const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport );

// the configured maximum payload this transport sends and reassembles --
// the config's max_payload_bytes after defaulting -- so a caller can size
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
// Fed by the ack blocks every session packet carries; recomputed round-robin,
// a sixth of the slots per update, every slot covered, so the per-update
// cost is flat at any client count. The cumulative totals count every wire
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
    uint32_t status;            // ROCKETNET_RECEIPT_ACKED or ROCKETNET_RECEIPT_LOSS
};

// The message config. Fields marked WIRE must be identical on both ends; the
// connection verifies them. Validation applies the ranges here and the cross
// rules of docs/CHANNELS.md section 14, and refuses, never clamps.

struct rocketnet_message_config_t
{
    int num_channels;                   // WIRE; 1 .. ROCKETNET_MAX_CHANNELS; default 32
    int window_messages;                // WIRE, power of two; 64 .. 1 << 20; default 1024
    int receive_ring_bytes;             // WIRE, multiple of 16; 4096 .. 1 << 20; default 262144
    int max_message_bytes;              // WIRE, <= receive_ring_bytes; ROCKETNET_MESSAGE_SINGLE_BYTES .. 1 << 20; default 262144
    int send_buffer_bytes;              // 12288 .. 1 << 28; default 800000
    int send_queue_messages;            // 16 .. 1 << 20; default 1024
    int packets_in_flight;              // 16 .. 1 << 16; default 256
    int max_packets_per_update;         // 1 .. 256; default 4
    int unreliable_deliveries;          // per connection per update; 1 .. 1 << 16; default 256
    int sequenced_holds;                // 0 .. 4096; default 16
    int receipt_ring;                   // 16 .. 1 << 16; default 256
    int rto_initial_ms;                 // 10 .. 60000; default 100
    int rto_min_ms;                     // 1 .. 60000; default 10
    int rto_max_ms;                     // 10 .. 60000; default 250
    int reliable_timeout_ms;            // 1000 .. 600000; default 10000
    int client_send_kbps;                // client default budget; 0 or lower uncapped; <= ROCKETNET_SEND_BUDGET_MAX_KBPS
    int server_send_kbps;                // server per-client default budget; 0 or lower uncapped; <= ROCKETNET_SEND_BUDGET_MAX_KBPS
};

// a NULL config is a counted no-op.
void rocketnet_message_default_config( struct rocketnet_message_config_t * config );

// the defaults for a caller whose struct is config_bytes long, a known size of
// the struct. 1 written; 0 refused, nothing written. a NULL config is a
// counted no-op that returns 0.
int rocketnet_message_default_config_sized( struct rocketnet_message_config_t * config, size_t config_bytes );

// 1 when the config is accepted, else 0 with a log line naming the rule.
// NULL means the defaults. Allocation-free; safe before init.
int rocketnet_message_validate_config( const struct rocketnet_message_config_t * config );

// As rocketnet_message_validate_config, with the section-2.1 code written to
// *status on every return path when status is not NULL (CONFIG_OUT_OF_RANGE on
// refusal). A refusal logs one error line.
int rocketnet_message_validate_config_ex( const struct rocketnet_message_config_t * config, int * status );

// The exact bytes of one connection's region (docs/CHANNELS.md 14): the
// formula plus the 64-byte alignment of each part. 0 for a refused config;
// NULL means the defaults. Allocation-free; callable before init or connect.
// A server holds one region per client slot, a client one.
uint64_t rocketnet_message_connection_bytes( const struct rocketnet_message_config_t * config );

// The formula, each term at this config, the padding, the region and the
// total for num_connections regions, as text into buffer. Returns the full
// length (as snprintf: a short buffer is truncated and terminated, and NULL
// with 0 measures), or 0 for a refused config. Allocation-free.
int rocketnet_message_connection_sizing( const struct rocketnet_message_config_t * config, int num_connections, char * buffer, int buffer_size );

// The same lines, logged at info level. Returns the region bytes, or 0 refused.
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
// pending per object at a time. The enabled types are ROCKETNET_UNRELIABLE,
// ROCKETNET_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABLE, ROCKETNET_RELIABLE_ORDERED,
// ROCKETNET_RELIABLE_SEQUENCED, ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT,
// ROCKETNET_RELIABLE_WITH_ACK_RECEIPT, and ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
// a type a caller's own mask leaves out is refused (counted
// MESSAGE_SEND_TYPE_NOT_ENABLED). Plain
// UNRELIABLE is one record in one packet, up to 1411 bytes. UNRELIABLE_SEQUENCED
// can use the transport's fragments for one larger unreliable RECORDS payload.
// Receipt-bearing UNRELIABLE uses the code-0 wire form at single-packet size and promotes a larger
// message to a reliable code-2 block with an ACKED-only receipt. Both are
// received on channel 0. The reliable types and promoted blocks are bounded
// by the config's max_message_bytes.
//
// A start the connection's region has no room for ends the connection:
// the server disconnects that client, the client disconnects itself, with the
// reason ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED, and start returns
// NULL. The send budget governs the wire, never the queue: a reliable message
// waits for budget and is never dropped; an unreliable one over budget is
// dropped and counted (MESSAGE_UNRELIABLE_OVER_BUDGET).
//
// Receiving: receive hands out the next message of the update on a channel as
// a const view and its length, valid until the next update. Every received
// message is released before the next update; update with one still held
// asserts in a debug build and returns ROCKETNET_ERROR_MESSAGE_NOT_RELEASED in
// a release build. Copy what is kept past the update.

// ---- client ----

// the bytes a caller block for a client object needs; 0 for a refused config
uint64_t rocketnet_message_client_required_memory( const struct rocketnet_message_config_t * message_config );

// a NULL connect_token is a counted no-op that returns NULL. NULL configs are the defaults.
// create reads no clock: the connect attempt starts at the first update, at whatever time the game passes it, and every time comes in through update.
struct rocketnet_message_client_t * rocketnet_message_client_create( void * context, const uint8_t * connect_token,
                                                                                    const struct rocketnet_client_transport_config_t * transport_config,
                                                                                    const struct rocketnet_message_config_t * message_config,
                                                                                    void * memory, uint64_t memory_bytes, uint32_t send_kbps );

// create with configs transport_config_bytes and message_config_bytes long: each
// known size is read over its defaults and created as above; an unknown size is
// refused with a log line and returns NULL before any other check. a NULL config
// is its defaults, whatever its size says.
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

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL client is a counted no-op that returns ROCKETNET_MESSAGE_OK.
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

// the receipt serial, nonzero; 0 when no start is pending or bytes_written is outside 1 to the bytes started. a NULL client is a counted no-op that returns 0.
uint32_t rocketnet_message_client_finish( struct rocketnet_message_client_t * client, int bytes_written );

// a NULL client is a counted no-op.
void rocketnet_message_client_abort( struct rocketnet_message_client_t * client );

// The convenience over start and finish. It copies data into the region
// record: start, write in place and finish do not. a NULL client or data is a counted no-op that returns 0.
uint32_t rocketnet_message_client_send( struct rocketnet_message_client_t * client, int channel, int reliability, const uint8_t * data, int bytes );

// 1 with the next message on channel, 0 when there is none. a NULL client, data or bytes is a counted no-op that returns 0.
int rocketnet_message_client_receive( struct rocketnet_message_client_t * client, int channel, const uint8_t ** data, int * bytes );

// Copies this update's receipt list into out, up to capacity entries; returns
// the copied count. Repeated calls see the same list until the next update.
int rocketnet_message_client_receipts( struct rocketnet_message_client_t * client, struct rocketnet_message_receipt_t * out, int capacity );

// a NULL client or data is a counted no-op; a pointer that is not a held view is a counted misuse.
void rocketnet_message_client_release( struct rocketnet_message_client_t * client, const uint8_t * data );

// 1 when the budget took the new rate, from now on. a NULL client is a counted no-op that returns 0.
int rocketnet_message_client_set_send_kbps( struct rocketnet_message_client_t * client, int32_t kbps );

// ROCKETNET_MESSAGE_ERROR_* the client ended its connection with, NONE while it has not. a NULL client is a counted no-op that returns NONE.
int rocketnet_message_client_disconnect_reason( struct rocketnet_message_client_t * client );

// the message counters. a NULL client is a counted no-op that returns NULL.
const struct rocketnet_counters_t * rocketnet_message_client_counters( struct rocketnet_message_client_t * client );

// ---- server ----

// the bytes a caller block for a server object needs, at the transport config's max_clients; 0 for a refused config
uint64_t rocketnet_message_server_required_memory( const struct rocketnet_server_transport_config_t * transport_config, const struct rocketnet_message_config_t * message_config );

// a NULL address is a counted no-op that returns NULL. NULL configs are the defaults.
struct rocketnet_message_server_t * rocketnet_message_server_create( void * context, const char * address,
                                                                                    const struct rocketnet_server_transport_config_t * transport_config,
                                                                                    const struct rocketnet_message_config_t * message_config,
                                                                                    void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps );

// create with configs transport_config_bytes and message_config_bytes long: each
// known size is read over its defaults and created as above; an unknown size is
// refused with a log line and returns NULL before any other check. a NULL config
// is its defaults, whatever its size says.
struct rocketnet_message_server_t * rocketnet_message_server_create_sized( void * context, const char * address,
                                                                                          const struct rocketnet_server_transport_config_t * transport_config, size_t transport_config_bytes,
                                                                                          const struct rocketnet_message_config_t * message_config, size_t message_config_bytes,
                                                                                          void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps );

// a NULL server is a counted no-op.
void rocketnet_message_server_destroy( struct rocketnet_message_server_t * server );

// the server's transport, for its port, stats and stop; never its batches. a NULL server is a counted no-op that returns NULL.
struct rocketnet_server_transport_t * rocketnet_message_server_transport( struct rocketnet_message_server_t * server );

// ROCKETNET_MESSAGE_OK, or ROCKETNET_ERROR_MESSAGE_NOT_RELEASED. a NULL server is a counted no-op that returns ROCKETNET_MESSAGE_OK.
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

// a NULL server is a counted no-op that returns 0.
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

// a NULL server, data or bytes is a counted no-op that returns 0.
int rocketnet_message_server_receive( struct rocketnet_message_server_t * server, int client_index, int channel, const uint8_t ** data, int * bytes );

// As rocketnet_message_client_receipts for one connected client slot. Invalid inputs return zero.
int rocketnet_message_server_receipts( struct rocketnet_message_server_t * server, int client_index, struct rocketnet_message_receipt_t * out, int capacity );

// a NULL server or data is a counted no-op; a pointer that is not a held view is a counted misuse.
void rocketnet_message_server_release( struct rocketnet_message_server_t * server, int client_index, const uint8_t * data );

// a NULL server is a counted no-op that returns 0.
int rocketnet_message_server_set_send_kbps( struct rocketnet_message_server_t * server, int client_index, int32_t kbps );

// ROCKETNET_MESSAGE_ERROR_* the slot's last connection ended with, until the slot's next connection. a NULL server is a counted no-op that returns NONE.
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

#ifdef __cplusplus
}
#endif

#endif // #ifndef ROCKETNET_LAYERS_H
