#!/usr/bin/env python3
"""Signed full-record referee regression; run beside the session-plane fixture."""

from pathlib import Path
import json
import sys
import unittest

# Reuse the real signed endpoint fixture without changing the directory lane.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/session_directory"))
from connection_authority import ConnectionErrorReply, MAX_RECORD_BYTES
from test_session_directory import ConnectionAuthorityTests, sample_register


class FullRecordFixture(ConnectionAuthorityTests):
    def test_frame_tie_full_record_keeps_a_new_split_available(self):
        self.referee_fixture()
        self.assertEqual(self.frame_check_in(0, [0, 1])["members"], [0, 1])
        # A long-lived match can have this many first-arrival decisions. Seed the
        # persisted history with the exact entry the signed endpoint just wrote,
        # then cross the real storage boundary through new signed requests. This
        # avoids thousands of redundant encrypted database writes in every suite.
        with self.store.connections._lock:
            record = json.loads(json.dumps(self.store.connections._records[self.sid]))
            template = next(iter(record["frame_ties"].values()))
            size = len(json.dumps(record, separators=(",", ":")).encode())
            value_bytes = len(json.dumps(template, separators=(",", ":")).encode())
            frame = 1000
            while True:
                key = f"0:7:{sample_register()['match_config_hash']}:{frame}"
                extra = len(json.dumps(key).encode()) + 1 + value_bytes + 1
                if size + extra > MAX_RECORD_BYTES - 600:
                    break
                record["frame_ties"][key] = dict(template)
                size += extra
                frame += 1
            self.assertEqual(len(json.dumps(record, separators=(",", ":")).encode()), size)
            self.store.connections._save(self.sid, record)
        for offset in range(8):
            self.now += 3
            self.wall += 3
            try:
                answer = self.frame_check_in(0, [0, 1], frame=frame + offset)
            except ConnectionErrorReply as unavailable:
                with self.store.connections._lock:
                    persisted = self.store.connections._records[self.sid]
                    persisted_count = len(persisted["frame_ties"])
                    persisted_bytes = len(json.dumps(persisted, separators=(",", ":")).encode())
                self.fail(f"seed=0 shape=full_referee_record: a new split froze with HTTP {unavailable.status} "
                          f"after {persisted_count} persisted decisions ({persisted_bytes} bytes)")
            self.assertEqual((answer["status"], answer["members"]), ("decided", [0, 1]))


def load_tests(loader, tests, pattern):
    return unittest.TestSuite([FullRecordFixture("test_frame_tie_full_record_keeps_a_new_split_available")])


if __name__ == "__main__":
    unittest.main()
