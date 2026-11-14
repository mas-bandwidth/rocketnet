/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The harden_load reconcile's chain (docs/HARDEN_LOAD.md, "The verdict"): one
// direction's packet equation over named counters, judged. Pure arithmetic over
// the terms the roles printed, so the unit suite can feed it planted rows
// (tests/test_unit_harden_chain.cpp). No time, no sockets, no files.

#pragma once

#ifndef ROCKETNET_TESTS_LOAD_HARDEN_CHAIN_H
#define ROCKETNET_TESTS_LOAD_HARDEN_CHAIN_H

#include <stdint.h>

// one direction's chain, every term named: what the sending library counted,
// what the sending namespace's kernel took or refused, what the qdisc and the
// device dropped on the way, what the receiving namespace's kernel saw, and
// what the receiving library took

struct harden_chain_t
{
    int64_t library_sent = 0, kernel_out = 0, kernel_sndbuf = 0, qdisc_sent = 0, qdisc_dropped = 0, device_dropped = 0;
    int64_t arrived_in = 0, arrived_inerrors = 0, arrived_noports = 0, arrived_ipdiscards = 0, arrived_device_dropped = 0;
    // of arrived_noports, the datagrams the receiving host refused at a closed port that came from a host outside the
    // cell (not the other end's test address): an internet scanner on the machine's public address, for one. The
    // kernel's NoPorts is the whole host's, so these are in it; they are no library's datagrams. Counted by the
    // receiving host's stray counter (scripts/harden-pair installs it; harden_load --stray-table reads it at the
    // cut). 0 where no counter was read. Never more than arrived_noports
    int64_t arrived_noports_strays = 0;
    int64_t library_received = 0;
    // refused by the receiving server before the library counts the packet
    // received: harden_refused_before_received below
    int64_t library_refused_before_received = 0;
    const char * refused_source = "the receive workers' refusals + the receive path's size drops"; // printed with the chain
    // the receiving device's discard delta includes the stop window (a Windows adapter's counter): up to that many
    // more packets than the library lost are named, so the library gap may be that far below zero
    int64_t device_overlap = 0;
    // whether each end's OS exposes the kernel, qdisc and device counters the step lines read (Linux does; Windows and
    // macOS read none): a step line that needs a counter its OS does not expose is printed "not available on this OS"
    // and is not part of the verdict. The verdict is the library gap against the drops that were named
    bool sender_counters = true;
    bool receiver_counters = true;
};

struct harden_chain_result_t
{
    int64_t cell_noports;  // arrived_noports less the strays: the no-port drops that can be the cell's
    bool strays_valid;     // 0 <= strays <= arrived_noports; a stray count above the host's NoPorts is a broken counter
    int64_t named_drops;
    int64_t library_gap;
    int64_t send_gap;
    int64_t path_gap;
    int64_t receive_gap;
    int64_t gso_slack;
    bool ok;
};

// The datagrams the receiving server refused before its library counted them
// received, each part counted where the refusal happened, never inferred from
// the received count. PACKETS_RECEIVED less the received count would cancel
// the received count out of the chain, which then judged PACKETS_RECEIVED
// alone: a datagram the transport counted and the library's received count
// missed would read as refused:
//  - worker_refused: the receive workers' prefilter refusals (off the main
//    thread), from the workers' own per-reason totals
//    (rocketnet_server_transport_worker_refused_for_testing, printed by the
//    server at its cut as c2s_worker_refused). Main's own prefilter
//    refusals are inside the library's received count. Every sender's
//    datagrams are in it, the hostile role's included (the chain's
//    library_sent counts hostile.sent).
//  - outside_size_drops: the receive path's own size drops (io_uring's
//    URING_RECEIVE_DROPPED_SIZE, IOCP_ and RIO_RECEIVE_TOO_SHORT/TOO_LARGE),
//    which the transport folds into PREFILTER_TOO_SHORT/TOO_LARGE outside
//    PACKETS_RECEIVED. The other transports count their size drops inside it.
//    On xdp, its kernel program's drops (XDP_KERNEL_DROPPED_*) and the frames
//    it dropped before reading a datagram (XDP_RECEIVE_FRAME_DROPPED) join
//    this part (harden_refused_from_terms).
// PREFILTER_TOO_SHORT/TOO_LARGE are never added whole: they mix both parts.
static inline int64_t harden_refused_before_received( int64_t worker_refused, int64_t outside_size_drops )
{
    return worker_refused + outside_size_drops;
}

// the same term from the reconcile's printed terms: get( key ) is a term's
// value, 0 for a term no role printed (a counter at 0 is not printed). One
// transport serves the cell, so the size drop counters of the others read 0
template <typename Get> static inline int64_t harden_refused_from_terms( const Get & get )
{
    int64_t outside_size_drops = get( "server.c2s_transport_URING_RECEIVE_DROPPED_SIZE" ) + get( "server.c2s_transport_IOCP_RECEIVE_TOO_SHORT" ) + get( "server.c2s_transport_IOCP_RECEIVE_TOO_LARGE" ) + get( "server.c2s_transport_RIO_RECEIVE_TOO_SHORT" ) + get( "server.c2s_transport_RIO_RECEIVE_TOO_LARGE" );
    // xdp has no receive workers' totals (no receive_drop_totals in its
    // driver table): its kernel program's drops and the frames it dropped
    // before reading a datagram are counted by its own rows, outside
    // PACKETS_RECEIVED, so they are named here
    static const char * const xdp_drops[] = {
        "server.c2s_transport_XDP_KERNEL_DROPPED_TOO_SMALL",
        "server.c2s_transport_XDP_KERNEL_DROPPED_TOO_LARGE",
        "server.c2s_transport_XDP_KERNEL_DROPPED_WRONG_FILTER",
        "server.c2s_transport_XDP_KERNEL_DROPPED_INVALID_TYPE",
        "server.c2s_transport_XDP_KERNEL_DROPPED_WRONG_EPOCH",
        "server.c2s_transport_XDP_KERNEL_DROPPED_REPLAYED",
        "server.c2s_transport_XDP_KERNEL_DROPPED_FIRST_FRAGMENT",
        "server.c2s_transport_XDP_KERNEL_DROPPED_NO_SOCKET",
        "server.c2s_transport_XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION",
        "server.c2s_transport_XDP_KERNEL_DROPPED_NO_SESSION",
        "server.c2s_transport_XDP_RECEIVE_FRAME_DROPPED",
    };
    for ( const char * name : xdp_drops )
    {
        outside_size_drops += get( name );
    }
    return harden_refused_before_received( get( "server.c2s_worker_refused" ), outside_size_drops );
}

// the clients to server chain, every term from the roles' printed terms: the
// reconcile's (tests/load/harden_load.cpp) and the unit suite's one path
template <typename Get> static inline harden_chain_t harden_chain_up_from_terms( const Get & get )
{
    harden_chain_t up;
    up.library_sent = get( "clients.c2s_packets_sent" ) + get( "hostile.sent" );
    up.kernel_out = get( "clients.c2s_udp_out" );
    up.kernel_sndbuf = get( "clients.c2s_udp_sndbuf" );
    up.qdisc_sent = get( "clients.c2s_tc_sent" );
    up.qdisc_dropped = get( "clients.c2s_tc_dropped" );
    up.device_dropped = get( "clients.c2s_dev_tx_dropped" );
    up.arrived_in = get( "server.c2s_udp_in" );
    up.arrived_inerrors = get( "server.c2s_udp_inerrors" );
    up.arrived_noports = get( "server.c2s_udp_noports" );
    up.arrived_noports_strays = get( "server.c2s_udp_noports_strays" );
    up.arrived_ipdiscards = get( "server.c2s_ip_indiscards" );
    up.arrived_device_dropped = get( "server.c2s_dev_rx_dropped" );
    up.sender_counters = get( "clients.c2s_kernel_read" ) != 0;
    up.receiver_counters = get( "server.c2s_kernel_read" ) != 0;
    up.device_overlap = get( "server.c2s_dev_rx_inexact" ) != 0 && up.arrived_device_dropped > 0 ? up.arrived_device_dropped : 0;
    up.library_received = get( "server.c2s_packets_received" );
    // the refusals before the library's received count, both parts on every
    // transport and with or without a hostile role, each from its own count
    up.library_refused_before_received = harden_refused_from_terms( get );
    up.refused_source = "c2s_worker_refused (the receive workers' own totals) + the receive path's size drops (URING_RECEIVE_DROPPED_SIZE, IOCP_/RIO_RECEIVE_TOO_SHORT/TOO_LARGE) + xdp's drops (XDP_KERNEL_DROPPED_*, XDP_RECEIVE_FRAME_DROPPED)";
    return up;
}

// the transport's count against the library's: every datagram the transport
// counts in PACKETS_RECEIVED is a worker refusal (folded from the workers'
// totals) or one main took (main counts it there and in the received count).
// Judged with the up chain, which no longer reads PACKETS_RECEIVED: not zero
// names a datagram one side counted and the other did not
template <typename Get> static inline int64_t harden_handoff_gap_from_terms( const Get & get )
{
    return get( "server.c2s_transport_PACKETS_RECEIVED" ) - ( get( "server.c2s_packets_received" ) + get( "server.c2s_worker_refused" ) );
}

// the up direction's verdict: its chain, and the handoff balanced
static inline bool harden_up_ok( bool chain_ok, int64_t handoff_gap )
{
    return chain_ok && handoff_gap == 0;
}

static inline harden_chain_result_t harden_chain_judge( const harden_chain_t & c, int duplicates_expected )
{
    harden_chain_result_t r;
    r.strays_valid = c.arrived_noports_strays >= 0 && c.arrived_noports_strays <= c.arrived_noports;
    // a stray term that cannot be right is not applied: the chain stands on the whole NoPorts, and fails
    r.cell_noports = r.strays_valid ? c.arrived_noports - c.arrived_noports_strays : c.arrived_noports;
    r.send_gap = c.library_sent - ( c.kernel_out + c.kernel_sndbuf );
    r.path_gap = c.kernel_out - ( c.arrived_in + c.arrived_inerrors + r.cell_noports + c.arrived_ipdiscards + c.qdisc_dropped + c.device_dropped + c.arrived_device_dropped );
    r.receive_gap = c.arrived_in - ( c.library_received + c.library_refused_before_received );
    // netem's duplicates are not counted anywhere: they are the path's
    // excess of arrivals, and must be zero unless the row duplicates.
    // The judged equation is the libraries': every packet the sender's
    // library counted sent is counted received by the receiver's library or
    // dropped by a named counter. The kernel's UDP counts step by step are
    // printed beside it: they count an skb, so a GSO send of N datagrams is
    // one in them (and in qdisc drops), and a step gap there names where a
    // library gap went
    r.named_drops = c.kernel_sndbuf + c.arrived_inerrors + r.cell_noports + c.arrived_ipdiscards + c.qdisc_dropped + c.device_dropped + c.arrived_device_dropped + c.library_refused_before_received;
    r.library_gap = c.library_sent - ( c.library_received + r.named_drops );
    // the kernel's drops count skbs, and a GSO skb carries several datagrams:
    // a qdisc drop, a send buffer error, or a GSO skb arriving whole (a veth
    // and a UDP_GRO socket keep it whole) at a closed port or a full socket
    // names one where the libraries lost several. The datagrams riding in a
    // GSO skb beyond its first are the send_gap (OutDatagrams counts the skb),
    // so while any of those drops, a gap of at most that many is a dropped
    // GSO skb's later segments; the gap is never below zero
    const int64_t skb_drops = c.qdisc_dropped + c.kernel_sndbuf + r.cell_noports + c.arrived_inerrors;
    r.gso_slack = skb_drops > 0 && r.send_gap > 0 ? r.send_gap : 0;
    const bool gap_ok = duplicates_expected ? r.library_gap <= 0 : r.library_gap >= -c.device_overlap && r.library_gap <= r.gso_slack;
    r.ok = r.strays_valid && gap_ok;
    return r;
}

#endif // ROCKETNET_TESTS_LOAD_HARDEN_CHAIN_H
