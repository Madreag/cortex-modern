import unittest
from feel import report
import cross_report


def census(points):
    return '\n'.join(f'[mem-census] tick={minute * 3600} private_mb={process} uptime_ms={minute * 60000} '
                     f'cow: entries=3 entry_mb={instrument} pixels=2 retired=0 retired_mb=0 last_image_mb=0'
                     for minute, process, instrument in points)


class MemoryBounds(unittest.TestCase):
    def test_p06_nine_mebibytes_each_minute_is_a_leak(self):
        result = report.reduce_memory_census(census([(minute, 3000 + 9 * minute, 0) for minute in range(121)]))
        self.assertNotEqual(result['status'], 'PASS', result)

    def test_growth_bound_is_independent_of_slope(self):
        result = report.reduce_memory_census(census([(minute, 3000 + minute, 0) for minute in range(140)]))
        self.assertEqual(result['status'], 'FAIL', result)
        self.assertTrue(result['census_slope_pass'])
        self.assertGreater(result['declared_bounds']['sizes']['private']['retained_bytes'], 128 * 1024 * 1024)

    def test_measured_cache_subtraction_and_complete_sampling(self):
        points = [(minute, 3000 + 100 * minute, 100 * minute) for minute in range(5)]
        result = report.reduce_memory_census(census(points))
        self.assertEqual(result['status'], 'PASS', result)
        self.assertEqual(result['declared_bounds']['sizes']['private']['retained_bytes'], 0)
        self.assertEqual(report.reduce_memory_census(census(points[:2]))['status'], 'NOT COVERED')
        self.assertEqual(report.reduce_memory_census(census(points[:2] + points[3:]))['status'], 'NOT COVERED')

    def test_census_pass_cannot_override_declared_bounds_or_a_missing_incarnation(self):
        bounds = dict(passed=True, sizes={'private': {}}, missing_samples=0)
        peer = dict(incarnation=0, memory_by_incarnation={'0': bounds}, memory_census={'0': {'status': 'PASS'}})
        self.assertEqual(cross_report.memory_verdict({'a': peer})['status'], 'PASS')
        peer['memory_by_incarnation']['0'] = dict(bounds, passed=False)
        self.assertEqual(cross_report.memory_verdict({'a': peer})['status'], 'FAIL')
        peer['memory_by_incarnation']['0'] = bounds
        peer['memory_by_incarnation']['1'] = bounds
        self.assertEqual(cross_report.memory_verdict({'a': peer})['status'], 'NOT COVERED')


if __name__ == '__main__':
    unittest.main()
