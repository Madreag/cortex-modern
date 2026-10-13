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
 lock={'local_peer_id':peer,'host_peer_id':1,'election_generation':0,'ai_held_peer_ids':[1],'peers':{'1':{'rejoins':2}},'timeouts':0,'peer_silence_leave_ms':10000,'completed_simulation_tick':12100,'next_frame':12101}
 seats=[{'lockstep_peer_id':remote,'committed':True,'dropped':True,'held_for_reclaim':True,'closed':False,'cpu':False,'holder_generation':1} for remote in (2,3)]
 write('engine/match-report.json',{'runner':{'lockstep':lock,'directory_at_end':{'confirmed_listed':True}},'connections':[],
                                 'reconnect':{'moderation':seats},'last_match':{'result':'Match ended by host','running_ticks':12100}})
 lines=['[net-ice] connected connection=1 after_ms=100 route=relay','[net-ice] connected connection=2 after_ms=100 route=relay',f'[net-match] brain placement committed: seat={peer-1} frame=5 unix_ms={epoch+83}']
 lines += [f'[net-match] brain placement applied: seat={seat} frame=38 unix_ms={epoch+633}' for seat in range(3)]
 at=235000 if role=='ally' else 201800 if role=='linux' else 201900
 lines += [f'[menu-script] exit unix_ms={epoch+at} lockstep_frame=12100 peer={peer}']
 if role!='ally':
  lines += ['[menu-script] open_local_pause the match menu PASS', '[menu-script] activate ButtonLeaveMatch PASS', '[menu-script] activate ButtonLeaveConfirm PASS', f'[net-match-leave] peer={peer} frame=12100 now_ms=201666 reason=Match left']
 if role=='ally':
  lines+=['[net-link-blackout] begin peer=1 frame=600 duration_ms=8000 now_ms=10000 unix_ms='+str(epoch+10000),
          '[net-link-blackout] end peer=1 elapsed_ms=8000 now_ms=18000 unix_ms='+str(epoch+18000)]
  lines += [f'[net-peer-departure] peer={remote} frame=12128 now_ms=202000 unix_ms={epoch+202000} source=round-notice' for remote in (2,3)]
  lines += ['[menu-script] open_local_pause the match menu PASS','[menu-script] activate ButtonEndMatch PASS',
            '[net-match] the host ends the round at frame 12128, playing to it from 12101',
            '[menu-script] activate ButtonLeaveConfirm PASS']
  lines += [f'[net-peer-departure] all-remote-ended applied=12100 now_ms=202000 unix_ms={epoch+202000}']
  lines += ['[menu-script] round-ended -> OK (members=3 state=Completed)']
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
mutate('late_leave','ally','engine/stdout.log',lambda b:b.replace(b'now_ms=202000',b'now_ms=212100'),'leave not noted within ten seconds')
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

# Leave notices and the host's own end have separate deadlines.
p=roots['ally']/'engine/stdout.log';old=p.read_bytes()
try:
 p.write_bytes(old.replace(b'peer=2 frame=12128 now_ms=202000',b'peer=2 frame=12128 now_ms=201630'))
 result=check('leave_notice_before_matching_host_display',True)
 assert result['leave_test']['leaves']['linux']['notice_ms']==-36
finally:p.write_bytes(old)
mutate('leave_notice_before_its_input_range','ally','engine/stdout.log',lambda b:b.replace(b'peer=2 frame=12128 now_ms=202000',b'peer=2 frame=12128 now_ms=200000'),'leave not noted within ten seconds')
mutate('remote_close_is_not_a_leave_notice','ally','engine/stdout.log',lambda b:b.replace(b'source=round-notice',b'source=remote-close'),'leave not noted within ten seconds')
mutate('seat_not_retained_after_leave','ally','engine/match-report.json',lambda b:b.replace(b'"held_for_reclaim": true',b'"held_for_reclaim": false'),'leave did not retain its AI seat')
mutate('seat_closed_after_leave','ally','engine/match-report.json',lambda b:b.replace(b'"closed": false',b'"closed": true'),'leave did not retain its AI seat')
mutate('missing_host_end_request','ally','engine/stdout.log',lambda b:b'\n'.join(line for line in b.splitlines() if b'the host ends the round' not in line),'missing unique host End Match request')
mutate('missing_host_end_gesture','ally','engine/stdout.log',lambda b:b'\n'.join(line for line in b.splitlines() if b'activate ButtonEndMatch' not in line),'missing unique host End Match gesture')
mutate('silent_peer_timing_disabled','ally','engine/match-report.json',lambda b:b.replace(b'"peer_silence_leave_ms": 10000',b'"peer_silence_leave_ms": 0'),'silent-peer timing')
mutate('silent_peer_timing_unbounded','ally','engine/match-report.json',lambda b:b.replace(b'"peer_silence_leave_ms": 10000',b'"peer_silence_leave_ms": 10001'),'silent-peer timing')

report=roots['ally']/'engine/match-report.json';log=roots['ally']/'engine/stdout.log';live=roots['ally']/'engine/live.jsonl'
oldreport,oldlog,oldlive=report.read_bytes(),log.read_bytes(),live.read_bytes()
try:
 rows=[json.loads(line) for line in oldlive.decode().splitlines()]
 for tick in range(12101,13201):
  row=dict(rows[-1],tick=tick,wall_ms=tick*1000/60,net_clock_ms=int(tick*1000/60),unix_ms=epoch+int(tick*1000/60),
           input_accepted_through=tick+28,total=str(tick),ai_held_peer_ids=[2,3] if tick>=12128 else [])
  rows.append(row)
 live.write_text('\n'.join(json.dumps(row) for row in rows)+'\n')
 value=json.loads(oldreport);value['runner']['lockstep'].update(completed_simulation_tick=13200,next_frame=13201)
 value['last_match']['running_ticks']=13200;report.write_text(json.dumps(value))
 log.write_bytes(oldlog.replace(b'ends the round at frame 12128, playing to it from 12101',b'ends the round at frame 13228, playing to it from 13201')
                .replace(b'all-remote-ended applied=12100 now_ms=202000',b'all-remote-ended applied=13200 now_ms=220000'))
 result=check('host_keeps_playing_with_ai_until_it_chooses_end',True)
 assert result['leave_test']['leaves']['linux']['ai_hold_frame']==12128
 assert result['leave_test']['end_anchor']['now_ms']==220000
 kept=live.read_bytes()
 live.write_bytes(rows_change(kept,lambda rows:[row.update(ai_held_peer_ids=[]) for row in rows if row['tick']>=12128]))
 check('departed_seats_not_played_by_ai',False,'departed seat not held within three ticks')
 live.write_bytes(kept)
 log.write_bytes(log.read_bytes().replace(b'applied=13200 now_ms=220000',b'applied=13200 now_ms=230001'))
 check('host_end_completion_later_than_ten_seconds',False,'host did not complete within ten seconds of End Match')
finally:
 report.write_bytes(oldreport);log.write_bytes(oldlog);live.write_bytes(oldlive)

try:
 value=json.loads(oldreport);value['runner'].pop('directory_at_end')
 value['runner']['lockstep'].update(completed_simulation_tick=None,next_frame=0,peer_silence_leave_ms=250)
 report.write_text(json.dumps(value))
 registered=b'[net-directory] registered session_id=synthetic-round heartbeat_s=5\n'
 log.write_bytes(registered+oldlog+b'[net-directory] state: registered -> deleting\n')
 result=check('completed_round_listing_from_ordered_registration',True)
 assert result['details']['ally']['listing_source']=='ordered registration log before round end'
 assert result['details']['ally']['completed_report_retained'] is False
 log.write_bytes(oldlog)
 check('no_listing_acknowledgment',False,'no confirmed public listing at round end')
 log.write_bytes(registered+b'[net-directory] state: registered -> deleting\n'+oldlog)
 check('listing_retracted_before_round_end',False,'no confirmed public listing at round end')
 log.write_bytes(registered+oldlog)
 value['last_match']['running_ticks']=12099;report.write_text(json.dumps(value))
 check('listing_end_must_name_completed_round',False,'no confirmed public listing at round end')
finally:report.write_bytes(oldreport);log.write_bytes(oldlog)
mutate('explicit_unlisted_round_snapshot','ally','engine/match-report.json',lambda b:b.replace(b'"confirmed_listed": true',b'"confirmed_listed": false'),'no confirmed public listing at round end')
# The leaver's last input lands up to the delay bound past the leave; the round notes the first frame without it.
live=roots['linux']/'engine/live.jsonl';log=roots['ally']/'engine/stdout.log'
oldlive,oldlog=live.read_bytes(),log.read_bytes()
try:
 live.write_bytes(rows_change(oldlive,lambda rows:[r.update(input_delay_ticks=36) for r in rows if r['tick']>=12000]))
 log.write_bytes(oldlog.replace(b'peer=2 frame=12128',b'peer=2 frame=12137'))
 result=check('leave_noted_at_first_frame_after_its_last_input',True)
 assert result['leave_test']['leaves']['linux']['input_delay_ticks']==36
 log.write_bytes(oldlog.replace(b'peer=2 frame=12128',b'peer=2 frame=12138'))
 check('leave_noted_past_its_last_input',False,'linux leave not noted within ten seconds')
finally:live.write_bytes(oldlive);log.write_bytes(oldlog)
# A seat the round bridged has its own input replaced until its return; acceptance outside a bridge still advances with play.
live=roots['ally']/'engine/live.jsonl';log=roots['ally']/'engine/stdout.log'
oldlive,oldlog=live.read_bytes(),log.read_bytes()
bridge=b'[net-frame-group] bridge frame=5000 members=6 voters=6\n';back=b'[net-frame-group] return frame=5089 members=7 voters=6\n'
try:
 live.write_bytes(rows_change(oldlive,lambda rows:[r.update(input_accepted_through=4999) for r in rows if 5000<=r['tick']<5044]))
 log.write_bytes(oldlog+bridge+back)
 result=check('host_seat_bridged_after_return_resumes_acceptance',True)
 assert result['details']['ally']['own_seat_bridges_after_return']==[[5000,5089]]
 log.write_bytes(oldlog)
 check('acceptance_lag_without_a_bridge',False,'acceptance stopped advancing')
 log.write_bytes(oldlog+bridge)
 check('host_seat_bridge_without_return',False,'bridged after return without a recorded return')
 log.write_bytes(oldlog+b'[net-frame-group] bridge frame=5000 members=5 voters=5\n'+back)
 check('another_seats_bridge_does_not_excuse_the_lag',False,'acceptance stopped advancing')
 live.write_bytes(rows_change(oldlive,lambda rows:[r.update(input_accepted_through=4999) for r in rows if 5000<=r['tick']<5100]))
 log.write_bytes(oldlog+bridge+back)
 check('acceptance_lag_past_the_return',False,'acceptance stopped advancing')
finally:live.write_bytes(oldlive);log.write_bytes(oldlog)
(root/'contracts.json').write_text(json.dumps({'synthetic':True,'relay_proof':False,'checks':checks},indent=2)+'\n')
print(json.dumps({'synthetic_contract_checks':len(checks),'passed':len(checks),'relay_proof':False}))
