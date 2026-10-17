import os,time,json,subprocess,sys,hashlib
from pathlib import Path
from datetime import datetime
from zoneinfo import ZoneInfo
root=Path('/home/erol/cortex-workers/session-plane-engineer'); s=Path(__file__).parent
out=s/sys.argv[1]; out.mkdir(exist_ok=False)
binary=root/'build-gcc/CortexCommand'
def stamp(): return datetime.now(ZoneInfo('America/Phoenix')).strftime('%Y-%m-%d %I:%M:%S %p MST')
def processes():
 result=[]
 for proc in Path('/proc').iterdir():
  if not proc.name.isdecimal(): continue
  try:
   args=(proc/'cmdline').read_bytes().split(b'\0')
   if args and Path(os.fsdecode(args[0])).name in ('ninja','CortexCommand','CortexCommand.exe'):
    result.append({'pid':int(proc.name),'exe':os.fsdecode(args[0]),'cwd':str((proc/'cwd').resolve())})
  except (OSError,ValueError): pass
 return result
while processes():
 print(stamp(),'Waiting for a quiet build/test slot',json.dumps(processes()),flush=True); time.sleep(10)
with (out/'build.log').open('w') as f: result=subprocess.run(['ninja','-C','build-gcc','-j4'],cwd=root,stdout=f,stderr=subprocess.STDOUT)
print(stamp(),'build',result.returncode,flush=True)
if result.returncode: raise SystemExit(result.returncode)
def sha(): return hashlib.sha256(binary.read_bytes()).hexdigest()
env={k:v for k,v in os.environ.items() if not k.startswith(('CC_TEST_HOLD_SCHEDULE','CC_TEST_HOLD_FIXED_FAILURE_REPORT','CC_TEST_SESSION_PLANE_CASE','CC_TEST_LOCKSTEP_SELFTEST_CASE','CCCP_TEST_MATCH_CASE'))}
env.pop('CCCP_HEADLESS',None)
env.update(CC_RUNNER_HEADED='1',DISPLAY=':0',CCCP_TEST_BINARY=str(binary))
summary={'source_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'binary_sha256':sha(),'began':stamp(),'complete':False,'results':[]}
summary['source_files']={}
for name in subprocess.check_output(['git','diff','--name-only','7b589c18e','--','Source','external/patches','tools/run_selftests.py','tools/test_module_version_warning.py'],cwd=root,text=True).splitlines():
 p=root/name
 if p.is_file(): summary['source_files'][name]=hashlib.sha256(p.read_bytes()).hexdigest()
(out/'source.diff').write_text(subprocess.check_output(['git','diff'],cwd=root,text=True))
def save(): (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
def run(name,args,settings):
 while processes():
  print(stamp(),'Waiting before',name,json.dumps(processes()),flush=True);time.sleep(10)
 summary['running']=name;save();print(stamp(),'BEGIN',name,flush=True)
 samples=[]
 with (out/(name+'.log')).open('w') as f:
  child=subprocess.Popen(args,cwd=root,env=env|settings,stdout=f,stderr=subprocess.STDOUT)
  while child.poll() is None:
   samples.append({'time':stamp(),'processes':processes(),'loadavg':os.getloadavg()})
   time.sleep(10)
 (out/(name+'-load.json')).write_text(json.dumps(samples,indent=2)+'\n')
 entry={'name':name,'exit_code':child.returncode,'binary_unchanged':sha()==summary['binary_sha256'],'ended':stamp(),
        'foreign_engines_observed':sorted({p['pid'] for sample in samples for p in sample['processes'] if Path(p['exe']).name.startswith('CortexCommand') and p['exe']!=str(binary)})}
 report=out/name/'result.json'
 if report.exists():
  data=json.loads(report.read_text());entry|={k:data.get(k) for k in ('passed','total','complete')}
 summary['results'].append(entry);save();print(stamp(),'END',json.dumps(entry),flush=True)
def suite(name,rows,settings,timeout):
 args=[sys.executable,'tools/run_selftests.py','--repo',str(root),'--out',str(out/name),'--timeout',str(timeout)]
 for row in rows: args+=['--only',row]
 run(name,args,settings)
suite('shape-inventory',['net-session-plane'],{'CC_TEST_HOLD_SCHEDULES':'0','CC_TEST_HOLD_SCHEDULE_THREADS':'7'},900)
run('slow-host-schedules',[sys.executable,str(s/'run-slow-host-proof.py'),str(out/'slow-host-schedules')],{})
suite('suite-54',[],{},300)
suite('random-5000',['net-session-plane-hold-schedules'],{'CC_TEST_SESSION_PLANE_CASE':'random_hold_schedules','CC_TEST_HOLD_SCHEDULES':'5000','CC_TEST_HOLD_SCHEDULE_SEED':'1','CC_TEST_HOLD_SCHEDULE_SHAPES':'0'},10800)
summary['running']=None;summary['complete']=True;summary['ended']=stamp();save()
raise SystemExit(any(x['exit_code'] or not x['binary_unchanged'] for x in summary['results']))
