"""Detect the lead-routed peer contract without starting an engine."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import spread_peers as spread


class NamedRoutingTests(unittest.TestCase):
    def test_every_peer_runs_on_exactly_the_named_box(self):
        peers = [spread.Peer('Host', reviewed=True), spread.Peer('Ana'), spread.Peer('Ben')]
        self.assertEqual(spread.named_peer_boxes(peers, spread.pairs('host=Linux,seat2=EROL-PC,Ben=Mac')),
                         {'Host': 'Linux', 'Ana': 'EROL-PC', 'Ben': 'Mac'})

    def test_missing_name_exits_two_before_any_transport(self):
        with patch.object(spread, 'installed_pool', side_effect=AssertionError('transport must not be consulted')):
            with self.assertRaises(SystemExit) as refused:
                with tempfile.TemporaryDirectory() as temporary:
                    spread.Case(Path(temporary), Path(temporary)/'out', [spread.Peer('host'), spread.Peer('client')],
                                spread.Match(51580), peer_boxes='host=EROL-PC')
        self.assertEqual(refused.exception.code, 2)

    def test_explicit_sharing_with_share_ok_is_accepted(self):
        self.assertEqual(spread.named_peer_boxes([spread.Peer('a'), spread.Peer('b')],
                                                spread.pairs('a=EROL-PC,b=EROL-PC')),
                         {'a': 'EROL-PC', 'b': 'EROL-PC'})

    def test_complete_assignment_keeps_other_arms_names_compatible(self):
        values = spread.pairs('host=ONE,seat2=TWO,seat3=THREE')
        self.assertEqual(spread.named_peer_boxes([spread.Peer('sp')], values), {'sp': 'ONE'})
        self.assertEqual(spread.named_peer_boxes([spread.Peer('host'), spread.Peer('client')], values),
                         {'host': 'ONE', 'client': 'TWO'})

    def test_explicit_sharing_without_share_ok_is_refused(self):
        for first in (spread.Peer('a', share_ok=False), spread.Peer('a', reviewed=True),
                      spread.Peer('a', held=True), spread.Peer('a', quiet=True)):
            with self.subTest(peer=first), tempfile.TemporaryDirectory() as temporary:
                out = Path(temporary)/'out'
                with self.assertRaisesRegex(spread.SpreadRefusal, 'TWO PEERS ON ONE BOX WITHOUT share_ok'), \
                        patch.object(spread, 'installed_pool', side_effect=AssertionError('no transport before refusal')):
                    spread.Case(Path(temporary), out, [first, spread.Peer('b')], spread.Match(51580),
                                peer_boxes='a=EROL-PC,b=EROL-PC')
                receipt = json.loads((out/'spread-result.json').read_text())
                self.assertEqual(receipt['topology'], 'spread')
                self.assertEqual(receipt['peer_boxes'], {'a':'EROL-PC','b':'EROL-PC'})
                self.assertEqual((receipt['refused_peer'], receipt['refused_box'], receipt['reason']),
                                 ('a','EROL-PC','TWO PEERS ON ONE BOX WITHOUT share_ok'))

    def test_retired_spread_exits_two_with_exact_routing_message(self):
        parser = argparse.ArgumentParser()
        spread.add_arguments(parser)
        with self.assertRaises(SystemExit) as refused:
            spread.configure(parser.parse_args(['--spread']))
        self.assertEqual(refused.exception.code, 2)
        self.assertEqual(str(refused.exception), 'NO BOX NAMED: the lead routes every peer (ROUTING.md section 6)')

    def test_every_driver_rejects_retired_spread_without_an_engine(self):
        repo = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temporary:
            common = ['--repo', str(repo), '--out', temporary, '--port', '51580', '--spread']
            cases = {'test_in_match_ux.py': ['--case', 'players'],
                     'test_menu_readback.py': ['--case', 'net-chat', '--size', '1280x720'],
                     'e2e_video.py': ['--scenario', 'mp-host-join'],
                     'feel_measure.py': [], 'spread_peers.py': []}
            for driver, arguments in cases.items():
                with self.subTest(driver=driver):
                    done = subprocess.run([sys.executable, '-B', str(repo/'tools'/driver), *common, *arguments],
                                          capture_output=True, text=True, timeout=20)
                    self.assertEqual(done.returncode, 2, done.stderr)
                    self.assertIn(spread.NO_BOX_NAMED, done.stderr)

    def test_helper_has_no_retired_selection_or_queue_calls(self):
        import ast
        tree = ast.parse(Path(spread.__file__).read_text(encoding='utf-8'))
        forbidden = {'candidates', 'priority_blocker', 'write_ticket', 'bind_ticket', 'queue_root'}
        self.assertEqual([node.func.attr for node in ast.walk(tree) if isinstance(node, ast.Call) and
                          isinstance(node.func, ast.Attribute) and node.func.attr in forbidden], [])


if __name__ == '__main__':
    unittest.main()
