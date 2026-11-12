#!/usr/bin/env python3
"""Check the platform policy table against the workflows it names.

Policy: docs/PLATFORMS.md, "The table" and "Policy rules". A support claim is
only a claim when a workflow file names the runner that proves it. This script
reads the markdown table directly; it does not parse YAML.
"""

import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCUMENT = ROOT / "docs" / "PLATFORMS.md"

FUNCTIONAL = ROOT / "tests" / "functional"
FUNCTIONAL_SUFFIXES = (".cpp", ".h", ".inc")

# A string literal naming a wildcard address, with or without a port: "0.0.0.0",
# "::", "[::]", "0.0.0.0:40000", "[::]:0".
WILDCARD = re.compile(r'"(?:0\.0\.0\.0|::|\[::\])(?::\d+)?"')

# Every wildcard literal tests/functional/ may hold, keyed by file and stripped line,
# with the reason it stays. A wildcard bind on macOS is subject to the
# application firewall, so a case that only needs loopback binds the loopback
# address of its family (127.0.0.1 or ::1) with port 0 instead. A line here
# that no longer occurs in the file fails the check as well, so the list
# never outlives the code.
WILDCARD_ALLOWED = {
    ("tests/functional/rocketnet_functional_tests.cpp",
     'rocketnet_address_parse( &refused_address, "0.0.0.0" );        // port 0: the kernel refuses it at send'):
        "a send destination, not a bind: the refused entry of the batch",
    ("tests/functional/rocketnet_functional_tests.cpp",
     'struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "[::]", &server_config, sizeof( server_config ) );'):
        "dual-stack transport case: an ipv4 client of the ipv6 wildcard",
    ("tests/functional/rocketnet_functional_tests.cpp",
     'rocketnet_check( rocketnet_address_parse( &bind_address, "[::]:0" ) );'):
        "rio dual-stack case (Windows): ipv4 and ipv6 clients of the ipv6 wildcard",
    ("tests/functional/rocketnet_functional_tests.cpp",
     'rocketnet_test_server_gso_wire_exchange( "test_gso_transport_server_wire_dual_stack", "[::]", "127.0.0.1", "127.0.0.1", 0, 1 );'):
        "gso dual-stack case (Linux): an ipv4 client of the ipv6 wildcard",
    ("tests/functional/rocketnet_functional_tests.cpp",
     'rocketnet_test_server_uring_wire_exchange( "[::]", "127.0.0.1", "127.0.0.1", 1 );'):
        "io_uring dual-stack case (Linux): an ipv4 client of the ipv6 wildcard",
    ("tests/functional/rocketnet_functional_tests.cpp",
     'void rocketnet_test_msgx_client_transport_wire_dual_stack() { msgx_client_wire( "[::]", "127.0.0.1" ); }'):
        "msgx dual-stack case (macOS): an ipv4 client of the ipv6 wildcard",
    ("tests/functional/rocketnet_functional_tests.cpp",
     'void rocketnet_test_iocp_client_transport_wire_dual_stack() { iocp_client_wire( "[::]", "127.0.0.1" ); }'):
        "iocp dual-stack case (Windows): an ipv4 client of the ipv6 wildcard",
}

TIERS = {"supported", "nightly", "unsupported"}
COLUMNS = {
    "operating system": "os",
    "architecture": "arch",
    "compiler": "compiler",
    "transports": "transports",
    "tier": "tier",
    "workflow": "workflow",
    "runner label": "label",
    "per commit functional": "functional",
}


def normalise_header(cell):
    """Reduce a header cell to the words used by COLUMNS.

    Policy: docs/PLATFORMS.md, "The table".
    """
    return re.sub(r"[^a-z0-9]+", " ", cell.strip().lower()).strip()


def split_row(line):
    """Split one markdown table row into stripped cells.

    Policy: docs/PLATFORMS.md, "The table".
    """
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def clean(cell):
    """Remove markdown code ticks and surrounding whitespace from a value.

    Policy: docs/PLATFORMS.md, "The table".
    """
    return cell.strip().strip("`").strip()


def is_placeholder(value):
    """True for the table's way of saying "no value".

    Policy: docs/PLATFORMS.md, "A claim with no leg is refused".
    """
    return value in ("", "-", "\u2014", "n/a", "none")


def parse_table(text):
    """Return every row of the policy table as a dict keyed by COLUMNS.

    Policy: docs/PLATFORMS.md, "The table". The first header line that names
    every required column starts the table; rows continue until a line that
    is not a markdown row.
    """
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not line.lstrip().startswith("|"):
            continue
        headers = [normalise_header(cell) for cell in split_row(line)]
        keys = [COLUMNS.get(header) for header in headers]
        if any(key is None for key in keys):
            continue
        if index + 1 >= len(lines) or not lines[index + 1].lstrip().startswith("|"):
            continue
        rows = []
        for row_line in lines[index + 2:]:
            if not row_line.lstrip().startswith("|"):
                break
            cells = split_row(row_line)
            if len(cells) != len(keys):
                raise ValueError(
                    f"row has {len(cells)} cells, header has {len(keys)}: {row_line.strip()}"
                )
            rows.append({key: clean(cell) for key, cell in zip(keys, cells)})
        return rows
    raise ValueError("no platform policy table found")


def label_tokens(label):
    """Split a runner label into the tokens a workflow must contain.

    Policy: docs/PLATFORMS.md, "A claim with no leg is refused". Labels are
    written as comma-separated YAML lists or as a single image name, so a
    token is any comma- or whitespace-separated part with quotes, brackets
    and code ticks removed.
    """
    tokens = []
    for token in re.split(r"[,\s]+", label):
        token = token.strip("[]\"'`")
        if token:
            tokens.append(token)
    return tokens


def job_blocks(text):
    """Split a workflow into the text of each top-level job.

    Policy: docs/PLATFORMS.md, "What supported promises". A job starts at a
    line indented two spaces that ends in a colon, inside the jobs: section,
    and runs to the next such line.
    """
    blocks = []
    current = None
    in_jobs = False
    for line in text.splitlines():
        if re.match(r"^jobs:\s*$", line):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        if re.match(r"^  [A-Za-z0-9_-]+:\s*$", line):
            current = []
            blocks.append(current)
        if current is not None:
            current.append(line)
    return ["\n".join(block) for block in blocks]


RUNNER_LINE = re.compile(r"^\s*(?:-\s+)?runs-on:|\brunner:\s")


def runner_label_sets(block):
    """Return each runner label set a job names, as a list of token sets.

    Policy: docs/PLATFORMS.md, "A claim with no leg is refused". A label set
    is one `runs-on:` value or one matrix `runner:` value: a bracketed list
    (YAML flow or a JSON string, `fromJSON(...)` alternatives each count as
    one list) or a single bare name. A comment line, or the comment part of
    a line, names no runner, and a row's tokens must all be in ONE set: a job
    whose matrix holds a Windows X64 runner and a macOS ARM64 runner holds no
    Windows ARM64 runner.
    """
    sets = []
    for line in block.splitlines():
        if line.lstrip().startswith("#") or not RUNNER_LINE.search(line):
            continue
        line = re.sub(r"\s+#[^'\"\]]*$", "", line)
        lists = re.findall(r"\[([^\[\]]*)\]", line)
        for body in lists:
            tokens = {token.strip(" \"'`") for token in body.split(",")}
            sets.append({token for token in tokens if token})
        if not lists:
            bare = re.match(r"^\s*(?:-\s+)?runs-on:\s*([A-Za-z0-9_.-]+)\s*$", line)
            if bare:
                sets.append({bare.group(1)})
    return sets


def holds_label(block, tokens):
    """True when one runner label set of the job holds every token of the row."""
    wanted = set(tokens)
    return any(wanted <= label_set for label_set in runner_label_sets(block))


# ---- GitHub expressions, read for one question: can this `if:` be true on a
# per-commit run? Per commit `inputs.full` is unset, so false; every other
# context and every function is unknown. The answer is three-valued (True,
# False, None for unknown), and a step is refused only when it is False.

UNKNOWN = object()
EXPRESSION_TOKEN = re.compile(
    r"\s*(?:(?P<string>'(?:[^']|'')*')|(?P<number>-?\d+(?:\.\d+)?)"
    r"|(?P<op>==|!=|<=|>=|&&|\|\||[!<>(),])"
    r"|(?P<path>[A-Za-z_][A-Za-z0-9_-]*(?:\.[A-Za-z0-9_*-]+|\[[^\]]*\])*))"
)
PER_COMMIT_CONTEXT = {"inputs.full": False}


def expression_tokens(text):
    """Split an expression into (kind, text) tokens; raise ValueError on any other text."""
    text = text.strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2]
    tokens = []
    position = 0
    while position < len(text):
        if text[position:].strip() == "":
            break
        match = EXPRESSION_TOKEN.match(text, position)
        if not match or match.end() == position:
            raise ValueError(f"cannot read {text[position:]!r}")
        kind = match.lastgroup
        tokens.append((kind, match.group(kind)))
        position = match.end()
    return tokens


def truth(value):
    """Truthiness as GitHub reads it, or None when the value is unknown."""
    if value is UNKNOWN:
        return None
    return value not in (False, None, 0, "")


class Expression:
    """A recursive-descent reader of one expression under a context."""

    def __init__(self, text, context):
        self.tokens = expression_tokens(text)
        self.position = 0
        self.context = context

    def peek(self):
        return self.tokens[self.position][1] if self.position < len(self.tokens) else None

    def take(self, expected=None):
        if self.position >= len(self.tokens):
            raise ValueError("expression ends early")
        token = self.tokens[self.position]
        if expected is not None and token[1] != expected:
            raise ValueError(f"expected {expected!r}, read {token[1]!r}")
        self.position += 1
        return token

    def value(self):
        result = self.disjunction()
        if self.position != len(self.tokens):
            raise ValueError(f"unread text from {self.peek()!r}")
        return result

    def disjunction(self):
        left = self.conjunction()
        while self.peek() == "||":
            self.take()
            right = self.conjunction()
            left_truth = truth(left)
            if left_truth is True:
                continue
            if left_truth is False:
                left = right
            elif truth(right) is True:
                left = True
            else:
                left = UNKNOWN
        return left

    def conjunction(self):
        left = self.equality()
        while self.peek() == "&&":
            self.take()
            right = self.equality()
            left_truth = truth(left)
            if left_truth is False:
                continue
            if left_truth is True:
                left = right
            elif truth(right) is False:
                left = False
            else:
                left = UNKNOWN
        return left

    def equality(self):
        left = self.unary()
        while self.peek() in ("==", "!=", "<", "<=", ">", ">="):
            operator = self.take()[1]
            right = self.unary()
            if left is UNKNOWN or right is UNKNOWN:
                left = UNKNOWN
                continue
            if isinstance(left, str) and isinstance(right, str):
                left, right = left.lower(), right.lower()
            try:
                left = {"==": left == right, "!=": left != right, "<": left < right,
                        "<=": left <= right, ">": left > right, ">=": left >= right}[operator]
            except TypeError:
                left = UNKNOWN
        return left

    def unary(self):
        if self.peek() == "!":
            self.take()
            operand = truth(self.unary())
            return UNKNOWN if operand is None else not operand
        return self.primary()

    def primary(self):
        kind, text = self.take()
        if text == "(":
            result = self.disjunction()
            self.take(")")
            return result
        if kind == "string":
            return text[1:-1].replace("''", "'")
        if kind == "number":
            return float(text)
        if kind != "path":
            raise ValueError(f"unexpected {text!r}")
        if self.peek() == "(":
            self.take()
            while self.peek() != ")":
                self.disjunction()
                if self.peek() == ",":
                    self.take()
                elif self.peek() != ")":
                    raise ValueError(f"expected ',' or ')', read {self.peek()!r}")
            self.take(")")
            return UNKNOWN
        literals = {"true": True, "false": False, "null": None}
        if text in literals:
            return literals[text]
        return self.context.get(text, UNKNOWN)


def can_run_per_commit(condition):
    """Return False when an `if:` is false on every per-commit run, else True.

    An unreadable expression raises ValueError, so the caller reports it
    rather than guessing.
    """
    if condition is None:
        return True
    return truth(Expression(condition, PER_COMMIT_CONTEXT).value()) is not False


def key_value(lines, index, indent):
    """Return the value of the `key:` at lines[index], continuation lines joined."""
    value = lines[index].split(":", 1)[1].strip()
    following = index + 1
    while following < len(lines):
        line = lines[following]
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        if line.strip() and not line.lstrip().startswith("#"):
            value += " " + line.strip()
        following += 1
    if value[:1] in (">", "|"):
        value = re.sub(r"^[>|][+-]?\s*", "", value)
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    return value


def job_condition(block):
    """Return the job-level `if:` of a job block, or None."""
    lines = block.splitlines()
    for index, line in enumerate(lines):
        if re.match(r"^    if:", line):
            return key_value(lines, index, 4)
    return None


def step_conditions(block, step):
    """Return the `if:` (or None) of every step of the job whose whole name is `step`."""
    lines = block.splitlines()
    found = []
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)(-\s+)?name:\s*(.*?)\s*$", line)
        if not match:
            continue
        name = match.group(3)
        if len(name) >= 2 and name[0] == name[-1] and name[0] in "\"'":
            name = name[1:-1]
        if name != step:
            continue
        key_indent = len(match.group(1)) + (len(match.group(2)) if match.group(2) else 0)
        start = index
        if not match.group(2):
            while start > 0 and not re.match(r"^\s{%d}-\s" % (key_indent - 2), lines[start]):
                start -= 1
        end = index + 1
        while end < len(lines):
            line_end = lines[end]
            if line_end.strip() and not line_end.lstrip().startswith("#") \
                    and len(line_end) - len(line_end.lstrip()) < key_indent:
                break
            end += 1
        condition = None
        own_key = re.compile(r"^(?:\s{%d}|\s{%d}-\s+)if:" % (key_indent, max(key_indent - 2, 0)))
        for position in range(start, end):
            if own_key.match(lines[position]):
                condition = key_value(lines, position, key_indent)
                break
        found.append(condition)
    return found


def matrix_transports(block, os_name):
    """Return the transports named as server or client in the job's TRANSPORT_MATRIX_<os_name> cells."""
    lines = block.splitlines()
    named = set()
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)TRANSPORT_MATRIX_(\w+):\s*\|\s*$", line)
        if not match or match.group(2) != os_name:
            continue
        indent = len(match.group(1))
        following = index + 1
        while following < len(lines) and (not lines[following].strip()
                                          or len(lines[following]) - len(lines[following].lstrip()) > indent):
            words = lines[following].split()
            named.update(words[:2])
            following += 1
    return named


OS_TOKENS = {"Linux": "LINUX", "macOS": "MACOS", "Windows": "WINDOWS"}


def check_transports(row, text, blocks):
    """Return the failures for a supported row's Transports column.

    Policy: docs/PLATFORMS.md, "What supported promises". Every transport
    the row names (outside a `build only:` clause, which claims a build and
    no run) must be a server or client transport of a TRANSPORT_MATRIX cell
    for the row's OS in a job of the named workflow that holds the row's
    runner label. The vocabulary is every transport any matrix of the
    workflow names, so a transport no matrix runs anywhere is still a word
    this check reads.
    """
    name = f"{row['os']} / {row['arch']} / {row['compiler']}"
    os_names = [OS_TOKENS[token] for token in label_tokens(row["label"]) if token in OS_TOKENS]
    if len(os_names) != 1:
        return [f"{name}: runner label {row['label']!r} names no one operating system "
                f"of {sorted(OS_TOKENS)}"]
    vocabulary = set()
    for os_name in OS_TOKENS.values():
        for block in job_blocks(text):
            vocabulary |= matrix_transports(block, os_name)
    vocabulary.discard("auto")
    claimed_text = re.sub(r"build only:[^;]*;?", "", row["transports"])
    claimed = [word for word in sorted(vocabulary) if re.search(r"(?<![A-Za-z0-9_])" + re.escape(word) + r"(?![A-Za-z0-9_])", claimed_text)]
    run = set()
    for block in blocks:
        run |= matrix_transports(block, os_names[0])
    return [
        f"{name}: transport {word!r} has no TRANSPORT_MATRIX_{os_names[0]} cell in a job of "
        f"{row['workflow']} that holds the runner label {row['label']!r}"
        for word in claimed if word not in run
    ]


def check_functional(row, text):
    """Return the failures for a supported row's per-commit functional claim.

    Policy: docs/PLATFORMS.md, "What supported promises", item 1. A supported
    row declares what functional coverage its per-commit leg runs: `full`
    followed by the whole name of the whole-suite step, or `subset:`
    followed by the whole names, separated by semicolons, of the steps that
    run the subset. Every named step must be a step, by its whole name, of a
    job of the named workflow one of whose runner label sets holds every
    token of the row's runner label, and neither the step's `if:` nor its
    job's `if:` may be false on every per-commit run (`if: false`, or
    `if: inputs.full`, which only the nightly caller sets).
    """
    name = f"{row['os']} / {row['arch']} / {row['compiler']}"
    value = row.get("functional", "")
    if is_placeholder(value):
        return [f"{name}: supported row declares no per-commit functional step "
                f"(write `full: <step>` or `subset: <step>; <step>`)"]
    match = re.match(r"^(full|subset)\s*:\s*(.+)$", value)
    if not match:
        return [f"{name}: per-commit functional {value!r} is not `full: <step>` or `subset: <step>; ...`"]
    if match.group(1) == "full":
        steps = [match.group(2).strip()]
    else:
        steps = [step.strip() for step in match.group(2).split(";") if step.strip()]
    tokens = label_tokens(row["label"])
    blocks = [block for block in job_blocks(text) if holds_label(block, tokens)]
    failures = []
    for step in steps:
        runs = False
        named = False
        for block in blocks:
            for condition in step_conditions(block, step):
                named = True
                try:
                    if can_run_per_commit(job_condition(block)) and can_run_per_commit(condition):
                        runs = True
                except ValueError as error:
                    failures.append(f"{name}: per-commit functional step {step!r}: cannot read its `if:` "
                                    f"({error}); the check refuses what it cannot read")
        if not named:
            failures.append(
                f"{name}: per-commit functional step {step!r} is not the whole name of a step "
                f"in any job of {row['workflow']} that holds the runner label {row['label']!r}"
            )
        elif not runs:
            failures.append(
                f"{name}: per-commit functional step {step!r} has an `if:` (or its job has one) "
                f"that is false on every per-commit run, so it is no per-commit leg"
            )
    return failures + check_transports(row, text, blocks)


def check_row(row, workflow_cache, root=ROOT):
    """Return the failures for one table row.

    Policy: docs/PLATFORMS.md, "Policy rules": a supported or nightly row must
    name a workflow file and a runner label, the file must exist, and one job
    of that file must hold every token of the label in one runner label set.
    An unsupported row names no leg.
    """
    name = f"{row['os']} / {row['arch']} / {row['compiler']}"
    tier = row["tier"].lower()
    workflow = row["workflow"]
    label = row["label"]

    if tier not in TIERS:
        return [f"{name}: tier {row['tier']!r} is not one of supported, nightly, unsupported"]

    if tier == "unsupported":
        if not is_placeholder(workflow) or not is_placeholder(label):
            return [f"{name}: unsupported row names a workflow or runner label"]
        return []

    if is_placeholder(workflow) or is_placeholder(label):
        return [
            f"{name}: {tier} claim has no CI leg "
            f"(workflow={workflow!r}, runner label={label!r})"
        ]

    path = root / workflow
    if not path.is_file():
        return [f"{name}: workflow file {workflow!r} does not exist"]

    text = workflow_cache.get(path)
    if text is None:
        text = path.read_text(encoding="utf-8")
        workflow_cache[path] = text

    tokens = label_tokens(label)
    if not any(holds_label(block, tokens) for block in job_blocks(text)):
        return [
            f"{name}: no job of {workflow} runs on a runner whose label set holds "
            f"every token of {label!r} (runs-on: or matrix runner: values, one set at a time)"
        ]
    if tier == "supported":
        return check_functional(row, text)
    return []


def check_wildcard_binds(root=ROOT, allowed=None):
    """Return the failures for wildcard address literals in tests/functional/.

    Every line of tests/functional/ holding a WILDCARD literal must be a key of
    WILDCARD_ALLOWED, at most as many times as the list names it, and every
    listed line must still occur. The allowed set defaults to WILDCARD_ALLOWED;
    only the self-check passes another.
    """
    if allowed is None:
        allowed = WILDCARD_ALLOWED
    found = Counter()
    where = {}
    directory = root / "tests" / "functional"
    for path in sorted(directory.iterdir()):
        if path.suffix not in FUNCTIONAL_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if WILDCARD.search(line):
                key = (relative, line.strip())
                found[key] += 1
                where.setdefault(key, []).append(number)

    failures = []
    for key, count in found.items():
        if key not in allowed or count > 1:
            for number in where[key][0 if key not in allowed else 1:]:
                failures.append(
                    f"{key[0]}:{number}: wildcard address in tests/functional/ is not on "
                    f"the listed set; bind 127.0.0.1 or ::1 with port 0, or list "
                    f"it in WILDCARD_ALLOWED with its reason: {key[1]}"
                )
    for key in allowed:
        if key not in found:
            failures.append(f"{key[0]}: listed wildcard line no longer occurs: {key[1]}")
    return failures


def self_check(check=check_wildcard_binds):
    """Prove check_wildcard_binds still reports violations; return failures.

    Builds a temporary functional-style tree and uses no repository file.
    Each case must be reported by `check`; a case it lets through is a failure
    of the checker itself.
    """
    import tempfile

    name = "tests/functional/case.cpp"
    listed_line = 'void listed_case() { wire( "[::]", "127.0.0.1" ); }'
    unlisted_line = 'rocketnet_check( rocketnet_address_parse( &bind_address, "0.0.0.0:40000" ) );'
    allowed = {(name, listed_line): "listed for the self-check"}

    cases = [
        ("unlisted wildcard literal", [unlisted_line, listed_line], "is not on the listed set"),
        ("listed line beyond its count", [listed_line, listed_line], "is not on the listed set"),
        ("listed line absent", ["int unrelated = 0;"], "no longer occurs"),
    ]
    problems = []
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "tests" / "functional").mkdir(parents=True)
        # The clean tree must pass, or the cases below prove nothing.
        (root / name).write_text(listed_line + "\n", encoding="utf-8")
        clean = check(root=root, allowed=allowed)
        if clean:
            problems.append(f"self-check: clean tree was reported: {clean}")
        for label, lines, expected in cases:
            (root / name).write_text("\n".join(lines) + "\n", encoding="utf-8")
            reported = check(root=root, allowed=allowed)
            if not any(expected in failure for failure in reported):
                problems.append(f"self-check: {label} was not reported ({expected!r})")
    return problems


def accept_everything(root=ROOT, allowed=None):
    """A deliberately broken check, used to see the self-check fail."""
    return []


POLICY_WORKFLOW = """\
on: [push]
jobs:
  build:
    runs-on: ${{ fromJSON(matrix.runner) }}
    strategy:
      matrix:
        include:
          - { os: linux, runner: '["self-hosted","Linux","X64","fleet"]' }
          - { os: macos, runner: '["self-hosted","macOS","ARM64","fleet"]' }
    steps:
      - name: Run functional suite (whole, on Linux; ladder)
        if: matrix.leg == 'functional' && (inputs.full || matrix.build_type == 'Debug')
        run: ./bin/functional
      - name: Run functional suite (one pinned case)
        run: ./bin/functional --run one
      - name: Transport family matrix
        env:
          TRANSPORT_MATRIX_LINUX: |
            portable portable
            gso portable
          TRANSPORT_MATRIX_MACOS: |
            portable portable
            msgx msgx
        run: true
  happy:
    # ARM64 is named in this comment only
    runs-on: ${{ fromJSON(matrix.os == 'windows' && '["self-hosted","Windows","X64"]' || '["self-hosted","macOS","ARM64"]') }}
    steps:
      - name: Run happy_path
        run: go test
  windows:
    runs-on: [self-hosted, Windows, X64]
    steps:
      - name: Transport family matrix
        env:
          TRANSPORT_MATRIX_WINDOWS: |
            iocp portable
            rio portable
        run: true
"""

POLICY_HEADER = ("| Operating system | Architecture | Compiler | Transports | Tier | Workflow | Runner label | Per-commit functional |\n"
                 "| --- | --- | --- | --- | --- | --- | --- | --- |\n")
POLICY_CLEAN_ROWS = [
    "| Linux | x86_64 | GCC | portable, gso | supported | w.yml | Linux, X64 | full: Run functional suite (whole, on Linux; ladder) |",
    "| macOS | arm64 | Clang | portable, msgx | supported | w.yml | macOS, ARM64 | full: Run functional suite (whole, on Linux; ladder) |",
    "| Windows | x86_64 | MSVC | portable, iocp, rio | supported | w.yml | Windows, X64 | subset: Run happy_path |",
    "| Linux | s390x | cross | build only: portable, rio; run: no transport | supported | w.yml | Linux, X64 | subset: Run functional suite (one pinned case) |",
    "| Linux | arm64 | none | none | unsupported | - | - | - |",
]


def check_policy_tree(root, rows):
    """Run every row of a scratch table against root/w.yml; return the failures."""
    table = parse_table(POLICY_HEADER + "\n".join(rows) + "\n")
    cache = {}
    failures = []
    for row in table:
        failures.extend(check_row(row, cache, root=root))
    return failures


def policy_self_check(check=check_policy_tree):
    """Prove the row checks still refuse each planted violation; return failures.

    Builds a scratch workflow and table and uses no repository file. The
    clean tree must pass; each plant must be reported with its words.
    """
    import tempfile

    whole = "Run functional suite (whole, on Linux; ladder)"
    cases = [
        ("supported Windows arm64 row on a job of Windows X64 and macOS ARM64 runners (V6-B B4)",
         {}, ["| Windows | arm64 | MSVC | portable | supported | w.yml | Windows, ARM64 | subset: Run happy_path |"],
         "no job of w.yml runs on a runner"),
        ("nightly Linux arm64 row whose ARM64 is only in a comment and another runner (V6-B B4)",
         {}, ["| Linux | arm64 | GCC | portable | nightly | w.yml | Linux, ARM64 | - |"],
         "no job of w.yml runs on a runner"),
        ("full: names a prefix of the whole-suite step (VERIFY-3 W12)",
         {}, ["| Linux | x86_64 | GCC | portable | supported | w.yml | Linux, X64 | full: Run functional suite |"],
         "is not the whole name of a step"),
        ("whole-suite step given if: false (VERIFY-3 W12)",
         {"if: matrix.leg == 'functional' && (inputs.full || matrix.build_type == 'Debug')": "if: false"}, [],
         "false on every per-commit run"),
        ("whole-suite step given if: inputs.full",
         {"if: matrix.leg == 'functional' && (inputs.full || matrix.build_type == 'Debug')": "if: ${{ inputs.full }}"}, [],
         "false on every per-commit run"),
        ("whole-suite step given if: <leg> && inputs.full",
         {"if: matrix.leg == 'functional' && (inputs.full || matrix.build_type == 'Debug')":
          "if: matrix.leg == 'functional' && inputs.full"}, [],
         "false on every per-commit run"),
        ("whole-suite step's job given if: inputs.full",
         {"    runs-on: ${{ fromJSON(matrix.runner) }}": "    if: inputs.full\n    runs-on: ${{ fromJSON(matrix.runner) }}"}, [],
         "false on every per-commit run"),
        ("whole-suite step with an if: the reader cannot read",
         {"if: matrix.leg == 'functional' && (inputs.full || matrix.build_type == 'Debug')": "if: matrix.leg = 'x'"}, [],
         "cannot read its `if:`"),
        ("Linux row claims rio, which only the Windows job's matrix runs (V6-B B4)",
         {}, ["| Linux | x86_64 | GCC | portable, rio | supported | w.yml | Linux, X64 | subset: Run happy_path |"],
         "transport 'rio' has no TRANSPORT_MATRIX_LINUX cell"),
    ]
    problems = []
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "w.yml").write_text(POLICY_WORKFLOW, encoding="utf-8")
        clean = check(root, POLICY_CLEAN_ROWS)
        if clean:
            problems.append(f"policy self-check: clean tree was reported: {clean}")
        for label, edits, extra_rows, expected in cases:
            text = POLICY_WORKFLOW
            for old, new in edits.items():
                if old not in text:
                    problems.append(f"policy self-check: {label}: fixture has no {old!r}")
                text = text.replace(old, new)
            (root / "w.yml").write_text(text, encoding="utf-8")
            reported = check(root, POLICY_CLEAN_ROWS + extra_rows)
            if not any(expected in failure for failure in reported):
                problems.append(f"policy self-check: {label} was not reported ({expected!r}); reported {reported}")
        (root / "w.yml").write_text(POLICY_WORKFLOW, encoding="utf-8")
    return problems, len(cases)


def accept_every_row(root, rows):
    """A deliberately broken row check, used to see the policy self-check fail."""
    return []




def main():
    """Check every row and exit 0 only when the whole policy holds.

    Policy: docs/PLATFORMS.md, "Checking the table".
    """
    if "--self-check-broken" in sys.argv[1:]:
        problems = self_check(accept_everything) + policy_self_check(accept_every_row)[0]
        for problem in problems:
            print(f"check_platform_policy: {problem}", file=sys.stderr)
        return 1 if problems else 0
    if "--self-check" in sys.argv[1:]:
        policy_problems, policy_cases = policy_self_check()
        problems = self_check() + policy_problems
        for problem in problems:
            print(f"check_platform_policy: {problem}", file=sys.stderr)
        if not problems:
            print(f"check_platform_policy: self-check: 3 wildcard and {policy_cases} row violations "
                  f"reported, clean trees pass")
        return 1 if problems else 0

    try:
        rows = parse_table(DOCUMENT.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"check_platform_policy: {error}", file=sys.stderr)
        return 1

    workflow_cache = {}
    failures = check_wildcard_binds()
    for row in rows:
        failures.extend(check_row(row, workflow_cache))

    if failures:
        for failure in failures:
            print(f"check_platform_policy: {failure}", file=sys.stderr)
        return 1

    supported = sum(1 for row in rows if row["tier"].lower() == "supported")
    nightly = sum(1 for row in rows if row["tier"].lower() == "nightly")
    unsupported = len(rows) - supported - nightly
    print(
        f"check_platform_policy: {supported} supported, {nightly} nightly, "
        f"{unsupported} unsupported row(s) match their named workflows; "
        f"{len(WILDCARD_ALLOWED)} listed wildcard line(s) in tests/functional/"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
