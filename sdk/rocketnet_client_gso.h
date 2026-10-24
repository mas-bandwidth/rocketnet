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
// Receive is recvmmsg in this card; UDP_GRO on receive is rn-client-gso-linux-b.
//
// The I/O thread of docs/CLIENT-IO.md (tla/RNClientIo.tla) owns the socket and
// calls the send path; the sender adds no state to that model: a send batch
// is one synchronous call that returns with every datagram sent or counted
// dropped, and the tier flag is the I/O thread's alone.
//
// The send path reaches the kernel only through rocketnet_client_gso_syscalls_t,
// so the unit cases (tests/test_unit_client_gso.cpp) drive the batching, the
// run split and the fallback with a fake socket: no socket, no thread, no time.

#ifndef ROCKETNET_CLIENT_GSO_H
#define ROCKETNET_CLIENT_GSO_H

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

// The syscall seam. sendmmsg returns the messages sent, or -1 with errno set;
// segment_probe returns 0 when the socket accepts UDP_SEGMENT.
struct rocketnet_client_gso_syscalls_t
{
    int (*sendmmsg)( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages );
    int (*segment_probe)( void * context, int socket );
};

// the kernel's: sendmmsg(2) and setsockopt(UDP_SEGMENT, 0)
extern const rocketnet_client_gso_syscalls_t rocketnet_client_gso_kernel_syscalls;

// One message's control block: room for the UDP_SEGMENT cmsg, aligned as a
// cmsghdr is (its first member is a size_t)
struct rocketnet_client_gso_control_t
{
    alignas( size_t ) char bytes[CMSG_SPACE( sizeof( uint16_t ) )];
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

// The gso client transport operations, the client column of the gso row.
extern const rocketnet_client_transport_io_operations_t rocketnet_client_gso_operations;

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
// Test seam: a client created while this is set keeps the plain tier, and the
// segmented messages every gso client has sent, summed.
extern std::atomic<bool> rocketnet_client_gso_test_disable_segment;
extern std::atomic<uint64_t> rocketnet_client_gso_test_segment_messages_sent;
#endif

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#endif // ROCKETNET_CLIENT_GSO_H
