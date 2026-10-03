"""Private module overlays carry their actual settings before measurement staging."""
from contextlib import ExitStack
import contextlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import cross_peers as cross
import run_sim_test


class PrivateRuntimeMetadata(unittest.TestCase):
    def test_fresh_module_overlay_has_settings_before_activity_staging(self):
        files = {}
        def wrote(path, value, *args, **kwargs): files[path.as_posix()] = value
        def read(path, *args, **kwargs): return files[path.as_posix()]
        def make(repo, flags, out, **kwargs):
            wrote(out/'runtime.json',json.dumps(dict(retained_runtime=str(kwargs['runtime']),copied=False)))
            return SimpleNamespace(out=out,cwd=kwargs['runtime'],argv=['engine',*flags])
        staged=[]
        def activity(run,spec):
            observed=json.loads(read(run.out/'runtime.json'))['settings_overrides']
            self.assertEqual(observed,run_sim_test.RUNTIME_SETTINGS)
            staged.append(True)
        spec=dict(own='/virtual/host',repo='/repo',peer='erol',ticks=1201,faults=[],incarnation=0,
                  roster='four-way',scene='Void Wanderers',initial_skill=100,env={},flags=[],timeout=30,
                  acceptance_row='mod-match')
        box=dict(kind='windows-task',directory_port=49148)
        with ExitStack() as stack:
            stack.enter_context(patch.object(Path,'mkdir'))
            stack.enter_context(patch.object(Path,'write_text',wrote))
            stack.enter_context(patch.object(Path,'read_text',read))
            stack.enter_context(patch.object(cross,'digest_file',return_value='a'*64))
            stack.enter_context(patch.object(cross,'write_json',side_effect=lambda path,value:wrote(path,json.dumps(value))))
            stack.enter_context(patch.object(run_sim_test,'make_run',side_effect=make))
            stack.enter_context(patch.object(run_sim_test,'seed_settings'))
            stack.enter_context(patch.object(cross.acceptance_cross,'directory_settings',side_effect=lambda spec,value:value))
            stack.enter_context(patch.object(cross.acceptance_cross,'prepare_mod_runtime',return_value=Path('/virtual/private-runtime')))
            stack.enter_context(patch.object(cross.acceptance_cross,'stage_activity',side_effect=activity))
            cross.prepare_instance(spec,'',box)
        self.assertEqual(staged,[True])

    def test_cancelled_initial_client_does_not_wait_for_publication(self):
        documents={}
        spec=dict(peer='edith',role='player',own='/virtual/edith',flags=['-net-match-peers','4'],session_wait_s=240)
        payload=dict(box=dict(name='EDITH',kind='windows-task'),specs=[spec],pin='')
        with patch.object(Path,'read_text',lambda path,**kw:json.dumps(payload) if path.name=='payload.json' else '{}'), \
                patch.object(Path,'is_file',lambda path:path.name=='stop.json'), \
                patch.object(cross,'assert_box_guard'), patch.object(cross,'read_capabilities',return_value=dict(peer_limit=4)), \
                patch.object(cross,'wait_for_payload_release'), patch.object(cross,'prepare_instance') as prepare, \
                patch.object(cross,'write_json',side_effect=lambda path,value:documents.update({path.name:value})), \
                patch.object(cross.time,'monotonic',return_value=1), \
                patch.object(cross.time,'sleep',side_effect=AssertionError('waited despite native cancellation')) as sleep, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cross.run_payload(Path('/virtual/payload.json')),1)
        prepare.assert_not_called(); sleep.assert_not_called()
        self.assertIn('coordinator cancelled',documents['payload-error.json']['error'])


if __name__=='__main__': unittest.main()
