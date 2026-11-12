#!/usr/bin/env python3
"""Refuse the CRT calls MSVC deprecates (C4996) where MSVC compiles them.

    python3 tools/check_msvc_crt.py [--root DIR]
    python3 tools/check_msvc_crt.py --self-test

Every Windows build is /W4 /WX, so one bare fopen, getenv, sprintf, strcpy ...
in a source MSVC compiles is a red build; it was found three times on
2026-11-12 only by a Windows CI run. This checker finds it on Linux first.

It reads every C and C++ source and header under tests/, tools/ and
examples/ (tracked by git under --root, default the repository), with
comments and string literals blanked, and refuses `name(` for every name in
DEPRECATED (the UCRT's C4996 set; see below), unless the call:

- is a member (`x.name(`, `x->name(`) or a namespace call other than `std::`
  (`ns::name(` is not the CRT's), or a declaration's own name;
- sits in a preprocessor region MSVC never compiles: under `#if`/`#ifdef`/
  `#elif` whose condition names __linux__, __APPLE__, __unix__, __FreeBSD__
  or __EMSCRIPTEN__, under `#ifndef _WIN32`/`#if !defined( _WIN32 )`/the
  same for _MSC_VER, or in the `#else` of `#if defined( _WIN32 )`/`_MSC_VER`;
- is in tests/portable_crt.h or tests/resource_file.h, the wrappers that
  route each call through its _s form on MSVC (rocketnet_test_getenv,
  rocketnet_test_fopen, ...);
- is in a file on ALLOW below, which names the file and the reason.

DEPRECATED: the names MSVC itself reports as C4996, measured on captain on
2026-11-12 (Visual Studio 18 2026, Windows SDK 10.0.26100.0): a C file that
takes the address of each of 251 candidates (every name the UCRT headers mark
with _CRT_INSECURE_DEPRECATE, _CRT_NONSTDC_DEPRECATE, _CRT_DEPRECATE_TEXT or
_CRT_OBSOLETE, the __DEFINE_CPP_OVERLOAD_* names such as strcpy and sprintf,
and controls) compiled with cl /W4; 218 warned. The controls printf, fprintf,
memcpy, snprintf and vsnprintf did not, nor did perror, swprintf, vswprintf
or wmemcpy. POSIX is the 99 of them whose message is "The POSIX name for this
item is deprecated". A toolchain that deprecates more is measured again.

One line per refusal: MSVC-CRT path:line name(...): use <wrapper> or guard it.
Exit 0 clean, 1 on a refusal (or nothing read), 2 on a usage error.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEPRECATED = sorted(set("""
    _beep _creat _cscanf _cscanf_l _ctime32 _ctime64 _cwscanf _cwscanf_l _ecvt _fcvt _fscanf_l _ftime32
    _ftime64 _fwscanf_l _gcvt _getdllprocaddr _getsystime _gmtime32 _gmtime64 _i64toa _i64tow _loaddll
    _localtime32 _localtime64 _mbccpy _mbccpy_l _mbstok _mbstok_l _open _scanf_l _seterrormode _setsystime
    _sleep _snprintf _snprintf_l _snscanf _snscanf_l _snwprintf _snwprintf_l _snwscanf _snwscanf_l _sopen
    _splitpath _sprintf_l _sscanf_l _strerror _strlwr _strlwr_l _strncat_l _strncpy_l _strnset _strnset_l
    _strset _strset_l _strtok_l _strupr _strupr_l _swscanf_l _ui64toa _ui64tow _umask _unloaddll _vcscanf
    _vcscanf_l _vcwscanf _vcwscanf_l _vsnprintf _vsnprintf_l _vsnwprintf _vsnwprintf_l _vsnwscanf_l
    _vsprintf_l _wasctime _wcreat _wcserror _wcstok _wctime32 _wctime64 _wctomb_l _wfopen _wfreopen
    _wgetenv _wopen _wscanf_l _wsopen _wsplitpath access asctime cgets chdir chmod chsize close cprintf
    cputs creat cscanf ctime cwait dup dup2 ecvt eof execl execle execlp execlpe execv execve execvp
    execvpe fcloseall fcvt fdopen fgetchar filelength fileno flushall fopen fputchar freopen fscanf fwscanf
    gcvt getch getche getcwd getenv getpid getw gmtime is_wctype isatty itoa j0 j1 jn kbhit lfind localtime
    locking lsearch lseek ltoa mbstowcs memccpy memicmp mkdir mktemp open putch putenv putw read rmdir
    rmtmp scanf setbuf setmode sopen spawnl spawnle spawnlp spawnlpe spawnv spawnve spawnvp spawnvpe
    sprintf sscanf strcat strcmpi strcpy strdup strerror stricmp strlwr strncat strncpy strnicmp strnset
    strrev strset strtok strupr swab swscanf tell tempnam tmpfile tmpnam tzset ultoa umask ungetch unlink
    vsprintf wcscat wcscpy wcsdup wcsicmp wcsicoll wcslwr wcsncat wcsncpy wcsnicmp wcsnset wcsrev wcsset
    wcstok wcstombs wcsupr wctomb write wscanf y0 y1 yn
""".split()))

# the POSIX names (_CRT_NONSTDC_DEPRECATE: "The POSIX name for this item is
# deprecated"); _CRT_NONSTDC_NO_WARNINGS silences these and only these
POSIX = set("""
    access cgets chdir chmod chsize close cprintf cputs creat cscanf cwait dup dup2 ecvt eof execl execle
    execlp execlpe execv execve execvp execvpe fcloseall fcvt fdopen fgetchar filelength fileno flushall
    fputchar gcvt getch getche getcwd getpid getw isatty itoa j0 j1 jn kbhit lfind locking lsearch lseek ltoa
    memccpy memicmp mkdir mktemp open putch putenv putw read rmdir rmtmp setmode sopen spawnl spawnle spawnlp
    spawnlpe spawnv spawnve spawnvp spawnvpe strcmpi strdup stricmp strlwr strnicmp strnset strrev strset
    strupr swab tell tempnam tzset ultoa umask ungetch unlink wcsdup wcsicmp wcsicoll wcslwr wcsnicmp wcsnset
    wcsrev wcsset wcsupr write y0 y1 yn
""".split())

# a file that defines one of these before its includes has chosen the
# deprecated form for that family (the examples do, for getenv and fopen)
SILENCERS = {"_CRT_SECURE_NO_WARNINGS": "insecure", "_CRT_NONSTDC_NO_WARNINGS": "posix",
             "_CRT_NONSTDC_NO_DEPRECATE": "posix"}

WRAPPERS = {"tests/portable_crt.h": "the _s wrappers themselves", "tests/resource_file.h": "the _wfopen_s path wrapper"}

# "path" (the whole file) or "path:name" (that function in that file): reason.
# Only files no MSVC build compiles (the generated Visual Studio projects of
# a full configure on captain, 2026-11-12, list every source MSVC compiles,
# and no workflow compiles these with cl), or calls no MSVC build reaches.
ALLOW = {
    "tests/test_xdp_filter.cpp": "CMake builds test_xdp_filter only with ROCKETNET_BPF_CLANG (Linux XDP)",
    "tests/fuzz/rocketnet_fuzz_xdp_filter.cpp": "Linux-only fuzz leg (LINUX_ONLY_TARGETS); no Windows fuzz matrix row",
    "tools/xdp_packet_blaster.c": "CMake builds it only on Linux with ROCKETNET_XDP",
    "tools/xver/xver.c": "CMake: if(NOT WIN32), POSIX only (getpid, unistd.h)",
    "tools/testdata/nightly_suite_timeout.cpp": "an orphan fixture (b316f8450): no build, script or workflow names it",
}

SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc")
SCOPES = ("tests/", "tools/", "examples/")

NOT_ON_MSVC = re.compile(r"__linux__|__APPLE__|__unix__|__FreeBSD__|__EMSCRIPTEN__")
WIN = re.compile(r"_WIN32|_MSC_VER|_WIN64")


def blank(text):
    """Comments and string/char literals to spaces, newlines kept."""
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
            out.append(c + " " * max(0, j - i - 2) + (c if j - i >= 2 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def skipped_lines(lines):
    """The set of 1-based lines MSVC never compiles, from the #if structure."""
    skipped = set()
    stack = []  # per #if: [this_branch_skipped, an_earlier_branch_was_windows_only]
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if m:
            kind, cond = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                if kind == "ifdef":
                    cond = "defined(%s)" % cond.strip()
                elif kind == "ifndef":
                    cond = "!defined(%s)" % cond.strip()
                stack.append([branch_skipped(cond), windows_only(cond)])
            elif kind == "elif" and stack:
                stack[-1] = [branch_skipped(cond) or stack[-1][1], stack[-1][1] or windows_only(cond)]
            elif kind == "else" and stack:
                stack[-1] = [stack[-1][1], stack[-1][1]]
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if any(frame[0] for frame in stack):
            skipped.add(number)
    return skipped


def term_not_on_msvc(term):
    """One && term that is false under MSVC."""
    term = term.strip("()")
    return bool(NOT_ON_MSVC.search(term)
                or re.fullmatch(r"!defined\(?(_WIN32|_MSC_VER|_WIN64)\)?", term)
                or re.fullmatch(r"ROCKETNET_PLATFORM==ROCKETNET_PLATFORM_(MAC|LINUX)", term)
                or re.fullmatch(r"ROCKETNET_PLATFORM!=ROCKETNET_PLATFORM_WINDOWS", term)
                # sdk/rocketnet_internal.h: 1 only under __linux__ (CMake defines it on Linux only)
                or re.fullmatch(r"ROCKETNET_XDP(==1|!=0)?", term))


def branch_skipped(cond):
    """MSVC never takes this branch: every || alternative has a && term false under MSVC."""
    squeezed = re.sub(r"//.*|\s+", "", cond)
    alternatives = squeezed.split("||")
    return all(any(term_not_on_msvc(t) for t in alt.split("&&")) for alt in alternatives)


def windows_only(cond):
    """This branch is taken only on Windows/MSVC, so a following #else is not."""
    squeezed = re.sub(r"//.*|\s+", "", cond).strip("()")
    return bool(re.fullmatch(r"defined\(?(_WIN32|_MSC_VER|_WIN64)\)?", squeezed)
                or re.fullmatch(r"ROCKETNET_PLATFORM==ROCKETNET_PLATFORM_WINDOWS", squeezed))


CALL = re.compile(r"(?<![A-Za-z0-9_])(%s)\s*\(" % "|".join(re.escape(name) for name in DEPRECATED))


def scan(path, text):
    refusals = []
    clean = blank(text)
    lines = clean.split("\n")
    skipped = skipped_lines(lines)
    silenced = set()
    for name, family in SILENCERS.items():
        if re.search(r"^\s*#\s*define\s+%s\b" % name, clean, re.M):
            silenced.add(family)
    # `#define getpid _getpid`: the file maps the name to another function
    redefined = set(re.findall(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+[A-Za-z_]", clean, re.M))
    allowed = {key.split(":", 1)[1] for key in ALLOW if key.startswith(path + ":")}
    for number, line in enumerate(lines, 1):
        if number in skipped or line.lstrip().startswith("#"):
            continue
        for m in CALL.finditer(line):
            if ("posix" if m.group(1) in POSIX else "insecure") in silenced:
                continue
            if m.group(1) in redefined or m.group(1) in allowed:
                continue
            before = line[:m.start()].rstrip()
            if before.endswith(".") or before.endswith("->"):
                continue
            if before.endswith("::") and not before.endswith("std::"):
                if before[:-2].rstrip() and re.search(r"[A-Za-z0-9_]$", before[:-2].rstrip()):
                    continue
            # a declaration or definition of a function by that name (a type before it)
            if re.search(r"\b(static|inline|int|void|char|bool|size_t|FILE|ssize_t|unsigned|long|double)\s*\*?\s*$", before):
                continue
            refusals.append("MSVC-CRT %s:%d %s(...): MSVC deprecates it (C4996, an error under /WX); "
                            "use the tests/portable_crt.h wrapper or guard it off Windows" % (path, number, m.group(1)))
    return refusals


def tracked(root):
    try:
        listing = subprocess.run(["git", "-C", root, "ls-files", "-z", "--"] + list(SCOPES), check=True,
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
        out.write("check failed: no C or C++ source under %s in %s; nothing was checked\n" % (" ".join(SCOPES), root))
        return 1
    refusals = []
    for path in files:
        if path in WRAPPERS or path in ALLOW:
            continue
        with open(os.path.join(root, path), errors="replace") as handle:
            refusals += scan(path, handle.read())
    for key in sorted(ALLOW):
        path, _, name = key.partition(":")
        if path not in files:
            refusals.append("MSVC-CRT allow-list entry %s names no tracked file" % key)
        elif name and name not in DEPRECATED:
            refusals.append("MSVC-CRT allow-list entry %s names no deprecated function" % key)
    for line in refusals:
        out.write(line + "\n")
    out.write("check_msvc_crt: %d files, %d names, %d refused\n" % (len(files), len(DEPRECATED), len(refusals)))
    return 1 if refusals else 0


def self_test():
    failed = 0
    cases = []
    # one plant per name: a bare call is refused
    for name in DEPRECATED:
        cases.append(("bare %s" % name, "void f( void ) { x = %s( a, b ); }\n" % name, True))
    cases += [
        ("a member call passes", "void f( void ) { s.open( a ); p->close(); }\n", False),
        ("a namespace's own function passes", "void f( void ) { util::fopen( a ); }\n", False),
        ("std:: is still the CRT", "void f( void ) { std::fopen( a, b ); }\n", True),
        ("a comment passes", "// fopen( a, b )\n/* getenv( x ) */\n", False),
        ("a string passes", "const char * s = \"fopen( a, b )\";\n", False),
        ("a __linux__ region passes", "#if defined( __linux__ )\nFILE * f = fopen( a, b );\n#endif\n", False),
        ("the #else of _WIN32 passes", "#if defined( _WIN32 )\nx();\n#else\nint fd = open( a, 0 );\n#endif\n", False),
        ("the _WIN32 branch is checked", "#if defined( _WIN32 )\nFILE * f = fopen( a, b );\n#else\nx();\n#endif\n", True),
        ("#ifndef _WIN32 passes", "#ifndef _WIN32\nclose( fd );\n#endif\n", False),
        ("#if ROCKETNET_XDP passes", "#if ROCKETNET_XDP\nstrcpy( a, b );\n#endif\n", False),
        ("a name the file redefines passes", "#if defined( _WIN32 )\n#define getpid _getpid\n#endif\nint p = getpid();\n", False),
        ("after the #endif is checked","#if defined( __linux__ )\nx();\n#endif\nclose( fd );\n", True),
        ("ROCKETNET_PLATFORM != WINDOWS passes", "#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS\nclose( fd );\n#endif\n", False),
        ("the #else of == WINDOWS passes", "#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS\nx();\n#else\nclose( fd );\n#endif\n", False),
        ("== LINUX || == MAC passes", "#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC\nclose( fd );\n#endif\n", False),
        ("== WINDOWS is checked", "#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS\nclose( fd );\n#endif\n", True),
        ("__linux__ && X passes", "#if defined( __linux__ ) && ROCKETNET_DEV\nclose( fd );\n#endif\n", False),
        ("__linux__ || _WIN32 is checked","#if defined( __linux__ ) || defined( _WIN32 )\nclose( fd );\n#endif\n", True),
        ("a definition's own name passes", "static int read( int fd );\n", False),
        ("an identifier containing a name passes", "void f( void ) { my_fopen( a ); reread( b ); }\n", False),
        ("_CRT_SECURE_NO_WARNINGS silences fopen", "#define _CRT_SECURE_NO_WARNINGS\nFILE * f = fopen( a, b );\n", False),
        ("_CRT_SECURE_NO_WARNINGS leaves POSIX names", "#define _CRT_SECURE_NO_WARNINGS\nclose( fd );\n", True),
        ("_CRT_NONSTDC_NO_WARNINGS silences close", "#define _CRT_NONSTDC_NO_WARNINGS\nclose( fd );\n", False),
        ("_CRT_NONSTDC_NO_WARNINGS leaves fopen", "#define _CRT_NONSTDC_NO_WARNINGS\nFILE * f = fopen( a, b );\n", True),
    ]
    for name, text, want_refused in cases:
        refused = bool(scan("plant.cpp", text))
        right = refused == want_refused
        if not right:
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
    right = empty_status == 1 and "nothing was checked" in "".join(sink)
    if not right:
        print("self-test %-44s FAIL" % "an empty tree is a failure")
        failed += 1
    total = len(cases) + 1
    print("check_msvc_crt self-test: %d cases (%d bare-call plants), %d failed" % (total, len(DEPRECATED), failed))
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
