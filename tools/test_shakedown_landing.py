"""The shakedown's retained-product failures, reproduced without launching an engine."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_inventory_oracle_evidence import INVENTORY,extract_defects,run_stream
from test_acceptance_resume3 import acceptance_render
import test_menu_readback as readback

REPO=Path(__file__).resolve().parents[1]


class ShakedownLanding(unittest.TestCase):
    def test_s01_full_readback_dumps_live_in_captures_and_product_stays_small(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/'capture'
            image=dict(path='dump_1.json',controls=['x'*(9<<20)],activity_table={'one':'two'},passed=False,error='native assertion')
            native=dict(case='landing',size='640x360',captures=[image],**{'pass':False},error='native assertion')
            argv=['readback','--repo',str(REPO),'--out',str(root),'--case','landing','--size','640x360','--port','48530']
            with patch.object(sys,'argv',argv),patch.object(readback,'run_case',return_value=native) as runner, \
                 patch.object(readback,'engine_executable',return_value=REPO/'VERSION.txt'),contextlib.redirect_stdout(io.StringIO()):
                code=readback.main()
            self.assertEqual(code,1);runner.assert_called_once()
            self.assertEqual(json.loads((root/'captures.json').read_text()),[image])
            self.assertLess((root/'result.json').stat().st_size,8<<20)
            product=json.loads((root/'result.json').read_text())
            self.assertFalse(product['pass']);self.assertEqual(product['captures_ref']['path'],'captures.json')

    def test_s02_mac_fetch_excludes_appledouble_at_creation(self):
        self.assertIn('COPYFILE_DISABLE=1 tar --no-xattrs',acceptance_render.RUN_TEMPLATE)

    def test_s03_fetched_posix_paths_localise_without_rewriting_hashed_receipts(self):
        path=REPO/'tools/localise_fetched.py'
        self.assertTrue(path.is_file(),'POSIX fetch needs the shakedown localization step')
        spec=importlib.util.spec_from_file_location('localise_unit',path);helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'S1root').mkdir()
            original='/Users/erol/cortex-workers/unit'
            data={'dir':original+'/S1root/S1/case','evidence_sha256':{original+'/S1root/S1/case/result.json':'a'*64}}
            for name in ('progress.json','DEFECTS.json','command.json','terminal.json'):
                (root/'S1root'/name).write_text(json.dumps(data))
            protected={name:(root/'S1root'/name).read_bytes() for name in ('command.json','terminal.json')}
            self.assertEqual(helper.localise(root,original),2)
            for name in ('progress.json','DEFECTS.json'):
                self.assertNotIn(original,(root/'S1root'/name).read_text())
            for name,raw in protected.items():self.assertEqual((root/'S1root'/name).read_bytes(),raw)
        self.assertIn('localise-mac',acceptance_render.RUN_TEMPLATE)
        self.assertIn('localise-linux',acceptance_render.RUN_TEMPLATE)

    def test_s04_join_product_is_its_verdict_and_runner_record_is_not_required(self):
        command=run_stream.Command('S1.join','@join_form.py',[])
        self.assertEqual(run_stream.product_relative(command),'run/mixed-history-join.json')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'run-result.json').write_text(json.dumps(dict(host={'exit_code':0},client={'exit_code':0})))
            (root/'mixed-history-join.json').write_text(json.dumps(dict(passed=True,reason='')))
            result=extract_defects.Extractor(root,True,None).run()
            self.assertTrue(result['collection_complete'],result['collection_errors'])
            self.assertEqual(result['hard_count'],0)
        spec=importlib.util.spec_from_file_location('join_form_unit',INVENTORY/'join_form.py')
        form=importlib.util.module_from_spec(spec);spec.loader.exec_module(form)
        self.assertTrue(callable(getattr(form,'record',None)))

    def test_s05_mac_uses_the_existing_luajit_suppressions(self):
        text=(REPO/'tools/macos/acceptance_stream.zsh').read_text()
        line=next(line for line in text.splitlines() if line.startswith('step asan-suite'))
        self.assertIn('UBSAN_OPTIONS="suppressions=$REPO/tools/sanitizers/ubsan.supp:',line)

    def test_s07_readback_error_is_named_even_when_it_quotes_a_runner_record(self):
        reason="assert_label LabelLobbyPlayer1 failed; capture {'exit_code': 0, 'case': 'lobby'}"
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'result.json').write_text(json.dumps(dict(cases=[{'case':'lobby','pass':False,'error':reason}])))
            result=extract_defects.Extractor(root,True,None).run()
            self.assertIn('assert_label LabelLobbyPlayer1 failed',json.dumps(result['defects']))

    def test_s08_foreign_posix_guard_refuses_before_launch_and_names_holder(self):
        path=REPO/'tools/acceptance_posix_guard.py'
        self.assertTrue(path.is_file(),'every POSIX runner must check the box reservation')
        spec=importlib.util.spec_from_file_location('posix_guard_unit',path);guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);marker=root/'ACCEPTANCE-STREAM-RUNNING';marker.mkdir();(marker/'owner').write_text('other:lane:123')
            with self.assertRaisesRegex(RuntimeError,'other:lane:123'):
                guard.assert_available({},root)
            guard.assert_available({'CC_ACCEPTANCE_BOX_OWNER':'other:lane:123'},root)
            from posix_test_runner import IsolatedRun
            import acceptance_posix_guard
            with patch.object(acceptance_posix_guard,'assert_available',side_effect=RuntimeError('held by other:lane:123')), \
                 patch('posix_test_runner.subprocess.Popen') as spawn:
                run=IsolatedRun([sys.executable,'-c','pass'],root,root/'out',startup_checks=False)
                with self.assertRaisesRegex(RuntimeError,'other:lane:123'):run.start()
                spawn.assert_not_called()


if __name__=='__main__':unittest.main()
