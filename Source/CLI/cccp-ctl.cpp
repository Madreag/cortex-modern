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
// Subcommands (V1.4):
//   test all                    Aggregate M1-M4 suite runner
//   test selftest               EC3 positive control (verify harness)
//   test scenario               Run a single scenario
//   test replay-determinism     Wraps -determinism-check (N runs, diff)
//   test thread-matrix          Wraps -determinism-check --threads (M4 acid test)
//   test cross-platform-checksum  Diff trace JSONs from multiple platforms
//   test mp-sync-drift          Spawn N engine processes in parallel, diff traces
//   test latency-injection      Deterministic network simulator + parallel diff
//   test snapshot-restore       Snapshot/restore tester (engine support pending M8)
//   test rollback-burst         Rollback tester (engine support pending M8)
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
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

	using nlohmann::json;
	namespace fs = std::filesystem;

	constexpr const char* kVersion = "0.4.0";

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

	// The engine references Data/... as relative paths so its CWD must contain Data/.
	// On Windows MSBuild output the engine lives at the repo root so gameBin.parent_path()
	// is correct. On Linux Meson the engine lives at builddir/CortexCommand while Data/
	// lives at the repo root — gameBin.parent_path() is wrong. Walk up looking for Data/.
	fs::path FindEngineWorkDir(const fs::path& gameBin) {
		fs::path cur = gameBin.parent_path();
		for (int up = 0; up < 8; ++up) {
			if (fs::exists(cur / "Data")) return cur;
			const fs::path parent = cur.parent_path();
			if (parent.empty() || parent == cur) break;
			cur = parent;
		}
		return gameBin.parent_path();
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

	// Wrap a command so the shell cd's into <dir> before invoking it, without
	// touching the parent process's CWD. Used by the --parallel branch of
	// RunEngines to dodge the std::async race on process-wide fs::current_path().
	std::string WrapCmdWithCd(const std::string& cmd, const fs::path& dir) {
		if (dir.empty()) return cmd;
		std::ostringstream w;
#ifdef _WIN32
		// std::system on Windows invokes cmd.exe; "cd /d" handles drive changes.
		w << "cd /d " << Quote(dir.string()) << " && " << cmd;
#else
		w << "cd " << Quote(dir.string()) << " && " << cmd;
#endif
		return w.str();
	}

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
		    "  test mp-suite                  Aggregate MP correctness suite (Blocks A-D in one)\n"
		    "  test mp-sync-drift             Spawn N engine processes in parallel, diff traces\n"
		    "  test latency-injection         Deterministic network simulator + parallel-engine diff\n"
		    "  test snapshot-restore          Sim snapshot/restore tester (phase 1 baseline; phases 2-3 pending M8)\n"
		    "  test rollback-burst            Rollback tester (phases 1-2 + burst plan; phase 3 pending M8)\n"
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

	void PrintTestMpSuiteHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test mp-suite — aggregate MP test runner (Blocks A-D)\n"
		    "\n"
		    "Runs the four MP-correctness subcommands as one CI-friendly batch:\n"
		    "  1. mp-sync-drift     (clean MATCH on M4ThreadStress)\n"
		    "  2. mp-sync-drift     (EC3-MP positive control on M1Baseline)\n"
		    "  3. latency-injection (perfect conditions — verifies pipeline)\n"
		    "  4. snapshot-restore  (baseline phase; phases 2-3 pending M8)\n"
		    "  5. rollback-burst    (baseline + plan; phase 3 pending M8)\n"
		    "\n"
		    "Exits 0 iff every step's harness_ok holds. Designed as the single CI\n"
		    "step that gates an MP-related PR merge: `cccp-ctl test mp-suite --quick`.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test mp-suite [options]\n"
		    "\n"
		    "Options:\n"
		    "  --ticks <N>            Per-step sim-tick cap. Default 200.\n"
		    "  --seed <N>             RNG seed propagated to every step. Default 42.\n"
		    "  --parallel             Enable mp-sync-drift / latency-injection parallel\n"
		    "                         spawn (CI / headless only).\n"
		    "  --quick                Cut ticks down for smoke runs (ticks → 60).\n"
		    "  --out-dir <path>       Direct all reports here.\n"
		    "  --game-bin <path>      Path to game binary. Default: auto-detect.\n"
		    "  --json                 JSON aggregate output.\n"
		    "  --quiet                Suppress per-step command echo.\n"
		    "  -h, --help             This help.\n";
	}

	void PrintTestMpSyncDriftHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test mp-sync-drift — two-process sim sync verification\n"
		    "                              (alias: cccp-ctl test sync-drift)\n"
		    "\n"
		    "Spawns >=2 engine processes in parallel, each running the same scenario+seed\n"
		    "with per-tick BLAKE3 hashing on. Compares the resulting traces tick-by-tick\n"
		    "and reports MATCH or DIVERGED-AT-TICK-N. A local MP bridge endpoint (Unix\n"
		    "domain socket on POSIX, named pipe on Windows) is created as a scaffolding\n"
		    "hook for the engine-side MP code that M6/M7 will add; today no engine\n"
		    "connects to it, so input distribution / tick coordination are post-hoc\n"
		    "trace comparison only.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test mp-sync-drift [--scenario <name>] [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>           Scenario preset-suffix. Default M4ThreadStress.\n"
		    "  --ticks <N>                 Sim-tick cap per process. Default 600.\n"
		    "  --seed <N>                  RNG seed. Default 42.\n"
		    "  --processes <N>             How many engine processes to spawn. Default 2.\n"
		    "  --peers <N>                 Alias for --processes <N+1> (1 host + N peers).\n"
		    "  --runs <N>                  Repeat the test N times. Default 1.\n"
		    "  --num-lua-states <N>        Pin Lua-state count per engine. Default 4.\n"
		    "  --input-script <path>       Pre-recorded input script (scaffolded — engine\n"
		    "                              -side replay pending M6 networking).\n"
		    "  --inject-divergence-role <role>\n"
		    "                              Drive -determinism-selftest-perturb on the\n"
		    "                              named role (`host` / `peer1` / `peer2` ...).\n"
		    "                              `peer` accepted as alias for `peer1`.\n"
		    "                              Test then PASSES iff divergence is detected.\n"
		    "                              Use to verify the comparator (EC3-MP control).\n"
		    "  --keep-traces               Keep per-process trace JSONs after diff.\n"
		    "  --parallel                  Spawn engines concurrently. Default OFF:\n"
		    "                              two visible game windows fight over input\n"
		    "                              focus on Windows desktop and stall. Use\n"
		    "                              --parallel only on headless / Xvfb / CI.\n"
		    "  --output <path>             Where to write the aggregate report.\n"
		    "  --out-dir <path>            Direct temp/output dir.\n"
		    "  --game-bin <path>           Path to game binary. Default: auto-detect.\n"
		    "  --json                      JSON output.\n"
		    "  --quiet                     Suppress per-spawn echo on stderr.\n"
		    "  -h, --help                  This help.\n"
		    "\n"
		    "Exit codes:\n"
		    "  0   MATCH (or, with --inject-divergence-role, divergence correctly detected)\n"
		    "  1   DIVERGED (or, with --inject-divergence-role, divergence NOT detected)\n"
		    "  2   engine spawn failure / usage error\n";
	}

	void PrintTestLatencyInjectionHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test latency-injection — deterministic network condition simulator\n"
		    "\n"
		    "Layers a tick-based network simulator over the Block A mp-sync-drift harness.\n"
		    "Generates a reproducible packet trace (per send tick: delivered? arrival tick?)\n"
		    "given (network-seed, packet-loss, latency, jitter, reorder). The trace is\n"
		    "saved as part of the report for M7's eventual lockstep / reconciliation logic\n"
		    "to consume. Today the engine has no network layer, so the sim itself always\n"
		    "matches across the two processes regardless of simulated conditions; the\n"
		    "value of this subcommand at M5.5 is verifying simulator determinism + saving\n"
		    "the reference packet traces M7 will replay against.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test latency-injection [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>           Scenario preset-suffix. Default M1Baseline.\n"
		    "  --ticks <N>                 Sim-tick cap per process. Default 600.\n"
		    "  --seed <N>                  Sim RNG seed. Default 42.\n"
		    "  --network-seed <N>          Network simulator RNG seed (separate from sim).\n"
		    "                              Default 1337. Decision #6 (separate seed).\n"
		    "  --runs <N>                  Repeat the test N times. Default 1.\n"
		    "  --processes <N>             How many engine processes. Default 2.\n"
		    "  --packet-loss <pct>         %% chance each send packet is dropped. Default 0.\n"
		    "  --latency-ms <ms>           Base one-way delay in ms. Default 0.\n"
		    "  --jitter-ms <ms>            +/- jitter in ms (uniform). Default 0.\n"
		    "  --reorder-pct <pct>         %% chance of adjacent-packet swap. Default 0.\n"
		    "  --num-lua-states <N>        Pin Lua-state count per engine. Default 4.\n"
		    "  --tick-rate <hz>            Sim tick rate for ms->tick conversion. Default 60.\n"
		    "  --parallel                  Spawn engines concurrently (CI / headless only).\n"
		    "  --keep-traces               Keep per-process engine trace JSONs.\n"
		    "  --output <path>             Where to write the aggregate report.\n"
		    "  --out-dir <path>            Direct temp/output dir.\n"
		    "  --game-bin <path>           Path to game binary. Default: auto-detect.\n"
		    "  --json                      JSON output.\n"
		    "  --quiet                     Suppress per-spawn echo on stderr.\n"
		    "  -h, --help                  This help.\n"
		    "\n"
		    "Exit codes:\n"
		    "  0   sim MATCH + packet trace reproducible across --runs\n"
		    "  1   sim DIVERGED or packet trace inconsistent across --runs\n"
		    "  2   engine spawn failure / usage error\n";
	}

	void PrintTestSnapshotRestoreHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test snapshot-restore — sim-state snapshot/restore tester\n"
		    "\n"
		    "Forward-looking M8 rollback infrastructure. Verifies that the engine's\n"
		    "snapshot/restore round-trips bit-identically: traceA (no snapshot) ==\n"
		    "traceB (snapshot at tick S, continue) == traceC (snapshot at tick S,\n"
		    "mutate, restore, continue). M5.5 ships the tester scaffolding; phase 1\n"
		    "(baseline) runs today, phases 2-3 gracefully report 'engine snapshot\n"
		    "API pending M8' until M8 lands. When M8 lands the same subcommand\n"
		    "exercises the full round-trip with no flag changes.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test snapshot-restore [--scenario <name>] [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>           Scenario preset-suffix. Default M1Baseline.\n"
		    "  --ticks <N>                 Total sim-tick cap. Default 300.\n"
		    "  --snapshot-at <tick>        Tick at which to snapshot (phase 2/3). Default 100.\n"
		    "  --resume-at <tick>          Tick at which to resume from snapshot. Default = snapshot-at.\n"
		    "  --seed <N>                  RNG seed. Default 42.\n"
		    "  --runs <N>                  Repeat the test N times. Default 1.\n"
		    "  --num-lua-states <N>        Pin Lua-state count. Default 4.\n"
		    "  --keep-traces               Keep per-phase trace JSONs.\n"
		    "  --strict                    Exit 1 (not 0) if engine snapshot API is missing.\n"
		    "                              Default: exit 0 with 'pending M8' message — Block C\n"
		    "                              is CI-friendly until M8 lands.\n"
		    "  --output <path>             Where to write the aggregate report.\n"
		    "  --out-dir <path>            Direct temp/output dir.\n"
		    "  --game-bin <path>           Path to game binary. Default: auto-detect.\n"
		    "  --json                      JSON output.\n"
		    "  --quiet                     Suppress per-spawn echo on stderr.\n"
		    "  -h, --help                  This help.\n"
		    "\n"
		    "Exit codes:\n"
		    "  0   baseline OK; phases 2-3 pending M8 (CI-friendly default)\n"
		    "  1   baseline failed OR (--strict) snapshot phases pending\n"
		    "  2   engine spawn failure / usage error\n";
	}

	void PrintTestRollbackBurstHelp(std::ostream& out) {
		out <<
		    "cccp-ctl test rollback-burst — rollback machinery tester\n"
		    "\n"
		    "Forward-looking M8 tester. The eventual rollback flow is: client\n"
		    "predicts input, gets a correction, rolls back to the mispredict tick,\n"
		    "re-executes with the correct input, arrives at the present-tick with\n"
		    "the reconciled state. The acceptance test compares final state to a\n"
		    "no-rollback baseline (must be identical) and reports rollback wall-\n"
		    "clock overhead per burst.\n"
		    "\n"
		    "M5.5 ships: phase 1 (baseline trace, runs today) + phase 2 (deterministic\n"
		    "burst plan — which ticks get mispredictions + the per-burst depth — saved\n"
		    "as JSON artifact M8 will replay). Phase 3 (apply burst plan, rollback,\n"
		    "verify) gracefully reports PENDING_M8 until the engine rollback API lands.\n"
		    "\n"
		    "Usage:\n"
		    "  cccp-ctl test rollback-burst [--scenario <name>] [options]\n"
		    "\n"
		    "Options:\n"
		    "  --scenario <name>           Scenario preset-suffix. Default M1Baseline.\n"
		    "  --ticks <N>                 Total sim-tick cap. Default 300.\n"
		    "  --seed <N>                  Sim RNG seed. Default 42.\n"
		    "  --burst-seed <N>            Burst-plan RNG seed. Default 4242.\n"
		    "  --burst-size <N>            Number of mispredictions to inject. Default 5.\n"
		    "  --burst-depth <N>           Frames between mispredict and correction. Default 6.\n"
		    "  --runs <N>                  Repeat the test N times. Default 1.\n"
		    "  --num-lua-states <N>        Pin Lua-state count. Default 4.\n"
		    "  --keep-traces               Keep per-phase trace JSONs.\n"
		    "  --strict                    Exit 1 if engine rollback API is missing.\n"
		    "                              Default: exit 0 with 'pending M8' message — Block D\n"
		    "                              is CI-friendly until M8 lands.\n"
		    "  --output <path>             Where to write the aggregate report.\n"
		    "  --out-dir <path>            Direct temp/output dir.\n"
		    "  --game-bin <path>           Path to game binary. Default: auto-detect.\n"
		    "  --json                      JSON output.\n"
		    "  --quiet                     Suppress per-spawn echo on stderr.\n"
		    "  -h, --help                  This help.\n"
		    "\n"
		    "Exit codes:\n"
		    "  0   baseline OK + burst plan deterministic; phase 3 pending M8 (default)\n"
		    "  1   baseline failed OR plan non-deterministic OR (--strict) phase 3 pending\n"
		    "  2   engine spawn failure / usage error\n";
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
			if (!o.is_open()) {
				std::cerr << "[cccp-ctl] failed to open report file: " << outPath.string() << "\n";
				return 2;
			}
			o << summary.dump(2) << "\n";
			text << "Report written:     " << outPath.string() << "\n";
		}

		PrintTextOrJson(out, summary, text.str());
		return anyDiverged ? 1 : 0;
	}

	// ----- MP harness shared infrastructure -----------------------------------
	//
	// Block A (mp-sync-drift) spawns N engine processes in parallel; Blocks B-D
	// reuse the same scaffolding. Today the harness post-hoc diffs trace JSONs;
	// the bridge endpoint is a structural placeholder for the M6/M7 engine-side
	// MP code. No third-party deps — POSIX UDS or Win32 named pipe + std::async.

	// Move-only RAII so early-return / exception paths can't leak the socket fd
	// or named-pipe HANDLE.
	struct BridgeEndpoint {
		std::string displayPath;
		bool created = false;
#ifdef _WIN32
		HANDLE handle = INVALID_HANDLE_VALUE;
#else
		int fd = -1;
		std::string sockPath;
#endif

		BridgeEndpoint() = default;
		BridgeEndpoint(const BridgeEndpoint&) = delete;
		BridgeEndpoint& operator=(const BridgeEndpoint&) = delete;
		BridgeEndpoint(BridgeEndpoint&& o) noexcept { MoveFrom(o); }
		BridgeEndpoint& operator=(BridgeEndpoint&& o) noexcept {
			if (this != &o) { Release(); MoveFrom(o); }
			return *this;
		}
		~BridgeEndpoint() { Release(); }

	private:
		void MoveFrom(BridgeEndpoint& o) {
			displayPath = std::move(o.displayPath);
			created = o.created;
			o.created = false;
#ifdef _WIN32
			handle = o.handle;
			o.handle = INVALID_HANDLE_VALUE;
#else
			fd = o.fd;
			sockPath = std::move(o.sockPath);
			o.fd = -1;
#endif
		}
		void Release() {
			if (!created) return;
#ifdef _WIN32
			if (handle != INVALID_HANDLE_VALUE) {
				::CloseHandle(handle);
				handle = INVALID_HANDLE_VALUE;
			}
#else
			if (fd >= 0) {
				::close(fd);
				fd = -1;
			}
			if (!sockPath.empty()) {
				::unlink(sockPath.c_str());
			}
#endif
			created = false;
		}
	};

	BridgeEndpoint CreateBridge(const fs::path& outDir, const std::string& tag) {
		BridgeEndpoint b;
#ifdef _WIN32
		(void)outDir;
		const std::string pipeName = "\\\\.\\pipe\\cccp-ctl-bridge-" + tag + "-" +
		                             std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId()));
		HANDLE h = ::CreateNamedPipeA(
		    pipeName.c_str(),
		    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		    8,
		    4096, 4096, 0,
		    nullptr);
		if (h == INVALID_HANDLE_VALUE) return b;
		b.handle = h;
		b.displayPath = pipeName;
		b.created = true;
#else
		const fs::path sockPath = outDir / ("bridge-" + tag + ".sock");
		const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) return b;
		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		const std::string p = sockPath.string();
		if (p.size() >= sizeof(addr.sun_path)) {
			::close(fd);
			return b;
		}
		std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
		::unlink(addr.sun_path);
		if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
			::close(fd);
			return b;
		}
		if (::listen(fd, 4) < 0) {
			::close(fd);
			::unlink(addr.sun_path);
			return b;
		}
		b.fd = fd;
		b.sockPath = p;
		b.displayPath = p;
		b.created = true;
#endif
		return b;
	}

	// One engine process's outcome inside an MP run.
	struct EngineRunResult {
		std::string role;
		fs::path outPath;
		int rc = -1;
		bool passed = false;
		bool tracesProduced = false;
		std::string finalHash;
		json tickHashes;
	};

	// Per-role pair diff against role[0] (host). Mirrors the cross-platform-checksum
	// pattern: first divergence tick, per-subsystem first divergence, per-role status.
	void DiffEngineTraces(
	    const std::vector<EngineRunResult>& results,
	    bool& divergedOut,
	    uint64_t& firstDivergenceTickOut,
	    uint64_t& comparedTicksOut,
	    json& perSubsystemOut,
	    std::map<std::string, std::string>& perRolePairResultOut) {
		divergedOut = false;
		firstDivergenceTickOut = 0;
		comparedTicksOut = 0;
		perSubsystemOut = json::object();
		perRolePairResultOut.clear();
		if (results.size() < 2) return;
		if (!results[0].tracesProduced) return;
		const json& baseTicks = results[0].tickHashes;
		if (!baseTicks.is_array() || baseTicks.empty()) return;

		size_t minTicks = baseTicks.size();
		for (size_t i = 1; i < results.size(); ++i) {
			if (!results[i].tracesProduced) continue;
			if (!results[i].tickHashes.is_array()) continue;
			minTicks = std::min(minTicks, results[i].tickHashes.size());
		}
		comparedTicksOut = static_cast<uint64_t>(minTicks);

		struct SubsystemDiv {
			bool diverged = false;
			uint64_t firstTick = 0;
			std::vector<std::string> divergentRoles;
		};
		std::map<std::string, SubsystemDiv> perSubsystem;

		for (size_t i = 1; i < results.size(); ++i) {
			perRolePairResultOut[results[i].role] = results[i].tracesProduced ? "MATCH" : "no trace";
		}

		for (size_t ti = 0; ti < minTicks; ++ti) {
			const json& baseTick = baseTicks[ti];
			const uint64_t tickNum = baseTick.value("tick", uint64_t{ti});
			const json& baseSubs = baseTick.value("subsystems", json::object());
			for (auto sit = baseSubs.begin(); sit != baseSubs.end(); ++sit) {
				const std::string sub = sit.key();
				const std::string baseHash = sit.value().get<std::string>();
				for (size_t pi = 1; pi < results.size(); ++pi) {
					if (!results[pi].tracesProduced) continue;
					if (!results[pi].tickHashes.is_array() || ti >= results[pi].tickHashes.size()) continue;
					const json& peerTick = results[pi].tickHashes[ti];
					const json& peerSubs = peerTick.value("subsystems", json::object());
					const std::string h = peerSubs.value(sub, std::string());
					if (h != baseHash) {
						auto& info = perSubsystem[sub];
						if (!info.diverged) {
							info.diverged = true;
							info.firstTick = tickNum;
						}
						if (std::find(info.divergentRoles.begin(), info.divergentRoles.end(), results[pi].role) == info.divergentRoles.end()) {
							info.divergentRoles.push_back(results[pi].role);
						}
						if (perRolePairResultOut[results[pi].role] == "MATCH") {
							perRolePairResultOut[results[pi].role] = "DIVERGED at tick " + std::to_string(tickNum);
						}
						if (!divergedOut) {
							divergedOut = true;
							firstDivergenceTickOut = tickNum;
						}
					}
				}
			}
		}

		for (const auto& [sub, info]: perSubsystem) {
			if (!info.diverged) continue;
			perSubsystemOut[sub] = {
			    {"first_divergence_tick", info.firstTick},
			    {"divergent_roles", info.divergentRoles},
			};
		}
	}

	// Role labels: host, peer1, peer2, peer3, ... Consistent numeric suffix
	// across all peers so `--inject-divergence-role peer1` is unambiguous.
	// `peer` (no suffix) is accepted as a back-compat alias for `peer1`.
	std::vector<std::string> BuildRoleLabels(int processes) {
		std::vector<std::string> roles;
		roles.reserve(processes);
		roles.push_back("host");
		for (int i = 1; i < processes; ++i) {
			roles.push_back("peer" + std::to_string(i));
		}
		return roles;
	}

	// Normalize a user-supplied role label. Accepts the legacy "peer" form and
	// remaps it to "peer1" so the rest of the harness sees one canonical name.
	std::string NormalizeRoleLabel(const std::string& s) {
		return s == "peer" ? std::string("peer1") : s;
	}

	// Spawn N engine subprocesses, wait for all, load each trace JSON into
	// EngineRunResult. Per-role extra flags (e.g. -determinism-selftest-perturb on
	// one role for the EC3-MP positive control) are appended to that role's command
	// line. parallel=true uses std::async; parallel=false runs sequentially. Default
	// is sequential because two visible game windows on Windows desktop fight over
	// input focus and stall; CI / headless / Xvfb runs are safe with --parallel.
	std::vector<EngineRunResult> RunEngines(
	    const fs::path& gameBin,
	    const std::string& scenario,
	    uint64_t seed,
	    uint64_t ticks,
	    int numLuaStates,
	    const std::vector<std::string>& roles,
	    const std::map<std::string, std::string>& perRoleExtraFlags,
	    const fs::path& runDir,
	    bool quietEcho,
	    bool parallel,
	    int& engineFailuresOut) {
		engineFailuresOut = 0;
		const size_t n = roles.size();
		std::vector<fs::path> roleOuts(n);
		std::vector<std::string> cmds(n);
		// Engine needs CWD = dir containing Data/. On Linux Meson that's the repo
		// root, not gameBin.parent_path() (= builddir/). Walk up to find it.
		const fs::path engineCwd = FindEngineWorkDir(gameBin);

		for (size_t p = 0; p < n; ++p) {
			roleOuts[p] = runDir / (roles[p] + ".json");
			std::ostringstream cmd;
			cmd << Quote(gameBin.string())
			    << " -scenario " << Quote(scenario)
			    << " -seed " << seed
			    << " -max-ticks " << ticks
			    << " -tick-hashes"
			    << " -num-lua-states " << numLuaStates
			    << " -out " << Quote(roleOuts[p].string());
			auto it = perRoleExtraFlags.find(roles[p]);
			if (it != perRoleExtraFlags.end() && !it->second.empty()) {
				cmd << " " << it->second;
			}
			cmds[p] = cmd.str();
		}

		std::vector<int> rcs(n, -1);
		if (parallel) {
			// Embed cd into the command so the shell handles cwd; avoids the
			// process-wide chdir race CwdGuard would introduce across threads.
			std::vector<std::future<int>> futures;
			futures.reserve(n);
			for (size_t p = 0; p < n; ++p) {
				if (!quietEcho) std::cerr << "[cccp-ctl] spawning role=" << roles[p] << " (parallel)\n";
				const std::string wrapped = WrapCmdWithCd(cmds[p], engineCwd);
				futures.push_back(std::async(std::launch::async, [wrapped, quietEcho]() {
					return RunSubprocess(wrapped, !quietEcho, {});
				}));
			}
			for (size_t p = 0; p < n; ++p) rcs[p] = futures[p].get();
		} else {
			for (size_t p = 0; p < n; ++p) {
				if (!quietEcho) std::cerr << "[cccp-ctl] running role=" << roles[p] << " (sequential " << (p + 1) << "/" << n << ")\n";
				rcs[p] = RunSubprocess(cmds[p], !quietEcho, engineCwd);
			}
		}

		std::vector<EngineRunResult> results;
		results.reserve(n);
		for (size_t p = 0; p < n; ++p) {
			EngineRunResult er;
			er.role = roles[p];
			er.rc = rcs[p];
			er.outPath = roleOuts[p];
			if (er.rc < 0 || !fs::exists(roleOuts[p])) {
				++engineFailuresOut;
				std::cerr << "[cccp-ctl] " << er.role << " engine failed (rc=" << er.rc
				          << ", trace=" << (fs::exists(roleOuts[p]) ? "present" : "missing") << ")\n";
				results.push_back(std::move(er));
				continue;
			}
			const json j = ReadJsonFile(roleOuts[p]);
			if (j.is_null() || !j.contains("runs") || !j["runs"].is_array() || j["runs"].empty()) {
				++engineFailuresOut;
				std::cerr << "[cccp-ctl] " << er.role << " produced no usable trace at " << roleOuts[p].string() << "\n";
				results.push_back(std::move(er));
				continue;
			}
			const json& run0 = j["runs"][0];
			er.passed = run0.value("passed", false);
			er.finalHash = run0.value("final_total_hash", std::string());
			er.tickHashes = run0.value("tick_hashes", json::array());
			er.tracesProduced = true;
			results.push_back(std::move(er));
		}
		return results;
	}

	// ----- subcommand: test mp-sync-drift -------------------------------------

	int CmdTestMpSyncDrift(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario = "M4ThreadStress";
		uint64_t seed = 42;
		uint64_t ticks = 600;
		int processes = 2;
		int runs = 1;
		int numLuaStates = 4;
		std::string injectDivRole;
		std::string inputScript;
		bool keepTraces = false;
		bool parallel = false;
		fs::path outPath;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestMpSyncDriftHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "processes") && hasV) { processes = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "peers") && hasV) { processes = std::atoi(argv[++i]) + 1; continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "num-lua-states") && hasV) { numLuaStates = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "input-script") && hasV) { inputScript = argv[++i]; continue; }
			if (ArgEq(a, "inject-divergence-role") && hasV) { injectDivRole = argv[++i]; continue; }
			if (ArgEq(a, "keep-traces")) { keepTraces = true; continue; }
			if (ArgEq(a, "parallel")) { parallel = true; continue; }
			if (ArgEq(a, "output") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestMpSyncDriftHelp(std::cerr);
			return 2;
		}
		if (scenario.empty()) {
			std::cerr << "[cccp-ctl] --scenario is required (default: M4ThreadStress).\n";
			return 2;
		}
		if (processes < 2) {
			std::cerr << "[cccp-ctl] --processes must be >= 2.\n";
			return 2;
		}
		if (ticks == 0) {
			std::cerr << "[cccp-ctl] --ticks must be > 0.\n";
			return 2;
		}
		if (runs < 1) runs = 1;
		if (runs == 1 && !out.quiet) {
			std::cerr << "[cccp-ctl] warning: --runs 1 cannot detect determinism divergence; consider --runs 2 or more.\n";
		}
		if (numLuaStates < 1) numLuaStates = 1;
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}

		const std::vector<std::string> roles = BuildRoleLabels(processes);
		if (!injectDivRole.empty()) injectDivRole = NormalizeRoleLabel(injectDivRole);
		if (!injectDivRole.empty() && std::find(roles.begin(), roles.end(), injectDivRole) == roles.end()) {
			std::cerr << "[cccp-ctl] --inject-divergence-role must be one of: ";
			for (size_t i = 0; i < roles.size(); ++i) {
				std::cerr << roles[i] << (i + 1 < roles.size() ? ", " : "");
			}
			std::cerr << "\n";
			return 2;
		}

		const fs::path runDir = PickOutDir(out, "cccp-ctl-mp-sync-drift");
		if (outPath.empty()) outPath = runDir / "report.json";

		if (!inputScript.empty() && !out.quiet) {
			std::cerr << "[cccp-ctl] --input-script accepted but engine-side input replay is\n"
			          << "          pending M6 (networking). Inputs today come from the scenario\n"
			          << "          script; the flag is reserved for forward compatibility.\n";
		}

		std::map<std::string, std::string> perRoleFlags;
		if (!injectDivRole.empty()) {
			perRoleFlags[injectDivRole] = "-determinism-selftest-perturb";
		}

		json runReports = json::array();
		bool anyDiverged = false;
		int totalEngineFailures = 0;

		for (int r = 0; r < runs; ++r) {
			const fs::path thisRunDir = runDir / ("run-" + std::to_string(r));
			std::error_code ec;
			fs::create_directories(thisRunDir, ec);
			BridgeEndpoint bridge = CreateBridge(thisRunDir, std::to_string(r));

			int engineFailures = 0;
			const std::vector<EngineRunResult> results = RunEngines(
			    gameBin, scenario, seed, ticks, numLuaStates,
			    roles, perRoleFlags, thisRunDir, out.quiet, parallel, engineFailures);
			totalEngineFailures += engineFailures;

			bool diverged = false;
			uint64_t firstDiv = 0;
			uint64_t comparedTicks = 0;
			json perSubsystem = json::object();
			std::map<std::string, std::string> perRolePairResult;
			if (engineFailures == 0) {
				DiffEngineTraces(results, diverged, firstDiv, comparedTicks, perSubsystem, perRolePairResult);
			}
			if (diverged) anyDiverged = true;

			json runReport = {
			    {"run_index", r},
			    {"bridge_path", bridge.displayPath},
			    {"bridge_status", bridge.created ? "scaffold (engine wiring pending M6/M7)" : "creation failed"},
			    {"engine_results", json::array()},
			    {"diverged", diverged},
			    {"first_divergence_tick", firstDiv},
			    {"compared_ticks", comparedTicks},
			    {"per_subsystem_first_divergence", perSubsystem},
			    {"per_role_pair_result", perRolePairResult},
			    {"engine_failures", engineFailures},
			};
			for (const auto& er: results) {
				runReport["engine_results"].push_back({
				    {"role", er.role},
				    {"rc", er.rc},
				    {"passed", er.passed},
				    {"final_hash", er.finalHash},
				    {"out_path", er.outPath.string()},
				    {"trace_present", er.tracesProduced},
				});
			}
			runReports.push_back(runReport);

			if (!keepTraces && engineFailures == 0) {
				std::error_code ec2;
				fs::remove_all(thisRunDir, ec2);
			}
		}

		const bool expectedDivergence = !injectDivRole.empty();
		bool harnessOk = false;
		if (totalEngineFailures > 0) {
			harnessOk = false;
		} else if (expectedDivergence) {
			harnessOk = anyDiverged;
		} else {
			harnessOk = !anyDiverged;
		}

		json summary = {
		    {"scenario", scenario},
		    {"ticks", ticks},
		    {"seed", seed},
		    {"processes", processes},
		    {"roles", roles},
		    {"runs", runs},
		    {"num_lua_states", numLuaStates},
		    {"injected_divergence_role", injectDivRole},
		    {"expected_divergence", expectedDivergence},
		    {"parallel", parallel},
		    {"diverged", anyDiverged},
		    {"engine_failures_total", totalEngineFailures},
		    {"harness_ok", harnessOk},
		    {"runs_detail", runReports},
		    {"report_path", outPath.string()},
		    {"out_dir", runDir.string()},
		};

		std::ofstream o(outPath);
		if (!o.is_open()) {
			std::cerr << "[cccp-ctl] failed to open report file: " << outPath.string() << "\n";
			return 2;
		}
		o << summary.dump(2) << "\n";

		std::ostringstream text;
		text << "Scenario:           " << scenario << "\n";
		text << "Processes:          " << processes << " (";
		for (size_t i = 0; i < roles.size(); ++i) text << roles[i] << (i + 1 < roles.size() ? ", " : "");
		text << ")\n";
		text << "Runs:               " << runs << "\n";
		text << "Seed:               " << seed << "\n";
		text << "Ticks:              " << ticks << "\n";
		text << "Num Lua states:     " << numLuaStates << "\n";
		text << "Spawn mode:         " << (parallel ? "parallel" : "sequential") << "\n";
		if (!injectDivRole.empty()) {
			text << "Injected perturb:   role=" << injectDivRole << " (expecting DIVERGED)\n";
		}
		text << "Engine failures:    " << totalEngineFailures << "\n";
		if (totalEngineFailures > 0) {
			text << "Result:             ENGINE FAILURE — one or more engines did not produce a trace\n";
		} else if (expectedDivergence) {
			text << "Result:             " << (anyDiverged ? "DIVERGED (as expected — harness OK)" : "MATCH (UNEXPECTED — harness BROKEN)") << "\n";
		} else {
			text << "Result:             " << (anyDiverged ? "DIVERGED" : "MATCH") << "\n";
		}
		text << "Report:             " << outPath.string() << "\n";

		PrintTextOrJson(out, summary, text.str());

		if (totalEngineFailures > 0) return 2;
		return harnessOk ? 0 : 1;
	}

	// ----- subcommand: test latency-injection ---------------------------------
	//
	// Deterministic tick-based network simulator. xorshift64* RNG seeded separately
	// from the sim seed so network conditions never leak into sim determinism.
	// Output is a packet trace M7's eventual lockstep / reconciliation logic will
	// consume; today no engine uses it.

	struct NetworkConditions {
		int packetLossPct = 0;
		double latencyMs = 0.0;
		double jitterMs = 0.0;
		int reorderPct = 0;
		double tickRateHz = 60.0;
	};

	struct PacketTraceEntry {
		uint64_t sendTick = 0;
		bool delivered = false;
		uint64_t arriveTick = 0;
		int latencyTicks = 0;
	};

	struct PacketTraceSummary {
		uint64_t total = 0;
		uint64_t delivered = 0;
		uint64_t dropped = 0;
		double meanArrivalDelayTicks = 0.0;
		int maxArrivalDelayTicks = 0;
		uint64_t reorderEvents = 0;
	};

	std::vector<PacketTraceEntry> SimulateNetworkPackets(
	    uint64_t numTicks,
	    const NetworkConditions& cond,
	    uint64_t networkSeed,
	    uint64_t& reorderEventsOut) {
		reorderEventsOut = 0;
		std::vector<PacketTraceEntry> packets;
		if (numTicks == 0) return packets;
		packets.reserve(numTicks);

		uint64_t st = networkSeed ? networkSeed : 0x9E3779B97F4A7C15ull;
		auto rng = [&st]() -> uint64_t {
			st ^= st >> 12;
			st ^= st << 25;
			st ^= st >> 27;
			return st * 0x2545F4914F6CDD1Dull;
		};
		auto rollPct = [&]() -> int { return static_cast<int>((rng() >> 32) % 100u); };

		const double msPerTick = cond.tickRateHz > 0.0 ? (1000.0 / cond.tickRateHz) : (1000.0 / 60.0);
		const int baseLatencyTicks = static_cast<int>(cond.latencyMs / msPerTick + 0.5);
		const int maxJitterTicks = static_cast<int>(cond.jitterMs / msPerTick + 0.5);

		for (uint64_t t = 0; t < numTicks; ++t) {
			PacketTraceEntry e;
			e.sendTick = t;
			e.delivered = (rollPct() >= cond.packetLossPct);
			if (e.delivered) {
				int delay = baseLatencyTicks;
				if (maxJitterTicks > 0) {
					const int span = 2 * maxJitterTicks + 1;
					const int j = static_cast<int>((rng() >> 32) % static_cast<uint64_t>(span)) - maxJitterTicks;
					delay += j;
				}
				if (delay < 0) delay = 0;
				e.latencyTicks = delay;
				e.arriveTick = t + static_cast<uint64_t>(delay);
			}
			packets.push_back(e);
		}

		// Adjacent-swap reorder. Only swap when both packets delivered.
		// Recompute latencyTicks so latencyTicks == arriveTick - sendTick (clamped >= 0).
		auto recomputeLatency = [](PacketTraceEntry& p) {
			const int64_t d = static_cast<int64_t>(p.arriveTick) - static_cast<int64_t>(p.sendTick);
			p.latencyTicks = d > 0 ? static_cast<int>(d) : 0;
		};
		for (size_t i = 0; i + 1 < packets.size(); ++i) {
			if (!packets[i].delivered || !packets[i + 1].delivered) continue;
			if (rollPct() < cond.reorderPct) {
				std::swap(packets[i].arriveTick, packets[i + 1].arriveTick);
				recomputeLatency(packets[i]);
				recomputeLatency(packets[i + 1]);
				++reorderEventsOut;
			}
		}
		return packets;
	}

	PacketTraceSummary SummarizePacketTrace(const std::vector<PacketTraceEntry>& packets, uint64_t reorderEvents) {
		PacketTraceSummary s;
		s.total = packets.size();
		s.reorderEvents = reorderEvents;
		uint64_t latencySum = 0;
		for (const auto& p: packets) {
			if (p.delivered) {
				++s.delivered;
				latencySum += static_cast<uint64_t>(p.latencyTicks);
				if (p.latencyTicks > s.maxArrivalDelayTicks) s.maxArrivalDelayTicks = p.latencyTicks;
			} else {
				++s.dropped;
			}
		}
		s.meanArrivalDelayTicks = s.delivered > 0 ? (static_cast<double>(latencySum) / static_cast<double>(s.delivered)) : 0.0;
		return s;
	}

	// Stable fingerprint of a packet trace for cross-run equality check (avoids
	// blowing up the report with the full trace when verifying determinism).
	std::string FingerprintPacketTrace(const std::vector<PacketTraceEntry>& packets) {
		uint64_t h = 0xcbf29ce484222325ull;
		auto mix = [&](uint64_t v) {
			h ^= v;
			h *= 0x100000001b3ull;
		};
		for (const auto& p: packets) {
			mix(p.sendTick);
			mix(p.delivered ? 1ull : 0ull);
			mix(p.arriveTick);
			mix(static_cast<uint64_t>(p.latencyTicks));
		}
		std::ostringstream o;
		o << std::hex << std::setw(16) << std::setfill('0') << h;
		return o.str();
	}

	int CmdTestLatencyInjection(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario = "M1Baseline";
		uint64_t seed = 42;
		uint64_t networkSeed = 1337;
		uint64_t ticks = 600;
		int runs = 1;
		int processes = 2;
		int numLuaStates = 4;
		NetworkConditions cond;
		bool parallel = false;
		bool keepTraces = false;
		fs::path outPath;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestLatencyInjectionHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "network-seed") && hasV) { networkSeed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "processes") && hasV) { processes = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "num-lua-states") && hasV) { numLuaStates = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "packet-loss") && hasV) { cond.packetLossPct = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "latency-ms") && hasV) { cond.latencyMs = std::strtod(argv[++i], nullptr); continue; }
			if (ArgEq(a, "jitter-ms") && hasV) { cond.jitterMs = std::strtod(argv[++i], nullptr); continue; }
			if (ArgEq(a, "reorder-pct") && hasV) { cond.reorderPct = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "tick-rate") && hasV) { cond.tickRateHz = std::strtod(argv[++i], nullptr); continue; }
			if (ArgEq(a, "parallel")) { parallel = true; continue; }
			if (ArgEq(a, "keep-traces")) { keepTraces = true; continue; }
			if (ArgEq(a, "output") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestLatencyInjectionHelp(std::cerr);
			return 2;
		}
		if (processes < 2) { std::cerr << "[cccp-ctl] --processes must be >= 2.\n"; return 2; }
		if (ticks == 0) { std::cerr << "[cccp-ctl] --ticks must be > 0.\n"; return 2; }
		if (runs < 1) runs = 1;
		if (runs == 1 && !out.quiet) {
			std::cerr << "[cccp-ctl] warning: --runs 1 cannot detect determinism divergence; consider --runs 2 or more.\n";
		}
		if (cond.packetLossPct < 0 || cond.packetLossPct > 100) {
			std::cerr << "[cccp-ctl] --packet-loss must be in [0, 100].\n"; return 2;
		}
		if (cond.reorderPct < 0 || cond.reorderPct > 100) {
			std::cerr << "[cccp-ctl] --reorder-pct must be in [0, 100].\n"; return 2;
		}
		if (cond.latencyMs < 0.0 || cond.jitterMs < 0.0) {
			std::cerr << "[cccp-ctl] --latency-ms / --jitter-ms must be >= 0.\n"; return 2;
		}
		if (cond.tickRateHz <= 0.0) {
			std::cerr << "[cccp-ctl] --tick-rate must be > 0.\n"; return 2;
		}
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}

		const std::vector<std::string> roles = BuildRoleLabels(processes);
		const fs::path runDir = PickOutDir(out, "cccp-ctl-latency-injection");
		if (outPath.empty()) outPath = runDir / "report.json";

		json runReports = json::array();
		bool anyDiverged = false;
		int totalEngineFailures = 0;
		std::vector<std::string> traceFingerprints;
		traceFingerprints.reserve(runs);

		for (int r = 0; r < runs; ++r) {
			const fs::path thisRunDir = runDir / ("run-" + std::to_string(r));
			std::error_code ec;
			fs::create_directories(thisRunDir, ec);

			// Network sim runs first so its determinism property is verified
			// regardless of engine behavior. Same (cond, networkSeed) => same trace.
			uint64_t reorderEvents = 0;
			const std::vector<PacketTraceEntry> packets = SimulateNetworkPackets(ticks, cond, networkSeed, reorderEvents);
			const std::string fingerprint = FingerprintPacketTrace(packets);
			traceFingerprints.push_back(fingerprint);
			const PacketTraceSummary pktSummary = SummarizePacketTrace(packets, reorderEvents);

			const fs::path pktTracePath = thisRunDir / "packet-trace.json";
			{
				json pktJson = {
				    {"network_seed", networkSeed},
				    {"packet_loss_pct", cond.packetLossPct},
				    {"latency_ms", cond.latencyMs},
				    {"jitter_ms", cond.jitterMs},
				    {"reorder_pct", cond.reorderPct},
				    {"tick_rate_hz", cond.tickRateHz},
				    {"total_packets", pktSummary.total},
				    {"delivered", pktSummary.delivered},
				    {"dropped", pktSummary.dropped},
				    {"mean_arrival_delay_ticks", pktSummary.meanArrivalDelayTicks},
				    {"max_arrival_delay_ticks", pktSummary.maxArrivalDelayTicks},
				    {"reorder_events", pktSummary.reorderEvents},
				    {"fingerprint", fingerprint},
				    {"packets", json::array()},
				};
				for (const auto& p: packets) {
					pktJson["packets"].push_back({
					    {"send_tick", p.sendTick},
					    {"delivered", p.delivered},
					    {"arrive_tick", p.arriveTick},
					    {"latency_ticks", p.latencyTicks},
					});
				}
				std::ofstream pf(pktTracePath);
				if (pf.is_open()) pf << pktJson.dump(2) << "\n";
			}

			int engineFailures = 0;
			const std::vector<EngineRunResult> results = RunEngines(
			    gameBin, scenario, seed, ticks, numLuaStates,
			    roles, {}, thisRunDir, out.quiet, parallel, engineFailures);
			totalEngineFailures += engineFailures;

			bool diverged = false;
			uint64_t firstDiv = 0;
			uint64_t comparedTicks = 0;
			json perSubsystem = json::object();
			std::map<std::string, std::string> perRolePairResult;
			if (engineFailures == 0) {
				DiffEngineTraces(results, diverged, firstDiv, comparedTicks, perSubsystem, perRolePairResult);
			}
			if (diverged) anyDiverged = true;

			json runReport = {
			    {"run_index", r},
			    {"packet_trace_path", pktTracePath.string()},
			    {"packet_trace_summary", {
			        {"total_packets", pktSummary.total},
			        {"delivered", pktSummary.delivered},
			        {"dropped", pktSummary.dropped},
			        {"mean_arrival_delay_ticks", pktSummary.meanArrivalDelayTicks},
			        {"max_arrival_delay_ticks", pktSummary.maxArrivalDelayTicks},
			        {"reorder_events", pktSummary.reorderEvents},
			        {"fingerprint", fingerprint},
			    }},
			    {"engine_results", json::array()},
			    {"diverged", diverged},
			    {"first_divergence_tick", firstDiv},
			    {"compared_ticks", comparedTicks},
			    {"per_subsystem_first_divergence", perSubsystem},
			    {"per_role_pair_result", perRolePairResult},
			    {"engine_failures", engineFailures},
			};
			for (const auto& er: results) {
				runReport["engine_results"].push_back({
				    {"role", er.role},
				    {"rc", er.rc},
				    {"passed", er.passed},
				    {"final_hash", er.finalHash},
				    {"out_path", er.outPath.string()},
				    {"trace_present", er.tracesProduced},
				});
			}
			runReports.push_back(runReport);

			if (!keepTraces && engineFailures == 0) {
				// Drop engine traces; keep packet-trace.json so the run artifact stays.
				for (const auto& er: results) {
					std::error_code ec2;
					fs::remove(er.outPath, ec2);
				}
			}
		}

		// Cross-run determinism check: all fingerprints must match.
		bool fingerprintConsistent = true;
		if (traceFingerprints.size() >= 2) {
			for (size_t i = 1; i < traceFingerprints.size(); ++i) {
				if (traceFingerprints[i] != traceFingerprints[0]) {
					fingerprintConsistent = false;
					break;
				}
			}
		}

		const bool harnessOk = (totalEngineFailures == 0) && !anyDiverged && fingerprintConsistent;

		json summary = {
		    {"scenario", scenario},
		    {"ticks", ticks},
		    {"seed", seed},
		    {"network_seed", networkSeed},
		    {"processes", processes},
		    {"roles", roles},
		    {"runs", runs},
		    {"num_lua_states", numLuaStates},
		    {"parallel", parallel},
		    {"network_conditions", {
		        {"packet_loss_pct", cond.packetLossPct},
		        {"latency_ms", cond.latencyMs},
		        {"jitter_ms", cond.jitterMs},
		        {"reorder_pct", cond.reorderPct},
		        {"tick_rate_hz", cond.tickRateHz},
		    }},
		    {"diverged", anyDiverged},
		    {"engine_failures_total", totalEngineFailures},
		    {"packet_trace_fingerprints", traceFingerprints},
		    {"packet_trace_consistent_across_runs", fingerprintConsistent},
		    {"harness_ok", harnessOk},
		    {"runs_detail", runReports},
		    {"report_path", outPath.string()},
		    {"out_dir", runDir.string()},
		};

		std::ofstream o(outPath);
		if (!o.is_open()) {
			std::cerr << "[cccp-ctl] failed to open report file: " << outPath.string() << "\n";
			return 2;
		}
		o << summary.dump(2) << "\n";

		std::ostringstream text;
		text << "Scenario:                " << scenario << "\n";
		text << "Processes:               " << processes << "\n";
		text << "Runs:                    " << runs << "\n";
		text << "Sim seed:                " << seed << "\n";
		text << "Network seed:            " << networkSeed << "\n";
		text << "Conditions:              packet-loss=" << cond.packetLossPct
		     << "% latency=" << cond.latencyMs << "ms jitter=" << cond.jitterMs
		     << "ms reorder=" << cond.reorderPct << "% tick-rate=" << cond.tickRateHz << "Hz\n";
		text << "Sim result:              ";
		if (totalEngineFailures > 0) text << "ENGINE FAILURE\n";
		else text << (anyDiverged ? "DIVERGED" : "MATCH") << "\n";
		text << "Packet trace:            " << (fingerprintConsistent ? "deterministic across runs" : "INCONSISTENT — simulator broken") << "\n";
		text << "Packet fingerprint:      " << (traceFingerprints.empty() ? std::string("(no runs)") : traceFingerprints[0]) << "\n";
		text << "Harness:                 " << (harnessOk ? "OK" : "FAILED") << "\n";
		text << "Report:                  " << outPath.string() << "\n";

		PrintTextOrJson(out, summary, text.str());

		if (totalEngineFailures > 0) return 2;
		return harnessOk ? 0 : 1;
	}

	// ----- subcommand: test snapshot-restore ----------------------------------
	//
	// M5.5: phase 1 (baseline) runs today via existing -scenario -tick-hashes.
	// Phases 2-3 (snapshot/restore round-trip) gracefully report 'pending M8'
	// until the engine's snapshot API lands. The subcommand structure stays
	// stable so M8 can fill in phases without changing the CLI surface.

	int CmdTestSnapshotRestore(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario = "M1Baseline";
		uint64_t seed = 42;
		uint64_t ticks = 300;
		uint64_t snapshotAt = 100;
		int64_t resumeAtArg = -1;
		int runs = 1;
		int numLuaStates = 4;
		bool keepTraces = false;
		bool strict = false;
		fs::path outPath;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestSnapshotRestoreHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "snapshot-at") && hasV) { snapshotAt = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "resume-at") && hasV) { resumeAtArg = std::strtoll(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "num-lua-states") && hasV) { numLuaStates = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "keep-traces")) { keepTraces = true; continue; }
			if (ArgEq(a, "strict")) { strict = true; continue; }
			if (ArgEq(a, "output") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestSnapshotRestoreHelp(std::cerr);
			return 2;
		}
		if (ticks == 0) { std::cerr << "[cccp-ctl] --ticks must be > 0.\n"; return 2; }
		if (runs < 1) runs = 1;
		if (runs == 1 && !out.quiet) {
			std::cerr << "[cccp-ctl] warning: --runs 1 cannot detect determinism divergence; consider --runs 2 or more.\n";
		}
		if (snapshotAt >= ticks) {
			std::cerr << "[cccp-ctl] --snapshot-at must be < --ticks (snapshot " << snapshotAt
			          << " >= ticks " << ticks << ").\n";
			return 2;
		}
		const uint64_t resumeAt = (resumeAtArg < 0) ? snapshotAt : static_cast<uint64_t>(resumeAtArg);
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}

		const fs::path runDir = PickOutDir(out, "cccp-ctl-snapshot-restore");
		if (outPath.empty()) outPath = runDir / "report.json";

		// Phase 1 only — phases 2/3 are scaffolded. We invoke the engine via the
		// same one-role harness as mp-sync-drift to reuse trace loading.
		const std::vector<std::string> roles = {"baseline"};

		json runReports = json::array();
		int totalEngineFailures = 0;
		bool baselineOk = true;
		std::string baselineFinalHash;
		uint64_t baselineTickCount = 0;

		for (int r = 0; r < runs; ++r) {
			const fs::path thisRunDir = runDir / ("run-" + std::to_string(r));
			std::error_code ec;
			fs::create_directories(thisRunDir, ec);

			int engineFailures = 0;
			const std::vector<EngineRunResult> results = RunEngines(
			    gameBin, scenario, seed, ticks, numLuaStates,
			    roles, {}, thisRunDir, out.quiet, false, engineFailures);
			totalEngineFailures += engineFailures;

			bool runBaselineOk = (engineFailures == 0) && !results.empty() && results[0].tracesProduced;
			if (runBaselineOk) {
				baselineFinalHash = results[0].finalHash;
				baselineTickCount = results[0].tickHashes.is_array() ? results[0].tickHashes.size() : 0;
			} else {
				baselineOk = false;
			}

			json runReport = {
			    {"run_index", r},
			    {"phase1_baseline", {
			        {"status", runBaselineOk ? "OK" : "FAILED"},
			        {"final_hash", runBaselineOk ? results[0].finalHash : std::string()},
			        {"ticks_recorded", runBaselineOk ? baselineTickCount : 0},
			        {"trace_path", runBaselineOk ? results[0].outPath.string() : std::string()},
			    }},
			    {"phase2_snapshot_at_tick", {
			        {"status", "PENDING_M8"},
			        {"requested_tick", snapshotAt},
			        {"note", "engine snapshot API not yet implemented"},
			    }},
			    {"phase3_mutate_restore_at_tick", {
			        {"status", "PENDING_M8"},
			        {"requested_tick", resumeAt},
			        {"note", "engine restore API not yet implemented"},
			    }},
			    {"engine_failures", engineFailures},
			};
			runReports.push_back(runReport);

			if (!keepTraces && runBaselineOk) {
				std::error_code ec2;
				fs::remove(results[0].outPath, ec2);
			}
		}

		const bool snapshotPending = true;
		const bool harnessOk = baselineOk && (!strict || !snapshotPending);

		json summary = {
		    {"scenario", scenario},
		    {"ticks", ticks},
		    {"seed", seed},
		    {"snapshot_at_tick", snapshotAt},
		    {"resume_at_tick", resumeAt},
		    {"runs", runs},
		    {"num_lua_states", numLuaStates},
		    {"strict", strict},
		    {"phase1_baseline_ok", baselineOk},
		    {"phase2_snapshot_status", "PENDING_M8"},
		    {"phase3_restore_status", "PENDING_M8"},
		    {"pending_engine_api", "engine MO snapshot/restore API per M8 plan"},
		    {"m8_contract", "traceA (baseline) == traceB (snapshot-then-continue) == traceC (snapshot+mutate+restore+continue)"},
		    {"engine_failures_total", totalEngineFailures},
		    {"harness_ok", harnessOk},
		    {"runs_detail", runReports},
		    {"report_path", outPath.string()},
		    {"out_dir", runDir.string()},
		};

		std::ofstream o(outPath);
		if (!o.is_open()) {
			std::cerr << "[cccp-ctl] failed to open report file: " << outPath.string() << "\n";
			return 2;
		}
		o << summary.dump(2) << "\n";

		std::ostringstream text;
		text << "Scenario:           " << scenario << "\n";
		text << "Ticks:              " << ticks << "\n";
		text << "Snapshot at tick:   " << snapshotAt << "\n";
		text << "Resume at tick:     " << resumeAt << "\n";
		text << "Runs:               " << runs << "\n";
		text << "Phase 1 (baseline): " << (baselineOk ? "OK" : "FAILED") << "\n";
		text << "                    " << baselineTickCount << " ticks, final hash " << baselineFinalHash << "\n";
		text << "Phase 2 (snapshot): PENDING_M8 — engine snapshot API not yet implemented\n";
		text << "Phase 3 (restore):  PENDING_M8 — engine restore API not yet implemented\n";
		text << "Strict mode:        " << (strict ? "ON (exits 1 while M8 pending)" : "off (CI-friendly default)") << "\n";
		text << "Harness:            " << (harnessOk ? "OK (tester scaffolding ready for M8)" : "FAILED") << "\n";
		text << "Report:             " << outPath.string() << "\n";

		PrintTextOrJson(out, summary, text.str());

		if (totalEngineFailures > 0) return 2;
		return harnessOk ? 0 : 1;
	}

	// ----- subcommand: test rollback-burst ------------------------------------
	//
	// M5.5 builds the tester ahead of M8 (rollback engine). Phase 1 = baseline
	// trace (runs today). Phase 2 = deterministic burst plan JSON M8 will replay.
	// Phase 3 = apply plan + rollback + verify, gated on engine rollback API.

	struct BurstEvent {
		uint64_t mispredictTick = 0;
		uint64_t correctionTick = 0;
		uint64_t burstDepth = 0;
	};

	std::vector<BurstEvent> GenerateBurstPlan(
	    uint64_t totalTicks,
	    int burstSize,
	    int burstDepth,
	    uint64_t burstSeed) {
		std::vector<BurstEvent> out;
		if (burstSize <= 0 || burstDepth <= 0 || totalTicks <= static_cast<uint64_t>(burstDepth)) return out;
		out.reserve(burstSize);

		uint64_t st = burstSeed ? burstSeed : 0xD1B54A32D192ED03ull;
		auto rng = [&st]() -> uint64_t {
			st ^= st >> 12;
			st ^= st << 25;
			st ^= st >> 27;
			return st * 0x2545F4914F6CDD1Dull;
		};

		const uint64_t window = totalTicks - static_cast<uint64_t>(burstDepth);
		for (int i = 0; i < burstSize; ++i) {
			BurstEvent e;
			e.mispredictTick = window > 0 ? ((rng() >> 32) % window) : 0;
			e.burstDepth = static_cast<uint64_t>(burstDepth);
			e.correctionTick = e.mispredictTick + e.burstDepth;
			out.push_back(e);
		}
		std::sort(out.begin(), out.end(), [](const BurstEvent& a, const BurstEvent& b) {
			return a.mispredictTick < b.mispredictTick;
		});
		return out;
	}

	std::string FingerprintBurstPlan(const std::vector<BurstEvent>& events) {
		uint64_t h = 0xcbf29ce484222325ull;
		auto mix = [&](uint64_t v) {
			h ^= v;
			h *= 0x100000001b3ull;
		};
		for (const auto& e: events) {
			mix(e.mispredictTick);
			mix(e.correctionTick);
			mix(e.burstDepth);
		}
		std::ostringstream o;
		o << std::hex << std::setw(16) << std::setfill('0') << h;
		return o.str();
	}

	int CmdTestRollbackBurst(int argc, char** argv, const fs::path& selfDir) {
		std::string scenario = "M1Baseline";
		uint64_t seed = 42;
		uint64_t burstSeed = 4242;
		uint64_t ticks = 300;
		int burstSize = 5;
		int burstDepth = 6;
		int runs = 1;
		int numLuaStates = 4;
		bool keepTraces = false;
		bool strict = false;
		fs::path outPath;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestRollbackBurstHelp(std::cout); return 0; }
			if (ArgEq(a, "scenario") && hasV) { scenario = argv[++i]; continue; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "burst-seed") && hasV) { burstSeed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "burst-size") && hasV) { burstSize = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "burst-depth") && hasV) { burstDepth = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "runs") && hasV) { runs = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "num-lua-states") && hasV) { numLuaStates = std::atoi(argv[++i]); continue; }
			if (ArgEq(a, "keep-traces")) { keepTraces = true; continue; }
			if (ArgEq(a, "strict")) { strict = true; continue; }
			if (ArgEq(a, "output") && hasV) { outPath = argv[++i]; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestRollbackBurstHelp(std::cerr);
			return 2;
		}
		if (ticks == 0) { std::cerr << "[cccp-ctl] --ticks must be > 0.\n"; return 2; }
		if (runs < 1) runs = 1;
		if (runs == 1 && !out.quiet) {
			std::cerr << "[cccp-ctl] warning: --runs 1 cannot detect determinism divergence; consider --runs 2 or more.\n";
		}
		if (burstSize < 0) burstSize = 0;
		if (burstDepth < 1) {
			std::cerr << "[cccp-ctl] --burst-depth must be >= 1.\n"; return 2;
		}
		if (static_cast<uint64_t>(burstDepth) >= ticks) {
			std::cerr << "[cccp-ctl] --burst-depth must be < --ticks.\n"; return 2;
		}
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}

		const fs::path runDir = PickOutDir(out, "cccp-ctl-rollback-burst");
		if (outPath.empty()) outPath = runDir / "report.json";
		const std::vector<std::string> roles = {"baseline"};

		json runReports = json::array();
		int totalEngineFailures = 0;
		bool baselineOk = true;
		std::vector<std::string> planFingerprints;
		planFingerprints.reserve(runs);

		for (int r = 0; r < runs; ++r) {
			const fs::path thisRunDir = runDir / ("run-" + std::to_string(r));
			std::error_code ec;
			fs::create_directories(thisRunDir, ec);

			// Phase 2: generate the burst plan up-front (deterministic from burstSeed).
			const std::vector<BurstEvent> plan = GenerateBurstPlan(ticks, burstSize, burstDepth, burstSeed);
			const std::string fingerprint = FingerprintBurstPlan(plan);
			planFingerprints.push_back(fingerprint);

			const fs::path planPath = thisRunDir / "burst-plan.json";
			{
				json planJson = {
				    {"burst_seed", burstSeed},
				    {"burst_size", burstSize},
				    {"burst_depth", burstDepth},
				    {"total_ticks", ticks},
				    {"fingerprint", fingerprint},
				    {"events", json::array()},
				};
				for (const auto& e: plan) {
					planJson["events"].push_back({
					    {"mispredict_tick", e.mispredictTick},
					    {"correction_tick", e.correctionTick},
					    {"burst_depth", e.burstDepth},
					});
				}
				std::ofstream pf(planPath);
				if (pf.is_open()) pf << planJson.dump(2) << "\n";
			}

			// Phase 1: baseline trace (no rollback simulation).
			int engineFailures = 0;
			const auto t0 = std::chrono::steady_clock::now();
			const std::vector<EngineRunResult> results = RunEngines(
			    gameBin, scenario, seed, ticks, numLuaStates,
			    roles, {}, thisRunDir, out.quiet, false, engineFailures);
			const auto t1 = std::chrono::steady_clock::now();
			const double baselineWallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
			totalEngineFailures += engineFailures;

			const bool runBaselineOk = (engineFailures == 0) && !results.empty() && results[0].tracesProduced;
			if (!runBaselineOk) baselineOk = false;

			json runReport = {
			    {"run_index", r},
			    {"phase1_baseline", {
			        {"status", runBaselineOk ? "OK" : "FAILED"},
			        {"final_hash", runBaselineOk ? results[0].finalHash : std::string()},
			        {"wall_ms", baselineWallMs},
			        {"trace_path", runBaselineOk ? results[0].outPath.string() : std::string()},
			    }},
			    {"phase2_burst_plan", {
			        {"status", "OK"},
			        {"plan_path", planPath.string()},
			        {"event_count", plan.size()},
			        {"fingerprint", fingerprint},
			    }},
			    {"phase3_apply_rollback", {
			        {"status", "PENDING_M8"},
			        {"note", "engine rollback API not yet implemented — plan saved for M8 replay"},
			    }},
			    {"engine_failures", engineFailures},
			};
			runReports.push_back(runReport);

			if (!keepTraces && runBaselineOk) {
				std::error_code ec2;
				fs::remove(results[0].outPath, ec2);
			}
		}

		// Plan determinism: every run's plan fingerprint must match.
		bool planConsistent = true;
		if (planFingerprints.size() >= 2) {
			for (size_t i = 1; i < planFingerprints.size(); ++i) {
				if (planFingerprints[i] != planFingerprints[0]) { planConsistent = false; break; }
			}
		}

		const bool rollbackPending = true;
		const bool harnessOk = baselineOk && planConsistent && (!strict || !rollbackPending);

		json summary = {
		    {"scenario", scenario},
		    {"ticks", ticks},
		    {"seed", seed},
		    {"burst_seed", burstSeed},
		    {"burst_size", burstSize},
		    {"burst_depth", burstDepth},
		    {"runs", runs},
		    {"num_lua_states", numLuaStates},
		    {"strict", strict},
		    {"phase1_baseline_ok", baselineOk},
		    {"phase2_plan_consistent_across_runs", planConsistent},
		    {"phase2_plan_fingerprint", planFingerprints.empty() ? std::string() : planFingerprints[0]},
		    {"phase3_apply_status", "PENDING_M8"},
		    {"engine_failures_total", totalEngineFailures},
		    {"harness_ok", harnessOk},
		    {"runs_detail", runReports},
		    {"report_path", outPath.string()},
		    {"out_dir", runDir.string()},
		};

		std::ofstream o(outPath);
		if (!o.is_open()) {
			std::cerr << "[cccp-ctl] failed to open report file: " << outPath.string() << "\n";
			return 2;
		}
		o << summary.dump(2) << "\n";

		std::ostringstream text;
		text << "Scenario:           " << scenario << "\n";
		text << "Ticks:              " << ticks << "\n";
		text << "Burst size:         " << burstSize << " (depth " << burstDepth << ")\n";
		text << "Runs:               " << runs << "\n";
		text << "Phase 1 (baseline): " << (baselineOk ? "OK" : "FAILED") << "\n";
		text << "Phase 2 (plan):     " << (planConsistent ? "deterministic across runs" : "INCONSISTENT — plan RNG broken") << "\n";
		text << "Plan fingerprint:   " << (planFingerprints.empty() ? std::string("(no runs)") : planFingerprints[0]) << "\n";
		text << "Phase 3 (apply):    PENDING_M8 — engine rollback API not yet implemented\n";
		text << "Strict mode:        " << (strict ? "ON (exits 1 while M8 pending)" : "off (CI-friendly default)") << "\n";
		text << "Harness:            " << (harnessOk ? "OK (tester scaffolding ready for M8)" : "FAILED") << "\n";
		text << "Report:             " << outPath.string() << "\n";

		PrintTextOrJson(out, summary, text.str());

		if (totalEngineFailures > 0) return 2;
		return harnessOk ? 0 : 1;
	}

	// ----- subcommand: test mp-suite ------------------------------------------
	//
	// Aggregates the four MP-correctness subcommands as one CI-friendly run.
	// Reuses self-invocation via g_selfPath (same trick as `test all`) so each
	// sub-step gets full arg validation + its own JSON report.

	int CmdTestMpSuite(int argc, char** argv, const fs::path& selfDir) {
		uint64_t seed = 42;
		uint64_t ticks = 200;
		bool quick = false;
		bool parallel = false;
		fs::path gameBin;
		OutputCtx out;

		for (int i = 0; i < argc; ++i) {
			const std::string a = argv[i];
			const bool hasV = (i + 1) < argc;
			if (a == "-h" || a == "--help") { PrintTestMpSuiteHelp(std::cout); return 0; }
			if (ArgEq(a, "seed") && hasV) { seed = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "ticks") && hasV) { ticks = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (ArgEq(a, "quick")) { quick = true; continue; }
			if (ArgEq(a, "parallel")) { parallel = true; continue; }
			if (ArgEq(a, "game-bin") && hasV) { gameBin = argv[++i]; continue; }
			if (ArgEq(a, "out-dir") && hasV) { out.outDir = argv[++i]; continue; }
			if (ArgEq(a, "json")) { out.json = true; continue; }
			if (ArgEq(a, "quiet")) { out.quiet = true; continue; }
			std::cerr << "[cccp-ctl] unknown option: " << a << "\n";
			PrintTestMpSuiteHelp(std::cerr);
			return 2;
		}
		if (quick) ticks = 60;
		if (gameBin.empty()) gameBin = AutoDetectGameBin(selfDir);
		if (gameBin.empty() || !fs::exists(gameBin)) {
			std::cerr << "[cccp-ctl] could not locate game binary; pass --game-bin <path>.\n";
			return 2;
		}

		const fs::path runDir = PickOutDir(out, "cccp-ctl-mp-suite");

		auto commonTail = [&](std::ostringstream& c) {
			c << " --game-bin " << Quote(gameBin.string());
			if (out.quiet) c << " --quiet";
		};
		auto loadReport = [&](const fs::path& p) -> json {
			if (!fs::exists(p)) return json();
			return ReadJsonFile(p);
		};

		struct Step {
			std::string name;
			std::string cmd;
			int rc = 0;
			bool harnessOk = false;
			std::string detail;
		};
		std::vector<Step> steps;

		auto runStep = [&](const std::string& name, std::function<std::string(const fs::path&)> buildCmd,
		                   std::function<bool(const json&)> harnessOkFromReport,
		                   std::function<std::string(const json&, int)> detailFromReport) {
			Step s;
			s.name = name;
			const fs::path report = runDir / (name + "-report.json");
			std::ostringstream c;
			c << buildCmd(report);
			commonTail(c);
			s.cmd = c.str();
			if (!out.quiet) std::cerr << "\n[cccp-ctl] === " << name << " ===\n";
			s.rc = RunSubprocess(s.cmd, !out.quiet);
			const json j = loadReport(report);
			s.harnessOk = harnessOkFromReport(j);
			s.detail = detailFromReport(j, s.rc);
			steps.push_back(s);
			if (!out.quiet) std::cerr << "  -> " << (s.harnessOk ? "PASS" : "FAIL") << " (" << s.detail << ")\n";
		};

		const std::string parallelFlag = parallel ? " --parallel" : "";

		// 1. mp-sync-drift clean (M4ThreadStress for thread-density signal).
		runStep("mp-sync-drift-clean",
		    [&](const fs::path& report) {
			    std::ostringstream c;
			    c << Quote(g_selfPath) << " test mp-sync-drift"
			      << " --scenario M4ThreadStress --processes 2 --runs 1"
			      << " --seed " << seed << " --ticks " << ticks
			      << " --output " << Quote(report.string()) << parallelFlag;
			    return c.str();
		    },
		    [](const json& j) { return !j.is_null() && j.value("harness_ok", false); },
		    [](const json& j, int rc) {
			    if (j.is_null()) return std::string("no report (rc=") + std::to_string(rc) + ")";
			    return std::string(j.value("diverged", false) ? "DIVERGED" : "MATCH");
		    });

		// 2. mp-sync-drift EC3-MP positive control (perturb peer1 — expects DIVERGED).
		runStep("mp-sync-drift-ec3",
		    [&](const fs::path& report) {
			    std::ostringstream c;
			    c << Quote(g_selfPath) << " test mp-sync-drift"
			      << " --scenario M1Baseline --processes 2 --runs 1"
			      << " --inject-divergence-role peer1"
			      << " --seed " << seed << " --ticks " << ticks
			      << " --output " << Quote(report.string()) << parallelFlag;
			    return c.str();
		    },
		    [](const json& j) { return !j.is_null() && j.value("harness_ok", false); },
		    [](const json& j, int rc) {
			    if (j.is_null()) return std::string("no report (rc=") + std::to_string(rc) + ")";
			    return std::string(j.value("diverged", false) ? "DIVERGED (expected)" : "MATCH (UNEXPECTED — comparator broken)");
		    });

		// 3. latency-injection perfect conditions (verifies pipeline + simulator determinism).
		runStep("latency-injection",
		    [&](const fs::path& report) {
			    std::ostringstream c;
			    c << Quote(g_selfPath) << " test latency-injection"
			      << " --scenario M1Baseline --processes 2 --runs 2"
			      << " --packet-loss 0 --latency-ms 0 --jitter-ms 0 --reorder-pct 0"
			      << " --seed " << seed << " --ticks " << ticks
			      << " --output " << Quote(report.string()) << parallelFlag;
			    return c.str();
		    },
		    [](const json& j) { return !j.is_null() && j.value("harness_ok", false); },
		    [](const json& j, int rc) {
			    if (j.is_null()) return std::string("no report (rc=") + std::to_string(rc) + ")";
			    const bool consistent = j.value("packet_trace_consistent_across_runs", false);
			    return std::string(consistent ? "sim MATCH + simulator deterministic" : "simulator NON-DETERMINISTIC");
		    });

		// 4. snapshot-restore baseline (phases 2-3 PENDING_M8).
		runStep("snapshot-restore",
		    [&](const fs::path& report) {
			    std::ostringstream c;
			    c << Quote(g_selfPath) << " test snapshot-restore"
			      << " --scenario M1Baseline --runs 1"
			      << " --snapshot-at " << (ticks / 3) << " --ticks " << ticks
			      << " --seed " << seed
			      << " --output " << Quote(report.string());
			    return c.str();
		    },
		    [](const json& j) { return !j.is_null() && j.value("harness_ok", false); },
		    [](const json& j, int rc) {
			    if (j.is_null()) return std::string("no report (rc=") + std::to_string(rc) + ")";
			    return std::string(j.value("phase1_baseline_ok", false) ? "phase 1 OK; phases 2-3 PENDING_M8" : "phase 1 FAILED");
		    });

		// 5. rollback-burst baseline + plan (phase 3 PENDING_M8).
		runStep("rollback-burst",
		    [&](const fs::path& report) {
			    std::ostringstream c;
			    c << Quote(g_selfPath) << " test rollback-burst"
			      << " --scenario M1Baseline --runs 2"
			      << " --burst-size 3 --burst-depth 5 --ticks " << ticks
			      << " --seed " << seed
			      << " --output " << Quote(report.string());
			    return c.str();
		    },
		    [](const json& j) { return !j.is_null() && j.value("harness_ok", false); },
		    [](const json& j, int rc) {
			    if (j.is_null()) return std::string("no report (rc=") + std::to_string(rc) + ")";
			    const bool consistent = j.value("phase2_plan_consistent_across_runs", false);
			    return std::string(consistent ? "phases 1-2 OK; phase 3 PENDING_M8" : "burst plan NON-DETERMINISTIC");
		    });

		int passed = 0, failed = 0;
		for (const auto& s: steps) (s.harnessOk ? passed : failed)++;
		const bool allOk = (failed == 0);

		json summary = {
		    {"mode", quick ? "quick" : "full"},
		    {"seed", seed},
		    {"ticks", ticks},
		    {"parallel", parallel},
		    {"total_steps", static_cast<int>(steps.size())},
		    {"passed", passed},
		    {"failed", failed},
		    {"all_passed", allOk},
		    {"out_dir", runDir.string()},
		    {"steps", json::array()},
		};
		for (const auto& s: steps) {
			summary["steps"].push_back({
			    {"name", s.name},
			    {"harness_ok", s.harnessOk},
			    {"rc", s.rc},
			    {"detail", s.detail},
			});
		}

		std::ostringstream text;
		text << "\n";
		text << "================================================================\n";
		text << "  cccp-ctl test mp-suite — MP SUITE SUMMARY (" << (quick ? "quick" : "full") << " mode)\n";
		text << "================================================================\n";
		for (const auto& s: steps) {
			text << "  " << (s.harnessOk ? "PASS  " : "FAIL  ") << std::left << std::setw(28) << s.name << "  " << s.detail << "\n";
		}
		text << "----------------------------------------------------------------\n";
		text << "  Total: " << steps.size() << " steps, " << passed << " passed, " << failed << " failed\n";
		text << "  Out dir: " << runDir.string() << "\n";
		text << "  Result: " << (allOk ? "ALL PASSED" : "FAILURES — see details above") << "\n";

		PrintTextOrJson(out, summary, text.str());
		return allOk ? 0 : 1;
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
		if (sub == "mp-sync-drift" || sub == "sync-drift") return CmdTestMpSyncDrift(subArgc, subArgv, selfDir);
		if (sub == "mp-suite") return CmdTestMpSuite(subArgc, subArgv, selfDir);
		if (sub == "latency-injection") return CmdTestLatencyInjection(subArgc, subArgv, selfDir);
		if (sub == "snapshot-restore") return CmdTestSnapshotRestore(subArgc, subArgv, selfDir);
		if (sub == "rollback-burst") return CmdTestRollbackBurst(subArgc, subArgv, selfDir);
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
