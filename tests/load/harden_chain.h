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
    const char * refused_source = "transport PACKETS_RECEIVED - library received + the receive path's size drops"; // printed with the chain
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
// received, as two disjoint parts:
//  - transport_received - library_received: a worker's prefilter (off the
//    main thread) counts each refusal into the transport's PACKETS_RECEIVED
//    and its reason, and main's stats never see it. Main's own prefilter
//    counts into both, so its refusals are in neither part of the difference.
//    Every sender's datagrams are in it, the hostile role's included (the
//    chain's library_sent counts hostile.sent).
//  - outside_size_drops: the receive path's own size drops (io_uring's
//    URING_RECEIVE_DROPPED_SIZE, IOCP_ and RIO_RECEIVE_TOO_SHORT/TOO_LARGE),
//    which the transport folds into PREFILTER_TOO_SHORT/TOO_LARGE outside
//    PACKETS_RECEIVED. The other transports count their size drops inside it.
// PREFILTER_TOO_SHORT/TOO_LARGE are never added whole: they mix both parts.
// A difference below zero is not clamped: it is a counter defect, and the
// chain shows it red.
static inline int64_t harden_refused_before_received( int64_t transport_received, int64_t library_received, int64_t outside_size_drops )
{
    return ( transport_received - library_received ) + outside_size_drops;
}

// the same term from the reconcile's printed terms: get( key ) is a term's
// value, 0 for a term no role printed (a counter at 0 is not printed). One
// transport serves the cell, so the size drop counters of the others read 0
template <typename Get> static inline int64_t harden_refused_from_terms( const Get & get )
{
    const int64_t outside_size_drops = get( "server.c2s_transport_URING_RECEIVE_DROPPED_SIZE" ) + get( "server.c2s_transport_IOCP_RECEIVE_TOO_SHORT" ) + get( "server.c2s_transport_IOCP_RECEIVE_TOO_LARGE" ) + get( "server.c2s_transport_RIO_RECEIVE_TOO_SHORT" ) + get( "server.c2s_transport_RIO_RECEIVE_TOO_LARGE" );
    return harden_refused_before_received( get( "server.c2s_transport_PACKETS_RECEIVED" ), get( "server.c2s_packets_received" ), outside_size_drops );
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
