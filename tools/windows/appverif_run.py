#!/usr/bin/env python3
"""Run one Windows executable under the Application Verifier Basics layers.

The script enables the Handles, Heaps, Locks, Memory, TLS, Threadpool and Leak
layers for the executable's image name, sets every stop those layers issue to
active, logged to the verifier log file with its stack trace, and never
breaking into a debugger (STOP_ERROR_REPORT), runs the executable, exports the
verifier log of every process the run starts with that image name (one log per
process: the suite runners start one child per case) to XML and disables
verification for the image again on every exit path, deleting the image's own
Image File Execution Options key, which appverif leaves behind empty
(remove_image_key; a key it cannot delete fails the run). The machine-wide
-logtofile setting is put back the way the run found it, on every exit path
too. Verifier settings and logs are keyed by image name, machine wide, so the
run verifies a copy of the executable beside it under a name of its own
(verified_image_path) and removes the copy and its logs on exit.

A stop fails the run three independent ways (verdict):
  - a logEntry parsed from any exported log: FAIL, exit 1, one line per stop
    with its layer, stop code and top frames;
  - a process exit status of STATUS_VERIFIER_STOP (0xC0000421) with no stop
    parsed: "verifier stop not logged", exit 3 (the stop settings or the log
    reading are wrong; never a pass and never a plain test failure);
  - an exported log that is empty, unparseable or holds no logSession, or no
    log exported at all: "verifier log unreadable", exit 3 (a log that cannot
    be read is not zero stops).
A test failure (any other non-zero exit) is FAIL with the program's own exit
status, and a timeout is FAIL, exit 1.

The tool proves it can see a stop before it reports a pass. --self-check runs
three small programs built from tools/windows/appverif_selfcheck/ under the
same settings: a double CloseHandle (must yield a Handles logEntry), a write
one past a 17-byte HeapAlloc block (must yield a Heaps logEntry) and a clean
control (must exit 0 with a log that holds a session and no logEntry). Any
other outcome fails the invocation, exit 3, before the executable is run. A run
reports PASS only when the self-check passed in the same invocation, or when
--self-check-passed-at names a stamp a passing self-check wrote (--write-stamp)
for this same tool, the same appverif.exe and the same build: the plants beside
the executable, unchanged since the stamp. Without either, the run is made and
its stops reported, but it ends "no self-check", exit 3, never PASS.

--xml FILE keeps the most recent log at FILE and the Nth log before it at FILE's
stem plus .N; the self-check's logs are kept beside it as FILE's stem plus
.selfcheck.<program>.xml.

usage: python tools/windows/appverif_run.py [--xml FILE] [--timeout SECONDS]
           [--self-check [--plants DIR] [--write-stamp FILE] | --self-check-passed-at FILE]
           [-- EXE [ARGS...]]
--plants defaults to the directory of EXE. --self-check with no EXE runs the
self-check alone.

Application Verifier settings live in the registry, so the script runs from an
elevated prompt. appverif.exe ships with the Windows SDK (Application Verifier
for x64).
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

BASICS_LAYERS = ("Handles", "Heaps", "Locks", "Memory", "TLS", "Threadpool", "Leak")

# Stop code ranges the Basics layers issue. The ranges are generous on
# purpose: appverif applies every code a registered provider issues and names
# the rest as unknown, and it refuses a code no registered provider issues, so
# configure_stops() sets the codes before each one appverif names that way.
STOP_CODE_RANGES = (
    (0x0001, 0x0020),  # Heaps
    (0x0200, 0x0220),  # Locks
    (0x0300, 0x0310),  # Handles
    (0x0350, 0x0360),  # TLS
    (0x0600, 0x0640),  # Memory
    (0x0700, 0x0720),  # Threadpool
    (0x0900, 0x0910),  # Leak
)

# The ErrorReport bits of `appverif -configure ... -with ErrorReport=VALUE`, as
# Application Verifier's own help (appverif.chm, "-configure") defines them.
STOP_ACTIVE = 0x001           # the stop is active; with this bit clear the stop is disabled
STOP_BREAKPOINT = 0x020       # break into a debugger with a breakpoint
STOP_EXCEPTION = 0x040        # break into a debugger with a verifier exception
STOP_LOG = 0x080              # log the stop to the log file
STOP_LOG_STACK_TRACE = 0x100  # log the stop's stack trace to the log file

# Active, logged with its stack trace, and no break: 0x181. No debugger is
# attached, so either break bit (0x20 or 0x40) would end the process on the
# first stop, continuable or not, with an unhandled breakpoint or verifier
# exception; without them a continuable stop (an invalid handle) is logged and
# the program goes on to log the next, and a non-continuable one (heap
# corruption) is logged and ends the process with STATUS_VERIFIER_STOP. Until
# 2026-11-12 this was 0x2, a bit the help does not define, with STOP_ACTIVE
# clear: every stop was disabled and every run logged nothing.
STOP_ERROR_REPORT_BITS = STOP_ACTIVE | STOP_LOG | STOP_LOG_STACK_TRACE
STOP_ERROR_REPORT = f"0x{STOP_ERROR_REPORT_BITS:X}"

# The process exit status of a process the verifier ended on a stop.
STATUS_VERIFIER_STOP = 0xC0000421

# The verdict's exit statuses beside the program's own: 1 is a stop, a
# timeout or a cleanup failure; 2 is a run that could not be set up; 3 means
# the run cannot say whether there was a stop (the stop was not logged, a log
# could not be read, or the tool has not shown in this build that it sees one).
EXIT_FAIL = 1
EXIT_SETUP = 2
EXIT_UNPROVEN = 3

AVRF_NS = "{Application Verifier}"
FRAMES_SHOWN = 8

# The export of one process's verifier log per process the run starts; more
# than this many logs is a runaway, not a suite.
MAX_EXPORTED_LOGS = 4096

# appverif -configure's error for a stop code no registered provider issues.
UNISSUED_STOP_PATTERN = re.compile(
    r"Stop code \[(0x[0-9a-fA-F]+)\][^\n]*not issued by any of the registered providers",
    re.IGNORECASE,
)

# Where appverif -logtofile keeps its one machine-wide setting (LogToFile).
GLOBAL_SETTINGS_KEY = (
    r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
    r"\{ApplicationVerifierGlobalSettings}"
)

# Where appverif -enable writes a verified image's settings: one key per image
# name under this one (GlobalFlag, VerifierDlls, VerifierFlags and one subkey
# per layer). appverif -disable and -delete settings clear the values and the
# layer subkeys but leave the image's key itself, empty (measured on captain,
# 2026-11-11: one empty key per run), so the run deletes that key too.
IFEO_KEY = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"

# The only image names whose IFEO key the run may delete: the copies
# verified_image_path names. Any other image's key is never touched.
VERIFIED_IMAGE_PATTERN = re.compile(r".+_appverif_[0-9]+\.exe", re.IGNORECASE)

# Frames from these modules are the verifier itself or the system call path
# that raised the stop; the frames shown start at the system API the caller
# entered.
SYSTEM_MODULES = {"vfbasics", "vrfcore", "verifier", "ntdll", "kernelbase", "kernel32"}

# The self-check's programs (sources in tools/windows/appverif_selfcheck/,
# built by CMakeLists.txt on Windows beside the test executables) and the
# layer each must be reported by; None is the clean control, which must be
# reported by none.
SELF_CHECK_PROGRAMS = (
    ("appverif_plant_double_close.exe", "Handles"),
    ("appverif_plant_heap_overrun.exe", "Heaps"),
    ("appverif_plant_clean.exe", None),
)

STAMP_VERSION = 1


def frame_module(frame: str) -> str:
    return frame.split("!", 1)[0].strip().lower()


def top_frames(frames: list) -> list:
    first_caller = next(
        (index for index, frame in enumerate(frames) if frame_module(frame) not in SYSTEM_MODULES),
        None,
    )
    if first_caller is None:
        return frames[:FRAMES_SHOWN]
    start = first_caller
    while start > 0 and frame_module(frames[start - 1]) in {"vfbasics", "vrfcore", "verifier"}:
        start -= 1
    api = [frames[start - 1]] if start > 0 else []
    callers = [frame for frame in frames[first_caller:] if frame_module(frame) not in SYSTEM_MODULES]
    return (api + callers)[:FRAMES_SHOWN]


def verified_image_path(exe_path: Path, token: str) -> Path:
    """The copy of exe_path a run verifies, beside it, under a name of its own.

    Application Verifier keys its settings and logs by image name, machine
    wide: while functional.exe is under verification every functional.exe on
    the host (a CI leg, another worker's build) is verified too, its logs land
    beside this run's, and its own -delete logs removes this run's. Beside the
    original, the copy finds the same PDB and the same sibling executables.
    """
    return exe_path.with_name(f"{exe_path.stem}_appverif_{token}{exe_path.suffix}")


class WinregKeys:
    """The HKLM keys the run deletes, through winreg (64-bit view)."""

    def __init__(self):
        import winreg

        self.winreg = winreg

    def exists(self, path: str) -> bool:
        try:
            with self.winreg.OpenKey(
                self.winreg.HKEY_LOCAL_MACHINE, path, 0,
                self.winreg.KEY_READ | self.winreg.KEY_WOW64_64KEY,
            ):
                return True
        except FileNotFoundError:
            return False

    def subkeys(self, path: str) -> list:
        names = []
        with self.winreg.OpenKey(
            self.winreg.HKEY_LOCAL_MACHINE, path, 0,
            self.winreg.KEY_READ | self.winreg.KEY_WOW64_64KEY,
        ) as key:
            index = 0
            while True:
                try:
                    names.append(self.winreg.EnumKey(key, index))
                except OSError:
                    return names
                index += 1

    def delete(self, path: str) -> None:
        self.winreg.DeleteKeyEx(self.winreg.HKEY_LOCAL_MACHINE, path, self.winreg.KEY_WOW64_64KEY, 0)


def image_key_path(image_name: str) -> str:
    """The IFEO key of a verified copy; refuses any other image name."""
    if not VERIFIED_IMAGE_PATTERN.fullmatch(image_name) or "\\" in image_name or "/" in image_name:
        raise ValueError(f"not a verified copy's image name: {image_name!r}")
    return f"{IFEO_KEY}\\{image_name}"


def delete_key_tree(keys, path: str) -> None:
    for name in keys.subkeys(path):
        delete_key_tree(keys, f"{path}\\{name}")
    keys.delete(path)


def remove_image_key(keys, image_name: str) -> bool:
    """Delete the run's IFEO key for image_name with every subkey; True once it is gone.

    The image name is the run's own copy (verified_image_path, the run's pid), so
    the key holds nothing but what this run's appverif calls wrote. A key that
    cannot be deleted is reported by the caller as a cleanup failure.
    """
    path = image_key_path(image_name)
    try:
        if keys.exists(path):
            delete_key_tree(keys, path)
        return not keys.exists(path)
    except OSError:
        return False


def find_appverif() -> str:
    found = shutil.which("appverif.exe") or shutil.which("appverif")
    if found:
        return found
    candidate = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "appverif.exe"
    if candidate.is_file():
        return str(candidate)
    raise FileNotFoundError(
        "appverif.exe not found: install Application Verifier for x64 from the Windows SDK"
    )


def suppress_crash_dialogs() -> None:
    if sys.platform == "win32":
        sem_failcriticalerrors = 0x0001
        sem_nogpfaulterrorbox = 0x0002
        sem_noopenfileerrorbox = 0x8000
        ctypes.windll.kernel32.SetErrorMode(
            sem_failcriticalerrors | sem_nogpfaulterrorbox | sem_noopenfileerrorbox
        )


def appverif_call(appverif: str, args: list, check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        [appverif, *args], capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    if check and proc.returncode != 0:
        output = " ".join((proc.stdout + " " + proc.stderr).split())
        raise RuntimeError(f"appverif {' '.join(args)} failed (exit {proc.returncode}): {output}")
    return proc


def stop_codes() -> list:
    return [f"0x{code:X}" for low, high in STOP_CODE_RANGES for code in range(low, high + 1)]


def unissued_stop_code(output: str):
    """The stop code appverif -configure names as issued by no provider, else None."""
    match = UNISSUED_STOP_PATTERN.search(output)
    if match is None:
        return None
    return int(match.group(1), 16)


def configure_stops(appverif: str, image_name: str, codes: list) -> int:
    """Set every issued stop in codes to STOP_ERROR_REPORT; return how many were set.

    appverif -configure fails naming the first code no registered provider
    issues, and the codes it keeps from that call are not part of its
    contract. One call with every code therefore leaves the stops around the
    first unissued one depending on what a failed call did, which breaks into
    a debugger instead of logging the stop: the run would die on the stop and
    log nothing about it. Each retry names every code but the one the last
    call named, so a code set by an earlier call is set again (idempotent) and
    a code that a failed call skipped is set too. There are at most len(codes)
    calls; any other failure raises, because a run whose stops are not all set
    to log is not a run that can report a stop.
    """
    remaining = [int(code, 16) for code in codes]
    for _ in range(len(codes) + 1):
        if not remaining:
            break
        proc = appverif_call(
            appverif,
            [
                "-configure",
                *[f"0x{code:X}" for code in remaining],
                "-for",
                image_name,
                "-with",
                f"ErrorReport={STOP_ERROR_REPORT}",
            ],
            check=False,
        )
        if proc.returncode == 0:
            return len(remaining)
        output = proc.stdout + "\n" + proc.stderr
        code = unissued_stop_code(output)
        if code is None or code not in remaining:
            raise RuntimeError(
                f"appverif -configure failed (exit {proc.returncode}): {' '.join(output.split())}"
            )
        remaining.remove(code)
    if remaining:
        raise RuntimeError(
            f"appverif -configure left {len(remaining)} stop codes unset for {image_name}"
        )
    raise RuntimeError(f"appverif -configure set no stop for {image_name}")


def logtofile_enabled():
    """Whether appverif -logtofile is enabled machine-wide, else None if unknown.

    The setting is one machine-wide value, not one per image: a run that
    turns it on and leaves it on changes what every later run on the host
    logs. None means the value could not be read, and the run leaves it as it
    found it rather than guessing.
    """
    if sys.platform != "win32":
        return None
    import winreg

    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            GLOBAL_SETTINGS_KEY,
            0,
            winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
        ) as key:
            value, _ = winreg.QueryValueEx(key, "LogToFile")
    except FileNotFoundError:
        # no key and no value is the setting the run found: not enabled
        return False
    except OSError:
        # a read the run was not allowed to make is not the value zero: leave
        # the machine-wide setting alone rather than disable it on a guess
        return None
    if isinstance(value, str):
        return value.strip() not in ("", "0")
    return bool(value)


def enabled_layers(appverif: str, image_name: str) -> list:
    proc = appverif_call(appverif, ["-query", "*", "-for", image_name], check=False)
    layers = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("Test [") and line.endswith("] enabled."):
            layers.append(line[len("Test ["):-len("] enabled.")])
    return layers


class LogUnreadable(Exception):
    """An exported verifier log that cannot say how many stops it holds."""


def parse_stops(xml_path: Path) -> list:
    """The stops one exported verifier log holds; raises LogUnreadable when it cannot say.

    A log the export wrote always holds the process's logSession, with its
    logEntry elements when there were stops. A missing or empty file, XML that
    does not parse, or a log with no logSession is not a log with zero stops:
    it is a run whose stops are unknown.
    """
    if not xml_path.is_file():
        raise LogUnreadable(f"{xml_path}: no such log")
    if xml_path.stat().st_size == 0:
        raise LogUnreadable(f"{xml_path}: empty log")
    try:
        root = ET.parse(xml_path).getroot()
    except ET.ParseError as error:
        raise LogUnreadable(f"{xml_path}: not a verifier log ({error})") from error
    if next(root.iter(f"{AVRF_NS}logSession"), None) is None:
        raise LogUnreadable(f"{xml_path}: no logSession in the log")
    stops = []
    for entry in root.iter(f"{AVRF_NS}logEntry"):
        frames = [
            trace.text.strip()
            for trace in entry.iter(f"{AVRF_NS}trace")
            if trace.text and trace.text.strip()
        ]
        stops.append(
            {
                "layer": entry.attrib.get("LayerName", "Unknown"),
                "stop_code": entry.attrib.get("StopCode", "Unknown"),
                "severity": entry.attrib.get("Severity", "Unknown"),
                "message": " ".join((entry.findtext(f"{AVRF_NS}message") or "").split()),
                "frames": top_frames(frames),
            }
        )
    return stops


def read_logs(log_paths: list):
    """(stops, unreadable): every stop of every log, and one line per log that cannot be read."""
    stops = []
    unreadable = []
    if not log_paths:
        unreadable.append("no verifier log was exported")
    for log_path in log_paths:
        try:
            stops.extend(parse_stops(log_path))
        except LogUnreadable as error:
            unreadable.append(str(error))
    return stops, unreadable


def is_verifier_stop_status(exit_code) -> bool:
    """Whether a process exit status is STATUS_VERIFIER_STOP, signed or unsigned."""
    return isinstance(exit_code, int) and (exit_code & 0xFFFFFFFF) == STATUS_VERIFIER_STOP


class RunResult:
    """What one run under the verifier left: its exit, its logs' stops, and what went wrong."""

    def __init__(self, image_name: str = "", exit_code=None, elapsed: float = 0.0, timed_out: bool = False,
                 stops=None, unreadable=None, setup_error=None, cleanup_failed: bool = False, logs: int = 0):
        self.image_name = image_name
        self.exit_code = exit_code
        self.elapsed = elapsed
        self.timed_out = timed_out
        self.stops = list(stops or [])
        self.unreadable = list(unreadable or [])
        self.setup_error = setup_error
        self.cleanup_failed = cleanup_failed
        self.logs = logs


def stop_line(stop: dict) -> str:
    frames = " <- ".join(stop["frames"]) if stop["frames"] else "<no frames>"
    return (
        f"appverif_run: STOP layer={stop['layer']} code={stop['stop_code']} "
        f"severity={stop['severity']} message={stop['message']!r} frames=[{frames}]"
    )


def verdict(result: RunResult, self_checked: bool):
    """(exit status, lines) for one run; PASS only with every check clean and a self-check behind it.

    Each way a stop can show is its own failure: a parsed stop (EXIT_FAIL), a
    STATUS_VERIFIER_STOP exit with no stop parsed and a log that cannot be read
    (both EXIT_UNPROVEN), so a stop the log does not hold never passes and is
    never mistaken for a test failure.
    """
    lines = [stop_line(stop) for stop in result.stops]
    summary = f"{result.image_name} ({result.elapsed:.2f}s, {len(result.stops)} verifier stops in {result.logs} logs"
    if result.setup_error is not None:
        lines.append(f"appverif_run: FAIL {summary}): could not run under the verifier: {result.setup_error}")
        return EXIT_SETUP, lines
    if result.stops:
        lines.append(f"appverif_run: FAIL {summary}, exit {result.exit_code})")
        return EXIT_FAIL, lines
    if is_verifier_stop_status(result.exit_code):
        lines.append(
            f"appverif_run: FAIL {summary}, exit 0x{STATUS_VERIFIER_STOP:X}): verifier stop not logged: "
            "the process was ended by a verifier stop that no log holds; the stop settings or the log reading are wrong"
        )
        return EXIT_UNPROVEN, lines
    if result.unreadable:
        for problem in result.unreadable:
            lines.append(f"appverif_run: LOG {problem}")
        lines.append(
            f"appverif_run: FAIL {summary}, exit {result.exit_code}): verifier log unreadable: "
            f"{len(result.unreadable)} logs cannot say whether there was a stop"
        )
        return EXIT_UNPROVEN, lines
    if result.timed_out:
        lines.append(f"appverif_run: FAIL {summary}, timed out)")
        return EXIT_FAIL, lines
    if result.exit_code != 0 or result.cleanup_failed:
        lines.append(f"appverif_run: FAIL {summary}, exit {result.exit_code})")
        code = result.exit_code
        return (code if isinstance(code, int) and code > 0 else EXIT_FAIL), lines
    if not self_checked:
        lines.append(
            f"appverif_run: FAIL {summary}, exit 0): no self-check: a PASS needs --self-check in the same "
            "invocation or --self-check-passed-at with a stamp from this build"
        )
        return EXIT_UNPROVEN, lines
    lines.append(f"appverif_run: PASS {summary})")
    return 0, lines


def judge_self_check(program: str, expected_layer, result: RunResult) -> list:
    """The problems with one self-check program's run; none means it behaved as planted."""
    problems = []
    if result.setup_error is not None:
        return [f"{program}: could not run under the verifier: {result.setup_error}"]
    if result.timed_out:
        problems.append(f"{program}: timed out")
    for problem in result.unreadable:
        problems.append(f"{program}: verifier log unreadable: {problem}")
    if result.cleanup_failed:
        problems.append(f"{program}: cleanup failed")
    layers = sorted({stop["layer"] for stop in result.stops})
    if expected_layer is None:
        if result.stops:
            problems.append(f"{program}: the clean control yielded {len(result.stops)} stops (layers {', '.join(layers)})")
        if result.exit_code != 0:
            problems.append(f"{program}: the clean control exited {result.exit_code}")
        return problems
    if not any(stop["layer"].lower() == expected_layer.lower() for stop in result.stops):
        seen = ", ".join(layers) if layers else "none"
        unlogged = " (ended by STATUS_VERIFIER_STOP: verifier stop not logged)" if is_verifier_stop_status(result.exit_code) else ""
        problems.append(f"{program}: no {expected_layer} logEntry (layers logged: {seen}; exit {result.exit_code}){unlogged}")
    return problems


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def same_dir(first: Path, second: Path) -> bool:
    return os.path.normcase(str(first.resolve())) == os.path.normcase(str(second.resolve()))


def make_stamp(tool_sha: str, appverif_sha: str, plants_dir: Path, plant_shas: dict, passed_at: str) -> dict:
    return {
        "version": STAMP_VERSION,
        "tool_sha256": tool_sha,
        "appverif_sha256": appverif_sha,
        "stop_error_report": STOP_ERROR_REPORT,
        "plants_dir": str(plants_dir.resolve()),
        "plants": dict(plant_shas),
        "passed_at": passed_at,
    }


def check_stamp(stamp, tool_sha: str, appverif_sha: str, exe_dir: Path, plant_sha) -> list:
    """The reasons a self-check stamp does not cover this run; none means it does.

    The stamp covers a run of this tool with these settings and this
    appverif.exe, over an executable of the same build: one whose directory
    holds the self-check programs, each unchanged since the self-check ran
    them. plant_sha(path) is the current digest of a file, None when absent.
    """
    if not isinstance(stamp, dict) or stamp.get("version") != STAMP_VERSION:
        return ["not a self-check stamp of this version"]
    problems = []
    if stamp.get("tool_sha256") != tool_sha:
        problems.append("written by another version of appverif_run.py")
    if stamp.get("appverif_sha256") != appverif_sha:
        problems.append("written with another appverif.exe")
    if stamp.get("stop_error_report") != STOP_ERROR_REPORT:
        problems.append(f"written with ErrorReport={stamp.get('stop_error_report')}, not {STOP_ERROR_REPORT}")
    plants_dir = stamp.get("plants_dir")
    if not isinstance(plants_dir, str) or not same_dir(Path(plants_dir), exe_dir):
        problems.append(f"its self-check programs were in {plants_dir}, not beside this executable in {exe_dir}: another build")
    plants = stamp.get("plants")
    if not isinstance(plants, dict) or sorted(plants) != sorted(name for name, _ in SELF_CHECK_PROGRAMS):
        problems.append("does not name every self-check program")
    else:
        for name, digest in sorted(plants.items()):
            current = plant_sha(exe_dir / name)
            if current != digest:
                problems.append(f"{name} beside this executable is not the one the self-check ran: another build")
    return problems


def run_under_verifier(appverif: str, exe_path: Path, args: list, xml_path, timeout) -> RunResult:
    """Run exe_path once under the Basics layers and read every log it leaves.

    xml_path keeps the logs when given; otherwise they go to a temporary
    directory that is removed. Every exit path takes the image's settings back.
    """
    result = RunResult()
    verified_path = verified_image_path(exe_path, str(os.getpid()))
    try:
        shutil.copy2(exe_path, verified_path)
    except OSError as error:
        result.setup_error = f"could not copy {exe_path} to {verified_path}: {error}"
        return result
    image_name = verified_path.name
    result.image_name = image_name

    temp_dir = None
    if xml_path is None:
        temp_dir = tempfile.mkdtemp(prefix="appverif_run_")
        xml_path = Path(temp_dir) / f"{image_name}.xml"

    logtofile_was_enabled = None
    try:
        logtofile_was_enabled = logtofile_enabled()
        appverif_call(appverif, ["-logtofile", "enable"])
        appverif_call(appverif, ["-delete", "logs", "-for", image_name], check=False)
        appverif_call(appverif, ["-enable", *BASICS_LAYERS, "-for", image_name])
        configure_stops(appverif, image_name, stop_codes())
        missing = [layer for layer in BASICS_LAYERS if layer not in enabled_layers(appverif, image_name)]
        if missing:
            raise RuntimeError(f"layers not enabled for {image_name}: {', '.join(missing)}")

        start = time.monotonic()
        try:
            result.exit_code = subprocess.run(
                [str(verified_path), *args], cwd=os.getcwd(), timeout=timeout
            ).returncode
        except subprocess.TimeoutExpired:
            result.timed_out = True
        result.elapsed = time.monotonic() - start

        log_paths = export_logs(appverif, image_name, exe_path, xml_path)
        result.logs = len(log_paths)
        result.stops, result.unreadable = read_logs(log_paths)
        print(f"appverif_run: {image_name} exported {len(log_paths)} verifier logs, one per process")
    except RuntimeError as error:
        result.setup_error = str(error)
    finally:
        appverif_call(appverif, ["-delete", "logs", "-for", image_name], check=False)
        appverif_call(appverif, ["-disable", "*", "-for", image_name], check=False)
        appverif_call(appverif, ["-delete", "settings", "-for", image_name], check=False)
        if logtofile_was_enabled is not None:
            # the machine-wide logging setting is put back the way the run
            # found it: a run that leaves it on changes what every later run
            # on this host logs
            appverif_call(
                appverif,
                ["-logtofile", "enable" if logtofile_was_enabled else "disable"],
                check=False,
            )
        # the image's own key, which appverif leaves behind empty, and anything
        # a failed -disable left in it: the name is this run's copy, so the
        # key holds only what this run's appverif calls wrote
        if sys.platform == "win32" and not remove_image_key(WinregKeys(), image_name):
            result.cleanup_failed = True
            print(f"appverif_run: could not remove HKLM\\{image_key_path(image_name)}", file=sys.stderr)
        left = enabled_layers(appverif, image_name)
        if left:
            result.cleanup_failed = True
            print(f"appverif_run: {image_name} is still under verification: {', '.join(left)}", file=sys.stderr)
        if temp_dir is not None:
            shutil.rmtree(temp_dir, ignore_errors=True)
        try:
            verified_path.unlink()
        except OSError as error:
            print(f"appverif_run: could not remove {verified_path}: {error}", file=sys.stderr)
    return result


def log_path_for(xml_path: Path, offset: int) -> Path:
    if offset == 0:
        return xml_path
    return xml_path.with_name(f"{xml_path.stem}.{-offset}{xml_path.suffix}")


def export_logs(appverif: str, image_name: str, exe_path: Path, xml_path: Path) -> list:
    """Export every verifier log of the image, the most recent first.

    appverif keeps one log per process, and `-export log` exports only the most
    recent one; Log=-N names the Nth log before it and fails once no log is
    left. Every process the run starts with this image name (the suite runners
    start one child per case) has its own log. The logs are deleted before the
    run, so every log exported belongs to it. More than MAX_EXPORTED_LOGS logs
    raise rather than loop on.
    """
    paths = []
    for index in range(MAX_EXPORTED_LOGS + 1):
        offset = -index
        path = log_path_for(xml_path, offset)
        if path.exists():
            path.unlink()
        proc = appverif_call(
            appverif,
            [
                "-export", "log", "-for", image_name,
                "-with", f"To={path}", f"Symbols={exe_path.parent}", f"Log={offset}",
            ],
            check=False,
        )
        if proc.returncode != 0 or not path.is_file():
            return paths
        if len(paths) == MAX_EXPORTED_LOGS:
            path.unlink()
            raise RuntimeError(
                f"more than {MAX_EXPORTED_LOGS} verifier logs for {image_name}; refusing an unbounded export"
            )
        paths.append(path)
    raise RuntimeError(f"verifier log export limit reached for {image_name}")


def self_check(appverif: str, plants_dir: Path, xml_path, timeout) -> list:
    """Run each self-check program under the run's settings; the problems, none when all behaved."""
    problems = []
    for program, expected_layer in SELF_CHECK_PROGRAMS:
        exe = plants_dir / program
        if not exe.is_file():
            problems.append(f"{program}: not found in {plants_dir} (built by CMakeLists.txt on Windows)")
            continue
        log = None
        if xml_path is not None:
            log = xml_path.with_name(f"{xml_path.stem}.selfcheck.{exe.stem}{xml_path.suffix}")
        result = run_under_verifier(appverif, exe, [], log, timeout)
        layers = ", ".join(f"{stop['layer']} {stop['stop_code']}" for stop in result.stops) or "no stop"
        print(f"appverif_run: self-check {program}: exit {result.exit_code}, {result.logs} logs, {layers}")
        found = judge_self_check(program, expected_layer, result)
        for stop in result.stops:
            print(stop_line(stop))
        problems.extend(found)
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run an executable under Application Verifier Basics and fail on any stop."
    )
    parser.add_argument("--xml", help="write the exported verifier log to this file and keep it")
    parser.add_argument("--timeout", type=float, default=None, help="kill the run after this many seconds")
    parser.add_argument("--self-check", action="store_true",
                        help="first show, on planted programs, that a stop is seen under these settings")
    parser.add_argument("--plants", help="the directory of the self-check programs (default: EXE's directory)")
    parser.add_argument("--write-stamp", help="after a passing self-check, write a stamp to this file")
    parser.add_argument("--self-check-passed-at",
                        help="a stamp a passing self-check of this build wrote; stands for --self-check")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- EXE [ARGS...]")
    args = parser.parse_args()
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command and not args.self_check:
        parser.error("missing executable to verify")
    if args.self_check and args.self_check_passed_at:
        parser.error("--self-check and --self-check-passed-at are alternatives")
    if args.write_stamp and not args.self_check:
        parser.error("--write-stamp needs --self-check")

    exe_path = Path(command[0]).resolve() if command else None
    if exe_path is not None and not exe_path.is_file():
        print(f"appverif_run: executable not found: {exe_path}", file=sys.stderr)
        return EXIT_SETUP
    if args.plants:
        plants_dir = Path(args.plants).resolve()
    elif exe_path is not None:
        plants_dir = exe_path.parent
    else:
        parser.error("--self-check with no executable needs --plants")
    try:
        appverif = find_appverif()
    except FileNotFoundError as error:
        print(f"appverif_run: {error}", file=sys.stderr)
        return EXIT_SETUP
    suppress_crash_dialogs()
    xml_path = Path(args.xml).resolve() if args.xml else None
    tool_sha = file_sha256(Path(__file__).resolve())
    appverif_sha = file_sha256(Path(appverif))

    self_checked = False
    if args.self_check_passed_at:
        try:
            stamp = json.loads(Path(args.self_check_passed_at).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            print(f"appverif_run: FAIL self-check stamp {args.self_check_passed_at} unreadable: {error}", file=sys.stderr)
            return EXIT_UNPROVEN
        problems = check_stamp(stamp, tool_sha, appverif_sha, exe_path.parent,
                               lambda path: file_sha256(path) if path.is_file() else None)
        if problems:
            for problem in problems:
                print(f"appverif_run: self-check stamp {args.self_check_passed_at}: {problem}", file=sys.stderr)
            print("appverif_run: FAIL the self-check stamp does not cover this run", file=sys.stderr)
            return EXIT_UNPROVEN
        print(f"appverif_run: self-check passed at {stamp.get('passed_at')} for this build ({args.self_check_passed_at})")
        self_checked = True

    if args.self_check:
        problems = self_check(appverif, plants_dir, xml_path, args.timeout)
        if problems:
            for problem in problems:
                print(f"appverif_run: self-check: {problem}", file=sys.stderr)
            print(f"appverif_run: FAIL self-check: {len(problems)} problems; this tool cannot show a stop here, "
                  "so no run under it is reported", file=sys.stderr)
            return EXIT_UNPROVEN
        print(f"appverif_run: self-check PASS: Handles and Heaps stops logged, clean control clean "
              f"(ErrorReport={STOP_ERROR_REPORT})")
        self_checked = True
        if args.write_stamp:
            plant_shas = {name: file_sha256(plants_dir / name) for name, _ in SELF_CHECK_PROGRAMS}
            stamp = make_stamp(tool_sha, appverif_sha, plants_dir, plant_shas,
                               time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
            Path(args.write_stamp).write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
            print(f"appverif_run: self-check stamp written to {args.write_stamp}")
        if exe_path is None:
            return 0

    result = run_under_verifier(appverif, exe_path, command[1:], xml_path, args.timeout)
    status, lines = verdict(result, self_checked)
    for line in lines:
        print(line, file=sys.stdout if status == 0 else sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
