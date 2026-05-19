#include "DeterminismCheck.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace RTE {

	namespace {

		using nlohmann::json;

		struct Args {
			std::string scenario;
			uint64_t    ticks = 600;
			uint64_t    seed = 42;
			int         runs = 10;
			std::string output = "determinism-report.json";
			std::string gameBin;        // override for child-process invocation
			bool        keepRuns = false; // keep tmp/runs/*.json after the diff
			bool        showHelp = false;
		};

		void PrintUsage(std::ostream& out) {
			out <<
			    "Usage: <bin> -determinism-check --scenario <name> [options]\n"
			    "\n"
			    "Spawns the game binary N times with the same seed and diffs the per-tick\n"
			    "BLAKE3 hash traces. Designed to run as a CI step or locally to verify the\n"
			    "M1 determinism-cleanup blocks (B-F) take effect.\n"
			    "\n"
			    "Options (single- or double-dash, value via space):\n"
			    "  --scenario <name>      Activity preset-suffix to run (e.g. M1Baseline).\n"
			    "                         Required. ScenarioRunner prepends \"Trust \" if needed.\n"
			    "  --ticks <N>            Per-run sim-tick cap. Default 600.\n"
			    "  --seed <S>             Deterministic seed for every run. Default 42.\n"
			    "  --runs <R>             Number of independent runs to compare. Default 10.\n"
			    "  --output <path>        JSON divergence report. Default determinism-report.json.\n"
			    "  --game-bin <path>      Override the binary to spawn for each run.\n"
			    "                         Default: argv[0] (this same executable).\n"
			    "  --keep-runs            Keep the per-run JSONs in <tmp>/cccp-determinism/runs/.\n"
			    "                         Default: deleted after diffing.\n"
			    "  -h, --help             Show this help and exit 0.\n";
		}

		// Match a token against both --foo and -foo forms (so -scenario and --scenario both work).
		bool MatchOpt(std::string_view arg, std::string_view longName, std::string_view shortName = {}) {
			if (arg == longName) return true;
			// "--foo" form
			if (arg.size() == longName.size() + 2 && arg.substr(0, 2) == "--" &&
			    arg.substr(2) == longName.substr(arg.substr(2)[0] == '-' ? 0 : 0)) {
				return arg.substr(2) == longName;
			}
			if (!shortName.empty() && arg == shortName) return true;
			return false;
		}

		// Simplified arg matcher: accept both -foo and --foo. Returns true on match.
		bool ArgEq(const std::string& a, const char* name) {
			std::string longForm = std::string("--") + name;
			std::string shortForm = std::string("-") + name;
			return a == longForm || a == shortForm;
		}

		Args ParseArgs(int argc, char** argv) {
			Args r;
			for (int i = 1; i < argc; ++i) {
				const std::string a = argv[i];
				const bool hasValue = (i + 1) < argc;

				// Skip the marker that put us here in the first place.
				if (a == "-determinism-check" || a == "--determinism-check") continue;

				if (a == "-h" || a == "--help") { r.showHelp = true; continue; }
				if (ArgEq(a, "scenario") && hasValue) { r.scenario = argv[++i]; continue; }
				if (ArgEq(a, "ticks") && hasValue) {
					r.ticks = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
					continue;
				}
				if (ArgEq(a, "seed") && hasValue) {
					r.seed = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
					continue;
				}
				if (ArgEq(a, "runs") && hasValue) {
					r.runs = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
					continue;
				}
				if (ArgEq(a, "output") && hasValue) { r.output = argv[++i]; continue; }
				if (ArgEq(a, "game-bin") && hasValue) { r.gameBin = argv[++i]; continue; }
				if (ArgEq(a, "keep-runs")) { r.keepRuns = true; continue; }
				// Unknown args are silently passed through (we tolerate downstream flags like
				// -cout that the user might still want to apply to children).
			}
			return r;
		}

		// Cross-platform quoted command builder. We use std::system, which routes through the
		// platform shell — Windows cmd.exe handles embedded quotes around the program path well
		// enough for our controlled inputs.
		std::string Quote(const std::string& s) {
			if (s.find(' ') == std::string::npos && s.find('"') == std::string::npos) {
				return s;
			}
			std::string out;
			out.reserve(s.size() + 2);
			out += '"';
			for (char c: s) {
				if (c == '"') {
					out += "\\\"";
				} else {
					out += c;
				}
			}
			out += '"';
			return out;
		}

		// Read a JSON file and return it as a value. Returns null on failure.
		json ReadJsonFile(const std::filesystem::path& p) {
			std::ifstream f(p);
			if (!f.is_open()) {
				return json();
			}
			try {
				return json::parse(f);
			} catch (const std::exception& e) {
				std::cerr << "[determinism-check] failed to parse " << p << ": " << e.what() << std::endl;
				return json();
			}
		}

		struct DivergenceReport {
			bool diverged = false;
			uint64_t    firstDivergenceTick = 0;
			// Per-subsystem first-divergence summary (subsystem name → tick where it first diverged).
			std::map<std::string, uint64_t> perSubsystemFirstDivergence;
			// Total mismatched ticks across the trace.
			uint64_t totalMismatchedTicks = 0;
			// How many ticks were actually compared (min over the per-run tick counts).
			uint64_t comparedTicks = 0;
			// Per-run tick counts (for diagnostics — divergent run lengths is itself a flag).
			std::vector<uint64_t> perRunTickCount;
		};

	} // namespace

	bool DeterminismCheck::IsRequested(int argc, char** argv) {
		for (int i = 1; i < argc; ++i) {
			if (argv[i] == nullptr) {
				continue;
			}
			const std::string a = argv[i];
			if (a == "-determinism-check" || a == "--determinism-check") {
				return true;
			}
		}
		return false;
	}

	int DeterminismCheck::Run(int argc, char** argv) {
		const Args args = ParseArgs(argc, argv);

		if (args.showHelp) {
			PrintUsage(std::cout);
			return 0;
		}
		if (args.scenario.empty()) {
			std::cerr << "[determinism-check] --scenario is required.\n";
			PrintUsage(std::cerr);
			return 2;
		}
		if (args.runs < 2) {
			std::cerr << "[determinism-check] --runs must be >= 2 (cannot diff a single run).\n";
			return 2;
		}
		if (args.ticks == 0) {
			std::cerr << "[determinism-check] --ticks must be > 0.\n";
			return 2;
		}

		// Resolve the binary to spawn: --game-bin > argv[0] absolute.
		std::filesystem::path binary;
		if (!args.gameBin.empty()) {
			binary = std::filesystem::path(args.gameBin);
		} else if (argc > 0 && argv[0] != nullptr) {
			binary = std::filesystem::absolute(std::filesystem::path(argv[0]));
		} else {
			std::cerr << "[determinism-check] cannot resolve binary path (argv[0] unavailable).\n";
			return 2;
		}
		if (!std::filesystem::exists(binary)) {
			std::cerr << "[determinism-check] binary does not exist: " << binary.string() << "\n";
			return 2;
		}

		// Temp dir for the per-run JSONs. We pick a process-unique subdir so concurrent CI
		// jobs don't stomp on each other.
		const auto now = std::chrono::system_clock::now().time_since_epoch().count();
		const std::filesystem::path tmpRoot =
		    std::filesystem::temp_directory_path() / ("cccp-determinism-" + std::to_string(now));
		std::filesystem::create_directories(tmpRoot);

		std::cout << "[determinism-check] binary  : " << binary.string() << "\n";
		std::cout << "[determinism-check] scenario: " << args.scenario << "\n";
		std::cout << "[determinism-check] ticks   : " << args.ticks << "\n";
		std::cout << "[determinism-check] seed    : " << args.seed << "\n";
		std::cout << "[determinism-check] runs    : " << args.runs << "\n";
		std::cout << "[determinism-check] tmp dir : " << tmpRoot.string() << "\n";

		// Run the binary N times with the same scenario+seed+ticks. -tick-hashes makes the
		// per-tick hash trace land in the JSON. -out routes the report.
		std::vector<std::filesystem::path> runOutputs;
		runOutputs.reserve(static_cast<size_t>(args.runs));
		for (int i = 0; i < args.runs; ++i) {
			std::filesystem::path runOut = tmpRoot / ("run_" + std::to_string(i) + ".json");
			std::ostringstream cmd;
			cmd << Quote(binary.string())
			    << " -scenario " << Quote(args.scenario)
			    << " -seed " << args.seed
			    << " -max-ticks " << args.ticks
			    << " -tick-hashes"
			    << " -out " << Quote(runOut.string());

			std::cout << "[determinism-check] run " << (i + 1) << "/" << args.runs
			          << ": " << cmd.str() << std::endl;
			const int rc = std::system(cmd.str().c_str());
			// The scenario itself may legitimately exit non-zero (a fail-result trust scenario).
			// What we really care about is whether the JSON was produced and parseable.
			if (rc < 0) {
				std::cerr << "[determinism-check] run " << i << " failed to spawn (rc=" << rc << ")\n";
				return 2;
			}
			if (!std::filesystem::exists(runOut)) {
				std::cerr << "[determinism-check] run " << i << " produced no JSON output: "
				          << runOut.string() << "\n";
				return 2;
			}
			runOutputs.push_back(runOut);
		}

		// Read every run's JSON and compute the per-tick comparison.
		std::vector<json> jsons;
		jsons.reserve(runOutputs.size());
		for (const auto& p: runOutputs) {
			json j = ReadJsonFile(p);
			if (j.is_null()) {
				std::cerr << "[determinism-check] run " << p.string() << " produced unparseable JSON.\n";
				return 2;
			}
			jsons.push_back(std::move(j));
		}

		// Pull the tick_hashes array out of each run. Schema is documented in MetricsCollector:
		//   runs[0].tick_hashes = [ { tick: N, total: "hex", subsystems: { "name": "hex", ... } }, ... ]
		auto extractTickHashes = [](const json& j) -> json {
			if (!j.contains("runs") || !j["runs"].is_array() || j["runs"].empty()) return json();
			const json& run0 = j["runs"][0];
			if (!run0.contains("tick_hashes") || !run0["tick_hashes"].is_array()) return json();
			return run0["tick_hashes"];
		};

		std::vector<json> allTickHashes;
		allTickHashes.reserve(jsons.size());
		for (size_t i = 0; i < jsons.size(); ++i) {
			json th = extractTickHashes(jsons[i]);
			if (th.is_null() || !th.is_array() || th.empty()) {
				std::cerr << "[determinism-check] run " << i
				          << " contained no tick_hashes array — was -tick-hashes plumbed through?\n";
				return 2;
			}
			allTickHashes.push_back(std::move(th));
		}

		DivergenceReport rep;
		rep.perRunTickCount.reserve(allTickHashes.size());
		uint64_t minTicks = UINT64_MAX;
		for (const auto& th: allTickHashes) {
			const uint64_t n = static_cast<uint64_t>(th.size());
			rep.perRunTickCount.push_back(n);
			if (n < minTicks) minTicks = n;
		}
		rep.comparedTicks = (minTicks == UINT64_MAX) ? 0 : minTicks;

		// Walk tick-by-tick. The runs' tick arrays are in order, so index t corresponds to tick t.
		bool foundFirstDivergence = false;
		for (uint64_t t = 0; t < rep.comparedTicks; ++t) {
			const json& ref = allTickHashes[0][static_cast<size_t>(t)];
			const std::string refTotal = ref.value("total", std::string());
			bool tickDivergent = false;
			for (size_t r = 1; r < allTickHashes.size(); ++r) {
				const json& cur = allTickHashes[r][static_cast<size_t>(t)];
				const std::string curTotal = cur.value("total", std::string());
				if (curTotal != refTotal) {
					tickDivergent = true;
				}
				// Per-subsystem walk — record the FIRST tick each subsystem diverges so the
				// summary fingers the culprit even when total diverged for many reasons.
				if (ref.contains("subsystems") && cur.contains("subsystems")) {
					const json& refSubs = ref["subsystems"];
					const json& curSubs = cur["subsystems"];
					for (auto it = refSubs.begin(); it != refSubs.end(); ++it) {
						const std::string name = it.key();
						const std::string refHex = it.value().is_string() ? it.value().get<std::string>() : "";
						std::string curHex;
						if (curSubs.contains(name) && curSubs[name].is_string()) {
							curHex = curSubs[name].get<std::string>();
						}
						if (refHex != curHex) {
							auto inserted = rep.perSubsystemFirstDivergence.emplace(name, t);
							(void)inserted; // emplace keeps the earliest tick for each subsystem
						}
					}
				}
			}
			if (tickDivergent) {
				++rep.totalMismatchedTicks;
				if (!foundFirstDivergence) {
					rep.firstDivergenceTick = t;
					foundFirstDivergence = true;
				}
			}
		}
		rep.diverged = foundFirstDivergence;

		// Write the divergence report. Schema is small and stable — read by humans and CI scripts.
		json reportJson;
		reportJson["scenario"] = args.scenario;
		reportJson["seed"] = args.seed;
		reportJson["ticks_requested"] = args.ticks;
		reportJson["runs"] = args.runs;
		reportJson["compared_ticks"] = rep.comparedTicks;
		reportJson["diverged"] = rep.diverged;
		reportJson["total_mismatched_ticks"] = rep.totalMismatchedTicks;
		if (rep.diverged) {
			reportJson["first_divergence_tick"] = rep.firstDivergenceTick;
			json subs = json::object();
			for (const auto& [name, tick]: rep.perSubsystemFirstDivergence) {
				subs[name] = tick;
			}
			reportJson["per_subsystem_first_divergence"] = subs;
		}
		json perRun = json::array();
		for (uint64_t n: rep.perRunTickCount) perRun.push_back(n);
		reportJson["per_run_tick_count"] = perRun;
		reportJson["binary"] = binary.string();

		std::ofstream out(args.output);
		if (!out.is_open()) {
			std::cerr << "[determinism-check] failed to open output: " << args.output << "\n";
			return 2;
		}
		out << reportJson.dump(2) << std::endl;
		out.close();

		std::cout << "\n[determinism-check] wrote report: " << args.output << "\n";
		if (rep.diverged) {
			std::cout << "[determinism-check] RESULT: DIVERGED\n";
			std::cout << "    first_divergence_tick: " << rep.firstDivergenceTick << "\n";
			std::cout << "    total_mismatched_ticks: " << rep.totalMismatchedTicks
			          << " / " << rep.comparedTicks << "\n";
			std::cout << "    per-subsystem first divergence:\n";
			for (const auto& [name, tick]: rep.perSubsystemFirstDivergence) {
				std::cout << "        " << name << ": tick " << tick << "\n";
			}
		} else {
			std::cout << "[determinism-check] RESULT: MATCHED (" << rep.comparedTicks
			          << " ticks across " << args.runs << " runs)\n";
		}

		// Cleanup per-run JSONs unless --keep-runs.
		if (!args.keepRuns) {
			std::error_code ec;
			std::filesystem::remove_all(tmpRoot, ec);
		} else {
			std::cout << "[determinism-check] per-run JSONs kept at: " << tmpRoot.string() << "\n";
		}

		return rep.diverged ? 1 : 0;
	}

} // namespace RTE
