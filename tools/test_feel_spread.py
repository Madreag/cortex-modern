"""Check native feel staging and replay scope without launching engines."""
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import feel_measure as feel


class FeelSpreadTests(unittest.TestCase):
    def test_arm_stages_in_its_live_owned_root_and_default_rejects_an_existing_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            out = root / 'arm'
            out.mkdir()
            case = SimpleNamespace(stage_root=Mock())
            with patch.object(feel, 'file_record', return_value={}), patch.object(feel, 'engine_executable', return_value='unused'), \
                    patch.object(feel, 'write_json', side_effect=RuntimeError('staging-reached')):
                with self.assertRaisesRegex(RuntimeError, 'staging-reached'):
                    feel._launch_case(root, 'arm', 100, 60, True, 51588, root/'input.txt', 'hash', 600, spread_case=case)
                case.stage_root.assert_called_once_with(out)
                with self.assertRaises(FileExistsError):
                    feel._launch_case(root, 'arm', 100, 60, True, 51588, root/'input.txt', 'hash', 600)

    def test_replay_checks_begin_after_the_match_case_has_closed(self):
        events = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            out = root / '100ms-60hz'
            out.mkdir()
            (out / 'match.ccreplay').write_bytes(b'replay')
            (out / 'manifest.json').write_text('{"ticks":1200}')
            options = SimpleNamespace(peer_boxes='host=EROL-PC,seat2=Linux', pool_dispatcher=None,
                                      pool_registry=None, port=51580)
            def run_case(repo, folder, peers, match, *, drive, **kwargs):
                events.extend(['match-open', 'match-closed'])
                return {'driver_result': str(out), 'peer_boxes': {'host':'EROL-PC', 'client':'Linux'}}
            with patch.object(feel, 'SPREAD_OPTIONS', options), patch.object(feel.spread, 'enabled', return_value=True), \
                    patch.object(feel.spread, 'run_case', side_effect=run_case), \
                    patch.object(feel, 'finish_case_replays', side_effect=lambda *args, **kwargs: events.append('replays')):
                feel.launch_case(root, '100ms-60hz', 100, 60, True, 51588, root/'input.txt', 'hash', 600)
            self.assertEqual(events, ['match-open', 'match-closed', 'replays'])

    def test_one_arm_selection_keeps_only_the_two_network_peers(self):
        parser, args = feel.parse_args(['--cases', '100ms-60hz', '--out', 'unused'])
        self.assertEqual(args.cases, ['100ms-60hz'])
        self.assertNotIn(feel.ONE_ARM_CASES[0], feel.TIMING_CASES)

    def test_native_replay_keeps_the_named_host_and_original_evidence_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            replay_out = root/'replay-off'
            expected = root/'replay_trace.json'
            options = SimpleNamespace(peer_boxes='host=EROL-PC,seat2=Linux', pool_dispatcher=None,
                                      pool_registry=None, port=51580)
            def run_case(repo, folder, peers, match, *, drive, **kwargs):
                self.assertEqual(folder, replay_out)
                self.assertEqual(kwargs['peer_boxes'], options.peer_boxes)
                self.assertEqual([peer.name for peer in peers], ['replay'])
                self.assertTrue(peers[0].quiet)
                def make_run(repo, argv, out, **kw):
                    self.assertEqual(out.parent, folder)
                    self.assertEqual(argv[argv.index('-out')+1], str(replay_out/expected.name))
                    out.mkdir(parents=True)
                    record = {'topology':'spread', 'box':'EROL-PC', 'exit_code':0}
                    def finish():
                        kw['expected'][0].write_bytes(b'trace')
                        Path(str(kw['expected'][0])+'.simdump.txt').write_bytes(b'state')
                        (out/'stdout.log').write_bytes(b'native replay')
                        return record
                    handle = SimpleNamespace(start=lambda: handle, finish=finish, close=lambda: None)
                    return handle
                return {'driver_result':drive(SimpleNamespace(make_run=make_run)), 'topology':'spread'}
            with patch.object(feel, 'SPREAD_OPTIONS', options), patch.object(feel.spread, 'enabled', return_value=True), \
                    patch.object(feel.spread, 'run_case', side_effect=run_case):
                record = feel.finish_replay(['-net-replay', str(root/'match.ccreplay'), '-out', str(expected)],
                                           replay_out, timeout=600, expected=[expected])
            self.assertEqual(record['box'], 'EROL-PC')
            self.assertEqual(expected.read_bytes(), b'trace')
            self.assertEqual(Path(str(expected)+'.simdump.txt').read_bytes(), b'state')
            self.assertEqual((replay_out/'stdout.log').read_bytes(), b'native replay')


if __name__ == '__main__':
    unittest.main()
