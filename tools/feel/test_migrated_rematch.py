import unittest
from feel.migrated_rematch import successor_host


class SuccessorTests(unittest.TestCase):
    def test_both_survivors_name_the_single_host(self):
        reports = {peer: {'service': {'is_host': peer == 'first'}} for peer in ('first', 'second')}
        logs = {peer: 'Host left - first is now hosting; boundary=305' for peer in reports}
        self.assertTrue(successor_host(reports, logs))

    def test_two_hosts_are_refused(self):
        reports = {peer: {'service': {'is_host': True}} for peer in ('first', 'second')}
        logs = {peer: 'Host left - first is now hosting; boundary=305' for peer in reports}
        self.assertFalse(successor_host(reports, logs))

    def test_disagreeing_announcements_are_refused(self):
        reports = {peer: {'service': {'is_host': peer == 'first'}} for peer in ('first', 'second')}
        logs = {peer: f'Host left - {peer} is now hosting; boundary=305' for peer in reports}
        self.assertFalse(successor_host(reports, logs))
