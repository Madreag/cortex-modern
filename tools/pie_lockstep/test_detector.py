"""Mutate complete synthetic rows and require the production detector to reject them."""
import argparse
import copy
import json
from pathlib import Path
import re

import verify_pie_close as verify


def replace_field(line, name, value):
    return re.sub(r'(?<= )' + name + r'=\S+', name + '=' + value, line, count=1)


def successful_metadata(root, case='next', sp=False, observe=False, mutation=''):
    peers = ['sp'] if sp else ['host', 'client']
    executable = str(root / 'synthetic-fixture.exe')
    manifest = dict(synthetic=True, case=case, sp=sp, input_delay=0 if sp else 3,
                    observe=observe, exe=executable, exe_sha256='0' * 64, port=47921)
    (root / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
    results = {}
    for index, peer in enumerate(peers):
        target = root / peer
        target.mkdir(exist_ok=True)
        trace = str(root / (peer + '_trace.json'))
        argv = [executable, '-headless', '-max-ticks', '320', '-tick-hashes', '-out', trace]
        argv += ['-scenario', 'PieSwitchSP'] if sp else [
            '-net-match-service-e2e', '-net-match-ticks', '320', '-net-match-input-delay', '3', '-net-port', '47921']
        if not sp:
            argv += ['-net-host'] if peer == 'host' else ['-net-join', '127.0.0.1']
        checks = ['exe_exists', 'exe_sha256_recorded', 'cwd_exists', 'data_junction_resolves',
                  'userdata_writable', 'out_dir_private_writable'] + ([] if sp else ['firewall_allow_rule_present'])
        success = dict(pid=100 + index, exit_code=0, timed_out=False, evidence_complete=True)
        results[peer] = dict(success)
        launch = dict(success, synthetic=True, runner='synthetic-fixture', started=True, job_closed=True,
                      argv=argv, exe_path=executable, exe_sha256='0' * 64, headless_env='1',
                      env_set={'CCCP_HEADLESS': '1', 'CC_SIM_DUMP': '27:320'},
                      evidence_expected=[trace, trace + '.simdump.txt'], evidence_present=[trace, trace + '.simdump.txt'],
                      evidence_missing=[], startup_checks=[dict(check=name, ok=True) for name in checks])
        if mutation == 'launch_exit':
            launch['exit_code'] = 7
        elif mutation == 'result_exit':
            results[peer]['exit_code'] = 7
        elif mutation == 'launch_incomplete':
            launch['evidence_complete'] = False
        elif mutation == 'launch_missing_success':
            del launch['exit_code']
        elif mutation == 'launch_missing_artifact':
            launch['evidence_expected'].pop()
            launch['evidence_present'].pop()
        elif mutation == 'launch_failed_startup':
            launch['startup_checks'][0]['ok'] = False
        if mutation != 'missing_launch' or peer != 'client':
            (target / 'launch.json').write_text(json.dumps(launch), encoding='utf-8')
    if mutation != 'missing_result':
        (root / 'run_result.json').write_text(json.dumps(results), encoding='utf-8')


def evaluate(root, case, expected, reason='', reference=None, allow_synthetic=True):
    try:
        result = verify.check(root, case, reference=reference, allow_synthetic=allow_synthetic)
    except (ValueError, KeyError, OSError, TypeError) as exc:
        result = dict(pass_check=False, failures=[str(exc)])
    (root / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    intended = not reason or any(reason in failure for failure in result['failures'])
    return dict(mutation=root.name, pass_check=result['pass_check'], expected=expected,
                intended_reason=reason, intended_reason_found=intended, reasons=result['failures'][:3])


def cancellation_mutations(source_root, out):
    results = []
    for case in verify.CANCEL_CASES:
        stem = case.replace('_', '-')
        source = source_root / ('net-' + stem + '-tip')
        sp_source = source_root / ('sp-' + stem + '-control-parity')
        reference = out / (case + '-reference')
        reference.mkdir()
        successful_metadata(reference, case, sp=True, observe=True)
        (reference / 'sp/runtime').mkdir()
        for name in ('sp_trace.json', 'sp_trace.json.simdump.txt'):
            (reference / name).write_bytes((sp_source / name).read_bytes())
        for name in ('stdout.log', 'runtime/LogConsole.txt'):
            (reference / 'sp' / name).write_bytes((sp_source / 'sp' / name).read_bytes())
        _, sp_actors, _, _ = verify.load_dump(reference / 'sp_trace.json.simdump.txt')
        sp_obs, errors = verify.load_observations((reference / 'sp/runtime/LogConsole.txt').read_text(), sp_actors, True)
        if errors:
            raise ValueError(errors)
        base_lines = []
        for line in (source / 'client_trace.json.simdump.txt').read_text().splitlines():
            match = verify.HEAD.fullmatch(line)
            if match:
                tick, uid, row = int(match[1]), int(match[2]), verify.fields(match[4])
                if int(row['mode']) == 2:
                    line = replace_field(line, 'ctrl', hex(int(row['ctrl'], 16) & ~verify.PLAYER_BITS))
                if uid == 1048736 and tick >= 199:
                    line = replace_field(line, 'pie', sp_actors[1048587][tick - 3]['pie'])
                base_lines.append(line)
            elif ' activity ' in line:
                base_lines.append(line)
        console = (source / 'host/runtime/LogConsole.txt').read_text()
        def shared_observation(match):
            uid, step = int(match[1]), int(match[2])
            if uid != 1048736 or step + 22 < 199:
                return match[0]
            tick = step + 22 - 3
            mode, state, callbacks, changes = sp_obs[(1048587, tick)]
            if case == 'delivery_cancel':
                changes = 2 if tick < 199 else 3
            return f'[pie-observe] uid={uid} step={step} mode={mode} state={state} callbacks={callbacks} modechanges={changes}'
        console = verify.OBSERVE.sub(shared_observation, console)
        for mutation in ('baseline', 'post_cancel_at_commit', 'post_cancel_at_end',
                         'post_cancel_missing_animation', 'post_cancel_callback_count',
                         'missing_reference', 'reference_failed_launch', 'missing_script_field'):
            root = out / (case + '-' + mutation)
            root.mkdir()
            successful_metadata(root, case, observe=True)
            lines = list(base_lines)
            altered_console = console
            if mutation == 'post_cancel_callback_count':
                altered_console = re.sub(r'(\[pie-observe\] uid=1048736 step=180 .*?modechanges=)\d+',
                                         lambda match: match[1] + '9', console)
                if altered_console == console:
                    raise ValueError('callback mutation does not change its target')
            elif mutation == 'missing_script_field':
                index = next(i for i, line in enumerate(lines) if line.startswith('202 actor uid=1048736 '))
                lines[index] = re.sub(r' script=\S+', '', lines[index])
            elif mutation.startswith('post_cancel'):
                changed_ticks = [202] if mutation == 'post_cancel_at_commit' else [320] if mutation == 'post_cancel_at_end' else range(202, 321)
                for tick in changed_ticks:
                    index = next(i for i, line in enumerate(lines) if line.startswith(f'{tick} actor uid=1048736 '))
                    wrong = '0:8:-:-:-' if mutation == 'post_cancel_at_end' else '3:8:-:-:-'
                    if mutation != 'post_cancel_missing_animation' and verify.fields(lines[index])['pie'] == wrong:
                        raise ValueError('mutation does not change its target')
                    lines[index] = replace_field(lines[index], 'pie', wrong)
                    step = tick - 22
                    altered_console = re.sub(rf'(\[pie-observe\] uid=1048736 step={step} mode=\d+ state=)[01]{{6}}',
                                             lambda match: match[1] + ('111110' if mutation == 'post_cancel_at_end' else '000000'), altered_console)
            for peer in ('host', 'client'):
                (root / (peer + '_trace.json.simdump.txt')).write_text('\n'.join(lines) + '\n', encoding='utf-8')
                (root / (peer + '_trace.json')).write_bytes((source / 'host_trace.json').read_bytes())
                (root / peer / 'runtime').mkdir()
                (root / peer / 'runtime/LogConsole.txt').write_text(altered_console, encoding='utf-8')
                (root / peer / 'stdout.log').write_bytes((source / 'host/stdout.log').read_bytes())
            chosen_reference = None if mutation == 'missing_reference' else reference
            if mutation == 'reference_failed_launch':
                chosen_reference = root / 'reference'
                chosen_reference.mkdir()
                successful_metadata(chosen_reference, case, sp=True, observe=True, mutation='launch_exit')
                (chosen_reference / 'sp/runtime').mkdir()
                for name in ('sp_trace.json', 'sp_trace.json.simdump.txt', 'sp/stdout.log', 'sp/runtime/LogConsole.txt'):
                    (chosen_reference / name).write_bytes((reference / name).read_bytes())
            reason = {'post_cancel_at_commit': 'tick 202 pie 3', 'post_cancel_at_end': 'tick 320 pie 0',
                      'post_cancel_missing_animation': 'tick 202 pie 3', 'missing_reference': 'requires a successful SP reference',
                      'post_cancel_callback_count': 'tick 202 script',
                      'missing_script_field': "missing fields ['script']",
                      'reference_failed_launch': 'SP reference sp launch exit_code=7'}.get(mutation, '')
            results.append(evaluate(root, case, mutation == 'baseline', reason, chosen_reference))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--cancel-root', type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    lines = []
    for line in (args.source / 'host_trace.json.simdump.txt').read_text().splitlines():
        if ' activity ' in line:
            lines.append(line)
        elif match := verify.HEAD.fullmatch(line):
            tick, uid, row = int(match[1]), int(match[2]), verify.fields(match[4])
            if int(row['mode']) == 2:
                line = replace_field(line, 'ctrl', hex(int(row['ctrl'], 16) & ~verify.PLAYER_BITS))
            if uid == 1048736 and tick >= 205:
                line = replace_field(line, 'pie', '3:8:-:-:-')
            lines.append(line)
    trace = json.loads((args.source / 'host_trace.json').read_text(encoding='utf-8-sig'))
    stdout = '[input-script] tick 30 player 0 pressed NEXT\n[input-script] tick 150 player 0 pressed PIEMENU_DIGITAL\n[input-script] tick 199 player 0 pressed NEXT\n'
    mutations = ['baseline', 'post_handoff_pie', 'replacement_seat', 'handoff_tick', 'activity_row',
                 'activity_missing_field', 'actor_missing_field', 'missing_actor_row', 'duplicate_actor',
                 'missing_replacement_edge', 'ai_player_bits', 'brain_next_prep', 'malformed_ctrl',
                 'one_peer_raw_change', 'hash_drop', 'hash_duplicate', 'hash_missing_field',
                 'hash_controller_diff', 'hash_terrain_diff', 'secondary_action_only',
                 'missing_launch', 'launch_exit', 'result_exit', 'launch_incomplete',
                 'launch_missing_success', 'launch_missing_artifact', 'launch_failed_startup',
                 'missing_result', 'synthetic_requires_opt_in', 'gun_missing_field']
    reasons = {'post_handoff_pie': 'tick 320 pie 0', 'replacement_seat': 'replacement seat',
               'handoff_tick': 'handoffs [205]', 'activity_row': 'activity state 5',
               'secondary_action_only': 'player bits 0x2', 'missing_launch': 'launch.json',
               'launch_exit': 'launch exit_code=7', 'result_exit': 'result exit_code=7',
               'launch_incomplete': 'launch evidence_complete=False', 'launch_missing_success': 'launch exit_code=None',
               'launch_missing_artifact': 'evidence_expected', 'launch_failed_startup': 'startup gates',
               'missing_result': 'run_result.json', 'synthetic_requires_opt_in': 'unit-test opt-in'}
    reasons['gun_missing_field'] = "missing fields ['rounds']"
    results = []
    for mutation in mutations:
        root = args.out / mutation
        root.mkdir()
        host = list(lines)
        client = list(lines)
        ht, ct = copy.deepcopy(trace), copy.deepcopy(trace)
        def change(prefix, key, value):
            for peer in (host, client):
                index = next(i for i, line in enumerate(peer) if line.startswith(prefix))
                peer[index] = replace_field(peer[index], key, value)
        if mutation == 'post_handoff_pie':
            change('320 actor uid=1048736 ', 'pie', '0:8:-:-:-')
        elif mutation == 'replacement_seat':
            change('204 actor uid=1048615 ', 'mode', '2')
        elif mutation == 'handoff_tick':
            change('204 actor uid=1048736 ', 'mode', '1')
            change('204 actor uid=1048615 ', 'mode', '2')
        elif mutation == 'activity_row':
            change('210 activity ', 'state', '5')
        elif mutation == 'activity_missing_field':
            for peer in (host, client):
                index = next(i for i, line in enumerate(peer) if line.startswith('210 activity '))
                peer[index] = re.sub(r' t2=\S+', '', peer[index])
        elif mutation == 'actor_missing_field':
            for peer in (host, client):
                index = next(i for i, line in enumerate(peer) if line.startswith('210 actor uid=1048736 '))
                peer[index] = re.sub(r' health=\S+', '', peer[index])
        elif mutation == 'gun_missing_field':
            for peer in (host, client):
                index = next(i for i, line in enumerate(peer) if line.startswith('210 actor uid=1048577 '))
                peer[index] = re.sub(r' rounds=\S+', '', peer[index])
        elif mutation == 'missing_actor_row':
            host = client = [line for line in host if not line.startswith('309 actor uid=1048736 ')]
        elif mutation == 'duplicate_actor':
            client.append(next(line for line in client if line.startswith('201 actor uid=1048736 ')))
        elif mutation == 'missing_replacement_edge':
            change('204 actor uid=1048615 ', 'ctrl', '0x580002')
        elif mutation == 'ai_player_bits':
            change('309 actor uid=1048736 ', 'ctrl', '0x500006')
        elif mutation == 'brain_next_prep':
            change('309 actor uid=1048577 ', 'ctrl', '0x4000000000')
        elif mutation == 'secondary_action_only':
            change('309 actor uid=1048736 ', 'ctrl', '0x2')
        elif mutation == 'malformed_ctrl':
            change('309 actor uid=1048736 ', 'ctrl', 'garbage')
        elif mutation == 'one_peer_raw_change':
            client[0] += ' extra=1'
        elif mutation.startswith('hash_'):
            rows = ct['runs'][0]['tick_hashes']
            if mutation == 'hash_drop':
                rows.pop()
            elif mutation == 'hash_duplicate':
                rows.append(rows[-1])
            elif mutation == 'hash_missing_field':
                del rows[209]['subsystems']['controller']
                ht = copy.deepcopy(ct)
            else:
                rows[209]['subsystems'][mutation.split('_')[1]] = 'f' * 64
        for peer, dump, data in (('host', host, ht), ('client', client, ct)):
            (root / (peer + '_trace.json.simdump.txt')).write_text('\n'.join(dump) + '\n', encoding='utf-8')
            (root / (peer + '_trace.json')).write_text(json.dumps(data), encoding='utf-8')
            (root / (peer + '.out.txt')).write_text(stdout, encoding='utf-8')
        successful_metadata(root, mutation=mutation)
        results.append(evaluate(root, 'next', mutation == 'baseline', reasons.get(mutation, ''),
                                allow_synthetic=mutation != 'synthetic_requires_opt_in'))
    if args.cancel_root:
        results.extend(cancellation_mutations(args.cancel_root, args.out))
    summary = dict(correct=all(r['pass_check'] == r['expected'] and r['intended_reason_found'] for r in results), results=results)
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(dict(correct=summary['correct'], cases=len(results),
                          positive_controls=sum(r['expected'] for r in results),
                          detecting_mutations=sum(not r['expected'] for r in results),
                          unexpected=[r for r in results if r['pass_check'] != r['expected'] or not r['intended_reason_found']]), indent=2))
    return 0 if summary['correct'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
