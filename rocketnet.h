/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    The transport foundation: bytes to the wire and back, at the highest IO rate
    each platform allows. Connection, reliability, fragmentation, broadcast, and
    nothing above that line. What a packet MEANS is never this library's
    business — see serialize, schema and delta for that.

    This header is the entire public contract. It is C, it is what a binding
    generator reads, and it is what a C# caller reaches through without a wrapper
    layer in between.

    ------------------------------------------------------------------------
    VOCABULARY, SETTLED UP FRONT

    A PACKET is what the caller sends and receives: a byte buffer with a
    delivery discipline. This is the word the genre already voted for — RakNet
    and ENet callers send packets — and it means the caller's unit, not the
    wire's.

    A TRANSPORT PACKET is what actually goes on the wire. RocketNet splits
    packets into transport packets on send and reassembles them on receive,
    sized from what the live transport reports. The caller never sees one.

    A TRANSPORT is the swappable engine underneath: plain sockets, io_uring,
    AF_XDP, whatever the platform offers. Same contract above, different
    ceiling below. See TRANSPORTS.

    ------------------------------------------------------------------------
    THE ONE OPINION

    This API is shaped the way it is to enable zero copy and flow broadcast.
    Everything unusual about it below follows from one of those two and nothing
    else. If a future addition serves neither, it is not RocketNet's to hold.

    ------------------------------------------------------------------------
    THE SEND MODEL, IN ONE PARAGRAPH

    You do not hand RocketNet your buffer. You start a packet, which claims
    writable memory addressed to a destination, write your bytes directly into
    it, and send it. That is what makes the send path genuinely zero copy — on
    AF_XDP the memory you wrote into IS the descriptor in the transmit ring —
    and it is what lets many threads send at once, since each start takes its
    own slot and no two threads ever contend for a shared assembly buffer.

        rocketnet_packet_t packet;
        rocketnet_server_start_packet( server, rocketnet_destination_client( client_index ),
                                       ROCKETNET_PACKET_RELIABLE, 1024, &packet );
        memcpy( packet.data, my_bytes, my_num_bytes );        // or write in place
        rocketnet_server_send_packet( server, &packet, my_num_bytes );

    Every successfully started packet ends in exactly one send or abandon.
    There is no other way to give the memory back, and leaking a started packet
    leaks pool capacity until the process ends.

    ------------------------------------------------------------------------
    DESTINATIONS

    Every server-side send names a destination: a single uint64 that encodes
    either one client or one flow. High bit clear, the low bits are a client
    index. High bit set, the low bits are a flow id, and the packet goes to
    every subscriber of that flow. One argument, both shapes, every entry
    point — a caller broadcasting to a region and a caller whispering to one
    client write the same line of code.

    Clients have no destinations. A client sends to its server, full stop, and
    when a client receives a packet it neither knows nor cares whether a flow
    was involved. It arrived reliably in order, or reliable most recent, or
    unreliably, as it does.

    ------------------------------------------------------------------------
    THREADING

    start_packet, send_packet, send_packets, send and abandon_packet are safe
    to call from any thread, concurrently, without external locking. That is
    the point of them.

    Everything else — create, destroy, update, connect, disconnect, flow
    membership, stats — is single threaded and belongs on whichever thread owns
    the object. update() is where received packets are dispatched and where
    timeouts fire, so callbacks always arrive on the thread that called
    update().

    ------------------------------------------------------------------------
    ERRORS

    Every call that can fail returns int: ROCKETNET_OK, or a negative
    ROCKETNET_ERROR_* value. Nothing throws, nothing longjmps, nothing aborts
    on a caller's mistake once asserts are compiled out. A failed start leaves
    the caller's rocketnet_packet_t zeroed rather than half-filled.
*/

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

#define ROCKETNET_VERSION_FULL                             "0.2.0"
#define ROCKETNET_VERSION_MAJOR_INT                              0
#define ROCKETNET_VERSION_MINOR_INT                              2
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

// The floor every transport must deliver. A transport that cannot guarantee at
// least this MTU is refused at create time with ROCKETNET_ERROR_MTU_TOO_SMALL
// rather than degraded around. The number sits deliberately below ethernet's
// 1500 so there is room for IP and UDP headers, tunnel overhead in the wild,
// and RocketNet's own framing and tricks — that margin is reserved budget, not
// slack to optimize away.
//
// Steady-state splitting does NOT use this number. It uses the MTU the live
// transport actually reports, which is usually larger. The floor exists so
// protocol constants that must hold everywhere — the handshake fits in one
// transport packet, a small packet never fragments — can be sized against a
// number that is true on every transport.
#define ROCKETNET_MIN_TRANSPORT_MTU                           1400

// A packet of at most this many bytes rides in a single transport packet on
// EVERY conforming transport, guaranteed, no fragmentation. Sized from the MTU
// floor with RocketNet's framing budget already subtracted. Above this, whether
// a packet fragments depends on the live transport's MTU.
#define ROCKETNET_SMALL_PACKET_BYTES                          1280

// Hard ceiling on a single packet, fragmented or not. Bounds reassembly memory
// per client so a peer cannot announce a gigabyte and make the server hold the
// pieces.
#define ROCKETNET_MAX_PACKET_BYTES                         1048576

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

// The destination does not name a connected client or a live flow.
#define ROCKETNET_ERROR_INVALID_DESTINATION                     -5

#define ROCKETNET_ERROR_UNKNOWN_FLOW                            -6
#define ROCKETNET_ERROR_INVALID_PACKET_TYPE                     -7

// The send pool is full. Every started packet is either waiting to go out or
// waiting to be acknowledged. Retry next tick, or send less.
#define ROCKETNET_ERROR_POOL_EXHAUSTED                          -8

// This one client has reached max_pool_bytes_per_client. Its own backlog is
// the problem, not the pool's. See the occupancy note on that field.
#define ROCKETNET_ERROR_QUOTA_EXCEEDED                          -9

// The reliable send queue for this destination is full. On a LATEST packet
// this cannot happen, because a newer packet replaces the older one rather
// than queueing behind it.
#define ROCKETNET_ERROR_QUEUE_FULL                             -10

#define ROCKETNET_ERROR_PACKET_TOO_LARGE                       -11

// The packet handle does not refer to a live started packet. Either it was
// already sent or abandoned, or it is a stale copy of a handle whose slot has
// since been reused. This is the generation counter earning its keep: without
// it the same mistake would be a silent write into someone else's packet.
#define ROCKETNET_ERROR_STALE_PACKET                           -12

// The two ends do not agree on the configuration the handshake hashes:
// max_packet_bytes and protocol version. Refused at connect, not discovered in
// a playtest.
#define ROCKETNET_ERROR_CONFIG_MISMATCH                        -13

#define ROCKETNET_ERROR_TOKEN_INVALID                          -14
#define ROCKETNET_ERROR_TOKEN_EXPIRED                          -15
#define ROCKETNET_ERROR_SERVER_FULL                            -16
#define ROCKETNET_ERROR_TRANSPORT_UNAVAILABLE                  -17

// The transport reported an MTU below ROCKETNET_MIN_TRANSPORT_MTU. Creation
// refuses; RocketNet does not run degraded on a transport that cannot meet the
// floor.
#define ROCKETNET_ERROR_MTU_TOO_SMALL                          -18

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
// TRANSPORTS
//
// The whole reason this library exists is that these differ wildly in what
// they cost. The contract above them never changes; only the ceiling does. A
// transport swap is one config value, and the entire test suite runs
// identically over every transport, so a swap is proven rather than hoped.
//
// Each transport reports its properties — MTU, batch limits, what it offloads
// — and the layers above adapt to what is reported rather than assuming. A
// property the core needs is queried, never hardcoded, so a swap can never
// silently violate an assumption.
// ---------------------------------------------------------------------------

// Pick the fastest transport this build and this machine actually support at
// runtime, falling back quietly toward SOCKET. This is what you want unless
// you are measuring.
#define ROCKETNET_TRANSPORT_AUTO                                 0

// Ordinary UDP sockets. Available everywhere, slowest, and the reference
// against which the others are measured. Also the negative control: a behavior
// that differs between SOCKET and any other transport is a transport bug by
// definition.
#define ROCKETNET_TRANSPORT_SOCKET                               1

// Linux sendmmsg with UDP generic segmentation offload, receive side GRO.
// Unprivileged, so it runs where kernel bypass is forbidden — this is the
// GameLift path.
#define ROCKETNET_TRANSPORT_GSO                                  2

// Linux io_uring. Unprivileged, batched, and the modern default on any recent
// kernel that is not allowed AF_XDP.
#define ROCKETNET_TRANSPORT_IO_URING                             3

// Linux AF_XDP kernel bypass. The ceiling. Requires privileges and a
// cooperating NIC; see the platform documentation, because the setup
// requirements are real and a server that cannot meet them should be asking
// for AUTO rather than failing.
#define ROCKETNET_TRANSPORT_XDP                                  4

// Windows IO completion ports.
#define ROCKETNET_TRANSPORT_IOCP                                 5

// Windows registered IO, the fastest path Windows offers.
#define ROCKETNET_TRANSPORT_RIO                                  6

// macOS and iOS kqueue.
#define ROCKETNET_TRANSPORT_KQUEUE                               7

// A transport the caller supplies as a function table in the config. This is
// how the loss and reorder soak harness works — it is just another transport
// behind the same seam, not special test machinery — and it is the door for
// platforms this header has not met yet.
#define ROCKETNET_TRANSPORT_CUSTOM                               8

// What a transport tells the core about itself. Queried once at create and
// after that treated as truth: packet splitting, coalescing budgets and batch
// sizes all derive from these numbers, per transport, at runtime.
typedef struct rocketnet_transport_properties_t
{
    // Largest transport packet this transport can carry, in bytes. Must be at
    // least ROCKETNET_MIN_TRANSPORT_MTU or creation refuses.
    int mtu;

    // Most transport packets accepted per send or receive call.
    int max_batch;

    // True if the transport slices a large buffer into transport packets
    // itself (GSO). The core hands it one large write instead of many small
    // ones and the kernel does the splitting.
    bool segmentation_offload;

    // True if started packet memory is the transport's own ring memory (XDP),
    // so sending is descriptor submission with no copy anywhere.
    bool zero_copy;

} rocketnet_transport_properties_t;

// One wire datagram, in the form a custom transport sends and receives.
typedef struct rocketnet_transport_packet_t
{
    const struct rocketnet_address_t * address;     // to, on send; from, on receive
    uint8_t * data;
    int bytes;

} rocketnet_transport_packet_t;

// A caller-supplied transport: ROCKETNET_TRANSPORT_CUSTOM plus this table in
// the config. The conformance bar is the standard test suite, which must pass
// over a custom transport exactly as it does over SOCKET.
typedef struct rocketnet_transport_interface_t
{
    void * context;

    int (*properties)( void * context, struct rocketnet_transport_properties_t * properties );

    int (*send)( void * context, const struct rocketnet_transport_packet_t * transport_packets, int num_transport_packets );

    int (*receive)( void * context, struct rocketnet_transport_packet_t * transport_packets, int max_transport_packets, int * num_transport_packets );

} rocketnet_transport_interface_t;

// ---------------------------------------------------------------------------
// Packet types — the three delivery disciplines
//
// The type is named per packet, at start, and that is the whole ceremony:
// there are no channels to configure and nothing for two ends to agree on. A
// packet's type is carried by RocketNet, not readable in the payload, and a
// receiver does not see it — by the time a packet is delivered, its discipline
// has already happened.
// ---------------------------------------------------------------------------

// Sent once. May be lost. No ordering owed, no retransmission, no
// acknowledgement tracking, and the cheapest of the three. A fragmented
// unreliable packet is all-or-nothing: missing one fragment when the
// reassembly window passes drops the packet whole, never delivers it partial.
#define ROCKETNET_PACKET_UNRELIABLE                              0

// Delivered eventually and in order, retransmitted until acknowledged.
//
// Ordering is per destination: reliable packets to one client arrive in the
// order they were sent, and reliable packets on one flow arrive in the order
// they were sent, but two destinations never block each other. Fragmented
// reliable packets are acknowledged and resent per fragment, so one lost
// fragment of a large packet costs one fragment rather than the whole packet.
//
// OPEN, deliberately: within a single destination, a large fragmented reliable
// packet delays smaller reliable packets sent after it. If a real workload
// needs unrelated reliable streams to one client, the answer is lightweight
// stream ids on this type — not before a real workload asks.
#define ROCKETNET_PACKET_RELIABLE                                1

// Reliable, most recent only. The newest packet supersedes every older one, on
// both sides, and both halves matter:
//
//   On receive, a packet older than one already delivered is discarded rather
//   than delivered late.
//
//   On send, a newer packet queued behind an older one that has not yet gone
//   out REPLACES it rather than queueing after it. A backlog collapses to a
//   single current value and the bandwidth is saved outright rather than spent
//   on state nobody will use.
//
// Supersession is per destination. This is the discipline for anything whose
// value is its freshness — input, a pose, a heartbeat.
//
// Superseding a started-but-unsent packet releases its memory, so on this type
// send does not mean the bytes will be transmitted. It means they are the
// current value until something newer arrives. A caller that needs to know
// what actually went out reads packets_superseded in stats.
#define ROCKETNET_PACKET_LATEST                                  2

// ---------------------------------------------------------------------------
// Client states
//
// Negative is terminal failure, zero is idle, positive is progress. Lifted
// from netcode.io deliberately: the security model comes across whole and so
// does its vocabulary.
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
// Opaque handles and identity
//
// Forward declarations only. The caller never sees a field and the layout is
// free to change between patch releases without breaking anyone.
// ---------------------------------------------------------------------------

struct rocketnet_client_t;
struct rocketnet_server_t;
struct rocketnet_address_t;

// On a server, a connected peer is a CLIENT INDEX: a plain int in
// [0, max_clients). An index is live from its connect callback to its
// disconnect callback and is reused after that, so an index held across a
// disconnect names somebody else — react to the disconnect callback rather
// than holding indices loose. This is the netcode.io slot model, kept because
// an int crosses every language boundary as itself and indexes a plain array
// on the other side.
//
// A flow is likewise named by a plain integer.
typedef uint32_t rocketnet_flow_id_t;

#define ROCKETNET_INVALID_FLOW_ID                               ((rocketnet_flow_id_t)0xFFFFFFFF)

// A destination: one uint64 that names either a client or a flow. High bit
// clear, the low bits are a client index. High bit set, the low bits are a
// flow id. Build them with the constructors below rather than by hand; the
// encoding is public so bindings can inline it, not so call sites can get
// creative with it.
typedef uint64_t rocketnet_destination_t;

#define ROCKETNET_DESTINATION_FLOW_BIT                          (1ULL << 63)

ROCKETNET_EXPORT_FUNC rocketnet_destination_t rocketnet_destination_client( int client_index );

ROCKETNET_EXPORT_FUNC rocketnet_destination_t rocketnet_destination_flow( rocketnet_flow_id_t flow_id );

// ---------------------------------------------------------------------------
// The started packet
//
// This is the piece with no equivalent in netcode or Network Next, and it is
// where the performance story lives.
//
// It is a value, not a pointer, and it is blittable, so a C# caller receives
// it as a struct and turns `data` and `bytes` into a Span<byte> over native
// memory with no marshalling and no copy. Passing it by pointer to send rather
// than by value is deliberate: it keeps the calling convention identical on
// every ABI this has to survive, including the Windows one where a 24-byte
// struct by value is already an implicit pointer.
//
// `token` is opaque and carries the pool, the slot within it, and a generation
// counter for that slot. The caller must never interpret it or do arithmetic
// on it. The generation counter is what turns "sent this twice" from memory
// corruption into ROCKETNET_ERROR_STALE_PACKET.
// ---------------------------------------------------------------------------

typedef struct rocketnet_packet_t
{
    // Writable packet memory owned by the transport. Valid from a successful
    // start until the matching send or abandon, and NOT valid after. Writing
    // through it after send is a use-after-free that no assert will catch,
    // because the memory is real and belongs to someone else by then.
    uint8_t * data;

    // Opaque. Pool, slot and generation. Do not interpret.
    uint64_t token;

    // Capacity, in bytes. Never less than the bytes requested at start; may be
    // more, because the pool allocates in size classes. Writing beyond this is
    // the caller's bug and the library cannot see it.
    int bytes;

} rocketnet_packet_t;

// ---------------------------------------------------------------------------
// A received packet, in the batch form
//
// The default receive path is the callback in the config struct, where the
// pointer's lifetime is obvious because it ends when the call returns. This
// struct is the opt-in for callers who genuinely need to hold received bytes
// past the callback — see rocketnet_server_receive.
// ---------------------------------------------------------------------------

typedef struct rocketnet_received_packet_t
{
    // Read-only packet memory owned by the transport. Valid until the matching
    // release. Reassembled fragments are contiguous here; the caller never
    // sees a fragment.
    const uint8_t * data;

    // Opaque. Pass back to release.
    uint64_t token;

    // The sender, on a server. Zero on a client, where there is only one peer.
    int client_index;

    int bytes;

} rocketnet_received_packet_t;

// ---------------------------------------------------------------------------
// Batch send descriptor
//
// One entry per started packet, so a server with many different unicast
// packets to place — a thousand per-client deltas per tick — pays one
// managed-to-native transition for the lot rather than one each. The
// destination was named at start; this only finishes them.
// ---------------------------------------------------------------------------

typedef struct rocketnet_packet_send_t
{
    rocketnet_packet_t packet;
    int bytes;                      // bytes actually written, <= packet.bytes
    int result;                     // filled by the library: ROCKETNET_OK or an error

} rocketnet_packet_send_t;

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

// A packet arrived. `data` is valid FOR THE DURATION OF THIS CALL AND NOT
// AFTER — copy out, or act on it here. This is the default and the recommended
// shape precisely because that sentence is short enough to be obeyed.
//
// There is deliberately no type argument and no flow argument: the delivery
// discipline already did its work before this fired, and which flow carried a
// packet is a server-side concept the client never holds. If the game needs to
// route, the first byte of the packet is the game's to spend.
//
// Called from inside update(), on the thread that called update(). Do not call
// destroy from here.
typedef void (*rocketnet_client_receive_callback_t)( struct rocketnet_client_t * client,
                                                     void * context,
                                                     const uint8_t * data,
                                                     int bytes );

typedef void (*rocketnet_server_receive_callback_t)( struct rocketnet_server_t * server,
                                                     void * context,
                                                     int client_index,
                                                     const uint8_t * data,
                                                     int bytes );

// A client completed the handshake and now occupies a client index.
// `user_data` is the ROCKETNET_USER_DATA_BYTES the backend sealed into the
// connect token, which is how the game learns who this is without trusting
// anything the client said. Valid for the duration of the call.
//
// Called from inside rocketnet_server_update().
typedef void (*rocketnet_server_connect_callback_t)( struct rocketnet_server_t * server,
                                                     void * context,
                                                     int client_index,
                                                     const uint8_t * user_data );

// A client disconnected, for any reason: it left, it timed out, the server
// disconnected it, or it exceeded its pool quota. `reason` is a
// ROCKETNET_ERROR_* value or ROCKETNET_OK for a clean disconnect.
//
// The index is dead when this returns and every flow subscription it had is
// already dropped. Called from inside rocketnet_server_update().
typedef void (*rocketnet_server_disconnect_callback_t)( struct rocketnet_server_t * server,
                                                        void * context,
                                                        int client_index,
                                                        int reason );

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

typedef struct rocketnet_client_config_t
{
    // "0.0.0.0:0" binds an ephemeral port, which is what a game client wants.
    char bind_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];

    // Must match the server's. Cheap protection against a stale build talking
    // to a current one.
    uint64_t protocol_id;

    // No traffic for this long ends the connection.
    float timeout_seconds;

    // Send a keep-alive if nothing else has gone out for this long.
    float keep_alive_seconds;

    // Largest single packet, either direction. Both ends must agree — it is
    // hashed into the handshake and a mismatch is refused with
    // ROCKETNET_ERROR_CONFIG_MISMATCH — because reassembly memory on each end
    // is sized from it, per peer.
    int max_packet_bytes;

    // Total startable send memory for this client, in bytes. A client sends
    // little and this can be small.
    int pool_bytes;

    // ROCKETNET_TRANSPORT_*. AUTO unless you are measuring.
    int transport;

    // Required when transport is ROCKETNET_TRANSPORT_CUSTOM, ignored
    // otherwise. The table is copied; its context pointer is the caller's to
    // keep alive.
    const struct rocketnet_transport_interface_t * transport_interface;

    int transport_send_buffer_bytes;
    int transport_receive_buffer_bytes;

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

    int max_clients;
    int max_flows;

    float timeout_seconds;
    float keep_alive_seconds;

    // See the client config note. Hashed into the handshake.
    int max_packet_bytes;

    // Total startable send memory across the whole server. This is the pool
    // whose occupancy is bounded, and it is the number to size against the
    // thousand-player target rather than against a comfortable margin.
    int pool_bytes;

    // The hard per-client bound, and the reason the pool cannot be pinned.
    //
    // A flow packet lives until every subscriber has acknowledged it or timed
    // out, so one written packet is referenced by many per-client delivery
    // states, and one bad client that acknowledges nothing would otherwise
    // hold the whole pool hostage on behalf of everyone else. When a client's
    // referenced bytes exceed this, THAT CLIENT is disconnected — not the
    // packet dropped, not the pool grown, not everyone stalled. The cost of a
    // bad peer falls on the bad peer.
    int max_pool_bytes_per_client;

    // ROCKETNET_TRANSPORT_*. AUTO unless you are measuring.
    int transport;

    // Required when transport is ROCKETNET_TRANSPORT_CUSTOM.
    const struct rocketnet_transport_interface_t * transport_interface;

    int transport_send_buffer_bytes;
    int transport_receive_buffer_bytes;

    // Number of send/receive queues, which is the going-wide dial. One queue
    // per core that will be sending is the shape this is built for. Zero means
    // pick a reasonable number from the machine.
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
//
// Two units appear here and they are never mixed: packets are the caller's
// sends and receives, transport packets are the wire's.
// ---------------------------------------------------------------------------

typedef struct rocketnet_connection_stats_t
{
    float rtt_ms;
    float jitter_ms;
    float packet_loss_percent;              // of transport packets

    uint64_t transport_packets_sent;
    uint64_t transport_packets_received;
    uint64_t transport_packets_acked;
    uint64_t transport_packets_lost;

    uint64_t packets_sent;
    uint64_t packets_received;

    // Packets a LATEST send discarded because something newer replaced them.
    // Not a loss — this is the discipline working, and the number is how you
    // see it working.
    uint64_t packets_superseded;

    uint64_t bytes_sent;
    uint64_t bytes_received;

    float bandwidth_kbps_up;
    float bandwidth_kbps_down;

    // Pool bytes this client currently keeps alive, against
    // max_pool_bytes_per_client. Watch this one: it is where a slow peer shows
    // up before it becomes a problem.
    int pool_bytes_referenced;

} rocketnet_connection_stats_t;

typedef struct rocketnet_server_stats_t
{
    int num_clients;
    int num_flows;

    // Pool occupancy across the whole server, and its high-water mark since
    // create. If the high-water mark reaches pool_bytes the server has been
    // failing starts and the number to change is pool_bytes.
    int pool_bytes_in_use;
    int pool_bytes_high_water;

    uint64_t packets_started;
    uint64_t packets_start_failed;

    uint64_t transport_packets_sent;
    uint64_t transport_packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;

    // Transport packets the security layer threw away before they touched
    // anything: bad MAC, replayed sequence, unknown sender. A rising number
    // here is an attack or a bug and it is worth alerting on.
    uint64_t transport_packets_rejected;

    float bandwidth_kbps_up;
    float bandwidth_kbps_down;

    // What actually resolved, which is not always what was asked for.
    // ROCKETNET_TRANSPORT_AUTO in the config becomes a concrete value here.
    int transport;

    // What the live transport reported. The number splitting is using.
    int transport_mtu;

} rocketnet_server_stats_t;

// ---------------------------------------------------------------------------
// Global lifecycle
//
// There is no global config struct, unlike Network Next, because there is
// nothing global to configure: no backend hostname, no buyer keys, no service
// to reach. Everything that varies varies per client or per server.
// ---------------------------------------------------------------------------

// Call once before anything else. `context` is handed to the allocator hooks.
// Returns ROCKETNET_OK or an error.
ROCKETNET_EXPORT_FUNC int rocketnet_init( void * context );

// Call once at shutdown, after every client and server is destroyed.
// Destroying them after this is undefined.
ROCKETNET_EXPORT_FUNC void rocketnet_term( void );

// Seconds since rocketnet_init, from the platform's monotonic clock. update()
// reads this itself; it is exposed because a caller pacing its own loop should
// not have to find a second clock that might disagree with ours.
ROCKETNET_EXPORT_FUNC double rocketnet_time( void );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_protocol_version( void );

// Human-readable text for a ROCKETNET_ERROR_* value. Static storage; never
// null, even for a value this library does not recognise.
ROCKETNET_EXPORT_FUNC const char * rocketnet_error_string( int result );

// True if this build and this machine can actually run the given transport
// right now — the XDP privileges are present, the kernel is the right vintage.
// Ask before configuring anything other than AUTO.
ROCKETNET_EXPORT_FUNC bool rocketnet_transport_available( int transport );

ROCKETNET_EXPORT_FUNC const char * rocketnet_transport_string( int transport );

// ---------------------------------------------------------------------------
// Hooks
//
// The library imposes neither malloc nor a logging system on its host. Set
// these before rocketnet_init.
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

// Parses "1.2.3.4:5000", "[::1]:5000" or a bare address. Returns ROCKETNET_OK
// or ROCKETNET_ERROR_INVALID_ARGUMENT. Does not resolve hostnames — that is a
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
// The netcode.io model, unchanged, because this is not a place to be creative:
// a backend the client cannot forge issues a short-lived token carrying
// per-client keys, the client presents it, the server validates it against the
// private key it shares with that backend, and an attacker gets nothing to
// amplify.
//
// This function belongs in the BACKEND, not in the game server and never in
// the client. It is in this header because the backend links this library too.
// ---------------------------------------------------------------------------

// Writes ROCKETNET_CONNECT_TOKEN_BYTES into `output`.
//
// `server_addresses` is the list the client may try, in order, which is how a
// backend hands out a fleet rather than a machine. `user_data` is
// ROCKETNET_USER_DATA_BYTES the server receives in its connect callback,
// sealed so the client can neither read nor alter it — this is where identity
// goes, and it is why the server never has to believe a client about who it
// is.
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

// Fills `key` with ROCKETNET_KEY_BYTES from the platform CSPRNG. Use it to
// make the private key a backend and a server share; do not invent one.
ROCKETNET_EXPORT_FUNC int rocketnet_random_key( uint8_t * key );

// ===========================================================================
// CLIENT
// ===========================================================================

// Creates a client. Allocates once, here, from the configured allocator;
// nothing after this allocates on the send or receive path.
//
// Returns NULL on failure, having logged why at ROCKETNET_LOG_LEVEL_ERROR.
// `context` is handed back in every callback. The config is copied, so it may
// be a stack struct.
ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context,
                                                                           const struct rocketnet_client_config_t * config );

// Destroys the client, disconnecting first if connected and sending the
// disconnect so the server does not have to wait for a timeout. Any packet
// still started is released. Safe on NULL.
ROCKETNET_EXPORT_FUNC void rocketnet_client_destroy( struct rocketnet_client_t * client );

// Begins connecting with a token from the backend. Returns ROCKETNET_OK if the
// token parsed and the attempt started — NOT if it succeeded. Poll
// rocketnet_client_state, which goes negative on failure.
ROCKETNET_EXPORT_FUNC int rocketnet_client_connect( struct rocketnet_client_t * client, const uint8_t * connect_token );

// Disconnects cleanly, telling the server rather than letting it time out.
// Returns to ROCKETNET_CLIENT_STATE_DISCONNECTED. Reusable: connect again with
// a fresh token.
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

// Starts a packet of `bytes` bytes with the given type. A client sends to its
// server; there is no destination. On success `packet->data` is yours to write
// until send or abandon, and `packet->bytes` is at least `bytes`.
//
// Cost: one atomic increment on the pool. No lock, no allocation, no syscall.
// Safe from any thread.
//
// Fails with ROCKETNET_ERROR_POOL_EXHAUSTED — retry next tick — or
// ROCKETNET_ERROR_PACKET_TOO_LARGE or _INVALID_PACKET_TYPE. On failure
// `*packet` is zeroed.
ROCKETNET_EXPORT_FUNC int rocketnet_client_start_packet( struct rocketnet_client_t * client,
                                                         int type,
                                                         int bytes,
                                                         struct rocketnet_packet_t * packet );

// Sends `bytes` of the started packet. The caller's ownership of the memory
// ends here whether this succeeds or fails; the handle is dead either way and
// must not be sent again.
//
// `bytes` may be less than what was started — start generously and send what
// you wrote. It may not be more.
//
// Cost: enqueue and one atomic. Nothing is copied, ever. Safe from any thread.
//
// Fails with ROCKETNET_ERROR_STALE_PACKET, _NOT_CONNECTED or _QUEUE_FULL. On
// _QUEUE_FULL the packet is dropped and its memory released: there is nothing
// useful the caller could do with a handle to bytes the queue has refused.
ROCKETNET_EXPORT_FUNC int rocketnet_client_send_packet( struct rocketnet_client_t * client,
                                                        const struct rocketnet_packet_t * packet,
                                                        int bytes );

// Gives a started packet back unsent. The only correct way to abandon one.
ROCKETNET_EXPORT_FUNC int rocketnet_client_abandon_packet( struct rocketnet_client_t * client,
                                                           const struct rocketnet_packet_t * packet );

// Start, copy, send in one call, for callers who already have the bytes
// somewhere else and cannot write in place.
//
// Cost: one copy of `bytes`. That is the whole difference from the start path
// and it is why this is the convenience rather than the default. It is here
// because a caller with an existing array should not have to pretend
// otherwise, and because for a 40-byte input packet the copy is beneath
// measurement.
ROCKETNET_EXPORT_FUNC int rocketnet_client_send( struct rocketnet_client_t * client,
                                                 int type,
                                                 const uint8_t * data,
                                                 int bytes );

// The opt-in receive form, for callers who must hold received bytes past the
// callback. Set receive_callback to NULL in the config and call this instead;
// setting both is ROCKETNET_ERROR_INVALID_ARGUMENT, because two receive paths
// racing over the same packets is a bug waiting to be written.
//
// Fills up to `max_packets` entries and writes the count to `num_packets`.
// Every filled entry MUST be released, and each one holds pool memory until it
// is. Returns ROCKETNET_OK even when nothing arrived.
//
// Cost: one transition for the batch instead of one per packet, which is the
// point of it for a C# caller.
ROCKETNET_EXPORT_FUNC int rocketnet_client_receive( struct rocketnet_client_t * client,
                                                    struct rocketnet_received_packet_t * packets,
                                                    int max_packets,
                                                    int * num_packets );

// Releases packets from rocketnet_client_receive. Releasing an
// already-released packet returns ROCKETNET_ERROR_STALE_PACKET rather than
// corrupting the pool.
ROCKETNET_EXPORT_FUNC int rocketnet_client_release( struct rocketnet_client_t * client,
                                                    const struct rocketnet_received_packet_t * packets,
                                                    int num_packets );

// Fills `stats`. Returns false if the client is not connected, leaving `stats`
// untouched. Never allocates.
ROCKETNET_EXPORT_FUNC bool rocketnet_client_stats( struct rocketnet_client_t * client, struct rocketnet_connection_stats_t * stats );

// ===========================================================================
// SERVER
// ===========================================================================

// Creates a server, binds its transport and verifies the transport meets the
// MTU floor. Returns NULL on failure, having logged why.
//
// A note that matters for anyone shipping this: nothing here reconfigures the
// machine. RocketNet will not disable hyperthreading, reassign NIC queues,
// unload another process's XDP programs or shell out to a compiler. If the XDP
// transport needs the machine prepared, it says so and fails, and preparing it
// is an operator's job with an operator's tools. A library that reconfigures
// its host is not one another studio can deploy.
ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context,
                                                                           const struct rocketnet_server_config_t * config );

// Disconnects every client cleanly, releases everything, and destroys the
// server. Safe on NULL.
ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server );

// Receives, dispatches, acknowledges, retransmits, ages out timeouts, and
// fires connect and disconnect callbacks. Call once per tick.
//
// With num_queues > 1 the receive work happens on the library's own queue
// threads, but callbacks are still delivered from inside this call on this
// thread, so a caller's game state is never touched concurrently. That is
// deliberate: going wide is for the IO, not for the caller's invariants.
ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC void rocketnet_server_flush( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC uint16_t rocketnet_server_port( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC const struct rocketnet_address_t * rocketnet_server_address( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_connected_clients( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC bool rocketnet_server_client_connected( struct rocketnet_server_t * server, int client_index );

// The client_id sealed in this client's connect token by the backend.
ROCKETNET_EXPORT_FUNC uint64_t rocketnet_server_client_id( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC const struct rocketnet_address_t * rocketnet_server_client_address( struct rocketnet_server_t * server, int client_index );

// Disconnects one client cleanly and fires the disconnect callback before
// returning. The index is free for reuse afterwards.
ROCKETNET_EXPORT_FUNC int rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_all( struct rocketnet_server_t * server );

// ---------------------------------------------------------------------------
// Server send: start, write, send
// ---------------------------------------------------------------------------

// Starts a packet of `bytes` bytes with the given type, addressed to
// `destination` — one client or one flow, same argument. On success
// `packet->data` is yours to write until send or abandon.
//
// Cost: one atomic increment on the pool. No lock, no allocation, no syscall.
// Safe from any thread, which is the reason the start is a single atomic
// rather than a lock: worker threads building packets in parallel is the shape
// this exists for.
//
// A packet started for a flow is written ONCE and delivered to every
// subscriber, each of which takes a reference to the same bytes rather than a
// copy of them. That is the broadcast story, and it is why start is where the
// destination goes: the library knows at write time that these bytes will fan
// out, and the caller never enumerates recipients anywhere.
//
// Fails with ROCKETNET_ERROR_POOL_EXHAUSTED, _QUOTA_EXCEEDED,
// _INVALID_DESTINATION, _INVALID_PACKET_TYPE or _PACKET_TOO_LARGE. On failure
// `*packet` is zeroed.
ROCKETNET_EXPORT_FUNC int rocketnet_server_start_packet( struct rocketnet_server_t * server,
                                                         rocketnet_destination_t destination,
                                                         int type,
                                                         int bytes,
                                                         struct rocketnet_packet_t * packet );

// Sends `bytes` of the started packet to the destination named at start. The
// caller's ownership of the memory ends here, success or failure.
//
// Cost: enqueue and one atomic. Nothing is copied. Safe from any thread.
//
// The memory is NOT free when this returns. A reliable packet lives until
// acknowledged — by every flow subscriber, for a flow — and a latest packet
// lives until superseded or sent. That is the consequence of retransmission
// and it is why send does not mean "done with it" — see
// max_pool_bytes_per_client.
//
// Sending to a flow with no subscribers is not an error. The memory is
// released and the call returns ROCKETNET_OK, because "nobody is looking at
// this region" is a normal state of the world and not a caller mistake.
ROCKETNET_EXPORT_FUNC int rocketnet_server_send_packet( struct rocketnet_server_t * server,
                                                        const struct rocketnet_packet_t * packet,
                                                        int bytes );

// Sends many started packets in one call, so a C# caller pays one transition
// rather than N — this is the thousand-per-client-deltas-per-tick path. Each
// entry's `result` is filled in; the call itself returns ROCKETNET_OK if it
// was well-formed, even when individual entries failed. Every packet is
// consumed either way.
ROCKETNET_EXPORT_FUNC int rocketnet_server_send_packets( struct rocketnet_server_t * server,
                                                         struct rocketnet_packet_send_t * sends,
                                                         int num_sends );

// Gives a started packet back unsent.
ROCKETNET_EXPORT_FUNC int rocketnet_server_abandon_packet( struct rocketnet_server_t * server,
                                                           const struct rocketnet_packet_t * packet );

// Start, copy, send in one call. Costs one copy of `bytes`; see
// rocketnet_client_send.
ROCKETNET_EXPORT_FUNC int rocketnet_server_send( struct rocketnet_server_t * server,
                                                 rocketnet_destination_t destination,
                                                 int type,
                                                 const uint8_t * data,
                                                 int bytes );

// ---------------------------------------------------------------------------
// Flows
//
// A flow is a named set of subscribed clients that the server publishes to
// without enumerating recipients at the call site: build the destination with
// rocketnet_destination_flow and every send entry point above just works.
//
// Two reasons it exists rather than the caller keeping its own index array.
// The small one is cost: a thousand-player server rebuilding a
// thousand-element array per region per tick is work that buys nothing. The
// large one is that this is the shape a relay fan-out needs, so a server
// written against flows today keeps working unchanged when the payload goes to
// a relay instead of out N times — and a server written against explicit index
// arrays does not.
//
// In THIS version a flow is fanned out by the server itself as N authenticated
// unicast sends. There is no relay and no IP multicast anywhere in this
// library. The API is shaped so adding one later does not change these
// signatures.
//
// SUBSCRIPTION IS AN OPERATION THE SERVER PERFORMS, and there is deliberately
// no client-side subscribe call anywhere in this header. That is a security
// property, not an inconvenience: in a competitive game, interest management
// is an anti-cheat boundary, and a client that could subscribe itself to the
// flow covering the far side of the map has a wallhack implemented at the
// protocol level. Having no client-initiated path deletes that attack surface
// rather than defending it. A client discovers what it is subscribed to by
// receiving it — no flow identity is carried in anything a client sees.
//
// Deciding WHICH flows a client should be in is game logic and stays above
// this library, which carries subscriptions and never computes them.
//
// Ordering on a reliable flow is per subscriber, from subscription onward: a
// client subscribed mid-stream receives packets started after its
// subscription, in order, and nothing from before it.
// ---------------------------------------------------------------------------

// Creates a flow. Returns ROCKETNET_INVALID_FLOW_ID if max_flows is reached.
ROCKETNET_EXPORT_FUNC rocketnet_flow_id_t rocketnet_server_flow_create( struct rocketnet_server_t * server );

// Destroys a flow. Subscribers are dropped; packets already sent to it still
// finish being delivered, because they belong to the clients now rather than
// to the flow.
ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_destroy( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscribe( struct rocketnet_server_t * server,
                                                           rocketnet_flow_id_t flow_id,
                                                           int client_index );

// The batch form, for the interest-management pass that moves many clients
// between flows at once.
ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscribe_many( struct rocketnet_server_t * server,
                                                                rocketnet_flow_id_t flow_id,
                                                                const int * client_indices,
                                                                int num_clients );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_unsubscribe( struct rocketnet_server_t * server,
                                                             rocketnet_flow_id_t flow_id,
                                                             int client_index );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_unsubscribe_many( struct rocketnet_server_t * server,
                                                                  rocketnet_flow_id_t flow_id,
                                                                  const int * client_indices,
                                                                  int num_clients );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscriber_count( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id );

// Fills `client_indices` with up to `max_clients` subscribers and writes the
// count.
ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscribers( struct rocketnet_server_t * server,
                                                             rocketnet_flow_id_t flow_id,
                                                             int * client_indices,
                                                             int max_clients,
                                                             int * num_clients );

// ---------------------------------------------------------------------------
// Server receive and statistics
// ---------------------------------------------------------------------------

// The opt-in receive form; see rocketnet_client_receive for the contract. Set
// receive_callback to NULL in the config to use this.
ROCKETNET_EXPORT_FUNC int rocketnet_server_receive( struct rocketnet_server_t * server,
                                                    struct rocketnet_received_packet_t * packets,
                                                    int max_packets,
                                                    int * num_packets );

ROCKETNET_EXPORT_FUNC int rocketnet_server_release( struct rocketnet_server_t * server,
                                                    const struct rocketnet_received_packet_t * packets,
                                                    int num_packets );

// Fills `stats` for the server as a whole. Always true on a live server.
ROCKETNET_EXPORT_FUNC bool rocketnet_server_stats( struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats );

// Fills `stats` for one client. False if the index is not connected, leaving
// `stats` untouched. Never allocates.
ROCKETNET_EXPORT_FUNC bool rocketnet_server_client_stats( struct rocketnet_server_t * server,
                                                          int client_index,
                                                          struct rocketnet_connection_stats_t * stats );

#endif // #ifndef ROCKETNET_H
