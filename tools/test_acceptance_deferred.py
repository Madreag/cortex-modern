"""A waiting late seat must not block monitoring of a live seat on the same box."""
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import cross_peers


class DeferredLaunch(unittest.TestCase):
    def test_initial_peer_is_monitored_before_late_peer_starts(self):
        retained = os.environ.get('CC_ACCEPTANCE_TEST_ROOT')
        if retained:
            root = Path(tempfile.mkdtemp(prefix='deferred-test-', dir=retained))
        else:
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            root = Path(temporary.name)
        (root/'session.json').write_text(json.dumps(dict(session='test-session')))
        (root/'preflight.json').write_text('{}')
        starts, first_polls = [], [0]

        class FakeRun:
            def __init__(self, spec):
                self.name = spec['peer']
                self.record = dict(pid=100+len(starts), exit_code=0)
                self.cwd = Path(spec['own'])/'engine/runtime'

            def start(self):
                starts.append((self.name, first_polls[0]))

            def poll(self):
                if self.name == 'edith-first':
                    first_polls[0] += 1
                    if first_polls[0] == 3:
                        (root/'session-late.json').write_text(json.dumps(dict(session='test-session')))
                    return 0 if first_polls[0] >= 4 else None
                return 0

            def finish(self): return self.record
            def close(self): pass

        def prepare(spec, pin, box):
            Path(spec['own']).mkdir(parents=True)
            return FakeRun(spec)

        specs = [dict(peer=name, role='player', box='EDITH', own=str(root/name/'incarnation-0'), root=str(root),
                      flags=['-net-match-peers','3','-net-join-session','<published-session-id>'],
                      incarnation=0, faults=[], barriers=[], recoveries=[], timeout=10,
                      preserve_evidence=True, acceptance_row='world-soak',
                      defer_until_session=name=='edith', session_leaf='session-late.json' if name=='edith' else 'session.json')
                 for name in ('edith-first', 'edith')]
        payload = dict(box=dict(name='EDITH', kind='windows-task'), specs=specs, pin='')
        path = root/'payload.json'
        path.write_text(json.dumps(payload))
        no_op = lambda *args, **kwargs: None
        observer = lambda *args, **kwargs: SimpleNamespace(observe=no_op)
        poller = lambda *args, **kwargs: SimpleNamespace(poll=lambda *a: [])
        with mock.patch.multiple(cross_peers, assert_box_guard=no_op, read_capabilities=lambda *a:dict(peer_limit=4),
                                 wait_for_payload_release=no_op, prepare_instance=prepare, refuse_mixed_build=no_op,
                                 sample_memory=lambda *a:dict(private=1), box_load=lambda *a:[],
                                 retain_checkpoints=no_op, seal_evidence=no_op,
                                 Tail=lambda *a:SimpleNamespace(read=lambda:[], drained=True, compress_consumed=no_op)), \
             mock.patch('feel.records.CaptureSealer', poller), mock.patch('feel.records.NativeFaultEffects', poller), \
             mock.patch('feel.records.LobbyWatch', poller), mock.patch('feel.records.RecoveryLedger', observer):
            code = cross_peers.run_payload(path)
        self.assertEqual(code, 0, (root/'payload-error.json').read_text() if (root/'payload-error.json').exists() else '')
        self.assertEqual([name for name, _ in starts], ['edith-first','edith'])
        self.assertGreaterEqual(starts[1][1], 3, 'late peer started before the initial peer was monitored')


if __name__ == '__main__':
    unittest.main(verbosity=2)
