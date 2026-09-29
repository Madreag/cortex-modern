"""Run the shared tools suites on POSIX, preserving N/A for Windows-only external paths.

The shared driver uses Path.is_absolute(), which interprets D:/... as a relative path on
POSIX. Normalize those external inventory paths before handing its suite list back to it.
Repository suites and every available external suite still run through the shared driver.
"""

import importlib.util
from pathlib import Path, PureWindowsPath
import sys


def main():
    path = Path(__file__).resolve().parents[1] / "run_tools_suites.py"
    spec = importlib.util.spec_from_file_location("shared_tools_suites", path)
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    if sys.platform != "win32":
        suites = []
        for name, arguments in driver.SUITES:
            script, *rest = arguments
            if PureWindowsPath(script).is_absolute() and not Path(script).is_absolute():
                script = str(Path("/") / script)
            suites.append((name, [script, *rest]))
        driver.SUITES = tuple(suites)
    return driver.main()


if __name__ == "__main__":
    raise SystemExit(main())
