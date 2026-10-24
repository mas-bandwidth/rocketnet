#!/usr/bin/env python3
#
#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0
#
# Turns a minimized libFuzzer finding into the unit tier's pcapng form:
#
#   python3 tools/fuzz_finding_to_pcapng.py <harness> <input> --name <name>
#
# rocketnet_fuzz with input[0] & 3 == 0 is the prefilter: one interface-0
# IPv4/UDP frame around input[1:]. rocketnet_fuzz_message with the same
# selector is one interface-1 message payload record, kind 3 then input[1:].
# The finding is copied to fuzz/<corpus>/regress_<name> beside the capture.
# Any other harness or selector is refused, as is a name outside [a-z0-9_]+
# or an existing target: a regression capture is never overwritten.
#
# The input is expected to be already minimized (-minimize_crash=1).

import os
import re
import sys

import pcapng

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = re.compile(r"^[a-z0-9_]+$")

class Refusal(Exception):
    """A finding that has no pcapng form, or a target that must not be written."""


def _no_form(harness, selector, corpus, name):
    return ("no pcapng form for %s selector %d; keep the input as "
            "fuzz/%s/regress_%s and file it" % (harness, selector, corpus, name))


def convert(root, harness, input_path, name):
    """Writes the capture and the seed under root; returns their paths."""
    if not NAME.fullmatch(name):
        raise Refusal("bad name %r; use [a-z0-9_]+" % name)
    with open(input_path, "rb") as f:
        data = f.read()
    if not data:
        raise Refusal("empty input has no selector byte")
    selector = data[0] & 3

    if harness == "rocketnet_fuzz":
        corpus = "corpus"
        if selector != 0:
            raise Refusal(_no_form(harness, selector, corpus, name))
        frame = pcapng.ipv4_udp_frame(pcapng.CLIENT_ADDRESS, pcapng.CLIENT_PORT,
                                      pcapng.SERVER_ADDRESS, pcapng.SERVER_PORT, data[1:])
        packets = [{"interface": pcapng.INTERFACE_WIRE, "linktype": pcapng.LINKTYPE_IPV4,
                    "timestamp_us": pcapng.CLOCK_START_US, "data": frame}]
    elif harness == "rocketnet_fuzz_message":
        corpus = "corpus_message"
        if selector != 0:
            raise Refusal(_no_form(harness, selector, corpus, name))
        record = bytes([pcapng.KIND_MESSAGE_PAYLOAD]) + data[1:]
        packets = [{"interface": pcapng.INTERFACE_OPENED, "linktype": pcapng.LINKTYPE_USER0,
                    "timestamp_us": pcapng.CLOCK_START_US, "data": record, "comment": "server"}]
    else:
        raise Refusal(_no_form(harness, selector, "corpus", name))

    target = os.path.join(root, "tests", "pcapng", "regress", name + ".pcapng")
    seed = os.path.join(root, "fuzz", corpus, "regress_" + name)
    for path in (target, seed):
        if os.path.exists(path):
            raise Refusal("refusing to overwrite existing %s" % os.path.relpath(path, root))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    os.makedirs(os.path.dirname(seed), exist_ok=True)
    pcapng.write_packets(target, packets)
    with open(seed, "wb") as f:
        f.write(data)
    return target, seed


def _expect_refusal(call, fragment):
    try:
        call()
    except Refusal as e:
        if fragment not in str(e):
            raise AssertionError("expected refusal %r, got %r" % (fragment, str(e)))
        return
    raise AssertionError("expected a refusal: %s" % fragment)


def selftest():
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        root = directory
        for part in ("fuzz/corpus", "fuzz/corpus_message", "tests/pcapng/regress"):
            os.makedirs(os.path.join(root, *part.split("/")), exist_ok=True)

        prefilter = bytes([0x00]) + b"\x05\x00\xb7\x59\x01\x00abc"
        prefilter_input = os.path.join(root, "prefilter.finding")
        with open(prefilter_input, "wb") as f:
            f.write(prefilter)
        target, seed = convert(root, "rocketnet_fuzz", prefilter_input, "prefilter_case")
        packets = pcapng.read_packets(target)
        assert len(packets) == 1, "prefilter capture must hold one packet"
        assert packets[0]["interface"] == pcapng.INTERFACE_WIRE
        assert packets[0]["timestamp_us"] == pcapng.CLOCK_START_US
        assert pcapng.udp_payload(packets[0]["data"]) == prefilter[1:], "prefilter bytes differ"
        with open(seed, "rb") as f:
            assert f.read() == prefilter, "prefilter seed differs"

        message = bytes([0x00]) + b"\x01\x02\x03\x04\x05"
        message_input = os.path.join(root, "message.finding")
        with open(message_input, "wb") as f:
            f.write(message)
        target, seed = convert(root, "rocketnet_fuzz_message", message_input, "message_case")
        packets = pcapng.read_packets(target)
        assert len(packets) == 1, "message capture must hold one record"
        assert packets[0]["interface"] == pcapng.INTERFACE_OPENED
        assert packets[0]["data"] == bytes([pcapng.KIND_MESSAGE_PAYLOAD]) + message[1:], "message bytes differ"
        with open(seed, "rb") as f:
            assert f.read() == message, "message seed differs"

        _expect_refusal(lambda: convert(root, "rocketnet_fuzz_other", prefilter_input, "unknown_harness"),
                        "no pcapng form for rocketnet_fuzz_other")
        selector_one = os.path.join(root, "selector_one.finding")
        with open(selector_one, "wb") as f:
            f.write(bytes([0x01]) + prefilter[1:])
        _expect_refusal(lambda: convert(root, "rocketnet_fuzz", selector_one, "selector_one"),
                        "no pcapng form for rocketnet_fuzz selector 1; keep the input as "
                        "fuzz/corpus/regress_selector_one and file it")
        _expect_refusal(lambda: convert(root, "rocketnet_fuzz", prefilter_input, "Bad-Name"),
                        "bad name")
        with open(target, "rb") as f:
            before = f.read()
        _expect_refusal(lambda: convert(root, "rocketnet_fuzz_message", message_input, "message_case"),
                        "refusing to overwrite existing")
        with open(target, "rb") as f:
            assert f.read() == before, "a refused conversion changed the target"

    print("fuzz_finding_to_pcapng selftest: ok (prefilter and message forms; refused unknown harness, "
          "selector 1, bad name, existing target)")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) == 5 and argv[3] == "--name":
        try:
            target, seed = convert(ROOT, argv[1], argv[2], argv[4])
        except (Refusal, OSError) as e:
            sys.stderr.write("%s\n" % e)
            return 2
        print("%s %s" % (os.path.relpath(target, ROOT), os.path.relpath(seed, ROOT)))
        return 0
    sys.stderr.write("usage: python3 tools/fuzz_finding_to_pcapng.py <harness> <input> --name <name>\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
