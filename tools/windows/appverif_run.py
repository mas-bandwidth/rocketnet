#!/usr/bin/env python3
"""Run one Windows executable under the Application Verifier Basics layers.

The script enables the Handles, Heaps, Locks, Memory, TLS, Threadpool and Leak
layers for the executable's image name, sets every stop those layers issue to
log to the verifier log file without a debugger (a stop ends the run with
STATUS_VERIFIER_STOP after it is logged), runs the executable, exports the
verifier log of every process the run starts with that image name (one log per
process: the suite runners start one child per case) to XML and disables
verification for the image again on every exit path. It exits non-zero when the
executable fails, when no log is exported, or when any log holds a verifier
stop, printing one line per stop with its layer, stop code and top frames.
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
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

BASICS_LAYERS = ("Handles", "Heaps", "Locks", "Memory", "TLS", "Threadpool", "Leak")

# Stop code ranges the Basics layers issue. appverif applies every code a
# registered provider issues and names the rest as unknown, so the ranges are
# generous on purpose.
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
MAX_EXPORTED_LOGS = 4096

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
    """Export verifier logs in reverse order, up to the safety limit."""
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


def terminate_process_tree(process: subprocess.Popen) -> None:
    """Stop the timed-out executable and every child it started on Windows."""
    if sys.platform != "win32":
        process.kill()
        process.wait()
        return

    taskkill = shutil.which("taskkill.exe")
    if taskkill is None:
        candidate = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "taskkill.exe"
        if candidate.is_file():
            taskkill = str(candidate)
    if taskkill is None:
        raise RuntimeError("taskkill.exe not found; cannot stop the timed-out process tree")

    stopped = subprocess.run(
        [taskkill, "/PID", str(process.pid), "/T", "/F"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    if stopped.returncode != 0:
        output = " ".join((stopped.stdout + " " + stopped.stderr).split())
        raise RuntimeError(f"taskkill failed to stop process tree {process.pid} (exit {stopped.returncode}): {output}")


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
    image_name = exe_path.name

    try:
        appverif = find_appverif()
    except FileNotFoundError as error:
        print(f"appverif_run: {error}", file=sys.stderr)
        return 2
    suppress_crash_dialogs()

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
    logtofile_attempted = False
    try:
        logtofile_attempted = True
        appverif_call(appverif, ["-logtofile", "enable"])
        appverif_call(appverif, ["-delete", "logs", "-for", image_name], check=False)
        appverif_call(appverif, ["-enable", *BASICS_LAYERS, "-for", image_name])
        appverif_call(
            appverif,
            ["-configure", *stop_codes(), "-for", image_name, "-with", f"ErrorReport={STOP_ERROR_REPORT}"],
            check=False,
        )
        missing = [layer for layer in BASICS_LAYERS if layer not in enabled_layers(appverif, image_name)]
        if missing:
            raise RuntimeError(f"layers not enabled for {image_name}: {', '.join(missing)}")

        start = time.monotonic()
        process = subprocess.Popen([str(exe_path), *command[1:]], cwd=os.getcwd())
        try:
            exit_code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            terminate_process_tree(process)
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
        appverif_call(appverif, ["-disable", "*", "-for", image_name], check=False)
        appverif_call(appverif, ["-delete", "settings", "-for", image_name], check=False)
        if logtofile_attempted:
            try:
                appverif_call(appverif, ["-logtofile", "disable"])
            except RuntimeError as error:
                cleanup_failed = True
                print(f"appverif_run: failed to restore -logtofile: {error}", file=sys.stderr)
        left = enabled_layers(appverif, image_name)
        if left:
            cleanup_failed = True
            print(f"appverif_run: {image_name} is still under verification: {', '.join(left)}", file=sys.stderr)
        if temp_dir is not None:
            shutil.rmtree(temp_dir, ignore_errors=True)

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
