#!/usr/bin/env python3
"""Release documentation verification for RocketNet v1.0.0.

Subcommands:
  archaeology  Check every tracked .md file for STANDARD.md, CHANNELS, rocketnet_*_send,
               card ids, the word card, dates, host names, S:NNNN line refs, MESSAGES
               sections above m13, short commit shas, PR and issue numbers, and
               change narration (prints nothing and exits 0 when clean).
  links        Check that every relative link and every #anchor in every tracked .md
               file resolves at head.
  usage        Compile every ```c and ```cpp fenced block of USAGE.md against
               sdk/rocketnet.h with -Wall -Werror (linking whole programs against the
               built library) and check every rocketnet_*/ROCKETNET_* identifier.
  selftest     Prove the fragment path: a one-line fragment with no #include of its
               own compiles, as C and as C++, with #include "rocketnet.h" prepended.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CHANGELOG_HEADING_RE = re.compile(
    r"^## \[[0-9]+\.[0-9]+\.[0-9]+\] - 20[0-9][0-9]-[01][0-9]-[0-3][0-9]$"
)
LICENSE_FILE_RE = re.compile(r"^LICENSE")
VISUAL_STUDIO_RE = re.compile(r"\bVisual Studio\b", re.IGNORECASE)

SPACE_PHRASE_PATTERNS = [
    re.compile(r"\baddress space\b", re.IGNORECASE),
    re.compile(r"\bbuffer space\b", re.IGNORECASE),
    re.compile(r"\bheader space\b", re.IGNORECASE),
    re.compile(r"\bqueue space\b", re.IGNORECASE),
    re.compile(r"\bordering space\b", re.IGNORECASE),
    re.compile(r"\bsequence space\b", re.IGNORECASE),
    re.compile(r"\bnumber space\b", re.IGNORECASE),
    re.compile(r"\bgeneration space\b", re.IGNORECASE),
    re.compile(r"\bstate space\b", re.IGNORECASE),
    re.compile(r"\bhalf-space\b", re.IGNORECASE),
    re.compile(r"\bfree space\b", re.IGNORECASE),
    re.compile(r"\btakes space\b", re.IGNORECASE),
    re.compile(r"\bthe space is padding\b", re.IGNORECASE),
]

ARCHAEOLOGY_PATTERNS = [
    ("STANDARD.md", re.compile(r"STANDARD\.md", re.IGNORECASE)),
    ("removed_send", re.compile(r"rocketnet_(client|server)_send\b", re.IGNORECASE)),
    ("worker_card", re.compile(r"rocketnet-[a-z-]+\.w[0-9]", re.IGNORECASE)),
    ("card", re.compile(r"\bcards?\b", re.IGNORECASE)),
    ("date", re.compile(r"20[0-9][0-9]-[01][0-9]-[0-3][0-9]", re.IGNORECASE)),
    ("host", re.compile(r"\b(amd-[123]|hulk|hetzner[0-9]*|vision|captain|studio|space)\b", re.IGNORECASE)),
    ("CHANNELS.md", re.compile(r"\bCHANNELS\b")),
    ("S_line_ref", re.compile(r"\bS:\d+\b")),
    ("m_section", re.compile(r"(?<!#)\bm(?:1[4-9]|[2-9][0-9])\b")),
    ("short_sha", re.compile(r"`(?=[0-9a-f]*[a-f])(?=[0-9a-f]*[0-9])[0-9a-f]{7,12}`")),
    ("PR", re.compile(r"\bPRs?\b")),
    ("issue_ref", re.compile(r"(?<![\w/(&])#[0-9]+\b")),
    ("change_narration", re.compile(r"\b(?:this change|the base of this change|landed|earlier code)\b", re.IGNORECASE)),
]

LINK_RE = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
ANCHOR_TAG_RE = re.compile(r"<a\s+id=\"([^\"]+)\"\s*>")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+)$")
IDENT_RE = re.compile(r"\b(?:rocketnet_[a-z0-9_]+|ROCKETNET_[A-Z0-9_]+)\b")


def tracked_markdown_files():
    out = subprocess.check_output(
        ["git", "ls-files", "*.md"], cwd=ROOT, text=True
    )
    return [line.strip() for line in out.splitlines() if line.strip()]


def scrub_allowlisted_line(rel_path, line):
    scrubbed = line
    if rel_path == "CHANGELOG.md" and CHANGELOG_HEADING_RE.match(line.strip()):
        scrubbed = re.sub(r"20[0-9][0-9]-[01][0-9]-[0-3][0-9]", "", scrubbed)
    if LICENSE_FILE_RE.match(Path(rel_path).name):
        scrubbed = re.sub(r"20[0-9][0-9]-[01][0-9]-[0-3][0-9]", "", scrubbed)
    scrubbed = VISUAL_STUDIO_RE.sub("", scrubbed)
    for pat in SPACE_PHRASE_PATTERNS:
        scrubbed = pat.sub("", scrubbed)
    return scrubbed


def check_archaeology():
    failures = []
    for rel in tracked_markdown_files():
        path = ROOT / rel
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        for lineno, line in enumerate(text.splitlines(), 1):
            scrubbed = scrub_allowlisted_line(rel, line)
            for label, pat in ARCHAEOLOGY_PATTERNS:
                for m in pat.finditer(scrubbed):
                    failures.append(f"{rel}:{lineno}: {label}: {m.group(0)!r}")

    if failures:
        for f in failures:
            print(f"archaeology: {f}", file=sys.stderr)
        return 1
    return 0


def github_slug(heading):
    s = heading.strip().lower()
    s = re.sub(r"[`*_~]", "", s)
    s = re.sub(r"[^\w\s-]", "", s)
    s = re.sub(r"[\s]+", "-", s)
    return s


def collect_anchors(md_path):
    text = md_path.read_text(encoding="utf-8")
    anchors = set()
    for m in ANCHOR_TAG_RE.finditer(text):
        anchors.add(m.group(1))
    in_fence = False
    for line in text.splitlines():
        s = line.lstrip()
        if s.startswith("```") or s.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        hm = HEADING_RE.match(line)
        if hm:
            anchors.add(github_slug(hm.group(2)))
    return anchors


def check_links():
    failures = []
    anchor_cache = {}
    for rel in tracked_markdown_files():
        path = (ROOT / rel).resolve()
        if path.is_file():
            anchor_cache[path] = collect_anchors(path)

    for rel in tracked_markdown_files():
        path = (ROOT / rel).resolve()
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        in_fence = False
        for lineno, line in enumerate(text.splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith("```") or stripped.startswith("~~~"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            no_code = re.sub(r"`[^`]*`", "", line)
            for m in LINK_RE.finditer(no_code):
                target = m.group(1).strip()
                if target.startswith(("http://", "https://", "mailto:")):
                    continue
                if "#" in target:
                    file_part, anchor = target.split("#", 1)
                else:
                    file_part, anchor = target, None
                resolved = (path.parent / file_part).resolve() if file_part else path
                if not resolved.exists():
                    failures.append(
                        f"{rel}:{lineno}: broken link {target!r} (resolved to {resolved})"
                    )
                elif anchor:
                    if resolved not in anchor_cache and resolved.is_file() and resolved.suffix.lower() == ".md":
                        anchor_cache[resolved] = collect_anchors(resolved)
                    if anchor not in anchor_cache.get(resolved, set()):
                        failures.append(
                            f"{rel}:{lineno}: broken anchor #{anchor} in {target!r}"
                        )

    if failures:
        for f in failures:
            print(f"links: {f}", file=sys.stderr)
        return 1
    print("check_docs links: ok")
    return 0


def extract_c_cpp_fences(md_text):
    blocks = []
    in_fence = False
    lang = None
    cur = []
    start_line = 0
    for lineno, line in enumerate(md_text.splitlines(), 1):
        s = line.strip()
        if not in_fence and s in ("```c", "```cpp"):
            in_fence = True
            lang = s[3:]
            cur = []
            start_line = lineno
        elif in_fence and s == "```":
            in_fence = False
            blocks.append((start_line, lang, "\n".join(cur)))
        elif in_fence:
            cur.append(line)
    return blocks


def has_top_level_function(code):
    return bool(re.search(r"^[A-Za-z_][A-Za-z0-9_\s\*]*\([^;]*\)\s*\{", code, re.MULTILINE))


FRAGMENT_INCLUDE = '#include "rocketnet.h"'


def fragment_source(code):
    """Wrap a fragment (statements with no function of their own) in a function body.

    The file starts with #include "rocketnet.h"; any #include lines the fragment has are
    hoisted to file scope above the function. The function's parameters are the names a
    fragment may use: client, server, client_index, channel and time.
    """
    includes = [FRAGMENT_INCLUDE]
    body = []
    for line in code.splitlines():
        if line.strip().startswith("#include"):
            if line.strip() not in includes:
                includes.append(line.strip())
        else:
            body.append(line)
    return (
        "\n".join(includes)
        + "\n\n"
        + "void rocketnet_usage_fragment( struct rocketnet_client_t * client, "
        "struct rocketnet_server_t * server, int client_index, int channel, double time )\n{\n"
        "    (void) client; (void) server; (void) client_index; (void) channel; (void) time;\n"
        + "\n".join(body)
        + "\n}\n"
    )


def find_compilers():
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    return cc, cxx


def check_selftest():
    """Prove the fragment path: a one-line fragment with no #include compiles as C and C++."""
    fragment = "rocketnet_client_update( client, time );"
    source = fragment_source(fragment)
    if not source.startswith(FRAGMENT_INCLUDE + "\n"):
        print("selftest: fragment source does not start with " + FRAGMENT_INCLUDE, file=sys.stderr)
        return 1
    cc, cxx = find_compilers()
    if not cc or not cxx:
        print("selftest: no C or C++ compiler found (set CC and CXX)", file=sys.stderr)
        return 1
    failures = []
    with tempfile.TemporaryDirectory(prefix="rocketnet-selftest-") as tmpdir:
        tmp = Path(tmpdir)
        for lang, compiler, std_flag in (("c", cc, "-std=c99"), ("cpp", cxx, "-std=c++17")):
            src_file = tmp / f"fragment.{lang}"
            src_file.write_text(source, encoding="utf-8")
            cmd = [compiler, std_flag, "-Wall", "-Werror", "-I", str(ROOT / "sdk"), "-c", str(src_file), "-o", str(tmp / f"fragment_{lang}.o")]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                failures.append(f"one-line fragment failed to compile as {lang}:\n{proc.stderr.strip()}")
    if failures:
        for f in failures:
            print(f"selftest: {f}", file=sys.stderr)
        return 1
    print("check_docs selftest: one-line fragment compiled as c and cpp")
    return 0


def check_usage():
    usage_path = ROOT / "USAGE.md"
    header_path = ROOT / "sdk" / "rocketnet.h"
    config_path = ROOT / "sdk" / "rocketnet_config.cpp"
    internal_path = ROOT / "sdk" / "rocketnet_internal.h"

    haystack = (
        header_path.read_text(encoding="utf-8")
        + "\n"
        + config_path.read_text(encoding="utf-8")
        + "\n"
        + internal_path.read_text(encoding="utf-8")
    )
    known = set(IDENT_RE.findall(haystack))

    usage_text = usage_path.read_text(encoding="utf-8")
    failures = []
    for lineno, line in enumerate(usage_text.splitlines(), 1):
        cleaned = re.sub(r"ROCKETNET_[A-Z0-9_]*\*", "", line)
        cleaned = re.sub(r"ROCKETNET_[A-Z0-9_]*<[^>]+>", "", cleaned)
        for m in IDENT_RE.finditer(cleaned):
            ident = m.group(0)
            if ident not in known:
                failures.append(
                    f"USAGE.md:{lineno}: identifier {ident!r} not in sdk/rocketnet.h or sdk/rocketnet_config.cpp"
                )

    blocks = extract_c_cpp_fences(usage_text)
    if not blocks:
        print("usage: no ```c or ```cpp fenced blocks found in USAGE.md", file=sys.stderr)
        return 1

    cc, cxx = find_compilers()
    if not cc or not cxx:
        print("usage: no C or C++ compiler found (set CC and CXX)", file=sys.stderr)
        return 1
    lib_static = ROOT / "build" / "librocketnet.a"
    lib_shared_so = ROOT / "bin" / "librocketnet.so"
    lib_shared_dylib = ROOT / "bin" / "librocketnet.dylib"

    compiled = 0
    with tempfile.TemporaryDirectory(prefix="rocketnet-usage-") as tmpdir:
        tmp = Path(tmpdir)
        for idx, (lineno, lang, code) in enumerate(blocks, 1):
            ext = "c" if lang == "c" else "cpp"
            compiler = cc if lang == "c" else cxx
            std_flag = "-std=c99" if lang == "c" else "-std=c++17"
            is_main = bool(re.search(r"\bmain\s*\(", code))
            if is_main or has_top_level_function(code):
                source_text = code + "\n"
            else:
                source_text = fragment_source(code)
            src_file = tmp / f"block_{idx}.{ext}"
            src_file.write_text(source_text, encoding="utf-8")
            if is_main:
                out_file = tmp / f"block_{idx}.bin"
                cmd = [compiler, std_flag, "-Wall", "-Werror", "-I", str(ROOT / "sdk"), str(src_file), "-o", str(out_file)]
                if lib_shared_so.is_file():
                    cmd += [str(lib_shared_so)]
                elif lib_shared_dylib.is_file():
                    cmd += [str(lib_shared_dylib)]
                elif lib_static.is_file():
                    cmd += [str(lib_static), "-lstdc++", "-lpthread", "-lm"]
                else:
                    failures.append(f"USAGE.md:{lineno}: built library not found in build/ or bin/ to link whole program")
                    continue
            else:
                out_file = tmp / f"block_{idx}.o"
                cmd = [compiler, std_flag, "-Wall", "-Werror", "-I", str(ROOT / "sdk"), "-c", str(src_file), "-o", str(out_file)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                failures.append(
                    f"USAGE.md:{lineno}: block {idx} ({lang}) failed to compile:\n{proc.stderr.strip()}"
                )
            else:
                compiled += 1

    if failures:
        for f in failures:
            print(f"usage: {f}", file=sys.stderr)
        return 1
    print(f"compiled {compiled} fenced C/C++ block(s) from USAGE.md")
    return 0


def main(argv):
    cmd = argv[0] if argv else "all"
    if cmd == "archaeology":
        return check_archaeology()
    if cmd == "links":
        return check_links()
    if cmd == "usage":
        return check_usage()
    if cmd == "selftest":
        return check_selftest()
    if cmd == "all":
        rc = 0
        rc |= check_archaeology()
        rc |= check_links()
        rc |= check_usage()
        rc |= check_selftest()
        return 1 if rc else 0
    print(f"unknown subcommand: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
