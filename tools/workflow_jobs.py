#!/usr/bin/env python3
"""Read the job names the GitHub Actions workflows produce, with no YAML library.

The release and landing tools name jobs (tools/release_gates.txt,
.github/rulesets/main.json); this module is how the checkers prove those names
are the workflows' own. It holds:

  - parse(text): a reader for the YAML subset the workflows use (block maps and
    sequences, flow lists and maps, quoted and plain scalars, block scalars,
    comments). Scalars stay strings. Anything it does not understand raises
    WorkflowError naming the line: a checker built on it fails loudly, never
    silently short.
  - Workflow / Job: triggers, jobs, each job's name template, `if:`, matrix and
    reusable `uses:`.
  - job_runs(job, event, full): evaluates a job's `if:` for a push to main
    (event "push") or a pull request (event "pull_request"), in the
    per-commit tier (full False) or the nightly tier (full True). It knows the
    operators and atoms the workflows use (github.event_name, github.ref,
    startsWith, inputs.full, always(), cancelled(), !, &&, ||, ==, !=,
    parentheses) and raises on anything else.
  - expand(job, full): the exact display names of a job's matrix rows, by
    GitHub's matrix rules (cartesian product, exclude, include merge-or-add),
    with `${{ fromJSON(inputs.full && 'A' || 'B') }}` and `${{ fromJSON('A') }}`
    values evaluated.
    A name term is a matrix value (`${{ matrix.K }}`) or a value printed only
    for rows that carry it (`${{ matrix.K && format(' ({0})', matrix.K) || '' }}`).
  - name_pattern(job): a regular expression any display name of the job
    matches, matrix values or not (a skipped matrix job is reported under its
    unexpanded template, which the pattern also matches).

Usage: workflow_jobs.py --dump <workflow.yml>   (the parsed file as JSON)
       workflow_jobs.py --names <workflow.yml>  (per-commit pull request names)
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = ROOT / ".github" / "workflows"

# a job-name term that prints only for the rows that carry a key:
# `matrix.K && format('F', matrix.K) || ''` (F a single-quoted literal, '' an escaped quote)
OPTIONAL_TERM = re.compile(r"matrix\.([A-Za-z_][A-Za-z0-9_-]*)\s*&&\s*format\(\s*'((?:[^']|'')*)'\s*,\s*"
                           r"matrix\.([A-Za-z_][A-Za-z0-9_-]*)\s*\)\s*\|\|\s*''")


class WorkflowError(Exception):
    pass


# ---------------------------------------------------------------- YAML subset

KEY = re.compile(r"""^("[^"]*"|'[^']*'|[^\s'"{\[#&*!|>%@`-][^:#]*?|-[^\s:#][^:#]*?):(?:\s+|$)(.*)$""")
BLOCK_SCALAR = re.compile(r"^[|>][+-]?\d*$")
ANCHOR = re.compile(r"^&([\w-]+)(?:\s+(.*))?$")
ALIAS = re.compile(r"^\*([\w-]+)$")


def strip_comment(text):
    """text without a trailing comment; '#' counts only after whitespace and outside quotes."""
    quote = None
    depth = 0
    for index, ch in enumerate(text):
        if quote:
            if ch == quote:
                if quote == "'" and index + 1 < len(text) and text[index + 1] == "'":
                    continue
                quote = None
            elif ch == "\\" and quote == '"':
                continue
        elif ch in "'\"" and (index == 0 or text[index - 1] in " \t[{,:" or depth):
            quote = ch
        elif ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        elif ch == "#" and (index == 0 or text[index - 1] in " \t"):
            return text[:index].rstrip()
    return text.rstrip()


def unquote(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] == "'":
        return text[1:-1].replace("''", "'")
    if len(text) >= 2 and text[0] == text[-1] == '"':
        return bytes(text[1:-1], "utf-8").decode("unicode_escape")
    return text


def parse_flow(text, where):
    """A flow scalar, list or map (strings only)."""
    text = text.strip()
    if not text or text[0] not in "[{":
        return unquote(text)
    value, rest = _flow_node(text, 0, where)
    if text[rest:].strip():
        raise WorkflowError(f"{where}: text after a flow collection: {text[rest:]!r}")
    return value


def _flow_node(text, i, where):
    while i < len(text) and text[i] in " \t":
        i += 1
    if i < len(text) and text[i] == "[":
        items, i = [], i + 1
        while True:
            while i < len(text) and text[i] in " \t":
                i += 1
            if i < len(text) and text[i] == "]":
                return items, i + 1
            item, i = _flow_node(text, i, where)
            items.append(item)
            while i < len(text) and text[i] in " \t":
                i += 1
            if i < len(text) and text[i] == ",":
                i += 1
            elif i < len(text) and text[i] == "]":
                return items, i + 1
            else:
                raise WorkflowError(f"{where}: unterminated flow list")
    if i < len(text) and text[i] == "{":
        mapping, i = {}, i + 1
        while True:
            while i < len(text) and text[i] in " \t":
                i += 1
            if i < len(text) and text[i] == "}":
                return mapping, i + 1
            key, i = _flow_scalar(text, i, ":", where)
            if i >= len(text) or text[i] != ":":
                raise WorkflowError(f"{where}: flow map entry without ':'")
            value, i = _flow_node(text, i + 1, where)
            mapping[key] = value
            while i < len(text) and text[i] in " \t":
                i += 1
            if i < len(text) and text[i] == ",":
                i += 1
            elif i < len(text) and text[i] == "}":
                return mapping, i + 1
            else:
                raise WorkflowError(f"{where}: unterminated flow map")
    return _flow_scalar(text, i, ",]}", where)


def _flow_scalar(text, i, stops, where):
    while i < len(text) and text[i] in " \t":
        i += 1
    if i < len(text) and text[i] in "'\"":
        quote, j = text[i], i + 1
        while j < len(text):
            if text[j] == quote:
                if quote == "'" and j + 1 < len(text) and text[j + 1] == "'":
                    j += 2
                    continue
                return unquote(text[i:j + 1]), j + 1
            if text[j] == "\\" and quote == '"':
                j += 1
            j += 1
        raise WorkflowError(f"{where}: unterminated quoted scalar")
    j = i
    while j < len(text):
        if text[j] in stops and (text[j] != ":" or j + 1 >= len(text) or text[j + 1] in " \t"):
            break
        j += 1
    return text[i:j].strip(), j


class _Parser:

    def __init__(self, text, name):
        self.lines = text.replace("\t", "    ").split("\n")
        self.name = name
        self.i = 0
        self.anchors = {}

    def where(self, index=None):
        return f"{self.name}:{(self.i if index is None else index) + 1}"

    def peek(self):
        while self.i < len(self.lines):
            raw = self.lines[self.i]
            stripped = raw.strip()
            if not stripped or stripped.startswith("#") or stripped == "---":
                self.i += 1
                continue
            return len(raw) - len(raw.lstrip(" ")), raw.strip()
        return None

    def node(self, indent):
        p = self.peek()
        if p is None or p[0] < indent:
            return None
        if p[1] == "-" or p[1].startswith("- "):
            return self.sequence(p[0])
        return self.mapping(p[0])

    def mapping(self, indent):
        result = {}
        while True:
            p = self.peek()
            if p is None or p[0] < indent:
                return result
            if p[0] > indent:
                raise WorkflowError(f"{self.where()}: unexpected indentation")
            if p[1] == "-" or p[1].startswith("- "):
                return result
            m = KEY.match(p[1])
            if not m:
                raise WorkflowError(f"{self.where()}: expected 'key: value', got {p[1]!r}")
            key = unquote(m.group(1).strip())
            line = self.i
            self.i += 1
            if key in result:
                raise WorkflowError(f"{self.where(line)}: duplicate key {key!r}")
            result[key] = self.value(m.group(2), indent, line)

    def value(self, rest, indent, line):
        rest = strip_comment(rest).strip()
        anchor = ANCHOR.match(rest)
        if anchor:
            result = self.value(anchor.group(2) or "", indent, line)
            self.anchors[anchor.group(1)] = result
            return result
        alias = ALIAS.match(rest)
        if alias:
            if alias.group(1) not in self.anchors:
                raise WorkflowError(f"{self.where(line)}: alias *{alias.group(1)} has no anchor")
            return self.anchors[alias.group(1)]
        if BLOCK_SCALAR.match(rest):
            body = []
            while self.i < len(self.lines):
                raw = self.lines[self.i]
                if raw.strip() and len(raw) - len(raw.lstrip(" ")) <= indent:
                    break
                body.append(raw)
                self.i += 1
            while body and not body[-1].strip():
                body.pop()
            cut = min((len(b) - len(b.lstrip(" ")) for b in body if b.strip()), default=0)
            body = [b[cut:] for b in body]
            if rest.startswith(">"):
                # folded: lines at the base indent join with a space; blank and
                # more-indented lines keep their line breaks
                folded = ""
                for index, b in enumerate(body):
                    if index == 0:
                        folded = b
                    elif b and not b.startswith(" ") and body[index - 1] and not body[index - 1].startswith(" "):
                        folded += " " + b
                    else:
                        folded += "\n" + b
                return folded
            return "\n".join(body)
        if rest == "":
            p = self.peek()
            if p is not None and p[0] > indent:
                return self.node(p[0])
            if p is not None and p[0] == indent and (p[1] == "-" or p[1].startswith("- ")):
                return self.sequence(indent)
            return None
        text = rest
        if rest[0] in "[{":
            while text.count("[") + text.count("{") > text.count("]") + text.count("}"):
                if self.i >= len(self.lines):
                    raise WorkflowError(f"{self.where(line)}: unterminated flow collection")
                text += " " + strip_comment(self.lines[self.i].strip())
                self.i += 1
            return parse_flow(text, self.where(line))
        if rest[0] not in "'\"":
            # a plain scalar continues on deeper lines
            while self.i < len(self.lines):
                raw = self.lines[self.i]
                stripped = raw.strip()
                if not stripped or stripped.startswith("#") or len(raw) - len(raw.lstrip(" ")) <= indent:
                    break
                text += " " + strip_comment(stripped)
                self.i += 1
        return parse_flow(text, self.where(line))

    def sequence(self, indent):
        items = []
        while True:
            p = self.peek()
            if p is None or p[0] != indent or not (p[1] == "-" or p[1].startswith("- ")):
                if p is not None and p[0] > indent:
                    raise WorkflowError(f"{self.where()}: unexpected indentation in a list")
                return items
            rest = p[1][1:]
            body = rest.lstrip(" ")
            column = indent + 1 + (len(rest) - len(body))
            if not body or body.startswith("#"):
                self.i += 1
                items.append(self.node(indent + 1))
                continue
            if body[0] not in "[{'\"" and KEY.match(body):
                # "- key: value": a map whose keys sit at the column after "- "
                self.lines[self.i] = " " * column + body
                items.append(self.mapping(column))
                continue
            line = self.i
            self.i += 1
            items.append(self.value(body, indent, line))


def parse(text, name="<workflow>"):
    parser = _Parser(text, name)
    result = parser.node(0)
    if parser.peek() is not None:
        raise WorkflowError(f"{parser.where()}: text the reader did not consume")
    return result if result is not None else {}


# ---------------------------------------------------------------- expressions

EXPR = re.compile(r"^\$\{\{(.*)\}\}$", re.S)
TOKEN = re.compile(r"\s*(&&|\|\||==|!=|!|\(|\)|,|'(?:[^']|'')*'|[A-Za-z_][\w.-]*)")


def strip_expr(text):
    m = EXPR.match(text.strip())
    return m.group(1).strip() if m else text.strip()


def _tokens(text):
    out, i = [], 0
    text = text.strip()
    while i < len(text):
        m = TOKEN.match(text, i)
        if not m or m.end() == i:
            raise WorkflowError(f"expression {text!r}: cannot read at {text[i:]!r}")
        out.append(m.group(1))
        i = m.end()
    return out


def evaluate(text, context):
    """Value of a workflow `if:` expression. context: event, ref, full."""
    tokens = _tokens(strip_expr(text))
    pos = [0]

    def peek():
        return tokens[pos[0]] if pos[0] < len(tokens) else None

    def take(expected=None):
        tok = peek()
        if tok is None or (expected is not None and tok != expected):
            raise WorkflowError(f"expression {text!r}: expected {expected or 'more'}")
        pos[0] += 1
        return tok

    def atom():
        tok = take()
        if tok == "!":
            return not truthy(atom())
        if tok == "(":
            value = disjunction()
            take(")")
            return value
        if tok.startswith("'"):
            return tok[1:-1].replace("''", "'")
        if tok in ("always", "success", "cancelled", "failure") and peek() == "(":
            take("(")
            take(")")
            return {"always": True, "success": True, "cancelled": False, "failure": False}[tok]
        if tok in ("startsWith", "endsWith", "contains") and peek() == "(":
            take("(")
            a = disjunction()
            take(",")
            b = disjunction()
            take(")")
            a, b = str(a), str(b)
            return {"startsWith": a.startswith(b), "endsWith": a.endswith(b), "contains": b in a}[tok]
        if tok == "github.event_name":
            return context["event"]
        if tok == "github.ref":
            return context["ref"]
        if tok == "inputs.full":
            return bool(context["full"])
        if tok in ("true", "false"):
            return tok == "true"
        raise WorkflowError(f"expression {text!r}: unknown term {tok!r}; teach tools/workflow_jobs.py")

    def truthy(value):
        return bool(value)

    def comparison():
        left = atom()
        while peek() in ("==", "!="):
            op = take()
            right = atom()
            left = (left == right) if op == "==" else (left != right)
        return left

    def conjunction():
        left = comparison()
        while peek() == "&&":
            take()
            right = comparison()
            left = right if truthy(left) else left
        return left

    def disjunction():
        left = conjunction()
        while peek() == "||":
            take()
            right = conjunction()
            left = left if truthy(left) else right
        return left

    value = disjunction()
    if peek() is not None:
        raise WorkflowError(f"expression {text!r}: unread text from {peek()!r}")
    return value


CONTEXTS = {
    "push": {"event": "push", "ref": "refs/heads/main"},
    "pull_request": {"event": "pull_request", "ref": "refs/pull/1/merge"},
}

FROM_JSON = re.compile(r"^fromJSON\((.*)\)$", re.S)
TIERED = re.compile(r"^inputs\.full\s*&&\s*('(?:[^']|'')*')\s*\|\|\s*('(?:[^']|'')*')$", re.S)
LITERAL = re.compile(r"^'((?:[^']|'')*)'$", re.S)


def matrix_value(value, full, where):
    """A matrix value as a Python list/dict/str, evaluating fromJSON expressions."""
    if not isinstance(value, str) or not value.strip().startswith("${{"):
        return value
    inner = strip_expr(value)
    m = FROM_JSON.match(inner)
    if not m:
        raise WorkflowError(f"{where}: matrix expression {value!r} is not fromJSON(...)")
    arg = m.group(1).strip()
    t = TIERED.match(arg)
    if t:
        arg = t.group(1) if full else t.group(2)
    lit = LITERAL.match(arg)
    if not lit:
        raise WorkflowError(f"{where}: matrix expression {value!r}: only fromJSON('...') and "
                            "fromJSON(inputs.full && '...' || '...') are understood")
    try:
        return json.loads(lit.group(1).replace("''", "'"))
    except ValueError as error:
        raise WorkflowError(f"{where}: matrix expression {value!r}: {error}") from error


# ---------------------------------------------------------------- workflows

class Job:

    def __init__(self, workflow, job_id, body):
        if not isinstance(body, dict):
            raise WorkflowError(f"{workflow.name}: job {job_id} is not a map")
        self.workflow = workflow
        self.id = job_id
        self.name = body.get("name") or job_id
        self.condition = body.get("if")
        self.uses = body.get("uses")
        strategy = body.get("strategy") or {}
        self.matrix = strategy.get("matrix") if isinstance(strategy, dict) else None

    def runs(self, event, full=False):
        if self.condition is None:
            return True
        context = dict(CONTEXTS[event])
        context["full"] = full
        return bool(evaluate(str(self.condition), context))

    def combinations(self, full=False):
        """The matrix rows as dicts (one empty dict when there is no matrix)."""
        if self.matrix is None:
            return [{}]
        where = f"{self.workflow.name}: job {self.id}"
        matrix = matrix_value(self.matrix, full, where)
        if not isinstance(matrix, dict):
            raise WorkflowError(f"{where}: matrix is not a map")
        keys, rows = [], [{}]
        for key, value in matrix.items():
            if key in ("include", "exclude"):
                continue
            values = matrix_value(value, full, where)
            if not isinstance(values, list):
                raise WorkflowError(f"{where}: matrix key {key} is not a list")
            keys.append(key)
            rows = [dict(row, **{key: str(v)}) for row in rows for v in values]
        if not keys:
            rows = []
        for entry in matrix_value(matrix.get("exclude") or [], full, where):
            entry = {k: str(v) for k, v in entry.items()}
            rows = [row for row in rows if any(row.get(k) != v for k, v in entry.items())]
        # GitHub's rule: an include entry is merged into every ORIGINAL row whose
        # original matrix values it does not overwrite; when it fits none, it
        # becomes a row of its own (and later entries never merge into it).
        originals = [dict(row) for row in rows]
        added = []
        for entry in matrix_value(matrix.get("include") or [], full, where):
            if not isinstance(entry, dict):
                raise WorkflowError(f"{where}: include entry is not a map")
            entry = {k: str(v) for k, v in entry.items()}
            matched = False
            for index, original in enumerate(originals):
                if all(original[k] == v for k, v in entry.items() if k in keys):
                    rows[index].update(entry)
                    matched = True
            if not matched:
                added.append(dict(entry))
        return rows + added

    def expand(self, full=False):
        """Exact display names, one per matrix row."""
        names = []
        for row in self.combinations(full):
            names.append(self.display(row))
        return names

    def display(self, row):
        template = str(self.name)
        if "${{" in template:
            def substitute(m):
                expr = m.group(1).strip()
                optional = OPTIONAL_TERM.fullmatch(expr)
                if optional:
                    # `matrix.K && format('F', matrix.K) || ''`: F with {0} as the row's K
                    # when the row has a non-empty K, else nothing (GitHub: a missing
                    # matrix key is null, and null && x is null, so the || gives '')
                    key, form, again = optional.group(1), optional.group(2), optional.group(3)
                    if key != again:
                        raise WorkflowError(f"{self.workflow.name}: job {self.id}: name term {expr!r} "
                                            f"tests matrix.{key} but formats matrix.{again}")
                    value = row.get(key, "")
                    return form.replace("''", "'").replace("{0}", value) if value else ""
                if not expr.startswith("matrix.") or expr[len("matrix."):] not in row:
                    raise WorkflowError(f"{self.workflow.name}: job {self.id}: name term {expr!r} is not a matrix value")
                return row[expr[len("matrix."):]]
            return re.sub(r"\$\{\{(.*?)\}\}", substitute, template)
        if row:
            return f"{template} ({', '.join(row.values())})"
        return template

    def pattern(self):
        """A regex every display name of this job matches (unexpanded template included)."""
        template = str(self.name)
        pieces = re.split(r"(\$\{\{.*?\}\})", template)
        body = ""
        for index, piece in enumerate(pieces):
            if index % 2 == 0:
                body += re.escape(piece)
            elif OPTIONAL_TERM.fullmatch(piece[3:-2].strip()):
                # an optional term may print nothing, or the unexpanded term itself
                body += ".*"
            else:
                body += ".+"
        if "${{" not in template and self.matrix is not None:
            body += r"(?: \(.+\))?"
        return body


class Workflow:

    def __init__(self, path):
        self.path = Path(path)
        self.name = self.path.name
        text = self.path.read_text(encoding="utf-8")
        self.data = parse(text, self.name)
        if not isinstance(self.data, dict):
            raise WorkflowError(f"{self.name}: not a map at the top")
        self.title = self.data.get("name") or self.name
        trigger = self.data.get("on", self.data.get("true"))
        if isinstance(trigger, str):
            trigger = {trigger: None}
        elif isinstance(trigger, list):
            trigger = {t: None for t in trigger}
        self.triggers = trigger or {}
        jobs = self.data.get("jobs") or {}
        if not isinstance(jobs, dict) or not jobs:
            raise WorkflowError(f"{self.name}: no jobs")
        self.jobs = [Job(self, job_id, body) for job_id, body in jobs.items()]

    def per_commit(self):
        return "push" in self.triggers or "pull_request" in self.triggers

    def path_filtered(self, event):
        config = self.triggers.get(event)
        return isinstance(config, dict) and any(k in config for k in ("paths", "paths-ignore"))

    def called(self, job):
        """The workflow a reusable-call job runs, from the same directory."""
        target = str(job.uses)
        if not target.startswith("./.github/workflows/"):
            raise WorkflowError(f"{self.name}: job {job.id} calls {target}, outside this repository's workflows")
        return Workflow(self.path.parent / target.rsplit("/", 1)[1])

    def patterns(self, prefix=""):
        """[(regex, job, display prefix)] for every display name the workflow's runs can carry."""
        found = []
        for job in self.jobs:
            if job.uses:
                found += self.called(job).patterns(prefix + str(job.name) + " / ")
            else:
                found.append((re.escape(prefix) + job.pattern(), job, prefix))
        return found

    def find(self, name):
        """The jobs whose display names can be `name`: the unexpanded template (how a skipped
        matrix job is reported), or one of the exact names of its matrix rows in either tier
        (the pattern alone when the matrix cannot be expanded)."""
        hits = []
        for regex, job, prefix in self.patterns():
            if not re.fullmatch(regex, name):
                continue
            if name == prefix + str(job.name):
                hits.append(job)
                continue
            try:
                names = {prefix + n for full in (False, True) for n in job.expand(full)}
            except WorkflowError:
                hits.append(job)
                continue
            if name in names:
                hits.append(job)
        return hits


def load(name, directory=WORKFLOWS):
    return Workflow(Path(directory) / name)


def required_names(directory=WORKFLOWS):
    """Every job name a pull request's per-commit run carries, from every workflow a pull
    request starts without a path filter (a path-filtered workflow cannot be a required
    check: it would block every pull request it does not run on). Sorted, unique."""
    names = []
    for path in sorted(Path(directory).glob("*.y*ml")):
        workflow = Workflow(path)
        if "pull_request" not in workflow.triggers or workflow.path_filtered("pull_request"):
            continue
        for job in workflow.jobs:
            if not job.runs("pull_request", full=False):
                continue
            if job.uses:
                raise WorkflowError(f"{workflow.name}: job {job.id} calls a reusable workflow per commit")
            names.extend(job.expand(full=False))
    duplicates = sorted({n for n in names if names.count(n) > 1})
    if duplicates:
        raise WorkflowError(f"two per-commit jobs share a name: {duplicates}")
    return sorted(names)


def main(argv):
    if len(argv) == 3 and argv[1] == "--dump":
        print(json.dumps(parse(Path(argv[2]).read_text(encoding="utf-8"), argv[2]), indent=1, sort_keys=True))
        return 0
    if len(argv) == 2 and argv[1] == "--names":
        for name in required_names():
            print(name)
        return 0
    print(__doc__.strip().splitlines()[-2])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
