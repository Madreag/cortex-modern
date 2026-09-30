"""Enforce the lane's free-memory floor before each private-desktop engine launch."""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import runpy
import sys
from contextlib import contextmanager

MIN_FREE_BYTES = 10 * 1024 ** 3


MARKER = Path('D:/mx/FEEL-MATRIX-RUNNING')
CROSS_GUARD = Path('D:/mx/BOX-FREE-FOR-CROSS')

def free_memory_bytes():
    if sys.platform != 'win32':
        raise RuntimeError('the Windows launch budget needs Windows memory counters')
    class MemoryStatus(ctypes.Structure):
        _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32),
                    *[(name, ctypes.c_uint64) for name in ('total', 'available', 'page_total', 'page_available',
                                                          'virtual_total', 'virtual_available', 'extended')]]
    status = MemoryStatus()
    status.length = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise ctypes.WinError()
    return status.available


def holds_marker():
    """Whether this process holds the box: the feel marker carries the token in its environment."""
    token = os.environ.get('CCCP_FEEL_MATRIX_RUN')
    if not token or not MARKER.is_file():
        return False
    try:
        return json.loads(MARKER.read_text(encoding='utf-8')).get('token') == token
    except (OSError, ValueError):
        return False


def install_memory_guard():
    if sys.platform != 'win32':
        return
    import win32_test_runner
    original = win32_test_runner.IsolatedRun.start
    if getattr(original, '_memory_guard', False):
        return
    def start(run):
        def refuse(reason, **budget):
            run.record['launch_budget'] = dict(refused=reason, **budget)
            run._save()
            raise RuntimeError(reason)
        marker = MARKER
        if marker.is_file() and json.loads(marker.read_text(encoding='utf-8')).get('token') != os.environ.get('CCCP_FEEL_MATRIX_RUN'):
            refuse('engine launch refused: another lane owns the feel matrix marker', marker=str(marker))
        # The cross driver claims the marker before its own local launch; only a process without it is kept off the reserved box.
        if CROSS_GUARD.exists() and not holds_marker():
            refuse('engine launch refused: the box is reserved for the cross match')
        free = free_memory_bytes()
        if free < MIN_FREE_BYTES:
            refuse(f'engine launch refused: {free} free bytes, requires {MIN_FREE_BYTES}', free_bytes=free, required_bytes=MIN_FREE_BYTES)
        result = original(run)
        run.record['launch_budget'] = dict(free_bytes=free, required_bytes=MIN_FREE_BYTES)
        run._save()
        return result
    start._memory_guard = True
    win32_test_runner.IsolatedRun.start = start


@contextmanager
def exclusive_matrix():
    marker = MARKER
    token = f'{os.getpid()}-netcode-feel'
    if CROSS_GUARD.exists() and not holds_marker():
        raise RuntimeError('the box is reserved for the cross match')
    # A caller that already holds the box (its token in the environment) runs the matrix inside its own reservation,
    # which stays for that caller to release.
    inherited = os.environ.get('CCCP_FEEL_MATRIX_RUN')
    if inherited and marker.is_file() and json.loads(marker.read_text(encoding='utf-8')).get('token') == inherited:
        yield
        return
    with marker.open('x', encoding='utf-8') as stream:
        json.dump(dict(pid=os.getpid(), token=token, note='exclusive feel matrix'), stream)
    previous = os.environ.get('CCCP_FEEL_MATRIX_RUN')
    os.environ['CCCP_FEEL_MATRIX_RUN'] = token
    try:
        yield
    finally:
        if marker.is_file() and json.loads(marker.read_text(encoding='utf-8')).get('token') == token:
            marker.unlink()
        if previous is None:
            os.environ.pop('CCCP_FEEL_MATRIX_RUN', None)
        else:
            os.environ['CCCP_FEEL_MATRIX_RUN'] = previous


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    install_memory_guard()
    target = Path(sys.argv[1]).resolve()
    sys.argv = [str(target), *sys.argv[2:]]
    runpy.run_path(str(target), run_name='__main__')
