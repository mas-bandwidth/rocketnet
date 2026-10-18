#!/bin/sh
# undocumented.sh: parse a MrDocs diagnostic log and emit tab-separated
# "file\tline\tsymbol\tkind\tarea" lines sorted deterministically, with
# optional "--area <name>" filtering.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FILTER_AREA=""
LOG_FILE=""
AREAS_FILE=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --area)
            if [ "$#" -lt 2 ]; then
                echo "error: --area requires a value" >&2
                exit 1
            fi
            FILTER_AREA="$2"
            shift 2
            ;;
        --area=*)
            FILTER_AREA="${1#--area=}"
            shift
            ;;
        *)
            if [ -z "$LOG_FILE" ]; then
                LOG_FILE="$1"
            elif [ -z "$AREAS_FILE" ]; then
                AREAS_FILE="$1"
            else
                echo "error: unexpected argument: $1" >&2
                exit 1
            fi
            shift
            ;;
    esac
done

[ -n "$LOG_FILE" ] || LOG_FILE="$ROOT/build/docs/mrdocs.log"
[ -n "$AREAS_FILE" ] || AREAS_FILE="$ROOT/docs/mrdocs/areas.tsv"

if [ ! -f "$LOG_FILE" ]; then
    echo "error: missing MrDocs log file: $LOG_FILE" >&2
    exit 1
fi

if [ ! -f "$AREAS_FILE" ]; then
    echo "error: missing areas file: $AREAS_FILE" >&2
    exit 1
fi

python3 - "$ROOT" "$LOG_FILE" "$AREAS_FILE" "$FILTER_AREA" << 'PY'
import os
import re
import sys

root, log_path, areas_path, filter_area = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

areas = []
with open(areas_path, encoding="utf-8") as f:
    for raw in f:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        area, files_s, ere_s = parts[0], parts[1], parts[2]
        files = set() if files_s == "-" else set(x.strip() for x in files_s.split(",") if x.strip())
        ere = None if ere_s == "-" else re.compile(ere_s)
        areas.append((area, files, ere))

def parse_scopes(rel_file):
    full = os.path.join(root, rel_file)
    if not os.path.exists(full):
        return None
    with open(full, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    scopes = []
    stack = []
    depth = 0
    pending = None
    for idx, raw in enumerate(lines, 1):
        line = re.sub(r"//.*$", "", raw)
        m_ns = re.match(r"^\s*namespace\s+([A-Za-z_][A-Za-z0-9_]*)", line)
        m_rec = re.match(r"^\s*(?:typedef\s+)?(struct|class|union)\s+([A-Za-z_][A-Za-z0-9_]*)\b([^;]*)$", line)
        if m_ns:
            pending = ("namespace", m_ns.group(1), idx)
        elif m_rec and ";" not in line:
            pending = (m_rec.group(1), m_rec.group(2), idx)
        for ch in line:
            if ch == "{":
                depth += 1
                if pending:
                    stack.append((pending[0], pending[1], pending[2], depth))
                    pending = None
            elif ch == "}":
                if stack and stack[-1][3] == depth:
                    kind, name, sline, _ = stack.pop()
                    scopes.append((sline, idx, kind, name))
                depth = max(0, depth - 1)
    scopes.sort(key=lambda s: (s[0], -s[1]))
    return scopes

scope_cache = {}
def qualify(rel_file, lineno, sym, kind, pending_records):
    if sym.startswith("<unnamed enum>::"):
        sym = sym[len("<unnamed enum>::"):]
    if "::" in sym or not sym or kind == "macro":
        return sym
    if rel_file not in scope_cache:
        scope_cache[rel_file] = parse_scopes(rel_file)
    file_scopes = scope_cache[rel_file]
    if file_scopes is not None:
        enclosing = []
        for sline, eline, skind, name in file_scopes:
            if sline < lineno <= eline:
                enclosing.append(name)
            elif sline == lineno and name != sym:
                enclosing.append(name)
        if enclosing:
            return "::".join(enclosing + [sym])
        return sym
    if kind == "variable":
        for rec_line, rec_name in pending_records.get(rel_file, []):
            if rec_line < lineno and rec_name != sym:
                return f"{rec_name}::{sym}"
    return sym

def classify(rel_file, qsym):
    for area, files, ere in areas:
        if rel_file in files:
            return area
        if ere is not None and ere.search(qsym):
            return area
    return "unassigned"

loc_re = re.compile(r"^(\S.*?):(\d+):(\d+):\s*$")
item_re = re.compile(r"^\s+\d+\)\s+(.*?):\s+(.*\S)\s*$")

raw_entries = []
cur_file = None
cur_line = None

with open(log_path, encoding="utf-8", errors="replace") as f:
    for raw in f:
        line = raw.rstrip("\r\n")
        m_loc = loc_re.match(line)
        if m_loc:
            p = m_loc.group(1).replace("\\", "/")
            if "/sdk/" in p:
                cur_file = "sdk/" + p.split("/sdk/")[-1]
            elif p.startswith("sdk/"):
                cur_file = p
            else:
                cur_file = p.rsplit("/", 1)[-1]
            cur_line = int(m_loc.group(2))
            continue
        if cur_file is not None:
            m_item = item_re.match(line)
            if m_item:
                raw_sym, msg = m_item.group(1), m_item.group(2)
                if msg.endswith(" is undocumented"):
                    kind = msg[:-len(" is undocumented")]
                elif msg == "Missing documentation for enum value":
                    kind = "enum-value"
                elif msg.startswith("Missing documentation for parameter ") or msg.endswith(" parameter is unnamed"):
                    kind = "param"
                elif msg == "symbol is documented but has no brief":
                    kind = "brief"
                else:
                    continue
                raw_entries.append((cur_file, cur_line, raw_sym, kind))
            elif not line.strip():
                cur_file = None
                cur_line = None

pending_records = {}
for rel_file, lineno, raw_sym, kind in raw_entries:
    if kind == "record":
        pending_records.setdefault(rel_file, []).append((lineno, raw_sym))
for k in pending_records:
    pending_records[k].sort(reverse=True)

rows = set()
for rel_file, lineno, raw_sym, kind in raw_entries:
    qsym = qualify(rel_file, lineno, raw_sym, kind, pending_records)
    area = classify(rel_file, qsym)
    if filter_area and area != filter_area:
        continue
    rows.add((rel_file, lineno, qsym, kind, area))

for rel_file, lineno, qsym, kind, area in sorted(rows, key=lambda r: (r[0], r[1], r[2], r[3], r[4])):
    print(f"{rel_file}\t{lineno}\t{qsym}\t{kind}\t{area}")
PY
