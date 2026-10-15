#!/usr/bin/env python3
#
# counters.py -- the hardware tier's judge (RUNBOOK.md, "The rows").
#
# RocketNet. Copyright 2026 Más Bandwidth LLC.
# Licensed under the GNU Affero General Public License v3.0
#
# Reads what run.sh captured at each window's edges and judges it. Nothing
# here reads a machine; it reads files, so it runs anywhere and its rules
# are the ones written below.
#
#   counters.py window <window-dir> --step <pps> --window <n> --queues <q>
#                      [--tol-abs N] [--tol-rel X] [--out windows.jsonl]
#   counters.py summary <windows.jsonl>
#
# A window directory holds, captured after the sender stopped and the
# receiver drained (no frame in flight):
#   rx-before.txt rx-after.txt   ethtool -S <receiver iface>
#   tx-before.txt tx-after.txt   ethtool -S <sender iface>
#   snap-before.txt snap-after.txt   the receiver's SNAP lines
#   done.txt                     the sender's DONE line
#
# THE RULES.
#
# Exact, in-process (a difference of one fails: it is an uncounted path,
# never noise):
#   E1  xdp_redirected == received + xsk_rx_dropped + xsk_rx_ring_full
#       (every frame the program redirected reached the transport or was
#       counted dropped by the socket)
#   E2  on a window whose NIC drop counters all read 0:
#       sent == received + xsk drops + the stats map's five drop counters
#   E3  received refusals == 0 and sender tx_invalid == 0 (the run's frames
#       are all well formed; any refusal is a finding, not load)
#
# Hardware, within tolerance (the NIC's own ethtool -S, read at the window
# edges, cannot be read atomically with the program's counters):
#   H1  sum of the receiver NIC's per-queue rx packets == the program's
#       verdict sum (xdp_redirected + the five drops), within tolerance
#   H2  the sender NIC's tx packets == sent, within tolerance
#   H3  on a window where the NIC drop counters move: sent ==
#       received + every counted drop, the NIC's included, within tolerance
#   tolerance = max(tol_abs, tol_rel * sent); default 32 frames or 1e-6
#
# Clean: zero NIC drops, zero socket drops, zero refusals on both ends.
# The knee is the highest step whose every window is clean and passes.

import argparse
import json
import re
import statistics
import sys

QUEUE_PACKETS = [
    re.compile(r'^rx[-_]queue[-_](\d+)[-_.]packets$'),   # ixgbe, ice: rx_queue_0_packets
    re.compile(r'^rx-(\d+)\.packets$'),                    # i40e: rx-0.packets
    re.compile(r'^rx(\d+)[-_.]packets$'),                  # rx0_packets
    re.compile(r'^queue_(\d+)_rx_packets$'),
]

TX_PACKETS = [
    re.compile(r'^tx_packets$'),
    re.compile(r'^port\.tx_unicast$'),
    re.compile(r'^tx_unicast$'),
]

# receive-side drop and error counters, by name, across drivers. Every
# nonzero delta is named in the window line, so a counter this list does
# not know is still visible in the raw files beside it.
DROP = re.compile(r'(drop|miss|no_buf|nobuf|no_dma|no_desc|discard|fifo|alloc_fail|crc|length_err|oversize|undersize|illegal|busy)')
NOT_RX = re.compile(r'(^|[._-])tx([._-]|$)|^tx|xdp_tx|xmit')


def parse_ethtool(path):
    values = {}
    with open(path) as f:
        for line in f:
            if ':' not in line:
                continue
            name, _, value = line.strip().rpartition(':')
            name = name.strip()
            value = value.strip()
            if not name or not re.fullmatch(r'-?\d+', value):
                continue
            values[name] = int(value)
    return values


def parse_kv(path, tag):
    with open(path) as f:
        for line in f:
            parts = line.split()
            if parts and parts[0] == tag:
                out = {}
                for part in parts[1:]:
                    key, _, value = part.partition('=')
                    out[key] = value
                return out
    raise SystemExit('counters.py: no %s line in %s' % (tag, path))


def delta(before, after):
    return {k: after[k] - before.get(k, 0) for k in after}


def per_queue(d, queues):
    found = {}
    for name, value in d.items():
        for pattern in QUEUE_PACKETS:
            m = pattern.match(name)
            if m:
                found[int(m.group(1))] = found.get(int(m.group(1)), 0) + value
                break
    return [found.get(q, 0) for q in range(max([queues - 1] + list(found.keys())) + 1)] if found else []


def tx_packets(d):
    for pattern in TX_PACKETS:
        for name, value in d.items():
            if pattern.match(name):
                return value
    return None


def drops(d):
    return {n: v for n, v in d.items() if v and DROP.search(n) and not NOT_RX.search(n)}


def num(snap, key):
    return int(snap.get(key, '0'))


def window(args):
    w = args.dir
    rx = delta(parse_ethtool(w + '/rx-before.txt'), parse_ethtool(w + '/rx-after.txt'))
    tx = delta(parse_ethtool(w + '/tx-before.txt'), parse_ethtool(w + '/tx-after.txt'))
    s0 = parse_kv(w + '/snap-before.txt', 'SNAP')
    s1 = parse_kv(w + '/snap-after.txt', 'SNAP')
    done = parse_kv(w + '/done.txt', 'DONE')

    def d(key):
        return num(s1, key) - num(s0, key)

    sent = int(done['sent'])
    received = d('received')
    redirected = d('xdp_redirected')
    xdp_drops = sum(d(k) for k in ('xdp_dropped_too_small', 'xdp_dropped_too_large', 'xdp_dropped_wrong_filter',
                                   'xdp_dropped_invalid_type', 'xdp_dropped_wrong_epoch'))
    xsk_drops = d('xsk_rx_dropped') + d('xsk_rx_ring_full')
    refusals = d('refusals')
    tx_invalid = int(done.get('tx_invalid', '0'))
    seconds = int(done['elapsed_ns']) / 1e9

    queues = per_queue(rx, args.queues)
    nic_rx = sum(queues)
    nic_drops = drops(rx)
    nic_drop_sum = sum(nic_drops.values())
    nic_tx = tx_packets(tx)
    tol = max(args.tol_abs, int(args.tol_rel * sent))

    failures = []
    if s1.get('xdp_map') != 'read':
        failures.append('stats map unread')
    if redirected != received + xsk_drops:
        failures.append('E1 redirected %d != received %d + xsk drops %d (diff %d)' % (redirected, received, xsk_drops, redirected - received - xsk_drops))
    if nic_drop_sum == 0 and sent != received + xsk_drops + xdp_drops:
        failures.append('E2 sent %d != received %d + xsk drops %d + xdp drops %d (diff %d)' % (sent, received, xsk_drops, xdp_drops, sent - received - xsk_drops - xdp_drops))
    if refusals != 0:
        failures.append('E3 receiver refusals %d (%s)' % (refusals, s1.get('refusal_names', '-')))
    if tx_invalid != 0:
        failures.append('E3 sender tx_invalid %d' % tx_invalid)
    if not queues:
        failures.append('H1 no per-queue rx packet counters recognised in ethtool -S')
    elif abs(nic_rx - (redirected + xdp_drops)) > tol:
        failures.append('H1 nic rx %d vs program verdicts %d beyond tolerance %d' % (nic_rx, redirected + xdp_drops, tol))
    if nic_tx is None:
        failures.append('H2 no tx packet counter recognised on the sender NIC')
    elif abs(nic_tx - sent) > tol:
        failures.append('H2 sender nic tx %d vs sent %d beyond tolerance %d' % (nic_tx, sent, tol))
    if nic_drop_sum and abs(sent - (received + xsk_drops + xdp_drops + nic_drop_sum)) > tol:
        failures.append('H3 sent %d vs received + every counted drop %d beyond tolerance %d' % (sent, received + xsk_drops + xdp_drops + nic_drop_sum, tol))

    clean = nic_drop_sum == 0 and xsk_drops == 0 and refusals == 0 and tx_invalid == 0
    record = {
        'step': args.step, 'window': args.window, 'sent': sent, 'received': received,
        'redirected': redirected, 'xdp_drops': xdp_drops, 'xsk_drops': xsk_drops,
        'refusals': refusals, 'tx_invalid': tx_invalid, 'nic_rx': nic_rx, 'nic_rx_per_queue': queues,
        'nic_drops': nic_drops, 'nic_tx': nic_tx, 'seconds': seconds,
        'pps_sent': sent / seconds if seconds else 0.0, 'pps_nic': nic_rx / seconds if seconds else 0.0,
        'clean': clean, 'pass': not failures, 'failures': failures, 'tolerance': tol,
        'back_pressure': int(done.get('back_pressure', '0')),
    }
    if args.out:
        with open(args.out, 'a') as f:
            f.write(json.dumps(record, sort_keys=True) + '\n')

    print('WINDOW step=%s window=%d %s clean=%s sent=%d received=%d redirected=%d xdp_drops=%d xsk_drops=%d '
          'nic_rx=%d nic_tx=%s nic_drops=%s per_queue=%s pps_sent=%.0f pps_nic=%.0f tol=%d%s' % (
              args.step, args.window, 'PASS' if not failures else 'FAIL', 'yes' if clean else 'no',
              sent, received, redirected, xdp_drops, xsk_drops, nic_rx, nic_tx,
              ','.join('%s:%d' % kv for kv in sorted(nic_drops.items())) or '0',
              ','.join(str(q) for q in queues), record['pps_sent'], record['pps_nic'], tol,
              ''.join(' | ' + f for f in failures)))
    return 0 if not failures else 1


def summary(args):
    records = []
    with open(args.path) as f:
        for line in f:
            if line.strip():
                records.append(json.loads(line))
    steps = {}
    for r in records:
        steps.setdefault(float(r['step']), []).append(r)
    knee = None
    all_pass = bool(records)
    for step in sorted(steps):
        rs = steps[step]
        pps = [r['pps_nic'] for r in rs]
        ok = all(r['pass'] for r in rs)
        clean = all(r['clean'] for r in rs)
        all_pass = all_pass and ok
        if ok and clean:
            knee = step
        print('STEP pps=%.0f windows=%d pass=%s clean=%s nic_pps_median=%.0f min=%.0f max=%.0f nic_drops=%d xsk_drops=%d' % (
            step, len(rs), 'yes' if ok else 'no', 'yes' if clean else 'no',
            statistics.median(pps), min(pps), max(pps),
            sum(sum(r['nic_drops'].values()) for r in rs), sum(r['xsk_drops'] for r in rs)))
    if all_pass and knee is not None:
        print('ROW throughput_by_hardware_counters PASS knee_pps=%.0f (highest step with every window clean and exact)' % knee)
        return 0
    if knee is None:
        print('ROW throughput_by_hardware_counters FAIL no clean step: lower the first rate step, or read the window lines')
    else:
        print('ROW throughput_by_hardware_counters FAIL a window broke an exact identity or a tolerance (knee so far %.0f)' % knee)
    return 1


def main():
    parser = argparse.ArgumentParser(description='the XDP hardware tier judge')
    sub = parser.add_subparsers(dest='command', required=True)
    w = sub.add_parser('window')
    w.add_argument('dir')
    w.add_argument('--step', required=True)
    w.add_argument('--window', type=int, required=True)
    w.add_argument('--queues', type=int, required=True)
    w.add_argument('--tol-abs', type=int, default=32)
    w.add_argument('--tol-rel', type=float, default=1e-6)
    w.add_argument('--out')
    s = sub.add_parser('summary')
    s.add_argument('path')
    args = parser.parse_args()
    if args.command == 'window':
        return window(args)
    return summary(args)


if __name__ == '__main__':
    sys.exit(main())
