/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    The transport foundation: bytes to the wire and back, at the highest IO rate
    each platform allows. Connection, reliability, fragmentation, broadcast, and
    nothing above that line. What a payload MEANS is never this library's
    business — see serialize, schema and delta for that.

    This header is the entire public contract. It is C, it is what a binding
    generator reads, and it is what a C# caller reaches through without a wrapper
    layer in between.

    ------------------------------------------------------------------------
    THE ONE OPINION

    This API is shaped the way it is to enable zero copy and multicast flows.
    Everything unusual about it below follows from one of those two and nothing
    else. If a future addition serves neither, it is not RocketNet's to hold.

    ------------------------------------------------------------------------
    THE SEND MODEL, IN ONE PARAGRAPH

    You do not hand RocketNet your buffer. You ask RocketNet for one, write your
    payload directly into it, and commit it. That is what makes the send path
    genuinely zero copy — on AF_XDP the memory you wrote into IS the descriptor
    in the transmit ring — and it is what lets many threads send at once, since
    each claim takes its own slot and no two threads ever contend for a shared
    assembly buffer.

        rocketnet_buffer_t buffer;
        rocketnet_server_claim( server, 1024, &buffer );
        memcpy( buffer.data, my_payload, my_bytes );          // or write in place
        rocketnet_server_commit( server, connection_id, channel, &buffer, my_bytes );

    Every successful claim ends in exactly one commit, commit_many, commit_flow,
    or abandon. There is no other way to give a buffer back, and leaking one
    leaks pool capacity until the process ends.

    ------------------------------------------------------------------------
    THREADING

    claim, commit, commit_many, commit_flow and abandon are safe to call from any
    thread, concurrently, without external locking. That is the point of them.

    Everything else — create, destroy, update, connect, disconnect, flow
    membership, stats — is single threaded and belongs on whichever thread owns
    the object. update() is where received packets are dispatched and where
    timeouts fire, so callbacks always arrive on the thread that called update().

    ------------------------------------------------------------------------
    ERRORS

    Every call that can fail returns int: ROCKETNET_OK, or a negative
    ROCKETNET_ERROR_* value. Nothing throws, nothing longjmps, nothing aborts on
    a caller's mistake once asserts are compiled out. A failed claim leaves the
    caller's rocketnet_buffer_t zeroed rather than half-filled.
*/

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

#define ROCKETNET_VERSION_FULL                             "0.1.0"
#define ROCKETNET_VERSION_MAJOR_INT                              0
#define ROCKETNET_VERSION_MINOR_INT                              1
#define ROCKETNET_VERSION_PATCH_INT                              0

// ---------------------------------------------------------------------------
// Export machinery
//
// One header serves static linking, building the shared library, and consuming
// the shared library, on the three platforms that matter. Define ROCKETNET_SHARED
// to use the shared library; the library's own build additionally defines
// ROCKETNET_EXPORT.
// ---------------------------------------------------------------------------

#if defined( _WIN32 )
    #define NOMINMAX
#endif

#if defined( ROCKETNET_SHARED )
    #if defined( _WIN32 )
        #ifdef ROCKETNET_EXPORT
            #if defined( __cplusplus )
                #define ROCKETNET_EXPORT_FUNC extern "C" __declspec(dllexport)
            #else
                #define ROCKETNET_EXPORT_FUNC extern __declspec(dllexport)
            #endif
        #else
            #if defined( __cplusplus )
                #define ROCKETNET_EXPORT_FUNC extern "C" __declspec(dllimport)
            #else
                #define ROCKETNET_EXPORT_FUNC extern __declspec(dllimport)
            #endif
        #endif
    #else
        // macOS and Linux. -fvisibility=hidden is expected on the library build,
        // so the public surface is exactly what this header marks.
        #if defined( __cplusplus )
            #define ROCKETNET_EXPORT_FUNC extern "C" __attribute__((visibility("default")))
        #else
            #define ROCKETNET_EXPORT_FUNC extern __attribute__((visibility("default")))
        #endif
    #endif
#else
    #if defined( __cplusplus )
        #define ROCKETNET_EXPORT_FUNC extern "C"
    #else
        #define ROCKETNET_EXPORT_FUNC extern
    #endif
#endif

// ---------------------------------------------------------------------------
// Platform identification
// ---------------------------------------------------------------------------

#define ROCKETNET_PLATFORM_UNKNOWN                               0
#define ROCKETNET_PLATFORM_WINDOWS                               1
#define ROCKETNET_PLATFORM_MAC                                   2
#define ROCKETNET_PLATFORM_LINUX                                 3
#define ROCKETNET_PLATFORM_IOS                                   4

#if defined( _WIN32 )
    #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_WINDOWS
#elif defined( __APPLE__ )
    #include "TargetConditionals.h"
    #if TARGET_OS_IPHONE
        #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_IOS
    #else
        #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_MAC
    #endif
#else
    #define ROCKETNET_PLATFORM ROCKETNET_PLATFORM_LINUX
#endif

// ---------------------------------------------------------------------------
// Limits and constants
// ---------------------------------------------------------------------------

// The largest payload that fits in a single packet after RocketNet's own header
// space. Ask a channel for more than this and it fragments; see
// rocketnet_channel_config_t.
#define ROCKETNET_MAX_PAYLOAD_BYTES                           1180

// Hard ceiling on a single payload, fragmented or not. Bounds reassembly memory
// per connection so a peer cannot announce a gigabyte and make the server hold
// the pieces.
#define ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES             1048576

#define ROCKETNET_MAX_CHANNELS                                   8

#define ROCKETNET_KEY_BYTES                                     32
#define ROCKETNET_CONNECT_TOKEN_BYTES                         2048
#define ROCKETNET_USER_DATA_BYTES                              256

#define ROCKETNET_MAX_ADDRESS_STRING_LENGTH                    256

// ---------------------------------------------------------------------------
// Result codes
// ---------------------------------------------------------------------------

#define ROCKETNET_OK                                             0
#define ROCKETNET_ERROR                                         -1
#define ROCKETNET_ERROR_INVALID_ARGUMENT                        -2
#define ROCKETNET_ERROR_OUT_OF_MEMORY                           -3
#define ROCKETNET_ERROR_NOT_CONNECTED                           -4
#define ROCKETNET_ERROR_UNKNOWN_CONNECTION                      -5
#define ROCKETNET_ERROR_UNKNOWN_FLOW                            -6
#define ROCKETNET_ERROR_UNKNOWN_CHANNEL                         -7

// The send buffer pool is full. Every claimed buffer is either waiting to go out
// or waiting to be acknowledged. Retry next tick, or send less.
#define ROCKETNET_ERROR_POOL_EXHAUSTED                          -8

// This one connection has reached max_pool_bytes_per_connection. Its own backlog
// is the problem, not the pool's. See the occupancy note on that field.
#define ROCKETNET_ERROR_QUOTA_EXCEEDED                          -9

// The channel's send queue is full. On a LATEST channel this cannot happen,
// because a newer payload replaces the older one rather than queueing behind it.
#define ROCKETNET_ERROR_QUEUE_FULL                             -10

#define ROCKETNET_ERROR_PAYLOAD_TOO_LARGE                      -11

// The buffer handle does not refer to a live claim. Either it was already
// committed or abandoned, or it is a stale copy of a handle whose slot has since
// been reused. This is the generation counter earning its keep: without it the
// same mistake would be a silent write into someone else's packet.
#define ROCKETNET_ERROR_STALE_BUFFER                           -12

// The two ends do not agree on the channel layout. See rocketnet_connection_config_t.
#define ROCKETNET_ERROR_CHANNEL_MISMATCH                       -13

#define ROCKETNET_ERROR_TOKEN_INVALID                          -14
#define ROCKETNET_ERROR_TOKEN_EXPIRED                          -15
#define ROCKETNET_ERROR_SERVER_FULL                            -16
#define ROCKETNET_ERROR_BACKEND_UNAVAILABLE                    -17

// ---------------------------------------------------------------------------
// Log levels
// ---------------------------------------------------------------------------

#define ROCKETNET_LOG_LEVEL_NONE                                 0
#define ROCKETNET_LOG_LEVEL_ERROR                                1
#define ROCKETNET_LOG_LEVEL_WARN                                 2
#define ROCKETNET_LOG_LEVEL_INFO                                 3
#define ROCKETNET_LOG_LEVEL_DEBUG                                4
#define ROCKETNET_LOG_LEVEL_SPAM                                 5

// ---------------------------------------------------------------------------
// IO backends
//
// The whole reason this library exists is that these differ wildly in what they
// cost. The contract above them does not change; only the ceiling does.
// ---------------------------------------------------------------------------

// Pick the fastest backend this build and this machine actually support at
// runtime, falling back quietly. This is what you want unless you are measuring.
#define ROCKETNET_BACKEND_AUTO                                   0

// Ordinary UDP sockets. Available everywhere, slowest, and the reference against
// which the others are measured.
#define ROCKETNET_BACKEND_SOCKET                                 1

// Linux AF_XDP kernel bypass. Requires privileges and a cooperating NIC; see the
// platform documentation, because the setup requirements are real and a server
// that cannot meet them should be asking for SOCKET rather than failing.
#define ROCKETNET_BACKEND_XDP                                    2

// Windows registered IO over completion ports.
#define ROCKETNET_BACKEND_IOCP                                   3

// macOS and iOS kqueue.
#define ROCKETNET_BACKEND_KQUEUE                                 4

// ---------------------------------------------------------------------------
// Channel kinds — the four delivery disciplines
//
// A channel is a delivery discipline, not a topic and not a message type. The
// transport never looks inside a payload; a LATEST channel compares sequence
// numbers it assigned itself.
//
// Fragmentation is the fourth discipline and is deliberately NOT a fourth kind.
// It is a property any channel may have, obtained by setting
// max_payload_bytes above ROCKETNET_MAX_PAYLOAD_BYTES. The record left open
// whether fragmentation should be reliable-only; it is available on all three
// kinds, because the motivating case — large broadcast baselines that today get
// fragmented and blind-resent by hand a layer up — is not a reliable-channel
// case, and a transport that cannot serve it leaves the hand-rolled version in
// place. See the per-kind notes for what fragmentation means on each.
// ---------------------------------------------------------------------------

// Sent once. May be lost. No ordering owed, no retransmission, no acknowledgement
// tracking, and the cheapest of the three. Fragmented payloads on this kind are
// all-or-nothing: a payload with a missing fragment is dropped whole once its
// reassembly window passes, never delivered partial.
#define ROCKETNET_CHANNEL_UNRELIABLE                             0

// Delivered eventually and in order, retransmitted until acknowledged. This is
// yojimbo's block messages without the message system: arbitrary size, acked,
// resent.
//
// Fragmented payloads on this kind are acknowledged per fragment and resent per
// fragment, so a single lost fragment of a large payload costs one fragment
// rather than the whole payload. That is not a future idea; it is what the
// proving-ground game already does by hand, moved to where it belongs.
//
// Ordering is per channel. Two reliable channels never block each other, which
// is how a caller designs head-of-line blocking out: put the large slow thing on
// its own channel and the small urgent thing on another.
#define ROCKETNET_CHANNEL_RELIABLE                               1

// Most recent only. The newest payload supersedes every older one, on both
// sides, and both halves matter:
//
//   On receive, a payload older than one already delivered is discarded rather
//   than delivered late.
//
//   On send, a newer payload queued behind an older one that has not yet gone
//   out REPLACES it rather than queueing after it. A backlog collapses to a
//   single current value and the bandwidth is saved outright rather than spent
//   on state nobody will use.
//
// This is the discipline for anything whose value is its freshness — input, a
// pose, a heartbeat — and it is the one yojimbo did not have.
//
// Superseding a claimed-but-unsent payload releases the superseded buffer, so on
// this kind commit does not mean the bytes will be transmitted. It means they
// are the current value until something newer arrives. A caller that needs to
// know what actually went out reads the channel's sent counter in stats.
#define ROCKETNET_CHANNEL_LATEST                                 2

// ---------------------------------------------------------------------------
// Client states
//
// Negative is terminal failure, zero is idle, positive is progress. Lifted from
// netcode.io deliberately: the security model comes across whole and so does its
// vocabulary.
// ---------------------------------------------------------------------------

#define ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED            -6
#define ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN            -5
#define ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT             -4
#define ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT    -3
#define ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT     -2
#define ROCKETNET_CLIENT_STATE_CONNECTION_DENIED                -1
#define ROCKETNET_CLIENT_STATE_DISCONNECTED                      0
#define ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST        1
#define ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE       2
#define ROCKETNET_CLIENT_STATE_CONNECTED                         3

// ---------------------------------------------------------------------------
// Opaque handles
//
// Forward declarations only. The caller never sees a field and the layout is
// free to change between patch releases without breaking anyone.
// ---------------------------------------------------------------------------

struct rocketnet_client_t;
struct rocketnet_server_t;
struct rocketnet_address_t;

// A flow is named by a plain integer rather than a pointer, so a flow id crosses
// to C# as a long and an array of them is blittable.
typedef uint32_t rocketnet_flow_id_t;

// A connection is likewise named by an integer rather than a pointer. Three
// reasons, all of which matter more than the pointer chase it costs: a stale
// pointer from a disconnected peer is a crash while a stale id is
// ROCKETNET_ERROR_UNKNOWN_CONNECTION, an array of ids is blittable so broadcast
// takes one transition instead of N, and an id is stable to log and to compare.
//
// Ids are never reused within the lifetime of a server.
typedef uint64_t rocketnet_connection_id_t;

#define ROCKETNET_INVALID_CONNECTION_ID                         ((rocketnet_connection_id_t)0)
#define ROCKETNET_INVALID_FLOW_ID                               ((rocketnet_flow_id_t)0xFFFFFFFF)

// ---------------------------------------------------------------------------
// The claimed buffer
//
// This is the piece with no equivalent in netcode or Network Next, and it is
// where the performance story lives.
//
// It is a value, not a pointer, and it is blittable, so a C# caller receives it
// as a struct and turns `data` and `bytes` into a Span<byte> over native memory
// with no marshalling and no copy. Passing it by pointer to commit rather than
// by value is deliberate: it keeps the calling convention identical on every ABI
// this has to survive, including the Windows one where a 24-byte struct by value
// is already an implicit pointer.
//
// `token` is opaque and carries the pool, the slot within it, and a generation
// counter for that slot. The caller must never interpret it or do arithmetic on
// it — that is the whole reason it exists rather than the caller deriving a slot
// index from `data` as flow's predecessor did. Deriving the index from the
// pointer made the buffer's lifetime the arena's lifetime, and reliability,
// supersede and broadcast all need a buffer to outlive its tick.
//
// The generation counter is what turns "committed this twice" from memory
// corruption into ROCKETNET_ERROR_STALE_BUFFER.
// ---------------------------------------------------------------------------

typedef struct rocketnet_buffer_t
{
    // Writable payload memory owned by the transport. Valid from a successful
    // claim until the matching commit or abandon, and NOT valid after. Writing
    // through it after commit is a use-after-free that no assert will catch,
    // because the memory is real and belongs to someone else by then.
    uint8_t * data;

    // Opaque. Pool, slot and generation. Do not interpret.
    uint64_t token;

    // Capacity claimed, in bytes. Never less than the bytes requested; may be
    // more, because the pool allocates in size classes. Writing beyond this is
    // the caller's bug and the library cannot see it.
    int bytes;

} rocketnet_buffer_t;

// ---------------------------------------------------------------------------
// A received payload, in the batch form
//
// The default receive path is the callback in the config struct, where the
// pointer's lifetime is obvious because it ends when the call returns. This
// struct is the opt-in for callers who genuinely need to hold received bytes
// past the callback — see rocketnet_server_receive.
// ---------------------------------------------------------------------------

typedef struct rocketnet_payload_t
{
    // Read-only payload memory owned by the transport. Valid until the matching
    // release. Reassembled fragments are contiguous here; the caller never sees
    // a fragment.
    const uint8_t * data;

    // Opaque. Pass back to release.
    uint64_t token;

    // Zero on a client, where there is only one peer.
    rocketnet_connection_id_t connection_id;

    int bytes;
    int channel;

} rocketnet_payload_t;

// ---------------------------------------------------------------------------
// Batch send descriptor
//
// One entry per (connection, channel, buffer) triple, so a server with many
// different unicast payloads to place pays one managed-to-native transition for
// the lot rather than one each.
//
// Note there is deliberately no claim_batch. Claims are not the per-item cost:
// under the broadcast encoding model a server claims a handful of payloads per
// tick and commits each to hundreds of connections, so commit and receive are
// where the transitions pile up and those are the two that have batch forms.
// If profiling on a real workload says otherwise, add it then.
// ---------------------------------------------------------------------------

typedef struct rocketnet_send_t
{
    rocketnet_connection_id_t connection_id;
    rocketnet_buffer_t buffer;
    int channel;
    int bytes;                      // bytes actually written, <= buffer.bytes
    int result;                     // filled by the library: ROCKETNET_OK or an error

} rocketnet_send_t;

// ---------------------------------------------------------------------------
// Callbacks
//
// A void * context supplied at creation comes back in every one of them, which
// is how a C API avoids globals and lets one process host several instances.
//
// C# callers: these are function pointers, so the delegate must be kept alive
// for as long as the object is. A collected delegate is a jump into freed code
// and it will not look like a networking bug when it happens.
// ---------------------------------------------------------------------------

// A payload arrived. `payload` is valid FOR THE DURATION OF THIS CALL AND NOT
// AFTER — copy out, or act on it here. This is the default and the recommended
// shape precisely because that sentence is short enough to be obeyed.
//
// Called from inside update(), on the thread that called update(). Do not call
// destroy from here.
typedef void (*rocketnet_client_receive_callback_t)( struct rocketnet_client_t * client,
                                                     void * context,
                                                     int channel,
                                                     const uint8_t * payload,
                                                     int bytes );

typedef void (*rocketnet_server_receive_callback_t)( struct rocketnet_server_t * server,
                                                     void * context,
                                                     rocketnet_connection_id_t connection_id,
                                                     int channel,
                                                     const uint8_t * payload,
                                                     int bytes );

// A client completed the handshake and is now a connection. `user_data` is the
// ROCKETNET_USER_DATA_BYTES the backend sealed into the connect token, which is
// how the game learns who this is without trusting anything the client said.
// Valid for the duration of the call.
//
// Called from inside rocketnet_server_update().
typedef void (*rocketnet_server_connect_callback_t)( struct rocketnet_server_t * server,
                                                     void * context,
                                                     rocketnet_connection_id_t connection_id,
                                                     const uint8_t * user_data );

// A connection ended, for any reason: the peer disconnected, it timed out, the
// server disconnected it, or it exceeded its buffer quota. `reason` is a
// ROCKETNET_ERROR_* value or ROCKETNET_OK for a clean disconnect.
//
// The id is dead by the time this returns and every flow membership it had is
// already dropped. Called from inside rocketnet_server_update().
typedef void (*rocketnet_server_disconnect_callback_t)( struct rocketnet_server_t * server,
                                                        void * context,
                                                        rocketnet_connection_id_t connection_id,
                                                        int reason );

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

typedef struct rocketnet_channel_config_t
{
    // ROCKETNET_CHANNEL_UNRELIABLE, _RELIABLE or _LATEST.
    int kind;

    // Largest single payload this channel accepts. Above
    // ROCKETNET_MAX_PAYLOAD_BYTES the channel fragments on send and reassembles
    // on receive, and the caller pays for that only by asking for it: reassembly
    // buffers are sized from this number, per connection.
    int max_payload_bytes;

    // How many payloads may be in flight or waiting on this channel before
    // commit returns ROCKETNET_ERROR_QUEUE_FULL. Ignored for LATEST, which is
    // always depth one by construction.
    int send_queue_size;

    // How many out-of-order payloads may be held while waiting for a gap to
    // fill, on a RELIABLE channel. Ignored otherwise.
    int receive_queue_size;

} rocketnet_channel_config_t;

// Fills a channel config with defaults for the given kind: one packet's worth of
// payload, no fragmentation, queues sized for a 60Hz caller.
ROCKETNET_EXPORT_FUNC void rocketnet_default_channel_config( struct rocketnet_channel_config_t * config, int kind );

// The channel layout, which BOTH ENDS must configure identically.
//
// A mismatch is not a subtle bug to discover in a playtest: RocketNet hashes the
// layout into the handshake and refuses the connection with
// ROCKETNET_ERROR_CHANNEL_MISMATCH. Channel index N means the same discipline on
// both ends or there is no connection at all.
//
// TODO(open, and writing the examples is what raised it): requiring both ends to
// build this identically means the same twenty lines appear in the client and in
// the server, and the only thing keeping them honest is a handshake that fails
// late. The obvious alternative is for the backend to seal the layout into the
// connect token, so the server declares it once and the client configures itself
// from what it was issued. That is better in every way except that it puts a
// transport concern into the token format and makes the token bigger, and it is
// not a decision to make in a header comment. Both ends configuring it is the
// conservative choice for now and the duplication is real.
typedef struct rocketnet_connection_config_t
{
    int num_channels;
    struct rocketnet_channel_config_t channel[ROCKETNET_MAX_CHANNELS];

    // No traffic for this long ends the connection.
    float timeout_seconds;

    // Send a keep-alive if nothing else has gone out for this long.
    float keep_alive_seconds;

} rocketnet_connection_config_t;

ROCKETNET_EXPORT_FUNC void rocketnet_default_connection_config( struct rocketnet_connection_config_t * config );

typedef struct rocketnet_client_config_t
{
    // "0.0.0.0:0" binds an ephemeral port, which is what a game client wants.
    char bind_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];

    // Must match the server's. Cheap protection against a stale build talking to
    // a current one.
    uint64_t protocol_id;

    struct rocketnet_connection_config_t connection;

    // Total claimable send memory for this client, in bytes. A client sends
    // little and this can be small.
    int pool_bytes;

    int socket_send_buffer_size;
    int socket_receive_buffer_size;

    int backend;                    // ROCKETNET_BACKEND_*

    rocketnet_client_receive_callback_t receive_callback;

} rocketnet_client_config_t;

ROCKETNET_EXPORT_FUNC void rocketnet_default_client_config( struct rocketnet_client_config_t * config );

typedef struct rocketnet_server_config_t
{
    // The address clients are told to reach, and the address actually bound.
    // They differ behind NAT and in a container, which is why there are two.
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    char bind_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];

    uint64_t protocol_id;

    // The private key this server shares with the backend that issues connect
    // tokens. Not a secret the client ever holds. Zero it after create if you
    // care, since RocketNet copies it.
    uint8_t private_key[ROCKETNET_KEY_BYTES];

    int max_connections;
    int max_flows;

    struct rocketnet_connection_config_t connection;

    // Total claimable send memory across the whole server. This is the pool
    // whose occupancy is bounded, and it is the number to size against the
    // thousand-player target rather than against a comfortable margin.
    int pool_bytes;

    // The hard per-connection bound, and the reason the pool cannot be pinned.
    //
    // A broadcast payload lives until every recipient has acknowledged it or
    // timed out, so one written buffer is referenced by many per-connection
    // delivery states, and one bad connection that acknowledges nothing would
    // otherwise hold the whole pool hostage on behalf of everyone else. When a
    // connection's referenced bytes exceed this, THAT CONNECTION is disconnected
    // — not the payload dropped, not the pool grown, not everyone stalled. The
    // cost of a bad peer falls on the bad peer.
    int max_pool_bytes_per_connection;

    int socket_send_buffer_size;
    int socket_receive_buffer_size;

    int backend;                    // ROCKETNET_BACKEND_*

    // Number of send/receive queues, which is the going-wide dial. One queue per
    // core that will be sending is the shape this is built for. Zero means pick
    // a reasonable number from the machine.
    int num_queues;

    rocketnet_server_receive_callback_t receive_callback;
    rocketnet_server_connect_callback_t connect_callback;
    rocketnet_server_disconnect_callback_t disconnect_callback;

} rocketnet_server_config_t;

ROCKETNET_EXPORT_FUNC void rocketnet_default_server_config( struct rocketnet_server_config_t * config );

// ---------------------------------------------------------------------------
// Statistics
//
// Filled into a caller-provided struct. Nothing is allocated and nothing is
// returned by pointer, so a C# caller reads them into a stack struct in a
// profiling overlay every frame without pressuring the collector.
// ---------------------------------------------------------------------------

typedef struct rocketnet_connection_stats_t
{
    float rtt_ms;
    float jitter_ms;
    float packet_loss_percent;

    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_acked;
    uint64_t packets_lost;

    uint64_t payloads_sent;
    uint64_t payloads_received;

    // Payloads a LATEST channel discarded because something newer replaced them.
    // Not a loss — this is the discipline working, and the number is how you see
    // it working.
    uint64_t payloads_superseded;

    uint64_t bytes_sent;
    uint64_t bytes_received;

    float bandwidth_kbps_up;
    float bandwidth_kbps_down;

    // Pool bytes this connection currently keeps alive, against
    // max_pool_bytes_per_connection. Watch this one: it is where a slow peer
    // shows up before it becomes a problem.
    int pool_bytes_referenced;

} rocketnet_connection_stats_t;

typedef struct rocketnet_server_stats_t
{
    int num_connections;
    int num_flows;

    // Pool occupancy across the whole server, and its high-water mark since
    // create. If the high-water mark reaches pool_bytes the server has been
    // failing claims and the number to change is pool_bytes.
    int pool_bytes_in_use;
    int pool_bytes_high_water;

    uint64_t claims;
    uint64_t claims_failed;

    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;

    // Packets the security layer threw away before they touched anything: bad
    // MAC, replayed sequence, unknown connection. A rising number here is an
    // attack or a bug and it is worth alerting on.
    uint64_t packets_rejected;

    float bandwidth_kbps_up;
    float bandwidth_kbps_down;

    // What the backend actually resolved to, which is not always what was asked
    // for. ROCKETNET_BACKEND_AUTO in the config becomes a concrete value here.
    int backend;

} rocketnet_server_stats_t;

// ---------------------------------------------------------------------------
// Global lifecycle
//
// There is no global config struct, unlike Network Next, because there is
// nothing global to configure: no backend hostname, no buyer keys, no service to
// reach. Everything that varies varies per client or per server.
// ---------------------------------------------------------------------------

// Call once before anything else. `context` is handed to the allocator hooks.
// Returns ROCKETNET_OK or an error.
ROCKETNET_EXPORT_FUNC int rocketnet_init( void * context );

// Call once at shutdown, after every client and server is destroyed. Destroying
// them after this is undefined.
ROCKETNET_EXPORT_FUNC void rocketnet_term( void );

// Seconds since rocketnet_init, from the platform's monotonic clock. update()
// reads this itself; it is exposed because a caller pacing its own loop should
// not have to find a second clock that might disagree with ours.
ROCKETNET_EXPORT_FUNC double rocketnet_time( void );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_protocol_version( void );

// Human-readable text for a ROCKETNET_ERROR_* value. Static storage; never null,
// even for a value this library does not recognise.
ROCKETNET_EXPORT_FUNC const char * rocketnet_error_string( int result );

// True if this build and this machine can actually run the given backend right
// now — the XDP privileges are present, the completion port API is the right
// vintage. Ask before configuring anything other than AUTO.
ROCKETNET_EXPORT_FUNC bool rocketnet_backend_available( int backend );

ROCKETNET_EXPORT_FUNC const char * rocketnet_backend_string( int backend );

// ---------------------------------------------------------------------------
// Hooks
//
// The library imposes neither malloc nor a logging system on its host. Set these
// before rocketnet_init.
// ---------------------------------------------------------------------------

ROCKETNET_EXPORT_FUNC void rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ),
                                                void (*free_function)( void * context, void * p ) );

ROCKETNET_EXPORT_FUNC void * rocketnet_malloc( void * context, size_t bytes );

ROCKETNET_EXPORT_FUNC void rocketnet_free( void * context, void * p );

ROCKETNET_EXPORT_FUNC void rocketnet_log_level( int level );

ROCKETNET_EXPORT_FUNC void rocketnet_log_function( void (*function)( int level, const char * format, ... ) );

ROCKETNET_EXPORT_FUNC void rocketnet_printf( int level, const char * format, ... );

ROCKETNET_EXPORT_FUNC void rocketnet_quiet( bool flag );

extern void (*rocketnet_assert_function_pointer)( const char * condition, const char * function, const char * file, int line );

ROCKETNET_EXPORT_FUNC void rocketnet_assert_function( void (*function)( const char * condition, const char * function, const char * file, int line ) );

#ifndef ROCKETNET_ASSERTS
    #ifdef NDEBUG
        #define ROCKETNET_ASSERTS 0
    #else
        #define ROCKETNET_ASSERTS 1
    #endif
#endif

#if ROCKETNET_ASSERTS
#define rocketnet_assert( condition )                                                                   \
do                                                                                                      \
{                                                                                                       \
    if ( !(condition) )                                                                                 \
    {                                                                                                    \
        rocketnet_assert_function_pointer( #condition, __FUNCTION__, __FILE__, __LINE__ );              \
    }                                                                                                    \
} while (0)
#else
#define rocketnet_assert( ignore ) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------

// Parses "1.2.3.4:5000", "[::1]:5000" or a bare address. Returns ROCKETNET_OK or
// ROCKETNET_ERROR_INVALID_ARGUMENT. Does not resolve hostnames — that is a
// blocking call and this library does not make blocking calls on a caller's
// behalf without saying so.
ROCKETNET_EXPORT_FUNC int rocketnet_address_parse( struct rocketnet_address_t * address, const char * string );

// Writes into the caller's buffer, which must be at least
// ROCKETNET_MAX_ADDRESS_STRING_LENGTH. Returns that buffer.
ROCKETNET_EXPORT_FUNC const char * rocketnet_address_to_string( const struct rocketnet_address_t * address, char * buffer );

ROCKETNET_EXPORT_FUNC bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b );

// Bytes needed for a rocketnet_address_t, so a caller in another language can
// allocate one without this header knowing its layout. The struct stays opaque
// because it holds a sockaddr and that differs per platform.
ROCKETNET_EXPORT_FUNC size_t rocketnet_address_bytes( void );

// ---------------------------------------------------------------------------
// Connect tokens
//
// The netcode.io model, unchanged, because this is not a place to be creative: a
// backend the client cannot forge issues a short-lived token carrying per-client
// keys, the client presents it, the server validates it against the private key
// it shares with that backend, and an attacker gets nothing to amplify.
//
// This function belongs in the BACKEND, not in the game server and never in the
// client. It is in this header because the backend links this library too.
// ---------------------------------------------------------------------------

// Writes ROCKETNET_CONNECT_TOKEN_BYTES into `output`.
//
// `server_addresses` is the list the client may try, in order, which is how a
// backend hands out a fleet rather than a machine. `user_data` is
// ROCKETNET_USER_DATA_BYTES the server receives in its connect callback,
// sealed so the client can neither read nor alter it — this is where identity
// goes, and it is why the server never has to believe a client about who it is.
//
// Returns ROCKETNET_OK or an error.
ROCKETNET_EXPORT_FUNC int rocketnet_generate_connect_token( uint64_t protocol_id,
                                                            uint64_t client_id,
                                                            int expire_seconds,
                                                            int timeout_seconds,
                                                            int num_server_addresses,
                                                            const char ** server_addresses,
                                                            const uint8_t * private_key,
                                                            const uint8_t * user_data,
                                                            uint8_t * output );

// Fills `key` with ROCKETNET_KEY_BYTES from the platform CSPRNG. Use it to make
// the private key a backend and a server share; do not invent one.
ROCKETNET_EXPORT_FUNC int rocketnet_random_key( uint8_t * key );

// ===========================================================================
// CLIENT
// ===========================================================================

// Creates a client. Allocates once, here, from the configured allocator; nothing
// after this allocates on the send or receive path.
//
// Returns NULL on failure, having logged why at ROCKETNET_LOG_LEVEL_ERROR.
// `context` is handed back in every callback. The config is copied, so it may be
// a stack struct.
ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context,
                                                                           const struct rocketnet_client_config_t * config );

// Destroys the client, disconnecting first if connected and sending the
// disconnect so the server does not have to wait for a timeout. Any buffer still
// claimed is released. Safe on NULL.
ROCKETNET_EXPORT_FUNC void rocketnet_client_destroy( struct rocketnet_client_t * client );

// Begins connecting with a token from the backend. Returns ROCKETNET_OK if the
// token parsed and the attempt started — NOT if it succeeded. Poll
// rocketnet_client_state, which goes negative on failure.
ROCKETNET_EXPORT_FUNC int rocketnet_client_connect( struct rocketnet_client_t * client, const uint8_t * connect_token );

// Disconnects cleanly, telling the server rather than letting it time out.
// Returns to ROCKETNET_CLIENT_STATE_DISCONNECTED. Reusable: connect again with a
// fresh token.
ROCKETNET_EXPORT_FUNC void rocketnet_client_disconnect( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC int rocketnet_client_state( struct rocketnet_client_t * client );

// Receives, dispatches, acknowledges, retransmits, and fires timeouts. The
// library does not own the caller's loop and never spawns a thread that calls
// this behind its back; call it once per tick.
//
// Receive callbacks fire from inside here, on this thread.
ROCKETNET_EXPORT_FUNC void rocketnet_client_update( struct rocketnet_client_t * client );

// Pushes everything queued to the wire now rather than at the next update.
// Useful immediately before a long frame; not needed in a steady loop.
ROCKETNET_EXPORT_FUNC void rocketnet_client_flush( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_client_port( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC const struct rocketnet_address_t * rocketnet_client_server_address( struct rocketnet_client_t * client );

// Claims `bytes` of writable send memory. On success `buffer->data` is yours to
// write until commit or abandon, and `buffer->bytes` is at least `bytes`.
//
// Cost: one atomic increment on the pool. No lock, no allocation, no syscall.
// Safe from any thread.
//
// Fails with ROCKETNET_ERROR_POOL_EXHAUSTED if the pool is full — retry next
// tick — or ROCKETNET_ERROR_PAYLOAD_TOO_LARGE. On failure `*buffer` is zeroed.
ROCKETNET_EXPORT_FUNC int rocketnet_client_claim( struct rocketnet_client_t * client, int bytes, struct rocketnet_buffer_t * buffer );

// Commits `bytes` of the claimed buffer to a channel. The caller's ownership of
// the memory ends here whether this succeeds or fails; the handle is dead either
// way and must not be committed again.
//
// `bytes` may be less than what was claimed — claim generously and commit what
// you wrote. It may not be more.
//
// Cost: enqueue and one atomic. Nothing is copied, ever. Safe from any thread.
//
// Fails with ROCKETNET_ERROR_STALE_BUFFER, _NOT_CONNECTED, _UNKNOWN_CHANNEL or
// _QUEUE_FULL. On _QUEUE_FULL the payload is dropped and the buffer released:
// there is nothing useful the caller could do with a handle to bytes the channel
// has refused.
ROCKETNET_EXPORT_FUNC int rocketnet_client_commit( struct rocketnet_client_t * client,
                                                   int channel,
                                                   const struct rocketnet_buffer_t * buffer,
                                                   int bytes );

// Gives a claimed buffer back unsent. The only correct way to abandon a claim.
ROCKETNET_EXPORT_FUNC int rocketnet_client_abandon( struct rocketnet_client_t * client, const struct rocketnet_buffer_t * buffer );

// Claim, copy, commit in one call, for callers who already have the bytes
// somewhere else and cannot write in place.
//
// Cost: one copy of `bytes`. That is the whole difference from the claim path
// and it is why this is the convenience rather than the default. It is here
// because a caller with an existing array should not have to pretend otherwise,
// and because for a 40-byte input packet the copy is beneath measurement.
ROCKETNET_EXPORT_FUNC int rocketnet_client_send( struct rocketnet_client_t * client,
                                                 int channel,
                                                 const uint8_t * payload,
                                                 int bytes );

// The opt-in receive form, for callers who must hold received bytes past the
// callback. Set receive_callback to NULL in the config and call this instead;
// setting both is ROCKETNET_ERROR_INVALID_ARGUMENT, because two receive paths
// racing over the same payloads is a bug waiting to be written.
//
// Fills up to `max_payloads` entries and writes the count to `num_payloads`.
// Every filled entry MUST be released, and each one holds pool memory until it
// is. Returns ROCKETNET_OK even when nothing arrived.
//
// Cost: one transition for the batch instead of one per payload, which is the
// point of it for a C# caller.
ROCKETNET_EXPORT_FUNC int rocketnet_client_receive( struct rocketnet_client_t * client,
                                                    struct rocketnet_payload_t * payloads,
                                                    int max_payloads,
                                                    int * num_payloads );

// Releases payloads from rocketnet_client_receive. Releasing an already-released
// payload returns ROCKETNET_ERROR_STALE_BUFFER rather than corrupting the pool.
ROCKETNET_EXPORT_FUNC int rocketnet_client_release( struct rocketnet_client_t * client,
                                                    const struct rocketnet_payload_t * payloads,
                                                    int num_payloads );

// Fills `stats`. Returns false if the client is not connected, leaving `stats`
// untouched. Never allocates.
ROCKETNET_EXPORT_FUNC bool rocketnet_client_stats( struct rocketnet_client_t * client, struct rocketnet_connection_stats_t * stats );

// ===========================================================================
// SERVER
// ===========================================================================

// Creates a server and binds its socket or attaches its kernel-bypass path.
// Returns NULL on failure, having logged why.
//
// A note that matters for anyone shipping this: nothing here reconfigures the
// machine. RocketNet will not disable hyperthreading, reassign NIC queues,
// unload another process's XDP programs or shell out to a compiler. If the XDP
// backend needs the machine prepared, it says so and fails, and preparing it is
// an operator's job with an operator's tools. A library that reconfigures its
// host is not one another studio can deploy.
ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context,
                                                                           const struct rocketnet_server_config_t * config );

// Disconnects every connection cleanly, releases everything, and destroys the
// server. Safe on NULL.
ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server );

// Receives, dispatches, acknowledges, retransmits, ages out timeouts, and fires
// connect and disconnect callbacks. Call once per tick.
//
// With num_queues > 1 the receive work happens on the library's own queue
// threads, but callbacks are still delivered from inside this call on this
// thread, so a caller's game state is never touched concurrently. That is
// deliberate: going wide is for the IO, not for the caller's invariants.
ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC void rocketnet_server_flush( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_server_port( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC const struct rocketnet_address_t * rocketnet_server_address( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC int rocketnet_server_connection_count( struct rocketnet_server_t * server );

// Fills `ids` with up to `max_ids` live connection ids and writes the count.
// The batch form exists so a caller broadcasting to everyone builds the array
// once per tick rather than crossing the boundary per connection.
ROCKETNET_EXPORT_FUNC int rocketnet_server_connection_ids( struct rocketnet_server_t * server,
                                                           rocketnet_connection_id_t * ids,
                                                           int max_ids,
                                                           int * num_ids );

ROCKETNET_EXPORT_FUNC bool rocketnet_server_connection_connected( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id );

ROCKETNET_EXPORT_FUNC const struct rocketnet_address_t * rocketnet_server_connection_address( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id );

// Disconnects one connection cleanly and fires the disconnect callback before
// returning. The id is invalid afterwards and is never reused.
ROCKETNET_EXPORT_FUNC int rocketnet_server_disconnect( struct rocketnet_server_t * server, rocketnet_connection_id_t connection_id );

ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_all( struct rocketnet_server_t * server );

// ---------------------------------------------------------------------------
// Server send: claim, write, commit
// ---------------------------------------------------------------------------

// Claims `bytes` of writable send memory from the server's pool.
//
// Cost: one atomic increment. No lock, no allocation, no syscall, and no
// per-connection state touched — a claimed buffer is not yet addressed to
// anyone, which is exactly what makes broadcast possible without a copy.
//
// Safe from any thread, which is the reason the claim is a single atomic rather
// than a lock: worker threads building payloads in parallel is the shape this
// exists for.
//
// Fails with ROCKETNET_ERROR_POOL_EXHAUSTED or _PAYLOAD_TOO_LARGE. On failure
// `*buffer` is zeroed.
ROCKETNET_EXPORT_FUNC int rocketnet_server_claim( struct rocketnet_server_t * server, int bytes, struct rocketnet_buffer_t * buffer );

// Commits a claimed buffer to one connection on one channel. The caller's
// ownership of the memory ends here, success or failure.
//
// Cost: enqueue and one atomic. Nothing is copied. Safe from any thread.
//
// The buffer is NOT free when this returns. On a reliable channel it lives until
// acknowledged; on a latest channel it lives until superseded or sent. That is
// the consequence of retransmission and it is why commit does not mean "done
// with it" — see max_pool_bytes_per_connection.
ROCKETNET_EXPORT_FUNC int rocketnet_server_commit( struct rocketnet_server_t * server,
                                                   rocketnet_connection_id_t connection_id,
                                                   int channel,
                                                   const struct rocketnet_buffer_t * buffer,
                                                   int bytes );

// Commits many different buffers to many different connections in one call, so a
// C# caller pays one transition rather than N. Each entry's `result` is filled
// in; the call itself returns ROCKETNET_OK if it was well-formed, even when
// individual entries failed. Every buffer is consumed either way.
ROCKETNET_EXPORT_FUNC int rocketnet_server_commit_batch( struct rocketnet_server_t * server,
                                                         struct rocketnet_send_t * sends,
                                                         int num_sends );

// BROADCAST, in its explicit form: commit ONE claimed buffer to MANY connections.
//
// This is a first-class send rather than a loop the caller writes, and the
// difference is the whole point. The payload is written once, and each
// connection takes a reference to those same bytes rather than a copy of them.
// The buffer lives until the last recipient acknowledges it or times out, and
// retransmission goes only to the subset that has not acknowledged.
//
// Cost: O(num_connections) reference increments and queue entries, and zero
// copies of the payload. Safe from any thread.
//
// COST HONESTLY STATED, because this is the number that decides whether
// broadcast lands or merely moves the problem: in this version each connection's
// packet is still SEALED SEPARATELY under that connection's own key, so the
// bulk cryptography is paid once per recipient even though the payload is
// written once. Encoding, assembly and memory are shared; encryption is not.
//
// TODO(open, and not for this header to settle): whether a hybrid scheme —
// sealing the bulk once under an ephemeral key and delivering that key to each
// recipient wrapped under theirs — is worth its complexity here. It is
// well-trodden ground with real prior art and it is emphatically not a place to
// invent a construction; it gets reviewed against established constructions by
// someone who does cryptography for a living before it ships. The naive path is
// measured first, because affordable and boring beats clever and unreviewed.
// Nothing in this API assumes a payload is sealed for exactly one recipient, so
// the answer can change without the signature changing.
ROCKETNET_EXPORT_FUNC int rocketnet_server_commit_many( struct rocketnet_server_t * server,
                                                        const rocketnet_connection_id_t * connection_ids,
                                                        int num_connections,
                                                        int channel,
                                                        const struct rocketnet_buffer_t * buffer,
                                                        int bytes );

// Gives a claimed buffer back unsent.
ROCKETNET_EXPORT_FUNC int rocketnet_server_abandon( struct rocketnet_server_t * server, const struct rocketnet_buffer_t * buffer );

// Claim, copy, commit in one call. Costs one copy of `bytes`; see
// rocketnet_client_send.
ROCKETNET_EXPORT_FUNC int rocketnet_server_send( struct rocketnet_server_t * server,
                                                 rocketnet_connection_id_t connection_id,
                                                 int channel,
                                                 const uint8_t * payload,
                                                 int bytes );

// ---------------------------------------------------------------------------
// Flows
//
// A flow is a named set of subscribed connections that the server publishes to
// once, without enumerating recipients at the call site.
//
// Two reasons it exists rather than the caller keeping its own id array. The
// small one is cost: a thousand-player server rebuilding a thousand-element
// array per flow per tick is work that buys nothing. The large one is that this
// is the shape a relay fan-out needs, so a server written against flows today
// keeps working unchanged when the payload goes to a relay instead of out N
// times — and a server written against explicit id arrays does not.
//
// In THIS version a flow is fanned out by the server itself as N authenticated
// unicast sends. There is no relay and no IP multicast anywhere in this library.
// The API is shaped so adding one later does not change these signatures.
//
// SUBSCRIPTION IS AN OPERATION THE SERVER PERFORMS, and there is deliberately no
// client-side subscribe call anywhere in this header. That is a security
// property, not an inconvenience: in a competitive game, interest management is
// an anti-cheat boundary, and a client that could subscribe itself to the flow
// covering the far side of the map has a wallhack implemented at the protocol
// level. Having no client-initiated path deletes that attack surface rather than
// defending it. A client discovers what it is subscribed to by receiving it.
//
// Deciding WHICH flows a client should be in is game logic and stays above this
// library, which carries subscriptions and never computes them.
// ---------------------------------------------------------------------------

// Creates a flow that publishes on `channel`. Returns ROCKETNET_INVALID_FLOW_ID
// if max_flows is reached.
//
// A flow is bound to one channel at creation so the receiving client can route
// on the channel it arrived on. That is the reason there is no flow id on the
// wire: putting one there would be structure in the payload, and a client that
// must distinguish two flows uses two channels. TODO(open): whether that holds
// up for a game with many small flows, or whether flow identity eventually has
// to be carried. Carrying it costs bytes on every packet and it is not obviously
// worth them, so it stays out until a real workload asks.
ROCKETNET_EXPORT_FUNC rocketnet_flow_id_t rocketnet_server_flow_create( struct rocketnet_server_t * server, int channel );

// Destroys a flow. Subscribers are dropped; payloads already committed to it
// still finish being delivered, because they belong to the connections now
// rather than to the flow.
ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_destroy( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscribe( struct rocketnet_server_t * server,
                                                           rocketnet_flow_id_t flow_id,
                                                           rocketnet_connection_id_t connection_id );

// The batch form, for the interest-management pass that moves many connections
// between flows at once.
ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscribe_many( struct rocketnet_server_t * server,
                                                                rocketnet_flow_id_t flow_id,
                                                                const rocketnet_connection_id_t * connection_ids,
                                                                int num_connections );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_unsubscribe( struct rocketnet_server_t * server,
                                                             rocketnet_flow_id_t flow_id,
                                                             rocketnet_connection_id_t connection_id );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_unsubscribe_many( struct rocketnet_server_t * server,
                                                                  rocketnet_flow_id_t flow_id,
                                                                  const rocketnet_connection_id_t * connection_ids,
                                                                  int num_connections );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscriber_count( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id );

// Fills `ids` with up to `max_ids` subscribers and writes the count.
ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscribers( struct rocketnet_server_t * server,
                                                             rocketnet_flow_id_t flow_id,
                                                             rocketnet_connection_id_t * ids,
                                                             int max_ids,
                                                             int * num_ids );

// BROADCAST, in the form to prefer: commit one claimed buffer to every
// subscriber of a flow. Identical semantics and identical costs to
// rocketnet_server_commit_many, including the cryptography note there, with the
// recipient set held by the library instead of rebuilt by the caller.
//
// A flow with no subscribers is not an error. The buffer is consumed and the
// call returns ROCKETNET_OK, because "nobody is looking at this region" is a
// normal state of the world and not a caller mistake.
ROCKETNET_EXPORT_FUNC int rocketnet_server_commit_flow( struct rocketnet_server_t * server,
                                                        rocketnet_flow_id_t flow_id,
                                                        const struct rocketnet_buffer_t * buffer,
                                                        int bytes );

// ---------------------------------------------------------------------------
// Server receive and statistics
// ---------------------------------------------------------------------------

// The opt-in receive form; see rocketnet_client_receive for the contract. Set
// receive_callback to NULL in the config to use this.
ROCKETNET_EXPORT_FUNC int rocketnet_server_receive( struct rocketnet_server_t * server,
                                                    struct rocketnet_payload_t * payloads,
                                                    int max_payloads,
                                                    int * num_payloads );

ROCKETNET_EXPORT_FUNC int rocketnet_server_release( struct rocketnet_server_t * server,
                                                    const struct rocketnet_payload_t * payloads,
                                                    int num_payloads );

// Fills `stats` for the server as a whole. Always true on a live server.
ROCKETNET_EXPORT_FUNC bool rocketnet_server_stats( struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats );

// Fills `stats` for one connection. False if the id is unknown, leaving `stats`
// untouched. Never allocates.
ROCKETNET_EXPORT_FUNC bool rocketnet_server_connection_stats( struct rocketnet_server_t * server,
                                                              rocketnet_connection_id_t connection_id,
                                                              struct rocketnet_connection_stats_t * stats );

#endif // #ifndef ROCKETNET_H
