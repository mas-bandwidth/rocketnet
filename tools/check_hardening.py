#!/usr/bin/env python3
"""Check release hardening on the built shared SDK library, on every platform.

    python3 tools/check_hardening.py bin/librocketnet.so
    python3 tools/check_hardening.py bin/librocketnet.dylib
    python  tools/check_hardening.py bin/Release/rocketnet.dll
    python3 tools/check_hardening.py bin          (a directory: the library in it)

The format is picked by the file's magic, not its name: ELF, Mach-O or PE
("MZ"). Each required property is one named check printed as a PASS or FAIL
line; properties the build does not turn on yet (CF guard, CET) are INFO lines
and never fail. The last line is the one-line verdict, and the exit code is 1
when any required check failed.

  ELF (readelf):  BIND_NOW, GNU_RELRO, __stack_chk_fail, GNU_STACK not executable
  Mach-O (otool -l, nm -u):  ___stack_chk_fail and ___stack_chk_guard imported,
                  no segment whose initprot is both writable and executable
  PE (read with struct, so no dumpbin or vcvars):  DYNAMIC_BASE, NX_COMPAT,
                  HIGH_ENTROPY_VA, a load configuration with a non-zero
                  SecurityCookie (/GS)

Python 3.9 (the Windows runner's), standard library only.
"""
from pathlib import Path
import re
import struct
import subprocess
import sys

PASS = "PASS"
FAIL = "FAIL"
INFO = "INFO"

ELF_MAGIC = b"\x7fELF"
MACHO_MAGICS = (
    b"\xcf\xfa\xed\xfe",  # MH_MAGIC_64, little endian
    b"\xce\xfa\xed\xfe",  # MH_MAGIC, little endian
    b"\xfe\xed\xfa\xcf",  # MH_MAGIC_64, big endian
    b"\xfe\xed\xfa\xce",  # MH_MAGIC, big endian
    b"\xca\xfe\xba\xbe",  # FAT_MAGIC (universal)
    b"\xca\xfe\xba\xbf",  # FAT_MAGIC_64
)
PE_MAGIC = b"MZ"

IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA = 0x0020
IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE = 0x0040
IMAGE_DLLCHARACTERISTICS_NX_COMPAT = 0x0100
IMAGE_DLLCHARACTERISTICS_GUARD_CF = 0x4000
IMAGE_DIRECTORY_ENTRY_DEBUG = 6
IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG = 10
IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS = 20
IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT = 0x0001

VM_PROT_WRITE = 0x2
VM_PROT_EXECUTE = 0x4


def run_tool(argv):
    """The seam the tests replace: run one platform tool, return its stdout."""
    return subprocess.check_output(argv, text=True)


def result(status, name, detail):
    return (status, name, detail)


# ---- ELF ------------------------------------------------------------------


def linux_checks(dynamic, segments, symbols, notes=""):
    """Checks on `readelf -d`, `readelf -lW`, `readelf -Ws` and `readelf -n` text."""
    results = []
    bind_now = "BIND_NOW" in dynamic or re.search(r"\(FLAGS(_1)?\)\s+Flags:.*\bNOW\b", dynamic) is not None
    results.append(result(PASS if bind_now else FAIL, "BIND_NOW", "dynamic section" if bind_now else "absent from the dynamic section (-Wl,-z,now)"))
    relro = re.search(r"^\s*GNU_RELRO\b", segments, re.MULTILINE) is not None
    results.append(result(PASS if relro else FAIL, "GNU_RELRO", "program header" if relro else "no GNU_RELRO program header (-Wl,-z,relro)"))
    stack_chk = re.search(r"\b__stack_chk_fail\b", symbols) is not None
    results.append(result(PASS if stack_chk else FAIL, "__stack_chk_fail", "imported" if stack_chk else "not imported (-fstack-protector-strong)"))
    stack = re.search(r"^\s*GNU_STACK\b(.*)$", segments, re.MULTILINE)
    if stack is None:
        results.append(result(FAIL, "GNU_STACK", "no GNU_STACK program header: the loader gives the library an executable stack"))
    else:
        # readelf -lW: GNU_STACK Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align;
        # the flags are the letters between the last size and the alignment.
        fields = stack.group(1).split()
        flags = "".join(field for field in fields[5:-1] if re.fullmatch(r"[RWE]+", field))
        if "E" in flags:
            results.append(result(FAIL, "GNU_STACK", f"executable stack (flags {flags})"))
        else:
            results.append(result(PASS, "GNU_STACK", f"not executable (flags {flags or '-'})"))
    features = re.search(r"x86 feature:\s*(.*)", notes)
    if features:
        results.append(result(INFO, "CET", f"x86 feature: {features.group(1).strip()}"))
    else:
        results.append(result(INFO, "CET", "no x86 feature note (IBT/SHSTK not marked)"))
    return results


def check_elf(library, run):
    return linux_checks(
        run(["readelf", "-d", str(library)]),
        run(["readelf", "-lW", str(library)]),
        run(["readelf", "-Ws", str(library)]),
        run(["readelf", "-n", str(library)]),
    )


# ---- Mach-O ---------------------------------------------------------------


def macho_segments(load_commands):
    """(segname, initprot) for each LC_SEGMENT/LC_SEGMENT_64 in `otool -l` text."""
    segments = []
    for block in re.split(r"^Load command \d+\s*$", load_commands, flags=re.MULTILINE)[1:]:
        if not re.search(r"^\s*cmd LC_SEGMENT(_64)?\s*$", block, re.MULTILINE):
            continue
        name = re.search(r"^\s*segname (\S+)", block, re.MULTILINE)
        initprot = re.search(r"^\s*initprot (0x[0-9a-fA-F]+|\d+)", block, re.MULTILINE)
        if initprot:
            segments.append((name.group(1) if name else "?", int(initprot.group(1), 0)))
    return segments


def macos_checks(load_commands, undefined):
    """Checks on `otool -l` and `nm -u` text."""
    results = []
    imported = set(line.strip().split()[-1] for line in undefined.splitlines() if line.strip())
    for symbol in ("___stack_chk_fail", "___stack_chk_guard"):
        present = symbol in imported
        results.append(result(PASS if present else FAIL, symbol, "imported" if present else "not imported (-fstack-protector-strong)"))
    segments = macho_segments(load_commands)
    writable_executable = [name for name, prot in segments if prot & VM_PROT_WRITE and prot & VM_PROT_EXECUTE]
    if not segments:
        results.append(result(FAIL, "W^X segments", "no segment load commands in otool -l output"))
    elif writable_executable:
        results.append(result(FAIL, "W^X segments", f"writable and executable initprot: {', '.join(writable_executable)}"))
    else:
        results.append(result(PASS, "W^X segments", f"{len(segments)} segments, none writable and executable"))
    return results


def check_macho(library, run):
    return macos_checks(run(["otool", "-l", str(library)]), run(["nm", "-u", str(library)]))


# ---- PE -------------------------------------------------------------------


class PortableExecutableError(ValueError):
    pass


def _unpack(fmt, data, offset):
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        raise PortableExecutableError(f"truncated PE image at offset {offset:#x}")
    return struct.unpack_from(fmt, data, offset)


def _rva_to_offset(rva, sections):
    for virtual_address, virtual_size, raw_size, raw_pointer in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_pointer + (rva - virtual_address)
    return None


def windows_checks(data):
    """Checks on the bytes of a PE image, read with struct."""
    results = []
    try:
        (pe_offset,) = _unpack("<I", data, 0x3C)
        if data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise PortableExecutableError("no PE signature")
        _machine, section_count, _, _, _, optional_size, _ = _unpack("<HHIIIHH", data, pe_offset + 4)
        optional = pe_offset + 24
        (magic,) = _unpack("<H", data, optional)
        if magic == 0x20B:
            directories, cookie_offset, cookie_format = optional + 112, 0x58, "<Q"
        elif magic == 0x10B:
            directories, cookie_offset, cookie_format = optional + 96, 0x3C, "<I"
        else:
            raise PortableExecutableError(f"unknown optional header magic {magic:#x}")
        (dll_characteristics,) = _unpack("<H", data, optional + 70)
        sections = []
        section_table = optional + optional_size
        for index in range(section_count):
            virtual_size, virtual_address, raw_size, raw_pointer = _unpack("<IIII", data, section_table + 40 * index + 8)
            sections.append((virtual_address, virtual_size, raw_size, raw_pointer))
        load_config_rva, load_config_size = _unpack("<II", data, directories + 8 * IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
        debug_rva, debug_size = _unpack("<II", data, directories + 8 * IMAGE_DIRECTORY_ENTRY_DEBUG)
    except PortableExecutableError as error:
        return [result(FAIL, "PE image", str(error))]

    for name, flag, why in (
        ("DYNAMIC_BASE", IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, "/DYNAMICBASE"),
        ("NX_COMPAT", IMAGE_DLLCHARACTERISTICS_NX_COMPAT, "/NXCOMPAT"),
        ("HIGH_ENTROPY_VA", IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA, "/HIGHENTROPYVA"),
    ):
        present = bool(dll_characteristics & flag)
        results.append(result(PASS if present else FAIL, name, f"DllCharacteristics {dll_characteristics:#06x}" if present else f"clear in DllCharacteristics {dll_characteristics:#06x} ({why})"))

    cookie = None
    if load_config_rva and load_config_size:
        load_config = _rva_to_offset(load_config_rva, sections)
        if load_config is not None:
            try:
                (size,) = _unpack("<I", data, load_config)
                if size >= cookie_offset + struct.calcsize(cookie_format):
                    (cookie,) = _unpack(cookie_format, data, load_config + cookie_offset)
            except PortableExecutableError:
                cookie = None
    if cookie:
        results.append(result(PASS, "SecurityCookie", f"load configuration cookie at {cookie:#x} (/GS)"))
    elif cookie == 0:
        results.append(result(FAIL, "SecurityCookie", "load configuration SecurityCookie is zero (/GS)"))
    else:
        results.append(result(FAIL, "SecurityCookie", "no load configuration directory holding a SecurityCookie (/GS)"))

    guard_cf = bool(dll_characteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF)
    results.append(result(INFO, "GUARD_CF", "set (/guard:cf)" if guard_cf else "clear (/guard:cf is the owner's call)"))
    cet = False
    debug = _rva_to_offset(debug_rva, sections) if debug_rva else None
    if debug is not None:
        for index in range(debug_size // 28):
            try:
                (debug_type,) = _unpack("<I", data, debug + 28 * index + 12)
                (raw_pointer,) = _unpack("<I", data, debug + 28 * index + 24)
                if debug_type == IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS:
                    (characteristics_ex,) = _unpack("<I", data, raw_pointer)
                    cet = bool(characteristics_ex & IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT)
            except PortableExecutableError:
                break
    results.append(result(INFO, "CET_COMPAT", "set (/CETCOMPAT)" if cet else "clear (/CETCOMPAT is the owner's call)"))
    return results


def check_pe(library, run):
    return windows_checks(Path(library).read_bytes())


# ---- dispatch -------------------------------------------------------------


def library_format(head):
    if head.startswith(ELF_MAGIC):
        return "elf"
    if head[:4] in MACHO_MAGICS:
        return "macho"
    if head.startswith(PE_MAGIC):
        return "pe"
    return None


def find_library(directory):
    """The old `bin` argument: the shared library inside a build output directory."""
    candidates = sorted(directory.glob("librocketnet.so*")) + sorted(directory.glob("librocketnet*.dylib"))
    candidates += sorted(directory.glob("Release/rocketnet.dll")) + sorted(directory.glob("rocketnet.dll"))
    files = [path for path in candidates if path.is_file()]
    return files[0] if files else None


def check(library, run=run_tool):
    """(results, verdict line, exit code) for one library."""
    with open(library, "rb") as handle:
        head = handle.read(4)
    kind = library_format(head)
    if kind is None:
        return [], f"{library}: not an ELF, Mach-O or PE library", 1
    results = {"elf": check_elf, "macho": check_macho, "pe": check_pe}[kind](library, run)
    failed = [name for status, name, _ in results if status == FAIL]
    passed = [name for status, name, _ in results if status == PASS]
    if failed:
        return results, f"{library}: HARDENING FAIL ({kind}): missing {', '.join(failed)}", 1
    return results, f"{library}: hardening ok ({kind}): {', '.join(passed)}", 0


def main(argv):
    target = Path(argv[1] if len(argv) > 1 else "bin")
    if target.is_dir():
        library = find_library(target)
        if library is None:
            print(f"No shared RocketNet library found in {target}", file=sys.stderr)
            return 1
    elif target.is_file():
        library = target
    else:
        print(f"{target}: no such library", file=sys.stderr)
        return 1
    results, verdict, code = check(library)
    for status, name, detail in results:
        print(f"{status} {name}: {detail}")
    print(verdict)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
