#!/usr/bin/env python3
"""Tests for the image name tools/windows/appverif_run.py verifies, and for
the removal of that image's registry key after the run (RemoveImageKeyTest).

Application Verifier settings and logs are keyed by image name, machine-wide:
while a run has functional.exe under verification, every functional.exe on the
host (a CI leg, another worker's build) is verified too, its logs land beside
the run's, and its own -delete logs removes the run's. The run therefore
verifies a copy of the executable under a name no other process uses.
"""

import contextlib
import importlib.util
import io
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


class ReportTest(unittest.TestCase):
    """--report keeps the run's own lines out of the executable's output: a
    whole functional-suite log is held to its skips and registry line by line."""

    def setUp(self):
        self.appverif_run = load_appverif_run()

    def test_lines_go_to_the_report_file_and_not_stdout(self):
        with tempfile.TemporaryDirectory(dir=".") as directory:
            path = Path(directory) / "report.txt"
            self.appverif_run.REPORT_PATH = str(path)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.appverif_run.report("appverif_run: PASS x")
                self.appverif_run.report("appverif_run: FAIL y", error=True)
            self.assertEqual(path.read_text(encoding="utf-8"), "appverif_run: PASS x\nappverif_run: FAIL y\n")
            self.assertEqual(out.getvalue(), "")
            self.assertEqual(err.getvalue(), "")

    def test_without_a_report_file_lines_go_to_stdout_and_failures_to_stderr(self):
        self.appverif_run.REPORT_PATH = None
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.appverif_run.report("appverif_run: PASS x")
            self.appverif_run.report("appverif_run: FAIL y", error=True)
        self.assertEqual(out.getvalue(), "appverif_run: PASS x\n")
        self.assertEqual(err.getvalue(), "appverif_run: FAIL y\n")


if __name__ == "__main__":
    unittest.main()
