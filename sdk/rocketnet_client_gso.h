/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gso transport's client side on Linux (sdk/rocketnet_client_gso_linux.cpp).
//
// The client sends every datagram to its one connected server (TRANSPORT.md
// t11), so a send batch is runs of equal-sized datagrams to one destination:
// each run of rocketnet_coalesce_send_run (sdk/rocketnet_coalesce.h) leaves as
// one sendmmsg message carrying a UDP_SEGMENT cmsg, and the kernel cuts it at
// the stride. UDP_SEGMENT is probed at create as the gso server probes it
// (sdk/rocketnet_server_gso_linux.cpp, the runtime probes); a refused probe is
// the plain tier, one datagram per message, still one sendmmsg per batch. A
// live EINVAL, EIO or EOPNOTSUPP on a segmented message flips the client to
// the plain tier for good and resends from that message's first packet, so
// the degradation loses nothing (the server's live-refusal rule).
//
// On receive UDP_GRO is probed at create by enabling it; a refused probe is
// the plain tier, the platform's recvmmsg. With GRO on, each recvmmsg takes
// one message into a 64 KiB coalesce buffer (a coalesced message can fill the
// whole of it, so one buffer holds one message), with its own control area for
// the UDP_GRO cmsg, and rocketnet_coalesce_split cuts it at the stride the
// kernel reports into the I/O thread's receive slots, in arrival order. A
// receive buffer with no room for every segment drops the rest, counted in
// gro_packets_dropped. A truncated control area or a UDP_GRO cmsg too short
// to hold its int is one fault (gro_cmsg_faults) and the message is one
// datagram, as the gso server reads it (sdk/rocketnet_server_gso_linux.cpp).
//
// The I/O thread of docs/CLIENT-IO.md (proofs/tla/RNClientIo.tla) owns the socket and
// calls the send path; the sender adds no state to that model: a send batch
// is one synchronous call that returns with every datagram sent or counted
// dropped, and the tier flag is the I/O thread's alone.
//
// The send path reaches the kernel only through rocketnet_client_gso_syscalls_t,
// so the unit cases (tests/test_unit_client_gso.cpp) drive the batching, the
// run split and the fallback with a fake socket: no socket, no thread, no time.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CLIENT_GSO_H
#define ROCKETNET_SDK_ROCKETNET_CLIENT_GSO_H

#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_address.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <atomic>

struct rocketnet_platform_send_t;
struct rocketnet_client_transport_io_operations_t;

// datagrams in one sendmmsg call: the client I/O batch (ROCKETNET_CLIENT_IO_BATCH)
#define ROCKETNET_CLIENT_GSO_SEND_BATCH                                      64
// segments in one UDP_SEGMENT message: the kernel's UDP_MAX_SEGMENTS floor
#define ROCKETNET_CLIENT_GSO_MAX_SEGMENTS                                    64
// payload bytes in one UDP_SEGMENT message, under the 64 KiB IP envelope
#define ROCKETNET_CLIENT_GSO_MAX_SUPER_BYTES                              65000

// segments in one UDP_GRO message: the kernel's UDP_GRO_CNT_MAX
#define ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS                            64
// bytes of the receive coalesce buffer: the 64 KiB IP envelope
#define ROCKETNET_CLIENT_GSO_COALESCE_BUFFER_BYTES                        65536

// The syscall seam. sendmmsg returns the messages sent, or -1 with errno set;
// segment_probe returns 0 when the socket accepts UDP_SEGMENT. recvmmsg
// returns the messages received, 0 or -1 with errno set (EAGAIN: none
// waiting); gro_enable returns 0 when the socket turned UDP_GRO on;
// read_address converts a received source and returns false for one the
// socket does not speak.
struct rocketnet_client_gso_syscalls_t
{
    int ( *sendmmsg )( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages );
    int ( *segment_probe )( void * context, int socket );
    int ( *recvmmsg )( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages );
    int ( *gro_enable )( void * context, int socket );
    bool ( *read_address )( void * context, const struct sockaddr_storage * storage, struct rocketnet_address_t * from );
};

// the kernel's: sendmmsg(2), setsockopt(UDP_SEGMENT, 0), recvmmsg(2) with
// MSG_DONTWAIT, setsockopt(UDP_GRO, 1), and the platform socket's source
// conversion (the syscall context is the rocketnet_platform_socket_t)
extern const rocketnet_client_gso_syscalls_t rocketnet_client_gso_kernel_syscalls;

// One message's control block: room for the UDP_SEGMENT cmsg, aligned as a
// cmsghdr is (its first member is a size_t)
struct rocketnet_client_gso_control_t
{
    alignas( size_t ) char bytes[CMSG_SPACE( sizeof( uint16_t ) )];
};

// The receive message's control area: room for the UDP_GRO cmsg (an int)
struct rocketnet_client_gso_receive_control_t
{
    alignas( size_t ) char bytes[CMSG_SPACE( sizeof( int ) )];
};

// The send path's state. Everything is sized here and lives in the transport's
// one create-time allocation; a send allocates nothing. I/O thread only, but
// for packet_send_failures, which any thread reads.
struct rocketnet_client_gso_t
{
    const rocketnet_client_gso_syscalls_t * syscalls;
    void * syscall_context;
    int socket;
    bool segment;                                       // UDP_SEGMENT accepted and never refused live

    uint64_t packets_sent;
    uint64_t send_syscalls;
    uint64_t segment_messages_sent;                     // sendmmsg messages that carried UDP_SEGMENT
    uint64_t segment_refusals;                          // live refusals that flipped the tier
    std::atomic<uint64_t> packet_send_failures;         // datagrams a refused sendmmsg dropped

    // per call scratch, one entry per message or per packet
    struct mmsghdr messages[ROCKETNET_CLIENT_GSO_SEND_BATCH];
    struct iovec packet_iovecs[ROCKETNET_CLIENT_GSO_SEND_BATCH];
    struct rocketnet_client_gso_control_t controls[ROCKETNET_CLIENT_GSO_SEND_BATCH];
    int message_first_packet[ROCKETNET_CLIENT_GSO_SEND_BATCH];
    int message_num_packets[ROCKETNET_CLIENT_GSO_SEND_BATCH];
    int run_packet_bytes[ROCKETNET_CLIENT_GSO_SEND_BATCH];
    struct rocketnet_address_t run_to[ROCKETNET_CLIENT_GSO_SEND_BATCH];

    // the receive path, I/O thread only
    bool gro;                                           // UDP_GRO on: receive through the coalesce buffer
    uint64_t receive_syscalls;
    uint64_t packets_received;
    uint64_t gro_batches;                               // received messages that held more than one datagram
    uint64_t gro_cmsg_faults;                           // truncated control areas and short UDP_GRO cmsgs
    uint64_t gro_packets_dropped;                       // segments past the room the receive buffer had
    struct mmsghdr receive_message;
    struct iovec receive_iovec;
    struct sockaddr_storage receive_name;
    struct rocketnet_client_gso_receive_control_t receive_control;
    int segment_offsets[ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS];
    int segment_lengths[ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS];
    alignas( 16 ) uint8_t coalesce_buffer[ROCKETNET_CLIENT_GSO_COALESCE_BUFFER_BYTES];
};

// Sets up gso for socket, which is connected to its one server before the
// first send. probe_segment false keeps the plain tier without asking the
// kernel. Returns whether UDP_SEGMENT is on.
bool rocketnet_client_gso_init( struct rocketnet_client_gso_t * gso, const rocketnet_client_gso_syscalls_t * syscalls, void * syscall_context, int socket, bool probe_segment );

// Sends num_packets datagrams to the connected server: sendmmsg in batches of
// ROCKETNET_CLIENT_GSO_SEND_BATCH, each equal-sized run one UDP_SEGMENT message.
// Borrows sends for the call only. Returns with every datagram sent or counted
// in packet_send_failures.
void rocketnet_client_gso_send_batch( struct rocketnet_client_gso_t * gso, const struct rocketnet_platform_send_t * sends, int num_packets );

// Sets up the receive path: probe_gro asks the kernel to turn UDP_GRO on,
// false keeps the plain tier without asking. Returns whether UDP_GRO is on.
// Call after rocketnet_client_gso_init.
bool rocketnet_client_gso_init_receive( struct rocketnet_client_gso_t * gso, bool probe_gro );

// The GRO stride of one received message of message_bytes: the UDP_GRO cmsg's
// int, or message_bytes when there is none. A truncated control area or a
// UDP_GRO cmsg too short to hold an int adds one to *faults and is
// message_bytes: the message is one datagram.
int rocketnet_client_gso_gro_segment_bytes( const struct msghdr * header, int message_bytes, uint64_t * faults );

// Receives into at most max_packets slots of packet_stride bytes from
// packet_data, with each source in from[i] and each true size in
// packet_bytes[i] (a datagram past packet_stride keeps its prefix): one
// recvmmsg message per call into the coalesce buffer, split at its stride, in
// arrival order. A message is read only while its segments can fit, but the
// first, which always is: its segments past max_packets are dropped and
// counted in gro_packets_dropped. Returns the slots filled.
int rocketnet_client_gso_receive_batch( struct rocketnet_client_gso_t * gso, struct rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );

// The gso client transport operations, the client column of the gso row.
extern const rocketnet_client_transport_io_operations_t rocketnet_client_gso_operations;

#if ROCKETNET_DEV
// Test seam: a client created while this is set keeps the plain tier, and the
// segmented messages every gso client has sent, summed.
extern std::atomic<bool> rocketnet_client_gso_test_disable_segment;
extern std::atomic<uint64_t> rocketnet_client_gso_test_segment_messages_sent;
// a client created while this is set keeps plain recvmmsg, and the GRO
// batches (messages of more than one datagram) every gso client received, summed
extern std::atomic<bool> rocketnet_client_gso_test_disable_gro;
extern std::atomic<uint64_t> rocketnet_client_gso_test_gro_batches;
#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#endif // ROCKETNET_SDK_ROCKETNET_CLIENT_GSO_H
