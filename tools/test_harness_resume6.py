"""Relay plan, secret retention and refresh counterexamples; no engines or network."""
from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

from test_harness_resume5 import built
from test_inventory_oracle_evidence import run_stream
import run_tools_suites

REPO = Path(__file__).resolve().parents[1]
LEAD = Path('D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools')


def module(path, name):
    if not path.is_file(): return None
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class RelayRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.plan = built()['plan']

    def row(self, rid):
        return next(row for row in self.plan['rows'] if row['id'] == rid and not row['share'].startswith(('X.mac','X.linux')))

    def policy(self):
        result = module(REPO/'tools/acceptance_relay_policy.py', 'relay_policy_unit')
        self.assertIsNotNone(result, 'relay credential and backend policy is required')
        return result

    def test_c501_cloudflare_driver_and_scenario_run_contract(self):
        row = self.row('gap.mp-relay-cloudflare')
        self.assertIn('{REPO}/tools/relay_cloudflare_match.py', row['argv'])
        self.assertEqual(row['relay_contract']['runs'], ['cloudflare','coturn','fixed','automatic'])
        self.assertEqual(row['relay_contract']['primary'], 'cloudflare')
        self.assertEqual(row['engine_boxes'], {'ALLY':1,'EDITH':1})

    def test_c502_directory_primary_and_coturn_alternative(self):
        scenario = json.loads((REPO/'tools/e2e/mp-direct-vs-relay.json').read_text())
        runs = {run['name']:run for run in scenario['runs']}
        self.assertEqual(runs['relay']['peers'][0]['settings']['NetworkHostRelayMode'], 'Directory')
        self.assertEqual(runs['relay']['directory_turn_config_path'], 'D:/mx/coturn-20260920/turn-config-cloudflare.json')
        self.assertIn('relay-coturn', runs)
        self.assertEqual(runs['relay-coturn']['peers'][0]['settings']['NetworkHostRelayMode'], 'Directory')
        self.assertTrue(any(item.get('directory_relay_offer') == 'cloudflare' and item['run']=='relay' for item in scenario['checklist']))
        for peer in runs['relay']['peers']:
            self.assertEqual(peer['settings']['NetworkConnectionMode'], 'RelayOnly')
            self.assertNotIn('NetworkTurnPass', peer['settings'])
            script = (REPO/'tools/e2e'/peer['menu_script']).read_text()
            self.assertNotIn('{TURN_PASS}', script)
        self.assertIn('ComboHostNetRelay Directory', (REPO/'tools/e2e'/runs['relay']['peers'][0]['menu_script']).read_text())

    def test_c503_refusal_uses_a_throwaway_backend_file(self):
        scenario = json.loads((REPO/'tools/e2e/mp-relay-reasons.json').read_text())
        refused = next(run for run in scenario['runs'] if run['name']=='refused')
        self.assertNotIn('directory_turn_config', refused, 'even invalid credentials belong in a throwaway file')
        self.assertEqual(refused.get('directory_turn_config_fixture'), 'cloudflare-refused')
        policy = self.policy()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); book = policy.CredentialBook()
            config = policy.directory_config(refused, root, book)
            self.assertEqual(config['backend'], 'cloudflare')
            self.assertEqual(len(config['turn_key_id']), 32)
            self.assertTrue((root/'cloudflare-refused.json').is_file())
            self.assertEqual(policy.directory_config({'name':'none'}, root, book), None)

    def test_c504_cloudflare_hold_renew_block_before_mint(self):
        for mode in ('hold','renew'):
            row=self.row('S1.turn-'+mode)
            self.assertEqual([arm['id'] for arm in row.get('relay_arms',[])], ['S1.turn-'+mode+'-cloudflare','S1.turn-'+mode])
            self.assertIn('A63.1',row['blocked_reason'])
            self.assertEqual(row['blocked_capability'],'relay.safe-login')
        policy=self.policy(); mint=Mock()
        with self.assertRaisesRegex(ValueError,'A63.1'):
            policy.mint_login({}, {}, mint)
        mint.assert_not_called()

    def test_c505_compare_runs_cloudflare_first(self):
        row=self.row('S1.relay-compare')
        self.assertEqual([arm['backend'] for arm in row.get('relay_arms',[])], ['cloudflare','coturn'])
        self.assertIn('--backends', row['execution']['args'])
        self.assertEqual(row['relay_arms'][0]['turn'], 'turn:turn.cloudflare.com:3478?transport=udp')

    def test_c506_item15_names_primary_and_alternative(self):
        words=self.plan.get('relay_policy',{}).get('item_15','')
        self.assertIn('Cloudflare through the directory',words)
        self.assertIn('coturn',words)
        self.assertIn("directory's coturn backend (the alternative)",words)
        self.assertIn("player's own fixed pair proven by one row",words)

    def test_c507_cross_arm_directory_first(self):
        self.assertEqual(self.plan.get('relay_policy',{}).get('cross_arms'),
                         [{'path':'directory-relay','backend':'cloudflare','primary':True},
                          {'path':'relay','backend':'coturn','primary':False}])

    def test_c508_suites_register_both_relay_and_directory(self):
        suites=dict(run_tools_suites.SUITES)
        self.assertEqual(suites.get('relay-cloudflare'), ['relay_cloudflare_test.py'])
        self.assertEqual(suites.get('session-directory'), ['session_directory/test_session_directory.py'])

    def test_c509_hotspot_is_conditional_on_explicit_session(self):
        hotspot=self.plan.get('relay_policy',{}).get('hotspot',{})
        self.assertEqual(hotspot.get('scenario'),'mp-relay-hotspot')
        self.assertEqual(hotspot.get('rows'),list('abcdef'))
        self.assertEqual(hotspot.get('state'),'AWAITING')
        self.assertIn('user approves a hotspot session',hotspot.get('reason',''))
        self.assertEqual(hotspot.get('operator'),'lead-tools/hotspot_relay.sh <row> [--network lte|5g]')
        self.assertFalse(any(row.get('scenario')=='mp-relay-hotspot' for row in self.plan['rows']))

    def test_c510_all_retained_files_gate_even_after_scrub(self):
        policy=self.policy()
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); book=policy.CredentialBook()
            book.add('minted-username','test-minted-user-123456')
            replay=root/'replay.bin'; replay.write_bytes(b'header\0{"username":"test-minted-user-123456","credential":"unknown-secret-12345"}')
            result=policy.scan_retained(root,book)
            self.assertFalse(result['passed'])
            self.assertTrue(result['files_with_logins'])
            replay.write_bytes(b'cleaned')
            second=policy.scan_retained(root,book,previous=result)
            self.assertFalse(second['passed'],'scrubbing retained evidence cannot turn the native credential leak green')
            self.assertNotIn('test-minted-user-123456',json.dumps(result))
        self.assertEqual(len(self.plan['rows']),342)


class MergeSourceGate(unittest.TestCase):
    def check(self, source):
        gate=module(LEAD/'inventory/merge_gate.py','merge_gate_unit')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); tree=root/'repo'; tree.mkdir()
            def git(*args):
                return subprocess.check_output(['git','-C',str(tree),*args],stderr=subprocess.DEVNULL,text=True).strip()
            git('init','-b',gate.WAVE);git('config','user.name','Test');git('config','user.email','test@example.invalid')
            (tree/'tools').mkdir();(tree/'tools/base.txt').write_text('base\n')
            git('add','.');git('commit','-m','Base');base=git('rev-parse','HEAD')
            target=tree/('Source/change.cpp' if source else 'tools/change.py');target.parent.mkdir(exist_ok=True);target.write_text('changed\n')
            git('add','.');git('commit','-m','Change');tip=git('rev-parse','HEAD');git('checkout','--detach',base)
            git('branch','-f',gate.WAVE,base);git('checkout',gate.WAVE)
            output=io.StringIO()
            with patch.object(gate,'check_report',return_value=(root/'report','RUNS\nfixture\n')),patch.object(gate,'MX',root/'mx'),redirect_stdout(output):
                code=gate.main([str(tree),tip,'--merge-number','1','--lane','unit','--no-build','--dry-run','--keep-scratch'])
            return code,output.getvalue()

    def test_source_branch_refuses_no_build(self):
        code,text=self.check(True)
        self.assertEqual(code,3,'a branch carrying Source changes must refuse --no-build')
        self.assertIn('Source changed: 1 files; a build is required',text)

    def test_tools_branch_accepts_and_prints_zero_source_files(self):
        code,text=self.check(False)
        self.assertEqual(code,0)
        self.assertIn('Source changed: 0 files',text)
        self.assertIn('diff --stat',text)


if __name__=='__main__': unittest.main()
