#!/usr/bin/env python3
"""Tests for tools/check_hardening.py on canned tool text and in-memory PE images.

No compiler, no real library, no subprocess: the readelf, otool and nm text is
canned here and the PE image is assembled with struct.
"""

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_hardening
from check_hardening import FAIL, INFO, PASS

READELF_DYNAMIC = """
Dynamic section at offset 0x1fd8 contains 30 entries:
  Tag        Type                         Name/Value
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x000000000000000e (SONAME)             Library soname: [librocketnet.so.1]
 0x0000000000000018 (BIND_NOW)
 0x000000006ffffffb (FLAGS_1)            Flags: NOW
 0x0000000000000000 (NULL)               0x0
"""

READELF_SEGMENTS = """
Elf file type is DYN (Shared object file)
Program Headers:
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0x000000 0x0000000000000000 0x0000000000000000 0x01a000 0x01a000 R   0x1000
  LOAD           0x01a000 0x000000000001a000 0x000000000001a000 0x090000 0x090000 R E 0x1000
  DYNAMIC        0x0b1dd8 0x00000000000b2dd8 0x00000000000b2dd8 0x000200 0x000200 RW  0x8
  GNU_STACK      0x000000 0x0000000000000000 0x0000000000000000 0x000000 0x000000 RW  0x10
  GNU_RELRO      0x0b0c40 0x00000000000b1c40 0x00000000000b1c40 0x0013c0 0x0013c0 R   0x1
"""

READELF_SYMBOLS = """
Symbol table '.dynsym' contains 3 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND __stack_chk_fail@GLIBC_2.4 (2)
     2: 0000000000001234    64 FUNC    GLOBAL DEFAULT   12 rocketnet_init
"""

READELF_NOTES = """
Displaying notes found in: .note.gnu.property
  Owner                Data size        Description
  GNU                  0x00000020       NT_GNU_PROPERTY_TYPE_0
      Properties: x86 feature: IBT, SHSTK
"""

OTOOL_HARDENED = """bin/librocketnet.dylib:
Load command 0
      cmd LC_SEGMENT_64
  cmdsize 552
  segname __TEXT
   vmaddr 0x0000000000000000
   vmsize 0x0000000000088000
  fileoff 0
 filesize 557056
  maxprot 0x00000005
 initprot 0x00000005
   nsects 6
    flags 0x0
Load command 1
      cmd LC_SEGMENT_64
  cmdsize 312
  segname __DATA_CONST
  maxprot 0x00000003
 initprot 0x00000003
Load command 2
      cmd LC_SEGMENT_64
  cmdsize 72
  segname __LINKEDIT
  maxprot 0x00000001
 initprot 0x00000001
Load command 3
          cmd LC_ID_DYLIB
      cmdsize 56
         name @rpath/librocketnet.dylib (offset 24)
"""

NM_UNDEFINED = """                 U ___stack_chk_fail
                 U ___stack_chk_guard
                 U _malloc
                 U _memcpy
"""

PE_DLL_HARDENED = 0x0020 | 0x0040 | 0x0100
COOKIE = 0x180010000


def build_pe(dll_characteristics=PE_DLL_HARDENED, cookie=COOKIE, load_config=True):
    """A minimal PE32+ DLL image: DOS header, PE header, one section holding the load configuration."""
    image = bytearray(0x400)
    image[0:2] = b"MZ"
    pe = 0x40
    struct.pack_into("<I", image, 0x3C, pe)
    image[pe:pe + 4] = b"PE\0\0"
    optional_size = 240
    struct.pack_into("<HHIIIHH", image, pe + 4, 0x8664, 1, 0, 0, 0, optional_size, 0x2022)
    optional = pe + 24
    struct.pack_into("<H", image, optional, 0x20B)
    struct.pack_into("<H", image, optional + 70, dll_characteristics)
    struct.pack_into("<I", image, optional + 108, 16)
    if load_config:
        struct.pack_into("<II", image, optional + 112 + 8 * 10, 0x1000, 0x140)
    section = optional + optional_size
    image[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", image, section + 8, 0x200, 0x1000, 0x200, 0x200)
    struct.pack_into("<I", image, 0x200, 0x140)
    struct.pack_into("<Q", image, 0x200 + 0x58, cookie)
    return bytes(image)


def statuses(results):
    return {name: status for status, name, _ in results}


class LinuxTest(unittest.TestCase):
    def test_linux_missing_bind_now_fails(self):
        dynamic = READELF_DYNAMIC.replace(" 0x0000000000000018 (BIND_NOW)\n", "").replace("Flags: NOW", "Flags: ORIGIN")
        self.assertEqual(statuses(check_hardening.linux_checks(dynamic, READELF_SEGMENTS, READELF_SYMBOLS))["BIND_NOW"], FAIL)

    def test_linux_missing_relro_fails(self):
        segments = "\n".join(line for line in READELF_SEGMENTS.splitlines() if "GNU_RELRO" not in line)
        self.assertEqual(statuses(check_hardening.linux_checks(READELF_DYNAMIC, segments, READELF_SYMBOLS))["GNU_RELRO"], FAIL)

    def test_linux_executable_stack_fails(self):
        segments = READELF_SEGMENTS.replace("0x000000 0x000000 RW  0x10", "0x000000 0x000000 RWE 0x10")
        self.assertNotEqual(segments, READELF_SEGMENTS)
        self.assertEqual(statuses(check_hardening.linux_checks(READELF_DYNAMIC, segments, READELF_SYMBOLS))["GNU_STACK"], FAIL)
        no_stack = "\n".join(line for line in READELF_SEGMENTS.splitlines() if "GNU_STACK" not in line)
        self.assertEqual(statuses(check_hardening.linux_checks(READELF_DYNAMIC, no_stack, READELF_SYMBOLS))["GNU_STACK"], FAIL)

    def test_linux_hardened_passes(self):
        results = check_hardening.linux_checks(READELF_DYNAMIC, READELF_SEGMENTS, READELF_SYMBOLS, READELF_NOTES)
        self.assertEqual(statuses(results), {"BIND_NOW": PASS, "GNU_RELRO": PASS, "__stack_chk_fail": PASS, "GNU_STACK": PASS, "CET": INFO})
        stack_missing = READELF_SYMBOLS.replace("__stack_chk_fail@GLIBC_2.4 (2)", "memcpy@GLIBC_2.14 (3)")
        self.assertEqual(statuses(check_hardening.linux_checks(READELF_DYNAMIC, READELF_SEGMENTS, stack_missing))["__stack_chk_fail"], FAIL)


class MacOSTest(unittest.TestCase):
    def test_macos_missing_stack_chk_fails(self):
        for symbol in ("___stack_chk_fail", "___stack_chk_guard"):
            undefined = "\n".join(line for line in NM_UNDEFINED.splitlines() if not line.endswith(symbol))
            self.assertEqual(statuses(check_hardening.macos_checks(OTOOL_HARDENED, undefined))[symbol], FAIL, symbol)

    def test_macos_writable_executable_segment_fails(self):
        load_commands = OTOOL_HARDENED.replace(" initprot 0x00000003", " initprot 0x00000007")
        self.assertNotEqual(load_commands, OTOOL_HARDENED)
        results = check_hardening.macos_checks(load_commands, NM_UNDEFINED)
        self.assertEqual(statuses(results)["W^X segments"], FAIL)
        self.assertIn("__DATA_CONST", [detail for _, name, detail in results if name == "W^X segments"][0])

    def test_macos_hardened_passes(self):
        results = check_hardening.macos_checks(OTOOL_HARDENED, NM_UNDEFINED)
        self.assertEqual(statuses(results), {"___stack_chk_fail": PASS, "___stack_chk_guard": PASS, "W^X segments": PASS})


class WindowsTest(unittest.TestCase):
    def assert_fails(self, image, name):
        self.assertEqual(statuses(check_hardening.windows_checks(image))[name], FAIL)

    def test_windows_missing_dynamic_base_fails(self):
        self.assert_fails(build_pe(PE_DLL_HARDENED & ~0x0040), "DYNAMIC_BASE")

    def test_windows_missing_nx_compat_fails(self):
        self.assert_fails(build_pe(PE_DLL_HARDENED & ~0x0100), "NX_COMPAT")

    def test_windows_missing_high_entropy_va_fails(self):
        self.assert_fails(build_pe(PE_DLL_HARDENED & ~0x0020), "HIGH_ENTROPY_VA")

    def test_windows_missing_security_cookie_fails(self):
        self.assert_fails(build_pe(cookie=0), "SecurityCookie")
        self.assert_fails(build_pe(load_config=False), "SecurityCookie")

    def test_windows_hardened_passes(self):
        results = check_hardening.windows_checks(build_pe())
        self.assertEqual(statuses(results), {
            "DYNAMIC_BASE": PASS, "NX_COMPAT": PASS, "HIGH_ENTROPY_VA": PASS, "SecurityCookie": PASS,
            "GUARD_CF": INFO, "CET_COMPAT": INFO,
        })
        guarded = statuses(check_hardening.windows_checks(build_pe(PE_DLL_HARDENED | 0x4000)))
        self.assertEqual(guarded["GUARD_CF"], INFO)


if __name__ == "__main__":
    unittest.main(verbosity=2)
