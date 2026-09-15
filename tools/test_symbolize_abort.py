"""Symbolize a known pdb address through tools/symbolize_abort.py."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXE = REPO / "Cortex Command.exe"
TOOL = REPO / "tools" / "symbolize_abort.py"
KNOWN = ("main", "WinMain", "SDL_main")


class TestSymbolizeAbort(unittest.TestCase):
    def test_tool_imports(self):
        import symbolize_abort  # noqa: F401

    def test_known_symbol_from_tip_pdb(self):
        self.assertTrue(EXE.is_file(), f"missing tip exe {EXE}")
        self.assertTrue(TOOL.is_file(), f"missing {TOOL}")
        import symbolize_abort

        names = []
        for name in KNOWN + ("WinMain@16",):
            info = symbolize_abort.lookup_name(str(EXE), name)
            if info is not None:
                names.append((name, info))
        self.assertGreaterEqual(len(names), 1, "SymFromName found no known symbol")
        chosen = names[:3] if len(names) >= 3 else names * 3
        chosen = chosen[:3]
        lines = ["FATAL: unhandled exception 0xC0000005 at 0x0\n"]
        for index, (name, info) in enumerate(chosen):
            lines.append(f"stack[{index}] exe+0x{info['offset']:X} {name}+0x0\n")
        with tempfile.TemporaryDirectory() as tmp:
            abort = Path(tmp) / "AbortCode.txt"
            abort.write_text("".join(lines), encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(TOOL), "--exe", str(EXE), "--abort", str(abort)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            for name, info in chosen:
                self.assertIn(info["name"], proc.stdout)

    def test_fatal_exe_offset_names_main(self):
        self.assertTrue(EXE.is_file(), f"missing tip exe {EXE}")
        self.assertTrue(TOOL.is_file(), f"missing {TOOL}")
        import symbolize_abort

        info = symbolize_abort.lookup_name(str(EXE), "main")
        self.assertIsNotNone(info, "SymFromName found no main")
        abort_text = (
            f"FATAL: unhandled exception 0xC0000005 at 0x7FF700001234 "
            f"(exe+0x{info['offset']:X})\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            abort = Path(tmp) / "AbortCode.txt"
            abort.write_text(abort_text, encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(TOOL), "--exe", str(EXE), "--abort", str(abort)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            fatal_rows = [line for line in proc.stdout.splitlines() if line.startswith("FATAL")]
            self.assertTrue(
                any("main" in row for row in fatal_rows),
                proc.stdout + proc.stderr,
            )


if __name__ == "__main__":
    os.environ.setdefault("CCCP_HEADLESS", "1")
    unittest.main()
