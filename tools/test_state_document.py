"""Controls for reading a raw contract-audit document as the records its writer wrote."""

import gzip
from pathlib import Path
import tempfile
import unittest

if __package__:
    from . import state_document
else:
    import state_document


def parse(payload):
    return list(state_document.records(iter(payload.splitlines(keepends=True))))


class Records(unittest.TestCase):
    def test_one_line_per_record_is_the_ordinary_case(self):
        self.assertEqual(parse(b"a = 1\nb = 2\n"), [b"a = 1\n", b"b = 2\n"])

    def test_a_string_may_carry_newlines_and_nuls(self):
        payload = b'a = "one\ntwo\x00three"\nb = 2\n'
        self.assertEqual(parse(payload), [b'a = "one\ntwo\x00three"\n', b"b = 2\n"])

    def test_an_escaped_quote_does_not_close_the_string(self):
        payload = b'a = "say \\"hi\\"\nstill inside"\nb = 2\n'
        self.assertEqual(
            parse(payload), [b'a = "say \\"hi\\"\nstill inside"\n', b"b = 2\n"]
        )

    def test_an_escaped_backslash_before_a_quote_closes_the_string(self):
        payload = b'a = "ends with a backslash\\\\"\nb = 2\n'
        self.assertEqual(
            parse(payload), [b'a = "ends with a backslash\\\\"\n', b"b = 2\n"]
        )

    def test_a_container_of_strings_is_one_record(self):
        payload = b'a = ["one\ntwo";"three"; ]\nb = 2\n'
        self.assertEqual(parse(payload), [b'a = ["one\ntwo";"three"; ]\n', b"b = 2\n"])

    def test_an_unterminated_string_is_handed_back_not_dropped(self):
        payload = b'a = "never closed\nb = 2\n'
        self.assertEqual(parse(payload), [payload])

    def test_a_document_without_a_final_newline_keeps_its_last_record(self):
        self.assertEqual(parse(b"a = 1\nb = 2"), [b"a = 1\n", b"b = 2"])


class Fields(unittest.TestCase):
    def test_the_field_is_the_path_before_the_first_separator_outside_the_quotes(self):
        self.assertEqual(
            state_document.field(b"clock.real_time = 89713\n"), "clock.real_time"
        )
        self.assertEqual(state_document.field(b'a.b = "x = y"\n'), "a.b")
        self.assertEqual(
            state_document.field(b'map[key="a = b"].value = 3\n'),
            'map[key="a = b"].value',
        )
        self.assertIsNone(state_document.field(b"not a field\n"))

    def test_a_line_inside_a_string_is_not_a_field(self):
        record = b'file = "head\nmo[1].MovableObject.m_RestTimer.Timer.m_StartRealTime = 5\ntail"\n'
        self.assertEqual(state_document.field(record), "file")
        self.assertFalse(state_document.single_line(record))

    def test_single_line_holds_for_the_records_the_writer_wrote_on_one_line(self):
        self.assertTrue(state_document.single_line(b"a = 1\n"))
        self.assertTrue(state_document.single_line(b"a = 1"))
        self.assertFalse(state_document.single_line(b'a = "one\ntwo"\n'))

    def test_text_drops_only_the_terminating_newline(self):
        self.assertEqual(state_document.text(b'a = "one\ntwo"\n'), 'a = "one\ntwo"')


class Documents(unittest.TestCase):
    def test_a_gzipped_and_a_plain_document_read_the_same(self):
        directory = Path(tempfile.mkdtemp())
        payload = b'a = "one\ntwo"\nb = 2\n'
        plain = directory / "state.txt"
        plain.write_bytes(payload)
        packed = directory / "state.txt.gz"
        with gzip.open(packed, "wb") as stream:
            stream.write(payload)
        for path in (plain, packed):
            with (
                self.subTest(path=path.name),
                state_document.open_document(path) as stream,
            ):
                self.assertEqual(
                    list(state_document.records(stream)),
                    [b'a = "one\ntwo"\n', b"b = 2\n"],
                )


if __name__ == "__main__":
    unittest.main()
