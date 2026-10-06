"""Detect concurrent named cases and genuine native run conflicts without engines."""
import contextlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import cross_peers as cross


class PeerRunGuardTests(unittest.TestCase):
    @contextlib.contextmanager
    def fake_box(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / 'Cortex Command.exe'
            executable.write_bytes(b'one pinned executable')
            box = dict(name='EROL-PC', kind='windows-local', executable=str(executable),
                       scratch=str(root), pool_root=str(root/'native'), engines_max=10, free_floor_gb=12)
            current = {}
            def read(path, **kwargs):
                path = Path(path)
                return json.loads(path.read_text()) if path.is_file() else None
            def write(path, label, *, token=None, extra=None, **kwargs):
                path = Path(path)
                if path.exists():
                    raise FileExistsError(str(path))
                path.parent.mkdir(parents=True, exist_ok=True)
                value = dict(extra or {}, token=token or 'owned-token', label=label)
                path.write_text(json.dumps(value))
                return value
            def release(path, token):
                if (value := read(path)) and value['token'] == token:
                    Path(path).unlink()
                    return True
                return False
            facts = SimpleNamespace(read_reservation=read, write_reservation=write, release_reservation=release)
            worker = SimpleNamespace(root_for=lambda box:Path(box['pool_root']),
                                     mutex=lambda *args, **kwargs:contextlib.nullcontext(), facts=facts)
            native = SimpleNamespace(assignment_for_launch=lambda:current['assignment'], box_facts=facts)
            load = SimpleNamespace(memory=lambda:(20.0, 48.0))
            def peer(case_id, port, run_root=None):
                claim = root/'native/claims'/('run-'+case_id+'.json')
                claim.parent.mkdir(parents=True, exist_ok=True)
                claim.write_text(json.dumps(dict(case_id=case_id, peer_id='host', engines=1, token=case_id)))
                current['assignment'] = dict(claim=str(claim), token=case_id, box=box, executable=str(executable))
                return dict(case_id=case_id, peer_id='host', lane='one-lane',
                            run_root=str(run_root or root/case_id/'host'), controller_root=str(root/case_id),
                            ports=[port], executable_sha256=cross.digest_file(executable))
            with patch.dict('sys.modules', pool_run=native, pool_worker=worker, box_load=load), \
                    patch.object(cross, 'inventory_guard', return_value=None), \
                    patch.object(cross, 'box_load', return_value=[dict(Name='Cortex Command.exe', ProcessId=42,
                                                                    ExecutablePath=str(executable))]), \
                    patch.object(cross, 'scratch_bytes', return_value=0):
                yield box, peer, root

    def test_two_cases_of_one_lane_with_distinct_roots_and_ports_launch(self):
        with self.fake_box() as (box, peer, root):
            first = peer('case-one', 51580)
            with cross.peer_run_scope(box, first):
                cross.assert_box_guard(box, pool_peer=first)
                second = peer('case-two', 51590)
                with cross.peer_run_scope(box, second):
                    cross.assert_box_guard(box, pool_peer=second)
                    self.assertTrue((Path(first['run_root'])/'.spread-run-owner.json').is_file())
                    self.assertTrue((Path(second['run_root'])/'.spread-run-owner.json').is_file())
            self.assertEqual(list((root/'native/peer-runs').glob('*.json')), [])

    def test_same_run_root_or_case_port_refuses_with_box_and_peer_named(self):
        for conflict in ('root', 'port'):
            with self.subTest(conflict=conflict), self.fake_box() as (box, peer, root):
                first = peer('case-one', 51580)
                with cross.peer_run_scope(box, first):
                    second = peer('case-two', 51580 if conflict == 'port' else 51590,
                                  Path(first['run_root']) if conflict == 'root' else None)
                    reason = 'RUN ROOT CONFLICT' if conflict == 'root' else 'PORT CONFLICT 51580'
                    with self.assertRaisesRegex(RuntimeError, 'EROL-PC.*'+reason+'.*host.*case-one'):
                        with cross.peer_run_scope(box, second):
                            self.fail('a conflicting run must never launch')
                    owner = json.loads((Path(first['run_root'])/'.spread-run-owner.json').read_text())
                    self.assertEqual(owner['case_id'], 'case-one')

    def test_capacity_floor_and_hash_still_refuse_before_a_launch(self):
        with self.fake_box() as (box, peer, root):
            value = peer('case', 51580)
            with patch.object(cross, 'box_load', return_value=[dict(Name='Cortex Command.exe')]*10):
                with self.assertRaisesRegex(RuntimeError, 'EROL-PC.*engine ceiling 10'):
                    cross.assert_box_guard(box, pool_peer=value)
            with patch.dict('sys.modules', box_load=SimpleNamespace(memory=lambda:(11.9,48))):
                with self.assertRaisesRegex(RuntimeError, 'EROL-PC.*below floor 12'):
                    cross.assert_box_guard(box, pool_peer=value)
            value['executable_sha256'] = '0'*64
            with self.assertRaisesRegex(RuntimeError, 'EROL-PC.*hash differs'):
                with cross.peer_run_scope(box, value):
                    self.fail('changed executable must never launch')
            self.assertFalse((root/'native/peer-runs').exists())

    def test_cleanup_does_not_remove_a_replaced_root_owner(self):
        with self.fake_box() as (box, peer, root):
            value = peer('case', 51580)
            marker = Path(value['run_root'])/'.spread-run-owner.json'
            with cross.peer_run_scope(box, value):
                marker.write_text(json.dumps(dict(token='foreign', case_id='other', peer_id='seat')))
            self.assertEqual(json.loads(marker.read_text())['token'], 'foreign')


if __name__ == '__main__':
    unittest.main()
