"""A spinning particle shows the sprite frame Windows and x86 Linux show for its angle, on every platform."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


PROGRAM = r'''
#include "ParticleFrame.h"
#include <bit>
#include <cstdint>
#include <cstdio>

int main() {
    // Angles whose place in the turn rounds a hair below zero: the end of the previous turn, its last frame.
    const uint32_t turnEnds[] = {0x44dbe957, 0xc5579781, 0x45603b2e, 0xc572b024};
    for (unsigned int frameCount : {4u, 8u}) {
        for (uint32_t bits : turnEnds) {
            const unsigned int frame = RTE::ParticleTurnFrame(std::bit_cast<float>(bits), frameCount);
            if (frame != frameCount - 1) {
                std::printf("angle_bits=0x%08x frames=%u frame=%u, the turn's last frame is %u\n", bits, frameCount, frame, frameCount - 1);
                return 1;
            }
        }
    }
    // Every 97th float angle from 1 to 32768 radians, both signs: the frames x86 picks, folded.
    const struct { unsigned int frameCount; uint64_t digest; } sweeps[] = {{4, 0x3719926190bbd023ULL}, {8, 0x6e7c31bbfd66b6b8ULL}};
    for (const auto& sweep : sweeps) {
        uint64_t digest = 1469598103934665603ULL;
        for (uint32_t bits = 0x3f800000u; bits < 0x47000000u; bits += 97) {
            for (uint32_t sign : {0u, 0x80000000u}) {
                digest = (digest ^ RTE::ParticleTurnFrame(std::bit_cast<float>(bits | sign), sweep.frameCount)) * 1099511628211ULL;
            }
        }
        if (digest != sweep.digest) {
            std::printf("frames=%u sweep digest %016llx, x86 picks %016llx\n", sweep.frameCount,
                        static_cast<unsigned long long>(digest), static_cast<unsigned long long>(sweep.digest));
            return 2;
        }
    }
    std::puts("PASS turn-end angles and the x86 frame sweep");
}
'''


class ParticleFrameTest(unittest.TestCase):
    def test_turn_end_angles_and_the_x86_sweep(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if compiler is None:
            self.skipTest("C++ compiler is absent")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binary = root / "frame.cpp", root / "frame"
            source.write_text(PROGRAM, encoding="utf-8")
            include = Path(__file__).resolve().parents[1] / "Source/System"
            build = subprocess.run([compiler, "-std=c++20", "-O2", "-fno-fast-math", "-ffp-contract=off",
                                    "-I", str(include), str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS turn-end angles", result.stdout)


if __name__ == "__main__":
    unittest.main()
