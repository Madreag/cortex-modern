// cccp-ctl — Cortex-Modern test + benchmark CLI
//
// Standalone binary that wraps the main game binary's `-scenario` and
// `-determinism-check` modes behind a structured subcommand surface. Per
// engine.html T-VALIDATION + tracker.html TR-VALID, this is the first-class
// test-runner that every subsequent MP milestone extends with its own
// `cccp-ctl test <mode-X>` subcommand.
//
// No engine dependencies — only stdlib + nlohmann/json (vendored at
// external/sources/nlohmann_json-3.12.0/). Mirrors the no-deps pattern of
// Source/CI/DeterminismCheck.cpp so the binary is small, fast to build, and
// safe to run on any platform that ships the main binary.
//
// Subcommands (V1.2):
//   test all                    Aggregate M1-M4 suite runner
//   test selftest               EC3 positive control (verify harness)
//   test scenario               Run a single scenario
//   test replay-determinism     Wraps -determinism-check (N runs, diff)
//   test thread-matrix          Wraps -determinism-check --threads (M4 acid test)
//   test cross-platform-checksum  Diff trace JSONs from multiple platforms
//   test sync-drift             [stub — needs M6 networking]
//   test latency-injection      [stub — needs M6 networking]
//   test rollback-burst         [stub — needs M9 rollback]
//   bench replay                Run scenario, report sim-compute throughput
//   trace inspect <path>        Pretty-print a trace JSON
//   info scenarios              List known scenarios + descriptions
//   info subsystems             List SimChecksum subsystems
//   info game-bin               Locate + validate the game binary

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

	using nlohmann::json;
	namespace fs = std::filesystem;

	constexpr const char* kVersion = "0.3.1";

	// Path to this cccp-ctl binary itself, set in main() from argv[0]. Used by
	// `test all` / `test selftest` to re-invoke ourselves with the right
	// subcommand for each sub-step.
	std::string g_selfPath;

	// Scenarios known to the test module as of M0-M4 (per Data/Tests.rte). New
	// scenarios just need adding here to surface in `info scenarios` / --help;
	// the binary passes whatever the user supplies through to ScenarioRunner
	// regardless, so an unknown name will just fall through to the engine.
	struct ScenarioInfo {
		const char* name;
		const char* milestone; // "M1" / "M2" / "M3" / "M4" / "AI-Trust"
		const char* description;
	};
	const std::vector<ScenarioInfo> kScenarios = {
	    {"M1Baseline",         "M1",       "Quiet single-actor baseline — proves the sim is deterministic with minimal activity."},
	    {"M1TerrainStress",    "M1",       "Heavy terrain destruction — exercises SceneMan/SLTerrain determinism."},
	    {"M1ActorStress",      "M1",       "High actor count — exercises MovableMan ordered iteration + per-actor RNG."},
	    {"M2LuaBaseline",      "M2",       "Lua determinism baseline — exercises per-Lua-state math.random + sorted pairs()."},
	    {"M2LuaRandomStress",  "M2",       "Heavy math.random use in Lua — proves per-state RNG seeding holds."},
	    {"M2PairsStress",      "M2",       "Heavy table iteration — proves the sorted pairs() patch is engaged."},
	    {"M2OsStubTest",       "M2",       "Validates os.time / os.clock stubs return tick-derived values."},
	    {"M2ModSmokeLoading",  "M2",       "Loads representative community mods — verifies mod corpus stays in sync."},
	    {"M3TerrainStress",    "M3",       "M1TerrainStress on the fixed-point physics core — proves Q40.24 destruction is bit-identical."},
	    {"M4ThreadStress",     "M4",       "Threaded sim across N Lua-state counts — the FFF-415 acid test (use with `test thread-matrix`)."},
	    // AI-NN trust scenarios: each grades a single named AI behaviour. Stock AI
	    // produces FAIL; the scenario's selftest mode (--trust-selftest) drives the
	    // named behaviour to verify the harness. Per MILESTONES_COMPLETION MC3 all
	    // 12 are "normal=FAIL, selftest=PASS". The per-scenario behaviour names live
	    // in Data/Tests.rte/Activities; ground-truth descriptions only for AI-03 +
	    // AI-12 (cross-checked there); others left generic deliberately.
	    {"AI-01",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-02",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-03",              "AI-Trust", "Trust scenario — actor must dig through terrain (gravity-only = FAIL)."},
	    {"AI-04",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-05",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-06",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-07",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-08",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-09",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-10",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-11",              "AI-Trust", "Trust scenario — see Data/Tests.rte/Activities for behaviour grading."},
	    {"AI-12",              "AI-Trust", "Trust scenario — actor must carve terrain (measured pixel-count change)."},
	};

	// SimChecksum subsystems exposed by the engine (per Source/Network/SimChecksum.*
	// + the per-subsystem rows in the trace JSONs). This list is the authoritative
	// view of "what cccp-ctl can show divergence on" and is hand-curated to match
	// what the engine produces. Cross-check against Source/CI/M*-trace.json.
	struct SubsystemInfo {
		const char* name;
		const char* milestone;
		const char* description;
	};
	const std::vector<SubsystemInfo> kSubsystems = {
	    {"tick",        "M1", "The per-tick fold of the previous subsystems' hashes (the total hash)."},
	    {"sim_rng",     "M1", "g_SimRNG state (M1 Block B sim/render RNG split)."},
	    {"lua_state",   "M2", "All Lua states' RNG + key tables (M2 deterministic Lua)."},
	    {"actors",      "M1", "All MovableMan actors' state (M1 Block C stable MOID iteration)."},
	    {"particles",   "M1", "MovableMan particles' state."},
	    {"controller",  "M1", "All Controller state per actor (53 control states + analog vectors + input mode)."},
	    {"decisions",   "M0", "AI decision-event channel folded into the tick hash."},
	    {"scene",       "M3", "SceneMan global state."},
	    {"terrain",     "M3", "SLTerrain pixel state (M3 fixed-point carve math)."},
	    {"carve_math",  "M3", "Per-carve math intermediate results (M3 fixed-point verification)."},
	};

	// ----- arg matching --------------------------------------------------------

	// Accept both -foo and --foo for every named flag (matches the rest of the
	// engine's CLI conventions; see ScenarioRunner::ParseArgs + DeterminismCheck).
	bool ArgEq(const std::string& a, const char* name) {
		const std::string longForm = std::string("--") + name;
		const std::string shortForm = std::string("-") + name;
		return a == longForm || a == shortForm;
	}

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

	std::vector<std::string> ParseStringList(const std::string& csv) {
		std::vector<std::string> out;
		std::stringstream ss(csv);
		std::string token;
		while (std::getline(ss, token, ',')) {
			if (!token.empty()) out.push_back(token);
		}
		return out;
	}

	// Cross-platform quoting for std::system. Matches DeterminismCheck::Quote.
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

	// ----- binary discovery + subprocess --------------------------------------

	// Look for the main game binary next to cccp-ctl, then in standard build-output
	// locations. Returns empty path if none found; --game-bin is the override.
	//
	// Walks up to 8 parent directories looking for either a direct hit on the
	// candidate names, or a `_Bin/x64/<config>` / `_BinTests/x64/<config>` / `build/`
	// / `builddir/` sub-dir containing one. Also detects a repo-root marker (RTEA.sln
	// or top-level meson.build) and tries the standard build outputs from there
	// regardless of how deep we are. 8 levels covers the typical worst case:
	// `<repo>/Source/CLI/_BinTests/x64/Release/` (5 deep) plus a few more.
	fs::path AutoDetectGameBin(const fs::path& selfDir) {
#ifdef _WIN32
		const std::vector<std::string> candidates = {
		    "Cortex Command.exe",
		    "CortexCommand.exe",
		    "cccp.exe",
		};
		const std::vector<std::string> configs = {"Final", "Release", "Debug", "Debug Minimal", "Debug Full"};
#else
		const std::vector<std::string> candidates = {
		    "CortexCommand",
		    "cccp",
		};
		const std::vector<std::string> configs = {"Final", "Release", "Debug"};
#endif
		auto tryDir = [&](const fs::path& dir) -> fs::path {
			for (const auto& name: candidates) {
				const fs::path p = dir / name;
				if (fs::exists(p)) return p;
			}
			return {};
		};
		auto tryBuildDirs = [&](const fs::path& root) -> fs::path {
			for (const auto& cfg: configs) {
				if (auto f = tryDir(root / "_Bin" / "x64" / cfg); !f.empty()) return f;
				if (auto f = tryDir(root / "_BinTests" / "x64" / cfg); !f.empty()) return f;
			}
			if (auto f = tryDir(root / "build"); !f.empty()) return f;
			if (auto f = tryDir(root / "builddir"); !f.empty()) return f;
			return {};
		};
		auto isRepoRoot = [](const fs::path& d) {
			// Unique-to-repo-root markers. Crucially NOT just "meson.build" — that
			// file exists at EVERY subdir of a Meson build (Source/, Source/CLI/,
			// Source/CI/, ...) so it would false-positive at level 4 and prematurely
			// stop the search before reaching the actual root.
			return fs::exists(d / "RTEA.sln")           // Windows-only solution file
			    || fs::exists(d / ".git")               // any git repo or worktree
			    || fs::exists(d / "external");          // cortex-modern third-party sources dir
		};

		// Walk up to 8 levels. At each level try the dir itself, the standard
		// build sub-dirs, and — if it looks like the repo root — exhaustively.
		fs::path cur = selfDir;
		for (int up = 0; up < 8; ++up) {
			if (auto f = tryDir(cur); !f.empty()) return f;
			if (auto f = tryBuildDirs(cur); !f.empty()) return f;
			if (isRepoRoot(cur)) {
				if (auto f = tryBuildDirs(cur); !f.empty()) return f;
				break; // no point going above the repo root
			}
			const fs::path parent = cur.parent_path();
			if (parent.empty() || parent == cur) break;
			cur = parent;
		}
		return {};
	}

	fs::path ResolveSelfDir(const char* argv0) {
		std::error_code ec;
		fs::path self = fs::absolute(fs::path(argv0 ? argv0 : ""), ec);
		if (ec || self.empty()) return fs::current_path();
		return self.parent_path();
	}

	// Decode std::system / pclose return value into the child's actual exit code.
	// On Windows both already return the exit code. On POSIX they return wait4-style
	// status that must be decoded via WIFEXITED/WEXITSTATUS. Returns -1 if the child
	// died from a signal or the call itself failed. Critical for `test selftest`,
	// which checks rc == 1 to verify divergence is signalled correctly.
	int DecodeExitStatus(int sysRc) {
		if (sysRc == -1) return -1;
#ifdef _WIN32
		return sysRc;
#else
		if (WIFEXITED(sysRc)) return WEXITSTATUS(sysRc);
		return -1;
#endif
	}

	// RAII chdir guard. The game binary needs to be launched with CWD == the
	// worktree root so it can find Data/Tests.rte/Index.ini and friends (which
	// are referenced as `Data/...` relative paths). Without this the game pops up
	// "RTE Assert! Failed to open data file 'Data/Tests.rte/Index.ini'!".
	// We chdir process-wide before std::system / popen and restore after. Safe
	// because cccp-ctl is single-threaded; outPath etc. are resolved to absolute
	// before the chdir so they stay valid in the subprocess.
	struct CwdGuard {
		fs::path prev;
		bool changed = false;
		CwdGuard(const fs::path& target) {
			if (target.empty()) return;
			std::error_code ec;
			prev = fs::current_path(ec);
			if (ec) return;
			fs::current_path(target, ec);
			if (ec) {
				std::cerr << "[cccp-ctl] failed to chdir to " << target.string() << ": " << ec.message() << "\n";
				return;
			}
			changed = true;
		}
		~CwdGuard() {
			if (!changed) return;
			std::error_code ec;
			fs::current_path(prev, ec);
		}
		CwdGuard(const CwdGuard&) = delete;
		CwdGuard& operator=(const CwdGuard&) = delete;
	};

	int RunSubprocess(const std::string& cmd, bool echoCmd = true, const fs::path& cwd = {}) {
		if (echoCmd) {
			// Echo goes to stderr so stdout stays clean for JSON consumers downstream.
			std::cerr << "[cccp-ctl] $ " << cmd;
			if (!cwd.empty()) std::cerr << "  (cwd=" << cwd.string() << ")";
			std::cerr << "\n" << std::flush;
		}
		CwdGuard guard(cwd);
		return DecodeExitStatus(std::system(cmd.c_str()));
	}

	// Run a subprocess and capture its stdout. Returns the captured stdout; rcOut
	// (optional) gets the decoded child exit code (already passed through
	// DecodeExitStatus). Used where stdout-as-data is the right channel; prefer
	// RunSubprocess + reading the child's --output file when the child writes a
	// JSON report, because the game binary's own stdout may pollute the stream.
	std::string RunCapture(const std::string& cmd, int* rcOut = nullptr, const fs::path& cwd = {}) {
		CwdGuard guard(cwd);
#ifdef _WIN32
		FILE* p = _popen(cmd.c_str(), "r");
#else
		FILE* p = popen(cmd.c_str(), "r");
#endif
		if (!p) {
			if (rcOut) *rcOut = -1;
			return {};
		}
		std::string out;
		char buf[4096];
		while (std::fgets(buf, sizeof(buf), p) != nullptr) out += buf;
#ifdef _WIN32
		const int rawRc = _pclose(p);
#else
		const int rawRc = pclose(p);
#endif
		if (rcOut) *rcOut = DecodeExitStatus(rawRc);
		return out;
	}

	json ReadJsonFile(const fs::path& p) {
		std::ifstream f(p);
		if (!f.is_open()) return json();
		try {
			return json::parse(f);
		} catch (const std::exception& e) {
			std::cerr << "[cccp-ctl] failed to parse " << p.string() << ": " << e.what() << "\n";
			return json();
		}
	}

	fs::path MakeUniqueTmpDir(const std::string& prefix) {
		for (int attempt = 0; attempt < 1000; ++attempt) {
			const auto now = std::chrono::system_clock::now().time_since_epoch().count();
			const fs::path p = fs::temp_directory_path() /
			                   (prefix + "-" + std::to_string(now) + "-" + std::to_string(attempt));
			std::error_code ec;
			if (fs::create_directory(p, ec)) return p;
		}
		std::cerr << "[cccp-ctl] could not create temp dir under " << fs::temp_directory_path().string() << "\n";
		std::exit(2);
	}

	// ----- output formatting ---------------------------------------------------

	// Per-invocation output/path settings. Each subcommand parses these from its
	// own argv (so they're available regardless of where they appear in the line).
	struct OutputCtx {
		bool json = false;
		bool quiet = false;     // --quiet: suppress [cccp-ctl] $ <cmd> echo lines
		fs::path outDir;        // --out-dir <path>: direct all temp/output here
	};

	// Pull a unique sub-directory under outDir (if set) or system-temp.
	fs::path PickOutDir(const OutputCtx& ctx, const std::string& prefix) {
		if (!ctx.outDir.empty()) {
			std::error_code ec;
			fs::create_directories(ctx.outDir, ec);
			// Make a unique sub-dir under it so multiple invocations don't collide.
			for (int attempt = 0; attempt < 1000; ++attempt) {
				const auto now = std::chrono::system_clock::now().time_since_epoch().count();
				const fs::path p = ctx.outDir / (prefix + "-" + std::to_string(now) + "-" + std::to_string(attempt));
				if (fs::create_directory(p, ec)) return p;
			}
		}
		return MakeUniqueTmpDir(prefix);
	}

	void PrintTextOrJson(const OutputCtx& ctx, const json& j, const std::string& humanSummary) {
		if (ctx.json) {
			std::cout << j.dump(2) << "\n";
		} else {
			std::cout << humanSummary;
			if (!humanSummary.empty() && humanSummary.back() != '\n') std::cout << "\n";
		}
	}

	// ----- help text -----------------------------------------------------------

	void PrintTopHelp(std::ostream& out) {
		out <<
		    "cccp-ctl " << kVersion << " — Cortex-Modern test + benchmark CLI\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl <command> [<subcommand>] [<args>]\n"
		    "\n"
		    "Test commands (correctness):\n"
		    "  test all                       Run the full M1-M4 determinism verification suite\n"
		    "  test selftest                  Verify the harness catches injected non-determinism (EC3)\n"
		    "  test scenario                  Run a single scenario, report pass/fail + metrics\n"
		    "  test replay-determinism        Run a scenario N times, diff per-tick hashes\n"
		    "  test thread-matrix             Run scenario across Lua-state counts (M4 acid test)\n"
		    "  test cross-platform-checksum   Diff trace JSONs from multiple platforms\n"
		    "  test sync-drift                [needs M6 — networking not yet shipped]\n"
		    "  test latency-injection         [needs M6 — networking not yet shipped]\n"
		    "  test rollback-burst            [needs M9 — rollback not yet shipped]\n"
		    "\n"
		    "Bench commands (performance):\n"
		    "  bench replay                   Run scenario N times, report sim-compute throughput\n"
		    "\n"
		    "Trace inspection:\n"
		    "  trace inspect                  Pretty-print a trace JSON (debugging aid)\n"
		    "\n"
		    "Discovery / introspection:\n"
		    "  info scenarios                 List known scenarios + which milestone introduced each\n"
		    "  info subsystems                List SimChecksum subsystems hashed per tick\n"
		    "  info game-bin                  Locate + validate the game binary\n"
		    "\n"
		    "Common options (every subcommand):\n"
		    "  --game-bin <path>              Path to game binary (default: auto-detect)\n"
		    "  --json                         Emit JSON to stdout (default: human-readable text)\n"
		    "  --quiet                        Suppress `[cccp-ctl] $ <cmd>` echo lines\n"
		    "  --out-dir <path>               Direct all reports + temp files here (default: system temp)\n"
		    "  -h, --help                     Show subcommand-specific help\n"
		    "  -V, --version                  Print version and exit\n"
		    "\n"
		    "Exit codes:\n"
		    "  0    success / MATCH\n"
		    "  1    test failed / DIVERGED\n"
		    "  2    usage error\n"
		    "  64   subcommand known but not yet implemented (stub)\n"
		    "\n"
		    "Examples:\n"
		    "  cccp-ctl test all --quick                      # CI smoke run, ~minutes\n"
		    "  cccp-ctl test all --runs 100                   # Full M1-M4 verification\n"
		    "  cccp-ctl test selftest                         # Verify the harness itself\n"
		    "  cccp-ctl test scenario --scenario M1Baseline --seed 42\n"
		    "  cccp-ctl test replay-determinism --scenario M1Baseline --runs 100\n"
		    "  cccp-ctl test thread-matrix --scenario M4ThreadStress --threads 1,2,4,8,16\n"
		    "  cccp-ctl test cross-platform-checksum --traces win.json,linux.json,macos.json\n"
		    "  cccp-ctl trace inspect M1Baseline-trace.json --ticks 1,100,599\n"
		    "  cccp-ctl info game-bin\n"
		    "  cccp-ctl bench replay --scenario M3TerrainStress --runs 5\n"
		    "\n"
		    "Run `cccp-ctl <command> --help` for subcommand-specific options.\n";
	}

	void PrintTestScenarioHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test scenario — run a single scenario\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test scenario --scenario <name> [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>     Scenario preset-suffix (e.g. M1Baseline). Required.\n"
		    "  --seed <N>            RNG seed. Default 42.\n"
		    "  --max-ticks <N>       Sim-tick cap. Default 600.\n"
		    "  --out <path>          Where to write the JSON report. Default: temp dir.\n"
		    "  --no-tick-hashes      Omit per-tick hashes from the report (smaller output).\n"
		    "                        Default: tick-hashes ON (required by trace inspect /\n"
		    "                        cross-platform-checksum).\n"
		    "  --trust-selftest      Drive the named behaviour in trust scenarios (AI-NN).\n"
		    "                        Required for AI-NN scenarios to PASS; stock AI fails them by design.\n"
		    "  --selftest-perturb    Inject one genuine non-determinism (the EC3 positive control).\n"
		    "  --out-dir <path>      Direct temp/output dir.\n"
		    "  --game-bin <path>     Path to game binary. Default: auto-detect.\n"
		    "  --json                JSON output. Default: human-readable summary.\n"
		    "  --quiet               Suppress [cccp-ctl] $ <cmd> echo on stderr.\n"
		    "  -h, --help            This help.\n"
		    "\n"
		    "Note: AI-NN scenarios are trust scenarios — stock AI does NOT perform the named\n"
		    "behaviour, so they exit 1 (FAIL) without --trust-selftest. With --trust-selftest\n"
		    "the test script drives the behaviour to verify the grading harness itself.\n"
		    "\n"
		    "Known scenarios (more may exist; use `cccp-ctl info scenarios` for details):\n";
		for (const auto& s: kScenarios) {
			out << "  " << std::left << std::setw(22) << s.name << "  [" << s.milestone << "]\n";
		}
	}

	void PrintTestReplayDeterminismHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test replay-determinism — run a scenario N times, diff per-tick hashes\n"
		    "\n"
		    "Wraps the main binary's `-determinism-check` mode.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test replay-determinism --scenario <name> [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>     Scenario preset-suffix. Required.\n"
		    "  --runs <N>            Number of repeat runs. Default 100. Must be >= 2.\n"
		    "  --seed <N>            RNG seed. Default 42.\n"
		    "  --ticks <N>           Per-run sim-tick cap. Default 600.\n"
		    "  --output <path>       Where to write the divergence report. Default: temp dir.\n"
		    "  --keep-runs           Keep per-run JSONs after diff.\n"
		    "  --selftest-perturb    Inject one non-determinism (positive control).\n"
		    "  --out-dir <path>      Direct temp/output dir.\n"
		    "  --game-bin <path>     Path to game binary. Default: auto-detect.\n"
		    "  --json                JSON output. Default: human-readable summary.\n"
		    "  --quiet               Suppress [cccp-ctl] $ <cmd> echo on stderr.\n"
		    "  -h, --help            This help.\n";
	}

	void PrintTestThreadMatrixHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test thread-matrix — run scenario across Lua-state counts, diff (M4 acid test)\n"
		    "\n"
		    "Wraps the main binary's `-determinism-check --threads N,N,N` mode.\n"
		    "The acceptance test for M4's 'bit-identical regardless of thread count' contract.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test thread-matrix --scenario <name> --threads <list> [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>     Scenario preset-suffix. Required.\n"
		    "  --threads <list>      Comma-separated Lua-state counts (e.g. 1,2,4,8,16). Required.\n"
		    "  --runs <N>            Runs per thread count. Default 3.\n"
		    "  --seed <N>            RNG seed. Default 42.\n"
		    "  --ticks <N>           Per-run sim-tick cap. Default 900.\n"
		    "  --output <path>       Where to write the divergence report. Default: temp dir.\n"
		    "  --keep-runs           Keep per-run JSONs after diff.\n"
		    "  --out-dir <path>      Direct temp/output dir.\n"
		    "  --game-bin <path>     Path to game binary. Default: auto-detect.\n"
		    "  --json                JSON output. Default: human-readable summary.\n"
		    "  --quiet               Suppress [cccp-ctl] $ <cmd> echo on stderr.\n"
		    "  -h, --help            This help.\n";
	}

	void PrintTestCrossPlatformHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test cross-platform-checksum — diff trace JSONs from multiple platforms\n"
		    "\n"
		    "Reads multiple trace JSON files (each produced by `<bin> -scenario X -tick-hashes\n"
		    "-out X.json` on a different platform) and reports the first per-tick subsystem\n"
		    "divergence. The M5 cross-platform determinism acceptance test.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test cross-platform-checksum --traces <path1>,<path2>,...\n"
		    "                                        [--labels <label1>,<label2>,...]\n"
		    "\n"
		    "Options:\n"
		    "  --traces <paths>      Comma-separated paths to trace JSONs. Required, >= 2.\n"
		    "  --labels <names>      Comma-separated labels for each trace (default: filename).\n"
		    "                        Useful for naming platforms: win,linux,macos,wasm.\n"
		    "  --output <path>       Where to write the divergence report. Default: stdout only.\n"
		    "  --out-dir <path>      Direct temp/output dir.\n"
		    "  --json                JSON output. Default: human-readable summary.\n"
		    "  --quiet               (No effect — this subcommand has no echoes; accepted for consistency.)\n"
		    "  -h, --help            This help.\n";
	}

	void PrintTestAllHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test all — run the full M1-M4 determinism verification suite\n"
		    "\n"
		    "Aggregates every other test subcommand into one CI-friendly run:\n"
		    "  - replay-determinism for each of the 9 M1/M2/M3 scenarios\n"
		    "  - thread-matrix for M4ThreadStress (the FFF-415 acid test)\n"
		    "  - selftest positive control on M1Baseline (EC3)\n"
		    "\n"
		    "Exits 0 iff every sub-step passes. Exits 1 iff any diverges. Designed as\n"
		    "the single CI step that gates a PR merge: `cccp-ctl test all --quick`.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test all [options]\n"
		    "\n"
		    "Options:\n"
		    "  --runs <N>            Runs per scenario for replay-determinism. Default 100.\n"
		    "  --seed <N>            RNG seed propagated to all sub-steps. Default 42.\n"
		    "  --ticks <N>           Sim-tick cap propagated to replay-determinism + selftest. Default 600.\n"
		    "  --quick               Smoke-run mode: --runs 3, --threads 1,4 only. ~minutes vs ~hour.\n"
		    "  --out-dir <path>      Direct all reports here. Default: system temp.\n"
		    "  --game-bin <path>     Path to game binary. Default: auto-detect.\n"
		    "  --json                JSON aggregate output. Default: human-readable per-step summaries.\n"
		    "  --quiet               Suppress per-step command echo lines.\n"
		    "  -h, --help            This help.\n";
	}

	void PrintTestSelftestHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test selftest — verify the harness catches injected non-determinism\n"
		    "\n"
		    "The EC3 positive control. Runs the same scenario twice:\n"
		    "  1. WITHOUT --selftest-perturb (should MATCH)\n"
		    "  2. WITH --selftest-perturb    (should DIVERGE — a fixed-tick std::random_device pull)\n"
		    "\n"
		    "Returns 0 iff both expected outcomes hold. Returns 1 if either fails — meaning\n"
		    "the harness itself is broken (false-positive or false-negative). This is the\n"
		    "first test to run on a new machine / CI runner / nightly build to confirm the\n"
		    "infrastructure is alive before trusting any other test result.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test selftest [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>     Scenario to use for the positive control. Default M1Baseline.\n"
		    "  --runs <N>            Repeat runs per phase. Default 5 (selftest is cheap).\n"
		    "  --seed <N>            RNG seed. Default 42.\n"
		    "  --ticks <N>           Per-run sim-tick cap. Default 600.\n"
		    "  --out-dir <path>      Direct reports here.\n"
		    "  --game-bin <path>     Path to game binary.\n"
		    "  --json                JSON output.\n"
		    "  --quiet               Suppress per-step command echo.\n"
		    "  -h, --help            This help.\n";
	}

	void PrintTraceInspectHelp(std::ostream& out) {
		out <<
		    "cccp-ctl trace inspect — pretty-print a trace JSON\n"
		    "\n"
		    "Reads a trace JSON (produced by `<bin> -scenario X -tick-hashes -out X.json` or\n"
		    "any test subcommand) and prints a human-readable summary. With --full or --ticks,\n"
		    "prints per-tick subsystem hashes — useful when investigating divergence beyond\n"
		    "the first-divergence-tick that cross-platform-checksum reports.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl trace inspect <path> [options]\n"
		    "\n"
		    "Options:\n"
		    "  --full                Print all per-tick subsystem hashes (heavy output).\n"
		    "  --ticks <list>        Print only specific ticks (comma-separated, e.g. 1,100,599).\n"
		    "  --subsystem <name>    Filter per-tick output to one subsystem only.\n"
		    "  --json                JSON output of the summary block.\n"
		    "  -h, --help            This help.\n";
	}

	void PrintInfoHelp(std::ostream& out) {
		out <<
		    "cccp-ctl info — discovery / introspection commands\n"
		    "\n"
		    "Sub-commands:\n"
		    "  info scenarios          List known scenarios + which milestone introduced each.\n"
		    "  info subsystems         List SimChecksum subsystems hashed per tick.\n"
		    "  info game-bin           Locate + validate the game binary.\n"
		    "\n"
		    "All sub-commands accept --json for machine-readable output.\n";
	}

	void PrintBenchReplayHelp(std::ostream& out) {
		out <<
		    "cccp-ctl bench replay — run scenario, report sim-compute throughput\n"
		    "\n"
		    "Uses the `__sim_compute_accum` instrumentation (added in M4) to measure\n"
		    "pacing-independent sim-update compute time. Reports min/median/max across runs.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl bench replay --scenario <name> [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>     Scenario preset-suffix. Required.\n"
		    "  --runs <N>            Number of runs to median. Default 5.\n"
		    "  --seed <N>            RNG seed. Default 42.\n"
		    "  --max-ticks <N>       Sim-tick cap. Default 1800.\n"
		    "  --num-lua-states <N>  Force Lua-state count (M4 thread-pool size).\n"
		    "  --out-dir <path>      Direct temp/output dir.\n"
		    "  --game-bin <path>     Path to game binary. Default: auto-detect.\n"
		    "  --json                JSON output. Default: human-readable summary.\n"
		    "  --quiet               Suppress per-run progress + [cccp-ctl] $ echo on stderr.\n"
		    "  -h, --help            This help.\n";
	}

	// ----- subcommand: test scenario ------------------------------------------

	int CmdTestScenario(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario;
		uint64_t seed = 42;
		uint64_t maxTicks = 600;
		fs::path outPath;
		bool tickHashes = true;      // V1.2: default ON. Downstream cmds need it.
		bool trustSelftest = false;  // V1.2: opt-in for AI-NN scenarios.
		bool selftestPerturb = false;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestScenarioHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "max-ticks") && hasV) { maxTicks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "out") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "tick-hashes")) { tickHashes = true; continue; } // explicit; already default
			if (ArgEq(a, "no-tick-hashes")) { tickHashes = false; continue; }
			if (ArgEq(a, "trust-selftest")) { trustSelftest = true; continue; }
			if (ArgEq(a, "selftest-perturb") || ArgEq(a, "determinism-selftest-perturb")) { selftestPerturb = true; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestScenarioHelp(std::cerr);
			return 2;
		}
		if (scenario.empty()) {
			std::cerr << "[cccp-ctl] --scenario is required.\n";
			return 2;
		}
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}

		if (outPath.empty()) outPath = PickOutDir(out, "cccp-ctl-scenario") / (scenario + ".json");

		std::ostringstream cmd;
		cmd << Quote(gameBin.string())
		    << " -scenario " << Quote(scenario)
		    << " -seed " << seed
		    << " -max-ticks " << maxTicks
		    << " -out " << Quote(outPath.string());
		if (tickHashes) cmd << " -tick-hashes";
		if (trustSelftest) cmd << " -trust-selftest";
		if (selftestPerturb) cmd << " -determinism-selftest-perturb";

		const int rc = RunSubprocess(cmd.str(), !out.quiet, gameBin.parent_path());
		if (rc < 0) {
			std::cerr << "[cccp-ctl] subprocess failed to spawn (rc=" << rc << ")\n";
			return 2;
		}
		if (!fs::exists(outPath)) {
			std::cerr << "[cccp-ctl] no report produced at " << outPath.string() << "\n";
			return 2;
		}

		const json report = ReadJsonFile(outPath);
		if (report.is_null()) return 2;

		// Pull the first run (single-scenario invocations always have exactly one).
		const json& runs = report.value("runs", json::array());
		if (!runs.is_array() || runs.empty()) {
			std::cerr << "[cccp-ctl] report contained no runs\n";
			return 2;
		}
		const json& run = runs[0];
		const bool passed = run.value("passed", false);
		const std::string finalHash = run.value("final_total_hash", std::string());
		const json numeric = run.value("numeric", json::object());

		json summary = {
		    {"scenario", scenario},
		    {"passed", passed},
		    {"final_total_hash", finalHash},
		    {"report_path", outPath.string()},
		    {"numeric", numeric},
		};

		std::ostringstream text;
		text << "Scenario:         " << scenario << "\n";
		text << "Result:           " << (passed ? "PASS" : "FAIL") << "\n";
		text << "Final hash:       " << finalHash << "\n";
		text << "Final tick:       " << numeric.value("final_tick", 0.0) << "\n";
		text << "Actor count:      " << numeric.value("actor_count", 0.0) << "\n";
		text << "Sim compute (us): " << numeric.value("__sim_compute_accum", 0.0) << "\n";
		text << "Wall seconds:     " << numeric.value("__wall_seconds", 0.0) << "\n";
		text << "Report:           " << outPath.string() << "\n";

		PrintTextOrJson(out, summary, text.str());
		return passed ? 0 : 1;
	}

	// ----- subcommand: test replay-determinism --------------------------------

	int CmdTestReplayDeterminism(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario;
		int runs = 100;
		uint64_t seed = 42;
		uint64_t ticks = 600;
		fs::path outPath;
		bool keepRuns = false;
		bool selftestPerturb = false;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestReplayDeterminismHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "output") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "keep-runs")) { keepRuns = true; continue; }
			if (ArgEq(a, "selftest-perturb") || ArgEq(a, "determinism-selftest-perturb")) { selftestPerturb = true; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestReplayDeterminismHelp(std::cerr);
			return 2;
		}
		if (scenario.empty()) {
			std::cerr << "[cccp-ctl] --scenario is required.\n";
			return 2;
		}
		if (runs < 2) {
			std::cerr << "[cccp-ctl] --runs must be >= 2.\n";
			return 2;
		}
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}
		if (outPath.empty()) outPath = PickOutDir(out, "cccp-ctl-rd") / "report.json";

		std::ostringstream cmd;
		cmd << Quote(gameBin.string())
		    << " -determinism-check"
		    << " --scenario " << Quote(scenario)
		    << " --runs " << runs
		    << " --seed " << seed
		    << " --ticks " << ticks
		    << " --output " << Quote(outPath.string());
		if (keepRuns) cmd << " --keep-runs";
		if (selftestPerturb) cmd << " --determinism-selftest-perturb";

		const int rc = RunSubprocess(cmd.str(), !out.quiet, gameBin.parent_path());
		// rc==0: all runs matched. rc==1: diverged. rc==2: usage / spawn error.
		if (rc == 2) {
			std::cerr << "[cccp-ctl] determinism-check returned usage/error.\n";
			return 2;
		}
		if (!fs::exists(outPath)) {
			std::cerr << "[cccp-ctl] no divergence report produced at " << outPath.string() << "\n";
			return 2;
		}

		const json report = ReadJsonFile(outPath);
		if (report.is_null()) return 2;

		const bool diverged = report.value("diverged", false);
		const uint64_t firstDiv = report.value("first_divergence_tick", uint64_t{0});
		const uint64_t comparedTicks = report.value("compared_ticks", uint64_t{0});

		json summary = {
		    {"scenario", scenario},
		    {"runs", runs},
		    {"diverged", diverged},
		    {"first_divergence_tick", firstDiv},
		    {"compared_ticks", comparedTicks},
		    {"report_path", outPath.string()},
		    {"per_subsystem_first_divergence", report.value("per_subsystem_first_divergence", json::object())},
		};

		std::ostringstream text;
		text << "Scenario:           " << scenario << "\n";
		text << "Runs:               " << runs << "\n";
		text << "Compared ticks:     " << comparedTicks << "\n";
		text << "Result:             " << (diverged ? "DIVERGED" : "MATCH") << "\n";
		if (diverged) {
			text << "First divergence:   tick " << firstDiv << "\n";
			const json& perSub = report.value("per_subsystem_first_divergence", json::object());
			if (perSub.is_object() && !perSub.empty()) {
				text << "Per subsystem:\n";
				for (auto it = perSub.begin(); it != perSub.end(); ++it) {
					text << "  " << it.key() << ":  tick " << it.value() << "\n";
				}
			}
		}
		text << "Report:             " << outPath.string() << "\n";

		PrintTextOrJson(out, summary, text.str());
		return diverged ? 1 : 0;
	}

	// ----- subcommand: test thread-matrix -------------------------------------

	int CmdTestThreadMatrix(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario;
		std::vector<int> threadCounts;
		int runs = 3;
		uint64_t seed = 42;
		uint64_t ticks = 900;
		fs::path outPath;
		bool keepRuns = false;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestThreadMatrixHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "threads") && hasV) { threadCounts = ParseIntList(argv[++i]); continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "output") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "keep-runs")) { keepRuns = true; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestThreadMatrixHelp(std::cerr);
			return 2;
		}
		if (scenario.empty()) {
			std::cerr << "[cccp-ctl] --scenario is required.\n";
			return 2;
		}
		if (threadCounts.empty()) {
			std::cerr << "[cccp-ctl] --threads is required (e.g. --threads 1,2,4,8,16).\n";
			return 2;
		}
		if (runs < 1) {
			std::cerr << "[cccp-ctl] --runs must be >= 1.\n";
			return 2;
		}
		// Matrix mode needs >=2 total runs to diff something.
		if (threadCounts.size() * static_cast<size_t>(runs) < 2) {
			std::cerr << "[cccp-ctl] thread matrix needs >=2 total runs to diff (got "
			          << (threadCounts.size() * runs) << ").\n";
			return 2;
		}
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}
		if (outPath.empty()) outPath = PickOutDir(out, "cccp-ctl-matrix") / "report.json";

		std::ostringstream tcCsv;
		for (size_t i = 0; i < threadCounts.size(); ++i) {
			tcCsv << threadCounts[i] << (i + 1 < threadCounts.size() ? "," : "");
		}

		std::ostringstream cmd;
		cmd << Quote(gameBin.string())
		    << " -determinism-check"
		    << " --scenario " << Quote(scenario)
		    << " --threads " << tcCsv.str()
		    << " --runs " << runs
		    << " --seed " << seed
		    << " --ticks " << ticks
		    << " --output " << Quote(outPath.string());
		if (keepRuns) cmd << " --keep-runs";

		const int rc = RunSubprocess(cmd.str(), !out.quiet, gameBin.parent_path());
		if (rc == 2) {
			std::cerr << "[cccp-ctl] determinism-check returned usage/error.\n";
			return 2;
		}
		if (!fs::exists(outPath)) {
			std::cerr << "[cccp-ctl] no divergence report produced at " << outPath.string() << "\n";
			return 2;
		}
		const json report = ReadJsonFile(outPath);
		if (report.is_null()) return 2;

		const bool diverged = report.value("diverged", false);
		const uint64_t firstDiv = report.value("first_divergence_tick", uint64_t{0});

		json summary = {
		    {"scenario", scenario},
		    {"threads", threadCounts},
		    {"runs_per_thread_count", runs},
		    {"total_runs", static_cast<int>(threadCounts.size()) * runs},
		    {"diverged", diverged},
		    {"first_divergence_tick", firstDiv},
		    {"report_path", outPath.string()},
		    {"per_subsystem_first_divergence", report.value("per_subsystem_first_divergence", json::object())},
		};

		std::ostringstream text;
		text << "Scenario:           " << scenario << "\n";
		text << "Thread counts:      " << tcCsv.str() << "\n";
		text << "Runs per count:     " << runs << " (= " << (threadCounts.size() * runs) << " total)\n";
		text << "Result:             " << (diverged ? "DIVERGED" : "MATCH (M4 acid test PASSED)") << "\n";
		if (diverged) {
			text << "First divergence:   tick " << firstDiv << "\n";
			const json& perSub = report.value("per_subsystem_first_divergence", json::object());
			if (perSub.is_object() && !perSub.empty()) {
				text << "Per subsystem:\n";
				for (auto it = perSub.begin(); it != perSub.end(); ++it) {
					text << "  " << it.key() << ":  tick " << it.value() << "\n";
				}
			}
		}
		text << "Report:             " << outPath.string() << "\n";

		PrintTextOrJson(out, summary, text.str());
		return diverged ? 1 : 0;
	}

	// ----- subcommand: test cross-platform-checksum ---------------------------

	int CmdTestCrossPlatformChecksum(int argc, char** argv, const fs::path& /*selfDir*/) {
		std::vector<std::string> traces;
		std::vector<std::string> labels;
		fs::path outPath;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestCrossPlatformHelp(std::cout); return 0; }
			if (ArgEq(a, "traces") && hasV) { traces = ParseStringList(argv[++i]); continue; }
			if (ArgEq(a, "labels") && hasV) { labels = ParseStringList(argv[++i]); continue; }
			if (ArgEq(a, "output") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestCrossPlatformHelp(std::cerr);
			return 2;
		}
		if (traces.size() < 2) {
			std::cerr << "[cccp-ctl] --traces needs at least 2 paths (comma-separated).\n";
			return 2;
		}
		if (labels.empty()) {
			for (const auto& t: traces) labels.push_back(fs::path(t).stem().string());
		}
		if (labels.size() != traces.size()) {
			std::cerr << "[cccp-ctl] --labels count must match --traces count.\n";
			return 2;
		}

		// Load every trace as JSON, extract runs[0].tick_hashes from each.
		std::vector<json> tickHashes;
		tickHashes.reserve(traces.size());
		for (size_t i = 0; i < traces.size(); ++i) {
			const json j = ReadJsonFile(traces[i]);
			if (j.is_null()) {
				std::cerr << "[cccp-ctl] could not read " << traces[i] << "\n";
				return 2;
			}
			if (!j.contains("runs") || !j["runs"].is_array() || j["runs"].empty()) {
				std::cerr << "[cccp-ctl] " << traces[i] << ": no runs array.\n";
				return 2;
			}
			const json& run0 = j["runs"][0];
			if (!run0.contains("tick_hashes") || !run0["tick_hashes"].is_array() || run0["tick_hashes"].empty()) {
				std::cerr << "[cccp-ctl] " << traces[i] << ": no tick_hashes array (was -tick-hashes set?).\n";
				return 2;
			}
			tickHashes.push_back(run0["tick_hashes"]);
		}

		// Find the minimum trace length and walk per-tick / per-subsystem.
		// A subsystem is divergent at a tick if any platform's hash differs from platform[0]'s.
		size_t minTicks = tickHashes[0].size();
		for (const auto& t: tickHashes) minTicks = std::min(minTicks, t.size());

		struct PerSubsystem {
			uint64_t firstDivergenceTick = 0;
			bool diverged = false;
			std::vector<std::string> divergentPlatforms; // populated at first divergence
		};
		std::map<std::string, PerSubsystem> perSubsystem;

		uint64_t firstAnyDivergence = 0;
		bool anyDiverged = false;
		uint64_t totalMismatchedTicks = 0;

		for (size_t ti = 0; ti < minTicks; ++ti) {
			const json& baseTick = tickHashes[0][ti];
			const uint64_t tickNum = baseTick.value("tick", uint64_t{ti});
			const json& baseSubs = baseTick.value("subsystems", json::object());
			bool tickDiverged = false;

			for (auto sit = baseSubs.begin(); sit != baseSubs.end(); ++sit) {
				const std::string& sub = sit.key();
				const std::string baseHash = sit.value().get<std::string>();
				std::vector<std::string> divPlats;
				for (size_t pi = 1; pi < tickHashes.size(); ++pi) {
					const json& tick = tickHashes[pi][ti];
					const json& subs = tick.value("subsystems", json::object());
					const std::string h = subs.value(sub, std::string());
					if (h != baseHash) divPlats.push_back(labels[pi]);
				}
				if (!divPlats.empty()) {
					if (!perSubsystem[sub].diverged) {
						perSubsystem[sub].diverged = true;
						perSubsystem[sub].firstDivergenceTick = tickNum;
						perSubsystem[sub].divergentPlatforms = divPlats;
					}
					if (!anyDiverged) {
						firstAnyDivergence = tickNum;
						anyDiverged = true;
					}
					tickDiverged = true;
				}
			}
			if (tickDiverged) totalMismatchedTicks++;
		}

		json perSubsystemJson = json::object();
		for (const auto& [sub, info]: perSubsystem) {
			if (info.diverged) {
				perSubsystemJson[sub] = {
				    {"first_divergence_tick", info.firstDivergenceTick},
				    {"divergent_platforms", info.divergentPlatforms},
				};
			}
		}

		json summary = {
		    {"platforms", labels},
		    {"traces", traces},
		    {"compared_ticks", minTicks},
		    {"diverged", anyDiverged},
		    {"first_divergence_tick", firstAnyDivergence},
		    {"total_mismatched_ticks", totalMismatchedTicks},
		    {"per_subsystem_first_divergence", perSubsystemJson},
		};

		std::ostringstream text;
		text << "Platforms:          " << labels.size() << " (";
		for (size_t i = 0; i < labels.size(); ++i) text << labels[i] << (i + 1 < labels.size() ? ", " : "");
		text << ")\n";
		text << "Compared ticks:     " << minTicks << "\n";
		text << "Result:             " << (anyDiverged ? "DIVERGED" : "MATCH (cross-platform bit-identical)") << "\n";
		if (anyDiverged) {
			text << "First divergence:   tick " << firstAnyDivergence << "\n";
			text << "Mismatched ticks:   " << totalMismatchedTicks << " / " << minTicks << "\n";
			text << "Per subsystem:\n";
			for (const auto& [sub, info]: perSubsystem) {
				if (!info.diverged) continue;
				text << "  " << std::left << std::setw(14) << sub << "  tick " << info.firstDivergenceTick << "  on: ";
				for (size_t i = 0; i < info.divergentPlatforms.size(); ++i) {
					text << info.divergentPlatforms[i] << (i + 1 < info.divergentPlatforms.size() ? ", " : "");
				}
				text << "\n";
			}
		}

		if (!outPath.empty()) {
			std::ofstream o(outPath);
			if (o.is_open()) {
				o << summary.dump(2) << "\n";
				text << "Report written:     " << outPath.string() << "\n";
			} else {
				std::cerr << "[cccp-ctl] failed to write report to " << outPath.string() << "\n";
			}
		}

		PrintTextOrJson(out, summary, text.str());
		return anyDiverged ? 1 : 0;
	}

	// ----- subcommand: bench replay -------------------------------------------

	int CmdBenchReplay(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario;
		int runs = 5;
		uint64_t seed = 42;
		uint64_t maxTicks = 1800;
		int numLuaStates = -1;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintBenchReplayHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "max-ticks") && hasV) { maxTicks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "num-lua-states") && hasV) { numLuaStates = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintBenchReplayHelp(std::cerr);
			return 2;
		}
		if (scenario.empty()) {
			std::cerr << "[cccp-ctl] --scenario is required.\n";
			return 2;
		}
		if (runs < 1) {
			std::cerr << "[cccp-ctl] --runs must be >= 1.\n";
			return 2;
		}
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}

		const fs::path tmp = PickOutDir(out, "cccp-ctl-bench");
		std::vector<double> simCompute;
		std::vector<double> wallSeconds;
		simCompute.reserve(runs);
		wallSeconds.reserve(runs);

		for (int r = 0; r < runs; ++r) {
			const fs::path outPath = tmp / ("run_" + std::to_string(r) + ".json");
			std::ostringstream cmd;
			cmd << Quote(gameBin.string())
			    << " -scenario " << Quote(scenario)
			    << " -seed " << seed
			    << " -max-ticks " << maxTicks
			    << " -out " << Quote(outPath.string());
			if (numLuaStates >= 0) cmd << " -num-lua-states " << numLuaStates;

			// Per-run progress goes to stderr so it doesn't pollute --json stdout.
			if (!out.quiet) std::cerr << "[cccp-ctl] bench run " << (r + 1) << "/" << runs << "\n";
			const int rc = RunSubprocess(cmd.str(), !out.quiet, gameBin.parent_path());
			if (rc < 0 || !fs::exists(outPath)) {
				std::cerr << "[cccp-ctl] bench run " << r << " failed.\n";
				return 2;
			}
			const json j = ReadJsonFile(outPath);
			if (j.is_null() || !j.contains("runs") || j["runs"].empty()) return 2;
			const json& run = j["runs"][0];
			const json& numeric = run.value("numeric", json::object());
			simCompute.push_back(numeric.value("__sim_compute_accum", 0.0));
			wallSeconds.push_back(numeric.value("__wall_seconds", 0.0));
		}

		std::vector<double> sortedSim = simCompute;
		std::sort(sortedSim.begin(), sortedSim.end());
		const double minV = sortedSim.front();
		const double maxV = sortedSim.back();
		const double median = sortedSim[sortedSim.size() / 2];

		json summary = {
		    {"scenario", scenario},
		    {"runs", runs},
		    {"num_lua_states", numLuaStates},
		    {"sim_compute_us", {
		        {"min", minV},
		        {"median", median},
		        {"max", maxV},
		        {"all", simCompute},
		    }},
		    {"wall_seconds", wallSeconds},
		};

		std::ostringstream text;
		text << "Scenario:           " << scenario << "\n";
		text << "Runs:               " << runs << "\n";
		if (numLuaStates >= 0) text << "Lua states (forced): " << numLuaStates << "\n";
		text << "Sim compute (us):\n";
		text << "  min:              " << static_cast<int64_t>(minV) << "\n";
		text << "  median:           " << static_cast<int64_t>(median) << "\n";
		text << "  max:              " << static_cast<int64_t>(maxV) << "\n";
		text << "  spread:           " << std::fixed << std::setprecision(2)
		     << (minV > 0 ? (100.0 * (maxV - minV) / minV) : 0.0) << "%\n";

		PrintTextOrJson(out, summary, text.str());
		return 0;
	}

	// ----- subcommand: test all -----------------------------------------------

	// One sub-step entry in `test all`'s aggregate result.
	struct TestStep {
		std::string name;
		std::string command;     // for diagnostics
		int rc = 0;
		bool passed = false;
		std::string detail;      // free-form one-liner, e.g. "diverged at tick 5"
	};

	int CmdTestAll(int argc, char** argv, const fs::path& selfDir) {
		int runs = 100;
		uint64_t seed = 42;
		uint64_t ticks = 600;
		bool quick = false;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestAllHelp(std::cout); return 0; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "quick")) { quick = true; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestAllHelp(std::cerr);
			return 2;
		}
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}
		if (quick) runs = 3;
		if (runs < 2) runs = 2; // replay-determinism needs >=2

		const std::vector<std::string> matchScenarios = {
		    "M1Baseline", "M1TerrainStress", "M1ActorStress",
		    "M2LuaBaseline", "M2LuaRandomStress", "M2PairsStress",
		    "M2OsStubTest", "M2ModSmokeLoading",
		    "M3TerrainStress",
		};
		const std::string threadList = quick ? "1,4" : "1,2,4,8,16";
		const fs::path runDir = PickOutDir(out, "cccp-ctl-all");

		std::vector<TestStep> steps;

		auto commonTail = [&](std::ostringstream& c) {
			c << " --game-bin " << Quote(gameBin.string());
			if (out.quiet) c << " --quiet";
		};
		// Read the JSON report the child wrote at the known --output path. This is
		// the divergence report from cccp-determinism-check directly — same schema
		// the child reads. Avoids stdout pollution from echo lines + game binary
		// output (which both go through to the terminal regardless).
		auto loadReport = [&](const fs::path& reportPath) -> json {
			if (!fs::exists(reportPath)) return json();
			return ReadJsonFile(reportPath);
		};

		// 1) replay-determinism on each match scenario.
		for (const auto& scen: matchScenarios) {
			TestStep step;
			step.name = "replay-determinism:" + scen;
			const fs::path reportPath = runDir / (scen + "-report.json");
			std::ostringstream c;
			c << Quote(g_selfPath) << " test replay-determinism"
			  << " --scenario " << Quote(scen)
			  << " --runs " << runs
			  << " --seed " << seed
			  << " --ticks " << ticks
			  << " --output " << Quote(reportPath.string());
			commonTail(c);
			step.command = c.str();
			if (!out.quiet) std::cerr << "\n[cccp-ctl] === " << step.name << " ===\n";
			step.rc = RunSubprocess(step.command, !out.quiet);
			const json j = loadReport(reportPath);
			if (j.is_null()) {
				step.passed = false;
				step.detail = "no report at " + reportPath.string() + " (rc=" + std::to_string(step.rc) + ")";
			} else {
				const bool diverged = j.value("diverged", false);
				step.passed = (step.rc == 0) && !diverged;
				step.detail = diverged
				    ? ("diverged at tick " + std::to_string(j.value("first_divergence_tick", uint64_t{0})))
				    : "MATCH";
			}
			steps.push_back(step);
			if (!out.quiet) std::cerr << "  -> " << (step.passed ? "PASS" : "FAIL") << " (" << step.detail << ")\n";
		}

		// 2) thread-matrix on M4ThreadStress.
		{
			TestStep step;
			step.name = "thread-matrix:M4ThreadStress";
			const fs::path reportPath = runDir / "M4ThreadStress-matrix-report.json";
			const int matrixRuns = std::max(1, runs / 30); // matrix is heavier; 3 for runs=100, 1 for quick
			std::ostringstream c;
			c << Quote(g_selfPath) << " test thread-matrix --scenario M4ThreadStress"
			  << " --threads " << threadList
			  << " --runs " << matrixRuns
			  << " --seed " << seed
			  << " --output " << Quote(reportPath.string());
			commonTail(c);
			step.command = c.str();
			if (!out.quiet) std::cerr << "\n[cccp-ctl] === " << step.name << " ===\n";
			step.rc = RunSubprocess(step.command, !out.quiet);
			const json j = loadReport(reportPath);
			if (j.is_null()) {
				step.passed = false;
				step.detail = "no report at " + reportPath.string() + " (rc=" + std::to_string(step.rc) + ")";
			} else {
				const bool diverged = j.value("diverged", false);
				step.passed = (step.rc == 0) && !diverged;
				step.detail = diverged
				    ? ("diverged at tick " + std::to_string(j.value("first_divergence_tick", uint64_t{0})))
				    : "MATCH (FFF-415 acid test PASSED)";
			}
			steps.push_back(step);
			if (!out.quiet) std::cerr << "  -> " << (step.passed ? "PASS" : "FAIL") << " (" << step.detail << ")\n";
		}

		// 3) selftest positive control (M1Baseline). Uses the child's exit code
		// as the signal (selftest exits 0 iff harness_ok, 1 if broken) — no JSON
		// parsing needed, avoids the stdout-pollution problem entirely.
		{
			TestStep step;
			step.name = "selftest:M1Baseline";
			std::ostringstream c;
			c << Quote(g_selfPath) << " test selftest"
			  << " --scenario M1Baseline"
			  << " --runs " << std::max(2, std::min(runs, 5))
			  << " --seed " << seed
			  << " --ticks " << ticks
			  << " --out-dir " << Quote((runDir / "selftest").string());
			commonTail(c);
			step.command = c.str();
			if (!out.quiet) std::cerr << "\n[cccp-ctl] === " << step.name << " ===\n";
			step.rc = RunSubprocess(step.command, !out.quiet);
			step.passed = (step.rc == 0);
			step.detail = step.passed
			    ? "harness catches injected divergence (EC3 OK)"
			    : ("harness FAILED the positive control (rc=" + std::to_string(step.rc) + ")");
			steps.push_back(step);
			if (!out.quiet) std::cerr << "  -> " << (step.passed ? "PASS" : "FAIL") << " (" << step.detail << ")\n";
		}

		// Aggregate.
		int passed = 0, failed = 0;
		for (const auto& s: steps) (s.passed ? passed : failed)++;
		const bool allPassed = (failed == 0);

		json summary = {
		    {"mode", quick ? "quick" : "full"},
		    {"runs_per_scenario", runs},
		    {"thread_matrix", threadList},
		    {"total_steps", static_cast<int>(steps.size())},
		    {"passed", passed},
		    {"failed", failed},
		    {"all_passed", allPassed},
		    {"out_dir", runDir.string()},
		    {"steps", json::array()},
		};
		for (const auto& s: steps) {
			summary["steps"].push_back({
			    {"name", s.name},
			    {"passed", s.passed},
			    {"rc", s.rc},
			    {"detail", s.detail},
			});
		}

		std::ostringstream text;
		text << "\n";
		text << "================================================================\n";
		text << "  cccp-ctl test all — SUITE SUMMARY (" << (quick ? "quick" : "full") << " mode)\n";
		text << "================================================================\n";
		for (const auto& s: steps) {
			text << "  " << (s.passed ? "PASS  " : "FAIL  ") << std::left << std::setw(40) << s.name << "  " << s.detail << "\n";
		}
		text << "----------------------------------------------------------------\n";
		text << "  Total: " << steps.size() << " steps, " << passed << " passed, " << failed << " failed\n";
		text << "  Out dir: " << runDir.string() << "\n";
		text << "  Result: " << (allPassed ? "ALL PASSED" : "FAILURES — see details above") << "\n";

		PrintTextOrJson(out, summary, text.str());
		return allPassed ? 0 : 1;
	}

	// ----- subcommand: test selftest ------------------------------------------
	//
	// EC3 positive control. Runs the scenario WITHOUT --selftest-perturb (expects MATCH)
	// then WITH --selftest-perturb (expects DIVERGED). harness_ok = both expected outcomes.

	int CmdTestSelftest(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario = "M1Baseline";
		int runs = 5;
		uint64_t seed = 42;
		uint64_t ticks = 600;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestSelftestHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestSelftestHelp(std::cerr);
			return 2;
		}
		if (runs < 2) runs = 2;
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}
		const fs::path runDir = PickOutDir(out, "cccp-ctl-selftest");
		const fs::path phase1Report = runDir / "phase1-clean.json";
		const fs::path phase2Report = runDir / "phase2-perturb.json";

		auto commonTail = [&](std::ostringstream& c) {
			c << " --game-bin " << Quote(gameBin.string());
			if (out.quiet) c << " --quiet";
		};

		// Phase 1 — expect MATCH.
		std::ostringstream cleanCmd;
		cleanCmd << Quote(g_selfPath) << " test replay-determinism"
		         << " --scenario " << Quote(scenario)
		         << " --runs " << runs << " --seed " << seed << " --ticks " << ticks
		         << " --output " << Quote(phase1Report.string());
		commonTail(cleanCmd);

		if (!out.quiet) std::cerr << "[cccp-ctl] selftest phase 1 — expect MATCH\n";
		const int rc1 = RunSubprocess(cleanCmd.str(), !out.quiet);
		const json j1 = fs::exists(phase1Report) ? ReadJsonFile(phase1Report) : json();
		const bool phase1Diverged = j1.is_null() ? true : j1.value("diverged", true);
		const bool phase1Pass = (rc1 == 0) && !phase1Diverged;

		// Phase 2 — expect DIVERGED (rc==1 from child).
		std::ostringstream perturbCmd;
		perturbCmd << Quote(g_selfPath) << " test replay-determinism"
		           << " --scenario " << Quote(scenario)
		           << " --runs " << runs << " --seed " << seed << " --ticks " << ticks
		           << " --output " << Quote(phase2Report.string())
		           << " --selftest-perturb";
		commonTail(perturbCmd);

		if (!out.quiet) std::cerr << "[cccp-ctl] selftest phase 2 — expect DIVERGED\n";
		const int rc2 = RunSubprocess(perturbCmd.str(), !out.quiet);
		const json j2 = fs::exists(phase2Report) ? ReadJsonFile(phase2Report) : json();
		const bool phase2Diverged = j2.is_null() ? false : j2.value("diverged", false);
		const bool phase2Pass = (rc2 == 1) && phase2Diverged;

		const bool harnessOk = phase1Pass && phase2Pass;

		json summary = {
		    {"scenario", scenario},
		    {"runs", runs},
		    {"phase1_clean", {
		        {"expected", "MATCH"},
		        {"got_diverged", phase1Diverged},
		        {"rc", rc1},
		        {"pass", phase1Pass},
		    }},
		    {"phase2_perturbed", {
		        {"expected", "DIVERGED"},
		        {"got_diverged", phase2Diverged},
		        {"rc", rc2},
		        {"pass", phase2Pass},
		    }},
		    {"harness_ok", harnessOk},
		    {"out_dir", runDir.string()},
		};

		std::ostringstream text;
		text << "Scenario:           " << scenario << "\n";
		text << "Phase 1 (clean):    " << (phase1Pass ? "PASS (MATCH as expected)" : "FAIL (expected MATCH; got DIVERGED or rc!=0)") << "\n";
		text << "Phase 2 (perturb):  " << (phase2Pass ? "PASS (DIVERGED as expected)" : "FAIL (expected DIVERGED; got MATCH or rc!=1)") << "\n";
		text << "Harness:            " << (harnessOk ? "OK — catches injected non-determinism (EC3 verified)" : "BROKEN — does not detect or false-positives") << "\n";
		text << "Out dir:            " << runDir.string() << "\n";

		PrintTextOrJson(out, summary, text.str());
		return harnessOk ? 0 : 1;
	}

	// ----- subcommand: trace inspect ------------------------------------------

	int CmdTraceInspect(int argc, char** argv) {
		if (argc < 1 || std::string(argv[0]) == "-h" || std::string(argv[0]) == "--help") {
			PrintTraceInspectHelp(std::cout);
			return argc < 1 ? 2 : 0;
		}
		const fs::path tracePath = argv[0];
		bool full = false;
		std::vector<int> ticks;
		std::string subsystemFilter;
		OutputCtx out;

		for (int i = 1; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTraceInspectHelp(std::cout); return 0; }
			if (ArgEq(a, "full")) { full = true; continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = ParseIntList(argv[++i]); continue; }
			if (ArgEq(a, "subsystem") && hasV) { subsystemFilter = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTraceInspectHelp(std::cerr);
			return 2;
		}
		if (!fs::exists(tracePath)) {
			std::cerr << "[cccp-ctl] trace not found: " << tracePath.string() << "\n";
			return 2;
		}
		const json j = ReadJsonFile(tracePath);
		if (j.is_null()) return 2;
		const json& runs = j.value("runs", json::array());
		if (!runs.is_array() || runs.empty()) {
			std::cerr << "[cccp-ctl] trace has no runs[] array\n";
			return 2;
		}
		const json& r0 = runs[0];

		const std::string scenario = r0.value("scenario", std::string());
		const uint64_t seed = r0.value("seed", uint64_t{0});
		const std::string finalHash = r0.value("final_total_hash", std::string());
		const bool passed = r0.value("passed", false);
		const json& numeric = r0.value("numeric", json::object());
		const json& eventCounts = r0.value("event_counts", json::object());
		const json& tickHashes = r0.value("tick_hashes", json::array());

		// Subsystems present in tick 0 (representative).
		std::vector<std::string> subsystems;
		if (tickHashes.is_array() && !tickHashes.empty()) {
			const json& subs = tickHashes[0].value("subsystems", json::object());
			for (auto it = subs.begin(); it != subs.end(); ++it) subsystems.push_back(it.key());
		}

		json summary = {
		    {"trace_path", tracePath.string()},
		    {"scenario", scenario},
		    {"seed", seed},
		    {"passed", passed},
		    {"final_total_hash", finalHash},
		    {"tick_count", tickHashes.size()},
		    {"subsystems_present", subsystems},
		    {"numeric", numeric},
		    {"event_counts_summary", eventCounts},
		};

		std::ostringstream text;
		text << "Trace:              " << tracePath.string() << "\n";
		text << "Scenario:           " << scenario << "\n";
		text << "Seed:               " << seed << "\n";
		text << "Result:             " << (passed ? "PASS" : "FAIL") << "\n";
		text << "Final hash:         " << finalHash << "\n";
		text << "Tick count:         " << tickHashes.size() << "\n";
		text << "Subsystems:         ";
		for (size_t i = 0; i < subsystems.size(); ++i) {
			text << subsystems[i] << (i + 1 < subsystems.size() ? ", " : "");
		}
		text << "\n";
		if (numeric.is_object() && !numeric.empty()) {
			text << "Numeric metrics:\n";
			for (auto it = numeric.begin(); it != numeric.end(); ++it) {
				text << "  " << std::left << std::setw(28) << it.key() << "  " << it.value() << "\n";
			}
		}
		if (eventCounts.is_object() && !eventCounts.empty()) {
			text << "Event counts:\n";
			for (auto it = eventCounts.begin(); it != eventCounts.end(); ++it) {
				text << "  " << std::left << std::setw(40) << it.key() << "  " << it.value() << "\n";
			}
		}
		if (full || !ticks.empty()) {
			text << "Per-tick hashes:\n";
			for (const auto& th: tickHashes) {
				const uint64_t tn = th.value("tick", uint64_t{0});
				if (!ticks.empty() && std::find(ticks.begin(), ticks.end(), static_cast<int>(tn)) == ticks.end()) continue;
				text << "  tick " << std::setw(5) << tn << "  total=" << th.value("total", std::string()).substr(0, 16) << "...\n";
				const json& subs = th.value("subsystems", json::object());
				for (auto it = subs.begin(); it != subs.end(); ++it) {
					if (!subsystemFilter.empty() && it.key() != subsystemFilter) continue;
					text << "    " << std::left << std::setw(14) << it.key() << "  " << it.value().get<std::string>().substr(0, 16) << "...\n";
				}
			}
		}

		PrintTextOrJson(out, summary, text.str());
		return 0;
	}

	// ----- info commands ------------------------------------------------------

	int CmdInfoScenarios(int argc, char** argv) {
		OutputCtx out;
		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			if (a == "-h" || a == "--help") { PrintInfoHelp(std::cout); return 0; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
		}
		json arr = json::array();
		for (const auto& s: kScenarios) {
			arr.push_back({{"name", s.name}, {"milestone", s.milestone}, {"description", s.description}});
		}
		std::ostringstream text;
		text << "Known scenarios (" << kScenarios.size() << " total):\n\n";
		std::string lastMilestone;
		for (const auto& s: kScenarios) {
			if (s.milestone != lastMilestone) {
				text << "[" << s.milestone << "]\n";
				lastMilestone = s.milestone;
			}
			text << "  " << std::left << std::setw(22) << s.name << "  " << s.description << "\n";
		}
		PrintTextOrJson(out, json{{"scenarios", arr}}, text.str());
		return 0;
	}

	int CmdInfoSubsystems(int argc, char** argv) {
		OutputCtx out;
		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			if (a == "-h" || a == "--help") { PrintInfoHelp(std::cout); return 0; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
		}
		json arr = json::array();
		for (const auto& s: kSubsystems) {
			arr.push_back({{"name", s.name}, {"milestone", s.milestone}, {"description", s.description}});
		}
		std::ostringstream text;
		text << "SimChecksum subsystems (" << kSubsystems.size() << " total):\n\n";
		for (const auto& s: kSubsystems) {
			text << "  " << std::left << std::setw(14) << s.name << "  [" << s.milestone << "]  " << s.description << "\n";
		}
		PrintTextOrJson(out, json{{"subsystems", arr}}, text.str());
		return 0;
	}

	int CmdInfoGameBin(int argc, char** argv, const fs::path& selfDir) {
		fs::path gameBin;
		OutputCtx out;
		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintInfoHelp(std::cout); return 0; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
		}
		const fs::path detected = gameBin.empty() ? AutoDetectGameBin(selfDir) : gameBin;
		const bool found = !detected.empty() && fs::exists(detected);

		json summary = {
		    {"override_used", !gameBin.empty()},
		    {"detected_path", detected.string()},
		    {"found", found},
		    {"self_dir", selfDir.string()},
		};
		if (found) {
			std::error_code ec;
			summary["size_bytes"] = static_cast<uint64_t>(fs::file_size(detected, ec));
		}

		std::ostringstream text;
		text << "cccp-ctl location:  " << selfDir.string() << "\n";
		text << "Override used:      " << (gameBin.empty() ? "no" : "yes") << "\n";
		text << "Detected path:      " << (detected.empty() ? "(none)" : detected.string()) << "\n";
		text << "Exists:             " << (found ? "yes" : "no") << "\n";
		if (found) {
			std::error_code ec;
			const auto sz = fs::file_size(detected, ec);
			if (!ec) text << "Size:               " << sz << " bytes\n";
		}
		if (!found) {
			text << "\nHint: pass --game-bin <path> to override. Auto-detect walks up to 8 parent\n";
			text << "directories from cccp-ctl's location and at each level tries:\n";
			text << "  - <dir>/Cortex Command.exe / CortexCommand.exe / cccp.exe (Win)\n";
			text << "  - <dir>/CortexCommand / cccp (Linux/macOS)\n";
			text << "  - <dir>/_Bin/x64/{Final,Release,Debug,Debug Minimal,Debug Full}/<name>\n";
			text << "  - <dir>/_BinTests/x64/{Final,Release,Debug}/<name>\n";
			text << "  - <dir>/build/<name> and <dir>/builddir/<name> (Meson)\n";
			text << "Walk stops early if a repo-root marker (RTEA.sln or meson.build) is found.\n";
		}

		PrintTextOrJson(out, summary, text.str());
		return found ? 0 : 1;
	}

	// ----- stubs --------------------------------------------------------------

	int CmdStub(const std::string& name, const std::string& gatingMilestone) {
		std::cerr << "[cccp-ctl] `test " << name << "` is not yet implemented — needs "
		          << gatingMilestone << ".\n"
		          << "          See engine.html T-VALIDATION for the planned command set.\n";
		return 64; // EX_USAGE-ish: command known, but not actionable here.
	}

	// ----- dispatch -----------------------------------------------------------

	int DispatchTest(int argc, char** argv, const fs::path& selfDir) {
		if (argc < 1) {
			std::cerr << "[cccp-ctl] `test` requires a subcommand. Run `cccp-ctl --help` for the list.\n";
			return 2;
		}
		const std::string sub = argv[0];
		const int subArgc = argc - 1;
		char** subArgv = argv + 1;
		if (sub == "all") return CmdTestAll(subArgc, subArgv, selfDir);
		if (sub == "selftest") return CmdTestSelftest(subArgc, subArgv, selfDir);
		if (sub == "scenario") return CmdTestScenario(subArgc, subArgv, selfDir);
		if (sub == "replay-determinism") return CmdTestReplayDeterminism(subArgc, subArgv, selfDir);
		if (sub == "thread-matrix") return CmdTestThreadMatrix(subArgc, subArgv, selfDir);
		if (sub == "cross-platform-checksum") return CmdTestCrossPlatformChecksum(subArgc, subArgv, selfDir);
		if (sub == "sync-drift") return CmdStub("sync-drift", "M6 (networking)");
		if (sub == "latency-injection") return CmdStub("latency-injection", "M6 (networking)");
		if (sub == "rollback-burst") return CmdStub("rollback-burst", "M9 (rollback)");
		if (sub == "-h" || sub == "--help") { PrintTopHelp(std::cout); return 0; }
		std::cerr << "[cccp-ctl] unknown test subcommand: " << sub << "\n";
		PrintTopHelp(std::cerr);
		return 2;
	}

	int DispatchInfo(int argc, char** argv, const fs::path& selfDir) {
		if (argc < 1) {
			PrintInfoHelp(std::cerr);
			return 2;
		}
		const std::string sub = argv[0];
		const int subArgc = argc - 1;
		char** subArgv = argv + 1;
		if (sub == "scenarios") return CmdInfoScenarios(subArgc, subArgv);
		if (sub == "subsystems") return CmdInfoSubsystems(subArgc, subArgv);
		if (sub == "game-bin") return CmdInfoGameBin(subArgc, subArgv, selfDir);
		if (sub == "version") { std::cout << "cccp-ctl " << kVersion << "\n"; return 0; }
		if (sub == "-h" || sub == "--help") { PrintInfoHelp(std::cout); return 0; }
		std::cerr << "[cccp-ctl] unknown info subcommand: " << sub << "\n";
		PrintInfoHelp(std::cerr);
		return 2;
	}

	int DispatchTrace(int argc, char** argv) {
		if (argc < 1) {
			std::cerr << "[cccp-ctl] `trace` requires a subcommand (currently: inspect).\n";
			return 2;
		}
		const std::string sub = argv[0];
		const int subArgc = argc - 1;
		char** subArgv = argv + 1;
		if (sub == "inspect") return CmdTraceInspect(subArgc, subArgv);
		if (sub == "-h" || sub == "--help") { PrintTraceInspectHelp(std::cout); return 0; }
		std::cerr << "[cccp-ctl] unknown trace subcommand: " << sub << "\n";
		PrintTraceInspectHelp(std::cerr);
		return 2;
	}

	int DispatchBench(int argc, char** argv, const fs::path& selfDir) {
		if (argc < 1) {
			std::cerr << "[cccp-ctl] `bench` requires a subcommand. Run `cccp-ctl --help` for the list.\n";
			return 2;
		}
		const std::string sub = argv[0];
		const int subArgc = argc - 1;
		char** subArgv = argv + 1;
		if (sub == "replay") return CmdBenchReplay(subArgc, subArgv, selfDir);
		if (sub == "-h" || sub == "--help") { PrintTopHelp(std::cout); return 0; }
		std::cerr << "[cccp-ctl] unknown bench subcommand: " << sub << "\n";
		PrintTopHelp(std::cerr);
		return 2;
	}

} // namespace

int main(int argc, char** argv) {
	const fs::path selfDir = ResolveSelfDir(argc > 0 ? argv[0] : "");
	// Cache self-path for sibling-re-invocation from `test all` / `test selftest`.
	g_selfPath = (argc > 0 && argv[0]) ? std::filesystem::absolute(argv[0]).string() : std::string("cccp-ctl");

	if (argc < 2) {
		PrintTopHelp(std::cerr);
		return 2;
	}
	const std::string cmd = argv[1];
	if (cmd == "-h" || cmd == "--help") { PrintTopHelp(std::cout); return 0; }
	if (cmd == "--version" || cmd == "-V") {
		std::cout << "cccp-ctl " << kVersion << "\n";
		return 0;
	}

	const int subArgc = argc - 2;
	char** subArgv = argv + 2;
	if (cmd == "test") return DispatchTest(subArgc, subArgv, selfDir);
	if (cmd == "bench") return DispatchBench(subArgc, subArgv, selfDir);
	if (cmd == "info") return DispatchInfo(subArgc, subArgv, selfDir);
	if (cmd == "trace") return DispatchTrace(subArgc, subArgv);

	std::cerr << "[cccp-ctl] unknown command: " << cmd << "\n";
	PrintTopHelp(std::cerr);
	return 2;
}
