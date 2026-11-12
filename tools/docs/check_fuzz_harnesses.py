#!/usr/bin/env python3
"""Check that every fuzz harness and corpus is named by the lists that run it.

tests/fuzz/Makefile's TARGETS is the owner of the portable harnesses and its
XDP_TARGETS of the Linux-only ones; rocketnet_fuzz_differential (it needs the
pinned netcode checkout) is fuzzed by fuzz-nightly.yml only. The check fails
on:

  - a tracked tests/fuzz/rocketnet_fuzz*.cpp harness source, or a tracked
    tests/fuzz/corpus* directory, whose harness no list names (a corpus that
    nothing replays, a harness that nothing builds);
  - a replay list that is not exactly TARGETS: every `harnesses="..."` list
    in ci.yml (the macOS and Windows replays, fuzz-smoke, the NDEBUG
    replay), the macOS replay loop in docs/MACOS.md, the CMake fuzz_replay_
    list;
  - ci.yml's seed_harnesses, which must be exactly the TARGETS harnesses whose
    source reads ROCKETNET_FUZZ_SEED_DIR (a seed generator);
  - fuzz-smoke's xdp_harnesses, which must be exactly XDP_TARGETS, joined to
    its replay list, with a CMake fuzz_replay_ driver for each;
  - fuzz-nightly.yml's matrices: the Linux job must fuzz TARGETS, XDP_TARGETS
    and the differential; the NDEBUG job TARGETS and the differential; the
    macOS, Windows and MSan jobs TARGETS; tools/fuzz_ndebug.sh's ALL_TARGETS
    TARGETS and the differential;
  - a count in the guides ("the fourteen harnesses") other than |TARGETS|.

  python3 tools/docs/check_fuzz_harnesses.py
  python3 tools/docs/check_fuzz_harnesses.py --self-test

Prints one OK line per checked list; exits 1 naming the file and the missing,
extra or miscounted harnesses.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NIGHTLY_ONLY = {"rocketnet_fuzz_differential"}
NUMBERS = {
    "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7,
    "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13,
    "fourteen": 14, "fifteen": 15, "sixteen": 16, "seventeen": 17,
}
COUNT = re.compile(r"\b(" + "|".join(NUMBERS) + r")\s+(?:replay binaries|harnesses|committed corpora)\b")
HARNESS = re.compile(r"\brocketnet_fuzz(?:_[a-z_]+)?\b")
LIST = re.compile(r'^\s*harnesses="(rocketnet_fuzz[^"]*)"', re.M)


def read(root, name):
    return (root / name).read_text(encoding="utf-8")


def makefile_list(root, variable):
    text = read(root, "tests/fuzz/Makefile")
    names = set()
    for match in re.finditer(rf"^{variable}\s*=[ \t]*((?:.*\\\n)*.*)$", text, re.M):
        names |= set(match.group(1).replace("\\", " ").split())
    return names


def tracked(root, *paths):
    result = subprocess.run(["git", "-C", str(root), "ls-files", "--"] + list(paths),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if result.returncode != 0:
        raise RuntimeError("git ls-files failed: " + result.stderr.strip())
    return [line for line in result.stdout.splitlines() if line]


class Report:
    def __init__(self):
        self.failed = 0
        self.lines = []

    def ok(self, text):
        self.lines.append("OK: " + text)

    def fail(self, text):
        self.lines.append("FAIL: " + text)
        self.failed = 1

    def same(self, label, names, want):
        names = set(names)
        if names == want:
            self.ok(f"{label} names the {len(want)} harnesses")
            return
        self.fail(f"{label}: missing {sorted(want - names)}, extra {sorted(names - want)}")


def job_block(text, job):
    """The text of a two-space job key, up to the next job key."""
    match = re.search(rf"^  {re.escape(job)}:\s*(?:#.*)?$", text, re.M)
    if match is None:
        return None
    rest = text[match.end():]
    following = re.search(r"^  [A-Za-z0-9_][\w-]*:\s*(?:#.*)?$", rest, re.M)
    return rest[:following.start()] if following else rest


def matrix_targets(block):
    names = set(re.findall(r'"target":"(rocketnet_fuzz[a-z_]*)"', block))
    names |= set(re.findall(r"^\s*-\s*(rocketnet_fuzz[a-z_]*)\s*$", block, re.M))
    return names


def check(root):
    report = Report()
    targets = makefile_list(root, "TARGETS")
    xdp = makefile_list(root, "XDP_TARGETS")
    if not targets:
        report.fail("tests/fuzz/Makefile: no TARGETS")
        return report
    known = targets | xdp | NIGHTLY_ONLY

    # every harness source and every corpus directory belongs to a named harness
    sources = [Path(f).stem for f in tracked(root, "tests/fuzz")
               if re.fullmatch(r"tests/fuzz/rocketnet_fuzz(_[a-z_]+)?\.cpp", f)]
    corpora = sorted(set(f.split("/")[2] for f in tracked(root, "tests/fuzz")
                         if re.fullmatch(r"tests/fuzz/corpus(_[a-z_]+)?/.+", f)))
    if not sources or not corpora:
        report.fail("tests/fuzz: git lists no harness source or no corpus directory")
    for name in sorted(sources):
        if name not in known:
            report.fail(f"tests/fuzz/{name}.cpp: a harness source no list names (TARGETS, XDP_TARGETS, the differential)")
    for corpus in corpora:
        harness = "rocketnet_fuzz" + corpus[len("corpus"):]
        if harness not in known:
            report.fail(f"tests/fuzz/{corpus}/: a corpus whose harness {harness} no list names; nothing replays it")
        if harness not in sources:
            report.fail(f"tests/fuzz/{corpus}/: no tests/fuzz/{harness}.cpp")
    for name in sorted(known):
        if name not in sources:
            report.fail(f"{name}: listed, but tests/fuzz/{name}.cpp is not tracked")
        elif "corpus" + name[len("rocketnet_fuzz"):] not in corpora:
            report.fail(f"{name}: listed, but has no tracked corpus directory")
    report.ok(f"{len(sources)} harness sources and {len(corpora)} corpus directories, each named")

    macos = read(root, "docs/MACOS.md")
    loop = re.search(r"^for harness in (.*?); do$", macos, re.M | re.S)
    if loop is None:
        report.fail("docs/MACOS.md: no `for harness in ...; do` replay loop")
    else:
        report.same("docs/MACOS.md replay loop", HARNESS.findall(loop.group(1)), targets)

    ci = read(root, ".github/workflows/ci.yml")
    lists = LIST.findall(ci)
    if len(lists) < 4:
        report.fail(f".github/workflows/ci.yml: {len(lists)} harnesses= lists, want the macOS, Windows, fuzz-smoke and NDEBUG replays")
    for index, names in enumerate(lists, 1):
        report.same(f".github/workflows/ci.yml harness list {index}", names.split(), targets)

    seeds = re.findall(r'^\s*seed_harnesses="([^"]*)"', ci, re.M)
    generators = set(t for t in targets if "ROCKETNET_FUZZ_SEED_DIR" in read(root, f"tests/fuzz/{t}.cpp"))
    if len(seeds) != 1:
        report.fail(f".github/workflows/ci.yml: {len(seeds)} seed_harnesses= lists, want 1")
    else:
        report.same(".github/workflows/ci.yml seed_harnesses (the harnesses with a seed generator)", seeds[0].split(), generators)

    smoke = job_block(ci, "fuzz-smoke")
    if smoke is None:
        report.fail(".github/workflows/ci.yml: no fuzz-smoke job")
    else:
        smoke_lists = LIST.findall(smoke)
        if len(smoke_lists) != 1:
            report.fail(f".github/workflows/ci.yml fuzz-smoke: {len(smoke_lists)} harnesses= replay lists, want 1")
        xdp_lists = re.findall(r'^\s*xdp_harnesses="([^"]*)"', smoke, re.M)
        if len(xdp_lists) != 1:
            report.fail(f".github/workflows/ci.yml fuzz-smoke: {len(xdp_lists)} xdp_harnesses= lists, want 1")
        else:
            report.same(".github/workflows/ci.yml fuzz-smoke xdp_harnesses", xdp_lists[0].split(), xdp)
        if not re.search(r'^\s*harnesses="\$harnesses \$xdp_harnesses"', smoke, re.M):
            report.fail('.github/workflows/ci.yml fuzz-smoke: xdp_harnesses is not joined to the replay list (harnesses="$harnesses $xdp_harnesses")')
        if re.search(r"\./bin/(?!fuzz_replay_)[A-Za-z0-9_.-]+", smoke):
            report.fail(".github/workflows/ci.yml fuzz-smoke: replays a libFuzzer binary, not a fuzz_replay_ driver")
        if "-max_total_time" in smoke:
            report.fail(".github/workflows/ci.yml fuzz-smoke: runs a time-boxed mutation pass; it belongs in fuzz-nightly.yml")

    cmake = read(root, "CMakeLists.txt")
    pair = re.search(r"set\(ROCKETNET_FUZZ_PAIR_HARNESSES ([^)]*)\)", cmake)
    replay = re.search(r"foreach\(harness (rocketnet_fuzz[^)]*)\)", cmake)
    if pair is None or replay is None:
        report.fail("CMakeLists.txt: no fuzz_replay_ harness list")
    else:
        names = replay.group(1).replace("${ROCKETNET_FUZZ_PAIR_HARNESSES}", pair.group(1)).split()
        report.same("CMakeLists.txt fuzz_replay_ targets", names, targets)
    for name in sorted(xdp):
        if not re.search(rf"add_executable\(fuzz_replay_{name}\b", cmake):
            report.fail(f"CMakeLists.txt: no fuzz_replay_{name} driver for the XDP target")

    nightly = read(root, ".github/workflows/fuzz-nightly.yml")
    for job, want in (("fuzz", targets | xdp | NIGHTLY_ONLY), ("fuzz-ndebug", targets | NIGHTLY_ONLY),
                      ("fuzz-macos", targets), ("fuzz-windows", targets), ("fuzz-msan", targets)):
        block = job_block(nightly, job)
        if block is None:
            report.fail(f".github/workflows/fuzz-nightly.yml: no {job} job")
            continue
        report.same(f".github/workflows/fuzz-nightly.yml {job} matrix", matrix_targets(block), want)

    script = read(root, "tools/fuzz_ndebug.sh")
    array = re.search(r"^ALL_TARGETS=\((.*?)\)", script, re.M | re.S)
    if array is None:
        report.fail("tools/fuzz_ndebug.sh: no ALL_TARGETS=( ... )")
    else:
        report.same("tools/fuzz_ndebug.sh ALL_TARGETS", HARNESS.findall(array.group(1)), targets | NIGHTLY_ONLY)

    for guide in ("docs/BUILD-REFERENCE.md", "docs/MACOS.md"):
        text = read(root, guide)
        for match in COUNT.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            phrase = " ".join(match.group(0).split())
            if NUMBERS[match.group(1)] != len(targets):
                report.fail(f"{guide}:{line}: '{phrase}', tests/fuzz/Makefile builds {len(targets)}")
            else:
                report.ok(f"{guide}:{line}: '{phrase}'")
    return report


FILES = ["tests/fuzz/Makefile", "docs/MACOS.md", "docs/BUILD-REFERENCE.md", ".github/workflows/ci.yml",
         ".github/workflows/fuzz-nightly.yml", "CMakeLists.txt", "tools/fuzz_ndebug.sh"]


def self_test():
    """Copies the real lists into a temporary git tree (each corpus as one
    placeholder file), checks the copy is green, then plants one fault per
    case and requires a FAIL."""
    failures = []
    cases = 0
    with tempfile.TemporaryDirectory() as temp:
        tree = Path(temp)
        for name in FILES:
            (tree / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, tree / name)
        for name in tracked(ROOT, "tests/fuzz"):
            if re.fullmatch(r"tests/fuzz/rocketnet_fuzz(_[a-z_]+)?\.cpp", name):
                (tree / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, tree / name)
        for corpus in set(f.split("/")[2] for f in tracked(ROOT, "tests/fuzz") if re.fullmatch(r"tests/fuzz/corpus(_[a-z_]+)?/.+", f)):
            (tree / "tests/fuzz" / corpus).mkdir(parents=True, exist_ok=True)
            (tree / "tests/fuzz" / corpus / "seed").write_text("x")
        subprocess.run(["git", "init", "-q", str(tree)], check=True)

        def stage():
            subprocess.run(["git", "-C", str(tree), "add", "-A"], check=True)

        stage()
        clean = check(tree)
        cases += 1
        if clean.failed:
            failures.append("clean copy: " + "; ".join(l for l in clean.lines if l.startswith("FAIL")))

        def plant(label, path, old, new, count=1):
            nonlocal cases
            cases += 1
            original = (tree / path).read_text(encoding="utf-8")
            if old not in original:
                failures.append(f"{label}: the plant's text is not in {path}")
                return
            (tree / path).write_text(original.replace(old, new, count), encoding="utf-8")
            stage()
            if not check(tree).failed:
                failures.append(f"{label}: want FAIL, got none")
            (tree / path).write_text(original, encoding="utf-8")
            stage()

        def plant_file(label, path, content):
            nonlocal cases
            cases += 1
            (tree / path).parent.mkdir(parents=True, exist_ok=True)
            (tree / path).write_text(content)
            stage()
            if not check(tree).failed:
                failures.append(f"{label}: want FAIL, got none")
            (tree / path).unlink()
            stage()

        plant_file("unnamed harness source", "tests/fuzz/rocketnet_fuzz_planted.cpp", "// planted\n")
        plant_file("unnamed corpus directory", "tests/fuzz/corpus_planted/seed", "x")
        plant("harness dropped from TARGETS (its corpus unreplayed)", "tests/fuzz/Makefile",
              " \\\n          rocketnet_fuzz_endpoint", "")
        plant("harness dropped from the fuzz-smoke list", ".github/workflows/ci.yml",
              "rocketnet_fuzz_gateway_netlink rocketnet_fuzz_endpoint\"\n          # and the Linux-only",
              "rocketnet_fuzz_gateway_netlink\"\n          # and the Linux-only")
        plant("seed list names a harness with no generator", ".github/workflows/ci.yml",
              'seed_harnesses="rocketnet_fuzz ', 'seed_harnesses="rocketnet_fuzz_endpoint rocketnet_fuzz ')
        plant("xdp_harnesses emptied", ".github/workflows/ci.yml",
              'xdp_harnesses="rocketnet_fuzz_xdp_filter"', 'xdp_harnesses=""')
        plant("xdp_harnesses not joined", ".github/workflows/ci.yml",
              'harnesses="$harnesses $xdp_harnesses"', 'echo "$xdp_harnesses"')
        plant("CMake XDP replay driver missing", "CMakeLists.txt",
              "add_executable(fuzz_replay_rocketnet_fuzz_xdp_filter", "add_executable(fuzz_replay_planted")
        plant("nightly Linux matrix loses the XDP filter", ".github/workflows/fuzz-nightly.yml",
              '"target":"rocketnet_fuzz_xdp_filter"', '"target":"rocketnet_fuzz_planted_gone"')
        plant("nightly NDEBUG matrix loses a harness", ".github/workflows/fuzz-nightly.yml",
              "          - rocketnet_fuzz_endpoint\n", "")
        plant("fuzz_ndebug.sh loses a harness", "tools/fuzz_ndebug.sh",
              "    rocketnet_fuzz_endpoint\n", "")
        plant("macOS loop loses a harness", "docs/MACOS.md",
              "rocketnet_fuzz_gateway_netlink rocketnet_fuzz_endpoint; do", "rocketnet_fuzz_gateway_netlink; do")
        plant("guide count wrong", "docs/BUILD-REFERENCE.md", "the fourteen harnesses", "the thirteen harnesses")

    if failures:
        for failure in failures:
            print("FAIL " + failure)
        print(f"check_fuzz_harnesses self-test: {len(failures)} of {cases} cases failed")
        return 1
    print(f"ok: check_fuzz_harnesses self-test, {cases} cases (a clean copy and {cases - 1} plants)")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    try:
        report = check(ROOT)
    except RuntimeError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 2
    for line in report.lines:
        print(line, file=sys.stderr if line.startswith("FAIL") else sys.stdout)
    return report.failed


if __name__ == "__main__":
    sys.exit(main(sys.argv))
