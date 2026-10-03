import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import e2e_video as video


class VideoOracleEvidence(unittest.TestCase):
    def test_declared_capture_minimum_and_each_watch_summary_are_required(self):
        from feel.test_harness_cost import complete_cost_log
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'manifest.json').write_text(json.dumps(dict(fps=30, frames_saved=60, frames_dropped=0, minimum_duration_s=4)))
            (root / 'frames.jsonl').write_text('\n'.join(json.dumps(dict(frame=i, wall_ms=i*1000/30, saved=True)) for i in range(60)))
            self.assertFalse(video.recording_health(root)['complete'])
            (root / 'manifest.json').write_text(json.dumps(dict(fps=30, frames_saved=60, frames_dropped=0)))
            lines = [complete_cost_log(first=1, last=60)]
            for name in video.SCREEN_WATCH_RULES:
                lines += ['[text-watch] armed '+json.dumps(dict(armed='h15-'+name, rule='layout', state='always')),
                          '[text-watch] summary '+json.dumps(dict(watch='h15-'+name, frames=60, active_frames=60, violations=0))]
            (root / 'stdout.log').write_text('\n'.join(lines))
            peer = dict(peer='host', root=str(root), video_dir=str(root))
            with patch.object(video, 'freeze_scan', return_value=[]):
                good = video.capture_evidence_items({}, dict(name='unit'), peer)
                self.assertTrue(all(row['probe']=='pass' for row in good), good)
                (root / 'stdout.log').write_text('\n'.join(lines[:-1]))
                missing = video.capture_evidence_items({}, dict(name='unit'), peer)
                self.assertTrue(any(row['probe']=='incomplete' and row['id'].startswith('screen-') for row in missing))

    def test_p13_capped_stop_does_not_excuse_nine_seconds_of_running_freeze(self):
        rows = [dict(frame=n, saved=True, wall_ms=n * 100, screen='game', service_state='Running') for n in range(101)]
        allowed = []
        result = video.running_stills([(0, 10)], rows, capped_stop_ms=9000, allowed=allowed)
        self.assertTrue(result, dict(stills=result, allowed=allowed))

    def test_p14_failed_decoder_is_not_a_successful_empty_still_scan(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixture.mp4'
            path.write_bytes(b'synthetic invalid video')
            with patch.object(video.subprocess, 'run', return_value=SimpleNamespace(returncode=1, stderr='Invalid data found', stdout='')):
                try:
                    result = video.freeze_scan('ffmpeg', path, [])
                except (RuntimeError, ValueError):
                    return
        self.assertNotEqual(result, [], 'ffmpeg exited 1 but the scanner returned a successful empty result')

    def test_p15_single_frame_capture_is_not_healthy(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'manifest.json').write_text(json.dumps(dict(fps=30, frames_saved=1, frames_dropped=0)))
            (root / 'frames.jsonl').write_text(json.dumps(dict(frame=0, wall_ms=0, saved=True)) + '\n')
            (root / 'dropped.jsonl').write_text('')
            result = video.recording_health(root)
        self.assertTrue(result['starved'] or result.get('complete') is False, result)

    def test_p16_missing_recorder_and_watches_are_explicit_failed_checks(self):
        with tempfile.TemporaryDirectory() as folder:
            peer = dict(peer='host', root=folder, record=dict(exit_code=0), menu_script_failures=[])
            capture = dict(name='unit', peers=[peer])
            result = video.review(dict(name='unit', checklist=[]), capture, folder)
        failures = [row for row in result['checklist'] if row.get('finding') or row.get('probe') in ('fail', 'incomplete')]
        self.assertTrue(failures, result)


if __name__ == '__main__':
    unittest.main()
