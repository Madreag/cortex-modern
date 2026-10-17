import os,sys,json,time,subprocess,hashlib
from pathlib import Path
root=Path.cwd().resolve();s=Path(__file__).parent
out=s/sys.argv[1];out.mkdir(exist_ok=False)
def active():
 result=[]
 for p in Path('/proc').iterdir():
  if not p.name.isdecimal():continue
  try:
   args=(p/'cmdline').read_bytes().split(b'\0')
   if args and Path(os.fsdecode(args[0])).name in ('ninja','CortexCommand','CortexCommand.exe'):result.append(int(p.name))
  except OSError:pass
 return result
while active():print('Waiting for quiet replay slot',active(),flush=True);time.sleep(10)
binary=root/'build-gcc/CortexCommand';sha=hashlib.sha256(binary.read_bytes()).hexdigest()
assert json.loads((s/'deferred-return-tip-final-validation/summary.json').read_text())['complete']
proof={'source_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'binary_sha256':sha,'scope':'Headed Linux single-box exact schedule safety replays; proof=false for a field/spread match','results':[],'complete':False}
(out/'source.diff').write_text(subprocess.check_output(['git','diff'],cwd=root,text=True))
env={k:v for k,v in os.environ.items() if not k.startswith(('CC_TEST_HOLD_SCHEDULE','CC_TEST_HOLD_FIXED_FAILURE_REPORT','CC_TEST_SESSION_PLANE_CASE','CC_TEST_LOCKSTEP_SELFTEST_CASE','CCCP_TEST_MATCH_CASE','CCCP_HEADLESS'))}
env.update(CC_RUNNER_HEADED='1',DISPLAY=':0',CCCP_TEST_BINARY=str(binary),CC_TEST_HOLD_SCHEDULES='0',CC_TEST_RETURN_INPUT_DETAILS='1')
rows = sys.argv[2:] or ['joiner_catching_up_at_105_percent_returns_over_100ms','four_seats_held_joiner_catches_up_then_runs_at_round_rate_over_100ms','four_seats_held_joiner_catches_up_then_runs_at_round_rate_over_200ms','three_seats_held_host_catches_up_then_runs_at_round_rate_over_100ms','three_seats_held_host_catches_up_then_runs_at_round_rate_over_250ms','four_seats_two_joiners_held_at_once_catch_up_at_105_percent_over_100ms','image_return_three_seats_over_100ms_keeps_equal_histories_and_bound']
for name in rows:
 while active():print('Waiting for quiet replay slot',active(),flush=True);time.sleep(10)
 assert hashlib.sha256(binary.read_bytes()).hexdigest()==sha
 proof['running']=name
 (out/'proof.json').write_text(json.dumps(proof,indent=2)+'\n')
 with (out/(name+'.log')).open('w') as f:
  result=subprocess.run([sys.executable,'tools/run_selftests.py','--repo',str(root),'--out',str(out/name),'--only','net-session-plane','--timeout','300'],cwd=root,env=env|{'CC_TEST_SESSION_PLANE_CASE':name},stdout=f,stderr=subprocess.STDOUT)
 proof['results'].append({'name':name,'exit_code':result.returncode})
 print(name,result.returncode,flush=True)
proof['running']=None;proof['complete']=True
(out/'proof.json').write_text(json.dumps(proof,indent=2)+'\n')
