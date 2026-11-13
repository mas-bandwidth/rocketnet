#!/usr/bin/env python3
"""Hold tools/wire_paths.txt to the tree.

release-check.yml's unreleased-wire-changes job runs `git log` over the wire
paths (the file's first section); git log over a path that no longer exists
prints nothing, so a renamed or deleted wire file would silently leave the
gate. Every wire path must be a file git tracks, no path may be listed twice,
and the wire list may not be empty.

A wire list that names only some of the sdk is only as good as the files left
off it (VERIFY-6 D5), so every tracked source under sdk/ (.h, .cpp, .c, .inc)
must also be on exactly one list: the wire list, `[not wire]` with a reason,
or `[wire-defining, classification pending owner, since 2026-11-12]` with the
evidence that it defines wire bytes. A source on no list fails, so a new or
renamed sdk file is a decision. The pending list is accepted and printed on
every run until the owner moves each file; it is not in the gate.

  python3 tools/check_wire_paths.py            check; exit 1 on any problem
  python3 tools/check_wire_paths.py --print    check, then print the wire paths
  python3 tools/check_wire_paths.py --self-test
  python3 tools/check_wire_paths.py --root DIR   DIR/tools/wire_paths.txt against DIR; none there is an error

Only standard-library Python and git are needed.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIST = "tools/wire_paths.txt"
NOT_WIRE = "not wire"
PENDING = "wire-defining, classification pending owner, since 2026-11-12"
SECTIONS = (NOT_WIRE, PENDING)
SOURCE_SUFFIXES = (".h", ".cpp", ".c", ".inc")


def read_sections(root):
    """{"": [(line, path, note)], NOT_WIRE: [...], PENDING: [...]} plus problems."""
    sections = {"": []}
    found = []
    current = ""
    lines = (root / LIST).read_text(encoding="utf-8").splitlines()
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            if current not in SECTIONS:
                found.append(f"{LIST}:{number}: unknown section [{current}]; the sections are " + ", ".join(f"[{s}]" for s in SECTIONS))
            elif current in sections:
                found.append(f"{LIST}:{number}: section [{current}] appears twice")
            sections.setdefault(current, [])
            continue
        if current == "":
            sections[""].append((number, line, ""))
            continue
        path, tab, note = raw.partition("\t")
        if not tab or not note.strip():
            found.append(f"{LIST}:{number}: {path.strip()} in [{current}] carries no reason: path, a tab, then why")
        sections[current].append((number, path.strip(), note.strip()))
    return sections, found


def read_list(root):
    sections, _ = read_sections(root)
    return [path for _, path, _ in sections[""]]


def tracked(root):
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True, capture_output=True).stdout
    return set(p for p in out.decode("utf-8").split("\0") if p)


def sdk_sources(files):
    return sorted(p for p in files if p.startswith("sdk/") and p.endswith(SOURCE_SUFFIXES))


def problems(root):
    if not (root / LIST).is_file():
        return [f"{LIST} is missing"]
    sections, found = read_sections(root)
    paths = [path for _, path, _ in sections[""]]
    if not paths:
        found.append(f"{LIST} lists no path: the wire gate would compare nothing")
    files = tracked(root)
    where = {}
    for name in [""] + [s for s in SECTIONS if s in sections]:
        label = f"[{name}]" if name else "the wire list"
        for number, path, _ in sections[name]:
            if path in where:
                found.append(f"{LIST}:{number}: {path} is listed twice (in {where[path]} and {label})")
                continue
            where[path] = label
            if any(c.isspace() for c in path):
                found.append(f"{LIST}:{number}: {path!r} holds whitespace; the workflow splits the list on it")
            if path not in files:
                found.append(f"{LIST}:{number}: {path} is not a tracked file; git log over it is silent, so the wire gate no longer sees it")
    for path in sdk_sources(files):
        if path not in where:
            found.append(f"{LIST}: {path} is on no list: put it on the wire list, or in [{NOT_WIRE}] or [{PENDING}] with a reason")
    return found


def pending(root):
    sections, _ = read_sections(root)
    return sections.get(PENDING, [])


def run(root, show):
    found = problems(root)
    for line in found:
        print(f"FAIL: {line}", file=sys.stderr)
    if found:
        return 1
    held = pending(root)
    if held:
        # stderr, so --print's stdout stays the bare path list release-check reads
        print(f"PENDING OWNER: {len(held)} sdk file(s) a review names as wire-defining are NOT in the wire gate"
              f" until the owner classifies them ({LIST}, [{PENDING}]):", file=sys.stderr)
        for _, path, note in held:
            print(f"PENDING OWNER: {path}: {note}", file=sys.stderr)
    if show:
        print("\n".join(read_list(root)))
    else:
        sections, _ = read_sections(root)
        print(f"OK: {len(read_list(root))} wire paths, each a tracked file; {len(sections.get(NOT_WIRE, []))} sdk sources not wire;"
              f" {len(held)} pending the owner; every tracked sdk source on one list")
    return 0


def self_test():
    failures = 0

    def case(name, files, listing, want_fail, want_text=""):
        nonlocal failures
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", tmp], check=True)
            for rel in files:
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("x\n")
            subprocess.run(["git", "-C", tmp, "add", "-A"], check=True)
            if listing is not None:
                (root / "tools").mkdir(exist_ok=True)
                (root / LIST).write_text(listing)
            found = problems(root)
            ok = bool(found) == want_fail and all(want_text in f for f in found[:1])
            print(f"{'ok' if ok else 'WRONG'}: {name}: {found if found else 'clean'}")
            failures += 0 if ok else 1

    files = ["sdk/rocketnet_wire.h", "standard/TRANSPORT.md", "sdk/rocketnet_tests.cpp", "sdk/rocketnet_endpoint.cpp", "sdk/Makefile"]
    tail = f"[{NOT_WIRE}]\nsdk/rocketnet_tests.cpp\tunit tests\n[{PENDING}]\nsdk/rocketnet_endpoint.cpp\t:1 writes\n"
    good = "# c\nsdk/rocketnet_wire.h\nstandard/TRANSPORT.md\n" + tail
    case("green: every path tracked, every sdk source on one list", files, good, False)
    case("red: a new sdk source on no list", files + ["sdk/rocketnet_new.cpp"], good, True, "rocketnet_new.cpp is on no list")
    case("red: a new kernel source on no list", files + ["sdk/xdp/new.c"], good, True, "xdp/new.c is on no list")
    case("red: a source on the wire list and on not wire", files,
         good.replace("sdk/rocketnet_tests.cpp\tunit", "sdk/rocketnet_wire.h\tx\nsdk/rocketnet_tests.cpp\tunit"), True, "listed twice")
    case("red: a not-wire line without a reason", files, good.replace("rocketnet_tests.cpp\tunit tests", "rocketnet_tests.cpp"), True, "carries no reason")
    case("red: a pending entry that is gone", files, good + "sdk/rocketnet_gone.h\tsomething\n", True, "rocketnet_gone.h is not a tracked file")
    case("red: an unknown section", files, good + "[wire maybe]\n", True, "unknown section")
    case("red: a listed path is gone", files, "sdk/rocketnet_wire.h\nsdk/rocketnet_gone.cpp\n" + tail, True, "rocketnet_gone.cpp is not a tracked file")
    case("red: a listed path exists but is untracked", files, "sdk/rocketnet_wire.h\nsdk/untracked.h\n" + tail, True, "untracked.h is not a tracked file")
    case("red: a path listed twice", files, "sdk/rocketnet_wire.h\nsdk/rocketnet_wire.h\n" + tail, True, "listed twice")
    case("red: a path with a space", files + ["sdk/two words.h"], "sdk/rocketnet_wire.h\nsdk/two words.h\n" + tail, True, "holds whitespace")
    case("red: only comments", files, "# nothing\n\n" + tail, True, "lists no path")
    case("red: the list is missing", files, None, True, "is missing")
    print("self-test: " + ("PASS" if failures == 0 else f"FAIL ({failures})"))
    return 1 if failures else 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) == 3 and argv[1] == "--root":
        return run(Path(argv[2]), False)
    if argv[1:] == ["--print"]:
        return run(ROOT, True)
    if argv[1:]:
        print(__doc__, file=sys.stderr)
        return 2
    return run(ROOT, False)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
