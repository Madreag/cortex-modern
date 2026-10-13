"""Ray strength keeps the same integer result on x86 and arm64, including overflow."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


PROGRAM = r'''
#include "DeterministicMath.h"
#include <bit>
#include <cstdio>
#include <initializer_list>

int main() {
    struct Case { uint32_t bits; int32_t expected; };
    const Case cases[] = {
        {0x00000000, 0}, {0x80000000, 0}, {0x00000001, 0},
        {0x3f400000, 0}, {0x3fe00000, 1}, {0xbfe00000, -1},
        {0x41c80000, 25}, {0x4effffff, 2147483520},
        {0x4f000000, INT32_MIN}, {0xcf000000, INT32_MIN},
        {0xcf000001, INT32_MIN}, {0x7f7fffff, INT32_MIN},
        {0xff7fffff, INT32_MIN}, {0x7f800000, INT32_MIN},
        {0xff800000, INT32_MIN}, {0x7fc00000, INT32_MIN}
    };
    for (const auto& row : cases) {
        volatile float input = std::bit_cast<float>(row.bits);
        const auto actual = RTE::DeterministicFloatToInt(input);
        if (actual != row.expected) {
            std::printf("bits=%08x expected=%d actual=%d\n", row.bits, row.expected, actual);
            return 1;
        }
    }
    // S10 tick 3398: material 1 must retain the existing x86 ray decision.
    volatile float unbounded = std::bit_cast<float>(uint32_t{0x7f7fffff});
    int strength = RTE::DeterministicFloatToInt(0.0F + unbounded);
    if (strength != INT32_MIN || strength >= 25) return 2;
    strength = RTE::DeterministicFloatToInt(static_cast<float>(strength) + 70.0F);
    if (strength != -2147483520 || strength >= 25) return 3;
    // Keep truncation at each visit, rather than accumulating fractional strength.
    strength = 0;
    for (float integrity : {0.75F, 0.75F, 1.75F}) {
        strength = RTE::DeterministicFloatToInt(static_cast<float>(strength) + integrity);
    }
    if (strength != 1) return 4;
    std::puts("PASS 16 conversion cases and the captured ray decisions");
}
'''


class FloatToIntTest(unittest.TestCase):
    def test_conversion_boundaries_and_ray_decision(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if compiler is None:
            self.skipTest("C++ compiler is absent")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binary = root / "conversion.cpp", root / "conversion"
            source.write_text(PROGRAM, encoding="utf-8")
            include = Path(__file__).resolve().parents[1] / "Source/System"
            build = subprocess.run([compiler, "-std=c++20", "-O2", "-fno-fast-math", "-ffp-contract=off",
                                    "-I", str(include), str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS 16 conversion cases", result.stdout)


if __name__ == "__main__":
    unittest.main()
