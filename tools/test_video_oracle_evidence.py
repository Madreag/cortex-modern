import json
from pathlib import Path
import tempfile
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import e2e_video as video


class VideoOracleEvidence(unittest.TestCase):
    def test_motion_between_matching_endpoints_is_visible(self):
        ffmpeg = video.find_ffmpeg()
        self.assertIsNotNone(ffmpeg, video.missing_ffmpeg())
        real_run = subprocess.run
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'motion.mkv'
            for pixels, expected in ((1280, 1280), (400, 400), (0, 0)):
                with self.subTest(pixels=pixels):
                    frames = []
                    for index in range(11):
                        frame = bytearray(64 * 64)
                        if index == 4:
                            for pixel in range(pixels):
                                frame[pixel] = 255
                        frames.append(frame)
                    encoded = real_run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
                                        '-f', 'rawvideo', '-pixel_format', 'gray', '-video_size', '64x64',
                                        '-framerate', '5', '-i', '-', '-c:v', 'ffv1', str(path)],
                                       input=b''.join(frames), capture_output=True, timeout=60)
                    self.assertEqual(encoded.returncode, 0, encoded.stderr)
                    rows = [dict(frame=i, wall_ms=i * 200, screen='game', service_state='Running') for i in range(11)]
                    def decode(command, **kwargs):
                        if '-vf' in command:
                            return SimpleNamespace(returncode=0, stderr='freeze_start: 0\nfreeze_end: 2\n')
                        return real_run(command, **kwargs)
                    with patch.object(video.subprocess, 'run', side_effect=decode):
                        stills = video.freeze_scan(ffmpeg, path, rows)
                    self.assertEqual(len(stills), 1)
                    self.assertEqual(stills[0]['changed_pixels'], expected)
                    self.assertEqual(stills[0]['changed_pixels'] >= video.MOTION_PIXELS, pixels > 0)

    def test_a_calm_view_with_small_live_motion_is_not_a_still(self):
        # A parked camera on a quiet view: 944 pixels of a 960x540 picture change (the closing-window still of f7502-testpc-mac-pin1).
        ffmpeg = video.find_ffmpeg()
        self.assertIsNotNone(ffmpeg, video.missing_ffmpeg())
        real_run = subprocess.run
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'calm.mkv'
            frames = []
            for index in range(45):
                frame = bytearray([90]) * (960 * 540)
                if index >= 20:
                    for row in range(59):
                        start = (200 + row) * 960 + 600
                        frame[start:start + 16] = bytes([200]) * 16
                frames.append(frame)
            encoded = real_run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-f', 'rawvideo', '-pixel_format', 'gray',
                                '-video_size', '960x540', '-framerate', '30', '-i', '-', '-c:v', 'ffv1', str(path)],
                               input=b''.join(frames), capture_output=True, timeout=60)
            self.assertEqual(encoded.returncode, 0, encoded.stderr)
            rows = [dict(frame=i, wall_ms=i * 1000 / 30, screen='game', service_state='Running') for i in range(45)]
            def decode(command, **kwargs):
                if '-vf' in command:
                    return SimpleNamespace(returncode=0, stderr='freeze_start: 0\nfreeze_end: 1.45\n')
                return real_run(command, **kwargs)
            with patch.object(video.subprocess, 'run', side_effect=decode):
                stills = video.freeze_scan(ffmpeg, path, rows)
            self.assertEqual([still['changed_pixels'] for still in stills], [944])
            self.assertGreaterEqual(stills[0]['changed_pixels'], video.MOTION_PIXELS, 'a picture with live motion is judged still')

    def test_each_recorder_encoder_keeps_a_frozen_picture_within_the_motion_levels(self):
        ffmpeg = video.find_ffmpeg()
        self.assertIsNotNone(ffmpeg, video.missing_ffmpeg())
        with tempfile.TemporaryDirectory() as folder:
            picture = Path(folder) / 'picture.pgm'
            # A busy picture with sharp edges, the encoder's hardest case for a frozen frame.
            picture.write_bytes(b'P5 960 540 255\n' + bytes((x * 7 + y * 13 + (x ^ y)) % 256 for y in range(540) for x in range(960)))
            for arguments in (['-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '20'], ['-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '20', '-g', '1']):
                with self.subTest(encoder=arguments):
                    clip = Path(folder) / 'frozen.mp4'
                    encoded = subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-loop', '1', '-framerate', '30', '-i', str(picture),
                                              '-frames:v', '60', *arguments, '-pix_fmt', 'yuv420p', str(clip)], capture_output=True, timeout=120)
                    if encoded.returncode != 0:
                        self.skipTest(f'{arguments[1]} unavailable: {encoded.stderr[-200:]}')
                    self.assertLess(video.span_changed_pixels(ffmpeg, clip, 0.0, 1.95), video.MOTION_PIXELS)

    def test_setup_frames_are_distinct_from_running_play(self):
        rows = [dict(frame=i, wall_ms=i * 200, screen='game', service_state='Running', activity_editing=True) for i in range(11)]
        self.assertEqual(video.running_stills([(0, 2)], rows), [])
        for row in rows:
            row['activity_editing'] = False
        self.assertTrue(video.running_stills([(0, 2)], rows))
        for row in rows:
            row.pop('activity_editing')
        self.assertTrue(video.running_stills([(0, 2)], rows), 'missing state must not excuse a frozen game')

    def test_census_note_does_not_change_the_measured_cost_verdict(self):
        from feel.test_harness_cost import complete_cost_log
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            peer = dict(peer='host', root=str(root))
            for cost, expected in ((2.8, 'pass'), (50.1, 'fail')):
                with self.subTest(cost=cost):
                    log = complete_cost_log(first=1, last=2, costs={'census': cost})
                    log += '[mem-census] tick=1 census_us=lua:2700\n'
                    (root / 'stdout.log').write_text(log)
                    with patch.object(video, 'freeze_scan', return_value=[]):
                        rows = video.capture_evidence_items({}, dict(name='unit'), peer)
                    measured = next(row for row in rows if row['id'] == 'harness-cost-host')
                    self.assertIn('census instant=', measured['instrumentation']['reason'])
                    self.assertEqual(measured['probe'], expected, measured)

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
