"""Make the single-player control the feel matrix's gates compare against: the pie-close fixture, uninstrumented, run once
on a given executable (the build before a change) with a tree's Data. The output folder is what feel_measure.py --sp-control
reads: launch.json, the trace, its sim dump and sp_summary.json.

    python tools/feel/make_sp_control.py --repo <tree> [--exe <control exe>] --out <new folder>
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run  # noqa: E402
from feel.report import file_record, write_json  # noqa: E402
from pie_lockstep.run_arm import FIXTURES, module_index  # noqa: E402

INPUT_SCRIPT = FIXTURES / 'pie_open_then_next.txt'


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repo', type=Path, required=True, help="the tree whose Data the control runs on")
    parser.add_argument('--exe', type=Path, help="the control executable (default: the tree's own)")
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args(argv)
    out = args.out
    if out.exists():
        parser.error(f'{out} exists; the control is written to a new folder')
    flags = ['-scenario', 'PieSwitchSP', '-seed', '42', '-max-ticks', '320', '-tick-hashes',
             '-out', str(out / 'trace.json'), '-input-script', str(INPUT_SCRIPT)]
    run = make_run(args.repo, flags, out, timeout=args.timeout, env=dict(CCCP_HEADLESS='1', CC_SIM_DUMP='27:320'),
                   expected=[out / 'trace.json', out / 'trace.json.simdump.txt'])
    if args.exe:
        run.argv[0] = str(args.exe.resolve())
    module = Path(run.cwd) / 'Userdata/UserScenes.rte'
    module.mkdir(exist_ok=True)
    (module / 'Index.ini').write_text(module_index(True), encoding='utf-8')
    (module / 'PieSwitchSP.lua').write_bytes((FIXTURES / 'PieSwitchSP.lua').read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()
    summary = {key: record.get(key) for key in ('pid', 'exit_code', 'timed_out', 'evidence_complete', 'cwd', 'verdict_lines')}
    write_json(out / 'sp_summary.json', summary)
    write_json(out / 'control.json', dict(executable=file_record(Path(run.argv[0])), fixture=file_record(FIXTURES / 'PieSwitchSP.lua'),
                                         input_script=file_record(INPUT_SCRIPT), argv=run.argv, summary=summary))
    print(json.dumps(summary), flush=True)
    return 0 if record.get('exit_code') == 0 and not record.get('timed_out') and record.get('evidence_complete') else 1


if __name__ == '__main__':
    raise SystemExit(main())
