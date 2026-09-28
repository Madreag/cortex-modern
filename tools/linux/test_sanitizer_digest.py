"""Keep vendor suppression accounting separate from engine findings."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import sanitizer_digest as digest


class SanitizerDigestTest(unittest.TestCase):
    def test_stripped_frame_keeps_its_module_and_offset(self):
        first = digest.FRAME.match("    #1 0x7c0a7b18aed9  (/vendor/libfmod.so.13+0xfced9) (BuildId: e2427658b42a9348)")
        second = digest.FRAME.match("    #1 0x7c1a7b18aed9  (/vendor/libfmod.so.13+0xfced9) (BuildId: e2427658b42a9348)")
        self.assertEqual(digest.frame_text(first), "/vendor/libfmod.so.13+0xfced9")
        self.assertEqual(digest.frame_text(first), digest.frame_text(second))

    def test_paths_with_spaces_and_engine_callers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "stderr.log"
            log.write_text(
                "../external/sources/allegro 4.4.3.1-custom/src/file.c:613:11: runtime error: "
                "call to function utf8_getx through pointer to incorrect function type 'int (*)(const char **)'\n"
                "    #0 utf8_getx external/sources/allegro 4.4.3.1-custom/src/unicode.c:346:1\n\n"
                "../Source/Lua/LuaAdapters.cpp:158:1: runtime error: "
                "call to function f through pointer to incorrect function type 'bool (*)(const void *)'\n"
                "    #0 f Source/Lua/LuaAdapters.cpp:158:1\n\n"
                "../external/sources/luabind-0.7.1/luabind/function.hpp:347:26: runtime error: "
                "call to function RTE::LuaAdaptersEntityCast::IsAHuman through pointer to incorrect function type\n"
                "    #0 f external/sources/luabind-0.7.1/luabind/function.hpp:347:26\n"
            )
            self.assertEqual(len(list(digest.reports(log))), 3)
            subprocess.run([sys.executable, digest.__file__, str(root)], check=True, capture_output=True)
            result = json.loads((root / "sanitizer-digest.json").read_text())
            self.assertEqual(result["reports"], {"ubsan": 2})
            self.assertEqual(result["suppressed_reports"], {"ubsan": 1})
            self.assertEqual(result["findings"], 2)
            self.assertIn("file.c:613:11", result["suppressed_items"][0]["headline"])


if __name__ == "__main__":
    unittest.main()
