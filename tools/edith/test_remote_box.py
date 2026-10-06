"""Detect payload installation that overwrites a shared session wrapper."""
import unittest
from pathlib import Path

from remote_box import RemoteBox


class FakeBox(RemoteBox):
    def __init__(self, wrapper, state='Ready'):
        super().__init__('test')
        self.wrapper, self.state, self.calls = wrapper, state, []

    def task_state(self):
        return self.state

    def read_text(self, path, timeout=120):
        self.calls.append(('read', str(path)))
        return self.wrapper

    def scp_to(self, source, target, timeout=300):
        self.calls.append(('copy', str(target)))

    def ssh(self, command, timeout=120, check=True):
        self.calls.append(('ssh', command))
        return ''


class PayloadTests(unittest.TestCase):
    def test_existing_wrapper_stays_in_place(self):
        box = FakeBox('& pwsh -NoProfile -File "$root/command.ps1"\nexit $LASTEXITCODE\n')
        box.start_task(Path('payload.ps1'), budget_s=0)
        self.assertIn(('copy', 'D:/mx/session1/command.ps1'), box.calls)
        self.assertNotIn(('copy', box.session_script), box.calls)
        self.assertEqual(box.calls[-1], ('ssh', 'Start-ScheduledTask -TaskName cortex-session1'))

    def test_legacy_payload_path_remains_supported(self):
        box = FakeBox('$env:CCCP_HEADLESS=\'1\'\n& python run.py\n')
        box.start_task(Path('payload.ps1'), budget_s=0)
        self.assertIn(('copy', box.session_script), box.calls)

    def test_non_ready_task_is_not_modified(self):
        box = FakeBox(None, state='Disabled')
        with self.assertRaises(RuntimeError):
            box.start_task(Path('payload.ps1'), budget_s=0)
        self.assertEqual(box.calls, [])


if __name__ == '__main__':
    unittest.main()
