"""A shallow acceptance checkout can receive a merged incremental bundle without full history."""
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


class ShallowRefresh(unittest.TestCase):
    def test_merged_incremental_refresh_supplies_missing_shallow_prerequisites(self):
        local=module(INVENTORY/'refresh_windows.py','shallow_refresh_local')
        native=module(INVENTORY/'refresh_windows.py','shallow_refresh_native')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';target=root/'target';source.mkdir()
            def git(repo,*args):return subprocess.check_output(['git','-C',str(repo),*args],stderr=subprocess.DEVNULL,text=True).strip()
            git(source,'init');git(source,'config','user.name','Test');git(source,'config','user.email','test@example.invalid')
            (source/'common.txt').write_text('shared\n');git(source,'add','.');git(source,'commit','-m','Common')
            git(source,'branch','side');main=git(source,'branch','--show-current')
            (source/'main.txt').write_text('main\n');git(source,'add','.');git(source,'commit','-m','Main')
            git(source,'checkout','side');(source/'side.txt').write_text('old side\n');git(source,'add','.');git(source,'commit','-m','Side')
            git(source,'checkout',main);git(source,'merge','--no-ff','-m','First merge','side')
            subprocess.check_output(['git','clone','--depth','1','-q',source.as_uri(),str(target)],stderr=subprocess.DEVNULL)
            base=git(target,'rev-parse','HEAD');self.assertEqual(git(target,'rev-parse','--is-shallow-repository'),'true')
            git(source,'checkout','side');(source/'side.txt').write_text('new side\n');git(source,'add','.');git(source,'commit','-m','Side update')
            git(source,'checkout',main);git(source,'merge','--no-ff','-m','Second merge','side');tip=git(source,'rev-parse','HEAD')
            here=root/'here';there=root/'there';here.mkdir();there.mkdir();events=[]
            def mapped(value):return there/Path(value).relative_to(Path('D:/mx'))
            original_run=subprocess.run
            def transport(argv,*args,**kwargs):
                if argv[0]=='ssh':
                    mapped(argv[-1].split("'")[1]).mkdir();return subprocess.CompletedProcess(argv,0)
                if argv[0]=='scp':
                    shutil.copyfile(argv[-2],mapped(argv[-1].split(':',1)[1]));return subprocess.CompletedProcess(argv,0)
                return original_run(argv,*args,**kwargs)
            def call(alias,request):
                request=dict(request)
                for name in ('bundle','staging'):
                    if name in request:request[name]=str(mapped(request[name]))
                events.append(request['action']);return native.native(request)
            outcome=None
            with patch.object(local,'TARGETS',{'ALLY':('stub',str(target))}),patch.object(local,'SCRATCH',here),patch.object(native,'SCRATCH',there),patch.object(native,'guard_state',return_value=dict(engines=[],markers=[])),patch.object(local,'remote_call',side_effect=call),patch.object(local.subprocess,'run',side_effect=transport),contextlib.redirect_stdout(io.StringIO()):
                try:outcome=local.main(['--box','ALLY','--repo',str(source),'--git-only'])
                except (subprocess.CalledProcessError,ValueError,RuntimeError) as error:outcome=type(error).__name__
            self.assertEqual(outcome,0,'merged bundle could not refresh the shallow acceptance tree')
            self.assertEqual(git(target,'rev-parse','HEAD'),tip)
            self.assertEqual((target/'side.txt').read_text(),'new side\n')
            self.assertEqual(git(target,'status','--porcelain'),'')
            self.assertNotEqual(base,tip)
            self.assertFalse(list(here.rglob('source.bundle')) or list(there.rglob('source.bundle')))
            self.assertLess(events.index('guard'),events.index('checkout'))


if __name__=='__main__':unittest.main()
