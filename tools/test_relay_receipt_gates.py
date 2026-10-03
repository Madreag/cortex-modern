"""A relay parent cannot borrow a different engine capability or pass without its sanitizer receipt."""
import json
import gzip
from pathlib import Path
import tempfile
import unittest

import acceptance_collection as collection
from test_harness_resume5 import built
from test_inventory_oracle_evidence import INVENTORY
from test_acceptance_resume3 import build_plan


class RelayReceiptGates(unittest.TestCase):
    def test_safe_login_proof_names_a_real_native_box(self):
        from acceptance_relay_policy import safe_login_proof
        reader=build_plan.acceptance_manifest
        proof=dict(box='EDITH',engine_row='A63.1',source_sha='a'*40,exe_sha256='b'*64,sanitizer_zero_logins=True)
        proof['pass']=True
        for box in ('EDITH','ALLY'):
            proof['box']=box
            self.assertTrue(safe_login_proof(proof,'a'*40,'b'*64))
            self.assertTrue(reader.capability_passes('relay.safe-login',proof,'a'*40,'b'*64))
        for box in ('ALLY|EDITH','Mac'):
            proof['box']=box
            self.assertFalse(safe_login_proof(proof,'a'*40,'b'*64))
            self.assertFalse(reader.capability_passes('relay.safe-login',proof,'a'*40,'b'*64))

    def test_escaped_ini_and_hex_logins_stay_red(self):
        from acceptance_relay_policy import CredentialBook,scan_retained
        forms=[b'{"payload":"{\\"username\\":\\"hidden-login\\",\\"credential\\":\\"hidden-password\\"}"}',
               b'NetworkTurnUser = hidden-login\nNetworkTurnPass = hidden-password\n',
               json.dumps({'wire':b'{"username":"hidden-login","credential":"hidden-password"}'.hex()}).encode()]
        for data in forms:
            with self.subTest(form=data[:25]),tempfile.TemporaryDirectory() as folder:
                root=Path(folder);(root/'native.bin').write_bytes(data)
                self.assertFalse(scan_retained(root,CredentialBook())['passed'])

    def test_private_settings_redact_both_login_parts_after_the_run(self):
        import edith_cross
        from types import SimpleNamespace
        from unittest.mock import Mock
        seed=Mock();spec={'settings':{'NetworkTurnUser':'private-user','NetworkTurnPass':'private-password'}}
        edith_cross.redact(SimpleNamespace(run=SimpleNamespace(seed_settings=seed)),object(),spec)
        self.assertEqual(spec['settings']['NetworkTurnUser'],'redacted')
        self.assertEqual(spec['settings']['NetworkTurnPass'],'redacted')

    def test_reader_rechecks_encoded_forms_despite_a_typed_clean_receipt(self):
        reader=build_plan.acceptance_manifest
        for data in (b'NetworkTurnPass=hidden-password\n',json.dumps({'wire':b'{"username":"hidden-user"}'.hex()}).encode()):
            with self.subTest(data=data[:20]),tempfile.TemporaryDirectory() as folder:
                root=Path(folder);(root/'native.log').write_bytes(data)
                collection.write(root/'secret-scan.json',dict(clean=True,files_scanned=1,files_with_logins=[]))
                with self.assertRaisesRegex(ValueError,'login'):
                    reader.relay_sanitizer({'relay_sanitizer':{'path':'secret-scan.json'}},reader.Evidence(root))

    def test_compressed_native_logs_are_scanned_and_clean_ones_are_readable(self):
        from acceptance_relay_policy import CredentialBook,scan_retained
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);path=root/'native.jsonl.gz'
            path.write_bytes(gzip.compress(b'{"ticks":1201}\n'))
            self.assertTrue(scan_retained(root,CredentialBook())['passed'])
            path.write_bytes(gzip.compress(b'{"username":"compressed-login","credential":"compressed-password"}\n'))
            self.assertFalse(scan_retained(root,CredentialBook())['passed'])

    def test_readback_proof_cannot_unblock_a63_login_logging(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);result=built();schedule=result['split_plan']
            schedule.update(collection_id='unit',source_sha='a'*40,exe_sha256='b'*64,sequence={'index':1})
            collection.write(root/'acceptance-plan.json',result['plan']);collection.write(root/'split-plan.json',schedule)
            collection.write(root/'capabilities/edith-readback.json',dict(box='EDITH',engine_row='A57.2',source_sha='a'*40,exe_sha256='b'*64,**{'pass':True}))
            choice=collection.start_section(root,2,marker=root/'absent',inventory_root=INVENTORY,optional_boxes={})
            hold=next(row for row in choice['rows'] if row['id']=='S1.turn-hold')
            self.assertEqual(hold['state'],'AWAITING')
            self.assertIn('A63.1',hold['reason'])

    def test_reader_requires_the_relay_sanitizer_receipt(self):
        reader=build_plan.acceptance_manifest
        judge=getattr(reader,'relay_sanitizer',None)
        self.assertTrue(callable(judge),'relay products require their own sanitizer evidence')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);evidence=reader.Evidence(root)
            spec={'relay_sanitizer':{'path':'run/secret-scan.json','required':True},'product':{'path':'run/result.json'}}
            with self.assertRaisesRegex((ValueError,FileNotFoundError),'sanitizer|secret-scan'):
                judge(spec,evidence)
            collection.write(root/'run/secret-scan.json',dict(clean=True,files_scanned=1,files_with_logins=[]))
            collection.write(root/'run/result.json',dict(passed=True))
            collection.write(root/'run/host.replay',{'username':'native-retained-login','credential':'native-retained-password'})
            with self.assertRaisesRegex(ValueError,'login'):
                judge(spec,evidence)


if __name__=='__main__':unittest.main()
