#!/usr/bin/env python3
"""The nightly watch: every scheduled workflow gave a verdict for its last
night, and every job that is not green has an owner, or the watch fails.

Which workflows: every file in .github/workflows with a `schedule:` trigger,
except the watch itself (nightly-watch.yml). Selection is by content, never by
name, so a new scheduled workflow is watched the day it lands. Zero scheduled
workflows is an error. `--list` prints the selection, one file per line.

For each workflow the watch reads the scheduled runs on main (newest first)
and the jobs of the newest completed one. It is red on:

  NEW-RED     a job concluded `failure` and no disposition row names it.
              Printed with its commit range and a bisect line:
                RED <workflow> :: <job> bad=<sha> good=<sha> commits=<n>
                BISECT git bisect start <bad> <good>
  NO-VERDICT  the night gave no verdict, printed as
                NO-VERDICT <workflow> :: <job or (run)> <why>
              for each of:
                - no scheduled run on main at all, for a file that has been on
                  main since before its last cron firing (NO-RUN);
                - the newest scheduled run was created before the last cron
                  firing (computed from the file's own `cron:` lines) that is
                  more than GRACE ago: the cron did not fire, the workflow is
                  disabled, or GitHub dropped the run (stale);
                - a run not completed past its next cron firing plus GRACE: it
                  hung or sat queued past the next night, so its own night has
                  no verdict (in progress);
                - run conclusion startup_failure, cancelled, timed_out or any
                  other non-success with no job to blame;
                - a completed run with zero jobs, or with every job skipped;
                - a job whose conclusion is cancelled, timed_out, skipped,
                  neutral, action_required, stale or null, unless a row owns
                  it (or, for skipped, declares the skip expected).
  STALE       a disposition row that is wrong now: its job is not red, its
              pull request is merged or closed while the job is still red, its
              date is more than ROW_MAX_AGE_DAYS old while the job is still red
              (re-confirm the cause and owner, or fix it), or its workflow is
              not scheduled.
  API-ERROR   could not read run state: a GET failed, printed as
                API-ERROR could not read run state: GET <url>: <why>
              A transient failure (HTTP 5xx, 429, a rate-limit 403, a failure
              before any status) is tried TRIES times, BACKOFF seconds apart or
              as long as GitHub asks (up to MAX_WAIT); any other status is not
              retried. After a request runs out of tries, later requests get
              one try each until one is answered. The failure is counted, the
              listing continues, and the watch exits 1: an unreadable API is
              never a pass.
  BAD-ROW     a disposition row that does not parse (exit 2).

A run still going and younger than its next firing plus GRACE is reported as
RUNNING and the newest completed run stands for the workflow (that run itself
is then checked against the previous firing). A file on main for less than one
cron period with no run yet is PENDING, not red.

tools/nightly_dispositions.txt, one row per owned red job, tab separated:

  <workflow file>\t<job name prefix>\t<owner>\t<date YYYY-MM-DD>\t<cause>

owner is `#<open pull request>` or a lower-case name (`glenn` is the default
owner when no pull request is open). The date is when the row was last
confirmed against the job's real state. A cause beginning `expected-skip:`
declares that the job's `skipped` conclusion is green; such a row is STALE
when no job of the newest run matches it.

Exit 0 green, 1 red, 2 for a bad row or no scheduled workflows.
Reads GITHUB_TOKEN and GITHUB_REPOSITORY from the environment; makes only GET
requests to the GitHub REST API. The self-test is
tools/test_check_nightly_dispositions.py (fake API, no network).
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.github.com"
WINDOW = 30  # earlier scheduled runs searched for a green job
# GitHub creates scheduled runs late under load (minutes is usual, an hour has
# been seen) and the watch must not call a night missing while its run is
# merely late; three hours is well past any delay seen, and well under the
# shortest period in the repository (one day).
GRACE = datetime.timedelta(hours=3)
# A row is a claim that someone is fixing a red. Two weeks is long enough for a
# fix to land and short enough that a forgotten row surfaces before it hides a
# second cause behind the same job name.
ROW_MAX_AGE_DAYS = 14
# A transient GitHub failure (5xx, a rate limit, a dropped connection) is tried four
# times, 5 + 10 + 20 s apart; a wait GitHub names (Retry-After, X-RateLimit-Reset) is
# honoured up to MAX_WAIT. The job's 15-minute deadline bounds the rest.
TRIES = 4
BACKOFF = (5, 10, 20)
MAX_WAIT = 120
WATCH_FILE = "nightly-watch.yml"
DISPOSITIONS = os.path.join("tools", "nightly_dispositions.txt")

UTC = datetime.timezone.utc


def api_base():
    """The REST API root: GITHUB_API_URL as the runner sets it, else github.com's."""
    return os.environ.get("GITHUB_API_URL") or API


class ApiError(Exception):
    def __init__(self, url, why="failed", status=None):
        super().__init__(f"{url}: {why}")
        self.url = url
        self.why = why
        self.status = status


# ---------------------------------------------------------------- cron


def _cron_field(text, low, high):
    values = set()
    for part in text.split(","):
        step = 1
        if "/" in part:
            part, step_text = part.split("/", 1)
            step = int(step_text)
            if step < 1:
                raise ValueError(f"cron step {step_text!r}")
        if part == "*":
            start, end = low, high
        elif "-" in part:
            a, b = part.split("-", 1)
            start, end = int(a), int(b)
        else:
            start = int(part)
            end = high if step != 1 else start
        if start < low or end > high or start > end:
            raise ValueError(f"cron value {part!r} outside {low}-{high}")
        values.update(range(start, end + 1, step))
    return values


class Cron:
    """A five-field cron line as GitHub reads it (UTC): numbers, `*`, `*/n`,
    `a-b`, `a-b/n` and comma lists. Day of month and day of week combine as
    in Vixie cron: when both are restricted, either one matching is enough."""

    def __init__(self, text):
        fields = text.split()
        if len(fields) != 5:
            raise ValueError(f"cron {text!r}: want five fields")
        self.text = text
        self.minutes = sorted(_cron_field(fields[0], 0, 59))
        self.hours = sorted(_cron_field(fields[1], 0, 23))
        self.days = _cron_field(fields[2], 1, 31)
        self.months = _cron_field(fields[3], 1, 12)
        weekdays = _cron_field(fields[4], 0, 7)
        self.weekdays = {d % 7 for d in weekdays}  # 0 and 7 are Sunday
        self.day_star = fields[2].startswith("*")
        self.weekday_star = fields[4].startswith("*")

    def _day_matches(self, day):
        if day.month not in self.months:
            return False
        dom = day.day in self.days
        dow = (day.isoweekday() % 7) in self.weekdays
        if self.day_star or self.weekday_star:
            return dom and dow
        return dom or dow

    def last_at_or_before(self, moment):
        day = moment.date()
        for _ in range(400):
            if self._day_matches(day):
                for hour in reversed(self.hours):
                    for minute in reversed(self.minutes):
                        fire = datetime.datetime(day.year, day.month, day.day, hour, minute, tzinfo=UTC)
                        if fire <= moment:
                            return fire
            day -= datetime.timedelta(days=1)
        return None

    def first_after(self, moment):
        day = moment.date()
        for _ in range(400):
            if self._day_matches(day):
                for hour in self.hours:
                    for minute in self.minutes:
                        fire = datetime.datetime(day.year, day.month, day.day, hour, minute, tzinfo=UTC)
                        if fire > moment:
                            return fire
            day += datetime.timedelta(days=1)
        return None


def last_firing(crons, moment):
    times = [t for t in (c.last_at_or_before(moment) for c in crons) if t]
    return max(times) if times else None


def next_firing(crons, moment):
    times = [t for t in (c.first_after(moment) for c in crons) if t]
    return min(times) if times else None


# ---------------------------------------------------------------- workflows

_SCHEDULE = re.compile(r"^\s+schedule:")
_CRON = re.compile(r"""^\s*-\s*cron:\s*(["']?)([^"'#]+?)\1\s*(#.*)?$""")


def scheduled_workflows(root):
    """[(file name, [Cron])] for every workflow file with a schedule trigger,
    the watch excepted. A file with `schedule:` and no parseable cron line is
    returned with an empty list (the caller reports it)."""
    directory = os.path.join(root, ".github", "workflows")
    found = []
    if not os.path.isdir(directory):
        return found    # zero workflows: the caller refuses it
    for name in sorted(os.listdir(directory)):
        if not name.endswith((".yml", ".yaml")) or name == WATCH_FILE:
            continue
        with open(os.path.join(directory, name), encoding="utf-8") as f:
            lines = f.read().splitlines()
        if not any(_SCHEDULE.match(line) for line in lines):
            continue
        crons = []
        for line in lines:
            match = _CRON.match(line)
            if match:
                crons.append(Cron(match.group(2)))
        found.append((name, crons))
    return found


def git_since(root, name):
    """When the file arrived on the checked-out branch (first-parent history),
    or None when history does not say (a shallow clone): then the file counts
    as old, so a missing run is red rather than excused."""
    path = os.path.join(".github", "workflows", name)
    try:
        out = subprocess.run(["git", "-C", root, "log", "--first-parent", "--diff-filter=A",
                              "--format=%cI", "-1", "HEAD", "--", path],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    if not out:
        return None
    return parse_time(out)


def parse_time(text):
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    return datetime.datetime.fromisoformat(text).astimezone(UTC)


# ---------------------------------------------------------------- API


def http_fetch(url):
    request = urllib.request.Request(url)
    request.add_header("Accept", "application/vnd.github+json")
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        request.add_header("Authorization", "Bearer " + token)
    request.add_header("X-GitHub-Api-Version", "2022-11-28")
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def _header(error, name):
    headers = getattr(error, "headers", None)
    if headers is None:
        return None
    value = headers.get(name)
    return None if value is None else str(value)


def retry_after(error, now_epoch):
    """Seconds GitHub asked the client to wait (Retry-After, or X-RateLimit-Reset when the
    remaining count is 0), or None when it named none."""
    value = _header(error, "Retry-After")
    if value is not None and value.strip().isdigit():
        return int(value.strip())
    if _header(error, "X-RateLimit-Remaining") == "0":
        reset = _header(error, "X-RateLimit-Reset")
        if reset is not None and reset.strip().isdigit():
            return max(0, int(reset.strip()) - int(now_epoch))
    return None


def transient(error):
    """True for a failure a later try can fix: HTTP 5xx, 429, a 403 that is a rate limit
    (GitHub's primary limit says X-RateLimit-Remaining: 0; its secondary limit sends
    Retry-After), and a failure before any HTTP status (connection refused or reset, DNS,
    timeout, a truncated or non-JSON body). Any other HTTP status (401, 404, 422, ...) is not
    transient: trying again reads the same answer."""
    if isinstance(error, urllib.error.HTTPError):
        code = error.code
        if code >= 500 or code == 429:
            return True
        if code == 403:
            return (_header(error, "X-RateLimit-Remaining") == "0"
                    or _header(error, "Retry-After") is not None)
        return False
    return isinstance(error, (OSError, ValueError))


def describe(error):
    if isinstance(error, urllib.error.HTTPError):
        return f"HTTP {error.code}"
    return f"{type(error).__name__}: {error}"


class Api:
    """GETs with bounded retries. A transient failure is tried TRIES times in all, waiting
    BACKOFF[i] seconds (or what GitHub asked, up to MAX_WAIT) between tries; any other failure
    is not retried. While the API is down (the last request ran out of tries on a transient
    failure) a request is tried once, so an API that stays down costs one backoff, not one per
    request; the first answer clears it."""

    def __init__(self, fetch, sleep, clock=time.time):
        self.fetch = fetch
        self.sleep = sleep
        self.clock = clock
        self.down = False

    def get(self, path, params=None):
        url = api_base() + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        tries = 1 if self.down else TRIES
        for attempt in range(tries):
            try:
                answer = self.fetch(url)
            except Exception as error:  # noqa: BLE001 -- every failure is classified and reported
                if not transient(error):
                    raise ApiError(url, f"{describe(error)}, not retried",
                                   status=getattr(error, "code", None)) from error
                if attempt + 1 < tries:
                    asked = retry_after(error, self.clock())
                    wait = BACKOFF[attempt] if asked is None else min(max(asked, BACKOFF[attempt]), MAX_WAIT)
                    self.sleep(wait)
                    continue
                self.down = True
                what = f"{tries} tries" if tries > 1 else "1 try (the API was down for the request before)"
                raise ApiError(url, f"{describe(error)} after {what}") from error
            self.down = False
            return answer
        raise AssertionError("unreachable")

    def scheduled_runs(self, repo, workflow, count):
        try:
            data = self.get(f"/repos/{repo}/actions/workflows/{workflow}/runs",
                            {"event": "schedule", "branch": "main", "per_page": count})
        except ApiError as error:
            # GitHub knows no workflow at that path until the file has been on the default
            # branch (or run somewhere): it has no runs, and the watch's NO-RUN / PENDING
            # rule, from the file's age on main, gives the verdict
            if error.status == 404:
                return []
            raise
        return data.get("workflow_runs", [])

    def run_jobs(self, repo, run_id):
        jobs = []
        page = 1
        while True:
            data = self.get(f"/repos/{repo}/actions/runs/{run_id}/jobs", {"per_page": 100, "page": page})
            jobs += data.get("jobs", [])
            if len(jobs) >= data.get("total_count", 0) or not data.get("jobs"):
                return jobs
            page += 1


# ---------------------------------------------------------------- dispositions

_OWNER = re.compile(r"^(#[0-9]+|[a-z][a-z0-9-]*)$")
_DATE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")


def read_dispositions(path, today, out):
    rows = []
    want = "want <workflow>\\t<job prefix>\\t<#PR or name>\\t<YYYY-MM-DD>\\t<cause>"
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            bad = None
            if len(fields) != 5 or not all(fields):
                bad = want
            elif not _OWNER.match(fields[2]) or fields[2] == "owner-needed":
                bad = f"owner {fields[2]!r} is not #<PR> or a lower-case name; {want}"
            elif not _DATE.match(fields[3]):
                bad = f"date {fields[3]!r} is not YYYY-MM-DD; {want}"
            else:
                try:
                    date = datetime.date.fromisoformat(fields[3])
                except ValueError:
                    date = None
                if date is None:
                    bad = f"date {fields[3]!r} is not a date"
                elif date > today:
                    bad = f"date {fields[3]} is in the future"
            if bad:
                print(f"BAD-ROW {path}:{number}: {bad}", file=out)
                return None
            owner = fields[2]
            rows.append({"workflow": fields[0], "job": fields[1], "owner": owner,
                         "pr": int(owner[1:]) if owner.startswith("#") else None,
                         "date": date, "cause": fields[4],
                         "skip": fields[4].startswith("expected-skip:"),
                         "line": number, "used": False})
    return rows


# ---------------------------------------------------------------- the watch


class Watch:
    def __init__(self, api, repo, rows, now, out, dispositions_name):
        self.api = api
        self.repo = repo
        self.rows = rows
        self.now = now
        self.out = out
        self.where = dispositions_name
        self.count = {"NEW-RED": 0, "NO-VERDICT": 0, "STALE": 0, "API-ERROR": 0,
                      "OWNED": 0, "GREEN": 0, "RUNNING": 0, "PENDING": 0}
        self.evaluated = set()
        self.scheduled = set()

    def say(self, text):
        print(text, file=self.out)

    def red(self, kind, text):
        self.count[kind] += 1
        self.say(f"{kind} {text}")

    def no_verdict(self, workflow, what, why):
        self.red("NO-VERDICT", f"{workflow} :: {what} {why}")

    def api_error(self, error):
        self.red("API-ERROR", f"could not read run state: GET {error.url}: {error.why}")

    def owner_of(self, workflow, job_name, skip=None):
        for row in self.rows:
            if row["workflow"] == workflow and job_name.startswith(row["job"]):
                if skip is None or row["skip"] == skip:
                    return row
        return None

    def check_owner(self, row, workflow, job_name, what):
        row["used"] = True
        age = (self.now.date() - row["date"]).days
        if age > ROW_MAX_AGE_DAYS:
            self.red("STALE", f"{self.where}:{row['line']}: row dated {row['date']} is {age} days old "
                     f"(limit {ROW_MAX_AGE_DAYS}) and {workflow} :: {job_name} is still {what}: "
                     f"re-confirm the cause and owner, or fix it")
            return
        if row["pr"] is not None:
            try:
                pr = self.api.get(f"/repos/{self.repo}/pulls/{row['pr']}")
            except ApiError as error:
                self.api_error(error)
                return
            if pr.get("state") != "open":
                merged = " (merged)" if pr.get("merged_at") else ""
                self.red("STALE", f"{self.where}:{row['line']}: #{row['pr']} is {pr.get('state')}{merged} "
                         f"and {workflow} :: {job_name} is still {what}")
                return
        self.count["OWNED"] += 1
        self.say(f"OWNED {workflow} :: {job_name} ({what}) by {row['owner']} since {row['date']}: {row['cause']}")

    def watch(self, workflow, crons, since):
        self.scheduled.add(workflow)
        start = self.flagged()
        if not crons:
            self.no_verdict(workflow, "(run)", "has `schedule:` but no cron line the watch can read")
            return
        try:
            runs = self.api.scheduled_runs(self.repo, workflow, WINDOW + 1)
        except ApiError as error:
            self.api_error(error)
            return
        due = last_firing(crons, self.now - GRACE)
        if not runs:
            if since is not None and due is not None and since > due:
                self.count["PENDING"] += 1
                self.say(f"PENDING {workflow}: on main since {since:%Y-%m-%d %H:%M}Z, "
                         f"no firing due yet (next {next_firing(crons, self.now):%Y-%m-%d %H:%M}Z)")
                self.evaluated.add(workflow)
                return
            self.no_verdict(workflow, "(run)", "NO-RUN: no scheduled run on main at all"
                            + (f" (on main since {since:%Y-%m-%d}, cron due {due:%Y-%m-%d %H:%M}Z)"
                               if since else ""))
            return

        newest = runs[0]
        newest_created = parse_time(newest["created_at"])
        if due is not None and newest_created < due - datetime.timedelta(minutes=5):
            self.no_verdict(workflow, "(run)", f"stale: newest scheduled run {newest['id']} was created "
                            f"{newest_created:%Y-%m-%d %H:%M}Z, before the firing due {due:%Y-%m-%d %H:%M}Z "
                            f"(`{'`, `'.join(c.text for c in crons)}`, grace {GRACE})")

        for run in runs:
            if run.get("status") == "completed":
                continue
            created = parse_time(run["created_at"])
            limit = next_firing(crons, created)
            if limit is not None and self.now > limit + GRACE:
                self.no_verdict(workflow, f"(run {run['id']})",
                                f"{run.get('status')} since {created:%Y-%m-%d %H:%M}Z, past the next firing "
                                f"{limit:%Y-%m-%d %H:%M}Z plus grace {GRACE}: this night has no verdict")
            elif run is newest:
                self.count["RUNNING"] += 1
                self.say(f"RUNNING {workflow} run={run['id']} {run.get('status')} since "
                         f"{created:%Y-%m-%d %H:%M}Z; the newest completed run stands for it")

        completed = [run for run in runs if run.get("status") == "completed"]
        if not completed:
            self.evaluated.add(workflow)
            return
        verdict = completed[0]
        if verdict is not newest:
            # The newest run is still going; the run standing for it must be
            # the previous night's, not an older one.
            own = last_firing(crons, newest_created + datetime.timedelta(minutes=5))
            previous = last_firing(crons, own - datetime.timedelta(minutes=1)) if own else None
            created = parse_time(verdict["created_at"])
            if previous is not None and created < previous - datetime.timedelta(minutes=5):
                self.no_verdict(workflow, "(run)", f"stale: newest completed scheduled run {verdict['id']} was "
                                f"created {created:%Y-%m-%d %H:%M}Z, before the firing due {previous:%Y-%m-%d %H:%M}Z "
                                "that the run still going does not cover")
        try:
            jobs = self.api.run_jobs(self.repo, verdict["id"])
        except ApiError as error:
            self.api_error(error)
            return
        self.evaluated.add(workflow)
        self.say(f"RUN {workflow} run={verdict['id']} sha={verdict['head_sha'][:12]} "
                 f"created={verdict['created_at']} conclusion={verdict.get('conclusion')} jobs={len(jobs)}")
        before = self.flagged()

        if not jobs:
            self.no_verdict(workflow, f"(run {verdict['id']})",
                            f"conclusion={verdict.get('conclusion')} with zero jobs")
            return
        if all(job.get("conclusion") == "skipped" for job in jobs):
            self.no_verdict(workflow, f"(run {verdict['id']})", f"every one of its {len(jobs)} jobs was skipped")
            return

        earlier = None
        for job in jobs:
            conclusion = job.get("conclusion")
            name = job["name"]
            if conclusion == "success":
                continue
            if conclusion == "skipped":
                skip_row = self.owner_of(workflow, name, skip=True)
                if skip_row is not None:
                    skip_row["used"] = True
                    self.say(f"SKIP-EXPECTED {workflow} :: {name}: {skip_row['cause']}")
                    continue
            if conclusion == "failure":
                if earlier is None:
                    earlier = []
                    for run in completed[1:]:
                        try:
                            earlier.append((run, self.api.run_jobs(self.repo, run["id"])))
                        except ApiError as error:
                            self.api_error(error)
                good = next((run["head_sha"] for run, run_jobs in earlier
                             if any(j["name"] == name and j.get("conclusion") == "success" for j in run_jobs)), None)
                bad = verdict["head_sha"]
                if good:
                    try:
                        compare = self.api.get(f"/repos/{self.repo}/compare/{good}...{bad}")
                        commits = compare.get("ahead_by", "?")
                    except ApiError as error:
                        self.api_error(error)
                        commits = "?"
                    self.say(f"RED {workflow} :: {name} bad={bad[:12]} good={good[:12]} commits={commits}")
                    self.say(f"BISECT git bisect start {bad[:12]} {good[:12]}")
                else:
                    self.say(f"RED {workflow} :: {name} bad={bad[:12]} good=none "
                             f"(no green in the last {WINDOW} scheduled runs)")
            row = self.owner_of(workflow, name, skip=False)
            what = conclusion if conclusion else f"{job.get('status')} (no conclusion)"
            if row is not None:
                self.check_owner(row, workflow, name, what)
            elif conclusion == "failure":
                self.red("NEW-RED", f"{workflow} :: {name}: no disposition in {self.where}")
            else:
                self.no_verdict(workflow, name, f"conclusion={what}")

        after = self.flagged()
        if after == before and verdict.get("conclusion") != "success":
            self.no_verdict(workflow, f"(run {verdict['id']})",
                            f"conclusion={verdict.get('conclusion')} and no job explains it")
        elif after == start:
            self.count["GREEN"] += 1

    def flagged(self):
        return sum(self.count[k] for k in ("NEW-RED", "NO-VERDICT", "STALE", "OWNED", "API-ERROR"))

    def finish_rows(self):
        for row in self.rows:
            if row["used"]:
                continue
            if row["workflow"] not in self.scheduled:
                self.red("STALE", f"{self.where}:{row['line']}: {row['workflow']} is not a scheduled workflow")
            elif row["workflow"] in self.evaluated:
                if row["skip"]:
                    self.red("STALE", f"{self.where}:{row['line']}: {row['workflow']} :: {row['job']} "
                             "matches no skipped job of the newest run; remove the row")
                else:
                    self.red("STALE", f"{self.where}:{row['line']}: {row['workflow']} :: {row['job']} "
                             "is not red; remove the row")


def main(argv=None, fetch=None, now=None, sleep=None, since=None, out=None):
    out = out or sys.stdout
    parser = argparse.ArgumentParser(description="The nightly watch.")
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser.add_argument("--dispositions", default=None)
    parser.add_argument("--list", action="store_true", help="print the scheduled workflows the watch reads")
    args = parser.parse_args(argv)

    workflows = scheduled_workflows(args.root)
    if not workflows:
        print("NO-WORKFLOWS: no workflow with a `schedule:` trigger; the watch would read nothing", file=out)
        return 2
    if args.list:
        for name, _ in workflows:
            print(name, file=out)
        return 0

    now = now or datetime.datetime.now(UTC)
    since = since or (lambda name: git_since(args.root, name))
    dispositions = args.dispositions or os.path.join(args.root, DISPOSITIONS)
    rows = read_dispositions(dispositions, now.date(), out)
    if rows is None:
        return 2
    # the watch's own "now" is the clock a rate-limit reset is measured against
    api = Api(fetch or http_fetch, sleep or time.sleep, clock=now.timestamp)
    repo = os.environ.get("GITHUB_REPOSITORY", "mas-bandwidth/rocketnet")
    watch = Watch(api, repo, rows, now, out, DISPOSITIONS)
    for name, crons in workflows:
        watch.watch(name, crons, since(name))
    watch.finish_rows()

    c = watch.count
    failures = c["NEW-RED"] + c["NO-VERDICT"] + c["STALE"] + c["API-ERROR"]
    print(f"nightly watch: {len(workflows)} scheduled workflows, {c['GREEN']} green, {c['OWNED']} owned, "
          f"{c['RUNNING']} running, {c['PENDING']} pending; NEW-RED {c['NEW-RED']}, "
          f"NO-VERDICT {c['NO-VERDICT']}, STALE {c['STALE']}, API-ERROR {c['API-ERROR']}", file=out)
    print(f"nightly watch: {'RED' if failures else 'green'}: {failures} unowned, no-verdict, stale or api-error",
          file=out)
    if c["API-ERROR"]:
        print(f"nightly watch: RED: could not read run state for {c['API-ERROR']} request(s); "
              "what those requests cover has no verdict tonight", file=out)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
