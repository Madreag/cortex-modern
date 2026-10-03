"""Localise only progress and collection paths after a POSIX fetch; keep hashed receipts byte-identical."""
from __future__ import annotations
import argparse
import json
from pathlib import Path,PurePosixPath
import sys


def localise(root, remote_root):
    from run_split import rewrite_paths
    root=Path(root).resolve();remote_root=PurePosixPath(remote_root)
    def adapt(value):
        if isinstance(value,dict):return {rewrite_paths(key,remote_root,root):adapt(child) for key,child in value.items()}
        if isinstance(value,list):return [adapt(child) for child in value]
        return rewrite_paths(value,remote_root,root)
    changed=0
    for name in ('progress.json','DEFECTS.json'):
        for path in sorted(root.rglob(name)):
            if path.is_symlink() or not path.resolve().is_relative_to(root):raise ValueError('fetched collection path leaves its share')
            text=path.read_text(encoding='utf-8-sig')
            if str(remote_root) not in text:continue
            path.write_text(json.dumps(adapt(json.loads(text)),indent=2)+'\n',encoding='utf-8');changed+=1
    return changed


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory',type=Path);parser.add_argument('remote_root');parser.add_argument('local_root',type=Path)
    options=parser.parse_args(argv);sys.path.insert(0,str(options.inventory))
    print(f'localised {localise(options.local_root,options.remote_root)} progress/collection files; hashed receipts retained')
    return 0


if __name__=='__main__':raise SystemExit(main())
