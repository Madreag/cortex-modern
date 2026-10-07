"""Check the spelling of newly added quoted project includes before a Mac build."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base',required=True,help='the commit new includes are counted from, for example the branch point')
    parser.add_argument('--out',type=Path,required=True)
    options=parser.parse_args()
    repo=Path(__file__).resolve().parents[2]
    tracked=subprocess.check_output(['git','ls-files'],cwd=repo,text=True).splitlines()
    names={Path(name).name for name in tracked}
    diff=subprocess.check_output(['git','diff','--unified=0',options.base,'HEAD','--','Source'],cwd=repo,text=True)
    added=sorted(set(re.findall(r'^\+\s*#include\s+"([^"]+)"',diff,re.M)))
    missing=[name for name in added if Path(name).name not in names]
    record=dict(commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
                added_quoted_includes=added,missing=missing,passed=not missing,
                scope='Exact case of tracked project include basenames; compiler validates standard and SDK headers.')
    options.out.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    print('include scan', 'PASS' if record['passed'] else 'FAIL', 'added',len(added),'missing',missing)
    return 0 if record['passed'] else 1


if __name__=='__main__': raise SystemExit(main())
