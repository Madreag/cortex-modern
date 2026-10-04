"""Clocked R3 progress derived from the late joiner's native GUI probe."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

from acceptance_runtime import write_json


def stage_probe(run, spec):
    directory = Path(spec['own'])/'transfer-probe'
    directory.mkdir(exist_ok=False)
    script = directory/'probe.json'
    # LabelDump runs before activation. These steps merely keep it alive past
    # the image's completion; they never request a seat's actor or controls.
    write_json(script, dict(schema=1, timeout_ms=180000, activate_phase='Running',
                           label_dump=dict(every_ms=1000), steps=[
                               dict(op='wait', elapsed_ms=5000), dict(op='finish')]))
    run.env['CC_TEST_NET_UI_SCRIPT'] = str(script)
    run.record.setdefault('env_set', {})['CC_TEST_NET_UI_SCRIPT'] = str(script)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def native_transfer(probe, log, engine_pid):
    require(type(engine_pid) is int and engine_pid > 0 and probe.get('pid') == engine_pid,
            'label probe does not belong to the native engine')
    require(probe.get('schema') == 1 and probe.get('pass') is True and probe.get('complete') is True,
            'native label probe did not complete successfully')
    records = probe.get('label_dumps')
    require(isinstance(records, list) and len(records) >= 3, 'native label dumps are missing')
    fields = []
    for record in records:
        transfer = record.get('transfer', {})
        at, received, total = record.get('at_ms'), transfer.get('received_bytes'), transfer.get('total_bytes')
        require(all(type(value) is int and value >= 0 for value in (at, received, total)) and received <= total,
                'native transfer clock or byte progress is invalid')
        require(isinstance(record.get('screen'), str) and isinstance(record.get('service'), str) and
                record.get('why') in ('boundary', 'cadence'), 'native label state is invalid')
        require(isinstance(record.get('lines'), list) and
                all(isinstance(line, dict) and isinstance(line.get('text'), str) for line in record['lines']),
                'native visible text is missing')
        fields.append((at, record['screen'], record['service'], received, total, record['why'] == 'boundary'))
    require(all(left[0] < right[0] for left, right in zip(fields, fields[1:])),
            'native label clock repeated or moved backward')
    log_records = []
    completions = []
    for number, line in enumerate(log.splitlines(), 1):
        match = re.fullmatch(r'\[net-ui-probe\] label dump at_ms=(\d+) screen=(.*?) service=(.*?) transfer=(\d+)/(\d+)( boundary)?', line)
        if match:
            log_records.append((number, (int(match[1]), match[2], match[3], int(match[4]), int(match[5]), bool(match[6]))))
        completed = re.fullmatch(r'\[net-match\] state transfer complete: (\d+) bytes', line)
        if completed:
            completions.append((number, int(completed[1])))
    require([value for _, value in log_records] == fields, 'native label file and stdout receipts differ')
    require(len(completions) == 1 and completions[0][1] > 0,
            'native StateChunk completion is missing or repeated')
    active = [index for index, field in enumerate(fields) if field[4] > 0]
    require(bool(active), 'native transfer start boundary is missing')
    first, last = active[0], active[-1]
    require(first > 0 and last+1 < len(fields) and active == list(range(first, last+1)),
            'native transfer boundaries are missing or repeated')
    total = completions[0][1]
    require(all(fields[index][4] == total for index in active), 'native StateChunk bytes differ from GUI progress')
    require(all(fields[a][3] <= fields[b][3] for a, b in zip(active, active[1:])),
            'native transfer byte progress moved backward')
    end = last+1
    require(log_records[first][0] < completions[0][0] < log_records[end][0],
            'native StateChunk completion is outside its GUI boundaries')
    covered = records[first-1:end+1]
    require(all(0 < b['at_ms']-a['at_ms'] <= 5000 for a, b in zip(covered, covered[1:])),
            'native label coverage has a gap over five seconds')
    begin_ms, end_ms = fields[first][0], fields[end][0]
    return dict(start_ms=begin_ms, end_ms=end_ms, elapsed_s=(end_ms-begin_ms)/1000,
                received_bytes=total, native_pid=engine_pid,
                method='native GUI byte-progress boundaries on the probe clock',
                native_progress_duration_bounds_s=[(fields[last][0]-begin_ms)/1000,
                                                   (end_ms-fields[first-1][0])/1000],
                completion_log_line=completions[0][0],
                labels=[dict(at_ms=record['at_ms'], texts=[line['text'] for line in record['lines']],
                             screen=record['screen'], service=record['service'], transfer=record['transfer'])
                        for record in covered])


def collect(own):
    own = Path(own)
    paths = dict(probe=own/'transfer-probe/net-ui-result.json', log=own/'engine/stdout.log',
                 runner=own/'record.json')
    raw = {name: path.read_bytes() for name, path in paths.items()}
    probe = json.loads(raw['probe'].decode('utf-8-sig'))
    runner = json.loads(raw['runner'].decode('utf-8-sig'))
    value = native_transfer(probe, raw['log'].decode('utf-8', errors='replace'), runner.get('pid'))
    value['sources'] = {name: dict(path=path.relative_to(own).as_posix(), bytes=len(raw[name]),
                                 sha256=hashlib.sha256(raw[name]).hexdigest()) for name, path in paths.items()}
    write_json(own/'acceptance-transfer.json', value)
    return value
