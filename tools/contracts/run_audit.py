"""Record production transitions without repairing engine state after the call."""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
import argparse
import hashlib
import gzip
import json
import re
import shutil
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(repo / 'tools'))
from run_sim_test import make_run

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

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
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--variant-env')
    parser.add_argument('--variants', nargs='+', default=[''])
    options = parser.parse_args()
    options.out.mkdir(parents=True, exist_ok=False)
    shutil.copy2(__file__, options.out / 'harness_source.py')
    names = subprocess.check_output(['git', 'ls-files', '--modified', '--others', '--exclude-standard', '-z'], cwd=repo).decode().split('\0')
    hashes = {}
    for name in sorted(set(names) - {''}):
        path = repo / name
        if not path.is_file(): continue
        target = options.out / 'source_files' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        hashes[name] = digest(path)
    patch = subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=repo, stderr=subprocess.DEVNULL)
    (options.out / 'source.patch').write_bytes(patch)
    exe_hash = digest(repo / 'Cortex Command.exe')
    inputs = {}
    for path in [options.replay, options.script, options.global_script, *options.snapshots]:
        if path:
            target = options.out / 'fixture_sources' / path.name
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(path, target)
            inputs[str(path.resolve())] = digest(path)
    (options.out / 'provenance.json').write_text(json.dumps({'utc': datetime.now(timezone.utc).isoformat(),
        'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
        'source_patch_sha256': hashlib.sha256(patch).hexdigest(), 'files': hashes, 'binary': exe_hash,
        'inputs': inputs, 'options': {key: str(value) for key, value in vars(options).items()}, 'status': 'AUDIT OBSERVATIONS, differences require classification'}, indent=2))

    def run_case(case):
        operation, tick, variant = case
        label = operation.replace(':', '_') + '_' + str(tick) + ('_' + variant if variant else '')
        out = options.out / label
        trace = out / 'trace.json'
        args = ['-net-replay', str(options.replay.resolve()), '-num-lua-states', 4,
            '-contract-audit', operation, '-contract-audit-tick', tick, '-max-ticks', tick + 5, '-out', trace]
        if options.script: args += ['-test-script', 'UserScenes.rte/ScriptState/' + options.script.name]
        environment = {'CC_TEST_ASYNC_PATH_PUBLICATION_TICK': '150', **dict(item.split('=', 1) for item in options.env)}
        if variant:
            assert options.variant_env
            environment[options.variant_env] = variant
        run = make_run(repo, args, out, 120, environment)
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
        log = (out / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
        for console in (Path(run.cwd) / 'LogConsole.txt', Path(str(trace) + '.console.txt')):
            if console.exists(): log += '\n' + console.read_text(encoding='utf-8-sig', errors='replace')
        errors = re.findall(r'^.*(?:ERROR:|RTE Assert|RTE Abort|stack traceback|\[scriptgraph\].*failed).*$', log, re.M)
        match = re.search(r'\[contract-audit\] complete operation=(\S+) prepared=(\d+) applied=(\d+) differences=(\d+)', log)
        result = {'operation': operation, 'tick': tick, 'completed': bool(match), 'process_clean': record['exit_code'] == 0 and not record['timed_out'],
            'desktop_unchanged': record['input_desktop_before'] == record['input_desktop_after'], 'binary': record['exe_sha256'], 'errors': errors}
        if variant: result['variant'] = variant
        result['native_checks'] = [{'observation': item[0], 'owners': int(item[1]), 'checked': int(item[2]), 'mismatches': int(item[3])}
            for item in re.findall(r'\[native-contract-check\] (\S+) owners=(\d+) checked=(\d+) mismatches=(\d+)', log)]
        result['native_mismatches'] = list(dict.fromkeys(re.findall(r'^.*\[native-contract-mismatch\].*$', log, re.M)))
        result['contract_checks'] = [{'family': item[0], 'observation': item[1], 'checked': int(item[2]), 'mismatches': int(item[3])}
            for item in dict.fromkeys(re.findall(r'\[(activity|reference)-contract-check\] (\S+) checked=(\d+) mismatches=(\d+)', log))]
        result['contract_mismatches'] = list(dict.fromkeys(re.findall(r'^.*\[(?:activity|reference)-contract-mismatch\].*$', log, re.M)))
        result['graph_observations'] = [{'observation': item[0], 'serialized': bool(int(item[1])), 'problem_count': int(item[2])}
            for item in re.findall(r'\[contract-audit\] observation=(\S+) fields=\d+ graph=(\d+) problems=(\d+)', log)]
        result['graph_capture_problems'] = re.findall(r'^.*(?:save snapshot refused|scriptgraph.*refused|unsupported userdata).*$' , log, re.M)
        if match:
            result.update(prepared=bool(int(match[2])), applied=bool(int(match[3])), raw_field_differences=int(match[4]))
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

    cases = [(operation, tick, variant) for operation in options.operations for tick in options.ticks for variant in options.variants]
    with ThreadPoolExecutor(max_workers=options.jobs) as pool:
        results = list(pool.map(run_case, cases))
    unchanged = all((repo / name).exists() and digest(repo / name) == value for name, value in hashes.items()) and digest(repo / 'Cortex Command.exe') == exe_hash and all(digest(Path(name)) == value for name, value in inputs.items())
    complete = unchanged and all(item['completed'] and item['process_clean'] and item['desktop_unchanged'] and item['binary'] == exe_hash for item in results)
    (options.out / 'result.json').write_text(json.dumps({'status': 'AUDIT OBSERVATIONS, not a fidelity pass', 'complete': complete, 'source_unchanged': unchanged, 'results': results}, indent=2))
    return 0 if complete else 1

if __name__ == '__main__': raise SystemExit(main())
