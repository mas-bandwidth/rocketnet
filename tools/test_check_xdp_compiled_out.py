#!/usr/bin/env python3
"""Tests for tools/check_xdp_compiled_out.py's verdicts on canned nm and readelf lines."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_xdp_compiled_out as check

COMPILED_OUT = [
    "rocketnet_driver_builtin.cpp.o:",
    "0000000000000440 T rocketnet_forbid_xdp_commit",
    "00000000000002a0 t xdp_not_built_create(void*, rocketnet_server_transport_io_config_t const*, rocketnet_address_t*)",
    "0000000000000020 d xdp_operations",
    "0000000000000000 D rocketnet_xdp_transport",
    "                 U rocketnet_xdp_transport",
    "                 U memcpy",
]

COMPILED_IN = [
    "rocketnet_server_socket_linux.cpp.o:",
    "0000000000001000 T rocketnet_server_socket_create(void*, char const*, unsigned char const*, int, int, char const*, int*)",
    "0000000000000000 r rocketnet_server_xdp_o",
    "0000000000000b00 T rocketnet_gateway_netlink_route(int, int, unsigned char*, int*)",
    "                 U bpf_object__open_mem",
    "                 U xsk_socket__create",
]

NEEDED_IN = [" 0x0000000000000001 (NEEDED)             Shared library: [libbpf.so.1]",
             " 0x0000000000000001 (NEEDED)             Shared library: [libxdp.so.1]"]
NEEDED_OUT = [" 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]"]


class CheckXdpCompiledOutTest(unittest.TestCase):
    def test_compiled_out_library_passes_absent(self):
        self.assertEqual(check.absent_problems(*check.classify(COMPILED_OUT, NEEDED_OUT)), [])

    def test_compiled_out_library_fails_present(self):
        self.assertNotEqual(check.present_problems(*check.classify(COMPILED_OUT, NEEDED_OUT)), [])

    def test_compiled_in_library_passes_present(self):
        self.assertEqual(check.present_problems(*check.classify(COMPILED_IN, NEEDED_IN)), [])

    def test_compiled_in_library_fails_absent(self):
        problems = check.absent_problems(*check.classify(COMPILED_IN, NEEDED_IN))
        self.assertIn("XDP symbol defined: rocketnet_server_socket_create", problems)
        self.assertIn("XDP symbol defined: rocketnet_server_xdp_o", problems)
        self.assertIn("libbpf/libxdp referenced: bpf_object__open_mem", problems)
        self.assertIn("links libbpf.so", problems)

    def test_one_stray_symbol_fails_absent(self):
        problems = check.absent_problems(*check.classify(COMPILED_OUT + ["0000000000000000 t xdp_create(void*)"], NEEDED_OUT))
        self.assertEqual(problems, ["XDP symbol defined: xdp_create"])

    def test_one_stray_import_fails_absent(self):
        problems = check.absent_problems(*check.classify(COMPILED_OUT + ["                 U bpf_map_update_elem"], NEEDED_OUT))
        self.assertEqual(problems, ["libbpf/libxdp referenced: bpf_map_update_elem"])


if __name__ == "__main__":
    unittest.main()
