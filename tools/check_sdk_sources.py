#!/usr/bin/env python3
"""Check that cmake/RocketnetSources.cmake names every SDK source file.

The library build (CMakeLists.txt) and every application that vendors sdk/
compile the list in cmake/RocketnetSources.cmake. This checks that

  - every sdk/*.cpp and sdk/*.c is listed exactly once, as a library source
    or as excluded (ROCKETNET_SDK_SOURCES_EXCLUDED);
  - every listed file exists;
  - a *_linux / *_windows / *_apple file sits in its platform's list, and a
    common file carries no platform suffix;

and that the check refuses a tree with an unlisted sdk/ file, so it can fail.
It needs cmake and no compiler.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SUFFIX_GROUP = {"_linux": "LINUX", "_windows": "WINDOWS", "_apple": "APPLE"}


def listed(root, directory):
    output = directory / "sdk-source-list.txt"
    subprocess.run(["cmake", f"-DROCKETNET_SDK_SOURCE_LIST_OUTPUT={output}",
                    "-P", str(root / "cmake/RocketnetSources.cmake")], check=True)
    rows = []
    for line in output.read_text(encoding="utf-8").splitlines():
        group, path = line.split(" ", 1)
        rows.append((group, path))
    return rows


def problems(root, directory):
    rows = listed(root, directory)
    found = []
    seen = {}
    for group, path in rows:
        if path in seen:
            found.append(f"{path} is listed twice ({seen[path]}, {group})")
        seen[path] = group
        if not (root / path).is_file():
            found.append(f"{path} is listed in {group} but does not exist")
        stem = Path(path).stem
        wanted = next((g for suffix, g in SUFFIX_GROUP.items() if stem.endswith(suffix)), "COMMON")
        if group != "EXCLUDED" and group != wanted:
            found.append(f"{path} is listed in {group}; its name puts it in {wanted}")
    for path in sorted((root / "sdk").glob("*.c*")):
        if path.suffix not in (".c", ".cpp"):
            continue
        name = path.relative_to(root).as_posix()
        if name not in seen:
            found.append(f"{name} is neither listed nor excluded in cmake/RocketnetSources.cmake")
    return found


def main():
    with tempfile.TemporaryDirectory(prefix="sdk sources ") as directory:
        directory = Path(directory)
        found = problems(ROOT, directory)
        if found:
            for line in found:
                print(f"FAIL: {line}", file=sys.stderr)
            return 1
        print("test_sdk_sources_listed")

        # An unlisted sdk/ file must be refused, or the check above could
        # never fail.
        probe = directory / "probe"
        (probe / "cmake").mkdir(parents=True)
        shutil.copyfile(ROOT / "cmake/RocketnetSources.cmake", probe / "cmake/RocketnetSources.cmake")
        shutil.copytree(ROOT / "sdk", probe / "sdk")
        (probe / "sdk/rocketnet_unlisted_probe.cpp").write_text("// probe\n", encoding="utf-8")
        found = problems(probe, directory)
        if not any("rocketnet_unlisted_probe.cpp is neither listed nor excluded" in line for line in found):
            print("FAIL: an unlisted sdk/ source passed the check", file=sys.stderr)
            return 1
        print("test_sdk_sources_unlisted_is_red")
    return 0


if __name__ == "__main__":
    sys.exit(main())
