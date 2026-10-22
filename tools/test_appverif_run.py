#!/usr/bin/env python3
"""Tests for the image name tools/windows/appverif_run.py verifies.

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


if __name__ == "__main__":
    unittest.main()
