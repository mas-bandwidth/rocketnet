#!/usr/bin/env python3
"""Fail when the fragment proof's copied functions drift from the SDK.

CBMC does not parse sdk/rocketnet_fragment.cpp. The proof keeps a copy.
The fragment-bounds recipe runs this before the proof.
"""

import pathlib
import sys

NAMES = (
    "rocketnet_read_fragment_header",
    "rocketnet_reassembly_clear_slot",
    "rocketnet_reassembly_bit",
    "rocketnet_reassembly_set_bit",
    "rocketnet_reassembly_process_fragment",
)


def _skip(src, i, state):
    n = len(src)
    c = src[i]
    nxt = src[i + 1] if i + 1 < n else ""
    if state == "code":
        if c == "/" and nxt == "/":
            return i + 2, "line"
        if c == "/" and nxt == "*":
            return i + 2, "block"
        if c == '"':
            return i + 1, "str"
        if c == "'":
            return i + 1, "chr"
        return None
    if state == "line":
        if c == "\n":
            return i + 1, "code"
        return i + 1, "line"
    if state == "block":
        if c == "*" and nxt == "/":
            return i + 2, "code"
        return i + 1, "block"
    if state == "str":
        if c == "\\":
            return min(i + 2, n), "str"
        if c == '"':
            return i + 1, "code"
        return i + 1, "str"
    if c == "\\":
        return min(i + 2, n), "chr"
    if c == "'":
        return i + 1, "code"
    return i + 1, "chr"


def extract(src, name):
    prefixes = (
        f"void {name}(",
        f"int {name}(",
        f"static void {name}(",
        f"static int {name}(",
    )
    start = -1
    idx = 0
    while True:
        found = -1
        for prefix in prefixes:
            j = src.find(prefix, idx)
            if j != -1 and (found == -1 or j < found):
                found = j
        if found == -1:
            return None
        line_start = src.rfind("\n", 0, found) + 1
        if src[line_start:found].strip() == "":
            start = line_start
            break
        idx = found + 1

    i = start
    state = "code"
    brace = None
    while i < len(src):
        skipped = _skip(src, i, state)
        if skipped is not None:
            i, state = skipped
            continue
        if src[i] == "{":
            brace = i
            break
        i += 1
    if brace is None:
        return None

    depth = 0
    i = brace
    state = "code"
    while i < len(src):
        skipped = _skip(src, i, state)
        if skipped is not None:
            i, state = skipped
            continue
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1
    return None


def main():
    here = pathlib.Path(__file__).resolve().parent
    sdk_path = here.parents[1] / "sdk" / "rocketnet_fragment.cpp"
    proof_path = here / "proof_fragment_bounds.c"
    sdk = sdk_path.read_text()
    proof = proof_path.read_text()
    bad = False
    for name in NAMES:
        copied = extract(proof, name)
        real = extract(sdk, name)
        if copied is None or real is None or copied != real:
            print(
                f"fragment-bounds: {name} does not match sdk/rocketnet_fragment.cpp",
                file=sys.stderr,
            )
            bad = True
    if bad:
        return 1
    print("fragment-bounds: copied functions match sdk/rocketnet_fragment.cpp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
