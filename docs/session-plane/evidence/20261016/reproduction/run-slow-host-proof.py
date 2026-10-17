import os, subprocess, sys, json, re
from pathlib import Path
root = Path('.')
base = Path(__file__).parent / sys.argv[1]
env = os.environ | {'CC_RUNNER_HEADED':'1','DISPLAY':':0','CCCP_TEST_BINARY':str(root/'build-gcc/CortexCommand'),'CC_TEST_HOLD_SCHEDULES':'0','CC_TEST_RETURN_INPUT_DETAILS':'1'}
results = []
for seats, internet, rtt in [(2,0,30),(2,1,30),(4,1,30),(2,0,200),(2,1,200),(4,1,200)]:
    name=f'{seats}-seats-referee-{internet}-{rtt}ms'
    schedule = f'seed=0 seats={seats} internet={internet} exchange=100 end=30000 rtt={",".join([str(rtt)]*(seats*(seats-1)//2))} jitter={",".join(["0"]*(seats*(seats-1)//2))} recover={",".join(["300"]*seats)} phase={",".join(["0"]*seats)} spell=1:slow:6000:10000:80'
    run=subprocess.run([sys.executable,'tools/run_selftests.py','--repo',str(root),'--out',str(base/name),'--timeout','300','--only','net-session-plane'],cwd=root,env=env|{'CC_TEST_HOLD_SCHEDULE':schedule})
    stdout=(base/name/'net-session-plane-selftest/stdout.log').read_text()
    waits=[]
    for line in stdout.splitlines():
        if '[net-frame-wait]' not in line or ' wait_ms=' not in line: continue
        fields=dict(re.findall(r'([a-z0-9_]+)=([^ ]+)',line))
        if int(fields.get('local','0'))<2 or int(fields['wait_ms'])==0: continue
        waits.append(fields)
    result={'name':name,'schedule':schedule,'runner_pass':run.returncode==0,'survivor_waits':waits,'survivor_wait_count':len(waits),'survivor_startup_wait_count':sum(int(w.get('frame','0'))<360 for w in waits),'wait_scope':'entire match','survivor_longest_ms':max((int(w['wait_ms']) for w in waits),default=0)}
    (base/name/'survivor-zero-wait-audit.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ['schedule','survivor_waits']}),flush=True)
    results.append(result)
(base/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
sys.exit(any(not r['runner_pass'] or r['survivor_waits'] for r in results))
