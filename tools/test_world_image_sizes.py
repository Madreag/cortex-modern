import os
import json
from pathlib import Path
import tempfile
import unittest

from world_image_sizes import image_measurement, markdown, offered_scenes, transfer_totals, offered_archive
from acceptance_mod import sha256


class ImageSizes(unittest.TestCase):
    def setUp(self):
        retained = os.environ.get("CC_ACCEPTANCE_TEST_ROOT")
        if retained:
            self.root = Path(tempfile.mkdtemp(prefix="image-test-", dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name)
        self.scene = dict(name="Grasslands", module="Base.rte")
        self.archive = self.root/"image.ccsave"
        self.archive.write_bytes(b"fixture"*100)

    def test_lobby_list_is_authoritative(self):
        source = dict(activity_table=[dict(preset="Persistent World", module="Base.rte", scenes=[self.scene])])
        self.assertEqual(offered_scenes(source), [self.scene])
        with self.assertRaises(ValueError):
            offered_scenes(dict(activity_table=[]))

    def test_stream_size_not_inferred_from_archive(self):
        row = image_measurement(self.scene, self.archive, "[net-match] state transfer complete: 800 bytes\n")
        self.assertEqual(row["archive_bytes"], 700)
        self.assertEqual(row["received_bytes"], 800)
        self.assertFalse(row["compresses"])

    def test_missing_duplicate_or_wrong_image_refused(self):
        for text in ("", "[net-match] state transfer complete: 800 bytes\n"*2):
            with self.subTest(text=text), self.assertRaises(ValueError):
                image_measurement(self.scene, self.archive, text)
        with self.assertRaises(ValueError):
            image_measurement(self.scene, self.archive, "[net-match] state transfer complete: 800 bytes\n", "b"*64)

    def test_table_discloses_missing_scenes(self):
        table, complete = markdown([self.scene], [])
        self.assertFalse(complete)
        self.assertIn("NOT RUN", table)
        row = image_measurement(self.scene, self.archive, "[net-match] state transfer complete: 500 bytes\n")
        table, complete = markdown([self.scene], [row])
        self.assertTrue(complete)
        self.assertIn("| 700 | 500 |", table)

    def test_unknown_scene_does_not_complete_table(self):
        with self.assertRaises(ValueError):
            markdown([self.scene], [dict(name="Not offered", module="Base.rte")])

    def test_archive_is_the_exact_image_the_host_offered(self):
        offer = dict(tick=60, digest=sha256(self.archive), path=str(self.archive), bytes=700)
        log = '[net-world] offer '+json.dumps(offer)+'\n'
        path, digest, tick = offered_archive(log, self.root)
        self.assertEqual(path, self.archive.resolve())
        self.assertEqual(digest, sha256(self.archive))
        self.assertEqual(tick, 60)
        with self.assertRaises(ValueError):
            offered_archive(log, self.root/'different-runtime')
        with self.assertRaises(ValueError):
            offered_archive(log+'[net-world] offer '+json.dumps({**offer, 'tick':61}), self.root)

    def test_post_transfer_failure_stays_visible_in_the_table(self):
        row = image_measurement(self.scene, self.archive, '[net-match] state transfer complete: 800 bytes\n')
        row['engine_run_passed'] = False
        table, complete = markdown([self.scene], [row])
        self.assertTrue(complete)
        self.assertIn('FAILED AFTER TRANSFER', table)


if __name__ == "__main__":
    unittest.main(verbosity=2)
