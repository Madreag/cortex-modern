"""Keep the cross reservation alive across native preflight and task dispatch."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import sys
import time

import cross_peers as cross
from acceptance_frozen_tools import receipt as frozen_receipt
from acceptance_native_runtime import acquire_shared_reservation, release_shared_reservation
from acceptance_runtime import write_json
from acceptance_native_load import acquire_reservation

ENV_FILE = 'lease-env.key'  # Private reservation tokens stay native and are excluded from evidence tar streams.


def alive(pid):
    if type(pid) is not int or pid <= 0:
        return False
    if sys.platform == 'win32':
        from win32_test_runner import _pid_alive
        return _pid_alive(pid)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def assert_idle_task(box):
    if not box['kind'].startswith('windows-'):
        return
    if box['kind'] == 'windows-task':
        state = cross.command(['pwsh','-NoProfile','-Command',
                           '(Get-ScheduledTask -TaskName cortex-session1).State.ToString()']).strip()
        if state != 'Ready':
            raise RuntimeError('native session task is occupied; no reservation or launch forced')
    else:
        cross.assert_box_guard(box)
    from feel.launch_budget import free_memory_bytes
    if free_memory_bytes() < box['launch_floor_gib']*1024**3:
        raise RuntimeError('native session memory floor is not met')


def hold(path):
    root = Path(path).parent
    payload = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    box = payload['box']
    if box['kind'] not in ('windows-local','windows-task','posix-ssh') or box.get('name') == 'Z13' or \
            (box['kind'] == 'windows-local' and box.get('name') != 'EROL-PC') or \
            platform.node().casefold() != box['hostname'].casefold():
        raise ValueError('reservation holder is not on the declared authorized machine')
    if frozen_receipt(Path(__file__).parent.parent) is None:
        raise ValueError('native reservation holder requires the frozen NOTE 11 bundle')
    if box.get('compiler_overlap_row') and (not payload.get('specs') or any(spec.get('acceptance_row') != box['compiler_overlap_row'] for spec in payload['specs'])):
        raise ValueError('compiler-overlap reservation differs from the native row')
    claim = shared = None
    error = None
    try:
        assert_idle_task(box)
        shared = acquire_shared_reservation(box, root, 60)
        claim = acquire_reservation(box, root, 60)
        if claim.get('borrowed'):
            raise RuntimeError('a new row holder cannot borrow an unrelated outer reservation')
        assert_idle_task(box)
        environment = {key:os.environ[key] for key in ('CCCP_FEEL_MATRIX_RUN','CC_ACCEPTANCE_BOX_OWNER') if key in os.environ}
        raw = (json.dumps(environment,sort_keys=True)+'\n').encode('utf-8')
        with (root/ENV_FILE).open('xb') as output:
            output.write(raw)
        deadline = time.monotonic()+1800
        write_json(root/'lease-ready.json',dict(box=box['name'],runner_pid=os.getpid(),
                   environment_sha256=hashlib.sha256(raw).hexdigest(), deadline_monotonic_s=deadline))
        while not (root/'lease-stop.json').is_file():
            if time.monotonic() >= deadline:
                raise TimeoutError('native reservation lifetime expired')
            if not cross.owns_reservation(box):
                raise RuntimeError('native reservation token changed')
            time.sleep(.25)
    except Exception as failure:
        error = str(failure)
        write_json(root/'lease-error.json',dict(error=error))
    finally:
        released = cross.release_reservation(claim) if claim is not None else False
        shared_released = release_shared_reservation(shared) if shared is not None else None
        write_json(root/'lease-released.json',dict(released=released, shared_released=shared_released, error=error))
    return int(error is not None)


@contextmanager
def borrow(box, root):
    root = Path(root)
    key = root/ENV_FILE
    if not key.is_file():
        yield False
        return
    raw = key.read_bytes()
    value = json.loads(raw)
    ready = json.loads((root/'lease-ready.json').read_text(encoding='utf-8'))
    if (root/'lease-released.json').exists() or ready.get('box') != box['name'] or \
            ready.get('environment_sha256') != hashlib.sha256(raw).hexdigest() or \
            not alive(ready.get('runner_pid')) or time.monotonic() >= ready.get('deadline_monotonic_s',0) or \
            not isinstance(value,dict) or set(value)-{'CCCP_FEEL_MATRIX_RUN','CC_ACCEPTANCE_BOX_OWNER'} or \
            not value.get('CCCP_FEEL_MATRIX_RUN'):
        raise ValueError('native outer reservation is missing, expired or changed')
    previous = {key:os.environ.get(key) for key in value}
    try:
        os.environ.update(value)
        if not cross.owns_reservation(box):
            raise ValueError('native outer reservation does not own the physical box')
        cross.assert_box_guard(box)
        yield True
    finally:
        for key,old in previous.items():
            if old is None: os.environ.pop(key,None)
            else: os.environ[key]=old


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hold',type=Path,required=True)
    return hold(parser.parse_args(argv).hold)


if __name__=='__main__':
    raise SystemExit(main())
