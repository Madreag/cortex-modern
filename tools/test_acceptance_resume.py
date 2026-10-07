"""Acceptance collection counterexamples using synthetic receipts only."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from test_inventory_oracle_evidence import INVENTORY
if INVENTORY.is_dir():
    import acceptance_manifest as reader
    import run_split
import run_tools_suites


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding='utf-8')


def fixture(root, *, item='1', boxes=('box-a',), passed=True, code=0, rerun=False):
    source = 'a'*40
    exe = b'unit executable bytes; never executed'
    sha = hashlib.sha256(exe).hexdigest()
    build = dict(commit=source, executable_sha256=sha)
    for box in boxes:
        write(root/f'{box}.identity.json', dict(box=box, platform='unit', source_sha=source, exe_sha256=sha, build=build))
    requirement = root/'requirements.md'; requirement.write_text(f'{item}. synthetic required row\n')
    directory = root/'S1/case'; directory.mkdir(parents=True)
    first, final = directory/'result.json', directory/'rerun-2/result.json'
    write(first, dict(passed=passed, checks=dict(measured=passed)))
    write(directory/'command.json', dict(id='case', collection_id='one', source_sha=source, driver='tools/feel_measure.py', attempt=1))
    command = dict(id='case', state='done', exit_code=code, dir=str(directory), driver='tools/feel_measure.py')
    product = first
    if rerun:
        write(final, dict(passed=True, checks=dict(measured=True)))
        write(final.parent/'command.json', dict(id='case', collection_id='one', source_sha=source, driver='tools/feel_measure.py', attempt=2))
        command['attempts'] = [dict(attempt=1, state='done', exit_code=1, dir=str(directory), product=dict(path=str(first))),
                               dict(attempt=2, state='done', exit_code=0, dir=str(final.parent), product=dict(path=str(final)))]
        product = final
    collection = root/'S1/DEFECTS.json'
    write(collection, dict(collection_complete=True, hard_count=int(not passed and not rerun), collection_id='one', source_sha=source,
                           commands=[command], evidence_sha256={str(path): reader.digest(path) for path in (first, final) if path.is_file()}))
    sequence = run_split.register_collection(root/'starts.jsonl', 'one', source, root)
    write(root/'split-plan.json', dict(collection_id='one', source_sha=source, sequence=sequence, shares=[dict(label='S1', ids=['case'])]))
    write(root/'review.json', dict(state='APPROVED', source_sha=source, covers=['case'], checklist=[dict(id='case', frames=[1], probe='pass')]))
    payload = {name: b'unit payload' for name in reader.REQUIRED_PACKAGE_FILES}
    payload.update({'Cortex Command.exe': exe, 'BUILD-RECEIPT.json': json.dumps(build).encode()})
    manifest = dict(source=dict(commit=source), files=[dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()) for name, data in payload.items()])
    write(root/'package.json', manifest)
    with zipfile.ZipFile(root/'package.zip', 'w') as archive:
        for name, data in payload.items(): archive.writestr(name, data)
        archive.writestr('MANIFEST.json', json.dumps(manifest))
    write(root/'verify.json', dict(passed=True, collection=[dict(id=name, status='PASS') for name in ('payload-and-build', 'unpacked-smoke')]))
    write(root/'approval.json', dict(state='APPROVED', source_sha=source, archive_sha256=reader.digest(root/'package.zip'), verification_sha256=reader.digest(root/'verify.json')))
    spec = dict(id='case', share='S1', acceptance_ids=[item], identities=[dict(path=f'{box}.identity.json', box=box) for box in boxes],
                boxes=list(boxes), roster='three-way' if len(boxes) == 3 else 'four-way' if len(boxes) == 4 else None,
                product=dict(path=str(product)), collection=dict(path=str(collection)), review=dict(path='review.json'), driver='tools/feel_measure.py')
    plan = root/'plan.json'
    write(plan, dict(schedule='split-plan.json', rows=[spec], package=dict(manifest='package.json', verification='verify.json', approval='approval.json', archive='package.zip')))
    return plan, requirement


@unittest.skipUnless(INVENTORY.is_dir(), 'external inventory tools are absent')
class AcceptanceResume(unittest.TestCase):
    def test_g_id_host_normalizes_writer_node_spelling(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement = fixture(root, boxes=('box-a',))
            result = reader.build_manifest(plan, requirement)
            self.assertEqual(result['rows'][0]['identities'][0]['box'], 'box-a')
            self.assertTrue(result['passed'], result['errors'])

    def test_g_sched_completed_product_failure_is_collected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement = fixture(root, passed=False, code=1)
            result = reader.build_manifest(plan, requirement)
            self.assertTrue(result['rows'][0]['collection_complete'], result['rows'][0]['errors'])
            self.assertEqual(result['rows'][0]['product_verdict'], 'FAIL')

    def test_g_reader_1819_uses_the_declared_three_box_roster(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement = fixture(root, item='18', boxes=('box-a', 'REMOTE', 'Mac'))
            result = reader.build_manifest(plan, requirement)
            self.assertTrue(result['passed'], (result['errors'], result['rows'][0]['errors']))

    def test_g_rerun_product_failure_cannot_be_replaced_by_green(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); plan, requirement = fixture(root, passed=False, rerun=True)
            result = reader.build_manifest(plan, requirement)
            self.assertFalse(result['rows'][0]['passed'], result['rows'][0])
            self.assertIn('rerun', ' '.join(result['rows'][0]['errors']).lower())

    def test_g_product_tools_suites_writes_a_hashable_verdict(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)/'result.json'
            with (patch.object(sys, 'argv', ['run_tools_suites.py', '--only', 'cross-report', '--out', str(out)]),
                  patch.object(run_tools_suites, 'run', return_value=('cross-report', 0, 'OK\n', {})),
                  contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
                try:
                    code = run_tools_suites.main()
                except SystemExit as error:
                    self.fail(f'verdict artifact option refused: exit={error.code}')
            self.assertEqual(code, 0)
            result = json.loads(out.read_text())
            self.assertTrue(result['passed'])
            self.assertEqual(result['counts']['failed'], 0)
            self.assertTrue(result['input_sha256'])
            self.assertTrue(Path(result['log']).is_file())


if __name__ == '__main__':
    unittest.main()
