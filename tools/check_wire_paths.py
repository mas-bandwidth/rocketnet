#!/usr/bin/env python3
"""Hold tools/wire_paths.txt to the tree.

release-check.yml's unreleased-wire-changes job runs `git log` over these
paths; git log over a path that no longer exists prints nothing, so a renamed
or deleted wire file would silently leave the gate. Every listed path must be
a file git tracks, no path may be listed twice, and the list may not be empty.

  python3 tools/check_wire_paths.py            check; exit 1 on any problem
  python3 tools/check_wire_paths.py --print    check, then print the paths
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


def read_list(root):
    text = (root / LIST).read_text(encoding="utf-8")
    return [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")]


def tracked(root):
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True, capture_output=True).stdout
    return set(p for p in out.decode("utf-8").split("\0") if p)


def problems(root):
    if not (root / LIST).is_file():
        return [f"{LIST} is missing"]
    paths = read_list(root)
    if not paths:
        return [f"{LIST} lists no path: the wire gate would compare nothing"]
    found = []
    files = tracked(root)
    seen = set()
    for path in paths:
        if path in seen:
            found.append(f"{LIST}: {path} is listed twice")
        seen.add(path)
        if any(c.isspace() for c in path):
            found.append(f"{LIST}: {path!r} holds whitespace; the workflow splits the list on it")
        if path not in files:
            found.append(f"{LIST}: {path} is not a tracked file; git log over it is silent, so the wire gate no longer sees it")
    return found


def run(root, show):
    found = problems(root)
    for line in found:
        print(f"FAIL: {line}", file=sys.stderr)
    if found:
        return 1
    if show:
        print("\n".join(read_list(root)))
    else:
        print(f"OK: {len(read_list(root))} wire paths, each a tracked file")
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

    files = ["sdk/rocketnet_wire.h", "standard/TRANSPORT.md"]
    case("green: every path tracked", files, "# c\nsdk/rocketnet_wire.h\nstandard/TRANSPORT.md\n", False)
    case("red: a listed path is gone", files, "sdk/rocketnet_wire.h\nsdk/rocketnet_gone.cpp\n", True, "rocketnet_gone.cpp is not a tracked file")
    case("red: a listed path exists but is untracked", ["sdk/rocketnet_wire.h"], "sdk/rocketnet_wire.h\nsdk/untracked.h\n", True, "untracked.h is not a tracked file")
    case("red: a path listed twice", files, "sdk/rocketnet_wire.h\nsdk/rocketnet_wire.h\n", True, "listed twice")
    case("red: a path with a space", files + ["sdk/two words.h"], "sdk/rocketnet_wire.h\nsdk/two words.h\n", True, "holds whitespace")
    case("red: only comments", files, "# nothing\n\n", True, "lists no path")
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
