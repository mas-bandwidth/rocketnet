#!/usr/bin/env python3
"""Read this run attempt's whole job list for the shard summary jobs, with bounded retries.

The shard summary jobs of ci.yml (debug-unit, lint-summary, tla-trace-summary
and the others that call tools/check_debug_unit_shards.py) read the GitHub
API's actions/runs/<id>/attempts/<n>/jobs and write it to <out.json>, the file
the checker reads. One read used to be one curl: a GitHub 5xx, a dropped
connection or a cut-off answer made the summary red with no verdict.

A read is tried TRIES times in all, waiting BACKOFF[i] seconds between tries,
inside one wall-clock deadline of DEADLINE seconds from the start, which sits
inside the summary job's two-minute bound. The deadline is a real bound, not
a per-operation timeout:

- Each request runs on a worker thread that the caller joins for at most the
  time left to the deadline: a GitHub answer that is slow but arrives (ten
  seconds for one page is seen in practice) is taken, not cut off at a
  per-request bound. A request still running at the deadline (DNS, connect,
  headers or a body that keeps trickling) is abandoned and counts as a
  timeout.
- A try starts only while at least REQUEST_FLOOR seconds are left before the
  deadline; with less, the read ends there, red, rather than send a request
  that has no time to be answered.
- The body is read in chunks of CHUNK bytes. Before each chunk the socket's
  timeout is set to the time left, and after each chunk the clock is checked,
  so a body that arrives slowly ends at the deadline. Once the response is
  closed (http.client closes it after the last byte) the socket is not
  touched again.
- A list that arrives past the deadline is refused (red), never published.

A try is retried on HTTP 5xx, 429 or a rate-limit 403, on a failure before any
HTTP status (refused, reset, timeout, DNS), and on a short answer:
- a body that is not JSON;
- a page without a "jobs" list or an integer "total_count";
- a page with fewer or more rows than total_count leaves for it;
- a total_count that changes between pages;
- a job listed twice.
Any other HTTP status (401, 404, a plain 403, ...) is not retried.

When the server names a wait, the wait is honoured: Retry-After, or the reset
time of an exhausted rate limit (X-RateLimit-Reset). A rate-limit answer that
names neither waits SECONDARY_WAIT, as GitHub asks for its secondary limits.
When that wait cannot fit before the deadline, the read fails at once rather
than asking again early. Ordinary 5xx and transport failures keep the BACKOFF
schedule.

The list is complete only when the rows read across every page (per_page=100)
number total_count. It is then published as {"total_count": N, "jobs": [...]}
through a temporary file in the same directory, renamed onto <out.json>, and
the checker compares the two numbers again. The temporary file is removed on
every failure. Never weaker than one read: a list still unreadable after the
last try is red, <out.json> is removed, and the checker is never given a
partial list.

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
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

TRIES = 4
BACKOFF = (2, 4, 8)
REQUEST_FLOOR = 10
DEADLINE = 45
SECONDARY_WAIT = 60
PER_PAGE = 100
MAX_PAGES = 20
CHUNK = 65536


class Short(ValueError):
    """An answer that is not the whole list: retried like a dropped connection."""


class Late(Exception):
    """A whole list that arrived after the deadline: refused, not retried."""


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


def server_wait(error, wall):
    """The seconds the server asks a client to wait before asking again, or None when it names no
    wait and the answer is not a rate limit (then the BACKOFF schedule applies). Retry-After is
    honoured on any answer; an exhausted primary limit waits until X-RateLimit-Reset; any other
    rate-limit answer waits SECONDARY_WAIT. A wait that cannot be read is SECONDARY_WAIT."""
    if not isinstance(error, urllib.error.HTTPError):
        return None
    headers = error.headers or {}
    after = headers.get("Retry-After")
    if after is not None:
        try:
            return max(0, int(after))
        except ValueError:
            return SECONDARY_WAIT
    if headers.get("X-RateLimit-Remaining") == "0":
        reset = str(headers.get("X-RateLimit-Reset") or "").strip()
        return max(0, int(reset) - wall()) if reset.isdigit() else SECONDARY_WAIT
    if error.code == 429:
        return SECONDARY_WAIT
    return None


def describe(error):
    if isinstance(error, urllib.error.HTTPError):
        return f"HTTP {error.code}"
    if isinstance(error, Short):
        return f"short answer: {error}"
    return f"{type(error).__name__}: {error}"


def socket_timeout_setter(response):
    """The response's socket settimeout, when the response exposes it, else a no-op: the
    chunked read's clock check and the thread join still bound the request."""
    sock = getattr(getattr(getattr(response, "fp", None), "raw", None), "_sock", None)
    return sock.settimeout if sock is not None and hasattr(sock, "settimeout") else (lambda seconds: None)


def read_body(response, left, set_timeout):
    """The whole body, CHUNK bytes at a time: before each chunk the socket's timeout is the time
    left, and after each chunk the clock is checked, so a body that keeps trickling ends at the
    request's deadline (left() raises TimeoutError once it has passed). The socket is never
    touched once the body is whole: http.client closes the connection after the last byte, so the
    loop ends when the response is closed, and an OSError from settimeout after a chunk was read
    (the socket closed under it) ends the body too; the body is then judged whole or short by the
    page checks, as any other."""
    closed = getattr(response, "isclosed", lambda: False)
    chunks = []
    while True:
        if closed():
            break
        remaining = left()
        try:
            set_timeout(remaining)
        except OSError:
            if not chunks:
                raise
            break
        chunk = response.read(CHUNK)
        if not chunk:
            break
        chunks.append(chunk)
    left()
    return b"".join(chunks)


def http_fetch(url, headers, left):
    """One GET, bounded by left(): the request runs on a worker thread joined for at most the time
    left, so DNS, connect, headers and body together cannot pass the deadline. A request still
    running then is abandoned (the thread is a daemon; the step exits) and is a timeout."""
    box = {}

    def run():
        try:
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(request, timeout=left()) as response:
                box["body"] = read_body(response, left, socket_timeout_setter(response))
        except BaseException as error:  # noqa: BLE001 -- handed to the caller's thread
            box["error"] = error

    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    worker.join(left())
    if worker.is_alive():
        raise TimeoutError("the request ran past its deadline")
    if "error" in box:
        raise box["error"]
    return box["body"]


def publish(listing, out, replace=os.replace):
    """Write the list to a temporary file beside <out> and rename it onto <out>, so <out> is
    either absent or whole. The temporary file (and <out>) are removed on any failure, an
    interrupt included, and the failure is raised again."""
    handle, temporary = tempfile.mkstemp(dir=out.parent, prefix=f".{out.name}.", suffix=".tmp")
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(listing))
            stream.flush()
            os.fsync(stream.fileno())
        replace(temporary, out)
    except BaseException:
        for path in (temporary, out):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
        raise


def read_once(fetch, base, headers, request_left):
    """Every page of the list, checked against total_count; raises on any failure. Each page is one
    request with its own deadline, request_left()."""
    rows, total, ids = [], None, set()
    for page in range(1, MAX_PAGES + 1):
        body = fetch(f"{base}?per_page={PER_PAGE}&page={page}", headers, request_left())
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


def fetch_jobs(env, out, fetch=http_fetch, sleep=time.sleep, clock=time.monotonic, log=print,
               wall=time.time, replace=os.replace):
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

    def request_left():
        """A left() for one request: the seconds until the deadline, raising TimeoutError once
        that has passed. A slow answer may use all of it."""
        stop = end

        def left():
            remaining = stop - clock()
            if remaining <= 0:
                raise TimeoutError(f"past the {DEADLINE} s deadline")
            return remaining
        return left

    why = "no try ran"
    tried = 0
    for attempt in range(TRIES):
        if attempt > 0 and end - clock() < REQUEST_FLOOR:
            # checked at the start of every retry, not only before its wait: a sleep that
            # returns late, or time lost after the check, never starts a try that has under
            # REQUEST_FLOOR seconds to be answered
            why += f", and fewer than {REQUEST_FLOOR} s were left for another try"
            break
        tried = attempt + 1
        try:
            listing = read_once(fetch, base, headers, request_left)
            late = clock() - end
            if late > 0:
                raise Late(f"the list arrived {late:.1f} s past the {DEADLINE} s deadline")
        except Late as error:
            log(f"JOB-LIST=FAIL no verdict: {error}, refused")
            return 1
        except Exception as error:  # noqa: BLE001 -- every failure is classified and reported
            why = describe(error)
            if not transient(error):
                log(f"JOB-LIST=FAIL no verdict: {why}, not retried")
                return 1
            if attempt + 1 == TRIES:
                break
            asked = server_wait(error, wall)
            wait = BACKOFF[attempt] if attempt < len(BACKOFF) else None
            if asked is not None:
                if clock() + asked + REQUEST_FLOOR > end:
                    log(f"JOB-LIST=FAIL no verdict: {why}, the server asks for {asked} s, which leaves "
                        f"fewer than {REQUEST_FLOOR} s before the {DEADLINE} s deadline: not asked again early")
                    return 1
                wait = max(asked, wait or 0)
            if wait is None or clock() + wait + REQUEST_FLOOR > end:
                why += f", and fewer than {REQUEST_FLOOR} s would be left for another try"
                break
            log(f"JOB-LIST try {attempt + 1}/{TRIES}: {why}; again in {wait} s")
            sleep(wait)
            continue
        try:
            publish(listing, out, replace)
        except Exception as error:  # noqa: BLE001 -- a failed write is red, with nothing left behind
            log(f"JOB-LIST=FAIL no verdict: the list could not be written: {describe(error)}")
            return 1
        log(f"JOB-LIST=OK {listing['total_count']} jobs (try {attempt + 1}/{TRIES})")
        return 0
    log(f"JOB-LIST=FAIL no verdict: {why} after {tried} of {TRIES} tries")
    return 1


def loopback_server(body, mode):
    """A server on 127.0.0.1 (an ephemeral port, on its own thread) that answers every GET with
    body, as Content-Length (mode "length") or chunked (mode "chunked"), and closes the connection
    after it: the shape of a GitHub API answer to urllib, which asks for Connection: close."""
    import http.server

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):  # noqa: N802 -- the http.server name
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Connection", "close")
            if mode == "chunked":
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                half = len(body) // 2
                for part in (body[:half], body[half:], b""):
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            else:
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            self.close_connection = True

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def self_test():
    """Fakes for the schedule: no clock, no sleep. Each case is a list of answers, one per request
    (bytes, or an exception to raise); the shard checker reads what was written. Two cases then
    run the real request path (http_fetch, urlopen, read_body) against a loopback server on
    127.0.0.1, with no network."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import check_debug_unit_shards as checker

    shards = checker.SHARDS
    env = {"GH_TOKEN": "t", "GITHUB_REPOSITORY": "o/r", "GITHUB_RUN_ID": "7", "GITHUB_RUN_ATTEMPT": "1"}

    def rows(names, start=0):
        return [{"id": start + n, "name": name, "conclusion": "success"} for n, name in enumerate(names)]

    def page(jobs, total=None):
        return json.dumps({"total_count": len(jobs) if total is None else total, "jobs": jobs}).encode()

    def status(code, headers=None):
        return urllib.error.HTTPError("u", code, "planted", headers or {}, None)

    wall_base = 1_000_000                                                # the fake wall clock at now 0

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
        ("a plain 403 is not retried", [status(403), green], 1, None, []),
        ("an exhausted limit waits until its reset",
         [status(403, {"X-RateLimit-Remaining": "0", "X-RateLimit-Reset": str(wall_base + 3)}), green], 0, True, [3]),
        ("an exhausted limit whose reset is past the deadline fails at once",
         [status(403, {"X-RateLimit-Remaining": "0", "X-RateLimit-Reset": str(wall_base + 600)}), green],
         1, None, []),
        ("an exhausted limit with no reset waits the secondary minute: fails at once",
         [status(403, {"X-RateLimit-Remaining": "0"}), green], 1, None, []),
        ("Retry-After 5 on a 503 waits 5 s, not 2", [status(503, {"Retry-After": "5"}), green], 0, True, [5]),
        ("Retry-After 60 on a 429 fails at once", [status(429, {"Retry-After": "60"}), green], 1, None, []),
        ("Retry-After 60 on a 403 fails at once", [status(403, {"Retry-After": "60"}), green], 1, None, []),
        ("a 429 that names no wait waits the secondary minute: fails at once", [status(429), green], 1, None, []),
        ("a short Retry-After never shortens the backoff", [status(502), status(503, {"Retry-After": "1"}), green],
         0, True, [2, 4]),
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

            def fetch(url, headers, left):
                if not queue:
                    raise AssertionError("more requests than answers")
                timeout = left()
                if not (0 < timeout <= DEADLINE) or headers.get("Authorization") != "Bearer t":
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

            code = fetch_jobs(env, out, fetch, sleep, lambda: now[0], log=lambda line: None,
                              wall=lambda: wall_base + now[0])
            expect(f"{label}: exit {code}, want {want}", code == want)
            expect(f"{label}: slept {slept}, want {want_sleeps}", slept == want_sleeps)
            expect(f"{label}: total sleep {sum(slept)} s is past the deadline", sum(slept) < DEADLINE)
            if want == 0:
                ok, why = checker.verdict(out.read_text(encoding="utf-8"))
                expect(f"{label}: checker {'PASS' if ok else 'FAIL ' + why}", ok == want_pass)
            else:
                expect(f"{label}: a failed read left {out.name} behind", not out.exists())

        # the deadline: a server that hangs a request for all its time uses the whole deadline in
        # one try; nothing starts after DEADLINE, and nothing waits past it
        out = Path(tmp) / "slow.json"
        now, slept, timeouts = [0.0], [], []

        def slow(url, headers, left):
            timeout = left()
            timeouts.append(timeout)
            now[0] += timeout
            raise TimeoutError("timed out")

        code = fetch_jobs(env, out, slow, lambda s: (slept.append(s), now.__setitem__(0, now[0] + s)),
                          lambda: now[0], log=lambda line: None)
        expect(f"deadline: exit {code}", code == 1)
        expect(f"deadline: ended at {now[0]} s, past {DEADLINE} s", now[0] <= DEADLINE)
        expect(f"deadline: {len(timeouts)} tries, want 1", len(timeouts) == 1)
        expect(f"deadline: the request was given {timeouts}, want the whole {DEADLINE} s",
               timeouts == [DEADLINE])

        # slow but working: GitHub has taken ten seconds for one page. A reply that takes 20 s is
        # taken on the first try (a flat 10 s request bound turned it into four timeouts, PR #1126
        # attempt 2), and after a fast 502 it is still taken
        for label, answers, want_sleeps in (("slow reply", [20], []), ("a 502, then a slow reply", ["502", 20], [2])):
            out = Path(tmp) / "slow-ok.json"
            now, slept, queue = [0.0], [], list(answers)

            def slow_ok(url, headers, left):
                answer = queue.pop(0)
                if answer == "502":
                    raise status(502)
                if left() < answer:
                    now[0] += left()
                    raise TimeoutError("timed out")
                now[0] += answer
                return green

            lines = []
            code = fetch_jobs(env, out, slow_ok, lambda s: (slept.append(s), now.__setitem__(0, now[0] + s)),
                              lambda: now[0], log=lines.append)
            expect(f"{label}: exit {code} {lines}", code == 0)
            expect(f"{label}: slept {slept}, want {want_sleeps}", slept == want_sleeps)

        # the floor: a try that failed after using most of the time is not followed by one that
        # would have under REQUEST_FLOOR seconds; the log says why it stopped
        out = Path(tmp) / "floor.json"
        now, slept = [0.0], []

        def reset_late(url, headers, left):
            now[0] += DEADLINE - REQUEST_FLOOR - 1
            raise ConnectionResetError("reset")

        lines = []
        code = fetch_jobs(env, out, reset_late, lambda s: (slept.append(s), now.__setitem__(0, now[0] + s)),
                          lambda: now[0], log=lines.append)
        expect(f"floor: exit {code}", code == 1)
        expect(f"floor: slept {slept}", slept == [])
        expect(f"floor: the log does not say why {lines}", bool(lines) and "would be left for another try" in lines[-1])

        # the floor at the start of the retry: the backoff fits (34 s used, 2 s wait, 9 s left
        # would not), but the sleep returns late, 3 s past its 2; no try starts with 6 s left
        out = Path(tmp) / "oversleep.json"
        now, slept, calls = [0.0], [], []

        def reset_at_32(url, headers, left):
            calls.append(now[0])
            now[0] += DEADLINE - REQUEST_FLOOR - 3
            raise ConnectionResetError("reset")

        def oversleep(seconds):
            slept.append(seconds)
            now[0] += seconds + 3

        lines = []
        code = fetch_jobs(env, out, reset_at_32, oversleep, lambda: now[0], log=lines.append)
        expect(f"oversleep: exit {code}", code == 1)
        expect(f"oversleep: tries started at {calls}, want one", len(calls) == 1)
        expect(f"oversleep: slept {slept}, want [2]", slept == [2])
        expect(f"oversleep: the log {lines}", bool(lines) and "were left for another try" in lines[-1]
               and "after 1 of 4 tries" in lines[-1])

        # a body that keeps trickling: one byte a second, never ending. The chunked read checks the
        # clock after every chunk and sets each chunk's socket timeout to the time left, so each try
        # ends at its request deadline and the whole read at DEADLINE; nothing is published
        class Trickle:
            def read(self, size):
                now[0] += 1
                return b" "

        out = Path(tmp) / "trickle.json"
        out.write_text("stale")
        now, slept, socket_timeouts = [0.0], [], []

        def trickle(url, headers, left):
            return read_body(Trickle(), left, socket_timeouts.append)

        code = fetch_jobs(env, out, trickle, lambda s: (slept.append(s), now.__setitem__(0, now[0] + s)),
                          lambda: now[0], log=lambda line: None)
        expect(f"trickle: exit {code}", code == 1)
        expect(f"trickle: ended at {now[0]} s, past {DEADLINE} s", now[0] <= DEADLINE)
        expect(f"trickle: a socket timeout past {DEADLINE} s",
               bool(socket_timeouts) and max(socket_timeouts) <= DEADLINE)
        expect("trickle: the stale file was left", not out.exists())

        # the socket closed under the read: after a chunk, settimeout's OSError ends the body;
        # before any chunk it is the request's failure. A closed response is never touched
        class Parts:
            def __init__(self, parts, closes=False):
                self.parts, self.closes = list(parts), closes

            def read(self, size):
                return self.parts.pop(0) if self.parts else b""

            def isclosed(self):
                return self.closes and not self.parts

        def settimeout_after(good):
            calls = [0]

            def set_timeout(seconds):
                calls[0] += 1
                if calls[0] > good:
                    raise OSError(9, "Bad file descriptor")
            return set_timeout

        def no_touch(seconds):
            raise AssertionError("the socket was touched after the response closed")

        forever = lambda: 1.0  # noqa: E731
        expect("closed after a chunk: the body", read_body(Parts([b"ab", b"cd"]), forever, settimeout_after(2)) == b"abcd")
        try:
            read_body(Parts([b"ab"]), forever, settimeout_after(0))
            expect("closed before any chunk: not raised", False)
        except OSError:
            pass
        expect("a closed response: the body", read_body(Parts([b"ab", b"cd"], closes=True), forever,
                                                         settimeout_after(2)) == b"abcd")
        closed_parts = Parts([b"ab"], closes=True)
        expect("a closed response: touched", read_body(closed_parts, forever,
                                                       lambda s: (closed_parts.parts or no_touch(s))) == b"ab")

        # a whole, valid list that arrives after the deadline is refused, not published
        out = Path(tmp) / "late.json"
        now = [0.0]

        def late(url, headers, left):
            left()
            now[0] += DEADLINE + 5
            return green

        code = fetch_jobs(env, out, late, lambda s: None, lambda: now[0], log=lambda line: None)
        expect(f"late: exit {code}", code == 1)
        expect("late: a list past the deadline was published", not out.exists())

        # publication: through a temporary file renamed onto <out>; a failed or interrupted
        # rename leaves neither <out> nor the temporary file
        def leftovers(directory, name):
            return sorted(p.name for p in Path(directory).iterdir() if p.name.startswith(f".{name}."))

        publish_dir = Path(tmp) / "publish"
        publish_dir.mkdir()
        out = publish_dir / "ok.json"
        now = [0.0]
        code = fetch_jobs(env, out, lambda u, h, left: green, lambda s: None, lambda: now[0],
                          log=lambda line: None)
        expect(f"publish: exit {code}", code == 0)
        expect("publish: the list is not whole", json.loads(out.read_text(encoding="utf-8"))["total_count"] == 4)
        expect(f"publish: temporary files left {leftovers(publish_dir, out.name)}",
               not leftovers(publish_dir, out.name))

        def refused_rename(source, target):
            raise OSError("planted: the rename failed")

        out = publish_dir / "refused.json"
        out.write_text("stale")
        code = fetch_jobs(env, out, lambda u, h, left: green, lambda s: None, lambda: now[0],
                          log=lambda line: None, replace=refused_rename)
        expect(f"publish refused: exit {code}", code == 1)
        expect("publish refused: <out> left", not out.exists())
        expect(f"publish refused: temporary files left {leftovers(publish_dir, out.name)}",
               not leftovers(publish_dir, out.name))

        class Killed(BaseException):
            """The step killed between the write and the rename."""

        def killed_rename(source, target):
            if not Path(source).read_text(encoding="utf-8"):
                raise AssertionError("the temporary file is empty before the rename")
            raise Killed()

        out = publish_dir / "killed.json"
        try:
            fetch_jobs(env, out, lambda u, h, left: green, lambda s: None, lambda: now[0],
                       log=lambda line: None, replace=killed_rename)
            expect("killed: the interrupt was swallowed", False)
        except Killed:
            pass
        expect("killed: <out> left", not out.exists())
        expect(f"killed: temporary files left {leftovers(publish_dir, out.name)}",
               not leftovers(publish_dir, out.name))

        missing = fetch_jobs({}, Path(tmp) / "none.json", log=lambda line: None)
        expect("no environment: not red", missing == 1)
        expect("no environment: a file was written", not (Path(tmp) / "none.json").exists())

        # the real request path, end to end: http_fetch, urlopen and read_body against a loopback
        # server whose response closes once its body is read, with Content-Length and chunked.
        # The fakes above never reach the socket; in CI the read closed the connection and the
        # next chunk's settimeout raised EBADF on every try (PR #1126)
        for mode in ("length", "chunked"):
            loopback = loopback_server(green, mode)
            try:
                port = loopback.server_address[1]
                out = Path(tmp) / f"loopback-{mode}.json"
                lines = []
                code = fetch_jobs(dict(env, GITHUB_API_URL=f"http://127.0.0.1:{port}"), out,
                                  sleep=lambda s: None, log=lines.append)
                expect(f"loopback {mode}: exit {code} {lines}", code == 0)
                expect(f"loopback {mode}: one try {lines}", lines == ["JOB-LIST=OK 4 jobs (try 1/4)"])
                if code == 0:
                    ok, why = checker.verdict(out.read_text(encoding="utf-8"))
                    expect(f"loopback {mode}: checker {why}", ok)
            finally:
                loopback.shutdown()
                loopback.server_close()
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
