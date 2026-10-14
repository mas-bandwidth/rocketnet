#!/usr/bin/env python3
"""Check generated and committed XDP archives against their shared source list."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import time

def inputs_for(root, directory):
    listing = directory / "source-list.txt"
    subprocess.run(["cmake", f"-DROCKETNET_XDP_SOURCE_LIST_OUTPUT={listing}",
                    "-P", str(root / "cmake/XdpSources.cmake")], check=True)
    paths = [root / name for name in listing.read_text().splitlines()]
    inputs = {path.name: path for path in paths}
    if not inputs or len(inputs) != len(paths):
        raise RuntimeError("XDP source basenames must be nonempty and unique")
    return inputs


def check_archive(data, inputs):
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        members = archive.getmembers()
        if len(members) != len(inputs) or {m.name for m in members} != {"./" + name for name in inputs}:
            raise RuntimeError(f"unexpected XDP archive entries: {[m.name for m in members]}")
        for member in members:
            if not member.isfile() or archive.extractfile(member).read() != inputs[member.name[2:]].read_bytes():
                raise RuntimeError(f"XDP source mismatch: {member.name}")


def embedded_bytes(header):
    text = header.read_text()
    body = re.search(r"rocketnet_server_xdp_tar_gz\[\]\s*=\s*\{(.*?)\};", text, re.S)
    length = re.search(r"rocketnet_server_xdp_tar_gz_len\s*=\s*(\d+)\s*;", text)
    if not body or not length:
        raise RuntimeError(f"missing XDP byte array or length: {header}")
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", body[1]))
    if len(data) != int(length[1]):
        raise RuntimeError(f"XDP array length mismatch: {header}")
    return data


def generate_and_check(root, output, inputs, xxd):
    staging = output / "source"
    staging.mkdir(parents=True, exist_ok=True)
    (staging / "server_xdp.o").write_bytes(b"stale object")
    (staging / "stray.c").write_text("not a source input\n")
    (staging / "nested").mkdir(exist_ok=True)
    (staging / "nested/unrelated").write_text("keep out\n")
    subprocess.run(["cmake", f"-DSOURCE_DIR={root}", f"-DOUTPUT_DIR={output}",
                    f"-DXXD={xxd}", "-P", str(root / "cmake/EmbedXdp.cmake")], check=True)
    data = (output / "rocketnet_server_xdp.tar.gz").read_bytes()
    check_archive(data, inputs)
    if embedded_bytes(output / "rocketnet_server_xdp.h") != data:
        raise RuntimeError("embedded header differs from source archive")


def check_reproducible(source, directory, inputs, xxd):
    archives = []
    builds = [(1_000_000_000, 0o022), (1_700_000_000, 0o077)]
    for index, (mtime, umask) in enumerate(builds):
        root = directory / f"repro{index}"
        for path in inputs.values():
            target = root / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            os.utime(target, (mtime, mtime))
        (root / "cmake").mkdir(exist_ok=True)
        for name in ("XdpSources.cmake", "EmbedXdp.cmake"):
            shutil.copyfile(source / "cmake" / name, root / "cmake" / name)
        if index:
            # Cross a wall-clock second boundary so a member timestamp
            # taken from the build time differs between the two builds.
            time.sleep(1.1)
        previous_umask = os.umask(umask)
        try:
            output = directory / f"repro{index}-generated"
            subprocess.run(["cmake", f"-DSOURCE_DIR={root}", f"-DOUTPUT_DIR={output}",
                            f"-DXXD={xxd}", "-P", str(root / "cmake/EmbedXdp.cmake")], check=True)
        finally:
            os.umask(previous_umask)
        archives.append((output / "rocketnet_server_xdp.tar.gz").read_bytes())

    if archives[0] != archives[1]:
        digests = [hashlib.sha256(data).hexdigest() for data in archives]
        raise RuntimeError(f"XDP archive is not reproducible: {digests}")
    with tarfile.open(fileobj=io.BytesIO(archives[0])) as archive:
        for member in archive.getmembers():
            if member.mtime != 0 or member.uid != 0 or member.gid != 0:
                raise RuntimeError(f"XDP archive member is not normalised: {member.name}")
    print("test_xdp_archive_reproducible")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--committed-only", action="store_true", help="check the Windows fallback without requiring xxd")
    args = parser.parse_args()
    source = args.source.resolve()
    work = source / "build"
    work.mkdir(exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="xdp archive ", dir=work) as directory:
        directory = Path(directory)
        inputs = inputs_for(source, directory)
        # Compare decoded source content, independent of archive timestamps.
        check_archive(embedded_bytes(source / "sdk/rocketnet_server_xdp.h"), inputs)
        if not args.committed_only:
            xxd = shutil.which("xxd")
            if not xxd:
                raise SystemExit("xxd is required for generation checks")
            for _ in range(2):
                generate_and_check(source, directory / "generated", inputs, xxd)

            check_reproducible(source, directory, inputs, xxd)

            # A maintainer adds one source in the shared list. Copy, tar and the
            # verifier must all follow it without their own additional name edits.
            fixture = directory / "fixture"
            for path in inputs.values():
                target = fixture / path.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
            (fixture / "cmake").mkdir(exist_ok=True)
            for name in ("XdpSources.cmake", "EmbedXdp.cmake"):
                shutil.copyfile(source / "cmake" / name, fixture / "cmake" / name)
            manifest = fixture / "cmake/XdpSources.cmake"
            text = manifest.read_text()
            end = text.index(")", text.index("set(ROCKETNET_XDP_SOURCES"))
            manifest.write_text(text[:end] + "\n    sdk/future_xdp_input.h" + text[end:])
            (fixture / "sdk/future_xdp_input.h").write_text("/* future archive input */\n")
            extended = inputs_for(fixture, directory)
            if len(extended) != len(inputs) + 1 or "future_xdp_input.h" not in extended:
                raise RuntimeError("future source was not added to the shared list")
            generate_and_check(fixture, directory / "future", extended, xxd)
    print("test_xdp_archive")


if __name__ == "__main__":
    main()
