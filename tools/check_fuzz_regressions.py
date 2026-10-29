#!/usr/bin/env python3
#
#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0
#
# Every minimized fuzz regression has both halves: a seed under
# tests/fuzz/corpus/regress_<name> or tests/fuzz/corpus_message/regress_<name>, and one
# tests/pcapng/regress/<name>.pcapng whose packet carries exactly the seed's
# bytes after its selector byte. A seed without a capture, a capture without
# a seed, and a capture whose bytes differ are each one line; any of them
# makes the check exit 1. With no regression files the check exits 0 and
# prints "0 regressions".
#
#   python3 tools/check_fuzz_regressions.py
#   python3 tools/check_fuzz_regressions.py --selftest

import os
import sys

import pcapng

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGESS = os.path.join("tests", "pcapng", "regress")
SEED_CORPORA = (("tests/fuzz/corpus", pcapng.INTERFACE_WIRE),
                ("tests/fuzz/corpus_message", pcapng.INTERFACE_OPENED))


def seeds(root):
    """(corpus, interface, name, path) for every regress_ seed, in order."""
    found = []
    for corpus, interface in SEED_CORPORA:
        directory = os.path.join(root, *corpus.split("/"))
        if not os.path.isdir(directory):
            continue
        for filename in sorted(os.listdir(directory)):
            path = os.path.join(directory, filename)
            if filename.startswith("regress_") and os.path.isfile(path):
                found.append((corpus, interface, filename[len("regress_"):], path))
    return found


def carries(interface, packet, seed):
    """True when the one packet holds the seed's bytes after the selector."""
    if packet["interface"] != interface:
        return False
    body = seed[1:]
    if interface == pcapng.INTERFACE_WIRE:
        return pcapng.udp_payload(packet["data"]) == body
    return packet["data"] == bytes([pcapng.KIND_MESSAGE_PAYLOAD]) + body


def check(root):
    """Returns (one line per orphan or mismatch, the number of paired seeds)."""
    root = os.path.abspath(root)
    regress = os.path.join(root, *REGESS.split("/"))
    problems = []
    paired = 0
    expected = set()

    for corpus, interface, name, path in seeds(root):
        expected.add(name)
        target = os.path.join(regress, name + ".pcapng")
        with open(path, "rb") as f:
            seed = f.read()
        if not os.path.exists(target):
            problems.append("orphan seed %s/%s: no %s/%s.pcapng" %
                            (corpus, os.path.basename(path), REGESS, name))
            continue
        try:
            packets = pcapng.read_packets(target)
        except (pcapng.PcapngError, OSError) as e:
            problems.append("mismatch %s/%s.pcapng: %s" % (REGESS, name, e))
            continue
        if len(packets) != 1:
            problems.append("mismatch %s/%s.pcapng: %d packets, expected 1" %
                            (REGESS, name, len(packets)))
            continue
        if not carries(interface, packets[0], seed):
            problems.append("mismatch %s/%s.pcapng: the packet does not carry %s/%s after the selector" %
                            (REGESS, name, corpus, os.path.basename(path)))
            continue
        paired += 1

    captures = []
    if os.path.isdir(regress):
        captures = sorted(n for n in os.listdir(regress) if n.endswith(".pcapng"))
    for filename in captures:
        name = filename[:-len(".pcapng")]
        if name not in expected:
            problems.append("orphan capture %s/%s: no tests/fuzz/corpus/regress_%s or tests/fuzz/corpus_message/regress_%s" %
                            (REGESS, filename, name, name))
    return problems, paired


def _write_seed(root, corpus, name, data):
    path = os.path.join(root, *corpus.split("/"), "regress_" + name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


def selftest():
    import tempfile

    import fuzz_finding_to_pcapng as converter

    with tempfile.TemporaryDirectory() as directory:
        root = directory
        for part in ("tests/fuzz/corpus", "tests/fuzz/corpus_message", "tests/pcapng/regress"):
            os.makedirs(os.path.join(root, *part.split("/")), exist_ok=True)

        prefilter = os.path.join(root, "prefilter.finding")
        with open(prefilter, "wb") as f:
            f.write(bytes([0x00]) + b"\x05\x00\xb7\x59\x01\x00abc")
        converter.convert(root, "rocketnet_fuzz", prefilter, "prefilter_case")
        message = os.path.join(root, "message.finding")
        with open(message, "wb") as f:
            f.write(bytes([0x00]) + b"\x01\x02\x03\x04\x05")
        converter.convert(root, "rocketnet_fuzz_message", message, "message_case")

        problems, paired = check(root)
        assert not problems, "a converted pair reported %r" % problems
        assert paired == 2, "expected two paired seeds, got %d" % paired

        target = os.path.join(root, *REGESS.split("/"), "prefilter_case.pcapng")
        with open(target, "rb") as f:
            image = f.read()
        os.unlink(target)
        problems, paired = check(root)
        assert len(problems) == 1 and "orphan seed" in problems[0], "a missing capture was not one orphan"
        assert paired == 1, "a missing capture must leave one pair"

        with open(target, "wb") as f:
            f.write(image)
        problems, paired = check(root)
        assert not problems and paired == 2, "the restored capture did not pair"

        seed = os.path.join(root, *"tests/fuzz/corpus".split("/"), "regress_prefilter_case")
        os.unlink(seed)
        problems, paired = check(root)
        assert len(problems) == 1 and "orphan capture" in problems[0], "a missing seed was not one orphan"
        assert paired == 1, "a missing seed must leave one pair"

    print("check_fuzz_regressions selftest: ok (two converted pairs; missing capture and missing seed each red)")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) != 1:
        sys.stderr.write("usage: python3 tools/check_fuzz_regressions.py [--selftest]\n")
        return 2
    problems, paired = check(ROOT)
    if problems:
        for line in problems:
            print(line)
        return 1
    print("%d regressions" % paired)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
