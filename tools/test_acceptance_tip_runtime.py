import unittest

import acceptance_tip_runtime as runtime


class DestinationBoundary(unittest.TestCase):
    def test_only_separate_row_tree_on_z13(self):
        self.assertEqual(runtime.destination('Z13', 'lane', 'EROL-TABLET').as_posix(), 'D:/Projects/z13-rows-build')

    def test_edith_stays_in_its_named_lane(self):
        self.assertEqual(runtime.destination('EDITH', 'lane', 'EDITH').as_posix(), 'D:/mx/lane/engine-tip')
        with self.assertRaises(ValueError): runtime.destination('EDITH', '../other', 'EDITH')

    def test_pc_and_wrong_physical_machine_refused_before_io(self):
        for box, hostname in [('Z13','EROL-PC'), ('EDITH','EROL-PC'), ('Z13','EDITH'), ('ALLY','EROL-ALLY7')]:
            with self.subTest(box=box, hostname=hostname), self.assertRaises(ValueError):
                runtime.destination(box, 'lane', hostname)


if __name__ == '__main__': unittest.main()
