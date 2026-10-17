"""Detect unbounded Lua garbage in ordinary single player, without forced collections.

One engine, four states, 1,800 ticks of discarded graphs. After 300 warmup ticks,
every census must have allocated Lua <= 2x the collector's live estimate, and
process private memory must stay within a 128 MiB range. The scenario never
calls collectgarbage and no deterministic/hash flag may select full collection.
"""
import argparse
import json
from pathlib import Path
import re

from run_sim_test import make_run, seed_settings

INDEX = """DataModule
	ModuleName = User Scenes
	ScanFolderContents = 1
	IgnoreMissingItems = 1
	AddActivity = GAScripted
		PresetName = Determinism LuaHeapChurnSP
		SceneName = Grasslands
		ScriptPath = UserScenes.rte/LuaHeapChurnSP.lua
		LuaClassName = LuaHeapChurnSP
		MinTeamsRequired = 1
		IsTestActivity = 1
		DefaultRequireClearPathToOrbit = 0
		DefaultFogOfWar = 0
		DefaultDeployUnits = 0
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    case = args.out / 'churn'
    trace = case / 'trace.json'
    run = make_run(args.repo, ['-scenario', 'LuaHeapChurnSP', '-seed', '42', '-max-ticks', '1800',
                              '-memory-census-ticks', '30', '-out', str(trace)],
                   case, 600, expected=[trace])
    seed_settings(run, {'AutosaveSeconds': '0'})
    module = Path(run.cwd) / 'Userdata/UserScenes.rte'
    module.mkdir(exist_ok=True)
    (module / 'Index.ini').write_text(INDEX, encoding='utf-8')
    (module / 'LuaHeapChurnSP.lua').write_bytes((args.repo / 'tools/fixtures/LuaHeapChurnSP.lua').read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()
    text = (case / 'stdout.log').read_text(encoding='utf-8', errors='replace')
    samples = []
    for line in text.splitlines():
        if line.startswith('[mem-census]'):
            fields = dict(re.findall(r'(\w+)=(\S+)', line))
            samples.append({name: int(fields[name]) for name in
                            ('tick', 'uptime_ms', 'private_mb', 'lua_bytes', 'lua_live_bytes', 'lua_committed_mb')})
    measured = [row for row in samples if row['tick'] > 300]
    ratios = [row['lua_bytes'] / max(1, row['lua_live_bytes']) for row in measured]
    private = [row['private_mb'] for row in measured]
    scenario = json.loads(trace.read_text(encoding='utf-8')) if trace.is_file() else {}
    completed = scenario.get('runs', [])
    checks = {
        'exit_zero': record.get('exit_code') == 0 and not record.get('timed_out'),
        'incremental_collector': '[lua] collector: incremental step at every tick end' in text
                                 and '[lua] collector: full collection' not in text,
        'completed_churn': len(completed) == 1 and completed[0].get('passed') is True
                           and completed[0].get('scenario') == 'LuaHeapChurnSP'
                           and completed[0].get('ticks') == 1800,
        'complete_window': len(measured) >= 49 and measured[-1]['tick'] >= 1770,
        'lua_within_twice_live': bool(ratios) and max(ratios) <= 2,
        'private_range_at_most_128_mib': bool(private) and max(private) - min(private) <= 128,
    }
    summary = {'checks': checks, 'pass': all(checks.values()), 'exe_sha256': record.get('exe_sha256'),
               'exit_code': record.get('exit_code'), 'warmup_ticks': 300, 'measured_ticks': [301, 1800],
               'maximum_lua_ratio': max(ratios, default=None),
               'private_range_mib': max(private) - min(private) if private else None, 'samples': samples}
    (args.out / 'churn-summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps({key: value for key, value in summary.items() if key != 'samples'}), flush=True)
    return 0 if summary['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
