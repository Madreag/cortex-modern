"""Compile and test typed task failures and an empty runtime exception capture.

Run from a compiler environment (for MSVC, an x64 Native Tools prompt). The
ordinary build uses exact headers from --revision. The second build substitutes
two observation seams in private copies: current_exception can return empty,
and the promise setter records/rejects a null argument before undefined behavior.
Both builds throw real std::bad_alloc inside full/light pool tasks, for void and
value futures. The empty-capture injection models foreign runtime unwinding; it
does not claim that a normal C++ bad_alloc produces a null exception pointer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--revision', default='HEAD')
    p.add_argument('--cxx', default='cl' if os.name == 'nt' else 'c++')
    p.add_argument('--fixture', type=Path, default=Path(__file__).with_name('fixtures') / 'ThreadPoolExceptions.cpp')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    revision = subprocess.check_output(['git', '-C', str(a.repo), 'rev-parse', a.revision], text=True).strip()
    driver = a.out / 'driver-as-run.py'
    driver.write_bytes(Path(__file__).read_bytes())
    source = a.out / 'ThreadPoolExceptions.cpp'
    source.write_bytes(a.fixture.read_bytes())
    headers = ('BS_thread_pool.hpp', 'BS_thread_pool_light.hpp')
    receipt = {'revision': revision, 'driver_sha256': hashlib.sha256(driver.read_bytes()).hexdigest(), 'fixture_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
               'headers': {}, 'builds': []}
    for mode in ('ordinary', 'empty-capture'):
        directory = a.out / mode
        directory.mkdir()
        for name in headers:
            path = 'external/include/thread-pool-3.5.0/include/' + name
            original = subprocess.check_output(['git', '-C', str(a.repo), 'show', f'{revision}:{path}'])
            receipt['headers'][name] = hashlib.sha256(original).hexdigest()
            text = original.decode()
            if mode == 'empty-capture':
                # Exactly one runtime-capture expression and promise-setter boundary per header.
                assert text.count('std::current_exception()') == 1, name
                assert text.count('task_promise->set_exception(') == 1, name
                text = text.replace('std::current_exception()', '::FailureProbe::CurrentException()')
                text = text.replace('task_promise->set_exception(', '::FailureProbe::SetException(*task_promise, ')
                (directory / name).write_text(text)
            else:
                (directory / name).write_bytes(original)
        exe = directory / ('pool-exceptions.exe' if os.name == 'nt' else 'pool-exceptions')
        if Path(a.cxx).name.lower() in ('cl', 'cl.exe'):
            command = [a.cxx, '/nologo', '/std:c++20', '/EHsc', '/O2', '/MD', '/W4', '/I' + str(directory),
                       str(source), '/Fe:' + str(exe), '/Fo:' + str(directory / 'test.obj')]
            if mode == 'empty-capture': command += ['/DINJECT_EMPTY_EXCEPTION']
        else:
            command = [a.cxx, '-std=c++20', '-O2', '-pthread', '-Wall', '-Wextra', '-I', str(directory),
                       str(source), '-o', str(exe)]
            if mode == 'empty-capture': command += ['-DINJECT_EMPTY_EXCEPTION']
        built = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (directory / 'build.log').write_text(built.stdout + built.stderr)
        row = {'mode': mode, 'command': command, 'build_exit': built.returncode}
        if built.returncode == 0:
            try:
                run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
                (directory / 'stdout.log').write_text(run.stdout + run.stderr)
                row.update(exit_code=run.returncode, timed_out=False,
                           exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
                print(run.stdout, end='', flush=True)
            except subprocess.TimeoutExpired as error:
                row.update(exit_code=None, timed_out=True)
                (directory / 'timeout.txt').write_text(str(error))
        receipt['builds'].append(row)
        (a.out / 'result.json').write_text(json.dumps(receipt, indent=2))
    receipt['pass'] = all(r['build_exit'] == 0 and r.get('exit_code') == 0 and not r.get('timed_out') for r in receipt['builds'])
    (a.out / 'result.json').write_text(json.dumps(receipt, indent=2))
    print(json.dumps(receipt))
    return 0 if receipt['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
