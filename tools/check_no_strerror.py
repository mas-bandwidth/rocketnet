#!/usr/bin/env python3
"""Refuse strerror and its kin in the library code a Linux build compiles.

    python3 tools/check_no_strerror.py [--root DIR]
    python3 tools/check_no_strerror.py --self-test

glibc's strerror, strerror_r and strerror_l look the message up through
gettext, which calls malloc and free on every call (two of each per call,
measured on glibc 2.43, in the C locale too); perror and printf's %m format
the same message. A library line that names an error that way calls the
system allocator, outside the caller's allocator, each time it runs: the
refused thread priority call did so at every client connect of an
unprivileged process, and the debug-level send failure lines at every failed
send whatever the log level. The library names an OS error with
rocketnet_platform_socket_error_text (sdk/rocketnet_platform.h) instead: the
number and its errno.h name from a fixed table.

It reads every C and C++ source and header under sdk/ (tracked by git under
--root, default the repository), with comments blanked, and refuses

- a call `name(` for strerror, strerror_r, strerror_l and perror (string and
  character literals blanked first; a member call `x.name(` or `x->name(`
  and another identifier that merely contains the name pass);
- a `%m` conversion inside a string literal (`%%m` is a literal percent sign
  and passes);

unless the line sits in a preprocessor region only an Apple build compiles:
under `#if`/`#ifdef`/`#elif` whose condition, in every `||` alternative, has
an `&&` term that is `defined( __APPLE__ )`, `ROCKETNET_PLATFORM ==
ROCKETNET_PLATFORM_MAC` or `ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS`.
Apple's strerror makes no allocation for an errno it knows (measured on macOS
27.2), so the msgx pump and send failure lines keep it. An `#else` is never
Apple-only.

One line per refusal: NO-STRERROR path:line what: why.
Exit 0 clean, 1 on a refusal (or nothing read), 2 on a usage error.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

NAMES = ("strerror", "strerror_r", "strerror_l", "perror")
SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc")
SCOPE = "sdk/"

CALL = re.compile(r"(?<![A-Za-z0-9_])(%s)\s*\(" % "|".join(NAMES))
APPLE_TERM = re.compile(r"defined\(?__APPLE__\)?|ROCKETNET_PLATFORM==ROCKETNET_PLATFORM_(MAC|IOS)")


def blank(text, strings):
    """Comments to spaces, newlines kept; string and character literals too when strings is true."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            if strings:
                out.append(c + " " * max(0, j - i - 2) + (c if j - i >= 2 else ""))
            else:
                out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def apple_only(cond):
    """Only an Apple build takes this branch: every || alternative has an Apple && term."""
    squeezed = re.sub(r"//.*|\s+", "", cond)
    if not squeezed:
        return False
    return all(any(APPLE_TERM.fullmatch(term.strip("()")) for term in alt.split("&&")) for alt in squeezed.split("||"))


def apple_lines(lines):
    """The set of 1-based lines only an Apple build compiles, from the #if structure."""
    apple = set()
    stack = []
    for number, raw in enumerate(lines, 1):
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", raw.strip())
        if m:
            kind, cond = m.group(1), m.group(2)
            if kind == "if":
                stack.append(apple_only(cond))
            elif kind == "ifdef":
                stack.append(apple_only("defined(%s)" % cond.strip()))
            elif kind == "ifndef":
                stack.append(False)
            elif kind == "elif" and stack:
                stack[-1] = apple_only(cond)
            elif kind == "else" and stack:
                stack[-1] = False
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if any(stack):
            apple.add(number)
    return apple


def scan(path, text):
    refusals = []
    code = blank(text, True).split("\n")
    with_strings = blank(text, False).split("\n")
    apple = apple_lines(code)
    for number, line in enumerate(code, 1):
        if number in apple or line.lstrip().startswith("#"):
            continue
        for m in CALL.finditer(line):
            before = line[:m.start()].rstrip()
            if before.endswith(".") or before.endswith("->"):
                continue
            refusals.append("NO-STRERROR %s:%d %s(...): glibc's allocates on every call; name the error with "
                            "rocketnet_platform_socket_error_text, or keep the line in an Apple-only region" % (path, number, m.group(1)))
        for literal in re.findall(r'"((?:[^"\\\n]|\\.)*)"', with_strings[number - 1]):
            if "%m" in literal.replace("%%", ""):
                refusals.append("NO-STRERROR %s:%d %%m in a format string: glibc formats it as strerror does; name the error with "
                                "rocketnet_platform_socket_error_text" % (path, number))
    return refusals


def tracked(root):
    try:
        listing = subprocess.run(["git", "-C", root, "ls-files", "-z", "--", SCOPE], check=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.decode()
    except (OSError, subprocess.CalledProcessError) as error:
        return None, "git ls-files failed under %s: %s" % (root, error)
    return [p for p in listing.split("\0") if p.endswith(SUFFIXES)], None


def check(root, out=sys.stdout):
    files, error = tracked(root)
    if error:
        out.write("check failed: %s\n" % error)
        return 1
    if not files:
        out.write("check failed: no C or C++ source under %s in %s; nothing was checked\n" % (SCOPE, root))
        return 1
    refusals = []
    for path in files:
        with open(os.path.join(root, path), errors="replace") as handle:
            refusals += scan(path, handle.read())
    for line in refusals:
        out.write(line + "\n")
    out.write("check_no_strerror: %d files, %d refused\n" % (len(files), len(refusals)))
    return 1 if refusals else 0


def self_test():
    failed = 0
    cases = []
    for name in NAMES:
        cases.append(("bare %s" % name, "void f( void ) { log( \"%%s\", %s( error ) ); }\n" % name, True))
    cases += [
        ("%m in a format string", "void f( void ) { log( \"send failed: %m\" ); }\n", True),
        ("%m after other conversions", "void f( void ) { log( \"%d sends failed (%m)\", n ); }\n", True),
        ("%%m is a literal percent sign", "void f( void ) { log( \"100%%match\" ); }\n", False),
        ("%s and %d pass", "void f( void ) { log( \"errno %d %s\", error, name ); }\n", False),
        ("%m outside a string passes", "int f( int a, int m ) { return a %m; }\n", False),
        ("a comment passes", "// strerror( error ) and %m\n/* perror( \"x\" ) */\n", False),
        ("the name in a string passes", "const char * s = \"strerror( error )\";\n", False),
        ("a member call passes", "void f( void ) { table.strerror( 1 ); p->perror( 2 ); }\n", False),
        ("an identifier containing a name passes", "void f( void ) { my_strerror( 1 ); strerror_text( 2 ); }\n", False),
        ("an __APPLE__ region passes", "#if defined( __APPLE__ )\nlog( strerror( error ) );\n#endif\n", False),
        ("#ifdef __APPLE__ passes", "#ifdef __APPLE__\nlog( strerror( error ) );\n#endif\n", False),
        ("__APPLE__ && X passes", "#if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC\nlog( strerror( error ) );\n#endif\n", False),
        ("== MAC || == IOS passes", "#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS\nlog( strerror( error ) );\n#endif\n", False),
        ("nested in an __APPLE__ region passes", "#if defined( __APPLE__ )\n#if ROCKETNET_DEV\nlog( strerror( error ) );\n#endif\n#endif\n", False),
        ("the #elif __APPLE__ branch passes", "#if defined( __linux__ )\nx();\n#elif defined( __APPLE__ )\nlog( strerror( error ) );\n#endif\n", False),
        ("__APPLE__ || __linux__ is checked", "#if defined( __APPLE__ ) || defined( __linux__ )\nlog( strerror( error ) );\n#endif\n", True),
        ("the #else of __APPLE__ is checked", "#if defined( __APPLE__ )\nx();\n#else\nlog( strerror( error ) );\n#endif\n", True),
        ("the #elif after __APPLE__ is checked", "#if defined( __APPLE__ )\nx();\n#elif defined( __linux__ )\nlog( strerror( error ) );\n#endif\n", True),
        ("after the #endif is checked", "#if defined( __APPLE__ )\nx();\n#endif\nlog( strerror( error ) );\n", True),
        ("a __linux__ region is checked", "#if defined( __linux__ )\nlog( strerror_r( error, text, sizeof( text ) ) );\n#endif\n", True),
        ("a _WIN32 region is checked", "#if defined( _WIN32 )\nperror( \"x\" );\n#endif\n", True),
        ("#ifndef __APPLE__ is checked", "#ifndef __APPLE__\nlog( strerror( error ) );\n#endif\n", True),
        ("!defined( __APPLE__ ) is checked", "#if !defined( __APPLE__ )\nlog( strerror( error ) );\n#endif\n", True),
    ]
    for name, text, want_refused in cases:
        refused = bool(scan("plant.cpp", text))
        if refused != want_refused:
            print("self-test %-44s FAIL (refused=%s)" % (name, refused))
            failed += 1
    # a repository with nothing to read is red, never "0 refused"
    with tempfile.TemporaryDirectory() as empty:
        subprocess.run(["git", "init", "-q", empty], check=True)
        sink = []

        class Sink:
            def write(self, text):
                sink.append(text)
        empty_status = check(empty, Sink())
    if not (empty_status == 1 and "nothing was checked" in "".join(sink)):
        print("self-test %-44s FAIL" % "an empty tree is a failure")
        failed += 1
    total = len(cases) + 1
    print("check_no_strerror self-test: %d cases, %d failed" % (total, failed))
    return 1 if failed else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    root = ROOT
    if len(argv) == 2 and argv[0] == "--root":
        root = argv[1]
    elif argv:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    return check(root)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
