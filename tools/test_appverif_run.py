#!/usr/bin/env python3
"""Tests for the image name tools/windows/appverif_run.py verifies, and for
the removal of that image's registry key after the run (RemoveImageKeyTest).

Application Verifier settings and logs are keyed by image name, machine-wide:
while a run has functional.exe under verification, every functional.exe on the
host (a CI leg, another worker's build) is verified too, its logs land beside
the run's, and its own -delete logs removes the run's. The run therefore
verifies a copy of the executable under a name no other process uses.
"""

import importlib.util
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
APPVERIF_RUN = REPO_ROOT / "tools" / "windows" / "appverif_run.py"


def load_appverif_run():
    spec = importlib.util.spec_from_file_location("appverif_run", APPVERIF_RUN)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class VerifiedImagePathTest(unittest.TestCase):
    def setUp(self):
        self.appverif_run = load_appverif_run()
        self.exe = Path("C:/build/bin/Debug/functional.exe")

    def test_copy_sits_beside_the_executable(self):
        copy = self.appverif_run.verified_image_path(self.exe, "1234")
        self.assertEqual(copy.parent, self.exe.parent)
        self.assertEqual(copy.suffix, ".exe")

    def test_copy_name_is_not_the_executable_name(self):
        copy = self.appverif_run.verified_image_path(self.exe, "1234")
        self.assertNotEqual(copy.name.lower(), self.exe.name.lower())
        self.assertTrue(copy.stem.startswith(self.exe.stem))

    def test_two_runs_verify_different_image_names(self):
        first = self.appverif_run.verified_image_path(self.exe, "1234")
        second = self.appverif_run.verified_image_path(self.exe, "5678")
        self.assertNotEqual(first.name.lower(), second.name.lower())


class FakeKeys:
    """An HKLM stand-in: key paths (case-insensitive, as the registry's) to
    their values. delete refuses a key with subkeys, as DeleteKeyEx does."""

    def __init__(self, paths, refuse=()):
        self.keys = {path.lower(): path for path in paths}
        self.refuse = {path.lower() for path in refuse}

    def exists(self, path):
        return path.lower() in self.keys

    def subkeys(self, path):
        prefix = path.lower() + "\\"
        return sorted(
            {
                self.keys[key][len(prefix):].split("\\", 1)[0]
                for key in self.keys
                if key.startswith(prefix)
            }
        )

    def delete(self, path):
        key = path.lower()
        if key in self.refuse:
            raise PermissionError(5, "Access is denied")
        if self.subkeys(path):
            raise OSError(5, "key has subkeys")
        del self.keys[key]


class RemoveImageKeyTest(unittest.TestCase):
    """appverif -disable and -delete settings leave the verified copy's IFEO
    key behind, empty; the run deletes it, and only it, on every exit path."""

    def setUp(self):
        self.appverif_run = load_appverif_run()
        self.ifeo = self.appverif_run.IFEO_KEY
        self.image = "functional_appverif_4242.exe"
        self.image_key = f"{self.ifeo}\\{self.image}"
        self.neighbours = [
            self.ifeo,
            f"{self.ifeo}\\functional.exe",
            f"{self.ifeo}\\functional_appverif_42.exe",
            f"{self.ifeo}\\{{ApplicationVerifierGlobalSettings}}",
        ]

    def test_empty_key_left_by_appverif_is_deleted(self):
        keys = FakeKeys(self.neighbours + [self.image_key])
        self.assertTrue(self.appverif_run.remove_image_key(keys, self.image))
        self.assertFalse(keys.exists(self.image_key))
        for path in self.neighbours:
            self.assertTrue(keys.exists(path), path)

    def test_key_with_layer_subkeys_is_deleted_whole(self):
        # a failed -disable leaves the layer subkeys too
        layers = [
            f"{self.image_key}\\Handles-{{9760941A}}",
            f"{self.image_key}\\Heaps-{{4D056CEB}}",
            f"{self.image_key}\\Heaps-{{4D056CEB}}\\nested",
        ]
        keys = FakeKeys(self.neighbours + [self.image_key] + layers)
        self.assertTrue(self.appverif_run.remove_image_key(keys, self.image))
        for path in [self.image_key] + layers:
            self.assertFalse(keys.exists(path), path)
        for path in self.neighbours:
            self.assertTrue(keys.exists(path), path)

    def test_absent_key_is_removed(self):
        keys = FakeKeys(self.neighbours)
        self.assertTrue(self.appverif_run.remove_image_key(keys, self.image))

    def test_key_that_cannot_be_deleted_is_reported(self):
        keys = FakeKeys(self.neighbours + [self.image_key], refuse=[self.image_key])
        self.assertFalse(self.appverif_run.remove_image_key(keys, self.image))

    def test_only_a_verified_copy_name_is_accepted(self):
        keys = FakeKeys(self.neighbours)
        for name in ("functional.exe", "{ApplicationVerifierGlobalSettings}", "", "x_appverif_.exe",
                     "a_appverif_1.exe\\..\\functional.exe", "a\\b_appverif_1.exe"):
            with self.assertRaises(ValueError, msg=name):
                self.appverif_run.remove_image_key(keys, name)
        for path in self.neighbours:
            self.assertTrue(keys.exists(path), path)

    def test_the_name_the_run_verifies_is_accepted(self):
        copy = self.appverif_run.verified_image_path(Path("C:/b/bin/Debug/test_iocp.exe"), "31337")
        self.assertTrue(self.appverif_run.image_key_path(copy.name).endswith("\\test_iocp_appverif_31337.exe"))


class StopSettingsTest(unittest.TestCase):
    """Every stop the run configures is active and logged with its stack trace,
    and never breaks into a debugger: no debugger is attached, so a break bit
    ends the process on the first stop. Until 2026-11-12 the value was 0x2,
    which disables every stop (bit 0x1 clear) and logs none."""

    def setUp(self):
        self.appverif_run = load_appverif_run()

    def configured_values(self, failures=()):
        """The ErrorReport values configure_stops passes to appverif, on a fake
        appverif that refuses each code in failures once as unissued."""
        calls = []
        refuse = list(failures)
        appverif_run = self.appverif_run

        class Proc:
            def __init__(self, returncode, stdout=""):
                self.returncode = returncode
                self.stdout = stdout
                self.stderr = ""

        def fake_call(appverif, args, check=True):
            calls.append(list(args))
            if refuse:
                code = refuse.pop(0)
                return Proc(1, f"Stop code [0x{code:X}] is not issued by any of the registered providers")
            return Proc(0)

        original = appverif_run.appverif_call
        appverif_run.appverif_call = fake_call
        try:
            appverif_run.configure_stops("appverif.exe", "test_appverif_1.exe", appverif_run.stop_codes())
        finally:
            appverif_run.appverif_call = original
        values = []
        for call in calls:
            self.assertEqual(call[0], "-configure")
            self.assertEqual(call[-2], "-with")
            setting = call[-1]
            self.assertTrue(setting.startswith("ErrorReport="), setting)
            values.append(int(setting[len("ErrorReport="):], 16))
        return values

    def test_every_configured_stop_is_active_and_logged(self):
        for value in self.configured_values() + self.configured_values(failures=(0x14, 0x305)):
            self.assertTrue(value & 0x1, f"ErrorReport=0x{value:X}: stop not active")
            self.assertTrue(value & 0x80, f"ErrorReport=0x{value:X}: stop not logged to the log file")
            self.assertTrue(value & 0x100, f"ErrorReport=0x{value:X}: stack trace not logged")
            self.assertFalse(value & 0x20, f"ErrorReport=0x{value:X}: breaks with no debugger attached")
            self.assertFalse(value & 0x40, f"ErrorReport=0x{value:X}: verifier exception with no debugger attached")

    def test_the_value_is_0x181(self):
        self.assertEqual(self.appverif_run.STOP_ERROR_REPORT, "0x181")

    def test_the_old_value_fails_the_bits(self):
        # the reversed witness: the shipped value until 2026-11-12 fails the
        # assertion above, so the test can tell the two apart
        self.assertFalse(0x2 & 0x1)
        self.assertFalse(0x2 & 0x80)


LOG_HEADER = '﻿<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n<avrf:logfile xmlns:avrf="Application Verifier">\n'
LOG_SESSION = '\t<avrf:logSession TimeStarted="2026-11-12 : 11:35:04" PID="17976" Version="2"/>\n'
LOG_SESSION_WITH_STOP = (
    '\t<avrf:logSession TimeStarted="2026-11-12 : 13:10:00" PID="2001" Version="2">\n'
    '\t\t<avrf:logEntry Time="2026-11-12 : 13:10:01" LayerName="Handles" StopCode="0x300" Severity="Error">\n'
    '\t\t\t<avrf:message>Invalid handle exception for current stack trace.</avrf:message>\n'
    '\t\t\t<avrf:stackTrace>\n'
    '\t\t\t\t<avrf:trace>vfbasics!+7ffb0000 ( @ 0)</avrf:trace>\n'
    '\t\t\t\t<avrf:trace>ntdll!KiUserExceptionDispatcher+2e ( @ 0)</avrf:trace>\n'
    '\t\t\t\t<avrf:trace>KERNELBASE!CloseHandle+4f ( @ 0)</avrf:trace>\n'
    '\t\t\t\t<avrf:trace>test_iocp_appverif_2001!test_windows_port_exclusive+90 (tests\\test_iocp.cpp @ 2451)</avrf:trace>\n'
    '\t\t\t\t<avrf:trace>test_iocp_appverif_2001!main+20 (tests\\test_iocp.cpp @ 3000)</avrf:trace>\n'
    '\t\t\t</avrf:stackTrace>\n'
    '\t\t</avrf:logEntry>\n'
    '\t</avrf:logSession>\n'
)
LOG_FOOTER = '</avrf:logfile>\n'


class CannedLogs(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.appverif_run = load_appverif_run()
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def log(self, name, text):
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return path

    def clean_log(self, name="clean.xml"):
        # the 205-byte log of a process with no stop, as captain exported it
        return self.log(name, LOG_HEADER + LOG_SESSION + LOG_FOOTER)

    def stop_log(self, name="stop.xml"):
        return self.log(name, LOG_HEADER + LOG_SESSION_WITH_STOP + LOG_FOOTER)

    def result(self, logs, exit_code=0, **kwargs):
        stops, unreadable = self.appverif_run.read_logs(logs)
        return self.appverif_run.RunResult(
            image_name="test_iocp_appverif_2001.exe", exit_code=exit_code, stops=stops,
            unreadable=unreadable, logs=len(logs), **kwargs,
        )


class VerdictTest(CannedLogs):
    """The three independent ways a stop fails the run, each on canned logs."""

    def verdict(self, result, self_checked=True):
        return self.appverif_run.verdict(result, self_checked)

    def test_clean_logs_pass(self):
        status, lines = self.verdict(self.result([self.clean_log("a.xml"), self.clean_log("b.xml")]))
        self.assertEqual(status, 0, lines)
        self.assertTrue(lines[-1].startswith("appverif_run: PASS"), lines)

    def test_parsed_stop_fails(self):
        # 1. a parsed logEntry: a continuable stop leaves exit 0 and only the log shows it
        status, lines = self.verdict(self.result([self.clean_log(), self.stop_log()], exit_code=0))
        self.assertEqual(status, self.appverif_run.EXIT_FAIL)
        self.assertIn("layer=Handles code=0x300", lines[0])
        self.assertIn("test_windows_port_exclusive", lines[0])
        self.assertIn("KERNELBASE!CloseHandle", lines[0])
        self.assertIn("1 verifier stops", lines[-1])

    def test_verifier_stop_exit_without_a_logged_stop_fails_on_its_own(self):
        # 2. STATUS_VERIFIER_STOP with zero parsed stops: the unfixed tool's
        # heap plant, red only by its exit status; now its own failure
        for code in (0xC0000421, 0xC0000421 - (1 << 32)):
            status, lines = self.verdict(self.result([self.clean_log()], exit_code=code))
            self.assertEqual(status, self.appverif_run.EXIT_UNPROVEN, code)
            self.assertIn("verifier stop not logged", lines[-1])
            self.assertNotIn("PASS", lines[-1])

    def test_logged_stop_with_verifier_stop_exit_is_a_stop(self):
        status, lines = self.verdict(self.result([self.stop_log()], exit_code=0xC0000421))
        self.assertEqual(status, self.appverif_run.EXIT_FAIL)
        self.assertNotIn("not logged", lines[-1])

    def test_log_without_session_is_not_zero_stops(self):
        # 3a. an exported log with no logSession
        bare = self.log("bare.xml", LOG_HEADER + LOG_FOOTER)
        status, lines = self.verdict(self.result([self.clean_log(), bare]))
        self.assertEqual(status, self.appverif_run.EXIT_UNPROVEN)
        self.assertIn("verifier log unreadable", lines[-1])
        self.assertTrue(any("no logSession" in line for line in lines), lines)

    def test_unparseable_log_is_not_zero_stops(self):
        # 3b. a log that does not parse (cut off mid-write)
        cut = self.log("cut.xml", (LOG_HEADER + LOG_SESSION_WITH_STOP)[:300])
        status, lines = self.verdict(self.result([cut]))
        self.assertEqual(status, self.appverif_run.EXIT_UNPROVEN)
        self.assertTrue(any("not a verifier log" in line for line in lines), lines)

    def test_empty_or_missing_log_is_not_zero_stops(self):
        empty = self.log("empty.xml", "")
        for logs in ([empty], [self.root / "missing.xml"], []):
            status, lines = self.verdict(self.result(logs))
            self.assertEqual(status, self.appverif_run.EXIT_UNPROVEN, logs)
            self.assertIn("verifier log unreadable", lines[-1])

    def test_no_self_check_never_passes(self):
        status, lines = self.verdict(self.result([self.clean_log()]), self_checked=False)
        self.assertEqual(status, self.appverif_run.EXIT_UNPROVEN)
        self.assertIn("no self-check", lines[-1])

    def test_test_failure_keeps_its_exit_status(self):
        status, _ = self.verdict(self.result([self.clean_log()], exit_code=7))
        self.assertEqual(status, 7)

    def test_timeout_and_cleanup_failure_fail(self):
        status, _ = self.verdict(self.result([self.clean_log()], exit_code=None, timed_out=True))
        self.assertEqual(status, self.appverif_run.EXIT_FAIL)
        status, _ = self.verdict(self.result([self.clean_log()], cleanup_failed=True))
        self.assertEqual(status, self.appverif_run.EXIT_FAIL)

    def test_setup_error_fails(self):
        result = self.appverif_run.RunResult(image_name="x_appverif_1.exe", setup_error="appverif -enable failed")
        status, lines = self.verdict(result)
        self.assertEqual(status, self.appverif_run.EXIT_SETUP)
        self.assertIn("could not run under the verifier", lines[-1])


class SelfCheckJudgementTest(CannedLogs):
    def heap_stop_log(self):
        return self.log("heap.xml", LOG_HEADER + LOG_SESSION_WITH_STOP.replace(
            'LayerName="Handles" StopCode="0x300"', 'LayerName="Heaps" StopCode="0xF"') + LOG_FOOTER)

    def judge(self, program, layer, result):
        return self.appverif_run.judge_self_check(program, layer, result)

    def test_plants_with_their_layer_pass(self):
        self.assertEqual(self.judge("p.exe", "Handles", self.result([self.stop_log()])), [])
        self.assertEqual(self.judge("p.exe", "Heaps", self.result([self.heap_stop_log()], exit_code=0xC0000421)), [])

    def test_plant_without_a_logentry_fails(self):
        # what the unfixed tool gave: exit 0 / 0xC0000421 and a session with no entry
        problems = self.judge("p.exe", "Handles", self.result([self.clean_log()], exit_code=0))
        self.assertEqual(len(problems), 1, problems)
        problems = self.judge("p.exe", "Heaps", self.result([self.clean_log()], exit_code=0xC0000421))
        self.assertTrue(any("verifier stop not logged" in problem for problem in problems), problems)

    def test_plant_with_another_layer_fails(self):
        problems = self.judge("p.exe", "Heaps", self.result([self.stop_log()]))
        self.assertTrue(any("no Heaps logEntry" in problem for problem in problems), problems)

    def test_clean_control_must_be_clean(self):
        self.assertEqual(self.judge("c.exe", None, self.result([self.clean_log()])), [])
        self.assertNotEqual(self.judge("c.exe", None, self.result([self.stop_log()])), [])
        self.assertNotEqual(self.judge("c.exe", None, self.result([self.clean_log()], exit_code=1)), [])
        self.assertNotEqual(self.judge("c.exe", None, self.result([self.log("e.xml", "")])), [])


class StampTest(unittest.TestCase):
    def setUp(self):
        self.appverif_run = load_appverif_run()
        self.exe_dir = Path("/build/bin/Debug")
        self.plants = {name: f"sha-{name}" for name, _ in self.appverif_run.SELF_CHECK_PROGRAMS}
        self.stamp = self.appverif_run.make_stamp("tool", "av", self.exe_dir, self.plants, "2026-11-12T18:00:00Z")

    def check(self, stamp, current=None, exe_dir=None, tool="tool", av="av"):
        digests = current if current is not None else self.plants

        def plant_sha(path):
            return digests.get(path.name)

        return self.appverif_run.check_stamp(stamp, tool, av, exe_dir or self.exe_dir, plant_sha)

    def test_stamp_of_this_build_covers_the_run(self):
        self.assertEqual(self.check(self.stamp), [])

    def test_stamp_of_another_build_is_refused(self):
        self.assertTrue(self.check(self.stamp, exe_dir=Path("/other/bin/Debug")))
        rebuilt = dict(self.plants, **{"appverif_plant_clean.exe": "rebuilt"})
        self.assertTrue(self.check(self.stamp, current=rebuilt))
        missing = {name: digest for name, digest in self.plants.items() if name != "appverif_plant_clean.exe"}
        self.assertTrue(self.check(self.stamp, current=missing))

    def test_stamp_of_another_tool_or_setting_is_refused(self):
        self.assertTrue(self.check(self.stamp, tool="edited"))
        self.assertTrue(self.check(self.stamp, av="other"))
        old = dict(self.stamp, stop_error_report="0x2")
        self.assertTrue(self.check(old))
        self.assertTrue(self.check({"version": 0}))
        self.assertTrue(self.check(None))


if __name__ == "__main__":
    unittest.main()
