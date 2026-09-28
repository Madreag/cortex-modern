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


def install_memory_guard():
    if sys.platform != 'win32':
        return
    import win32_test_runner
    original = win32_test_runner.IsolatedRun.start
    if getattr(original, '_memory_guard', False):
        return
    def start(run):
        marker = Path('D:/mx/FEEL-MATRIX-RUNNING')
        if marker.is_file() and json.loads(marker.read_text(encoding='utf-8')).get('token') != os.environ.get('CCCP_FEEL_MATRIX_RUN'):
            raise RuntimeError('engine launch refused: another lane owns the feel matrix marker')
        if Path('D:/mx/BOX-FREE-FOR-CROSS').exists():
            raise RuntimeError('engine launch refused: the box is reserved for the cross match')
        free = free_memory_bytes()
        if free < MIN_FREE_BYTES:
            raise RuntimeError(f'engine launch refused: {free} free bytes, requires {MIN_FREE_BYTES}')
        result = original(run)
        run.record['launch_budget'] = dict(free_bytes=free, required_bytes=MIN_FREE_BYTES)
        run._save()
        return result
    start._memory_guard = True
    win32_test_runner.IsolatedRun.start = start


@contextmanager
def exclusive_matrix():
    marker = Path('D:/mx/FEEL-MATRIX-RUNNING')
    token = f'{os.getpid()}-netcode-feel'
    if Path('D:/mx/BOX-FREE-FOR-CROSS').exists():
        raise RuntimeError('the box is reserved for the cross match')
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
