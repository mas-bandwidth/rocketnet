#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""STANDARD.md conformance: the document against the implementation.

The family pattern (serialize's tools/conformance): the checker reads the
specification AS A DOCUMENT -- the published tables, parsed as markdown,
with no reference to the source tree -- and compares the rows it NAMES
below against the same numbers as the COMPILER sees them, taken from
dump_constants.cpp built through the real headers.

WHAT IT COMPARES: the wire-format rows, the Timing table's rows, the glance
table's timing, sizing and key-derivation rows, and the field names of the
two transport configuration tables. Every implemented value arrives through
the toolchain, never as a number written down on this side:

  * the wire constants and the config tables come from dump_constants.cpp,
    compiled through the real headers, so they are expanded macros and the
    real X-macro tables;
  * the timing defaults and the table-sizing ratios come from the server
    transport's own translation unit, run through that same preprocessor and
    read at the assignment and the create call -- a default set through a
    macro arrives as the number the macro holds, and a default that moves in
    the implementation moves here;
  * the key derivation context strings come from sdk/rocketnet_keys.cpp the
    same way, read at the derivation call each one is passed to.

A row the document states and the code cannot produce -- the value is
hardcoded rather than declared, or the behaviour is absent -- is a mismatch
too, and a mismatch is tolerated ONLY while its id is listed in
known_divergences.tsv. A listed divergence whose document and code now agree
is stale and also fails, so the file must shrink as fixes land.

WHAT IT DOES NOT CHECK, so nobody reads more into a green than is there:
the Magic row -- the standard publishes no default magic and the code ships
none, so there is nothing to compare; the config tables' default VALUES and
the reassembly slot default -- both are printed from the real declarations,
but the document publishes no number for either, so there is nothing to
compare them against (a default a config field carries IS reached, when a
protocol default is initialised from that field); and the total connect
deadline and connection timeout rows, which the document states as the
connect token's own lifetime and timeout rather than as constants. The table
sizing ratio for encryption_map (a flat multiple of max_clients against the
standard's "far above the slot count") and cookie key rotation
(unimplemented in code) are listed as known divergences. The count printed at
the end says how many checks ran.

That list is a promise and it has already been wrong twice: the reader at
PR #130's gate found Filter epoch missing from it, and missing for the worst
possible reason -- the epoch IS a wire constant, dump_constants already
printed it, and this file already compared it through the Test title
constant's wording, so the value was checked while its own published row
could say anything at all. It is checked on its own row now. The reader at
attempt 2's gate found the timing and sizing rows compared against literals
written into dump_constants.cpp instead of against the implementation, so
mutating the code left the gate green while the header claimed the values
were read from it. When a row is excluded, the exclusion has to be true of
it; when a row is checked, the value has to come from the code.

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
SDK = os.path.join(ROOT, "sdk")
SERVER_TRANSPORT = os.path.join(SDK, "rocketnet_server_transport.cpp")
KEYS = os.path.join(SDK, "rocketnet_keys.cpp")


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


def preprocess(path):
    """An implementation translation unit as the compiler sees it.

    Macros expanded, comments gone: what comes back is the code that runs.
    A default assigned through a macro arrives as the number the macro
    holds, so nothing on this side of the comparison is a copy of a value
    that could stay still while the implementation moves.
    """
    cxx = os.environ.get("CXX", "c++")
    return subprocess.run(
        [cxx, "-std=c++11", "-E", "-P", "-I", SDK, path],
        check=True, capture_output=True, text=True).stdout


def protocol_default(source, field, code):
    """The value the server transport's create gives protocol->FIELD.

    A literal is that default. `config->name` resolves through the default
    the real config table carries for that field, which dump_constants
    prints from the X-macro table. Anything else -- a computed expression,
    a value this checker would have to guess at -- is refused.
    """
    match = re.search(r"protocol->%s\s*=\s*([^;]+);" % re.escape(field), source)
    if match is None:
        raise LookupError("the server transport never assigns protocol->%s" % field)
    expression = " ".join(match.group(1).split())
    if re.match(r"^-?\d+(\.\d+)?$", expression):
        return expression
    configured = re.match(r"^config->(\w+)$", expression)
    if configured:
        name = configured.group(1)
        key = "CFG_SERVER_DEFAULT_%s" % name
        if key not in code:
            raise LookupError(
                "protocol->%s is initialised from config->%s, whose default "
                "dump_constants does not print" % (field, name))
        return code[key]
    raise LookupError(
        "protocol->%s is initialised from %r, which is neither a literal nor "
        "a config field: the checker refuses to guess a default" % (field, expression))


def sizing_ratio(source, create_function):
    """The multiple of max_clients a server table is created with."""
    match = re.search(r"%s\(\s*context,\s*max_clients\s*\*\s*([^,)]+)"
                      % re.escape(create_function), source)
    if match is None:
        raise LookupError(
            "the server transport creates no %s( context, max_clients * ... )"
            % create_function)
    expression = " ".join(match.group(1).split())
    if not re.match(r"^\d+$", expression):
        raise LookupError(
            "%s sizes from %r, not a plain multiple of max_clients"
            % (create_function, expression))
    return expression


def kdf_context(source, field):
    """The context string the key derivation for keys->FIELD is called with."""
    match = re.search(r"rocketnet_derive_key\(\s*keys->%s\b[^;]*?\"([^\"]+)\"\s*\)"
                      % re.escape(field), source, re.S)
    if match is None:
        raise LookupError(
            "sdk/rocketnet_keys.cpp derives keys->%s with no context string "
            "this checker can read" % field)
    return match.group(1)


def cookie_rotation_seconds(source):
    """The period the server rotates its cookie key at: 0 when it never does.

    The search is for any rotation at all in the server transport's unit.
    When one appears the divergence is fixed and this checker has to read
    its real period, so the refusal is the point: a rotation period of 0
    must not survive the fix silently.
    """
    if re.search(r"rotat", source, re.IGNORECASE):
        raise LookupError(
            "the server transport now mentions key rotation: read its period "
            "here and drop timing.cookie_rotation from known_divergences.tsv")
    return "0"


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
    transport_source = preprocess(SERVER_TRANSPORT)
    keys_source = preprocess(KEYS)
    failures = []
    checked = 0

    def compare(what, published, implemented):
        nonlocal checked
        checked += 1
        if str(published) != str(implemented):
            failures.append("%s: STANDARD.md says %s, the implementation says %s"
                            % (what, published, implemented))

    def compare_number(what, published, implemented):
        nonlocal checked
        checked += 1
        try:
            agrees = float(published) == float(implemented)
        except (TypeError, ValueError):
            agrees = str(published) == str(implemented)
        if not agrees:
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

    # the timing defaults are read where the server transport assigns them,
    # in its own translation unit through the same preprocessor. The
    # divergence that send_rate, handshake_deadline and cookie_lifetime are
    # missing from the transport configuration table is tracked above by
    # config_field; cookie key rotation is unimplemented in code and is
    # tracked as a stated divergence against the 0 that means "never".

    send_rate = protocol_default(transport_source, "send_rate", code)
    handshake_deadline = protocol_default(transport_source, "handshake_deadline_seconds", code)
    cookie_lifetime = protocol_default(transport_source, "cookie_lifetime_seconds", code)
    cookie_rotation = cookie_rotation_seconds(transport_source)

    cell = timing_row("Send rate")
    if cell is not None:
        compare_number("send rate (timing table)", ints(cell)[0], send_rate)

    cell = timing_row("Handshake deadline")
    if cell is not None:
        compare_number("handshake deadline (timing table)", ints(cell)[0], handshake_deadline)

    cell = timing_row("Cookie lifetime")
    if cell is not None:
        compare_number("cookie lifetime (timing table)", ints(cell)[0], cookie_lifetime)

    cell = timing_row("Cookie key rotation")
    if cell is not None:
        stated_divergence("timing.cookie_rotation", "cookie key rotation",
                          ints(cell)[0], cookie_rotation)

    # ---------------- the glance table's timing and sizing rows ----------------

    cell = row("Send rate")
    if cell is not None:
        compare_number("send rate (glance table)", ints(cell)[0], send_rate)

    cell = row("Handshake deadline")
    if cell is not None:
        compare_number("handshake deadline (glance table)", ints(cell)[0], handshake_deadline)

    cell = row("Cookie lifetime / key rotation")
    if cell is not None:
        numbers = ints(cell)
        compare_number("cookie lifetime (glance table)", numbers[0], cookie_lifetime)
        stated_divergence("timing.cookie_rotation", "cookie key rotation (glance table)",
                          numbers[1], cookie_rotation)

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

    # the sizing ratios are read where the server transport creates the
    # tables, so the multiple of max_clients each one holds is the one the
    # implementation compiles -- a function-local ratio macro arrives here
    # already expanded to its number.

    used_token_history_ratio = sizing_ratio(transport_source,
                                            "rocketnet_used_token_history_create")
    admitted_token_set_ratio = sizing_ratio(transport_source,
                                            "rocketnet_admitted_token_set_create")
    encryption_map_ratio = sizing_ratio(transport_source,
                                        "rocketnet_encryption_map_create")

    cell = row("Used-token history")
    if cell is not None:
        compare_number("used-token history entries per slot", ints(cell)[0],
                       used_token_history_ratio)

    cell = row("Admitted-token set")
    if cell is not None:
        compare_number("admitted-token set multiple of the slot count",
                       ints(cell)[0], admitted_token_set_ratio)

    # the standard's mapping-table rule: "size the mapping table far above
    # the slot count". The implementation's multiple is read above; the
    # divergence is that a flat small multiple is at the floor of that rule
    # rather than far above it, and the document states no number to compare.

    stated_divergence("sizing.encryption_map", "encryption map sizing",
                      "far above the slot count",
                      "%s x max_clients" % encryption_map_ratio)

    # ---------------- the key derivation context strings ----------------
    #
    # the strings are literals at the derivation calls in the
    # implementation's own unit; the row publishes all four, in the order
    # the standard's derivation sections use them.

    implemented_contexts = [kdf_context(keys_source, field) for field in
                            ("handshake_client_to_server", "handshake_server_to_client",
                             "session_client_to_server", "session_server_to_client")]

    cell = row("KDF context strings")
    if cell is not None:
        published = re.findall(r"`([^`]+)`", cell)
        if not published:
            failures.append("STANDARD.md's KDF context strings row publishes no strings")
        else:
            compare("key derivation context strings", " ".join(published),
                    " ".join(implemented_contexts))

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
