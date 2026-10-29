#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""The RocketNet standard (standard/*.md) against the implementation.

The checker reads RN-1 (standard/TRANSPORT.md), RN-2 (standard/MESSAGES.md)
and RN-R (standard/RATIONALE.md) as documents, and the outline from
standard/STYLE.md. It reads nothing else of the source tree but the pin files
(standard/pins/*.tsv) and the vector twins (test_unit_standard_*.cpp). It
checks, and fails with one line per fault naming the section id:

  outline     every document's section headings are the outline's, in order,
              each with its anchor; RN-R adds 'rh Revision history'
  pins        every pin row's sdk expression, compiled through the real
              headers in one generated translation unit, equals the pin
              file's value and the value cell of the document row it names;
              every document row with a value has a pin, and an
              #unpinned line fails; every pin lies in a section its file owns
  vectors     every 'vector <id>' block is well formed, sits in the section
              its id names, and equals the RN_STANDARD_VECTOR twin of that id
              in a test file; every twin has a block; a twin counts only
              outside comments, in a file that calls rn_standard_vectors_walk
  index       each Appendix A vector index is the one the vectors imply
  references  every link is a section link that resolves and carries its
              target's heading; no bare section id in running text
  language    RN-1 and RN-2 use no lower-case 'must', 'shall', 'should' or
              'required', and cite no file path, TLA+ module or test
              function; RN-R and RN-1 tc carry no capitalised BCP 14 key word

Pending sections ('Status: pending') are listed, never failed.

    python3 tools/conformance/check_rn_standard.py
    python3 tools/conformance/check_rn_standard.py --section t3
    python3 tools/conformance/check_rn_standard.py --print-index

Exit 0 green, 1 a fault, 2 a usage or build error.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STANDARD_DIR = os.path.join(ROOT, "standard")
PINS_DIR = os.path.join(STANDARD_DIR, "pins")

DOCUMENTS = {
    "RN-1": "TRANSPORT.md",
    "RN-2": "MESSAGES.md",
    "RN-R": "RATIONALE.md",
}
FILE_TAG = {name: tag for tag, name in DOCUMENTS.items()}

# The translation unit every pin compiles in: the headers the constants
# live in, and the two units whose defaults and labels are macros, included
# under the guard that keeps only their declarations (as dump_constants.cpp
# does). A pin file adds headers with '#include' comment lines.
BASE_INCLUDES = """
#include "rocketnet.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_fragment.h"
#include "rocketnet_history.h"
#include "rocketnet_replay.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_send_budget.h"
#define ROCKETNET_CONFORMANCE_DEFAULTS_ONLY
#include "rocketnet_server_transport.cpp"
#include "rocketnet_keys.cpp"
#undef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY
#include "sodium/sodium.h"

// the configuration tables' defaults, by field name: rn_pin_server_default_<field>,
// rn_pin_client_default_<field>, rn_pin_message_default_<field>. A bytes field
// has no scalar default and no name here.
#define RN_PIN_INT_( tag, name, def ) static const long long rn_pin_##tag##_default_##name = (long long) ( def );
#define RN_PIN_STR_( tag, name, def ) static const char * const rn_pin_##tag##_default_##name = def;
#define RN_PIN_S_INT( name, def, min, max, flags )          RN_PIN_INT_( server, name, def )
#define RN_PIN_S_UINT64( name, def, flags )                 RN_PIN_INT_( server, name, def )
#define RN_PIN_S_BYTES( name, size, def, flags )
#define RN_PIN_S_STRING( name, capacity, def, flags )       RN_PIN_STR_( server, name, def )
#define RN_PIN_C_INT( name, def, min, max, flags )          RN_PIN_INT_( client, name, def )
#define RN_PIN_C_UINT64( name, def, flags )                 RN_PIN_INT_( client, name, def )
#define RN_PIN_C_BYTES( name, size, def, flags )
#define RN_PIN_C_STRING( name, capacity, def, flags )       RN_PIN_STR_( client, name, def )
#define RN_PIN_M_INT( name, def, min, max, flags )          RN_PIN_INT_( message, name, def )
#define RN_PIN_M_UINT64( name, def, flags )                 RN_PIN_INT_( message, name, def )
#define RN_PIN_M_BYTES( name, size, def, flags )
#define RN_PIN_M_STRING( name, capacity, def, flags )       RN_PIN_STR_( message, name, def )
ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( RN_PIN_S_INT, RN_PIN_S_UINT64, RN_PIN_S_BYTES, RN_PIN_S_STRING )
ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( RN_PIN_C_INT, RN_PIN_C_UINT64, RN_PIN_C_BYTES, RN_PIN_C_STRING )
ROCKETNET_MESSAGE_CONFIG_TABLE( RN_PIN_M_INT, RN_PIN_M_UINT64, RN_PIN_M_BYTES, RN_PIN_M_STRING )
"""

PRINTERS = r"""
#include <stdio.h>
#include <type_traits>

static void rn_pin_print( int row, const char * value ) { printf( "%d\ts\t%s\n", row, value ); }
static void rn_pin_print( int row, double value ) { printf( "%d\tr\t%.17g\n", row, value ); }
static void rn_pin_print( int row, float value ) { printf( "%d\tr\t%.9g\n", row, (double) value ); }
template <typename T> static void rn_pin_print( int row, T value )
{
    static_assert( std::is_integral<T>::value || std::is_enum<T>::value, "a pin expression is an integer, a real or a string" );
    if ( (long long) value < 0 )
        printf( "%d\ti\t%lld\n", row, (long long) value );
    else
        printf( "%d\ti\t%llu\n", row, (unsigned long long) value );
}
"""

SECTION_ID = r"(?:t\d{1,2}|t[abc]|m\d{1,2}|ma|rh)"
ANCHOR_RE = re.compile(r'^<a id="(' + SECTION_ID + r'(?:\.\d+)*)"></a>\s*$')
HEADING_RE = re.compile(r"^(#{2,4}) (" + SECTION_ID + r"(?:\.\d+)*)\. (.+?)\s*$")
FENCE_RE = re.compile(r"^```(.*)$")
HEX_ROW_RE = re.compile(r"^[0-9a-f]{2}( [0-9a-f]{2}){0,15}$")
VECTOR_ID_RE = re.compile(r"^(" + SECTION_ID + r")\.[a-z0-9-]+$")
LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)\s]*)\)")
TWIN_RE = re.compile(r'(?<![A-Za-z0-9_])RN_STANDARD_VECTOR\(\s*"([^"]+)"\s*,\s*((?:"[0-9a-fA-F ]*"\s*)+)\)')
BARE_ID_RE = re.compile(r"(?<![\w.#/`-])(" + SECTION_ID + r")(?:\.\d+)*(?![\w-])")
KEYWORDS_RE = re.compile(r"\b(MUST NOT|MUST|REQUIRED|SHALL NOT|SHALL|SHOULD NOT|SHOULD|NOT RECOMMENDED|RECOMMENDED|MAY|OPTIONAL)\b")
LOWER_RE = re.compile(r"\b(must|shall|should|required)\b")
PATH_RE = re.compile(r"\b[\w/.-]+\.(?:cpp|h|c|py|tla|tsv|json|yml)\b|\b\w+\.\w+:\d+\b")
TLA_RE = re.compile(r"\bTLA\+|\bTLC\b")
TEST_RE = re.compile(r"\btest_\w+")
INDEX_BEGIN = "<!-- vector-index:begin -->"
INDEX_END = "<!-- vector-index:end -->"


class Fault(Exception):
    pass


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def read_outline():
    text = read(os.path.join(STANDARD_DIR, "STYLE.md"))
    if "<!-- outline:begin -->" not in text or "<!-- outline:end -->" not in text:
        raise Fault("STYLE.md: no outline markers")
    block = text.split("<!-- outline:begin -->", 1)[1].split("<!-- outline:end -->", 1)[0]
    outline = []
    for line in block.strip().splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 3 or cells[0] in ("Id",) or set("".join(cells)) <= set("-: "):
            continue
        outline.append((cells[0], cells[1], cells[2]))
    if not outline:
        raise Fault("STYLE.md: the outline is empty")
    return outline


class Section:
    def __init__(self, ident, heading, level, line):
        self.id = ident
        self.heading = heading
        self.level = level
        self.line = line
        self.lines = []          # (line number, text) of the body, subsections included for a top-level section
        self.anchored = False

    @property
    def pending(self):
        return any(text.strip() == "Status: pending" for _, text in self.lines)


class Document:
    """One markdown document split into sections, with its code fences."""

    def __init__(self, tag, path):
        self.tag = tag
        self.path = path
        self.name = os.path.basename(path)
        self.text = read(path)
        self.lines = self.text.split("\n")
        self.sections = []       # top-level, in order
        self.anchors = {}        # id -> Section (top-level and subsections)
        self.fences = []         # (section id, info, [body lines], line number)
        self._parse()

    def _parse(self):
        top = None
        pending_anchor = None
        in_fence = False
        fence_info = None
        fence_body = []
        fence_line = 0
        for number, line in enumerate(self.lines, 1):
            if in_fence:
                if line.startswith("```"):
                    self.fences.append((top.id if top else None, fence_info, fence_body, fence_line))
                    in_fence = False
                else:
                    fence_body.append(line)
                if top:
                    top.lines.append((number, line))
                continue
            match = FENCE_RE.match(line)
            if match:
                in_fence = True
                fence_info = match.group(1).strip()
                fence_body = []
                fence_line = number
                if top:
                    top.lines.append((number, line))
                continue
            match = ANCHOR_RE.match(line)
            if match:
                pending_anchor = match.group(1)
                continue
            match = HEADING_RE.match(line)
            if match:
                level, ident, heading = len(match.group(1)), match.group(2), match.group(3)
                section = Section(ident, heading, level, number)
                section.anchored = pending_anchor == ident
                pending_anchor = None
                self.anchors[ident] = section
                if level == 2:
                    self.sections.append(section)
                    top = section
                    continue
            pending_anchor = None
            if top:
                top.lines.append((number, line))

    def section_of_line(self, number):
        owner = None
        for section in self.sections:
            if section.line <= number:
                owner = section
        return owner.id if owner else None


def strip_inline(text):
    """Running text only: no inline code and no links."""
    text = re.sub(r"`[^`]*`", " ", text)
    text = LINK_RE.sub(" ", text)
    return text


# ---------------------------------------------------------------------------
# outline


def check_outline(docs, outline, faults, only):
    for tag in ("RN-1", "RN-2", "RN-R"):
        doc = docs[tag]
        if tag == "RN-R":
            expected = [(i, h) for i, h, _ in outline] + [("rh", "Revision history")]
        else:
            expected = [(i, h) for i, h, d in outline if d == tag]
        found = [(s.id, s.heading) for s in doc.sections]
        for position, (ident, heading) in enumerate(expected):
            if only and ident != only:
                continue
            if ident not in doc.anchors or doc.anchors[ident].level != 2:
                faults.append((ident, "%s has no section '## %s. %s'" % (doc.name, ident, heading)))
                continue
            section = doc.anchors[ident]
            if section.heading != heading:
                faults.append((ident, "%s:%d heading is %r, the outline says %r"
                               % (doc.name, section.line, section.heading, heading)))
            if not section.anchored:
                faults.append((ident, '%s:%d heading has no <a id="%s"></a> line above it'
                               % (doc.name, section.line, ident)))
            if position >= len(found) or found[position][0] != ident:
                faults.append((ident, "%s: section %s is out of the outline's order" % (doc.name, ident)))
            matches = [candidate for candidate in doc.sections if candidate.id == ident]
            if len(matches) > 1:
                faults.append((ident, "%s:%d section %s appears %d times; outline sections appear exactly once"
                               % (doc.name, matches[1].line, ident, len(matches))))
        if not only:
            known = {i for i, _ in expected}
            for section in doc.sections:
                if section.id not in known:
                    faults.append((section.id, "%s:%d section %s is not in the outline"
                                   % (doc.name, section.line, section.id)))


# ---------------------------------------------------------------------------
# pins


def parse_value(text):
    text = text.strip().strip("`").strip()
    if text == "none":
        return ("none", None)
    if re.fullmatch(r'"[^"]*"', text):
        return ("s", text[1:-1])
    if re.fullmatch(r"-?0x[0-9A-Fa-f]+", text):
        return ("n", int(text, 16))
    if re.fullmatch(r"-?\d+", text):
        return ("n", int(text))
    if re.fullmatch(r"-?\d+\.\d*(e-?\d+)?|-?\d+e-?\d+", text):
        value = float(text)
        return ("n", int(value)) if value == int(value) else ("n", value)
    return ("?", text)


def compiled_value(kind, text):
    if kind == "s":
        return ("s", text)
    if kind == "i":
        return ("n", int(text))
    value = float(text)
    return ("n", int(value)) if value == int(value) else ("n", value)


def show(value):
    kind, v = value
    return '"%s"' % v if kind == "s" else str(v)


def pin_document(path):
    base = os.path.splitext(os.path.basename(path))[0]
    if base == "transport":
        return "RN-1"
    if base == "messages":
        return "RN-2"
    if re.fullmatch(r"t\d{1,2}|t[abc]", base):
        return "RN-1"
    if re.fullmatch(r"m\d{1,2}|ma", base):
        return "RN-2"
    return None


def read_pins(faults):
    pins = []
    includes = []
    unpinned = []
    for path in sorted(glob.glob(os.path.join(PINS_DIR, "*.tsv"))):
        name = os.path.basename(path)
        if name == "retired.tsv":
            continue
        tag = pin_document(path)
        if tag is None:
            faults.append((None, "pins/%s: a pin file is transport.tsv, messages.tsv or <section id>.tsv" % name))
            continue
        for number, line in enumerate(read(path).split("\n"), 1):
            if not line.strip():
                continue
            if line.startswith("#unpinned"):
                fields = line.split("\t")
                if len(fields) != 3 or not fields[1].strip() or not fields[2].strip():
                    faults.append((None, "pins/%s:%d: an #unpinned line is '#unpinned<TAB>name<TAB>reason'" % (name, number)))
                else:
                    unpinned.append({"file": name, "line": number, "doc": tag, "name": fields[1].strip()})
                continue
            if line.startswith("#"):
                if line.startswith("#include"):
                    includes.append(line.strip())
                continue
            fields = line.split("\t")
            if len(fields) != 3 or not all(f.strip() for f in fields):
                faults.append((None, "pins/%s:%d: a pin row is three tab-separated fields" % (name, number)))
                continue
            pins.append({"file": name, "line": number, "doc": tag,
                         "name": fields[0].strip(), "expr": fields[1].strip(),
                         "value": fields[2].strip()})
    return pins, includes, unpinned


def owned_sections(path):
    """The sections a pin file may hold rows of (standard/STYLE.md section 10)."""
    base = os.path.splitext(os.path.basename(path))[0]
    if base == "transport":
        return {"t1", "t2", "t3"}
    if base == "messages":
        return {"m1"}
    return {base}


def document_rows(doc):
    """name -> (value cell, line, section id) for every table with Name and Value columns."""
    rows = {}
    duplicates = []
    header = None
    in_fence = False
    for number, line in enumerate(doc.lines, 1):
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        stripped = line.strip()
        if in_fence or not stripped.startswith("|"):
            header = None
            continue
        cells = [c.strip() for c in stripped.strip("|").split("|")]
        if header is None:
            header = [c.lower() for c in cells]
            continue
        if set("".join(cells)) <= set("-: "):
            continue
        if "name" not in header or "value" not in header:
            continue
        name = cells[header.index("name")].strip("`")
        value = cells[header.index("value")] if header.index("value") < len(cells) else ""
        if not value.strip():
            continue
        if name in rows:
            duplicates.append((name, number))
        rows[name] = (value, number, doc.section_of_line(number))
    return rows, duplicates


def compile_pins(pins, includes):
    lines = [BASE_INCLUDES] + includes + [PRINTERS, "int main()", "{"]
    for index, pin in enumerate(pins):
        lines.append("    rn_pin_print( %d, ( %s ) );" % (index, pin["expr"]))
    lines += ["    return 0;", "}", ""]
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "rn_standard_pins.cpp")
        binary = os.path.join(tmp, "rn_standard_pins")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines))
        command = [cxx, "-std=c++11", "-w", "-I", os.path.join(ROOT, "sdk"), "-I", ROOT, source, "-o", binary]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            raise Fault("the pin translation unit does not compile (a pin expression the headers do not declare?)")
        output = subprocess.run([binary], check=True, capture_output=True, text=True).stdout
    values = {}
    for line in output.splitlines():
        index, kind, text = line.split("\t", 2)
        values[int(index)] = compiled_value(kind, text)
    return values


def check_pins(docs, pins, includes, unpinned, faults, only):
    rows = {}
    for tag in ("RN-1", "RN-2"):
        rows[tag], duplicates = document_rows(docs[tag])
        for name, number in duplicates:
            faults.append((docs[tag].section_of_line(number),
                           "%s:%d two table rows are named %r" % (docs[tag].name, number, name)))
    selected = []
    pinned_names = {(pin["doc"], pin["name"]) for pin in pins}
    for entry in unpinned:
        row = rows[entry["doc"]].get(entry["name"])
        section = row[2] if row else None
        if only and section != only:
            continue
        faults.append((section, "pins/%s:%d #unpinned %r: every row with a value is pinned; a declaration is not a pin"
                       % (entry["file"], entry["line"], entry["name"])))
    for tag in ("RN-1", "RN-2"):
        for name, (cell, number, section) in rows[tag].items():
            if only and section != only:
                continue
            if parse_value(cell)[0] == "none":
                continue
            if (tag, name) not in pinned_names:
                faults.append((section, "%s:%d row %r has a value and no pin; add it to pins/%s.tsv"
                               % (DOCUMENTS[tag], number, name, section)))
    for pin in pins:
        row = rows[pin["doc"]].get(pin["name"])
        pin["row"] = row
        section = row[2] if row else None
        if only and section != only:
            continue
        selected.append(pin)
        if row is None:
            faults.append((None, "pins/%s:%d %r is not a Name of any table row in %s"
                           % (pin["file"], pin["line"], pin["name"], DOCUMENTS[pin["doc"]])))
        elif section not in owned_sections(pin["file"]):
            faults.append((section, "pins/%s:%d %r lies in section %s, which this pin file does not own"
                           % (pin["file"], pin["line"], pin["name"], section)))
    if not selected:
        return 0
    compiled = compile_pins(selected, includes)
    for index, pin in enumerate(selected):
        if pin["row"] is None:
            continue
        cell, number, section = pin["row"]
        published = parse_value(cell)
        pinned = parse_value(pin["value"])
        code = compiled[index]
        where = "%s:%d" % (DOCUMENTS[pin["doc"]], number)
        if published[0] == "none":
            faults.append((section, "%s row %r is pinned but its Value is none" % (where, pin["name"])))
            continue
        if published[0] == "?":
            faults.append((section, "%s row %r: the Value cell %r is not one value" % (where, pin["name"], cell)))
            continue
        if published != pinned:
            faults.append((section, "%s row %r: the document says %s, pins/%s says %s"
                           % (where, pin["name"], cell.strip().strip("`"), pin["file"], pin["value"])))
        if pinned != code:
            faults.append((section, "%s row %r: pins/%s says %s, the implementation's %s is %s"
                           % (where, pin["name"], pin["file"], pin["value"], pin["expr"], show(code))))
    return len(selected)


# ---------------------------------------------------------------------------
# vectors


def document_vectors(docs, faults):
    vectors = {}
    for tag in ("RN-1", "RN-2", "RN-R"):
        doc = docs[tag]
        for section, info, body, number in doc.fences:
            if not info.startswith("vector"):
                continue
            where = "%s:%d" % (doc.name, number)
            parts = info.split()
            if len(parts) != 2 or parts[0] != "vector" or not VECTOR_ID_RE.match(parts[1]):
                faults.append((section, "%s vector block info %r is not 'vector <section id>.<name>'" % (where, info)))
                continue
            ident = parts[1]
            if tag == "RN-R":
                faults.append((section, "%s vector %s is in RN-R; a vector sits in its normative section" % (where, ident)))
                continue
            if VECTOR_ID_RE.match(ident).group(1) != section:
                faults.append((section, "%s vector %s sits in section %s, not the section its id names"
                               % (where, ident, section)))
            rows = [line for line in body if line.strip()]
            hex_rows = []
            seen_hex = False
            for line in rows:
                if HEX_ROW_RE.match(line):
                    seen_hex = True
                    hex_rows.append(line)
                elif seen_hex:
                    faults.append((section, "%s vector %s: %r after the bytes; inputs come first" % (where, ident, line)))
                elif not re.match(r"^[a-z][\w -]*: ", line):
                    faults.append((section, "%s vector %s: input line %r is not 'name: value'" % (where, ident, line)))
            if not hex_rows:
                faults.append((section, "%s vector %s has no bytes" % (where, ident)))
            for row in hex_rows[:-1]:
                if len(row.split()) != 16:
                    faults.append((section, "%s vector %s: a row before the last holds %d bytes, not 16"
                                   % (where, ident, len(row.split()))))
            if ident in vectors:
                faults.append((section, "%s vector id %s is used twice" % (where, ident)))
            vectors[ident] = {"section": section, "doc": tag, "where": where,
                              "hex": "".join("".join(r.split()) for r in hex_rows)}
    return vectors


def strip_c_comments(source):
    """Source with comments blanked, string literals kept: a twin in a comment is no twin."""
    return re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda m: m.group(0) if m.group(0).startswith('"') else " ", source, flags=re.S)


def test_twins(faults):
    twins = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "tests", "test_unit_standard_*.cpp"))):
        name = os.path.basename(path)
        source = strip_c_comments(read(path))
        matches = list(TWIN_RE.finditer(source))
        if matches and "rn_standard_vectors_walk(" not in source:
            faults.append((None, "%s declares vector twins and never calls rn_standard_vectors_walk" % name))
        for match in matches:
            ident = match.group(1)
            hexes = "".join(re.findall(r'"([^"]*)"', match.group(2)))
            hexes = hexes.replace(" ", "").lower()
            if ident in twins:
                faults.append((ident.split(".")[0], "%s: twin %s is declared twice" % (name, ident)))
            twins[ident] = {"file": name, "hex": hexes}
    return twins


def check_vectors(vectors, twins, faults, only):
    count = 0
    for ident, vector in vectors.items():
        if only and vector["section"] != only:
            continue
        count += 1
        twin = twins.get(ident)
        if twin is None:
            faults.append((vector["section"], "%s vector %s has no RN_STANDARD_VECTOR twin in any test_unit_standard_*.cpp"
                           % (vector["where"], ident)))
            continue
        a, b = vector["hex"], twin["hex"]
        if a != b:
            first = next((i for i in range(0, min(len(a), len(b)), 2) if a[i:i + 2] != b[i:i + 2]), min(len(a), len(b)))
            faults.append((vector["section"], "%s vector %s differs from its twin in %s at byte %d (document %s, test %s; %d and %d bytes)"
                           % (vector["where"], ident, twin["file"], first // 2,
                              a[first:first + 2] or "end", b[first:first + 2] or "end", len(a) // 2, len(b) // 2)))
    for ident, twin in twins.items():
        section = ident.split(".")[0]
        if only and section != only:
            continue
        if ident not in vectors:
            faults.append((section, "%s: twin %s has no vector block in any document" % (twin["file"], ident)))
    return count


def check_counter_registry(docs, faults, only):
    """The t18 counter table must be the SDK's registry, in index order.

    The t18 table (Name, Class, Family columns) is the observable contract of
    sdk/rocketnet_counters.h: a reason in one and not the other fails, a
    reason renamed or reordered fails, and a class or family that differs
    fails. DONE-WHEN (4) of the timing-and-counters card needs the table
    checked against the SDK registry, not just against its own vector.
    """
    doc = docs["RN-1"]
    section = doc.anchors.get("t18")
    if section is None or section.pending:
        return
    if only and only != "t18":
        return

    registry = []
    path = os.path.join(ROOT, "sdk", "rocketnet_counters.h")
    text = read(path).replace("\\\n", "")
    match = re.search(r'define ROCKETNET_COUNTER_ROWS\( X \)\s*(.*?)\n#undef', text, re.S)
    if not match:
        faults.append(("t18", "rocketnet_counters.h: no ROCKETNET_COUNTER_ROWS macro"))
        return
    klass_lower = {"REFUSAL": "refusal", "EXPECTED": "expected",
                   "PATH_LOSS": "path_loss", "ARRIVAL": "arrival"}
    for name, klass, family in re.findall(r'X\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', match.group(1)):
        registry.append((name, klass_lower.get(klass, klass.lower()), "none" if family == "NONE" else family))

    table = []
    in_fence = False
    in_table = False
    for number, line in section.lines:
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        stripped = line.strip()
        if not stripped.startswith("|"):
            in_table = False
            continue
        cells = [c.strip() for c in stripped.strip("|").split("|")]
        header = [c.lower().strip("`") for c in cells]
        if not in_table and header[:1] == ["name"] and "class" in header and "family" in header:
            in_table = True
            continue
        if not in_table or len(cells) < 3:
            continue
        if set("".join(cells)) <= set("-: "):
            continue
        name = cells[0].strip("`")
        klass = cells[1].strip("`")
        family = cells[2].strip("`")
        table.append((name, klass, family))

    if table != registry:
        faults.append(("t18", "the counter table differs from sdk/rocketnet_counters.h (%d table rows, %d registry rows)"
                       % (len(table), len(registry))))
        for index, (a, b) in enumerate(zip(table, registry)):
            if a != b:
                faults.append(("t18", "counter %d: table has %r, the registry has %r" % (index, a, b)))
                break


def expected_index(doc, vectors):
    entries = [(v["section"], i) for i, v in vectors.items() if v["doc"] == doc.tag]
    order = {s.id: n for n, s in enumerate(doc.sections)}
    entries.sort(key=lambda e: (order.get(e[0], 999), e[1]))
    if not entries:
        return "No vectors yet."
    lines = ["| Section | Vector |", "|---|---|"]
    for section, ident in entries:
        heading = doc.anchors[section].heading if section in doc.anchors else section
        lines.append("| [%s %s](#%s) | `%s` |" % (section, heading, section, ident))
    return "\n".join(lines)


def print_index(docs, vectors):
    for tag, appendix in (("RN-1", "ta"), ("RN-2", "ma")):
        doc = docs[tag]
        print("%s Appendix A vector index (%s)" % (appendix, doc.name))
        print(expected_index(doc, vectors))


def check_index(docs, vectors, faults, only):
    for tag, appendix in (("RN-1", "ta"), ("RN-2", "ma")):
        if only and only != appendix:
            continue
        doc = docs[tag]
        if INDEX_BEGIN in doc.text or INDEX_END in doc.text:
            faults.append((appendix, "%s still holds vector index markers; the index is printed by --print-index" % doc.name))


# ---------------------------------------------------------------------------
# references and language


def running_lines(doc, section):
    """(line number, text) of a section's running text: no fences, anchors, headings or index."""
    out = []
    in_fence = False
    in_index = False
    for number, line in section.lines:
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if INDEX_BEGIN in line:
            in_index = True
        if INDEX_END in line:
            in_index = False
            continue
        if in_fence or in_index or ANCHOR_RE.match(line) or HEADING_RE.match(line):
            continue
        out.append((number, line))
    return out


def check_references(docs, faults, only):
    by_file = {doc.name: doc for doc in docs.values()}
    for tag in ("RN-1", "RN-2", "RN-R"):
        doc = docs[tag]
        for section in doc.sections:
            if only and section.id != only:
                continue
            for number, line in section.lines:
                for match in LINK_RE.finditer(line):
                    text, target = match.group(1), match.group(2)
                    file_part, hash_mark, anchor = target.partition("#")
                    if not hash_mark or (file_part and file_part not in by_file):
                        faults.append((section.id, "%s:%d link %r is not a link to a section of the three documents"
                                       % (doc.name, number, target)))
                        continue
                    target_doc = by_file[file_part] if file_part else doc
                    where = "%s:%d" % (doc.name, number)
                    if anchor not in target_doc.anchors:
                        faults.append((section.id, "%s link %r: %s has no section %s" % (where, target, target_doc.name, anchor)))
                        continue
                    heading = target_doc.anchors[anchor].heading
                    want = "%s %s" % (anchor, heading)
                    if file_part and target_doc is not doc:
                        want = "%s %s" % (target_doc.tag, want)
                    if text != want:
                        faults.append((section.id, "%s link to %s reads %r, its text is %r" % (where, target, text, want)))
            for number, line in running_lines(doc, section):
                plain = strip_inline(line)
                match = BARE_ID_RE.search(plain)
                if match:
                    faults.append((section.id, "%s:%d bare section id %r; link it with its heading"
                                   % (doc.name, number, match.group(0))))


def check_language(docs, faults, only):
    for tag in ("RN-1", "RN-2", "RN-R"):
        doc = docs[tag]
        for section in doc.sections:
            if only and section.id != only:
                continue
            informative = tag == "RN-R" or section.id == "tc"
            for number, line in running_lines(doc, section):
                plain = strip_inline(line)
                where = "%s:%d" % (doc.name, number)
                if informative:
                    match = KEYWORDS_RE.search(plain)
                    if match and tag == "RN-R":
                        faults.append((section.id, "%s informative text uses the key word %r" % (where, match.group(1))))
                    elif match and not section.pending:
                        faults.append((section.id, "%s informative text uses the key word %r" % (where, match.group(1))))
                    continue
                # Inline code may contain a field named "required", but it
                # must not hide an implementation citation from the standard.
                citations = LINK_RE.sub(" ", line)
                for pattern, what in ((LOWER_RE, "lower-case requirement word"),
                                      (PATH_RE, "file citation"),
                                      (TLA_RE, "model citation"),
                                      (TEST_RE, "test citation")):
                    match = pattern.search(plain if pattern is LOWER_RE else citations)
                    if match:
                        faults.append((section.id, "%s %s %r in normative text" % (where, what, match.group(0))))


# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description="Check the RocketNet standard against the implementation.")
    parser.add_argument("--section", help="check one section id only, and print whether it is pending or written")
    parser.add_argument("--print-index", action="store_true", help="print each Appendix A vector index")
    parser.add_argument("--write-index", action="store_true", help="alias for --print-index; writes no file")
    args = parser.parse_args()

    faults = []
    try:
        outline = read_outline()
        docs = {tag: Document(tag, os.path.join(STANDARD_DIR, name)) for tag, name in DOCUMENTS.items()}
        only = args.section
        if only is not None:
            known = {i for i, _, _ in outline} | {"rh"}
            if only not in known:
                print("unknown section id %s" % only)
                return 2
            owner = next((d for d in ("RN-1", "RN-2") if only in docs[d].anchors), "RN-R")
            section = docs[owner].anchors.get(only)
            print("%s %s" % (only, "pending" if section is None or section.pending else "written"))

        check_outline(docs, outline, faults, only)
        pins, includes, unpinned_rows = read_pins(faults)
        pin_count = check_pins(docs, pins, includes, unpinned_rows, faults, only)
        vectors = document_vectors(docs, faults)
        twins = test_twins(faults)
        vector_count = check_vectors(vectors, twins, faults, only)
        check_index(docs, vectors, faults, only)
        check_counter_registry(docs, faults, only)
        check_references(docs, faults, only)
        check_language(docs, faults, only)
        if args.print_index or args.write_index:
            print_index(docs, vectors)
    except Fault as fault:
        print("rn standard: %s" % fault)
        return 2

    pending = [s.id for tag in ("RN-1", "RN-2") for s in docs[tag].sections if s.pending
               and (not only or s.id == only)]
    rationale_pending = [s.id for s in docs["RN-R"].sections if s.pending and (not only or s.id == only)]

    if faults:
        print("rn standard FAILED: %d fault(s)" % len(faults))
        for section, message in faults:
            print("  %s: %s" % (section or "-", message))
        return 1

    print("rn standard: %d pins, %d vectors, %d pending sections (%s), %d pending rationale sections; green"
          % (pin_count, vector_count, len(pending), " ".join(pending) or "none", len(rationale_pending)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
