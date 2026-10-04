"""The row collector reads native evidence without requiring an unfrozen API."""
import hashlib
import inspect
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import acceptance_frozen_report as collector
import cross_report


class FrozenCollection(unittest.TestCase):
    def test_collection_body_is_the_pinned_frozen_prefix(self):
        source=inspect.getsource(collector.collect_native)
        source=source[:source.index('    return dict(manifest=manifest')]
        source=source.replace('def collect_native(root):','def build_report(root):',1)
        self.assertEqual(hashlib.sha256(source.encode()).hexdigest(),collector.COLLECTION_SHA256)

    def test_native_mixed_binary_and_foreign_load_remain_failures(self):
        retained=os.environ.get('CC_ACCEPTANCE_TEST_ROOT')
        if retained:
            root=Path(tempfile.mkdtemp(prefix='frozen-collector-',dir=retained))
        else:
            temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup);root=Path(temporary.name)
        own=root/'erol/incarnation-0';(own/'engine').mkdir(parents=True)
        manifest=dict(boxes=[dict(name='EROL-PC',kind='windows-local')],
                      specs=[dict(peer='erol',box='EROL-PC',role='host',root=str(root),own=str(own))],
                      preflights={'EROL-PC':dict(executable_sha256='a'*64,load=[])},driver_findings=[],
                      faults=[],ticks=1201,quiet_window=True,
                      storage=dict(presentation_chunk_bytes=8388608,presentation_retained_chunks=16),
                      memory=dict(warmup_s=120,slope_bytes_per_minute=8388608,retained_bytes=134217728,sample_seconds=60))
        for path,value in [(root/'manifest.json',manifest),(own/'record.json',dict(exe_sha256='b'*64,exit_code=0)),
                           (own/'match-report.json',{})]:
            path.write_text(json.dumps(value))
        (root/'samples.jsonl').write_text(json.dumps(dict(peer='erol',incarnation=0,elapsed_s=1,
            load=[dict(ProcessId=99,Name='Cortex Command.exe')],same_box_instances=1))+'\n')
        (own/'engine/stdout.log').write_text('native log exists\n')
        with patch.object(cross_report,'build_report',side_effect=AssertionError('unfrozen API called')):
            result=collector.collect(root)
        self.assertEqual(len(result['mixed_builds']),1)
        self.assertFalse(result['peers']['erol']['feel_gated'])
        self.assertFalse(result['peers']['erol']['presentation_valid'])
        self.assertFalse(result['peers']['erol']['tick_timing_valid'])


if __name__ == '__main__': unittest.main()
