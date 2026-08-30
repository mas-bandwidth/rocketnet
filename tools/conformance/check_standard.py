#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""STANDARD.md conformance: the document against the implementation.

The family pattern (serialize's tools/conformance): the checker reads the
specification AS A DOCUMENT -- the published tables, parsed as markdown,
with no reference to the source tree -- and compares the rows it NAMES
below against the same numbers as the COMPILER sees them, taken from
dump_constants.cpp built through the real headers.

WHAT IT DOES NOT CHECK, so nobody reads more into a green than is there:
the rows it names are the wire-format ones. The glance table's timing and
sizing rows -- send rate, handshake deadline, cookie lifetime and rotation,
redundant disconnects, replay buffer, used-token history, admitted-token
set -- and the Magic and KDF context-string rows are NOT compared, because
their values live in configuration defaults and key derivation rather than
in a wire constant this file can expand. Adding a row here is cheap and
every one of them is worth adding; until then a green means the named rows
agree, and the count printed at the end says how many that is.

Neither side is allowed to be the only source. A row missing from
STANDARD.md fails as loudly as a mismatched value, because the failure this
guards against is a constant that moved in the code and nowhere else --
and the cheapest way to make that check pass would otherwise be to delete
the row.

Run it from anywhere:  python3 tools/conformance/check_standard.py
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STANDARD = os.path.join(ROOT, "STANDARD.md")
DUMP = os.path.join(HERE, "dump_constants.cpp")


def build_and_run_dump():
    """Compile the constants dump through the real sdk headers and run it."""
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        binary = os.path.join(tmp, "dump_constants")
        compile_command = [
            cxx, "-std=c++11", "-Wall",
            "-I", os.path.join(ROOT, "sdk"), DUMP, "-o", binary,
        ]
        subprocess.run(compile_command, check=True)
        output = subprocess.run([binary], check=True, capture_output=True, text=True).stdout

    values = {}
    for line in output.splitlines():
        if not line.strip():
            continue
        name, _, rest = line.partition(" ")
        values[name] = rest.strip()
    return values


def read_table(text, heading, header_cell):
    """Return the rows of the first markdown table under `heading`.

    Each row is a list of stripped cells. Raises when the heading or its
    table is gone -- a table that vanished is drift, not a pass.
    """
    start = text.find("\n## " + heading + "\n")
    if start < 0:
        raise LookupError("STANDARD.md has no '## %s' section" % heading)
    start = text.index("\n", start + 1) + 1
    rows = []
    seen_header = False
    for line in text[start:].splitlines():
        stripped = line.strip()
        if stripped.startswith("|"):
            cells = [cell.strip() for cell in stripped.strip("|").split("|")]
            if not seen_header:
                if cells[0].lower() != header_cell.lower():
                    raise LookupError(
                        "the table under '%s' starts with %r, expected %r"
                        % (heading, cells[0], header_cell))
                seen_header = True
                continue
            if set("".join(cells)) <= set("-: "):
                continue
            rows.append(cells)
        elif rows:
            break
        elif stripped.startswith("## "):
            break
    if not rows:
        raise LookupError("no table rows found under '## %s'" % heading)
    return rows


def ints(cell):
    # a hyphen between two digits is a RANGE, not a sign: "0-9" is two
    # numbers and "-7 to 4" is a negative and a positive
    return [int(n) for n in re.findall(r"(?<![\d.])-?\d+", cell.replace(",", ""))]


def main():
    with open(STANDARD, "r", encoding="utf-8") as handle:
        text = handle.read()

    code = build_and_run_dump()
    failures = []
    checked = 0

    def compare(what, published, implemented):
        nonlocal checked
        checked += 1
        if str(published) != str(implemented):
            failures.append("%s: STANDARD.md says %s, the implementation says %s"
                            % (what, published, implemented))

    # ---------------- constants at a glance ----------------
    #
    # each row is matched by its Constant cell and read with the parse its
    # own wording needs. A row this checker names and cannot find is a
    # failure: the number moved and the index did not follow.

    glance = {row[0]: row[1] for row in read_table(text, "Constants at a glance", "Constant")}

    def row(label):
        if label not in glance:
            failures.append("STANDARD.md's constants table has no %r row" % label)
            return None
        return glance[label]

    cell = row("Version")
    if cell is not None:
        published = re.search(r"0x[0-9A-Fa-f]+", cell)
        compare("version constant",
                published.group(0).upper().replace("0X", "0x") if published else cell,
                code["ROCKETNET_VERSION"])

    cell = row("Prefilter header")
    if cell is not None:
        compare("prefilter header bytes", ints(cell)[0], code["ROCKETNET_PREFILTER_BYTES"])

    cell = row("Test title constant")
    if cell is not None:
        published = re.search(r"`([0-9a-f ]+)`", cell)
        compare("test filter value",
                published.group(1) if published else cell,
                code["ROCKETNET_TEST_FILTER_VALUE"])
        compare("unkeyed filter epoch", ints(cell)[-1], code["ROCKETNET_FILTER_EPOCH_UNKEYED"])

    cell = row("Packet types")
    if cell is not None:
        published = ints(cell)
        compare("highest packet type", published[1], code["PACKET_TYPE_9"])
        compare("lowest packet type", published[0], code["PACKET_TYPE_0"])
        compare("first reserved type", published[2], int(code["PACKET_TYPE_9"]) + 1)

    cell = row("Client states")
    if cell is not None:
        published = ints(cell)
        compare("lowest client state", published[0], code["ROCKETNET_CLIENT_STATE_MIN"])
        compare("highest client state", published[1], code["ROCKETNET_CLIENT_STATE_MAX"])

    cell = row("Associated data lengths")
    if cell is not None:
        published = ints(cell)
        compare("associated data bytes", published[0], code["ROCKETNET_AD_BYTES"])
        compare("associated data bytes with ephemeral", published[1],
                code["ROCKETNET_AD_WITH_EPHEMERAL_BYTES"])

    cell = row("Per-packet overhead")
    if cell is not None:
        published = ints(cell)
        compare("per-packet overhead", published[0], code["ROCKETNET_PACKET_OVERHEAD_BYTES"])
        compare("minimum ack block", published[1], code["ROCKETNET_ACK_BLOCK_MIN_BYTES"])

    cell = row("Ack block")
    if cell is not None:
        compare("minimum ack block", ints(cell)[0], code["ROCKETNET_ACK_BLOCK_MIN_BYTES"])

    cell = row("Fragment header")
    if cell is not None:
        compare("fragment header bytes", ints(cell)[0], code["ROCKETNET_FRAGMENT_HEADER_BYTES"])

    cell = row("Fragment count")
    if cell is not None:
        compare("maximum fragments", ints(cell)[1], code["ROCKETNET_MAX_FRAGMENTS"])

    cell = row("Send budget")
    if cell is not None:
        published = ints(cell)
        compare("send budget default", published[0], code["ROCKETNET_SEND_BUDGET"])
        compare("send budget maximum", published[1], code["ROCKETNET_RECEIVE_CEILING"])

    cell = row("Receive ceiling")
    if cell is not None:
        compare("receive ceiling", ints(cell)[0], code["ROCKETNET_RECEIVE_CEILING"])

    cell = row("Connect token")
    if cell is not None:
        published = ints(cell)
        compare("connect token bytes", published[0], code["ROCKETNET_CONNECT_TOKEN_BYTES"])
        compare("signed connect data bytes", published[1], code["ROCKETNET_SIGNED_CONNECT_DATA_BYTES"])
        compare("sealed block bytes", published[2], code["ROCKETNET_SEALED_BLOCK_BYTES"])
        compare("private block bytes", published[3], code["ROCKETNET_PRIVATE_BLOCK_BYTES"])

    cell = row("Sequence")
    if cell is not None:
        compare("sequence wire bytes", ints(cell)[0], code["ROCKETNET_SEQUENCE_BYTES"])

    cell = row("Cookie key")
    if cell is not None:
        compare("cookie key bytes", ints(cell)[0], code["ROCKETNET_KEY_BYTES"])

    # ---------------- the packet size table ----------------
    #
    # every type the standard publishes must exist in the implementation
    # with the same number, and its published wire size must be the size
    # the receiver checks: the fixed total for a handshake type, the floor
    # for a session type. A session row also publishes the shared receive
    # ceiling, and that must be the ceiling too.

    sizes = read_table(text, "Packet sizes", "Type")
    published_types = set()
    for cells in sizes:
        packet_type = int(cells[0])
        published_types.add(packet_type)
        numbers = ints(cells[2])
        compare("packet type %d (%s)" % (packet_type, cells[1]),
                packet_type, code["PACKET_TYPE_%d" % packet_type])
        compare("wire size for type %d (%s)" % (packet_type, cells[1]),
                numbers[0], code["WIRE_SIZE_%d" % packet_type])
        if "to" in cells[2]:
            compare("receive ceiling on type %d (%s)" % (packet_type, cells[1]),
                    numbers[1], code["ROCKETNET_RECEIVE_CEILING"])

    implemented_types = {int(name.split("_")[-1]) for name in code if name.startswith("PACKET_TYPE_")}
    missing = sorted(implemented_types - published_types)
    if missing:
        failures.append("the implementation defines packet type(s) %s that "
                        "STANDARD.md's packet size table does not publish"
                        % ", ".join(str(t) for t in missing))

    # ---------------- verdict ----------------

    if failures:
        print("STANDARD.md conformance FAILED (%d of %d checks):" % (len(failures), checked))
        for failure in failures:
            print("  - " + failure)
        print("")
        print("The specification and the implementation disagree. Decide which one "
              "is wrong and fix that one.")
        return 1

    print("STANDARD.md conformance: %d published values agree with the implementation." % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
