#include <new>
/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"

#if ROCKETNET_XDP

// Finish one AF_XDP RX pass after peeking num_packets descriptors and
// consuming num_consumed of them while the staging buffer had room: cancel the
// peeked descriptors not consumed before releasing num_consumed (modelled in
// proofs/tla/RNXdpReceiveRing.tla, Finish).
void rocketnet_xdp_receive_ring_finish( uint32_t num_packets, uint32_t num_consumed,
                                        void ( *cancel )( void * ring, uint32_t count ),
                                        void ( *release )( void * ring, uint32_t count ),
                                        void * ring )
{
    rocketnet_assert( num_consumed <= num_packets );
    if ( num_consumed < num_packets )
        cancel( ring, num_packets - num_consumed );
    release( ring, num_consumed );
}

#include "rocketnet_server_socket.h"
#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"
#include "rocketnet_xdp_frame.h"
#include "rocketnet_driver.h"
#include "rocketnet_hash.h"

#include <ifaddrs.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "rocketnet_xdp_compat.h"
#include <sys/resource.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

#include <rocketnet_server_xdp.h>
#include "rocketnet_pending_frames.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_xdp_replay_marks.h"
#include "rocketnet_memory.h"
#include "rocketnet_xdp_free_list.h"
#include "rocketnet_xdp_socket_config.h"
#include "rocketnet_gateway_netlink.h"
#include "rocketnet_replay.h"

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <atomic>

// the kernel prefilter drops nothing the session replay window would accept
static_assert( ROCKETNET_XDP_REPLAY_WINDOW == ROCKETNET_REPLAY_WINDOW_BITS, "the kernel replay window is the userspace replay window" );

// XDP umem frame size must match libxdp expectation
static_assert( ROCKETNET_XDP_FRAME_SIZE == 4096, "ROCKETNET_XDP_FRAME_SIZE must be 4096" );

struct alignas( 64 ) rocketnet_server_socket_send_packet_info_t
{
    rocketnet_address_t to;
    size_t packet_bytes;
    uint8_t destination_mac[ETH_ALEN];  // chosen at start by the reply-MAC rule
    bool mac_learned;                   // destination_mac is the client's learned MAC
};

// IMPORTANT: The 4096 byte padding walls in these structs are load-bearing. Each field
// between walls is touched by a different thread, and without the walls those fields
// would share cache lines and false sharing would destroy multithreaded send and
// receive performance. Nothing goes wrong if you remove them -- it just gets slow.
// Do not remove them.

struct rocketnet_server_socket_send_buffer_t
{
    uint8_t padding_0[4096];

    std::atomic<int> num_packets{ 0 };

    uint8_t padding_1[4096];

    int packet_start_index;

    uint8_t padding_2[4096];

    rocketnet_server_socket_send_packet_info_t packet_info[ROCKETNET_XDP_SEND_QUEUE_SIZE];

    uint8_t padding_3[4096];

    alignas( 16 ) uint8_t packet_data[ROCKETNET_XDP_SEND_QUEUE_SIZE][ROCKETNET_XDP_FRAME_SIZE];

    uint8_t padding_4[4096];
};

struct rocketnet_server_socket_receive_buffer_t
{
    uint8_t padding_0[4096];

    uint8_t padding_1[4096];

    uint32_t num_packets = 0;
    uint8_t eth[ROCKETNET_XDP_RECV_QUEUE_SIZE][ETH_ALEN];
    rocketnet_address_t from[ROCKETNET_XDP_RECV_QUEUE_SIZE];
    size_t packet_bytes[ROCKETNET_XDP_RECV_QUEUE_SIZE];
    uint8_t packet_data[ROCKETNET_XDP_RECV_QUEUE_SIZE][ROCKETNET_XDP_FRAME_SIZE];

    uint8_t padding_2[4096];
};

// Preallocated staging ownership. All methods are called with the matching
// handoff mutex held; they exchange indices only, never packet bytes. The
// producer is exclusive, ready buffers wait in FIFO order, and a claimed
// buffer remains owned by its consumer until recycle().
template <int N> struct rocketnet_xdp_buffer_exchange_t
{
    static_assert( N >= 3, "producer, consumer and spare XDP buffers" );
    int producer = 0;
    int free_index[(size_t) N] = {};
    int free_head = 0;
    int free_count = N - 1;
    int ready_index[(size_t) N] = {};
    int ready_head = 0;
    int ready_count = 0;

    rocketnet_xdp_buffer_exchange_t()
    {
        for ( int i = 1; i < N; ++i )
            free_index[i - 1] = i;
    }

    bool publish()
    {
        if ( free_count == 0 )
            return false;
        const int old = producer;
        producer = free_index[free_head];
        free_head = ( free_head + 1 ) % N;
        --free_count;
        ready_index[( ready_head + ready_count ) % N] = old;
        ++ready_count;
        return true;
    }

    int claim()
    {
        if ( ready_count == 0 )
            return -1;
        const int index = ready_index[ready_head];
        ready_head = ( ready_head + 1 ) % N;
        --ready_count;
        return index;
    }

    void recycle( int index )
    {
        rocketnet_assert( index >= 0 && index < N && free_count < N );
        free_index[( free_head + free_count ) % N] = index;
        ++free_count;
    }
};

struct rocketnet_server_xdp_socket_t
{
    uint8_t padding_0[4096];

    int queue;
    int num_os_cpus;

    uint8_t padding_1[4096];

    void * buffer;
    struct xsk_umem * umem;
    struct xsk_ring_cons receive_queue;
    struct xsk_ring_prod send_queue;
    struct xsk_ring_cons complete_queue;
    struct xsk_ring_prod fill_queue;
    struct xsk_socket * xsk;
    bool zero_copy;
    uint64_t ring_bytes;                        // the four rings' descriptors, in the direct allocation count while xsk and umem live

    uint8_t padding_2[4096];

    std::atomic<bool> quit{ 0 };
    std::atomic<bool> stopped{ false };

    uint8_t padding_4[4096];

    // frames the receive loop dropped, by reason (rocketnet_xdp_frame_receive);
    // written only by this queue's xdp thread
    struct rocketnet_xdp_receive_drops_t receive_drops;

    // frames the flush did not send because the codec refused to build them
    // (a destination of the family this server does not serve); written by
    // this queue's xdp thread outside the handoff lock
    std::atomic<uint64_t> send_refused_build{ 0 };

    // staged packets the flush did not copy because their length is 0 or
    // past ROCKETNET_RECEIVE_CEILING (rocketnet_xdp_send_copy); written by
    // this queue's xdp thread outside the handoff lock
    std::atomic<uint64_t> send_refused_length{ 0 };

    // staged packets the start path refused because the queue was full or its
    // counter had wrapped negative (rocketnet_xdp_send_claim); written by the
    // fill threads, read by this queue's xdp thread at flush time
    std::atomic<uint64_t> send_refused_overflow{ 0 };

    // the overflow count this queue's xdp thread already warned about; only
    // that thread writes it
    uint64_t send_refused_overflow_warned{ 0 };

    uint8_t padding_5[4096];

    rocketnet_xdp_buffer_exchange_t<ROCKETNET_XDP_NUM_BUFFERS> send_handoff;
    rocketnet_xdp_buffer_exchange_t<ROCKETNET_XDP_NUM_BUFFERS> receive_handoff;
    int receive_held_index = -1; // game view; returned at the next receive call
    uint32_t receive_read_index = 0;

    uint8_t padding_6[4096];

    rocketnet_platform_thread_t * xdp_thread;

    rocketnet_platform_mutex_t send_mutex;
    rocketnet_platform_mutex_t receive_mutex;

    int num_free_send_frames;
    uint64_t send_frames[ROCKETNET_XDP_NUM_FRAMES / 2];
    std::atomic<int> send_flow_index{ -1 };

    uint8_t server_ethernet_address[ETH_ALEN];
    rocketnet_address_t server_address;     // the public address and port (host-order port): its family is the family this socket serves
    int serves_both;                        // 1: ipv4_address and ipv6_address are both live
    rocketnet_address_t ipv4_address;
    rocketnet_address_t ipv6_address;

    struct rocketnet_server_socket_receive_buffer_t receive_buffer[ROCKETNET_XDP_NUM_BUFFERS];

    struct rocketnet_server_socket_send_buffer_t send_buffer[ROCKETNET_XDP_NUM_BUFFERS];

    uint8_t padding_7[4096];
};

struct rocketnet_server_socket_t
{
    void * context;
    int state;
    rocketnet_address_t public_address;

    uint8_t server_ethernet_address[ETH_ALEN];
    uint8_t gateway_ethernet_address[ETH_ALEN];
    bool gateway_known;

    // the reply-MAC rule (rocketnet_xdp_frame.h): learned from the source MAC
    // of each packet the transport accepts for a connection (client_accepted),
    // written on the main thread while the transport processes a receive
    // view, read by start_packet; fill threads only read it, and they are
    // joined before each receive
    struct rocketnet_xdp_reply_macs_t reply_macs;
    uint8_t process_eth[ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE][ETH_ALEN];

    int interface_index;
    struct bpf_object * program_object;  // opened from the embedded server_xdp.o; libxdp does not own it, so destroy closes it after the program
    rocketnet_xdp_program_t * program;
    bool attached_native;
    bool attached_skb;
    char pin_root[160];                 // this INSTANCE's own bpffs directory, named by a random token with its identity metadata encoded: created by this create, adopted never
    bool pin_root_created;              // set only when this instance's mkdir succeeded -- destroy removes nothing it did not make
    int num_pinned;                     // the pin paths recorded BY CONSTRUCTION at mkdir (their names are deterministic) -- destroy unlinks exactly these
    char pinned_paths[4][200];
    int config_map_fd = -1;             // -1 until bpf_obj_get returns a descriptor: 0 is a valid one
    int socket_map_fd = -1;
    int stats_map_fd = -1;
    int replay_map_fd = -1;
    struct rocketnet_xdp_replay_marks_t * replay_marks;     // pending forgets and marks, this socket owns it
    uint64_t replay_commit_failures;
    bool replay_zero_mark_refused;                          // a zero mark the buffer refused since the last commit
    bool replay_hold_all_greetings;                         // the last commit had one: hold every keep-alive
    uint64_t replay_greetings_held;                         // keep-alives held for a mark not yet applied

    std::atomic<uint64_t> packet_id{ 0 };

    int num_queues;
    int receive_start_queue; // the next receive_packets call starts at this queue
    rocketnet_server_xdp_socket_t * socket;

    rocketnet_server_socket_process_packets_t process_packets;

    // the per-cpu stats map read buffer (rocketnet_xdp_stats_cpus_fit), inside
    // the create-time allocation; written only by rocketnet_server_socket_xdp_counters
    struct rocketnet_server_xdp_stats_t stats_values[ROCKETNET_XDP_STATS_MAX_CPUS];

    int serves_both;
    rocketnet_address_t ipv4_address;
    rocketnet_address_t ipv6_address;
};

void rocketnet_server_socket_destroy( rocketnet_server_socket_t * server );
static void rocketnet_server_socket_abort_create( rocketnet_server_socket_t * server )
{
    rocketnet_server_socket_stop( server );
    while ( !rocketnet_server_socket_stopped( server ) )
        rocketnet_platform_sleep( 0.001 );
    rocketnet_server_socket_destroy( server );
}

static bool get_interface_mac_address( const char * interface_name, uint8_t * mac_address )
{
    char path[256];
    const int path_length = snprintf( path, sizeof( path ), "/sys/class/net/%s/address", interface_name );
    if ( path_length < 0 || (size_t) path_length >= sizeof( path ) )
    {
        return false;
    }

    FILE * file = fopen( path, "r" );
    if ( !file )
    {
        return false;
    }

    char mac_address_string[64];

    if ( fgets( mac_address_string, sizeof( mac_address_string ), file ) == NULL )
    {
        fclose( file );
        rocketnet_error( "could not parse mac address of network interface %s from %s (xx:xx:xx:xx:xx:xx)", interface_name, path );
        return false;
    }

    fclose( file );

    // the same strict parse the override uses, so a short or empty line refuses

    if ( !rocketnet_xdp_parse_mac_address( mac_address_string, mac_address ) )
    {
        rocketnet_error( "could not parse mac address of network interface %s from %s (xx:xx:xx:xx:xx:xx)", interface_name, path );
        return false;
    }

    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };
    if ( memcmp( mac_address, zero_mac, 6 ) == 0 )
    {
        rocketnet_error( "mac address of network interface %s is all-zero in %s", interface_name, path );
        return false;
    }

    return true;
}

// the gateway's link-layer address for one family on this interface, through
// rtnetlink (sdk/rocketnet_gateway_netlink.h): the default route's next hop
// on the interface, then that next hop's neighbour entry. No default route
// on the interface returns true with *found_route false, the one case the
// server starts without a gateway; a gateway with no usable neighbour entry
// carrying a link-layer address refuses, and nothing resolves it.

static bool get_gateway_mac_address_netlink( const char * interface_name, int interface_index, int address_type, uint8_t * mac_address, bool * found_route )
{
    *found_route = false;

    const bool ipv6 = address_type == ROCKETNET_ADDRESS_IPV6;
    const int family = ipv6 ? AF_INET6 : AF_INET;
    const char * family_name = ipv6 ? "ipv6" : "ipv4";

    uint8_t gateway[16];
    memset( gateway, 0, sizeof( gateway ) );

    int error = 0;
    const int route = rocketnet_gateway_netlink_route( family, interface_index, gateway, &error );
    if ( route == ROCKETNET_GATEWAY_NETLINK_DONE )
        return true;
    if ( route != ROCKETNET_GATEWAY_NETLINK_FOUND )
    {
        rocketnet_error( "%s gateway discovery: route query on %s failed: %s (%s)", family_name, interface_name,
                         rocketnet_gateway_netlink_result_name( route ), strerror( error ) );
        return false;
    }

    *found_route = true;

    char gateway_ip_string[INET6_ADDRSTRLEN];
    if ( !inet_ntop( family, gateway, gateway_ip_string, sizeof( gateway_ip_string ) ) )
        strcpy( gateway_ip_string, "?" );

    const int neighbor = rocketnet_gateway_netlink_neighbor( family, interface_index, gateway, mac_address, &error );
    if ( neighbor == ROCKETNET_GATEWAY_NETLINK_FOUND )
        return true;
    if ( neighbor == ROCKETNET_GATEWAY_NETLINK_DONE )
    {
        rocketnet_error( "%s gateway discovery: no neighbor entry with a link-layer address for %s on %s", family_name, gateway_ip_string, interface_name );
        return false;
    }

    rocketnet_error( "%s gateway discovery: neighbor query for %s on %s failed: %s (%s)", family_name, gateway_ip_string, interface_name,
                     rocketnet_gateway_netlink_result_name( neighbor ), strerror( error ) );
    return false;
}

// false refuses create (a malformed, zero or multicast MAC, or a discovery
// that could not complete). true with *found false: this interface has no
// default route for the server's family, the one case the server starts
// without a gateway. address_type picks the family discovery asks
// rtnetlink about.

static bool get_gateway_mac_address( const char * interface_name, int interface_index, int address_type, uint8_t * mac_address, bool * found )
{
    memset( mac_address, 0, 6 );
    *found = false;

    // a parameter override under the loud-override law: not a config
    // table field (it overrides a discovered value, not a declared one),
    // but it reads through the platform wrapper, logs what it displaces
    // (the address discovery it preempts), and STRICT-PARSES like every
    // other env value -- a malformed MAC refuses loudly rather than
    // silently becoming 00:00:00:00:00:00 through strtol

    // a value too long for the buffer is set, and refused below as no mac address

    char gateway_ethernet_address_env[64];
    const int gateway_ethernet_address_found = rocketnet_platform_getenv( "ROCKETNET_GATEWAY_ETHERNET_ADDRESS", gateway_ethernet_address_env, sizeof( gateway_ethernet_address_env ) );
    if ( gateway_ethernet_address_found < 0 || ( gateway_ethernet_address_found > 0 && gateway_ethernet_address_env[0] != '\0' ) )
    {
        // the same strict parse the sysfs read uses

        if ( gateway_ethernet_address_found <= 0 || !rocketnet_xdp_parse_mac_address( gateway_ethernet_address_env, mac_address ) )
        {
            rocketnet_error( "could not parse ROCKETNET_GATEWAY_ETHERNET_ADDRESS=\"%s\" as a mac address (xx:xx:xx:xx:xx:xx)", gateway_ethernet_address_env );
            return false;
        }

        rocketnet_info( "server config override ROCKETNET_GATEWAY_ETHERNET_ADDRESS=%s (displaces gateway discovery)", gateway_ethernet_address_env );
    }
    else
    {
        bool found_route = false;
        if ( !get_gateway_mac_address_netlink( interface_name, interface_index, address_type, mac_address, &found_route ) )
            return false;
        if ( !found_route )
            return true;
    }

    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };
    if ( ( mac_address[0] & 1 ) || memcmp( mac_address, zero_mac, 6 ) == 0 )
    {
        rocketnet_error( "gateway mac address %02x:%02x:%02x:%02x:%02x:%02x is zero or multicast",
                         mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4], mac_address[5] );
        return false;
    }

    *found = true;

    return true;
}

#define INVALID_FRAME UINT64_MAX

static uint64_t alloc_send_frame( rocketnet_server_xdp_socket_t * socket )
{
    uint64_t frame = INVALID_FRAME;

    if ( socket->num_free_send_frames > 0 )
    {
        socket->num_free_send_frames--;
        frame = socket->send_frames[socket->num_free_send_frames];
        socket->send_frames[socket->num_free_send_frames] = INVALID_FRAME;
    }

    return frame;
}

static void free_send_frame( rocketnet_server_xdp_socket_t * socket, uint64_t frame )
{
    const uint64_t region_begin = 0;
    const uint64_t region_end = (uint64_t) ROCKETNET_XDP_NUM_FRAMES / 2 * ROCKETNET_XDP_FRAME_SIZE;
    if ( !rocketnet_xdp_free_list_push( socket->send_frames, &socket->num_free_send_frames, ROCKETNET_XDP_NUM_FRAMES / 2, frame, region_begin, region_end ) )
    {
        rocketnet_error( "xdp send frame dropped: %llu", (unsigned long long) frame );
    }
}

static void rocketnet_server_socket_xdp_thread_function( rocketnet_platform_thread_t *, void * data );

// the public address as the 16 network-order bytes the XDP config carries
// (the rocketnet address stores IPv6 as host-order 16-bit groups)

static void rocketnet_server_socket_ipv6_bytes( const rocketnet_address_t * address, uint8_t * bytes )
{
    for ( int i = 0; i < 8; i++ )
    {
        bytes[i * 2] = (uint8_t) ( address->data.ipv6[i] >> 8 );
        bytes[i * 2 + 1] = (uint8_t) ( address->data.ipv6[i] & 0xFF );
    }
}

// a server's public address is global: an IPv6 link-local address
// (fe80::/10) names no route off the link, so the pre-flight and create
// refuse it by name

static bool rocketnet_server_socket_ipv6_link_local( const rocketnet_address_t * address )
{
    return address->type == ROCKETNET_ADDRESS_IPV6 && ( address->data.ipv6[0] & 0xFFC0 ) == 0xFE80;
}

// the network interface carrying the public address, read-only -- shared by
// the pre-flight and create so they cannot disagree about which interface
// the transport would use. IPv4 matches an AF_INET address, IPv6 an AF_INET6
// address, each on an UP interface.

static bool rocketnet_server_socket_find_interface( const rocketnet_address_t * public_address, char * interface_name, size_t interface_name_size, unsigned int * interface_index )
{
    interface_name[0] = '\0';
    *interface_index = 0;

    if ( public_address->type != ROCKETNET_ADDRESS_IPV4 && public_address->type != ROCKETNET_ADDRESS_IPV6 )
        return false;

    uint32_t public_address_ipv4 = 0;
    uint8_t public_address_ipv6[16];
    memset( public_address_ipv6, 0, sizeof( public_address_ipv6 ) );
    if ( public_address->type == ROCKETNET_ADDRESS_IPV4 )
        memcpy( &public_address_ipv4, public_address->data.ipv4, 4 );     // network order, as sin_addr holds it
    else
        rocketnet_server_socket_ipv6_bytes( public_address, public_address_ipv6 );

    struct ifaddrs * addrs;
    if ( getifaddrs( &addrs ) != 0 )
        return false;

    bool found = false;

    for ( struct ifaddrs * iap = addrs; iap != NULL; iap = iap->ifa_next )
    {
        if ( !iap->ifa_addr || !( iap->ifa_flags & IFF_UP ) )
            continue;

        bool match = false;

        if ( public_address->type == ROCKETNET_ADDRESS_IPV4 && iap->ifa_addr->sa_family == AF_INET )
        {
            const struct sockaddr_in * sa = (const struct sockaddr_in *) iap->ifa_addr;
            match = sa->sin_addr.s_addr == public_address_ipv4;
        }
        else if ( public_address->type == ROCKETNET_ADDRESS_IPV6 && iap->ifa_addr->sa_family == AF_INET6 )
        {
            const struct sockaddr_in6 * sa = (const struct sockaddr_in6 *) iap->ifa_addr;
            match = memcmp( sa->sin6_addr.s6_addr, public_address_ipv6, 16 ) == 0;
        }

        if ( match )
        {
            strncpy( interface_name, iap->ifa_name, interface_name_size - 1 );
            interface_name[interface_name_size - 1] = '\0';
            *interface_index = if_nametoindex( iap->ifa_name );
            found = *interface_index != 0;
            break;
        }
    }

    freeifaddrs( addrs );

    return found;
}

// read one small sysfs value; returns false when the file does not exist

static bool rocketnet_server_socket_read_sysfs( const char * path, char * value, size_t value_size )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
        return false;
    value[0] = '\0';
    if ( fgets( value, (int) value_size, file ) )
    {
        char * newline = strchr( value, '\n' );
        if ( newline )
            *newline = '\0';
    }

    fclose( file );

    return true;
}

// THE COMMIT BARRIER: a process that must never commit an XDP data plane
// -- the test suite installs this once, structurally, for every present
// and future test -- forbids the committing create outright. the
// pre-flight stays reachable (it is non-mutating by contract); create
// refuses at the top, before its first act. allow exists for a caller
// that deliberately opts back in; nothing in the suite does.

static std::atomic<bool> rocketnet_server_socket_commit_forbidden_flag( false );

void rocketnet_server_socket_forbid_commit()
{
    rocketnet_server_socket_commit_forbidden_flag.store( true );
}

#if ROCKETNET_DEV
void rocketnet_server_socket_allow_commit()
{
    rocketnet_server_socket_commit_forbidden_flag.store( false );
}
#endif // #if ROCKETNET_DEV

#if defined( ROCKETNET_DEV ) && ROCKETNET_DEV
// The suite's development control: select the pre-flight answer so the
// production pin and ladder decisions can be asserted on a host that is
// neither admitted nor provisioned. It guards only this non-mutating
// predicate; a create that reaches an attach still needs the commit barrier
// lifted, which the suite never does for an available answer.

static std::atomic<int> rocketnet_server_socket_preflight_test_result( -1 );

void rocketnet_server_socket_set_preflight_test_result( int result )
{
    rocketnet_assert( result == 0 || result == 1 );
    rocketnet_server_socket_preflight_test_result.store( result );
}

void rocketnet_server_socket_clear_preflight_test_result()
{
    rocketnet_server_socket_preflight_test_result.store( -1 );
}
#endif // #if defined( ROCKETNET_DEV ) && ROCKETNET_DEV

// cpu list helpers for the boot-tier checks: "1-7,9" style lists as
// /sys/devices/system/cpu/isolated and /proc/irq/N/smp_affinity_list
// print them

static bool rocketnet_server_socket_cpu_in_list( const char * list, int cpu )
{
    const char * cursor = list;
    while ( *cursor )
    {
        int first = -1;
        int last = -1;
        char * end = NULL;
        first = (int) strtol( cursor, &end, 10 );
        if ( end == cursor )
            break;
        cursor = end;
        last = first;

        if ( *cursor == '-' )
        {
            cursor++;
            last = (int) strtol( cursor, &end, 10 );
            if ( end == cursor )
                break;
            cursor = end;
        }

        if ( cpu >= first && cpu <= last )
            return true;
        if ( *cursor == ',' )
            cursor++;
    }

    return false;
}

static int rocketnet_server_socket_count_cpu_list( const char * list )
{
    int count = 0;
    const char * cursor = list;
    while ( *cursor )
    {
        int first = -1;
        int last = -1;
        char * end = NULL;
        first = (int) strtol( cursor, &end, 10 );
        if ( end == cursor )
            break;
        cursor = end;
        last = first;

        if ( *cursor == '-' )
        {
            cursor++;
            last = (int) strtol( cursor, &end, 10 );
            if ( end == cursor )
                break;
            cursor = end;
        }

        if ( last >= first )
            count += last - first + 1;
        if ( *cursor == ',' )
            cursor++;
    }

    return count;
}

// The R4 interrupt-line matcher, one boundary definition implemented
// twice (this function and the script's awk form, held to the same case
// set by the fixture test here and the script's selftest): the interface
// name matches as a bounded token -- preceded by line start, whitespace,
// or '-' (transport-prefixed names like i40e-ens1f0-TxRx-0, the exact
// family B3 tells operators to build) and followed by '-', ':', '@',
// whitespace, or end. pure string logic, exported for the fixture test.
// accepted asymmetries with the script's form, recorded: this reader
// splits lines past its 4096-byte buffer where awk is unbounded (real
// /proc/interrupts lines are far shorter); '\r' rides this after-set for
// fgets robustness but is unreachable through awk (no CRLF procfs); the
// empty interface name refuses here and is unreachable in the script
// (empty arguments refuse at the door).

bool rocketnet_server_socket_interrupt_line_matches_interface( const char * line, const char * interface_name )
{
    rocketnet_assert( line );
    rocketnet_assert( interface_name );

    const size_t name_length = strlen( interface_name );
    if ( name_length == 0 )
        return false;

    const char * match = strstr( line, interface_name );
    while ( match )
    {
        const char before = match == line ? ' ' : match[-1];
        const char after = match[name_length];
        const bool before_ok = before == ' ' || before == '\t' || before == '-';
        const bool after_ok = after == '-' || after == ':' || after == '@' || after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '\0';
        if ( before_ok && after_ok )
            return true;
        match = strstr( match + 1, interface_name );
    }

    return false;
}

// a kernel command line token, bounded: preceded by start or space,
// followed by space or end -- "irqaffinity=0" must not match
// "irqaffinity=0-3" or "irqaffinity=012"

static bool rocketnet_server_socket_cmdline_has_token( const char * cmdline, const char * token )
{
    const size_t token_length = strlen( token );
    const char * match = strstr( cmdline, token );
    while ( match )
    {
        const char before = match == cmdline ? ' ' : match[-1];
        const char after = match[token_length];
        if ( ( before == ' ' ) && ( after == ' ' || after == '\n' || after == '\0' ) )
            return true;
        match = strstr( match + 1, token );
    }

    return false;
}

// THE PRE-FLIGHT: non-mutating, checking the two-tier condition
// list in scripts/rocketnet-provision's header -- the single documented
// source this check, that script and BUILDING.md all derive from. Every
// unmet condition names the knob, the wanted value, and the script;
// nothing here changes machine state (probe descriptors are created and
// closed, never bound, never attached). All conditions that can be
// checked from here are checked, so one run names every errand at once
// rather than one per attempt.

uint64_t rocketnet_xdp_umem_bytes( int queues )
{
    if ( queues < 1 || queues > ROCKETNET_XDP_MAX_QUEUES )
        return 0;
    uint64_t bytes = 0;
    if ( !rocketnet_memory_add( bytes, (uint64_t) queues,
                                (uint64_t) ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE ) ||
         bytes > SIZE_MAX )
        return 0;
    return bytes;
}

uint64_t rocketnet_server_xdp_allocation_bytes( int queues )
{
    if ( queues < 1 || queues > ROCKETNET_XDP_MAX_QUEUES )
        return 0;
    uint64_t bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_socket_t ),
                                                     sizeof( rocketnet_xdp_replay_marks_t ),
                                                     (uint64_t) queues * sizeof( rocketnet_server_xdp_socket_t ) } );
    if ( !bytes || !rocketnet_memory_add( bytes, (uint64_t) queues, rocketnet_platform_thread_allocation_bytes() ) )
        return 0;
    return bytes <= SIZE_MAX ? bytes : 0;
}

uint64_t rocketnet_server_socket_memory_required( int num_queues )
{
    if ( num_queues < 1 || num_queues > ROCKETNET_XDP_MAX_QUEUES )
        return 0;
    const uint64_t umem = rocketnet_xdp_umem_bytes( num_queues );
    uint64_t bytes = sizeof( rocketnet_server_socket_t );
    if ( !umem ||
         !rocketnet_memory_add( bytes, 1, sizeof( rocketnet_xdp_replay_marks_t ) ) ||
         !rocketnet_memory_add( bytes, (uint64_t) num_queues, sizeof( rocketnet_server_xdp_socket_t ) ) ||
         !rocketnet_memory_add( bytes, 1, umem ) || bytes > SIZE_MAX )
        return 0;
    return bytes;
}

static bool rocketnet_server_socket_interface_is_veth( const char * interface_name )
{
    // the transport name the kernel reports for the interface (ETHTOOL_GDRVINFO)
    const int probe_fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( probe_fd < 0 )
        return false;
    struct ethtool_drvinfo info;
    memset( &info, 0, sizeof( info ) );
    info.cmd = ETHTOOL_GDRVINFO;

    struct ifreq ifr;
    memset( &ifr, 0, sizeof( ifr ) );

    // ifr is zeroed above, so the copied name stays NUL terminated

    memcpy( ifr.ifr_name, interface_name, strnlen( interface_name, sizeof( ifr.ifr_name ) - 1 ) );
    ifr.ifr_data = (char *) &info;

    const bool veth = ioctl( probe_fd, SIOCETHTOOL, &ifr ) == 0 && strcmp( info.driver, "veth" ) == 0;
    close( probe_fd );

    return veth;
}

static bool rocketnet_server_socket_preflight_with_cpus( const char * public_address_string, int num_queues, int num_os_cpus )
{
    rocketnet_assert( public_address_string );
    rocketnet_assert( num_queues >= 1 );

#if defined( ROCKETNET_DEV ) && ROCKETNET_DEV
    const int test_result = rocketnet_server_socket_preflight_test_result.load();
    if ( test_result >= 0 )
    {
        rocketnet_info( "xdp pre-flight: development control selects %s", test_result ? "available" : "unavailable" );
        return test_result != 0;
    }
#endif // #if defined( ROCKETNET_DEV ) && ROCKETNET_DEV

    const uint64_t memory_bytes = rocketnet_server_socket_memory_required( num_queues );
    const uint64_t memory_budget = (uint64_t) ROCKETNET_XDP_MEMORY_MIB * 1024 * 1024;
    if ( !memory_bytes || memory_bytes > memory_budget )
    {
        rocketnet_warn( "xdp geometry refused: requires %llu bytes, component budget %llu bytes",
                        (unsigned long long) memory_bytes, (unsigned long long) memory_budget );
        return false;
    }

    rocketnet_info( "xdp userspace queues and UMEM require %llu bytes before kernel and thread overhead",
                    (unsigned long long) memory_bytes );

    // R1: root. the data plane requires euid 0 -- the same refusal create
    // itself would give, checked here so the ladder can name it without
    // touching anything.

    if ( geteuid() != 0 )
    {
        rocketnet_warn( "xdp pre-flight: not root (R1 of scripts/rocketnet-provision). the XDP data plane requires euid 0" );
        return false;
    }

    bool provisioned = true;

    // R2: an UP interface carries the public address

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) ||
         ( public_address.type != ROCKETNET_ADDRESS_IPV4 && public_address.type != ROCKETNET_ADDRESS_IPV6 ) )
    {
        rocketnet_warn( "xdp pre-flight: public address %s is not a parseable ipv4 or ipv6 address (R2 of scripts/rocketnet-provision)", public_address_string );
        return false;
    }

    if ( rocketnet_server_socket_ipv6_link_local( &public_address ) )
    {
        rocketnet_warn( "xdp pre-flight: public address %s is ipv6 link-local (fe80::/10); a server's public address is global (R2 of scripts/rocketnet-provision)", public_address_string );
        return false;
    }

    char interface_name[1024];
    unsigned int interface_index = 0;
    if ( !rocketnet_server_socket_find_interface( &public_address, interface_name, sizeof( interface_name ), &interface_index ) )
    {
        rocketnet_warn( "xdp pre-flight: no UP interface carries the public address (R2 of scripts/rocketnet-provision)" );
        return false;
    }

    // R3: the exclusive xdp slot -- no program of anyone's attached to the
    // interface. read-only query; the library never unloads anyone (the
    // script does the clearing, deliberately).

    {
        // every attach mode is read: a multi-mode attachment reports prog_id 0
        rocketnet_xdp_attached_t attached;
        memset( &attached, 0, sizeof( attached ) );

        const int query_result = rocketnet_xdp_query_attached( (int) interface_index, &attached );
        if ( query_result != 0 )
        {
            // silence must never verify: an unanswerable exclusivity query
            // is an unmet condition, not a pass

            // Both XDP netlink query APIs return a negative error code.

            rocketnet_warn( "xdp pre-flight: could not query %s for an attached XDP program (%s) (R3)", interface_name, strerror( -query_result ) );
            provisioned = false;
        }
        else if ( rocketnet_xdp_attached_any( &attached ) )
        {
            unsigned int attached_program_id = attached.prog_id;
            const char * attached_mode = "single";

            if ( attached.drv_prog_id != 0 )
            {
                attached_program_id = attached.drv_prog_id;
                attached_mode = "native";
            }
            else if ( attached.skb_prog_id != 0 )
            {
                attached_program_id = attached.skb_prog_id;
                attached_mode = "generic";
            }
            else if ( attached.hw_prog_id != 0 )
            {
                attached_program_id = attached.hw_prog_id;
                attached_mode = "offload";
            }

            rocketnet_warn( "xdp pre-flight: an XDP program (id %u, %s mode) is already attached to %s (R3): run scripts/rocketnet-provision %s %d", attached_program_id, attached_mode, interface_name, interface_name, num_queues );
            provisioned = false;
        }
    }

    // B1: the boot tier's kernel command line -- irqaffinity=0 exactly,
    // isolcpus= and nohz_full= with values (nosmt's effect is verified by
    // state under R6). grub changes, so the script can only verify these
    // too; the errand is a reboot, named as one.

    char value[4096];
    if ( rocketnet_server_socket_read_sysfs( "/proc/cmdline", value, sizeof( value ) ) )
    {
        if ( !rocketnet_server_socket_cmdline_has_token( value, "irqaffinity=0" ) )
        {
            rocketnet_warn( "xdp pre-flight: kernel command line lacks irqaffinity=0 -- exactly that token (B1): grub + reboot, per scripts/rocketnet-provision" );
            provisioned = false;
        }

        const char * ranged_tokens[2] = { "isolcpus=", "nohz_full=" };

        for ( int i = 0; i < 2; i++ )
        {
            // left-bounded like every other token: "xisolcpus=1-7" is not
            // the parameter. accepted-unreachable divergence, recorded: a
            // command line carrying the same token twice (one valid, one
            // malformed) can grade differently here than in the script --
            // this check takes the first left-bounded occurrence, the
            // script's shell pattern answers for the line as a whole.

            const char * token = strstr( value, ranged_tokens[i] );
            while ( token && token != value && token[-1] != ' ' )
                token = strstr( token + 1, ranged_tokens[i] );

            if ( !token || token[strlen( ranged_tokens[i] )] == '\0' || token[strlen( ranged_tokens[i] )] == ' ' )
            {
                rocketnet_warn( "xdp pre-flight: kernel command line lacks %s<range> (B1): grub + reboot, per scripts/rocketnet-provision", ranged_tokens[i] );
                provisioned = false;
            }
        }
    }
    else
    {
        // unreadable is unmet, exactly as the script's empty read fails
        // every B1 token -- silence must never verify

        rocketnet_warn( "xdp pre-flight: /proc/cmdline is unreadable (B1): cannot verify the boot tier" );
        provisioned = false;
    }

    // B2: the isolated cpu set exists and is non-empty -- isolcpus= taken
    // effect. an absent file is the same errand as an empty one, in both
    // this check and the script.

    char isolated[1024];
    bool have_isolated = rocketnet_server_socket_read_sysfs( "/sys/devices/system/cpu/isolated", isolated, sizeof( isolated ) ) && isolated[0] != '\0';
    if ( !have_isolated )
    {
        rocketnet_warn( "xdp pre-flight: no isolated cpus (B2): B1's isolcpus= has not taken effect (grub + reboot, per scripts/rocketnet-provision)" );
        provisioned = false;
    }

    // R4's scaling clause: one queue wants one isolated core

    if ( have_isolated && rocketnet_server_socket_count_cpu_list( isolated ) < num_queues )
    {
        rocketnet_warn( "xdp pre-flight: only %d isolated cpus for %d queues (R4 scales with B1): one queue wants one isolated core", rocketnet_server_socket_count_cpu_list( isolated ), num_queues );
        provisioned = false;
    }

    // R4's pin targets: the threads pin to num_os_cpus .. num_os_cpus +
    // num_queues - 1, and every one of those cpus must be isolated

    if ( have_isolated )
    {
        int first_missing = -1;
        if ( !rocketnet_xdp_pin_targets_isolated( isolated, num_os_cpus, num_queues, &first_missing ) )
        {
            rocketnet_warn( "xdp pre-flight: pin target cpu %d is not in the isolated set %s (R4): one queue, one core -- run scripts/rocketnet-provision %s %d", first_missing, isolated, interface_name, num_queues );
            provisioned = false;
        }
    }

    // R4: each of the interface's queue IRQs is pinned to exactly one cpu
    // FROM THE ISOLATED SET, no cpu doubled -- a range or comma is the
    // scheduler default, a non-isolated cpu (0 included, which
    // irqaffinity=0 parks everything on) is not a pin, and two queues on
    // one core is not one-queue-one-core. interfaces with no per-queue
    // IRQs (VMs) pass. the interface name matches as a whole token, never
    // a substring ("lo" must not match "velocity0").

    {
        FILE * interrupts = fopen( "/proc/interrupts", "r" );
        if ( !interrupts )
        {
            // silence must never verify: an unreadable interrupt table is
            // an unmet condition, not a pass

            rocketnet_warn( "xdp pre-flight: /proc/interrupts is unreadable (R4): cannot verify irq pinning" );
            provisioned = false;
        }
        else
        {
            int matched_irqs = 0;

            uint8_t cpu_seen[1024];
            memset( cpu_seen, 0, sizeof( cpu_seen ) );

            char line[4096];
            while ( fgets( line, sizeof( line ), interrupts ) )
            {
                if ( !rocketnet_server_socket_interrupt_line_matches_interface( line, interface_name ) )
                    continue;

                int irq = 0;
                if ( sscanf( line, " %d:", &irq ) != 1 )
                    continue;

                matched_irqs++;

                char affinity_path[256];
                snprintf( affinity_path, sizeof( affinity_path ), "/proc/irq/%d/smp_affinity_list", irq );

                char affinity[256];
                if ( !rocketnet_server_socket_read_sysfs( affinity_path, affinity, sizeof( affinity ) ) )
                {
                    rocketnet_warn( "xdp pre-flight: irq %d affinity is unreadable (R4): cannot verify its pin", irq );
                    provisioned = false;
                    continue;
                }

                if ( strchr( affinity, '-' ) || strchr( affinity, ',' ) )
                {
                    rocketnet_warn( "xdp pre-flight: irq %d affinity is '%s', want a single isolated cpu (R4): run scripts/rocketnet-provision %s %d", irq, affinity, interface_name, num_queues );
                    provisioned = false;
                    continue;
                }

                errno = 0;

                char * affinity_end = NULL;
                const long parsed_cpu = strtol( affinity, &affinity_end, 10 );

                while ( *affinity_end == ' ' || *affinity_end == '\t' ||
                        *affinity_end == '\r' || *affinity_end == '\n' )
                {
                    affinity_end++;
                }

                if ( affinity_end == affinity || *affinity_end != '\0' ||
                     errno == ERANGE || parsed_cpu < INT_MIN || parsed_cpu > INT_MAX )
                {
                    rocketnet_warn( "xdp pre-flight: irq %d affinity is unreadable (R4): cannot verify its pin", irq );
                    provisioned = false;
                    continue;
                }

                const int cpu = (int) parsed_cpu;

                if ( have_isolated && !rocketnet_server_socket_cpu_in_list( isolated, cpu ) )
                {
                    rocketnet_warn( "xdp pre-flight: irq %d is pinned to cpu %d, which is not in the isolated set %s (R4): run scripts/rocketnet-provision %s %d", irq, cpu, isolated, interface_name, num_queues );
                    provisioned = false;
                }

                if ( cpu < 0 || cpu >= (int) sizeof( cpu_seen ) )
                {
                    // outside the checkable range is unmet, never silently
                    // skipped

                    rocketnet_warn( "xdp pre-flight: irq %d affinity cpu %d is outside the checkable range (R4)", irq, cpu );
                    provisioned = false;
                }
                else
                {
                    if ( cpu_seen[cpu] )
                    {
                        rocketnet_warn( "xdp pre-flight: two of %s's IRQs are pinned to cpu %d (R4): one queue, one core -- run scripts/rocketnet-provision %s %d", interface_name, cpu, interface_name, num_queues );
                        provisioned = false;
                    }

                    cpu_seen[cpu] = 1;
                }
            }

            fclose( interrupts );

            if ( matched_irqs == 0 )
            {
                // HONESTY over a false rationale: some transports' IRQ names
                // never carry the netdev name (mlx5-class), and VMs may
                // have no per-queue vectors at all -- R4 cannot be
                // verified BY NAME here, and saying so is different from
                // claiming there is nothing to pin

                rocketnet_info( "xdp pre-flight: no /proc/interrupts line names %s (R4): either this NIC has no per-queue vectors, or its transport's irq naming does not embed the interface name (mlx5-class) -- irq pinning cannot be verified by name here; pin and verify via the NIC transport's own tooling", interface_name );
            }
            else if ( have_isolated && matched_irqs > rocketnet_server_socket_count_cpu_list( isolated ) )
            {
                // the same refusal the script gives a round robin that
                // would wrap: more queue irqs than isolated cores IS the
                // doubling

                rocketnet_warn( "xdp pre-flight: %d of %s's IRQs but only %d isolated cpus (R4): one queue, one core -- grow the isolated range or shrink the queue count", matched_irqs, interface_name, rocketnet_server_socket_count_cpu_list( isolated ) );
                provisioned = false;
            }
        }
    }

    // R6: smt off (absent knob passes; belt-and-braces under B1's nosmt)

    if ( rocketnet_server_socket_read_sysfs( "/sys/devices/system/cpu/smt/control", value, sizeof( value ) ) )
    {
        if ( strcmp( value, "off" ) != 0 && strcmp( value, "forceoff" ) != 0 && strcmp( value, "notsupported" ) != 0 && strcmp( value, "notimplemented" ) != 0 )
        {
            rocketnet_warn( "xdp pre-flight: smt is '%s', want off (R6): run scripts/rocketnet-provision %s %d", value, interface_name, num_queues );
            provisioned = false;
        }
    }

    // R5: the busy-poll pair (absent knobs pass)

    char path[2048];
    snprintf( path, sizeof( path ), "/sys/class/net/%s/napi_defer_hard_irqs", interface_name );

    if ( rocketnet_server_socket_read_sysfs( path, value, sizeof( value ) ) && strcmp( value, "2" ) != 0 )
    {
        rocketnet_warn( "xdp pre-flight: napi_defer_hard_irqs is %s, want 2 (R5): run scripts/rocketnet-provision %s %d", value, interface_name, num_queues );
        provisioned = false;
    }

    snprintf( path, sizeof( path ), "/sys/class/net/%s/gro_flush_timeout", interface_name );

    if ( rocketnet_server_socket_read_sysfs( path, value, sizeof( value ) ) && strcmp( value, "200000" ) != 0 )
    {
        rocketnet_warn( "xdp pre-flight: gro_flush_timeout is %s, want 200000 (R5): run scripts/rocketnet-provision %s %d", value, interface_name, num_queues );
        provisioned = false;
    }

    // R7: the NIC's combined channel count equals the configured queue
    // count (a NIC without channel support passes)

    {
        int probe_fd = socket( AF_INET, SOCK_DGRAM, 0 );
        if ( probe_fd < 0 )
        {
            // silence must never verify

            rocketnet_warn( "xdp pre-flight: could not open a probe socket for the channel query (%s) (R7)", strerror( errno ) );
            provisioned = false;
        }
        else
        {
            struct ethtool_channels channels;
            memset( &channels, 0, sizeof( channels ) );
            channels.cmd = ETHTOOL_GCHANNELS;

            struct ifreq ifr;
            memset( &ifr, 0, sizeof( ifr ) );

            // ifr is zeroed above, so the copied name stays NUL terminated

            memcpy( ifr.ifr_name, interface_name, strnlen( interface_name, sizeof( ifr.ifr_name ) - 1 ) );
            ifr.ifr_data = (char *) &channels;

            const int ethtool_result = ioctl( probe_fd, SIOCETHTOOL, &ifr );
            if ( ethtool_result != 0 && ( errno == EOPNOTSUPP || errno == ENOTSUP ) )
            {
                // the honest pass, stated like its R4 sibling: no channel
                // surface means the count cannot be wrong, and saying so
                // is different from silence

                rocketnet_info( "xdp pre-flight: %s has no channel surface (R7 passes: the transport does not support channel queries)", interface_name );
            }
            else if ( ethtool_result != 0 )
            {
                rocketnet_warn( "xdp pre-flight: the channel query on %s failed (%s) (R7): cannot verify the queue count", interface_name, strerror( errno ) );
                provisioned = false;
            }
            else if ( channels.combined_count == 0 && (int) channels.rx_count == num_queues && (int) channels.tx_count == num_queues &&
                      rocketnet_server_socket_interface_is_veth( interface_name ) )
            {
                // veth reports separate rx and tx channels and never combined
                // ones, so the queue count is carried in both: the kernel
                // tier's on-link case binds the server on a veth pair

                rocketnet_info( "xdp pre-flight: %s has %u rx and %u tx channels and no combined ones (R7 passes: the queue count is %d)", interface_name, channels.rx_count, channels.tx_count, num_queues );
            }
            else
            {
                if ( (int) channels.combined_count != num_queues )
                {
                    rocketnet_warn( "xdp pre-flight: combined queue count is %u, want %d (R7): run scripts/rocketnet-provision %s %d", channels.combined_count, num_queues, interface_name, num_queues );
                    provisioned = false;
                }
            }

            close( probe_fd );
        }
    }

    // K1, kernel capability, per the ruling's checklist: an AF_XDP socket
    // created and closed without a bind...

    {
        int xdp_fd = socket( AF_XDP, SOCK_RAW, 0 );
        if ( xdp_fd < 0 )
        {
            rocketnet_warn( "xdp pre-flight: kernel refused an AF_XDP socket (%s) (K1). this kernel cannot run the XDP data plane", strerror( errno ) );
            provisioned = false;
        }
        else
        {
            close( xdp_fd );
        }
    }

    // ...and a BPF program loaded and closed without an attach

    {
        struct bpf_insn instructions[2];
        memset( instructions, 0, sizeof( instructions ) );
        instructions[0].code = BPF_ALU64 | BPF_MOV | BPF_K;         // r0 = XDP_PASS
        instructions[0].imm = 2;
        instructions[1].code = BPF_JMP | BPF_EXIT;

        int program_fd = rocketnet_xdp_probe_program( instructions, 2 );
        if ( program_fd < 0 )
        {
            rocketnet_warn( "xdp pre-flight: kernel refused a BPF program load (%s) (K1). this kernel cannot run the XDP data plane", strerror( errno ) );
            provisioned = false;
        }
        else
        {
            close( program_fd );
        }
    }

    return provisioned;
}

bool rocketnet_server_socket_preflight( const char * public_address_string, int num_queues )
{
    // the public pre-flight answers for the transport's default cpu count
    return rocketnet_server_socket_preflight_with_cpus( public_address_string, num_queues, 1 );
}

rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * public_address_string, const uint8_t * packet_filter, int num_queues, int num_os_cpus, const char * second_server_address, int * create_status )
{
    rocketnet_assert( num_queues >= 1 );
    rocketnet_assert( public_address_string );
    rocketnet_assert( packet_filter );

    rocketnet_address_t second_address;
    memset( &second_address, 0, sizeof( second_address ) );

    int serves_both = 0;

    // the committing create refuses outright in a process that forbade it
    // (the test suite's structural barrier) -- before its first act, so no
    // path below this line is reachable there

    if ( rocketnet_server_socket_commit_forbidden_flag.load() )
    {
        rocketnet_warn( "xdp create refused: this process forbids committing an XDP data plane" );
        return NULL;
    }

    // the queue and cpu counts, and since the review the public
    // address too, arrive from the transport config table (with loud
    // override logging and deprecated aliases handled there) -- no env is
    // read here, so the address the pre-flight verified is the address
    // this create binds, always

    rocketnet_info( "server public address is %s", public_address_string );

    rocketnet_address_t public_address;
    if ( !rocketnet_address_parse( &public_address, public_address_string ) )
    {
        rocketnet_error( "server could not parse public address" );
        return NULL;
    }

    // One family, or both when second_server_address names the other family
    // on the same port (ServedFamilies). Two interfaces refuse.

    if ( public_address.type != ROCKETNET_ADDRESS_IPV4 && public_address.type != ROCKETNET_ADDRESS_IPV6 )
    {
        rocketnet_error( "server public address is neither ipv4 nor ipv6" );
        return NULL;
    }

    if ( rocketnet_server_socket_ipv6_link_local( &public_address ) )
    {
        rocketnet_error( "server public address is ipv6 link-local (fe80::/10); a server's public address is global" );
        return NULL;
    }

    if ( second_server_address && second_server_address[0] != '\0' )
    {
        if ( !rocketnet_address_parse( &second_address, second_server_address ) )
        {
            rocketnet_error( "server second_public_address does not parse" );
            if ( create_status )
                *create_status = ROCKETNET_STATUS_INVALID_ADDRESS;
            return NULL;
        }

        if ( second_address.type == public_address.type )
        {
            rocketnet_error( "server second_public_address is the same family as the public address" );
            if ( create_status )
                *create_status = ROCKETNET_STATUS_INVALID_ADDRESS;
            return NULL;
        }

        if ( second_address.port != public_address.port )
        {
            rocketnet_error( "server second_public_address is on a different port from the public address" );
            if ( create_status )
                *create_status = ROCKETNET_STATUS_INVALID_ADDRESS;
            return NULL;
        }

        if ( rocketnet_server_socket_ipv6_link_local( &second_address ) )
        {
            rocketnet_error( "server public address is ipv6 link-local (fe80::/10); a server's public address is global" );
            if ( create_status )
                *create_status = ROCKETNET_STATUS_INVALID_ADDRESS;
            return NULL;
        }

        serves_both = 1;
    }

    // VERIFY, then bind: the non-mutating pre-flight refuses unless
    // the machine is provisioned -- provisioning itself lives in
    // scripts/rocketnet-provision, run deliberately by the operator, and
    // nothing below this point changes machine state. everything from here
    // is the library's own per-socket work: its BPF program built, loaded
    // and attached, AF_XDP binds, the memlock rlimit.

    if ( !rocketnet_server_socket_preflight_with_cpus( public_address_string, num_queues, num_os_cpus ) )
    {
        rocketnet_error( "xdp pre-flight failed: this machine is not provisioned for the XDP data plane (see scripts/rocketnet-provision)" );
        return NULL;
    }

    rocketnet_server_socket_t * server_socket = (rocketnet_server_socket_t *) rocketnet_malloc( context, sizeof( rocketnet_server_socket_t ) );
    if ( !server_socket )
    {
        rocketnet_create_status( create_status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    new ( server_socket ) rocketnet_server_socket_t();

    server_socket->context = context;

    server_socket->replay_marks = (struct rocketnet_xdp_replay_marks_t *) rocketnet_malloc( context, sizeof( struct rocketnet_xdp_replay_marks_t ) );
    if ( !server_socket->replay_marks )
    {
        rocketnet_create_status( create_status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_error( "server could not allocate the replay marks buffer" );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    rocketnet_xdp_replay_marks_reset( server_socket->replay_marks );

    // find the network interface that matches the public address -- the
    // same read-only lookup the pre-flight just passed

    char interface_name[1024];
    unsigned int interface_index = 0;
    if ( !rocketnet_server_socket_find_interface( &public_address, interface_name, sizeof( interface_name ), &interface_index ) )
    {
        rocketnet_error( "server could not find any network interface matching public address" );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    server_socket->interface_index = (int) interface_index;

    if ( serves_both )
    {
        char second_interface_name[1024];
        unsigned int second_interface_index = 0;
        if ( !rocketnet_server_socket_find_interface( &second_address, second_interface_name, sizeof( second_interface_name ), &second_interface_index ) )
        {
            rocketnet_error( "server could not find any network interface matching public address" );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        if ( second_interface_index != interface_index )
        {
            rocketnet_error( "server xdp cannot serve both families: the addresses are on different interfaces" );
            if ( create_status )
                *create_status = ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED;
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        server_socket->serves_both = 1;

        if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
        {
            server_socket->ipv4_address = public_address;
            server_socket->ipv6_address = second_address;
        }
        else
        {
            server_socket->ipv6_address = public_address;
            server_socket->ipv4_address = second_address;
        }
    }

    rocketnet_info( "server network interface is %s", interface_name );

    server_socket->num_queues = num_queues;

    // look up the ethernet address of the network interface

    if ( !get_interface_mac_address( interface_name, server_socket->server_ethernet_address ) )
    {
        rocketnet_error( "server could not get mac address of network interface" );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    rocketnet_info( "server ethernet address is %02x.%02x.%02x.%02x.%02x.%02x",
                    server_socket->server_ethernet_address[0],
                    server_socket->server_ethernet_address[1],
                    server_socket->server_ethernet_address[2],
                    server_socket->server_ethernet_address[3],
                    server_socket->server_ethernet_address[4],
                    server_socket->server_ethernet_address[5] );

    // look up the gateway ethernet address for the network interface.
    // Both families discover one gateway MAC each. The env override still
    // covers both, because get_gateway_mac_address reads it for either family.

    if ( serves_both )
    {
        bool ipv4_known = false;
        bool ipv6_known = false;
        uint8_t ipv4_mac[6];
        uint8_t ipv6_mac[6];
        if ( !get_gateway_mac_address( interface_name, server_socket->interface_index, ROCKETNET_ADDRESS_IPV4, ipv4_mac, &ipv4_known ) ||
             !get_gateway_mac_address( interface_name, server_socket->interface_index, ROCKETNET_ADDRESS_IPV6, ipv6_mac, &ipv6_known ) )
        {
            rocketnet_error( "server could not get gateway mac address" );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        if ( ipv4_known )
        {
            memcpy( server_socket->gateway_ethernet_address, ipv4_mac, 6 );
            server_socket->gateway_known = true;
        }

        rocketnet_xdp_reply_macs_reset( &server_socket->reply_macs, NULL );

        if ( ipv4_known )
        {
            memcpy( server_socket->reply_macs.gateway_ipv4, ipv4_mac, 6 );
            server_socket->reply_macs.gateway_ipv4_known = true;
        }

        if ( ipv6_known )
        {
            memcpy( server_socket->reply_macs.gateway_ipv6, ipv6_mac, 6 );
            server_socket->reply_macs.gateway_ipv6_known = true;
        }
    }
    else if ( !get_gateway_mac_address( interface_name, server_socket->interface_index, public_address.type, server_socket->gateway_ethernet_address, &server_socket->gateway_known ) )
    {
        rocketnet_error( "server could not get gateway mac address" );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    if ( server_socket->serves_both )
    {
        if ( server_socket->reply_macs.gateway_ipv4_known )
            rocketnet_info( "ipv4 gateway ethernet address is %02x.%02x.%02x.%02x.%02x.%02x",
                            server_socket->reply_macs.gateway_ipv4[0],
                            server_socket->reply_macs.gateway_ipv4[1],
                            server_socket->reply_macs.gateway_ipv4[2],
                            server_socket->reply_macs.gateway_ipv4[3],
                            server_socket->reply_macs.gateway_ipv4[4],
                            server_socket->reply_macs.gateway_ipv4[5] );
        else
            rocketnet_warn( "no ipv4 gateway on %s: ipv4 replies go only to clients learned on link", interface_name );
        if ( server_socket->reply_macs.gateway_ipv6_known )
            rocketnet_info( "ipv6 gateway ethernet address is %02x.%02x.%02x.%02x.%02x.%02x",
                            server_socket->reply_macs.gateway_ipv6[0],
                            server_socket->reply_macs.gateway_ipv6[1],
                            server_socket->reply_macs.gateway_ipv6[2],
                            server_socket->reply_macs.gateway_ipv6[3],
                            server_socket->reply_macs.gateway_ipv6[4],
                            server_socket->reply_macs.gateway_ipv6[5] );
        else
            rocketnet_warn( "no ipv6 gateway on %s: ipv6 replies go only to clients learned on link", interface_name );
    }
    else if ( server_socket->gateway_known )
    {
        rocketnet_info( "gateway ethernet address is %02x.%02x.%02x.%02x.%02x.%02x",
                        server_socket->gateway_ethernet_address[0],
                        server_socket->gateway_ethernet_address[1],
                        server_socket->gateway_ethernet_address[2],
                        server_socket->gateway_ethernet_address[3],
                        server_socket->gateway_ethernet_address[4],
                        server_socket->gateway_ethernet_address[5] );
    }
    else
    {
        rocketnet_warn( "server found no gateway on %s: replies go only to clients learned on link, and a reply to an unlearned address is refused", interface_name );
    }

    // replies go to the source MAC learned from each answered client, else
    // to the gateway

    if ( !serves_both )
        rocketnet_xdp_reply_macs_reset( &server_socket->reply_macs, server_socket->gateway_known ? server_socket->gateway_ethernet_address : NULL );

    // STATE IDENTITY (re-derived twice at review): the identity
    // input contains NOTHING process-derived -- a random 64-bit token
    // names the root, because pid+sequence fails deterministically in
    // containers (pid 1 always, the sequence resetting every restart).
    // the root's NAME carries the instance metadata a sweep needs to
    // judge staleness -- pid, process start time, and the host boot id --
    // because bpffs holds no regular files, and a directory name is
    // atomic with the mkdir that claims it:
    //
    //     rocketnet_xdp_<token16>_<pid>_<starttime>_<bootid32>
    //
    // (underscore-delimited: bpffs refuses '.' in names)
    //
    // the root is created by THIS create's own mkdir and never adopted
    // (an existing path -- astronomically unlikely under a random token --
    // refuses loudly rather than letting LIBBPF_PIN_BY_NAME resurrect
    // someone else's state). the four pin paths under it are recorded BY
    // CONSTRUCTION here, at mkdir, since their names are deterministic:
    // every failure path from here on unlinks exactly the recorded paths
    // and removes the root, so no create outcome -- grant, refusal, or
    // mid-create failure -- leaves state behind. only a crash can, and
    // the operator's deliberate sweep judges those by the metadata in the
    // name.

    {
        const uint64_t token = rocketnet_random_uint64();

        char boot_id[64] = "unknown";
        {
            char raw[128];
            if ( rocketnet_server_socket_read_sysfs( "/proc/sys/kernel/random/boot_id", raw, sizeof( raw ) ) )
            {
                int j = 0;
                for ( int i = 0; raw[i] != '\0' && j < (int) sizeof( boot_id ) - 1; i++ )
                    if ( raw[i] != '-' )
                        boot_id[j++] = raw[i];
                boot_id[j] = '\0';
            }
        }

        uint64_t start_time = 0;
        {
            char stat_line[1024];
            if ( rocketnet_server_socket_read_sysfs( "/proc/self/stat", stat_line, sizeof( stat_line ) ) )
            {
                const char * cursor = strrchr( stat_line, ')' );        // comm may contain spaces; fields resume after the last ')'
                if ( cursor )
                {
                    cursor++;
                    for ( int field = 0; field < 19 && cursor; field++ )        // state is field 3 overall; starttime is field 22 = 19 fields later
                        cursor = strchr( cursor + 1, ' ' );
                    if ( cursor )
                        start_time = strtoull( cursor + 1, NULL, 10 );
                }
            }
        }

        snprintf( server_socket->pin_root, sizeof( server_socket->pin_root ), "/sys/fs/bpf/rocketnet_xdp_%016" PRIx64 "_%d_%" PRIu64 "_%s",
                  token, (int) getpid(), start_time, boot_id );
    }

    if ( mkdir( server_socket->pin_root, 0700 ) != 0 )
    {
        // each errno is its own operator errand, named as one

        if ( errno == EEXIST )
            rocketnet_error( "pin root %s already exists: stale state is never adopted -- sweep deliberately with scripts/rocketnet-provision sweep, or remove it by hand", server_socket->pin_root );
        else if ( errno == ENOENT )
            rocketnet_error( "could not create pin root %s: bpffs is not mounted (mount -t bpf bpf /sys/fs/bpf)", server_socket->pin_root );
        else
            rocketnet_error( "could not create pin root %s (%s)", server_socket->pin_root, strerror( errno ) );

        server_socket->pin_root[0] = '\0';
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    server_socket->pin_root_created = true;

    // the pins recorded by construction, at the moment the root exists

    {
        static const char * pinned_map_names[4] = { "server_xdp_config_map", "server_xdp_socket_map", "server_xdp_stats_map", "server_xdp_replay_map" };

        for ( int i = 0; i < 4; i++ )
        {
            snprintf( server_socket->pinned_paths[i], sizeof( server_socket->pinned_paths[0] ), "%s/%s", server_socket->pin_root, pinned_map_names[i] );
        }

        server_socket->num_pinned = 4;
    }

    // load the server_xdp program and attach it to the network interface.
    // server_xdp.o is compiled at build time and embedded as bytes
    // (cmake/EmbedXdp.cmake): it opens from memory, with no compiler, no
    // temporary directory and no child process on the server host.

    rocketnet_info( "loading server_xdp..." );

    {
        // the open opts carry this instance's pin root, so LIBBPF_PIN_BY_NAME
        // pins land under it -- never under a name another instance shares

        struct bpf_object_open_opts open_opts;
        memset( &open_opts, 0, sizeof( open_opts ) );
        open_opts.sz = sizeof( open_opts );
        open_opts.object_name = "server_xdp";
        open_opts.pin_root_path = server_socket->pin_root;

        struct bpf_object * object = bpf_object__open_mem( rocketnet_server_xdp_o, rocketnet_server_xdp_o_len, &open_opts );
        if ( !object || libbpf_get_error( object ) )
        {
            rocketnet_error( "could not load server_xdp program" );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        server_socket->program_object = object;

        rocketnet_xdp_program_t * program = rocketnet_xdp_program_open( object );
        if ( !program )
        {
            rocketnet_error( "could not load server_xdp program" );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        server_socket->program = program;
    }

    rocketnet_info( "server_xdp loaded successfully." );

    rocketnet_info( "attaching server_xdp to network interface %s", interface_name );

    int ret = rocketnet_xdp_program_attach( server_socket->program, server_socket->interface_index, false );
    if ( ret == 0 )
    {
        server_socket->attached_native = true;
    }
    else
    {
        rocketnet_info( "falling back to skb mode..." );
        ret = rocketnet_xdp_program_attach( server_socket->program, server_socket->interface_index, true );
        if ( ret == 0 )
        {
            server_socket->attached_skb = true;
        }
        else
        {
            rocketnet_error( "failed to attach server_xdp program to interface %s", interface_name );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }
    }

    // allow unlimited locking of memory, so all memory needed for packet buffers can be locked

    struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };

    if ( setrlimit( RLIMIT_MEMLOCK, &rlim ) )
    {
        rocketnet_error( "server could not setrlimit" );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    // get file descriptors for maps so we can communicate with the server_xdp
    // program running in kernel space -- under this instance's own pin root

    // the SAME recorded paths destroy will unlink: one construction, one
    // bound, no re-derivation to drift

    server_socket->config_map_fd = bpf_obj_get( server_socket->pinned_paths[0] );
    if ( server_socket->config_map_fd < 0 )
    {
        rocketnet_error( "server could not get config map: %s", strerror( errno ) );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    server_socket->socket_map_fd = bpf_obj_get( server_socket->pinned_paths[1] );
    if ( server_socket->socket_map_fd < 0 )
    {
        rocketnet_error( "server could not get socket map: %s", strerror( errno ) );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    server_socket->stats_map_fd = bpf_obj_get( server_socket->pinned_paths[2] );
    if ( server_socket->stats_map_fd < 0 )
    {
        rocketnet_error( "server could not get stats map: %s", strerror( errno ) );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    server_socket->replay_map_fd = bpf_obj_get( server_socket->pinned_paths[3] );
    if ( server_socket->replay_map_fd < 0 )
    {
        rocketnet_error( "server could not get replay map: %s", strerror( errno ) );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    // configure the kernel prefilter: the destination it accepts and the
    // per-title filter value and epoch. until this write the program passes
    // untouched, so a server that fails before this point never black-holed
    // the box's other traffic.

    {
        struct rocketnet_server_xdp_config_t config;
        memset( &config, 0, sizeof( config ) );

        // the server's own family gets its public address. the other
        // family's slot gets that family's loopback address (127.0.0.1, ::1)
        // rather than zero, because zero accepts any destination: a NIC never
        // delivers a loopback destination (the stack drops it as martian), so
        // the program redirects only this server's family and passes the
        // other family's traffic to the kernel untouched

        if ( serves_both )
        {
            // Both destinations. A loopback placeholder would pass the other family.
            memcpy( &config.public_address, server_socket->ipv4_address.data.ipv4, 4 );
            rocketnet_server_socket_ipv6_bytes( &server_socket->ipv6_address, config.public_address_ipv6 );
        }
        else if ( public_address.type == ROCKETNET_ADDRESS_IPV4 )
        {
            memcpy( &config.public_address, public_address.data.ipv4, 4 );     // network order
            config.public_address_ipv6[15] = 1;
        }
        else
        {
            config.public_address = htonl( INADDR_LOOPBACK );
            rocketnet_server_socket_ipv6_bytes( &public_address, config.public_address_ipv6 );
        }

        config.public_port = rocketnet_platform_htons( public_address.port );
        memcpy( config.packet_filter, packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
        config.filter_epoch = ROCKETNET_FILTER_EPOCH_UNKEYED;

        __u32 key = 0;
        if ( bpf_map_update_elem( server_socket->config_map_fd, &key, &config, BPF_ANY ) != 0 )
        {
            rocketnet_error( "server could not write config map: %s", strerror( errno ) );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }
    }

    // initialize server xdp sockets (one socket per-NIC queue)

    server_socket->socket = (rocketnet_server_xdp_socket_t *) rocketnet_malloc( server_socket->context, (size_t) num_queues * sizeof( rocketnet_server_xdp_socket_t ) );
    if ( server_socket->socket == NULL )
    {
        rocketnet_create_status( create_status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_error( "server could not allocate sockets" );
        rocketnet_server_socket_abort_create( server_socket );
        return NULL;
    }

    // Construct every queue before any resource acquisition can fail.

    for ( int queue = 0; queue < num_queues; queue++ )
        new ( &server_socket->socket[queue] ) rocketnet_server_xdp_socket_t();

    for ( int queue = 0; queue < num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

        socket->queue = queue;
        socket->num_os_cpus = num_os_cpus;

#if defined( ROCKETNET_DEV ) && ROCKETNET_DEV
        // development-only failure seam: ROCKETNET_TEST_XDP_FAIL_AT_QUEUE=<n>
        // fails create at the top of queue n, after the earlier queues are
        // fully built (their mutexes created, their xdp threads running) and
        // before queue n creates anything, so destroy meets a queue whose
        // mutexes were never created (test_xdp_filter.cpp:1603,
        // xdp_create_fails_part_way_tears_down)
        {
            char fail_at_queue[ROCKETNET_PLATFORM_GETENV_MAX_BYTES];
            if ( rocketnet_platform_getenv( "ROCKETNET_TEST_XDP_FAIL_AT_QUEUE", fail_at_queue, sizeof( fail_at_queue ) ) > 0 && atoi( fail_at_queue ) == queue )
            {
                rocketnet_error( "server xdp create failed at queue %d (injected by ROCKETNET_TEST_XDP_FAIL_AT_QUEUE)", queue );
                rocketnet_server_socket_abort_create( server_socket );
                return NULL;
            }
        }
#endif // #if defined( ROCKETNET_DEV ) && ROCKETNET_DEV

        // allocate umem

        // send frames [0, NUM_FRAMES/2), then the fill frames up to ROCKETNET_XDP_UMEM_FRAMES

        const int buffer_size = ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE;

        if ( posix_memalign( &socket->buffer, (size_t) getpagesize(), (size_t) buffer_size ) )
        {
            socket->buffer = NULL;              // destroy removes the count only for a granted block
            rocketnet_create_status( create_status, ROCKETNET_STATUS_OUT_OF_MEMORY );
            rocketnet_error( "server could not allocate buffer" );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        rocketnet_direct_allocation_add( (uint64_t) buffer_size );

        // create umem and xdp socket: zero copy first, copy if the transport refuses the zero-copy bind.
        // A refused bind leaves the umem's rings set, so the fallback starts from a fresh umem.

        int result = 0;

        for ( int attempt = 0; attempt < 2; attempt++ )
        {
            const bool zero_copy = attempt == 0;

            struct xsk_umem_config umem_config;
            rocketnet_xdp_umem_config( &umem_config );

            result = xsk_umem__create( &socket->umem, socket->buffer, buffer_size, &socket->fill_queue, &socket->complete_queue, &umem_config );
            if ( result )
            {
                rocketnet_error( "server could not create umem: %s (errno %d)", strerror( -result ), -result );
                rocketnet_server_socket_abort_create( server_socket );
                return NULL;
            }

            struct xsk_socket_config xsk_config;
            rocketnet_xdp_socket_config( &xsk_config, ROCKETNET_XDP_RECV_QUEUE_SIZE, ROCKETNET_XDP_SEND_QUEUE_SIZE, zero_copy );

            result = xsk_socket__create( &socket->xsk, interface_name, (__u32) queue, socket->umem, &socket->receive_queue, &socket->send_queue, &xsk_config );
            if ( result == 0 )
                break;

            socket->xsk = NULL;

            if ( !zero_copy )
            {
                rocketnet_error( "server could not create xsk socket for queue %d: %s (errno %d)", queue, strerror( -result ), -result );
                rocketnet_server_socket_abort_create( server_socket );
                return NULL;
            }

            rocketnet_info( "server xdp queue %d refused the zero copy bind (%s), binding in copy mode", queue, strerror( -result ) );

            xsk_umem__delete( socket->umem );
            socket->umem = NULL;
        }

        // the kernel's fill, completion, receive and send rings, mapped by
        // libxdp at the umem and socket creates: their descriptor bytes join
        // the direct allocation count until destroy deletes them

        socket->ring_bytes = (uint64_t) ( ROCKETNET_XDP_FILL_RING_SIZE + XSK_RING_CONS__DEFAULT_NUM_DESCS ) * sizeof( uint64_t ) +
                             (uint64_t) ( ROCKETNET_XDP_RECV_QUEUE_SIZE + ROCKETNET_XDP_SEND_QUEUE_SIZE ) * sizeof( struct xdp_desc );
        rocketnet_direct_allocation_add( socket->ring_bytes );

        // record the mode the kernel bound

        struct xdp_options xdp_options;
        memset( &xdp_options, 0, sizeof( xdp_options ) );

        socklen_t xdp_options_length = sizeof( xdp_options );
        if ( getsockopt( xsk_socket__fd( socket->xsk ), SOL_XDP, XDP_OPTIONS, &xdp_options, &xdp_options_length ) != 0 )
        {
            rocketnet_error( "server could not read the xdp options for queue %d", queue );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        socket->zero_copy = ( xdp_options.flags & XDP_OPTIONS_ZEROCOPY ) != 0;

        // configure the xdp socket to receive packets from the xdp program

        __u32 key = (__u32) queue;
        __u32 value = (__u32) xsk_socket__fd( socket->xsk );

        if ( bpf_map_update_elem( server_socket->socket_map_fd, &key, &value, BPF_ANY ) < 0 )
        {
            rocketnet_error( "server failed to add xdp socket for queue %d to map", queue );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        // copy across data needed by the socket to send packets

        memcpy( socket->server_ethernet_address, server_socket->server_ethernet_address, ETH_ALEN );
        socket->server_address = public_address;
        socket->serves_both = server_socket->serves_both;
        if ( server_socket->serves_both )
        {
            socket->ipv4_address = server_socket->ipv4_address;
            socket->ipv6_address = server_socket->ipv6_address;
        }

        // initialize send frame allocator

        rocketnet_assert( ROCKETNET_XDP_SEND_QUEUE_SIZE <= ROCKETNET_XDP_NUM_FRAMES / 4 );
        rocketnet_assert( ROCKETNET_XDP_RECV_QUEUE_SIZE <= ROCKETNET_XDP_NUM_FRAMES / 4 );

        for ( int j = 0; j < ROCKETNET_XDP_NUM_FRAMES / 2; j++ )
        {
            socket->send_frames[j] = (uint64_t) ( j * ROCKETNET_XDP_FRAME_SIZE );
        }

        socket->num_free_send_frames = ROCKETNET_XDP_NUM_FRAMES / 2;

        // make sure we start off with the correct send buffer index

        socket->send_flow_index.store( socket->send_handoff.producer, std::memory_order_relaxed );

        // populate fill ring for packets to be received in, once: every frame
        // of the receive region goes on it, and a received frame goes back to
        // it, never to a free list. The ring has room for twice these frames
        // (ROCKETNET_XDP_FILL_RING_SIZE), so the worker always has a slot to
        // hand a frame back in

        {
            uint32_t index;
            result = (int) xsk_ring_prod__reserve( &socket->fill_queue, ROCKETNET_XDP_FILL_FRAMES, &index );
            if ( result != ROCKETNET_XDP_FILL_FRAMES )
            {
                rocketnet_error( "server failed to populate fill queue: %d", result );
                rocketnet_server_socket_abort_create( server_socket );
                return NULL;
            }

            for ( int i = 0; i < ROCKETNET_XDP_FILL_FRAMES; i++ )
            {
                *xsk_ring_prod__fill_addr( &socket->fill_queue, index + (uint32_t) i ) =
                    (uint64_t) ( ROCKETNET_XDP_NUM_FRAMES / 2 + i ) * ROCKETNET_XDP_FRAME_SIZE;
            }

            xsk_ring_prod__submit( &socket->fill_queue, ROCKETNET_XDP_FILL_FRAMES );
        }

        // create send mutex

        if ( !rocketnet_platform_mutex_create( &socket->send_mutex ) )
        {
            rocketnet_error( "failed to create send mutex for queue %d", queue );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        // create receive mutex

        if ( !rocketnet_platform_mutex_create( &socket->receive_mutex ) )
        {
            rocketnet_error( "failed to create receive mutex for queue %d", queue );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }

        // start xdp thread

        rocketnet_info( "starting xdp thread for socket queue %d", socket->queue );

        socket->xdp_thread = rocketnet_platform_thread_create( server_socket->context, rocketnet_server_socket_xdp_thread_function, socket, create_status );
        if ( !socket->xdp_thread )
        {
            rocketnet_error( "server could not create xdp thread %d", queue );
            rocketnet_server_socket_abort_create( server_socket );
            return NULL;
        }
    }

    // the bound mode, once per server: zero copy or copy (each queue's mode is on its socket)

    int zero_copy_queues = 0;

    for ( int queue = 0; queue < num_queues; queue++ )
    {
        if ( server_socket->socket[queue].zero_copy )
            zero_copy_queues++;
    }

    if ( zero_copy_queues == num_queues )
        rocketnet_info( "server xdp sockets bound in zero copy mode on %s", interface_name );
    else if ( zero_copy_queues == 0 )
        rocketnet_info( "server xdp sockets bound in copy mode on %s", interface_name );
    else
        rocketnet_info( "server xdp sockets bound in zero copy mode on %d of %d queues and copy mode on the rest on %s", zero_copy_queues, num_queues, interface_name );

    // the server has started successfully

    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_info( "server started on %s [xdp]", rocketnet_address_to_string( &public_address, address_string ) );

    server_socket->public_address = public_address;
    server_socket->state = ROCKETNET_SERVER_SOCKET_RUNNING;

    return server_socket;
}

void rocketnet_server_socket_destroy( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    const bool stopped = rocketnet_server_socket_stopped( server_socket );
    rocketnet_assert( stopped );
    if ( !stopped )
        return;

    if ( server_socket->program != NULL )
    {
        if ( server_socket->attached_native )
        {
            rocketnet_xdp_program_detach( server_socket->program, server_socket->interface_index, false );
        }

        if ( server_socket->attached_skb )
        {
            rocketnet_xdp_program_detach( server_socket->program, server_socket->interface_index, true );
        }

        rocketnet_xdp_program_close( server_socket->program );
    }

    if ( server_socket->program_object != NULL )
        bpf_object__close( server_socket->program_object );

    if ( server_socket->config_map_fd >= 0 )
        close( server_socket->config_map_fd );
    if ( server_socket->socket_map_fd >= 0 )
        close( server_socket->socket_map_fd );
    if ( server_socket->stats_map_fd >= 0 )
        close( server_socket->stats_map_fd );
    if ( server_socket->replay_map_fd >= 0 )
        close( server_socket->replay_map_fd );

    rocketnet_free( server_socket->context, server_socket->replay_marks );

    // this instance's bpffs state goes with it: the paths recorded by
    // construction at mkdir -- so EVERY create outcome that reaches
    // destroy, mid-create failures included, unlinks them all (a pin that
    // never came to exist unlinks as a harmless ENOENT) -- and only the
    // directory this instance's own mkdir made. a directory that will not
    // empty is refused loudly, naming the operator's sweep.

    for ( int i = 0; i < server_socket->num_pinned; i++ )
        unlink( server_socket->pinned_paths[i] );

    if ( server_socket->pin_root_created && rmdir( server_socket->pin_root ) != 0 )
        rocketnet_warn( "could not remove pin root %s (%s): sweep deliberately with scripts/rocketnet-provision sweep", server_socket->pin_root, strerror( errno ) );

    // the queue array is allocated late in create, so a create that failed
    // before it exists reaches here with num_queues already set and the
    // array still null -- walk it only when it is real

    for ( int i = 0; server_socket->socket != NULL && i < server_socket->num_queues; i++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[i];

        // stop xdp thread

        if ( socket->xdp_thread )
        {
            rocketnet_platform_thread_join( socket->xdp_thread );
            rocketnet_platform_thread_destroy( socket->xdp_thread );
        }

        // destroy mutexes: a create that failed part way reaches here with
        // later queues' mutexes never created, so only a created one is
        // destroyed (the IOCP transport's guard, rocketnet_server_iocp_windows.cpp:436)

        if ( socket->send_mutex.ok )
            rocketnet_platform_mutex_destroy( &socket->send_mutex );
        if ( socket->receive_mutex.ok )
            rocketnet_platform_mutex_destroy( &socket->receive_mutex );

        // destroy xdp socket

        if ( socket->xsk )
        {
            xsk_socket__delete( socket->xsk );
        }

        if ( socket->umem )
        {
            xsk_umem__delete( socket->umem );
        }

        if ( socket->ring_bytes )
        {
            rocketnet_direct_allocation_remove( socket->ring_bytes );
            socket->ring_bytes = 0;
        }

        if ( socket->buffer )
        {
            free( socket->buffer );
            rocketnet_direct_allocation_remove( (uint64_t) ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE );
        }

        socket->~rocketnet_server_xdp_socket_t();
    }

    rocketnet_free( server_socket->context, server_socket->socket );

    void * allocation_context = server_socket->context;
    server_socket->~rocketnet_server_socket_t();
    rocketnet_free( allocation_context, server_socket );
}

void rocketnet_server_socket_stop( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    server_socket->state = ROCKETNET_SERVER_SOCKET_STOPPING;
    for ( int i = 0; server_socket->socket && i < server_socket->num_queues; ++i )
        server_socket->socket[i].quit.store( true, std::memory_order_release );
}

bool rocketnet_server_socket_stopped( rocketnet_server_socket_t * server_socket )
{
    if ( !server_socket || server_socket->state == ROCKETNET_SERVER_SOCKET_RUNNING )
        return false;
    for ( int i = 0; server_socket->socket && i < server_socket->num_queues; ++i )
        if ( server_socket->socket[i].xdp_thread &&
             !server_socket->socket[i].stopped.load( std::memory_order_acquire ) )
            return false;
    server_socket->state = ROCKETNET_SERVER_SOCKET_STOPPED;

    return true;
}

// the staging slot a finish or abort names, from the byte offset of its
// packet pointer past the staging buffer's first slot: a negative, unaligned
// or past-the-end offset is refused (-1) BEFORE it is divided, since C
// division truncates toward zero and would map -4095..-1 to slot 0. a
// pointer from before the last flip lands here too, and fails closed as a
// lost packet instead of an out-of-bounds write

int rocketnet_server_socket_send_slot( ptrdiff_t offset )
{
    if ( offset < 0 || offset % ROCKETNET_XDP_FRAME_SIZE != 0 )
        return -1;
    const ptrdiff_t slot = offset / ROCKETNET_XDP_FRAME_SIZE;
    if ( slot >= ROCKETNET_XDP_SEND_QUEUE_SIZE )
        return -1;
    return (int) slot;
}

// the TX queue for a destination: a hash of its address bytes and port, so
// every packet and fragment to one client goes through one queue and leaves
// in the order it was staged. the packet id carries the queue (id modulo the
// queue count), so finish and abort find it again

int rocketnet_server_socket_send_queue( const rocketnet_address_t * to, int num_queues )
{
    rocketnet_assert( to );
    rocketnet_assert( num_queues >= 1 );

    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t * address = (const uint8_t *) &to->data;
    const int address_bytes = to->type == ROCKETNET_ADDRESS_IPV6 ? 16 : 4;
    for ( int i = 0; i < address_bytes; i++ )
    {
        hash ^= address[i];
        hash *= 0x100000001B3ULL;
    }

    hash ^= (uint8_t) ( to->port & 0xFF );
    hash *= 0x100000001B3ULL;
    hash ^= (uint8_t) ( to->port >> 8 );
    hash *= 0x100000001B3ULL;
    return (int) ( hash % (uint64_t) num_queues );
}

// proofs/tla/RNDriverSendStaging.tla XdpStart: stages a packet in the producer buffer
// being filled, or refuses it (NULL) when no producer buffer is filling

uint8_t * rocketnet_server_socket_start_packet_internal( struct rocketnet_server_socket_t * server_socket, uint64_t /* packet_id */, int queue, const rocketnet_address_t * to, const uint8_t * destination_mac )
{
    rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

    const int flow_index = socket->send_flow_index.load( std::memory_order_acquire );
    if ( flow_index < 0 )
        return NULL;
    rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[flow_index];

    // the lower bound matters too: an app that stages forever without ever
    // flushing wraps this counter negative after ~2^31 packets, and a
    // negative index here is an out-of-bounds write, not a dropped packet.
    // the claim counts a refusal instead of logging on this hot path

    const int packet_index = rocketnet_xdp_send_claim( send_buffer->num_packets, ROCKETNET_XDP_SEND_QUEUE_SIZE, socket->send_refused_overflow );
    if ( packet_index < 0 )
        return NULL;

    send_buffer->packet_info[packet_index].to = *to;
    send_buffer->packet_info[packet_index].packet_bytes = 0;
    memcpy( send_buffer->packet_info[packet_index].destination_mac, destination_mac, ETH_ALEN );

    const uintptr_t learned_begin = (uintptr_t) &server_socket->reply_macs.entries[0];
    const uintptr_t learned_end = (uintptr_t) &server_socket->reply_macs.entries[ROCKETNET_XDP_REPLY_MAC_ENTRIES];
    send_buffer->packet_info[packet_index].mac_learned = (uintptr_t) destination_mac >= learned_begin && (uintptr_t) destination_mac < learned_end;

    return send_buffer->packet_data[packet_index];
}

// proofs/tla/ServerStopSpread.tla Room: the fewest free slots of any queue

int rocketnet_server_socket_send_room( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    int room = ROCKETNET_XDP_SEND_QUEUE_SIZE;
    for ( int queue = 0; queue < server_socket->num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];
        const int flow_index = socket->send_flow_index.load( std::memory_order_acquire );
        if ( flow_index < 0 )
            return 0;
        const int used = socket->send_buffer[flow_index].num_packets.load( std::memory_order_relaxed );
        const int slots = used < 0 || used >= ROCKETNET_XDP_SEND_QUEUE_SIZE ? 0 : ROCKETNET_XDP_SEND_QUEUE_SIZE - used;
        if ( slots < room )
            room = slots;
    }

    return room;
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: every queue's buffers back
// with its producer, the producer empty

bool rocketnet_server_socket_send_idle( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    for ( int queue = 0; queue < server_socket->num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];
        const int flow_index = socket->send_flow_index.load( std::memory_order_acquire );
        if ( flow_index >= 0 && socket->send_buffer[flow_index].num_packets.load( std::memory_order_relaxed ) != 0 )
            return false;
        if ( !rocketnet_platform_mutex_try_acquire( &socket->send_mutex ) )
            return false;
        const bool idle = socket->send_handoff.ready_count == 0 && socket->send_handoff.free_count == ROCKETNET_XDP_NUM_BUFFERS - 1;
        rocketnet_platform_mutex_release( &socket->send_mutex );
        if ( !idle )
            return false;
    }

    return true;
}

uint8_t * rocketnet_server_socket_start_packet( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    // the reply-MAC rule (rocketnet_xdp_frame.h), before a send slot is
    // taken: a response the transport reserves while processing a receive
    // view passes that view's own from[] entry, which names the frame it
    // answers, used only with no gateway. no MAC (no gateway, nothing
    // learned, no frame answered) is a refusal the transport counts
    // (ROCKETNET_TRANSPORT_PACKET_UNREACHABLE)

    const uint8_t * answered_mac = NULL;
    const uintptr_t view_begin = (uintptr_t) &server_socket->process_packets.from[0];
    const uintptr_t view_end = (uintptr_t) &server_socket->process_packets.from[server_socket->process_packets.num_packets];
    const uintptr_t to_pointer = (uintptr_t) to;
    if ( to_pointer >= view_begin && to_pointer < view_end )
        answered_mac = server_socket->process_eth[( to_pointer - view_begin ) / sizeof( rocketnet_address_t )];

    uint64_t refused = 0;
    const uint8_t * destination_mac = rocketnet_xdp_reply_mac( &server_socket->reply_macs, to, answered_mac, &refused );
    if ( !destination_mac )
    {
        *packet_id = ROCKETNET_TRANSPORT_PACKET_UNREACHABLE;
        return NULL;
    }

    // one queue per destination; the id is a multiple of the queue count
    // plus the queue, so finish and abort recover the queue from the id

    const int queue = rocketnet_server_socket_send_queue( to, server_socket->num_queues );

    *packet_id = server_socket->packet_id.fetch_add( 1 ) * (uint64_t) server_socket->num_queues + (uint64_t) queue;

    return rocketnet_server_socket_start_packet_internal( server_socket, *packet_id, queue, to, destination_mac );
}

// proofs/tla/RNDriverSendStaging.tla XdpStart: finish writes the byte count of the packet slot start_packet staged in the producer buffer
// proofs/tla/RNDriverSendStaging.tla Publish: a pointer outside the producer buffer (one from before the last flip) is refused instead of rewriting a published packet

void rocketnet_server_socket_finish_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    const int queue = (int) ( packet_id % (uint64_t) server_socket->num_queues );

    rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

    const int flow_index = socket->send_flow_index.load( std::memory_order_relaxed );
    if ( flow_index < 0 )
        return;
    rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[flow_index];

    const ptrdiff_t offset = ( packet_data - send_buffer->packet_data[0] );

    const int packet_index = rocketnet_server_socket_send_slot( offset );

    // derived from pointer arithmetic against the CURRENT staging buffer: a
    // pointer from before the last flip lands wildly out of range here, and
    // failing closed turns a contract violation into a lost packet instead
    // of an out-of-bounds write

    if ( packet_index < 0 || packet_index >= ROCKETNET_XDP_SEND_QUEUE_SIZE )
        return;

    // a packet started before its client's MAC was learned took the gateway
    // (or the answered frame's MAC). The reply to a client's first
    // authenticated packet is that case: the transport reserves it before
    // the packet opens, and the open learns the MAC before this commit.
    // Taking the learned MAC here sends it the way the same update's
    // payloads go; through the gateway it arrived after them (the router's
    // hairpin, ~200 us on the Linux x64 XDP bench), and the client was handed payloads before
    // the keep-alive that connects it. The table is written only by update,
    // never during a fill.

    rocketnet_server_socket_send_packet_info_t * info = &send_buffer->packet_info[packet_index];
    if ( !info->mac_learned )
    {
        const struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( &server_socket->reply_macs, &info->to );
        if ( entry )
        {
            memcpy( info->destination_mac, entry->mac, ETH_ALEN );
            info->mac_learned = true;
        }
    }

    // the packet is complete wire bytes as the protocol wrote them; nothing
    // is prepended here -- the xdp thread adds the ethernet, ip and udp
    // headers when it copies the packet into its umem frame

    info->packet_bytes = (size_t) packet_bytes;
}

void rocketnet_server_socket_abort_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( server_socket );

    const int queue = (int) ( packet_id % (uint64_t) server_socket->num_queues );

    rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

    const int flow_index = socket->send_flow_index.load( std::memory_order_relaxed );
    if ( flow_index < 0 )
        return;
    rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[flow_index];

    const ptrdiff_t offset = ( packet_data - send_buffer->packet_data[0] );

    const int packet_index = rocketnet_server_socket_send_slot( offset );

    if ( packet_index < 0 || packet_index >= ROCKETNET_XDP_SEND_QUEUE_SIZE )
        return;

    send_buffer->packet_info[packet_index].packet_bytes = 0;
}

// proofs/tla/RNXdpReplay.tla Commit, HoldGreetingUntilMarked: before a producer
// buffer is published, drop each keep-alive to a session whose first mark
// (a zero) the kernel refused, or that the buffer could not queue: the first
// keep-alive is how a client learns it is connected, so until the mark lands
// the client sends no session packet and the old session's entry drops
// nothing of the new session. Only keep-alives: the client refuses payloads
// before it is connected, and a disconnect still leaves. The keep-alive is
// sent again when next due. Main thread, like the commit; nothing is held in
// steady state, and the check is then one load per publish.

static void rocketnet_server_socket_hold_greetings( struct rocketnet_server_socket_t * server_socket, rocketnet_server_socket_send_buffer_t * send_buffer )
{
    const bool hold_all = server_socket->replay_hold_all_greetings;
    if ( !hold_all && !( server_socket->replay_marks && rocketnet_xdp_replay_marks_holding_greetings( server_socket->replay_marks ) ) )
        return;
    const int num_packets = send_buffer->num_packets.load( std::memory_order_relaxed );
    for ( int i = 0; i < num_packets && i < ROCKETNET_XDP_SEND_QUEUE_SIZE; i++ )
    {
        rocketnet_server_socket_send_packet_info_t * info = &send_buffer->packet_info[i];

        // the wire bytes start at the frame's wire offset, the type first

        if ( info->packet_bytes == 0 || send_buffer->packet_data[i][ROCKETNET_FRAME_WIRE_OFFSET] != ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET )
            continue;
        if ( hold_all || rocketnet_xdp_replay_marks_holds_greeting( server_socket->replay_marks, &info->to ) )
        {
            info->packet_bytes = 0;
            server_socket->replay_greetings_held++;
        }
    }
}

void rocketnet_server_socket_send_packets( struct rocketnet_server_socket_t * server_socket )
{
    for ( int queue = 0; queue < server_socket->num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];
        const int index = socket->send_flow_index.load( std::memory_order_relaxed );
        if ( index < 0 || socket->send_buffer[index].num_packets.load( std::memory_order_relaxed ) <= 0 )
            continue;
        rocketnet_server_socket_hold_greetings( server_socket, &socket->send_buffer[index] );
        if ( !rocketnet_platform_mutex_try_acquire( &socket->send_mutex ) )
            continue;
        // proofs/tla/RNDriverSendStaging.tla Publish (PublishWith, retaining the
        // producer): with no free replacement, keep
        // the producer and its complete packets staged. The next call retries
        // after worker recycle (WorkerRetire).

        if ( socket->send_handoff.publish() )
        {
            const int replacement = socket->send_handoff.producer;
            rocketnet_platform_mutex_release( &socket->send_mutex );

            // This newly acquired buffer has no other owner. Reset work
            // stays outside the handoff lock, before publication to fills.

            socket->send_buffer[replacement].num_packets.store( 0, std::memory_order_relaxed );
            socket->send_buffer[replacement].packet_start_index = 0;
            socket->send_flow_index.store( replacement, std::memory_order_release );
            continue;
        }

        rocketnet_platform_mutex_release( &socket->send_mutex );
    }
}


#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif // #ifndef ENOTSUPP

// The replay high-water writer. Main thread only, like the rest of the server
// update path. Forget and mark append to the socket's pending buffer; commit
// applies them to the pinned map and resets the buffer keeping every forget
// the kernel refused, failures counted and named by call, never by key.

bool rocketnet_server_socket_replay_forget( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( address );
    rocketnet_assert( server_socket->replay_marks );

    const bool queued = rocketnet_xdp_replay_marks_forget( server_socket->replay_marks, address );
    rocketnet_xdp_reply_macs_forget( &server_socket->reply_macs, address );

    return queued;
}

void rocketnet_server_socket_client_accepted( struct rocketnet_server_socket_t * server_socket, int view_index )
{
    rocketnet_assert( server_socket );
    if ( view_index < 0 || view_index >= server_socket->process_packets.num_packets )
        return;
    rocketnet_xdp_reply_macs_learn( &server_socket->reply_macs, &server_socket->process_packets.from[view_index], server_socket->process_eth[view_index] );
}

bool rocketnet_server_socket_replay_mark( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * address, uint32_t high_water )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( address );
    rocketnet_assert( server_socket->replay_marks );

    const bool queued = rocketnet_xdp_replay_marks_mark( server_socket->replay_marks, address, high_water );

    // a new session's zero mark the buffer refused: its key is not held
    // anywhere, so the next publish holds every keep-alive (proofs/tla/RNXdpReplay.tla
    // HoldGreetingUntilMarked, markOwed); the walk owes the mark and asks again

    if ( !queued && high_water == 0 )
        server_socket->replay_zero_mark_refused = true;
    return queued;
}

static void rocketnet_server_socket_replay_commit_failed( struct rocketnet_server_socket_t * server_socket, const char * call )
{
    server_socket->replay_commit_failures++;
    rocketnet_debug( "replay commit: %s failed (%s)", call, strerror( errno ) );
}

void rocketnet_server_socket_replay_commit( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( server_socket->replay_marks );

    struct rocketnet_xdp_replay_marks_t * marks = server_socket->replay_marks;

    // proofs/tla/RNXdpReplay.tla, Commit: every forget is applied, then every mark,
    // in one commit; this is the only writer of the kernel map. A forget the
    // kernel refuses stays pending for the next commit (ForgetOwedUntilApplied)
    // until it lands or a newer mark of its key lands (RetryNeverDeletesNewerMark);
    // a mark the kernel refuses stays pending too (MarkOwedUntilApplied), so a
    // reconnect's refused zero mark is retried rather than lost.

    bool forget_failed[ROCKETNET_XDP_REPLAY_MAX_ENTRIES] = {};

    if ( marks->forget_count > 0 )
    {
        struct bpf_map_batch_opts opts;
        memset( &opts, 0, sizeof( opts ) );
        opts.sz = sizeof( opts );
        __u32 count = marks->forget_count;
        const bool batch_deleted = bpf_map_delete_batch( server_socket->replay_map_fd, marks->forgets, &count, &opts ) == 0;

        for ( uint32_t i = 0; i < marks->forget_count; i++ )
        {
            // a missing key is not a failure; a refused batch (an older kernel,
            // a missing key, or any other error) is walked one syscall at a
            // time so each forget's outcome is known

            forget_failed[i] = false;
            if ( !batch_deleted && bpf_map_delete_elem( server_socket->replay_map_fd, &marks->forgets[i] ) != 0 && errno != ENOENT )
            {
                forget_failed[i] = true;
                rocketnet_server_socket_replay_commit_failed( server_socket, "bpf_map_delete_elem" );
            }
        }
    }

    bool mark_applied[ROCKETNET_XDP_REPLAY_MAX_ENTRIES] = {};

    if ( marks->mark_count > 0 )
    {
        struct bpf_map_batch_opts opts;
        memset( &opts, 0, sizeof( opts ) );
        opts.sz = sizeof( opts );
        opts.elem_flags = BPF_ANY;
        __u32 count = marks->mark_count;
        const bool batch_updated = bpf_map_update_batch( server_socket->replay_map_fd, marks->mark_keys, marks->mark_values, &count, &opts ) == 0;

        for ( uint32_t i = 0; i < marks->mark_count; i++ )
        {
            // a refused batch (an older kernel, or any other error) is walked
            // one syscall at a time, so each mark's outcome is known: an
            // applied mark supersedes a refused forget of the same key
            // (RetryNeverDeletesNewerMark); BPF_ANY makes a repeat harmless

            mark_applied[i] = true;
            if ( !batch_updated && bpf_map_update_elem( server_socket->replay_map_fd, &marks->mark_keys[i], &marks->mark_values[i], BPF_ANY ) != 0 )
            {
                mark_applied[i] = false;
                rocketnet_server_socket_replay_commit_failed( server_socket, "bpf_map_update_elem" );
            }
        }
    }

    // a full buffer's refusals land in the commit failure count; a refused
    // forget is kept unless an applied mark of its key superseded it, and a
    // refused mark is kept for the next commit

    server_socket->replay_commit_failures += rocketnet_xdp_replay_marks_reset_keeping( marks, forget_failed, mark_applied );

    // the publish after this commit holds the greeting of every session whose
    // zero mark is kept or was not queued (rocketnet_server_socket_hold_greetings)

    server_socket->replay_hold_all_greetings = server_socket->replay_zero_mark_refused;
    server_socket->replay_zero_mark_refused = false;
}

uint64_t rocketnet_server_socket_replay_commit_failures( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return server_socket->replay_commit_failures;
}

#if ROCKETNET_DEV
// Exercise the native staging entry points without creating kernel resources.
bool rocketnet_server_socket_test_buffer_exchange()
{
    const uint64_t planned_bytes = sizeof( rocketnet_server_socket_t ) +
                                   sizeof( rocketnet_xdp_replay_marks_t ) + sizeof( rocketnet_server_xdp_socket_t ) +
                                   (uint64_t) ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE;
    bool valid = rocketnet_server_socket_memory_required( 1 ) == planned_bytes;
    rocketnet_xdp_buffer_exchange_t<3> exchange;
    valid &= exchange.producer == 0;
    valid &= exchange.publish() && exchange.producer == 1;
    valid &= exchange.publish() && exchange.producer == 2;
    valid &= !exchange.publish() && exchange.producer == 2;
    valid &= exchange.claim() == 0;
    valid &= exchange.claim() == 1;
    exchange.recycle( 0 );
    valid &= exchange.publish() && exchange.producer == 0;
    valid &= exchange.claim() == 2;

    void * server_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_socket_t ) );
    void * socket_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_xdp_socket_t ) );
    if ( !server_storage || !socket_storage )
    {
        rocketnet_free( NULL, socket_storage );
        rocketnet_free( NULL, server_storage );
        return false;
    }

    auto * server = new ( server_storage ) rocketnet_server_socket_t();
    auto * socket = new ( socket_storage ) rocketnet_server_xdp_socket_t();
    server->num_queues = 1;
    server->socket = socket;

    const bool send_mutex_ok = rocketnet_platform_mutex_create( &socket->send_mutex );
    const bool receive_mutex_ok = rocketnet_platform_mutex_create( &socket->receive_mutex );
    if ( !send_mutex_ok || !receive_mutex_ok )
    {
        if ( send_mutex_ok )
            rocketnet_platform_mutex_destroy( &socket->send_mutex );
        if ( receive_mutex_ok )
            rocketnet_platform_mutex_destroy( &socket->receive_mutex );
        socket->~rocketnet_server_xdp_socket_t();
        server->~rocketnet_server_socket_t();
        rocketnet_free( NULL, socket_storage );
        rocketnet_free( NULL, server_storage );
        return false;
    }

    socket->send_flow_index.store( socket->send_handoff.producer );

    static const uint8_t gateway_mac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 2 };
    rocketnet_xdp_reply_macs_reset( &server->reply_macs, gateway_mac );

    rocketnet_address_t to = {};
    to.type = ROCKETNET_ADDRESS_IPV4;

    uint64_t packet_id = 0;
    uint8_t * packet = rocketnet_server_socket_start_packet( server, &to, &packet_id );
    valid &= packet == socket->send_buffer[0].packet_data[0];

    if ( packet )
    {
        rocketnet_server_socket_finish_packet( server, packet_id, packet, 64 );
        rocketnet_server_socket_send_packets( server );
        valid &= socket->send_flow_index.load() == 1;
        rocketnet_platform_mutex_acquire( &socket->send_mutex );

        const int worker_index = socket->send_handoff.claim();
        rocketnet_platform_mutex_release( &socket->send_mutex );
        valid &= worker_index == 0;
        valid &= socket->send_buffer[0].packet_info[0].packet_bytes == 64;
    }

    socket->receive_buffer[0].num_packets = 1;
    socket->receive_buffer[0].packet_bytes[0] = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
    socket->receive_buffer[0].from[0] = to;
    rocketnet_platform_mutex_acquire( &socket->receive_mutex );
    valid &= socket->receive_handoff.publish();
    rocketnet_platform_mutex_release( &socket->receive_mutex );
    rocketnet_server_socket_receive_packets( server );
    valid &= socket->receive_held_index == 0;
    valid &= server->process_packets.num_packets == 1;
    valid &= server->process_packets.packet_data[0] == socket->receive_buffer[0].packet_data[0];

    // A failed game-thread try-lock may clear the expired view, but it must
    // retain ownership and retry recycling on the following call.

    rocketnet_platform_mutex_acquire( &socket->receive_mutex );
    rocketnet_server_socket_receive_packets( server );
    valid &= socket->receive_held_index == 0;
    valid &= socket->receive_buffer[0].num_packets == 0;
    rocketnet_platform_mutex_release( &socket->receive_mutex );
    rocketnet_server_socket_receive_packets( server );
    valid &= socket->receive_held_index == -1;
    valid &= socket->receive_buffer[0].num_packets == 0;

    if ( socket->send_mutex.ok )
        rocketnet_platform_mutex_destroy( &socket->send_mutex );
    if ( socket->receive_mutex.ok )
        rocketnet_platform_mutex_destroy( &socket->receive_mutex );
    socket->~rocketnet_server_xdp_socket_t();
    server->~rocketnet_server_socket_t();
    rocketnet_free( NULL, socket_storage );
    rocketnet_free( NULL, server_storage );

    return valid;
}

// A queue whose send flow index is -1 (constructed, before create publishes
// the producer) has no staging buffer. finish and abort through any pointer
// must return without forming send_buffer[-1]: tools/check_xdp_guard_control.py
// removes both guards and expects this check to trip the sanitizer.
bool rocketnet_server_socket_test_inactive_flow()
{
    void * server_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_socket_t ) );
    void * socket_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_xdp_socket_t ) );
    if ( !server_storage || !socket_storage )
    {
        rocketnet_free( NULL, socket_storage );
        rocketnet_free( NULL, server_storage );
        return false;
    }

    auto * server = new ( server_storage ) rocketnet_server_socket_t();
    auto * socket = new ( socket_storage ) rocketnet_server_xdp_socket_t();
    server->num_queues = 1;
    server->socket = socket;

    static const uint8_t gateway_mac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 2 };
    rocketnet_xdp_reply_macs_reset( &server->reply_macs, gateway_mac );

    rocketnet_address_t to = {};
    to.type = ROCKETNET_ADDRESS_IPV4;

    bool valid = socket->send_flow_index.load() == -1;
    for ( int i = 0; i < ROCKETNET_XDP_NUM_BUFFERS; i++ )
        socket->send_buffer[i].packet_info[0].packet_bytes = size_t( 40 + i );

    uint64_t packet_id = 0;
    valid &= rocketnet_server_socket_start_packet( server, &to, &packet_id ) == NULL;

    for ( int i = 0; i < ROCKETNET_XDP_NUM_BUFFERS; i++ )
    {
        uint8_t * packet = socket->send_buffer[i].packet_data[0];
        rocketnet_server_socket_finish_packet( server, packet_id, packet, 64 );
        rocketnet_server_socket_abort_packet( server, packet_id, packet );
    }

    rocketnet_server_socket_send_packets( server );

    valid &= socket->send_flow_index.load() == -1;

    for ( int i = 0; i < ROCKETNET_XDP_NUM_BUFFERS; i++ )
    {
        valid &= socket->send_buffer[i].packet_info[0].packet_bytes == size_t( 40 + i );
        valid &= socket->send_buffer[i].num_packets.load() == 0;
    }

    socket->~rocketnet_server_xdp_socket_t();
    server->~rocketnet_server_socket_t();
    rocketnet_free( NULL, socket_storage );
    rocketnet_free( NULL, server_storage );

    return valid;
}

// The reply that answers a client's first authenticated packet (the
// keep-alive that connects it) is reserved before that packet opens, so no
// MAC is learned yet and the rule picks the gateway; the open then learns
// the client's MAC, and finish takes it. Without that, on a LAN with a
// gateway the keep-alive took the router's hairpin and arrived ~200 us after
// the payloads sent to the learned MAC in the same update (Linux x64 XDP bench, 10-22).
bool rocketnet_server_socket_test_reply_mac_refreshed_at_finish()
{
    void * server_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_socket_t ) );
    void * socket_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_xdp_socket_t ) );
    if ( !server_storage || !socket_storage )
    {
        rocketnet_free( NULL, socket_storage );
        rocketnet_free( NULL, server_storage );
        return false;
    }

    auto * server = new ( server_storage ) rocketnet_server_socket_t();
    auto * socket = new ( socket_storage ) rocketnet_server_xdp_socket_t();
    server->num_queues = 1;
    server->socket = socket;
    socket->send_flow_index.store( socket->send_handoff.producer );

    static const uint8_t gateway_mac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 2 };
    static const uint8_t client_mac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x4e };
    rocketnet_xdp_reply_macs_reset( &server->reply_macs, gateway_mac );

    rocketnet_address_t to = {};
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 192;
    to.data.ipv4[1] = 168;
    to.data.ipv4[2] = 1;
    to.data.ipv4[3] = 78;
    to.port = 36800;

    bool valid = true;
    rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[socket->send_flow_index.load()];

    // reserved before the client's MAC is learned: the gateway, for now

    uint64_t reply_id = 0;
    uint8_t * reply = rocketnet_server_socket_start_packet( server, &to, &reply_id );
    valid &= reply != NULL;
    valid &= reply && memcmp( send_buffer->packet_info[0].destination_mac, gateway_mac, ETH_ALEN ) == 0;

    // the packet it answers opens and teaches the client's MAC

    rocketnet_xdp_reply_macs_learn( &server->reply_macs, &to, client_mac );

    // a payload staged after: the learned MAC

    uint64_t payload_id = 0;
    uint8_t * payload = rocketnet_server_socket_start_packet( server, &to, &payload_id );
    valid &= payload != NULL;

    if ( reply && payload )
    {
        rocketnet_server_socket_finish_packet( server, reply_id, reply, 41 );
        rocketnet_server_socket_finish_packet( server, payload_id, payload, 68 );
        valid &= memcmp( send_buffer->packet_info[0].destination_mac, client_mac, ETH_ALEN ) == 0;
        valid &= memcmp( send_buffer->packet_info[1].destination_mac, client_mac, ETH_ALEN ) == 0;
    }

    // a reply to an address never learned keeps the gateway

    rocketnet_address_t stranger = to;
    stranger.port = 36801;

    uint64_t stranger_id = 0;
    uint8_t * stranger_reply = rocketnet_server_socket_start_packet( server, &stranger, &stranger_id );
    valid &= stranger_reply != NULL;

    if ( stranger_reply )
    {
        rocketnet_server_socket_finish_packet( server, stranger_id, stranger_reply, 38 );
        valid &= memcmp( send_buffer->packet_info[2].destination_mac, gateway_mac, ETH_ALEN ) == 0;
    }

    socket->~rocketnet_server_xdp_socket_t();
    server->~rocketnet_server_socket_t();
    rocketnet_free( NULL, socket_storage );
    rocketnet_free( NULL, server_storage );

    return valid;
}

// Exercise the replay writer's pending buffer and its commit failure path
// without a map or a kernel resource: replay_map_fd -1 refuses every syscall.
bool rocketnet_server_socket_test_replay_marks()
{
    void * server_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_socket_t ) );
    if ( !server_storage )
        return false;
    auto * server = new ( server_storage ) rocketnet_server_socket_t();
    server->replay_marks = (struct rocketnet_xdp_replay_marks_t *) rocketnet_malloc( NULL, sizeof( struct rocketnet_xdp_replay_marks_t ) );
    if ( !server->replay_marks )
    {
        server->~rocketnet_server_socket_t();
        rocketnet_free( NULL, server_storage );
        return false;
    }

    rocketnet_xdp_replay_marks_reset( server->replay_marks );
    server->replay_map_fd = -1;

    rocketnet_address_t a = {};
    a.type = ROCKETNET_ADDRESS_IPV4;
    a.data.ipv4[0] = 198;
    a.data.ipv4[1] = 51;
    a.data.ipv4[2] = 100;
    a.data.ipv4[3] = 7;
    a.port = 50000;

    rocketnet_address_t b = a;
    b.port = 50001;

    rocketnet_server_socket_replay_forget( server, &a );
    rocketnet_server_socket_replay_mark( server, &a, 41 );
    rocketnet_server_socket_replay_mark( server, &b, 7 );

    bool valid = server->replay_marks->forget_count == 1;
    valid &= server->replay_marks->mark_count == 2;

    const uint64_t failures_before = rocketnet_server_socket_replay_commit_failures( server );

    rocketnet_server_socket_replay_commit( server );

    // the refused forget and both refused marks stay pending for the next commit

    valid &= server->replay_marks->forget_count == 1;
    valid &= server->replay_marks->mark_count == 2;
    valid &= rocketnet_server_socket_replay_commit_failures( server ) > failures_before;

    // fill the forgets past the buffer: the refusal reaches the commit
    // failure count beside one failure per refused forget and per kept mark
    // retried and refused again (b's forget was not queued, so its mark stays)

    for ( uint32_t i = 1; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        rocketnet_address_t c = a;
        c.port = (uint16_t) ( 2000 + i );
        rocketnet_server_socket_replay_forget( server, &c );
    }

    rocketnet_server_socket_replay_forget( server, &b );
    valid &= server->replay_marks->forget_count == ROCKETNET_XDP_REPLAY_MAX_ENTRIES;
    valid &= server->replay_marks->refused == 1;

    const uint64_t failures_full = rocketnet_server_socket_replay_commit_failures( server );
    rocketnet_server_socket_replay_commit( server );
    valid &= rocketnet_server_socket_replay_commit_failures( server ) == failures_full + ROCKETNET_XDP_REPLAY_MAX_ENTRIES + 2 + 1;
    valid &= server->replay_marks->forget_count == ROCKETNET_XDP_REPLAY_MAX_ENTRIES;
    valid &= server->replay_marks->mark_count == 2;
    valid &= server->replay_marks->refused == 0;

    // proofs/tla/RNXdpReplay.tla HoldGreetingUntilMarked against a staged producer
    // buffer: a keep-alive to a session whose zero mark the kernel refused is
    // held at publish; its other packets, and a keep-alive to another
    // session, leave

    void * socket_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_xdp_socket_t ) );
    if ( socket_storage )
    {
        auto * socket = new ( socket_storage ) rocketnet_server_xdp_socket_t();
        server->num_queues = 1;
        server->socket = socket;
        socket->send_flow_index.store( socket->send_handoff.producer );

        static const uint8_t gateway_mac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 2 };
        rocketnet_xdp_reply_macs_reset( &server->reply_macs, gateway_mac );

        rocketnet_server_socket_send_buffer_t * send_buffer = &socket->send_buffer[socket->send_flow_index.load()];

        rocketnet_xdp_replay_marks_reset( server->replay_marks );
        valid &= rocketnet_server_socket_replay_mark( server, &a, 0 );
        rocketnet_server_socket_replay_commit( server );
        valid &= rocketnet_xdp_replay_marks_holds_greeting( server->replay_marks, &a );
        valid &= !rocketnet_xdp_replay_marks_holds_greeting( server->replay_marks, &b );

        const uint8_t types[3] = { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET };
        const rocketnet_address_t * destinations[3] = { &a, &a, &b };

        for ( int i = 0; i < 3; i++ )
        {
            uint64_t id = 0;
            uint8_t * frame = rocketnet_server_socket_start_packet( server, destinations[i], &id );
            valid &= frame != NULL;
            if ( !frame )
                continue;
            frame[ROCKETNET_FRAME_WIRE_OFFSET] = types[i];
            rocketnet_server_socket_finish_packet( server, id, frame, 20 );
        }

        const uint64_t held_before = server->replay_greetings_held;
        rocketnet_server_socket_hold_greetings( server, send_buffer );
        valid &= send_buffer->packet_info[0].packet_bytes == 0;
        valid &= send_buffer->packet_info[1].packet_bytes == 20;
        valid &= send_buffer->packet_info[2].packet_bytes == 20;
        valid &= server->replay_greetings_held == held_before + 1;

        // a zero mark the full buffer refused names no key: the next publish
        // holds every keep-alive, until a commit with no such refusal

        rocketnet_xdp_replay_marks_reset( server->replay_marks );

        for ( uint32_t i = 0; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
        {
            rocketnet_address_t c = a;
            c.port = (uint16_t) ( 3000 + i );
            rocketnet_xdp_replay_marks_mark( server->replay_marks, &c, 7 );
        }

        valid &= !rocketnet_server_socket_replay_mark( server, &a, 0 );
        rocketnet_server_socket_replay_commit( server );
        valid &= server->replay_hold_all_greetings;
        rocketnet_server_socket_hold_greetings( server, send_buffer );
        valid &= send_buffer->packet_info[2].packet_bytes == 0;
        valid &= send_buffer->packet_info[1].packet_bytes == 20;
        rocketnet_xdp_replay_marks_reset( server->replay_marks );
        rocketnet_server_socket_replay_commit( server );
        valid &= !server->replay_hold_all_greetings;

        server->num_queues = 0;
        server->socket = NULL;
        socket->~rocketnet_server_xdp_socket_t();
        rocketnet_free( NULL, socket_storage );
    }
    else
    {
        valid = false;
    }

    rocketnet_server_socket_destroy( server );

    return valid;
}
#endif // #if ROCKETNET_DEV

void rocketnet_server_socket_process_staged_packet( rocketnet_server_socket_t * server_socket, uint8_t * eth, rocketnet_address_t * from, uint8_t * packet_data, int packet_bytes )
{
    if ( packet_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
        return;

    // Bound by the ARRAY, not by the queue depth. These are different numbers:
    // ROCKETNET_XDP_RECV_QUEUE_SIZE is 4096*4 = 16384 (how deep one queue's staging
    // buffer is), while process_packets.from/packet_bytes/packet_data are sized
    // ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE = ROCKETNET_RECEIVE_PACKETS_PER_CLIENT *
    // ROCKETNET_MAX_CLIENTS = 16 * 1000 = 16000. Guarding on the larger one let 384
    // entries write past the end of all three arrays — and since packet_data is
    // the last member of rocketnet_server_socket_t, the tail of that overrun put
    // attacker-timed POINTERS past the allocation, which the caller then
    // dereferences. num_packets accumulates across every queue and both
    // off-buffers before it is reset in receive_packets, so one flooded queue
    // reaches it. The portable path was always bounded correctly.

    if ( server_socket->process_packets.num_packets >= ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE )
        return;

    const int index = server_socket->process_packets.num_packets++;

    // the packet is complete wire bytes as they left the client; the
    // protocol's own reading order takes it from here

    server_socket->process_packets.from[index] = *from;
    server_socket->process_packets.packet_data[index] = packet_data;
    server_socket->process_packets.packet_bytes[index] = packet_bytes;
    memcpy( server_socket->process_eth[index], eth, ETH_ALEN );
}

static void pin_thread_to_cpu( int cpu )
{
    // CPU_SET writes bit cpu of a CPU_SETSIZE-bit set: a cpu outside it is
    // refused here in every build, before CPU_SET

    if ( !rocketnet_xdp_cpu_in_set( cpu, CPU_SETSIZE ) )
    {
        rocketnet_error( "xdp thread not pinned: cpu %d is outside 0 .. %d", cpu, CPU_SETSIZE - 1 );
        return;
    }

    // an offline cpu refuses like an out-of-set one: the queue stays
    // unpinned rather than aborting inside its thread

    const int num_cpus = (int) sysconf( _SC_NPROCESSORS_ONLN );
    if ( cpu >= num_cpus )
    {
        rocketnet_error( "xdp thread not pinned: cpu %d is not online (0 .. %d)", cpu, num_cpus - 1 );
        return;
    }

    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    CPU_SET( (size_t) cpu, &cpuset );

    pthread_t current_thread = pthread_self();

    const int result = pthread_setaffinity_np( current_thread, sizeof( cpu_set_t ), &cpuset );
    if ( result != 0 )
    {
        char call[64];
        snprintf( call, sizeof( call ), "pthread_setaffinity_np cpu %d", cpu );
        rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_AFFINITY, "thread affinity", call, strerror( result ), "the thread runs on any cpu" );
    }
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );
    return &server_socket->process_packets;
}

// The transport's send_failures operation sums frames each queue's flush drops
// because the copy rejects their length or the codec refuses to build them,
// and the packet starts it refused because the queue was full.
// The worker publishes these counts atomically so a request/query never waits
// behind its send mutex while it copies a batch into UMEM.

uint64_t rocketnet_server_socket_send_failures( struct rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    uint64_t failures = 0;

    for ( int queue = 0; queue < server_socket->num_queues; queue++ )
    {
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];
        failures += socket->send_refused_length.load( std::memory_order_relaxed ) +
                    socket->send_refused_build.load( std::memory_order_relaxed ) +
                    socket->send_refused_overflow.load( std::memory_order_relaxed );
    }

    return failures;
}

#if ROCKETNET_DEV
bool rocketnet_server_socket_test_send_failures_without_mutex()
{
    // A query must be valid even while the worker owns the send mutex. These
    // synthetic queues deliberately have no mutexes to acquire.
    void * server_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_socket_t ) );
    void * queue_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_xdp_socket_t ) );
    if ( !server_storage || !queue_storage )
    {
        rocketnet_free( NULL, queue_storage );
        rocketnet_free( NULL, server_storage );
        return false;
    }

    auto * server = new ( server_storage ) rocketnet_server_socket_t();
    auto * queue = new ( queue_storage ) rocketnet_server_xdp_socket_t();
    server->num_queues = 1;
    server->socket = queue;
    queue->send_refused_length.store( 5 );
    queue->send_refused_build.store( 7 );
    queue->send_refused_overflow.store( 3 );

    const bool valid = rocketnet_server_socket_send_failures( server ) == 15;
    queue->~rocketnet_server_xdp_socket_t();
    server->~rocketnet_server_socket_t();
    rocketnet_free( NULL, queue_storage );
    rocketnet_free( NULL, server_storage );

    return valid;
}
#endif // #if ROCKETNET_DEV

void rocketnet_server_socket_xdp_counters( struct rocketnet_server_socket_t * server_socket, uint64_t * counters )
{
    rocketnet_assert( server_socket );
    rocketnet_assert( counters );

    memset( counters, 0, ROCKETNET_SERVER_XDP_NUM_COUNTERS * sizeof( uint64_t ) );

    // the stats map is per-CPU: one value struct per possible CPU, summed here

    const int num_cpus = libbpf_num_possible_cpus();
    if ( !rocketnet_xdp_stats_cpus_fit( num_cpus ) )
    {
        if ( num_cpus > 0 )
            rocketnet_error( "xdp stats not read: %d possible cpus is past %d", num_cpus, ROCKETNET_XDP_STATS_MAX_CPUS );
        return;
    }

    struct rocketnet_server_xdp_stats_t * values = server_socket->stats_values;

    __u32 key = 0;
    if ( bpf_map_lookup_elem( server_socket->stats_map_fd, &key, values ) != 0 )
        return;

    for ( int cpu = 0; cpu < num_cpus; cpu++ )
    {
        for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        {
            counters[i] += values[cpu].counters[i];
        }
    }
}

// proofs/tla/RNXdpReceiveRing.tla Drain: the application takes the buffer the XDP
// worker staged, claimed under the receive mutex

void rocketnet_server_socket_receive_packets( rocketnet_server_socket_t * server_socket )
{
    rocketnet_assert( server_socket );

    server_socket->process_packets.num_packets = 0;

    const int num_queues = server_socket->num_queues;
    int filled_queue = -1;

    // a rotating start: queue 0 is not served first on every call

    for ( int step = 0; step < num_queues; step++ )
    {
        const int queue = rocketnet_xdp_drain_queue( server_socket->receive_start_queue, step, num_queues );
        rocketnet_server_xdp_socket_t * socket = &server_socket->socket[queue];

        // Last call's view has expired. A partly consumed buffer remains
        // owned here with its cursor; a finished one may be reset outside
        // the lock and returned before the next FIFO buffer is claimed.

        if ( socket->receive_held_index >= 0 &&
             socket->receive_read_index >= socket->receive_buffer[socket->receive_held_index].num_packets )
        {
            socket->receive_buffer[socket->receive_held_index].num_packets = 0;
            if ( !rocketnet_platform_mutex_try_acquire( &socket->receive_mutex ) )
                continue;
            socket->receive_handoff.recycle( socket->receive_held_index );
            socket->receive_held_index = socket->receive_handoff.claim();
            socket->receive_read_index = 0;
            rocketnet_platform_mutex_release( &socket->receive_mutex );
        }
        else if ( socket->receive_held_index < 0 )
        {
            if ( !rocketnet_platform_mutex_try_acquire( &socket->receive_mutex ) )
                continue;
            socket->receive_held_index = socket->receive_handoff.claim();
            socket->receive_read_index = 0;
            rocketnet_platform_mutex_release( &socket->receive_mutex );
        }

        if ( socket->receive_held_index < 0 )
            continue;
        // the budget already ended on an earlier queue: still recycle this
        // one, and take no packet from it

        if ( filled_queue >= 0 )
            continue;
        rocketnet_server_socket_receive_buffer_t * receive_buffer = &socket->receive_buffer[socket->receive_held_index];
        for ( uint32_t i = socket->receive_read_index; i < receive_buffer->num_packets; i++ )
        {
            if ( server_socket->process_packets.num_packets >= ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE )
            {
                filled_queue = queue;
                break;
            }

            uint8_t * eth = receive_buffer->eth[i];
            rocketnet_address_t from = receive_buffer->from[i];
            uint8_t * packet_data = receive_buffer->packet_data[i];
            const int packet_bytes = (int) receive_buffer->packet_bytes[i];

            rocketnet_assert( packet_bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
            rocketnet_server_socket_process_staged_packet( server_socket, eth, &from, packet_data, packet_bytes );
            socket->receive_read_index = i + 1;
        }
    }

    if ( num_queues > 0 )
    {
        // the budget ended on filled_queue: the next call starts after it.
        // a call that did not fill the budget still advances by one.
        const int origin = filled_queue >= 0 ? filled_queue : server_socket->receive_start_queue;
        server_socket->receive_start_queue = rocketnet_xdp_drain_queue( origin, 1, num_queues );
    }
}

static void rocketnet_server_socket_xdp_thread_function( rocketnet_platform_thread_t *, void * data )
{
    rocketnet_server_xdp_socket_t * socket = (rocketnet_server_xdp_socket_t *) data;

    rocketnet_assert( socket );

    pin_thread_to_cpu( socket->num_os_cpus + socket->queue );

    // flow_server_socket_linux.cpp:1374-1381: pin, then SCHED_RR at the
    // minimum on itself. the priority call follows the off switch; the pin
    // is the queue's cpu and always runs

    if ( rocketnet_platform_thread_priority_enabled() )
    {
        struct sched_param param;
        param.sched_priority = sched_get_priority_min( SCHED_RR );

        const int result = pthread_setschedparam( pthread_self(), SCHED_RR, &param );
        if ( result != 0 )
        {
            char call[64];
            snprintf( call, sizeof( call ), "pthread_setschedparam SCHED_RR %d", param.sched_priority );
            rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_SCHED, "thread priority", call, strerror( result ), "the thread runs at its default priority" );
        }
    }

    rocketnet_info( "pinning xdp thread %d to CPU %d", socket->queue, socket->num_os_cpus + socket->queue );

    struct pollfd fds[1];
    fds[0].fd = xsk_socket__fd( socket->xsk );
    fds[0].events = POLLIN;

    int num_batch_packets = 0;
    int send_worker_index = -1;
    uint64_t batch_frames[ROCKETNET_XDP_SEND_BATCH_SIZE];
    int batch_packet_bytes[ROCKETNET_XDP_SEND_BATCH_SIZE];

    rocketnet_pending_frames_t<ROCKETNET_XDP_RECV_QUEUE_SIZE> pending_return;

    auto return_pending_frames = [&]()
    {
        return pending_return.flush(
            [&]( uint32_t count, uint32_t * index )
            { return xsk_ring_prod__reserve( &socket->fill_queue, count, index ); },
            [&]( uint32_t index, uint64_t frame )
            { *xsk_ring_prod__fill_addr( &socket->fill_queue, index ) = frame; },
            [&]( uint32_t count )
            { xsk_ring_prod__submit( &socket->fill_queue, count ); } );
    };

    bool reported_frame_pressure = false;

    for ( ;; )
    {
        const bool stopping = socket->quit.load( std::memory_order_acquire );

        // Complete one final nonwaiting I/O pass after stop was requested.
        // busy poll the transport

        poll( fds, 1, 0 );

        // return pending frames to the fill queue

        // libxdp reserves the entire requested count or zero. A failed
        // reservation retains the entire batch for a later iteration. The
        // fill ring has a slot for every frame beyond the kernel's unpublished
        // batch (ROCKETNET_XDP_FILL_RING_SIZE), so a flush never waits on the
        // kernel ending its batch (proofs/tla/RNXdpReceiveRing.tla Refill: the
        // pending return flushed at the top of the pass).

        return_pending_frames();

        // see if there are any packets to receive (proofs/tla/RNXdpReceiveRing.tla
        // Peek: xsk_ring_cons__peek, at most the staging room and pending storage)

        uint32_t receive_index;

        rocketnet_server_socket_receive_buffer_t * receive_buffer =
            &socket->receive_buffer[socket->receive_handoff.producer];
        const uint32_t receive_capacity = ROCKETNET_XDP_RECV_QUEUE_SIZE - receive_buffer->num_packets;
        const uint32_t receive_limit = (uint32_t) pending_return.available() < receive_capacity ? (uint32_t) pending_return.available() : receive_capacity;
        uint32_t num_packets = receive_limit ? xsk_ring_cons__peek( &socket->receive_queue, receive_limit, &receive_index ) : 0;

        if ( num_packets > 0 )
        {
            // receive packets

            // How many descriptors this pass actually took. The loop below can
            // stop early when the staging buffer fills, and everything after
            // this point -- the release, the refill, the pending-return spill --
            // must cover exactly that many, not the number peeked. Releasing
            // more would hand the kernel uninitialized stack as umem addresses:
            // rejected (leaking the frame forever) or in range and aliasing a
            // LIVE frame, which is the packet corruption the shouty comment at
            // rocketnet_constants.h:22 warns about.
            //
            // The peek above advanced the ring's cached consumer past all
            // num_packets descriptors; rocketnet_xdp_receive_ring_finish
            // cancels the num_packets - num_consumed descriptors not taken
            // (xsk_ring_cons__cancel) before releasing num_consumed
            // (xsk_ring_cons__release), so the next peek returns the skipped
            // descriptors first. The frame ownership is modelled in
            // proofs/tla/RNXdpReceiveRing.tla; the loop below is its Take, one
            // descriptor read and its frame held while the staging buffer has room.

            uint32_t num_consumed = 0;

            // The frames this pass hands back to the fill ring: one per
            // descriptor read whose frame lies in the receive region, so it
            // can be fewer than num_consumed (rocketnet_xdp_fill_return).

            uint32_t num_returned = 0;

            uint64_t frame[ROCKETNET_XDP_RECV_QUEUE_SIZE];

            const uint64_t receive_region_begin = (uint64_t) ROCKETNET_XDP_NUM_FRAMES / 2 * ROCKETNET_XDP_FRAME_SIZE;
            const uint64_t receive_region_end = (uint64_t) ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE;

            for ( uint32_t i = 0; i < num_packets; i++ )
            {
                if ( receive_buffer->num_packets >= ROCKETNET_XDP_RECV_QUEUE_SIZE )
                    break;

                const struct xdp_desc * __restrict__ desc = xsk_ring_cons__rx_desc( &socket->receive_queue, receive_index + i );

                num_consumed++;

                // only a receive-region frame goes back to the fill queue; a
                // descriptor naming any other frame is dropped, never refilled
                // (proofs/tla/RNXdpReceiveRing.tla FillHoldsOnlyReceiveFrames)

                if ( rocketnet_xdp_fill_return( desc->addr, receive_region_begin, receive_region_end, &frame[num_returned] ) )
                    num_returned++;

                // the descriptor is checked against the receive region and
                // its own frame before a byte of it is read; a refused one is
                // counted once, in receive_drops.descriptor (a refused one in
                // the receive region still went back to the fill queue above,
                // like any dropped frame)

                const uint8_t * __restrict__ packet_data = rocketnet_xdp_receive_descriptor( socket->buffer,
                                                                                             receive_region_begin, receive_region_end,
                                                                                             desc->addr, desc->len, &socket->receive_drops );
                if ( !packet_data )
                    continue;

                // the codec dispatches on the ethertype: IPv4 at its
                // offsets, IPv6 at the 40-byte fixed header, the from-address
                // in the family it read. a frame it does not take (another
                // ethertype, an IPv6 extension header, the family this server
                // does not serve, a frame too short for any server packet) is
                // counted by reason in receive_drops, never parsed at the
                // wrong offsets

                rocketnet_address_t from;
                const uint8_t * payload_data = NULL;
                int payload_bytes = 0;

                const int address_type = socket->serves_both ? -1 : socket->server_address.type;
                if ( rocketnet_xdp_frame_receive( packet_data, (size_t) desc->len, address_type, &from, &payload_data, &payload_bytes, &socket->receive_drops ) )
                {
                    const int index = (int) receive_buffer->num_packets;

                    receive_buffer->from[index] = from;

                    memcpy( receive_buffer->eth[index], packet_data + ETH_ALEN, ETH_ALEN );      // the ethernet source

                    memcpy( receive_buffer->packet_data[index], payload_data, (size_t) payload_bytes );

                    receive_buffer->packet_bytes[index] = (size_t) payload_bytes;

                    receive_buffer->num_packets++;
                }
            }

            rocketnet_xdp_receive_ring_finish( num_packets, num_consumed, []( void * r, uint32_t n )
                                               { xsk_ring_cons__cancel( (struct xsk_ring_cons *) r, n ); }, []( void * r, uint32_t n )
                                               { xsk_ring_cons__release( (struct xsk_ring_cons *) r, n ); }, &socket->receive_queue );

            // The peek was bounded by available pending storage, so every
            // returned frame remains owned until a full fill reservation. The
            // fill credit is the returned count, not num_consumed: a refused
            // descriptor outside the receive region is released from the RX
            // ring above but never refilled.

            const bool retained = pending_return.append( frame, num_returned );
            rocketnet_assert( retained );

            if ( !retained )
            {
                rocketnet_error( "xdp pending frame ownership invariant failed" );
                socket->quit = true;
                break;
            }

            return_pending_frames();
        }

        // The worker finished every write before publishing this buffer.
        // If no spare exists it retains the producer and stops peeking once
        // full; the kernel RX ring supplies backpressure without overwrites.

        if ( receive_buffer->num_packets > 0 )
        {
            rocketnet_platform_mutex_acquire( &socket->receive_mutex );
            socket->receive_handoff.publish();
            rocketnet_platform_mutex_release( &socket->receive_mutex );
        }

        // mark any completed send packet frames as free to be reused

        uint32_t complete_index;

        unsigned int num_completed = xsk_ring_cons__peek( &socket->complete_queue, XSK_RING_CONS__DEFAULT_NUM_DESCS, &complete_index );

        if ( num_completed > 0 )
        {
            for ( int i = 0; i < (int) num_completed; i++ )
            {
                uint64_t frame = *xsk_ring_cons__comp_addr( &socket->complete_queue, complete_index + (uint32_t) i );
                free_send_frame( socket, frame );
            }

            xsk_ring_cons__release( &socket->complete_queue, num_completed );
        }

        // if we don't already have a batch of packets to send, get one

        if ( num_batch_packets == 0 )
        {
            if ( send_worker_index < 0 )
            {
                rocketnet_platform_mutex_acquire( &socket->send_mutex );
                send_worker_index = socket->send_handoff.claim();
                rocketnet_platform_mutex_release( &socket->send_mutex );
            }

            const int on_index = send_worker_index;
            if ( on_index >= 0 )
            {
                rocketnet_server_socket_send_buffer_t * __restrict__ send_buffer = &socket->send_buffer[on_index];

                // IMPORTANT: clamp num packets because it's an atomic increment across multiple threads so it can go over max if unlucky

                int num_staged_packets = send_buffer->num_packets;

                if ( num_staged_packets > ROCKETNET_XDP_SEND_QUEUE_SIZE )
                {
                    num_staged_packets = ROCKETNET_XDP_SEND_QUEUE_SIZE;
                }

                rocketnet_assert( num_staged_packets >= 0 );
                rocketnet_assert( num_staged_packets <= ROCKETNET_XDP_SEND_QUEUE_SIZE );

                // grab a batch of non-zero byte packets from the send buffer

                const int start_index = send_buffer->packet_start_index;

                rocketnet_assert( start_index >= 0 );
                rocketnet_assert( start_index <= ROCKETNET_XDP_SEND_QUEUE_SIZE );

                for ( int i = start_index; i < num_staged_packets; i++ )
                {
                    if ( num_batch_packets >= ROCKETNET_XDP_SEND_BATCH_SIZE )
                        break;

                    if ( send_buffer->packet_info[i].packet_bytes > 0 )
                    {
                        const uint64_t frame = alloc_send_frame( socket );

                        if ( frame == INVALID_FRAME )
                        {
                            // Leave packet_start_index untouched. Completions in
                            // the next iteration may release frames; never kill
                            // the host or lose the staged packet on pressure.
                            if ( !reported_frame_pressure )
                                rocketnet_error( "xdp send frames exhausted; retaining staged packets for retry" );
                            reported_frame_pressure = true;
                            break;
                        }

                        reported_frame_pressure = false;

                        uint8_t * packet_data = (uint8_t *) socket->buffer + frame;

                        // the server's own address is the source, in the
                        // family it serves; the codec builds IPv4 or IPv6
                        // (with the mandatory UDP checksum) and refuses a
                        // destination of the other family, which is counted
                        // and never sent as a malformed frame

                        // the length is checked before the copy: 0 or past
                        // ROCKETNET_RECEIVE_CEILING is refused and counted in
                        // send_refused_length, and its frame is freed

                        const rocketnet_address_t * source = &socket->server_address;
                        if ( socket->serves_both )
                            source = send_buffer->packet_info[i].to.type == ROCKETNET_ADDRESS_IPV4 ? &socket->ipv4_address : &socket->ipv6_address;
                        uint64_t refused_length = 0;
                        const int copied_bytes = rocketnet_xdp_send_copy( packet_data, ROCKETNET_XDP_FRAME_SIZE, send_buffer->packet_info[i].to.type, send_buffer->packet_data[i] + ROCKETNET_FRAME_WIRE_OFFSET, send_buffer->packet_info[i].packet_bytes, &refused_length );
                        if ( refused_length )
                            socket->send_refused_length.fetch_add( refused_length, std::memory_order_relaxed );

                        const int frame_bytes = ( copied_bytes < 0 ) ? 0 : rocketnet_xdp_frame_build( packet_data, ROCKETNET_XDP_FRAME_SIZE, socket->server_ethernet_address, send_buffer->packet_info[i].destination_mac, source, &send_buffer->packet_info[i].to, copied_bytes );

                        if ( copied_bytes < 0 )
                        {
                            free_send_frame( socket, frame );
                        }
                        else if ( frame_bytes <= 0 )
                        {
                            free_send_frame( socket, frame );
                            socket->send_refused_build.fetch_add( 1, std::memory_order_relaxed );
                        }
                        else
                        {
                            batch_frames[num_batch_packets] = frame;
                            batch_packet_bytes[num_batch_packets] = frame_bytes;
                            num_batch_packets++;
                        }
                    }

                    send_buffer->packet_start_index++;
                }

                // UMEM now owns every copied packet. Only after the final copy
                // may this worker reset and return staging to the free ring
                // (proofs/tla/RNDriverSendStaging.tla WorkerRetire).

                if ( send_buffer->packet_start_index == num_staged_packets )
                {
                    send_buffer->num_packets.store( 0, std::memory_order_relaxed );
                    send_buffer->packet_start_index = 0;
                    rocketnet_platform_mutex_acquire( &socket->send_mutex );
                    socket->send_handoff.recycle( on_index );
                    rocketnet_platform_mutex_release( &socket->send_mutex );

                    // one warning per queue per flush for the starts this
                    // queue refused, naming the queue and the count

                    const uint64_t refused_total = socket->send_refused_overflow.load( std::memory_order_relaxed );
                    if ( refused_total > socket->send_refused_overflow_warned )
                    {
                        rocketnet_warn( "xdp queue %d refused %llu packet starts past its send queue size",
                                        socket->queue, (unsigned long long) ( refused_total - socket->send_refused_overflow_warned ) );
                        socket->send_refused_overflow_warned = refused_total;
                    }

                    send_worker_index = -1;
                }
            }
        }

        // actually send the packets

        if ( num_batch_packets > 0 )
        {
            // reserve entries in the send queue

            uint32_t send_queue_index;
            int send_packets = (int) xsk_ring_prod__reserve( &socket->send_queue, (uint32_t) num_batch_packets, &send_queue_index );

            // All-or-zero: zero keeps the batch intact for retry.

            if ( send_packets == num_batch_packets )
            {
                // setup descriptors for packets in batch to be sent

                for ( int i = 0; i < send_packets; i++ )
                {
                    struct xdp_desc * __restrict__ desc = xsk_ring_prod__tx_desc( &socket->send_queue, send_queue_index + (uint32_t) i );
                    desc->addr = batch_frames[i];
                    desc->len = (__u32) batch_packet_bytes[i];
                }

                // submit send queue to transport

                xsk_ring_prod__submit( &socket->send_queue, (uint32_t) send_packets );

                // batch has been sent

                num_batch_packets = 0;
            }
        }

        if ( xsk_ring_prod__needs_wakeup( &socket->send_queue ) )
        {
            sendto( xsk_socket__fd( socket->xsk ), NULL, 0, MSG_DONTWAIT, NULL, 0 );
        }

        // The SCHED_RR worker spins on its dedicated isolated queue CPU by
        // design: NAPI runs in softirq on the queue CPUs, and the isolated-CPU
        // pre-flight above is the guard that keeps every queue on its own core.
        // This pass deliberately does not sleep. rocketnet_platform_sleep
        // returns at once for a non-positive request, so a zero-duration call
        // would never yield the CPU.

        if ( stopping )
            break;
    }

    // Last access to queue storage, acquired by stopped before destruction.

    socket->stopped.store( true, std::memory_order_release );
}

#else // #if ROCKETNET_XDP

int rocketnet_server_socket_linux_cpp_dummy = 0;

#endif // #if ROCKETNET_XDP
