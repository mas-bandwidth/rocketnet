#!/usr/bin/env python3
"""Check local Markdown links and fragments in maintained RocketNet guides.

Historical run/audit/release/qualification records and generated proof records
are not rewritten as current guides. They are still checked when linked as a
target. External URLs are not fetched. Only standard-library Python is needed.
"""

import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\s*\)")
REFERENCE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>]+>|\S+)", re.M)


def prose(text):
    """Keep line numbers while excluding fenced examples and comments."""
    lines = []
    fence = None
    for line in text.splitlines():
        match = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if fence:
            if match and match[1][0] == fence[0] and len(match[1]) >= len(fence):
                fence = None
            lines.append("")
        elif match:
            fence = match[1]
            lines.append("")
        elif line.startswith("    ") or line.startswith("\t"):
            lines.append("")
        else:
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


def documents(root):
    paths = set(root.glob("*.md"))
    for path in (root / "docs").rglob("*.md"):
        rel = path.relative_to(root / "docs")
        if rel.parts[0] in ("audits", "runs", "lan-bench"):
            continue
        if path.name.startswith(("RELEASE-NOTES-", "QUALIFICATION-")):
            continue
        paths.add(path)
    paths.update((root / "standard").glob("*.md"))
    paths.update((root / "examples").rglob("README.md"))
    return sorted(paths)


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


def main():
    paths = documents(ROOT)
    checked, failures = check(paths, ROOT)
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        print(f"FAIL: {len(failures)} invalid links of {checked} in {len(paths)} guides", file=sys.stderr)
        return 1
    print(f"OK: {checked} local links and fragments in {len(paths)} guides")
    return 0


if __name__ == "__main__":
    sys.exit(main())
