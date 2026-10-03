from pathlib import Path
import tempfile
import unittest

import test_print_discipline as discipline


class OracleScope(unittest.TestCase):
    def test_roster_exclusion_is_exactly_its_selftest_function(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root / 'Source/Network/NetSeatRoster.cpp'
            path.parent.mkdir(parents=True)
            path.write_text('void Before() { std::cout << "bad before"; }\n'
                'int NetSeatRosterSelfTest::Run() {\n'
                '  auto verdict = [] { std::cout << "PASS }"; }; // }\n'
                '  std::cout << "PASS"; return 0;\n}\n'
                'void After() { std::cerr << "bad after"; }\n')
            rows = discipline.check(root, [])['network_threads_print_whole_lines']
            self.assertEqual([row['line'] for row in rows], [1, 6])

    def test_surface_script_asserts_singleton_policy_and_bound(self):
        path = Path(__file__).parent / 'e2e/ui-surfaces.host.menu.txt'
        script = path.read_text()
        self.assertNotIn('combo_select ComboHostNetSlowPolicy Pause for them', script)
        self.assertIn('assert_combo_items ComboHostNetSlowPolicy Give the seat to the AI (host too) until they catch up', script)
        self.assertIn('assert_label TextHostNetSlowBound 4', script)
        self.assertIn('assert_enabled TextHostNetSlowBound 1', script)


if __name__ == '__main__':
    unittest.main()
