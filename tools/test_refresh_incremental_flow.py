"""Run the real refresh orchestration against two temporary Git trees; SSH is a local file transport stand-in."""
import contextlib
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_harness_resume6 import module
from test_inventory_oracle_evidence import INVENTORY


class IncrementalRefreshFlow(unittest.TestCase):
    def test_real_refresh_verifies_checkout_then_removes_both_bundles(self):
        local=module(INVENTORY/'refresh_windows.py','refresh_flow_local')
        native=module(INVENTORY/'refresh_windows.py','refresh_flow_native')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';target=root/'target';source.mkdir()
            def git(repo,*args):return subprocess.check_output(['git','-C',str(repo),*args],stderr=subprocess.DEVNULL,text=True).strip()
            git(source,'init');git(source,'config','user.name','Test');git(source,'config','user.email','test@example.invalid')
            (source/'sample.txt').write_text('base\n');git(source,'add','.');git(source,'commit','-m','Base')
            subprocess.check_output(['git','clone','--no-hardlinks','-q',str(source),str(target)],stderr=subprocess.DEVNULL)
            (source/'sample.txt').write_text('tip\n');git(source,'add','.');git(source,'commit','-m','Tip');tip=git(source,'rev-parse','HEAD')
            here=root/'here';there=root/'there';here.mkdir();there.mkdir();events=[]
            def mapped(value):return there/Path(value).relative_to(Path('D:/mx'))
            original_run=subprocess.run
            def transport(argv,*args,**kwargs):
                if argv[0]=='ssh':
                    remote_root=argv[-1].split("'")[1];mapped(remote_root).mkdir();events.append('remote directory')
                    return subprocess.CompletedProcess(argv,0)
                if argv[0]=='scp':
                    destination=mapped(argv[-1].split(':',1)[1]);shutil.copyfile(argv[-2],destination);events.append('bundle copied')
                    return subprocess.CompletedProcess(argv,0)
                return original_run(argv,*args,**kwargs)
            def call(alias,request):
                request=dict(request)
                if 'bundle' in request:request['bundle']=str(mapped(request['bundle']))
                result=native.native(request)
                events.append(request['action'])
                if request['action']=='remove-bundle':
                    self.assertIn('checkout',events,'bundle was removed before checkout verification')
                    self.assertEqual(git(target,'rev-parse','HEAD'),tip)
                return result
            idle=lambda repo:dict(engines=[],markers=[])
            with patch.object(local,'TARGETS',{'ALLY':('stub',str(target))}),patch.object(local,'SCRATCH',here),patch.object(native,'SCRATCH',there),patch.object(native,'guard_state',side_effect=idle),patch.object(local,'remote_call',side_effect=call),patch.object(local.subprocess,'run',side_effect=transport),contextlib.redirect_stdout(io.StringIO()):
                code=local.main(['--box','ALLY','--repo',str(source),'--git-only'])
            self.assertEqual(code,0)
            self.assertEqual(git(target,'rev-parse','HEAD'),tip)
            self.assertEqual((target/'sample.txt').read_text(),'tip\n')
            self.assertFalse(list(here.rglob('source.bundle')) or list(there.rglob('source.bundle')),'a verified refresh kept a runtime bundle')
            self.assertLess(events.index('guard'),events.index('bundle copied'))

if __name__=='__main__':unittest.main()
