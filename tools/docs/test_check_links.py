#!/usr/bin/env python3
"""Exercise broken moved links, compatibility anchors, and fenced examples."""

import contextlib
import io
import subprocess
import tempfile
import unittest
from pathlib import Path

import check_links
from check_links import anchors, check


class LinkTests(unittest.TestCase):
    def test_heading_slugs_and_compatibility_anchor(self):
        text = '# API `name_value` (C)\n## Repeat\n## Repeat\n<a id="old-name"></a>\n'
        self.assertEqual(anchors(text), {'api-name_value-c', 'repeat', 'repeat-1', 'old-name'})

    def test_fenced_headings_do_not_create_targets(self):
        text = '# Real\n```sh\n# Shell comment\n```\n~~~text\n## Example\n~~~\n    # Indented code\n'
        self.assertEqual(anchors(text), {'real'})

    def test_moved_target_fragment_and_reference_links(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / 'docs').mkdir()
            (root / 'docs' / 'target.md').write_text('# New\n<a id="old"></a>\n')
            page = root / 'README.md'
            page.write_text('[ok](docs/target.md#old)\n[wrong](docs/target.md#gone)\n'
                            '[missing](moved.md)\n[ref]: docs/target.md#new\n'
                            '```md\n[example](not-a-file.md)\n```\n'
                            '<!-- [comment](also-not-a-file.md) -->\n')
            count, failures = check([page], root)
            self.assertEqual(count, 4)
            self.assertEqual(len(failures), 2)
            self.assertIn('missing heading or anchor', failures[0])
            self.assertIn('missing target', failures[1])


class TrackedTreeTests(unittest.TestCase):
    """The guide list is git's: a guide outside the old roots is checked, a
    historical record is not, and a stale record name or an empty list fails."""

    def make_tree(self, root, files):
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        for rel, text in files.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        subprocess.run(['git', '-C', str(root), 'add', '-A'], check=True)

    def run_main(self, root):
        saved = check_links.ROOT
        check_links.ROOT = root
        try:
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = check_links.main()
            return code, out.getvalue() + err.getvalue()
        finally:
            check_links.ROOT = saved

    def base(self):
        return {'README.md': '[guide](tests/README.md)\n',
                'tests/README.md': '# Tests\n[readme](../README.md)\n',
                'docs/audits/old.md': '[gone](nowhere.md)\n',
                'docs/runs/old.md': '# Run\n',
                'docs/lan-bench/old.md': '# Bench\n',
                'docs/RELEASE-NOTES-v0.md': '[gone](nowhere.md)\n',
                'docs/QUALIFICATION-v0.md': '# Q\n'}

    def test_clean_tree_passes_and_skips_historical_records(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.make_tree(root, self.base())
            code, text = self.run_main(root)
            self.assertEqual(code, 0, text)
            self.assertIn('in 2 guides', text)

    def test_broken_link_in_tests_readme_fails(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            files = self.base()
            files['tests/README.md'] += '[broken](no-such-file.md)\n'
            self.make_tree(root, files)
            code, text = self.run_main(root)
            self.assertEqual(code, 1, text)
            self.assertIn('tests/README.md:3: no-such-file.md: missing target', text)

    def test_untracked_guide_is_not_a_guide(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.make_tree(root, self.base())
            (root / 'scratch.md').write_text('[broken](no-such-file.md)\n')
            code, text = self.run_main(root)
            self.assertEqual(code, 0, text)

    def test_stale_historical_name_fails(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            files = self.base()
            del files['docs/lan-bench/old.md']
            self.make_tree(root, files)
            code, text = self.run_main(root)
            self.assertEqual(code, 1, text)
            self.assertIn("historical record 'docs/lan-bench/'", text)

    def test_empty_guide_list_fails(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.make_tree(root, {})
            code, text = self.run_main(root)
            self.assertEqual(code, 1, text)
            self.assertIn('no maintained .md guide', text)


if __name__ == '__main__':
    unittest.main()
