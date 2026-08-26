/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    You send PACKETS: reliable in-order, latest (reliable, most recent only),
    or unreliable. The wire carries TRANSPORT PACKETS, built by a swappable
    TRANSPORT (sockets, GSO, io_uring, AF_XDP, ...). A server addresses any
    send to a DESTINATION: one client, or one flow of subscribed clients.
*/

#ifndef ROCKETNET_H
#define ROCKETNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ROCKETNET_VERSION_FULL                             "0.2.0"
#define ROCKETNET_VERSION_MAJOR_INT                              0
#define ROCKETNET_VERSION_MINOR_INT                              2
#define ROCKETNET_VERSION_PATCH_INT                              0

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

// every transport must deliver at least this MTU, or create refuses
#define ROCKETNET_MIN_TRANSPORT_MTU                           1400

// packets up to this size never fragment, on any transport
#define ROCKETNET_SMALL_PACKET_BYTES                          1280

// hard ceiling on a single packet, fragmented or not
#define ROCKETNET_MAX_PACKET_BYTES                         1048576

#define ROCKETNET_KEY_BYTES                                     32
#define ROCKETNET_CONNECT_TOKEN_BYTES                         2048
#define ROCKETNET_USER_DATA_BYTES                              256
#define ROCKETNET_MAX_ADDRESS_STRING_LENGTH                    256

#define ROCKETNET_OK                                             0
#define ROCKETNET_ERROR                                         -1
#define ROCKETNET_ERROR_INVALID_ARGUMENT                        -2
#define ROCKETNET_ERROR_OUT_OF_MEMORY                           -3
#define ROCKETNET_ERROR_NOT_CONNECTED                           -4
#define ROCKETNET_ERROR_INVALID_DESTINATION                     -5
#define ROCKETNET_ERROR_POOL_EXHAUSTED                          -6
#define ROCKETNET_ERROR_QUOTA_EXCEEDED                          -7
#define ROCKETNET_ERROR_QUEUE_FULL                              -8
#define ROCKETNET_ERROR_PACKET_TOO_LARGE                        -9
#define ROCKETNET_ERROR_STALE_PACKET                           -10
#define ROCKETNET_ERROR_TRANSPORT_UNAVAILABLE                  -11
#define ROCKETNET_ERROR_MTU_TOO_SMALL                          -12

#define ROCKETNET_LOG_LEVEL_NONE                                 0
#define ROCKETNET_LOG_LEVEL_ERROR                                1
#define ROCKETNET_LOG_LEVEL_WARN                                 2
#define ROCKETNET_LOG_LEVEL_INFO                                 3
#define ROCKETNET_LOG_LEVEL_DEBUG                                4
#define ROCKETNET_LOG_LEVEL_SPAM                                 5

#define ROCKETNET_TRANSPORT_AUTO                                 0        // fastest available, falls back to SOCKET
#define ROCKETNET_TRANSPORT_SOCKET                               1        // plain UDP sockets. everywhere. the reference
#define ROCKETNET_TRANSPORT_GSO                                  2        // linux sendmmsg + UDP GSO/GRO. unprivileged
#define ROCKETNET_TRANSPORT_IO_URING                             3        // linux io_uring. unprivileged
#define ROCKETNET_TRANSPORT_XDP                                  4        // linux AF_XDP kernel bypass. the ceiling
#define ROCKETNET_TRANSPORT_IOCP                                 5        // windows IO completion ports
#define ROCKETNET_TRANSPORT_RIO                                  6        // windows registered IO
#define ROCKETNET_TRANSPORT_KQUEUE                               7        // macOS and iOS
#define ROCKETNET_TRANSPORT_CUSTOM                               8        // caller-supplied function table

#define ROCKETNET_PACKET_UNRELIABLE                              0        // sent once, may be lost
#define ROCKETNET_PACKET_RELIABLE                                1        // delivered, in order per destination
#define ROCKETNET_PACKET_LATEST                                  2        // reliable, most recent only. newer supersedes older

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

struct rocketnet_client_t;
struct rocketnet_server_t;
struct rocketnet_address_t;

typedef uint32_t rocketnet_flow_id_t;

#define ROCKETNET_INVALID_FLOW_ID                               ((rocketnet_flow_id_t)0xFFFFFFFF)

// a destination names one client (high bit clear) or one flow (high bit set)
typedef uint64_t rocketnet_destination_t;

#define ROCKETNET_DESTINATION_FLOW_BIT                          (1ULL << 63)

ROCKETNET_EXPORT_FUNC rocketnet_destination_t rocketnet_destination_client( int client_index );
ROCKETNET_EXPORT_FUNC rocketnet_destination_t rocketnet_destination_flow( rocketnet_flow_id_t flow_id );

// a started packet: write into data, then send or abandon exactly once
typedef struct rocketnet_packet_t
{
    uint8_t * data;                 // valid from start until send or abandon
    uint64_t token;                 // opaque
    int bytes;                      // capacity, >= bytes requested at start
} rocketnet_packet_t;

// a received packet. hold until release
typedef struct rocketnet_received_packet_t
{
    const uint8_t * data;
    uint64_t token;                 // opaque
    int client_index;               // sender on a server, zero on a client
    int bytes;
} rocketnet_received_packet_t;

typedef struct rocketnet_packet_send_t
{
    rocketnet_packet_t packet;
    int bytes;                      // bytes written, <= packet.bytes
    int result;                     // filled by the library
} rocketnet_packet_send_t;

// what a transport reports about itself. packet splitting derives from these
typedef struct rocketnet_transport_properties_t
{
    int mtu;                        // >= ROCKETNET_MIN_TRANSPORT_MTU
    int max_batch;                  // most transport packets per send/receive call
    bool segmentation_offload;      // transport slices large writes itself (GSO)
    bool zero_copy;                 // packet memory is ring memory (XDP)
} rocketnet_transport_properties_t;

typedef struct rocketnet_transport_packet_t
{
    const struct rocketnet_address_t * address;     // to on send, from on receive
    uint8_t * data;
    int bytes;
} rocketnet_transport_packet_t;

// a custom transport. must pass the standard test suite, same as SOCKET
typedef struct rocketnet_transport_interface_t
{
    void * context;
    int (*properties)( void * context, struct rocketnet_transport_properties_t * properties );
    int (*send)( void * context, const struct rocketnet_transport_packet_t * transport_packets, int num_transport_packets );
    int (*receive)( void * context, struct rocketnet_transport_packet_t * transport_packets, int max_transport_packets, int * num_transport_packets );
} rocketnet_transport_interface_t;

// callbacks fire inside rocketnet_server_update, on the calling thread
typedef void (*rocketnet_server_connect_callback_t)( struct rocketnet_server_t * server, void * context, int client_index, const uint8_t * user_data );
typedef void (*rocketnet_server_disconnect_callback_t)( struct rocketnet_server_t * server, void * context, int client_index );

// pass NULL to create for all defaults. timeout and protocol id come from the
// connect token; everything else the server decides
typedef struct rocketnet_client_config_t
{
    char bind_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];             // default "0.0.0.0:0"
    int transport;                                                      // default ROCKETNET_TRANSPORT_AUTO
    const struct rocketnet_transport_interface_t * transport_interface; // CUSTOM only
} rocketnet_client_config_t;

typedef struct rocketnet_server_config_t
{
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];           // the address clients are told to reach
    char bind_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];             // the address actually bound. default = server_address
    uint64_t protocol_id;
    uint8_t private_key[ROCKETNET_KEY_BYTES];                           // shared with the backend that issues connect tokens
    int max_clients;
    int max_flows;                                                      // 0 = default
    int max_packet_bytes;                                               // 0 = ROCKETNET_MAX_PACKET_BYTES
    int pool_bytes;                                                     // send memory. 0 = sized from max_clients
    int num_queues;                                                     // 0 = from the machine
    int transport;                                                      // default ROCKETNET_TRANSPORT_AUTO
    const struct rocketnet_transport_interface_t * transport_interface; // CUSTOM only
    rocketnet_server_connect_callback_t connect_callback;
    rocketnet_server_disconnect_callback_t disconnect_callback;
} rocketnet_server_config_t;

// packets = the caller's unit. transport packets = the wire's
typedef struct rocketnet_connection_stats_t
{
    float rtt_ms;
    float jitter_ms;
    float packet_loss_percent;
    uint64_t transport_packets_sent;
    uint64_t transport_packets_received;
    uint64_t transport_packets_acked;
    uint64_t transport_packets_lost;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_superseded;    // LATEST replaced by newer. not a loss
    uint64_t bytes_sent;
    uint64_t bytes_received;
    float bandwidth_kbps_up;
    float bandwidth_kbps_down;
    int pool_bytes_referenced;      // a slow peer shows up here first
} rocketnet_connection_stats_t;

typedef struct rocketnet_server_stats_t
{
    int num_clients;
    int num_flows;
    int pool_bytes_in_use;
    int pool_bytes_high_water;
    uint64_t packets_started;
    uint64_t packets_start_failed;
    uint64_t transport_packets_sent;
    uint64_t transport_packets_received;
    uint64_t transport_packets_rejected;    // bad MAC, replay, unknown sender
    uint64_t bytes_sent;
    uint64_t bytes_received;
    float bandwidth_kbps_up;
    float bandwidth_kbps_down;
    int transport;                  // what AUTO resolved to
    int transport_mtu;              // what the live transport reported
} rocketnet_server_stats_t;

// ---------------------------------------------------------------------------

ROCKETNET_EXPORT_FUNC int rocketnet_init( void * context );

ROCKETNET_EXPORT_FUNC void rocketnet_term( void );

ROCKETNET_EXPORT_FUNC double rocketnet_time( void );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_protocol_version( void );

ROCKETNET_EXPORT_FUNC const char * rocketnet_error_string( int result );

ROCKETNET_EXPORT_FUNC bool rocketnet_transport_available( int transport );

ROCKETNET_EXPORT_FUNC const char * rocketnet_transport_string( int transport );

ROCKETNET_EXPORT_FUNC void rocketnet_allocator( void * (*malloc_function)( void * context, size_t bytes ), void (*free_function)( void * context, void * p ) );

ROCKETNET_EXPORT_FUNC void rocketnet_log_level( int level );

ROCKETNET_EXPORT_FUNC void rocketnet_log_function( void (*function)( int level, const char * format, ... ) );

ROCKETNET_EXPORT_FUNC void rocketnet_quiet( bool flag );

ROCKETNET_EXPORT_FUNC void rocketnet_assert_function( void (*function)( const char * condition, const char * function, const char * file, int line ) );

// ---------------------------------------------------------------------------

ROCKETNET_EXPORT_FUNC int rocketnet_address_parse( struct rocketnet_address_t * address, const char * string );

ROCKETNET_EXPORT_FUNC const char * rocketnet_address_to_string( const struct rocketnet_address_t * address, char * buffer );

ROCKETNET_EXPORT_FUNC bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b );

ROCKETNET_EXPORT_FUNC size_t rocketnet_address_bytes( void );

// ---------------------------------------------------------------------------

// netcode.io connect token model. this belongs in the backend, never the client
ROCKETNET_EXPORT_FUNC int rocketnet_generate_connect_token( uint64_t protocol_id,
                                                            uint64_t client_id,
                                                            int expire_seconds,
                                                            int timeout_seconds,
                                                            int num_server_addresses,
                                                            const char ** server_addresses,
                                                            const uint8_t * private_key,
                                                            const uint8_t * user_data,
                                                            uint8_t * output );

ROCKETNET_EXPORT_FUNC int rocketnet_random_key( uint8_t * key );

// ---------------------------------------------------------------------------

ROCKETNET_EXPORT_FUNC struct rocketnet_client_t * rocketnet_client_create( void * context, const struct rocketnet_client_config_t * config );

ROCKETNET_EXPORT_FUNC void rocketnet_client_destroy( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC int rocketnet_client_connect( struct rocketnet_client_t * client, const uint8_t * connect_token );

ROCKETNET_EXPORT_FUNC void rocketnet_client_disconnect( struct rocketnet_client_t * client );

ROCKETNET_EXPORT_FUNC int rocketnet_client_state( struct rocketnet_client_t * client );

// call once per tick
ROCKETNET_EXPORT_FUNC void rocketnet_client_update( struct rocketnet_client_t * client );

// start, write packet->data, send. zero copy. safe from any thread
ROCKETNET_EXPORT_FUNC int rocketnet_client_start_packet( struct rocketnet_client_t * client, int type, int bytes, struct rocketnet_packet_t * packet );

ROCKETNET_EXPORT_FUNC int rocketnet_client_send_packet( struct rocketnet_client_t * client, const struct rocketnet_packet_t * packet, int bytes );

ROCKETNET_EXPORT_FUNC int rocketnet_client_abandon_packet( struct rocketnet_client_t * client, const struct rocketnet_packet_t * packet );

// start, copy, send in one call, for bytes you already have somewhere else
ROCKETNET_EXPORT_FUNC int rocketnet_client_send( struct rocketnet_client_t * client, int type, const uint8_t * data, int bytes );

// fills up to max_packets received packets. release every filled entry
ROCKETNET_EXPORT_FUNC int rocketnet_client_receive( struct rocketnet_client_t * client, struct rocketnet_received_packet_t * packets, int max_packets, int * num_packets );

ROCKETNET_EXPORT_FUNC int rocketnet_client_release( struct rocketnet_client_t * client, const struct rocketnet_received_packet_t * packets, int num_packets );

ROCKETNET_EXPORT_FUNC bool rocketnet_client_stats( struct rocketnet_client_t * client, struct rocketnet_connection_stats_t * stats );

// ---------------------------------------------------------------------------

ROCKETNET_EXPORT_FUNC struct rocketnet_server_t * rocketnet_server_create( void * context, const struct rocketnet_server_config_t * config );

ROCKETNET_EXPORT_FUNC void rocketnet_server_destroy( struct rocketnet_server_t * server );

// call once per tick. connect and disconnect callbacks fire from inside here
ROCKETNET_EXPORT_FUNC void rocketnet_server_update( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC int rocketnet_server_num_connected_clients( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC bool rocketnet_server_client_connected( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC uint64_t rocketnet_server_client_id( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC const struct rocketnet_address_t * rocketnet_server_client_address( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC int rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index );

ROCKETNET_EXPORT_FUNC void rocketnet_server_disconnect_all( struct rocketnet_server_t * server );

// start, write packet->data, send. zero copy. safe from any thread.
// a flow packet is written once and fanned out to every subscriber
ROCKETNET_EXPORT_FUNC int rocketnet_server_start_packet( struct rocketnet_server_t * server, rocketnet_destination_t destination, int type, int bytes, struct rocketnet_packet_t * packet );

ROCKETNET_EXPORT_FUNC int rocketnet_server_send_packet( struct rocketnet_server_t * server, const struct rocketnet_packet_t * packet, int bytes );

// send many started packets in one call
ROCKETNET_EXPORT_FUNC int rocketnet_server_send_packets( struct rocketnet_server_t * server, struct rocketnet_packet_send_t * sends, int num_sends );

ROCKETNET_EXPORT_FUNC int rocketnet_server_abandon_packet( struct rocketnet_server_t * server, const struct rocketnet_packet_t * packet );

// start, copy, send in one call
ROCKETNET_EXPORT_FUNC int rocketnet_server_send( struct rocketnet_server_t * server, rocketnet_destination_t destination, int type, const uint8_t * data, int bytes );

// flows live on the server only. clients just receive packets
ROCKETNET_EXPORT_FUNC rocketnet_flow_id_t rocketnet_server_flow_create( struct rocketnet_server_t * server );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_destroy( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscribe( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id, int client_index );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_unsubscribe( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id, int client_index );

ROCKETNET_EXPORT_FUNC int rocketnet_server_flow_subscriber_count( struct rocketnet_server_t * server, rocketnet_flow_id_t flow_id );

// fills up to max_packets received packets. release every filled entry
ROCKETNET_EXPORT_FUNC int rocketnet_server_receive( struct rocketnet_server_t * server, struct rocketnet_received_packet_t * packets, int max_packets, int * num_packets );

ROCKETNET_EXPORT_FUNC int rocketnet_server_release( struct rocketnet_server_t * server, const struct rocketnet_received_packet_t * packets, int num_packets );

ROCKETNET_EXPORT_FUNC bool rocketnet_server_stats( struct rocketnet_server_t * server, struct rocketnet_server_stats_t * stats );

ROCKETNET_EXPORT_FUNC bool rocketnet_server_client_stats( struct rocketnet_server_t * server, int client_index, struct rocketnet_connection_stats_t * stats );

#endif // #ifndef ROCKETNET_H
