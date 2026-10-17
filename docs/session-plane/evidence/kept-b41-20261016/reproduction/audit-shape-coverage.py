import fnmatch, hashlib, json, re, sys
from pathlib import Path
root=Path.cwd().resolve()
run=Path(sys.argv[1])
log=(run/'net-session-plane-selftest/stdout.log').read_text()
# Include completed same-executable suite/driver observations. A failure in
# either measured run wins; a later pass does not erase the earlier result.
fixed=json.loads((run/'result.json').read_text())
for folder,row_name,log_folder in [('suite-54','net-session-plane','net-session-plane-selftest'),
 ('suite-54','net-session-plane-hold-schedules','net-session-plane-hold-schedules-selftest'),
 ('random-5000','net-session-plane','net-session-plane-selftest'),
 ('random-5000','net-session-plane-hold-schedules','net-session-plane-hold-schedules-selftest')]:
 extra=run.parent/folder
 if not (extra/'result.json').exists(): continue
 report=json.loads((extra/'result.json').read_text())
 row=report.get('results',{}).get(row_name,{})
 if report.get('complete') and not row.get('timed_out',True) and report.get('exe_sha256')==fixed.get('exe_sha256'):
  log+='\n'+(extra/log_folder/'stdout.log').read_text()
rows={}
for line in log.splitlines():
 m=re.match(r'\[net-session-plane-selftest\] (PASS|FAIL) ([A-Za-z0-9_]+)(?:[ :]|$)',line)
 if m:
  state,name=m.groups()
  if name not in rows or state=='FAIL': rows[name]={'state':state,'line':line}
coverage=json.loads((root/'Source/Network/NetSessionPlaneShapeCoverage.json').read_text())
audit={}
for key,entry in coverage.items():
 patterns=entry['rows']
 hits={name:row for name,row in rows.items() if any(fnmatch.fnmatchcase(name,pat) for pat in patterns)}
 absent=[pat for pat in patterns if not any(fnmatch.fnmatchcase(name,pat) for name in rows)]
 state='RED' if any(v['state']=='FAIL' for v in hits.values()) else 'MISSING' if absent else 'GREEN'
 audit[key]={'state':state,'rows':hits,'unmatched_patterns':absent,'scope':entry.get('scope')}
capacity=Path(__file__).parent/'ownership-tip-directory-capacity/result.json'
if capacity.exists():
 evidence=json.loads(capacity.read_text())
 if all((root/name).is_file() and hashlib.sha256((root/name).read_bytes()).hexdigest()==digest for name,digest in evidence['files'].items()):
  state='GREEN' if evidence['exit_code']==0 else 'RED'
  audit['referee:28']={'state':state,'rows':{'test_frame_tie_full_record_keeps_a_new_split_available':{
   'state':'PASS' if state=='GREEN' else 'FAIL','line':'Real signed endpoint capacity regression; see its unmodified first result',
   'result':str(capacity),'log':str(capacity.parent/'test.log'),'source_files_verified':True}},
   'unmatched_patterns':[],'scope':'real signed endpoint; isolated temporary persisted record; separate Python fixture'}
out=run/'shape-coverage-audit.json'
out.write_text(json.dumps(audit,indent=2)+'\n')
counts={state:sum(row['state']==state for row in audit.values()) for state in ('GREEN','RED','MISSING')}
print(json.dumps({'shapes':len(audit),'counts':counts,'artifact':str(out)},indent=2))
