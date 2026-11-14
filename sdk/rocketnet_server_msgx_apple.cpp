/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include <new>

#include "rocketnet_internal.h"

#if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// The macOS batched server transport. rocketnet_server_msgx.h has the shape;
// this file is the machinery.
//
// Threads and what each owns:
// - main: the socket table (opens, retires, closes), each worker's producer
//   index and its publish, the receive claim.
// - fill threads: claim slots in the producer bank of the destination's
//   worker, stamp each with its client's socket slot and generation.
// - transmit worker w (holder w of the table): takes its published banks in
//   order, groups each bank by socket slot keeping staged order, sends each
//   run with one sendmsg_x on the client's connected socket, the rest on the
//   shared socket, inside one table pass per bank.
// - the receive worker (holder num_workers): a kqueue over the shared socket
//   and every connected socket, recvmsg_x batches, the prefilter before the
//   bank, one table pass per kevent round.

#include "rocketnet_server_msgx.h"
#include "rocketnet_msgx_socket_table.h"
#include "rocketnet_msgx_send_runs.h"
#include "rocketnet_msgx_receive_fill.h"
#include "rocketnet_buffer_handoff.h"
#include "rocketnet_receive_prefilter.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <string.h>
#include <atomic>

// struct msghdr_x is the syscall ABI from xnu bsd/sys/socket_private.h, as
// sdk/rocketnet_platform.cpp declares it; the calls are resolved by dlsym

struct rocketnet_server_msgx_msghdr_x_t
{
    void * msg_name;
    socklen_t msg_namelen;
    struct iovec * msg_iov;
    int msg_iovlen;
    void * msg_control;
    socklen_t msg_controllen;
    int msg_flags;
    size_t msg_datalen;
};

typedef ssize_t ( *rocketnet_server_msgx_send_fn )( int s, const struct rocketnet_server_msgx_msghdr_x_t * msgp, u_int cnt, int flags );
typedef ssize_t ( *rocketnet_server_msgx_recv_fn )( int s, struct rocketnet_server_msgx_msghdr_x_t * msgp, u_int cnt, int flags );

// IMPORTANT: the 4096 byte walls keep fields different threads write on
// different cache lines, as in the gso and xdp transports.

struct rocketnet_server_msgx_send_info_t
{
    rocketnet_address_t to;
    int packet_bytes;
    int table_slot;                 // -1: the shared socket
    uint32_t generation;            // the table slot's generation at the stamp
    uint8_t padding[64 - sizeof( rocketnet_address_t ) - 3 * sizeof( int )];
};

static_assert( sizeof( rocketnet_server_msgx_send_info_t ) == 64, "one send info per cache line" );

struct rocketnet_server_msgx_send_bank_t
{
    uint8_t padding_0[4096];

    std::atomic<uint64_t> num_packets{ 0 };     // claims, refused at or past the bank's size

    uint8_t padding_1[4096];

    rocketnet_server_msgx_send_info_t info[ROCKETNET_MSGX_SEND_QUEUE_SIZE];

    uint8_t padding_2[4096];

    alignas( 16 ) uint8_t data[ROCKETNET_MSGX_SEND_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];
};

struct rocketnet_server_msgx_worker_t
{
    uint8_t padding_0[4096];

    struct rocketnet_server_msgx_t * msgx;
    int index;
    rocketnet_platform_thread_t * thread;
    rocketnet_platform_wake_t * wake;

    uint8_t padding_1[4096];

    // fill threads read producer; main writes it only at a publish, after
    // the fill threads joined (proofs/tla/RNBankFifoLayout.tla Publish)
    int producer;
    std::atomic<uint64_t> serial{ 0 };          // the worker's packet ids: serial * workers + index

    uint8_t padding_2[4096];

    // only bank indices cross this mutex (sdk/rocketnet_buffer_handoff.h)
    rocketnet_platform_mutex_t mutex = {};
    rocketnet_buffer_handoff_t<ROCKETNET_MSGX_TX_BANKS> handoff;
    std::atomic<bool> quit{ false };
    std::atomic<bool> shutdown{ false };
    std::atomic<bool> stopped{ true };
    std::atomic<bool> full{ false };

    uint8_t padding_3[4096];

    // the worker's own counters, read by main
    std::atomic<uint64_t> sent_connected{ 0 };
    std::atomic<uint64_t> sent_shared{ 0 };
    std::atomic<uint64_t> send_calls{ 0 };
    std::atomic<uint64_t> dropped{ 0 };
    std::atomic<uint64_t> refused{ 0 };
    std::atomic<uint64_t> retired{ 0 };
    std::atomic<uint64_t> dead{ 0 };
    std::atomic<uint64_t> staging_full{ 0 };    // fill threads

    uint8_t padding_4[4096];

    // worker only: the bank sorted by socket slot, staged order kept
    int order[ROCKETNET_MSGX_SEND_QUEUE_SIZE];
    int bucket[ROCKETNET_MSGX_TABLE_MAX_SLOTS + 2];
    struct rocketnet_server_msgx_msghdr_x_t messages[ROCKETNET_MSGX_SEND_BATCH];
    struct iovec iovecs[ROCKETNET_MSGX_SEND_BATCH];

    uint8_t padding_5[4096];

    struct rocketnet_server_msgx_send_bank_t banks[ROCKETNET_MSGX_TX_BANKS];
};

struct rocketnet_server_msgx_recv_bank_t
{
    uint32_t num_packets;
    uint64_t receive_calls;
    uint64_t refused;
    rocketnet_address_t from[ROCKETNET_MSGX_RECV_QUEUE_SIZE];
    int packet_bytes[ROCKETNET_MSGX_RECV_QUEUE_SIZE];
    alignas( 16 ) uint8_t packet_data[ROCKETNET_MSGX_RECV_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];
};

struct rocketnet_server_msgx_receiver_t
{
    uint8_t padding_0[4096];

    rocketnet_platform_thread_t * thread;
    std::atomic<bool> quit{ false };
    std::atomic<bool> stopped{ true };

    uint8_t padding_1[4096];

    // the mutex guards the handoff's roles and committed counts only
    // (sdk/rocketnet_receive_handoff.h, proofs/tla/RNMsgxReceiveFill.tla
    // Commit and Call); main's cursors in it are main's alone
    rocketnet_platform_mutex_t mutex = {};
    struct rocketnet_receive_handoff_t hand;
    uint64_t main_stopped = 0;      // main only: banked and never handed to the server at the stop, RECEIVE_DROP_STOPPED
    bool stop_counted = false;      // main only: StopCount has run
    uint64_t main_retired = 0;      // main only: datagrams read off a retired socket at its close, RECEIVE_DROP_RETIRED

    uint8_t padding_2[4096];

    struct rocketnet_receive_drop_tally_t tally;   // the worker writes, main sums

    uint8_t padding_3[4096];

    // worker only
    bool round_read;                // a read was made this round: the round ends with a commit
    struct kevent events[ROCKETNET_MSGX_RECV_EVENTS];
    struct rocketnet_server_msgx_msghdr_x_t messages[ROCKETNET_MSGX_RECV_BATCH];
    struct iovec iovecs[ROCKETNET_MSGX_RECV_BATCH];
    struct sockaddr_storage names[ROCKETNET_MSGX_RECV_BATCH];
    alignas( 16 ) uint8_t scratch[ROCKETNET_MSGX_RECV_BATCH][2048];

    uint8_t padding_4[4096];

    struct rocketnet_server_msgx_recv_bank_t banks[3];
};

struct rocketnet_server_msgx_t
{
    void * context;
    int num_workers;
    int max_clients;
    bool ipv6;
    bool stopping;
    rocketnet_address_t bind;                   // with the bound port: every client socket binds it
    uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES];

    rocketnet_server_msgx_send_fn sendmsg_x;
    rocketnet_server_msgx_recv_fn recvmsg_x;

    int shared_fd;
    int kq;
    uint64_t update;                            // main: the table's update count

    struct rocketnet_server_msgx_worker_t * workers[ROCKETNET_MSGX_MAX_WORKERS];
    struct rocketnet_server_msgx_receiver_t * receiver;
    struct rocketnet_msgx_socket_table_t * table;

    struct rocketnet_server_socket_process_packets_t process_packets;

    // main-folded receive totals
    uint64_t packets_received;
    uint64_t receive_calls;
    uint64_t receive_refused;
    int shared_receive_buffer;                  // SO_RCVBUF the kernel granted the shared socket

    // finishes refused: a pointer that names no slot of the producer bank, or
    // a finish after stop. Each is a packet lost, folded into SEND_FAILED as
    // the gso transport folds its own (rocketnet_server_gso_send_finish_refused)
    std::atomic<uint64_t> finish_refused{ 0 };

#if ROCKETNET_DEV
    // rocketnet_server_msgx_test_fail_sends: the next count send calls
    // (sendmsg_x or sendto, any worker) answer -1 with error
    std::atomic<int> test_fail_sends{ 0 };
    std::atomic<int> test_fail_send_error{ 0 };
#endif // #if ROCKETNET_DEV
};

// ---------------------------------------------------------------------------
// addresses
// ---------------------------------------------------------------------------

static socklen_t rocketnet_server_msgx_address_to_sockaddr( const rocketnet_address_t * input, bool socket_ipv6, struct sockaddr_storage * out )
{
    rocketnet_address_t to = *input;
    if ( socket_ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }
        struct sockaddr_in6 * addr = (struct sockaddr_in6 *) out;
        rocketnet_platform_address_to_sockaddr_in6( &to, addr );
        addr->sin6_len = sizeof( struct sockaddr_in6 );
        return sizeof( struct sockaddr_in6 );
    }
    if ( to.type != ROCKETNET_ADDRESS_IPV4 )
    {
        return 0;
    }
    memset( out, 0, sizeof( struct sockaddr_in ) );
    rocketnet_platform_address_to_sockaddr_in( &to, (struct sockaddr_in *) out );
    return sizeof( struct sockaddr_in );
}

static bool rocketnet_server_msgx_sockaddr_to_address( const struct sockaddr_storage * input, bool socket_ipv6, rocketnet_address_t * out )
{
    if ( input->ss_family == AF_INET6 )
    {
        const struct sockaddr_in6 * addr = (const struct sockaddr_in6 *) input;
        out->type = ROCKETNET_ADDRESS_IPV6;
        rocketnet_platform_sockaddr_in6_to_address( addr, out );
        return rocketnet_address_accept_received_source( out, socket_ipv6 );
    }
    if ( input->ss_family == AF_INET )
    {
        rocketnet_platform_sockaddr_in_to_address( (const struct sockaddr_in *) input, out );
        return rocketnet_address_accept_received_source( out, socket_ipv6 );
    }
    return false;
}

struct rocketnet_server_msgx_option_socket_t
{
    int fd;
};

static void rocketnet_server_msgx_set_socket_option( void * user, int level, int option_name, int value )
{
    const struct rocketnet_server_msgx_option_socket_t * socket_to_set = (const struct rocketnet_server_msgx_option_socket_t *) user;
    setsockopt( socket_to_set->fd, level, option_name, &value, sizeof( value ) );
}

// a non-blocking UDP socket bound to the server's address and port with
// SO_REUSEADDR and SO_REUSEPORT, so the shared socket and every client's
// connected socket hold one port. -errno on failure.
//
// The shared socket binds first with neither option and sets them after the
// bind (as the Linux transports set SO_REUSEPORT): a bind with them set skips
// the wildcard check, so an ephemeral port could be one another process holds
// on another address or family, and an explicit port could join another
// process's group; either way the more specific socket takes the datagrams.
// Set after the bind, the bind itself never lands on a port another socket
// holds (the IPv6 wildcard under an IPv4 reservation, as portable binds it),
// and the client sockets still join it.
//
// The port is NOT this server's alone once it is bound. Measured on macOS for
// one user: a later socket that sets SO_REUSEPORT binds the same port, and
// unicast goes to the most specific match, so a socket on the server's own
// address, or an AF_INET socket beside a dual-stack shared socket, takes every
// datagram meant for the shared socket (handshakes included). A socket on a
// specific address with SO_REUSEADDR does the same to any wildcard UDP socket
// on macOS, this transport's or not. A socket with neither option is refused.
// Whether another user's process can join was not measured; docs/MACOS.md
// says what this means for a server on a shared machine.
static int rocketnet_server_msgx_open_bound( const rocketnet_address_t * bind_address, bool ipv6, int buffer_bytes, bool shared )
{
    const int fd = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( fd < 0 )
    {
        return -errno;
    }

    int one = 1;
    int zero = 0;
    bool ok = fcntl( fd, F_SETFD, FD_CLOEXEC ) == 0;
    ok = ok && fcntl( fd, F_SETFL, fcntl( fd, F_GETFL, 0 ) | O_NONBLOCK ) == 0;
    ok = ok && ( shared || setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) ) == 0 );
    ok = ok && ( shared || setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) == 0 );
    ok = ok && setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof( one ) ) == 0;
    ok = ok && ( !ipv6 || setsockopt( fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof( zero ) ) == 0 );
    // Darwin refuses a buffer above kern.ipc.maxsockbuf's share outright: the
    // platform's halving walk takes the most it grants (the shared socket
    // warns with what was asked and granted)
    ok = ok && rocketnet_platform_set_socket_buffer_size( fd, SO_SNDBUF, buffer_bytes, "send", shared );
    if ( !ok )
    {
        const int error = errno;
        close( fd );
        return -error;
    }

    if ( shared && ipv6 )
    {
        // the IPv6 wildcard binds under an IPv4 reservation of its port, as
        // the portable socket does: Darwin would otherwise bind it over a port
        // another socket holds in IPv4, and that socket keeps every IPv4
        // datagram. A client socket joins the port the shared socket owns.
        uint16_t bound_port = 0;
        const int error = rocketnet_platform_bind_dual_stack( fd, bind_address, &bound_port );
        if ( error != 0 )
        {
            close( fd );
            return -error;
        }
    }
    else
    {
        struct sockaddr_storage sockaddr;
        const socklen_t socklen = rocketnet_server_msgx_address_to_sockaddr( bind_address, ipv6, &sockaddr );
        if ( socklen == 0 || bind( fd, (struct sockaddr *) &sockaddr, socklen ) != 0 )
        {
            const int error = socklen == 0 ? EAFNOSUPPORT : errno;
            close( fd );
            return -error;
        }
    }

    if ( shared && ( setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) ) != 0 ||
                     setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) != 0 ) )
    {
        const int error = errno;
        close( fd );
        return -error;
    }

    struct rocketnet_server_msgx_option_socket_t option_socket = { fd };
    rocketnet_platform_socket_apply_dont_fragment( ipv6, rocketnet_server_msgx_set_socket_option, &option_socket );
    return fd;
}

// ---------------------------------------------------------------------------
// the socket table's kernel: open and close
// ---------------------------------------------------------------------------

static int rocketnet_server_msgx_table_open( void * user, const rocketnet_address_t * address )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;

    const int fd = rocketnet_server_msgx_open_bound( &msgx->bind, msgx->ipv6, ROCKETNET_MSGX_CLIENT_SOCKET_BUFFER, false );
    if ( fd < 0 )
    {
        return fd;
    }

    rocketnet_platform_set_socket_buffer_size( fd, SO_RCVBUF, ROCKETNET_MSGX_CLIENT_SOCKET_BUFFER, "receive", false );

    struct sockaddr_storage sockaddr;
    const socklen_t socklen = rocketnet_server_msgx_address_to_sockaddr( address, msgx->ipv6, &sockaddr );
    if ( socklen == 0 || connect( fd, (struct sockaddr *) &sockaddr, socklen ) != 0 )
    {
        const int error = socklen == 0 ? EAFNOSUPPORT : errno;
        close( fd );
        return -error;
    }
    return fd;
}

// the descriptor of a retired socket is closed here, on main. The poller forgot
// it at the retire, so what the kernel queued for it since reaches no one:
// every datagram it still holds is counted (RECEIVE_DROP_RETIRED) before the
// close. Later datagrams of that address reach the shared socket. The count
// is one syscall, getsockopt (SO_NUMRCVPKT, the datagrams in the receive
// buffer), not a read of each on main (R70); the kernel still walks the
// queued records to answer it, so it is not constant time. It is a count
// taken once before the close, not accounting atomic with concurrent arrivals: a
// datagram the kernel queues between the count and the close goes uncounted,
// the same boundary the bounded drain had. Only if the kernel refuses the
// option are they read and counted, bounded
static void rocketnet_server_msgx_table_close( void * user, int fd )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;
    int queued = 0;
    socklen_t queued_bytes = sizeof( queued );
    if ( getsockopt( fd, SOL_SOCKET, SO_NUMRCVPKT, &queued, &queued_bytes ) == 0 && queued >= 0 )
    {
        if ( msgx && msgx->receiver )
        {
            msgx->receiver->main_retired += (uint64_t) queued;
        }
        close( fd );
        return;
    }
    uint8_t scratch[ROCKETNET_FRAME_STRIDE];
    uint64_t drained = 0;
    for ( int i = 0; i < ROCKETNET_MSGX_RETIRED_DRAIN_LIMIT; i++ )
    {
        if ( recv( fd, scratch, sizeof( scratch ), MSG_DONTWAIT ) >= 0 )
        {
            drained++;
        }
        else if ( errno == EAGAIN || errno == EWOULDBLOCK )
        {
            break;
        }
        // any other error (a pending ICMP refusal, a truncated datagram) is
        // cleared by this read; the next read finds the next datagram
    }
    if ( msgx && msgx->receiver )
    {
        msgx->receiver->main_retired += drained;
    }
    close( fd );
}

// the receive poller forgets a descriptor before the table retires it, so a
// kevent round that starts after the retire never returns it; a round that
// started before is inside a pass the quarantine waits for
// (proofs/tla/RNMsgxSocketTable.tla Retire)
static void rocketnet_server_msgx_deregister( void * user, int fd )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;
    struct kevent change;
    EV_SET( &change, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL );
    kevent( msgx->kq, &change, 1, NULL, 0, NULL );
}

static bool rocketnet_server_msgx_register( struct rocketnet_server_msgx_t * msgx, int fd, int slot, uint32_t generation )
{
    // udata carries the slot and the generation, so a holder's dead report
    // names the socket it read (the table ignores a stale generation)
    const uintptr_t udata = ( (uintptr_t) generation << 32 ) | (uintptr_t) (uint32_t) ( slot + 1 );
    struct kevent change;
    EV_SET( &change, fd, EVFILT_READ, EV_ADD, 0, 0, (void *) udata );
    return kevent( msgx->kq, &change, 1, NULL, 0, NULL ) == 0;
}

// ---------------------------------------------------------------------------
// send: main and fill threads
// ---------------------------------------------------------------------------

int rocketnet_server_msgx_send_queues( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    return msgx->num_workers;
}

// a client's socket slot modulo the workers, else its address hash
// (sdk/rocketnet_msgx_socket_table.h queue_of), so the clients the table
// holds spread evenly across the workers
int rocketnet_server_msgx_send_queue_of( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * to )
{
    rocketnet_assert( msgx );
    int slot = -1;
    return rocketnet_msgx_socket_table_queue_of( msgx->table, to, msgx->num_workers, &slot );
}

int rocketnet_server_msgx_send_room_queue( struct rocketnet_server_msgx_t * msgx, int queue )
{
    rocketnet_assert( msgx );
    rocketnet_assert( queue >= 0 && queue < msgx->num_workers );
    struct rocketnet_server_msgx_worker_t * worker = msgx->workers[queue];
    if ( msgx->stopping || worker->producer < 0 )
    {
        return 0;
    }
    const uint64_t used = worker->banks[worker->producer].num_packets.load( std::memory_order_relaxed );
    return used >= ROCKETNET_MSGX_SEND_QUEUE_SIZE ? 0 : ROCKETNET_MSGX_SEND_QUEUE_SIZE - (int) used;
}

// proofs/tla/ServerStopSpread.tla Room: the least room of any worker, so a
// caller that spreads by send_room never overfills one
int rocketnet_server_msgx_send_room( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    int room = ROCKETNET_MSGX_SEND_QUEUE_SIZE;
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        const int queue_room = rocketnet_server_msgx_send_room_queue( msgx, w );
        room = queue_room < room ? queue_room : room;
    }
    return room;
}

uint8_t * rocketnet_server_msgx_start_packet( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( msgx );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    if ( msgx->stopping )
    {
        return NULL;
    }

    // one lookup names the worker and the slot the stamp takes
    int table_slot = -1;
    const int w = rocketnet_msgx_socket_table_queue_of( msgx->table, to, msgx->num_workers, &table_slot );
    struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
    if ( worker->producer < 0 )
    {
        return NULL;
    }
    struct rocketnet_server_msgx_send_bank_t * bank = &worker->banks[worker->producer];

    // one atomic add claims the slot; a claim at or past the bank's end is
    // refused and never written (proofs/tla/RNUringQueueBankLayout.tla Start)
    const uint64_t index = bank->num_packets.fetch_add( 1, std::memory_order_relaxed );
    if ( index >= ROCKETNET_MSGX_SEND_QUEUE_SIZE )
    {
        worker->staging_full.fetch_add( 1, std::memory_order_relaxed );
        return NULL;
    }
    const int slot = (int) index;

    rocketnet_server_msgx_send_info_t * info = &bank->info[slot];
    info->to = *to;
    info->packet_bytes = 0;
    info->table_slot = rocketnet_msgx_socket_table_stamp_slot( msgx->table, table_slot, &info->generation );

    *packet_id = worker->serial.fetch_add( 1, std::memory_order_relaxed ) * (uint64_t) msgx->num_workers + (uint64_t) w;
    return bank->data[slot];
}

// the producer slot a frame pointer names, or -1: the offset is checked
// before it is divided (proofs/tla/RNUringQueueBankLayout.tla SlotFrom)
static int rocketnet_server_msgx_slot_of( struct rocketnet_server_msgx_t * msgx, uint64_t packet_id, const uint8_t * packet_data,
                                          struct rocketnet_server_msgx_send_bank_t ** bank_out )
{
    const int w = (int) ( packet_id % (uint64_t) msgx->num_workers );
    struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
    if ( worker->producer < 0 || !packet_data )
    {
        return -1;
    }
    struct rocketnet_server_msgx_send_bank_t * bank = &worker->banks[worker->producer];
    const uintptr_t first = (uintptr_t) bank->data[0];
    const uintptr_t address = (uintptr_t) packet_data;
    if ( address < first )
    {
        return -1;
    }
    const uintptr_t offset = address - first;
    if ( offset % ROCKETNET_FRAME_STRIDE != 0 || offset / ROCKETNET_FRAME_STRIDE >= ROCKETNET_MSGX_SEND_QUEUE_SIZE )
    {
        return -1;
    }
    *bank_out = bank;
    return (int) ( offset / ROCKETNET_FRAME_STRIDE );
}

void rocketnet_server_msgx_finish_packet( struct rocketnet_server_msgx_t * msgx, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( msgx );
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );
    if ( msgx->stopping )
    {
        msgx->finish_refused.fetch_add( 1, std::memory_order_relaxed );
        return;
    }
    struct rocketnet_server_msgx_send_bank_t * bank = NULL;
    const int slot = rocketnet_server_msgx_slot_of( msgx, packet_id, packet_data, &bank );
    if ( slot < 0 )
    {
        msgx->finish_refused.fetch_add( 1, std::memory_order_relaxed );
        return;
    }
    bank->info[slot].packet_bytes = packet_bytes;
}

void rocketnet_server_msgx_abort_packet( struct rocketnet_server_msgx_t * msgx, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( msgx );
    if ( msgx->stopping )
    {
        return;
    }
    struct rocketnet_server_msgx_send_bank_t * bank = NULL;
    const int slot = rocketnet_server_msgx_slot_of( msgx, packet_id, packet_data, &bank );
    if ( slot < 0 )
    {
        return;
    }
    bank->info[slot].packet_bytes = 0;
}

// proofs/tla/RNBankFifoLayout.tla Publish: main hands a worker its filled
// producer and takes a free bank, under a try-lock: a busy mutex or no free
// bank keeps the producer for the next flush (REFLUSHES), main never waits
static void rocketnet_server_msgx_publish( struct rocketnet_server_msgx_worker_t * worker )
{
    if ( worker->producer < 0 )
    {
        return;
    }
    struct rocketnet_server_msgx_send_bank_t * bank = &worker->banks[worker->producer];
    if ( bank->num_packets.load( std::memory_order_relaxed ) == 0 )
    {
        return;
    }
    if ( !rocketnet_platform_mutex_try_acquire( &worker->mutex ) )
    {
        return;
    }
    const int next = rocketnet_buffer_handoff_publish( &worker->handoff, worker->producer, 1 );
    if ( next >= 0 )
    {
        worker->producer = next;
    }
    rocketnet_platform_mutex_release( &worker->mutex );

    if ( next >= 0 )
    {
        rocketnet_assert( worker->banks[next].num_packets.load( std::memory_order_relaxed ) == 0 );
        rocketnet_platform_wake_set( worker->wake );
    }
}

void rocketnet_server_msgx_send_packets( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    if ( msgx->stopping )
    {
        return;
    }
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        rocketnet_server_msgx_publish( worker );
        if ( worker->full.load( std::memory_order_acquire ) )
        {
            rocketnet_platform_wake_set( worker->wake );
        }
    }
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: every producer empty and
// every worker holding no bank
#if ROCKETNET_DEV
void rocketnet_server_msgx_test_fail_sends( struct rocketnet_server_msgx_t * msgx, int count, int error )
{
    rocketnet_assert( msgx );
    msgx->test_fail_send_error.store( error, std::memory_order_relaxed );
    msgx->test_fail_sends.store( count > 0 ? count : 0, std::memory_order_release );
}

int rocketnet_server_msgx_test_sockets_open( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    return msgx->table->num_open;
}
#endif // #if ROCKETNET_DEV

bool rocketnet_server_msgx_send_idle( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        // the producer is read under the mutex: after stop the worker takes
        // it as its final bank and writes it there
        if ( !rocketnet_platform_mutex_try_acquire( &worker->mutex ) )
        {
            return false;
        }
        const int producer = worker->producer;
        const bool idle = producer < 0 ||
                          ( worker->banks[producer].num_packets.load( std::memory_order_relaxed ) == 0 &&
                            worker->handoff.ready_count == 0 && worker->handoff.free_count == ROCKETNET_MSGX_TX_BANKS - 1 );
        rocketnet_platform_mutex_release( &worker->mutex );
        if ( !idle )
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// send: the transmit worker
// ---------------------------------------------------------------------------

// proofs/tla/RNBankFifoLayout.tla Take; at quit with nothing ready the
// worker takes main's last producer as its final bank (proofs/tla/RNGsoHandoff.tla TxTake)
static int rocketnet_server_msgx_take( struct rocketnet_server_msgx_worker_t * worker, bool * final )
{
    *final = false;
    rocketnet_platform_mutex_acquire( &worker->mutex );
    int index = rocketnet_buffer_handoff_take( &worker->handoff );
    if ( index < 0 && worker->quit.load( std::memory_order_acquire ) )
    {
        index = worker->producer;
        worker->producer = -1;
        *final = true;
    }
    rocketnet_platform_mutex_release( &worker->mutex );
    return index;
}

// proofs/tla/RNBankFifoLayout.tla Release: a bank is free again only after
// every send of it returned
static void rocketnet_server_msgx_release( struct rocketnet_server_msgx_worker_t * worker, int index )
{
    worker->banks[index].num_packets.store( 0, std::memory_order_relaxed );
    rocketnet_platform_mutex_acquire( &worker->mutex );
    rocketnet_buffer_handoff_release( &worker->handoff, index );
    rocketnet_buffer_handoff_owed( &worker->handoff );
    rocketnet_platform_mutex_release( &worker->mutex );
}

// the bank's slots in socket-slot order, staged order kept within a slot:
// a counting sort, the shared socket's packets (slot -1) first
// (sdk/rocketnet_msgx_send_runs.h, proofs/tla/RNMsgxSendRuns.tla Sort)
static int rocketnet_server_msgx_sort( struct rocketnet_server_msgx_worker_t * worker, const struct rocketnet_server_msgx_send_bank_t * bank, int count,
                                       int capacity )
{
    rocketnet_msgx_send_runs_sort( bank->info, count, capacity, worker->bucket, worker->order );
    return count;
}

// the send seam: true when this call is to answer -1 with errno set
static bool rocketnet_server_msgx_test_send_fails( struct rocketnet_server_msgx_t * msgx )
{
#if ROCKETNET_DEV
    int pending = msgx->test_fail_sends.load( std::memory_order_acquire );
    while ( pending > 0 )
    {
        if ( msgx->test_fail_sends.compare_exchange_weak( pending, pending - 1, std::memory_order_acq_rel ) )
        {
            errno = msgx->test_fail_send_error.load( std::memory_order_relaxed );
            return true;
        }
    }
#else
    (void) msgx;
#endif // #if ROCKETNET_DEV
    return false;
}

// what one call of rocketnet_server_msgx_send_bank works on: the callbacks'
// user of sdk/rocketnet_msgx_send_runs.h
struct rocketnet_server_msgx_send_context_t
{
    struct rocketnet_server_msgx_t * msgx;
    struct rocketnet_server_msgx_worker_t * worker;
    struct rocketnet_server_msgx_send_bank_t * bank;
};

static int rocketnet_server_msgx_run_resolve( void * user, int table_slot, uint32_t generation )
{
    struct rocketnet_server_msgx_send_context_t * context = (struct rocketnet_server_msgx_send_context_t *) user;
    return rocketnet_msgx_socket_table_resolve( context->msgx->table, table_slot, generation );
}

static void rocketnet_server_msgx_run_report_dead( void * user, int table_slot, uint32_t generation )
{
    struct rocketnet_server_msgx_send_context_t * context = (struct rocketnet_server_msgx_send_context_t *) user;
    rocketnet_msgx_socket_table_report_dead( context->msgx->table, table_slot, generation );
}

// one sendmsg_x of the run on the client's connected socket
static int rocketnet_server_msgx_run_send( void * user, int fd, const int * packets, int num_packets )
{
    struct rocketnet_server_msgx_send_context_t * context = (struct rocketnet_server_msgx_send_context_t *) user;
    struct rocketnet_server_msgx_worker_t * worker = context->worker;
    struct rocketnet_server_msgx_send_bank_t * bank = context->bank;
    rocketnet_assert( num_packets >= 1 && num_packets <= ROCKETNET_MSGX_SEND_BATCH );
    for ( int n = 0; n < num_packets; n++ )
    {
        const int i = packets[n];
        worker->iovecs[n].iov_base = bank->data[i] + ROCKETNET_FRAME_WIRE_OFFSET;
        worker->iovecs[n].iov_len = (size_t) bank->info[i].packet_bytes;
        memset( &worker->messages[n], 0, sizeof( worker->messages[n] ) );
        worker->messages[n].msg_iov = &worker->iovecs[n];
        worker->messages[n].msg_iovlen = 1;
    }
    worker->send_calls.fetch_add( 1, std::memory_order_relaxed );
    const ssize_t result = rocketnet_server_msgx_test_send_fails( context->msgx ) ? -1 : context->msgx->sendmsg_x( fd, worker->messages, (u_int) num_packets, 0 );
    if ( result >= 0 )
    {
        return result > num_packets ? num_packets : (int) result;
    }
    return rocketnet_msgx_send_error_kind( errno );
}

// one packet on the shared socket
static int rocketnet_server_msgx_run_send_shared( void * user, int i )
{
    struct rocketnet_server_msgx_send_context_t * context = (struct rocketnet_server_msgx_send_context_t *) user;
    struct rocketnet_server_msgx_t * msgx = context->msgx;
    struct rocketnet_server_msgx_send_bank_t * bank = context->bank;
    struct sockaddr_storage sockaddr;
    const socklen_t socklen = rocketnet_server_msgx_address_to_sockaddr( &bank->info[i].to, msgx->ipv6, &sockaddr );
    if ( socklen == 0 )
    {
        return ROCKETNET_MSGX_SHARED_DROPPED;
    }
    context->worker->send_calls.fetch_add( 1, std::memory_order_relaxed );
    const ssize_t result = rocketnet_server_msgx_test_send_fails( msgx ) ? -1 : sendto( msgx->shared_fd, bank->data[i] + ROCKETNET_FRAME_WIRE_OFFSET, (size_t) bank->info[i].packet_bytes, 0, (const struct sockaddr *) &sockaddr, socklen );
    if ( result >= 0 )
    {
        return ROCKETNET_MSGX_SHARED_SENT;
    }
    return rocketnet_msgx_send_error_kind( errno ) == ROCKETNET_MSGX_SEND_FULL ? ROCKETNET_MSGX_SHARED_FULL : ROCKETNET_MSGX_SHARED_DROPPED;
}

// Sends the sorted bank from *cursor (sdk/rocketnet_msgx_send_runs.h,
// proofs/tla/RNMsgxSendRuns.tla). Returns false when a socket is full:
// *cursor is the first packet not yet taken, the rest kept in order for the
// next attempt. Runs inside one table pass (holder worker->index).
static bool rocketnet_server_msgx_send_bank( struct rocketnet_server_msgx_t * msgx, struct rocketnet_server_msgx_worker_t * worker,
                                             struct rocketnet_server_msgx_send_bank_t * bank, int count, int * cursor )
{
    struct rocketnet_server_msgx_send_context_t context = { msgx, worker, bank };
    struct rocketnet_msgx_send_runs_io_t io;
    io.user = &context;
    io.resolve = rocketnet_server_msgx_run_resolve;
    io.send = rocketnet_server_msgx_run_send;
    io.send_shared = rocketnet_server_msgx_run_send_shared;
    io.report_dead = rocketnet_server_msgx_run_report_dead;

    int run_packets[ROCKETNET_MSGX_SEND_BATCH];
    struct rocketnet_msgx_send_runs_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    const bool sent = rocketnet_msgx_send_runs_send( bank->info, worker->order, count, ROCKETNET_MSGX_SEND_BATCH, run_packets, cursor, &io, &counters );

    worker->sent_connected.fetch_add( counters.sent_connected, std::memory_order_relaxed );
    worker->sent_shared.fetch_add( counters.sent_shared, std::memory_order_relaxed );
    worker->dropped.fetch_add( counters.dropped, std::memory_order_relaxed );
    worker->refused.fetch_add( counters.refused, std::memory_order_relaxed );
    worker->retired.fetch_add( counters.retired, std::memory_order_relaxed );
    worker->dead.fetch_add( counters.dead, std::memory_order_relaxed );
    return sent;
}

static void rocketnet_server_msgx_worker( rocketnet_platform_thread_t *, void * context )
{
    struct rocketnet_server_msgx_worker_t * worker = (struct rocketnet_server_msgx_worker_t *) context;
    struct rocketnet_server_msgx_t * msgx = worker->msgx;
    const int capacity = msgx->table->capacity;

    for ( ;; )
    {
        bool final = false;
        const int index = rocketnet_server_msgx_take( worker, &final );
        if ( index < 0 )
        {
            if ( final )
            {
                break;
            }
            rocketnet_platform_wake_wait( worker->wake, false, 0.1 );
            continue;
        }

        struct rocketnet_server_msgx_send_bank_t * bank = &worker->banks[index];
        uint64_t claimed = bank->num_packets.load( std::memory_order_relaxed );
        const int count = claimed > ROCKETNET_MSGX_SEND_QUEUE_SIZE ? ROCKETNET_MSGX_SEND_QUEUE_SIZE : (int) claimed;
        rocketnet_server_msgx_sort( worker, bank, count, capacity );

        int cursor = 0;
        int retries_after_quit = 0;
        for ( ;; )
        {
            rocketnet_msgx_socket_table_holder_begin( msgx->table, worker->index );
            const bool sent = rocketnet_server_msgx_send_bank( msgx, worker, bank, count, &cursor );
            rocketnet_msgx_socket_table_holder_end( msgx->table, worker->index );
            if ( sent )
            {
                break;
            }
            worker->full.store( true, std::memory_order_release );
            // proofs/tla/RNMsgxSendRuns.tla GiveUp: past the retries after
            // quit, what is still owed is dropped, counted
            if ( rocketnet_msgx_send_runs_give_up( worker->shutdown.load( std::memory_order_acquire ), &retries_after_quit, ROCKETNET_MSGX_SEND_RETRIES_AFTER_QUIT ) )
            {
                for ( int i = cursor; i < count; i++ )
                {
                    if ( bank->info[worker->order[i]].packet_bytes > 0 )
                    {
                        worker->dropped.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
                break;
            }
            rocketnet_platform_wake_wait( worker->wake, false, ROCKETNET_MSGX_SEND_RETRY_SECONDS );
        }
        worker->full.store( false, std::memory_order_release );

        if ( final )
        {
            break;
        }
        rocketnet_server_msgx_release( worker, index );
    }

    worker->stopped.store( true, std::memory_order_release );
}

// ---------------------------------------------------------------------------
// receive
// ---------------------------------------------------------------------------

static struct rocketnet_server_msgx_recv_bank_t * rocketnet_server_msgx_bank( struct rocketnet_server_msgx_receiver_t * receiver, int index )
{
    return index >= 0 && index < 3 ? &receiver->banks[index] : NULL;
}

// what one drain of rocketnet_msgx_receive_drain works on: the callbacks'
// user of sdk/rocketnet_msgx_receive_fill.h
struct rocketnet_server_msgx_drain_context_t
{
    struct rocketnet_server_msgx_t * msgx;
    struct rocketnet_server_msgx_receiver_t * receiver;
    int slot;                       // the socket's table slot, -1 for the shared socket
    uint32_t generation;
};

// the receive worker's bank: the worker writes only at or past its committed
// count, main reads only below it, and the worker is the only thread that
// moves hand.worker (under the mutex, at a commit)
static struct rocketnet_server_msgx_recv_bank_t * rocketnet_server_msgx_worker_bank( struct rocketnet_server_msgx_receiver_t * receiver )
{
    return rocketnet_server_msgx_bank( receiver, receiver->hand.worker );
}

static int rocketnet_server_msgx_fill_banked( void * user )
{
    struct rocketnet_server_msgx_drain_context_t * context = (struct rocketnet_server_msgx_drain_context_t *) user;
    return (int) rocketnet_server_msgx_worker_bank( context->receiver )->num_packets;
}

// one recvmsg_x of at most want datagrams into the scratch
static int rocketnet_server_msgx_fill_read( void * user, int fd, int want )
{
    struct rocketnet_server_msgx_drain_context_t * context = (struct rocketnet_server_msgx_drain_context_t *) user;
    struct rocketnet_server_msgx_receiver_t * receiver = context->receiver;
    rocketnet_assert( want >= 1 && want <= ROCKETNET_MSGX_RECV_BATCH );
    for ( int i = 0; i < want; i++ )
    {
        receiver->iovecs[i].iov_base = receiver->scratch[i];
        receiver->iovecs[i].iov_len = sizeof( receiver->scratch[i] );
        memset( &receiver->messages[i], 0, sizeof( receiver->messages[i] ) );
        receiver->messages[i].msg_name = &receiver->names[i];
        receiver->messages[i].msg_namelen = sizeof( struct sockaddr_storage );
        receiver->messages[i].msg_iov = &receiver->iovecs[i];
        receiver->messages[i].msg_iovlen = 1;
    }
    rocketnet_server_msgx_worker_bank( receiver )->receive_calls++;
    receiver->round_read = true;
    const ssize_t count = context->msgx->recvmsg_x( fd, receiver->messages, (u_int) want, MSG_DONTWAIT );
    if ( count < 0 )
    {
        const int error = errno;
        return error > 0 ? -error : -EIO;
    }
    return count > want ? want : (int) count;
}

// datagram i of the last read: banked, or counted by name (the prefilter's
// reasons, TOO_LARGE, RECEIVE_FAILED for a source it cannot read). The drain
// asked for no more than the bank has room for
static void rocketnet_server_msgx_fill_take( void * user, int i )
{
    struct rocketnet_server_msgx_drain_context_t * context = (struct rocketnet_server_msgx_drain_context_t *) user;
    struct rocketnet_server_msgx_t * msgx = context->msgx;
    struct rocketnet_server_msgx_receiver_t * receiver = context->receiver;
    struct rocketnet_server_msgx_recv_bank_t * bank = rocketnet_server_msgx_worker_bank( receiver );
    const struct rocketnet_server_msgx_msghdr_x_t * message = &receiver->messages[i];
    const int bytes = (int) message->msg_datalen;

    if ( message->msg_flags & MSG_TRUNC )
    {
        rocketnet_receive_drop_tally_count( &receiver->tally, ROCKETNET_RECEIVE_DROP_TOO_LARGE );
        return;
    }

    const enum rocketnet_receive_drop_t drop = rocketnet_receive_prefilter( receiver->scratch[i], bytes, msgx->packet_filter );
    if ( drop != ROCKETNET_RECEIVE_ACCEPT )
    {
        rocketnet_receive_drop_tally_count( &receiver->tally, drop );
        return;
    }

    rocketnet_address_t from;
    if ( message->msg_namelen == 0 || !rocketnet_server_msgx_sockaddr_to_address( &receiver->names[i], msgx->ipv6, &from ) )
    {
        rocketnet_receive_drop_tally_count( &receiver->tally, ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED );
        return;
    }

    rocketnet_assert( bank->num_packets < ROCKETNET_MSGX_RECV_QUEUE_SIZE );
    const uint32_t index = bank->num_packets++;
    bank->from[index] = from;
    bank->packet_bytes[index] = bytes;
    memcpy( bank->packet_data[index], receiver->scratch[i], (size_t) bytes );
}

// proofs/tla/RNMsgxReceiveFill.tla Commit (RNReceiveHandoff.tla Commit):
// under the mutex the worker makes its bank's count visible to main and, when
// main has started on that bank, moves to the free one, which main emptied
// before freeing it. Only indices and counts move under the mutex. True when
// the worker moved
static bool rocketnet_server_msgx_commit_receive( struct rocketnet_server_msgx_receiver_t * receiver )
{
    const uint32_t count = rocketnet_server_msgx_worker_bank( receiver )->num_packets;
    rocketnet_platform_mutex_acquire( &receiver->mutex );
    const int next = rocketnet_receive_handoff_commit( &receiver->hand, count );
    rocketnet_platform_mutex_release( &receiver->mutex );
    return next >= 0;
}

static bool rocketnet_server_msgx_fill_commit( void * user )
{
    struct rocketnet_server_msgx_drain_context_t * context = (struct rocketnet_server_msgx_drain_context_t *) user;
    return rocketnet_server_msgx_commit_receive( context->receiver );
}

// a failed read (rocketnet_msgx_receive_error_kind): an ICMP error queued for
// the client is counted beside ECONNREFUSED in receive_refused and read past;
// ENOBUFS is read past and counted RECEIVE_FAILED, as before; EINTR is read
// past uncounted (a signal, nothing lost); a dead connected socket is
// reported to the table; a dead or failed read counts RECEIVE_FAILED
static void rocketnet_server_msgx_fill_failed( void * user, int fd, int kind, int error )
{
    (void) fd;
    struct rocketnet_server_msgx_drain_context_t * context = (struct rocketnet_server_msgx_drain_context_t *) user;
    struct rocketnet_server_msgx_receiver_t * receiver = context->receiver;
    if ( kind == ROCKETNET_MSGX_RECEIVE_WEATHER )
    {
        if ( error == ENOBUFS )
        {
            rocketnet_receive_drop_tally_count( &receiver->tally, ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED );
        }
        else if ( error != EINTR )
        {
            rocketnet_server_msgx_worker_bank( receiver )->refused++;
        }
        return;
    }
    if ( kind == ROCKETNET_MSGX_RECEIVE_DEAD && context->slot >= 0 )
    {
        rocketnet_msgx_socket_table_report_dead( context->msgx->table, context->slot, context->generation );
    }
    rocketnet_receive_drop_tally_count( &receiver->tally, ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED );
}

// one ready socket drained into the worker's bank, into room only
// (sdk/rocketnet_msgx_receive_fill.h): false when the bank is full and main
// has not started on it, so the round stops and the kernel keeps the rest
static bool rocketnet_server_msgx_drain( struct rocketnet_server_msgx_t * msgx, struct rocketnet_server_msgx_receiver_t * receiver, int fd,
                                         int slot, uint32_t generation )
{
    struct rocketnet_server_msgx_drain_context_t context = { msgx, receiver, slot, generation };
    struct rocketnet_msgx_receive_io_t io;
    io.user = &context;
    io.banked = rocketnet_server_msgx_fill_banked;
    io.read = rocketnet_server_msgx_fill_read;
    io.take = rocketnet_server_msgx_fill_take;
    io.commit = rocketnet_server_msgx_fill_commit;
    io.failed = rocketnet_server_msgx_fill_failed;
    return rocketnet_msgx_receive_drain( &io, fd, ROCKETNET_MSGX_RECV_QUEUE_SIZE, ROCKETNET_MSGX_RECV_BATCH, ROCKETNET_MSGX_RECV_SOCKET_BUDGET );
}

#if ROCKETNET_DEV
// rocketnet_server_msgx_test_receive_source: the fake recvmsg_x answers one
// batch of one valid keep-alive from 127.0.0.1:port, then EAGAIN
static uint16_t rocketnet_server_msgx_test_source_port = 0;
static int rocketnet_server_msgx_test_source_left = 0;
static const uint8_t rocketnet_server_msgx_test_source_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x5A, 0x17, 0xC3, 0x08 };

static ssize_t rocketnet_server_msgx_test_source_receive( int, struct rocketnet_server_msgx_msghdr_x_t * messages, u_int count, int )
{
    if ( rocketnet_server_msgx_test_source_left <= 0 || count < 1 )
    {
        errno = EAGAIN;
        return -1;
    }
    rocketnet_server_msgx_test_source_left--;

    struct sockaddr_in * in4 = (struct sockaddr_in *) messages[0].msg_name;
    memset( in4, 0, sizeof( struct sockaddr_storage ) );
    in4->sin_family = AF_INET;
    in4->sin_port = htons( rocketnet_server_msgx_test_source_port );
    in4->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    messages[0].msg_namelen = sizeof( struct sockaddr_in );

    uint8_t * data = (uint8_t *) messages[0].msg_iov[0].iov_base;
    memset( data, 'k', ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    uint8_t * p = data;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, rocketnet_server_msgx_test_source_filter );
    messages[0].msg_datalen = ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
    messages[0].msg_flags = 0;
    return 1;
}

// One valid keep-alive from 127.0.0.1:port through the production drain
// (rocketnet_server_msgx_drain) of an IPv4 transport with no socket: banked
// is what reached the worker bank, failed the drain's RECEIVE_FAILED.
bool rocketnet_server_msgx_test_receive_source( uint16_t port, int * banked, uint64_t * failed )
{
    if ( !banked || !failed )
    {
        return false;
    }
    void * msgx_storage = rocketnet_malloc( NULL, sizeof( struct rocketnet_server_msgx_t ) );
    void * receiver_storage = rocketnet_malloc( NULL, sizeof( struct rocketnet_server_msgx_receiver_t ) );
    if ( !msgx_storage || !receiver_storage )
    {
        rocketnet_free( NULL, msgx_storage );
        rocketnet_free( NULL, receiver_storage );
        return false;
    }
    memset( msgx_storage, 0, sizeof( struct rocketnet_server_msgx_t ) );
    memset( receiver_storage, 0, sizeof( struct rocketnet_server_msgx_receiver_t ) );
    struct rocketnet_server_msgx_t * msgx = new ( msgx_storage ) rocketnet_server_msgx_t();
    struct rocketnet_server_msgx_receiver_t * receiver = new ( receiver_storage ) rocketnet_server_msgx_receiver_t();

    msgx->ipv6 = false;
    memcpy( msgx->packet_filter, rocketnet_server_msgx_test_source_filter, sizeof( rocketnet_server_msgx_test_source_filter ) );
    msgx->recvmsg_x = rocketnet_server_msgx_test_source_receive;
    msgx->receiver = receiver;
    rocketnet_receive_handoff_init( &receiver->hand );
    rocketnet_receive_drop_tally_reset( &receiver->tally );

    rocketnet_server_msgx_test_source_port = port;
    rocketnet_server_msgx_test_source_left = 1;
    const bool done = rocketnet_server_msgx_drain( msgx, receiver, -1, -1, 0 );

    *banked = done ? (int) rocketnet_server_msgx_worker_bank( receiver )->num_packets : -1;
    *failed = receiver->tally.values[ROCKETNET_RECEIVE_DROP_RECEIVE_FAILED].load( std::memory_order_relaxed );

    receiver->~rocketnet_server_msgx_receiver_t();
    msgx->~rocketnet_server_msgx_t();
    rocketnet_free( NULL, receiver_storage );
    rocketnet_free( NULL, msgx_storage );
    return true;
}
#endif // #if ROCKETNET_DEV

static void rocketnet_server_msgx_receive_worker( rocketnet_platform_thread_t *, void * context )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) context;
    struct rocketnet_server_msgx_receiver_t * receiver = msgx->receiver;
    const int holder = msgx->num_workers;

    while ( !receiver->quit.load( std::memory_order_relaxed ) )
    {
        // the pass covers the kevent call: a descriptor it returns cannot be
        // closed until the pass ends (proofs/tla/RNMsgxSocketTable.tla RxResolve)
        rocketnet_msgx_socket_table_holder_begin( msgx->table, holder );

        bool held = false;
        receiver->round_read = false;
        struct timespec timeout = { 0, 1000000 };
        const int ready = kevent( msgx->kq, NULL, 0, receiver->events, ROCKETNET_MSGX_RECV_EVENTS, &timeout );
        for ( int e = 0; e < ready && !held; e++ )
        {
            const struct kevent * event = &receiver->events[e];
            if ( event->filter != EVFILT_READ )
            {
                continue;
            }
            const uintptr_t udata = (uintptr_t) event->udata;
            const int slot = (int) (uint32_t) ( udata & 0xFFFFFFFFu ) - 1;
            const uint32_t generation = (uint32_t) ( (uint64_t) udata >> 32 );
            // RxResolve: a connected socket's event is read only while its
            // slot still holds that descriptor at that generation; a retired
            // one is skipped (the shared socket has no slot)
            if ( slot >= 0 && rocketnet_msgx_socket_table_resolve( msgx->table, slot, generation ) != (int) event->ident )
            {
                continue;
            }
            // a bounded read per socket per round, so one flooded socket
            // (the shared one under a blast) cannot hold the others back;
            // what it leaves stays readable for the next round. A full bank
            // main has not started on ends the round (RNMsgxReceiveFill
            // HoldFull): every socket keeps what it holds
            held = !rocketnet_server_msgx_drain( msgx, receiver, (int) event->ident, slot, generation );
        }

        rocketnet_msgx_socket_table_holder_end( msgx->table, holder );

        // RNMsgxReceiveFill RoundCommit and RoundKeep: a round that read
        // commits its bank's count, so main's next call returns what it banked
        // (a held round committed at its HoldFull)
        if ( !held && receiver->round_read )
        {
            rocketnet_server_msgx_commit_receive( receiver );
        }

        // RNMsgxReceiveFill Sleep: a held round reads nothing until main
        // starts on the full bank; the sockets stay readable, so the worker
        // waits instead of polling them again at once
        if ( held )
        {
            rocketnet_platform_sleep( ROCKETNET_MSGX_RECV_HOLD_SECONDS );
        }
    }

    // what this worker banked and main never handed the server is counted by
    // main, once it finds the worker exited (rocketnet_server_msgx_stopped,
    // RNMsgxReceiveFill StopCount); the release orders the banks' counts
    // before main's acquire
    receiver->stopped.store( true, std::memory_order_release );
}

// proofs/tla/RNMsgxReceiveFill.tla Call: main's side of one receive call
// (rocketnet_msgx_receive_call) over the receiver's banks

static bool rocketnet_server_msgx_call_try_lock( void * user )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;
    return rocketnet_platform_mutex_try_acquire( &msgx->receiver->mutex );
}

static void rocketnet_server_msgx_call_unlock( void * user )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;
    rocketnet_platform_mutex_release( &msgx->receiver->mutex );
}

static void rocketnet_server_msgx_call_reset( void * user, int index )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;
    rocketnet_server_msgx_bank( msgx->receiver, index )->num_packets = 0;
}

static void rocketnet_server_msgx_fold_bank( struct rocketnet_server_msgx_t * msgx, struct rocketnet_server_msgx_recv_bank_t * bank )
{
    msgx->receive_calls += bank->receive_calls;
    msgx->receive_refused += bank->refused;
    bank->receive_calls = 0;
    bank->refused = 0;
}

static void rocketnet_server_msgx_call_fold( void * user, int index )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;
    rocketnet_server_msgx_fold_bank( msgx, rocketnet_server_msgx_bank( msgx->receiver, index ) );
}

static bool rocketnet_server_msgx_call_take( void * user, int index, uint32_t source )
{
    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) user;
    if ( msgx->process_packets.num_packets >= msgx->process_packets.capacity )
    {
        return false;
    }
    struct rocketnet_server_msgx_recv_bank_t * bank = rocketnet_server_msgx_bank( msgx->receiver, index );
    const int target = msgx->process_packets.num_packets++;
    msgx->process_packets.from[target] = bank->from[source];
    msgx->process_packets.packet_data[target] = bank->packet_data[source];
    msgx->process_packets.packet_bytes[target] = bank->packet_bytes[source];
    return true;
}

// proofs/tla/RNMsgxReceiveFill.tla StopCount: main, once, when it first
// finds the worker exited after the stop, so main owns every bank. What the
// worker banked that main never handed the server is counted
// RECEIVE_DROP_STOPPED (rocketnet_msgx_receive_stop_count) and the tallies
// of every bank not yet folded are folded. Main hands nothing after the stop
// (rocketnet_server_msgx_receive_packets), so nothing is counted twice
static void rocketnet_server_msgx_count_stopped( struct rocketnet_server_msgx_t * msgx )
{
    struct rocketnet_server_msgx_receiver_t * receiver = msgx->receiver;
    if ( !receiver || receiver->stop_counted )
    {
        return;
    }
    receiver->stop_counted = true;
    const uint32_t count[3] = { receiver->banks[0].num_packets, receiver->banks[1].num_packets, receiver->banks[2].num_packets };
    receiver->main_stopped += rocketnet_msgx_receive_stop_count( &receiver->hand, count );
    struct rocketnet_server_msgx_recv_bank_t * held = rocketnet_server_msgx_bank( receiver, receiver->hand.main );
    held->num_packets = 0;
    rocketnet_server_msgx_fold_bank( msgx, held );
    if ( receiver->hand.worker != receiver->hand.main )
    {
        struct rocketnet_server_msgx_recv_bank_t * filling = rocketnet_server_msgx_bank( receiver, receiver->hand.worker );
        filling->num_packets = 0;
        rocketnet_server_msgx_fold_bank( msgx, filling );
    }
}

void rocketnet_server_msgx_receive_packets( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );

    // retry a deferred send publish even on an update with no flush
    for ( int w = 0; w < msgx->num_workers && !msgx->stopping; w++ )
    {
        rocketnet_server_msgx_publish( msgx->workers[w] );
    }

    msgx->process_packets.num_packets = 0;
    struct rocketnet_server_msgx_receiver_t * receiver = msgx->receiver;

    // RNMsgxReceiveFill MainStop: once stop is asked main hands nothing more;
    // what is still banked is counted when the worker has exited (StopCount,
    // R69: a bank read after the stop would hold a tail no later update reads)
    if ( msgx->stopping )
    {
        return;
    }

    // proofs/tla/RNMsgxReceiveFill.tla Call (RNReceiveHandoff.tla Call):
    // main reads the worker's bank in place up to its committed count, after
    // the unread tail of the bank the worker left, so a datagram committed
    // before this call took the mutex is returned by it when the call began
    // with no backlog from a cut call and its capacity does not cut it
    // (DeliveredByNextCall)
    struct rocketnet_msgx_receive_call_io_t io;
    io.user = msgx;
    io.try_lock = rocketnet_server_msgx_call_try_lock;
    io.unlock = rocketnet_server_msgx_call_unlock;
    io.reset = rocketnet_server_msgx_call_reset;
    io.fold = rocketnet_server_msgx_call_fold;
    io.take = rocketnet_server_msgx_call_take;
    bool busy = false; // a busy mutex leaves main with the limits it holds
    msgx->packets_received += rocketnet_msgx_receive_call( &receiver->hand, &io, &busy );
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_msgx_process_packets( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    return &msgx->process_packets;
}

void rocketnet_server_msgx_receive_drop_totals( struct rocketnet_server_msgx_t * msgx, uint64_t * values )
{
    rocketnet_assert( msgx );
    memset( values, 0, sizeof( uint64_t ) * ROCKETNET_RECEIVE_NUM_DROPS );
    rocketnet_receive_drop_tally_sum( &msgx->receiver->tally, 1, values );
    values[ROCKETNET_RECEIVE_DROP_STOPPED] += msgx->receiver->main_stopped;
    values[ROCKETNET_RECEIVE_DROP_RETIRED] += msgx->receiver->main_retired;
    values[ROCKETNET_RECEIVE_ACCEPT] = 0;
}

// ---------------------------------------------------------------------------
// the socket table's feed (main)
// ---------------------------------------------------------------------------

// proofs/tla/RNMsgxSocketTable.tla Mark: a connected client seen at address
bool rocketnet_server_msgx_mark( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * address )
{
    rocketnet_assert( msgx );
    if ( msgx->stopping )
    {
        return true;
    }
    const uint64_t opened = msgx->table->counts.opened;
    const bool done = rocketnet_msgx_socket_table_mark( msgx->table, address );
    if ( msgx->table->counts.opened != opened )
    {
        const int slot = rocketnet_msgx_socket_table_lookup( msgx->table, address );
        rocketnet_assert( slot >= 0 );
        struct rocketnet_msgx_socket_slot_t * entry = &msgx->table->slots[slot];
        if ( !rocketnet_server_msgx_register( msgx, entry->fd, slot, entry->generation ) )
        {
            // a socket the poller cannot watch would swallow its client's
            // datagrams: retire it at once (counted as dead) and cool down
            rocketnet_msgx_socket_table_report_dead( msgx->table, slot, entry->generation );
        }
    }
    return done;
}

// proofs/tla/RNMsgxSocketTable.tla Forget then ForgetDereg: the table retires
// the socket first (its generation moves), then the poller forgets the
// descriptor. An event the receive worker takes in between fails its
// resolve (RxResolve) and is not read; the descriptor stays open until the
// sweep sees that pass end
bool rocketnet_server_msgx_forget( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * address )
{
    rocketnet_assert( msgx );
    int retired_fd = -1;
    const bool done = rocketnet_msgx_socket_table_forget( msgx->table, address, &retired_fd );
    if ( retired_fd >= 0 )
    {
        rocketnet_server_msgx_deregister( msgx, retired_fd );
    }
    return done;
}

// proofs/tla/RNMsgxSocketTable.tla RetireDead, Close and BeginUpdate
void rocketnet_server_msgx_commit( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    rocketnet_msgx_socket_table_sweep( msgx->table, rocketnet_server_msgx_deregister, msgx );
    rocketnet_msgx_socket_table_begin_update( msgx->table, ++msgx->update );
}

// ---------------------------------------------------------------------------
// create, stop, destroy
// ---------------------------------------------------------------------------

// the descriptors a socket per client needs: raise the soft limit when it is
// short, and refuse by name when it cannot be raised far enough
static bool rocketnet_server_msgx_descriptors( int max_clients )
{
    const rlim_t need = (rlim_t) max_clients + ROCKETNET_MSGX_DESCRIPTOR_SLACK;
    struct rlimit limit;
    if ( getrlimit( RLIMIT_NOFILE, &limit ) != 0 )
    {
        rocketnet_error( "msgx transport refused: cannot read the descriptor limit (%s)", strerror( errno ) );
        return false;
    }
    if ( limit.rlim_cur >= need )
    {
        return true;
    }
    if ( limit.rlim_max != RLIM_INFINITY && limit.rlim_max < need )
    {
        rocketnet_error( "msgx transport refused: the descriptor limit's hard maximum %llu is below the %llu a socket per client needs (%d clients)",
                         (unsigned long long) limit.rlim_max, (unsigned long long) need, max_clients );
        return false;
    }
    struct rlimit raised = limit;
    raised.rlim_cur = need;
    if ( setrlimit( RLIMIT_NOFILE, &raised ) != 0 )
    {
        rocketnet_error( "msgx transport refused: cannot raise the descriptor limit from %llu to %llu for a socket per client (%s)",
                         (unsigned long long) limit.rlim_cur, (unsigned long long) need, strerror( errno ) );
        return false;
    }
    rocketnet_info( "msgx transport: descriptor limit raised from %llu to %llu for a socket per client",
                    (unsigned long long) limit.rlim_cur, (unsigned long long) need );
    return true;
}

uint64_t rocketnet_server_msgx_allocation_bytes( int num_workers, int max_clients )
{
    if ( num_workers < 1 || num_workers > ROCKETNET_MSGX_MAX_WORKERS || max_clients < 1 || max_clients > ROCKETNET_MSGX_TABLE_MAX_SLOTS )
    {
        return 0;
    }
    uint64_t bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_msgx_t ), sizeof( rocketnet_msgx_socket_table_t ),
                                                     sizeof( rocketnet_server_msgx_receiver_t ) } );
    const uint64_t view = rocketnet_server_process_packets_allocation_bytes( rocketnet_server_receive_packets_per_update( max_clients ) );
    const uint64_t worker = rocketnet_memory_allocations( { sizeof( rocketnet_server_msgx_worker_t ) } );
    if ( !bytes || !view || !worker || !rocketnet_memory_add( bytes, 1, view ) || !rocketnet_memory_add( bytes, (uint64_t) num_workers, worker ) ||
         !rocketnet_memory_add( bytes, (uint64_t) num_workers + 1, rocketnet_platform_thread_allocation_bytes() ) ||
         !rocketnet_memory_add( bytes, (uint64_t) num_workers, rocketnet_platform_wake_allocation_bytes() ) )
    {
        return 0;
    }
    return bytes;
}

static void rocketnet_server_msgx_free( struct rocketnet_server_msgx_t * msgx )
{
    void * context = msgx->context;
    for ( int w = 0; w < ROCKETNET_MSGX_MAX_WORKERS; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        if ( !worker )
        {
            continue;
        }
        if ( worker->wake )
        {
            rocketnet_platform_wake_destroy( worker->wake );
        }
        if ( worker->mutex.ok )
        {
            rocketnet_platform_mutex_destroy( &worker->mutex );
        }
        worker->~rocketnet_server_msgx_worker_t();
        rocketnet_free( context, worker );
    }
    if ( msgx->receiver )
    {
        if ( msgx->receiver->mutex.ok )
        {
            rocketnet_platform_mutex_destroy( &msgx->receiver->mutex );
        }
        msgx->receiver->~rocketnet_server_msgx_receiver_t();
        rocketnet_free( context, msgx->receiver );
        msgx->receiver = NULL;     // the table's close_all below reads it
    }
    if ( msgx->table )
    {
        rocketnet_msgx_socket_table_close_all( msgx->table );
        msgx->table->~rocketnet_msgx_socket_table_t();
        rocketnet_free( context, msgx->table );
    }
    if ( msgx->shared_fd >= 0 )
    {
        close( msgx->shared_fd );
    }
    if ( msgx->kq >= 0 )
    {
        close( msgx->kq );
    }
    rocketnet_server_process_packets_destroy( context, &msgx->process_packets );
    msgx->~rocketnet_server_msgx_t();
    rocketnet_free( context, msgx );
}

static void rocketnet_server_msgx_abort_create( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_server_msgx_stop( msgx );
    while ( !rocketnet_server_msgx_stopped( msgx ) )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_server_msgx_destroy( msgx );
}

struct rocketnet_server_msgx_t * rocketnet_server_msgx_create( void * context, struct rocketnet_address_t * bind_address, int num_workers,
                                                               int max_clients, const uint8_t * packet_filter, int * status )
{
    rocketnet_assert( bind_address );
    rocketnet_assert( packet_filter );
    rocketnet_assert( bind_address->type == ROCKETNET_ADDRESS_IPV4 || bind_address->type == ROCKETNET_ADDRESS_IPV6 );

    if ( num_workers <= 0 )
    {
        num_workers = ROCKETNET_MSGX_DEFAULT_WORKERS;
    }
    if ( num_workers > ROCKETNET_MSGX_MAX_WORKERS || num_workers + 1 > ROCKETNET_MSGX_TABLE_MAX_HOLDERS )
    {
        rocketnet_error( "msgx transport refused: num_msgx_transmit_workers %d is out of range (1 to %d)", num_workers, ROCKETNET_MSGX_MAX_WORKERS );
        rocketnet_create_status( status, ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
        return NULL;
    }
    if ( max_clients < 1 || max_clients > ROCKETNET_MSGX_TABLE_MAX_SLOTS )
    {
        rocketnet_error( "msgx transport max_clients %d is out of range (1 to %d)", max_clients, ROCKETNET_MSGX_TABLE_MAX_SLOTS );
        return NULL;
    }

    const rocketnet_server_msgx_send_fn send_fn = (rocketnet_server_msgx_send_fn) dlsym( RTLD_DEFAULT, "sendmsg_x" );
    const rocketnet_server_msgx_recv_fn recv_fn = (rocketnet_server_msgx_recv_fn) dlsym( RTLD_DEFAULT, "recvmsg_x" );
    if ( !send_fn || !recv_fn )
    {
        rocketnet_warn( "msgx transport refused: sendmsg_x or recvmsg_x is not available" );
        return NULL;
    }

    if ( !rocketnet_server_msgx_descriptors( max_clients ) )
    {
        return NULL;
    }

    struct rocketnet_server_msgx_t * msgx = (struct rocketnet_server_msgx_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_msgx_t ) );
    if ( !msgx )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }
    new ( msgx ) rocketnet_server_msgx_t();
    msgx->context = context;
    msgx->num_workers = num_workers;
    msgx->max_clients = max_clients;
    msgx->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    msgx->stopping = false;
    msgx->sendmsg_x = send_fn;
    msgx->recvmsg_x = recv_fn;
    msgx->shared_fd = -1;
    msgx->kq = -1;
    msgx->update = 0;
    memcpy( msgx->packet_filter, packet_filter, ROCKETNET_PACKET_FILTER_BYTES );
    for ( int w = 0; w < ROCKETNET_MSGX_MAX_WORKERS; w++ )
    {
        msgx->workers[w] = NULL;
    }
    msgx->receiver = NULL;
    msgx->table = NULL;
    msgx->packets_received = 0;
    msgx->receive_calls = 0;
    msgx->receive_refused = 0;
    msgx->shared_receive_buffer = 0;
    memset( &msgx->process_packets, 0, sizeof( msgx->process_packets ) );

    bool ok = rocketnet_server_process_packets_create( context, &msgx->process_packets, rocketnet_server_receive_packets_per_update( max_clients ) );

    msgx->table = ok ? (struct rocketnet_msgx_socket_table_t *) rocketnet_malloc( context, sizeof( struct rocketnet_msgx_socket_table_t ) ) : NULL;
    if ( msgx->table )
    {
        new ( msgx->table ) rocketnet_msgx_socket_table_t();
        rocketnet_msgx_socket_table_init( msgx->table, max_clients, num_workers + 1, ROCKETNET_MSGX_OPENS_PER_UPDATE,
                                          rocketnet_server_msgx_table_open, rocketnet_server_msgx_table_close, msgx );
    }
    ok = ok && msgx->table;

    msgx->receiver = ok ? (struct rocketnet_server_msgx_receiver_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_msgx_receiver_t ) ) : NULL;
    if ( msgx->receiver )
    {
        new ( msgx->receiver ) rocketnet_server_msgx_receiver_t();
        struct rocketnet_server_msgx_receiver_t * receiver = msgx->receiver;
        receiver->thread = NULL;
        rocketnet_receive_handoff_init( &receiver->hand );
        receiver->main_stopped = 0;
        receiver->stop_counted = false;
        receiver->round_read = false;
        receiver->main_retired = 0;
        for ( int b = 0; b < 3; b++ )
        {
            receiver->banks[b].num_packets = 0;
            receiver->banks[b].receive_calls = 0;
            receiver->banks[b].refused = 0;
        }
        rocketnet_receive_drop_tally_reset( &receiver->tally );
    }
    ok = ok && msgx->receiver;

    for ( int w = 0; ok && w < num_workers; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = (struct rocketnet_server_msgx_worker_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_msgx_worker_t ) );
        if ( !worker )
        {
            ok = false;
            break;
        }
        new ( worker ) rocketnet_server_msgx_worker_t();
        worker->msgx = msgx;
        worker->index = w;
        worker->thread = NULL;
        worker->wake = NULL;
        worker->producer = 0;
        rocketnet_buffer_handoff_init( &worker->handoff, 0 );
        msgx->workers[w] = worker;
    }

    if ( !ok )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_server_msgx_free( msgx );
        return NULL;
    }

    // the shared socket: handshakes, and every client without its own socket.
    // Its receive buffer takes a whole update's inbound at the full client
    // count, as every transport's does.
    msgx->shared_fd = rocketnet_server_msgx_open_bound( bind_address, msgx->ipv6, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, true );
    if ( msgx->shared_fd < 0 )
    {
        if ( rocketnet_platform_bind_refused( -msgx->shared_fd ) )
        {
            rocketnet_create_status( status, ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
        }
        rocketnet_error( "msgx transport: failed to open the shared socket: %s", strerror( -msgx->shared_fd ) );
        msgx->shared_fd = -1;
        rocketnet_server_msgx_free( msgx );
        return NULL;
    }
    // Darwin refuses a request above its limit (kern.ipc.maxsockbuf, 8 MiB
    // stock, of which about 7.1 MiB is grantable) with ENOBUFS and keeps its
    // default (about 768 KiB): the platform's halving walk takes the most it
    // grants, and the log line says what was asked and what was granted
    const int receive_request = rocketnet_server_transport_receive_buffer_request( "msgx", max_clients );
    rocketnet_platform_set_socket_buffer_size( msgx->shared_fd, SO_RCVBUF, receive_request, "receive", false );
    int granted_receive = 0;
    socklen_t optlen = sizeof( granted_receive );
    getsockopt( msgx->shared_fd, SOL_SOCKET, SO_RCVBUF, &granted_receive, &optlen );
    msgx->shared_receive_buffer = granted_receive;
    rocketnet_server_receive_buffer_granted( "msgx", receive_request, granted_receive );

    if ( bind_address->port == 0 )
    {
        struct sockaddr_storage bound;
        socklen_t bound_length = sizeof( bound );
        rocketnet_address_t bound_address;
        if ( getsockname( msgx->shared_fd, (struct sockaddr *) &bound, &bound_length ) != 0 ||
             !rocketnet_server_msgx_sockaddr_to_address( &bound, msgx->ipv6, &bound_address ) )
        {
            rocketnet_error( "msgx transport: failed to read the bound port" );
            rocketnet_server_msgx_free( msgx );
            return NULL;
        }
        bind_address->port = bound_address.port;
    }
    msgx->bind = *bind_address;

    msgx->kq = kqueue();
    if ( msgx->kq < 0 || !rocketnet_server_msgx_register( msgx, msgx->shared_fd, -1, 0 ) )
    {
        rocketnet_error( "msgx transport: failed to create its kqueue: %s", strerror( errno ) );
        rocketnet_server_msgx_free( msgx );
        return NULL;
    }

    // threads last, so a partial create has no thread to chase
    for ( int w = 0; w < num_workers; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        if ( !rocketnet_platform_mutex_create( &worker->mutex ) )
        {
            rocketnet_server_msgx_abort_create( msgx );
            return NULL;
        }
        worker->wake = rocketnet_platform_wake_create( context, NULL, status );
        if ( !worker->wake )
        {
            rocketnet_server_msgx_abort_create( msgx );
            return NULL;
        }
        worker->stopped.store( false, std::memory_order_relaxed );
        worker->thread = rocketnet_platform_thread_create( context, rocketnet_server_msgx_worker, worker, status );
        if ( !worker->thread )
        {
            worker->stopped.store( true, std::memory_order_relaxed );
            rocketnet_server_msgx_abort_create( msgx );
            return NULL;
        }
        rocketnet_platform_server_thread_priority( worker->thread );
    }

    struct rocketnet_server_msgx_receiver_t * receiver = msgx->receiver;
    if ( !rocketnet_platform_mutex_create( &receiver->mutex ) )
    {
        rocketnet_server_msgx_abort_create( msgx );
        return NULL;
    }
    receiver->stopped.store( false, std::memory_order_relaxed );
    receiver->thread = rocketnet_platform_thread_create( context, rocketnet_server_msgx_receive_worker, msgx, status );
    if ( !receiver->thread )
    {
        receiver->stopped.store( true, std::memory_order_relaxed );
        rocketnet_server_msgx_abort_create( msgx );
        return NULL;
    }
    rocketnet_platform_server_thread_priority( receiver->thread );

    rocketnet_info( "msgx transport: port %d, %d transmit workers, a connected socket per client (up to %d)",
                    msgx->bind.port, num_workers, max_clients );
    return msgx;
}

void rocketnet_server_msgx_begin_shutdown( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        worker->shutdown.store( true, std::memory_order_release );
        if ( worker->wake )
        {
            rocketnet_platform_wake_set( worker->wake );
        }
    }
}

// proofs/tla/RNGsoHandoff.tla MainStop: quit for every worker; each worker's
// last producer passes to it with the store
void rocketnet_server_msgx_stop( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    if ( msgx->stopping )
    {
        return;
    }
    msgx->stopping = true;
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        if ( !worker )
        {
            continue;
        }
        worker->shutdown.store( true, std::memory_order_release );
        worker->quit.store( true, std::memory_order_release );
        if ( worker->wake )
        {
            rocketnet_platform_wake_set( worker->wake );
        }
    }
    if ( msgx->receiver )
    {
        // RNMsgxReceiveFill MainStop: main hands the server nothing more
        // (rocketnet_server_msgx_receive_packets); what is still banked is
        // counted once the worker has exited (StopCount, in
        // rocketnet_server_msgx_stopped)
        msgx->receiver->quit.store( true, std::memory_order_release );
    }
}

bool rocketnet_server_msgx_stopped( struct rocketnet_server_msgx_t * msgx )
{
    if ( !msgx || !msgx->stopping )
    {
        return false;
    }
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        if ( msgx->workers[w] && !msgx->workers[w]->stopped.load( std::memory_order_acquire ) )
        {
            return false;
        }
    }
    if ( msgx->receiver && !msgx->receiver->stopped.load( std::memory_order_acquire ) )
    {
        return false;
    }
    rocketnet_server_msgx_count_stopped( msgx );
    return true;
}

static void rocketnet_server_msgx_log_stats( struct rocketnet_server_msgx_t * msgx )
{
    struct rocketnet_server_msgx_stats_t counters;
    rocketnet_server_msgx_stats( msgx, &counters );
    rocketnet_info( "msgx transport stats: sent %llu (connected %llu, shared %llu) calls %llu dropped %llu refused %llu retired %llu dead %llu "
                    "staging_full %llu received %llu receive_calls %llu receive_refused %llu sockets opened %llu closed %llu open %llu "
                    "open_failed %llu deferred %llu table_full %llu retired_forget %llu retired_dead %llu cooldown %llu quarantine %llu",
                    (unsigned long long) counters.packets_sent,
                    (unsigned long long) counters.sent_connected,
                    (unsigned long long) counters.sent_shared,
                    (unsigned long long) counters.send_calls,
                    (unsigned long long) counters.send_dropped,
                    (unsigned long long) counters.send_refused,
                    (unsigned long long) counters.send_retired,
                    (unsigned long long) counters.socket_dead,
                    (unsigned long long) counters.staging_full,
                    (unsigned long long) counters.packets_received,
                    (unsigned long long) counters.receive_calls,
                    (unsigned long long) counters.receive_refused,
                    (unsigned long long) counters.sockets_opened,
                    (unsigned long long) counters.sockets_closed,
                    (unsigned long long) counters.sockets_open,
                    (unsigned long long) counters.socket_open_failed,
                    (unsigned long long) counters.socket_open_deferred,
                    (unsigned long long) counters.socket_table_full,
                    (unsigned long long) counters.sockets_retired_forget,
                    (unsigned long long) counters.sockets_retired_dead,
                    (unsigned long long) counters.socket_cooldown,
                    (unsigned long long) counters.sockets_in_quarantine );
}

// proofs/tla/RNGsoHandoff.tla Join and Free: every worker joined before any
// descriptor is closed or any storage freed
void rocketnet_server_msgx_destroy( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    const bool stopped = rocketnet_server_msgx_stopped( msgx );
    rocketnet_assert( stopped );
    if ( !stopped )
    {
        return;
    }
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        if ( worker && worker->thread )
        {
            rocketnet_platform_thread_join( worker->thread );
            rocketnet_platform_thread_destroy( worker->thread );
            worker->thread = NULL;
        }
    }
    if ( msgx->receiver && msgx->receiver->thread )
    {
        rocketnet_platform_thread_join( msgx->receiver->thread );
        rocketnet_platform_thread_destroy( msgx->receiver->thread );
        msgx->receiver->thread = NULL;
    }
    if ( msgx->table && msgx->receiver )
    {
        rocketnet_server_msgx_log_stats( msgx );
    }
    rocketnet_server_msgx_free( msgx );
}

void rocketnet_server_msgx_stats( struct rocketnet_server_msgx_t * msgx, struct rocketnet_server_msgx_stats_t * stats )
{
    rocketnet_assert( msgx );
    rocketnet_assert( stats );
    memset( stats, 0, sizeof( *stats ) );
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        const struct rocketnet_server_msgx_worker_t * worker = msgx->workers[w];
        if ( !worker )
        {
            continue;
        }
        stats->sent_connected += worker->sent_connected.load( std::memory_order_relaxed );
        stats->sent_shared += worker->sent_shared.load( std::memory_order_relaxed );
        stats->send_calls += worker->send_calls.load( std::memory_order_relaxed );
        stats->send_dropped += worker->dropped.load( std::memory_order_relaxed );
        stats->send_refused += worker->refused.load( std::memory_order_relaxed );
        stats->send_retired += worker->retired.load( std::memory_order_relaxed );
        stats->socket_dead += worker->dead.load( std::memory_order_relaxed );
        stats->staging_full += worker->staging_full.load( std::memory_order_relaxed );
    }
    stats->packets_sent = stats->sent_connected + stats->sent_shared;
    stats->packets_received = msgx->packets_received;
    stats->receive_calls = msgx->receive_calls;
    stats->receive_refused = msgx->receive_refused;
    const struct rocketnet_msgx_table_counts_t * table = &msgx->table->counts;
    stats->sockets_opened = table->opened;
    stats->sockets_closed = table->closed;
    stats->sockets_open = (uint64_t) msgx->table->num_open;
    stats->socket_open_failed = table->open_failed;
    stats->socket_open_deferred = table->open_deferred;
    stats->socket_table_full = table->table_full;
    stats->sockets_retired_forget = table->retired_forget;
    stats->sockets_retired_dead = table->retired_dead;
    stats->socket_cooldown = table->cooldown;
    stats->sockets_in_quarantine = (uint64_t) msgx->table->num_retired;
    stats->shared_receive_buffer = (uint64_t) msgx->shared_receive_buffer;
}

uint64_t rocketnet_server_msgx_send_failures( struct rocketnet_server_msgx_t * msgx )
{
    rocketnet_assert( msgx );
    uint64_t dropped = 0;
    for ( int w = 0; w < msgx->num_workers; w++ )
    {
        if ( msgx->workers[w] )
        {
            dropped += msgx->workers[w]->dropped.load( std::memory_order_relaxed );
        }
    }
    // and the frames a refused finish never staged
    dropped += msgx->finish_refused.load( std::memory_order_relaxed );
    return dropped;
}

#endif // #if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
