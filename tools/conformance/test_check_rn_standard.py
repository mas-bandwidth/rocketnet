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


class SubsectionAnchorTests(unittest.TestCase):
    def faults(self, subsection):
        with tempfile.TemporaryDirectory() as directory:
            docs = {}
            for tag, filename in standard.DOCUMENTS.items():
                path = Path(directory) / filename
                text = '# Fixture\n'
                if tag != "RN-1":
                    text += '<a id="m1"></a>\n## m1. Fixture\n\n'
                if tag == "RN-2":
                    text += subsection
                if tag == "RN-R":
                    text += '<a id="rh"></a>\n## rh. Revision history\n'
                path.write_text(text, encoding="utf-8")
                docs[tag] = standard.Document(tag, path)
            faults = []
            standard.check_outline(docs, [("m1", "Fixture", "RN-2")], faults, None)
            return faults

    def test_explicit_subsection_anchor_is_required(self):
        self.assertTrue(self.faults('### m1.1. Detail\n'))

    def test_matching_subsection_anchor_passes(self):
        self.assertEqual(self.faults('<a id="m1.1"></a>\n### m1.1. Detail\n'), [])

    def test_wrong_subsection_anchor_fails(self):
        self.assertTrue(self.faults('<a id="m1.2"></a>\n### m1.1. Detail\n'))

    def test_duplicate_subsection_id_fails(self):
        section = '<a id="m1.1"></a>\n### m1.1. Detail\n\n'
        self.assertTrue(self.faults(section + section))


class PacketLayoutTests(unittest.TestCase):
    def faults(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "TRANSPORT.md"
            path.write_text('<a id="t8"></a>\n## t8. Packets\n\n'
                            '<a id="t8.6"></a>\n### t8.6. Payload\n\n'
                            '| Name | Offset | Size | Type | Meaning | Valid values |\n'
                            '|---|---|---|---|---|---|\n' + rows, encoding="utf-8")
            faults = []
            standard.check_packet_layouts({"RN-1": standard.Document("RN-1", path)}, faults, None)
            return faults

    def payload(self, tag_offset):
        return ('| header | 0 | 10 | bytes[10] | header | any |\n'
                '| ack block | 10 | 7 to 1027 | bytes[n] | ACK | any |\n'
                '| payload data | 10 plus the ack block size | variable | bytes[n] | data | any |\n'
                f'| AEAD tag | {tag_offset} | 16 | bytes[16] | tag | any |\n')

    def test_contiguous_symbolic_fields_pass(self):
        self.assertEqual(self.faults(self.payload('10 plus the ack block size and the payload size')), [])

    def test_payload_tag_six_byte_gap_fails(self):
        self.assertTrue(self.faults(self.payload('16 plus the ack block size and the payload size')))

    def test_missing_payload_term_fails(self):
        self.assertTrue(self.faults(self.payload('10 plus the ack block size')))

    def test_unknown_offset_notation_is_not_silently_skipped(self):
        self.assertTrue(self.faults(self.payload('after all preceding data')))


if __name__ == "__main__":
    unittest.main()
