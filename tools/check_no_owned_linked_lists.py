#!/usr/bin/env python3
"""Refuse RocketNet-owned linked lists, pointer-linked or index-linked.

Policy: docs/SEQUENCE-BUFFERS.md, "Owned storage manifest". Bounded
registries, queues and pools are arrays, rings, stacks or sequence buffers;
none links its elements to each other. This audit reads every tracked C and
C++ source outside the vendored libsodium tree, removes comments, string and
character literals, and works on tokens. It never matches names such as
next, head or list: a field is a link only by how the code uses it.

A link is any of:

  self-pointer   a struct or class member whose type is a pointer to that
                 same struct (the classic pointer-linked node)
  traverse       a statement that replaces a cursor with a field read through
                 that cursor: d = table[d].next, p = p->next, or the same
                 through an alias (e = &table[d]; d = e->next), with an
                 optional mask (i = meta[i] & MASK)
  link store     a field store that names another element, followed within
                 the next few statements of the same block by the cursor
                 move that makes it a chain: table[tail].next = d; tail = d
                 (append) or table[d].next = head; head = d (push)

An integer that is never read back as the next cursor of a walk, such as an
IPv6 next_header byte, a ring head that advances arithmetically, or a local
loop counter, is not a link.

A link field declared by a RocketNet struct is owned and always fails: there
is no allowlist for owned state. A link field that no scanned source declares
comes from an operating-system header (struct ifaddrs, IP_ADAPTER_ADDRESSES).
Such a traversal passes only when the manifest's "External ABI traversal
sites" table names its file, function and field, the function releases the
list, and the cursor is never stored into a member, array element or global.
Every row of that table must also still match a site, so the table cannot
outlive the code.

Usage:
  check_no_owned_linked_lists.py            audit the repository
  check_no_owned_linked_lists.py --self-test  run the red and green controls
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "docs" / "SEQUENCE-BUFFERS.md"
SUFFIXES = (".c", ".h", ".cpp", ".cc", ".hpp", ".inc")
VENDORED = ("sodium/",)
LINK_STORE_WINDOW = 4

TOKEN = re.compile(r"""
    (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<number>\.?[0-9][A-Za-z0-9_.']*)
  | (?P<op>->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|::|[{}()\[\];,.=<>!~?:+\-*/%&|^#])
""", re.VERBOSE)

KEYWORDS = {"const", "volatile", "struct", "class", "union", "enum", "static",
            "unsigned", "signed", "return", "sizeof", "if", "while", "for",
            "switch", "case", "else", "do", "typedef", "inline", "extern",
            "constexpr", "auto", "register", "mutable", "template", "typename",
            "public", "private", "protected", "virtual", "explicit", "operator",
            "new", "delete", "noexcept", "override", "final", "static_cast",
            "reinterpret_cast", "const_cast", "goto", "default"}


def strip_code(text):
    """Blank comments, string and character literals and preprocessor lines,
    keeping every newline so line numbers stay exact."""
    out = []
    i = 0
    n = len(text)
    line_start = True
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if line_start and c == "#":
            # a preprocessor line, with continuations
            while i < n and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                    out.append(" \n")
                    i += 2
                    continue
                out.append(" ")
                i += 1
            continue
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        if c == '"' or c == "'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out.append("  " if text[i + 1] != "\n" else " \n")
                    i += 2
                    continue
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append(" ")
            i += 1
            continue
        if c == "\n":
            line_start = True
        elif not c.isspace():
            line_start = False
        out.append(c)
        i += 1
    return "".join(out)


def tokenize(code):
    tokens = []
    line = 1
    pos = 0
    for m in TOKEN.finditer(code):
        line += code.count("\n", pos, m.start())
        pos = m.start()
        tokens.append((m.group(0), line))
    return tokens


def is_ident(tok):
    return re.match(r"[A-Za-z_]", tok) is not None and tok not in KEYWORDS


def parse_lvalue(toks, i):
    """An lvalue chain from toks[i]: IDENT ( (.|->) IDENT | [ ... ] )*.
    Returns (normalized text, end index) or (None, i)."""
    if i >= len(toks) or not is_ident(toks[i]):
        return None, i
    parts = [toks[i]]
    j = i + 1
    while j < len(toks):
        if toks[j] in (".", "->") and j + 1 < len(toks) and is_ident(toks[j + 1]):
            parts.append(toks[j] + toks[j + 1])
            j += 2
        elif toks[j] == "[":
            depth = 0
            k = j
            while k < len(toks):
                if toks[k] == "[":
                    depth += 1
                elif toks[k] == "]":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            if k >= len(toks):
                break
            parts.append("[" + " ".join(toks[j + 1:k]) + "]")
            j = k + 1
        else:
            break
    return "".join(parts), j


def strip_casts(toks):
    """Drop leading parenthesised type casts and redundant parentheses."""
    changed = True
    while changed and toks:
        changed = False
        if toks[0] == "(":
            depth = 0
            for k, t in enumerate(toks):
                if t == "(":
                    depth += 1
                elif t == ")":
                    depth -= 1
                    if depth == 0:
                        break
            inner = toks[1:k]
            rest = toks[k + 1:]
            if inner and all(is_ident(t) or t in ("*", "const", "struct", "::", "unsigned") for t in inner) and rest:
                toks = rest
                changed = True
            elif k == len(toks) - 1 and inner:
                toks = inner
                changed = True
    return toks


def split_access(text):
    """For an lvalue chain ending in a subscript followed by members, return
    (container, index, field) for its last subscript: a[b].c.d gives
    (a, b, c.d). A chain ending in the subscript gives field ''."""
    depth = 0
    last_open = last_close = -1
    for k, ch in enumerate(text):
        if ch == "[":
            if depth == 0:
                last_open = k
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth == 0:
                last_close = k
    if last_open < 0:
        return None
    container = text[:last_open]
    index = text[last_open + 1:last_close].replace(" ", "")
    tail = text[last_close + 1:]
    if "[" in tail:
        return None
    field = re.sub(r"^(\.|->)", "", tail)
    field = re.sub(r"(\.|->)", ".", field)
    return container, index, field


def last_member(text):
    m = re.search(r"(?:\.|->)([A-Za-z_][A-Za-z0-9_]*)$", text)
    return m.group(1) if m else None


class Block:
    """One top-level brace block: a function body, or a struct body."""

    def __init__(self, name, statements):
        self.name = name
        self.statements = statements          # [(tokens, line)]


def blocks_of(tokens):
    """Split a file into top-level blocks and their statements. A statement
    ends at ; { or } at any nesting, so a for header yields its three parts."""
    blocks = []
    depth = 0
    i = 0
    current = None
    stmt = []
    stmt_line = 0
    paren = 0
    while i < len(tokens):
        tok, line = tokens[i]
        if tok == "(":
            paren += 1
        elif tok == ")":
            paren -= 1
        if tok == "{":
            if depth == 0:
                name = block_name([t for t, _ in tokens[max(0, i - 40):i]])
                current = Block(name, [])
                blocks.append(current)
            elif stmt and current is not None:
                current.statements.append((stmt, stmt_line))
            stmt = []
            depth += 1
        elif tok == "}":
            if stmt and current is not None:
                current.statements.append((stmt, stmt_line))
            stmt = []
            depth -= 1
            if depth <= 0:
                depth = 0
                current = None
        elif tok == ";":
            if stmt and current is not None:
                current.statements.append((stmt, stmt_line))
            stmt = []
        else:
            if not stmt:
                stmt_line = line
            stmt.append(tok)
        i += 1
    return blocks


def block_name(before):
    """The function name in front of a body: the identifier before the last
    balanced parenthesis group. A struct body is named 'struct NAME'."""
    toks = list(before)
    for k in range(len(toks) - 1, -1, -1):
        if toks[k] in (";", "}", "{"):
            toks = toks[k + 1:]
            break
    for k in range(len(toks) - 2, -1, -1):
        if toks[k] in ("struct", "class", "union") and is_ident(toks[k + 1]):
            if "(" not in toks and ")" not in toks:
                return "struct " + toks[k + 1]
    end = len(toks) - 1
    while end >= 0 and toks[end] != ")":
        end -= 1
    if end < 0:
        return ""
    depth = 0
    k = end
    while k >= 0:
        if toks[k] == ")":
            depth += 1
        elif toks[k] == "(":
            depth -= 1
            if depth == 0:
                break
        k -= 1
    if k > 0 and is_ident(toks[k - 1]):
        return toks[k - 1]
    return ""


def assignments(stmt):
    """Every top-level 'L = R' in a statement fragment, as (L, R tokens)."""
    found = []
    for k, tok in enumerate(stmt):
        if tok != "=":
            continue
        # walk back over the lvalue
        start = k - 1
        depth = 0
        while start >= 0:
            t = stmt[start]
            if t in ("]", ")"):
                depth += 1
            elif t in ("[", "("):
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0 and not (is_ident(t) or t in (".", "->") or re.match(r"[0-9]", t)):
                break
            start -= 1
        lhs, end = parse_lvalue(stmt, start + 1)
        if lhs is None or end != k:
            continue
        rhs = []
        depth = 0
        for t in stmt[k + 1:]:
            if t in ("(", "["):
                depth += 1
            elif t in (")", "]"):
                if depth == 0:
                    break
                depth -= 1
            elif t in (",", "=", "?") and depth == 0:
                break
            rhs.append(t)
        found.append((lhs, rhs))
    return found


def primary(rhs):
    """The lvalue an RHS reads, with casts, a mask (& | with a constant or
    tag) and parentheses removed; None when the RHS computes anything else."""
    toks = strip_casts(list(rhs))
    lv, end = parse_lvalue(toks, 0)
    if lv is None:
        # gen | lvalue: a tag or-ed onto a link
        if len(toks) >= 3 and toks[1] in ("|", "&"):
            lv, end = parse_lvalue(toks, 2)
            if lv is not None and end == len(toks):
                return lv
        return None
    if end == len(toks):
        return lv
    if toks[end] in ("&", "|") and end + 1 < len(toks):
        return lv
    return None


def mentions(rhs, value):
    toks = strip_casts(list(rhs))
    for k in range(len(toks)):
        lv, _ = parse_lvalue(toks, k)
        if lv == value:
            return True
    return False


def declared_fields(all_tokens):
    """Every member name declared inside a struct, class or union body in the
    scanned sources, with the struct's name and whether its type is a pointer
    to the same struct."""
    fields = set()
    self_pointers = []
    for path, tokens in all_tokens.items():
        toks = [t for t, _ in tokens]
        k = 0
        while k < len(toks):
            if toks[k] in ("struct", "class", "union") and k + 2 < len(toks) and is_ident(toks[k + 1]) and toks[k + 2] in ("{", ":"):
                name = toks[k + 1]
                j = k + 2
                while j < len(toks) and toks[j] != "{":
                    j += 1
                depth = 0
                member = []
                body_start = j
                while j < len(toks):
                    t = toks[j]
                    if t == "{":
                        depth += 1
                        member = []
                    elif t == "}":
                        depth -= 1
                        member = []
                        if depth == 0:
                            break
                    elif t == ";":
                        if depth == 1 and member and "(" not in member:
                            names = [m for m in member if is_ident(m)]
                            if names:
                                field = names[-1]
                                fields.add(field)
                                if name in member[:-1] and "*" in member:
                                    self_pointers.append((path, tokens[j][1], name, field))
                        member = []
                    else:
                        member.append(t)
                    j += 1
                k = body_start + 1
                continue
            k += 1
    return fields, self_pointers


def constant_tables(tokens):
    """Arrays declared const with an initializer at file scope: fixed data
    that no code admits to, removes from or relinks. A walk through one reads
    a compiled-in table (such as a counter family's parent row), not storage."""
    names = set()
    toks = [t for t, _ in tokens]
    depth = 0
    for k, t in enumerate(toks):
        if t == "{":
            depth += 1
        elif t == "}":
            depth -= 1
        elif depth == 0 and t == "const" and k + 3 < len(toks):
            j = k + 1
            while j < len(toks) and toks[j] not in ("[", "=", ";", "(", "{"):
                j += 1
            if j < len(toks) and toks[j] == "[" and is_ident(toks[j - 1]):
                m = j
                while m < len(toks) and toks[m] not in ("=", ";", "("):
                    m += 1
                if m < len(toks) and toks[m] == "=":
                    names.add(toks[j - 1])
    return names


def analyse_block(block, path):
    """Traversals and link stores in one block: (line, kind, field, cursor)."""
    out = []
    flat = []
    for stmt, line in block.statements:
        for lhs, rhs in assignments(stmt):
            flat.append((lhs, rhs, line))
    aliases = {}            # pointer -> (container, index)
    copies = {}             # cursor -> set of names it was copied from
    for n, (lhs, rhs, line) in enumerate(flat):
        prim = primary(rhs)
        toks = strip_casts(list(rhs))
        if toks[:1] == ["&"]:
            lv, end = parse_lvalue(toks, 1)
            if lv is not None and end == len(toks):
                acc = split_access(lv)
                if acc and acc[2] == "":
                    aliases[lhs] = acc[:2]
        elif len(toks) >= 3 and toks[1] == "+":
            a, e1 = parse_lvalue(toks, 0)
            b, e2 = parse_lvalue(toks, 2)
            if a and b and e1 == 1 and e2 == len(toks):
                aliases[lhs] = (a, b)
        if prim is not None and "[" not in prim and "." not in prim and "->" not in prim:
            copies.setdefault(lhs, set()).add(prim)
        elif prim is not None and "[" not in prim:
            copies.setdefault(lhs, set()).add(prim)

        def same(a, b):
            return a == b or b in copies.get(a, ()) or a in copies.get(b, ())

        # traverse
        if prim is not None:
            acc = split_access(prim)
            if acc and same(lhs, acc[1]):
                out.append((line, "traverse", acc[2] or acc[0], lhs, acc[0]))
            else:
                m = re.match(r"^(.*)->([A-Za-z_][A-Za-z0-9_]*)$", prim)
                if m:
                    base, field = m.group(1), m.group(2)
                    if base == lhs:
                        out.append((line, "traverse", field, lhs, ""))
                    elif base in aliases and same(lhs, aliases[base][1]):
                        out.append((line, "traverse", field, lhs, aliases[base][0]))
        # link store
        target = None
        acc = split_access(lhs)
        if acc:
            target = (acc[0], acc[1], acc[2] or acc[0])
        else:
            m = re.match(r"^(.*)->([A-Za-z_][A-Za-z0-9_]*)$", lhs)
            if m and m.group(1) in aliases:
                container, index = aliases[m.group(1)]
                target = (container, index, m.group(2))
        moved = prim is not None and split_access(prim) is not None and split_access(prim)[0] == (target[0] if target else None)
        if target is not None and not moved and is_ident(target[1].split("->")[0].split(".")[0]) and re.match(r"^[A-Za-z_][A-Za-z0-9_.>\-]*$", target[1]):
            for lhs2, rhs2, _ in flat[n + 1:n + 1 + LINK_STORE_WINDOW]:
                rhs2_toks = strip_casts(list(rhs2))
                prim2, end2 = parse_lvalue(rhs2_toks, 0)
                if prim2 is None or end2 != len(rhs2_toks):
                    continue
                pair = {lhs2, prim2}
                index = target[1]
                if index in pair:
                    other = (pair - {index}).pop() if len(pair) == 2 else None
                    if other is not None and mentions(rhs, other):
                        out.append((line, "link store", target[2], index, target[0]))
                        break
    return out


def tracked_sources(root):
    try:
        listed = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        listed = [str(p.relative_to(root)) for p in root.rglob("*") if p.is_file()]
    return sorted(p for p in listed if p.endswith(SUFFIXES) and not p.startswith(VENDORED))


def read_external_table(text):
    """Rows of the manifest's external table: (file, function, field, release)."""
    rows = []
    section = re.search(r"^## External ABI traversal sites\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not section:
        return None
    for line in section.group(1).splitlines():
        cells = [c.strip().strip("`") for c in line.strip().strip("|").split("|")]
        if len(cells) >= 4 and line.strip().startswith("|") and not set(cells[0]) <= set("-: ") and cells[0] != "File":
            rows.append((cells[0], cells[1], cells[2], cells[3]))
    return rows


def read_approved_table(text):
    """Rows of the manifest's approved index-link table: (file, function, field)."""
    rows = []
    section = re.search(r"^## Approved index links\n(.*?)(?=^## |\Z)", text or "", re.M | re.S)
    if not section:
        return rows
    for line in section.group(1).splitlines():
        cells = [c.strip().strip("`") for c in line.strip().strip("|").split("|")]
        if len(cells) >= 4 and line.strip().startswith("|") and not set(cells[0]) <= set("-: ") and cells[0] != "File":
            rows.append((cells[0], cells[1], cells[2]))
    return rows


def audit(sources, manifest_text):
    """sources: {path: text}. Returns (owned, external, problems)."""
    tokens = {p: tokenize(strip_code(t)) for p, t in sources.items()}
    fields, self_pointers = declared_fields(tokens)
    owned = []
    external = []
    problems = []
    constants = audit.constants = []
    for path, line, struct, field in self_pointers:
        owned.append((path, line, "self-pointer", "%s::%s" % (struct, field), ""))
    for path, toks in tokens.items():
        tables = constant_tables(toks)
        for block in blocks_of(toks):
            if block.name.startswith("struct "):
                continue
            for line, kind, field, cursor, container in analyse_block(block, path):
                if container and re.split(r"->|\.", container)[-1] in tables:
                    constants.append((path, line, kind, field, block.name))
                    continue
                leaf = field.split(".")[-1]
                if leaf in fields or container:
                    if leaf in fields or (container and leaf):
                        owned.append((path, line, kind, field, block.name))
                        continue
                external.append((path, line, kind, field, block.name, cursor, block))
    # an index link the manifest approves by file, function and field (a
    # decision recorded beside it) is not an owned list; a row that matches no
    # site is a manifest problem, so the table cannot outlive the code
    approved = read_approved_table(manifest_text)
    approved_matched = set()
    kept = []
    for site in owned:
        path, line, kind, field, function = site
        row = (path, function, field.split(".")[-1])
        if kind != "self-pointer" and row in approved:
            approved_matched.add(row)
            continue
        kept.append(site)
    owned[:] = kept
    for row in approved:
        if row not in approved_matched:
            problems.append("manifest approved row %s %s %s matches no site" % row)
    rows = read_external_table(manifest_text) if manifest_text is not None else []
    if rows is None:
        problems.append("the manifest has no 'External ABI traversal sites' section")
        rows = []
    matched = set()
    for path, line, kind, field, function, cursor, block in external:
        row = next((r for r in rows if r[0] == path and r[1] == function and r[2] == field), None)
        if row is None:
            problems.append("%s:%d: %s through OS field %s in %s is not in the manifest's external table" % (path, line, kind, field, function))
            continue
        matched.add(row)
        released = any(t == row[3] for stmt, _ in block.statements for t in stmt)
        if not released:
            problems.append("%s:%d: %s never calls %s to release the OS list" % (path, line, function, row[3]))
        for stmt, sline in block.statements:
            for lhs, rhs in assignments(stmt):
                if ("->" in lhs or "." in lhs or "[" in lhs) and primary(rhs) == cursor:
                    problems.append("%s:%d: %s stores the OS cursor %s into %s" % (path, sline, function, cursor, lhs))
    for row in rows:
        if row not in matched:
            problems.append("manifest external row %s %s %s matches no site" % row[:3])
    return owned, external, problems


def run(root):
    sources = {}
    for rel in tracked_sources(root):
        try:
            sources[rel] = (root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            print("FAIL cannot read %s: %s" % (rel, error))
            return 1
    manifest = MANIFEST.read_text(encoding="utf-8") if MANIFEST.exists() else None
    owned, external, problems = audit(sources, manifest)
    for path, line, kind, field, function in sorted(set(owned)):
        print("FAIL %s:%d: owned linked list (%s) through %s in %s" % (path, line, kind, field, function or "declaration"))
    for path, line, kind, field, function, _, _ in sorted(external, key=lambda e: (e[0], e[1])):
        print("EXTERNAL %s:%d: %s through OS field %s in %s" % (path, line, kind, field, function))
    for path, line, kind, field, function in audit.constants:
        print("CONSTANT %s:%d: %s through %s in %s reads a const compiled-in table, not owned storage" % (path, line, kind, field, function))
    for problem in problems:
        print("FAIL " + problem)
    print("scanned %d files: %d owned link sites, %d external ABI traversals, %d manifest problems"
          % (len(sources), len(set(owned)), len(external), len(problems)))
    if owned or problems:
        print("FAIL: RocketNet owns a linked list or the manifest disagrees with the source")
        return 1
    print("PASS: no RocketNet-owned linked list remains")
    return 0


SELF_TEST_MANIFEST = """
## External ABI traversal sites

| File | Function | Field | Release |
|---|---|---|---|
| os.c | find | ifa_next | freeifaddrs |
"""

RED = {
    "pointer node": "struct node_t { int value; struct node_t * next; };",
    "index walk": "struct cell_t { unsigned link; };\nvoid walk( struct cell_t * cells, unsigned d ) { for ( ; d != 0; d = cells[d].link ) {} }",
    "alias walk": "struct cell_t { unsigned after; };\nvoid walk( struct cell_t * cells, unsigned d ) { while ( d ) { struct cell_t * c = &cells[d]; d = c->after; } }",
    "free list pop": "struct pool_t { unsigned * meta; unsigned top; };\nunsigned take( struct pool_t * p ) { unsigned i = p->top; p->top = p->meta[i] & 0xFFFFF; return i; }",
    "free list push": "struct desc_t { unsigned chain; };\nstruct region_t { struct desc_t * d; unsigned top; };\nvoid put( struct region_t * r, unsigned i ) { r->d[i].chain = r->top; r->top = i; }",
    "fifo append": "struct desc_t { unsigned succ; };\nstruct q_t { struct desc_t * d; unsigned tail; };\nvoid add( struct q_t * q, unsigned i ) { q->d[q->tail].succ = i; q->tail = i; }",
    "pointer walk": "struct item_t { int v; struct item_t * forward; };\nint sum( struct item_t * p ) { int s = 0; for ( ; p; p = p->forward ) s += p->v; return s; }",
    "mutable parent table": "struct row_t { int parent; };\nstatic struct row_t rows[3];\nint depth( int c ) { int d = 0; for ( ; c >= 0; c = rows[c].parent ) d++; return d; }",
    "unlisted os walk": "void scan( struct ifaddrs * a ) { for ( ; a; a = a->ifa_next ) {} freeifaddrs( a ); }",
    "retained os cursor": "struct keep_t { struct ifaddrs * held; };\nvoid find( struct keep_t * k, struct ifaddrs * a ) { for ( ; a; a = a->ifa_next ) { k->held = a; } freeifaddrs( a ); }",
}

GREEN = {
    "ipv6 next header": "struct ipv6_t { unsigned char next_header; };\nint skip( const struct ipv6_t * h, int offset ) { int next_header = h->next_header; while ( next_header == 0 ) { next_header = offset; offset = offset + 8; } return offset; }",
    "loop counter": "int count( const int * a, int n ) { int s = 0; for ( int next = 0; next < n; next = next + 1 ) s += a[next]; return s; }",
    "ring head": "struct ring_t { int head; int slots[8]; };\nint pop( struct ring_t * r ) { const int v = r->slots[r->head]; r->head = ( r->head + 1 ) % 8; return v; }",
    "free stack": "struct stack_t { unsigned items[8]; unsigned count; };\nvoid push( struct stack_t * s, unsigned v ) { s->items[s->count] = v; s->count = s->count + 1; }",
    "handoff pick": "struct h_t { int free_index[4]; int free_head; };\nint take( struct h_t * h ) { const int next = h->free_index[h->free_head]; h->free_head = ( h->free_head + 1 ) % 4; return next; }",
    "listed os walk": "int find( struct ifaddrs * a ) { int found = 0; for ( struct ifaddrs * i = a; i; i = i->ifa_next ) found++; freeifaddrs( a ); return found; }",
    "backward shift erase": "struct e_t { unsigned key; };\nvoid erase( struct e_t * t, unsigned i, unsigned j ) { while ( j ) { t[i] = t[j]; i = j; j = j - 1; } }",
    "const parent table": "struct row_t { int parent; };\nstatic const struct row_t rows[3] = { { -1 }, { 0 }, { 1 } };\nint depth( int c ) { int d = 0; for ( ; c >= 0; c = rows[c].parent ) d++; return d; }",
    "comment and string": "/* d = cells[d].next; */\nconst char * s = \"p = p->next\";",
}


def self_test():
    failures = 0
    for name, source in RED.items():
        path = "os.c" if "os" in name else "red.c"
        owned, _, problems = audit({path: source}, SELF_TEST_MANIFEST if "os" in name else "## External ABI traversal sites\n")
        ok = bool(owned or problems)
        print("%s red control %s" % ("PASS" if ok else "FAIL", name))
        failures += not ok
    for name, source in GREEN.items():
        path = "os.c" if "os" in name else "green.c"
        owned, _, problems = audit({path: source}, SELF_TEST_MANIFEST if "os" in name else "## External ABI traversal sites\n")
        ok = not owned and not problems
        print("%s green control %s%s" % ("PASS" if ok else "FAIL", name, "" if ok else " %r %r" % (owned, problems)))
        failures += not ok
    owned, _, problems = audit({"other.c": "int x;"}, SELF_TEST_MANIFEST)
    ok = any("matches no site" in p for p in problems)
    print("%s red control stale external row" % ("PASS" if ok else "FAIL"))
    failures += not ok
    print("self-test %s" % ("passed" if failures == 0 else "failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if sys.argv[1:]:
        print(__doc__)
        sys.exit(2)
    sys.exit(run(ROOT))
