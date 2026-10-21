#!/usr/bin/env python3
"""Check that every ROCKETNET_EXPORT declaration is in tools/exports.txt."""

import argparse
from pathlib import Path
import re
import sys

EXPORT = re.compile(r"\bROCKETNET_EXPORT\b")
NAME = re.compile(r"\b[A-Za-z_]\w*\b")
DATA_POINTER = re.compile(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)")


def header_export_names(header_path):
    text = Path(header_path).read_text()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    lines = []
    development = 0
    for raw in text.splitlines():
        line = raw.strip()
        if development:
            if line.startswith("#if"):
                development += 1
            elif line.startswith("#endif"):
                development -= 1
            continue
        if line == "#if ROCKETNET_DEV":
            development = 1
            continue
        if line.startswith("#"):
            continue
        lines.append(line)
    names = []
    for declaration in " ".join(lines).split(";"):
        exported = EXPORT.search(declaration)
        if not exported:
            continue
        declaration = declaration[exported.start():]
        pointer = DATA_POINTER.search(declaration)
        if pointer and declaration.find("(") == pointer.start():
            name = pointer.group(1)
        else:
            before_parameters = declaration.split("(", 1)[0]
            words = NAME.findall(before_parameters)
            if not words:
                continue
            name = words[-1]
        if name not in names:
            names.append(name)
    return names


def export_list_names(exports_path):
    return {line.strip() for line in Path(exports_path).read_text().splitlines()
            if line.strip() and not line.startswith("#")}


def check(header_path, exports_path):
    declared = header_export_names(header_path)
    listed = export_list_names(exports_path)
    missing = sorted(set(declared) - listed)
    extra = sorted(listed - set(declared))
    for name in missing:
        print(f"Export declared in the header but missing from the list: {name}", file=sys.stderr)
    for name in extra:
        print(f"Export in the list but not declared in the header: {name}", file=sys.stderr)
    if missing or extra:
        return 1
    print(f"Header exports: {len(declared)} declared, list matches")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument("--header", type=Path, default=root / "sdk" / "rocketnet.h")
    parser.add_argument("--exports", type=Path, default=Path(__file__).with_name("exports.txt"))
    args = parser.parse_args()
    return check(args.header, args.exports)


if __name__ == "__main__":
    sys.exit(main())
