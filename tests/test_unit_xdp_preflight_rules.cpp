/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The xdp pre-flight's machine conditions, each over a fake /proc and /sys
// tree this test writes in its own working directory (the pre-flight reads
// under a root; production's root is ""): the kernel command line (B1), the
// queue IRQs (R4), the real-time throttle (R8), and the queue vectors read
// again after the attach. No root, no NIC, no socket, no clock: the files are
// the machine. The VLAN tag rule (R2) is judged from a given flags read.
// scripts/rocketnet-provision's selftest drives the script's side of the
// same rules.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_xdp_frame.h"
#include "../sdk/rocketnet_server_socket.h"

#if ROCKETNET_XDP

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ftw.h>
#include <linux/ethtool.h>

// a fake root under the working directory, emptied first

static int preflight_remove_entry( const char * path, const struct stat *, int, struct FTW * )
{
    return remove( path );
}

static void preflight_root_clear( const char * root )
{
    struct stat status;
    if ( stat( root, &status ) == 0 )
    {
        rocketnet_unit_check( nftw( root, preflight_remove_entry, 16, FTW_DEPTH | FTW_PHYS ) == 0 );
    }
}

static void preflight_root_write( const char * root, const char * relative, const char * content )
{
    char path[1024];
    rocketnet_unit_check( snprintf( path, sizeof( path ), "%s%s", root, relative ) < (int) sizeof( path ) );

    // each directory on the way

    for ( char * slash = strchr( path + 1, '/' ); slash; slash = strchr( slash + 1, '/' ) )
    {
        *slash = '\0';
        rocketnet_unit_check( mkdir( path, 0700 ) == 0 || errno == EEXIST );
        *slash = '/';
    }

    FILE * file = fopen( path, "w" );
    rocketnet_unit_check( file );
    rocketnet_unit_check( fputs( content, file ) >= 0 );
    rocketnet_unit_check( fclose( file ) == 0 );
}

static void preflight_cmdline( const char * root, const char * cmdline, const char * isolated )
{
    preflight_root_clear( root );
    preflight_root_write( root, "/proc/cmdline", cmdline );
    if ( isolated )
    {
        preflight_root_write( root, "/sys/devices/system/cpu/isolated", isolated );
    }
}

// B1: irqaffinity= takes any cpu list that keeps interrupts off the isolated
// cpus, so num_os_cpus 2 (isolcpus=2-3) may keep irqaffinity=0-1

static void test_xdp_transport_preflight_irqaffinity_list()
{
    const char * root = "xdp_preflight_b1";

    preflight_cmdline( root, "ro quiet irqaffinity=0-1 isolcpus=2-3 nohz_full=2-3\n", "2-3\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_boot_line( root ) );

    preflight_cmdline( root, "ro irqaffinity=0 isolcpus=1-2 nohz_full=1-2 nosmt\n", "1-2\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_boot_line( root ) );

    preflight_cmdline( root, "irqaffinity=0,4 isolcpus=1-3 nohz_full=1-3\n", "1-3\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_boot_line( root ) );

    // a cpu of the list in the isolated set: the interrupts land on a worker

    preflight_cmdline( root, "irqaffinity=0-2 isolcpus=2-3 nohz_full=2-3\n", "2-3\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );

    // the rule compares ranges, never walks a range's cpus: the widest list
    // there is answers at once, and the cpu named is the lowest shared one

    int isolated_cpu = -1;
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=0-2147483647", "5-7,9", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_ISOLATED );
    rocketnet_unit_check( isolated_cpu == 5 );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=0-4,8,10-2147483647", "5-7,9", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=0-4,8-12", "5-7,9", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_ISOLATED );
    rocketnet_unit_check( isolated_cpu == 9 );

    // the forms the script's cpu_list_has refuses are refused here too (its
    // selftest holds the same cases): a trailing comma, an empty entry, a
    // leading comma, a blank after a comma
    {
        const char * malformed[] = { "irqaffinity=0,", "irqaffinity=0,\n", "irqaffinity=0,,1", "irqaffinity=,0", "irqaffinity=0, 1", "irqaffinity=-1", "irqaffinity=2147483648" };
        for ( size_t i = 0; i < sizeof( malformed ) / sizeof( malformed[0] ); i++ )
        {
            rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( malformed[i], "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MALFORMED );
        }
    }
    preflight_cmdline( root, "irqaffinity=0, isolcpus=1-2 nohz_full=1-2\n", "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );

    // the old exact token, a leading zero, and cpu 0 itself isolated
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "ro irqaffinity=0 quiet", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=00", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "ro\tirqaffinity=0", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "ro\tirqaffinity=1", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_ISOLATED );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=0", "0-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_ISOLATED );
    rocketnet_unit_check( isolated_cpu == 0 );

    // an isolated set that is not a cpu list judges the form only
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=1", "1-3,", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );

    // the token twice: the kernel applies each in order, so the last one is judged
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=1 ro irqaffinity=0", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=0 ro irqaffinity=2", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_ISOLATED );
    rocketnet_unit_check( isolated_cpu == 2 );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=0 xirqaffinity=2", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );
    rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( "irqaffinity=0 irqaffinity=0,", "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MALFORMED );

    // the value's bound: 255 characters is a value, 256 is not
    {
        char line[300] = "irqaffinity=0";
        size_t length = strlen( line );
        while ( length - strlen( "irqaffinity=" ) < 255 )
        {
            memcpy( line + length, ",0", 2 );
            length += 2;
        }
        line[strlen( "irqaffinity=" ) + 255] = '\0';
        rocketnet_unit_check( strlen( line ) - strlen( "irqaffinity=" ) == 255 );
        rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( line, "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MET );
        line[strlen( "irqaffinity=" ) + 255] = ',';
        line[strlen( "irqaffinity=" ) + 256] = '0';
        line[strlen( "irqaffinity=" ) + 257] = '\0';
        rocketnet_unit_check( rocketnet_xdp_cmdline_irqaffinity( line, "1-3", &isolated_cpu ) == ROCKETNET_XDP_IRQAFFINITY_MALFORMED );
    }

    // absent, not left-bounded, empty, malformed

    preflight_cmdline( root, "isolcpus=1-2 nohz_full=1-2\n", "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );
    preflight_cmdline( root, "xirqaffinity=0 isolcpus=1-2 nohz_full=1-2\n", "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );
    preflight_cmdline( root, "irqaffinity= isolcpus=1-2 nohz_full=1-2\n", "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );
    preflight_cmdline( root, "irqaffinity=0x isolcpus=1-2 nohz_full=1-2\n", "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );
    preflight_cmdline( root, "irqaffinity=3-1 isolcpus=1-2 nohz_full=1-2\n", "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );

    // the other two tokens are still required

    preflight_cmdline( root, "irqaffinity=0 nohz_full=1-2\n", "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );

    // unreadable

    preflight_root_clear( root );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_boot_line( root ) );

    preflight_root_clear( root );
}

// R8: a kernel before 6.12, or a later one built with CONFIG_RT_GROUP_SCHED,
// stops each spinning SCHED_RR worker unless sched_rt_runtime_us is -1

static void preflight_rt( const char * root, const char * release, const char * runtime, const char * config )
{
    preflight_root_clear( root );
    preflight_root_write( root, "/proc/sys/kernel/osrelease", release );
    if ( runtime )
    {
        preflight_root_write( root, "/proc/sys/kernel/sched_rt_runtime_us", runtime );
    }
    if ( config )
    {
        char relative[256];
        char name[128];
        snprintf( name, sizeof( name ), "%s", release );
        char * newline = strchr( name, '\n' );
        if ( newline )
        {
            *newline = '\0';
        }
        snprintf( relative, sizeof( relative ), "/boot/config-%s", name );
        preflight_root_write( root, relative, config );
    }
}

static void test_xdp_transport_preflight_rt_throttle()
{
    const char * root = "xdp_preflight_r8";

    // 6.8 (Ubuntu 24.04): the stock 950000 throttles; -1 meets R8

    preflight_rt( root, "6.8.0-45-generic\n", "950000\n", NULL );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );
    preflight_rt( root, "6.8.0-45-generic\n", "-1\n", NULL );
    rocketnet_unit_check( rocketnet_server_socket_preflight_rt_throttle( root ) );
    preflight_rt( root, "5.15.0-100-generic\n", "950000\n", "# CONFIG_RT_GROUP_SCHED is not set\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );
    preflight_rt( root, "6.11.0-1-generic\n", "950000\n", NULL );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );

    // the sysctl unreadable on a throttling kernel: unmet

    preflight_rt( root, "6.8.0-45-generic\n", NULL, NULL );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );

    // 6.12 and later without CONFIG_RT_GROUP_SCHED (bench-xdp-8's 7.0.0-43-generic
    // reads 950000 and is not throttled)

    preflight_rt( root, "7.0.0-43-generic\n", "950000\n", "CONFIG_SMP=y\n# CONFIG_RT_GROUP_SCHED is not set\nCONFIG_NO_HZ_FULL=y\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_rt_throttle( root ) );
    preflight_rt( root, "6.12.0\n", "950000\n", "CONFIG_RT_GROUP_SCHED=n\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_rt_throttle( root ) );

    // ... and with it: throttled until -1

    preflight_rt( root, "7.0.0-43-generic\n", "950000\n", "CONFIG_SMP=y\nCONFIG_RT_GROUP_SCHED=y\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );
    preflight_rt( root, "7.0.0-43-generic\n", "-1\n", "CONFIG_RT_GROUP_SCHED=y\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_rt_throttle( root ) );

    // a config that opens but cannot be read (a directory at the path) is
    // unknown, never "the option is absent": unmet at 950000, met at -1

    {
        char path[256];
        preflight_rt( root, "7.0.0-43-generic\n", "950000\n", NULL );
        snprintf( path, sizeof( path ), "%s/boot", root );
        rocketnet_unit_check( mkdir( path, 0755 ) == 0 );
        snprintf( path, sizeof( path ), "%s/boot/config-7.0.0-43-generic", root );
        rocketnet_unit_check( mkdir( path, 0755 ) == 0 );
        rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );
        preflight_root_write( root, "/proc/sys/kernel/sched_rt_runtime_us", "-1\n" );
        rocketnet_unit_check( rocketnet_server_socket_preflight_rt_throttle( root ) );
    }

    // 6.12 or later with no build config to read: it cannot be told, so
    // it is unmet unless the sysctl already reads -1

    preflight_rt( root, "7.0.0-43-generic\n", "950000\n", NULL );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );
    preflight_rt( root, "7.0.0-43-generic\n", "-1\n", NULL );
    rocketnet_unit_check( rocketnet_server_socket_preflight_rt_throttle( root ) );

    // a release that does not parse is taken as throttling

    preflight_rt( root, "weird\n", "950000\n", NULL );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );
    preflight_rt( root, "7\n", "950000\n", "# CONFIG_RT_GROUP_SCHED is not set\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );

    // the release unreadable: unmet

    preflight_root_clear( root );
    preflight_root_write( root, "/proc/sys/kernel/sched_rt_runtime_us", "950000\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_rt_throttle( root ) );

    preflight_root_clear( root );
}

// R4: queue vector <interface>-TxRx-k on the cpu xdp thread k pins to,
// num_os_cpus + k; any other vector of the interface off those cpus

static void preflight_irq( const char * root, int irq, const char * affinity )
{
    char relative[128];
    snprintf( relative, sizeof( relative ), "/proc/irq/%d/smp_affinity_list", irq );
    preflight_root_write( root, relative, affinity );
}

static const char * preflight_i40e_two =
    "            CPU0       CPU1       CPU2       CPU3       CPU4\n"
    "  40:          0          0          0          0          0  IR-PCI-MSIX-0000:05:00.0    1-edge      i40e-eth0-TxRx-0\n"
    "  41:          0          0          0          0          0  IR-PCI-MSIX-0000:05:00.0    2-edge      i40e-eth0-TxRx-1\n"
    "  42:          0          0          0          0          0  IR-PCI-MSIX-0000:05:00.1    1-edge      i40e-eth1-TxRx-0\n";

static void test_xdp_transport_preflight_irq_pairs_queue_with_worker()
{
    const char * root = "xdp_preflight_r4";

    // num_os_cpus 2: thread 0 on cpu 2, thread 1 on cpu 3. The vectors on
    // the first two isolated cpus (1 and 2) are each on an isolated cpu,
    // once, but neither is on its own worker's cpu: unmet

    preflight_root_clear( root );
    preflight_root_write( root, "/sys/devices/system/cpu/isolated", "1-4\n" );
    preflight_root_write( root, "/proc/interrupts", preflight_i40e_two );
    preflight_irq( root, 40, "1\n" );
    preflight_irq( root, 41, "2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 2 ) );

    // paired: met

    preflight_irq( root, 40, "2\n" );
    preflight_irq( root, 41, "3\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 2 ) );

    // swapped: each queue's interrupt on the other worker

    preflight_irq( root, 40, "3\n" );
    preflight_irq( root, 41, "2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 2 ) );

    // the default num_os_cpus (0 reads as 1): threads on 1 and 2

    preflight_irq( root, 40, "1\n" );
    preflight_irq( root, 41, "2\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 0 ) );
    rocketnet_unit_check( rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );

    // a queue vector past the server's queues

    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 1, 1 ) );

    // a range is never a pin

    preflight_irq( root, 41, "1-2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );

    // unreadable affinity, unreadable table

    preflight_root_clear( root );
    preflight_root_write( root, "/sys/devices/system/cpu/isolated", "1-4\n" );
    preflight_root_write( root, "/proc/interrupts", preflight_i40e_two );
    preflight_irq( root, 40, "1\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );
    preflight_root_clear( root );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );

    preflight_root_clear( root );
}

// the vector shape an ixgbe port shows after repin with XDP attached
// (captured on hulk enp9s0f0, the script selftest's capture): queue vectors
// on the workers, tx-only and misc vectors moved to the housekeeping cpu 0.
// Only the queue vectors are pinned; the rest must stay off the workers.

static const char * preflight_ixgbe_attached =
    " 179:  0 0 IR-PCI-MSI 4718592-edge      enp9s0f0-TxRx-0\n"
    " 180:  0 0 IR-PCI-MSI 4718593-edge      enp9s0f0-TxRx-1\n"
    " 186:  0 0 IR-PCI-MSI 4718599-edge      enp9s0f0-tx-2\n"
    " 210:  0 0 IR-PCI-MSI 4718623-edge      enp9s0f0-tx-31\n"
    " 211:  0 0 IR-PCI-MSI 4718624-edge      enp9s0f0\n";

static void test_xdp_transport_preflight_irq_other_vectors_off_workers()
{
    const char * root = "xdp_preflight_r4_ixgbe";

    preflight_root_clear( root );
    preflight_root_write( root, "/sys/devices/system/cpu/isolated", "1-15\n" );
    preflight_root_write( root, "/proc/interrupts", preflight_ixgbe_attached );
    preflight_irq( root, 179, "1\n" );
    preflight_irq( root, 180, "2\n" );
    preflight_irq( root, 186, "0\n" );
    preflight_irq( root, 210, "0\n" );
    preflight_irq( root, 211, "0\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_irqs( root, "enp9s0f0", 2, 1 ) );

    // a vector that is not a queue's, on a non-worker isolated cpu or on a
    // housekeeping list: still off the workers

    preflight_irq( root, 211, "5\n" );
    preflight_irq( root, 186, "0,3-4\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_irqs( root, "enp9s0f0", 2, 1 ) );

    // the misc vector on a worker's cpu: unmet

    preflight_irq( root, 211, "2\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "enp9s0f0", 2, 1 ) );
    preflight_irq( root, 211, "0\n" );
    preflight_irq( root, 210, "0-15\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "enp9s0f0", 2, 1 ) );

    preflight_root_clear( root );
}

// a driver whose vectors carry no -TxRx-<k> name keeps the rule it had: each
// vector of the interface on its own isolated cpu

static void test_xdp_transport_preflight_irq_unnamed_queues_keep_isolated_rule()
{
    const char * root = "xdp_preflight_r4_ena";

    preflight_root_clear( root );
    preflight_root_write( root, "/sys/devices/system/cpu/isolated", "1-3\n" );
    preflight_root_write( root, "/proc/interrupts",
                          "  30:  0 0 PCI-MSI-edge      eth0-Tx-Rx-0\n"
                          "  31:  0 0 PCI-MSI-edge      eth0-Tx-Rx-1\n" );
    preflight_irq( root, 30, "1\n" );
    preflight_irq( root, 31, "2\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );
    preflight_irq( root, 31, "1\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );
    preflight_irq( root, 31, "0\n" );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );

    // no vector names the interface: R4 cannot be checked by name, and says so

    preflight_root_write( root, "/proc/interrupts", "  30:  0 0 PCI-MSI-edge      mlx5_comp0@pci:0000:03:00.0\n" );
    rocketnet_unit_check( rocketnet_server_socket_preflight_irqs( root, "eth0", 2, 1 ) );

    preflight_root_clear( root );
}

// after the attach: a driver that rebuilt its vectors on it (ixgbe) leaves
// them on cpu 0 under irqaffinity=0; create counts and names them

static void test_xdp_transport_attach_moved_queue_irqs_are_found()
{
    const char * root = "xdp_preflight_attach";

    preflight_root_clear( root );
    preflight_root_write( root, "/proc/interrupts", preflight_ixgbe_attached );
    preflight_irq( root, 179, "0\n" );
    preflight_irq( root, 180, "0\n" );
    preflight_irq( root, 186, "0\n" );
    preflight_irq( root, 210, "0\n" );
    preflight_irq( root, 211, "0\n" );

    char first[256];
    rocketnet_unit_check( rocketnet_server_socket_queue_irqs_off_worker( root, "enp9s0f0", 2, 1, first, sizeof( first ) ) == 2 );
    rocketnet_unit_check( strstr( first, "irq 179" ) != NULL );
    rocketnet_unit_check( strstr( first, "cpu 1" ) != NULL );

    // one back on its worker

    preflight_irq( root, 179, "1\n" );
    rocketnet_unit_check( rocketnet_server_socket_queue_irqs_off_worker( root, "enp9s0f0", 2, 1, first, sizeof( first ) ) == 1 );
    rocketnet_unit_check( strstr( first, "irq 180" ) != NULL );

    // all home, or a queue the server does not run: none

    preflight_irq( root, 180, "2\n" );
    rocketnet_unit_check( rocketnet_server_socket_queue_irqs_off_worker( root, "enp9s0f0", 2, 1, first, sizeof( first ) ) == 0 );
    rocketnet_unit_check( rocketnet_server_socket_queue_irqs_off_worker( root, "enp9s0f0", 1, 1, first, sizeof( first ) ) == 0 );

    // an unreadable affinity is off (it cannot be shown home)

    preflight_root_clear( root );
    preflight_root_write( root, "/proc/interrupts", preflight_ixgbe_attached );
    preflight_irq( root, 179, "1\n" );
    rocketnet_unit_check( rocketnet_server_socket_queue_irqs_off_worker( root, "enp9s0f0", 2, 1, first, sizeof( first ) ) == 1 );

    // no queue vectors to read: none

    preflight_root_clear( root );
    rocketnet_unit_check( rocketnet_server_socket_queue_irqs_off_worker( root, "enp9s0f0", 2, 1, first, sizeof( first ) ) == 0 );
}

// R2 on a VLAN: the filter reads the 802.1Q tag in the frame, so the parent
// NIC must hand it the tag (rx-vlan-offload off). The judge takes the
// ETHTOOL_GFLAGS read's outcome (0, or the errno it failed with) and flags

static void test_xdp_transport_preflight_vlan_tag_reaches_the_filter()
{
    // an untagged server: the flag is never read and never matters

    rocketnet_unit_check( rocketnet_server_socket_preflight_vlan_tag( "eth0", 0, 0, ETH_FLAG_RXVLAN ) );
    rocketnet_unit_check( rocketnet_server_socket_preflight_vlan_tag( "eth0", 0, ENODEV, 0 ) );

    // a VLAN server whose parent strips the tag on receive: unmet

    rocketnet_unit_check( !rocketnet_server_socket_preflight_vlan_tag( "eth0", 300, 0, ETH_FLAG_RXVLAN ) );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_vlan_tag( "eth0", 300, 0, ETH_FLAG_RXVLAN | ETH_FLAG_TXVLAN ) );

    // offload off: met, whatever else is on

    rocketnet_unit_check( rocketnet_server_socket_preflight_vlan_tag( "eth0", 300, 0, 0 ) );
    rocketnet_unit_check( rocketnet_server_socket_preflight_vlan_tag( "eth0", 300, 0, ETH_FLAG_TXVLAN ) );

    // a read that failed cannot show the tag arrives: unmet

    rocketnet_unit_check( !rocketnet_server_socket_preflight_vlan_tag( "eth0", 300, EOPNOTSUPP, 0 ) );
    rocketnet_unit_check( !rocketnet_server_socket_preflight_vlan_tag( "eth0", 300, ENODEV, 0 ) );
}

REGISTER_UNIT_TEST( test_xdp_transport_preflight_vlan_tag_reaches_the_filter );
REGISTER_UNIT_TEST( test_xdp_transport_preflight_irqaffinity_list );
REGISTER_UNIT_TEST( test_xdp_transport_preflight_rt_throttle );
REGISTER_UNIT_TEST( test_xdp_transport_preflight_irq_pairs_queue_with_worker );
REGISTER_UNIT_TEST( test_xdp_transport_preflight_irq_other_vectors_off_workers );
REGISTER_UNIT_TEST( test_xdp_transport_preflight_irq_unnamed_queues_keep_isolated_rule );
REGISTER_UNIT_TEST( test_xdp_transport_attach_moved_queue_irqs_are_found );

#endif // #if ROCKETNET_XDP
