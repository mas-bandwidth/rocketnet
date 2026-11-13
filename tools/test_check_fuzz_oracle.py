#!/usr/bin/env python3
"""Process-level tests for tools/check_fuzz_oracle.py's decisions, with a fake
compiler named in a fixture CMakeCache.txt and a fixture tree (--root), so
nothing is compiled: a control whose oracle accepts the planted base64
truncation (exits 0) makes the tool exit nonzero, controls whose oracle
refuses both mutants make it exit 0, a message trigger the unmutated reader
also fails, or a message control caught by some other property, exits
nonzero, and a tree with zero corpus files or a build directory with no cache
exits nonzero naming the count.

The fake compiler copies one fake program to every -o target; the program
reads its own name (baseline, control, baseline_message, control_message) and
FAKE_ORACLE."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().with_name("check_fuzz_oracle.py")

FAKE_COMPILER = """#!/bin/sh
out=""
while [ $# -gt 0 ]; do
    if [ "$1" = "-o" ]; then out=$2; shift; fi
    shift
done
[ -n "$out" ] || exit 1
cp "$FAKE_PROGRAM" "$out" && chmod +x "$out"
"""

FAKE_PROGRAM = """#!/bin/sh
case "$(basename "$0")" in
    baseline) exit 0 ;;
    control)
        if [ "$FAKE_ORACLE" = blind ]; then exit 0; fi
        echo "fuzz oracle failed: base64 overlong field must refuse" >&2
        exit 1 ;;
    baseline_message)
        if [ "$FAKE_ORACLE" = message-baseline-trips ]; then
            echo "fuzz oracle failed: a record has data" >&2
            exit 1
        fi
        exit 0 ;;
    control_message)
        if [ "$FAKE_ORACLE" = message-other-property ]; then
            echo "fuzz oracle failed: RECORDS repacks to its length" >&2
            exit 1
        fi
        echo "fuzz oracle failed: a record has data" >&2
        exit 1 ;;
esac
exit 3
"""

BASE64 = """int rocketnet_base64_decode( const char * input, uint8_t * output, int output_size )
{
    int output_length = 0;
    if ( output_length > output_size )
    {
        return 0;
    }
    return output_length;
}
"""

MESSAGE = """int rocketnet_message_read_record( int length, int remaining, int header )
{
    if ( length == 0 || length > remaining - header )
    {
        return ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH;
    }
    return 0;
}
"""


class CheckFuzzOracleProcessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        compiler = self.dir / "fake-cxx"
        compiler.write_text(FAKE_COMPILER)
        compiler.chmod(0o755)
        program = self.dir / "fake-program"
        program.write_text(FAKE_PROGRAM)
        program.chmod(0o755)
        self.build = self.dir / "build-fuzz"
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text(f"CMAKE_CXX_COMPILER:FILEPATH={compiler}\n")
        self.root = self.dir / "root"
        (self.root / "sdk").mkdir(parents=True)
        (self.root / "sdk" / "rocketnet_base64.cpp").write_text(BASE64)
        (self.root / "sdk" / "rocketnet_message.cpp").write_text(MESSAGE)
        self.corpus = self.root / "tests" / "fuzz" / "corpus"
        self.corpus.mkdir(parents=True)
        (self.corpus / "base64_over_key_field").write_bytes(b"AAAA")
        self.env = dict(os.environ, FAKE_PROGRAM=str(program))

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, build, oracle):
        env = dict(self.env, FAKE_ORACLE=oracle)
        return subprocess.run([sys.executable, str(TOOL), str(build), "--root", str(self.root)],
                              capture_output=True, text=True, env=env)

    def test_oracle_refusing_both_mutants_passes(self):
        result = self.run_tool(self.build, "sharp")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("test_fuzz_exact_field_oracle", result.stdout)

    def test_oracle_blind_to_truncation_fails(self):
        result = self.run_tool(self.build, "blind")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("memory-safe truncation did not fail the exact-field oracle", result.stderr)

    def test_message_trigger_tripping_the_unmutated_reader_fails(self):
        # a control whose trigger also fails the original reader proves nothing
        result = self.run_tool(self.build, "message-baseline-trips")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("the unmutated message reader does not replay the zero-length trigger clean", result.stderr)

    def test_message_control_tripping_another_property_fails(self):
        # the bypass must be caught by the property it breaks, by name
        result = self.run_tool(self.build, "message-other-property")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("message length bypass did not fail the property oracle", result.stderr)

    def test_zero_corpus_files_fail(self):
        (self.corpus / "base64_over_key_field").unlink()
        result = self.run_tool(self.build, "sharp")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 corpus files", result.stderr)

    def test_empty_build_directory_fails(self):
        empty = self.dir / "empty"
        empty.mkdir()
        result = self.run_tool(empty, "sharp")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0 cache entries", result.stderr)


if __name__ == "__main__":
    unittest.main()
