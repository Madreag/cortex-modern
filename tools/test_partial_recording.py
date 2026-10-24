"""A recording whose engine a scene kills holds the pictures it finished; the driver takes those and carries the scene on."""
from pathlib import Path
import subprocess
import tempfile
import unittest

import e2e_video

FFMPEG = e2e_video.find_ffmpeg()
REPO = Path(__file__).resolve().parents[1]
# The recorder's own output flags (FrameRecorder::LaunchEncoder): half-second fragments a killed encoder cannot take back.
FRAGMENTED = ["-movflags", "+empty_moov+default_base_moof", "-frag_duration", "500000"]
FRAMES = 120


@unittest.skipUnless(FFMPEG, "no ffmpeg on this box")
class PartialRecordingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def recording(self, flags, kept_share):
        """Four seconds at 30 fps, cut where a killed encoder stops writing."""
        whole = self.root / "whole.mp4"
        subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=30:duration=4",
                        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "20", "-pix_fmt", "yuv420p", *flags, str(whole)], check=True)
        cut = self.root / "capture.mp4"
        data = whole.read_bytes()
        cut.write_bytes(data[:int(len(data) * kept_share)])
        rows = [{"frame": index, "video_frame": index, "wall_ms": 1000 + index * 33, "sim_tick": index, "screen": "game"} for index in range(FRAMES)]
        return cut, rows

    def test_the_recorder_writes_fragments(self):
        source = (REPO / "Source" / "System" / "FrameRecorder.cpp").read_text(encoding="utf-8")
        self.assertTrue(" ".join(FRAGMENTED) in source, "the recorder's encoder writes an MP4 a killed process leaves unreadable")

    def test_a_dropped_peer_keeps_the_pictures_its_recording_holds(self):
        video, rows = self.recording(FRAGMENTED, 0.7)
        picked = rows[::10]
        frames = self.root / "frames"
        held = e2e_video.extract_frames(FFMPEG, video, picked, frames, partial=True)
        pictures = sorted(path.name for path in frames.glob("frame-*.png"))
        self.assertTrue(0 < len(pictures) < len(picked), f"{len(pictures)} of {len(picked)} pictures from a recording cut at 70 %")
        self.assertEqual(pictures, [f"frame-{row['frame']:06d}.png" for row in picked[:len(pictures)]], "the pictures are not the recording's first ones")
        self.assertEqual((held["wanted"], held["extracted"]), (len(picked), len(pictures)))

    def test_an_unindexed_recording_of_a_dropped_peer_yields_no_pictures_and_no_failure(self):
        video, rows = self.recording([], 0.7)
        held = e2e_video.extract_frames(FFMPEG, video, rows[::10], self.root / "frames", partial=True)
        self.assertEqual(held["extracted"], 0)
        self.assertTrue(held.get("reason"), "an unreadable recording is accepted without saying why")

    def test_a_live_peer_broken_recording_still_fails(self):
        video, rows = self.recording([], 0.7)
        with self.assertRaises(RuntimeError):
            e2e_video.extract_frames(FFMPEG, video, rows[::10], self.root / "frames")

    def test_a_dropped_peer_sheet_tiles_what_its_recording_holds(self):
        video, rows = self.recording(FRAGMENTED, 0.7)
        video_dir = self.root / "video"
        video_dir.mkdir()
        sheet = e2e_video.contact_sheet(video_dir, rows, self.root / "sheet.png", 10, FFMPEG, video, partial=True)
        self.assertTrue(sheet.get("written"), sheet)
        self.assertLess(sheet["partial"]["extracted"], sheet["partial"]["wanted"])


if __name__ == "__main__":
    unittest.main()
