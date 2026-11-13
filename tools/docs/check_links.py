#!/usr/bin/env python3
"""Check local Markdown links and fragments in every tracked RocketNet guide.

The guides are every .md file `git ls-files` names, less the historical
run/audit/release/qualification records named in HISTORICAL_DIRS and
HISTORICAL_PREFIXES, which are not rewritten as current guides (they are still
checked when linked as a target). An empty guide list fails, and so does a
named historical record that no longer names a tracked file. External URLs are
not fetched. Only standard-library Python and git are needed.
"""

import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\s*\)")
REFERENCE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>]+>|\S+)", re.M)


LIST_ITEM = re.compile(r"^\s{0,3}(?:[-*+]|[0-9]{1,9}[.)])(?:\s|$)")


def prose(text):
    """Keep line numbers while excluding fenced examples, indented code and comments.

    An indented line (four spaces or a tab) is code only where CommonMark makes
    it an indented code block: after a blank line (or another code line) and
    not inside a list item. Inside a list, or right after a paragraph line, it
    is a continuation line of prose, and its links are checked.
    """
    lines = []
    fence = None
    previous_blank = True
    in_code = False
    in_list = False
    for line in text.splitlines():
        match = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if fence:
            if match and match[1][0] == fence[0] and len(match[1]) >= len(fence):
                # a closed fence ends any paragraph, so an indented line after it is code
                fence = None
                previous_blank = True
            lines.append("")
            continue
        if match:
            fence = match[1]
            in_code = False
            previous_blank = False
            lines.append("")
            continue
        if not line.strip():
            previous_blank = True
            lines.append(line)
            continue
        if line.startswith("    ") or line.startswith("\t"):
            if in_code or (previous_blank and not in_list):
                in_code = True
                lines.append("")
            else:
                lines.append(line)
            previous_blank = False
            continue
        in_code = False
        if LIST_ITEM.match(line):
            in_list = True
        elif previous_blank:
            in_list = False
        previous_blank = False
        lines.append(line)
    return re.sub(r"<!--.*?-->", lambda m: "\n" * m[0].count("\n"), "\n".join(lines), flags=re.S)


def anchors(text):
    text = prose(text)
    found = set(re.findall(r'<a\s+(?:id|name)=["\']([^"\']+)["\']', text))
    counts = {}
    for match in re.finditer(r"^#{1,6}\s+(.+?)(?:\s+#+)?\s*$", text, re.M):
        title = re.sub(r"<[^>]*>", "", match[1])
        title = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", title)
        base = re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")
        count = counts.get(base, 0)
        found.add(base + (f"-{count}" if count else ""))
        counts[base] = count + 1
    return found


# The historical records, by name: each is the record of a run, an audit, a
# release or a qualification as it stood, so it is not rewritten when a guide
# moves. Every other tracked .md file is a maintained guide and is checked.
HISTORICAL_DIRS = ("docs/audits/", "docs/runs/", "docs/lan-bench/")
HISTORICAL_PREFIXES = ("docs/RELEASE-NOTES-", "docs/QUALIFICATION-")


def historical(rel):
    return rel.startswith(HISTORICAL_DIRS) or rel.startswith(HISTORICAL_PREFIXES)


def tracked_markdown(root):
    """Every .md file git tracks under root, as root-relative POSIX paths."""
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", "*.md"],
                         check=True, capture_output=True).stdout
    return sorted(p for p in out.decode("utf-8").split("\0") if p)


def documents(root):
    return [root / rel for rel in tracked_markdown(root) if not historical(rel)]


def check(paths, root):
    failures = []
    checked = 0
    cache = {}
    for path in paths:
        text = prose(path.read_text(encoding="utf-8"))
        for match in list(LINK.finditer(text)) + list(REFERENCE.finditer(text)):
            url = match[1].strip("<>")
            parsed = urlsplit(url)
            if parsed.scheme or parsed.netloc:
                continue
            checked += 1
            target = (path.parent / unquote(parsed.path)).resolve() if parsed.path else path
            line = text.count("\n", 0, match.start()) + 1
            label = f"{path.relative_to(root)}:{line}: {url}"
            if not target.exists():
                failures.append(f"{label}: missing target")
            elif parsed.fragment and target.suffix.lower() == ".md":
                if target not in cache:
                    cache[target] = anchors(target.read_text(encoding="utf-8"))
                if unquote(parsed.fragment) not in cache[target]:
                    failures.append(f"{label}: missing heading or anchor")
    return checked, failures


def stale_exclusions(root):
    """Each named historical directory or prefix must still name a tracked file."""
    tracked = tracked_markdown(root)
    return [f"historical record {name!r} names no tracked .md file; remove it from the list"
            for name in HISTORICAL_DIRS + HISTORICAL_PREFIXES
            if not any(rel.startswith(name) for rel in tracked)]


def main():
    paths = documents(ROOT)
    if not paths:
        print("FAIL: git ls-files found no maintained .md guide to check", file=sys.stderr)
        return 1
    checked, failures = check(paths, ROOT)
    failures += stale_exclusions(ROOT)
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        print(f"FAIL: {len(failures)} invalid links of {checked} in {len(paths)} guides", file=sys.stderr)
        return 1
    print(f"OK: {checked} local links and fragments in {len(paths)} guides")
    return 0


if __name__ == "__main__":
    sys.exit(main())
