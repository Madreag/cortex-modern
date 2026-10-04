"""Credential gates at cleanup, native packaging and each declared relay run."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock,patch
import uuid
import os

import acceptance_remote as remote
import acceptance_relay_policy as policy
import turn_relay_rows as turn
import acceptance_peer_session as sessions
from feel import relay_join
from test_acceptance_resume3 import build_plan


class RelayPrivacyEdges(unittest.TestCase):
    def test_unknown_reparse_point_cannot_be_excluded_from_a_relay_root(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/'row';root.mkdir();link=root/'unclassified-link';link.write_text('pointer')
            (root/'native.log').write_text('clean\n');book=policy.CredentialBook();book.add('minted-username','fixture-'+uuid.uuid4().hex)
            original=Path.is_symlink
            with patch.object(Path,'is_symlink',lambda path:True if path==link else original(path)):
                scan=policy.scan_retained(root,book)
            self.assertFalse(scan['passed'],'an unexplained reparse point was silently outside the relay check')
            self.assertTrue(scan['unscanned'])

    def test_only_the_manifest_bound_external_data_link_is_excluded(self):
        with tempfile.TemporaryDirectory() as folder:
            base=Path(folder);root=base/'row';runtime=root/'host/runtime';data=runtime/'Data';data.mkdir(parents=True)
            shared=base/'assets';shared.mkdir()
            (root/'host/runtime.json').write_text(json.dumps(dict(cwd=str(runtime),data=str(shared))))
            book=policy.CredentialBook();book.add('minted-username','fixture-'+uuid.uuid4().hex)
            original_link,original_resolve=Path.is_symlink,Path.resolve
            with patch.object(Path,'is_symlink',lambda path:True if path==data else original_link(path)),patch.object(Path,'resolve',lambda path,*a,**k:shared if path==data else original_resolve(path,*a,**k)):
                self.assertTrue(policy.scan_retained(root,book)['passed'])
                (root/'host/runtime.json').write_text(json.dumps(dict(cwd=str(runtime),data=str(base/'different-assets'))))
                self.assertFalse(policy.scan_retained(root,book)['passed'],'a different target borrowed the shared-data exclusion')

    def test_remote_timeout_stops_then_sweeps_before_returning_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);pair=sessions.Pair.__new__(sessions.Pair)
            box=SimpleNamespace(name='ALLY',repo='D:/repo',ssh='unused')
            transport=Mock();transport.wait_done.side_effect=['TIMEOUT','done rc=137']
            pair.boxes={'host':(box,transport,SimpleNamespace(ps_quote=repr),root)}
            book=policy.CredentialBook();book.add('minted-username','fixture-'+uuid.uuid4().hex)
            with patch.object(remote,'sanitize_remote',return_value=dict(passed=True,safe_to_copy=True)) as scan,patch.object(remote,'fetch_evidence') as fetch:
                with self.assertRaises(TimeoutError):pair.finish('host',root/'task',root/'done',1,root/'local',book=book)
            self.assertTrue(scan.called,'native task timeout bypassed the relay cleanup gate')
            fetch.assert_not_called()
            self.assertEqual(transport.wait_done.call_count,2,'cleanup did not wait for the stopped task')

    def test_login_is_inherited_when_the_runner_snapshots_its_environment(self):
        login=('fixture-'+uuid.uuid4().hex,uuid.uuid4().hex);observed=[]
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/'row'
            def factory(repo,args,out,timeout,env):
                observed.append((os.environ.get('CC_TEST_TURN_USER'),os.environ.get('CC_TEST_TURN_PASS')))
                out.mkdir(parents=True);(out/'stdout.log').write_text('[net-p2p-selftest] PASS\n')
                return SimpleNamespace(start=lambda:None,finish=lambda:dict(exit_code=0),close=lambda:None)
            with patch.object(sys,'argv',['turn','hold','--turn','localhost:3478','--out',str(root)]),patch.object(turn,'read_login',return_value=login),patch.object(turn,'make_run',side_effect=factory),patch.object(turn.time,'sleep'),contextlib.redirect_stdout(io.StringIO()):
                turn.main()
            self.assertTrue(observed==[login],'runner environment snapshot did not inherit both private components')

    def test_placeholders_do_not_count_as_observed_logins(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'native.log').write_text('clean\n')
            book=policy.CredentialBook()
            for value in ('{TURN_USER}','{TURN_PASS}','__ACCEPTANCE_TURN_USER__','redacted'):book.add('placeholder',value)
            self.assertFalse(policy.scan_retained(root,book)['passed'],'secret-scan.json/placeholders supplied a false nonempty book')

    def test_preflight_retires_the_old_fixed_login_reader(self):
        import preflight
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'retired.conf';path.write_text('user=fixture-'+uuid.uuid4().hex+':'+uuid.uuid4().hex+'\n')
            with self.assertRaises((ValueError,RuntimeError)):preflight.turn_login(path)

    def test_preflight_mints_from_the_directory_coturn_backend(self):
        import preflight
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'backend.json'
            path.write_text(json.dumps(dict(backend='coturn',static_auth_secret=uuid.uuid4().hex,relay_urls=['turn:relay.example:3478?transport=udp'])))
            try:login=preflight.turn_login(path)
            except (ValueError,RuntimeError):self.fail('preflight still expects a retired fixed-account line')
            self.assertTrue(login[0].split(':')[0].isdigit() and bool(login[1]),'preflight did not mint a time-limited directory pair')

    def test_native_sanitizer_preserves_failure_and_blocks_an_empty_book(self):
        login='fixture-'+uuid.uuid4().hex
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);native=root/'stdout.log';native.write_text('raw user '+login+'\n')
            book=policy.CredentialBook();book.add('minted-username',login)
            result=remote.sanitize_native(root,policy.native_book(book).digests())
            self.assertFalse(result['passed']);self.assertTrue(result['safe_to_copy'])
            self.assertFalse(login in native.read_text(),'stdout.log/native digest sweep missed the login')
            with self.assertRaisesRegex(ValueError,'nonempty'):remote.sanitize_native(root,dict(items=[]))

    def test_remote_empty_book_refuses_before_any_pack_or_copy(self):
        with tempfile.TemporaryDirectory() as folder:
            box=SimpleNamespace(repo='D:/repo',ssh='unused');transport=Mock();quote=SimpleNamespace(ps_quote=lambda value:repr(value))
            with self.assertRaisesRegex(ValueError,'nonempty'):
                remote.fetch_evidence(box,transport,quote,Path(folder)/'native',Path(folder)/'copy',book=policy.CredentialBook())
            transport.ssh.assert_not_called();transport.scp_from.assert_not_called()

    def test_t3_cleanup_failure_still_sweeps_native_output(self):
        login='fixture-'+uuid.uuid4().hex
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/'row'
            def make_run(repo,args,out,timeout,env):
                cwd=out/'runtime';(cwd/'Userdata').mkdir(parents=True)
                def start():
                    (out/'stdout.log').write_text('raw user '+login+'\n')
                    raise RuntimeError('native start stopped')
                def close():raise RuntimeError('native close stopped')
                return SimpleNamespace(cwd=cwd,out=out,start=start,close=close)
            @contextlib.contextmanager
            def directory(*args,**kwargs):
                kwargs['secret_book'].add('minted-username',login)
                yield dict(DIRECTORY_URL='127.0.0.1:49492',DIRECTORY_PIN='pin')
            with patch.dict(os.environ,CC_TEST_TURN_USER=login,CC_TEST_TURN_PASS=uuid.uuid4().hex),patch.object(sys,'argv',['row','--turn','turn:localhost:3478','--out',str(root)]),patch.object(relay_join,'make_run',side_effect=make_run),patch.object(relay_join,'patch_settings'),patch.object(relay_join,'stage_baseline'),patch.object(relay_join,'start_service',return_value=Mock()),patch.object(relay_join,'require_turn_reachable',return_value=0),patch.object(relay_join.threading,'Thread',return_value=Mock()),patch('e2e.directory.serve',side_effect=directory),patch.object(policy,'directory_config',return_value={'backend':'coturn','static_auth_secret':'fixture-only'}):
                with self.assertRaises(RuntimeError):relay_join.main()
            self.assertFalse(login in (root/'host/stdout.log').read_text(),'stdout.log/close exception bypassed the mandatory finally sweep')

    def test_t5_reader_refuses_a_clean_receipt_with_an_empty_book(self):
        reader=build_plan.acceptance_manifest
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            (root/'secret-scan.json').write_text(json.dumps(dict(passed=True,clean=True,files_scanned=1,book_values=0)))
            with self.assertRaisesRegex(ValueError,'book|observed'):
                reader.relay_sanitizer({'relay_sanitizer':{'path':'secret-scan.json'}},reader.Evidence(root))

    def test_t5_reader_requires_secret_checks_for_every_driver_run(self):
        reader=build_plan.acceptance_manifest
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            (root/'secret-scan.json').write_text(json.dumps(dict(passed=True,clean=True,files_scanned=1,book_values=1)))
            (root/'review.json').write_text(json.dumps(dict(passed=True,runs=[dict(name='cloudflare',passed=True)],checklist=[])))
            spec=dict(relay_sanitizer={'path':'secret-scan.json'},relay_contract={'runs':['cloudflare']},product={'path':'review.json'})
            with self.assertRaisesRegex(ValueError,'per-run|secret check'):
                reader.relay_sanitizer(spec,reader.Evidence(root))

if __name__=='__main__':unittest.main()
