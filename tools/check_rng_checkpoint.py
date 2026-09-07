"""Emit a standalone test of the production RNG checkpoint code for each toolchain."""

import argparse
import hashlib
import json
from pathlib import Path


DRIVER = r'''
static uint64_t continuation(std::mt19937 engine) {
    uint64_t hash = 14695981039346656037ULL;
    for (int i = 0; i < 10000; ++i) {
        uint32_t value = static_cast<uint32_t>(engine());
        for (int byte = 0; byte < 4; ++byte) {
            hash = (hash ^ ((value >> (byte * 8)) & 255U)) * 1099511628211ULL;
        }
    }
    return hash;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string mode = argv[1];
    int cases = 0;
    if (mode == "write") {
        std::ofstream out(argv[2], std::ios::binary);
        for (uint64_t seed: {0ULL, 42ULL, 0xffffffffULL, 0xfedcba9876543210ULL}) {
            for (int skip: {0, 1, 226, 227, 396, 397, 623, 624, 625, 9999, 100000}) {
                RandomGenerator source;
                source.Seed(seed);
                for (int i = 0; i < skip; ++i) source.RandomNum<float>();
                out << continuation(source.GetEngineState()) << ' ' << source.SerializeCheckpoint() << '\n';
                ++cases;
            }
        }
        out.close();
        if (!out) return 3;
    } else if (mode == "read") {
        std::ifstream in(argv[2], std::ios::binary);
        if (!in) return 3;
        std::string line;
        while (std::getline(in, line)) {
            const size_t split = line.find(' ');
            const uint64_t expected = std::stoull(line.substr(0, split));
            const std::string saved = line.substr(split + 1);
            RandomGenerator restored;
            if (!restored.RestoreCheckpoint(saved) || restored.SerializeCheckpoint() != saved ||
                continuation(restored.GetEngineState()) != expected) return 4;
            for (const std::string& bad: {std::string(), "MT2" + saved.substr(3),
                                        saved.substr(0, saved.rfind(' ')), saved + " extra"}) {
                if (restored.RestoreCheckpoint(bad) || restored.SerializeCheckpoint() != saved) return 5;
            }
            ++cases;
        }
    } else {
        return 2;
    }
    std::cout << (cases == 44 ? "PASS " : "FAIL ") << mode << ' ' << cases << " checkpoints\n";
    return cases == 44 ? 0 : 6;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    header = repo / "Source/System/RTETools.h"
    source = repo / "Source/System/RTETools.cpp"
    h_text, c_text = header.read_text(), source.read_text()
    declaration = h_text[h_text.index("\tclass RandomGenerator {"):h_text.index("\n\t// Sim/render RNG split:")]
    definitions = c_text[c_text.index("\tnamespace {\n\t\tstruct MTCheckpointSeed"):c_text.index("\n\tvoid SeedRNG()")]
    includes = "\n".join(f"#include <{name}>" for name in (
        "algorithm", "array", "cmath", "cstdint", "fstream", "iostream", "iterator", "locale",
        "memory", "random", "sstream", "string", "string_view", "type_traits"))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(includes + "\nvoid (*g_RNGDrawHook)(uint64_t) = nullptr;\n" + declaration + definitions + DRIVER)
    manifest = {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest() for path in (header, source)}
    manifest[args.out.name] = hashlib.sha256(args.out.read_bytes()).hexdigest()
    args.out.with_suffix(".sources.json").write_text(json.dumps(manifest, indent=2))
    print(args.out)


if __name__ == "__main__":
    main()
