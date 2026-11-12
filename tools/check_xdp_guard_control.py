#!/usr/bin/env python3
"""Prove the native staging test rejects removal of both inactive-index guards."""
import argparse
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1],
                    help="git checkout whose HEAD is archived and built (default: this repository)")
root = parser.parse_args().root.resolve()
build = root / "build"
build.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="xdp guard control ", dir=build) as directory:
    work = Path(directory)
    source = work / "source"
    source.mkdir()
    try:
        archived = subprocess.check_output(["git", "archive", "HEAD"], cwd=root)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"0 files: git archive HEAD under {root} exited {error.returncode}: nothing to check")
    with tarfile.open(fileobj=io.BytesIO(archived)) as archive:
        archive.extractall(source, filter="data")
    file = source / "sdk/rocketnet_server_socket_linux.cpp"
    if not file.is_file():
        raise SystemExit(f"0 files: the archive of HEAD under {root} holds no sdk/rocketnet_server_socket_linux.cpp")
    text = file.read_text()
    guard = "    if ( flow_index < 0 )\n    {\n        return;\n    }\n"
    if text.count(guard) != 2:
        raise SystemExit(f"expected the two native finish/abort guards, found {text.count(guard)}")
    file.write_text(text.replace(guard, ""))
    output = work / "bin"
    subprocess.run(["cmake", "-S", str(source), "-B", str(work / "build"),
                    "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={output}",
                    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
                    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"], check=True)
    cache = (work / "build/CMakeCache.txt").read_text().splitlines()
    compiler = next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_CXX_COMPILER:"))
    print("Inactive-index negative control compiler:", flush=True)
    subprocess.run([compiler, "--version"], check=True)
    subprocess.run(["cmake", "--build", str(work / "build"), "--target", "test_xdp", "--parallel", "2"], check=True)
    env = os.environ.copy()
    env.update(ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    result = subprocess.run([str(output / "test_xdp")], env=env, text=True, capture_output=True)
    if result.returncode == 0 or "index -1 out of bounds" not in result.stderr:
        raise SystemExit(f"inactive-index control did not produce the expected sanitizer failure: {result.returncode}\n{result.stdout}\n{result.stderr}")
    print(next(line for line in result.stderr.splitlines() if "index -1 out of bounds" in line))
print("test_xdp_inactive_generation_control")
