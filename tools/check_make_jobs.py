#!/usr/bin/env python3
"""Every parallel build in the tree carries a bounded job count.

A bare `make -j`, `cmake --build ... --parallel` or `cmake --build ... -j`
with no number starts every compile at once under the Makefile generator
(the default on Linux and macOS), which can exhaust memory (exit 137) on the
benches. The top Makefile bounds its child make at JOBS (the processors
capped at 8); every other build names its number.

Files read: every tracked file (`git ls-files`) that is a Makefile
(Makefile, GNUmakefile, *.mk), a workflow or action (.github/**.yml), a shell
script (*.sh, or a shebang naming sh or bash), Python (*.py) or Go (*.go),
outside the vendored sodium/ tree. Documentation (*.md, docs/roadmap.sexp)
is not a build and is not read.

Refused, with file:line:
  - a make invocation ($(MAKE), ${MAKE}, `make`) carrying `-j` or `--jobs`
    with no number;
  - `--parallel` or (on a `cmake --build` line) `-j` followed by nothing,
    another option, a line continuation, a shell operator or a redirection
    (`2>&1` is a file descriptor, not a job count);
  - in Python and Go argument lists, "--parallel" or "-j" as the last
    element of the list.

A run that read no file fails (git could not list, or the list was empty).

  python3 tools/check_make_jobs.py            check the tree
  python3 tools/check_make_jobs.py --self-test the planted red and green controls
"""

import os
import re
import subprocess
import sys
import tempfile

SHELL_SHEBANG = re.compile(r"^#!.*\b(sh|bash)\b")
MAKE_CALL = re.compile(r"(\$\(MAKE\)|\$\{MAKE\}|(^|[\s;&|(`'\"])make(\s|$))")
CMAKE_BUILD = re.compile(r"cmake\s+--build\b")
# a flag, then nothing that is a value: end, another option, continuation,
# operator, or a redirection (`2>&1`, `> log`, `<in`: digits before < or > are
# a file descriptor, not a job count)
BARE_AFTER = r"(?=\s*($|-|\\|;|&|\||\)|#|[0-9]*[<>]))"
BARE_PARALLEL = re.compile(r"(^|[\s,(])--parallel" + BARE_AFTER)
BARE_J = re.compile(r"(^|[\s,(])-j(?=\s|$|\))(?!\s+(?![0-9]*[<>])[0-9$\"'])")
BARE_JOBS = re.compile(r"(^|[\s,(])--jobs(?=\s|$|\))(?!\s+(?![0-9]*[<>])[0-9$\"'])")
LIST_BARE = re.compile(r"[\"'](--parallel|-j)[\"']\s*,?\s*[\]\)}]")


def kind_of(path, head):
    base = os.path.basename(path)
    if path.startswith("sodium/"):
        return None
    if base in ("Makefile", "GNUmakefile") or base.endswith(".mk"):
        return "make"
    if path.startswith(".github/") and (base.endswith(".yml") or base.endswith(".yaml")):
        return "shell"
    if base.endswith(".sh") or SHELL_SHEBANG.match(head):
        return "shell"
    if base.endswith(".py") or base.endswith(".go"):
        return "list"
    return None


def check_text(path, kind, text):
    findings = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if kind in ("make", "shell") and stripped.startswith("#"):
            continue
        if kind == "list" and (stripped.startswith("#") or stripped.startswith("//")):
            continue
        if kind in ("make", "shell"):
            if MAKE_CALL.search(line) and (BARE_J.search(line) or BARE_JOBS.search(line)):
                findings.append(f"{path}:{number}: a make with no job count: {stripped}")
                continue
            if BARE_PARALLEL.search(line):
                findings.append(f"{path}:{number}: --parallel with no job count: {stripped}")
                continue
            if CMAKE_BUILD.search(line) and BARE_J.search(line):
                findings.append(f"{path}:{number}: cmake --build -j with no job count: {stripped}")
                continue
        if kind == "list" and LIST_BARE.search(line):
            findings.append(f"{path}:{number}: a job flag with no count: {stripped}")
    return findings


def tracked_files(root):
    result = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode(errors="replace"))
        return None
    return [name for name in result.stdout.decode().split("\0") if name]


def check_tree(root):
    names = tracked_files(root)
    if names is None:
        print("check_make_jobs: git ls-files failed; nothing was checked")
        return 2
    findings = []
    read = 0
    for name in names:
        path = os.path.join(root, name)
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue
        kind = kind_of(name, text.split("\n", 1)[0])
        if kind is None:
            continue
        read += 1
        findings.extend(check_text(name, kind, text))
    for finding in findings:
        print(finding)
    if read == 0:
        print("check_make_jobs: no build file read; nothing was checked")
        return 1
    if findings:
        print(f"check_make_jobs: {len(findings)} unbounded parallel build(s) in {read} files")
        return 1
    print(f"ok: every parallel build in {read} files has a job count")
    return 0


RED = [
    ("Makefile", "\t$(MAKE) -j -C sub\n"),
    ("Makefile", "\t+cmake --build build $(if $(filter -j%,$(MAKEFLAGS)),,--parallel)\n"),
    ("Makefile", "\t$(MAKE) -C sub $(if $(X),-j)\n"),
    ("tests/fuzz/Makefile", "all:\n\t$(MAKE) -C x -j\n"),
    ("Makefile", "\tmake --jobs all\n"),
    (".github/workflows/ci.yml", "        run: cmake --build build --parallel\n"),
    (".github/workflows/ci.yml", "        run: cmake --build build --parallel --target test\n"),
    (".github/workflows/ci.yml", "          cmake --build build --parallel \\\n            --target x\n"),
    (".github/workflows/ci.yml", "          cmake --build build -j && ./bin/test\n"),
    ("tools/run_x.sh", "cmake --build \"$dir\" --target test --parallel\n"),
    (".github/workflows/x.yml", "          cmake --build build --parallel 2>&1 | tee logs/build.log\n"),
    (".github/workflows/x.yml", "          cmake --build build --parallel > logs/build.log\n"),
    (".github/workflows/x.yml", "          cmake --build build --parallel</dev/null\n"),
    (".github/workflows/x.yml", "          cmake --build build -j 2>&1 | tee logs/build.log\n"),
    ("Makefile", "\tmake --jobs 2>/dev/null\n"),
    ("scripts/thing", "#!/usr/bin/env bash\nmake -j\n"),
    ("tools/build.py", "run([\"cmake\", \"--build\", d, \"--parallel\"])\n"),
    ("tools/x/x.go", "{\"cmake\", \"--build\", dir, \"-j\"},\n"),
]

GREEN = [
    ("Makefile", "\t+cmake --build build $(if $(filter -j%,$(MAKEFLAGS)),,--parallel $(JOBS))\n"),
    ("Makefile", "\t$(MAKE) -j$(JOBS) -C sub\n"),
    ("Makefile", "\t$(MAKE) -j 4 -C sub\n"),
    (".github/workflows/ci.yml", "        run: cmake --build build --parallel 8\n"),
    (".github/workflows/ci.yml", "          cmake --build build --parallel 8 2>&1 | tee logs/build.log\n"),
    (".github/workflows/ci.yml", "          cmake --build build -j 4 2>&1 | tee logs/build.log\n"),
    (".github/workflows/ci.yml", "        run: cmake --build build --parallel \"$jobs\" --target t\n"),
    (".github/workflows/ci.yml", "        # cmake --build build --parallel\n"),
    ("tools/build.py", "run([\"cmake\", \"--build\", d, \"--parallel\", \"4\"])\n"),
    ("tools/x/x.go", "{\"cmake\", \"--build\", dir, \"--parallel\", fmt.Sprint(n)},\n"),
    ("docs/BUILDING.md", "cmake --build build --parallel\n"),
    ("sodium/Makefile", "\tmake -j\n"),
]


def self_test():
    failures = 0
    for path, text in RED:
        kind = kind_of(path, text.split("\n", 1)[0])
        found = check_text(path, kind, text) if kind else []
        if not found:
            print(f"FAIL red control passed: {path}: {text.strip()!r}")
            failures += 1
    for path, text in GREEN:
        kind = kind_of(path, text.split("\n", 1)[0])
        found = check_text(path, kind, text) if kind else []
        if found:
            print(f"FAIL green control refused: {found[0]}")
            failures += 1
    # the whole tree path: a repository with no build file, and git unable to list
    with tempfile.TemporaryDirectory() as work:
        subprocess.run(["git", "-C", work, "init", "-q"], check=True)
        with open(os.path.join(work, "README.md"), "w") as handle:
            handle.write("cmake --build build --parallel\n")
        subprocess.run(["git", "-C", work, "add", "README.md"], check=True)
        if check_tree(work) != 1:
            print("FAIL a tree with no build file did not fail")
            failures += 1
        with open(os.path.join(work, "Makefile"), "w") as handle:
            handle.write("all:\n\t$(MAKE) -j -C sub\n")
        subprocess.run(["git", "-C", work, "add", "Makefile"], check=True)
        if check_tree(work) != 1:
            print("FAIL a tree with a bare -j did not fail")
            failures += 1
        with open(os.path.join(work, "Makefile"), "w") as handle:
            handle.write("all:\n\t$(MAKE) -j4 -C sub\n")
        if check_tree(work) != 0:
            print("FAIL a clean tree did not pass")
            failures += 1
        if check_tree(os.path.join(work, "no-such-directory")) != 2:
            print("FAIL git unable to list did not exit 2")
            failures += 1
    total = len(RED) + len(GREEN) + 4
    print(f"check_make_jobs self-test: {total} controls, {failures} failed")
    return 1 if failures else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if sys.argv[1:]:
        print(__doc__)
        return 2
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if root.returncode != 0:
        print("check_make_jobs: not inside a git repository; nothing was checked")
        return 2
    return check_tree(root.stdout.strip())


if __name__ == "__main__":
    sys.exit(main())
