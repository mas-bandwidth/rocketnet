#!/usr/bin/env python3
"""Refuse fixed-direction byte swaps and host-order reads of wire data in the library.

Every multi-byte wire field is written and read byte by byte
(sdk/rocketnet_read_write.h), so the bytes are the same on any host. A swap
that runs whatever the host's byte order is right on a little-endian host
and wrong on a big-endian one, and only a big-endian run can see it: the XDP
program's three length fields went through __builtin_bswap16 and dropped
every request on a big-endian server. This check refuses that class in the
source text, where review can see it.

The library is every C and C++ file under sdk/, the XDP kernel program
(sdk/xdp/) included, comments and literals removed and line continuations
joined. Rules:

  swap         __builtin_bswap16/32/64/128, bswap_16/32/64, __bswap_16/32/64,
               _byteswap_ushort/_ulong/_uint64 and OSSwapInt16/32/64.
  named swap   a call or definition of a function whose name says it swaps
               bytes (bswap, byteswap, byte_swap, swap_bytes, swapbytes,
               endian_swap, or swap16/32/64).
  shift swap   one logical line that moves a value both ways by 8 bits with a
               byte mask (0xff00 or 0x00ff), or both ways by 24 bits: a
               hand-written 16- or 32-bit swap.
  host read    a pointer cast to a 16-, 32- or 64-bit integer type that is
               dereferenced or indexed in the same expression, or a memcpy
               whose size is exactly sizeof one such type: the integer is
               read or written in host byte order.
  netlink      htons, ntohs, htonl, ntohl or a library hton/ntoh helper in
               the netlink gateway: netlink headers and attributes are host
               order, so a network-order conversion there is the mistake.

The first three pass inside a preprocessor branch whose condition names the
byte order (ROCKETNET_BIG_ENDIAN, __BYTE_ORDER__, __ORDER_*_ENDIAN__,
__LITTLE_ENDIAN__, __BIG_ENDIAN__, BYTE_ORDER): there the swap has a
direction. sdk/rocketnet_read_write.h, the one file that defines the
byte-wise readers and writers, is not read. Every other site passes only
through the ALLOWED list below, each entry a file, a text the line holds,
and the reason; an entry that matches nothing fails, so the list cannot go
stale.

Usage:
  check_byte_order.py              the check over sdk/
  check_byte_order.py FILE...      the check over the named files
  check_byte_order.py --root DIR   the check over DIR/sdk/ (0 files there is an error)
  check_byte_order.py --self-test  the red and green controls
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUFFIXES = (".c", ".h", ".cpp", ".cc", ".hpp", ".inc")

# the one file that defines the byte-wise readers and writers

DEFINER = "sdk/rocketnet_read_write.h"

NETLINK = "sdk/rocketnet_gateway_netlink_linux.cpp"

# (file, text the line holds, reason). Host-order sites that are right
# because the other party is the local kernel or this process.

ALLOWED = [
    ("sdk/rocketnet_platform.cpp", "( (uint16_t *) &out->sin6_addr )[i] = rocketnet_platform_htons(",
     "the kernel's sockaddr_in6 holds network-order groups; the endian-aware helper converts each"),
    ("sdk/rocketnet_platform.cpp", "rocketnet_platform_ntohs( ( (const uint16_t *) &in->sin6_addr )[i] )",
     "the kernel's sockaddr_in6 holds network-order groups; the endian-aware helper converts each"),
    ("sdk/rocketnet_server_gso_linux.cpp", "( (uint16_t *) &addr->sin6_addr )[i] = rocketnet_platform_htons(",
     "the kernel's sockaddr_in6 holds network-order groups; the endian-aware helper converts each"),
    ("sdk/rocketnet_server_gso_linux.cpp", "rocketnet_platform_ntohs( ( (const uint16_t *) &addr->sin6_addr )[i] )",
     "the kernel's sockaddr_in6 holds network-order groups; the endian-aware helper converts each"),
    ("sdk/rocketnet_server_uring_linux.cpp", "( (uint16_t *) &addr->sin6_addr )[i] = rocketnet_platform_htons(",
     "the kernel's sockaddr_in6 holds network-order groups; the endian-aware helper converts each"),
    ("sdk/rocketnet_uring_codec.h", "ntohs( ( (const uint16_t *) &addr->sin6_addr )[i] )",
     "the kernel's sockaddr_in6 holds network-order groups; libc's ntohs converts each"),
    ("sdk/rocketnet_gateway_netlink_linux.cpp", "memcpy( value, attributes->value[type], sizeof( uint32_t ) )",
     "a netlink attribute is host order: the local kernel wrote it"),
    ("sdk/rocketnet_platform.h", "long long observed = *(volatile long long *) value;",
     "the Windows atomic max reads a counter of this process as the interlocked type; no wire byte"),
]

SWAP = re.compile(r"\b(?:__builtin_bswap(?:16|32|64|128)|bswap_(?:16|32|64)|__bswap_(?:16|32|64)"
                  r"|_byteswap_(?:ushort|ulong|uint64)|OSSwapInt(?:16|32|64))\b")
NAMED_SWAP = re.compile(r"\b(\w*(?:bswap|byteswap|byte_swap|swap_bytes|swapbytes|endian_swap)\w*|swap(?:16|32|64))\s*\(",
                        re.IGNORECASE)
SHIFT_8 = re.compile(r"<<\s*8\b")
SHIFT_8_BACK = re.compile(r">>\s*8\b")
MASK_8 = re.compile(r"\b0x(?:00)?ff(?:00)?\b", re.IGNORECASE)
MASK_16 = re.compile(r"\b0x(?:ff00|00ff)\b", re.IGNORECASE)
SHIFT_24 = re.compile(r"<<\s*24\b")
SHIFT_24_BACK = re.compile(r">>\s*24\b")
INTEGER = r"(?:u?int(?:16|32|64)_t|__u(?:16|32|64)|__be(?:16|32|64)|__le(?:16|32|64)|unsigned\s+short|short|unsigned\s+int|unsigned\s+long(?:\s+long)?|long\s+long)"
CAST = r"\(\s*(?:const\s+)?(?:volatile\s+)?" + INTEGER + r"\s*(?:const\s*)?\*\s*\)"
CAST_DEREF = re.compile(r"\*\s*" + CAST)
CAST_INDEX = re.compile(r"\(\s*" + CAST + r"[^;]*?\)\s*\[")
MEMCPY_SCALAR = re.compile(r"\bmemcpy\s*\([^;]*,\s*sizeof\s*\(\s*" + INTEGER + r"\s*\)\s*\)")
NETWORK_ORDER = re.compile(r"\b(?:htons|ntohs|htonl|ntohl|\w*_htons|\w*_ntohs|\w*_htonl|\w*_ntohl)\s*\(")
BYTE_ORDER_CONDITION = re.compile(r"ENDIAN|BYTE_ORDER")

LITERALS = re.compile(
    r"(?P<number>(?<![\w.])\.?\d(?:'?[\w.]|[eEpP][+-])*)"
    r'|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*'"
    r"|/\*.*?\*/|//[^\n]*",
    re.DOTALL,
)

DIRECTIVE = re.compile(r"^\s*#\s*(\w+)(.*)$")


def blank(text):
    """Remove comments and literals, keeping every newline in place. A number is kept."""
    return LITERALS.sub(lambda m: m.group() if m.group("number") else "".join("\n" if c == "\n" else " " for c in m.group()), text)


def logical_lines(text):
    """Join backslash continuations; yield (first physical line number, text)."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        start = i
        line = lines[i]
        while line.endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line[:-1] + " " + lines[i]
        yield start + 1, line
        i += 1


def shift_swap(line):
    if SHIFT_8.search(line) and SHIFT_8_BACK.search(line) and (MASK_16.search(line) or MASK_8.search(line)):
        return True
    return bool(SHIFT_24.search(line) and SHIFT_24_BACK.search(line))


def scan_text(text, relative):
    """Return [(line number, rule, source line)] for every finding in one file."""
    if relative == DEFINER:
        return []
    raw_lines = text.split("\n")
    findings = []
    # one frame per open #if: whether its condition (or an earlier branch's) names the byte order
    stack = []
    for number, line in logical_lines(blank(text)):
        m = DIRECTIVE.match(line)
        if m:
            name, rest = m.group(1), m.group(2)
            if name in ("if", "ifdef", "ifndef"):
                stack.append(bool(BYTE_ORDER_CONDITION.search(rest)))
            elif name == "elif" and stack:
                stack[-1] = stack[-1] or bool(BYTE_ORDER_CONDITION.search(rest))
            elif name == "endif" and stack:
                stack.pop()
            continue
        source = raw_lines[number - 1].strip() if number - 1 < len(raw_lines) else line.strip()
        ordered = any(stack)
        if not ordered:
            for match in SWAP.finditer(line):
                findings.append((number, "swap " + match.group(0), source))
            for match in NAMED_SWAP.finditer(line):
                if not SWAP.match(match.group(1)):
                    findings.append((number, "named swap " + match.group(1), source))
            if shift_swap(line):
                findings.append((number, "shift swap", source))
        if CAST_DEREF.search(line) or CAST_INDEX.search(line):
            findings.append((number, "host read (integer pointer cast)", source))
        if MEMCPY_SCALAR.search(line):
            findings.append((number, "host read (memcpy of one integer)", source))
        if relative == NETLINK:
            for match in NETWORK_ORDER.finditer(line):
                findings.append((number, "netlink is host order: " + match.group(0).rstrip("( "), source))
    return findings


def library_files(root):
    return [path for path in sorted((root / "sdk").rglob("*")) if path.suffix in SUFFIXES and path.is_file()]


def check(paths, root, allowed):
    failures = []
    used = set()
    for path in paths:
        relative = path.resolve().relative_to(root.resolve()).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, rule, source in scan_text(text, relative):
            hit = None
            for index, (file_name, needle, _reason) in enumerate(allowed):
                if file_name == relative and needle in source:
                    hit = index
                    break
            if hit is not None and not rule.startswith(("swap", "named swap", "shift swap", "netlink")):
                used.add(hit)
                continue
            failures.append("%s:%d: %s: %s" % (relative, number, rule, source))
    scanned = {path.resolve().relative_to(root.resolve()).as_posix() for path in paths}
    for index, (file_name, needle, _reason) in enumerate(allowed):
        if file_name in scanned and index not in used:
            failures.append("%s: allowed entry matches no line: %s" % (file_name, needle))
    return failures


# --- the red and green controls ---------------------------------------------

CONTROLS = [
    ("the XDP length bug", "sdk/xdp/x.c", "const __u32 udp_len = __builtin_bswap16( udp->len );\n", 1),
    ("bswap_32 from byteswap.h", "sdk/x.cpp", "uint32_t v = bswap_32( w );\n", 1),
    ("glibc __bswap_64", "sdk/x.cpp", "uint64_t v = __bswap_64( w );\n", 1),
    ("msvc _byteswap_ushort", "sdk/x.cpp", "uint16_t v = _byteswap_ushort( w );\n", 1),
    ("apple OSSwapInt32", "sdk/x.cpp", "uint32_t v = OSSwapInt32( w );\n", 1),
    ("a named swap helper call", "sdk/x.cpp", "uint16_t v = rn_swap_bytes16( w );\n", 1),
    ("a named swap helper definition", "sdk/x.cpp", "static uint32_t my_byteswap32( uint32_t w ) { return w; }\n", 1),
    ("swap16 call", "sdk/x.cpp", "uint16_t v = swap16( w );\n", 1),
    ("a variable named swap16 is no call", "sdk/x.cpp", "uint16_t swap16 = 0x1122;\n", 0),
    ("hand-written 16-bit shift swap", "sdk/x.cpp", "uint16_t v = (uint16_t) ( ( ( w << 8 ) & 0xFF00 ) | ( ( w >> 8 ) & 0x00FF ) );\n", 1),
    ("hand-written 32-bit shift swap", "sdk/x.cpp", "uint32_t v = ( w << 24 ) | ( ( w << 8 ) & 0xff0000 ) | ( ( w >> 8 ) & 0xff00 ) | ( w >> 24 );\n", 1),
    ("byte-wise read is no swap", "sdk/x.cpp", "uint16_t v = (uint16_t) ( p[0] | ( p[1] << 8 ) );\n", 0),
    ("byte-wise write is no swap", "sdk/x.cpp", "p[0] = (uint8_t) ( v & 0xff ); p[1] = (uint8_t) ( v >> 8 );\n", 0),
    ("swap under #if ROCKETNET_BIG_ENDIAN", "sdk/x.cpp", "#if ROCKETNET_BIG_ENDIAN\nv = __builtin_bswap32( v );\n#endif\n", 0),
    ("swap in the #else of a byte-order #if", "sdk/x.cpp", "#if ROCKETNET_BIG_ENDIAN\nreturn v;\n#else\nreturn (uint16_t) ( ( ( v << 8 ) & 0xFF00 ) | ( ( v >> 8 ) & 0x00FF ) );\n#endif\n", 0),
    ("swap under __BYTE_ORDER__", "sdk/x.c", "#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n#define N(x) __builtin_bswap16(x)\n#endif\n", 0),
    ("swap nested under a byte-order #if", "sdk/x.c", "#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n#ifdef __GNUC__\nv = __builtin_bswap16( v );\n#endif\n#endif\n", 0),
    ("swap after the byte-order #if ends", "sdk/x.c", "#if ROCKETNET_BIG_ENDIAN\n#endif\nv = __builtin_bswap16( v );\n", 1),
    ("swap under an unrelated #if", "sdk/x.c", "#if defined( __linux__ )\nv = __builtin_bswap16( v );\n#endif\n", 1),
    ("swap in a comment", "sdk/x.c", "// __builtin_bswap16( udp->len )\n/* bswap_32( w ) */\n", 0),
    ("swap in a string", "sdk/x.c", "const char * s = \"__builtin_bswap16\";\n", 0),
    ("swap in the definer file", "sdk/rocketnet_read_write.h", "v = __builtin_bswap16( v );\n", 0),
    ("endian-aware conversions", "sdk/xdp/x.c", "const __u32 n = bpf_ntohs( udp->len ); if ( eth->h_proto == __constant_htons( ETH_P_IP ) ) {}\n", 0),
    ("host read: deref of a u16 cast", "sdk/x.cpp", "uint16_t length = *(const uint16_t *) ( packet + 4 );\n", 1),
    ("host read: deref of a __be16 cast", "sdk/xdp/x.c", "__u16 n = *( __be16 * ) p;\n", 1),
    ("host read: indexed u32 cast", "sdk/x.cpp", "uint32_t w = ( (const uint32_t *) buffer )[2];\n", 1),
    ("host read: memcpy of one integer", "sdk/x.cpp", "memcpy( &sequence, packet + 1, sizeof( uint32_t ) );\n", 1),
    ("an array memcpy is no scalar read", "sdk/x.cpp", "memcpy( counters, last, 8 * sizeof( uint64_t ) );\n", 0),
    ("a cast not dereferenced", "sdk/x.cpp", "rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &counters->values[c], 1 );\n", 0),
    ("an allocation cast", "sdk/x.cpp", "ids = (uint64_t *) rocketnet_malloc( context, n * sizeof( uint64_t ) );\n", 0),
    ("host read under a byte-order #if still fails", "sdk/x.cpp", "#if ROCKETNET_BIG_ENDIAN\nuint16_t n = *(const uint16_t *) p;\n#endif\n", 1),
    ("netlink htons", NETLINK, "header.nlmsg_len = htons( length );\n", 1),
    ("netlink library ntohl", NETLINK, "const uint32_t index = rocketnet_platform_ntohl( value );\n", 1),
    ("netlink host order is fine", NETLINK, "header.nlmsg_len = length;\n", 0),
    ("htons outside netlink is fine", "sdk/x.cpp", "out->sin_port = rocketnet_platform_htons( port );\n", 0),
]


def self_test():
    failed = 0
    for name, relative, text, expected in CONTROLS:
        got = len(scan_text(text, relative))
        verdict = "ok" if got == expected else "FAIL"
        if got != expected:
            failed += 1
        print("self-test %-50s expected %d finding(s), got %d: %s" % (name, expected, got, verdict))

    # the allow list: a listed host read passes, a listed swap does not, and a
    # stale entry fails
    import tempfile
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch)
        (root / "sdk").mkdir()
        listed = root / "sdk" / "a.cpp"
        listed.write_text("uint16_t g = ( (const uint16_t *) &in->sin6_addr )[i];\nv = __builtin_bswap16( v );\n")
        allowed = [("sdk/a.cpp", "&in->sin6_addr", "control"), ("sdk/a.cpp", "__builtin_bswap16", "control"),
                   ("sdk/a.cpp", "no such line", "control")]
        failures = check([listed], root, allowed)
        expected = ["sdk/a.cpp:2: swap __builtin_bswap16: v = __builtin_bswap16( v );",
                    "sdk/a.cpp: allowed entry matches no line: __builtin_bswap16",
                    "sdk/a.cpp: allowed entry matches no line: no such line"]
        verdict = "ok" if failures == expected else "FAIL"
        if failures != expected:
            failed += 1
            for failure in failures:
                print("  got: %s" % failure)
        print("self-test %-50s expected %d failure(s), got %d: %s" % ("the allow list", len(expected), len(failures), verdict))

        # zero inputs: a tree with no library file is red, never "OK (0 files)"
        (root / "empty").mkdir()
        got = main(["check_byte_order.py", "--root", str(root / "empty")])
        verdict = "ok" if got == 1 else "FAIL"
        if got != 1:
            failed += 1
        print("self-test %-50s expected exit 1, got %d: %s" % ("zero library files", got, verdict))

    if failed:
        print("SELF-TEST FAILED %d" % failed)
        return 1
    print("SELF-TEST OK")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    root, args = ROOT, argv[1:]
    if args[:1] == ["--root"] and len(args) > 1:
        root, args = Path(args[1]), args[2:]
    if args and args[0].startswith("-"):
        print(__doc__)
        return 2
    paths = [Path(arg) for arg in args] if args else library_files(root)
    if not paths:
        # zero inputs is an error: an empty tree would otherwise pass unread
        print("BYTE ORDER FAILED: 0 files under %s: nothing was checked" % (root / "sdk"))
        return 1
    failures = check(paths, root, ALLOWED)
    for failure in failures:
        print("FAIL check_byte_order: %s" % failure)
    if failures:
        print("BYTE ORDER FAILED %d" % len(failures))
        return 1
    print("BYTE ORDER OK (%d files)" % len(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
