#!/usr/bin/env python3
"""Judge a commit against tools/release_gates.txt through the GitHub Actions API.

The library behind tools/release.py (before a tag), tools/check_release_tsan.py
(Release Check's second check, on the tag push) and tools/land.py (before a
merge). One rule for every gate: not-green is red. A run or job that is
cancelled, timed out, skipped without a `skip` row, missing, queued, in
progress or failed refuses, by name. When a workflow ran more than once on the
commit, the newest completed run decides (its newest attempt), the output says
so, and a newer run that has not finished refuses: its verdict is not in.

The API is injected: every function takes an object with get(path, params) ->
decoded JSON (raising NotFound on 404), and the tools add post/put. Api below
is the real one (urllib, token from GITHUB_TOKEN or GH_TOKEN, else `gh auth
token`, which honours GH_CONFIG_DIR); the tests use fixtures and no network.
"""

import base64
import datetime
import json
import os
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GATES_FILE = ROOT / "tools" / "release_gates.txt"
RULESET_PATH = ".github/rulesets/main.json"
REPOSITORY = "mas-bandwidth/rocketnet"
MAIN = "main"

KINDS = ("run", "run-if-ran", "jobs", "jobs-if-ran", "job", "skip", "full", "tlc-rerun", "tag")
EVENTS = ("push", "pull_request", "nightly", "tag")
VERSION = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z][0-9A-Za-z.-]*))?$")
SLICE = re.compile(r"^TLC re-run slice (\d+)/(\d+)$")
FULL_RERUN = "TLC re-run full"
TLC_LOOKBACK_DAYS = 31


class GatesError(Exception):
    pass


class ApiError(Exception):
    pass


class NotFound(ApiError):
    pass


# ---------------------------------------------------------------- versions

def version_key(text):
    m = VERSION.match(text)
    if not m:
        raise GatesError(f"{text!r} is not a version vMAJOR.MINOR.PATCH[-PRERELEASE]")
    numbers = tuple(int(m.group(i)) for i in (1, 2, 3))
    if m.group(4) is None:
        return numbers + (1, ())
    parts = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in re.split(r"[.-]", m.group(4)))
    return numbers + (0, parts)


# From v1.1.8 a point release (PATCH not 0) is gated by the per-commit tier
# and the tag run only; the nightly-tier rows gate a minor or major release
# (owner, 2026-11-13: "I want each point release to have fast tests only").
POINT_RELEASE_RULE_FROM = "v1.1.8"


def is_point_release(version):
    """True for vX.Y.Z with Z > 0 and no pre-release suffix, from
    POINT_RELEASE_RULE_FROM on; a pre-release is judged by every tier."""
    key = version_key(version)
    return key[2] > 0 and key[3] == 1 and key >= version_key(POINT_RELEASE_RULE_FROM)


# ---------------------------------------------------------------- the list

class Gate:

    def __init__(self, workflow, job, kind, since, events, reason, line):
        self.workflow = workflow
        self.job = job
        self.kind = kind
        self.since = since
        self.events = events
        self.reason = reason
        self.line = line

    def label(self):
        job = "" if self.job == "-" else f" '{self.job}'"
        return f"{self.workflow}{job} [{self.kind}, {','.join(self.events)}, from {self.since}]"

    def in_force(self, version):
        return version_key(version) >= version_key(self.since)


def parse_gates(text, name="release_gates.txt"):
    gates, seen = [], set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split(" | ", 5)]
        if len(fields) != 6 or not all(fields):
            raise GatesError(f"{name}:{number}: expected 'workflow | job | kind | from | events | reason'")
        workflow, job, kind, since, events, reason = fields
        if not workflow.endswith((".yml", ".yaml")) or "/" in workflow:
            raise GatesError(f"{name}:{number}: workflow {workflow!r} is not a file name under .github/workflows")
        if kind not in KINDS:
            raise GatesError(f"{name}:{number}: kind {kind!r} is not one of {', '.join(KINDS)}")
        version_key(since)
        event_list = tuple(e.strip() for e in events.split(","))
        if not event_list or any(e not in EVENTS for e in event_list):
            raise GatesError(f"{name}:{number}: events {events!r} are not from {', '.join(EVENTS)}")
        if (job == "-") != (kind in ("run", "run-if-ran", "jobs", "jobs-if-ran")):
            raise GatesError(f"{name}:{number}: kind {kind} {'takes' if job == '-' else 'takes no'} job name")
        if (kind == "tag") != (event_list == ("tag",)):
            raise GatesError(f"{name}:{number}: a tag gate, and only a tag gate, has the event 'tag'")
        key = (workflow, job, kind, event_list)
        if key in seen:
            raise GatesError(f"{name}:{number}: duplicate gate")
        seen.add(key)
        gates.append(Gate(workflow, job, kind, since, event_list, reason, number))
    if not gates:
        raise GatesError(f"{name}: no gates: an empty list would pass everything")
    return gates


def load_gates(path=GATES_FILE):
    return parse_gates(Path(path).read_text(encoding="utf-8"), Path(path).name)


# ---------------------------------------------------------------- the API

class Api:
    """GitHub REST over urllib. get/post/put return decoded JSON; 404 raises NotFound."""

    def __init__(self, token, base="https://api.github.com"):
        self.token = token
        self.base = base.rstrip("/")

    def _send(self, method, path, params=None, body=None):
        url = self.base + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(url, data=data, method=method, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": "Bearer " + self.token,
            "X-GitHub-Api-Version": "2022-11-28",
        })
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                raw = response.read()
                return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as error:
            if error.code == 404:
                raise NotFound(f"{method} {path}: 404") from error
            raise ApiError(f"{method} {path}: HTTP {error.code} {error.read()[:300]!r}") from error
        except (urllib.error.URLError, ValueError, OSError) as error:
            raise ApiError(f"{method} {path}: {error}") from error

    def get(self, path, params=None):
        return self._send("GET", path, params)

    def post(self, path, body):
        return self._send("POST", path, body=body)

    def put(self, path, body):
        return self._send("PUT", path, body=body)


def token_from_environment():
    for name in ("GITHUB_TOKEN", "GH_TOKEN"):
        if os.environ.get(name):
            return os.environ[name]
    try:
        out = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, timeout=30, check=False)
    except OSError as error:
        raise ApiError(f"no GITHUB_TOKEN and `gh auth token` could not run: {error}") from error
    if out.returncode != 0 or not out.stdout.strip():
        raise ApiError("no GITHUB_TOKEN and `gh auth token` gave no token (set GH_CONFIG_DIR?)")
    return out.stdout.strip()


def paged(api, path, key, params=None, max_pages=30):
    items = []
    for page in range(1, max_pages + 1):
        answer = api.get(path, dict(params or {}, per_page=100, page=page))
        batch = answer.get(key)
        if not isinstance(batch, list):
            raise ApiError(f"GET {path}: no '{key}' list in the answer")
        items.extend(batch)
        if len(batch) < 100:
            return items
    raise ApiError(f"GET {path}: more than {max_pages} pages")


def file_at(api, repo, path, ref):
    """Text of a file at a ref, or None when the file is not there."""
    try:
        answer = api.get(f"/repos/{repo}/contents/{path}", {"ref": ref})
    except NotFound:
        return None
    if answer.get("encoding") != "base64" or "content" not in answer:
        raise ApiError(f"contents {path}@{ref}: not a base64 file")
    return base64.b64decode(answer["content"]).decode("utf-8")


def resolve(api, repo, ref):
    return api.get(f"/repos/{repo}/commits/{ref}")["sha"]


class Ancestry:
    """Cached 'is base an ancestor of head' through the compare API."""

    def __init__(self, api, repo):
        self.api, self.repo, self.cache = api, repo, {}

    def __call__(self, base, head):
        if base == head:
            return True
        if (base, head) not in self.cache:
            answer = self.api.get(f"/repos/{self.repo}/compare/{base}...{head}")
            self.cache[(base, head)] = answer.get("status") in ("ahead", "identical")
        return self.cache[(base, head)]


def shipping_version(header):
    """ROCKETNET_VERSION_FULL of the shipping (#if !ROCKETNET_DEV) branch of sdk/rocketnet.h."""
    m = re.search(r"^#if !ROCKETNET_DEV\b(.*?)^#else", header, re.S | re.M)
    if not m:
        return None
    full = re.search(r'#define\s+ROCKETNET_VERSION_FULL\s+"([^"]+)"', m.group(1))
    return full.group(1) if full else None


def required_contexts(api, repo, ref):
    """The required check names of .github/rulesets/main.json at ref, or None when absent."""
    text = file_at(api, repo, RULESET_PATH, ref)
    if text is None:
        return None
    return contexts_of(json.loads(text))


def contexts_of(ruleset):
    names = []
    for rule in ruleset.get("rules", []):
        if rule.get("type") == "required_status_checks":
            names += [c["context"] for c in rule["parameters"]["required_status_checks"]]
    return names


# ---------------------------------------------------------------- runs

def parse_time(text):
    if not text:
        return None
    return datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)


def seconds(job):
    start, end = parse_time(job.get("started_at")), parse_time(job.get("completed_at"))
    if start is None or end is None:
        return None
    return int((end - start).total_seconds())


def run_matches(run, sha, event, version=None):
    if run.get("head_sha") != sha:
        return False
    kind = run.get("event")
    if event == "push":
        return kind == "push" and run.get("head_branch") == MAIN
    if event == "pull_request":
        return kind == "pull_request"
    if event == "nightly":
        return kind in ("schedule", "workflow_dispatch")
    if event == "tag":
        return kind == "push" and run.get("head_branch") == version
    raise GatesError(f"unknown event {event}")


def newest_first(runs):
    return sorted(runs, key=lambda r: (r.get("run_started_at") or r.get("created_at") or "", r["id"]), reverse=True)


class Context:
    """One judgement's inputs and caches."""

    def __init__(self, api, repo, sha, version=None, now=None):
        self.api = api
        self.repo = repo
        self.sha = sha
        self.version = version
        self.now = now or datetime.datetime.now(datetime.timezone.utc)
        self.ancestor = Ancestry(api, repo)
        self._runs = {}
        self._jobs = {}

    def runs(self, workflow, event):
        if workflow not in self._runs:
            self._runs[workflow] = paged(self.api, f"/repos/{self.repo}/actions/workflows/{workflow}/runs",
                                         "workflow_runs", {"head_sha": self.sha})
        return newest_first([r for r in self._runs[workflow] if run_matches(r, self.sha, event, self.version)])

    def jobs(self, run_id, refresh=False):
        if refresh or run_id not in self._jobs:
            self._jobs[run_id] = paged(self.api, f"/repos/{self.repo}/actions/runs/{run_id}/jobs", "jobs",
                                       {"filter": "latest"})
        return self._jobs[run_id]

    def forget(self):
        self._runs.clear()
        self._jobs.clear()


def describe(run):
    attempt = run.get("run_attempt", 1)
    return f"run {run['id']} (attempt {attempt}, {run.get('event')}, {run.get('head_branch')})"


class Verdict:

    def __init__(self, gate, ok, detail, red=(), runs=()):
        self.gate = gate
        self.ok = ok
        self.detail = detail
        self.red = list(red)       # [(job name, conclusion, seconds, run)] for the landing tool
        self.runs = list(runs)     # runs this verdict read

    def line(self):
        return f"{'PASS' if self.ok else 'FAIL'}  {self.gate if isinstance(self.gate, str) else self.gate.label()}: {self.detail}"


def pick(runs):
    """(deciding completed run, newer unfinished runs, total)."""
    unfinished = []
    for run in runs:
        if run.get("status") == "completed":
            return run, unfinished, len(runs)
        unfinished.append(run)
    return None, unfinished, len(runs)


def deciding_run(ctx, gate, event):
    """(run or None, red Verdict or None, note)."""
    runs = ctx.runs(gate.workflow, event)
    if not runs:
        if gate.kind.endswith("-if-ran"):
            return None, Verdict(gate, True, f"did not run on {ctx.sha[:10]} ({event}); judged only when it runs"), ""
        return None, Verdict(gate, False, f"MISSING: no {event} run of {gate.workflow} on {ctx.sha[:10]}"), ""
    run, unfinished, total = pick(runs)
    if unfinished:
        names = ", ".join(f"{describe(r)} {r.get('status')}" for r in unfinished)
        return None, Verdict(gate, False, f"IN PROGRESS: {names} (newer than any finished run)", runs=unfinished), ""
    note = f" (newest completed of {total} runs decides)" if total > 1 else ""
    return run, None, note


def judge_run(ctx, gate, event, light_jobs=False):
    run, red, note = deciding_run(ctx, gate, event)
    if red:
        return red
    conclusion = run.get("conclusion")
    ok = conclusion == "success"
    if not ok and light_jobs:
        # a red FULL job turns the whole run red; the workflow's jobs gate
        # judges it job by job, holding only the LIGHT jobs
        return Verdict(gate, True, f"{describe(run)}{note}: {conclusion}; light: judged job by job by its jobs gate",
                       runs=[run])
    return Verdict(gate, ok, f"{describe(run)}{note}: {conclusion}", runs=[run])


def judge_jobs(ctx, gate, event, skips, full=frozenset()):
    run, red, note = deciding_run(ctx, gate, event)
    if red:
        return red
    jobs = ctx.jobs(run["id"])
    if not jobs:
        return Verdict(gate, False, f"{describe(run)}{note}: ZERO jobs reported", runs=[run])
    good, skipped, bad, full_red = 0, 0, [], []
    for job in jobs:
        name = job.get("name", "")
        conclusion = job.get("conclusion") if job.get("status") == "completed" else job.get("status")
        if conclusion == "success":
            good += 1
        elif conclusion == "skipped" and name in skips:
            skipped += 1
        elif name in full:
            full_red.append((name, conclusion, seconds(job)))
        else:
            if conclusion == "skipped":
                conclusion = "skipped (no skip row in tools/release_gates.txt)"
            bad.append((name, conclusion, seconds(job), run))
    detail = f"{describe(run)}{note}: {good} success, {skipped} skipped as listed"
    if full:
        detail += f"; light: {len(full)} FULL jobs do not gate"
    if full_red:
        detail += ("; FULL NOT GREEN (does not gate; record it for the next point release): "
                   + "; ".join(f"{n}: {c}" + (f" ({s} s)" if s is not None else "") for n, c, s in full_red))
    if bad:
        detail += "; RED: " + "; ".join(f"{n}: {c}" + (f" ({s} s)" if s is not None else "") for n, c, s, _ in bad)
    return Verdict(gate, not bad, detail, red=bad, runs=[run])


def judge_job(ctx, gate, event):
    runs = ctx.runs(gate.workflow, event)
    holding = []
    for run in runs:
        wanted = gate.job.replace("<ref>", str(run.get("head_branch")))
        if run.get("status") != "completed":
            holding.append((run, wanted, None))
            continue
        job = next((j for j in ctx.jobs(run["id"]) if j.get("name") == wanted), None)
        if job is not None:
            holding.append((run, wanted, job))
            break
    if not holding:
        return Verdict(gate, False, f"MISSING: no {event} run of {gate.workflow} on {ctx.sha[:10]} has job "
                                    f"'{gate.job}' ({len(runs)} runs read)")
    unfinished = [r for r, _, j in holding if j is None]
    run, wanted, job = holding[-1]
    if job is None:
        return Verdict(gate, False, f"IN PROGRESS: {', '.join(describe(r) for r in unfinished)}; no finished run has it")
    if unfinished:
        return Verdict(gate, False, f"IN PROGRESS: {', '.join(describe(r) for r in unfinished)} is newer than "
                                    f"{describe(run)}")
    conclusion = job.get("conclusion")
    total = len(runs)
    note = f" (newest completed run with the job, of {total} runs)" if total > 1 else ""
    red = [] if conclusion == "success" else [(wanted, conclusion, seconds(job), run)]
    return Verdict(gate, conclusion == "success", f"{describe(run)}{note}: '{wanted}': {conclusion}", red=red,
                   runs=[run])


def judge_tlc_rerun(ctx, gate):
    # 1. a full re-run on the exact commit
    for run in ctx.runs(gate.workflow, "nightly"):
        if run.get("status") != "completed":
            continue
        job = next((j for j in ctx.jobs(run["id"]) if j.get("name") == FULL_RERUN), None)
        if job is not None:
            if job.get("conclusion") == "success":
                return Verdict(gate, True, f"{describe(run)}: '{FULL_RERUN}': success on the exact commit")
            full_red = f"'{FULL_RERUN}' in {describe(run)}: {job.get('conclusion')}"
            break
    else:
        full_red = f"no '{FULL_RERUN}' job on {ctx.sha[:10]}"
    # 2. every slice of the last N days, on ancestors of the commit
    since = (ctx.now - datetime.timedelta(days=TLC_LOOKBACK_DAYS)).strftime("%Y-%m-%d")
    scheduled = paged(ctx.api, f"/repos/{ctx.repo}/actions/workflows/{gate.workflow}/runs", "workflow_runs",
                      {"event": "schedule", "created": f">={since}"})
    slices = {}   # n -> k -> (run, created)
    for run in newest_first(scheduled):
        if run.get("status") != "completed" or run.get("event") != "schedule":
            continue
        created = parse_time(run.get("created_at"))
        for job in ctx.jobs(run["id"]):
            m = SLICE.match(job.get("name", ""))
            if not m or job.get("conclusion") != "success":
                continue
            k, n = int(m.group(1)), int(m.group(2))
            if n <= 0 or k >= n or created is None or created < ctx.now - datetime.timedelta(days=n):
                continue
            if k in slices.setdefault(n, {}):
                continue
            if ctx.ancestor(run["head_sha"], ctx.sha):
                slices[n][k] = (run, created)
    best = None
    for n, found in sorted(slices.items()):
        missing = [k for k in range(n) if k not in found]
        if not missing:
            runs = ", ".join(f"{k}/{n} run {found[k][0]['id']}" for k in range(n))
            return Verdict(gate, True, f"every slice of the last {n} days succeeded on an ancestor: {runs}")
        if best is None or len(missing) < len(best[1]):
            best = (n, missing)
    if best is None:
        return Verdict(gate, False, f"{full_red}; no successful 'TLC re-run slice K/N' job in scheduled runs of "
                                    f"the last {TLC_LOOKBACK_DAYS} days on an ancestor of {ctx.sha[:10]}")
    n, missing = best
    return Verdict(gate, False, f"{full_red}; slices of N={n} missing in the last {n} days on ancestors: "
                                f"{', '.join(f'{k}/{n}' for k in missing)}")


def judge_required(ctx, names, event, workflows, label):
    """Every required name appears as a job of the deciding per-commit runs."""
    seen = set()
    for workflow in workflows:
        run, _, _ = pick(ctx.runs(workflow, event))
        if run is not None:
            seen.update(j.get("name") for j in ctx.jobs(run["id"]))
    missing = [n for n in names if n not in seen]
    detail = f"{len(names)} required job names; " + (f"MISSING: {'; '.join(missing)}" if missing else "all present")
    return Verdict(label, not missing, detail, red=[(n, "missing", None, None) for n in missing])


def evaluate(ctx, gates, events, version=None, contexts=None, contexts_label="", light=False):
    """[Verdict] for every gate with one of events, in force for version (None: all in force).
    Also returns the gates left out as not in force. light: the jobs `full` rows in
    force name are printed with their result but do not gate (a point release and
    its landing); otherwise they gate like every other job."""
    verdicts, not_in_force = [], []
    for event in events:
        rows = [g for g in gates if event in g.events]
        active = [g for g in rows if version is None or g.in_force(version)]
        not_in_force += [g for g in rows if g not in active and g not in not_in_force]
        skips, full = {}, {}
        for g in active:
            if g.kind == "skip":
                skips.setdefault(g.workflow, set()).add(g.job)
            elif g.kind == "full" and light:
                full.setdefault(g.workflow, set()).add(g.job)
        judged = {g.workflow for g in active if g.kind in ("jobs", "jobs-if-ran")}
        jobs_workflows = []
        for g in active:
            if g.kind in ("run", "run-if-ran"):
                verdicts.append(judge_run(ctx, g, event, g.workflow in full and g.workflow in judged))
            elif g.kind in ("jobs", "jobs-if-ran"):
                verdicts.append(judge_jobs(ctx, g, event, skips.get(g.workflow, set()),
                                           frozenset(full.get(g.workflow, ()))))
                jobs_workflows.append(g.workflow)
            elif g.kind == "job":
                verdicts.append(judge_job(ctx, g, event))
            elif g.kind == "tlc-rerun":
                verdicts.append(judge_tlc_rerun(ctx, g))
            elif g.kind == "tag":
                verdicts.append(judge_job(ctx, g, "tag"))
        if contexts is not None and jobs_workflows and event in ("push", "pull_request"):
            verdicts.append(judge_required(ctx, contexts, event, jobs_workflows,
                                           f"required job names ({contexts_label}) [{event}]"))
    return verdicts, not_in_force


# ---------------------------------------------------------------- a release commit

def judge_release(api, repo, version, ref, out, stage="pre", gates=None, now=None):
    """Print each precondition and gate; return (ok, sha, Context).

    stage "pre": before the tag (tools/release.py): the tag must not exist elsewhere.
    stage "tag": on the tag push (tools/check_release_tsan.py): the tag gates are
    this run's own jobs and are not judged."""
    gates = gates if gates is not None else load_gates()
    red = []

    def say(ok, text):
        print(f"{'PASS' if ok else 'FAIL'}  {text}", file=out)
        if not ok:
            red.append(text)

    if not VERSION.match(version):
        say(False, f"version {version!r} is not vMAJOR.MINOR.PATCH[-PRERELEASE]")
        return False, None, None
    try:
        sha = resolve(api, repo, ref)
    except NotFound:
        say(False, f"commit {ref} does not exist in {repo}")
        return False, None, None
    print(f"release {version} at {sha} ({repo})", file=out)
    ctx = Context(api, repo, sha, version, now)

    main_sha = resolve(api, repo, MAIN)
    say(ctx.ancestor(sha, main_sha), f"commit is on {MAIN} (ancestor of {MAIN} {main_sha[:10]})")
    header = file_at(api, repo, "sdk/rocketnet.h", sha)
    shipping = shipping_version(header) if header else None
    say(shipping == version, f"sdk/rocketnet.h shipping ROCKETNET_VERSION_FULL is {shipping!r}, want {version!r}")
    if stage == "pre":
        notes = f"docs/RELEASE-NOTES-{version}.md"
        say(file_at(api, repo, notes, sha) is not None, f"release notes {notes} exist at the commit")
        try:
            tag = api.get(f"/repos/{repo}/git/ref/tags/{version}")
            target = tag["object"]["sha"]
            if tag["object"].get("type") == "tag":
                target = api.get(f"/repos/{repo}/git/tags/{target}")["object"]["sha"]
            if target != sha:
                say(False, f"tag {version} already exists at {target[:10]}, not this commit: a tag is never moved")
            else:
                print(f"NOTE  tag {version} already exists at this commit (a release run would not re-tag; "
                      f"--resume waits for its Release Check and publishes)", file=out)
        except NotFound:
            say(True, f"tag {version} does not exist yet")

    contexts = required_contexts(api, repo, sha)
    point = is_point_release(version)
    verdicts, not_in_force = evaluate(ctx, gates, ("push",) if point else ("push", "nightly"), version, contexts,
                                      f"{RULESET_PATH} at the commit", light=point)
    if point:
        for g in gates:
            if "nightly" in g.events and g.in_force(version):
                print(f"MINOR ONLY {g.label()}: a point release ({version}) is gated by the per-commit tier; "
                      f"this gate runs on main's schedule and gates the next minor release", file=out)
    for verdict in verdicts:
        print(verdict.line(), file=out)
        if not verdict.ok:
            red.append(verdict.line())
    tag_gates = [g for g in gates if g.kind == "tag" and g.in_force(version)]
    if stage == "pre":
        for g in tag_gates:
            print(f"LATER {g.label()}: judged on the tag push, before publishing", file=out)
    for g in not_in_force:
        print(f"NOT IN FORCE for {version}: {g.label()}", file=out)
    if red:
        print(f"VERDICT: REFUSE {version} at {sha[:10]}: {len(red)} red", file=out)
        for text in red:
            print(f"  red: {text}", file=out)
        return False, sha, ctx
    print(f"VERDICT: PASS {version} at {sha[:10]}: every gate in force is green", file=out)
    return True, sha, ctx
