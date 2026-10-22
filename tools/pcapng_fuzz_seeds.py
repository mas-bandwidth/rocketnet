#!/usr/bin/env python3
#
#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0
#
# Seed the fuzz harnesses from the pcapng corpus: every captured wire
# datagram becomes a prefilter seed and every captured kind-3 message
# payload becomes a message seed, and the committed seed set is checked.
# The repository root comes from this file's own path; only tests/pcapng/
# is read and only the two corpus directories are written.
#
#   --write       regenerate the seeds under fuzz/corpus and fuzz/corpus_message
#   --check       compare the committed seeds with what the corpus implies
#   --selftest   self-test in a temporary directory, touching nothing outside

import hashlib
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcapng

MAX_SEEDS_PER_DIR = 64
PREFIX_PREFILTER = "prefilter_pcap_"
PREFIX_MESSAGE = "message_pcap_"


def repo_root():
    return Path(__file__).resolve().parents[1]


def _sha12(data):
    return hashlib.sha256(data).hexdigest()[:12]


def compute_seeds(pcapng_root):
    """Return ({prefilter name: bytes}, {message name: bytes}) in file and packet order."""
    prefilter = {}
    message = {}
    seen_pre = set()
    seen_msg = set()

    def scan(directory):
        if not directory.is_dir():
            return
        for path in sorted(directory.glob("*.pcapng")):
            stem = path.stem
            for packet in pcapng.read_packets(str(path)):
                if packet["interface"] == 0:
                    payload = pcapng.udp_payload(packet["data"])
                    if payload is None:
                        continue
                    seed = b"\x00" + bytes(payload)
                    if seed in seen_pre:
                        continue
                    seen_pre.add(seed)
                    prefilter[PREFIX_PREFILTER + stem + "_" + _sha12(seed)] = seed
                elif packet["interface"] == 1:
                    data = packet["data"]
                    if not data or data[0] != pcapng.KIND_MESSAGE_PAYLOAD:
                        continue
                    seed = b"\x00" + bytes(data[1:])
                    if seed in seen_msg:
                        continue
                    seen_msg.add(seed)
                    message[PREFIX_MESSAGE + stem + "_" + _sha12(seed)] = seed

    scan(pcapng_root)
    scan(pcapng_root / "regress")
    return prefilter, message


def _truncate(seeds):
    items = list(seeds.items())
    return items[:MAX_SEEDS_PER_DIR], max(0, len(items) - MAX_SEEDS_PER_DIR)


def write_seeds(root):
    prefilter, message = compute_seeds(root / "tests" / "pcapng")
    pre_items, pre_cut = _truncate(prefilter)
    msg_items, msg_cut = _truncate(message)

    pre_dir = root / "fuzz" / "corpus"
    msg_dir = root / "fuzz" / "corpus_message"
    pre_dir.mkdir(parents=True, exist_ok=True)
    msg_dir.mkdir(parents=True, exist_ok=True)

    for name, data in pre_items:
        (pre_dir / name).write_bytes(data)
    for name, data in msg_items:
        (msg_dir / name).write_bytes(data)

    print("prefilter: wrote %d seeds, cut %d" % (len(pre_items), pre_cut))
    print("message: wrote %d seeds, cut %d" % (len(msg_items), msg_cut))
    return 0


def check_seeds(root):
    prefilter, message = compute_seeds(root / "tests" / "pcapng")
    expected = {}
    for name, data in _truncate(prefilter)[0] + _truncate(message)[0]:
        expected[name] = data

    actual = {}
    for directory, prefix in ((root / "fuzz" / "corpus", PREFIX_PREFILTER),
                              (root / "fuzz" / "corpus_message", PREFIX_MESSAGE)):
        if directory.is_dir():
            for path in sorted(directory.glob(prefix + "*")):
                actual[path.name] = path.read_bytes()

    problems = []
    for name in sorted(expected):
        if name not in actual:
            problems.append("missing %s" % name)
        elif actual[name] != expected[name]:
            problems.append("differ %s" % name)
    for name in sorted(actual):
        if name not in expected:
            problems.append("extra %s" % name)

    for line in problems:
        print(line)
    return 1 if problems else 0


def selftest():
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        pcapng_dir = root / "tests" / "pcapng"
        pcapng_dir.mkdir(parents=True)

        frame1 = pcapng.ipv4_udp_frame(
            pcapng.CLIENT_ADDRESS, pcapng.CLIENT_PORT,
            pcapng.SERVER_ADDRESS, pcapng.SERVER_PORT,
            b"\x05\x00\xb7\x59\x01\x00abc")
        frame2 = pcapng.ipv4_udp_frame(
            pcapng.CLIENT_ADDRESS, pcapng.CLIENT_PORT,
            pcapng.SERVER_ADDRESS, pcapng.SERVER_PORT,
            b"\x07\x42")

        pcapng.write_packets(str(pcapng_dir / "a.pcapng"), [
            {"interface": 0, "linktype": pcapng.LINKTYPE_IPV4,
             "timestamp_us": pcapng.CLOCK_START_US, "data": frame1},
            {"interface": 0, "linktype": pcapng.LINKTYPE_IPV4,
             "timestamp_us": pcapng.CLOCK_START_US + 1000, "data": frame1},
            {"interface": 1, "linktype": pcapng.LINKTYPE_USER0,
             "timestamp_us": pcapng.CLOCK_START_US + 2000,
             "data": bytes([pcapng.KIND_MESSAGE_PAYLOAD]) + b"\xaa\xbb"},
            {"interface": 1, "linktype": pcapng.LINKTYPE_USER0,
             "timestamp_us": pcapng.CLOCK_START_US + 3000,
             "data": bytes([pcapng.KIND_ACK_BLOCK])},
        ])
        pcapng.write_packets(str(pcapng_dir / "b.pcapng"), [
            {"interface": 0, "linktype": pcapng.LINKTYPE_IPV4,
             "timestamp_us": pcapng.CLOCK_START_US + 4000, "data": frame2},
        ])

        write_seeds(root)

        pre_dir = root / "fuzz" / "corpus"
        msg_dir = root / "fuzz" / "corpus_message"
        pre_files = sorted(pre_dir.glob(PREFIX_PREFILTER + "*"))
        msg_files = sorted(msg_dir.glob(PREFIX_MESSAGE + "*"))

        assert len(pre_files) == 2, "expected 2 prefilter seeds, got %d" % len(pre_files)
        for f in pre_files + msg_files:
            assert f.read_bytes()[0] == 0x00, "selector byte not 0x00 in %s" % f.name
        assert len(msg_files) == 1, "expected 1 message seed, got %d" % len(msg_files)

        pre_files[0].unlink()
        rc = check_seeds(root)
        assert rc == 1, "check did not fail after a seed was deleted"

    print("pcapng_fuzz_seeds selftest: ok")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--write":
        return write_seeds(repo_root())
    if len(argv) == 2 and argv[1] == "--check":
        return check_seeds(repo_root())
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    sys.stderr.write(__doc__ + "\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
