#!/usr/bin/env python3
"""Run one Windows executable under the Application Verifier Basics layers.

The script enables the Handles, Heaps, Locks, Memory, TLS, Threadpool and Leak
layers for the executable's image name, sets every stop those layers issue to
log to the verifier log file without a debugger (a stop ends the run with
STATUS_VERIFIER_STOP after it is logged), runs the executable, exports the
verifier log of every process the run starts with that image name (one log per
process: the suite runners start one child per case) to XML and disables
verification for the image again on every exit path. The machine-wide
-logtofile setting is put back the way the run found it, on every exit path
too. Verifier settings and logs are keyed by image name, machine wide, so the
run verifies a copy of the executable beside it under a name of its own
(verified_image_path) and removes the copy and its logs on exit. It exits
non-zero when the executable fails, when no log is exported, or when any log
holds a verifier stop, printing one line per stop with its layer, stop code
and top frames.
--xml FILE keeps the most recent log at FILE and the Nth log before it at FILE's
stem plus .N.

usage: python tools/windows/appverif_run.py [--xml FILE] [--timeout SECONDS] -- EXE [ARGS...]

Application Verifier settings live in the registry, so the script runs from an
elevated prompt. appverif.exe ships with the Windows SDK (Application Verifier
for x64).
"""
import argparse
import ctypes
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

# Stop report flags: log the stop and its stack trace to the log file; no
# debugger is attached.
STOP_ERROR_REPORT = "0x2"

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

# Frames from these modules are the verifier itself or the system call path
# that raised the stop; the frames shown start at the system API the caller
# entered.
SYSTEM_MODULES = {"vfbasics", "vrfcore", "verifier", "ntdll", "kernelbase", "kernel32"}


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

    appverif -configure sets the codes before the first code no registered
    provider issues, then fails naming that code and sets none after it. One
    call with every code therefore leaves every stop after the first unissued
    one at the default, which breaks into a debugger instead of logging the
    stop: the run would die on the stop and log nothing about it. Each call
    starts after the code the last call named, so there are at most len(codes)
    calls; any other failure raises, because a run whose stops are not all set
    to log is not a run that can report a stop.
    """
    remaining = [int(code, 16) for code in codes]
    configured = 0
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
            configured += len(remaining)
            remaining = []
            break
        output = proc.stdout + "\n" + proc.stderr
        code = unissued_stop_code(output)
        if code is None or code not in remaining:
            raise RuntimeError(
                f"appverif -configure failed (exit {proc.returncode}): {' '.join(output.split())}"
            )
        index = remaining.index(code)
        configured += index
        remaining = remaining[index + 1:]
    if remaining:
        raise RuntimeError(
            f"appverif -configure left {len(remaining)} stop codes unset for {image_name}"
        )
    if configured == 0:
        raise RuntimeError(f"appverif -configure set no stop for {image_name}")
    return configured


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
    except (FileNotFoundError, OSError):
        return False
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


def parse_stops(xml_path: Path) -> list:
    if not xml_path.is_file() or xml_path.stat().st_size == 0:
        return []
    root = ET.parse(xml_path).getroot()
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run an executable under Application Verifier Basics and fail on any stop."
    )
    parser.add_argument("--xml", help="write the exported verifier log to this file and keep it")
    parser.add_argument("--timeout", type=float, default=None, help="kill the run after this many seconds")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- EXE [ARGS...]")
    args = parser.parse_args()
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("missing executable to verify")

    exe_path = Path(command[0]).resolve()
    if not exe_path.is_file():
        print(f"appverif_run: executable not found: {exe_path}", file=sys.stderr)
        return 2
    try:
        appverif = find_appverif()
    except FileNotFoundError as error:
        print(f"appverif_run: {error}", file=sys.stderr)
        return 2
    suppress_crash_dialogs()

    verified_path = verified_image_path(exe_path, str(os.getpid()))
    try:
        shutil.copy2(exe_path, verified_path)
    except OSError as error:
        print(f"appverif_run: could not copy {exe_path} to {verified_path}: {error}", file=sys.stderr)
        return 2
    image_name = verified_path.name

    temp_dir = None
    if args.xml:
        xml_path = Path(args.xml).resolve()
    else:
        temp_dir = tempfile.mkdtemp(prefix="appverif_run_")
        xml_path = Path(temp_dir) / f"{image_name}.xml"

    stops = []
    exit_code = None
    elapsed = 0.0
    timed_out = False
    cleanup_failed = False
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
            exit_code = subprocess.run(
                [str(verified_path), *command[1:]], cwd=os.getcwd(), timeout=args.timeout
            ).returncode
        except subprocess.TimeoutExpired:
            timed_out = True
        elapsed = time.monotonic() - start

        log_paths = export_logs(appverif, image_name, exe_path, xml_path)
        if not log_paths:
            raise RuntimeError(f"no verifier log for {image_name} was exported to {xml_path}")
        for log_path in log_paths:
            stops.extend(parse_stops(log_path))
        print(f"appverif_run: {image_name} exported {len(log_paths)} verifier logs, one per process")
    except RuntimeError as error:
        print(f"appverif_run: {error}", file=sys.stderr)
        exit_code = exit_code if exit_code not in (None, 0) else 2
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
        left = enabled_layers(appverif, image_name)
        if left:
            cleanup_failed = True
            print(f"appverif_run: {image_name} is still under verification: {', '.join(left)}", file=sys.stderr)
        if temp_dir is not None:
            shutil.rmtree(temp_dir, ignore_errors=True)
        try:
            verified_path.unlink()
        except OSError as error:
            print(f"appverif_run: could not remove {verified_path}: {error}", file=sys.stderr)

    for stop in stops:
        frames = " <- ".join(stop["frames"]) if stop["frames"] else "<no frames>"
        print(
            f"appverif_run: STOP layer={stop['layer']} code={stop['stop_code']} "
            f"severity={stop['severity']} message={stop['message']!r} frames=[{frames}]",
            file=sys.stderr,
        )

    summary = f"{image_name} ({elapsed:.2f}s, {len(stops)} verifier stops"
    if timed_out:
        print(f"appverif_run: FAIL {summary}, timed out after {args.timeout}s)", file=sys.stderr)
        return 1
    if stops:
        print(f"appverif_run: FAIL {summary}, exit {exit_code})", file=sys.stderr)
        return 1
    if exit_code != 0 or cleanup_failed:
        print(f"appverif_run: FAIL {summary}, exit {exit_code})", file=sys.stderr)
        return exit_code if isinstance(exit_code, int) and exit_code > 0 else 1
    print(f"appverif_run: PASS {summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
