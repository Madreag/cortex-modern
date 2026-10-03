"""Start the hashed acceptance stream as a one-shot launchd GUI-session job."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lane', type=Path, required=True)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--collection-id', required=True)
    parser.add_argument('--stream-sha256', required=True)
    parser.add_argument('--label-suffix', default='')
    options = parser.parse_args(argv)
    lane = options.lane.resolve(); stream = lane/'stream.zsh'
    if hashlib.sha256(stream.read_bytes()).hexdigest() != options.stream_sha256:
        raise ValueError('shipped Mac stream hash differs from the declared tree file')
    if (lane/'completion.json').exists(): raise ValueError('this Mac stream already completed; retain it and use a new lane')
    if options.label_suffix and not re.fullmatch(r'[A-Za-z0-9_-]+', options.label_suffix):
        parser.error('label suffix must contain only letters, digits, underscore and hyphen')
    label = 'com.cortex.acceptance.' + options.collection_id + ('.' + options.label_suffix if options.label_suffix else '')
    agent = lane/'acceptance-stream.plist'
    data = dict(Label=label, ProgramArguments=['/bin/zsh', str(stream)], WorkingDirectory=str(lane), RunAtLoad=True,
        KeepAlive=False, ProcessType='Background', StandardOutPath=str(lane/'job.out'), StandardErrorPath=str(lane/'job.err'),
        EnvironmentVariables=dict(SHA=options.source_sha, ACCEPTANCE_COLLECTION=options.collection_id, LANE=str(lane),
                                  CCCP_HEADLESS='1', PYTHONDONTWRITEBYTECODE='1'))
    with agent.open('xb') as output: plistlib.dump(data, output)
    session = f'gui/{os.getuid()}'
    subprocess.run(['/bin/launchctl', 'bootstrap', session, str(agent)], check=True)
    receipt = dict(label=label, session=session, stream=str(stream), stream_sha256=options.stream_sha256,
                   source_sha=options.source_sha, collection_id=options.collection_id, plist_sha256=hashlib.sha256(agent.read_bytes()).hexdigest())
    (lane/'launch-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(receipt))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
