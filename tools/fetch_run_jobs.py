#!/usr/bin/env python3
"""Read this run attempt's whole job list for the shard summary jobs, with bounded retries.

The shard summary jobs of ci.yml (debug-unit, lint-summary, tla-trace-summary
and the others that call tools/check_debug_unit_shards.py) read the GitHub
API's actions/runs/<id>/attempts/<n>/jobs and write it to <out.json>, the file
the checker reads. One read used to be one curl: a GitHub 5xx, a dropped
connection or a cut-off answer made the summary red with no verdict.

A read is tried TRIES times in all, waiting BACKOFF[i] seconds between tries,
and gives up at DEADLINE seconds from the start (no wait or request runs past
it; each request's timeout is the smaller of REQUEST_TIMEOUT and the time
left), inside the summary job's two-minute bound. A try is retried on HTTP 5xx,
429 or a rate-limit 403, on a failure before any HTTP status (refused, reset,
timeout, DNS) and on a short answer: a body that is not JSON, a page without a
"jobs" list or an integer "total_count", a page with fewer or more rows than
total_count leaves for it, a total_count that changes between pages, or a job
listed twice. Any other HTTP status (401, 404, ...) is not retried. The list is
complete only when the rows read across every page (per_page=100) number
total_count; it is then written as {"total_count": N, "jobs": [...]} and the
checker compares the two again. Never weaker than one read: a list still
unreadable after the last try is red, <out.json> is removed, and the checker
is never given a partial list.

Environment: GH_TOKEN, GITHUB_REPOSITORY, GITHUB_RUN_ID, GITHUB_RUN_ATTEMPT,
GITHUB_API_URL (default https://api.github.com).

Prints JOB-LIST=OK <n> jobs and exits 0, or JOB-LIST=FAIL <why> and exits 1.

Usage: fetch_run_jobs.py <out.json>
       fetch_run_jobs.py --self-test
"""

import http.client
import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

TRIES = 4
BACKOFF = (2, 4, 8)
REQUEST_TIMEOUT = 10
DEADLINE = 45
PER_PAGE = 100
MAX_PAGES = 20


class Short(ValueError):
    """An answer that is not the whole list: retried like a dropped connection."""


def transient(error):
    """True for a failure a later try can fix (tools/check_nightly_dispositions.py transient(), plus
    a body cut off mid-read): HTTP 5xx, 429, a rate-limit 403, any failure before an HTTP status,
    and a short or non-JSON answer. Any other HTTP status reads the same answer again."""
    if isinstance(error, urllib.error.HTTPError):
        if error.code >= 500 or error.code == 429:
            return True
        headers = error.headers or {}
        return error.code == 403 and (headers.get("X-RateLimit-Remaining") == "0"
                                      or headers.get("Retry-After") is not None)
    return isinstance(error, (OSError, ValueError, http.client.HTTPException))


def describe(error):
    if isinstance(error, urllib.error.HTTPError):
        return f"HTTP {error.code}"
    if isinstance(error, Short):
        return f"short answer: {error}"
    return f"{type(error).__name__}: {error}"


def http_fetch(url, headers, timeout):
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def read_once(fetch, base, headers, timeout_left):
    """Every page of the list, checked against total_count; raises on any failure."""
    rows, total, ids = [], None, set()
    for page in range(1, MAX_PAGES + 1):
        body = fetch(f"{base}?per_page={PER_PAGE}&page={page}", headers, timeout_left())
        try:
            data = json.loads(body)
        except ValueError as error:
            raise Short(f"page {page} is not JSON: {error}") from error
        jobs = data.get("jobs") if isinstance(data, dict) else None
        count = data.get("total_count") if isinstance(data, dict) else None
        if not isinstance(jobs, list) or type(count) is not int or count < 0:
            raise Short(f"page {page} has no jobs list or no integer total_count")
        if total is None:
            total = count
        elif count != total:
            raise Short(f"total_count changed from {total} to {count} at page {page}")
        want = min(PER_PAGE, total - len(rows))
        if len(jobs) != want:
            raise Short(f"page {page} has {len(jobs)} rows, total_count {total} leaves {want} for it")
        for job in jobs:
            key = job.get("id") if isinstance(job, dict) else None
            if key is not None:
                if key in ids:
                    raise Short(f"job {key} is listed twice")
                ids.add(key)
        rows.extend(jobs)
        if len(rows) == total:
            return {"total_count": total, "jobs": rows}
    raise Short(f"{len(rows)} of {total} rows after {MAX_PAGES} pages")


def fetch_jobs(env, out, fetch=http_fetch, sleep=time.sleep, clock=time.monotonic, log=print):
    """0 when <out> holds the whole job list; 1, with <out> removed, when it could not be read."""
    out = Path(out)
    try:
        out.unlink()
    except FileNotFoundError:
        pass
    names = ("GH_TOKEN", "GITHUB_REPOSITORY", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT")
    missing = [name for name in names if not env.get(name)]
    if missing:
        log(f"JOB-LIST=FAIL no verdict: {', '.join(missing)} not set")
        return 1
    api = (env.get("GITHUB_API_URL") or "https://api.github.com").rstrip("/")
    base = (f"{api}/repos/{env['GITHUB_REPOSITORY']}/actions/runs/{env['GITHUB_RUN_ID']}"
            f"/attempts/{env['GITHUB_RUN_ATTEMPT']}/jobs")
    headers = {"Authorization": "Bearer " + env["GH_TOKEN"], "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"}
    end = clock() + DEADLINE

    def timeout_left():
        left = end - clock()
        if left <= 0:
            raise TimeoutError(f"the {DEADLINE} s deadline passed")
        return min(REQUEST_TIMEOUT, left)

    why = "no try ran"
    for attempt in range(TRIES):
        try:
            listing = read_once(fetch, base, headers, timeout_left)
        except Exception as error:  # noqa: BLE001 -- every failure is classified and reported
            why = describe(error)
            if not transient(error):
                log(f"JOB-LIST=FAIL no verdict: {why}, not retried")
                return 1
            wait = BACKOFF[attempt] if attempt < len(BACKOFF) else None
            if attempt + 1 == TRIES or wait is None or clock() + wait >= end:
                break
            log(f"JOB-LIST try {attempt + 1}/{TRIES}: {why}; again in {wait} s")
            sleep(wait)
            continue
        out.write_text(json.dumps(listing), encoding="utf-8")
        log(f"JOB-LIST=OK {listing['total_count']} jobs (try {attempt + 1}/{TRIES})")
        return 0
    log(f"JOB-LIST=FAIL no verdict: {why} after {attempt + 1} of {TRIES} tries")
    return 1


def self_test():
    """Fakes only: no socket, no clock, no sleep. Each case is a list of answers, one per request
    (bytes, or an exception to raise); the shard checker reads what was written."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import check_debug_unit_shards as checker

    shards = checker.SHARDS
    env = {"GH_TOKEN": "t", "GITHUB_REPOSITORY": "o/r", "GITHUB_RUN_ID": "7", "GITHUB_RUN_ATTEMPT": "1"}

    def rows(names, start=0):
        return [{"id": start + n, "name": name, "conclusion": "success"} for n, name in enumerate(names)]

    def page(jobs, total=None):
        return json.dumps({"total_count": len(jobs) if total is None else total, "jobs": jobs}).encode()

    def status(code):
        return urllib.error.HTTPError("u", code, "planted", {}, None)

    green = page(rows(shards))
    many = rows(shards) + rows([f"other {n}" for n in range(146)], 4)   # 150 jobs: two pages

    cases = [
        # (label, answers, want exit, want checker PASS, want sleeps)
        ("one clean read", [green], 0, True, []),
        ("a 502, then the list", [status(502), green], 0, True, [2]),
        ("a dropped stream, then the list", [http.client.RemoteDisconnected("stream not closed"), green], 0, True, [2]),
        ("refused, then the list", [ConnectionRefusedError("refused"), green], 0, True, [2]),
        ("a body cut off, then the list", [green[:40], green], 0, True, [2]),
        ("a cut-off read, then the list", [http.client.IncompleteRead(b"{"), green], 0, True, [2]),
        ("a 500, a 503, a 504, then the list", [status(500), status(503), status(504), green], 0, True, [2, 4, 8]),
        ("two full pages", [page(many[:100], 150), page(many[100:], 150)], 0, True, []),
        ("the second page fails once", [page(many[:100], 150), status(502), page(many[:100], 150),
                                        page(many[100:], 150)], 0, True, [2]),
        ("5xx on every try", [status(502)] * TRIES, 1, None, [2, 4, 8]),
        ("partial: total_count 6, four rows, every try", [page(rows(shards), 6)] * TRIES, 1, None, [2, 4, 8]),
        ("partial: a short second page, every try", [page(many[:100], 150), page(many[100:130], 150)] * TRIES,
         1, None, [2, 4, 8]),
        ("total_count changes between pages", [page(many[:100], 150), page(many[100:], 151)] * TRIES,
         1, None, [2, 4, 8]),
        ("a job listed twice across pages", [page(many[:100], 150), page(many[99:149], 150)] * TRIES,
         1, None, [2, 4, 8]),
        ("no total_count", [json.dumps({"jobs": rows(shards)}).encode()] * TRIES, 1, None, [2, 4, 8]),
        ("a 404 is not retried", [status(404), green], 1, None, []),
        ("a 401 is not retried", [status(401), green], 1, None, []),
        ("a rate-limit 403 is retried", [urllib.error.HTTPError("u", 403, "limit", {"X-RateLimit-Remaining": "0"},
                                                                None), green], 0, True, [2]),
        ("a list with a failed shard is read, and the checker is red",
         [page(rows(shards[:3]) + [{"id": 9, "name": shards[3], "conclusion": "failure"}])], 0, False, []),
        ("a list missing a shard is read, and the checker is red", [page(rows(shards[:3]))], 0, False, []),
    ]
    bad = 0

    def expect(label, ok):
        nonlocal bad
        if not ok:
            print(f"self-test {label}: FAIL")
            bad += 1

    with tempfile.TemporaryDirectory() as tmp:
        for number, (label, answers, want, want_pass, want_sleeps) in enumerate(cases):
            out = Path(tmp) / f"case{number}.json"
            out.write_text("stale")
            queue, slept, now = list(answers), [], [0.0]

            def fetch(url, headers, timeout):
                if not queue:
                    raise AssertionError("more requests than answers")
                if not (0 < timeout <= REQUEST_TIMEOUT) or headers.get("Authorization") != "Bearer t":
                    raise AssertionError(f"request timeout {timeout} or headers {headers}")
                if "/repos/o/r/actions/runs/7/attempts/1/jobs?per_page=100&page=" not in url:
                    raise AssertionError(f"url {url}")
                answer = queue.pop(0)
                if isinstance(answer, BaseException):
                    raise answer
                return answer

            def sleep(seconds):
                slept.append(seconds)
                now[0] += seconds

            code = fetch_jobs(env, out, fetch, sleep, lambda: now[0], log=lambda line: None)
            expect(f"{label}: exit {code}, want {want}", code == want)
            expect(f"{label}: slept {slept}, want {want_sleeps}", slept == want_sleeps)
            expect(f"{label}: total sleep {sum(slept)} s is past the deadline", sum(slept) < DEADLINE)
            if want == 0:
                ok, why = checker.verdict(out.read_text(encoding="utf-8"))
                expect(f"{label}: checker {'PASS' if ok else 'FAIL ' + why}", ok == want_pass)
            else:
                expect(f"{label}: a failed read left {out.name} behind", not out.exists())

        # the deadline: a server that hangs each request for its whole timeout gets no try that
        # starts after DEADLINE, and no wait past it
        out = Path(tmp) / "slow.json"
        now, slept, timeouts = [0.0], [], []

        def slow(url, headers, timeout):
            timeouts.append(timeout)
            now[0] += timeout
            raise TimeoutError("timed out")

        code = fetch_jobs(env, out, slow, lambda s: (slept.append(s), now.__setitem__(0, now[0] + s)),
                          lambda: now[0], log=lambda line: None)
        expect(f"deadline: exit {code}", code == 1)
        expect(f"deadline: ended at {now[0]} s, past {DEADLINE} s", now[0] <= DEADLINE)
        expect(f"deadline: {len(timeouts)} tries", 1 < len(timeouts) <= TRIES)

        missing = fetch_jobs({}, Path(tmp) / "none.json", log=lambda line: None)
        expect("no environment: not red", missing == 1)
        expect("no environment: a file was written", not (Path(tmp) / "none.json").exists())
    if bad:
        return 1
    print("fetch_run_jobs: self-test ok")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print(__doc__)
        return 2
    return fetch_jobs(os.environ, argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
