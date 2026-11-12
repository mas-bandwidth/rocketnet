#!/usr/bin/env python3
"""Tests for tools/check_abi.py: the recorded 1.x ABI refuses a moved field,
a removed export and an unrecorded field, and passes the current header.
Each case edits a copy of sdk/rocketnet.h or tools/exports.txt in a
temporary directory and compiles it with $CC (default cc)."""

import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_abi

RECEIPT = """struct rocketnet_receipt_t
{"""


class CheckAbiTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.header = Path(self.tmp.name) / "rocketnet.h"
        self.exports = Path(self.tmp.name) / "exports.txt"
        self.header.write_text(check_abi.HEADER.read_text())
        self.exports.write_text(check_abi.EXPORTS.read_text())
        self.baseline = check_abi.read_baseline(check_abi.BASELINE)

    def tearDown(self):
        self.tmp.cleanup()

    def problems(self):
        current = check_abi.measured_lines(self.header, self.exports, os.environ.get("CC", "cc"))
        return check_abi.compare(self.baseline, current)

    def test_the_current_header_matches(self):
        self.assertEqual(self.problems(), [])

    def test_a_moved_field_is_a_break(self):
        text = self.header.read_text()
        self.assertIn(RECEIPT, text)
        self.header.write_text(text.replace(RECEIPT, RECEIPT + "\n    uint32_t moved_in;", 1))
        problems = self.problems()
        self.assertIn("ABI BREAK: field rocketnet_receipt_t status 4 4 (recorded in abi_v1.txt)", problems)
        self.assertTrue(any(p.startswith("NOT RECORDED: field rocketnet_receipt_t moved_in") for p in problems), problems)

    def test_a_removed_export_is_a_break(self):
        names = check_abi.read_exports(self.exports)
        self.exports.write_text("\n".join(name for name in names if name != "rocketnet_init") + "\n")
        self.assertEqual(self.problems(), ["ABI BREAK: export rocketnet_init (recorded in abi_v1.txt)"])

    def replace(self, old, new):
        text = self.header.read_text()
        self.assertIn(old, text)
        self.header.write_text(text.replace(old, new, 1))

    def assert_break(self, prefix):
        problems = self.problems()
        self.assertTrue(any(p.startswith("ABI BREAK: " + prefix) for p in problems), problems)

    def test_a_changed_return_type_is_a_break(self):
        self.replace("ROCKETNET_EXPORT uint32_t rocketnet_version( void );",
                     "ROCKETNET_EXPORT uint64_t rocketnet_version( void );")
        self.assert_break("prototype rocketnet_version uint32_t rocketnet_version(void)")

    def test_a_changed_parameter_type_is_a_break(self):
        self.replace("rocketnet_client_connect( struct rocketnet_client_t * client, const char * connect_token );",
                     "rocketnet_client_connect( struct rocketnet_client_t * client, char * connect_token );")
        self.assert_break("prototype rocketnet_client_connect ")

    def test_an_added_parameter_is_a_break(self):
        self.replace("rocketnet_client_destroy( struct rocketnet_client_t * client );",
                     "rocketnet_client_destroy( struct rocketnet_client_t * client, int flags );")
        self.assert_break("prototype rocketnet_client_destroy ")

    def test_a_renamed_parameter_is_not_a_break(self):
        self.replace("rocketnet_client_destroy( struct rocketnet_client_t * client );",
                     "rocketnet_client_destroy( struct rocketnet_client_t * the_client );")
        self.assertEqual(self.problems(), [])

    def test_a_changed_enum_value_is_a_break(self):
        self.replace("ROCKETNET_LOG_LEVEL_ERROR = 1,", "ROCKETNET_LOG_LEVEL_ERROR = 7,")
        self.assert_break("enum rocketnet_log_level_t ROCKETNET_LOG_LEVEL_ERROR 1")

    def test_a_changed_macro_constant_is_a_break(self):
        self.replace("#define ROCKETNET_MAX_PACKET_PAYLOAD_BYTES                 1416",
                     "#define ROCKETNET_MAX_PACKET_PAYLOAD_BYTES                 1400")
        self.assert_break("macro ROCKETNET_MAX_PACKET_PAYLOAD_BYTES 1416")

    def test_a_same_size_field_type_change_is_a_break(self):
        self.replace("    uint64_t send_refused;", "    int64_t send_refused;")
        self.assert_break("fieldtype rocketnet_conditions_t send_refused uint64_t")

    def test_a_renamed_field_is_a_break(self):
        self.replace("    double seconds_since_heard;", "    double seconds_since_last_heard;")
        self.assert_break("field rocketnet_conditions_t seconds_since_heard ")

    def test_a_changed_callback_typedef_is_a_break(self):
        text = self.header.read_text()
        old = re.search(r"typedef void \( \*rocketnet_log_function_t \)\( void \* context,", text)
        self.assertIsNotNone(old)
        self.header.write_text(text.replace(old.group(0), "typedef void ( *rocketnet_log_function_t )( const void * context,", 1))
        self.assert_break("typedef rocketnet_log_function_t ")

    def test_an_export_without_a_prototype_is_refused(self):
        self.replace("ROCKETNET_EXPORT uint32_t rocketnet_version( void );", "")
        problems = self.problems()
        self.assertIn("NOT RECORDED: prototype rocketnet_version <not found in rocketnet.h> "
                      "(a 1.x addition is added to abi_v1.txt)", problems)

    def test_an_unrecorded_export_must_be_recorded(self):
        self.exports.write_text(self.exports.read_text() + "rocketnet_new_function\n")
        self.assertEqual(self.problems(),
                         ["NOT RECORDED: export rocketnet_new_function (a 1.x addition is added to abi_v1.txt)",
                          "NOT RECORDED: prototype rocketnet_new_function <not found in rocketnet.h> "
                          "(a 1.x addition is added to abi_v1.txt)"])


if __name__ == "__main__":
    unittest.main()
