#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""STANDARD.md conformance: the document against the implementation.

The family pattern (serialize's tools/conformance): the checker reads the
specification AS A DOCUMENT -- the published tables, parsed as markdown,
with no reference to the source tree -- and compares the rows it NAMES
below against the same numbers as the COMPILER sees them, taken from
dump_constants.cpp built through the real headers.

WHAT IT COMPARES: the wire-format rows, and additionally the Timing
table's rows, the glance table's timing and sizing rows, the KDF
context-string row, and the field names of the two transport configuration
tables. Values read from a header macro or a config-table default are
compared directly. A row the document states and the code cannot produce --
the value is hardcoded rather than declared, or the behaviour is absent --
is a mismatch too, and a mismatch is tolerated ONLY while its id is listed
in known_divergences.tsv. A listed divergence whose document and code now
agree is stale and also fails, so the file must shrink as fixes land.

WHAT IT DOES NOT CHECK, so nobody reads more into a green than is there:
the Magic row -- the standard publishes no default magic and the code ships
none, so there is nothing to compare -- and the table-sizing ratios the code
holds as literals in rocketnet_server_transport.cpp (the used-token history,
admitted-token set and encryption-map multiples of max_clients), which are
listed as known divergences until they are declared in a table this dump can
read. The count printed at the end says how many checks ran.

That list is a promise and it has already been wrong once: the reader at
PR #130's gate found Filter epoch missing from it, and missing for the
worst possible reason -- the epoch IS a wire constant, dump_constants
already printed it, and this file already compared it through the Test
title constant's wording, so the value was checked while its own published
row could say anything at all. It is checked on its own row now. When a row
is excluded, the exclusion has to be true of it.

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

    # the epoch's own row, not only the epoch as the Test title constant's
    # trailing number: a value checked through another row's wording is a
    # row that can say anything (#130's reader)

    cell = row("Filter epoch")
    if cell is not None:
        compare("unkeyed filter epoch (its own row)", ints(cell)[0],
                code["ROCKETNET_FILTER_EPOCH_UNKEYED"])

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

    # ---------------- known divergences ----------------
    #
    # a divergence is tolerated only while its id is listed in the checked-in
    # file; the file must shrink as fixes land, so a listed id whose two
    # sides now agree is a stale-entry failure.

    divergences = {}
    divergence_path = os.path.join(HERE, "known_divergences.tsv")
    if os.path.exists(divergence_path):
        with open(divergence_path, "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.rstrip("\n")
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                fields = line.split("\t")
                divergences[fields[0]] = fields[1] if len(fields) > 1 else "unowned"

    def stated_divergence(identifier, what, published, implemented):
        nonlocal checked
        checked += 1
        if str(published) == str(implemented):
            if identifier in divergences:
                failures.append(
                    "known divergence %r is stale: STANDARD.md and the "
                    "implementation now agree on %s" % (identifier, what))
            return
        if identifier not in divergences:
            failures.append("%s: STANDARD.md says %s, the implementation says %s"
                            % (what, published, implemented))

    # ---------------- the transport configuration tables ----------------
    #
    # the field names come from the real X-macro tables. A per-server
    # timing value the standard calls configuration and the table does not
    # declare is the drift the audit found: it lives hardcoded in the
    # source and no operator can set it.

    server_fields = {name[len("CFG_SERVER_FIELD_"):] for name in code
                     if name.startswith("CFG_SERVER_FIELD_")}
    client_fields = {name[len("CFG_CLIENT_FIELD_"):] for name in code
                     if name.startswith("CFG_CLIENT_FIELD_")}

    def config_field(identifier, table, fields, name):
        nonlocal checked
        checked += 1
        if name in fields:
            if identifier in divergences:
                failures.append(
                    "known divergence %r is stale: the %s config table now "
                    "declares %s" % (identifier, table, name))
        elif identifier not in divergences:
            failures.append(
                "the standard states a configurable %s timing value, but the "
                "%s transport config table does not declare a %r field"
                % (name, table, name))

    config_field("timing.send_rate", "server", server_fields, "send_rate")
    config_field("timing.handshake_deadline", "server", server_fields,
                 "handshake_deadline_seconds")
    config_field("timing.cookie_lifetime", "server", server_fields,
                 "cookie_lifetime_seconds")
    config_field("config.max_token_lifetime_seconds", "server", server_fields,
                 "max_token_lifetime_seconds")
    config_field("config.allow_negative_timeout", "server", server_fields,
                 "allow_negative_timeout")

    # ---------------- the timing table ----------------

    assert "ROCKETNET_HISTORY_PACKETS" in code
    timing = {row[0]: row[1] for row in read_table(text, "Timing", "Value")}

    def timing_row(label):
        if label not in timing:
            failures.append("STANDARD.md's timing table has no %r row" % label)
            return None
        return timing[label]

    cell = timing_row("Redundant disconnect packets")
    if cell is not None:
        compare("redundant disconnect packets (timing table)", ints(cell)[0],
                code["REDUNDANT_DISCONNECT_PACKETS"])
        compare("redundant disconnect packets (client)",
                ints(cell)[0], code["CLIENT_REDUNDANT_DISCONNECT_PACKETS"])

    # the send rate, the handshake deadline and the cookie lifetime are
    # stated as configurable timing values; the code hardcodes them and
    # leaves them out of the config table, so each is a stated divergence.

    cell = timing_row("Send rate")
    if cell is not None:
        stated_divergence("timing.send_rate", "send rate",
                          ints(cell)[0], "hardcoded, not configurable")

    cell = timing_row("Handshake deadline")
    if cell is not None:
        stated_divergence("timing.handshake_deadline", "handshake deadline",
                          ints(cell)[0], "hardcoded, not configurable")

    cell = timing_row("Cookie lifetime")
    if cell is not None:
        stated_divergence("timing.cookie_lifetime", "cookie lifetime",
                          ints(cell)[0], "hardcoded, not configurable")

    cell = timing_row("Cookie key rotation")
    if cell is not None:
        stated_divergence("timing.cookie_rotation", "cookie key rotation",
                          ints(cell)[0], "no rotation is implemented")

    # ---------------- the glance table's sizing and KDF rows ----------------

    cell = row("Redundant disconnects")
    if cell is not None:
        compare("redundant disconnects (glance table)", ints(cell)[0],
                code["REDUNDANT_DISCONNECT_PACKETS"])

    cell = row("Replay buffer")
    if cell is not None:
        compare("replay buffer at least", ints(cell)[0],
                code["ROCKETNET_HISTORY_PACKETS"])
        compare("replay window bits", ints(cell)[0],
                code["ROCKETNET_REPLAY_WINDOW_BITS"])

    cell = row("Used-token history")
    if cell is not None:
        stated_divergence("sizing.used_token_history", "used-token history",
                          ints(cell)[0], "not declared in a table this dump reads")

    cell = row("Admitted-token set")
    if cell is not None:
        stated_divergence("sizing.admitted_token_set", "admitted-token set",
                          ints(cell)[0], "not declared in a table this dump reads")

    # the standard's mapping-table rule: "size the mapping table far above
    # the slot count", ratio documented by the implementation. The code
    # holds a flat 2 x max_clients, which is a ratio at the floor rather
    # than far above it, and is not declared in a table this dump reads.

    stated_divergence("sizing.encryption_map", "encryption map sizing",
                      "far above the slot count", "2 x max_clients")

    cell = row("KDF context strings")
    if cell is not None:
        published = re.findall(r"`([A-Za-z0-9]+)`", cell)
        implemented = [code["KDF_HANDSHAKE_CLIENT_TO_SERVER"],
                       code["KDF_HANDSHAKE_SERVER_TO_CLIENT"],
                       code["KDF_SESSION_CLIENT_TO_SERVER"],
                       code["KDF_SESSION_SERVER_TO_CLIENT"]]
        compare("KDF context strings", published, implemented)

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
