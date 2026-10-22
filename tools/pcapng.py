#!/usr/bin/env python3
#
#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0
#
# The python side of the pcapng corpus (tests/pcapng/README.md is the design):
# the same subset tests/pcapng.h reads and writes, standard library only.
#
#   read_packets(path)            -> [ { interface, linktype, timestamp_us, data }, ... ]
#   write_packets(path, packets)  takes the same dicts (an optional "comment"
#                                 key writes the EPB opt_comment, and reads back)
#   udp_payload(data)             -> the datagram inside an interface-0 frame
#
#   python3 tools/pcapng.py --selftest
#   python3 tools/pcapng.py hostile <fuzz/corpus> <out.pcapng>
#   python3 tools/pcapng.py fragments <fuzz/corpus> <out.pcapng>
#   python3 tools/pcapng.py summary <file.pcapng> ...

import os
import struct
import sys
import tempfile

LINKTYPE_IPV4 = 228
LINKTYPE_USER0 = 147

INTERFACE_WIRE = 0
INTERFACE_OPENED = 1

KIND_ACK_BLOCK = 1
KIND_FRAGMENT = 2
KIND_MESSAGE_PAYLOAD = 3

MAX_FILE_BYTES = 128 * 1024
MAX_CORPUS_BYTES = 512 * 1024

BLOCK_SHB = 0x0A0D0D0A
BLOCK_IDB = 0x00000001
BLOCK_EPB = 0x00000006
BYTE_ORDER_MAGIC = 0x1A2B3C4D
OPT_ENDOFOPT = 0
OPT_COMMENT = 1
SNAPLEN = 65535

# the test endpoints (functional/channel_harness.h)
CLIENT_ADDRESS = (198, 51, 100, 7)
CLIENT_PORT = 56789
SERVER_ADDRESS = (203, 0, 113, 10)
SERVER_PORT = 40000

# the corpus clock: the functional tier's test clock, 1000.0 s, in microseconds
CLOCK_START_US = 1000 * 1000 * 1000


class PcapngError(Exception):
    """A refusal, by name (the same names tests/pcapng.h returns)."""

    def __init__(self, name):
        Exception.__init__(self, name)
        self.name = name


def _pad4(data):
    return data + b"\x00" * ((4 - len(data) % 4) % 4)


def _block(block_type, body):
    body = _pad4(body)
    total = 12 + len(body)
    return struct.pack("<II", block_type, total) + body + struct.pack("<I", total)


def write_packets(path, packets):
    """Writes one section, interfaces 0 (IPv4) and 1 (USER0), then the packets in order."""
    out = bytearray()
    out += _block(BLOCK_SHB, struct.pack("<IHHq", BYTE_ORDER_MAGIC, 1, 0, -1))
    linktypes = [LINKTYPE_IPV4, LINKTYPE_USER0]
    for linktype in linktypes:
        out += _block(BLOCK_IDB, struct.pack("<HHI", linktype, 0, SNAPLEN))
    for packet in packets:
        interface = packet["interface"]
        if interface not in (0, 1) or packet["linktype"] != linktypes[interface]:
            raise PcapngError("PCAPNG_UNKNOWN_LINKTYPE")
        data = bytes(packet["data"])
        ts = int(packet["timestamp_us"])
        body = struct.pack("<IIIII", interface, ts >> 32, ts & 0xFFFFFFFF, len(data), len(data))
        body += _pad4(data)
        comment = packet.get("comment")
        if comment:
            value = comment.encode("utf-8")
            body += struct.pack("<HH", OPT_COMMENT, len(value)) + _pad4(value)
            body += struct.pack("<HH", OPT_ENDOFOPT, 0)
        out += _block(BLOCK_EPB, body)
    if len(out) > MAX_FILE_BYTES:
        raise PcapngError("PCAPNG_FILE_OVER_CAP")
    with open(path, "wb") as f:
        f.write(bytes(out))


def read_buffer(image):
    """The reader over a whole file image; raises PcapngError by name."""
    if len(image) > MAX_FILE_BYTES:
        raise PcapngError("PCAPNG_FILE_OVER_CAP")
    packets = []
    interfaces = []
    offset = 0
    section = False
    while offset < len(image):
        remaining = len(image) - offset
        if remaining < 12:
            raise PcapngError("PCAPNG_TRUNCATED_BLOCK")
        block_type, total = struct.unpack_from("<II", image, offset)
        if total % 4:
            raise PcapngError("PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4")
        if total < 12 or total > remaining:
            raise PcapngError("PCAPNG_TRUNCATED_BLOCK")
        if struct.unpack_from("<I", image, offset + total - 4)[0] != total:
            raise PcapngError("PCAPNG_TRUNCATED_BLOCK")
        body = image[offset + 8: offset + total - 4]
        if not section:
            if block_type != BLOCK_SHB or len(body) < 16:
                raise PcapngError("PCAPNG_NOT_A_SECTION")
            magic, major, minor = struct.unpack_from("<IHH", body, 0)
            if magic != BYTE_ORDER_MAGIC or major != 1 or minor != 0:
                raise PcapngError("PCAPNG_NOT_A_SECTION")
            section = True
        elif block_type == BLOCK_SHB:
            raise PcapngError("PCAPNG_SECOND_SECTION")
        elif block_type == BLOCK_IDB:
            if len(body) < 8:
                raise PcapngError("PCAPNG_TRUNCATED_BLOCK")
            linktype = struct.unpack_from("<H", body, 0)[0]
            if linktype not in (LINKTYPE_IPV4, LINKTYPE_USER0):
                raise PcapngError("PCAPNG_UNKNOWN_LINKTYPE")
            interfaces.append(linktype)
        elif block_type == BLOCK_EPB:
            if len(body) < 20:
                raise PcapngError("PCAPNG_TRUNCATED_BLOCK")
            interface, high, low, captured, original = struct.unpack_from("<IIIII", body, 0)
            if interface >= len(interfaces):
                raise PcapngError("PCAPNG_UNKNOWN_INTERFACE")
            room = len(body) - 20
            padded = (captured + 3) & ~3
            if captured != original or padded > room:
                raise PcapngError("PCAPNG_BAD_PACKET_LENGTH")
            packet = {
                "interface": interface,
                "linktype": interfaces[interface],
                "timestamp_us": (high << 32) | low,
                "data": bytes(body[20: 20 + captured]),
            }
            option = 20 + padded
            while option < len(body):
                if len(body) - option < 4:
                    raise PcapngError("PCAPNG_BAD_OPTION")
                code, length = struct.unpack_from("<HH", body, option)
                if code == OPT_ENDOFOPT:
                    break
                value_padded = (length + 3) & ~3
                if value_padded > len(body) - option - 4:
                    raise PcapngError("PCAPNG_BAD_OPTION")
                if code == OPT_COMMENT:
                    packet["comment"] = bytes(body[option + 4: option + 4 + length]).decode("utf-8")
                option += 4 + value_padded
            packets.append(packet)
        else:
            raise PcapngError("PCAPNG_UNKNOWN_BLOCK")
        offset += total
    if not section:
        raise PcapngError("PCAPNG_NOT_A_SECTION")
    return packets


def read_packets(path):
    with open(path, "rb") as f:
        image = f.read(MAX_FILE_BYTES + 1)
    return read_buffer(image)


def _ipv4_checksum(header):
    total = 0
    for i in range(0, len(header), 2):
        total += (header[i] << 8) | header[i + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total


def ipv4_udp_frame(source, source_port, destination, destination_port, datagram):
    """The synthetic interface-0 frame around a datagram (tests/pcapng.h does the same)."""
    total = 28 + len(datagram)
    header = bytearray(struct.pack(">BBHHHBBH4B4B", 0x45, 0, total, 0, 0, 64, 17, 0, *(tuple(source) + tuple(destination))))
    checksum = (~_ipv4_checksum(header)) & 0xFFFF
    header[10] = checksum >> 8
    header[11] = checksum & 0xFF
    udp = struct.pack(">HHHH", source_port, destination_port, 8 + len(datagram), 0)
    return bytes(header) + udp + bytes(datagram)


def udp_payload(data):
    """The datagram inside an interface-0 frame, or None when it is not one."""
    data = bytes(data)
    if len(data) < 28 or data[0] != 0x45 or data[9] != 17:
        return None
    total = struct.unpack_from(">H", data, 2)[0]
    if total != len(data) or _ipv4_checksum(data[:20]) != 0xFFFF:
        return None
    if struct.unpack_from(">H", data, 24)[0] != total - 20:
        return None
    return data[28:]


def _corpus_files(corpus, prefix):
    names = sorted(n for n in os.listdir(corpus) if n.startswith(prefix))
    out = []
    for name in names:
        with open(os.path.join(corpus, name), "rb") as f:
            out.append((name, f.read()))
    return out


def build_hostile(corpus, out_path):
    """Every fuzz/corpus/prefilter_* seed, selector byte removed, client to server on interface 0."""
    packets = []
    ts = CLOCK_START_US
    for name, data in _corpus_files(corpus, "prefilter_"):
        frame = ipv4_udp_frame(CLIENT_ADDRESS, CLIENT_PORT, SERVER_ADDRESS, SERVER_PORT, data[1:])
        packets.append({"interface": INTERFACE_WIRE, "linktype": LINKTYPE_IPV4, "timestamp_us": ts, "data": frame,
                        "comment": name})
        ts += 1000
    write_packets(out_path, packets)
    return packets


def build_fragments(corpus, out_path):
    """Every fuzz/corpus/fragment_* seed's length-prefixed records (u16 little-endian length,
    then fragment type data, as fuzz/rocketnet_fuzz.cpp reads them), each one kind-2 record
    on interface 1, received by the server, alternating between reassembly connections 0
    and 1 from the first record of each seed as the fuzz harness does."""
    packets = []
    ts = CLOCK_START_US
    for name, data in _corpus_files(corpus, "fragment_"):
        body = data[1:]
        offset = 0
        connection = 0
        while offset + 2 <= len(body):
            length = struct.unpack_from("<H", body, offset)[0]
            offset += 2
            record = body[offset: offset + length]
            offset += length
            packets.append({"interface": INTERFACE_OPENED, "linktype": LINKTYPE_USER0, "timestamp_us": ts,
                            "data": bytes([KIND_FRAGMENT]) + record,
                            "comment": "server connection=%d %s" % (connection, name)})
            connection ^= 1
            ts += 1000
    write_packets(out_path, packets)
    return packets


def summary(path):
    packets = read_packets(path)
    counts = {}
    for p in packets:
        if p["interface"] == INTERFACE_WIRE:
            datagram = udp_payload(p["data"])
            key = "wire type %d" % datagram[0] if datagram else "wire not-udp"
        else:
            key = "opened kind %d" % p["data"][0] if p["data"] else "opened empty"
        counts[key] = counts.get(key, 0) + 1
    print("%s: %d packets, %d bytes" % (path, len(packets), os.path.getsize(path)))
    for key in sorted(counts):
        print("  %s: %d" % (key, counts[key]))


def _expect_refusal(image, name):
    try:
        read_buffer(image)
    except PcapngError as e:
        if e.name != name:
            raise AssertionError("expected %s, got %s" % (name, e.name))
        return
    raise AssertionError("expected %s, the image read" % name)


def selftest():
    packets = [
        {"interface": 0, "linktype": LINKTYPE_IPV4, "timestamp_us": CLOCK_START_US,
         "data": ipv4_udp_frame(CLIENT_ADDRESS, CLIENT_PORT, SERVER_ADDRESS, SERVER_PORT, b"\x05\x00\xb7\x59\x01\x00abc")},
        {"interface": 1, "linktype": LINKTYPE_USER0, "timestamp_us": CLOCK_START_US + 16667,
         "data": bytes([KIND_MESSAGE_PAYLOAD]) + b"\x01\x02\x03\x04\x05", "comment": "client"},
        {"interface": 1, "linktype": LINKTYPE_USER0, "timestamp_us": (1 << 33) + 7, "data": bytes([KIND_ACK_BLOCK])},
    ]
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "selftest.pcapng")
        write_packets(path, packets)
        back = read_packets(path)
        assert back == packets, "round trip differs"
        assert udp_payload(back[0]["data"]) == b"\x05\x00\xb7\x59\x01\x00abc", "udp payload differs"
        assert udp_payload(back[1]["data"]) is None, "a USER0 record is not a frame"
        with open(path, "rb") as f:
            image = f.read()

    # the four malformed cases, each refused by name
    _expect_refusal(image[:-2], "PCAPNG_TRUNCATED_BLOCK")
    bad_length = bytearray(image)
    struct.pack_into("<I", bad_length, 4, struct.unpack_from("<I", image, 4)[0] + 2)
    _expect_refusal(bytes(bad_length), "PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4")
    bad_linktype = bytearray(image)
    shb_total = struct.unpack_from("<I", image, 4)[0]
    struct.pack_into("<H", bad_linktype, shb_total + 8, 1)
    _expect_refusal(bytes(bad_linktype), "PCAPNG_UNKNOWN_LINKTYPE")
    _expect_refusal(image + b"\x00" * (MAX_FILE_BYTES + 1 - len(image)), "PCAPNG_FILE_OVER_CAP")

    # the writer refuses what the reader would
    try:
        write_packets(os.devnull, [{"interface": 0, "linktype": LINKTYPE_USER0, "timestamp_us": 0, "data": b""}])
        raise AssertionError("writer took a mismatched linktype")
    except PcapngError as e:
        assert e.name == "PCAPNG_UNKNOWN_LINKTYPE"
    print("pcapng selftest: ok (round trip of %d packets; refused truncated, length not a multiple of 4, "
          "unknown linktype, over cap)" % len(packets))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) == 4 and argv[1] == "hostile":
        print("%s: %d packets" % (argv[3], len(build_hostile(argv[2], argv[3]))))
        return 0
    if len(argv) == 4 and argv[1] == "fragments":
        print("%s: %d records" % (argv[3], len(build_fragments(argv[2], argv[3]))))
        return 0
    if len(argv) >= 3 and argv[1] == "summary":
        for path in argv[2:]:
            summary(path)
        return 0
    sys.stderr.write(__doc__ if __doc__ else "usage: pcapng.py --selftest | hostile <corpus> <out> | fragments <corpus> <out> | summary <file>...\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
