"""Run the tree's complete suite with the Mac inventory guard checked at every launch."""
import argparse
import os
from pathlib import Path
import sys


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--guard',type=Path,required=True)
    options,remaining=parser.parse_known_args()
    sys.path.insert(0,str(options.repo/'tools'))
    import posix_test_runner
    import run_selftests
    original=posix_test_runner.IsolatedRun.start
    def guarded_start(run):
        if not options.guard.is_file():
            raise RuntimeError(f'inventory guard active: {options.guard}')
        return original(run)
    posix_test_runner.IsolatedRun.start=guarded_start
    os.environ['CCCP_HEADLESS']='1'
    os.environ['CCCP_TEST_BINARY']=str(options.repo/'build-gcc/CortexCommand')
    os.environ['CCCP_POSIX_HOP']='ssh' if sys.platform=='darwin' else 'off'
    sys.argv=[str(options.repo/'tools/run_selftests.py'),'--repo',str(options.repo),'--out',str(options.out),*remaining]
    return run_selftests.main()


if __name__=='__main__': raise SystemExit(main())
