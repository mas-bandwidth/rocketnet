#!/usr/bin/env python3
"""Every fenced C block in the documentation is compiled or checked against its source.

Reads every ```c (or ```cpp) block in docs/*.md and README.md and puts each
in exactly one class:

  unit      the block's first line starts `#include "rocketnet.h"`: it is a whole
            C translation unit, and it must compile against sdk/rocketnet.h
            (cc -std=c11 -fsyntax-only, warnings as errors, unused names
            allowed, since a doc unit defines functions nothing calls);
  cited     tools/docs/check_getting_started.py holds it to its cited
            example lines (this check runs that one and takes its OK list);
  excerpt   listed in EXCERPTS: a quotation of sdk/rocketnet.h, each of
            whose declarations must occur there, whitespace aside;
  fragment  listed in FRAGMENTS with the declarations it assumes: it is
            compiled inside a function after them.

A block in no class fails, and so does a list entry that names no block
(the lists never outlive the docs). A unit or fragment that does not
compile fails with the compiler's first error. Prints one line per block.

  --self-test   plant each refusal in a scratch docs tree and fail if any is let through
"""

import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

FENCE = re.compile(r"^```(?:c|cpp|c\+\+)\s*$")
UNIT_FIRST_LINE = '#include "rocketnet.h"'

# (doc, the block's first line): why it is a quotation of the header
EXCERPTS = {
    ("docs/ERRORS-AND-LOGGING.md", "typedef void (*rocketnet_log_function_t)("):
        "the log callback's declaration, quoted from sdk/rocketnet.h",
}

# (doc, the block's first line): the declarations the fragment assumes
FRAGMENTS = {
    ("docs/LINUX.md", "server_config.num_os_cpus = 1;"):
        "struct rocketnet_server_config_t server_config;\nrocketnet_server_default_config( &server_config );",
    ("docs/USAGE.md", "struct rocketnet_client_config_t config;"):
        "",
}

COMPILE = ["-std=c11", "-fsyntax-only", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
           "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
           "-Wno-unused-but-set-variable", "-x", "c", "-"]


def doc_files(root):
    files = sorted((root / "docs").glob("*.md"))
    readme = root / "README.md"
    if readme.is_file():
        files.append(readme)
    return files


def blocks(root):
    """Return (doc, fence line, [lines]) for every fenced C block."""
    found = []
    for path in doc_files(root):
        lines = path.read_text(encoding="utf-8").splitlines()
        index = 0
        while index < len(lines):
            if FENCE.match(lines[index]):
                end = index + 1
                while end < len(lines) and not lines[end].startswith("```"):
                    end += 1
                found.append((path.relative_to(root).as_posix(), index + 1, lines[index + 1:end]))
                index = end
            index += 1
    return found


def first_line(body):
    for line in body:
        if line.strip():
            return line.strip()
    return ""


def compile_text(compiler, root, text):
    """Return None when the text compiles, else the compiler's first error line."""
    result = subprocess.run([compiler, "-I", str(root / "sdk")] + COMPILE, input=text,
                            capture_output=True, text=True)
    if result.returncode == 0:
        return None
    for line in result.stderr.splitlines():
        if "error" in line:
            return line.strip()
    return (result.stderr.strip() or "exit %d" % result.returncode)[:300]


def declarations(text):
    """The text's declarations with every space removed, split at each semicolon."""
    squeezed = re.sub(r"\s+", "", text)
    return [part for part in squeezed.split(";") if part]


def cited_blocks(root):
    """The (doc, fence line) pairs check_getting_started.py checked, and whether it passed."""
    import check_getting_started
    saved = check_getting_started.ROOT
    check_getting_started.ROOT = root
    out = io.StringIO()
    err = io.StringIO()
    failed = False
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            for guide in (root / "docs" / "GETTING-STARTED.md", root / "docs" / "PRODUCTION.md"):
                if guide.is_file() and check_getting_started.check_guide(guide):
                    failed = True
    finally:
        check_getting_started.ROOT = saved
    cited = set()
    for line in out.getvalue().splitlines():
        match = re.match(r"^OK (\S+):(\d+) == ", line)
        if match:
            cited.add((match.group(1), int(match.group(2))))
    return cited, failed, err.getvalue().strip()


def check(root=ROOT, excerpts=None, fragments=None, compiler=None):
    """Return (problems, report lines)."""
    excerpts = EXCERPTS if excerpts is None else excerpts
    fragments = FRAGMENTS if fragments is None else fragments
    compiler = compiler or os.environ.get("CC") or shutil.which("cc") or "cc"
    header = "".join(part + ";" for part in declarations((root / "sdk" / "rocketnet.h").read_text(encoding="utf-8")))
    cited, cited_failed, cited_errors = cited_blocks(root)
    problems = []
    report = []
    if cited_failed:
        problems.append("tools/docs/check_getting_started.py fails: %s" % cited_errors.splitlines()[-1:])
    used = set()
    for doc, line, body in blocks(root):
        where = "%s:%d" % (doc, line)
        head = first_line(body)
        key = (doc, head)
        if head.startswith(UNIT_FIRST_LINE):
            error = compile_text(compiler, root, "\n".join(body) + "\n")
            if error:
                problems.append("%s: C unit does not compile: %s" % (where, error))
            else:
                report.append("compiled  %s (%d lines)" % (where, len(body)))
        elif (doc, line) in cited:
            report.append("cited     %s (check_getting_started.py)" % where)
        elif key in excerpts:
            used.add(key)
            if not all(part + ";" in header for part in declarations("\n".join(body))):
                problems.append("%s: listed as a quotation of sdk/rocketnet.h, but the header does not hold it" % where)
            else:
                report.append("excerpt   %s (in sdk/rocketnet.h)" % where)
        elif key in fragments:
            used.add(key)
            text = "%s\nvoid rocketnet_doc_fragment( void )\n{\n%s\n%s\n}\n" % (
                UNIT_FIRST_LINE, fragments[key], "\n".join(body))
            error = compile_text(compiler, root, text)
            if error:
                problems.append("%s: C fragment does not compile in its listed context: %s" % (where, error))
            else:
                report.append("fragment  %s (compiled in its listed context)" % where)
        else:
            problems.append("%s: C block is neither a unit (first line %s), cited, an excerpt nor a listed "
                            "fragment: %r" % (where, UNIT_FIRST_LINE, head[:80]))
    for key in sorted(set(excerpts) | set(fragments)):
        if key not in used:
            problems.append("%s: listed block %r is no longer a C block there" % key)
    return problems, report


def self_test(compiler=None):
    """Plant each refusal in a scratch tree; return the cases let through."""
    sdk_header = (ROOT / "sdk").resolve()
    cases = [
        ("a non-C line in a unit (VERIFY-5 P10)",
         {"docs/USAGE.md": "```c\n#include \"rocketnet.h\"\nthis is not C;\n```\n"}, {}, {},
         "C unit does not compile"),
        ("a unit calling a function the API does not have",
         {"docs/USAGE.md": "```c\n#include \"rocketnet.h\"\nvoid f( void ) { rocketnet_invented_call( 1 ); }\n```\n"},
         {}, {}, "C unit does not compile"),
        ("an unlisted fragment",
         {"docs/USAGE.md": "```c\nconfig.transport = \"xdp\";\n```\n"}, {}, {},
         "is neither a unit"),
        ("a listed fragment that no longer compiles",
         {"docs/USAGE.md": "```c\nconfig.no_such_field = 1;\n```\n"}, {},
         {("docs/USAGE.md", "config.no_such_field = 1;"): "struct rocketnet_client_config_t config;"},
         "C fragment does not compile"),
        ("an excerpt the header does not hold",
         {"docs/USAGE.md": "```c\nint rocketnet_not_in_the_header( void );\n```\n"},
         {("docs/USAGE.md", "int rocketnet_not_in_the_header( void );"): "a quotation"}, {},
         "the header does not hold it"),
        ("a stale list entry",
         {"docs/USAGE.md": "No code here.\n"}, {},
         {("docs/USAGE.md", "config.gone = 1;"): ""},
         "is no longer a C block there"),
    ]
    clean_docs = {"docs/USAGE.md": "```c\n#include \"rocketnet.h\"\nint f( void ) { return rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK; }\n```\n"
                                   "```c\nstruct rocketnet_client_config_t config;\nrocketnet_client_default_config( &config );\n```\n"}
    clean_fragments = {("docs/USAGE.md", "struct rocketnet_client_config_t config;"): ""}
    let_through = []
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "docs").mkdir()
        (root / "sdk").symlink_to(sdk_header)

        def write(docs):
            for path in (root / "docs").glob("*.md"):
                path.unlink()
            for name, text in docs.items():
                (root / name).write_text(text, encoding="utf-8")

        write(clean_docs)
        problems, _ = check(root, {}, clean_fragments, compiler)
        if problems:
            let_through.append("clean scratch tree was refused: %s" % problems)
        for label, docs, excerpts, fragments, expected in cases:
            write(docs)
            problems, _ = check(root, excerpts, fragments, compiler)
            if not any(expected in problem for problem in problems):
                let_through.append("%s was not refused (%r); reported %s" % (label, expected, problems))
    return let_through, len(cases)


def main():
    if "--self-test" in sys.argv[1:]:
        let_through, count = self_test()
        for line in let_through:
            print("check_doc_c_blocks: self-test: " + line, file=sys.stderr)
        if let_through:
            return 1
        print("check_doc_c_blocks: self-test: %d planted blocks refused, clean tree passes" % count)
        return 0
    problems, report = check()
    for line in report:
        print(line)
    for line in problems:
        print("check_doc_c_blocks: " + line, file=sys.stderr)
    if problems:
        return 1
    print("check_doc_c_blocks: %d C blocks in the docs: each compiled, cited or quoted from the header" % len(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
