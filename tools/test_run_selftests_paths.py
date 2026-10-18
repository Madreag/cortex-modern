"""Repository CLI paths must survive the engine's isolated working directory."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import run_selftests as suite


class RepositoryPathContract(unittest.TestCase):
    def test_relative_repo_input_opens_from_the_private_runtime(self):
        with tempfile.TemporaryDirectory() as scratch:
            base = Path(scratch)
            repo = base / 'checkout with spaces'
            fixtures = repo / 'tools/pie_lockstep/fixtures'
            fixtures.mkdir(parents=True)
            payload = b'press 27 PIE_MENU_ACTIVE\n'
            (fixtures / 'pie_open_then_next.txt').write_bytes(payload)
            (fixtures / 'PieSwitchSP.lua').write_text('-- fixture staging probe\n')
            executable = repo / 'engine'
            executable.write_bytes(b'not an engine')
            opened = []

            class Run:
                def __init__(self, args, out):
                    self.args = args
                    self.cwd = out / 'runtime'
                    (self.cwd / 'Userdata').mkdir(parents=True)

                def start(self):
                    supplied = Path(self.args[self.args.index('-input-script') + 1])
                    target = supplied if supplied.is_absolute() else self.cwd / supplied
                    try:
                        opened.append(target.read_bytes())
                    except FileNotFoundError:
                        opened.append(None)
                    raise RuntimeError('file-open probe complete; no engine launched')

                def close(self):
                    pass

            runner = types.ModuleType('run_sim_test')
            runner.make_run = lambda repo, args, out, timeout, **kwargs: Run(args, out)
            argv = ['run_selftests.py', '--repo', str(repo.relative_to(base)),
                    '--out', str(base / 'results'), '--only', 'single-player-reference']
            with contextlib.chdir(base), patch.object(sys, 'argv', argv), \
                    patch.object(suite, 'engine_executable', return_value=executable), \
                    patch.dict(sys.modules, run_sim_test=runner), \
                    contextlib.redirect_stdout(io.StringIO()):
                suite.main()
            self.assertEqual(opened, [payload], 'the unchanged fixture must open from the isolated runtime')


if __name__ == '__main__':
    unittest.main()
