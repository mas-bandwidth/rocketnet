/*
    rocketnet transport probe (#81)

    Static, self-contained feature report for the Linux transport matrix.
    Prints what the running kernel and NICs actually support: io_uring
    availability and opcodes, UDP GSO/GRO socket options, per-interface
    tx-udp-segmentation offload, SO_REUSEPORT steering (cBPF vs eBPF),
    and the sysctls that gate them.

    Every check is a runtime attempt with the errno recorded -- nothing
    is inferred from the kernel version except where a feature has no
    probeable surface (named inline where so).

    Build:  gcc -O2 -static -o probe probe.c
    Run:    ./probe            (no privileges required; privileged
                                checks report their refusal)
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <linux/filter.h>
#include <linux/bpf.h>

/*
    io_uring ABI, defined locally so this file builds on hosts without
    <linux/io_uring.h> (the probe targets arbitrary hosts, GameLift
    images included). Numbers are kernel ABI, stable by contract.
*/

struct probe_io_sqring_offsets
{
    uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
    uint64_t user_addr;
};
struct probe_io_cqring_offsets
{
    uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
    uint64_t user_addr;
};

struct probe_io_uring_params
{
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct probe_io_sqring_offsets sq_off;
    struct probe_io_cqring_offsets cq_off;
};

struct probe_io_uring_probe_op
{
    uint8_t op;
    uint8_t resv;
    uint16_t flags;                                     /* IO_URING_OP_SUPPORTED = 1 << 0 */
    uint32_t resv2;
};

struct probe_io_uring_probe
{
    uint8_t last_op;
    uint8_t ops_len;
    uint16_t resv;
    uint32_t resv2[3];
    struct probe_io_uring_probe_op ops[256];
};

struct probe_io_uring_buf_reg
{
    uint64_t ring_addr;
    uint32_t ring_entries;
    uint16_t bgid;
    uint16_t flags;
    uint64_t resv[3];
};

#define PROBE_IORING_REGISTER_PROBE          8
#define PROBE_IORING_REGISTER_PBUF_RING     22
#define PROBE_IORING_UNREGISTER_PBUF_RING   23

#define PROBE_IORING_SETUP_SQPOLL       (1U << 1)

#define PROBE_IO_URING_OP_SUPPORTED     (1U << 0)

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup     425
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register  427
#endif

static const struct
{
    int op;
    const char * name;
} interesting_ops[] = {
    { 9, "IORING_OP_SENDMSG" },
    { 10, "IORING_OP_RECVMSG" },
    { 26, "IORING_OP_SEND" },
    { 27, "IORING_OP_RECV" },
    { 31, "IORING_OP_PROVIDE_BUFFERS" },
    { 45, "IORING_OP_SOCKET" },
    { 47, "IORING_OP_SEND_ZC" },
    { 48, "IORING_OP_SENDMSG_ZC" },
};

/* UDP socket options: numeric so the probe builds against old libc headers */
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#ifndef SO_ATTACH_REUSEPORT_CBPF
#define SO_ATTACH_REUSEPORT_CBPF 51
#endif
#ifndef SO_ATTACH_REUSEPORT_EBPF
#define SO_ATTACH_REUSEPORT_EBPF 52
#endif

static void print_sysctl( const char * path, const char * name )
{
    FILE * f = fopen( path, "r" );
    if ( !f )
    {
        printf( "  %-32s absent (%s)\n", name, strerror( errno ) );
        return;
    }
    char buf[128] = { 0 };
    if ( fgets( buf, sizeof( buf ), f ) )
    {
        buf[strcspn( buf, "\n" )] = 0;
        printf( "  %-32s %s\n", name, buf );
    }
    fclose( f );
}

static void probe_kernel( void )
{
    struct utsname u;
    uname( &u );
    printf( "kernel:\n" );
    printf( "  %-32s %s %s\n", "release", u.release, u.version );
    printf( "  %-32s %s\n", "machine", u.machine );
    print_sysctl( "/proc/sys/kernel/io_uring_disabled", "kernel.io_uring_disabled" );
    print_sysctl( "/proc/sys/kernel/unprivileged_bpf_disabled", "kernel.unprivileged_bpf_disabled" );
    print_sysctl( "/proc/sys/kernel/perf_event_paranoid", "kernel.perf_event_paranoid" );
    print_sysctl( "/proc/sys/net/core/rmem_max", "net.core.rmem_max" );
    print_sysctl( "/proc/sys/net/core/wmem_max", "net.core.wmem_max" );
}

static void probe_io_uring( void )
{
    printf( "io_uring:\n" );

    struct probe_io_uring_params params;
    memset( &params, 0, sizeof( params ) );
    int ring_fd = (int) syscall( __NR_io_uring_setup, 4, &params );
    if ( ring_fd < 0 )
    {
        printf( "  %-32s NO (%s)\n", "io_uring_setup", strerror( errno ) );
        return;
    }
    printf( "  %-32s YES (features=0x%x)\n", "io_uring_setup", params.features );

    static struct probe_io_uring_probe probe;
    memset( &probe, 0, sizeof( probe ) );
    int result = (int) syscall( __NR_io_uring_register, ring_fd, PROBE_IORING_REGISTER_PROBE, &probe, 256 );
    if ( result < 0 )
    {
        printf( "  %-32s NO (%s)\n", "IORING_REGISTER_PROBE", strerror( errno ) );
    }
    else
    {
        printf( "  %-32s last_op=%d\n", "IORING_REGISTER_PROBE", probe.last_op );
        for ( size_t i = 0; i < sizeof( interesting_ops ) / sizeof( interesting_ops[0] ); i++ )
        {
            int op = interesting_ops[i].op;
            int supported = op <= probe.last_op && ( probe.ops[op].flags & PROBE_IO_URING_OP_SUPPORTED );
            printf( "  %-32s %s\n", interesting_ops[i].name, supported ? "YES" : "NO" );
        }
    }

    /*
        Provided buffer rings: a register op, not an opcode, so attempt a
        real registration. 4096 covers a page on every target we ship to.
    */
    void * ring_mem = mmap( NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if ( ring_mem != MAP_FAILED )
    {
        struct probe_io_uring_buf_reg reg;
        memset( &reg, 0, sizeof( reg ) );
        reg.ring_addr = (uint64_t) (uintptr_t) ring_mem;
        reg.ring_entries = 8;
        reg.bgid = 0;
        result = (int) syscall( __NR_io_uring_register, ring_fd, PROBE_IORING_REGISTER_PBUF_RING, &reg, 1 );
        if ( result < 0 )
            printf( "  %-32s NO (%s)\n", "IORING_REGISTER_PBUF_RING", strerror( errno ) );
        else
        {
            printf( "  %-32s YES\n", "IORING_REGISTER_PBUF_RING" );
            syscall( __NR_io_uring_register, ring_fd, PROBE_IORING_UNREGISTER_PBUF_RING, &reg, 1 );
        }
        munmap( ring_mem, 4096 );
    }

    close( ring_fd );

    /* SQPOLL wants privileges on older kernels: attempt it, record the answer */
    memset( &params, 0, sizeof( params ) );
    params.flags = PROBE_IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 100;
    int sqpoll_fd = (int) syscall( __NR_io_uring_setup, 4, &params );
    if ( sqpoll_fd < 0 )
        printf( "  %-32s NO (%s)\n", "IORING_SETUP_SQPOLL", strerror( errno ) );
    else
    {
        printf( "  %-32s YES\n", "IORING_SETUP_SQPOLL" );
        close( sqpoll_fd );
    }

    /*
        Multishot receive (IORING_RECV_MULTISHOT) is a request flag, not an
        opcode; it has no probe surface. Kernel >= 6.0 has it. Version-
        inferred only -- the harness proves it by running.
    */
    printf( "  %-32s (no probe surface; kernel >= 6.0 has it)\n", "IORING_RECV_MULTISHOT" );
}

static void probe_udp_gso_gro( void )
{
    printf( "udp socket options:\n" );

    int fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( fd < 0 )
    {
        printf( "  %-32s socket() failed (%s)\n", "UDP_SEGMENT", strerror( errno ) );
        return;
    }

    int value = 1200;
    if ( setsockopt( fd, IPPROTO_UDP, UDP_SEGMENT, &value, sizeof( value ) ) == 0 )
        printf( "  %-32s YES\n", "UDP_SEGMENT (GSO)" );
    else
        printf( "  %-32s NO (%s)\n", "UDP_SEGMENT (GSO)", strerror( errno ) );

    value = 1;
    if ( setsockopt( fd, IPPROTO_UDP, UDP_GRO, &value, sizeof( value ) ) == 0 )
        printf( "  %-32s YES\n", "UDP_GRO" );
    else
        printf( "  %-32s NO (%s)\n", "UDP_GRO", strerror( errno ) );

    close( fd );
}

static void probe_interface_features( void )
{
    printf( "interface offloads (ethtool features):\n" );

    struct if_nameindex * interfaces = if_nameindex();
    if ( !interfaces )
    {
        printf( "  if_nameindex failed (%s)\n", strerror( errno ) );
        return;
    }

    int fd = socket( AF_INET, SOCK_DGRAM, 0 );

    /* kernel feature-string names (ETH_SS_FEATURES), not ethtool display names */
    static const char * wanted[] = { "tx-udp-segmentation", "rx-gro", "rx-gro-hw", "rx-udp-gro-forwarding" };

    for ( struct if_nameindex * ifc = interfaces; ifc->if_index != 0; ifc++ )
    {
        struct ifreq ifr;
        memset( &ifr, 0, sizeof( ifr ) );
        strncpy( ifr.ifr_name, ifc->if_name, IFNAMSIZ - 1 );

        struct
        {
            struct ethtool_sset_info hdr;
            uint32_t buf[1];
        } sset_info;
        memset( &sset_info, 0, sizeof( sset_info ) );
        sset_info.hdr.cmd = ETHTOOL_GSSET_INFO;
        sset_info.hdr.sset_mask = 1ULL << ETH_SS_FEATURES;
        ifr.ifr_data = (void *) &sset_info;
        if ( ioctl( fd, SIOCETHTOOL, &ifr ) < 0 )
        {
            printf( "  %-16s ETHTOOL_GSSET_INFO failed (%s)\n", ifc->if_name, strerror( errno ) );
            continue;
        }
        uint32_t count = sset_info.buf[0];

        size_t strings_bytes = sizeof( struct ethtool_gstrings ) + (size_t) count * ETH_GSTRING_LEN;
        if ( ( ( strings_bytes - sizeof( struct ethtool_gstrings ) ) / ETH_GSTRING_LEN ) != (size_t) count )
        {
            printf( "  %-16s string table size overflow\n", ifc->if_name );
            continue;
        }
        struct ethtool_gstrings * strings = calloc( 1, strings_bytes );
        if ( !strings )
        {
            printf( "  %-16s failed to allocate string table (%s)\n", ifc->if_name, strerror( errno ) );
            continue;
        }
        strings->cmd = ETHTOOL_GSTRINGS;
        strings->string_set = ETH_SS_FEATURES;
        strings->len = count;
        ifr.ifr_data = (void *) strings;
        if ( ioctl( fd, SIOCETHTOOL, &ifr ) < 0 )
        {
            printf( "  %-16s ETHTOOL_GSTRINGS failed (%s)\n", ifc->if_name, strerror( errno ) );
            free( strings );
            continue;
        }

        size_t feature_words = ( (size_t) count + 31 ) / 32;
        if ( feature_words > ( SIZE_MAX - sizeof( struct ethtool_gfeatures ) ) / sizeof( struct ethtool_get_features_block ) )
        {
            printf( "  %-16s feature table size overflow\n", ifc->if_name );
            free( strings );
            continue;
        }
        struct ethtool_gfeatures * features = calloc( 1, sizeof( struct ethtool_gfeatures ) + feature_words * sizeof( struct ethtool_get_features_block ) );
        if ( !features )
        {
            printf( "  %-16s failed to allocate feature table (%s)\n", ifc->if_name, strerror( errno ) );
            free( strings );
            continue;
        }
        features->cmd = ETHTOOL_GFEATURES;
        features->size = (uint32_t) feature_words;
        ifr.ifr_data = (void *) features;
        if ( ioctl( fd, SIOCETHTOOL, &ifr ) < 0 )
        {
            printf( "  %-16s ETHTOOL_GFEATURES failed (%s)\n", ifc->if_name, strerror( errno ) );
            free( strings );
            free( features );
            continue;
        }

        printf( "  %s:\n", ifc->if_name );
        for ( size_t w = 0; w < sizeof( wanted ) / sizeof( wanted[0] ); w++ )
        {
            int found = 0;
            for ( uint32_t i = 0; i < count; i++ )
            {
                const char * name = (const char *) strings->data + i * ETH_GSTRING_LEN;
                if ( strncmp( name, wanted[w], ETH_GSTRING_LEN ) == 0 )
                {
                    int active = ( features->features[i / 32].active & ( 1U << ( i % 32 ) ) ) != 0;
                    printf( "    %-30s %s\n", wanted[w], active ? "on" : "off" );
                    found = 1;
                    break;
                }
            }
            if ( !found )
                printf( "    %-30s (not present)\n", wanted[w] );
        }

        free( strings );
        free( features );
    }

    close( fd );
    if_freenameindex( interfaces );
}

static void probe_reuseport_steering( void )
{
    printf( "reuseport steering:\n" );

    int fd = socket( AF_INET, SOCK_DGRAM, 0 );
    int one = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) );

    struct sockaddr_in addr;
    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    addr.sin_port = 0;
    if ( bind( fd, (struct sockaddr *) &addr, sizeof( addr ) ) < 0 )
    {
        printf( "  %-32s bind failed (%s)\n", "SO_ATTACH_REUSEPORT_CBPF", strerror( errno ) );
        close( fd );
        return;
    }

    /* classic BPF: return 0 -> steer everything to socket index 0 */
    struct sock_filter cbpf_code[] = { { 0x06, 0, 0, 0 } };       /* BPF_RET | BPF_K, 0 */
    struct sock_fprog cbpf_prog = { 1, cbpf_code };
    if ( setsockopt( fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &cbpf_prog, sizeof( cbpf_prog ) ) == 0 )
        printf( "  %-32s YES\n", "SO_ATTACH_REUSEPORT_CBPF" );
    else
        printf( "  %-32s NO (%s)\n", "SO_ATTACH_REUSEPORT_CBPF", strerror( errno ) );

    /* eBPF: load an SK_REUSEPORT program (mov r0, 0; exit) then attach */
    struct bpf_insn ebpf_code[] = {
        { .code = 0xb7, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = 0 },   /* mov64 r0, 0 */
        { .code = 0x95, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = 0 },   /* exit */
    };
    union bpf_attr attr;
    memset( &attr, 0, sizeof( attr ) );
    attr.prog_type = BPF_PROG_TYPE_SK_REUSEPORT;
    attr.insn_cnt = 2;
    attr.insns = (uint64_t) (uintptr_t) ebpf_code;
    attr.license = (uint64_t) ( uintptr_t ) "GPL";
    int prog_fd = (int) syscall( __NR_bpf, BPF_PROG_LOAD, &attr, sizeof( attr ) );
    if ( prog_fd < 0 )
    {
        printf( "  %-32s NO: prog load refused (%s)\n", "SO_ATTACH_REUSEPORT_EBPF", strerror( errno ) );
    }
    else
    {
        if ( setsockopt( fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_EBPF, &prog_fd, sizeof( prog_fd ) ) == 0 )
            printf( "  %-32s YES\n", "SO_ATTACH_REUSEPORT_EBPF" );
        else
            printf( "  %-32s NO: attach refused (%s)\n", "SO_ATTACH_REUSEPORT_EBPF", strerror( errno ) );
        close( prog_fd );
    }

    close( fd );
}

int main( void )
{
    printf( "rocketnet transport probe (#81)\n" );
    printf( "============================\n" );
    probe_kernel();
    probe_io_uring();
    probe_udp_gso_gro();
    probe_interface_features();
    probe_reuseport_steering();
    return 0;
}
