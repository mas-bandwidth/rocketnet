#!/usr/bin/env python3
"""Exercise broken moved links, compatibility anchors, and fenced examples."""

import tempfile
import unittest
from pathlib import Path

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


if __name__ == '__main__':
    unittest.main()
