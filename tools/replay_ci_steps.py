#!/usr/bin/env python3
"""Run one job of a workflow's steps here, as the runner would, before asking for a dispatch.

    python3 tools/replay_ci_steps.py WORKFLOW JOB [--set NAME=VALUE ...] [--logs DIR] [--list]
                                     [--result FILE] [--allow-over-cap]
    python3 tools/replay_ci_steps.py --rows WORKFLOW [--job JOB ...] [--set inputs.full=true]
    python3 tools/replay_ci_steps.py --coverage WORKFLOW --commit SHA [--job JOB ...]
                                     [--set inputs.full=true] RESULT ...
    python3 tools/replay_ci_steps.py --self-test

Run it from the top of a fresh checkout of the commit (a new `git worktree`,
never a tree with build products in it: CI's checkout is clean). It reads
WORKFLOW (e.g. .github/workflows/ci.yml), takes the job whose key is JOB
(e.g. lint, build-and-test), and runs its run: steps in order:

- `if:` is evaluated as the runner does for the forms this repository uses:
  ==, !=, &&, ||, !, parentheses, startsWith, contains, format, success(),
  failure(), always() and cancelled(); a step with no `if:` runs on success().
  A step after a red one is skipped unless its `if:` says otherwise.
- `${{ ... }}` in run: and env: is replaced the same way. matrix.* and
  inputs.* come from --set (e.g. --set matrix.leg=checks --set
  matrix.build_type=Release --set matrix.dev=OFF; runner.os defaults to this
  host's); a matrix.* the job reads and --set did not give is an error,
  never a silent empty string. steps.* outputs are empty.
- The env: of the workflow, the job and the step apply, in that order.
- The shell is the one the runner would use: the step's own `shell:`, else
  the job's `defaults: run: shell:`, else the workflow's (ci.yml sets `bash
  --noprofile --norc -euo pipefail {0}`), else GitHub's documented default
  for a run step on Linux and macOS, `bash -e {0}`. `shell: bash` is `bash
  --noprofile --norc -eo pipefail {0}`, `sh` is `sh -e {0}`, `python` runs
  python3, `pwsh` is `pwsh -command ". '{0}'"`, and any other value is a
  command template whose {0} is the step's script. working-directory: is
  honoured the same way (step, then the job's defaults, then the workflow's).
- What a step writes to $GITHUB_ENV and $GITHUB_PATH reaches the steps after
  it. RUNNER_TEMP, GITHUB_WORKSPACE, GITHUB_RUN_ID and the rest are set to
  local stand-ins.
- YAML anchors on a step's run: (`run: &name |`) and their aliases
  (`run: *name`) resolve within the file.

`uses:` steps (checkout, caches, uploads) are not run and are listed as such.
Each run step prints its name, its exit status and its seconds; its output
goes to DIR/step-N.log (default a new temporary directory, outside the
checkout) and the tail of a red one is printed. --list prints the steps and
whether each would run.

TIME. The job's `timeout-minutes:` is evaluated as the runner would (per
commit `${{ inputs.full && 7200 || 2 }}` is two minutes; GitHub's default
is 360) and so is each step's. The replay never stops a step at a cap: it
runs every step to its end and then reports, on its last line, both whether
every step passed and the seconds the run steps took against the job's cap.
A job whose run steps sum past the cap, or a step past its own cap, is
reported as OVER CAP with the seconds. Checkout, caches and the runner's own
setup are not timed, so the real job takes longer than the sum.

Exit 0 when every step that ran exited 0 within the caps; 1 when a step was
red; 3 when every step passed but the job or a step went OVER CAP, unless
--allow-over-cap (off by default) says this host may be slower than the
runner, in which case OVER CAP is still printed and the exit is 0; 2 for a
usage or workflow error.

--result FILE writes the replay's verdict as JSON (workflow, job, the --set
values, the commit at HEAD, whether the tree was clean when the replay
started, each step's exit and seconds, the total, the cap, the exit status)
for --coverage.

ROWS AND COVERAGE. A split job runs as several matrix rows (the shards of the
trace check, Debug unit, functional; the checks and source_checks rows) or
several jobs (the lint shards). --rows WORKFLOW lists every row of every job
the workflow runs on a commit (or of the --job JOBs named), with the check
name GitHub gives it and the --set values that replay it: the matrix is
expanded as GitHub does (the product of its lists, then exclude:, then
include:, which adds its values to every row it matches on the matrix's own
keys and is a new row when it matches none). --set inputs.full=true lists
the nightly tier instead. A job with `if: inputs.full` is listed as not run
on a commit.

--coverage WORKFLOW --commit SHA RESULT... reads replay result files (or
every *.json in a directory) and says, row by row, which rows have a green
replay at SHA from a clean tree and which have none (RED, OVER CAP, NOT
CLEAN, or NONE). A result matches a row when its job is the row's, every
matrix value of the row (runs-on's included) and every one the job reads is
the same in the result (empty when the row has none), its other --set
values are the coverage's own, it ran on the OS the row's runs-on names (read
from the labels: linux, ubuntu, macos, windows; labels that name none are not
checked), and its workflow file is byte for byte the WORKFLOW given here (git
blob id). A replay is recorded as clean only when the tree was clean and
WORKFLOW was the checkout's own file as committed at HEAD, so run --coverage
in a checkout of SHA. Result files are read as written: a file made by hand
is believed.
A green replay that went over the cap under --allow-over-cap counts as
GREEN and says so with its seconds. Exit 0 only when every row listed is
GREEN and no listed job is one a replay cannot cover: name with --job the
jobs a replay stands in for.

A job with needs: (a summary job such as lint's and the trace check's reads
the run's own job list from GitHub) depends on other jobs' results, which
exist only in a real run: it CANNOT BE REPLAYED, and --rows and --coverage
say so for each one by name rather than leave it out.

This is a stand-in, not a CI run: the runner image, its load and its tools
are this host's. A green replay says the step commands pass here; only the
dispatched run says CI is green.
"""

import json
import os
import platform
import re
import shlex
import subprocess
import sys
import tempfile
import time


class ReplayError(Exception):
    pass


def indent(line):
    return len(line) - len(line.lstrip(" "))


def unquote(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        return text[1:-1]
    return text


def block(lines, start, parent_indent):
    """The lines of a | or > block scalar starting after lines[start - 1]."""
    body = []
    base = None
    i = start
    while i < len(lines):
        line = lines[i]
        if line.strip() and indent(line) <= parent_indent:
            break
        if base is None and line.strip():
            base = indent(line)
        body.append(line[base:] if base is not None and len(line) >= base else line.strip())
        i += 1
    while body and not body[-1].strip():
        body.pop()
    return body, i


def scalar(lines, i, key_indent, rest):
    """The value of a key whose text after the colon is rest, at lines[i]."""
    anchor = None
    if rest.startswith("&"):
        parts = rest.split(None, 1)
        anchor = parts[0][1:]
        rest = parts[1] if len(parts) > 1 else ""
    if rest in ("|", "|-", ">", ">-") or rest == "":
        body, _ = block(lines, i + 1, key_indent)
        value = " ".join(body) if rest.startswith(">") else "\n".join(body)
    else:
        value = unquote(rest)
    return value, anchor


def mapping(lines, i, key_indent):
    """A mapping of plain key: value lines indented under lines[i]."""
    out = {}
    j = i + 1
    while j < len(lines):
        line = lines[j]
        if not line.strip() or line.strip().startswith("#"):
            j += 1
            continue
        if indent(line) <= key_indent:
            break
        key, sep, value = line.strip().partition(":")
        if not sep:
            raise ReplayError("line %d: not a key: value line: %s" % (j + 1, line.strip()))
        if value.strip() in ("|", ">", "|-", ">-"):
            body, j = block(lines, j + 1, indent(line))
            out[key.strip()] = "\n".join(body)
            continue
        out[key.strip()] = unquote(value)
        j += 1
    return out


def defaults_run(lines, i, key_indent):
    """The run: mapping (shell, working-directory) of a defaults: key at lines[i]."""
    j = i + 1
    while j < len(lines):
        line = lines[j]
        if not line.strip() or line.strip().startswith("#"):
            j += 1
            continue
        if indent(line) <= key_indent:
            break
        if indent(line) == key_indent + 2 and line.strip() == "run:":
            out = mapping(lines, j, key_indent + 2)
            unknown = set(out) - {"shell", "working-directory"}
            if unknown:
                raise ReplayError("line %d: defaults: run: %s is not known to the replay" % (j + 1, sorted(unknown)))
            return out
        j += 1
    return {}


def job_keys(text):
    """Every job key under jobs:, in file order."""
    lines = text.split("\n")
    keys = []
    inside = False
    for line in lines:
        if line.strip() and not line.startswith(" ") and not line.startswith("#"):
            inside = line.rstrip() == "jobs:"
            continue
        match = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
        if inside and match:
            keys.append(match.group(1))
    return keys


def parse(text, job_key):
    lines = text.split("\n")
    anchors = {}
    workflow_env = {}
    workflow_defaults = {}
    job = None
    for i, line in enumerate(lines):
        if line.startswith("env:"):
            workflow_env = mapping(lines, i, 0)
        if line.startswith("defaults:"):
            workflow_defaults = defaults_run(lines, i, 0)
    for i, line in enumerate(lines):
        if line == "  %s:" % job_key:
            job = i
            break
    if job is None:
        raise ReplayError("no job %r under jobs:" % job_key)
    end = len(lines)
    for i in range(job + 1, len(lines)):
        if lines[i].strip() and not lines[i].strip().startswith("#") and indent(lines[i]) <= 2:
            end = i
            break
    info = {"name": None, "if": None, "needs": [], "timeout": None, "defaults": {}, "runs-on": None,
            "matrix": None, "matrix_keys": set()}
    strategy = None
    # every run: anchor in the file, for the aliases
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("run: &"):
            value, anchor = scalar(lines, i, indent(line), stripped[len("run:"):].strip())
            anchors[anchor] = value
    job_env = {}
    steps = []
    i = job + 1
    while i < end:
        line = lines[i]
        stripped = line.strip()
        if indent(line) == 4 and stripped.startswith("env:"):
            job_env = mapping(lines, i, 4)
        if indent(line) == 4 and not stripped.startswith("#"):
            key, sep, rest = stripped.partition(":")
            rest = rest.strip()
            if sep and key == "defaults":
                info["defaults"] = defaults_run(lines, i, 4)
            elif sep and key == "timeout-minutes":
                info["timeout"] = unquote(rest)
            elif sep and key == "if":
                info["if"] = unquote(rest)
            elif sep and key == "name":
                info["name"] = unquote(rest)
            elif sep and key == "runs-on":
                info["runs-on"] = rest
            elif sep and key == "needs":
                if rest:
                    value, _ = flow(rest, 0)
                    info["needs"] = value if isinstance(value, list) else [value]
                else:
                    needs = []
                    k = i + 1
                    while k < end and (not lines[k].strip() or indent(lines[k]) > 4):
                        if lines[k].strip().startswith("- "):
                            needs.append(unquote(lines[k].strip()[2:]))
                        k += 1
                    info["needs"] = needs
            elif sep and key == "strategy":
                strategy = i
        if stripped.startswith("- ") and indent(line) == 6:
            step = {"name": None, "run": None, "if": None, "env": {}, "shell": None, "uses": None, "wd": None, "id": None,
                    "timeout": None}
            # the step's keys: the first sits after "- ", the rest at indent 8
            j = i
            while j < end:
                current = lines[j]
                if j > i and current.strip() and indent(current) <= 6:
                    break
                body = current.strip()
                if j == i:
                    body = body[2:]
                elif indent(current) != 8:
                    j += 1
                    continue
                key, sep, rest = body.partition(":")
                rest = rest.strip()
                if sep:
                    if key == "name":
                        step["name"] = unquote(rest)
                    elif key == "if":
                        step["if"] = unquote(rest)
                    elif key == "shell":
                        step["shell"] = unquote(rest)
                    elif key == "timeout-minutes":
                        step["timeout"] = unquote(rest)
                    elif key == "uses":
                        step["uses"] = rest
                    elif key == "id":
                        step["id"] = unquote(rest)
                    elif key == "working-directory":
                        step["wd"] = unquote(rest)
                    elif key == "env":
                        step["env"] = mapping(lines, j, 8)
                    elif key == "run":
                        if rest.startswith("*"):
                            name = rest[1:].strip()
                            if name not in anchors:
                                raise ReplayError("line %d: alias *%s names no anchor" % (j + 1, name))
                            step["run"] = anchors[name]
                        else:
                            step["run"], _ = scalar(lines, j, 8, rest)
                j += 1
            if step["name"] is None:
                step["name"] = step["uses"] or (step["run"] or "").split("\n")[0]
            steps.append(step)
            i = j
            continue
        i += 1
    strategy_end = strategy
    if strategy is not None:
        strategy_end = strategy + 1
        while strategy_end < end and (not lines[strategy_end].strip() or indent(lines[strategy_end]) > 4):
            strategy_end += 1
        info["matrix"] = matrix_block(lines, strategy, strategy_end)
    for k in range(job + 1, end):
        if strategy is not None and strategy <= k < strategy_end:
            continue
        if lines[k].strip().startswith("runs-on:") or lines[k].strip().startswith("#"):
            continue
        # an if: is an expression whole; elsewhere only ${{ }} reads the matrix
        spans = [lines[k]] if re.match(r"^\s*(- )?if:", lines[k]) else re.findall(r"\$\{\{(.*?)\}\}", lines[k])
        for span in spans:
            info["matrix_keys"].update(re.findall(r"\bmatrix\.([A-Za-z_][A-Za-z0-9_-]*)", span))
    return {"workflow_env": workflow_env, "workflow_defaults": workflow_defaults, "job_env": job_env, "steps": steps,
            "job": info}


# --- the matrix --------------------------------------------------------------

def flow(text, pos):
    """A YAML flow value ({...}, [...], a quoted or plain scalar) at text[pos:]; returns (value, end)."""
    while pos < len(text) and text[pos] == " ":
        pos += 1
    if pos >= len(text):
        return None, pos
    head = text[pos]
    if head in "{[":
        close = "}" if head == "{" else "]"
        out = {} if head == "{" else []
        pos += 1
        while True:
            while pos < len(text) and text[pos] in " ,":
                pos += 1
            if pos >= len(text):
                raise ReplayError("unclosed %s in %r" % (head, text))
            if text[pos] == close:
                return out, pos + 1
            if head == "{":
                colon = text.find(":", pos)
                if colon < 0:
                    raise ReplayError("no key: in %r" % text)
                key = unquote(text[pos:colon])
                value, pos = flow(text, colon + 1)
                out[key] = value
            else:
                value, pos = flow(text, pos)
                out.append(value)
    if head in "'\"":
        end = pos + 1
        while end < len(text):
            if text[end] == head and head == "'" and text[end + 1:end + 2] == "'":
                end += 2
                continue
            if text[end] == "\\" and head == '"':
                end += 2
                continue
            if text[end] == head:
                break
            end += 1
        raw = text[pos + 1:end]
        value = raw.replace("''", "'") if head == "'" else json.loads('"%s"' % raw)
        return value, end + 1
    end = pos
    while end < len(text) and text[end] not in ",]}" and not text.startswith(" #", end):
        end += 1
    return plain(text[pos:end].strip()), end


def plain(word):
    if word in ("true", "false"):
        return word == "true"
    if word in ("null", "~", ""):
        return None
    if re.match(r"^-?\d+$", word):
        return int(word)
    return word


def matrix_value(lines, i, key_indent, rest):
    """The value of a matrix key: an expression, a flow value or a block list."""
    if rest.startswith("${{"):
        return rest
    if rest and not rest.startswith("#"):
        return flow(rest, 0)[0]
    items = []
    j = i + 1
    while j < len(lines) and (not lines[j].strip() or indent(lines[j]) > key_indent):
        stripped = lines[j].strip()
        if stripped.startswith("- "):
            body = stripped[2:].strip()
            if body.startswith("{") or body.startswith("["):
                items.append(flow(body, 0)[0])
            elif re.match(r"^[A-Za-z0-9_-]+:", body):
                item = {}
                item_indent = indent(lines[j]) + 2
                key, _, value = body.partition(":")
                item[key.strip()] = flow(value, 0)[0]
                k = j + 1
                while k < len(lines) and (not lines[k].strip() or indent(lines[k]) >= item_indent):
                    inner = lines[k].strip()
                    if inner and not inner.startswith("#"):
                        if indent(lines[k]) != item_indent or ":" not in inner:
                            raise ReplayError("line %d: matrix entry the replay cannot read: %s" % (k + 1, inner))
                        key, _, value = inner.partition(":")
                        item[key.strip()] = flow(value, 0)[0]
                    k += 1
                items.append(item)
                j = k
                continue
            else:
                items.append(flow(body, 0)[0])
        elif stripped and not stripped.startswith("#"):
            raise ReplayError("line %d: matrix entry the replay cannot read: %s" % (j + 1, stripped))
        j += 1
    return items


def matrix_block(lines, strategy, end):
    """strategy: matrix: as {key: value or ${{ expression }}}; None when the strategy has no matrix."""
    for i in range(strategy + 1, end):
        if lines[i].strip() == "matrix:":
            key_indent = indent(lines[i]) + 2
            out = {}
            j = i + 1
            while j < end:
                line = lines[j]
                stripped = line.strip()
                if stripped and not stripped.startswith("#"):
                    if indent(line) < key_indent:
                        break
                    if indent(line) == key_indent:
                        key, sep, rest = stripped.partition(":")
                        if not sep:
                            raise ReplayError("line %d: matrix entry the replay cannot read: %s" % (j + 1, stripped))
                        out[key.strip()] = matrix_value(lines, j, key_indent, rest.strip())
                j += 1
            return out
    return None


def text_of(value):
    if value is True:
        return "true"
    if value is False:
        return "false"
    if value is None:
        return ""
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def expand(matrix, context):
    """The matrix's rows, as GitHub expands them: the product, then exclude:, then include:."""
    resolved = {}
    for key, value in matrix.items():
        if isinstance(value, str) and value.startswith("${{"):
            value = evaluate(value.strip()[3:-2], context)
        resolved[key] = value
    base = [key for key in resolved if key not in ("include", "exclude")]
    rows = []
    if base:
        rows = [{}]
        for key in base:
            if not isinstance(resolved[key], list):
                raise ReplayError("matrix.%s is not a list" % key)
            rows = [dict(row, **{key: value}) for row in rows for value in resolved[key]]
    for entry in resolved.get("exclude") or []:
        rows = [row for row in rows
                if not all(key in row and text_of(row[key]) == text_of(value) for key, value in entry.items())]
    added = []
    for entry in resolved.get("include") or []:
        matched = False
        for row in rows:
            # an include joins every row whose own matrix values it agrees with
            if all(text_of(row[key]) == text_of(value) for key, value in entry.items() if key in base):
                row.update(entry)
                matched = True
        if not matched:
            added.append(dict(entry))
    return rows + added


# --- expressions ---------------------------------------------------------

TOKEN = re.compile(r"\s*(?:(?P<member>(?<=\))\.[A-Za-z_][A-Za-z0-9_\-]*)|(?P<str>'(?:[^']|'')*')|(?P<num>-?\d+(?:\.\d+)?)|(?P<op>==|!=|&&|\|\||<=|>=|[!()<>,])|(?P<id>[A-Za-z_][A-Za-z0-9_\-]*(?:\.[A-Za-z0-9_\-\*]+)*))")


def tokenize(text):
    tokens = []
    pos = 0
    text = text.strip()
    while pos < len(text):
        match = TOKEN.match(text, pos)
        if not match or match.end() == pos:
            raise ReplayError("cannot read the expression %r at %r" % (text, text[pos:]))
        pos = match.end()
        for kind in ("member", "str", "num", "op", "id"):
            if match.group(kind) is not None:
                tokens.append((kind, match.group(kind)))
                break
    return tokens


def truthy(value):
    return value not in (None, False, "", 0)


def loose(value):
    if isinstance(value, str):
        return value.lower()
    if value is True:
        return "true"
    if value is False:
        return "false"
    if value is None:
        return ""
    return str(value)


class Context:
    def __init__(self, sets, status="success"):
        self.sets = sets
        self.status = status
        # steps.<id>.outcome and .conclusion: success, failure or skipped for
        # a step with an id: that has been reached; a uses: step, which the
        # replay does not run, counts as success
        self.outcomes = {}

    def lookup(self, name):
        if name in ("true", "false", "null"):
            return {"true": True, "false": False, "null": None}[name]
        root = name.split(".")[0]
        if name in self.sets:
            value = self.sets[name]
            return {"true": True, "false": False}.get(value, value)
        if root == "matrix":
            raise ReplayError("%s is not set: pass --set %s=VALUE (the job's matrix row)" % (name, name))
        if root == "inputs":
            return None
        if root == "runner" and name == "runner.os":
            return {"Linux": "Linux", "Darwin": "macOS", "Windows": "Windows"}.get(platform.system(), platform.system())
        parts = name.split(".")
        if root == "steps" and len(parts) == 3 and parts[2] in ("outcome", "conclusion"):
            return self.outcomes.get(parts[1], "")
        if root in ("steps", "needs", "env", "vars", "secrets", "github", "runner", "job", "strategy"):
            return None
        raise ReplayError("unknown name %r in an expression" % name)


def evaluate(text, context):
    tokens = tokenize(text)
    pos = [0]

    def peek():
        return tokens[pos[0]] if pos[0] < len(tokens) else (None, None)

    def take(expected=None):
        token = peek()
        if token[0] is None or (expected and token[1] != expected):
            raise ReplayError("expression %r: expected %r" % (text, expected))
        pos[0] += 1
        return token

    def primary():
        kind, value = peek()
        if value == "!":
            take()
            return not truthy(primary())
        if value == "(":
            take()
            result = disjunction()
            take(")")
            return result
        if kind == "str":
            take()
            return value[1:-1].replace("''", "'")
        if kind == "num":
            take()
            return float(value) if "." in value else int(value)
        if kind == "id":
            take()
            if peek()[1] == "(":
                take("(")
                args = []
                while peek()[1] != ")":
                    args.append(disjunction())
                    if peek()[1] == ",":
                        take()
                take(")")
                result = call(value, args)
                # fromJSON(...).field: a member of the call's value
                while peek()[0] == "member":
                    field = take()[1][1:]
                    result = result.get(field) if isinstance(result, dict) else None
                return result
            return context.lookup(value)
        raise ReplayError("expression %r: unexpected %r" % (text, value))

    def call(name, args):
        if name == "success":
            return context.status == "success"
        if name == "failure":
            return context.status == "failure"
        if name == "always":
            return True
        if name == "cancelled":
            return False
        if name == "startsWith":
            return loose(args[0]).startswith(loose(args[1]))
        if name == "endsWith":
            return loose(args[0]).endswith(loose(args[1]))
        if name == "contains":
            return loose(args[1]) in loose(args[0])
        if name == "fromJSON":
            try:
                return json.loads(args[0])
            except (TypeError, ValueError) as error:
                raise ReplayError("expression %r: fromJSON(%r): %s" % (text, args[0], error))
        if name == "format":
            return re.sub(r"\{(\d+)\}", lambda m: "" if args[1 + int(m.group(1))] is None else str(args[1 + int(m.group(1))]), args[0])
        raise ReplayError("expression %r: function %s() is not known to the replay" % (text, name))

    def comparison():
        left = primary()
        while peek()[1] in ("==", "!=", "<", ">", "<=", ">="):
            op = take()[1]
            right = primary()
            if op == "==":
                left = loose(left) == loose(right)
            elif op == "!=":
                left = loose(left) != loose(right)
            else:
                left = {"<": left < right, ">": left > right, "<=": left <= right, ">=": left >= right}[op]
        return left

    def conjunction():
        left = comparison()
        while peek()[1] == "&&":
            take()
            right = comparison()
            left = right if truthy(left) else left
        return left

    def disjunction():
        left = conjunction()
        while peek()[1] == "||":
            take()
            right = conjunction()
            left = left if truthy(left) else right
        return left

    result = disjunction()
    if pos[0] != len(tokens):
        raise ReplayError("expression %r: trailing %r" % (text, tokens[pos[0]][1]))
    return result


def substitute(text, context):
    def one(match):
        value = evaluate(match.group(1), context)
        if value is True:
            return "true"
        if value is False:
            return "false"
        return "" if value is None else str(value)
    return re.sub(r"\$\{\{(.*?)\}\}", one, text)


def condition(step, context):
    text = step["if"]
    if text is None:
        return context.status == "success"
    text = text.strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2]
    uses_status = re.search(r"\b(success|failure|always|cancelled)\s*\(", text)
    result = truthy(evaluate(text, context))
    return result if uses_status else result and context.status == "success"


# --- running ---------------------------------------------------------------

def step_shell(step, parsed):
    """The shell: the step's own, else the job's defaults, else the workflow's; None is GitHub's default."""
    for shell in (step["shell"], parsed["job"]["defaults"].get("shell"), parsed["workflow_defaults"].get("shell")):
        if shell:
            return shell
    return None


def step_directory(step, parsed):
    for directory in (step["wd"], parsed["job"]["defaults"].get("working-directory"),
                      parsed["workflow_defaults"].get("working-directory")):
        if directory:
            return directory
    return None


def shell_command(shell, script_path, name="step"):
    """The command GitHub runs a run: step's script with, on Linux and macOS."""
    if shell is None:
        # GitHub's documented default for a run step with no shell anywhere
        return ["bash", "-e", script_path]
    if shell == "bash":
        return ["bash", "--noprofile", "--norc", "-eo", "pipefail", script_path]
    if shell == "sh":
        return ["sh", "-e", script_path]
    if shell == "python":
        return ["python3", script_path]
    if shell == "pwsh":
        return ["pwsh", "-command", ". '%s'" % script_path]
    words = shlex.split(shell)
    if not any("{0}" in word for word in words):
        raise ReplayError("step %r: shell %r is not known to the replay and names no {0}" % (name, shell))
    return [word.replace("{0}", script_path) for word in words]


def minutes(text, context, default):
    """A timeout-minutes value (a number or an expression), in seconds."""
    if text is None:
        return default * 60.0
    value = substitute(text, context) if "${{" in text else text
    try:
        return float(value) * 60.0
    except ValueError:
        raise ReplayError("timeout-minutes %r reads %r, not a number" % (text, value))


def git_blob(path):
    """The git blob id of the file at path as it is on disk, or None."""
    try:
        done = subprocess.run(["git", "hash-object", path], capture_output=True, text=True)
    except OSError:
        return None
    return done.stdout.strip() if done.returncode == 0 else None


def workflow_at_head(path):
    """The blob id of WORKFLOW as committed at HEAD here, or None when it is not a tracked file of this checkout."""
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    except OSError:
        return None
    if top.returncode != 0:
        return None
    relative = os.path.relpath(os.path.realpath(path), os.path.realpath(top.stdout.strip()))
    if relative.startswith(".."):
        return None
    done = subprocess.run(["git", "rev-parse", "HEAD:%s" % relative.replace(os.sep, "/")], capture_output=True,
                          text=True)
    return done.stdout.strip() if done.returncode == 0 else None


def git_state():
    """The commit at HEAD here and whether the tree is clean (untracked files count), or (None, False)."""
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True)
        status = subprocess.run(["git", "status", "--porcelain"], capture_output=True, text=True)
    except OSError:
        return None, False
    if head.returncode != 0 or status.returncode != 0:
        return None, False
    return head.stdout.strip(), status.stdout.strip() == ""


def checkout_state(workflow):
    """(commit, clean, workflow blob): clean only when the tree is clean and WORKFLOW is this checkout's own
    file as committed at HEAD, so a result's commit is the commit whose workflow and steps ran."""
    commit, clean = git_state()
    blob = git_blob(workflow)
    return commit, clean and blob is not None and blob == workflow_at_head(workflow), blob


def read_env_file(path):
    out = {}
    if not os.path.exists(path):
        return out
    lines = open(path).read().split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        if "<<" in line and "=" not in line.split("<<")[0]:
            key, delimiter = line.split("<<", 1)
            value = []
            i += 1
            while i < len(lines) and lines[i] != delimiter:
                value.append(lines[i])
                i += 1
            out[key] = "\n".join(value)
        elif "=" in line:
            key, value = line.split("=", 1)
            out[key] = value
        i += 1
    return out


def replay(workflow, job_key, sets, logs, list_only=False, out=sys.stdout, allow_over_cap=False, result=None,
           clock=time.monotonic, state=checkout_state):
    parsed = parse(open(workflow).read(), job_key)
    workflow_env, job_env, steps = parsed["workflow_env"], parsed["job_env"], parsed["steps"]
    context = Context(sets)
    # the commit and whether the tree is clean, read before any step writes to it
    commit, clean, blob = state(workflow) if result is not None and not list_only else (None, False, None)
    if result is not None and not list_only:
        # a result that cannot be written is found now, not after the steps have run
        try:
            os.makedirs(os.path.dirname(os.path.abspath(result)), exist_ok=True)
            open(result, "w").close()
        except OSError as error:
            raise ReplayError("--result %s cannot be written: %s" % (result, error))
    job_cap = minutes(parsed["job"]["timeout"], context, 360)
    total = 0.0
    over = []
    timings = []
    os.makedirs(logs, exist_ok=True)
    logs = os.path.abspath(logs)
    temp = os.path.join(logs, "runner-temp")
    os.makedirs(temp, exist_ok=True)
    github_env = os.path.join(logs, "github-env")
    github_path = os.path.join(logs, "github-path")
    for path in (github_env, github_path):
        open(path, "w").close()
    base = dict(os.environ)
    base.update({
        "CI": "true", "GITHUB_ACTIONS": "true", "GITHUB_WORKSPACE": os.getcwd(), "RUNNER_TEMP": temp,
        "RUNNER_NAME": base.get("RUNNER_NAME", "replay-" + platform.node()), "RUNNER_OS": context.lookup("runner.os"),
        "GITHUB_RUN_ID": base.get("GITHUB_RUN_ID", str(900000000 + os.getpid())), "GITHUB_RUN_ATTEMPT": "1",
        "GITHUB_ENV": github_env, "GITHUB_PATH": github_path, "GITHUB_OUTPUT": os.path.join(logs, "github-output"),
        "GITHUB_STEP_SUMMARY": os.path.join(logs, "step-summary.md"),
    })
    ran = 0
    if not list_only:
        out.write("replay: %s job %s at %s, logs in %s\n" % (workflow, job_key, os.getcwd(), logs))
    for number, step in enumerate(steps, 1):
        if step["run"] is None:
            out.write("-- not run (uses: %s): %s\n" % (step["uses"], step["name"]))
            if step["id"]:
                context.outcomes[step["id"]] = "success"
            continue
        go = condition(step, context)
        if step["id"]:
            context.outcomes[step["id"]] = "success" if go else "skipped"
        if list_only:
            out.write("%s %s\n" % ("RUN " if go else "skip", step["name"]))
            continue
        if not go:
            out.write("-- skipped by its if: %s\n" % step["name"])
            continue
        env = dict(base)
        for key, value in workflow_env.items():
            env[key] = substitute(value, context)
        for key, value in job_env.items():
            env[key] = substitute(value, context)
        env.update(read_env_file(github_env))
        extra_path = [p for p in open(github_path).read().split("\n") if p]
        if extra_path:
            env["PATH"] = os.pathsep.join(list(reversed(extra_path)) + [env.get("PATH", "")])
        for key, value in step["env"].items():
            env[key] = substitute(value, context)
        shell = step_shell(step, parsed)
        # GitHub names the script for its shell; pwsh dot-sources only a .ps1
        suffix = {"pwsh": ".ps1", "python": ".py"}.get(shell, ".sh")
        script = os.path.join(temp, "step-%d%s" % (number, suffix))
        with open(script, "w") as handle:
            handle.write(substitute(step["run"], context) + "\n")
        log_path = os.path.join(logs, "step-%d.log" % number)
        command = shell_command(shell, script, step["name"])
        step_cap = minutes(step["timeout"], context, 0) if step["timeout"] is not None else None
        out.write("== %s\n" % step["name"])
        out.flush()
        start = clock()
        with open(log_path, "w") as log:
            status = subprocess.run(command, cwd=step_directory(step, parsed), env=env, stdout=log,
                                    stderr=subprocess.STDOUT).returncode
        seconds = clock() - start
        total += seconds
        ran += 1
        cap_text = "" if step_cap is None else ", its cap %.0f s" % step_cap
        out.write("   exit %d after %.1f s%s (%s)\n" % (status, seconds, cap_text, log_path))
        timings.append({"name": step["name"], "exit": status, "seconds": round(seconds, 1), "cap": step_cap})
        if step_cap is not None and seconds > step_cap:
            over.append("step %r took %.1f s against its cap of %.0f s" % (step["name"], seconds, step_cap))
            out.write("   OVER CAP: %.1f s against the step's cap of %.0f s\n" % (seconds, step_cap))
        if status != 0:
            if step["id"]:
                context.outcomes[step["id"]] = "failure"
            context.status = "failure"
            tail = open(log_path, errors="replace").read().split("\n")[-8:]
            out.write("".join("   | %s\n" % line for line in tail))
            out.write("FAIL %s\n" % step["name"])
    if list_only:
        return 0
    if total > job_cap:
        over.insert(0, "the run steps took %.1f s against the job's cap of %.0f s" % (total, job_cap))
    green = context.status == "success"
    if not green:
        status = 1
    elif over and not allow_over_cap:
        status = 3
    else:
        status = 0
    for line in over:
        out.write("OVER CAP: %s\n" % line)
    if over and allow_over_cap:
        out.write("OVER CAP allowed by --allow-over-cap: this host may be slower than the runner; the runner "
                  "cancels a job at its cap\n")
    out.write("replay: %s job %s, %d step(s) run, %s, %.1f s against the job's cap of %.0f s%s "
              "(checkout, caches and runner setup not timed)\n"
              % (workflow, job_key, ran, "green" if green else "RED", total, job_cap,
                 ": OVER CAP" if total > job_cap else (": a step OVER CAP" if over else ", within the cap")))
    if result is not None:
        with open(result, "w") as handle:
            json.dump({"workflow": os.path.basename(workflow), "workflow_blob": blob, "job": job_key, "sets": sets,
                       "commit": commit,
                       "clean": clean, "host": platform.node(), "runner_os": Context({}).lookup("runner.os"),
                       "green": green, "seconds": round(total, 1),
                       "cap_seconds": job_cap, "over_cap": over, "allow_over_cap": allow_over_cap,
                       "exit": status, "steps": timings}, handle, indent=1, sort_keys=True)
            handle.write("\n")
        out.write("replay: result in %s (commit %s, tree and workflow %s at the start)\n"
                  % (result, commit or "unknown", "clean" if clean else "NOT CLEAN or not this checkout's"))
    return status


# --- rows and coverage -------------------------------------------------------

def workflow_rows(workflow, jobs, sets):
    """(rows, summaries, skipped): every replayable row the workflow runs, every summary job, every job not run.

    A row is {job, name, values (matrix key -> text, the keys the job's steps read), sets (the --set list)}.
    """
    text = open(workflow).read()
    keys = job_keys(text)
    for job in jobs:
        if job not in keys:
            raise ReplayError("no job %r under jobs: in %s" % (job, workflow))
    rows, summaries, skipped = [], [], []
    for job in jobs or keys:
        parsed = parse(text, job)
        info = parsed["job"]
        context = Context(dict(sets))
        if info["if"] is not None and not condition({"if": info["if"]}, context):
            skipped.append((job, info["name"] or job, info["if"]))
            continue
        if info["needs"]:
            summaries.append((job, info["name"] or job, info["needs"]))
            continue
        matrix_rows = expand(info["matrix"], context) if info["matrix"] else [{}]
        all_keys = set()
        for row in matrix_rows:
            all_keys.update(row)
        for row in matrix_rows:
            row_sets = dict(sets)
            for key in all_keys | info["matrix_keys"]:
                row_sets["matrix.%s" % key] = text_of(row.get(key))
            row_context = Context(row_sets)
            name = substitute(info["name"], row_context) if info["name"] else job
            if row and "matrix" not in (info["name"] or ""):
                # GitHub adds the row's values to a name that does not read the matrix
                name = "%s (%s)" % (name, ", ".join(text_of(value) for value in row.values()))
            # every value of the row (runs-on's included) and every one the job reads: a replay matches
            # a row only on all of them, so one replay never stands in for two rows
            values = {key: text_of(row.get(key)) for key in sorted(all_keys | info["matrix_keys"])}
            runs_on = info["runs-on"] or ""
            match = re.match(r"^\$\{\{(.*)\}\}$", runs_on)
            if match:
                try:
                    value = evaluate(match.group(1), row_context)
                    runs_on = ",".join(value) if isinstance(value, list) else text_of(value)
                except ReplayError:
                    pass
            rows.append({"job": job, "name": name, "values": values, "runs-on": runs_on,
                         "sets": ["matrix.%s=%s" % (key, value) for key, value in sorted(values.items())]})
    return rows, summaries, skipped


def cannot_replay(job, name, needs):
    return ("CANNOT REPLAY %s (job %s): it needs %s, whose results exist only in a real run (a summary job "
            "reads the run's job list); no replay covers it\n" % (name, job, ", ".join(needs)))


def row_os(runs_on):
    """The OS a row's runs-on names (Linux, macOS, Windows), or None when its labels do not say."""
    labels = runs_on.lower()
    for word, name in (("windows", "Windows"), ("macos", "macOS"), ("linux", "Linux"), ("ubuntu", "Linux")):
        if word in labels:
            return name
    return None


def print_rows(workflow, jobs, sets, out=sys.stdout):
    rows, summaries, skipped = workflow_rows(workflow, jobs, sets)
    tier = "nightly tier (inputs.full)" if truthy(Context(sets).lookup("inputs.full")) else "a commit"
    for row in rows:
        command = " ".join(["python3 tools/replay_ci_steps.py", workflow, row["job"]] +
                           ["--set %s" % shlex.quote(item) for item in row["sets"]])
        out.write("ROW %s\n    runs-on %s\n    %s\n" % (row["name"], row["runs-on"], command))
    for job, name, needs in summaries:
        out.write(cannot_replay(job, name, needs))
    for job, name, why in skipped:
        out.write("NOT RUN %s (job %s) on %s: if: %s\n" % (name, job, tier, why))
    out.write("rows: %s on %s: %d replayable row(s), %d summary job(s) that cannot be replayed, %d job(s) not run\n"
              % (workflow, tier, len(rows), len(summaries), len(skipped)))
    return 0


def read_results(paths):
    results = []
    for path in paths:
        files = [path]
        if os.path.isdir(path):
            files = sorted(os.path.join(path, name) for name in os.listdir(path) if name.endswith(".json"))
        for name in files:
            try:
                with open(name) as handle:
                    data = json.load(handle)
            except (OSError, ValueError) as error:
                raise ReplayError("result %s cannot be read: %s" % (name, error))
            data["file"] = name
            results.append(data)
    return results


def context_sets(sets):
    """The --set values that are not the matrix row, inputs.full read as true or not."""
    out = {}
    for key, value in sets.items():
        if key.startswith("matrix."):
            continue
        if key == "inputs.full":
            value = "true" if truthy(plain(value)) and value != "false" else ""
        if value != "":
            out[key] = value
    return out


def matches(row, result, workflow, commit, sets, blob):
    if result.get("job") != row["job"] or result.get("workflow") != os.path.basename(workflow):
        return False
    # the replay ran this very workflow file (the one the rows come from), committed at its commit
    if blob is None or result.get("workflow_blob") != blob:
        return False
    if not result.get("commit") or not commit or not result["commit"].startswith(commit):
        return False
    wanted = row_os(row["runs-on"])
    if wanted is not None and result.get("runner_os") != wanted:
        return False
    replayed = result.get("sets") or {}
    # the same context (inputs, github.*, runner.*) and the same value for every matrix key
    if context_sets(replayed) != context_sets(sets):
        return False
    matrix = dict((key[len("matrix."):], value) for key, value in replayed.items() if key.startswith("matrix."))
    for key in set(matrix) | set(row["values"]):
        if matrix.get(key, "") != row["values"].get(key, ""):
            return False
    return True


def coverage(workflow, jobs, sets, commit, paths, out=sys.stdout):
    if len(commit) < 7:
        raise ReplayError("--commit wants at least seven hex digits of the commit, got %r" % commit)
    rows, summaries, skipped = workflow_rows(workflow, jobs, sets)
    results = read_results(paths)
    blob = git_blob(workflow)
    if blob is None:
        raise ReplayError("git hash-object %s failed: coverage cannot tie results to this workflow" % workflow)
    counts = {"GREEN": 0, "RED": 0, "OVER CAP": 0, "NOT CLEAN": 0, "NONE": 0}
    for row in rows:
        found = [result for result in results if matches(row, result, workflow, commit, sets, blob)]
        verdict, detail = "NONE", "no replay result at %s" % commit
        for result in found:
            if not result.get("clean"):
                rank, text = "NOT CLEAN", "replayed from a tree that was not clean"
            elif not result.get("green"):
                rank, text = "RED", "a step was red"
            elif result.get("over_cap") and not result.get("allow_over_cap"):
                rank, text = "OVER CAP", "; ".join(result["over_cap"])
            else:
                rank, text = "GREEN", "%.1f s against the cap of %.0f s%s" % (
                    result.get("seconds", 0), result.get("cap_seconds", 0),
                    (": OVER CAP, allowed (%s)" % "; ".join(result["over_cap"])) if result.get("over_cap") else "")
            order = ["NONE", "NOT CLEAN", "RED", "OVER CAP", "GREEN"]
            if order.index(rank) > order.index(verdict):
                verdict, detail = rank, "%s (%s, on %s)" % (text, result["file"], result.get("host", "?"))
        counts[verdict] += 1
        out.write("%-9s %s: %s\n" % (verdict, row["name"], detail))
    for job, name, needs in summaries:
        out.write(cannot_replay(job, name, needs))
    out.write("coverage: %s at %s: %d row(s): %d green, %d red, %d over cap, %d not clean, %d with no replay; "
              "%d job(s) with needs: that no replay covers\n"
              % (workflow, commit, len(rows), counts["GREEN"], counts["RED"], counts["OVER CAP"], counts["NOT CLEAN"],
                 counts["NONE"], len(summaries)))
    # a job no replay can cover is not covered: name only the jobs a replay stands in for (--job)
    return 0 if rows and counts["GREEN"] == len(rows) and not summaries else 1


# --- self-test ---------------------------------------------------------------

FIXTURE = """\
name: fixture
env:
  WORKFLOW_LEVEL: workflow
jobs:
  other:
    runs-on: x
    steps:
      - name: Not this job
        run: exit 7
  demo:
    runs-on: x
    env:
      JOB_LEVEL: job
    steps:
      - uses: actions/checkout@v4
      - name: Only on the checks leg
        if: matrix.leg == 'checks'
        run: echo leg-ran > leg.txt
      - name: Only on the unit leg
        if: matrix.leg == 'unit'
        run: echo should-not > unit.txt
      - name: Write the environment on
        run: |
          echo "HANDED=on" >> "$GITHUB_ENV"
          echo "${{ format('{0}/{1}', runner.os, matrix.leg) }}" > row.txt
      - name: Read it back with every env level
        env:
          STEP_LEVEL: "step ${{ matrix.leg }}"
        run: echo "$HANDED $WORKFLOW_LEVEL $JOB_LEVEL $STEP_LEVEL" > envs.txt
      - name: The anchored block
        run: &shared |
          echo anchored >> anchor.txt
      - name: Its alias
        run: *shared
      - name: A member of a fromJSON() value
        if: contains(fromJSON(matrix.json).target, 'fuzz')
        run: echo "${{ fromJSON(matrix.json).target }} ${{ fromJSON(matrix.json).absent || 'none' }}" > member.txt
      - name: A green step with an id
        id: made
        run: echo made > made.txt
      - name: A red pipe stage under the default shell passes
        run: false | true
      - name: The same under shell bash is red
        id: red
        shell: bash
        run: false | true
      - name: Runs after a red step on the green id's outcome
        if: ${{ !cancelled() && steps.made.outcome == 'success' }}
        run: echo on-made > on-made.txt
      - name: Reads the red id's outcome
        if: ${{ !cancelled() && steps.red.conclusion == 'failure' }}
        run: echo on-red > on-red.txt
      - name: Not run on an outcome the red id does not have
        if: ${{ !cancelled() && steps.red.outcome == 'success' }}
        run: echo should-not > red-success.txt
      - name: Skipped after a red step
        run: echo should-not > after.txt
      - name: Runs after a red step when asked
        if: ${{ always() }}
        run: echo always > always.txt
"""


# The workflow's and a job's defaults: run: shell:, as ci.yml sets them.
SHELLS = """\
name: shells
defaults:
  run:
    shell: bash --noprofile --norc -euo pipefail {0}
jobs:
  workflow-default:
    runs-on: x
    steps:
      - name: A red first pipe stage
        run: false | cat
      - name: An unset variable
        if: always()
        run: echo "$NOT_SET_ANYWHERE"
      - name: A green step
        if: always()
        run: echo green > green.txt
  job-default:
    runs-on: x
    defaults:
      run:
        shell: bash
    steps:
      - name: An unset variable under the job's bash
        run: echo "${NOT_SET_ANYWHERE}unset-ok" > job-unset.txt
      - name: A red first pipe stage under the job's bash
        run: false | cat
  step-shell:
    runs-on: x
    steps:
      - name: The step's own sh
        shell: sh
        run: false | true
      - name: A template shell
        shell: bash --noprofile --norc -e {0}
        run: echo template > template.txt
      - name: A template with {0} inside a word
        shell: bash -c "source {0}"
        run: echo embedded > embedded.txt
"""

# The caps: a per-commit job of two minutes, a step with its own cap.
CAPS = """\
name: caps
jobs:
  capped:
    runs-on: x
    timeout-minutes: ${{ inputs.full && 7200 || 2 }}
    steps:
      - name: one
        run: "true"
      - name: two
        run: "true"
      - name: three
        run: "true"
  step-capped:
    runs-on: x
    steps:
      - name: within the job, past its own cap
        timeout-minutes: 1
        run: "true"
"""

# A split job: a matrix product, an exclude: expression, include: rows that join
# a row and rows that match none, a nightly-only job and a summary job.
ROWS = """\
name: rows
jobs:
  split:
    name: ${{ matrix.os }} / ${{ matrix.leg }}${{ matrix.shard && format(' ({0})', matrix.shard) || '' }}
    runs-on: ${{ fromJSON(matrix.runner) }}
    strategy:
      fail-fast: false
      matrix:
        os: [linux, macos]
        leg: [unit, checks]
        exclude: ${{ fromJSON(inputs.full && '[]' || '[{"os":"macos","leg":"checks"},{"os":"linux","leg":"unit"}]') }}
        include:
          - { os: linux, runner: '["self-hosted","Linux"]' }
          - { os: macos, runner: '["self-hosted","macOS"]' }
          - { os: linux, leg: checks, split: true }
          - { os: linux, leg: unit, shard: 0/2, runner: '["self-hosted","Linux"]' }
          - { os: linux, leg: unit, shard: 1/2, runner: '["self-hosted","Linux"]' }
    steps:
      - name: Leg
        if: matrix.leg == 'checks' && !matrix.split
        run: echo ${{ matrix.shard }}
  blocks:
    name: block (${{ matrix.leg }})
    runs-on: x
    strategy:
      matrix:
        include:
          - leg: a
            shard: 0/2
          - leg: b
            shard: none
    steps:
      - run: echo ${{ matrix.shard }}
  build:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest]
    steps:
      - run: "true"
  nightly:
    if: inputs.full
    runs-on: x
    steps:
      - run: "true"
  summary:
    name: split summary
    needs: [split, blocks]
    if: always()
    runs-on: x
    steps:
      - run: "true"
"""


def self_test():
    failed = 0
    with tempfile.TemporaryDirectory() as directory:
        workflow = os.path.join(directory, "fixture.yml")
        with open(workflow, "w") as handle:
            handle.write(FIXTURE)
        cwd = os.getcwd()
        os.chdir(directory)
        try:
            class Sink:
                text = ""

                def write(self, text):
                    self.text += text

                def flush(self):
                    pass

            sink = Sink()
            status = replay(workflow, "demo", {"matrix.leg": "checks", "matrix.json": "{\"target\": \"rocketnet_fuzz_x\"}"}, os.path.join(directory, "logs"), out=sink)

            def read(name):
                path = os.path.join(directory, name)
                return open(path).read().strip() if os.path.exists(path) else None

            runner_os = Context({}).lookup("runner.os")
            checks = [
                ("the red step makes the replay exit 1", status == 1),
                ("a step whose if: holds ran", read("leg.txt") == "leg-ran"),
                ("a step whose if: fails did not run", read("unit.txt") is None),
                ("format() and matrix.* substitute", read("row.txt") == "%s/checks" % runner_os),
                ("GITHUB_ENV, workflow, job and step env reach a step", read("envs.txt") == "on workflow job step checks"),
                ("an anchor and its alias both ran", read("anchor.txt") == "anchored\nanchored"),
                ("the default shell has no pipefail", "FAIL A red pipe stage" not in sink.text),
                ("shell: bash has pipefail", "FAIL The same under shell bash is red" in sink.text),
                ("a step after a red one is skipped", read("after.txt") is None),
                ("if: always() runs after a red one", read("always.txt") == "always"),
                ("fromJSON(...).field in if: and run:", read("member.txt") == "rocketnet_fuzz_x none"),
                ("steps.<id>.outcome reads a green step", read("on-made.txt") == "on-made"),
                ("steps.<id>.conclusion reads a red step", read("on-red.txt") == "on-red"),
                ("a red step's outcome is not success", read("red-success.txt") is None),
                ("uses: steps are named, not run", "not run (uses: actions/checkout@v4)" in sink.text),
                ("only the named job ran", "Not this job" not in sink.text),
            ]
            try:
                replay(workflow, "demo", {}, os.path.join(directory, "logs2"), out=Sink())
                checks.append(("an unset matrix value is an error", False))
            except ReplayError as error:
                checks.append(("an unset matrix value is an error", "matrix.leg is not set" in str(error)))
            try:
                evaluate("hashFiles('x')", Context({}))
                checks.append(("an unknown function is an error", False))
            except ReplayError:
                checks.append(("an unknown function is an error", True))
            try:
                parse(FIXTURE, "absent")
                checks.append(("an absent job is an error", False))
            except ReplayError:
                checks.append(("an absent job is an error", True))

            # (a) the shell the workflow would use
            shells = os.path.join(directory, "shells.yml")
            with open(shells, "w") as handle:
                handle.write(SHELLS)
            sink = Sink()
            status = replay(shells, "workflow-default", {}, os.path.join(directory, "logs-shells-1"), out=sink)
            checks += [
                ("shell: workflow defaults have pipefail", "FAIL A red first pipe stage" in sink.text),
                ("shell: workflow defaults have -u", "FAIL An unset variable" in sink.text),
                ("shell: workflow defaults, green step stays green", read("green.txt") == "green"),
                ("shell: workflow defaults, the replay is red", status == 1 and "RED" in sink.text),
            ]
            sink = Sink()
            status = replay(shells, "job-default", {}, os.path.join(directory, "logs-shells-2"), out=sink)
            checks += [
                ("shell: job defaults override the workflow's (no -u)", read("job-unset.txt") == "unset-ok"),
                ("shell: job defaults' bash has pipefail", "FAIL A red first pipe stage under" in sink.text),
            ]
            sink = Sink()
            status = replay(shells, "step-shell", {}, os.path.join(directory, "logs-shells-3"), out=sink)
            checks += [
                ("shell: a step's own sh overrides the workflow's", "FAIL The step's own sh" not in sink.text),
                ("shell: a {0} template runs the script", read("template.txt") == "template"),
                ("shell: {0} inside a word is the script", read("embedded.txt") == "embedded"),
            ]

            # (b) the job's cap and each step's, on a clock the test turns
            caps = os.path.join(directory, "caps.yml")
            with open(caps, "w") as handle:
                handle.write(CAPS)

            def clock(seconds):
                ticks = [0]

                def now():
                    ticks[0] += 1
                    return ticks[0] * seconds
                return now

            sink = Sink()
            status = replay(caps, "capped", {}, os.path.join(directory, "logs-caps-1"), out=sink, clock=clock(50))
            checks += [
                ("cap: run steps past the job's cap exit 3", status == 3),
                ("cap: says green and OVER CAP with the seconds",
                 "3 step(s) run, green, 150.0 s against the job's cap of 120 s: OVER CAP" in sink.text),
                ("cap: each step's seconds are printed", sink.text.count("exit 0 after 50.0 s") == 3),
            ]
            sink = Sink()
            status = replay(caps, "capped", {}, os.path.join(directory, "logs-caps-2"), out=sink, clock=clock(50),
                            allow_over_cap=True)
            checks += [
                ("cap: --allow-over-cap exits 0", status == 0),
                ("cap: --allow-over-cap still says OVER CAP",
                 "OVER CAP allowed by --allow-over-cap" in sink.text and ": OVER CAP" in sink.text),
            ]
            sink = Sink()
            status = replay(caps, "capped", {"inputs.full": "true"}, os.path.join(directory, "logs-caps-3"),
                            out=sink, clock=clock(50))
            checks.append(("cap: the nightly tier's cap holds the same steps",
                           status == 0 and "against the job's cap of 432000 s, within the cap" in sink.text))
            sink = Sink()
            status = replay(caps, "capped", {}, os.path.join(directory, "logs-caps-4"), out=sink, clock=clock(30))
            checks.append(("cap: within the cap exits 0", status == 0 and "OVER CAP" not in sink.text))
            sink = Sink()
            status = replay(caps, "step-capped", {}, os.path.join(directory, "logs-caps-5"), out=sink,
                            clock=clock(61))
            checks.append(("cap: a step past its own cap exits 3",
                           status == 3 and "61.0 s against the step's cap of 60 s" in sink.text))

            # (c) every row of a split job, and which rows a replay covers
            rows_path = os.path.join(directory, "rows.yml")
            with open(rows_path, "w") as handle:
                handle.write(ROWS)
            rows, summaries, skipped = workflow_rows(rows_path, [], {})
            names = [row["name"] for row in rows]
            linux = '["self-hosted","Linux"]'
            macos = '["self-hosted","macOS"]'
            checks += [
                ("rows: product, exclude, include joins and new rows",
                 names == ["linux / checks", "macos / unit", "linux / unit (0/2)", "linux / unit (1/2)",
                           "block (a)", "block (b)", "build (ubuntu-latest)", "build (macos-latest)"]),
                ("rows: an include joins the row it matches", rows[0]["values"] == {
                    "leg": "checks", "os": "linux", "runner": linux, "shard": "", "split": "true"}),
                ("rows: runs-on's matrix value is a row value", rows[1]["values"]["runner"] == macos),
                ("rows: a summary job cannot be replayed, by name",
                 summaries == [("summary", "split summary", ["split", "blocks"])]),
                ("rows: a nightly-only job is not run on a commit", [job for job, _, _ in skipped] == ["nightly"]),
            ]
            full_rows, _, full_skipped = workflow_rows(rows_path, [], {"inputs.full": "true"})
            checks.append(("rows: nightly tier, later includes overwrite added values",
                           [row["name"] for row in full_rows] == [
                               "linux / unit (1/2)", "linux / checks", "macos / unit", "macos / checks",
                               "block (a)", "block (b)", "build (ubuntu-latest)", "build (macos-latest)",
                               "nightly"] and not full_skipped))
            sink = Sink()
            print_rows(rows_path, [], {}, out=sink)
            checks.append(("rows: --rows names the summary job it cannot replay",
                           "CANNOT REPLAY split summary (job summary)" in sink.text))

            results = os.path.join(directory, "results")
            os.makedirs(results)
            sha = "abc1234" + "0" * 33
            rows_blob = git_blob(rows_path)

            def result(name, job, values, **fields):
                data = {"workflow": "rows.yml", "workflow_blob": rows_blob, "job": job, "commit": sha,
                        "clean": True, "green": True,
                        "runner_os": "Linux", "seconds": 10.0, "cap_seconds": 120.0, "over_cap": [],
                        "allow_over_cap": False,
                        "sets": dict(("matrix.%s" % key, value) for key, value in values.items())}
                data.update(fields)
                with open(os.path.join(results, name + ".json"), "w") as handle:
                    json.dump(data, handle)

            split = {"leg": "", "os": "", "shard": "", "split": "", "runner": linux}
            result("checks", "split", dict(split, leg="checks", os="linux", split="true"))
            result("checks-wrong-values", "split", dict(split, leg="checks", os="linux"), green=False)
            result("macos-unit", "split", dict(split, leg="unit", os="macos", runner=macos), runner_os="macOS",
                   over_cap=["150 s against 120 s"])
            result("unit-0", "split", dict(split, leg="unit", os="linux", shard="0/2"),
                   over_cap=["150 s against 120 s"], allow_over_cap=True)
            result("unit-1-other-commit", "split", dict(split, leg="unit", os="linux", shard="1/2"),
                   commit="def5678" + "0" * 33)
            result("block-a", "blocks", {"leg": "a", "shard": "0/2"}, green=False)
            result("block-b", "blocks", {"leg": "b", "shard": "none"}, clean=False)
            # one Linux replay with no matrix values must not stand in for either build row, nor one with
            # the macOS row's value run on Linux for the macOS row
            result("build-no-values", "build", {})
            result("build-ubuntu", "build", {"os": "ubuntu-latest"})
            result("build-macos-on-linux", "build", {"os": "macos-latest"})
            sink = Sink()
            status = coverage(rows_path, [], {}, "abc1234", [results], out=sink)
            lines = dict((line[10:].split(":")[0], line[:9].strip()) for line in sink.text.split("\n")
                         if line[:9].strip() in ("GREEN", "RED", "OVER CAP", "NOT CLEAN", "NONE"))
            checks += [
                ("coverage: each row's verdict", lines == {
                    "linux / checks": "GREEN", "macos / unit": "OVER CAP", "linux / unit (0/2)": "GREEN",
                    "linux / unit (1/2)": "NONE", "block (a)": "RED", "block (b)": "NOT CLEAN",
                    "build (ubuntu-latest)": "GREEN", "build (macos-latest)": "NONE"}),
                ("coverage: a row with no green replay exits 1", status == 1),
                ("coverage: names the summary job it cannot replay",
                 "CANNOT REPLAY split summary (job summary)" in sink.text),
            ]
            sink = Sink()
            status = coverage(rows_path, ["build"], {"github.event_name": "push"}, "abc1234", [results], out=sink)
            checks.append(("coverage: a replay under other --set values covers nothing",
                           status == 1 and "2 with no replay" in sink.text))
            sink = Sink()
            status = coverage(rows_path, ["build"], {"inputs.full": "false"}, "abc1234", [results], out=sink)
            checks.append(("coverage: inputs.full=false is the per-commit tier", "1 green" in sink.text))

            # a real replay's result file covers its row, at the commit it ran on
            ran = os.path.join(directory, "ran")
            os.makedirs(ran)
            for row in workflow_rows(rows_path, ["blocks"], {})[0]:
                sets = dict(item.split("=", 1) for item in row["sets"])
                replay(rows_path, "blocks", sets, os.path.join(directory, "logs-" + row["values"]["leg"]), out=Sink(),
                       result=os.path.join(ran, row["values"]["leg"] + ".json"),
                       state=lambda workflow: (sha, True, git_blob(workflow)))
            sink = Sink()
            status = coverage(rows_path, ["blocks"], {}, sha, [ran], out=sink)
            checks.append(("coverage: replay --result files cover their rows",
                           status == 0 and "2 row(s): 2 green" in sink.text))
            sink = Sink()
            status = coverage(rows_path, ["blocks"], {}, "def5678", [ran], out=sink)
            checks.append(("coverage: another commit's replays cover nothing",
                           status == 1 and "2 with no replay" in sink.text))
            # a replay of an edited copy of the workflow covers nothing, whatever checkout it ran in
            edited_dir = os.path.join(directory, "edited")
            os.makedirs(edited_dir)
            edited = os.path.join(edited_dir, "rows.yml")
            with open(edited, "w") as handle:
                handle.write(ROWS.replace("      - run: echo ${{ matrix.shard }}", "      - run: echo edited"))
            edited_results = os.path.join(directory, "edited-results")
            os.makedirs(edited_results)
            for row in workflow_rows(edited, ["blocks"], {})[0]:
                sets = dict(item.split("=", 1) for item in row["sets"])
                replay(edited, "blocks", sets, os.path.join(directory, "logs-e-" + row["values"]["leg"]), out=Sink(),
                       result=os.path.join(edited_results, row["values"]["leg"] + ".json"),
                       state=lambda workflow: (sha, True, git_blob(workflow)))
            sink = Sink()
            status = coverage(rows_path, ["blocks"], {}, sha, [edited_results], out=sink)
            checks.append(("coverage: a replay of an edited workflow covers nothing",
                           status == 1 and "2 with no replay" in sink.text))

            # clean means the tree is clean and WORKFLOW is the checkout's own file as committed
            repo = os.path.join(directory, "repo")
            os.makedirs(os.path.join(repo, "wf"))
            committed = os.path.join(repo, "wf", "rows.yml")
            with open(committed, "w") as handle:
                handle.write(ROWS)
            git = ["git", "-C", repo, "-c", "user.name=replay", "-c", "user.email=replay@invalid"]
            subprocess.run(git[:3] + ["init", "-q"], check=True)
            subprocess.run(git + ["add", "wf/rows.yml"], check=True)
            subprocess.run(git + ["commit", "-q", "-m", "fixture"], check=True)
            os.chdir(repo)
            clean_here = checkout_state(committed)[1]
            outside = checkout_state(rows_path)[1]
            with open(committed, "a") as handle:
                handle.write("# edited\n")
            edited_here = checkout_state(committed)[1]
            os.chdir(directory)
            checks += [
                ("clean: the checkout's own committed workflow is clean", clean_here is True),
                ("clean: a workflow outside the checkout is not", outside is False),
                ("clean: an edited workflow is not", edited_here is False),
            ]

            sink = Sink()
            status = coverage(rows_path, ["blocks", "summary"], {}, sha, [ran], out=sink)
            checks.append(("coverage: a job with needs: listed keeps the exit 1",
                           status == 1 and "2 row(s): 2 green" in sink.text and "CANNOT REPLAY" in sink.text))
        finally:
            os.chdir(cwd)
    for name, ok in checks:
        print("self-test %-54s %s" % (name, "ok" if ok else "FAIL"))
        if not ok:
            failed += 1
    if failed:
        print(sink.text)
        print("SELF-TEST FAILED %d" % failed)
        return 1
    print("SELF-TEST OK (%d cases)" % len(checks))
    return 0


def main(argv):
    args = argv[1:]
    if args == ["--self-test"]:
        return self_test()
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if args else 2
    sets = {}
    logs = None
    list_only = False
    allow_over_cap = False
    result = None
    mode = None
    commit = None
    jobs = []
    positional = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--set" and i + 1 < len(args):
            key, sep, value = args[i + 1].partition("=")
            if not sep:
                print("replay: --set wants NAME=VALUE, got %r" % args[i + 1])
                return 2
            sets[key] = value
            i += 2
        elif arg == "--logs" and i + 1 < len(args):
            logs = args[i + 1]
            i += 2
        elif arg == "--result" and i + 1 < len(args):
            result = args[i + 1]
            i += 2
        elif arg == "--commit" and i + 1 < len(args):
            commit = args[i + 1]
            i += 2
        elif arg == "--job" and i + 1 < len(args):
            jobs.append(args[i + 1])
            i += 2
        elif arg == "--list":
            list_only = True
            i += 1
        elif arg == "--allow-over-cap":
            allow_over_cap = True
            i += 1
        elif arg in ("--rows", "--coverage"):
            mode = arg
            i += 1
        elif arg.startswith("-"):
            print("replay: unknown flag %r" % arg)
            return 2
        else:
            positional.append(arg)
            i += 1
    try:
        if mode == "--rows":
            if len(positional) != 1:
                print("replay: --rows wants WORKFLOW")
                return 2
            return print_rows(positional[0], jobs, sets)
        if mode == "--coverage":
            if len(positional) < 2 or commit is None:
                print("replay: --coverage wants WORKFLOW --commit SHA RESULT...")
                return 2
            return coverage(positional[0], jobs, sets, commit, positional[1:])
        if len(positional) != 2 or jobs or commit:
            print("replay: want WORKFLOW JOB")
            return 2
        if logs is None:
            # outside the checkout, which CI's is clean of
            logs = tempfile.mkdtemp(prefix="replay-ci-steps-")
        return replay(positional[0], positional[1], sets, logs, list_only, allow_over_cap=allow_over_cap,
                      result=result)
    except ReplayError as error:
        print("replay: %s" % error)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
