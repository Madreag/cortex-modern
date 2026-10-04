import json
import os
from pathlib import Path
import tempfile
import unittest

from acceptance_cross_report import build_report, native_prerequisites, refusal_log_excerpt, native_route_evidence


class CrossReceiptReport(unittest.TestCase):
    def run_root(self):
        retained = os.environ.get("CC_ACCEPTANCE_TEST_ROOT")
        if retained:
            return Path(tempfile.mkdtemp(prefix="cross-report-test-", dir=retained))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    def test_clocked_transfer_bytes_must_match_the_native_receiver(self):
        from test_acceptance_transfer import native_probe, native_log
        line = native_log(native_probe())
        error = 'transfer: native StateChunk receipt missing, repeated, or differs from clocked byte count'
        for receipt, log, rejected in ((123, line, False), (124, line, True), (None, line, True),
                                       (123, '', True), (123, line+line, True)):
            with self.subTest(receipt=receipt, log=log):
                root = self.run_root()
                names = ('erol', 'edith', 'mac', 'linux')
                manifest = dict(acceptance_row='world-join', ticks=3601, preflights={}, driver_findings=[],
                                boxes=[dict(name=n, kind='windows-local') for n in names],
                                specs=[dict(peer=n, box=n, own=str(root/n/'incarnation-0'), root=str(root)) for n in names])
                (root/'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
                own = root/'edith/incarnation-0'
                (own/'engine').mkdir(parents=True)
                (own/'engine/stdout.log').write_text(log, encoding='utf-8')
                (own/'record.json').write_text(json.dumps(dict(pid=42)), encoding='utf-8')
                if receipt is not None:
                    (own/'transfer-probe').mkdir()
                    (own/'transfer-probe/net-ui-result.json').write_text(json.dumps(native_probe(receipt)), encoding='utf-8')
                # A caller-supplied summary must never substitute for the native probe.
                (own/'acceptance-transfer.json').write_text(json.dumps(dict(received_bytes=123)), encoding='utf-8')
                result = build_report(root)
                self.assertEqual(error in result['failures'], rejected)

    def test_route_uses_native_candidate_and_route_receipts(self):
        host = '[net-ice] selected candidate=srflx connection=100\n[net-route] RouteAllowed route=direct allowed=1 connection=100\n'
        client = '[net-ice] selected candidate=srflx connection=200\n[net-route] RouteAllowed route=direct allowed=1 connection=200\n'
        result = native_route_evidence(host, client)
        self.assertEqual(result['route'], 'direct')
        self.assertTrue(result['nat_to_nat'])
        self.assertTrue(result['stun'])

    def test_soak_transfer_uses_one_native_receipt_without_r6_clock_labels(self):
        line = '[net-match] state transfer complete: 123 bytes\n'
        for log, expected in ((line, 123), ('', None), (line+line, None)):
            with self.subTest(log=log):
                root = self.run_root()
                names = ('erol', 'edith-first', 'edith')
                manifest = dict(acceptance_row='world-soak', ticks=219601, preflights={},
                                driver_findings=[], soak={},
                                boxes=[dict(name=n, kind='windows-local') for n in names],
                                specs=[dict(peer=n, box=n, own=str(root/n/'incarnation-0'), root=str(root)) for n in names])
                (root/'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
                own = root/'edith/incarnation-0'
                (own/'engine').mkdir(parents=True)
                (own/'engine/stdout.log').write_text(log, encoding='utf-8')
                result = build_report(root)
                facts = json.loads((root/'acceptance-facts.json').read_text(encoding='utf-8'))
                self.assertEqual(facts['transfer'].get('received_bytes'), expected)
                transfer_failures = [value for value in result['failures'] if value.startswith('transfer:')]
                self.assertEqual(bool(transfer_failures), expected is None)

    def test_reactivation_cannot_skip_history_after_the_first_late_activation(self):
        for late_ticks, expected in (((1500, 2000), 1500), ((1500, 1750, 2000), None)):
            with self.subTest(late_ticks=late_ticks):
                root = self.run_root()
                names = ('erol', 'edith', 'mac', 'linux')
                manifest = dict(acceptance_row='world-join', ticks=3601, preflights={}, driver_findings=[],
                                boxes=[dict(name=n, kind='windows-local') for n in names],
                                specs=[dict(peer=n, box=n, own=str(root/n/'incarnation-0'), root=str(root)) for n in names])
                (root/'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
                for name, label, ticks in (('erol', 'activate', (1500, 2000)), ('edith', 'catch-up complete', late_ticks)):
                    engine = root/name/'incarnation-0/engine'
                    engine.mkdir(parents=True)
                    text = ''.join(f'[net-world] {label} peer=3 at={tick}\n' for tick in ticks)
                    (engine/'stdout.log').write_text(text, encoding='utf-8')
                build_report(root)
                facts = json.loads((root/'acceptance-facts.json').read_text(encoding='utf-8'))
                self.assertEqual(facts['join']['activation_tick'], expected)

    def test_unrelated_or_refused_routes_do_not_prove_nat_join(self):
        host = '[net-ice] selected candidate=srflx connection=100\n[net-route] RouteAllowed route=direct allowed=1 connection=100\n'
        candidate = '[net-ice] selected candidate=srflx connection=200\n'
        for route in ('[net-route] RouteAllowed route=direct allowed=1 connection=201\n',
                      '[net-route] RouteAllowed route=direct allowed=0 connection=200\n',
                      '[net-route] RouteAllowed route=relay allowed=1 connection=200\n',
                      '[net-ice] route=direct connection=200\n'):
            with self.subTest(route=route):
                result = native_route_evidence(host, candidate+route)
                self.assertIsNone(result['route'])
                self.assertFalse(result['nat_to_nat'])

    def test_host_route_cannot_be_substituted_by_an_unpaired_candidate(self):
        client = '[net-ice] selected candidate=srflx connection=200\n[net-route] RouteAllowed route=direct allowed=1 connection=200\n'
        result = native_route_evidence('[net-ice] selected candidate=srflx connection=100\n', client)
        self.assertFalse(result['nat_to_nat'])

    def test_refusal_report_does_not_copy_connection_details(self):
        text = '[net-session] admission refused reason=ModuleManifestMismatch endpoint=fixture-private-endpoint\n'
        self.assertEqual(refusal_log_excerpt(text), 'ModuleManifestMismatch')
        self.assertEqual(refusal_log_excerpt('[net-session] admission accepted'), '')

    def native(self):
        native = dict(exit_code=0, setup_error="", runtime_error="",
                      desync_check=dict(mismatches=0, compares=40, compare_margin=1),
                      service=dict(runner=dict(match_config=dict(peer_count=4))))
        return native, dict(exit_code=0, timed_out=False, exe_sha256="e"*64), dict(executable_sha256="e"*64)

    def test_matching_live_history_cannot_erase_native_desync(self):
        native, record, preflight = self.native()
        self.assertEqual(native_prerequisites(native, record, "", preflight, 4), [])
        native["desync_check"]["mismatches"] = 1
        self.assertIn("native desync check missing or failed", native_prerequisites(native, record, "", preflight, 4))

    def test_wrong_loaded_binary_cannot_use_another_preflight(self):
        native, record, preflight = self.native()
        record["exe_sha256"] = "f"*64
        self.assertIn("running binary differs from or lacks its preflight identity",
                      native_prerequisites(native, record, "", preflight, 4))

    def test_engine_errors_survive_a_zero_exit_and_equal_hashes(self):
        native, record, preflight = self.native()
        result = native_prerequisites(native, record, "ready\n[net-ui-probe] FAIL input not applied\n", preflight, 4)
        self.assertIn("engine findings in stdout.log lines 2", result)

    def test_native_peer_count_is_not_inferred_from_the_manifest(self):
        native, record, preflight = self.native()
        native["service"]["runner"]["match_config"]["peer_count"] = 3
        self.assertIn("native adopted peer count differs from the row", native_prerequisites(native, record, "", preflight, 4))

    def test_empty_run_cannot_be_green_from_its_plan(self):
        root = self.run_root()
        names = ("erol", "edith", "mac", "linux")
        manifest = dict(acceptance_row="mod-match", ticks=1201, preflights={}, driver_findings=[],
                        boxes=[dict(name=n, kind="windows-local" if n == "erol" else "posix-ssh") for n in names],
                        specs=[dict(peer=n, box=n, own=str(root/n/"incarnation-0"), root=str(root)) for n in names])
        (root/"manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        result = build_report(root)
        self.assertFalse(result["v1_passed"])
        self.assertIn("preflight: distinct real machine receipts missing", result["failures"])
        self.assertIn("pc: native match outcome missing or failed", result["failures"])
        self.assertTrue((root/"acceptance-facts.json").is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
