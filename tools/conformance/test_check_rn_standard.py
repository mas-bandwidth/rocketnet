#!/usr/bin/env python3

"""Regression tests for the standard's prose/citation boundary."""

import tempfile
import unittest
from pathlib import Path

import check_rn_standard as standard


class LanguageTests(unittest.TestCase):
    def faults(self, text, tag="RN-2", ident="m10"):
        with tempfile.TemporaryDirectory() as directory:
            docs = {}
            for name in standard.DOCUMENTS:
                path = Path(directory) / standard.DOCUMENTS[name]
                path.write_text(
                    f'<a id="{ident}"></a>\n## {ident}. Fixture\n\n{text}\n'
                    if name == tag else "# Empty fixture\n", encoding="utf-8")
                docs[name] = standard.Document(name, path)
            faults = []
            standard.check_language(docs, faults, None)
            return faults

    def test_inline_file_citation_is_rejected(self):
        self.assertTrue(self.faults('See `sdk/rocketnet_message_server.cpp`.'))

    def test_plain_file_citation_is_rejected(self):
        self.assertTrue(self.faults('See sdk/rocketnet_message_server.cpp.'))

    def test_inline_test_citation_is_rejected(self):
        self.assertTrue(self.faults('See `test_channel_delivery`.'))

    def test_inline_model_citation_is_rejected(self):
        self.assertTrue(self.faults('The `TLA+` model defines the rule.'))

    def test_inline_field_and_normative_actor_are_allowed(self):
        self.assertEqual(self.faults('A sender MUST set `required` to zero.'), [])

    def test_informative_implementation_citation_is_allowed(self):
        self.assertEqual(self.faults('See `sdk/rocketnet_message_server.cpp`.',
                                     tag="RN-R", ident="m10"), [])

    def test_informative_keyword_is_rejected(self):
        self.assertTrue(self.faults('A sender MUST set the field.', tag="RN-R"))


if __name__ == "__main__":
    unittest.main()
