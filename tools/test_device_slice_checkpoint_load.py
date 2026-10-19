"""Load a retained device-slice fixture image in a fresh engine and check every value and alias after restoration."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--source', type=Path, required=True, help='Retained fixture runtime containing Autosaves and UserScenes.rte')
a = p.parse_args()
sys.path.insert(0, str(a.repo / 'tools'))
from run_sim_test import make_run, seed_settings, engine_executable, file_sha256

a.out.mkdir(parents=True, exist_ok=False)
(a.out / 'driver-as-run.py').write_bytes(Path(__file__).read_bytes())
saved = min((a.source / 'Autosaves').glob('*.ccsave'), key=lambda path: int(path.stem.rsplit('-', 1)[1]))
run = make_run(a.repo, ['-load-game', 'slice-image', '-checkpoint-restore-continue', '-max-ticks', '120'], a.out / 'load', 180,
               env={'CCCP_HEADLESS': '1', 'CC_RUNNER_IGNORE_FULLSCREEN': '1', 'CC_TEST_MEMORY_CENSUS_TICKS': '1'})
seed_settings(run, {'EnableLuaDebugging': '0', 'AutosaveSeconds': '0', 'ResolutionX': '960', 'ResolutionY': '540',
                    'EnableVSync': '0'})
module = Path(run.cwd) / 'Userdata/UserSavedGames.rte'
module.mkdir(parents=True, exist_ok=True)
(module / 'Index.ini').write_text('DataModule\n\tModuleName = User Saved Games\n')
shutil.copy2(saved, module / 'slice-image.ccsave')
shutil.copytree(a.source / 'Userdata/UserScenes.rte', Path(run.cwd) / 'Userdata/UserScenes.rte')
try:
    record = run.start().finish()
finally:
    run.close()
stdout = (a.out / 'load/stdout.log').read_text(errors='replace')
console = '\n'.join(path.read_text(errors='replace') for path in Path(run.cwd).rglob('LogConsole.txt'))
errors = [line for text in (stdout, console) for line in text.splitlines()
          if re.search(r'FeelBaseline.lua:\d+:|restored device slice lost|image dropped the slice holder|stack traceback|'
                       r'saved game could not be constructed|saved script state did not restore|could not launch the activity', line)]
verified = sorted(set(map(int, re.findall(r'\[r0-slice-fixture\][ _]+verified[ _]+updates=(\d+)[ _]+grenade=5[ _]+aliases=6', stdout))))
continuation = re.search(r'\[checkpoint-restore-continue\] start=(\d+) end=(\d+).*verdict=(true|1)', stdout)
checks = {'normal_exit': record.get('exit_code') == 0 and not record.get('timed_out'),
          'loaded': '[load-game] loaded "slice-image"' in stdout,
          'value_and_alias_checks_executed': len(verified) >= 4 and verified[-1] - verified[0] >= 90,
          'continued_120_ticks': bool(continuation and int(continuation[2]) - int(continuation[1]) == 120),
          'no_fixture_or_restore_error': not errors}
result = {'source': str(saved), 'source_sha256': hashlib.sha256(saved.read_bytes()).hexdigest(),
          'exe_sha256': file_sha256(engine_executable(a.repo)),
          'record': record, 'checks': checks, 'verified_updates': verified, 'errors': errors[:30], 'pass': all(checks.values())}
(a.out / 'summary.json').write_text(json.dumps(result, indent=2))
print(json.dumps({key: value for key, value in result.items() if key not in ('record', 'errors')}))
raise SystemExit(0 if result['pass'] else 1)
