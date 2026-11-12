#!/usr/bin/env python3
"""Run one job of a workflow's steps here, as the runner would, before asking for a dispatch.

    python3 tools/replay_ci_steps.py WORKFLOW JOB [--set NAME=VALUE ...] [--logs DIR] [--list]
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
- The shell is the runner's: `bash -e` when the step names none, `bash
  --noprofile --norc -eo pipefail` for `shell: bash`, `sh -e` for `shell: sh`.
  working-directory: is honoured.
- What a step writes to $GITHUB_ENV and $GITHUB_PATH reaches the steps after
  it. RUNNER_TEMP, GITHUB_WORKSPACE, GITHUB_RUN_ID and the rest are set to
  local stand-ins.
- YAML anchors on a step's run: (`run: &name |`) and their aliases
  (`run: *name`) resolve within the file.

`uses:` steps (checkout, caches, uploads) are not run and are listed as such.
Each run step prints its name, its exit status and its seconds; its output
goes to DIR/step-N.log (default a new temporary directory, outside the
checkout) and the tail of a red one is
printed. Exit 0 when every step that ran exited 0, else 1; 2 for a usage or
workflow error. --list prints the steps and whether each would run.

This is a stand-in, not a CI run: the runner image, its load and its tools
are this host's. A green replay says the step commands pass here; only the
dispatched run says CI is green.
"""

import json
import os
import platform
import re
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


def parse(text, job_key):
    lines = text.split("\n")
    anchors = {}
    workflow_env = {}
    job = None
    for i, line in enumerate(lines):
        if line.startswith("env:"):
            workflow_env = mapping(lines, i, 0)
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
        if stripped.startswith("- ") and indent(line) == 6:
            step = {"name": None, "run": None, "if": None, "env": {}, "shell": None, "uses": None, "wd": None, "id": None}
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
                        step["shell"] = rest
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
    return workflow_env, job_env, steps


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

def shell_command(step, script_path):
    shell = step["shell"]
    if shell is None:
        return ["bash", "-e", script_path]
    if shell == "bash":
        return ["bash", "--noprofile", "--norc", "-eo", "pipefail", script_path]
    if shell == "sh":
        return ["sh", "-e", script_path]
    if shell == "python":
        return ["python3", script_path]
    raise ReplayError("step %r: shell %r is not known to the replay" % (step["name"], shell))


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


def replay(workflow, job_key, sets, logs, list_only=False, out=sys.stdout):
    workflow_env, job_env, steps = parse(open(workflow).read(), job_key)
    context = Context(sets)
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
        script = os.path.join(temp, "step-%d.sh" % number)
        with open(script, "w") as handle:
            handle.write(substitute(step["run"], context) + "\n")
        log_path = os.path.join(logs, "step-%d.log" % number)
        out.write("== %s\n" % step["name"])
        out.flush()
        start = time.monotonic()
        with open(log_path, "w") as log:
            status = subprocess.run(shell_command(step, script), cwd=step["wd"] or None, env=env, stdout=log,
                                    stderr=subprocess.STDOUT).returncode
        ran += 1
        out.write("   exit %d after %.1f s (%s)\n" % (status, time.monotonic() - start, log_path))
        if status != 0:
            if step["id"]:
                context.outcomes[step["id"]] = "failure"
            context.status = "failure"
            tail = open(log_path, errors="replace").read().split("\n")[-8:]
            out.write("".join("   | %s\n" % line for line in tail))
            out.write("FAIL %s\n" % step["name"])
    if list_only:
        return 0
    out.write("replay: %s job %s, %d step(s) run, %s\n" % (workflow, job_key, ran, "RED" if context.status != "success" else "green"))
    return 0 if context.status == "success" else 1


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
        elif arg == "--list":
            list_only = True
            i += 1
        elif arg.startswith("-"):
            print("replay: unknown flag %r" % arg)
            return 2
        else:
            positional.append(arg)
            i += 1
    if len(positional) != 2:
        print("replay: want WORKFLOW JOB")
        return 2
    if logs is None:
        # outside the checkout, which CI's is clean of
        logs = tempfile.mkdtemp(prefix="replay-ci-steps-")
    try:
        return replay(positional[0], positional[1], sets, logs, list_only)
    except ReplayError as error:
        print("replay: %s" % error)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
