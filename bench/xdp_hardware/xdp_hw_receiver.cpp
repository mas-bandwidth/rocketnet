/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    xdp_hw_receiver -- the hardware tier's receiver: the library's own server
    transport on the xdp driver, so the path under test is the shipped one
    (attach in native mode, AF_XDP bind per queue, the workers on
    num_os_cpus + queue). It adds nothing to the data path; it reads what the
    path already counts, at the moments run.sh asks, and answers on stdout.

    Started and stopped by run.sh inside one held run; never left running.

    usage:
        xdp_hw_receiver --address <ipv4:port> --queues <n> --os-cpus <first-worker-core>

    stdin commands, one per line; each answer is one line on stdout:
        snap         SNAP received=... xdp_redirected=... xdp_dropped_*=...
                     xsk_rx_dropped=... xsk_rx_ring_full=... refusals=...
        xsk          XSK lines, one per AF_XDP socket, then XSKEND
        lat_reset    forget the latency samples
        lat_report   LATENCY count=... p50_ns=... (software timestamps)
        quit         stop, destroy (the library detaches), exit 0

    Counts come from three places, none of them written by this program:
    the transport's counters (PACKETS_RECEIVED and every REFUSAL-class
    counter, found by name and class through the public counter API), the
    XDP program's per-CPU stats map (opened read-only from this instance's
    pin root, summed across CPUs), and each AF_XDP socket's kernel
    statistics (getsockopt XDP_STATISTICS) and options (XDP_OPTIONS, whose
    ZEROCOPY flag is what the zero_copy_bind_per_queue row reads).
*/

#include "rocketnet.h"
#include "rocketnet_wire.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_xdp.h>

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define XDPHW_PROBE_MAGIC "RNXDPHW1"

static const char * xdp_counter_names[ROCKETNET_SERVER_XDP_NUM_COUNTERS] =
{
    "xdp_redirected",
    "xdp_dropped_too_small",
    "xdp_dropped_too_large",
    "xdp_dropped_wrong_filter",
    "xdp_dropped_invalid_type",
    "xdp_dropped_wrong_epoch",
    "xdp_passed_ipv6_extension",
};

static_assert( ROCKETNET_SERVER_XDP_NUM_COUNTERS == 7, "name every stats map counter above, and count each new drop in counters.py's E2" );

static uint64_t monotonic_ns()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

struct xsk_info_t
{
    int fd;
    int queue;
    int ifindex;
    bool zerocopy;
    bool options_read;
    struct xdp_statistics stats;
    bool stats_read;
};

// every AF_XDP socket this process holds: the library's, one per queue.
// read-only: getsockname, getsockopt.

static std::vector<xsk_info_t> find_xsk_sockets()
{
    std::vector<xsk_info_t> result;
    DIR * dir = opendir( "/proc/self/fd" );
    if ( !dir )
        return result;
    struct dirent * entry;
    while ( ( entry = readdir( dir ) ) != NULL )
    {
        if ( entry->d_name[0] == '.' )
            continue;
        const int fd = atoi( entry->d_name );
        struct sockaddr_xdp address;
        socklen_t address_bytes = sizeof( address );
        memset( &address, 0, sizeof( address ) );
        if ( getsockname( fd, (struct sockaddr*) &address, &address_bytes ) != 0 )
            continue;
        if ( address.sxdp_family != AF_XDP )
            continue;
        xsk_info_t info;
        memset( &info, 0, sizeof( info ) );
        info.fd = fd;
        info.queue = (int) address.sxdp_queue_id;
        info.ifindex = (int) address.sxdp_ifindex;
        struct xdp_options options;
        socklen_t options_bytes = sizeof( options );
        if ( getsockopt( fd, SOL_XDP, XDP_OPTIONS, &options, &options_bytes ) == 0 )
        {
            info.options_read = true;
            info.zerocopy = ( options.flags & XDP_OPTIONS_ZEROCOPY ) != 0;
        }
        socklen_t stats_bytes = sizeof( info.stats );
        if ( getsockopt( fd, SOL_XDP, XDP_STATISTICS, &info.stats, &stats_bytes ) == 0 )
            info.stats_read = true;
        result.push_back( info );
    }
    closedir( dir );
    std::sort( result.begin(), result.end(), []( const xsk_info_t & a, const xsk_info_t & b ) { return a.queue < b.queue; } );
    return result;
}

// the stats map, opened by path from this instance's pin root
// /sys/fs/bpf/rocketnet_xdp_<token>_<pid>_<starttime>_<bootid>

static int open_stats_map()
{
    char marker[64];
    snprintf( marker, sizeof( marker ), "_%d_", (int) getpid() );
    DIR * dir = opendir( "/sys/fs/bpf" );
    if ( !dir )
        return -1;
    int fd = -1;
    struct dirent * entry;
    while ( ( entry = readdir( dir ) ) != NULL )
    {
        if ( strncmp( entry->d_name, "rocketnet_xdp_", 14 ) != 0 || strlen( entry->d_name ) <= 14 + 16 )
            continue;
        if ( strstr( entry->d_name + 14 + 16, marker ) != entry->d_name + 14 + 16 )
            continue;
        char path[512];
        snprintf( path, sizeof( path ), "/sys/fs/bpf/%s/server_xdp_stats_map", entry->d_name );
        fd = bpf_obj_get( path );
        if ( fd >= 0 )
        {
            fprintf( stderr, "xdp_hw_receiver: stats map %s\n", path );
            break;
        }
    }
    closedir( dir );
    return fd;
}

static bool read_stats_map( int fd, uint64_t * counters )
{
    memset( counters, 0, sizeof( uint64_t ) * ROCKETNET_SERVER_XDP_NUM_COUNTERS );
    if ( fd < 0 )
        return false;
    const int num_cpus = libbpf_num_possible_cpus();
    if ( num_cpus <= 0 )
        return false;
    std::vector<struct rocketnet_server_xdp_stats_t> values( num_cpus );
    __u32 key = 0;
    if ( bpf_map_lookup_elem( fd, &key, values.data() ) != 0 )
        return false;
    for ( int cpu = 0; cpu < num_cpus; cpu++ )
        for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
            counters[i] += values[cpu].counters[i];
    return true;
}

static int find_counter( const char * name )
{
    const int n = rocketnet_num_counters();
    for ( int i = 0; i < n; i++ )
    {
        const char * counter_name = rocketnet_counter_name( i );
        if ( counter_name && strcmp( counter_name, name ) == 0 )
            return i;
    }
    return -1;
}

static void usage()
{
    fprintf( stderr, "usage: xdp_hw_receiver --address <ipv4:port> --queues <n> --os-cpus <first-worker-core>\n" );
    exit( 2 );
}

int main( int argc, char ** argv )
{
    const char * address = NULL;
    int queues = 0;
    int os_cpus = 0;

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--address" ) == 0 && i + 1 < argc )
            address = argv[++i];
        else if ( strcmp( argv[i], "--queues" ) == 0 && i + 1 < argc )
            queues = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--os-cpus" ) == 0 && i + 1 < argc )
            os_cpus = atoi( argv[++i] );
        else
            usage();
    }
    if ( !address || queues <= 0 || os_cpus <= 0 )
        usage();

    setvbuf( stdout, NULL, _IOLBF, 0 );

    if ( !rocketnet_init( NULL ) )
    {
        printf( "FAIL init\n" );
        return 1;
    }

    // the development config: the published development keys and the test
    // filter, which the sender's frames carry. no key is printed here.

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof( config.driver ), "xdp" );       // a one-element list is a pin: xdp or loud failure
    config.num_xdp_queues = queues;
    config.num_os_cpus = os_cpus;
    config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, address, &config );
    if ( !server )
    {
        printf( "FAIL create: the xdp driver refused; the library's log above names the reason\n" );
        rocketnet_term();
        return 1;
    }

    const char * driver = rocketnet_server_transport_driver( server );
    printf( "READY driver=%s pid=%d queues=%d os_cpus=%d\n", driver ? driver : "(none)", (int) getpid(), queues, os_cpus );

    const int packets_received_counter = find_counter( "PACKETS_RECEIVED" );
    if ( packets_received_counter < 0 )
    {
        printf( "FAIL counter PACKETS_RECEIVED not found by name\n" );
    }

    std::vector<int> refusal_counters;
    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        if ( rocketnet_counter_class( i ) == ROCKETNET_COUNTER_CLASS_REFUSAL )
            refusal_counters.push_back( i );
    }

    const int stats_map_fd = open_stats_map();
    if ( stats_map_fd < 0 )
        printf( "WARN stats map not found under /sys/fs/bpf for pid %d\n", (int) getpid() );

    std::vector<uint64_t> latency_ns;
    latency_ns.reserve( 1 << 20 );
    uint64_t probe_payloads = 0;
    uint64_t other_payloads = 0;

    bool quit = false;
    char line[256];
    size_t line_bytes = 0;

    while ( !quit )
    {
        rocketnet_server_transport_update( server, rocketnet_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        if ( num_payloads > 0 )
        {
            const uint64_t now = monotonic_ns();
            for ( int i = 0; i < num_payloads; i++ )
            {
                int client_index = 0;
                int payload_bytes = 0;
                const uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &payload_bytes );
                if ( payload && payload_bytes >= 24 && memcmp( payload, XDPHW_PROBE_MAGIC, 8 ) == 0 )
                {
                    uint64_t sent_ns;
                    memcpy( &sent_ns, payload + 16, 8 );
                    if ( now >= sent_ns )
                        latency_ns.push_back( now - sent_ns );
                    probe_payloads++;
                }
                else
                {
                    other_payloads++;
                }
            }
        }

        struct pollfd pfd;
        pfd.fd = 0;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if ( poll( &pfd, 1, 0 ) <= 0 )
            continue;
        if ( pfd.revents & ( POLLHUP | POLLERR ) && !( pfd.revents & POLLIN ) )
        {
            quit = true;
            break;
        }

        char c;
        const ssize_t n = read( 0, &c, 1 );
        if ( n <= 0 )
        {
            quit = true;
            break;
        }
        if ( c != '\n' )
        {
            if ( line_bytes + 1 < sizeof( line ) )
                line[line_bytes++] = c;
            continue;
        }
        line[line_bytes] = '\0';
        line_bytes = 0;

        if ( strcmp( line, "snap" ) == 0 )
        {
            const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
            const uint64_t received = ( counters && packets_received_counter >= 0 ) ? rocketnet_counter_value( counters, packets_received_counter ) : 0;
            uint64_t refusals = 0;
            std::string refusal_names;
            for ( int index : refusal_counters )
            {
                const uint64_t value = counters ? rocketnet_counter_value( counters, index ) : 0;
                if ( value )
                {
                    refusals += value;
                    refusal_names += std::string( refusal_names.empty() ? "" : "," ) + rocketnet_counter_name( index ) + ":" + std::to_string( value );
                }
            }
            uint64_t xdp[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
            const bool xdp_read = read_stats_map( stats_map_fd, xdp );
            uint64_t xsk_rx_dropped = 0, xsk_rx_ring_full = 0, xsk_rx_invalid = 0, xsk_fill_empty = 0;
            for ( const xsk_info_t & info : find_xsk_sockets() )
            {
                xsk_rx_dropped += info.stats.rx_dropped;
                xsk_rx_ring_full += info.stats.rx_ring_full;
                xsk_rx_invalid += info.stats.rx_invalid_descs;
                xsk_fill_empty += info.stats.rx_fill_ring_empty_descs;
            }
            printf( "SNAP received=%" PRIu64 " refusals=%" PRIu64, received, refusals );
            for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
                printf( " %s=%" PRIu64, xdp_counter_names[i], xdp[i] );
            printf( " xdp_map=%s xsk_rx_dropped=%" PRIu64 " xsk_rx_ring_full=%" PRIu64 " xsk_rx_invalid=%" PRIu64 " xsk_fill_empty=%" PRIu64 " probe_payloads=%" PRIu64 " other_payloads=%" PRIu64 " refusal_names=%s\n",
                xdp_read ? "read" : "unread", xsk_rx_dropped, xsk_rx_ring_full, xsk_rx_invalid, xsk_fill_empty, probe_payloads, other_payloads,
                refusal_names.empty() ? "-" : refusal_names.c_str() );
        }
        else if ( strcmp( line, "xsk" ) == 0 )
        {
            for ( const xsk_info_t & info : find_xsk_sockets() )
            {
                printf( "XSK queue=%d ifindex=%d zerocopy=%s rx_dropped=%" PRIu64 " rx_ring_full=%" PRIu64 "\n",
                    info.queue, info.ifindex, info.options_read ? ( info.zerocopy ? "1" : "0" ) : "unknown",
                    (uint64_t) info.stats.rx_dropped, (uint64_t) info.stats.rx_ring_full );
            }
            printf( "XSKEND\n" );
        }
        else if ( strcmp( line, "lat_reset" ) == 0 )
        {
            latency_ns.clear();
            probe_payloads = 0;
            printf( "LATRESET\n" );
        }
        else if ( strcmp( line, "lat_report" ) == 0 )
        {
            std::vector<uint64_t> sorted = latency_ns;
            std::sort( sorted.begin(), sorted.end() );
            auto pct = [&]( double p ) -> uint64_t
            {
                if ( sorted.empty() )
                    return 0;
                size_t index = (size_t) ( p * (double) ( sorted.size() - 1 ) + 0.5 );
                return sorted[std::min( index, sorted.size() - 1 )];
            };
            printf( "LATENCY count=%zu p50_ns=%" PRIu64 " p90_ns=%" PRIu64 " p99_ns=%" PRIu64 " p999_ns=%" PRIu64 " max_ns=%" PRIu64 " clock=CLOCK_MONOTONIC source=software\n",
                sorted.size(), pct( 0.50 ), pct( 0.90 ), pct( 0.99 ), pct( 0.999 ), sorted.empty() ? 0 : sorted.back() );
        }
        else if ( strcmp( line, "quit" ) == 0 )
        {
            quit = true;
        }
        else if ( line[0] != '\0' )
        {
            printf( "UNKNOWN %s\n", line );
        }
    }

    if ( stats_map_fd >= 0 )
        close( stats_map_fd );

    rocketnet_server_transport_stop( server );
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED )
        rocketnet_server_transport_update( server, rocketnet_time() );
    rocketnet_server_transport_destroy( server );
    rocketnet_term();
    printf( "STOPPED\n" );
    return 0;
}
