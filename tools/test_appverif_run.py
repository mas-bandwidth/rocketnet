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
import subprocess
import tempfile
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


class LayersAndStopsTest(unittest.TestCase):
    """What the run turns on: the layers that cover the library's own lock,
    and stops that are active and logged."""

    def setUp(self):
        self.appverif_run = load_appverif_run()

    def test_srwlock_layer_and_its_stops_are_enabled(self):
        # the library's mutex is an SRWLOCK; the Locks layer checks only
        # critical sections
        self.assertIn("SRWLock", self.appverif_run.BASICS_LAYERS)
        self.assertIn("Exceptions", self.appverif_run.BASICS_LAYERS)
        codes = self.appverif_run.stop_codes()
        for code in ("0x250", "0x251", "0x255", "0x650"):
            self.assertIn(code, codes)

    def test_every_stop_is_active_and_logged_with_its_stack(self):
        value = int(self.appverif_run.STOP_ERROR_REPORT, 16)
        self.assertEqual(value & 0x1, 0x1, "bit 0x1 clear switches every stop off")
        self.assertEqual(value & 0x80, 0x80)
        self.assertEqual(value & 0x100, 0x100)
        self.assertEqual(value & 0x60, 0, "no breakpoint, no verifier exception: no debugger is attached")

    def test_no_issued_stop_is_refused(self):
        def unissued(appverif, args, check=True):
            code = args[1]
            return subprocess.CompletedProcess(
                args, 1, f"Stop code [{code}] is not issued by any of the registered providers", "")

        self.appverif_run.appverif_call = unissued
        with self.assertRaisesRegex(RuntimeError, "set no stop"):
            self.appverif_run.configure_stops("appverif", "x_appverif_1.exe", ["0x1", "0x2", "0x3"])


class ExportLogsTest(unittest.TestCase):
    """Every exported log belongs to this run: an earlier run's numbered
    siblings are deleted first."""

    def setUp(self):
        self.appverif_run = load_appverif_run()
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def fake_export(self, logs):
        def call(appverif, args, check=True):
            to = next(arg[len("To="):] for arg in args if arg.startswith("To="))
            offset = int(next(arg[len("Log="):] for arg in args if arg.startswith("Log=")))
            if -offset >= logs:
                return subprocess.CompletedProcess(args, 1, "", "no log")
            Path(to).write_text("<log/>")
            return subprocess.CompletedProcess(args, 0, "", "")
        return call

    def test_stale_numbered_logs_are_removed(self):
        for name in ("out.xml", "out.1.xml", "out.2.xml", "out.5.xml", "other.xml", "out.x.xml", "out.3.txt"):
            (self.dir / name).write_text("old")
        self.appverif_run.appverif_call = self.fake_export(2)
        paths = self.appverif_run.export_logs("appverif", "x_appverif_1.exe", self.dir / "x.exe", self.dir / "out.xml")
        self.assertEqual([p.name for p in paths], ["out.xml", "out.1.xml"])
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()),
                         ["other.xml", "out.1.xml", "out.3.txt", "out.x.xml", "out.xml"])
        self.assertEqual((self.dir / "out.xml").read_text(), "<log/>")

    def test_runaway_export_is_refused(self):
        self.appverif_run.MAX_EXPORTED_LOGS = 3
        self.appverif_run.appverif_call = self.fake_export(10)
        with self.assertRaisesRegex(RuntimeError, "more than 3 verifier logs"):
            self.appverif_run.export_logs("appverif", "x_appverif_1.exe", self.dir / "x.exe", self.dir / "out.xml")


if __name__ == "__main__":
    unittest.main()
