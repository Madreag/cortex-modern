"""The native Windows capacity counter must enumerate real processes without WMI."""
import ast
import ctypes
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest

import spread_peers


@unittest.skipUnless(sys.platform == "win32", "Windows process counter")
class NativeProcesses(unittest.TestCase):
    def test_real_processes_and_parent_without_wmi(self):
        worker = os.environ.get("CORTEX_NATIVE_WORKER")
        if not worker:
            self.skipTest("CORTEX_NATIVE_WORKER must name the installed native worker")
        source = spread_peers.native_cpu_wait_source(Path(worker).read_text(encoding="utf-8"))
        tree = ast.parse(source)
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                     and node.name in ("processes", "native_windows_processes")]

        def refuse_wmi(*args, **kwargs):
            raise AssertionError("the capacity counter invoked WMI process enumeration")

        scope = dict(sys=sys, ctypes=ctypes, subprocess=SimpleNamespace(
            run=refuse_wmi, CREATE_NO_WINDOW=subprocess.CREATE_NO_WINDOW))
        exec(compile(ast.Module(body=functions, type_ignores=[]), "native-process-counter", "exec"), scope)
        rows = scope["processes"]()
        self.assertIn(os.getpid(), {row["pid"] for row in rows})
        with subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"],
                              creationflags=subprocess.CREATE_NO_WINDOW) as child:
            try:
                row = next(row for row in scope["processes"]() if row["pid"] == child.pid)
                self.assertEqual(row["parent"], os.getpid())
                self.assertEqual(Path(row["name"]).name.casefold(), Path(sys.executable).name.casefold())
            finally:
                child.terminate()
                child.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
