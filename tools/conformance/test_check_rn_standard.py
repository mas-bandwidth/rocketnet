#!/usr/bin/env python3

"""Regression tests for the standard's prose/citation boundary."""

import re
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


class PinExpressionTests(unittest.TestCase):
    def faults(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "m5.tsv").write_text(
                "#include \"rocketnet_message.h\"\n" + "".join("%s\t%s\t%s\n" % row for row in rows),
                encoding="utf-8")
            saved = standard.PINS_DIR
            standard.PINS_DIR = directory
            try:
                faults = []
                standard.read_pins(faults)
                return [text for _, text in faults]
            finally:
                standard.PINS_DIR = saved

    def test_bare_integer_is_refused(self):
        faults = self.faults([("ring unit", "16", "16")])
        self.assertEqual(len(faults), 1)
        self.assertIn("names no sdk identifier", faults[0])

    def test_literal_arithmetic_is_refused(self):
        self.assertEqual(len(self.faults([("window messages limit", "1 << 20", "1048576")])), 1)

    def test_standard_library_macro_is_refused(self):
        self.assertEqual(len(self.faults([("channel wide boundary", "UINT8_MAX + 1", "256")])), 1)

    def test_sdk_expressions_pass(self):
        self.assertEqual(self.faults([
            ("ring unit", "ROCKETNET_MESSAGE_RING_UNIT", "16"),
            ("window messages limit", "rn_pin_message_max_reliable_messages_in_flight", "1048576"),
            ("key bytes", "crypto_aead_chacha20poly1305_IETF_KEYBYTES", "32"),
            ("minimum numbered record", "ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + ROCKETNET_MESSAGE_NUMBER_BYTES + 1", "7"),
        ]), [])

    def test_repository_pins_all_name_the_sdk(self):
        faults = []
        standard.read_pins(faults)
        self.assertEqual([text for _, text in faults if "names no sdk identifier" in text], [])


class RulePinTests(unittest.TestCase):
    def pins(self):
        faults = []
        pins, includes, unpinned = standard.read_pins(faults)
        self.assertEqual(faults, [])
        self.assertEqual(unpinned, [])
        return pins, includes

    def sdk(self, name):
        return (Path(standard.ROOT) / "sdk" / name).read_text(encoding="utf-8")

    def define(self, name):
        match = re.search(r"^#define " + name + r"\s+(.+?)\s*$", self.sdk("rocketnet_wire.h"), re.M)
        self.assertIsNotNone(match, name)
        return match.group(1)

    def test_t5_pins_name_the_prefilter_constants(self):
        pins = {pin["name"]: pin for pin in self.pins()[0]}
        self.assertEqual(pins["packet type field width"]["expr"], "ROCKETNET_PACKET_TYPE_FIELD_BYTES")
        self.assertEqual(pins["filter epoch field width"]["expr"], "ROCKETNET_FILTER_EPOCH_FIELD_BYTES")
        self.assertEqual(pins["first keyed filter epoch"]["expr"], "ROCKETNET_FILTER_EPOCH_KEYED_FIRST")
        self.assertEqual(pins["keyed filter input width"]["expr"], "ROCKETNET_KEYED_FILTER_INPUT_BYTES")

    def test_keyed_filter_input_is_its_named_fields(self):
        # the packet type, the u16 packet length, then the 4 bytes after the
        # prefilter header: never the filter value's width or a bare number
        self.assertEqual(self.define("ROCKETNET_KEYED_FILTER_INPUT_BYTES"),
                         "( ROCKETNET_PACKET_TYPE_FIELD_BYTES + ROCKETNET_KEYED_FILTER_LENGTH_BYTES + "
                         "ROCKETNET_KEYED_FILTER_PAYLOAD_BYTES )")
        self.assertEqual(self.define("ROCKETNET_KEYED_FILTER_MIN_PACKET_BYTES"),
                         "( ROCKETNET_PREFILTER_BYTES + ROCKETNET_KEYED_FILTER_PAYLOAD_BYTES )")
        pins = [{"expr": expr} for expr in ("ROCKETNET_KEYED_FILTER_LENGTH_BYTES",
                                            "ROCKETNET_KEYED_FILTER_PAYLOAD_BYTES",
                                            "ROCKETNET_KEYED_FILTER_MIN_PACKET_BYTES")]
        self.assertEqual(standard.compile_pins(pins, []), {0: ("n", 2), 1: ("n", 4), 2: ("n", 10)})

    def test_record_header_geometry_is_written_once(self):
        # the one-record limits reach the function that writes record headers,
        # never a copy of its geometry in a header the pins compile
        for path in sorted((Path(standard.ROOT) / "sdk").glob("*.h")):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("3 + 3 + 3", text, path.name)
            self.assertIsNone(re.search(r"\brocketnet_max_unfragmented_message_bytes\s*\([^;]*\)\s*\{", text), path.name)
        self.assertRegex(self.sdk("rocketnet_message.cpp"),
                         r"\nint rocketnet_max_unfragmented_message_bytes\s*\(")
        self.assertNotIn("rocketnet_standard_pins.h", standard.BASE_INCLUDES)

    def test_m2_limits_pin_the_real_rule_functions(self):
        pins, includes = self.pins()
        self.assertIn('#include "rocketnet_message.cpp"', includes)
        limits = [pin for pin in pins if pin["name"].endswith("record limit") or pin["name"].endswith("record limit, WIDE")]
        self.assertEqual(len({pin["name"] for pin in limits}), 7)
        for pin in limits:
            self.assertRegex(pin["expr"], r"^(rocketnet_max_unfragmented_message_bytes|rocketnet_message_max_unfragmented_bytes"
                                          r"|rn_endpoint_unreliable_record_limit)\(")
        self.assertTrue(any(pin["expr"].startswith("rn_endpoint_unreliable_record_limit(") for pin in limits))
        values = standard.compile_pins(limits, includes)
        for index, pin in enumerate(limits):
            self.assertEqual(values[index], standard.parse_value(pin["value"]))

    def test_unsigned_64_bit_pins_keep_bit_63(self):
        # a uint64 value with bit 63 set is that unsigned number: never printed
        # or stored as a negative one (Argon F14)
        includes = ["RN_PIN_S_UINT64( planted_top_bit, 0x8000000000000001ull, 0 )"]
        pins = [{"expr": "0xFFFFFFFFFFFFFFFFull"},
                {"expr": "(uint64_t) 0x8000000000000000ull"},
                {"expr": "rn_pin_server_default_planted_top_bit"},
                {"expr": "(int64_t) -1"}]
        self.assertEqual(standard.compile_pins(pins, includes),
                         {0: ("n", 18446744073709551615), 1: ("n", 9223372036854775808),
                          2: ("n", 9223372036854775809), 3: ("n", -1)})

    def test_unreliable_record_rule_is_over_exactly_above_the_limit(self):
        _, includes = self.pins()
        expression = ("rn_endpoint_unreliable_over_record_capacity( "
                      "ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, "
                      "ROCKETNET_MESSAGE_WIDE_CHANNEL, %d, "
                      "ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )")
        pins = [{"expr": expression % 1404}, {"expr": expression % 1405}]
        self.assertEqual(standard.compile_pins(pins, includes), {0: ("n", 0), 1: ("n", 1)})


if __name__ == "__main__":
    unittest.main()
