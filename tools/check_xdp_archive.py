#!/usr/bin/env python3
"""Check embedded XDP source contents after dirty incremental staging."""
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

source = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
inputs = {
    "Makefile": source / "server_xdp/Makefile",
    "server_xdp.c": source / "server_xdp/server_xdp.c",
    "rocketnet_wire.h": source / "sdk/rocketnet_wire.h",
    "rocketnet_xdp_limits.h": source / "sdk/rocketnet_xdp_limits.h",
}
xxd = shutil.which("xxd")
if not xxd:
    raise SystemExit("xxd is required")
work = source / "build"
work.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="xdp archive ", dir=work) as directory:
    output = pathlib.Path(directory)
    staging = output / "source"
    staging.mkdir()
    for iteration in range(2):
        (staging / "server_xdp.o").write_bytes(b"stale object")
        (staging / "stray.c").write_text("not a source input\n")
        (staging / "nested").mkdir(exist_ok=True)
        (staging / "nested/unrelated").write_text("keep out\n")
        subprocess.run(["cmake", f"-DSOURCE_DIR={source}", f"-DOUTPUT_DIR={output}",
                        f"-DXXD={xxd}", "-P", str(source / "cmake/EmbedXdp.cmake")], check=True)
        with tarfile.open(output / "rocketnet_server_xdp.tar.gz") as archive:
            members = archive.getmembers()
            if len(members) != len(inputs) or {m.name for m in members} != {"./" + name for name in inputs}:
                raise SystemExit(f"unexpected XDP archive entries: {[m.name for m in members]}")
            for member in members:
                if not member.isfile() or archive.extractfile(member).read() != inputs[member.name[2:]].read_bytes():
                    raise SystemExit(f"XDP source mismatch: {member.name}")
        expected = subprocess.check_output([xxd, "-i", "-n", "rocketnet_server_xdp_tar_gz",
                                            str(output / "rocketnet_server_xdp.tar.gz")])
        if (output / "rocketnet_server_xdp.h").read_bytes() != expected:
            raise SystemExit("embedded header differs from source archive")
print("test_xdp_archive")
