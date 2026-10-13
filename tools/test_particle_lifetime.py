"""Particle decay keeps the captured Windows lifetime and its smoke expiry on every platform."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


PROGRAM = r'''
#include "ParticleLifetime.h"
#include <bit>
#include <cstdio>

int main() {
    struct Case { uint32_t lifetime, age, randomBits, expected; };
    const Case cases[] = {
        {577, 0, 0x3f000000, 289},
        {577, 576, 0x3f000000, 577},
        {577, 577, 0x3f000000, 577},
        {500, 501, 0, 500},
        {500, 501, 0x3f000000, 2147484148U},
        {0, 0, 0x3f000000, 0},
        // Captured S10 lifetime/age/RNG inputs and Windows outputs, ticks 3849 onward.
        {577, 583, 1056054400, 2263990849U},
        {469, 483, 1050741269, 2944071509U},
        {398, 399, 1031031424, 4038809998U},
        {547, 549, 1037864068, 3832443299U},
        {324, 333, 1044455076, 3484964932U},
        {289, 299, 1053014185, 2653138081U}
    };
    for (const auto& row : cases) {
        volatile float random = std::bit_cast<float>(row.randomBits);
        const auto actual = RTE::DecayParticleLifetime(row.lifetime, row.age, random);
        if (actual != row.expected) {
            std::printf("lifetime=%u age=%u random_bits=%u expected=%u actual=%u\n",
                        row.lifetime, row.age, row.randomBits, row.expected, actual);
            return 1;
        }
    }
    // RepeaterShot scales its smoke lifetime by the dying projectile's lifetime.
    const auto lifetime = RTE::DecayParticleLifetime(500, 516, 0.25F);
    if (lifetime != 3221225972U) return 2;
    const int smoke = static_cast<int>(400.0 / (1.0 + 516.646 / lifetime));
    if (smoke != 399 || !(399.984 > smoke) || 383.318 > smoke) return 3;
    std::puts("PASS lifetime boundaries, captured decay outputs and smoke expiry");
}
'''


class ParticleLifetimeTest(unittest.TestCase):
    def test_captured_decay_and_smoke_expiry(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if compiler is None:
            self.skipTest("C++ compiler is absent")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binary = root / "lifetime.cpp", root / "lifetime"
            source.write_text(PROGRAM, encoding="utf-8")
            include = Path(__file__).resolve().parents[1] / "Source/System"
            build = subprocess.run([compiler, "-std=c++20", "-O2", "-fno-fast-math", "-ffp-contract=off",
                                    "-I", str(include), str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS lifetime boundaries", result.stdout)


if __name__ == "__main__":
    unittest.main()
