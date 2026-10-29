#!/usr/bin/env python3
"""Check that sdk/rocketnet.h is a clean, self-contained, plain-C public header.

(a) the header alone compiles as C99, C11 and C++11, pedantic, warnings as
    errors, plain and with ROCKETNET_SHARED and ROCKETNET_DEV defined;
(b) every #include names only stdint.h, stddef.h or stdbool.h;
(c) every function and data declaration carries ROCKETNET_EXPORT and sits
    inside the one extern "C" block, and no inline (the two default_config
    inlines excepted, in their one exact form), static, default argument,
    overload, namespace, class, template, operator or reference appears;
(d) a fresh empty directory holding only rocketnet.h and the prebuilt
    development static library builds, links and runs tools/header_alone as C
    and as C++: a message server and client on 127.0.0.1 exchange one message
    each way;
(e) no comment cites an internal design reference (a proofs/tla/ module, a docs/
    design note, a README- note, a pull request or issue number); user
    documents such as USAGE.md and BUILDING.md may be cited.

Prints one line per check and exits 0 only when every check passes.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from check_dll_consumer import msvc_environment, msvc_tool

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "sdk" / "rocketnet.h"
ALLOWED_INCLUDES = {"stdint.h", "stddef.h", "stdbool.h"}
ALLOWED_INLINES = ("rocketnet_client_default_config", "rocketnet_server_default_config")
ALLOWED_LIGHT_CONFIG_INLINES = (
    ("rocketnet_client_light_config", "rocketnet_client_config_t", "rocketnet_client_light_config_internal"),
    ("rocketnet_server_light_config", "rocketnet_server_config_t", "rocketnet_server_light_config_internal"),
)
BANNED_WORDS = ("inline", "__inline", "__forceinline", "static", "namespace", "class",
                "template", "operator", "typename", "using", "virtual")

failures = []


def report(ok, line):
    print(("ok    " if ok else "FAIL  ") + line)
    if not ok:
        failures.append(line)


def strip_comments(text):
    # comments become spaces, newlines kept so line numbers survive
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                  lambda m: m.group(0) if m.group(0).startswith('"') else blank(m), text, flags=re.S)


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


# (e) -----------------------------------------------------------------------

INTERNAL_REFERENCE = re.compile(r"proofs/tla/|\.tla\b|docs/|README-|PR #|#[0-9]{3}")


def check_no_internal_references(text):
    found = 0
    for match in re.finditer(r"//[^\n]*|/\*.*?\*/", text, flags=re.S):
        hit = INTERNAL_REFERENCE.search(match.group(0))
        if hit:
            found += 1
            report(False, "(e) line %d: comment cites an internal reference: %s" %
                   (line_of(text, match.start() + hit.start()), hit.group(0)))
    if not found:
        report(True, "(e) no comment cites an internal design reference")


# (a) -----------------------------------------------------------------------

def compile_alone(compiler_c, compiler_cxx):
    if sys.platform == "win32":
        compile_alone_cl()
        return
    languages = [
        ("C99", compiler_c, ["-std=c99", "-pedantic", "-Wall", "-Wextra", "-Wstrict-prototypes", "-Werror", "-fsyntax-only", "-x", "c"]),
        ("C11", compiler_c, ["-std=c11", "-pedantic", "-Wall", "-Wextra", "-Wstrict-prototypes", "-Werror", "-fsyntax-only", "-x", "c"]),
        ("C++11", compiler_cxx, ["-std=c++11", "-pedantic", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-x", "c++"]),
    ]
    variants = [("", []), (" with ROCKETNET_SHARED", ["-DROCKETNET_SHARED"]),
                (" with ROCKETNET_DEV=1", ["-DROCKETNET_DEV=1"])]
    # a translation unit that includes the header, so its static inline
    # default_config calls are an included header's, as in every program
    unit_directory = tempfile.mkdtemp(prefix="rocketnet-header-alone-")
    unit = Path(unit_directory) / "include_rocketnet.c"
    unit.write_text('#include "rocketnet.h"\n')
    for name, compiler, flags in languages:
        for suffix, defines in variants:
            command = [compiler] + flags + defines + ["-I", str(HEADER.parent), str(unit)]
            result = subprocess.run(command, capture_output=True, text=True)
            detail = "" if result.returncode == 0 else ": " + (result.stderr.strip().splitlines() or ["exit %d" % result.returncode])[0]
            report(result.returncode == 0, "(a) the header alone compiles as %s%s%s" % (name, suffix, detail))


def compile_alone_cl():
    # cl has no C99 or C++11 mode: its default C, /std:c11 and /std:c++14
    # (its oldest C++ mode) stand in; /Zs is syntax only, /W4 /WX warnings as errors
    try:
        compiler = msvc_tool("cl")
        environment = msvc_environment()
    except (RuntimeError, OSError, KeyError, subprocess.CalledProcessError) as error:
        report(False, "(a) no MSVC cl to compile the header alone: %s" % error)
        return
    languages = [
        ("C (cl default)", [], "/Tc"),
        ("C11 (cl /std:c11)", ["/std:c11"], "/Tc"),
        ("C++14 (cl /std:c++14)", ["/std:c++14", "/permissive-"], "/Tp"),
    ]
    variants = [("", []), (" with ROCKETNET_SHARED", ["/DROCKETNET_SHARED"]),
                (" with ROCKETNET_DEV=1", ["/DROCKETNET_DEV=1"])]
    for name, flags, source_flag in languages:
        for suffix, defines in variants:
            unit_directory = tempfile.mkdtemp(prefix="rocketnet-header-alone-")
            unit = Path(unit_directory) / "include_rocketnet.c"
            unit.write_text('#include "rocketnet.h"\n')
            command = [compiler, "/nologo", "/Zs", "/W4", "/WX", "/I" + str(HEADER.parent)] + flags + defines + [source_flag + str(unit)]
            result = subprocess.run(command, capture_output=True, text=True, env=environment)
            lines = [line for line in (result.stdout + result.stderr).strip().splitlines() if line.strip() and line.strip() != HEADER.name]
            detail = "" if result.returncode == 0 else ": " + (lines or ["exit %d" % result.returncode])[0]
            report(result.returncode == 0, "(a) the header alone compiles as %s%s%s" % (name, suffix, detail))


def check_channel_bound(compiler_c, compiler_cxx):
    """The bound is 32 by default, and a build that defines it (the library and the program alike) gets its value."""
    variants = [("default", [], 32),
                ("defined", ["ROCKETNET_MAX_CHANNELS=512"], 512)]
    with tempfile.TemporaryDirectory(prefix="rocketnet-channel-bound-") as directory:
        source = Path(directory) / "bound.c"
        for label, defines, expected in variants:
            source.write_text('#include "rocketnet.h"\n'
                              '#if ROCKETNET_MAX_CHANNELS != %d\n' % expected +
                              '#error "channel bound does not match the library build"\n'
                              '#endif\n')
            if sys.platform == "win32":
                try:
                    compiler = msvc_tool("cl")
                    environment = msvc_environment()
                except (RuntimeError, OSError, KeyError, subprocess.CalledProcessError) as error:
                    report(False, "(a) channel bound: no MSVC cl: %s" % error)
                    return
                commands = [("C", [compiler, "/nologo", "/Zs", "/W4", "/WX", "/Tc" + str(source)]),
                            ("C++", [compiler, "/nologo", "/Zs", "/W4", "/WX", "/Tp" + str(source)])]
                flags = ["/I" + str(HEADER.parent)] + ["/D" + d for d in defines]
            else:
                environment = None
                commands = [("C99", [compiler_c, "-std=c99", "-x", "c"]),
                            ("C++11", [compiler_cxx, "-std=c++11", "-x", "c++"])]
                flags = ["-pedantic", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                         "-I", str(HEADER.parent)] + ["-D" + d for d in defines] + [str(source)]
            for language, command in commands:
                result = subprocess.run(command + flags, capture_output=True, text=True, env=environment)
                detail = "" if result.returncode == 0 else ": " + (result.stderr + result.stdout).strip()
                report(result.returncode == 0, "(a) %s channel bound is %d in %s%s" % (label, expected, language, detail))


# (b) -----------------------------------------------------------------------

def check_includes(text):
    found = False
    for number, line in enumerate(text.splitlines(), 1):
        match = re.match(r"\s*#\s*include\s*([<\"])([^>\"]+)[>\"]", line)
        if not match:
            if re.match(r"\s*#\s*include", line):
                report(False, "(b) line %d: an #include this check cannot read: %s" % (number, line.strip()))
            continue
        found = True
        name = match.group(2)
        if match.group(1) != "<" or name not in ALLOWED_INCLUDES:
            report(False, "(b) line %d: #include %s%s%s is not a standard header this API allows" % (number, match.group(1), name, ">" if match.group(1) == "<" else '"'))
    if not any(line.startswith("(b) line") for line in failures):
        report(found, "(b) every #include is one of %s" % ", ".join(sorted(ALLOWED_INCLUDES)))


# (c) -----------------------------------------------------------------------

def check_declarations(raw):
    text = strip_comments(raw)

    # preprocessor lines (with continuations) are not declarations
    code_chars = list(text)
    for match in re.finditer(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", text, flags=re.M):
        for i in range(match.start(), match.end()):
            if code_chars[i] != "\n":
                code_chars[i] = " "
    code = "".join(code_chars)

    # exactly one extern "C" block, opened and closed under #ifdef __cplusplus
    opens = [m for m in re.finditer(r'extern\s*"C"', text)]
    open_block = re.search(r'#ifdef\s+__cplusplus\s*\n\s*extern\s*"C"\s*\{\s*\n\s*#endif', text)
    close_block = None
    for m in re.finditer(r'#ifdef\s+__cplusplus\s*\n\s*\}\s*\n\s*#endif', text):
        close_block = m
    one_block = len(opens) == 1 and open_block is not None and close_block is not None and close_block.start() > open_block.end()
    report(one_block, '(c) one #ifdef __cplusplus extern "C" { #endif ... } block, and no other extern "C"')
    if not one_block:
        return
    block_start = open_block.end()
    block_end = close_block.start()

    # the block's own brace and the extern "C" are not part of any declaration
    code = code[:open_block.start()] + re.sub(r"[^\n]", " ", code[open_block.start():open_block.end()]) + code[open_block.end():]
    code = code[:close_block.start()] + re.sub(r"[^\n]", " ", code[close_block.start():close_block.end()]) + code[close_block.end():]

    # The allowed inline config wrappers are exact forms. The default_config
    # wrappers set config_bytes and call the library's _internal fill; the
    # light_config wrappers do the same for the size-preserving public view.
    # Exactly these, each once, are blanked here so the declaration checks
    # below still apply to every other public declaration.
    for name in ALLOWED_INLINES:
        pattern = (r"static\s+inline\s+enum\s+rocketnet_status_t\s+%s\s*\(\s*struct\s+(\w+)\s*\*\s*config\s*\)\s*\{"
                   r"\s*if\s*\(\s*config\s*\)\s*config->config_bytes\s*=\s*sizeof\s*\(\s*\*config\s*\)\s*;"
                   r"\s*return\s+%s_internal\s*\(\s*config\s*\)\s*;\s*\}") % ( name, name )
        found = list(re.finditer(pattern, code))
        report(len(found) == 1, "(c) the inline %s is the one allowed form, exactly once" % name)
        for m in found:
            code = code[:m.start()] + re.sub(r"[^\n]", " ", code[m.start():m.end()]) + code[m.end():]

    for name, config_type, internal_name in ALLOWED_LIGHT_CONFIG_INLINES:
        pattern = (r"static\s+inline\s+enum\s+rocketnet_status_t\s+%s\s*\(\s*struct\s+%s\s*\*\s*config\s*\)\s*\{"
                   r"\s*if\s*\(\s*config\s*\)\s*config->config_bytes\s*=\s*sizeof\s*\(\s*\*config\s*\)\s*;"
                   r"\s*return\s+%s\s*\(\s*config\s*\)\s*;\s*\}") % (name, config_type, internal_name)
        found = list(re.finditer(pattern, code))
        report(len(found) == 1, "(c) the inline %s is the one allowed form, exactly once" % name)
        for m in found:
            code = code[:m.start()] + re.sub(r"[^\n]", " ", code[m.start():m.end()]) + code[m.end():]

    # the enum-size static asserts, under their language guards, are no declarations
    for m in list(re.finditer(r"(?:_Static_assert|static_assert)\s*\((?:[^;]*?)\)\s*;", code, flags=re.S)):
        code = code[:m.start()] + re.sub(r"[^\n]", " ", code[m.start():m.end()]) + code[m.end():]

    for word in BANNED_WORDS:
        for m in re.finditer(r"\b%s\b" % word, code):
            report(False, "(c) line %d: '%s' has no place in a plain C public header" % (line_of(code, m.start()), word))
    for m in re.finditer(r"&", code):
        report(False, "(c) line %d: '&' (a C++ reference) in a declaration" % line_of(code, m.start()))

    # top-level statements: split at ';' outside braces
    statements, depth, start = [], 0, 0
    for i, ch in enumerate(code):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            segment = code[start:i]
            words = segment.split()
            if depth == 0 and words and words[0] not in ("struct", "enum", "union", "typedef") and "(" in segment[:segment.index("{")]:
                report(False, "(c) line %d: a function body: a public header declares, never defines" % line_of(code, start + len(code[start:i]) - len(code[start:i].lstrip())))
                statements.append((start, code[start:i]))   # a function body ends its statement
                start = i + 1
        elif ch == ";" and depth == 0:
            statements.append((start, code[start:i]))
            start = i + 1
    if code[start:].strip():
        report(False, "(c) line %d: text after the last declaration: %s" % (line_of(code, start), code[start:].strip()[:60]))

    names = {}
    num_functions = num_data = 0
    for offset, statement in statements:
        body = " ".join(statement.split())
        if not body:
            continue
        first = offset + len(statement) - len(statement.lstrip())
        number = line_of(code, first)
        if body.startswith("typedef ") or re.match(r"(struct|enum|union)\s+\w+$", body):
            continue                                    # a typedef or an opaque handle
        if re.match(r"(struct|enum|union)\b[^(]*\{", body):
            if body[body.rindex("}") + 1:].strip():
                report(False, "(c) line %d: data declared after a type body: %s" % (number, body[body.rindex("}") + 1:].strip()))
            continue                                    # a struct or an enum
        if "(" not in body:
            report(False, "(c) line %d: a declaration that is neither a type nor a function: %s" % (number, body[:60]))
            continue
        data = re.match(r"[^(]*\(\s*\*\s*(\w+)\s*\)\s*\(", body)
        if data and body.split("(")[0].split()[0] == "extern":
            name = data.group(1)
            num_data += 1
        else:
            match = re.match(r"([^(]*?)(\w+)\s*\(", body)
            if not match:
                report(False, "(c) line %d: cannot read the declaration: %s" % (number, body[:60]))
                continue
            name = match.group(2)
            num_functions += 1
            params = body[body.index("(") + 1:body.rindex(")")]
            if re.fullmatch(r"\s*", params):
                report(False, "(c) line %d: %s() is an old-style prototype; write ( void )" % (number, name))
            if "=" in body:
                report(False, "(c) line %d: %s has a default argument" % (number, name))
        if not re.search(r"\bROCKETNET_EXPORT\b", body):
            report(False, "(c) line %d: %s does not carry ROCKETNET_EXPORT" % (number, name))
        if not ( block_start <= first <= block_end ):
            report(False, '(c) line %d: %s sits outside the extern "C" block' % (number, name))
        if name in names:
            report(False, "(c) line %d: %s is declared again (first at line %d): an overload" % (number, name, names[name]))
        names.setdefault(name, number)

    if not any(line.startswith("(c) line") for line in failures):
        report(num_functions > 0, '(c) %d functions and %d data declarations, each ROCKETNET_EXPORT, each once, all inside the extern "C" block; no inline, static, default argument, overload, namespace, class, template or reference' % (num_functions, num_data))
    return num_functions, num_data


# (d) -----------------------------------------------------------------------

def cmake_cache(build):
    values = {}
    cache = build / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(errors="replace").splitlines():
            match = re.match(r"([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
            if match:
                values[match.group(1)] = match.group(2)
    return values


def system_libraries(build):
    # the platform's system libraries the prebuilt static library links
    # against, plus the C++ runtime: nothing of RocketNet's own
    if sys.platform == "darwin":
        return ["-lc++", "-framework", "CoreFoundation", "-framework", "SystemConfiguration"]
    libraries = []
    cache = cmake_cache(build)
    for dependency in ("Bpf", "Xdp", "Uring"):
        value = cache.get("ROCKETNET_%s_LIBRARY" % dependency, "")
        if value and not value.endswith("-NOTFOUND"):
            libraries.append(value)
    return libraries + ["-lstdc++", "-lm", "-lpthread"]


HEADER_ALONE_INCLUDES = {"main.c": {"rocketnet.h", "stdio.h", "stdlib.h", "string.h", "time.h"}, "main.cpp": {"main.c"}}


def self_contained(build, config, compiler_c, compiler_cxx):
    sources = ROOT / "tools" / "header_alone"
    # the programs include rocketnet.h and standard C headers only
    for name, allowed in sorted(HEADER_ALONE_INCLUDES.items()):
        included = re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', (sources / name).read_text(), flags=re.M)
        extra = [header for header in included if header not in allowed]
        report(not extra, "(d) tools/header_alone/%s includes %s" % (name, ", ".join(included)) +
               ("" if not extra else ": not allowed: " + ", ".join(extra)))
    if sys.platform == "win32":
        library = build / config / "rocketnet_dev.lib"
    else:
        library = build / "librocketnet_dev.a"
    if not library.exists():
        report(False, "(d) no prebuilt development static library at %s" % library)
        return
    directory = Path(tempfile.mkdtemp(prefix="rocketnet_header_alone_"))
    source_directory = Path(tempfile.mkdtemp(prefix="rocketnet_header_alone_source_"))
    # no include path but the directory: the compiler's environment paths are cleared
    if sys.platform == "win32":
        try:
            base_environment = msvc_environment()
            cl = msvc_tool("cl")
        except (RuntimeError, OSError, KeyError, subprocess.CalledProcessError) as error:
            report(False, "(d) no MSVC cl to build tools/header_alone: %s" % error)
            return
    else:
        base_environment = os.environ
    environment = {key: value for key, value in base_environment.items()
                   if key not in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH")}
    try:
        # the sources compile from their own directory, which holds only them,
        # so a quoted include finds nothing beside the source
        for name in HEADER_ALONE_INCLUDES:
            shutil.copy(sources / name, source_directory / name)
        shutil.copy(HEADER, directory / "rocketnet.h")
        shutil.copy(library, directory / library.name)
        contents = sorted(os.listdir(directory))
        report(contents == sorted(["rocketnet.h", library.name]),
               "(d) the directory holds only %s" % ", ".join(contents))
        builds = [
            ("C99", compiler_c, ["-std=c99", "-pedantic", "-Wall", "-Wextra", "-Werror"], source_directory / "main.c", "header_alone_c"),
            ("C++11", compiler_cxx, ["-std=c++11", "-pedantic", "-Wall", "-Wextra", "-Werror"], source_directory / "main.cpp", "header_alone_cpp"),
        ]
        for name, compiler, flags, source, program in builds:
            if sys.platform == "win32":
                # /Tc or /Tp names the one source file's language; the libraries stay plain link inputs
                crt = "/MDd" if config.lower() == "debug" else "/MD"
                command = [cl, "/nologo", "/W4", "/WX", crt, "/D_CRT_SECURE_NO_WARNINGS", "/I", str(directory),
                           ("/Tc" if name == "C99" else "/Tp") + str(source), str(directory / library.name),
                           "ws2_32.lib", "iphlpapi.lib", "advapi32.lib",
                           "/Fe:" + str(directory / (program + ".exe")), "/Fo:" + str(directory) + "\\"]
                executable = directory / (program + ".exe")
            else:
                executable = directory / program
                command = [compiler] + flags + ["-I", str(directory), str(source), str(directory / library.name)] + \
                          system_libraries(build) + ["-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True, cwd=directory, env=environment)
            if result.returncode != 0:
                report(False, "(d) %s client and server build against rocketnet.h and %s alone: %s" %
                       (name, library.name, (result.stderr.strip().splitlines() or
                                             [line for line in result.stdout.splitlines() if "error" in line] or
                                             ["exit %d" % result.returncode])[0]))
                continue
            report(True, "(d) %s client and server build against rocketnet.h and %s alone" % (name, library.name))
            try:
                run = subprocess.run([str(executable)], capture_output=True, text=True, cwd=directory, timeout=60)
                output = (run.stdout.strip().splitlines() or [""])[-1]
                errors = (run.stderr.strip().splitlines() or [""])[-1]
                report(run.returncode == 0, "(d) %s run exits %d: %s" % (name, run.returncode, output if run.returncode == 0 else errors))
            except subprocess.TimeoutExpired:
                report(False, "(d) %s run did not finish in 60 seconds" % name)
    finally:
        shutil.rmtree(directory, ignore_errors=True)
        shutil.rmtree(source_directory, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, default=ROOT / "build", help="the build directory holding the prebuilt static library")
    parser.add_argument("--config", default="Release", help="the Windows build configuration")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--no-run", action="store_true", help="checks (a) to (c) only, no build of tools/header_alone")
    parser.add_argument("--header", type=Path, help="check this header instead of sdk/rocketnet.h")
    args = parser.parse_args()
    global HEADER
    if args.header:
        HEADER = args.header.resolve()
    build = args.build if args.build.is_absolute() else Path.cwd() / args.build

    raw = HEADER.read_text()
    print("%s: %d lines" % (HEADER.relative_to(ROOT) if ROOT in HEADER.parents else HEADER, len(raw.splitlines())))
    compile_alone(args.cc, args.cxx)
    check_channel_bound(args.cc, args.cxx)
    check_includes(raw)
    check_declarations(raw)
    check_no_internal_references(raw)
    if not args.no_run:
        self_contained(build, args.config, args.cc, args.cxx)
    if failures:
        print("public header: %d check(s) failed" % len(failures))
        return 1
    print("public header: clean, plain C and self-contained")
    return 0


if __name__ == "__main__":
    sys.exit(main())
