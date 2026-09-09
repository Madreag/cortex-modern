"""Record production transitions without repairing engine state after the call."""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
import argparse
import base64
import hashlib
import gzip
import itertools
import json
import re
import shutil
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(repo / 'tools'))
from run_sim_test import make_run
from compare_sim_traces import strict_compare
from compare_snapshots import compare_graphs, parse_graph
import cross_process_state

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def compare_lua_observations(first, first_stage, second, second_stage, cross_process=False):
    """All five complete Lua graphs; no local-AI or presentation projection."""
    results = []
    for index in range(5):
        a = Path(first) / f'trace.json.contract.{first_stage}.lua{index}'
        b = Path(second) / f'trace.json.contract.{second_stage}.lua{index}'
        result = {'vm': index, 'first': str(a), 'second': str(b)}
        try:
            a_bytes, b_bytes = a.read_bytes(), b.read_bytes()
            result.update(first_sha256=hashlib.sha256(a_bytes).hexdigest(), second_sha256=hashlib.sha256(b_bytes).hexdigest(),
                          detail=compare_graphs(parse_graph(a_bytes), parse_graph(b_bytes), actor_uids=None, cross_process=cross_process), passed=True)
        except Exception as error:
            result.update(passed=False, error=str(error))
        results.append(result)
    return {'passed': all(item['passed'] for item in results), 'full_comparison': True,
            'cross_process': cross_process, 'vms': results}


def compare_state_files(first, second, destination, cross_process=False):
    """Compare exact raw documents; native string values can contain newlines/NULs."""
    if cross_process:
        return compare_state_fields(first, second, destination)
    count = 0
    open_first = gzip.open if str(first).endswith('.gz') else open
    open_second = gzip.open if str(second).endswith('.gz') else open
    hashes = [hashlib.sha256(), hashlib.sha256()]
    sizes = [0, 0]
    with open_first(first, 'rb') as a_stream, open_second(second, 'rb') as b_stream, gzip.open(destination, 'wt', encoding='utf-8') as output:
        offset = 0
        while True:
            a, b = a_stream.read(65536), b_stream.read(65536)
            if not a and not b: break
            for index, block in enumerate((a, b)):
                hashes[index].update(block)
                sizes[index] += len(block)
            if a != b:
                output.write(json.dumps({'byte_offset': offset, 'first_base64': base64.b64encode(a).decode(),
                                         'second_base64': base64.b64encode(b).decode()}) + '\n')
                count += 1
            offset += 65536
    return {'equal': count == 0, 'changed_blocks': count, 'first_bytes': sizes[0], 'second_bytes': sizes[1],
            'first_sha256': hashes[0].hexdigest(), 'second_sha256': hashes[1].hexdigest(), 'artifact': str(destination),
            'classification': 'Exact unfiltered document comparison; differences are 64-KiB byte blocks, not a field count.'}


def compare_state_fields(first, second, destination):
    """The reference runs in another process, so project the named real-clock fields and keep the rest."""
    open_first = gzip.open if str(first).endswith('.gz') else open
    open_second = gzip.open if str(second).endswith('.gz') else open
    hashes, sizes, counts = [hashlib.sha256(), hashlib.sha256()], [0, 0], [0, 0]
    differences, projected_lines, projected = 0, 0, {}
    with open_first(first, 'rb') as a_stream, open_second(second, 'rb') as b_stream, gzip.open(destination, 'wt', encoding='utf-8') as output:
        for index, (a, b) in enumerate(itertools.zip_longest(a_stream, b_stream)):
            for position, block in enumerate((a, b)):
                if block is not None:
                    hashes[position].update(block)
                    sizes[position] += len(block)
                    counts[position] += 1
            if a == b: continue
            record = {'line': index, 'reference': text_line(a), 'candidate': text_line(b)}
            fields = [cross_process_state.field_of(value) if value is not None else None for value in (record['reference'], record['candidate'])]
            record['field'] = fields[0]
            reason = cross_process_state.real_clock_reason(fields[0]) if fields[0] is not None and fields[0] == fields[1] else None
            if reason:
                family = projected.setdefault(cross_process_state.family(fields[0]), {'count': 0, 'reason': reason})
                family['count'] += 1
                projected_lines += 1
            else:
                differences += 1
            record.update(projected=bool(reason), reason=reason)
            output.write(json.dumps(record) + '\n')
    return {'equal': differences == 0 and counts[0] == counts[1], 'cross_process': True, 'changed_lines': differences,
            'projected_lines': projected_lines, 'projected': projected, 'first_lines': counts[0], 'second_lines': counts[1],
            'first_bytes': sizes[0], 'second_bytes': sizes[1], 'first_sha256': hashes[0].hexdigest(),
            'second_sha256': hashes[1].hexdigest(), 'artifact': str(destination),
            'classification': 'Line-exact document comparison. Every difference is retained in the artifact; only the fields named in '
                              'cross_process_state.REAL_CLOCK_FIELDS are projected, counted per family with the writer that makes each per-process.'}


def text_line(line):
    return None if line is None else line.decode('utf-8', 'replace').removesuffix('\n')


# A transition named here must apply; every load transaction answers to --expect-load instead, and
# 'observe'/'stage' record an outcome of their own.
APPLYING_OPERATIONS = ('save', 'memory', 'file', 'hold', 'preview')
LOAD_OPERATIONS = ('load', 'ordinary-load')
STAGE_TRANSACTIONS = ('stage-reject', 'stage-ordinary-reject')
# A refusal is allowed to print why it refused; an assert, an abort or a traceback never is.
CRASH_ERROR = re.compile(r'RTE Assert|RTE Abort|stack traceback')


def refused_load(item):
    """A load transaction whose candidate the engine actually turned down."""
    staged = item.get('staged_candidate') or {}
    return (item['operation'].split(':', 1)[0] in LOAD_OPERATIONS + STAGE_TRANSACTIONS
            and (item.get('applied') is not True or staged.get('replacement_accepted') is False))


def unexpected_errors(item):
    """Only the refusal a deliberate refusal case provoked is expected; an accepted load stays strict."""
    exempt = refused_load(item)
    return [line for line in item['errors'] if not exempt or CRASH_ERROR.search(line)]


def gate(item, expect_load, exe_hash):
    """The recorded outcome of the transition, not just a clean process."""
    base = item['operation'].split(':', 1)[0]
    must_apply = base in APPLYING_OPERATIONS or (base in LOAD_OPERATIONS and expect_load == 'accepted')
    return [name for name, passed in (
        ('completed', item['completed']),
        ('process_clean', item['process_clean']),
        ('desktop_unchanged', item['desktop_unchanged']),
        ('binary', item['binary'] == exe_hash),
        ('applied', item.get('applied') is True or not must_apply),
        ('errors', not unexpected_errors(item)),
        ('graph_capture', not item['graph_capture_problems']),
        ('graphs_serialized', all(observation['serialized'] and observation['problem_count'] == 0
                                  for observation in item['graph_observations']))) if not passed]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--replay', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--operations', nargs='+', default=['observe', 'save', 'stage', 'file', 'memory', 'hold', 'preview'])
    parser.add_argument('--ticks', type=int, nargs='+', default=[50])
    parser.add_argument('--script', type=Path)
    parser.add_argument('--global-script', type=Path)
    parser.add_argument('--late', action='store_true')
    parser.add_argument('--snapshots', type=Path, nargs='*', default=[])
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--build-manifest', type=Path, help='Immutable compiled-source and executable manifest, allowing unrelated source development during this run')
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--variant-env')
    parser.add_argument('--variants', nargs='+', default=[''])
    parser.add_argument('--continue-through', type=int, help='continue to this complete tick after the operation; also run a valid reference')
    parser.add_argument('--reference-operation', default='observe', help='operation for each independent continuation reference')
    parser.add_argument('--expect-load', choices=['any', 'accepted', 'rejected'], default='any')
    parser.add_argument('--continuation-perturb', action='store_true', help='positive control: one extra simulation RNG draw after the observed operation')
    parser.add_argument('--expect-continuation-difference', action='store_true', help='require the positive control to produce a detected post-operation trace divergence')
    parser.add_argument('--seed-marker', type=int, default=0, help='put a distinct test marker in all Lua roots before saving a candidate')
    parser.add_argument('--require-marker', type=int, help='require the candidate marker in every VM after loading and at final continuation')
    parser.add_argument('--timeout', type=int, default=180)
    options = parser.parse_args()
    if options.continue_through is not None:
        if options.continue_through <= max(options.ticks): parser.error('--continue-through must be greater than every operation tick')
        allowed = lambda operation: operation == 'observe' or operation.startswith(('ordinary-load:', 'stage-reference:', 'stage-reject:', 'stage-ordinary-reject:'))
        if not all(allowed(operation) for operation in [*options.operations, options.reference_operation]):
            parser.error('continuation requires observe, ordinary-load, or stage transaction operations')
    elif options.continuation_perturb or options.expect_continuation_difference:
        parser.error('continuation controls require --continue-through')
    if options.expect_continuation_difference and not options.continuation_perturb:
        parser.error('--expect-continuation-difference requires --continuation-perturb')
    if options.seed_marker and options.continue_through is not None:
        parser.error('seed-marker is only for a separate seed capture, not continuation cases')
    options.out.mkdir(parents=True, exist_ok=False)
    shutil.copy2(__file__, options.out / 'harness_source.py')
    build = json.loads(options.build_manifest.read_text()) if options.build_manifest else None
    build_files = dict(build['artifacts']) if build else {}
    if build:
        build_files[str(options.build_manifest.resolve())] = digest(options.build_manifest)
        assert all(digest(Path(name)) == value for name, value in build_files.items()), 'compiled source artifacts changed'
        assert digest(repo / 'Cortex Command.exe') == build['exe_sha256'], 'executable differs from compiled-source manifest'
        shutil.copy2(options.build_manifest, options.out / 'compiled-build.json')
    names = [] if build else subprocess.check_output(['git', 'ls-files', '--modified', '--others', '--exclude-standard', '-z'], cwd=repo).decode().split('\0')
    hashes = {}
    for name in sorted(set(names) - {''}):
        path = repo / name
        if not path.is_file(): continue
        target = options.out / 'source_files' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        hashes[name] = digest(path)
    patch = Path(build['source_patch']).read_bytes() if build else subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=repo, stderr=subprocess.DEVNULL)
    (options.out / 'source.patch').write_bytes(patch)
    exe_hash = digest(repo / 'Cortex Command.exe')
    inputs = {}
    harness_inputs = [Path(__file__), repo / 'tools/run_sim_test.py', repo / 'tools/win32_test_runner.py', repo / 'tools/compare_sim_traces.py',
                      repo / 'tools/compare_snapshots.py', repo / 'tools/snapshot_runtime.py',
                      repo / 'tools/cross_process_state.py']
    for path in [options.replay, options.script, options.global_script, *options.snapshots, *harness_inputs]:
        if path:
            target = options.out / 'fixture_sources' / path.name
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(path, target)
            inputs[str(path.resolve())] = digest(path)
    (options.out / 'provenance.json').write_text(json.dumps({'utc': datetime.now(timezone.utc).isoformat(),
        'head': build['head'] if build else subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
        'source_basis': str(options.build_manifest.resolve()) if build else 'live source held unchanged for the run',
        'source_patch_sha256': hashlib.sha256(patch).hexdigest(), 'files': hashes, 'binary': exe_hash,
        'inputs': inputs, 'options': {key: str(value) for key, value in vars(options).items()}, 'status': 'AUDIT OBSERVATIONS, differences require classification'}, indent=2))

    def unchanged_inputs():
        return (all((repo / name).exists() and digest(repo / name) == value for name, value in hashes.items())
                and digest(repo / 'Cortex Command.exe') == exe_hash
                and all(Path(name).exists() and digest(Path(name)) == value for name, value in {**inputs, **build_files}.items()))

    def run_case(case):
        operation, tick, variant, reference = case
        assert unchanged_inputs(), 'audit inputs changed before case'
        label = ('reference_' if reference else '') + operation.replace(':', '_') + '_' + str(tick) + ('_' + variant if variant else '')
        out = options.out / label
        trace = out / 'trace.json'
        args = ['-net-replay', str(options.replay.resolve()), '-num-lua-states', 4,
            '-contract-audit', operation, '-contract-audit-tick', tick,
            '-max-ticks', options.continue_through or tick + 5, '-out', trace]
        if options.continue_through:
            args += ['-tick-hashes', '-contract-audit-continue-through', options.continue_through]
            if options.continuation_perturb and not reference: args += ['-contract-audit-continuation-perturb']
        if options.seed_marker: args += ['-contract-audit-seed-marker', options.seed_marker]
        if options.script: args += ['-test-script', 'UserScenes.rte/ScriptState/' + options.script.name]
        environment = {'CC_TEST_ASYNC_PATH_PUBLICATION_TICK': '150', **dict(item.split('=', 1) for item in options.env)}
        if options.continue_through: environment['CC_SIM_DUMP'] = f'{options.continue_through - 1}:{options.continue_through - 1}'
        if variant:
            assert options.variant_env
            environment[options.variant_env] = variant
        run = make_run(repo, args, out, options.timeout, environment)
        module = Path(run.cwd) / 'Userdata/UserScenes.rte'
        (module / 'ScriptState').mkdir(parents=True, exist_ok=True)
        index = 'DataModule\n\tModuleName = User Scenes\n\tIgnoreMissingItems = 1\n'
        if options.script:
            shutil.copy2(options.script, module / 'ScriptState' / options.script.name)
        if options.global_script:
            shutil.copy2(options.global_script, module / 'ScriptState' / options.global_script.name)
            index += '\tAddGlobalScript = GlobalScript\n\t\tPresetName = Checkpoint Global\n'
            index += f'\t\tScriptPath = UserScenes.rte/ScriptState/{options.global_script.name}\n\t\tLuaClassName = CheckpointGlobalScript\n'
            if options.late: index += '\t\tLateUpdate = 1\n'
            with (Path(run.cwd) / 'Userdata/Settings.ini').open('a') as stream:
                stream.write('\n\tEnableGlobalScript = UserScenes.rte/Checkpoint Global\n')
        (module / 'Index.ini').write_text(index)
        if options.snapshots:
            saves = Path(run.cwd) / 'Userdata/UserSavedGames.rte'
            saves.mkdir(exist_ok=True)
            (saves / 'Index.ini').write_text('DataModule\n\tModuleName = Scripted Activity Saves\n')
            for snapshot in options.snapshots: shutil.copy2(snapshot, saves / snapshot.name)
        try:
            record = run.start().finish()
        finally:
            run.close()
        assert unchanged_inputs(), 'audit inputs changed during case'
        log = (out / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
        for console in (Path(run.cwd) / 'LogConsole.txt', Path(str(trace) + '.console.txt')):
            if console.exists(): log += '\n' + console.read_text(encoding='utf-8-sig', errors='replace')
        errors = re.findall(r'^.*(?:ERROR:|RTE Assert|RTE Abort|stack traceback|\[scriptgraph\].*failed).*$', log, re.M)
        match = re.search(r'\[contract-audit\] complete operation=(\S+) prepared=(\d+) applied=(\d+) differences=(\d+)', log)
        result = {'case': label, 'out': str(out), 'operation': operation, 'tick': tick, 'reference': reference,
            'completed': bool(match), 'process_clean': record['exit_code'] == 0 and not record['timed_out'],
            'desktop_unchanged': record['input_desktop_before'] == record['input_desktop_after'], 'binary': record['exe_sha256'], 'errors': errors}
        if variant: result['variant'] = variant
        result['native_checks'] = [{'observation': item[0], 'owners': int(item[1]), 'checked': int(item[2]), 'mismatches': int(item[3])}
            for item in re.findall(r'\[native-contract-check\] (\S+) owners=(\d+) checked=(\d+) mismatches=(\d+)', log)]
        result['native_mismatches'] = list(dict.fromkeys(re.findall(r'^.*\[native-contract-mismatch\].*$', log, re.M)))
        result['contract_checks'] = [{'family': item[0], 'observation': item[1], 'checked': int(item[2]), 'mismatches': int(item[3])}
            for item in dict.fromkeys(re.findall(r'\[(activity|reference)-contract-check\] (\S+) checked=(\d+) mismatches=(\d+)', log))]
        result['contract_mismatches'] = list(dict.fromkeys(re.findall(r'^.*\[(?:activity|reference)-contract-mismatch\].*$', log, re.M)))
        result['contract_mismatches_by_observation'] = {}
        for item in result['contract_checks']:
            stage = item['observation']
            result['contract_mismatches_by_observation'][stage] = result['contract_mismatches_by_observation'].get(stage, 0) + item['mismatches']
        if operation == 'memory-perturb':
            counts = result['contract_mismatches_by_observation']
            result['perturbation_detected'] = counts.get('perturbed', 0) > 0
            result['restored_contract_checks_passed'] = 'after' in counts and counts['after'] == 0
        result['graph_observations'] = [{'observation': item[0], 'serialized': bool(int(item[1])), 'problem_count': int(item[2])}
            for item in re.findall(r'\[contract-audit\] observation=(\S+) fields=\d+ graph=(\d+) problems=(\d+)', log)]
        result['graph_capture_problems'] = re.findall(r'^.*(?:save snapshot refused|scriptgraph.*refused|unsupported userdata).*$' , log, re.M)
        if match:
            result.update(prepared=bool(int(match[2])), applied=bool(int(match[3])), raw_field_differences=int(match[4]))
        stage = re.search(r'\[contract-audit-stage\] valid_staged=(\d+) replacement_attempted=(\d+) replacement_accepted=(\d+) retained_launched=(\d+) entry=(\S+) tick_before=(\d+) tick_after=(\d+)', log)
        if stage:
            result['staged_candidate'] = dict(zip(('valid_staged', 'replacement_attempted', 'replacement_accepted', 'retained_launched'), map(lambda value: bool(int(value)), stage.groups()[:4])))
            result['staged_candidate'].update(entry=stage[5], tick_before=int(stage[6]), tick_after=int(stage[7]))
        pending = re.search(r'\[contract-audit-pending\] differences=(\d+) before_fields=(\d+) after_fields=(\d+)', log)
        if pending: result['pending_observations'] = dict(zip(('differences', 'before_fields', 'after_fields'), map(int, pending.groups())))
        result['candidate_markers'] = [{'observation': stage, 'vm': int(vm), 'value': value}
            for stage, vm, value in dict.fromkeys(re.findall(r'\[contract-audit-marker\] observation=(\S+) vm=(\d+) value=(\S+)', log))]
        if options.continue_through:
            final = re.search(r'\[contract-audit-continuation\] completed_tick=(\d+) requested_tick=(\d+)', log)
            trace_valid, trace_detail = strict_compare(trace, trace, options.continue_through)
            checks = {'completed_tick': bool(final) and int(final[1]) == options.continue_through and int(final[2]) == options.continue_through,
                      'trace_valid': trace_valid, 'no_tick_repair': 'invalid_tick_change=1' not in log,
                      'dump_exists': Path(str(trace) + '.simdump.txt').is_file(),
                      'before_graph_valid': any(item['observation'] == 'before' and item['serialized'] and item['problem_count'] == 0 for item in result['graph_observations']),
                      'continued_graph_valid': any(item['observation'] == 'continued' and item['serialized'] and item['problem_count'] == 0 for item in result['graph_observations'])}
            checkpoint_errors = Path(run.cwd) / 'Userdata/CheckpointErrors.txt'
            checks['no_fixture_errors'] = not checkpoint_errors.exists() or not checkpoint_errors.read_text(errors='replace').strip()
            if reference:
                checks['reference_no_errors'] = not errors
                checks['reference_applied'] = result.get('applied') is True
            elif options.expect_load != 'any':
                if stage:
                    checks['expected_load_return'] = (not result['staged_candidate']['replacement_accepted']) == (options.expect_load == 'rejected')
                    checks['valid_candidate_staged'] = result['staged_candidate']['valid_staged']
                    checks['retained_candidate_launched'] = result['staged_candidate']['retained_launched']
                else:
                    checks['expected_load_return'] = result.get('applied') is (options.expect_load == 'accepted')
            if options.require_marker is not None:
                for observation in ('after', 'continued'):
                    values = {item['vm']: item['value'] for item in result['candidate_markers'] if item['observation'] == observation}
                    checks[observation + '_candidate_marker'] = values == {index: str(options.require_marker) for index in range(5)}
            result['continuation'] = {'through_tick': options.continue_through, 'checks': checks,
                                      'trace_detail': trace_detail, 'deliberate_rng_draw': 'deliberate_rng_draw=1' in log,
                                      'status': 'REFERENCE_PENDING' if reference else 'COMPARISON_PENDING'}
        native_fixture = re.search(r'\[native-contract-fixture\] constructed=(\d+) distinct_property_cases=(\d+) gaps=(\d+)', log)
        if options.script and options.script.name == 'mod_native_contracts.lua':
            result['native_fixture'] = dict(zip(('constructed', 'distinct_property_cases', 'gaps'), map(int, native_fixture.groups()))) if native_fixture else None
        for path in list(out.glob('trace.json.contract.*.state.txt')) + list(out.glob('trace.json.contract.*.gaps.txt')):
            assert path.resolve().parent == out.resolve()
            original_hash = digest(path)
            compressed = path.with_suffix(path.suffix + '.gz')
            with path.open('rb') as source, gzip.open(compressed, 'wb', compresslevel=3) as target:
                shutil.copyfileobj(source, target)
            with gzip.open(compressed, 'rb') as source:
                assert hashlib.file_digest(source, 'sha256').hexdigest() == original_hash
            path.unlink()
        (out / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'case': label, **{k:v for k,v in result.items() if k not in ('errors', 'graph_capture_problems', 'native_mismatches')},
            'error_count': len(result['errors']), 'first_error': result['errors'][0] if result['errors'] else None,
            'native_mismatch_count': len(result['native_mismatches'])}), flush=True)
        return result

    references = {}
    if options.continue_through:
        for tick in options.ticks:
            for variant in options.variants:
                reference = run_case((options.reference_operation, tick, variant, True))
                reference['continuation']['status'] = 'VALID_REFERENCE' if all(reference['continuation']['checks'].values()) and reference['completed'] and reference['process_clean'] and reference['desktop_unchanged'] else 'INVALID_REFERENCE'
                references[tick, variant] = reference
                (Path(reference['out']) / 'result.json').write_text(json.dumps(reference, indent=2))
        if any(item['continuation']['status'] != 'VALID_REFERENCE' for item in references.values()):
            (options.out / 'result.json').write_text(json.dumps({'status': 'INVALID_REFERENCE', 'complete': False, 'references': list(references.values()), 'results': []}, indent=2))
            return 1
    cases = [(operation, tick, variant, False) for operation in options.operations for tick in options.ticks for variant in options.variants]
    with ThreadPoolExecutor(max_workers=options.jobs) as pool:
        results = list(pool.map(run_case, cases))
    if options.continue_through:
        for result in results:
            reference = references[result['tick'], result.get('variant', '')]
            out, ref = Path(result['out']), Path(reference['out'])
            continuation = result['continuation']
            checks = continuation['checks']
            equal, detail = strict_compare(ref / 'trace.json', out / 'trace.json', options.continue_through)
            continuation['reference_trace'] = detail
            checks['same_binary'] = result['binary'] == reference['binary'] == exe_hash
            expected_fault = options.expect_continuation_difference
            if expected_fault:
                prefix_equal, prefix = strict_compare(ref / 'trace.json', out / 'trace.json', result['tick'], prefix=True)
                continuation['reference_prefix'] = prefix
                checks['positive_control_detected'] = (continuation['deliberate_rng_draw'] and prefix_equal and not equal
                    and detail.get('first_divergence') is not None and detail['first_divergence'] > result['tick'])
            else:
                checks['reference_trace'] = equal
                first_dump, second_dump = ref / 'trace.json.simdump.txt', out / 'trace.json.simdump.txt'
                checks['reference_dump'] = first_dump.exists() and second_dump.exists() and digest(first_dump) == digest(second_dump)
            # The reference is a separate process, so its real-time anchors are its own unless a load restored them.
            graphs = compare_lua_observations(ref, 'continued', out, 'continued', cross_process=True)
            (out / 'continuation-lua-comparison.json').write_text(json.dumps(graphs, indent=2))
            if not expected_fault: checks['reference_full_lua'] = graphs['passed']
            try:
                continuation['reference_raw'] = compare_state_files(ref / 'trace.json.contract.continued.state.txt.gz',
                    out / 'trace.json.contract.continued.state.txt.gz', out / 'continuation-raw-differences.jsonl.gz', cross_process=True)
            except Exception as error:
                continuation['reference_raw'] = {'error': str(error)}
                checks['reference_raw_available'] = False
            if options.expect_load == 'rejected':
                stage_transaction = result['operation'].startswith(('stage-reject:', 'stage-ordinary-reject:'))
                first_stage, second_stage = ('staged_before', 'staged_after') if stage_transaction else ('before', 'after')
                same_lua = compare_lua_observations(out, first_stage, out, second_stage)
                (out / 'refusal-lua-comparison.json').write_text(json.dumps(same_lua, indent=2))
                checks['refusal_full_lua'] = same_lua['passed']
                first_identity = out / f'trace.json.contract.{first_stage}.identity.txt'
                second_identity = out / f'trace.json.contract.{second_stage}.identity.txt'
                checks['refusal_original_identities'] = first_identity.exists() and second_identity.exists() and digest(first_identity) == digest(second_identity)
                try:
                    raw = compare_state_files(out / f'trace.json.contract.{first_stage}.state.txt.gz',
                        out / f'trace.json.contract.{second_stage}.state.txt.gz', out / 'refusal-raw-differences.jsonl.gz')
                    continuation['refusal_raw'] = raw
                    checks['refusal_direct_state'] = raw['equal']
                except Exception as error:
                    continuation['refusal_raw'] = {'error': str(error)}
                    checks['refusal_direct_state'] = False
                if stage_transaction:
                    checks['pending_candidate_direct_state'] = result.get('pending_observations', {}).get('differences') == 0
            checks.update(process_clean=result['process_clean'], completed=result['completed'], desktop_unchanged=result['desktop_unchanged'])
            continuation['passed'] = all(checks.values())
            continuation['status'] = ('POSITIVE_CONTROL_PASS' if expected_fault else 'CONTINUATION_AND_REFUSAL_CHECKS_PASS') if continuation['passed'] else 'FAILED_CHECKS_REQUIRE_CLASSIFICATION'
            continuation['scope'] = 'Full trace, final native simulation dump and five complete Lua graphs; unfiltered cross-run raw fields retained for separate classification.'
            (out / 'result.json').write_text(json.dumps(result, indent=2))
            print(json.dumps({'case': result['case'], 'continuation_status': continuation['status'], 'failed_checks': [key for key, passed in checks.items() if not passed]}), flush=True)

    for item in results:
        item['gate_failures'] = gate(item, options.expect_load, exe_hash)
        (Path(item['out']) / 'result.json').write_text(json.dumps(item, indent=2))
    unchanged = unchanged_inputs()
    complete = unchanged and not any(item['gate_failures'] for item in results)
    continuation_passed = all(item['continuation']['passed'] for item in results) if options.continue_through else None
    verdict = {'rule': 'Every case completes on the pinned binary with a clean process and desktop, applies the transitions expected to apply, '
                       'logs no engine error outside a deliberate refusal, and serialises all five Lua graphs.',
               'failed_cases': [{'case': item['case'], 'operation': item['operation'], 'gate_failures': item['gate_failures'],
                                 'applied': item.get('applied'), 'errors': item['errors']} for item in results if item['gate_failures']],
               'refused_transitions': [item['case'] for item in results if item.get('applied') is False],
               'error_cases': {item['case']: item['errors'] for item in results if item['errors']},
               'unexpected_error_cases': {item['case']: unexpected_errors(item) for item in results if unexpected_errors(item)},
               'graph_problem_cases': {item['case']: item['graph_capture_problems'] + [observation for observation in item['graph_observations']
                                                                                       if not observation['serialized'] or observation['problem_count']]
                                       for item in results if item['graph_capture_problems']
                                       or any(not observation['serialized'] or observation['problem_count'] for observation in item['graph_observations'])},
               'raw_field_differences': {item['case']: item.get('raw_field_differences') for item in results},
               'raw_field_differences_total': sum(item.get('raw_field_differences') or 0 for item in results),
               'raw_note': 'Raw field differences are recorded, never gated: they require the retained classifiers.'}
    (options.out / 'result.json').write_text(json.dumps({'status': 'AUDIT OBSERVATIONS, not a complete fidelity pass', 'complete': complete,
        'source_unchanged': unchanged, 'continuation_checks_passed': continuation_passed, 'verdict': verdict,
        'references': list(references.values()), 'results': results}, indent=2))
    return 0 if complete and continuation_passed is not False else 1

if __name__ == '__main__': raise SystemExit(main())
