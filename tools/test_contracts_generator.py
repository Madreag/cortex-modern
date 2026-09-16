"""The generated ContractAudit.h must stay byte-identical to the committed header."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE / "contracts"))

from generate_observer import main as generate_main


class GeneratedHeader(unittest.TestCase):
    def test_header_only_matches_committed_bytes(self):
        """A regenerated header that differs in any byte from the committed file is a fail."""
        committed = (REPO / "Source" / "System" / "ContractAudit.h").read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ContractAudit.h"
            buf = io.StringIO()
            with redirect_stdout(buf):
                code = generate_main(["--repo", str(REPO), "--header-only", "--output", str(out)])
            self.assertEqual(code, 0, buf.getvalue())
            self.assertEqual(out.read_bytes(), committed)


if __name__ == "__main__":
    unittest.main()
