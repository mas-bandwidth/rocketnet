#!/usr/bin/env python3
"""Run a Windows executable under Application Verifier Basics layers and fail on any stop."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

BASICS_LAYERS = (
    "Handles",
    "Heaps",
    "Locks",
    "Memory",
    "TLS",
    "Threadpool",
    "Leak",
)
AVRF_NS = "{Application Verifier}"


def find_appverif() -> str:
    found = shutil.which("appverif.exe") or shutil.which("appverif")
    if found:
        return found
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    candidate = Path(system_root) / "System32" / "appverif.exe"
    if candidate.is_file():
        return str(candidate)
    raise FileNotFoundError(
        "appverif.exe not found; install Application Verifier from the Windows SDK"
    )


def suppress_crash_dialogs() -> None:
    if sys.platform == "win32":
        sem_failcriticalerrors = 0x0001
        sem_nogpfaulterrorbox = 0x0002
        sem_noopenfileerrorbox = 0x8000
        ctypes.windll.kernel32.SetErrorMode(
            sem_failcriticalerrors | sem_nogpfaulterrorbox | sem_noopenfileerrorbox
        )


def run_appverif(appverif: str, args: list[str], check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        [appverif, *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"appverif { .join(args)} failed (exit {proc.returncode}): "
            f"{proc.stdout.strip()} {proc.stderr.strip()}"
        )
    return proc


def parse_stops(xml_path: Path) -> list[dict]:
    if not xml_path.is_file() or xml_path.stat().st_size == 0:
        return []
    tree = ET.parse(xml_path)
    root = tree.getroot()
    stops = []
    for entry in root.iter(f"{AVRF_NS}logEntry"):
        layer = entry.attrib.get("LayerName", "Unknown")
        stop_code = entry.attrib.get("StopCode", "Unknown")
        severity = entry.attrib.get("Severity", "Unknown")
        message = (entry.findtext(f"{AVRF_NS}message") or "").strip()
        traces = [
            t.text.strip()
            for t in entry.findall(f".//{AVRF_NS}trace")
            if t.text and t.text.strip()
        ]
        stops.append(
            {
                "layer": layer,
                "stop_code": stop_code,
                "severity": severity,
                "message": message,
                "frames": traces[:8],
            }
        )
    return stops


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--xml",
        help="Path to write the exported Application Verifier XML log.",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Executable and arguments to run under Application Verifier.",
    )
    args = parser.parse_args()
    cmd = list(args.command)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        parser.error("missing executable to verify")

    exe_path = Path(cmd[0]).resolve()
    if not exe_path.is_file():
        print(f"appverif_run: executable not found: {exe_path}", file=sys.stderr)
        return 2

    image_name = exe_path.name
    symbol_dir = str(exe_path.parent)
    appverif = find_appverif()
    suppress_crash_dialogs()

    temp_xml = None
    if args.xml:
        xml_path = Path(args.xml).resolve()
    else:
        fd, temp_name = tempfile.mkstemp(prefix=f"avrf_{image_name}_", suffix=".xml")
        os.close(fd)
        temp_xml = Path(temp_name).resolve()
        xml_path = temp_xml

    stops = []
    proc_rc = 1
    elapsed = 0.0
    try:
        run_appverif(appverif, ["-logtofile", "enable"])
        run_appverif(appverif, ["-delete", "logs", "-for", image_name], check=False)
        run_appverif(appverif, ["-enable", *BASICS_LAYERS, "-for", image_name])

        t0 = time.monotonic()
        proc = subprocess.run([str(exe_path), *cmd[1:]])
        elapsed = time.monotonic() - t0
        proc_rc = proc.returncode

        if xml_path.exists():
            xml_path.unlink()
        run_appverif(
            appverif,
            [
                "-export",
                "log",
                "-for",
                image_name,
                "-with",
                f"To={xml_path}",
                f"Symbols={symbol_dir}",
            ],
            check=False,
        )
        stops = parse_stops(xml_path)
    finally:
        run_appverif(appverif, ["-disable", "*", "-for", image_name], check=False)
        run_appverif(appverif, ["-delete", "settings", "-for", image_name], check=False)
        if temp_xml is not None and temp_xml.exists():
            try:
                temp_xml.unlink()
            except OSError:
                pass

    if stops:
        for stop in stops:
            frames_str = " -> ".join(stop["frames"]) if stop["frames"] else "<no frames>"
            print(
                f"appverif_run: STOP layer={stop[layer]} code={stop[stop_code]} "
                f"severity={stop[severity]} message={stop[message]!r} "
                f"frames=[{frames_str}]",
                file=sys.stderr,
            )
        return 1

    if proc_rc != 0:
        print(
            f"appverif_run: {image_name} exited with code {proc_rc} ({elapsed:.2f}s, 0 verifier stops)",
            file=sys.stderr,
        )
        return proc_rc if proc_rc > 0 else 1

    print(f"appverif_run: {image_name} passed ({elapsed:.2f}s, 0 verifier stops)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
