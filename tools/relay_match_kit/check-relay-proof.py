"""Synthetic collector contract checks; these are not engine or relay proof."""
import copy
import argparse
import importlib.util
import json
from pathlib import Path

base=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('relay_proof',Path(__file__).with_name('relay-proof.py'))
proof=importlib.util.module_from_spec(spec);spec.loader.exec_module(proof)
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out',type=Path,required=True)
root=parser.parse_args().out.resolve()
root.mkdir(exist_ok=False)
roots={role:root/role for role in proof.ROLES}
epoch=1700000000000
for peer,(role,p) in enumerate(roots.items(),1):
 (p/'engine').mkdir(parents=True);(p/'inputs').mkdir()
 def write(name,value): (p/name).write_text(json.dumps(value)+'\n')
 write('participant-result.json',dict(pass_=True,exit_code=0,timed_out=False,synthetic=True))
 write('inputs/net-ui-result.json',{'pass':True,'complete':True,'script':{'steps':[{'op':'wait','seat_ready':'local','brains_ready':3,'within_ms':5000},{'op':'assert','equals':{'ai_held_peer_ids':[]}}]}})
 write('relay-launch.json',{'leave_test':{'tick_cap':0,'path':'pause-menu-leave'},'synthetic':True})
 lock={'local_peer_id':peer,'host_peer_id':1,'election_generation':0,'ai_held_peer_ids':[1],'peers':{'1':{'rejoins':2}},'timeouts':0,'peer_silence_leave_ms':10000}
 write('engine/match-report.json',{'runner':{'lockstep':lock,'directory_at_end':{'confirmed_listed':True}},'connections':[]})
 lines=['[net-ice] connected connection=1 after_ms=100 route=relay','[net-ice] connected connection=2 after_ms=100 route=relay',f'[net-match] brain placement committed: seat={peer-1} frame=5 unix_ms={epoch+83}']
 lines += [f'[net-match] brain placement applied: seat={seat} frame=38 unix_ms={epoch+633}' for seat in range(3)]
 at=235000 if role=='ally' else 201800 if role=='linux' else 201900
 lines += [f'[menu-script] exit unix_ms={epoch+at} lockstep_frame=12100 peer={peer}']
 if role!='ally':
  lines += ['[menu-script] open_local_pause the match menu PASS', '[menu-script] activate ButtonLeaveMatch PASS', '[menu-script] activate ButtonLeaveConfirm PASS', f'[net-match-leave] peer={peer} frame=12100 now_ms=201666 reason=Match left']
 else: lines += ['[menu-script] round-ended -> OK (members=3 state=Starting)']
 if role=='ally':
  lines+=['[net-link-blackout] begin peer=1 frame=600 duration_ms=8000 now_ms=10000 unix_ms='+str(epoch+10000),
          '[net-link-blackout] end peer=1 elapsed_ms=8000 now_ms=18000 unix_ms='+str(epoch+18000)]
  lines += [f'[net-peer-departure] peer={remote} frame=12100 now_ms=202000 unix_ms={epoch+202000} source=remote-close' for remote in (2,3)]
  lines += [f'[net-peer-departure] all-remote-ended applied=12100 now_ms=202000 unix_ms={epoch+202000}']
 (p/'engine/stdout.log').write_text('\n'.join(lines)+'\n')
 with (p/'engine/live.jsonl').open('w') as f:
  for tick in range(1,12101):
   f.write(json.dumps({'tick':tick,'peer':peer,'round':1,'wall_ms':tick*1000/60,'net_clock_ms':int(tick*1000/60),'unix_ms':epoch+int(tick*1000/60),
                      'input_accepted_through':tick+28,'input_delay_ticks':28,'paused':tick<=40,'total':str(tick),'ai_held_peer_ids':[1] if 724<=tick<=1350 else []})+'\n')
checks=[]
def check(name,expected,needle=None):
 result=proof.score(roots)
 assert result['pass'] is expected,(name,result['errors'])
 if needle:assert any(needle in error for error in result['errors']),(name,result['errors'])
 checks.append({'name':name,'pass':True,'expected_verdict':expected,'errors':result['errors']})
 return result
check('synthetic_complete_receipt',True)
def mutate(name,role,filename,change,needle):
 p=roots[role]/filename;old=p.read_bytes()
 try:
  changed=change(old)
  if changed is None:p.unlink()
  else:p.write_bytes(changed)
  check(name,False,needle)
 finally:p.write_bytes(old)
mutate('missing_host_report','ally','engine/match-report.json',lambda b:None,'match-report.json: FileNotFoundError')
mutate('malformed_report','linux','engine/match-report.json',lambda b:b'{broken','match-report.json: JSONDecodeError')
mutate('missing_participant','mac','participant-result.json',lambda b:None,'participant-result.json: FileNotFoundError')
mutate('nonzero_host_exit','ally','participant-result.json',lambda b:b.replace(b'"exit_code": 0',b'"exit_code": 1'),'participant/probe/exit failed')
mutate('late_leave','ally','engine/stdout.log',lambda b:b.replace(b'now_ms=202000',b'now_ms=212100'),'leave not adjudicated within ten seconds')
mutate('no_host_terminal_event','ally','engine/stdout.log',lambda b:b'\n'.join(l for l in b.splitlines() if b'all-remote-ended' not in l),'host did not complete within ten seconds')
def rows_change(b,change):
 rows=[json.loads(x) for x in b.decode().splitlines()]
 change(rows)
 return ('\n'.join(json.dumps(r) for r in rows)+'\n').encode()
maclog=roots['mac']/'engine/stdout.log'
maclive=roots['mac']/'engine/live.jsonl'
linuxlive=roots['linux']/'engine/live.jsonl'
saved={p:p.read_bytes() for p in (maclog,maclive,linuxlive)}
try:
 maclog.write_bytes(saved[maclog].replace(b'frame=12100 now_ms=201666',b'frame=12098 now_ms=201633'))
 maclive.write_bytes(rows_change(saved[maclive],lambda rows:[r.update(total='mac_departed') for r in rows if r['tick']>=12098]))
 linuxlive.write_bytes(rows_change(saved[linuxlive],lambda rows:rows[-1].update(total='linux_departed')))
 result=check('leaver_final_tick_and_surviving_ticks',True)
 assert result['common_ticks']==12097 and result['compared_ticks']==12099
 assert result['hash_coverage'][-1]=={'first_tick':12098,'last_tick':12099,'ticks':2,'roles':['ally','linux']}
 assert result['hash_exclusions']['mac']['rows']==3 and result['hash_exclusions']['linux']['rows']==1
 entry=checks[-1]
 entry.update(hash_coverage=result['hash_coverage'],hash_exclusions=result['hash_exclusions'])
 linuxlive.write_bytes(rows_change(linuxlive.read_bytes(),lambda rows:rows[-2].update(total='changed_survivor')))
 result=proof.score(roots)
 assert not result['pass'] and result['hash_mismatches']==[12099],result['errors']
 entry['survivor_mismatch_detected']={'tick':12099,'pass':result['pass'],'errors':result['errors']}
finally:
 for p,content in saved.items():p.write_bytes(content)
live=roots['linux']/'engine/live.jsonl';old=live.read_bytes()
try:
 live.write_bytes(rows_change(old,lambda rows:[r.update(wall_ms=r['tick']*1000/60.1) for r in rows]))
 result=check('catch_up_rate_60_1_and_pacing_bounds',True)
 entry=checks[-1]
 entry['positive_rate']=result['details']['linux']['timing']['in_play_tps']
 entry['cases']=[]
 def rate_case(name,change,needle):
  live.write_bytes(rows_change(old,change))
  result=proof.score(roots)
  assert not result['pass'] and any(needle in error for error in result['errors']),(name,result['errors'])
  timing=result['details']['linux']['timing']
  entry['cases'].append({'name':name,'pass':True,'expected_verdict':False,'errors':result['errors'],
                         'in_play_tps':timing['in_play_tps'],'median_60_tick_interval_ms':timing['median_60_tick_interval_ms'],
                         'consecutive_60s':timing['consecutive_60s']})
 rate_case('full_span_below_59_9',lambda rows:[r.update(wall_ms=r['tick']*1000/59.8) for r in rows],'full-match in-play rate below 59.9')
 rate_case('one_minute_deficit_with_full_span_above_59_9',lambda rows:[r.update(wall_ms=r['tick']*1000/60 if r['tick']<=40 else
           40*1000/60+min(r['tick']-40,3600)*1000/59.7+max(r['tick']-3640,0)*1000/60.5) for r in rows],
           '60-second in-play window below 59.9')
 assert entry['cases'][-1]['in_play_tps']>=59.9
 def uneven_intervals(rows):
  elapsed=0
  for row in rows:
   elapsed+=1000/54 if ((row['tick']-1)//60)%20<11 else 14.4
   row['wall_ms']=elapsed
 rate_case('median_interval_more_than_one_ms_off',uneven_intervals,'median 60-tick average interval')
 assert entry['cases'][-1]['in_play_tps']>=59.9
 assert all(window['tps']>=59.9 for window in entry['cases'][-1]['consecutive_60s'])
finally:live.write_bytes(old)
mutate('host_never_returns','ally','engine/live.jsonl',lambda b:rows_change(b,lambda rows:[r.update(ai_held_peer_ids=[1]) for r in rows if r['tick']>=724]),'host never returned')
mutate('quorum_receipt_stuck','linux','engine/live.jsonl',lambda b:rows_change(b,lambda rows:[r.update(input_accepted_through=1599) for r in rows if r['tick']>=2000]),'acceptance stopped advancing')
mutate('delay_exceeds_bound','mac','engine/live.jsonl',lambda b:rows_change(b,lambda rows:rows[1999].update(input_delay_ticks=37)),'1..36 tick bound')
mutate('unintended_healthy_hold','ally','engine/live.jsonl',lambda b:rows_change(b,lambda rows:rows[1999].update(ai_held_peer_ids=[3])),'unintended healthy seat hold')
mutate('missing_input_progress','linux','engine/live.jsonl',lambda b:rows_change(b,lambda rows:[r.pop('input_accepted_through') for r in rows]),'per-tick input progress evidence')
mutate('short_full_match','mac','engine/live.jsonl',lambda b:rows_change(b,lambda rows:rows.__delitem__(slice(11999,None))),'insufficient common full-match ticks')
mutate('transport_drop','mac','engine/stdout.log',lambda b:b+b'\n[net-transport] closed peer=1 reason=Connection dropped\n','transport connection dropped')
mutate('reliable_overflow','linux','engine/stdout.log',lambda b:b+b'\n1048336 reliable bytes buffered\n','reliable receive buffer overflow')
mutate('host_held_again','mac','engine/live.jsonl',lambda b:rows_change(b,lambda rows:rows[1999].update(ai_held_peer_ids=[1])),'became held again')
mutate('late_return','ally','engine/live.jsonl',lambda b:rows_change(b,lambda rows:[r.update(ai_held_peer_ids=[1]) for r in rows if 724<=r['tick']<=1800]),'return not observed within ten seconds')
mutate('changed_common_hash','linux','engine/live.jsonl',lambda b:rows_change(b,lambda rows:rows[999].update(total='wrong')),'total hash mismatch')
for name,tick,gap,needle in [('blackout_hitch',700,75,'unaffected peer pauses over 50')]:
 mutate(name,'linux','engine/live.jsonl',lambda b,t=tick,g=gap:rows_change(b,lambda rows:[r.update(wall_ms=r['wall_ms']+g) for r in rows if r['tick']>=t]),needle)
outside=roots['linux']/'engine/live.jsonl';old=outside.read_bytes()
try:
 outside.write_bytes(rows_change(old,lambda rows:[r.update(wall_ms=r['wall_ms']+max(0,125-(r['tick']-2000)*15)) for r in rows if 2000<=r['tick']<=2009]))
 check('nonblackout_pause_is_measured',True)
 assert proof.score(roots)['details']['linux']['steady_gaps_over_100ms']
finally:outside.write_bytes(old)
mutate('missing_host_round_wait','ally','engine/stdout.log',lambda b:b'\n'.join(l for l in b.splitlines() if b'round-ended -> OK' not in l),'host exit did not follow both joiner departures')
mutate('missing_exit_timestamp','linux','engine/stdout.log',lambda b:b'\n'.join(l for l in b.splitlines() if b'[menu-script] exit' not in l),'missing unique engine exit')
mutate('missing_real_leave','mac','engine/stdout.log',lambda b:b'\n'.join(l for l in b.splitlines() if b'net-match-leave' not in l),'missing unique in-match Leave')
mutate('missing_leave_gesture','linux','engine/stdout.log',lambda b:b'\n'.join(l for l in b.splitlines() if b'ButtonLeaveConfirm' not in l),'missing completed real gesture')
mutate('late_placement_frame','mac','engine/stdout.log',lambda b:b.replace(b'applied: seat=0 frame=38',b'applied: seat=0 frame=306'),'outside 0..300 lockstep frames')
mutate('missing_monotonic_evidence','linux','engine/live.jsonl',lambda b:rows_change(b,lambda rows:[r.pop('net_clock_ms') for r in rows]),'local monotonic evidence')
# Entirely independent UTC epochs and monotonic origins are allowed. The local
# log/row clocks move together; only shared lockstep frame numbers cross boxes.
for role,offset in [('linux',900000),('mac',-600000)]:
 p=roots[role]/'engine/stdout.log';old=p.read_bytes()
 import re
 changed=re.sub(rb'unix_ms=(\d+)',lambda m:b'unix_ms='+str(int(m[1])+offset).encode(),old)
 try:
  p.write_bytes(changed);check('unsynchronized_utc_'+role,True)
 finally:p.write_bytes(old)
for role,offset in [('ally',7000000),('linux',9000000),('mac',11000000)]:
 log=roots[role]/'engine/stdout.log';live=roots[role]/'engine/live.jsonl'
 oldlog,oldlive=log.read_bytes(),live.read_bytes()
 try:
  log.write_bytes(re.sub(rb'now_ms=(\d+)',lambda m:b'now_ms='+str(int(m[1])+offset).encode(),oldlog))
  live.write_bytes(rows_change(oldlive,lambda rows:[r.update(net_clock_ms=r['net_clock_ms']+offset) for r in rows]))
  check('independent_monotonic_origin_'+role,True)
 finally:log.write_bytes(oldlog);live.write_bytes(oldlive)
p=roots['ally']/'participant-result.json';old=p.read_bytes()
try:
 d=json.loads(old);d.update(pass_=False,timed_out=True,exit_code=124);p.write_text(json.dumps(d))
 result=proof.score(roots)
 assert not result['pass'] and result['details']['ally']['rate_status']=='unscored_truncated'
 assert not any('full-match rate' in e for e in result['errors'])
 checks.append({'name':'truncation_is_not_scored_as_slow_pacing','pass':True,'expected_verdict':False})
finally:p.write_bytes(old)
(root/'contracts.json').write_text(json.dumps({'synthetic':True,'relay_proof':False,'checks':checks},indent=2)+'\n')
print(json.dumps({'synthetic_contract_checks':len(checks),'passed':len(checks),'relay_proof':False}))
