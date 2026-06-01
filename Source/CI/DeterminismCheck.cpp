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
			// Positive control. Forwarded verbatim to every child run; the diff logic
			// below NEVER reads it — divergence is reported only from the compared hashes.
			bool        selftestPerturb = false;
			// Thread-count matrix. When non-empty, the scenario is run at each
			// listed Lua-state count and the per-tick traces are diffed across counts.
			std::vector<int> threadCounts;
		};

		void PrintUsage(std::ostream& out) {
			out <<
			    "Usage: <bin> -determinism-check --scenario <name> [options]\n"
			    "\n"
			    "Spawns the game binary N times with the same seed and diffs the per-tick\n"
			    "hash traces. Designed to run as a CI step or locally to verify the sim\n"
			    "stays deterministic.\n"
			    "\n"
			    "Options (single- or double-dash, value via space):\n"
			    "  --scenario <name>      Activity preset-suffix to run (e.g. SimBaseline).\n"
			    "                         Required. ScenarioRunner prepends \"Determinism \" if needed.\n"
			    "  --ticks <N>            Per-run sim-tick cap. Default 600.\n"
			    "  --seed <S>             Deterministic seed for every run. Default 42.\n"
			    "  --runs <R>             Number of independent runs to compare. Default 10.\n"
			    "                         With --threads, this is runs-PER-thread-count.\n"
			    "  --threads <list>       Thread-count matrix: comma-separated Lua-state\n"
			    "                         counts, e.g. 1,2,4,8,16. Runs the scenario at each\n"
			    "                         count and diffs the per-tick traces ACROSS counts.\n"
			    "                         Empty (default) = repeat-runs mode at the engine\n"
			    "                         default count.\n"
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

		// Parse a comma-separated integer list ("1,2,4,8,16") into a vector. Bad tokens are skipped.
		std::vector<int> ParseIntList(const std::string& csv) {
			std::vector<int> out;
			std::stringstream ss(csv);
			std::string token;
			while (std::getline(ss, token, ',')) {
				if (token.empty()) continue;
				char* end = nullptr;
				const long v = std::strtol(token.c_str(), &end, 10);
				if (end != token.c_str() && v >= 1) {
					out.push_back(static_cast<int>(v));
				}
			}
			return out;
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
				if (ArgEq(a, "threads") && hasValue) {
					r.threadCounts = ParseIntList(argv[++i]);
					continue;
				}
				if (ArgEq(a, "output") && hasValue) { r.output = argv[++i]; continue; }
				if (ArgEq(a, "game-bin") && hasValue) { r.gameBin = argv[++i]; continue; }
				if (ArgEq(a, "keep-runs")) { r.keepRuns = true; continue; }
				if (a == "-determinism-selftest-perturb" || a == "--determinism-selftest-perturb") {
					r.selftestPerturb = true;
					continue;
				}
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

		// Build the child-process command line. threadCount >= 0 pins the Lua-state count
		// via -num-lua-states (the thread-matrix lever); -1 leaves the engine default.
		std::string BuildChildCmd(const std::filesystem::path& binary, const Args& args,
		                          const std::filesystem::path& runOut, int threadCount) {
			std::ostringstream cmd;
			cmd << Quote(binary.string())
			    << " -scenario " << Quote(args.scenario)
			    << " -seed " << args.seed
			    << " -max-ticks " << args.ticks
			    << " -tick-hashes";
			if (threadCount >= 0) {
				cmd << " -num-lua-states " << threadCount;
			}
			if (args.selftestPerturb) {
				cmd << " -determinism-selftest-perturb";
			}
			cmd << " -out " << Quote(runOut.string());
			return cmd.str();
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

		// One spawned child run: which thread count it used, where its JSON landed.
		struct ChildRun {
			int                   threadCount = -1; // -1 = engine default (repeat-runs mode)
			int                   runIndex = 0;
			std::filesystem::path output;
		};

		struct DivergenceReport {
			bool diverged = false;
			uint64_t    firstDivergenceTick = 0;
			// Per-subsystem first-divergence summary (subsystem name → tick where it first diverged).
			std::map<std::string, uint64_t> perSubsystemFirstDivergence;
			// Total mismatched ticks across the trace.
			uint64_t totalMismatchedTicks = 0;
			// How many ticks were actually compared (min over the per-run tick counts).
			uint64_t comparedTicks = 0;
			// Runs disagreed on trace length — an early-exiting run (crash/abort/assert), itself a
			// non-determinism the per-tick overlap diff would otherwise miss.
			bool lengthMismatch = false;
			// Per-run tick counts (for diagnostics — divergent run lengths is itself a flag).
			std::vector<uint64_t> perRunTickCount;
			// Per-run divergence flag (true if that run ever differed from run 0).
			std::vector<bool> perRunDiverged;
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

		const bool matrixMode = !args.threadCounts.empty();

		// In matrix mode the comparison is across thread counts, so a single run per count is
		// already a valid diff; repeat-runs mode needs >= 2 to have anything to compare.
		if (matrixMode) {
			if (args.runs < 1) {
				std::cerr << "[determinism-check] --runs must be >= 1.\n";
				return 2;
			}
			if (args.threadCounts.size() * static_cast<size_t>(args.runs) < 2) {
				std::cerr << "[determinism-check] thread matrix needs >= 2 total runs to diff.\n";
				return 2;
			}
		} else if (args.runs < 2) {
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

		// Temp dir for the per-run JSONs - a genuinely unique subdir per run, since a
		// clock value alone collides when concurrent orchestrators share a tick.
		std::filesystem::path tmpRoot;
		{
			std::error_code ec;
			for (int attempt = 0; attempt < 1000; ++attempt) {
				const auto now = std::chrono::system_clock::now().time_since_epoch().count();
				tmpRoot = std::filesystem::temp_directory_path() /
				          ("cccp-determinism-" + std::to_string(now) + "-" + std::to_string(attempt));
				if (std::filesystem::create_directory(tmpRoot, ec)) {
					break;
				}
			}
		}

		// Build the flat child list. Matrix mode: every (threadCount, runIndex) pair.
		// Repeat-runs mode: runs at the engine-default count (threadCount = -1).
		std::vector<ChildRun> children;
		if (matrixMode) {
			for (int tc: args.threadCounts) {
				for (int r = 0; r < args.runs; ++r) {
					ChildRun c;
					c.threadCount = tc;
					c.runIndex = r;
					c.output = tmpRoot / ("run_t" + std::to_string(tc) + "_" + std::to_string(r) + ".json");
					children.push_back(std::move(c));
				}
			}
		} else {
			for (int r = 0; r < args.runs; ++r) {
				ChildRun c;
				c.threadCount = -1;
				c.runIndex = r;
				c.output = tmpRoot / ("run_" + std::to_string(r) + ".json");
				children.push_back(std::move(c));
			}
		}

		std::cout << "[determinism-check] binary  : " << binary.string() << "\n";
		std::cout << "[determinism-check] scenario: " << args.scenario << "\n";
		std::cout << "[determinism-check] ticks   : " << args.ticks << "\n";
		std::cout << "[determinism-check] seed    : " << args.seed << "\n";
		if (matrixMode) {
			std::cout << "[determinism-check] mode    : thread-count matrix\n";
			std::cout << "[determinism-check] threads : ";
			for (size_t i = 0; i < args.threadCounts.size(); ++i) {
				std::cout << args.threadCounts[i] << (i + 1 < args.threadCounts.size() ? "," : "");
			}
			std::cout << "  (x" << args.runs << " runs each = " << children.size() << " total)\n";
		} else {
			std::cout << "[determinism-check] runs    : " << args.runs << "\n";
		}
		std::cout << "[determinism-check] tmp dir : " << tmpRoot.string() << "\n";

		// Run the binary once per child. Same scenario+seed+ticks; in matrix mode the Lua-state
		// count varies. -tick-hashes makes the per-tick hash trace land in the JSON.
		for (size_t i = 0; i < children.size(); ++i) {
			const ChildRun& child = children[i];
			const std::string cmd = BuildChildCmd(binary, args, child.output, child.threadCount);

			std::cout << "[determinism-check] run " << (i + 1) << "/" << children.size();
			if (child.threadCount >= 0) {
				std::cout << " (threads=" << child.threadCount << ")";
			}
			std::cout << ": " << cmd << std::endl;

			const int rc = std::system(cmd.c_str());
			// The scenario itself may legitimately exit non-zero (a fail-result trust scenario).
			// What we really care about is whether the JSON was produced and parseable.
			if (rc < 0) {
				std::cerr << "[determinism-check] run " << i << " failed to spawn (rc=" << rc << ")\n";
				return 2;
			}
			if (!std::filesystem::exists(child.output)) {
				std::cerr << "[determinism-check] run " << i << " produced no JSON output: "
				          << child.output.string() << "\n";
				return 2;
			}
		}

		// Read every run's JSON and compute the per-tick comparison.
		std::vector<json> jsons;
		jsons.reserve(children.size());
		for (const auto& child: children) {
			json j = ReadJsonFile(child.output);
			if (j.is_null()) {
				std::cerr << "[determinism-check] run " << child.output.string() << " produced unparseable JSON.\n";
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
		rep.perRunDiverged.assign(allTickHashes.size(), false);
		uint64_t minTicks = UINT64_MAX;
		uint64_t maxTicks = 0;
		for (const auto& th: allTickHashes) {
			const uint64_t n = static_cast<uint64_t>(th.size());
			rep.perRunTickCount.push_back(n);
			if (n < minTicks) minTicks = n;
			if (n > maxTicks) maxTicks = n;
		}
		rep.comparedTicks = (minTicks == UINT64_MAX) ? 0 : minTicks;
		// Identical seed+scenario must produce identical-length traces; differing lengths mean a
		// run exited early, and the overlap-only diff below would otherwise false-MATCH.
		rep.lengthMismatch = (minTicks != maxTicks);

		// Walk tick-by-tick. The runs' tick arrays are in order, so index t corresponds to tick t.
		// run 0 is the reference; every other run is diffed against it.
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
					rep.perRunDiverged[r] = true;
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
		rep.diverged = foundFirstDivergence || rep.lengthMismatch;
		if (rep.lengthMismatch && !foundFirstDivergence) {
			rep.firstDivergenceTick = rep.comparedTicks; // where the shortest run stopped
		}

		// Write the divergence report. Schema is small and stable — read by humans and CI scripts.
		json reportJson;
		reportJson["scenario"] = args.scenario;
		reportJson["seed"] = args.seed;
		reportJson["ticks_requested"] = args.ticks;
		reportJson["mode"] = matrixMode ? "thread-count-matrix" : "repeat-runs";
		reportJson["runs"] = args.runs;
		reportJson["total_runs"] = static_cast<uint64_t>(children.size());
		if (matrixMode) {
			json tc = json::array();
			for (int t: args.threadCounts) tc.push_back(t);
			reportJson["thread_counts"] = tc;
		}
		reportJson["compared_ticks"] = rep.comparedTicks;
		reportJson["diverged"] = rep.diverged;
		reportJson["length_mismatch"] = rep.lengthMismatch;
		reportJson["total_mismatched_ticks"] = rep.totalMismatchedTicks;
		if (rep.diverged) {
			reportJson["first_divergence_tick"] = rep.firstDivergenceTick;
			json subs = json::object();
			for (const auto& [name, tick]: rep.perSubsystemFirstDivergence) {
				subs[name] = tick;
			}
			reportJson["per_subsystem_first_divergence"] = subs;
		}
		// Per-child detail: thread count, tick count, whether it diverged from run 0.
		json runsDetail = json::array();
		for (size_t i = 0; i < children.size(); ++i) {
			json rd;
			rd["index"] = static_cast<uint64_t>(i);
			rd["thread_count"] = children[i].threadCount;
			rd["run_index"] = children[i].runIndex;
			rd["tick_count"] = (i < rep.perRunTickCount.size()) ? rep.perRunTickCount[i] : 0;
			rd["diverged"] = (i < rep.perRunDiverged.size()) ? static_cast<bool>(rep.perRunDiverged[i]) : false;
			runsDetail.push_back(rd);
		}
		reportJson["runs_detail"] = runsDetail;
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
			if (rep.lengthMismatch) {
				std::cout << "    length mismatch — per-run tick counts:";
				for (uint64_t n: rep.perRunTickCount) std::cout << " " << n;
				std::cout << "  (a run exited early)\n";
			}
			std::cout << "    per-subsystem first divergence:\n";
			for (const auto& [name, tick]: rep.perSubsystemFirstDivergence) {
				std::cout << "        " << name << ": tick " << tick << "\n";
			}
			if (matrixMode) {
				std::cout << "    diverged runs (vs run 0):\n";
				for (size_t i = 1; i < children.size(); ++i) {
					if (i < rep.perRunDiverged.size() && rep.perRunDiverged[i]) {
						std::cout << "        run " << i << " threads=" << children[i].threadCount
						          << " (run-index " << children[i].runIndex << ")\n";
					}
				}
			}
		} else {
			std::cout << "[determinism-check] RESULT: MATCHED (" << rep.comparedTicks
			          << " ticks across " << children.size() << " runs";
			if (matrixMode) {
				std::cout << " at thread counts ";
				for (size_t i = 0; i < args.threadCounts.size(); ++i) {
					std::cout << args.threadCounts[i] << (i + 1 < args.threadCounts.size() ? "," : "");
				}
			}
			std::cout << ")\n";
			if (rep.comparedTicks < args.ticks) {
				std::cout << "[determinism-check] NOTE: trace ended at " << rep.comparedTicks
				          << " ticks (< requested " << args.ticks << ") — scenario exited early but"
				          << " deterministically; check its pass/fail grade.\n";
			}
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
