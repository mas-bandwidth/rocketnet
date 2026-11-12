"""A reasoned allow list of exact lines, shared by the line checkers.

File format (tab-separated, '#' starts a comment line, blank lines ignored):

    <path relative to the repository root>\t<the exact line, stripped>\t<reason>

An entry covers every line of that file whose stripped text equals the
entry's text. Refused, as findings: an entry with fewer than three fields or
an empty reason, an entry listed twice, and a stale entry (one no flagged
line of the scan matched), so the list can only shrink to what is true.
"""

from pathlib import Path


def read(path):
    """(entries, problems): entries maps (path, text) -> reason."""
    entries = {}
    problems = []
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return {}, [f"{path}: {error}"]
    for number, raw in enumerate(lines, 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) != 3 or not all(f.strip() for f in fields):
            problems.append(f"{path}:{number}: want '<path>\\t<exact line>\\t<reason>', three non-empty fields")
            continue
        key = (fields[0].strip(), fields[1].strip())
        if key in entries:
            problems.append(f"{path}:{number}: listed twice: {key[0]}: {key[1]}")
            continue
        entries[key] = fields[2].strip()
    return entries, problems


def judge(flagged, entries, allowlist_path):
    """flagged: [(path, number, text)]. Returns (refused, allowed, stale problems)."""
    used = set()
    refused = []
    allowed = []
    for path, number, text in flagged:
        key = (path, text.strip())
        if key in entries:
            used.add(key)
            allowed.append((path, number, text))
        else:
            refused.append((path, number, text))
    stale = [f"{allowlist_path}: stale entry (no such line flagged): {p}: {t}"
             for (p, t) in sorted(set(entries) - used)]
    return refused, allowed, stale
