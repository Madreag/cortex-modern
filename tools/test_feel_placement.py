"""A feel arm's three engines are placed inside the CPU mask the runner's job gives them: a job with an affinity limit
keeps its processes inside it, so a placement outside the mask fails at launch. No engine starts."""

import os
import sys
import unittest
from unittest.mock import patch

import feel_measure

PEERS = ['host', 'client', 'survivor']


def placements(mask_text, cpus=32):
    # A synthetic machine of `cpus` processors: the runner clips a mask to the system's, and boxes differ.
    import win32_test_runner
    system = (1 << cpus) - 1
    with patch.dict(os.environ, {'CC_RUNNER_AFFINITY_MASK': mask_text}), patch.object(feel_measure.os, 'cpu_count', return_value=cpus), \
            patch.object(win32_test_runner, 'affinity_of', return_value=(system, system)):
        return feel_measure.engine_placements(PEERS)


@unittest.skipUnless(sys.platform == 'win32', 'the runner places engines on Windows only')
class Placement(unittest.TestCase):
    def test_every_engine_sits_inside_the_job_mask(self):
        placed = placements('0x0000FFFF')
        self.assertEqual(sorted(placed), sorted(PEERS))
        for peer, placement in placed.items():
            self.assertTrue(placement['mask'], peer)
            self.assertEqual(placement['mask'] & ~0xFFFF, 0, f'{peer} {placement["mask"]:#x} leaves the job mask 0xffff')
        masks = [placed[peer]['mask'] for peer in PEERS]
        self.assertEqual(masks[0] & masks[1] | masks[0] & masks[2] | masks[1] & masks[2], 0, 'two engines share a processor')
        self.assertEqual(masks[0] | masks[1] | masks[2], 0xFFFF)
        self.assertEqual((placed['host']['mask'], placed['client']['mask'], placed['survivor']['mask']), (0x3F, 0xFC0, 0xF000))
        self.assertEqual(placed['host']['logical'], '0-5')
        self.assertEqual(placed['host']['priority'], 'above_normal')

    def test_a_box_with_no_mask_keeps_the_whole_machine_split(self):
        placed = placements('none')
        self.assertEqual((placed['host']['mask'], placed['client']['mask'], placed['survivor']['mask']), (0xFFF, 0x3FF000, 0xFFC00000))
        self.assertEqual(placed['survivor']['logical'], '22-31')

    def test_whole_pairs_split_a_sparse_mask(self):
        placed = placements('0x00FF00F0')
        self.assertEqual((placed['host']['mask'], placed['client']['mask'], placed['survivor']['mask']), (0xF0, 0xF0000, 0xF00000))
        self.assertEqual(placed['host']['logical'], '4-7')
        self.assertEqual(placed['survivor']['logical'], '20-23')
        placed = placements('0x000003F8')
        self.assertEqual((placed['host']['mask'], placed['client']['mask'], placed['survivor']['mask']), (0x38, 0xC0, 0x300))
        self.assertEqual(placed['host']['logical'], '3-5')

    def test_a_mask_too_small_for_three_places_nothing(self):
        self.assertEqual(placements('0x0000000F'), {})

    def test_a_placement_narrows_the_engines_own_job(self):
        import ctypes
        import subprocess
        import win32_test_runner as runner
        job = runner.check(runner.create_job(None, None))
        try:
            limits = runner.EXT()
            limits.BasicLimitInformation.LimitFlags = 0x2000 | 0x10
            limits.BasicLimitInformation.Affinity = 0xF
            runner.check(runner.set_job(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)))
            child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                handle = int(child._handle)
                runner.check(runner.assign_job(job, handle))
                self.assertEqual(runner.affinity_of(handle)[0], 0xF)
                # The process alone cannot leave its job's mask: the runner's affinity limit holds it there.
                ctypes.WinDLL('kernel32').SetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.c_size_t(0x3))
                self.assertEqual(runner.affinity_of(handle)[0], 0xF)
                feel_measure.narrow_job_affinity(job, 0x3)
                self.assertEqual(runner.affinity_of(handle)[0], 0x3)
            finally:
                child.kill()
                child.wait()
        finally:
            runner.close_handle(job)

    def test_two_engines_are_not_placed(self):
        with patch.dict(os.environ, {'CC_RUNNER_AFFINITY_MASK': '0x0000FFFF'}):
            self.assertEqual(feel_measure.engine_placements(['host', 'client']), {})


if __name__ == '__main__':
    unittest.main()
