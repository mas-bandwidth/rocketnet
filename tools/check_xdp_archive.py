#!/usr/bin/env python3
"""Check the embedded server_xdp.o against its sources.

The library build compiles sdk/xdp/server_xdp.c to a BPF object and embeds
its bytes in build/generated/rocketnet_server_xdp.h (cmake/EmbedXdp.cmake).
On Linux with xxd and a clang that has the BPF target this checks that

  - two builds of the same sources from different checkout paths, mtimes and
    umasks give the same object bytes (the object is reproducible);
  - the generated header's array holds exactly the object it was made from;
  - a build tree's embedded object equals a fresh compile of the current
    sources with the tree's own BPF clang, ROCKETNET_HISTORY_PACKETS and
    ROCKETNET_MAX_CLIENTS (generated/xdp-build-settings.txt), so a stale
    object fails and a current one at non-default settings passes (pass the
    build tree, default ./build);
  - a changed source gives a different object, so the stale check can fail;
  - an embed given no MAX_CLIENTS or HISTORY_PACKETS compiles the program at
    the SDK's defaults, the same object as one given the defaults, and its
    header compiles into a library at the defaults;
  - the generated header refuses a library built with another
    ROCKETNET_MAX_CLIENTS or ROCKETNET_HISTORY_PACKETS than the program, and
    accepts a custom ROCKETNET_MAX_CLIENTS=10000 passed to both.

Elsewhere, or with --committed-only, it checks that every input the shared
list (cmake/XdpSources.cmake) names exists; that check needs no compiler.
"""
import argparse
import hashlib
import locale
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

if sys.platform == "win32" and not getattr(sys.flags, "utf8_mode", 0):
    os.environ["PYTHONUTF8"] = "1"
    if sys.argv and sys.argv[0] and Path(sys.argv[0]).is_file():
        raise SystemExit(subprocess.run([sys.executable, "-X", "utf8", *sys.argv], env=os.environ).returncode)


def utf8_env():
    return {**os.environ, "PYTHONUTF8": "1"}


def inputs_for(root, directory):
    root, directory = Path(root), Path(directory)
    if not root.exists():
        try:
            recovered = Path(str(root).encode(locale.getpreferredencoding(False)).decode("utf-8"))
            if recovered.exists():
                root = recovered
        except (UnicodeEncodeError, UnicodeDecodeError):
            pass
    listing = directory / "source-list.txt"
    subprocess.run(["cmake", f"-DROCKETNET_XDP_SOURCE_LIST_OUTPUT={str(listing)}",
                    "-P", str(root / "cmake/XdpSources.cmake")],
                   check=True, encoding="utf-8", env=utf8_env())
    paths = [root / name for name in listing.read_text(encoding="utf-8").splitlines()]
    if not paths:
        raise RuntimeError("the XDP source list is empty")
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"XDP source list names a missing input: {path}")
    return paths


def embedded_bytes(header):
    text = header.read_text(encoding="utf-8")
    body = re.search(r"rocketnet_server_xdp_o\[\d*\]\s*=\s*\{(.*?)\};", text, re.S)
    length = re.search(r"rocketnet_server_xdp_o_len\s*=\s*(\d+)\s*;", text)
    if not body or not length:
        raise RuntimeError(f"missing server_xdp.o byte array or length: {header}")
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", body[1]))
    count = int(length[1])
    if len(data) != count:
        raise RuntimeError(f"server_xdp.o array length mismatch: {header}")
    return data


def multiarch():
    cc = os.environ.get("CC", "cc")
    return subprocess.run([cc, "-dumpmachine"], check=True, capture_output=True,
                          encoding="utf-8").stdout.strip()


# The default ROCKETNET_MAX_CLIENTS (sdk/rocketnet.h, CMakeLists.txt).
DEFAULT_MAX_CLIENTS = 1000


def build_settings(build, clang):
    """The BPF clang, ROCKETNET_HISTORY_PACKETS and ROCKETNET_MAX_CLIENTS a
    build tree embedded its object with: lines 1, 3 and 4 of
    generated/xdp-build-settings.txt (CMakeLists.txt). With no build tree it
    is the given clang, no history (the SDK default) and the default client
    count."""
    if build is None:
        return clang, None, DEFAULT_MAX_CLIENTS
    settings = build / "generated/xdp-build-settings.txt"
    if not settings.exists():
        return clang, None, DEFAULT_MAX_CLIENTS
    lines = [line.strip() for line in settings.read_text(encoding="utf-8").splitlines()]
    if len(lines) < 4 or not lines[2].isdigit() or not lines[3].isdigit():
        raise RuntimeError(f"{settings} names no ROCKETNET_HISTORY_PACKETS and ROCKETNET_MAX_CLIENTS")
    if not Path(lines[0]).is_file():
        raise RuntimeError(f"{settings} names a BPF clang that is not there: {lines[0]}")
    return lines[0], int(lines[2]), int(lines[3])


def embed(root, output, xxd, clang, max_clients, history=None):
    """Runs cmake/EmbedXdp.cmake; max_clients or history None passes no
    MAX_CLIENTS or HISTORY_PACKETS, so the program takes the SDK default."""
    limits = [] if max_clients is None else [f"-DMAX_CLIENTS={max_clients}"]
    limits += [] if history is None else [f"-DHISTORY_PACKETS={history}"]
    subprocess.run(["cmake", f"-DSOURCE_DIR={str(root)}", f"-DOUTPUT_DIR={str(output)}",
                    f"-DXXD={str(xxd)}", f"-DCLANG={str(clang)}", f"-DMULTIARCH={multiarch()}",
                    *limits,
                    "-P", str(root / "cmake/EmbedXdp.cmake")],
                   check=True, encoding="utf-8", env=utf8_env())
    data = (output / "server_xdp.o").read_bytes()
    if not data.startswith(b"\x7fELF"):
        raise RuntimeError(f"{output / 'server_xdp.o'} is not an ELF object")
    if embedded_bytes(output / "rocketnet_server_xdp.h") != data:
        raise RuntimeError("the generated header's array differs from server_xdp.o")
    return data


def library_compiles(root, output, defines):
    """Compiles a library source that includes rocketnet.h and the generated
    header, as sdk/rocketnet_server_socket_linux.cpp does, with the given
    library defines; returns (compiled, diagnostics)."""
    source = output / "library_probe.c"
    source.write_text('#include "rocketnet.h"\n#include <rocketnet_server_xdp.h>\n'
                      "int rocketnet_library_probe(void);\n"
                      "int rocketnet_library_probe(void) { return (int) rocketnet_server_xdp_o_len + rocketnet_server_xdp_o[0]; }\n",
                      encoding="utf-8")
    cc = os.environ.get("CC", "cc")
    result = subprocess.run([cc, *defines, "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                             f"-I{str(output)}", f"-I{str(root / 'sdk')}", str(source)],
                            capture_output=True, encoding="utf-8", env=utf8_env())
    return result.returncode == 0, result.stderr


def check_embed_limits(source, directory, inputs, xxd, clang):
    root = directory / "limits"
    copy_tree(source, root, inputs, 1_000_000_000)

    unset = directory / "generated unset"
    default_object = embed(root, unset, xxd, clang, None)
    explicit_object = embed(root, directory / "generated default", xxd, clang, DEFAULT_MAX_CLIENTS)
    if default_object != explicit_object:
        raise RuntimeError("an embed given no MAX_CLIENTS did not compile the program at the SDK default")
    header = (unset / "rocketnet_server_xdp.h").read_text(encoding="utf-8")
    if f"#define ROCKETNET_SERVER_XDP_MAX_CLIENTS {DEFAULT_MAX_CLIENTS}\n" not in header:
        raise RuntimeError("the generated header does not record the default ROCKETNET_MAX_CLIENTS")
    compiled, diagnostics = library_compiles(root, unset, [])
    if not compiled:
        raise RuntimeError(f"the default embed does not compile into a default library:\n{diagnostics}")
    print("test_xdp_embed_default_needs_no_defines")

    for define, macro in (("-DROCKETNET_MAX_CLIENTS=10000", "ROCKETNET_MAX_CLIENTS"),
                          ("-DROCKETNET_HISTORY_PACKETS=8192", "ROCKETNET_HISTORY_PACKETS")):
        compiled, diagnostics = library_compiles(root, unset, [define])
        if compiled or f"another {macro} than this library" not in diagnostics:
            raise RuntimeError(f"a library built with {define} accepted a default program:\n{diagnostics}")
    print("test_xdp_embed_mismatch_is_refused")

    custom = directory / "generated custom"
    embed(root, custom, xxd, clang, 10000)
    compiled, diagnostics = library_compiles(root, custom, ["-DROCKETNET_MAX_CLIENTS=10000"])
    if not compiled:
        raise RuntimeError(f"a 10000-client embed does not compile into a 10000-client library:\n{diagnostics}")
    compiled, diagnostics = library_compiles(root, custom, [])
    if compiled or "another ROCKETNET_MAX_CLIENTS than this library" not in diagnostics:
        raise RuntimeError(f"a default library accepted a 10000-client program:\n{diagnostics}")
    print("test_xdp_embed_custom_max_clients")


def check_build_settings(source, directory, inputs, xxd, clang):
    """A build tree configured with -DROCKETNET_HISTORY_PACKETS=8192 is
    current when its object was embedded at 8192, and stale when its settings
    say 8192 but its object was embedded at the default."""
    root = directory / "settings"
    copy_tree(source, root, inputs, 1_000_000_000)
    for name, embedded_history, current in (("history 8192", 8192, True), ("history default", None, False)):
        probe = directory / f"probe {name}"
        (probe / "generated").mkdir(parents=True)
        (probe / "generated/xdp-build-settings.txt").write_text(
            f"{clang}\n{multiarch()}\n8192\n{DEFAULT_MAX_CLIENTS}\n", encoding="utf-8")
        embed(root, probe / "generated", xxd, clang, DEFAULT_MAX_CLIENTS, embedded_history)
        tree_clang, history, max_clients = build_settings(probe, None)
        fresh = embed(root, directory / f"fresh {name}", xxd, tree_clang, max_clients, history)
        try:
            check_fresh(probe, fresh)
        except RuntimeError:
            if current:
                raise
        else:
            if not current:
                raise RuntimeError("an object embedded at the default history passed against settings that name 8192")
    print("test_xdp_object_current_at_build_settings")


def copy_tree(source, root, inputs, mtime):
    for path in inputs:
        target = root / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        os.utime(target, (mtime, mtime))
    (root / "cmake").mkdir(exist_ok=True)
    for name in ("XdpSources.cmake", "EmbedXdp.cmake"):
        shutil.copyfile(source / "cmake" / name, root / "cmake" / name)


def check_fresh(build, fresh):
    header = build / "generated/rocketnet_server_xdp.h"
    embedded = embedded_bytes(header)
    if embedded != fresh:
        digests = [hashlib.sha256(data).hexdigest() for data in (embedded, fresh)]
        raise RuntimeError(f"stale embedded server_xdp.o in {header}: embedded {digests[0]}, "
                           f"current sources compile to {digests[1]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, nargs="?", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build", type=Path, help="build tree whose embedded object is checked (default: <source>/build when it has one)")
    parser.add_argument("--committed-only", action="store_true", help="check only that the listed inputs exist; needs no compiler")
    args = parser.parse_args()
    source = args.source.resolve()
    work = source / "build"
    work.mkdir(exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="xdp object ", dir=work) as directory:
        directory = Path(directory)
        inputs = inputs_for(source, directory)
        print("test_xdp_sources")
        if args.committed_only or not sys.platform.startswith("linux"):
            return
        xxd, clang = shutil.which("xxd"), shutil.which("clang")
        build = args.build or (work if (work / "generated/rocketnet_server_xdp.h").exists() else None)
        clang, history, max_clients = build_settings(build.resolve() if build is not None else None, clang)
        if not xxd or not clang:
            raise SystemExit("xxd and clang with the BPF target are required for the object checks")

        objects = []
        for index, (mtime, umask) in enumerate([(1_000_000_000, 0o022), (1_700_000_000, 0o077)]):
            root = directory / f"checkout {index}" / ("a" * (index + 1))
            copy_tree(source, root, inputs, mtime)
            if index:
                # Cross a wall-clock second boundary so anything taken from
                # the build time differs between the two builds.
                time.sleep(1.1)
            previous_umask = os.umask(umask)
            try:
                objects.append(embed(root, directory / f"generated {index}", xxd, clang, max_clients, history))
            finally:
                os.umask(previous_umask)
        if objects[0] != objects[1]:
            digests = [hashlib.sha256(data).hexdigest() for data in objects]
            raise RuntimeError(f"server_xdp.o is not reproducible: {digests}")
        print("test_xdp_object_reproducible")

        # A changed source must change the object, or the stale check below
        # could never fail.
        changed = directory / "changed"
        copy_tree(source, changed, inputs, 1_000_000_000)
        filter_source = changed / "sdk/xdp/server_xdp.c"
        filter_source.write_text(filter_source.read_text(encoding="utf-8")
                                 + "\nchar rocketnet_xdp_stale_probe[] SEC(\".rodata\") = \"stale\";\n",
                                 encoding="utf-8")
        stale = embed(changed, directory / "generated changed", xxd, clang, max_clients, history)
        probe = directory / "probe build"
        (probe / "generated").mkdir(parents=True)
        shutil.copyfile(directory / "generated changed/rocketnet_server_xdp.h", probe / "generated/rocketnet_server_xdp.h")
        try:
            check_fresh(probe, objects[0])
        except RuntimeError:
            pass
        else:
            raise RuntimeError("an object built from a changed source passed the stale check")
        if stale == objects[0]:
            raise RuntimeError("a changed source compiled to the same object")
        print("test_xdp_object_stale_is_red")

        check_embed_limits(source, directory, inputs, xxd, clang)
        check_build_settings(source, directory, inputs, xxd, clang)

        if build is not None:
            check_fresh(build.resolve(), objects[0])
            print(f"test_xdp_object_current ({build})")
    print("test_xdp_archive")


if __name__ == "__main__":
    main()
