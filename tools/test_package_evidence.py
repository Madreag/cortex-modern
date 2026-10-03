import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import package_windows


class PackageEvidence(unittest.TestCase):
    def fixture(self, root):
        for name in package_windows.REQUIRED_PAYLOAD - {package_windows.BUILD_RECEIPT}:
            path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('payload')
        exe = package_windows.sha256(root / package_windows.EXECUTABLE)
        (root / package_windows.BUILD_RECEIPT).write_text(json.dumps(dict(commit='a'*40, executable_sha256=exe)))
        entries = [dict(path=name, bytes=(root/name).stat().st_size, sha256=package_windows.sha256(root/name))
                   for name in sorted(package_windows.REQUIRED_PAYLOAD)]
        manifest = dict(schema=1, version='1.0.0', tag='v1.0.0', source=dict(commit='a'*40), executable=package_windows.EXECUTABLE,
                        build_receipt=package_windows.BUILD_RECEIPT, files=entries)
        (root/'MANIFEST.json').write_text(json.dumps(manifest))
        smoke = root/'smoke'; smoke.mkdir()
        spec = json.loads((Path(package_windows.__file__).parent/'e2e/sp-smoke.json').read_text())
        runs = [dict(name=row['name'], peers=[dict(peer=p['name'], record=dict(exit_code=0, exe_sha256=exe))
            for p in row.get('peers', spec.get('peers', []))]) for row in spec['runs']]
        (smoke/'capture.json').write_text(json.dumps(dict(scenario='sp-smoke', runs=runs,
            source=dict(tip='a'*40, package='v1.0.0'), exe=dict(path=str(root/package_windows.EXECUTABLE), sha256=exe))))
        items = [dict(id=prefix+p['peer'], run=run['name'], probe='pass') for run in runs for p in run['peers']
                 for prefix in ('recording-rate-', 'recording-stills-')]
        (smoke/'review.json').write_text(json.dumps(dict(checklist=items, verdict='agent-review-required')))
        return manifest

    def test_payload_build_and_unpacked_smoke_are_all_required(self):
        for case in ('clean', 'missing_data', 'build_hash', 'missing_smoke', 'different_exe', 'missing_run'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                manifest = self.fixture(root)
                if case=='missing_data':
                    manifest['files'] = [row for row in manifest['files'] if not row['path'].startswith('Data/')]
                    (root/'MANIFEST.json').write_text(json.dumps(manifest))
                if case=='build_hash':
                    (root/package_windows.BUILD_RECEIPT).write_text(json.dumps(dict(commit='a'*40, executable_sha256='b'*64)))
                if case in ('missing_smoke', 'different_exe', 'missing_run'):
                    path = root/'smoke/capture.json'
                    data = json.loads(path.read_text())
                    if case=='missing_smoke': data = {}
                    if case=='different_exe': data['exe']['sha256'] = 'b'*64
                    if case=='missing_run': data['runs'].pop()
                    path.write_text(json.dumps(data))
                result = package_windows.verify_evidence(root)
                self.assertEqual(result['passed'], case=='clean', result)
                self.assertEqual([row['id'] for row in result['collection']], ['payload-and-build', 'unpacked-smoke'])

    def test_p22_empty_manifest_does_not_verify_a_package(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'MANIFEST.json').write_text(json.dumps(dict(files=[])))
            with contextlib.redirect_stdout(io.StringIO()):
                code = package_windows.verify(root)
        self.assertNotEqual(code, 0)


if __name__ == '__main__':
    unittest.main()
