"""Cache detectors use temporary private snapshots, never an engine or owner tree."""
import hashlib
import builtins
import contextlib
import json
import os
import stat
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import spread_peers as spread


class CacheTests(unittest.TestCase):
    @unittest.skipUnless(os.name == 'nt', 'native read-only file disposition is Windows-specific')
    def test_readonly_git_object_prunes_without_changing_another_hard_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, retained = Path(temporary)/'snapshots', Path(temporary)/'retained-object'
            root.mkdir()
            for index in range(4):
                snapshot = root/str(index)
                snapshot.mkdir()
                receipt = snapshot/'pool-inputs.json'
                receipt.write_text(json.dumps(dict(head=str(index), manifest={})))
                os.utime(receipt, (100+index, 100+index))
            cached = root/'0/.git/objects/00/object'
            cached.parent.mkdir(parents=True)
            cached.write_bytes(b'original immutable git object')
            os.link(cached, retained)
            os.chmod(cached, stat.S_IREAD)
            attributes = retained.stat().st_file_attributes
            self.assertTrue(attributes & stat.FILE_ATTRIBUTE_READONLY)
            result = spread.prune_snapshots(root, root/'3')
            self.assertEqual(result['removed'], ['0'])
            self.assertEqual(retained.read_bytes(), b'original immutable git object')
            self.assertEqual(retained.stat().st_file_attributes, attributes)

    def test_link_limit_materializes_a_verified_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, target = Path(temporary)/'blob', Path(temporary)/'snapshot'/'file'
            source.write_bytes(b'content pinned by sha256')
            expected = hashlib.sha256(source.read_bytes()).hexdigest()
            failure = OSError('more links on a file than the file system supports')
            failure.winerror = 1142
            with patch.object(spread.os, 'link', side_effect=failure) as link:
                spread.cache_link_or_copy(source, target, expected)
            self.assertEqual(target.read_bytes(), source.read_bytes())
            self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(), expected)
            self.assertEqual(link.call_count, 1)

    def test_each_materialization_prunes_to_the_last_three_snapshots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/'cache'/'snapshots'
            root.mkdir(parents=True)
            for index in range(6):
                snapshot = root/str(index)
                snapshot.mkdir()
                receipt = snapshot/'pool-inputs.json'
                receipt.write_text(json.dumps(dict(head=str(index), manifest={})))
                os.utime(receipt, (100+index, 100+index))
            result = spread.prune_snapshots(root, root/'5')
            self.assertEqual(sorted(path.name for path in root.iterdir()), ['3', '4', '5'])
            self.assertEqual(sorted(result['removed']), ['0', '1', '2'])

    def test_active_snapshot_and_unknown_directory_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/'snapshots'
            root.mkdir()
            for index in range(5):
                snapshot = root/str(index)
                snapshot.mkdir()
                receipt = snapshot/'pool-inputs.json'
                receipt.write_text(json.dumps(dict(head=str(index), manifest={})))
                os.utime(receipt, (100+index, 100+index))
            foreign = root/'unmarked'
            foreign.mkdir()
            (foreign/'owner').write_text('preserve this directory')
            result = spread.prune_snapshots(root, root/'4', protected=[root/'0'])
            self.assertTrue((root/'0').is_dir())
            self.assertEqual((foreign/'owner').read_text(), 'preserve this directory')
            self.assertEqual(result['deferred_active'], ['0'])

    def test_a_directory_link_inside_an_old_snapshot_never_reaches_its_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, outside = Path(temporary)/'snapshots', Path(temporary)/'owner'
            root.mkdir(); outside.mkdir()
            sentinel = outside/'untouched'
            sentinel.write_bytes(b'owner content')
            for index in range(4):
                snapshot = root/str(index)
                snapshot.mkdir()
                receipt = snapshot/'pool-inputs.json'
                receipt.write_text(json.dumps(dict(head=str(index), manifest={})))
                os.utime(receipt, (100+index, 100+index))
            spread.link_directory(outside, root/'0'/'linked-owner')
            spread.prune_snapshots(root, root/'3')
            self.assertEqual(sentinel.read_bytes(), b'owner content')
            self.assertFalse((root/'0').exists())

    def test_a_conflicting_cache_file_is_not_overwritten_by_copy_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, target = Path(temporary)/'blob', Path(temporary)/'existing'
            source.write_bytes(b'expected'); target.write_bytes(b'foreign evidence')
            expected = hashlib.sha256(source.read_bytes()).hexdigest()
            with patch.object(spread.os, 'link', side_effect=OSError('link failed')), \
                 self.assertRaisesRegex(spread.SpreadRefusal, 'immutable cache artifact differs'):
                spread.cache_link_or_copy(source, target, expected)
            self.assertEqual(target.read_bytes(), b'foreign evidence')

    def test_shipped_worker_materializes_with_fallback_and_prunes_every_time(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache, native = root/'cache', root/'native'
            snapshots = cache/'snapshots'
            snapshots.mkdir(parents=True)
            source = root/'blob'
            source.write_bytes(b'shipped content')
            expected = hashlib.sha256(source.read_bytes()).hexdigest()
            for index in range(4):
                path = snapshots/str(index)
                path.mkdir()
                (path/'pool-inputs.json').write_text(json.dumps(dict(head=str(index), manifest={})))
                os.utime(path/'pool-inputs.json', (100+index, 100+index))
            code = '''def _materialize(value):
    source,target,expected = Path(value['source']),Path(value['target']),value['expected']
    target.parent.mkdir(parents=True,exist_ok=True)
    os.link(source,target)
    atomic_json(target.parent/'pool-inputs.json',dict(head=value['head'],manifest=value['manifest']))
    return dict(repo=str(target.parent),verified=True)
def materialize(value):
    return _materialize(value)
if __name__=='__main__':raise SystemExit(main())
'''
            def atomic(path, value):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(value))
            namespace = dict(__name__='fake_worker', Path=Path, os=os, cache_root=lambda b:cache,
                             snapshot_root=lambda v:Path(v['target']).parent, root_for=lambda b:native,
                             facts=SimpleNamespace(read_reservation=lambda *a,**k:None),
                             mutex=lambda *a,**k:contextlib.nullcontext(), atomic_json=atomic)
            exec(compile(spread.native_cache_source(code), '<cache-worker>', 'exec'), namespace)
            failure = OSError('link limit'); failure.winerror = 1142
            original_import = builtins.__import__
            def control_import(name, *args, **kwargs):
                if name == 'run_sim_test':
                    raise ModuleNotFoundError("No module named 'run_sim_test'")
                return original_import(name, *args, **kwargs)
            # The materializer imports only the published control ship set,
            # before repository tools are available on its module path.
            with patch.object(spread.os, 'link', side_effect=failure), \
                 patch.object(builtins, '__import__', side_effect=control_import):
                result = namespace['materialize'](dict(box=dict(name='NAMED'), source=str(source),
                    target=str(snapshots/'new'/'file'), expected=expected, head='new', manifest=dict(file=expected)))
            self.assertTrue(result['verified'])
            self.assertEqual((snapshots/'new'/'file').read_bytes(), source.read_bytes())
            self.assertEqual(sorted(path.name for path in snapshots.iterdir()), ['2', '3', 'new'])


if __name__ == '__main__':
    unittest.main()
