/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The kernel side of the XDP kernel tier. Runs only inside the guest that
// run.sh boots, as the guest's root, against the guest's own kernel: nothing
// here ever reaches the machine that runs run.sh.
//
//     xdp_vm_attach verify OBJECT
//         load every program in OBJECT through this kernel's verifier and
//         print "verified_insns PROGRAM N" for each; on a refusal print the
//         verifier log tail and exit 1
//
//     xdp_vm_attach testrun OBJECT MANIFEST PCAPDIR
//         run every manifest frame through BPF_PROG_TEST_RUN, with the XSKMAP
//         retyped to a DEVMAP whose slot 0 is the loopback interface (the
//         method of test_xdp_filter), and compare verdict and counter
//
//     xdp_vm_attach serve OBJECT IFNAME native|skb MANIFEST RECEIVED.pcap COUNTERS READYFILE STOPFILE
//         attach OBJECT to IFNAME in exactly the mode named (never a fall to
//         another mode), bind an AF_XDP socket in XDP_COPY mode to queue 0,
//         put it in server_xdp_socket_map, write the config map from the
//         manifest's #config line, touch READYFILE, write every frame that
//         arrives on the socket to RECEIVED.pcap until STOPFILE exists, then
//         write the counters to COUNTERS, detach, close the socket and remove
//         every pin it made
//
// The open sequence mirrors sdk/rocketnet_server_socket_linux.cpp:
// xdp_program__open_file with a pin root of its own, xdp_program__attach,
// xsk_socket__create with XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD and the socket
// fd written into server_xdp_socket_map at the queue index.

#define _GNU_SOURCE

#include "rocketnet_wire.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <linux/bpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FRAME_CAPACITY 2048
#define MAX_FRAMES 4096
#define MAX_ROWS 4096

#define PIN_ROOT "/sys/fs/bpf/rocketnet_xdp_vm"

// --------------------------------------------------------------------------
// counters by name, so a manifest written for a newer program fails by name
// on an older build rather than reading the wrong slot

struct counter_name_t
{
    const char * name;
    int index;
};

static const struct counter_name_t counter_names[] =
{
    { "REDIRECTED",             ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED },
    { "DROPPED_TOO_SMALL",      ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL },
    { "DROPPED_TOO_LARGE",      ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE },
    { "DROPPED_WRONG_FILTER",   ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER },
    { "DROPPED_INVALID_TYPE",   ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE },
    { "DROPPED_WRONG_EPOCH",    ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH },
#ifdef ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION
    { "PASSED_IPV6_EXTENSION",  ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION },
#endif
};

#define NUM_COUNTER_NAMES ( (int) ( sizeof(counter_names) / sizeof(counter_names[0]) ) )

static int counter_index( const char * name )
{
    if ( strcmp( name, "-" ) == 0 )
        return -1;
    for ( int i = 0; i < NUM_COUNTER_NAMES; i++ )
        if ( strcmp( counter_names[i].name, name ) == 0 )
            return counter_names[i].index;
    return -2;
}

// --------------------------------------------------------------------------
// the classic pcap file

struct pcap_t
{
    int count;
    int size[MAX_FRAMES];
    uint8_t * data[MAX_FRAMES];
};

static uint32_t read32( const uint8_t * p, int swap )
{
    uint32_t v = (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
    if ( swap )
        v = ( v >> 24 ) | ( ( v >> 8 ) & 0xFF00 ) | ( ( v << 8 ) & 0xFF0000 ) | ( v << 24 );
    return v;
}

static int pcap_read( const char * path, struct pcap_t * pcap )
{
    memset( pcap, 0, sizeof(*pcap) );
    FILE * file = fopen( path, "rb" );
    if ( !file )
        return 0;
    uint8_t header[24];
    int ok = fread( header, 1, 24, file ) == 24;
    const uint32_t magic = ok ? read32( header, 0 ) : 0;
    const int swap = magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1;
    ok = ok && ( swap || magic == 0xa1b2c3d4 || magic == 0xa1b23c4d ) && read32( header + 20, swap ) == 1;
    uint8_t record[16];
    while ( ok && fread( record, 1, 16, file ) == 16 )
    {
        const uint32_t length = read32( record + 8, swap );
        if ( length > FRAME_CAPACITY || pcap->count == MAX_FRAMES )
        {
            ok = 0;
            break;
        }
        uint8_t * data = (uint8_t*) malloc( length ? length : 1 );
        if ( !data || fread( data, 1, length, file ) != length )
        {
            free( data );
            ok = 0;
            break;
        }
        pcap->data[pcap->count] = data;
        pcap->size[pcap->count] = (int) length;
        pcap->count++;
    }
    fclose( file );
    if ( !ok )
        fprintf( stderr, "%s is not a readable ethernet pcap file\n", path );
    return ok;
}

static void write32( uint8_t * p, uint32_t v )
{
    p[0] = (uint8_t) v; p[1] = (uint8_t) ( v >> 8 ); p[2] = (uint8_t) ( v >> 16 ); p[3] = (uint8_t) ( v >> 24 );
}

static FILE * pcap_create( const char * path )
{
    FILE * file = fopen( path, "wb" );
    if ( !file )
        return NULL;
    uint8_t header[24];
    memset( header, 0, sizeof(header) );
    write32( header, 0xa1b2c3d4 );
    header[4] = 2; header[6] = 4;
    write32( header + 16, 65535 );
    write32( header + 20, 1 );
    fwrite( header, 1, sizeof(header), file );
    return file;
}

static void pcap_append( FILE * file, const uint8_t * data, int size )
{
    uint8_t record[16];
    memset( record, 0, sizeof(record) );
    write32( record + 8, (uint32_t) size );
    write32( record + 12, (uint32_t) size );
    fwrite( record, 1, sizeof(record), file );
    fwrite( data, 1, (size_t) size, file );
}

// --------------------------------------------------------------------------
// the manifest

struct row_t
{
    char file[256];
    int index;
    int queue;
    char verdict[32];
    char counter[64];
    char label[128];
};

struct config_line_t
{
    uint8_t ipv4[4];
    uint8_t ipv6[16];
    uint16_t port;
    uint8_t filter[4];
    uint8_t epoch;
    int present;
};

static struct row_t rows[MAX_ROWS];

// "#config port=40000 ipv4=10.211.0.1 ipv6=fd00:211::1 filter=b7590100 epoch=0"

static int parse_config( const char * line, struct config_line_t * config )
{
    char ipv4[64] = "", ipv6[64] = "", filter[16] = "";
    int port = 0, epoch = 0;
    if ( sscanf( line, "#config port=%d ipv4=%63s ipv6=%63s filter=%15s epoch=%d", &port, ipv4, ipv6, filter, &epoch ) != 5 )
        return 0;
    if ( inet_pton( AF_INET, ipv4, config->ipv4 ) != 1 || inet_pton( AF_INET6, ipv6, config->ipv6 ) != 1 || strlen( filter ) != 8 )
        return 0;
    for ( int i = 0; i < 4; i++ )
    {
        unsigned int byte;
        if ( sscanf( filter + i * 2, "%2x", &byte ) != 1 )
            return 0;
        config->filter[i] = (uint8_t) byte;
    }
    config->port = (uint16_t) port;
    config->epoch = (uint8_t) epoch;
    config->present = 1;
    return 1;
}

static int manifest_read( const char * path, struct config_line_t * config )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
    {
        fprintf( stderr, "could not open %s: %s\n", path, strerror( errno ) );
        return -1;
    }
    memset( config, 0, sizeof(*config) );
    char line[1024];
    int count = 0;
    while ( fgets( line, sizeof(line), file ) )
    {
        if ( strncmp( line, "#config ", 8 ) == 0 )
        {
            if ( !parse_config( line, config ) )
            {
                fprintf( stderr, "%s: malformed #config line\n", path );
                fclose( file );
                return -1;
            }
            continue;
        }
        if ( line[0] == '#' || line[0] == '\n' || count == MAX_ROWS )
            continue;
        struct row_t * row = &rows[count];
        memset( row, 0, sizeof(*row) );
        if ( sscanf( line, "%255[^\t]\t%d\t%d\t%31[^\t]\t%63[^\t\n]\t%127[^\t\n]", row->file, &row->index, &row->queue, row->verdict, row->counter, row->label ) < 5 )
        {
            fprintf( stderr, "%s: malformed row: %s", path, line );
            fclose( file );
            return -1;
        }
        count++;
    }
    fclose( file );
    if ( !config->present )
    {
        fprintf( stderr, "%s has no #config line\n", path );
        return -1;
    }
    return count;
}

static struct pcap_t * pcap_cached( const char * dir, const char * name )
{
    static char names[16][256];
    static struct pcap_t * files[16];
    for ( int i = 0; i < 16; i++ )
    {
        if ( files[i] && strcmp( names[i], name ) == 0 )
            return files[i];
        if ( !files[i] )
        {
            char path[1024];
            if ( snprintf( path, sizeof(path), "%s/%s", dir, name ) >= (int) sizeof(path) )
                return NULL;
            files[i] = (struct pcap_t*) calloc( 1, sizeof(struct pcap_t) );
            if ( !files[i] || !pcap_read( path, files[i] ) )
            {
                free( files[i] );
                files[i] = NULL;
                return NULL;
            }
            const size_t length = strnlen( name, sizeof(names[i]) - 1 );
            memcpy( names[i], name, length );
            names[i][length] = '\0';
            return files[i];
        }
    }
    return NULL;
}

// --------------------------------------------------------------------------
// the program's maps

// The config is written as the map's own value size: 12 bytes before the
// IPv6 receive change and 32 after it. The offsets are the ones
// rocketnet_wire.h fixes: address 0, port 4, filter value 6, epoch 10, and
// the IPv6 destination at 16 when the value has room for it.

static int write_config( int fd, int value_size, const struct config_line_t * config )
{
    uint8_t value[256];
    if ( value_size < 12 || value_size > (int) sizeof(value) )
    {
        fprintf( stderr, "the config map value is %d bytes, which this harness does not know\n", value_size );
        return 0;
    }
    memset( value, 0, sizeof(value) );
    memcpy( value + 0, config->ipv4, 4 );
    const uint16_t port = htons( config->port );
    memcpy( value + 4, &port, 2 );
    memcpy( value + 6, config->filter, 4 );
    value[10] = config->epoch;
    if ( value_size >= 32 )
        memcpy( value + 16, config->ipv6, 16 );
    const __u32 key = 0;
    if ( bpf_map_update_elem( fd, &key, value, BPF_ANY ) != 0 )
    {
        fprintf( stderr, "could not write the config map: %s\n", strerror( errno ) );
        return 0;
    }
    return 1;
}

static void read_counters( int fd, int value_size, uint64_t * counters, int slots )
{
    memset( counters, 0, (size_t) slots * sizeof(uint64_t) );
    const int cpus = libbpf_num_possible_cpus();
    const int stride = ( value_size + 7 ) / 8 * 8;
    uint8_t * values = (uint8_t*) calloc( (size_t) ( cpus > 0 ? cpus : 1 ), (size_t) stride );
    const __u32 key = 0;
    if ( values && bpf_map_lookup_elem( fd, &key, values ) == 0 )
    {
        for ( int cpu = 0; cpu < cpus; cpu++ )
        {
            const uint64_t * value = (const uint64_t*) ( values + cpu * stride );
            for ( int i = 0; i < slots && i < value_size / 8; i++ )
                counters[i] += value[i];
        }
    }
    free( values );
}

// --------------------------------------------------------------------------
// the verifier log

static char verifier_log[1 << 22];

static void print_log_tail( int lines )
{
    const size_t length = strnlen( verifier_log, sizeof(verifier_log) );
    size_t end = length;
    while ( end > 0 && ( verifier_log[end - 1] == '\n' || verifier_log[end - 1] == ' ' ) )
        end--;
    size_t start = end;
    int seen = 0;
    while ( start > 0 )
    {
        if ( verifier_log[start - 1] == '\n' && ++seen == lines )
            break;
        start--;
    }
    if ( end > start )
        printf( "verifier log tail:\n%.*s\n", (int) ( end - start ), verifier_log + start );
}

static int print_libbpf( enum libbpf_print_level level, const char * format, va_list args )
{
    if ( level == LIBBPF_DEBUG )
        return 0;
    return vfprintf( stderr, format, args );
}

static struct bpf_object * open_object( const char * path )
{
    libbpf_set_print( print_libbpf );
    struct bpf_object_open_opts options;
    memset( &options, 0, sizeof(options) );
    options.sz = sizeof(options);
    options.kernel_log_buf = verifier_log;
    options.kernel_log_size = sizeof(verifier_log);
    options.kernel_log_level = 1;
    struct bpf_object * object = bpf_object__open_file( path, &options );
    if ( object == NULL )
    {
        fprintf( stderr, "could not open %s: %s\n", path, strerror( errno ) );
        return NULL;
    }
    // nothing is pinned by a verify or a test run
    struct bpf_map * map;
    bpf_object__for_each_map( map, object )
        bpf_map__set_pin_path( map, NULL );
    return object;
}

static void raise_memlock( void )
{
    struct rlimit limit = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit( RLIMIT_MEMLOCK, &limit );
}

// --------------------------------------------------------------------------
// verify

static int command_verify( const char * path )
{
    raise_memlock();
    struct bpf_object * object = open_object( path );
    if ( !object )
        return 1;
    const int result = bpf_object__load( object );
    if ( result != 0 )
    {
        printf( "load of %s refused: %s\n", path, strerror( -result ) );
        print_log_tail( 30 );
        bpf_object__close( object );
        return 1;
    }
    struct bpf_program * program;
    int programs = 0;
    bpf_object__for_each_program( program, object )
    {
        struct bpf_prog_info info;
        __u32 info_size = sizeof(info);
        memset( &info, 0, sizeof(info) );
        if ( bpf_prog_get_info_by_fd( bpf_program__fd( program ), &info, &info_size ) != 0 )
        {
            printf( "could not read the program info of %s: %s\n", bpf_program__name( program ), strerror( errno ) );
            bpf_object__close( object );
            return 1;
        }
        printf( "verified_insns %s %u\n", bpf_program__name( program ), info.verified_insns );
        programs++;
    }
    print_log_tail( 1 );
    bpf_object__close( object );
    if ( programs == 0 )
    {
        printf( "%s holds no program\n", path );
        return 1;
    }
    return 0;
}

// --------------------------------------------------------------------------
// testrun

static const char * verdict_name( int verdict )
{
    switch ( verdict )
    {
        case XDP_ABORTED:   return "XDP_ABORTED";
        case XDP_DROP:      return "XDP_DROP";
        case XDP_PASS:      return "XDP_PASS";
        case XDP_TX:        return "XDP_TX";
        case XDP_REDIRECT:  return "XDP_REDIRECT";
        default:            return "XDP_?";
    }
}

static int command_testrun( const char * path, const char * manifest, const char * dir )
{
    raise_memlock();
    struct config_line_t config;
    const int count = manifest_read( manifest, &config );
    if ( count < 0 )
        return 1;

    struct bpf_object * object = open_object( path );
    if ( !object )
        return 1;
    struct bpf_program * program = bpf_object__find_program_by_name( object, "server_xdp_filter" );
    struct bpf_map * socket_map = bpf_object__find_map_by_name( object, "server_xdp_socket_map" );
    if ( !program || !socket_map || bpf_map__set_type( socket_map, BPF_MAP_TYPE_DEVMAP ) != 0 )
    {
        printf( "the object has no server_xdp_filter or no server_xdp_socket_map to retype\n" );
        return 1;
    }
    const int result = bpf_object__load( object );
    if ( result != 0 )
    {
        printf( "load refused: %s\n", strerror( -result ) );
        print_log_tail( 30 );
        return 1;
    }
    struct bpf_map * config_map = bpf_object__find_map_by_name( object, "server_xdp_config_map" );
    struct bpf_map * stats_map = bpf_object__find_map_by_name( object, "server_xdp_stats_map" );
    if ( !config_map || !stats_map )
    {
        printf( "the object lacks its config and stats maps\n" );
        return 1;
    }
    const __u32 key = 0;
    const __u32 loopback = if_nametoindex( "lo" );
    if ( loopback == 0 || bpf_map_update_elem( bpf_map__fd( socket_map ), &key, &loopback, BPF_ANY ) != 0 )
    {
        printf( "could not point the redirect map at the loopback interface: %s\n", strerror( errno ) );
        return 1;
    }
    if ( !write_config( bpf_map__fd( config_map ), (int) bpf_map__value_size( config_map ), &config ) )
        return 1;

    const int stats_fd = bpf_map__fd( stats_map );
    const int stats_size = (int) bpf_map__value_size( stats_map );
    const int slots = stats_size / 8;
    uint64_t before[64], after[64];
    if ( slots > 64 )
        return 1;

    int failures = 0;
    for ( int i = 0; i < count; i++ )
    {
        const struct row_t * row = &rows[i];
        struct pcap_t * pcap = pcap_cached( dir, row->file );
        if ( !pcap || row->index >= pcap->count )
        {
            printf( "  manifest row %s:%d names no frame\n", row->file, row->index );
            failures++;
            continue;
        }
        const int want_counter = counter_index( row->counter );
        if ( want_counter == -2 )
        {
            printf( "  FAIL %s:%d %s: counter %s is not in this build of the program\n", row->file, row->index, row->label, row->counter );
            failures++;
            continue;
        }

        read_counters( stats_fd, stats_size, before, slots );

        static uint8_t out[4096];
        struct bpf_test_run_opts options;
        memset( &options, 0, sizeof(options) );
        options.sz = sizeof(options);
        options.data_in = pcap->data[row->index];
        options.data_size_in = (__u32) pcap->size[row->index];
        options.data_out = out;
        options.data_size_out = sizeof(out);
        options.repeat = 1;
        if ( bpf_prog_test_run_opts( bpf_program__fd( program ), &options ) != 0 )
        {
            printf( "  FAIL %s:%d %s: test_run refused the frame: %s\n", row->file, row->index, row->label, strerror( errno ) );
            failures++;
            continue;
        }

        read_counters( stats_fd, stats_size, after, slots );

        int ok = strcmp( verdict_name( (int) options.retval ), row->verdict ) == 0;
        if ( !ok )
            printf( "  FAIL %s:%d %s: verdict %s, the manifest says %s\n", row->file, row->index, row->label, verdict_name( (int) options.retval ), row->verdict );
        for ( int c = 0; c < slots; c++ )
        {
            const uint64_t want = ( c == want_counter ) ? 1 : 0;
            if ( after[c] - before[c] != want )
            {
                printf( "  FAIL %s:%d %s: counter slot %d moved by %llu, the manifest says %llu\n", row->file, row->index, row->label, c,
                        (unsigned long long) ( after[c] - before[c] ), (unsigned long long) want );
                ok = 0;
            }
        }
        if ( !ok )
            failures++;
    }
    printf( "testrun frames %d failed %d\n", count, failures );
    bpf_object__close( object );
    return failures == 0 ? 0 : 1;
}

// --------------------------------------------------------------------------
// serve

#define UMEM_FRAMES 4096
#define UMEM_FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define RING_SIZE 2048

static void remove_pins( void )
{
    DIR * dir = opendir( PIN_ROOT );
    if ( !dir )
        return;
    struct dirent * entry;
    while ( ( entry = readdir( dir ) ) )
    {
        if ( entry->d_name[0] == '.' )
            continue;
        char path[512];
        snprintf( path, sizeof(path), "%s/%s", PIN_ROOT, entry->d_name );
        unlink( path );
    }
    closedir( dir );
    rmdir( PIN_ROOT );
}

static int command_serve( const char * path, const char * ifname, const char * mode_name, const char * manifest,
                          const char * received_path, const char * counters_path, const char * readyfile, const char * stopfile )
{
    raise_memlock();
    libbpf_set_print( print_libbpf );

    struct config_line_t config;
    if ( manifest_read( manifest, &config ) < 0 )
        return 1;

    enum xdp_attach_mode mode;
    if ( strcmp( mode_name, "native" ) == 0 )
        mode = XDP_MODE_NATIVE;
    else if ( strcmp( mode_name, "skb" ) == 0 )
        mode = XDP_MODE_SKB;
    else
    {
        fprintf( stderr, "mode is native or skb\n" );
        return 2;
    }

    const int ifindex = (int) if_nametoindex( ifname );
    if ( ifindex == 0 )
    {
        printf( "no interface %s\n", ifname );
        return 1;
    }

    if ( mkdir( PIN_ROOT, 0700 ) != 0 )
    {
        printf( "could not create the pin root %s: %s\n", PIN_ROOT, strerror( errno ) );
        return 1;
    }

    DECLARE_LIBBPF_OPTS( bpf_object_open_opts, open_options, .pin_root_path = PIN_ROOT );
    struct xdp_program * program = xdp_program__open_file( path, "server_xdp", &open_options );
    if ( libxdp_get_error( program ) )
    {
        printf( "could not open %s as an XDP program\n", path );
        remove_pins();
        return 1;
    }

    int result = xdp_program__attach( program, ifindex, mode, 0 );
    if ( result != 0 )
    {
        char message[256];
        libxdp_strerror( result, message, sizeof(message) );
        printf( "attach in %s mode refused by the kernel: %s\n", mode_name, message );
        xdp_program__close( program );
        remove_pins();
        return 1;
    }
    printf( "attached %s mode on %s\n", mode_name, ifname );

    struct bpf_object * object = xdp_program__bpf_obj( program );
    struct bpf_map * config_map = bpf_object__find_map_by_name( object, "server_xdp_config_map" );
    struct bpf_map * stats_map = bpf_object__find_map_by_name( object, "server_xdp_stats_map" );
    struct bpf_map * socket_map = bpf_object__find_map_by_name( object, "server_xdp_socket_map" );

    int failed = !config_map || !stats_map || !socket_map;
    if ( failed )
        printf( "the attached object lacks its maps\n" );

    void * buffer = NULL;
    struct xsk_umem * umem = NULL;
    struct xsk_socket * xsk = NULL;
    struct xsk_ring_prod fill;
    struct xsk_ring_cons complete;
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    const size_t buffer_size = (size_t) UMEM_FRAMES * UMEM_FRAME_SIZE;

    if ( !failed )
    {
        buffer = mmap( NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
        struct xsk_umem_config umem_config;
        memset( &umem_config, 0, sizeof(umem_config) );
        umem_config.fill_size = RING_SIZE;
        umem_config.comp_size = RING_SIZE;
        umem_config.frame_size = UMEM_FRAME_SIZE;
        if ( buffer == MAP_FAILED || xsk_umem__create( &umem, buffer, buffer_size, &fill, &complete, &umem_config ) != 0 )
        {
            printf( "could not create the umem\n" );
            if ( buffer == MAP_FAILED )
                buffer = NULL;
            failed = 1;
        }
    }

    if ( !failed )
    {
        struct xsk_socket_config xsk_config;
        memset( &xsk_config, 0, sizeof(xsk_config) );
        xsk_config.rx_size = RING_SIZE;
        xsk_config.tx_size = RING_SIZE;
        xsk_config.bind_flags = XDP_COPY;
        xsk_config.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
        result = xsk_socket__create( &xsk, ifname, 0, umem, &rx, &tx, &xsk_config );
        if ( result != 0 )
        {
            printf( "AF_XDP bind in copy mode on %s queue 0 refused: %s\n", ifname, strerror( -result ) );
            failed = 1;
        }
        else
            printf( "af_xdp bound copy mode on %s queue 0\n", ifname );
    }

    if ( !failed )
    {
        const __u32 key = 0;
        const __u32 fd = (__u32) xsk_socket__fd( xsk );
        if ( bpf_map_update_elem( bpf_map__fd( socket_map ), &key, &fd, BPF_ANY ) != 0 )
        {
            printf( "could not put the socket in server_xdp_socket_map: %s\n", strerror( errno ) );
            failed = 1;
        }
        else if ( !write_config( bpf_map__fd( config_map ), (int) bpf_map__value_size( config_map ), &config ) )
            failed = 1;
    }

    if ( !failed )
    {
        __u32 index;
        if ( xsk_ring_prod__reserve( &fill, RING_SIZE, &index ) != RING_SIZE )
        {
            printf( "could not fill the umem fill ring\n" );
            failed = 1;
        }
        else
        {
            for ( int i = 0; i < RING_SIZE; i++ )
                *xsk_ring_prod__fill_addr( &fill, index++ ) = (__u64) i * UMEM_FRAME_SIZE;
            xsk_ring_prod__submit( &fill, RING_SIZE );
        }
    }

    int received = 0;
    if ( !failed )
    {
        FILE * out = pcap_create( received_path );
        FILE * ready = fopen( readyfile, "w" );
        if ( !out || !ready )
        {
            printf( "could not create %s or %s\n", received_path, readyfile );
            failed = 1;
        }
        if ( ready )
            fclose( ready );

        // the guest's own clock bounds the wait, so a lost stop file ends the row
        const time_t deadline = time( NULL ) + 60;
        int draining = 0;
        while ( out && !failed && time( NULL ) < deadline )
        {
            struct pollfd p = { xsk_socket__fd( xsk ), POLLIN, 0 };
            const int ready_count = poll( &p, 1, 100 );
            __u32 index;
            const int n = ready_count > 0 ? (int) xsk_ring_cons__peek( &rx, 64, &index ) : 0;
            if ( n > 0 )
            {
                __u32 fill_index;
                while ( xsk_ring_prod__reserve( &fill, (__u32) n, &fill_index ) != (__u32) n )
                    ;
                for ( int i = 0; i < n; i++ )
                {
                    const struct xdp_desc * desc = xsk_ring_cons__rx_desc( &rx, index + i );
                    const uint8_t * data = (const uint8_t*) xsk_umem__get_data( buffer, desc->addr );
                    pcap_append( out, data, (int) desc->len );
                    received++;
                    *xsk_ring_prod__fill_addr( &fill, fill_index + i ) = xsk_umem__extract_addr( desc->addr );
                }
                xsk_ring_cons__release( &rx, (__u32) n );
                xsk_ring_prod__submit( &fill, (__u32) n );
                continue;
            }
            // after the stop file appears, one more empty poll drains the ring
            if ( access( stopfile, F_OK ) == 0 )
            {
                if ( draining )
                    break;
                draining = 1;
            }
        }
        if ( out )
            fclose( out );
        printf( "af_xdp received %d frames\n", received );
    }

    if ( stats_map )
    {
        const int stats_size = (int) bpf_map__value_size( stats_map );
        uint64_t counters[64];
        const int slots = stats_size / 8 < 64 ? stats_size / 8 : 64;
        read_counters( bpf_map__fd( stats_map ), stats_size, counters, slots );
        FILE * file = fopen( counters_path, "w" );
        for ( int i = 0; file && i < NUM_COUNTER_NAMES; i++ )
            if ( counter_names[i].index < slots )
                fprintf( file, "%s %llu\n", counter_names[i].name, (unsigned long long) counters[counter_names[i].index] );
        if ( file )
            fclose( file );
    }

    if ( xsk )
        xsk_socket__delete( xsk );
    if ( umem )
        xsk_umem__delete( umem );
    if ( buffer )
        munmap( buffer, buffer_size );

    result = xdp_program__detach( program, ifindex, mode, 0 );
    if ( result != 0 )
    {
        char message[256];
        libxdp_strerror( result, message, sizeof(message) );
        printf( "detach refused: %s\n", message );
        failed = 1;
    }
    else
        printf( "detached\n" );
    xdp_program__close( program );
    remove_pins();
    return failed ? 1 : 0;
}

static int usage( void )
{
    fprintf( stderr,
             "usage: xdp_vm_attach verify OBJECT\n"
             "       xdp_vm_attach testrun OBJECT MANIFEST PCAPDIR\n"
             "       xdp_vm_attach serve OBJECT IFNAME native|skb MANIFEST RECEIVED.pcap COUNTERS READYFILE STOPFILE\n" );
    return 2;
}

int main( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    if ( argc == 3 && strcmp( argv[1], "verify" ) == 0 )
        return command_verify( argv[2] );
    if ( argc == 5 && strcmp( argv[1], "testrun" ) == 0 )
        return command_testrun( argv[2], argv[3], argv[4] );
    if ( argc == 10 && strcmp( argv[1], "serve" ) == 0 )
        return command_serve( argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9] );
    return usage();
}
