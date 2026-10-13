#!/usr/bin/env python3
"""Check release hardening on the built shared SDK library."""
from pathlib import Path
import subprocess
import sys

directory = Path(sys.argv[1] if len(sys.argv) > 1 else "bin")
libraries = sorted(directory.glob("librocketnet.so*"))
if not libraries:
    sys.exit(f"No shared RocketNet library found in {directory}")
library = next((path for path in libraries if path.is_file()), libraries[0])

dynamic = subprocess.check_output(["readelf", "-d", str(library)], text=True)
segments = subprocess.check_output(["readelf", "-l", str(library)], text=True)
symbols = subprocess.check_output(["readelf", "-Ws", str(library)], text=True)
missing = []
if "BIND_NOW" not in dynamic:
    missing.append("BIND_NOW")
if "GNU_RELRO" not in segments:
    missing.append("GNU_RELRO")
if "__stack_chk_fail" not in symbols:
    missing.append("__stack_chk_fail")
if missing:
    sys.exit(f"{library}: missing hardening: {', '.join(missing)}")
print(f"{library}: BIND_NOW, GNU_RELRO, __stack_chk_fail present")
