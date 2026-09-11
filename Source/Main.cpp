/*          ______   ______   ______  ______  ______  __  __       ______   ______   __    __   __    __   ______   __   __   _____
           /\  ___\ /\  __ \ /\  == \/\__  _\/\  ___\/\_\_\_\     /\  ___\ /\  __ \ /\ "-./  \ /\ "-./  \ /\  __ \ /\ "-.\ \ /\  __-.
           \ \ \____\ \ \/\ \\ \  __<\/_/\ \/\ \  __\\/_/\_\/_    \ \ \____\ \ \/\ \\ \ \-./\ \\ \ \-./\ \\ \  __ \\ \ \-.  \\ \ \/\ \
            \ \_____\\ \_____\\ \_\ \_\ \ \_\ \ \_____\/\_\/\_\    \ \_____\\ \_____\\ \_\ \ \_\\ \_\ \ \_\\ \_\ \_\\ \_\\"\_\\ \____-
             \/_____/ \/_____/ \/_/ /_/  \/_/  \/_____/\/_/\/_/     \/_____/ \/_____/ \/_/  \/_/ \/_/  \/_/ \/_/\/_/ \/_/ \/_/ \/____/
   ______   ______   __    __   __    __   __  __   __   __   __   ______  __  __       ______  ______   ______      __   ______   ______   ______
  /\  ___\ /\  __ \ /\ "-./  \ /\ "-./  \ /\ \/\ \ /\ "-.\ \ /\ \ /\__  _\/\ \_\ \     /\  == \/\  == \ /\  __ \    /\ \ /\  ___\ /\  ___\ /\__  _\
  \ \ \____\ \ \/\ \\ \ \-./\ \\ \ \-./\ \\ \ \_\ \\ \ \-.  \\ \ \\/_/\ \/\ \____ \    \ \  _-/\ \  __< \ \ \/\ \  _\_\ \\ \  __\ \ \ \____\/_/\ \/
   \ \_____\\ \_____\\ \_\ \ \_\\ \_\ \ \_\\ \_____\\ \_\\"\_\\ \_\  \ \_\ \/\_____\    \ \_\   \ \_\ \_\\ \_____\/\_____\\ \_____\\ \_____\  \ \_\
    \/_____/ \/_____/ \/_/  \/_/ \/_/  \/_/ \/_____/ \/_/ \/_/ \/_/   \/_/  \/_____/     \/_/    \/_/ /_/ \/_____/\/_____/ \/_____/ \/_____/   \/_/

/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\*/

/// <summary>
/// Main driver implementation of the Retro Terrain Engine.
/// Data Realms, LLC - http://www.datarealms.com
/// Cortex Command Community Project - https://github.com/cortex-command-community
/// Cortex Command Community Project Discord - https://discord.gg/TSU6StNQUG
/// </summary>

#include "allegro.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include "GUI.h"
#include "GUIInputWrapper.h"
#include "MainMenuGUI.h"
#include "NetModerationGUI.h"
#include "NetModerationGUIProbe.h"
#include "AllegroScreen.h"
#include "AllegroBitmap.h"

#include "MainMenuGUI.h"
#include "ScenarioGUI.h"
#include "PauseMenuGUI.h"
#include "TitleScreen.h"
#include "LoadingScreen.h"

#include "MenuMan.h"
#include "SaveLoadMenuGUI.h"
#include "ConsoleMan.h"
#include "Constants.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "PresetMan.h"
#include "UInputMan.h"
#include "PerformanceMan.h"
#include "FrameMan.h"
#include "PostProcessMan.h"
#include "SceneMan.h"
#include "SLTerrain.h"
#include "MetaMan.h"
#include "WindowMan.h"
#include "GLResourceMan.h"
#include "CameraMan.h"
#include "ActivityMan.h"
#include "GameActivity.h"
#include "MovableObject.h"
#include "RTETools.h"
#include "PrimitiveMan.h"
#include "ThreadMan.h"
#include "LuaMan.h"
#include "MusicMan.h"
#include "AudioMan.h"
#include "SoundSimulation.h"
#include "AudioCheckpoint.h"
#include "System.h"

#include "ControllerFrame.h"
#include "GnsTransport.h"
#include "NetAdmissionSelfTest.h"
#include "NetAuthSelfTest.h"
#include "NetIdentity.h"
#include "NetIdentitySelfTest.h"
#include "NetLanDiscovery.h"
#include "NetLockstep.h"
#include "NetMatchReplay.h"
#include "NetLockstepSelfTest.h"
#include "NetMatchRunner.h"
#include "NetMatchService.h"
#include "NetMatchSelfTest.h"
#include "NetProtocolSelfTest.h"
#include "NetReconnectSelfTest.h"
#include "NetReconnectSessionSelfTest.h"
#include "NetSession.h"
#include "NetSessionSelfTest.h"
#include "SimChecksum.h"
#include "NetA7Journal.h"
#include "ScenarioRunner.h"
#include "InputScript.h"
#include "AIWriteScript.h"
#include "FaultInjection.h"
#include "LocalPrediction.h"
#include "TerrainLayerSnapshot.h"
#include "DeterminismCheck.h"
#include "MetricsCollector.h"
#include "ContractAudit.h"

#include "RenderTarget.h"
#include "tracy/Tracy.hpp"

#include "imgui_impl_sdl3.h"

#ifdef _WIN32
#include "windows.h"
#include <crtdbg.h>
#endif

#include <algorithm>
#include <bit>
#include <cfloat>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <deque>
#include <array>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

extern "C" {
FILE __iob_func[3] = {*stdin, *stdout, *stderr};
}

using namespace RTE;

// Per-tick state hashing — armed by the -tick-hashes CLI flag, off in normal play.
static bool s_recordTickHashes = false;
static std::string s_menuMpTraceError;
static bool s_bitmapSaveSelfTest = false;
static int s_bitmapSaveSelfTestResult = -1;
static bool s_cameraNullSceneSelfTest = false;
static bool s_saveIoSelfTest = false;
static bool s_saveIoSelfTestQueued = false;
static std::string s_saveIoSelfTestName;
static bool s_saveMenuSelfTest = false;
static bool s_saveMenuSelfTestPassed = true;
static bool s_menuScriptFailed = false;
static std::string s_snapshotRoundtripSelfTestName;
static bool s_snapshotRoundtripSelfTestPassed = false;
static bool s_snapshotRoundtripLockAudio = false;
static bool s_snapshotRoundtripPerturbAudio = false;
static bool s_snapshotRoundtripCheckPlayback = false;
static std::string s_contractAuditOperation;
static long long s_contractAuditTick = 50;
static bool s_contractAuditFinished = false;
static std::string s_loadSelfTestName;
static bool s_loadSelfTestExpected = false;
static bool s_loadSelfTestPassed = false;
static bool s_saveCatalogSelfTest = false;
static bool s_saveCallbacksSelfTest = false;
static bool s_saveCallbacksSelfTestPassed = false;
static bool s_purgeSelfTest = false;
static bool s_purgeSelfTestPassed = false;
static bool s_globalCallbacksSelfTest = false;
static bool s_globalCallbacksSelfTestPassed = false;

// CLI -num-lua-states override for the determinism thread-count matrix. -1 = no override.
static constexpr int c_NetSessionDefaultLuaStates = 4;
static int s_cliNumLuaStatesOverride = -1;

// Post-module-load diagnostic. Empty means disabled.
static std::string s_netIdentityDumpPath;

// Debug-only transport/session smoke. This exits before gameplay starts.
static bool s_netHost = false;
static std::string s_netJoinAddress;
static uint16_t s_netPort = 41010;
static std::string s_netSessionReportPath;
static bool s_netExitAfterReady = false;
static bool s_netAllowUserdata = false;
// Phase B, unattended gates: the host stands in for a moderator on the seat it is told to watch.
static bool s_netH4Substitute = false;
static uint16_t s_netH4SubstituteSeat = 0;
static uint64_t s_netH4SubstituteDelayMs = 0;
static bool s_netH4SubstituteCancel = false;
static bool s_netLockstep = false;
static bool s_netMatch = false;
static bool s_netMatchServiceE2E = false;
static std::string s_netMatchServiceE2EPreset = "P4 Alpha Duel";
static std::string s_netLockstepReportPath;
// A capped stop holds the link while the relay host hands over what it still owes; a client one
// input-delay behind needs those forwards to finish its own last tick.
static constexpr uint32_t c_CappedStopDrainMs = 8000;
static constexpr uint32_t c_CappedStopLingerMs = 1500;
static uint64_t s_netLockstepTicks = 0;
static uint16_t s_netLockstepInputDelay = 0;
static uint8_t s_netMatchPeers = 2;
static std::string s_netMatchMode = "pvp";
static std::string s_netMatchOwnershipPolicy = "team-owner";
static bool s_netMatchServiceE2EEnteredEditor = false;
static NetMatchE2ETickClock s_netMatchE2ETicks;
static long s_netMatchE2EActorCensus = -1;
static long s_netMatchE2EActorCensusPeak = -1; //!< The max actor count seen, so a transient heal double-spawn that later sheds back to normal is still visible.
// Loop-pace accounting, accumulated only while a lockstep match or playback runs: the honest
// wall-tps and per-tick sim cost that steer the pace and rollback work.
static uint64_t s_paceIterations = 0;
static uint64_t s_paceSimTicks = 0;
static long long s_paceSimUs = 0;
static long long s_paceUpdateUs = 0;
static long long s_paceDrawUs = 0;
// Rollback fidelity probe: capture at tick T, record K hashed ticks, restore + rewind,
// re-run the SAME ticks, compare. Green = the restore layer reproduces the sim byte-exactly.
static long long s_rbProbeAtTick = 0;
static long long s_rbProbeWindow = 30;
static bool s_rbProbeRequested = false;
static int s_rbProbePhase = 0; //!< 0 idle, 1 first pass, 2 restore staged, 3 re-run pass.
static std::vector<SimChecksum::Result> s_rbProbeFirst;
static std::vector<SimChecksum::Result> s_rbProbeSecond;
static long long s_rbProbeSimCount = 0;
static long long s_rbProbeSimTimeTicks = 0;
static bool s_rbProbeInMemory = true;
static bool s_rbProbeUseLoadGame = false;
static bool s_rbProbeLaunchRestorePending = false;
static bool s_rbProbeMemoryRestorePending = false;
static MovableMan::WorldSnapshot s_rbProbeWorld;
static MovableMan::WorldSetAside s_rbProbeOriginals;
static std::string s_rbProbeLuaIdentityAtCapture;
static std::vector<std::string> s_rbProbeLuaGraphsAtCapture;
static std::deque<long long> s_rbProbeSchedule;
static int s_rbProbeFuzzCount = 0;
static uint64_t s_rbProbeFuzzSeed = 1;
static int s_rbProbePassCount = 0;
static int s_rbProbeFailCount = 0;
static std::string s_rbProbeFirstFailure;
static std::string s_rbProbeCapturedDeep;
static std::vector<std::string> s_rbProbeFirstDeep;
static long long s_rbProbeDeepDivergence = -1;
static bool s_rbProbeRestoreMismatch = false;

static int s_netMatchServiceE2EExitCode = 0;
static int s_netMatchServiceE2ERematches = 0;
static int s_netMatchResyncs = 0;
static bool s_netMatchResyncOnDesync = false;
static bool s_netMatchAutoDelay = false;
static std::string s_netMatchServiceE2EError;
static std::string s_netReplayInPath;
static std::string s_netReplayVerifyPath;
static uint64_t s_netReplayDumpFrom = 1;
static uint64_t s_netReplayDumpTo = 0;
static long long s_lpInvarianceTick = 0;
static std::vector<int> s_lpInvarianceDepths;
static std::vector<int> s_lpInvarianceRepeats;
static int s_lpInvarianceFailures = -1; //!< -1 = not run, else the count of failed checks.
static std::string s_lpExpectEquip; //!< -lpinv-expect: the preset the previews must hold once their horizon passes its pickup tick.
static long long s_lpExpectEquipTick = 0;
static long long s_lpExpectEquipSlack = 0; //!< Ticks the preview's pickup may lag the canonical one (the reach ray is a random cast).
static long long s_lpExpectFireTick = 0;
static long long s_lpExpectFireSlack = 0;
static std::string s_netReplayOutPath;
static int s_netReplayExitCode = 0;
static uint64_t s_netReplayTicks = 0;
bool ConfigureNetMatchServiceE2EActivity(const std::string& activityPreset, std::string* error);
bool StageResyncedMatchActivity(std::string* error);

// A dead-end transport for replay playback: nothing to poll, nowhere to send.
class NullNetTransport final : public INetTransport {
public:
	bool StartHost(uint16_t, std::string*) override { return true; }
	bool Connect(const std::string&, uint16_t, std::string*) override { return true; }
	bool Send(NetPeerId, NetTransportLane, const std::vector<uint8_t>&, std::string*, bool*) override { return true; }
	void Disconnect(NetPeerId, const std::string&) override {}
	void Stop() override {}
	std::vector<NetTransportEvent> PollEvents() override { return {}; }
};
static std::string s_menuScriptPath;
static std::string s_menuScriptOutDir;
// §9b's moderation panel, driven headless: the gate names the actions, the seat and how long to wait
// before each. They take the panel's own path, so a gate exercises what a host clicks.
static std::vector<std::string> s_netMatchE2eModerate;
static size_t s_netMatchE2eModerateAt = 0;
static int s_netMatchE2eModerateSeat = -1;
static uint64_t s_netMatchE2eModerateDelayMs = 0;
static uint64_t s_netMatchE2eModerateReadyMs = 0;

bool NetGameplayRequested() {
	return s_netLockstep || s_netMatch;
}

const char* ActivityStateName(Activity::ActivityState state);

/// <summary>
/// Initializes all the essential managers.
/// </summary>
void InitializeManagers() {
	ThreadMan::Construct();
	TimerMan::Construct();
	PresetMan::Construct();
	SettingsMan::Construct();
	WindowMan::Construct();
	GLResourceMan::Construct();
	LuaMan::Construct();
	FrameMan::Construct();
	PerformanceMan::Construct();
	PostProcessMan::Construct();
	PrimitiveMan::Construct();
	AudioMan::Construct();
	GUISound::Construct();
	MusicMan::Construct();
	UInputMan::Construct();
	ConsoleMan::Construct();
	SceneMan::Construct();
	MovableMan::Construct();
	MetaMan::Construct();
	MenuMan::Construct();
	CameraMan::Construct();
	ActivityMan::Construct();
	LoadingScreen::Construct();
	MetricsCollector::Construct();
	SimChecksum::Construct();
	NetMatchService::Construct();

	g_ThreadMan.Initialize();
	g_SettingsMan.Initialize();

	// Apply the CLI -num-lua-states override after SettingsMan loads (so it wins over the file)
	// and before LuaMan creates its threaded states.
	if (s_cliNumLuaStatesOverride >= 0) {
		g_SettingsMan.SetNumberOfLuaStatesOverride(s_cliNumLuaStatesOverride);
	}

	g_WindowMan.Initialize();
	g_GLResourceMan.Initialize();

	g_LuaMan.Initialize();
	g_TimerMan.Initialize();
	g_FrameMan.Initialize();
	g_PostProcessMan.Initialize();
	g_PerformanceMan.Initialize();

	if (g_AudioMan.Initialize()) {
		g_GUISound.Initialize();
		g_MusicMan.Initialize();
		if (std::getenv("CCCP_HEADLESS") != nullptr) {
			g_AudioMan.SetOutputSilenced(true);
			std::cout << "[audio] output silenced for the headless run" << std::endl;
		}
	}

	g_UInputMan.Initialize();
	g_ConsoleMan.Initialize();
	g_SceneMan.Initialize();
	g_MovableMan.Initialize();
	g_MetaMan.Initialize();
	g_MenuMan.Initialize();

	// Overwrite Settings.ini after all the managers are created to fully populate the file. Up until this moment Settings.ini is populated only with minimal required properties to run.
	// If Settings.ini already exists and is fully populated, this will deal with overwriting it to apply any overrides performed by the managers at boot (e.g resolution validation).
	if (g_SettingsMan.SettingsNeedOverwrite()) {
		g_SettingsMan.UpdateSettingsFile();
	}
}

/// <summary>
/// Destroys all the managers and frees all loaded data before termination.
/// </summary>
void DestroyManagers() {
	g_SimChecksum.Destroy();
	g_NetMatchService.Destroy();
	g_MetricsCollector.Destroy();
	g_MetaMan.Destroy();
	g_PerformanceMan.Destroy();
	g_MovableMan.Destroy();
	g_SceneMan.Destroy();
	g_ActivityMan.Destroy();
	g_GUISound.Destroy();
	g_AudioMan.Destroy();
	g_MusicMan.Destroy();
	g_PresetMan.Destroy();
	g_UInputMan.Destroy();
	g_PostProcessMan.Destroy();
	g_FrameMan.Destroy();
	g_TimerMan.Destroy();
	g_LuaMan.Destroy();
	ContentFile::FreeAllLoaded();
	g_ConsoleMan.Destroy();
	g_GLResourceMan.Destroy();
	g_WindowMan.Destroy();

#ifdef DEBUG_BUILD
	Entity::ClassInfo::DumpPoolMemoryInfo(Writer("MemCleanupInfo.txt"));
#endif
}

int ShutDown(int exitCode) {
	if (!s_contractAuditOperation.empty() && !s_contractAuditFinished) exitCode = EXIT_FAILURE;
	if (s_menuScriptFailed) exitCode = EXIT_FAILURE;
	if (s_bitmapSaveSelfTest && s_bitmapSaveSelfTestResult != 0) exitCode = EXIT_FAILURE;
	if (!s_snapshotRoundtripSelfTestName.empty() && !s_snapshotRoundtripSelfTestPassed) exitCode = EXIT_FAILURE;
	if (!s_loadSelfTestName.empty() && !s_loadSelfTestPassed) exitCode = EXIT_FAILURE;
	if (s_saveCallbacksSelfTest && !s_saveCallbacksSelfTestPassed) exitCode = EXIT_FAILURE;
	if (s_purgeSelfTest && !s_purgeSelfTestPassed) exitCode = EXIT_FAILURE;
	if (s_globalCallbacksSelfTest && !s_globalCallbacksSelfTestPassed) exitCode = EXIT_FAILURE;
	if (s_saveIoSelfTest) {
		const bool saved = s_saveIoSelfTestQueued && g_ActivityMan.WaitForSaveGameTask();
		std::cout << "[save-selftest] completed=" << saved << std::endl;
		if (!saved || !s_saveMenuSelfTestPassed) exitCode = EXIT_FAILURE;
	}
	g_ThreadMan.GetPriorityThreadPool().wait_for_tasks();
	g_ThreadMan.GetBackgroundThreadPool().wait_for_tasks();
	LocalPrediction::Clear();
	if (s_rbProbeOriginals.held) {
		// These originals outlive the managers, so a refusal here is discarded while there is still an engine to do it.
		if (!g_MovableMan.ReinstateWorld(s_rbProbeOriginals)) {
			g_MovableMan.DiscardWorld(s_rbProbeOriginals);
		}
		exitCode = EXIT_FAILURE;
	}
	s_rbProbeWorld.Clear();
	g_ConsoleMan.SaveAllText("LogConsole.txt");
	DestroyManagers();
	allegro_exit();
	SDL_Quit();
	std::cout.flush();
	std::cerr.flush();
	return exitCode;
}

/// <summary>
/// Command-line argument handling.
/// </summary>
/// <param name="argCount">Argument count.</param>
/// <param name="argValue">Argument values.</param>
bool HandleMainArgs(int argCount, char** argValue) {
	// Discard the first argument because it's always the executable path/name
	argCount--;
	argValue++;
	if (argCount == 0) {
		return true;
	}
	bool launchModeSet = false;
	bool singleModuleSet = false;

	for (int i = 0; i < argCount;) {
		std::string currentArg = argValue[i];
		bool lastArg = i + 1 == argCount;

		if (currentArg == "-cout") {
			System::EnableLoggingToCLI();
		}

		if (currentArg == "-ext-validate") {
			System::EnableExternalModuleValidationMode();
		}

		// Arm per-tick state hashing for the determinism trace.
		if (currentArg == "-tick-hashes") {
			s_recordTickHashes = true;
			// Deterministic runs drain async path solves each frame so they can't race the node-cost rewrite.
			g_SettingsMan.SetForceImmediatePathingRequestCompletion(true);
		}
		if (currentArg == "-bitmap-save-selftest") {
			s_bitmapSaveSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-camera-null-scene-selftest") {
			s_cameraNullSceneSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-save-callback-selftest") {
			s_saveCallbacksSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-global-callback-selftest") {
			s_globalCallbacksSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-purge-selftest") {
			s_purgeSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-save-catalog-selftest") {
			s_saveCatalogSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-contract-audit" && i + 1 < argCount) {
			s_contractAuditOperation = argValue[i + 1];
			i += 2;
			continue;
		}
		if (currentArg == "-contract-audit-tick" && i + 1 < argCount) {
			s_contractAuditTick = std::stoll(argValue[i + 1]);
			i += 2;
			continue;
		}
		if (currentArg == "-snapshot-roundtrip-lock-audio" || currentArg == "-snapshot-roundtrip-perturb-audio" || currentArg == "-snapshot-roundtrip-check-playback") {
			if (currentArg == "-snapshot-roundtrip-lock-audio") s_snapshotRoundtripLockAudio = true;
			if (currentArg == "-snapshot-roundtrip-perturb-audio") s_snapshotRoundtripPerturbAudio = true;
			if (currentArg == "-snapshot-roundtrip-check-playback") s_snapshotRoundtripCheckPlayback = true;
			++i;
			continue;
		}
		if (currentArg == "-snapshot-roundtrip-selftest" && i + 1 < argCount) {
			s_snapshotRoundtripSelfTestName = argValue[i + 1];
			i += 2;
			continue;
		}
		if ((currentArg == "-load-io-selftest" || currentArg == "-load-io-success-selftest") && i + 1 < argCount) {
			s_loadSelfTestName = argValue[i + 1];
			s_loadSelfTestExpected = currentArg == "-load-io-success-selftest";
			i += 2;
			continue;
		}
		if ((currentArg == "-save-io-selftest" || currentArg == "-save-menu-selftest") && i + 1 < argCount) {
			s_saveIoSelfTest = true;
			s_saveMenuSelfTest = currentArg == "-save-menu-selftest";
			s_saveIoSelfTestName = argValue[i + 1];
			i += 2;
			continue;
		}

		// Scenario direct-launch + determinism flags (-scenario, -seed, -max-ticks, ...).
		if (int consumed = ScenarioRunner::ParseArgs(argCount, argValue, i); consumed > 0) {
			i += consumed;
			continue;
		}

		if (!lastArg && currentArg == "-net-identity-dump") {
			s_netIdentityDumpPath = argValue[++i];
			continue;
		}

		if (currentArg == "-net-host") {
			s_netHost = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-join") {
			s_netJoinAddress = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-port") {
			const long parsedPort = std::strtol(argValue[++i], nullptr, 10);
			if (parsedPort > 0 && parsedPort <= 65535) {
				s_netPort = static_cast<uint16_t>(parsedPort);
			}
			continue;
		}

		if (!lastArg && currentArg == "-net-session-report") {
			s_netSessionReportPath = argValue[++i];
			continue;
		}

		if (currentArg == "-net-no-reconnect-admission") {
			NetMatchService::SetAdmissionEnabled(false);
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-reconnect-ticket") {
			NetMatchService::SetTicketStorePath(argValue[++i]);
			continue;
		}

		// Phase B, unattended gates: the joiner asks the host for a seat instead of joining one, and
		// the host approves the first applicant for that seat the way a moderator would.
		if (!lastArg && currentArg == "-net-h4-apply") {
			NetMatchService::SetApplyForSeat(true, static_cast<uint16_t>(std::stoi(argValue[++i])));
			continue;
		}

		if (!lastArg && currentArg == "-net-h4-substitute") {
			s_netH4Substitute = true;
			s_netH4SubstituteSeat = static_cast<uint16_t>(std::stoi(argValue[++i]));
			NetMatchService::SetAutoSubstitute(s_netH4Substitute, s_netH4SubstituteSeat, s_netH4SubstituteDelayMs, s_netH4SubstituteCancel);
			continue;
		}

		if (!lastArg && currentArg == "-net-h4-substitute-delay") {
			s_netH4SubstituteDelayMs = static_cast<uint64_t>(std::stoll(argValue[++i]));
			NetMatchService::SetAutoSubstitute(s_netH4Substitute, s_netH4SubstituteSeat, s_netH4SubstituteDelayMs, s_netH4SubstituteCancel);
			continue;
		}

		if (currentArg == "-net-h4-substitute-cancel") {
			s_netH4SubstituteCancel = true;
			NetMatchService::SetAutoSubstitute(s_netH4Substitute, s_netH4SubstituteSeat, s_netH4SubstituteDelayMs, s_netH4SubstituteCancel);
			++i;
			continue;
		}

		if (currentArg == "-net-exit-after-ready") {
			s_netExitAfterReady = true;
			++i;
			continue;
		}

		if (currentArg == "-net-allow-userdata") {
			s_netAllowUserdata = true;
			++i;
			continue;
		}

		if (currentArg == "-net-lockstep") {
			s_netLockstep = true;
			++i;
			continue;
		}

		if (currentArg == "-net-match") {
			s_netMatch = true;
			++i;
			continue;
		}

		if (currentArg == "-net-match-service-e2e") {
			s_netMatchServiceE2E = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-match-service-preset") {
			s_netMatchServiceE2EPreset = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-menu-script") {
			s_menuScriptPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-menu-script-out") {
			s_menuScriptOutDir = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-lockstep-report") {
			s_netLockstepReportPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-match-report") {
			s_netLockstepReportPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-lockstep-ticks") {
			s_netLockstepTicks = static_cast<uint64_t>(std::strtoull(argValue[++i], nullptr, 10));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-ticks") {
			s_netLockstepTicks = static_cast<uint64_t>(std::strtoull(argValue[++i], nullptr, 10));
			continue;
		}

		if (!lastArg && currentArg == "-net-lockstep-input-delay") {
			const unsigned long parsedDelay = std::strtoul(argValue[++i], nullptr, 10);
			s_netLockstepInputDelay = static_cast<uint16_t>(std::min<unsigned long>(parsedDelay, NetLockstepCodec::c_MaxInputDelayFrames));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-input-delay") {
			const unsigned long parsedDelay = std::strtoul(argValue[++i], nullptr, 10);
			s_netLockstepInputDelay = static_cast<uint16_t>(std::min<unsigned long>(parsedDelay, NetLockstepCodec::c_MaxInputDelayFrames));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-ownership-policy") {
			s_netMatchOwnershipPolicy = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-match-peers") {
			const unsigned long parsedPeers = std::strtoul(argValue[++i], nullptr, 10);
			s_netMatchPeers = static_cast<uint8_t>(std::clamp<unsigned long>(parsedPeers, 2, NetMatchConfigUtil::c_MaxPeerCount));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-mode") {
			s_netMatchMode = argValue[++i];
			continue;
		}

		if (currentArg == "-net-match-e2e-resync") {
			s_netMatchResyncOnDesync = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-h4-fault") {
			const std::string kind = argValue[++i];
			NetH4SetFault(NetH4FaultFromName(kind));
			std::cout << "[net-h4-fault] armed " << kind << std::endl;
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-moderate") {
			// Repeatable: the actions run in order, one per delay, on the one seat.
			s_netMatchE2eModerate.emplace_back(argValue[++i]);
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-moderate-seat") {
			s_netMatchE2eModerateSeat = static_cast<int>(std::strtol(argValue[++i], nullptr, 10));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-moderate-delay") {
			s_netMatchE2eModerateDelayMs = std::strtoull(argValue[++i], nullptr, 10);
			continue;
		}

		if (!lastArg && currentArg == "-net-replay-out") {
			s_netReplayOutPath = argValue[++i];
			ScenarioRunner::ArmLockstepReplayRecord(s_netReplayOutPath);
			continue;
		}

		if (!lastArg && currentArg == "-net-fake-lag") {
			GnsTransport::SetSimulatedLagMs(static_cast<int>(std::strtol(argValue[++i], nullptr, 10)));
			continue;
		}

		if (!lastArg && currentArg == "-net-local-prediction") {
			LocalPrediction::SetCommandLineOverride(std::string(argValue[++i]) == "off" ? 0 : 1);
			continue;
		}

		if (!lastArg && currentArg == "-rollback-fidelity-probe") {
			// T:K — capture after tick T, gate K re-run ticks against the first pass.
			const std::string probeSpec = argValue[++i];
			const size_t colon = probeSpec.find(':');
			s_rbProbeAtTick = std::strtoll(probeSpec.c_str(), nullptr, 10);
			s_rbProbeRequested = true;
			if (colon != std::string::npos) {
				s_rbProbeWindow = std::strtoll(probeSpec.c_str() + colon + 1, nullptr, 10);
			}
			continue;
		}

		if (!lastArg && currentArg == "-rollback-fidelity-probe-mode") {
			const std::string mode = argValue[++i];
			s_rbProbeUseLoadGame = mode == "launch";
			s_rbProbeInMemory = mode != "file" && !s_rbProbeUseLoadGame;
			continue;
		}

		if (!lastArg && currentArg == "-rollback-fidelity-fuzz") {
			// seed:count:K — count random capture ticks in one process, each gated over K re-run ticks.
			const std::string spec = argValue[++i];
			const size_t first = spec.find(':');
			const size_t second = first == std::string::npos ? std::string::npos : spec.find(':', first + 1);
			s_rbProbeFuzzSeed = std::strtoull(spec.c_str(), nullptr, 10);
			s_rbProbeRequested = true;
			if (first != std::string::npos) {
				s_rbProbeFuzzCount = static_cast<int>(std::strtol(spec.c_str() + first + 1, nullptr, 10));
			}
			if (second != std::string::npos) {
				s_rbProbeWindow = std::strtoll(spec.c_str() + second + 1, nullptr, 10);
			}
			continue;
		}

		if (currentArg == "-net-match-auto-delay") {
			s_netMatchAutoDelay = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-replay") {
			s_netReplayInPath = argValue[++i];
			continue;
		}
		if (!lastArg && currentArg == "-net-replay-verify") {
			s_netReplayVerifyPath = argValue[++i];
			continue;
		}
		if (!lastArg && currentArg == "-net-replay-dump") {
			// <from>:<to> — with -net-replay-verify, print the recorded frames and commands of those ticks.
			const std::string spec = argValue[++i];
			const size_t colon = spec.find(':');
			if (colon == std::string::npos) {
				std::cerr << "[net-replay-dump] bad range '" << spec << "': expected <from>:<to>" << std::endl;
				return false;
			}
			s_netReplayDumpFrom = std::strtoull(spec.c_str(), nullptr, 10);
			s_netReplayDumpTo = std::strtoull(spec.c_str() + colon + 1, nullptr, 10);
			continue;
		}
		if (!lastArg && currentArg == "-input-script") {
			// A fixture's inputs stand in for the player's devices at the UInputMan boundary.
			std::string scriptError;
			if (!InputScript::Load(argValue[++i], &scriptError)) {
				std::cerr << "[input-script] " << scriptError << std::endl;
				return false;
			}
			continue;
		}
		if (!lastArg && currentArg == "-ai-write-script") {
			// A fixture's direct AI writes on a local actor, made inside the owner's AI pass.
			std::string scriptError;
			if (!AIWriteScript::Load(argValue[++i], &scriptError)) {
				std::cerr << "[ai-write-script] " << scriptError << std::endl;
				return false;
			}
			continue;
		}
		if (!lastArg && currentArg == "-digital-aim-speed") {
			// <player>:<multiplier> — this machine's digital aim speed for the player, a per-machine setting the sim may only read off the wire.
			const std::string spec = argValue[++i];
			const size_t colon = spec.find(':');
			char* end = nullptr;
			const float speed = colon == std::string::npos ? 0.0F : std::strtof(spec.c_str() + colon + 1, &end);
			const int player = colon == std::string::npos ? -1 : std::atoi(spec.substr(0, colon).c_str());
			if (colon == std::string::npos || player < 0 || player >= Players::MaxPlayerCount || !end || *end != '\0' || !(speed > 0.0F)) {
				std::cerr << "[digital-aim-speed] bad spec '" << spec << "': expected <player>:<multiplier>" << std::endl;
				return false;
			}
			g_UInputMan.GetControlScheme(player)->SetDigitalAimSpeed(speed);
			std::cout << "[digital-aim-speed] player " << player << " -> " << speed << std::endl;
			continue;
		}
		if (!lastArg && currentArg == "-local-prediction-depth") {
			const std::string text = argValue[++i];
			if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
				std::cerr << "[localpred] bad depth '" << text << "': expected a whole number" << std::endl;
				return false;
			}
			LocalPrediction::SetDepthOverride(static_cast<int>(std::strtol(text.c_str(), nullptr, 10)));
			continue;
		}
		if (!lastArg && currentArg == "-local-prediction-invariance") {
			// T:d1,d2,...:r1,r2,... — at tick T run previews of each depth, each repeat count, and prove the canonical world untouched.
			const std::string spec = argValue[++i];
			const auto parsePositive = [](const std::string& text, long long& out) {
				if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
					return false;
				}
				out = std::strtoll(text.c_str(), nullptr, 10);
				return out > 0;
			};
			const auto parseList = [&parsePositive](const std::string& text, std::vector<int>& out) {
				size_t start = 0;
				while (true) {
					const size_t comma = text.find(',', start);
					long long value = 0;
					if (!parsePositive(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start), value)) {
						return false;
					}
					out.push_back(static_cast<int>(value));
					if (comma == std::string::npos) {
						return true;
					}
					start = comma + 1;
				}
			};
			const size_t first = spec.find(':');
			const size_t second = first == std::string::npos ? std::string::npos : spec.find(':', first + 1);
			long long tick = 0;
			bool ok = parsePositive(spec.substr(0, first), tick);
			if (ok && first != std::string::npos) {
				const std::string depths = spec.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
				ok = depths.empty() || parseList(depths, s_lpInvarianceDepths);
			}
			if (ok && second != std::string::npos) {
				const std::string repeats = spec.substr(second + 1);
				ok = repeats.empty() || parseList(repeats, s_lpInvarianceRepeats);
			}
			if (!ok) {
				std::cerr << "[lpinv] bad spec '" << spec << "': expected T:d1,d2,...:r1,r2,... with positive whole numbers" << std::endl;
				return false;
			}
			s_lpInvarianceTick = tick;
			if (s_lpInvarianceDepths.empty()) {
				s_lpInvarianceDepths = {1, 3, 8, 14};
			}
			if (s_lpInvarianceRepeats.empty()) {
				s_lpInvarianceRepeats = {1, 3};
			}
			continue;
		}
		if (!lastArg && currentArg == "-lpinv-expect") {
			// equip=<preset>@<tick>[~<slack>],fire@<tick>[~<slack>] — the canonical ticks the previews must reproduce once their horizon passes them.
			const std::string spec = argValue[++i];
			bool ok = !spec.empty();
			size_t start = 0;
			while (ok && start < spec.size()) {
				const size_t comma = spec.find(',', start);
				const std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				const size_t at = item.rfind('@');
				const size_t tilde = item.find('~', at == std::string::npos ? 0 : at);
				const std::string tickText = at == std::string::npos ? std::string() : item.substr(at + 1, tilde == std::string::npos ? std::string::npos : tilde - at - 1);
				const std::string slackText = tilde == std::string::npos ? std::string("0") : item.substr(tilde + 1);
				if (at == std::string::npos || tickText.empty() || tickText.find_first_not_of("0123456789") != std::string::npos || slackText.empty() || slackText.find_first_not_of("0123456789") != std::string::npos) {
					ok = false;
					break;
				}
				const long long tick = std::strtoll(tickText.c_str(), nullptr, 10);
				const long long slack = std::strtoll(slackText.c_str(), nullptr, 10);
				if (item.rfind("equip=", 0) == 0 && at > 6) {
					s_lpExpectEquip = item.substr(6, at - 6);
					s_lpExpectEquipTick = tick;
					s_lpExpectEquipSlack = slack;
				} else if (item.substr(0, at) == "fire") {
					s_lpExpectFireTick = tick;
					s_lpExpectFireSlack = slack;
				} else {
					ok = false;
				}
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
			if (!ok || (s_lpExpectEquip.empty() && s_lpExpectFireTick <= 0)) {
				std::cerr << "[lpinv] bad expectation '" << spec << "': expected equip=<preset>@<tick>,fire@<tick>" << std::endl;
				return false;
			}
			continue;
		}

		if (!lastArg && !singleModuleSet && currentArg == "-module") {
			std::string moduleToLoad = argValue[++i];
			if (moduleToLoad.find(System::GetModulePackageExtension()) == moduleToLoad.length() - System::GetModulePackageExtension().length()) {
				g_PresetMan.SetSingleModuleToLoad(moduleToLoad);
				singleModuleSet = true;
			}
		}
		if (!launchModeSet) {
			if (!lastArg && currentArg == "-editor") {
				g_ActivityMan.SetEditorToLaunch(argValue[++i]);
				launchModeSet = true;
			}
		}
		++i;
	}
	if (launchModeSet) {
		g_SettingsMan.SetSkipIntro(true);
	}
	return true;
}

/// <summary>
/// Polls the SDL event queue and passes events to be handled by the relevant managers.
/// </summary>
void PollSDLEvents() {
	NetModerationGUIProbe::BeforePoll();
	SDL_Event sdlEvent;
	while (SDL_PollEvent(&sdlEvent)) {
		switch (sdlEvent.type) {
			case SDL_EVENT_QUIT :
				System::SetQuit(true);
				return;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				System::SetQuit(true);
				return;
			case SDL_EVENT_KEY_UP :
			case SDL_EVENT_KEY_DOWN :
			case SDL_EVENT_TEXT_INPUT :
			case SDL_EVENT_MOUSE_MOTION :
			case SDL_EVENT_MOUSE_BUTTON_UP :
			case SDL_EVENT_MOUSE_BUTTON_DOWN :
			case SDL_EVENT_MOUSE_WHEEL :
			case SDL_EVENT_GAMEPAD_AXIS_MOTION :
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN :
			case SDL_EVENT_GAMEPAD_BUTTON_UP :
			case SDL_EVENT_JOYSTICK_AXIS_MOTION :
			case SDL_EVENT_JOYSTICK_BUTTON_DOWN :
			case SDL_EVENT_JOYSTICK_BUTTON_UP :
			case SDL_EVENT_JOYSTICK_ADDED :
			case SDL_EVENT_JOYSTICK_REMOVED :
				g_UInputMan.HandleInputEvent(sdlEvent);
				break;
			default:
				break;
		}
		ImGui_ImplSDL3_ProcessEvent(&sdlEvent);
		if (sdlEvent.type >= SDL_EVENT_WINDOW_FIRST && sdlEvent.type <= SDL_EVENT_WINDOW_LAST) {
			g_WindowMan.QueueWindowEvent(sdlEvent);
		}
	}
}

// The e2e driver uses the same live panel as the host.
static void DriveModerationE2e() {
	if (s_netMatchE2eModerateAt >= s_netMatchE2eModerate.size()) {
		return;
	}
	NetModerationGUI* menu = g_MenuMan.GetNetworkPanel();
	if (!menu) {
		return;
	}
	const std::vector<NetH4ModerationSeat> seats = g_NetMatchService.GetModerationSeats();
	const bool anythingToDecide = std::any_of(seats.begin(), seats.end(), [](const NetH4ModerationSeat& seat) {
		return seat.dropped || seat.closed || seat.substituting;
	});
	if (!anythingToDecide) {
		return;
	}
	const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	if (s_netMatchE2eModerateReadyMs == 0) {
		s_netMatchE2eModerateReadyMs = nowMs + s_netMatchE2eModerateDelayMs;
		std::cout << "[net-match-e2e] moderate armed actions=" << s_netMatchE2eModerate.size()
		          << " seat=" << s_netMatchE2eModerateSeat << " delay_ms=" << s_netMatchE2eModerateDelayMs << std::endl;
	}
	if (nowMs < s_netMatchE2eModerateReadyMs) {
		return;
	}
	// A substitution needs an applicant; until one turns up the panel's own button is disabled too,
	// so the driver waits exactly as a host would rather than pressing a dead button.
	const std::string& action = s_netMatchE2eModerate[s_netMatchE2eModerateAt];
	if (!menu->AutomationModerate(action, s_netMatchE2eModerateSeat)) {
		return;
	}
	std::cout << "[net-match-e2e] moderate " << action << " seat=" << s_netMatchE2eModerateSeat << " done" << std::endl;
	++s_netMatchE2eModerateAt;
	s_netMatchE2eModerateReadyMs = nowMs + s_netMatchE2eModerateDelayMs;
}

/// <summary>
/// Game menus loop.
/// </summary>
// A scripted-menu step failed: print it and exit non-zero so the automation harness can't false-green.
static void MenuScriptFail(const std::string& reason) {
	std::cerr << "[menu-script] FAILED: " << reason << std::endl;
	s_menuScriptFailed = true;
	System::SetQuit(true);
}

// Drives the real MainMenuGUI from a script for automated UI testing: one step per call, after the
// interactive main menu is up. Screenshots use the normal render path. Quits when the script ends.
void ProcessMenuScript() {
	static std::vector<std::string> steps;
	static size_t stepIndex = 0;
	static int waitFrames = 0;
	static bool loaded = false;
	static std::string waitCond;
	static int waitCondTimeout = 0;

	if (!loaded) {
		std::ifstream in(s_menuScriptPath);
		if (!in) {
			return MenuScriptFail("could not open menu-script file: " + s_menuScriptPath);
		}
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') { line.pop_back(); }
			if (line.empty() || line[0] == '#') { continue; }
			steps.push_back(line);
		}
		loaded = true;
		std::cout << "[menu-script] loaded " << steps.size() << " steps" << std::endl;
		if (steps.empty()) {
			return MenuScriptFail("menu-script has no steps: " + s_menuScriptPath);
		}
	}
	static bool introSkipped = false;
	if (!g_MenuMan.IsMainMenuInteractive()) {
		if (!introSkipped) {
			g_MenuMan.SkipTitleIntroForAutomation();
		}
		return;
	}
	introSkipped = true;
	if (waitFrames > 0) {
		--waitFrames;
		return;
	}
	// Condition wait: block until the service reaches a member count / state, robust to variable FPS across
	// two contending instances (frame-count waits can't synchronize two real-time peers reliably).
	if (!waitCond.empty()) {
		const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
		bool met = false;
		if (waitCond.rfind("members:", 0) == 0) {
			met = static_cast<int>(snapshot.members.size()) >= std::atoi(waitCond.c_str() + 8);
		} else if (waitCond.rfind("connected:", 0) == 0) {
			met = std::count_if(snapshot.members.begin(), snapshot.members.end(), [](const NetLobbyMember& member) { return member.connected; }) == std::atoi(waitCond.c_str() + 10);
		} else if (waitCond == "allready") {
			met = snapshot.members.size() > 1 && std::all_of(snapshot.members.begin(), snapshot.members.end(), [](const NetLobbyMember& member) { return member.connected && member.ready; });
		} else if (waitCond.rfind("state:", 0) == 0) {
			met = snapshot.serviceState == waitCond.substr(6);
		} else if (waitCond == "remoteready") {
			met = snapshot.remoteReady;
		} else if (waitCond.starts_with("error:")) {
			met = snapshot.errorText.find(waitCond.substr(6)) != std::string::npos;
		}
		if (met || --waitCondTimeout <= 0) {
			std::cout << "[menu-script] " << waitCond << " -> " << (met ? "OK" : "TIMEOUT") << " (members=" << snapshot.members.size() << " state=" << snapshot.serviceState << ")" << std::endl;
			if (!met) { return MenuScriptFail("condition wait timed out: " + waitCond); }
			waitCond.clear();
		}
		return;
	}
	if (stepIndex >= steps.size()) {
		std::cout << "[menu-script] complete" << std::endl;
		System::SetQuit(true);
		return;
	}
	std::istringstream iss(steps[stepIndex++]);
	std::string cmd;
	iss >> cmd;
	MainMenuGUI* menu = g_MenuMan.GetMainMenu();
	if (cmd == "wait") {
		iss >> waitFrames;
	} else if (cmd == "wait_members") {
		int n = 0;
		iss >> n;
		waitCond = "members:" + std::to_string(n);
		waitCondTimeout = 4000;
	} else if (cmd == "wait_state") {
		std::string s;
		iss >> s;
		waitCond = "state:" + s;
		waitCondTimeout = 4000;
	} else if (cmd == "wait_error") {
		std::string text;
		std::getline(iss >> std::ws, text);
		if (text.empty()) return MenuScriptFail("wait_error requires a nonempty substring");
		waitCond = "error:" + text;
		waitCondTimeout = 4000;
	} else if (cmd == "wait_remote_ready") {
		waitCond = "remoteready";
		waitCondTimeout = 4000;
	} else if (cmd == "wait_connected") {
		int n = 0;
		iss >> n;
		waitCond = "connected:" + std::to_string(n);
		waitCondTimeout = 4000;
	} else if (cmd == "wait_all_ready") {
		waitCond = "allready";
		waitCondTimeout = 4000;
	} else if (cmd == "screenshot") {
		std::string name;
		iss >> name;
		// SaveScreenToPNG prepends System::GetScreenshotDirectory() ("ScreenShots/"); use a plain name.
		g_FrameMan.SaveScreenToPNG(name.c_str());
		std::cout << "[menu-script] screenshot ScreenShots/" << name << " screen=" << menu->AutomationActiveScreenName() << std::endl;
	} else if (cmd == "activate") {
		std::string control;
		iss >> control;
		const bool ok = menu->AutomationActivateControl(control);
		std::cout << "[menu-script] activate " << control << " ok=" << ok << std::endl;
		if (!ok) { return MenuScriptFail("activate failed (control missing, disabled, or hidden): " + control); }
	} else if (cmd == "assert_control") {
		std::string control;
		iss >> control;
		const bool exists = menu->AutomationControlExists(control);
		std::cout << "[menu-script] assert_control " << control << " " << (exists ? "PASS" : "FAIL") << std::endl;
		if (!exists) { return MenuScriptFail("assert_control names no control in the skin: " + control); }
	} else if (cmd == "moderate") {
		// The same panel action a host clicks, driven from a menu script.
		std::string action;
		int seat = -1;
		iss >> action;
		if (!(iss >> seat)) {
			seat = -1;
		}
		const bool ok = menu->AutomationModerate(action, seat);
		std::cout << "[menu-script] moderate " << action << " seat=" << seat << " ok=" << ok << std::endl;
		if (!ok) { return MenuScriptFail("moderate found no seat to act on: " + action); }
	} else if (cmd == "settext") {
		std::string control;
		std::string text;
		iss >> control;
		std::getline(iss, text);
		if (!text.empty() && text[0] == ' ') { text.erase(0, 1); }
		if (!menu->AutomationSetText(control, text)) { return MenuScriptFail("settext failed (textbox missing): " + control); }
	} else if (cmd == "assert_screen") {
		std::string expected;
		iss >> expected;
		const std::string actual = menu->AutomationActiveScreenName();
		const bool pass = actual == expected;
		std::cout << "[menu-script] assert_screen expected=" << expected << " actual=" << actual << " " << (pass ? "PASS" : "FAIL") << std::endl;
		if (!pass) { return MenuScriptFail("assert_screen expected " + expected + " got " + actual); }
	} else if (cmd == "assert_status" || cmd == "assert_error") {
		std::string sub;
		std::getline(iss, sub);
		if (!sub.empty() && sub[0] == ' ') { sub.erase(0, 1); }
		const std::string status = cmd == "assert_error" ? menu->AutomationMultiplayerError() : menu->AutomationMultiplayerStatus();
		const bool pass = status.find(sub) != std::string::npos;
		std::cout << "[menu-script] " << cmd << " \"" << sub << "\" status=\"" << status << "\" " << (pass ? "PASS" : "FAIL") << std::endl;
		if (!pass) { return MenuScriptFail(cmd + " missing substring: " + sub); }
	} else if (cmd == "assert_substate") {
		std::string expected;
		iss >> expected;
		const std::string actual = menu->AutomationMultiplayerSubScreen();
		const bool pass = actual == expected;
		std::cout << "[menu-script] assert_substate expected=" << expected << " actual=" << actual << " " << (pass ? "PASS" : "FAIL") << std::endl;
		if (!pass) { return MenuScriptFail("assert_substate expected " + expected + " got " + actual); }
	} else if (cmd == "dump_lobby") {
		const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
		std::cout << "[menu-script] dump_lobby state=" << snapshot.serviceState << " members=" << snapshot.members.size()
				  << " error=\"" << snapshot.errorText << "\" status=\"" << snapshot.statusText << "\"";
		for (const NetLobbyMember& member: snapshot.members) {
			std::cout << " | " << member.displayName << "(team" << static_cast<int>(member.team)
					  << (member.isLocal ? ",local" : ",remote") << ",ping" << member.pingMs << ")";
		}
		std::cout << std::endl;
	} else if (cmd == "assert_enabled") {
		std::string control;
		int expected = 0;
		iss >> control >> expected;
		const int actual = menu->AutomationControlEnabled(control) ? 1 : 0;
		const bool pass = actual == expected;
		std::cout << "[menu-script] assert_enabled " << control << " expected=" << expected << " actual=" << actual << " " << (pass ? "PASS" : "FAIL") << std::endl;
		if (!pass) { return MenuScriptFail("assert_enabled " + control + " expected " + std::to_string(expected)); }
	} else if (cmd == "exit") {
		System::SetQuit(true);
	} else {
		return MenuScriptFail("unknown command: " + cmd);
	}
}

void RunMenuLoop() {
	g_MenuMan.SetIsInMenuScreen(true);
	g_UInputMan.DisableKeys(false);
	g_UInputMan.TrapMousePos(false);

	while (!System::IsSetToQuit()) {
		g_WindowMan.ClearBackbuffer();
		PollSDLEvents();

		g_WindowMan.Update();

		g_UInputMan.Update();
		g_TimerMan.Update();
		g_TimerMan.UpdateSim();
		g_AudioMan.Update();
		g_MusicMan.Update();

		if (g_WindowMan.ResolutionChanged()) {
			g_MenuMan.Reinitialize();
			g_ConsoleMan.Destroy();
			g_ConsoleMan.Initialize();
			g_LoadingScreen.CreateLoadingSplash();
			g_WindowMan.CompleteResolutionChange();
		}

		if (g_MenuMan.Update()) {
			g_UInputMan.EndFrame();
			break;
		}

		g_ConsoleMan.Update();

		g_UInputMan.EndFrame();
		g_WindowMan.GetScreenBuffer()->Begin();
		g_MenuMan.Draw();
		g_ConsoleMan.Draw(g_FrameMan.GetBackBuffer32());
		g_WindowMan.GetScreenBuffer()->End();
		g_WindowMan.UploadFrame();

		if (!s_menuScriptPath.empty()) {
			ProcessMenuScript();
		}
	}

	g_MenuMan.SetIsInMenuScreen(false);
}

/// <summary>
/// Local-perspective result text for a finished network match.
/// </summary>
static std::string BuildNetMatchResultText() {
	const GameActivity* gameActivity = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
	const int winnerTeam = gameActivity ? gameActivity->GetWinnerTeam() : Activity::NoTeam;
	if (winnerTeam == Activity::NoTeam) {
		return "Match over: draw";
	}
	return winnerTeam == g_NetMatchService.GetLocalTeam() ? "Victory!" : "Defeat";
}

/// <summary>
/// Game simulation loop.
// CC_SIM_DUMP=<from>:<to> writes every MO's exact-bit state per tick beside the -out trace
// (".simdump.txt"), for host-vs-client divergence forensics.
static void DumpSimStateIfArmed(uint64_t simTick) {
	static uint64_t s_from = 1;
	static uint64_t s_to = 0;
	static std::ofstream s_out;
	static bool s_checked = false;
	if (!s_checked) {
		s_checked = true;
		const char* env = std::getenv("CC_SIM_DUMP");
		unsigned long long from = 0;
		unsigned long long to = 0;
		if (env && std::sscanf(env, "%llu:%llu", &from, &to) == 2 && to >= from) {
			const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
			s_out.open(base + ".simdump.txt", std::ios::trunc);
			if (s_out.is_open()) {
				s_from = from;
				s_to = to;
			}
		}
	}
	if (!s_out.is_open() || simTick < s_from || simTick > s_to) {
		return;
	}
	g_MovableMan.DumpSimState(simTick, s_out);
}

// CC_TERRAIN_DUMP=<tick> saves the material and FG color bitmaps beside the -out trace at that tick
// (and on a runtime Desync stop), for host-vs-client terrain-layer divergence forensics.
static void DumpTerrainNow(const std::string& suffix) {
	if (!g_SceneMan.GetScene() || !g_SceneMan.GetScene()->GetTerrain()) {
		return;
	}
	const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
	auto dumpRaw = [&](const char* name, BITMAP* bitmap) {
		std::ofstream out(base + "." + suffix + "." + name + ".bin", std::ios::binary | std::ios::trunc);
		const int32_t dims[2] = {bitmap->w, bitmap->h};
		out.write(reinterpret_cast<const char*>(dims), sizeof(dims));
		for (int y = 0; y < bitmap->h; ++y) {
			out.write(reinterpret_cast<const char*>(bitmap->line[y]), bitmap->w);
		}
	};
	dumpRaw("mat", g_SceneMan.GetScene()->GetTerrain()->GetMaterialBitmap());
	dumpRaw("fg", g_SceneMan.GetScene()->GetTerrain()->GetFGColorBitmap());
	std::cout << "[terrain-dump] " << suffix << " saved" << std::endl;
}

static bool TerrainDumpArmed() {
	static const bool s_armed = std::getenv("CC_TERRAIN_DUMP") != nullptr;
	return s_armed;
}

// CC_RNG_DRAW_TRACE=1 emits one "rng" tracer event per g_SimRNG draw inside the CC_TERRAIN_EVENTS
// window: the running draw index plus the travel context that consumed it.
static void InstallRNGDrawTraceIfArmed() {
	if (!std::getenv("CC_RNG_DRAW_TRACE")) {
		return;
	}
	g_RNGDrawHook = [](uint64_t drawCount) {
		SceneMan::TraceTerrainEvent("rng", static_cast<int>(drawCount & 0xFFFFFFFFu), static_cast<int>(drawCount >> 32), 0, 0, static_cast<int>(SceneMan::GetTerrainEventContext()));
	};
	g_SimRNG.SetDrawTraceEnabled(true);
}

// CC_TICK_PROBE=1 appends one counts+RNG line per tick beside the -out trace; light enough not to
// disturb the pacing the desync hunt depends on.
// CC_TICK_PROBE_BOX=<x1>:<y1>:<x2>:<y2> adds per-tick FNV hashes of the boxed terrain mat+fg bytes.
static uint64_t BoxedTerrainHash(BITMAP* bitmap, int x1, int y1, int x2, int y2) {
	uint64_t hash = 1469598103934665603ULL;
	if (!bitmap) {
		return hash;
	}
	x1 = std::max(0, x1);
	y1 = std::max(0, y1);
	x2 = std::min(bitmap->w - 1, x2);
	y2 = std::min(bitmap->h - 1, y2);
	for (int y = y1; y <= y2; ++y) {
		for (int x = x1; x <= x2; ++x) {
			hash = (hash ^ static_cast<uint64_t>(bitmap->line[y][x])) * 1099511628211ULL;
		}
	}
	return hash;
}

static void TickProbeIfArmed(uint64_t simTick) {
	static std::ofstream s_out;
	static int s_state = 0;
	static int s_box[4] = {0, 0, -1, -1};
	static bool s_boxArmed = false;
	if (s_state == 0) {
		s_state = std::getenv("CC_TICK_PROBE") ? 1 : -1;
		if (s_state == 1) {
			const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
			s_out.open(base + ".tickprobe.txt", std::ios::trunc);
			if (const char* boxEnv = std::getenv("CC_TICK_PROBE_BOX")) {
				s_boxArmed = std::sscanf(boxEnv, "%d:%d:%d:%d", &s_box[0], &s_box[1], &s_box[2], &s_box[3]) == 4;
			}
		}
	}
	if (s_state != 1 || !s_out.is_open()) {
		return;
	}
	const std::string rngState = g_SimRNG.SerializeStateForHashing();
	s_out << simTick << " a=" << g_MovableMan.GetActorCount() << " p=" << g_MovableMan.GetParticleCount() << " rng=" << std::hash<std::string>{}(rngState) << " draws=" << g_SimRNG.GetDrawCount();
	if (s_boxArmed && g_SceneMan.GetScene() && g_SceneMan.GetScene()->GetTerrain()) {
		s_out << " tm=" << std::hex
		      << BoxedTerrainHash(g_SceneMan.GetScene()->GetTerrain()->GetMaterialBitmap(), s_box[0], s_box[1], s_box[2], s_box[3])
		      << " tf=" << BoxedTerrainHash(g_SceneMan.GetScene()->GetTerrain()->GetFGColorBitmap(), s_box[0], s_box[1], s_box[2], s_box[3])
		      << std::dec;
	}
	s_out << "\n";
}

// CC_TRACK_UID=<uid>[,<uid>...] appends per-tick bit-exact pose/vel/rest rows for those MOs into the
// terrain event trace (tags trk/trk2); in-memory, so it keeps the pacing the desync hunt depends on.
static void TrackUidsIfArmed(uint64_t simTick) {
	static std::vector<long> s_uids;
	static int s_state = 0;
	if (s_state == 0) {
		s_state = -1;
		if (const char* env = std::getenv("CC_TRACK_UID")) {
			const std::string list(env);
			size_t start = 0;
			while (start < list.size()) {
				size_t end = list.find(',', start);
				if (end == std::string::npos) {
					end = list.size();
				}
				const long uid = std::strtol(list.substr(start, end - start).c_str(), nullptr, 10);
				if (uid > 0) {
					s_uids.push_back(uid);
				}
				start = end + 1;
			}
			if (!s_uids.empty()) {
				s_state = 1;
			}
		}
	}
	if (s_state != 1) {
		return;
	}
	// One FP-state row per tick: a driver flipping the FP control state (FTZ/DAZ) mid-run would fork denormal math.
#if defined(__aarch64__)
	uint64_t fpcr = 0;
	asm volatile("mrs %0, fpcr" : "=r"(fpcr));
	SceneMan::TraceTerrainEvent("fpu", static_cast<int32_t>(fpcr), 0, 0, 0, 0);
#elif defined(_MSC_VER)
	SceneMan::TraceTerrainEvent("fpu", static_cast<int32_t>(_mm_getcsr()), static_cast<int32_t>(_control87(0, 0)), 0, 0, 0);
#else
	SceneMan::TraceTerrainEvent("fpu", static_cast<int32_t>(_mm_getcsr()), 0, 0, 0, 0);
#endif
	for (long uid: s_uids) {
		const MovableObject* mo = g_MovableMan.FindObjectByUniqueID(uid);
		if (!mo) {
			continue;
		}
		const auto bits = [](float value) { return std::bit_cast<int32_t>(value); };
		SceneMan::TraceTerrainEvent("trk", bits(mo->GetPos().m_X), bits(mo->GetPos().m_Y), bits(mo->GetVel().m_X), bits(mo->GetVel().m_Y), static_cast<int>(uid));
		float rotAngle = 0.0F;
		float angVel = 0.0F;
		if (const MOSprite* sprite = dynamic_cast<const MOSprite*>(mo)) {
			rotAngle = sprite->GetRotAngle();
			angVel = sprite->GetAngularVel();
		}
		SceneMan::TraceTerrainEvent("trk2", bits(rotAngle), bits(angVel), static_cast<int>(mo->GetRestTimerElapsedSimMS()), mo->GetVelOscillations(), static_cast<int>(uid));
		const int flags = (mo->GetsHitByMOs() ? 1 : 0) | (mo->IgnoresAtomGroupHits() ? 2 : 0) | (mo->GetTraveling() ? 4 : 0) | (mo->ToSettle() ? 8 : 0) | (mo->ToDelete() ? 16 : 0) | (mo->HitsMOs() ? 32 : 0);
		SceneMan::TraceTerrainEvent("trk3", flags, bits(mo->GetPrevPos().m_X), bits(mo->GetPrevPos().m_Y), 0, static_cast<int>(uid));
	}
}

// One-shot per-MO state dump for the Desync stop; pacing-neutral, unlike the per-tick CC_SIM_DUMP.
static void DumpSimStateNow(const std::string& suffix) {
	const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
	std::ofstream out(base + "." + suffix + ".simstate.txt", std::ios::binary | std::ios::trunc);
	if (out.is_open()) {
		g_MovableMan.DumpSimState(g_TimerMan.GetSimUpdateCount(), out);
		std::cout << "[sim-dump] " << suffix << " saved" << std::endl;
	}
}

// The full per-MO dump as text; the fidelity probe compares this, not just the checksum.
static std::string DumpSimStateToString() {
	std::ostringstream out;
	g_MovableMan.DumpSimState(g_TimerMan.GetSimUpdateCount(), out);
	return out.str();
}

static void WriteProbeText(const std::string& suffix, const std::string& text) {
	const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
	std::ofstream out(base + "." + suffix + ".simstate.txt", std::ios::binary | std::ios::trunc);
	out << text;
}

// Compares the restored world's dump against the captured one; a mismatch is a fidelity hole the hash may miss.
static void CheckRestoredDeepState() {
	const std::string restored = DumpSimStateToString();
	WriteProbeText("rb_restored", restored);
	s_rbProbeRestoreMismatch = restored != s_rbProbeCapturedDeep;
	if (s_rbProbeRestoreMismatch) {
		WriteProbeText("rb_restored_" + std::to_string(s_rbProbeAtTick), restored);
		std::cout << "[rbprobe] RESTORE MISMATCH: the restored world's dump differs from the captured one (rb_captured vs rb_restored)" << std::endl;
	}
}

// Every capture takes the settle first, as CaptureWorld and SetAsideWorld do: a graph taken in front of it names the objects it sweeps.
static void SettleBeforeCapture() {
	g_LuaMan.CollectGarbageForCheckpoint();
}

// The probe's comparison capture; CheckRestoredScriptGraphs holds the result against the restored world.
static bool CaptureProbeScriptGraphs() {
	SettleBeforeCapture();
	std::vector<std::string> problems;
	if (!g_MovableMan.SerializeScriptGraphs(s_rbProbeLuaGraphsAtCapture, problems)) {
		for (const std::string& problem: problems) {
			std::cout << "[rbprobe] FAIL: script graph capture refused: " << problem << std::endl;
		}
		return false;
	}
	return true;
}

// The contract audit's observation is a capture too.
static bool ObserveScriptGraphs(std::vector<std::string>& graphs, std::vector<std::string>& problems) {
	SettleBeforeCapture();
	return g_MovableMan.SerializeScriptGraphs(graphs, problems);
}

// The restored Lua state must serialize exactly as the captured one did: the graph text is canonical.
static void CheckRestoredScriptGraphs() {
	std::vector<std::string> restored;
	std::vector<std::string> problems;
	if (!g_MovableMan.SerializeScriptGraphs(restored, problems)) {
		s_rbProbeRestoreMismatch = true;
		for (const std::string& problem: problems) {
			std::cout << "[rbprobe] RESTORE MISMATCH: the restored Lua state cannot be carried: " << problem << std::endl;
		}
	}
	if (!g_MovableMan.GetScriptGraphFailure().empty()) {
		s_rbProbeRestoreMismatch = true;
		std::cout << "[rbprobe] RESTORE MISMATCH: the set-aside could not carry the Lua state: " << g_MovableMan.GetScriptGraphFailure() << std::endl;
	}
	const size_t count = std::max(restored.size(), s_rbProbeLuaGraphsAtCapture.size());
	for (size_t i = 0; i < count; ++i) {
		const std::string& before = i < s_rbProbeLuaGraphsAtCapture.size() ? s_rbProbeLuaGraphsAtCapture[i] : std::string();
		const std::string& after = i < restored.size() ? restored[i] : std::string();
		if (before != after) {
			s_rbProbeRestoreMismatch = true;
			WriteProbeText("rb_luagraph_" + std::to_string(i) + "_capture", before);
			WriteProbeText("rb_luagraph_" + std::to_string(i) + "_restored", after);
			std::cout << "[rbprobe] RESTORE MISMATCH: Lua state " << i << " serializes differently after the restore (rb_luagraph_" << i << "_capture vs _restored)" << std::endl;
		}
	}
}

// Everything a preview may touch besides the MO dump: clocks, RNG, identity counter, queues, activity
// scalars, terrain layers and the Lua bindings. The camera and the previews themselves are the only
// presentation-side changes a preview is allowed to make.
static std::string DescribeCanonicalExtras(std::vector<std::string>& problems) {
	std::ostringstream out;
	out << "sim_count=" << g_TimerMan.GetSimUpdateCount() << " sim_ticks=" << g_TimerMan.GetSimTimeTicks() << " accumulator=" << g_TimerMan.GetSimAccumulator() << "\n";
	out << "rng_draws=" << g_SimRNG.GetDrawCount() << " rng_state=" << g_SimRNG.GetEngineState() << "\n";
	out << "uid_counter=" << MovableObject::GetUniqueIDCounter() << "\n";
	out << "lua_state_cursor=" << g_LuaMan.GetScriptStateCursor() << "\n";
	const MovableMan::AddQueueMark mark = g_MovableMan.MarkAddQueues();
	out << "queues actors=" << mark.actors << " items=" << mark.items << " particles=" << mark.particles << " alarms=" << mark.alarms << "\n";
	if (const Activity* activity = g_ActivityMan.GetActivity()) {
		Activity::RollbackState state;
		activity->CaptureRollbackState(state);
		out << "activity state=" << static_cast<int>(state.state);
		for (int team = 0; team < Activity::MaxTeamCount; ++team) {
			out << " t" << team << "=" << std::hexfloat << state.teamFunds[team] << std::defaultfloat << "/" << state.teamDeaths[team] << (state.teamActive[team] ? "a" : "");
		}
		out << "\n";
		out << "players";
		for (int player = 0; player < Players::MaxPlayerCount; ++player) {
			out << " p" << player << "=" << state.controlledActorUID[player] << "/" << state.brainUID[player] << (state.brainEvacuated[player] ? "e" : "");
		}
		out << "\n";
	}
	out << "rosters\n" << g_MovableMan.DescribeTeamRosters();
	out << "render_hidden=" << g_MovableMan.GetRenderHiddenCount() << " speculative=" << (g_MovableMan.IsSpeculative() ? 1 : 0) << " registry=" << g_MovableMan.GetKnownObjectsCount() << "\n";
	TerrainLayerSnapshot terrain;
	if (terrain.Capture()) {
		auto fnv = [](const std::vector<uint8_t>& bytes) {
			uint64_t h = 1469598103934665603ULL;
			for (const uint8_t b: bytes) {
				h = (h ^ b) * 1099511628211ULL;
			}
			return h;
		};
		out << "terrain mat=" << std::hex << fnv(terrain.mat) << " fg=" << fnv(terrain.fg) << " bg=" << fnv(terrain.bg) << std::dec << "\n";
	}
	out << "scripts\n" << g_MovableMan.DescribeScriptBindings();
	if (const Scene* scene = g_SceneMan.GetScene()) {
		auto stream = std::make_unique<std::stringstream>();
		std::stringstream* raw = stream.get();
		Writer writer(std::move(stream));
		for (const Scene::Area* area: scene->GetAreas()) {
			writer.NewProperty("Area");
			area->SaveSnapshot(writer);
			writer.ObjectEnd();
		}
		out << "scene_areas\n" << raw->str();
	}
	std::vector<std::string> graphs;
	g_MovableMan.SerializeScriptGraphs(graphs, problems);
	for (size_t index = 0; index < graphs.size(); ++index) {
		out << "lua_graph " << index << " " << graphs[index].size() << "\n" << graphs[index] << "\n";
	}
	return out.str();
}

// One rendered frame with the previews standing in for their actors, on the render RNG and off the MOID grid.
// The probe's save file is per process, so concurrent probes never read each other's world.
static std::string RollbackProbeSaveName() {
	return "rbprobe_" + std::to_string(System::GetProcessID());
}

static void DrawFrameWithPreviews() {
	RandomGenerator* prevSimRNG = t_simRNGOverride;
	t_simRNGOverride = &g_RenderRNG;
	g_SceneMan.SetRenderDrawContext(true);
	LocalPrediction::BeginRender();
	g_FrameMan.Draw();
	g_MenuMan.DrawNetworkUI();
	ScenarioRunner::DrawNetUiToasts();
	g_WindowMan.DrawPostProcessBuffer();
	g_WindowMan.UploadFrame();
	LocalPrediction::EndRender();
	g_SceneMan.SetRenderDrawContext(false);
	t_simRNGOverride = prevSimRNG;
	NetModerationGUIProbe::AfterDraw();
}

static void UpdateResyncUI(uint32_t elapsedSeconds) {
	PollSDLEvents();
	g_UInputMan.Update(false);
	if (g_UInputMan.KeyPressed(SDLK_F6) || (g_MenuMan.IsNetworkPanelOpen() && g_UInputMan.AnyStartPress(false))) {
		g_MenuMan.ToggleNetworkPanel();
	}
	g_MenuMan.UpdateNetworkUI();
	g_WindowMan.ClearBackbuffer();
	clear_to_color(g_FrameMan.GetBackBuffer32(), makeacol32(20, 22, 27, 255));
	AllegroBitmap bitmap(g_FrameMan.GetBackBuffer32());
	const int centerX = g_WindowMan.GetResX() / 2;
	const int centerY = g_WindowMan.GetResY() / 2;
	g_FrameMan.GetLargeFont(true)->DrawAligned(&bitmap, centerX, centerY - 12, "Resyncing the match...", GUIFont::Centre);
	g_FrameMan.GetSmallFont(true)->DrawAligned(&bitmap, centerX, centerY + 8,
	    std::to_string(elapsedSeconds) + "s elapsed  /  Seats [F6]", GUIFont::Centre);
	g_MenuMan.DrawNetworkUI();
	ScenarioRunner::DrawNetUiToasts();
	ScenarioRunner::NoteResyncOverlayFrame();
	g_WindowMan.UploadFrame();
	NetModerationGUIProbe::AfterDraw();
	g_UInputMan.EndFrame();
	g_UInputMan.EndSimUpdate();
}

// The previews' gameplay against -lpinv-expect. A preview that starts before the canonical pickup must
// have picked up once its horizon passes that tick (plus the slack of the reach ray's random cast) and
// never before it; one that starts after already holds the item and takes nothing. The shot likewise.
static std::string CheckPreviewOutcome(long long startTick, long long horizon) {
	const LocalPrediction::Outcome& outcome = LocalPrediction::GetLastOutcome();
	if (outcome.violations > 0) {
		return "the previews wrote to the world " + std::to_string(outcome.violations) + " time(s)";
	}
	const bool holdsExpected = !s_lpExpectEquip.empty() && outcome.equipped == s_lpExpectEquip;
	const bool pickedUp = holdsExpected && outcome.taken == 1;
	if (!s_lpExpectEquip.empty()) {
		if (startTick >= s_lpExpectEquipTick) {
			if (outcome.taken != 0 || !holdsExpected) {
				return "after the canonical pickup the preview should hold '" + s_lpExpectEquip + "' and take nothing, but it took " + std::to_string(outcome.taken) + " and holds '" + outcome.equipped + "'";
			}
		} else if (horizon < s_lpExpectEquipTick) {
			if (outcome.taken != 0) {
				return "a pickup before its tick " + std::to_string(s_lpExpectEquipTick) + " (horizon " + std::to_string(horizon) + ", took " + std::to_string(outcome.taken) + ")";
			}
		} else if (horizon >= s_lpExpectEquipTick + s_lpExpectEquipSlack) {
			if (!pickedUp) {
				return "expected the pickup of '" + s_lpExpectEquip + "' by tick " + std::to_string(horizon) + " but the preview took " + std::to_string(outcome.taken) + " resident(s) and holds '" + outcome.equipped + "'";
			}
		} else if (outcome.taken > 1) {
			return "the pickup took " + std::to_string(outcome.taken) + " residents, expected at most 1";
		}
	}
	if (s_lpExpectFireTick > 0 && startTick < s_lpExpectFireTick) {
		const bool fired = holdsExpected && (outcome.firedOnce || (outcome.takenRounds >= 0 && outcome.roundsInMag >= 0 && outcome.roundsInMag < outcome.takenRounds));
		if (horizon < s_lpExpectFireTick) {
			if (fired) {
				return "a shot before its tick " + std::to_string(s_lpExpectFireTick) + " (horizon " + std::to_string(horizon) + ")";
			}
		} else if (horizon >= s_lpExpectFireTick + s_lpExpectFireSlack && !fired) {
			return "expected a shot from '" + s_lpExpectEquip + "' by tick " + std::to_string(horizon) + " but rounds went " + std::to_string(outcome.takenRounds) + " -> " + std::to_string(outcome.roundsInMag) + " and fired_once=" + std::to_string(outcome.firedOnce ? 1 : 0);
		}
	}
	return "";
}

// -local-prediction-invariance: at tick T, run and discard previews of every depth and repeat count and
// require the canonical world (dump + extras) byte-identical afterwards. The run then continues, so the
// trace compare against a no-preview reference closes the resume half of the guarantee.
static void LocalPredictionInvarianceOnTick(uint64_t simTick) {
	if (s_lpInvarianceTick <= 0 || simTick != static_cast<uint64_t>(s_lpInvarianceTick)) {
		return;
	}
	const int savedDepth = LocalPrediction::GetDepthOverride();
	g_MovableMan.WaitForActorsSeeTask();
	g_MovableMan.CompleteQueuedMOIDDrawings();
	std::vector<std::string> problems;
	const std::string before = DumpSimStateToString() + DescribeCanonicalExtras(problems);
	if (!problems.empty()) {
		for (const std::string& problem: problems) {
			std::cout << "[lpinv] FAIL: cannot capture canonical Lua state: " << problem << std::endl;
		}
		s_lpInvarianceFailures = 1;
		s_netReplayExitCode = 5;
		g_MetricsCollector.RecordString("lpinv_result", "fail");
		return;
	}
	WriteProbeText("lpinv_before", before);
	int failures = 0;
	int cases = 0;
	for (const int depth: s_lpInvarianceDepths) {
		for (const int repeats: s_lpInvarianceRepeats) {
			const std::string label = "depth " + std::to_string(depth) + " x" + std::to_string(repeats);
			bool caseFailed = false;
			const auto fail = [&caseFailed, &label](const std::string& what) {
				caseFailed = true;
				std::cout << "[lpinv] FAIL " << label << ": " << what << std::endl;
			};
			LocalPrediction::SetDepthOverride(depth);
			const uint64_t previewsBefore = LocalPrediction::GetPreviewCount();
			std::string outcomeFailure;
			for (int n = 0; n < repeats; ++n) {
				LocalPrediction::Clear();
				LocalPrediction::RunPreview();
				// A real frame: the substitution, the HUD and the hidden residents all go through the draw.
				DrawFrameWithPreviews();
				if (const std::string failure = CheckPreviewOutcome(static_cast<long long>(simTick), static_cast<long long>(simTick) + depth); !failure.empty() && outcomeFailure.empty()) {
					outcomeFailure = failure;
				}
			}
			const uint64_t previewsRun = LocalPrediction::GetPreviewCount() - previewsBefore;
			const std::string outcome = LocalPrediction::DescribeLastOutcome();
			if (FaultInjected("preview_mutate_canonical") && depth == s_lpInvarianceDepths.front() && repeats == s_lpInvarianceRepeats.front()) {
				if (Actor* victim = g_MovableMan.GetFirstBrainActor(0)) {
					victim->SetVel(victim->GetVel() + Vector(0.001F, 0.0F));
				}
			}
			if (FaultInjected("preview_mutate_lua") && depth == s_lpInvarianceDepths.front() && repeats == s_lpInvarianceRepeats.front()) {
				g_LuaMan.GetMasterScriptState().RunScriptString("_InvarianceFault = { changed = true }");
			}
			LocalPrediction::Clear();
			problems.clear();
			const std::string after = DumpSimStateToString() + DescribeCanonicalExtras(problems);
			++cases;
			for (const std::string& problem: problems) {
				fail("cannot capture canonical Lua state: " + problem);
			}
			if (previewsRun != static_cast<uint64_t>(repeats)) {
				fail(std::to_string(previewsRun) + " previews ran, expected " + std::to_string(repeats) + " (no local actor to preview?)");
			}
			if (!outcomeFailure.empty()) {
				fail(outcomeFailure + " [" + outcome + "]");
			}
			if (after != before) {
				WriteProbeText("lpinv_after_d" + std::to_string(depth) + "_x" + std::to_string(repeats), after);
				fail("canonical state changed after discarded previews (lpinv_before vs lpinv_after_d" + std::to_string(depth) + "_x" + std::to_string(repeats) + ")");
			}
			if (caseFailed) {
				++failures;
			} else {
				std::cout << "[lpinv] ok " << label << ": " << previewsRun << " previews, " << outcome << ", canonical state byte-identical" << std::endl;
			}
		}
	}
	LocalPrediction::SetDepthOverride(savedDepth);
	s_lpInvarianceFailures = failures;
	std::cout << "[lpinv] " << (failures == 0 ? "PASS" : "FAIL") << " tick " << simTick << ": " << (cases - failures) << "/" << cases << " cases left the canonical world untouched" << std::endl;
	g_MetricsCollector.RecordString("lpinv_result", failures == 0 ? "pass" : "fail");
	g_MetricsCollector.Record("lpinv_cases", cases);
	g_MetricsCollector.Record("lpinv_failures", failures);
	if (failures > 0) {
		s_netReplayExitCode = 5;
	}
}

// A requested test that never reached its tick is a failed test; stopping early cannot pass it.
static void CheckRequiredProbesCompleted() {
	const long long stoppedAt = g_TimerMan.GetSimUpdateCount();
	if (s_lpInvarianceTick > 0 && s_lpInvarianceFailures < 0) {
		std::cout << "[lpinv] FAIL: invariance test at tick " << s_lpInvarianceTick << " never executed (the run stopped at tick " << stoppedAt << ")" << std::endl;
		g_MetricsCollector.RecordString("lpinv_result", "not_run");
		s_netReplayExitCode = 5;
	}
	if (s_rbProbeRequested && s_rbProbePhase != 4) {
		std::cout << "[rbprobe] FIDELITY FAIL: the probe at tick " << s_rbProbeAtTick << " did not complete (phase " << s_rbProbePhase << " when the run stopped at tick " << stoppedAt << ")" << std::endl;
		g_MetricsCollector.RecordString("rbprobe_result", "incomplete");
		if (s_netReplayExitCode == 0) {
			s_netReplayExitCode = 1;
		}
	}
}

static void DumpTerrainIfArmed(uint64_t simTick) {
	static uint64_t s_tick = 0;
	static bool s_checked = false;
	if (!s_checked) {
		s_checked = true;
		if (const char* env = std::getenv("CC_TERRAIN_DUMP")) {
			s_tick = std::strtoull(env, nullptr, 10);
		}
	}
	if (s_tick == 0 || simTick != s_tick) {
		return;
	}
	DumpTerrainNow("t" + std::to_string(simTick));
}

// Every object registered at the capture that is still registered afterwards must hold the same Lua object.
static bool LuaIdentityPreserved(const std::string& atCapture, const std::string& now) {
	std::map<std::string, std::string> before;
	std::istringstream in(atCapture);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string state;
		words >> state;
		std::string entry;
		while (words >> entry) {
			before[state + ":" + entry.substr(0, entry.find('@'))] = entry;
		}
	}
	std::istringstream after(now);
	while (std::getline(after, line)) {
		std::istringstream words(line);
		std::string state;
		words >> state;
		std::string entry;
		while (words >> entry) {
			const auto it = before.find(state + ":" + entry.substr(0, entry.find('@')));
			if (it != before.end() && it->second != entry) {
				return false;
			}
		}
	}
	return true;
}

// The harness's own captures, checked the way the engine's are: a root that has lost its last reference
// must not survive into a capture the settle is about to sweep.
static bool RunHarnessCaptureSelfTest() {
	// A Lua-owned scripted object with its last reference dropped and nothing collected yet.
	const auto park = [](long& uid) -> MovableObject* {
		LuaStateWrapper& master = g_LuaMan.GetMasterScriptState();
		const long first = MovableObject::GetUniqueIDCounter();
		if (master.RunScriptString("_HarnessCaptureParked = CreateMOPixel(\"Spark Yellow 1\", \"Base.rte\")") != 0) {
			return nullptr;
		}
		MovableObject* parked = nullptr;
		for (long candidate = first; candidate <= MovableObject::GetUniqueIDCounter() && !parked; ++candidate) {
			parked = g_MovableMan.FindObjectByUniqueID(candidate);
			uid = candidate;
		}
		if (parked) {
			// Registered with initialized scripts is what makes it a graph root.
			parked->MoveScriptsToState(master);
			parked->AdoptScriptObject();
		}
		master.RunScriptString("_HarnessCaptureParked = nil");
		return parked;
	};
	const auto settleThroughAHold = []() {
		MovableMan::WorldSetAside aside;
		return g_MovableMan.SetAsideWorld(aside, false) && g_MovableMan.ReinstateWorld(aside);
	};
	bool probeOrder = false;
	long probeUID = 0;
	if (park(probeUID) && probeUID > 0) {
		const std::vector<std::string> heldCapture = s_rbProbeLuaGraphsAtCapture;
		const bool heldMismatch = s_rbProbeRestoreMismatch;
		s_rbProbeRestoreMismatch = false;
		const bool captured = CaptureProbeScriptGraphs();
		const bool settled = settleThroughAHold();
		CheckRestoredScriptGraphs();
		const bool swept = g_MovableMan.FindObjectByUniqueID(probeUID) == nullptr;
		probeOrder = captured && settled && swept && !s_rbProbeRestoreMismatch;
		std::cout << "[harness-order] probe uid=" << probeUID << " captured=" << captured << " settled=" << settled
		          << " swept=" << swept << " mismatch=" << s_rbProbeRestoreMismatch << std::endl;
		s_rbProbeLuaGraphsAtCapture = heldCapture;
		s_rbProbeRestoreMismatch = heldMismatch;
	}
	std::cout << "[script-graph-selftest] " << (probeOrder ? "PASS" : "FAIL") << " rollback_probe_capture_settles_first" << std::endl;
	bool observeOrder = false;
	long observeUID = 0;
	if (park(observeUID) && observeUID > 0) {
		std::vector<std::string> before, after, problems;
		const bool first = ObserveScriptGraphs(before, problems);
		const bool settled = settleThroughAHold();
		const bool second = ObserveScriptGraphs(after, problems);
		const bool swept = g_MovableMan.FindObjectByUniqueID(observeUID) == nullptr;
		observeOrder = first && settled && second && swept && before == after;
		std::cout << "[harness-order] observe uid=" << observeUID << " swept=" << swept
		          << " graphs_equal=" << (before == after) << std::endl;
	}
	std::cout << "[script-graph-selftest] " << (observeOrder ? "PASS" : "FAIL") << " contract_audit_observation_settles_first" << std::endl;
	// Last in the run: a build that fails this one leaves a world held.
	bool refusalKeepsTheNextHold = false;
	{
		bool refused = false;
		bool stillHeld = false;
		{
			// A caller that gives up on a refused reinstate, exactly as RestartActivity's local record does.
			MovableMan::WorldSetAside aside;
			if (g_MovableMan.SetAsideWorld(aside, false)) {
				aside.runtimeGlobals = "not a runtime globals archive";
				refused = !g_MovableMan.ReinstateWorld(aside);
				stillHeld = aside.held && g_MovableMan.HasWorldSetAside();
			}
		}
		MovableMan::WorldSetAside next;
		const bool nextHold = g_MovableMan.SetAsideWorld(next, false);
		refusalKeepsTheNextHold = refused && stillHeld && nextHold && g_MovableMan.ReinstateWorld(next) && !g_MovableMan.HasWorldSetAside();
		std::cout << "[setaside-latch] refused=" << refused << " still_held=" << stillHeld << " next_hold=" << nextHold << std::endl;
	}
	std::cout << "[script-graph-selftest] " << (refusalKeepsTheNextHold ? "PASS" : "FAIL") << " refused_reinstate_leaves_the_next_hold_possible" << std::endl;
	return probeOrder && observeOrder && refusalKeepsTheNextHold;
}

// Observes completed tick boundaries. All state restoration belongs to the production
// checkpoint APIs; the probe only rewinds its recorded input stream and compares observations.
void RollbackProbeOnHashedTick(uint64_t simTick, const SimChecksum::Result& tickResult) {
	if (s_rbProbeFuzzCount > 0 && s_rbProbeSchedule.empty() && s_rbProbeAtTick <= 0 && s_rbProbePhase == 0) {
		// Lay out the random capture ticks once the run's cap is known; cycles never overlap.
		const long long maxTick = static_cast<long long>(ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 1800);
		const long long first = std::max<long long>(40, static_cast<long long>(simTick) + 2);
		const long long last = maxTick - s_rbProbeWindow - 4;
		std::mt19937_64 rng(s_rbProbeFuzzSeed);
		std::vector<long long> picks;
		for (int n = 0; n < s_rbProbeFuzzCount && last > first; ++n) {
			picks.push_back(first + static_cast<long long>(rng() % static_cast<uint64_t>(last - first + 1)));
		}
		std::sort(picks.begin(), picks.end());
		long long floor = first;
		for (long long pick: picks) {
			const long long tick = std::max(pick, floor);
			if (tick > last) {
				break;
			}
			s_rbProbeSchedule.push_back(tick);
			floor = tick + s_rbProbeWindow + 3;
		}
		if (s_rbProbeSchedule.empty()) {
			std::cout << "[rbfuzz] no room for probes under the tick cap" << std::endl;
			s_rbProbeFuzzCount = 0;
			return;
		}
		s_rbProbeAtTick = s_rbProbeSchedule.front();
		s_rbProbeSchedule.pop_front();
		std::cout << "[rbfuzz] " << (s_rbProbeSchedule.size() + 1) << " probes scheduled, window " << s_rbProbeWindow << ", first at " << s_rbProbeAtTick << std::endl;
	}
	if (s_rbProbePhase == 0 && simTick == static_cast<uint64_t>(s_rbProbeAtTick)) {
		const auto captureStart = std::chrono::steady_clock::now();
		double worldCaptureMs = 0.0;
		if (!CaptureProbeScriptGraphs()) {
			System::SetQuit(true);
			return;
		}
		if (s_rbProbeInMemory) {
			const auto worldStart = std::chrono::steady_clock::now();
			if (!g_MovableMan.CaptureWorld(s_rbProbeWorld)) {
				std::cout << "[rbprobe] FAIL: world capture refused (add queues not drained)" << std::endl;
				System::SetQuit(true);
				return;
			}
			worldCaptureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - worldStart).count();
		} else if (!g_ActivityMan.SaveCurrentGame(RollbackProbeSaveName())) {
			std::cout << "[rbprobe] FAIL: the capture save was refused" << std::endl;
			System::SetQuit(true);
			return;
		}
		s_rbProbeSimCount = g_TimerMan.GetSimUpdateCount();
		s_rbProbeSimTimeTicks = g_TimerMan.GetSimTimeTicks();
		if (ScenarioRunner::IsLockstepReplayPlayback()) {
			ScenarioRunner::ArmReplayRewindBuffer(simTick + 1, static_cast<uint64_t>(s_rbProbeWindow));
		}
		const double captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count();
		std::cout << "[rbprobe] capture_ms=" << captureMs << " world_ms=" << worldCaptureMs << " mos=" << (s_rbProbeWorld.actors.size() + s_rbProbeWorld.items.size() + s_rbProbeWorld.particles.size()) << std::endl;
		if (FaultInjected("snapshot_skew") && !s_rbProbeWorld.actors.empty()) {
			// A deliberately wrong captured field: the restore must be caught by the deep compare.
			Actor* skewed = s_rbProbeWorld.actors.front();
			skewed->SetVel(skewed->GetVel() + Vector(0.001F, 0.0F));
			std::cout << "[rbprobe] fault injected: snapshot_skew on uid " << skewed->GetUniqueID() << std::endl;
		}
		s_rbProbeCapturedDeep = DumpSimStateToString();
		s_rbProbeLuaIdentityAtCapture = g_MovableMan.DescribeLuaIdentity();
		WriteProbeText("rb_captured", s_rbProbeCapturedDeep);
		WriteProbeText("rb_captured_" + std::to_string(simTick), s_rbProbeCapturedDeep);
		s_rbProbeFirstDeep.clear();
		s_rbProbeDeepDivergence = -1;
		s_rbProbeRestoreMismatch = false;
		DumpTerrainNow("rb_cap");
		s_rbProbePhase = 1;
		std::cout << "[rbprobe] captured at tick " << simTick << std::endl;
	} else if (s_rbProbePhase == 1 && simTick > static_cast<uint64_t>(s_rbProbeAtTick)) {
		s_rbProbeFirst.push_back(tickResult);
		s_rbProbeFirstDeep.push_back(DumpSimStateToString());
		if (static_cast<long long>(s_rbProbeFirst.size()) >= s_rbProbeWindow) {
			if (!s_rbProbeInMemory && !g_ActivityMan.WaitForSaveGameTask()) {
				std::cout << "[rbprobe] FAIL: the capture save did not complete" << std::endl;
				System::SetQuit(true);
				return;
			}
			if (s_rbProbeInMemory) {
				s_rbProbeMemoryRestorePending = true;
			} else if (s_rbProbeUseLoadGame) {
				s_rbProbeLaunchRestorePending = true;
			} else if (!g_ActivityMan.LoadGameToRestart(RollbackProbeSaveName())) {
				std::cout << "[rbprobe] FAIL: the restore load was refused" << std::endl;
				System::SetQuit(true);
				return;
			} else {
				g_ActivityMan.RemoveSavedGame(RollbackProbeSaveName());
			}
			s_rbProbePhase = 2;
			std::cout << "[rbprobe] window recorded; restore staged" << std::endl;
		}
	} else if (s_rbProbePhase == 3 && simTick > static_cast<uint64_t>(s_rbProbeAtTick)) {
		s_rbProbeSecond.push_back(tickResult);
		if (s_rbProbeDeepDivergence < 0 && s_rbProbeSecond.size() <= s_rbProbeFirstDeep.size()) {
			const std::string secondDeep = DumpSimStateToString();
			const std::string& firstDeep = s_rbProbeFirstDeep[s_rbProbeSecond.size() - 1];
			if (secondDeep != firstDeep) {
				s_rbProbeDeepDivergence = s_rbProbeAtTick + static_cast<long long>(s_rbProbeSecond.size());
				WriteProbeText("rb_deep_pass1", firstDeep);
				WriteProbeText("rb_deep_pass2", secondDeep);
				WriteProbeText("rb_deep_" + std::to_string(s_rbProbeAtTick) + "_pass1", firstDeep);
				WriteProbeText("rb_deep_" + std::to_string(s_rbProbeAtTick) + "_pass2", secondDeep);
			}
		}
		if (static_cast<long long>(s_rbProbeSecond.size()) >= s_rbProbeWindow) {
			long long firstDivergence = -1;
			size_t divergentIndex = 0;
			for (size_t i = 0; i < s_rbProbeFirst.size(); ++i) {
				if (SimChecksum::SimGatedHash(s_rbProbeFirst[i]) != SimChecksum::SimGatedHash(s_rbProbeSecond[i])) {
					firstDivergence = s_rbProbeAtTick + 1 + static_cast<long long>(i);
					divergentIndex = i;
					break;
				}
			}
			if (s_rbProbeInMemory) {
				// The re-run is discarded; the run continues on the originals, whose Lua objects never left.
				if (!g_MovableMan.ReinstateWorld(s_rbProbeOriginals)) {
					s_rbProbeRestoreMismatch = true;
					System::SetQuit(true);
				}
				const std::string identityNow = g_MovableMan.DescribeLuaIdentity();
				if (!LuaIdentityPreserved(s_rbProbeLuaIdentityAtCapture, identityNow)) {
					s_rbProbeRestoreMismatch = true;
					WriteProbeText("rb_lua_identity_capture", s_rbProbeLuaIdentityAtCapture);
					WriteProbeText("rb_lua_identity_after", identityNow);
					std::cout << "[rbprobe] FIDELITY FAIL: a Lua object identity changed across the probe (capture " << s_rbProbeAtTick << ")" << std::endl;
				}
			}
			if (firstDivergence < 0 && s_rbProbeDeepDivergence < 0 && !s_rbProbeRestoreMismatch) {
				++s_rbProbePassCount;
				std::cout << "[rbprobe] FIDELITY PASS: " << s_rbProbeWindow << " ticks byte-identical after the restore, hash and full dump (capture " << s_rbProbeAtTick << ")" << std::endl;
			} else if (firstDivergence < 0) {
				++s_rbProbeFailCount;
				const std::string where = s_rbProbeRestoreMismatch ? "restore mismatch at capture " + std::to_string(s_rbProbeAtTick) : "dump divergence at tick " + std::to_string(s_rbProbeDeepDivergence);
				std::cout << "[rbprobe] FIDELITY FAIL: hashes identical but " << where << " (capture " << s_rbProbeAtTick << ")" << std::endl;
				if (s_rbProbeFirstFailure.empty()) {
					s_rbProbeFirstFailure = "capture " + std::to_string(s_rbProbeAtTick) + ": " + where;
				}
			} else {
				++s_rbProbeFailCount;
				std::string divergentSubsystems;
				for (const auto& [name, hash]: s_rbProbeFirst[divergentIndex].per_subsystem) {
					if (name == "controller") {
						continue;
					}
					const auto secondIt = s_rbProbeSecond[divergentIndex].per_subsystem.find(name);
					if (secondIt == s_rbProbeSecond[divergentIndex].per_subsystem.end() || secondIt->second != hash) {
						divergentSubsystems += (divergentSubsystems.empty() ? "" : ",") + name;
					}
				}
				std::cout << "[rbprobe] FIDELITY FAIL: first divergence at tick " << firstDivergence
				          << " subsystems=" << divergentSubsystems << " dump=" << (s_rbProbeRestoreMismatch ? std::string("restore mismatch") : std::to_string(s_rbProbeDeepDivergence)) << " (capture " << s_rbProbeAtTick << ")" << std::endl;
				if (s_rbProbeFirstFailure.empty()) {
					s_rbProbeFirstFailure = "capture " + std::to_string(s_rbProbeAtTick) + " diverged at " + std::to_string(firstDivergence) + " [" + divergentSubsystems + "]";
				}
			}
			// Both passes' tracked-UID rows are in the tracer buffer; flush them for the fidelity diff.
			SceneMan::FlushTerrainEvents((!ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim")) + ".rbprobe.terrainevents.txt");
			s_rbProbeFirst.clear();
			s_rbProbeSecond.clear();
			s_rbProbeFirstDeep.clear();
			if (!s_rbProbeSchedule.empty()) {
				s_rbProbeAtTick = s_rbProbeSchedule.front();
				s_rbProbeSchedule.pop_front();
				s_rbProbePhase = 0;
				return;
			}
			s_rbProbePhase = 4;
			g_MetricsCollector.RecordString("rbprobe_result", s_rbProbeFailCount == 0 ? "pass" : "fail");
			if (s_rbProbeFuzzCount > 0) {
				std::cout << "[rbfuzz] " << (s_rbProbeFailCount == 0 ? "PASS" : "FAIL") << " " << s_rbProbePassCount << "/" << (s_rbProbePassCount + s_rbProbeFailCount) << " probes byte-identical";
				if (s_rbProbeFailCount > 0) {
					std::cout << "; first failure: " << s_rbProbeFirstFailure;
				}
				std::cout << std::endl;
			}
			if (s_rbProbeFailCount > 0) {
				s_netReplayExitCode = 1;
				System::SetQuit(true);
			}
		}
	}
}

/// </summary>
static bool IsFirstE2ERematchReady() {
	const Activity* activity = g_ActivityMan.GetActivity();
	return s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestRematch && s_netMatchServiceE2ERematches == 0 &&
	       activity && activity->IsOver() && s_netMatchE2ETicks.Total() >= 100;
}

static void HandleControllerReplayFailure(bool& returnToMenuAfterNetworkEnd) {
	const std::string error = ScenarioRunner::GetControllerReplayError();
	if (error.find("Desync") != std::string::npos) {
		if (TerrainDumpArmed()) {
			DumpTerrainNow("desync");
			DumpSimStateNow("desync");
		}
		const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
		SceneMan::FlushTerrainEvents(base + ".desync.terrainevents.txt");
	}
	if (ScenarioRunner::IsActive()) {
		std::cerr << "[scenario] controller replay failed: " << error << std::endl;
		System::SetQuit(true);
	} else if (!s_netReplayInPath.empty()) {
		// Playback ends when the recording's marker does; every other stop is a distinct, named failure.
		s_netReplayTicks = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
		using Outcome = ScenarioRunner::LockstepReplayOutcome;
		Outcome outcome = ScenarioRunner::GetLockstepReplayOutcome();
		if (outcome == Outcome::Playing || outcome == Outcome::None) {
			outcome = Outcome::SimFailure;
			ScenarioRunner::SetLockstepReplayOutcome(outcome);
		}
		if (outcome != Outcome::Completed) {
			std::cerr << "[net-replay] playback stopped: " << ScenarioRunner::ReplayOutcomeName(outcome) << ": " << error << std::endl;
			s_netReplayExitCode = outcome == Outcome::Truncated ? 2 : (outcome == Outcome::Corrupt ? 3 : 4);
		}
		g_ActivityMan.EndActivity();
		ScenarioRunner::ClearControllerReplayError();
		System::SetQuit(true);
	} else {
		const uint64_t e2eTickCap = s_netLockstepTicks > 0 ? s_netLockstepTicks : 600;
		const uint64_t matchTick = ParseLockstepStopTick(error, static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
		const bool e2eReachedCap = s_netMatchServiceE2E && NetMatchE2EReachedCap(s_netMatchE2ETicks.Total(), matchTick, e2eTickCap);
		const bool e2ePeerStoppedAfterCap = e2eReachedCap &&
			(error.find("Complete:") != std::string::npos ||
			 error.find("MissingFrameTimeout") != std::string::npos ||
			 error.find("PeerDisconnected") != std::string::npos);
		if (!s_netMatchServiceE2E && s_recordTickHashes && g_NetMatchService.WasEverStarted()) {
			const uint64_t cap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 600;
			if (g_MetricsCollector.GetTickHashCount() < cap || error.find("Complete:") == std::string::npos) {
				s_menuMpTraceError = error;
				std::cerr << "[menu-mp] trace stopped: " << error << std::endl;
			}
			g_ActivityMan.EndActivity();
			ScenarioRunner::ClearControllerReplayError();
			System::SetQuit(true);
		} else if (s_netMatchServiceE2E && e2ePeerStoppedAfterCap) {
			g_NetMatchService.Complete("e2e complete");
			g_ActivityMan.EndActivity();
			ScenarioRunner::ClearControllerReplayError();
			System::SetQuit(true);
		} else if (error.find("PeerLeft:") != std::string::npos && g_NetMatchService.GetState() == NetMatchServiceState::Running) {
			// The last peer announced its leave, so the match is over rather than broken: it ends the
			// way a finished one does, which keeps the seats and the admission counters in the report.
			const Activity* leftActivity = g_ActivityMan.GetActivity();
			const std::string result = (leftActivity && leftActivity->IsOver()) ? BuildNetMatchResultText() : "The other player left the match";
			g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
			g_NetMatchService.FinishMatch(result);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			if (s_netMatchServiceE2E) {
				System::SetQuit(true);
			} else {
				returnToMenuAfterNetworkEnd = true;
			}
		} else if (!s_netMatchServiceE2E && error.find("Complete:") != std::string::npos && g_NetMatchService.GetState() == NetMatchServiceState::Running) {
			// The peer finished cleanly a beat ahead of us; mirror the clean end, not an error.
			// If our activity is not over, they left mid-match rather than finishing it.
			const Activity* skewActivity = g_ActivityMan.GetActivity();
			const std::string result = (skewActivity && skewActivity->IsOver()) ? BuildNetMatchResultText() : "The other player left the match";
			g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
			g_NetMatchService.FinishMatch(result);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			returnToMenuAfterNetworkEnd = true;
		} else if (error.find("Complete:") != std::string::npos &&
		           error.find("e2e complete") == std::string::npos &&
		           (g_NetMatchService.GetState() == NetMatchServiceState::Completed ||
		            error.find("match over") != std::string::npos)) {
			if (g_NetMatchService.GetState() == NetMatchServiceState::Running) {
				g_NetMatchService.FinishMatch(BuildNetMatchResultText());
			}
			g_ActivityMan.EndActivity();
			ScenarioRunner::ClearControllerReplayError();
			if (s_netMatchServiceE2E) {
				System::SetQuit(true);
			} else {
				returnToMenuAfterNetworkEnd = true;
			}
		} else if ((error.find("Desync") != std::string::npos || error.find("ResyncRequested") != std::string::npos) &&
		           g_NetMatchService.IsResyncOnDesyncEnabled() && s_netMatchResyncs < 3 &&
		           g_NetMatchService.GetState() == NetMatchServiceState::Running) {
			// A desync (or a host-requested resync, e.g. a rejoin) heals in place: the host
			// snapshots its state, every peer reloads the identical file, the match plays on.
			++s_netMatchResyncs;
			g_ConsoleMan.PrintString("NETWORK: Resyncing from the host (" + std::to_string(s_netMatchResyncs) + "): " + error);
			std::cout << "[net-match] resync: " << (error.find("ResyncRequested") != std::string::npos ? "requested" : "desync detected") << ", reloading from the host snapshot" << std::endl;
			ScenarioRunner::PushNetUiToast("resync_start", "Resyncing the match...");
			ScenarioRunner::ClearControllerReplayError();
			std::string resyncError;
			bool resyncOk = g_NetMatchService.ResyncMatch(&resyncError);
			if (resyncOk) {
				std::string launchPreset;
				const auto resyncWaitStart = std::chrono::steady_clock::now();
				while (!g_NetMatchService.ConsumeReadyToLaunch(launchPreset)) {
					UpdateResyncUI(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - resyncWaitStart).count()));
					if (System::IsSetToQuit()) {
						resyncError = "quit requested during resync";
						resyncOk = false;
						break;
					}
					if (g_NetMatchService.GetState() == NetMatchServiceState::Failed) {
						resyncError = g_NetMatchService.GetErrorText();
						resyncOk = false;
						break;
					}
					if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - resyncWaitStart).count() > 60) {
						resyncError = "timed out waiting for the resync round";
						resyncOk = false;
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			}
			if (resyncOk) {
				resyncOk = StageResyncedMatchActivity(&resyncError);
			}
			if (resyncOk) {
				g_TimerMan.PauseSim(true);
				if (!g_ActivityMan.RestartActivity()) {
					resyncError = "resync activity restart failed";
					resyncOk = false;
				}
			}
			if (resyncOk) {
				std::cout << "[net-match] resync: match relaunched from the snapshot" << std::endl;
				// The relaunch drops the queue; the heal toast reports the frame it landed on.
				ScenarioRunner::ClearNetUiToasts();
				ScenarioRunner::PushNetUiToast("resync_finish", "Match resynced (healed at frame " + std::to_string(ScenarioRunner::GetLockstepAppliedFrame()) + ")");
				if (s_netMatchServiceE2E) {
					s_netMatchE2ETicks.OnResyncRelaunch();
				}
			} else if (resyncError == "match over") {
				g_NetMatchService.FinishMatch("match over");
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				ScenarioRunner::ClearControllerReplayError();
				if (s_netMatchServiceE2E) {
					System::SetQuit(true);
				} else {
					returnToMenuAfterNetworkEnd = true;
				}
			} else {
				std::cerr << "[net-match] resync failed: " << resyncError << std::endl;
				g_ConsoleMan.PrintString("NETWORK: Resync failed: " + resyncError);
				g_NetMatchService.ReportRuntimeError("resync failed: " + resyncError);
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				if (s_netMatchServiceE2E) {
					s_netMatchServiceE2EError = "resync failed: " + resyncError;
					s_netMatchServiceE2EExitCode = 1;
					System::SetQuit(true);
				} else {
					returnToMenuAfterNetworkEnd = true;
				}
			}
		} else {
			std::cerr << "[net-match] controller sync failed: " << error << std::endl;
			g_ConsoleMan.PrintString("NETWORK: Match stopped: " + error);
			g_ConsoleMan.SetEnabled(true);
			g_NetMatchService.ReportRuntimeError(error);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			if (s_netMatchServiceE2E) {
				s_netMatchServiceE2EError = error;
				s_netMatchServiceE2EExitCode = 1;
				System::SetQuit(true);
			} else {
				returnToMenuAfterNetworkEnd = true;
			}
		}
	}
}

/// A launch that fails leaves no scene, so the loop below has nothing to run against - a resync whose
/// snapshot will not apply lands exactly here. The recovery text rides the service, so §11's state
/// machine says why the seat was lost.
/// @return Whether the game loop may still be entered.
static bool HandleFailedActivityLaunch() {
	const std::string reason = "could not launch the activity";
	std::cerr << "[net-match] " << reason << std::endl;
	g_ConsoleMan.PrintString("ERROR: " + reason);
	if (g_NetMatchService.GetState() != NetMatchServiceState::Idle) {
		g_NetMatchService.ReportRuntimeError(reason);
	}
	g_ActivityMan.EndActivity();
	g_ActivityMan.SetInActivity(false);
	if (s_netMatchServiceE2E) {
		if (s_netMatchServiceE2EExitCode == 0) {
			s_netMatchServiceE2EError = reason;
			s_netMatchServiceE2EExitCode = 1;
		}
		System::SetQuit(true);
		return false;
	}
	g_TimerMan.PauseSim(true);
	g_MenuMan.HandleTransitionIntoMenuLoop();
	RunMenuLoop();
	return !System::IsSetToQuit();
}

void RunGameLoop() {
	if (System::IsSetToQuit()) {
		return;
	}
	g_TimerMan.PauseSim(false);

	if (g_ActivityMan.ActivitySetToRestart()) {
		g_LoadingScreen.DrawLoadingSplash();
		g_WindowMan.UploadFrame();
		if (!g_ActivityMan.RestartActivity() && !HandleFailedActivityLaunch()) {
			return;
		}
	}

	long long updateStartTime = 0;
	long long updateTotalTime = 0;
	long long updateEndAndDrawStartTime = 0;
	long long drawStartTime = 0;
	long long drawTotalTime = 0;

	while (!System::IsSetToQuit()) {
		bool returnToMenuAfterNetworkEnd = false;
		updateStartTime = g_TimerMan.GetAbsoluteTime();

		PollSDLEvents();
		g_WindowMan.Update();
		g_WindowMan.ClearBackbuffer();

		g_TimerMan.Update();

		if (!g_ActivityMan.ActivityRunning()) {
			LocalPrediction::Clear();
		}

		const bool paceActiveAtIterStart = ScenarioRunner::IsLockstepControllerSyncActive();
		static bool s_pacePrevActive = false;
		if (paceActiveAtIterStart && !s_pacePrevActive) {
			// A fresh pace window per round, so multi-round runs don't blend their numbers.
			s_paceIterations = 0;
			s_paceSimTicks = 0;
			s_paceSimUs = 0;
			s_paceUpdateUs = 0;
			s_paceDrawUs = 0;
			ScenarioRunner::ResetLockstepWaitUs();
			g_TimerMan.ResetPaceCounters();
		}
		s_pacePrevActive = paceActiveAtIterStart;

		// A free-running lockstep match takes one tick per iteration, so the preview and the polls still run per tick.
		const bool freeRunLockstep = ScenarioRunner::GetArgs().freeRunSim && ScenarioRunner::IsLockstepControllerSyncActive();
		if (ScenarioRunner::GetArgs().freeRunSim) {
			g_TimerMan.SetFreeRunSim(freeRunLockstep);
		}

		// Simulation update, as many times as the fixed update step allows in the span since last frame draw.
		while (g_TimerMan.TimeForSimUpdate()) {
			ZoneScopedN("Simulation Update");

			const long long paceTickStartUs = g_TimerMan.GetAbsoluteTime();
			g_PerformanceMan.NewPerformanceSample();
			g_PerformanceMan.UpdateMSPSU();
			g_TimerMan.UpdateSim();
			g_AudioMan.RetireFinishedSimulationSounds();

			g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::SimTotal);

			const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
			// Sample the sim hash on an interval during live lockstep for the runtime desync check. NOT under
			// -tick-hashes recording (the offline gate is the check there); a real menu match has no -tick-hashes.
			constexpr uint64_t c_DesyncCheckIntervalTicks = 30;
			const bool desyncSampleTick = ScenarioRunner::IsLockstepControllerSyncActive() && !s_recordTickHashes &&
			                              (simTick % c_DesyncCheckIntervalTicks == 0);
			const bool a7HashTick = NetA7Journal::Enabled() && ScenarioRunner::IsLockstepControllerSyncActive();
			const bool hashThisTick = s_recordTickHashes || desyncSampleTick || a7HashTick;
			if (hashThisTick) {
				g_SimChecksum.BeginTick(simTick);
			}

			// Positive control — inject one genuine non-determinism at a fixed tick so the offline determinism
			// gate OR the runtime desync detector sees a guaranteed divergence. One-shot: a resynced
			// match reuses tick numbers, and the healed round must NOT be re-poisoned.
			static bool s_perturbFired = false;
			if ((ScenarioRunner::IsActive() || s_netMatchServiceE2E) && ScenarioRunner::GetArgs().selftestPerturb && simTick == 50 && !s_perturbFired) {
				s_perturbFired = true;
				std::random_device perturbDevice;
				const unsigned perturbAdvance = (perturbDevice() % 64u) + 1u;
				for (unsigned k = 0; k < perturbAdvance; ++k) {
					g_SimRNG.RandomNum<uint32_t>();
				}
			}

			// E2E control: the host pauses at tick 250 and unpauses at 430; both sims must stop and
			// resume on the same frame with sim time frozen across the gap.
			if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestPauseCommand) {
				if (simTick == 250) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{0, true}});
				} else if (simTick == 430) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{0, false}});
				}
			}
			const bool lockstepPausedTick = ScenarioRunner::IsLockstepPaused();
			if (lockstepPausedTick) {
				// The sim holds still: read the sim-rate resume key, exchange an empty frame so
				// commands and stops still flow, and step the shared resume countdown.
				if (!s_netMatchServiceE2E && g_UInputMan.KeyPressedSim(SDLK_P)) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{g_NetMatchService.GetLocalTeam(), false}});
				}
				g_MovableMan.RunLockstepPausedTick();
				ScenarioRunner::AdvanceLockstepPausedTick();
				if (ScenarioRunner::IsLockstepPaused() && simTick % 30 == 0) {
					g_FrameMan.SetScreenText(ScenarioRunner::GetLockstepResumeCountdown() > 0 ? "Match resuming..." : "Match paused - press P to resume", 0);
				}
			}
			if (!lockstepPausedTick) {
				g_LuaMan.Update();

				// E2E control: host-issued funds command at tick 50; both peers must apply it identically.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestFundsCommand && static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) == 50) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSetTeamFunds{0, 5000}});
				}
				// E2E control: host-issued spawn command at tick 50; both peers must clone the identical actor.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestSpawnCommand && static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) == 50) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSpawnActor{"AHuman", "Green Dummy", "Base.rte", 1000.0F, 200.0F, 0}});
				}
				// E2E control: host-issued delivery at tick 50; both peers must build the identical craft, hold, and flight.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestDeliverCommand && static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) == 50) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameDeliverCargo{"ACDropShip", "Dropship MK1", "Base.rte", 880.0F, 100.0F, 0, {{"AHuman", "Green Dummy", "Base.rte"}, {"AHuman", "Green Dummy", "Base.rte"}}}});
				}
				// E2E control: the host orders its dummy and its brain at fixed ticks; both peers must hold the identical AI mode, waypoints and squad.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestAIOrderCommand && (simTick == 50 || simTick == 200 || simTick == 400 || simTick == 600)) {
					std::vector<Actor*> units;
					for (Actor* actor: *g_MovableMan.GetTeamRoster(0)) {
						if (!actor->IsInGroup("Brains") && dynamic_cast<AHuman*>(actor)) {
							units.push_back(actor);
						}
					}
					std::sort(units.begin(), units.end(), [](const Actor* lhs, const Actor* rhs) { return lhs->GetUniqueID() < rhs->GetUniqueID(); });
					Actor* brain = g_MovableMan.GetFirstBrainActor(0);
					if (!units.empty() && brain) {
						Actor* unit = units.front();
						const auto order = [](const Actor* actor, uint8_t op, const Vector& point, const Actor* target) {
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameAIOrder{static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), op, point.m_X, point.m_Y, target ? static_cast<int64_t>(target->GetUniqueID()) : 0}});
						};
						if (simTick == 50) {
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSetActorAIMode{static_cast<int64_t>(unit->GetUniqueID()), 0, static_cast<uint8_t>(Actor::AIMODE_GOTO)}});
							order(unit, NetGameAIOrder::SceneWaypoint, unit->GetPos() + Vector(300.0F, 0.0F), nullptr);
						} else if (simTick == 200) {
							order(unit, NetGameAIOrder::MOWaypoint, unit->GetPos(), brain);
						} else if (simTick == 400) {
							order(brain, NetGameAIOrder::FormSquad, brain->GetPos() + Vector(600.0F, 0.0F), nullptr);
						} else {
							order(brain, NetGameAIOrder::DisbandSquad, brain->GetPos(), nullptr);
						}
						std::cout << "[net-match-service-e2e] ai order issued at tick " << simTick << " unit " << unit->GetUniqueID() << " brain " << brain->GetUniqueID() << std::endl;
					}
				}
				// E2E control: host-issued inventory ops on its brain at fixed ticks; both peers must mutate identically.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestInventoryCommand &&
				    (simTick == 210 || simTick == 240 || simTick == 270 || simTick == 300)) {
					if (const Actor* brain = g_MovableMan.GetFirstBrainActor(0)) {
						NetGameInventoryOp op;
						op.actorUID = static_cast<int64_t>(brain->GetUniqueID());
						op.team = 0;
						if (simTick == 210) {
							op.op = NetGameInventoryOp::Reorder;
							op.a = 0;
							op.b = 1;
						} else if (simTick == 240) {
							op.op = NetGameInventoryOp::SwapEquipped;
							op.a = 0;
							op.b = 0;
						} else if (simTick == 270) {
							op.op = NetGameInventoryOp::Reload;
							op.a = 0;
							op.b = -1;
						} else {
							op.op = NetGameInventoryOp::Drop;
							op.a = -1;
							op.b = 0;
							op.hasDropDirection = true;
							op.dirX = 0.7F;
							op.dirY = -0.7F;
						}
						std::cout << "[net-match-service-e2e] inventory op " << static_cast<int>(op.op) << " at tick " << simTick << " actor " << op.actorUID << std::endl;
						ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, op});
					}
				}
				// E2E control: the host grants funds then places a REAL buy order through GameActivity::CreateDelivery,
				// exercising the confirm seam -> wire -> queued arrival -> identical funds deduction on both peers.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestBuyCommand) {
					if (simTick == 50) {
						ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSetTeamFunds{0, 5000}});
					} else if (simTick == 80) {
						if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity())) {
							const SceneObject* craft = dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset("ACDropShip", "Dropship MK1", "Base.rte"));
							const SceneObject* dummy = dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset("AHuman", "Green Dummy", "Base.rte"));
							if (craft && dummy) {
								gameActivity->AddOverridePurchase(craft, 0);
								gameActivity->AddOverridePurchase(dummy, 0);
								gameActivity->AddOverridePurchase(dummy, 0);
								gameActivity->SetLandingZone(Vector(900.0F, 0.0F), 0);
								const bool ordered = gameActivity->CreateDelivery(0);
								std::cout << "[net-match-service-e2e] buy order placed: " << (ordered ? "ok" : "FAILED") << std::endl;
							}
						}
					} else if (simTick == 700) {
						if (const Activity* activity = g_ActivityMan.GetActivity()) {
							std::cout << "[net-match-service-e2e] team 0 funds at tick 700: " << activity->GetTeamFunds(0) << std::endl;
						}
					}
				}
				// E2E control: host scuttles the delivered craft at tick 100; both peers must gib it identically.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestScuttleCommand && simTick == 100) {
					if (const int64_t craftUID = g_MovableMan.GetFirstCraftUniqueID(0)) {
						ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameScuttleCraft{craftUID, 0}});
					}
				}
				// Test control: fake a hung peer — this peer stops producing frames for 8s; the other side
				// must ride out the stall within the grace window and both must still finish identical.
				// Works in e2e AND interactive matches so the headed stall overlay can be exercised.
				if (ScenarioRunner::GetArgs().selftestStall && ScenarioRunner::IsLockstepControllerSyncActive() && simTick == 300) {
					std::cout << "[net-match] stall: sleeping 8s at tick 300" << std::endl;
					std::this_thread::sleep_for(std::chrono::seconds(8));
				}
				// Test control: leave the match at tick 300 like a pause-menu quit; the peer must get a clean end.
				if (ScenarioRunner::GetArgs().selftestLeave && ScenarioRunner::IsLockstepControllerSyncActive() && simTick == 300) {
					std::cout << "[net-match] leave: quitting to menu at tick 300" << std::endl;
					g_ActivityMan.EndActivity();
					g_ActivityMan.SetInActivity(false);
				}
				// E2E control: this peer spawns a SECOND brain for its own team; the win condition must ride
				// through the original brain's death because the team still has the spawned one.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestBrainSpawnCommand && simTick == 40) {
					std::cout << "[net-match-service-e2e] brain spawn: team 1 at 1250,700 (sentry)" << std::endl;
					// Spawn the spare as SENTRY like the real brains (P4AlphaDuel), so it holds position and
					// survives the original's death instead of wandering into the kill zone on BRAINHUNT.
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSpawnActor{"AHuman", "Brain Robot", "Base.rte", 1250.0F, 700.0F, 1, Actor::AIMODE_SENTRY}});
				}
				// Test control: the host delivers two crafts just above the enemy brain and scuttles each as
				// its hatch opens; both peers must trace the identical game-over transition. The spawn height
				// is computed from the terrain and rides the synced command, so both peers see the same drop.
				// Works in interactive matches too, so a headed match can be ended deterministically.
				if (ScenarioRunner::GetArgs().selftestBrainKillCommand && ScenarioRunner::IsLockstepControllerSyncActive()) {
					// Not before tick 80: an activity Over inside the first 100 running ticks reads as a broken setup.
					// Teams 2/3 exist only in 3/4-peer matches; their kill windows are query-gated no-ops otherwise.
					const bool teamOneWindow = simTick == 80 || simTick == 100;
					const bool teamTwoWindow = simTick == 130 || simTick == 150;
					const bool teamThreeWindow = simTick == 180 || simTick == 200;
					if (teamOneWindow || teamTwoWindow || teamThreeWindow) {
						const int targetTeam = teamOneWindow ? 1 : (teamTwoWindow ? 2 : 3);
						const bool firstDrop = simTick == 80 || simTick == 130 || simTick == 180;
						// Aim at the live brain and drop low; static coordinates drift off on a different sim's physics.
						if (const Actor* enemyBrain = g_MovableMan.GetFirstBrainActor(targetTeam)) {
							const float dropX = enemyBrain->GetPos().m_X + (firstDrop ? 4.0F : -4.0F);
							const float dropY = g_SceneMan.FindAltitude(Vector(dropX, 0.0F), 2000, 20) - 60.0F;
							std::cout << "[net-match-service-e2e] brain-kill deliver: tick=" << simTick << " team=" << targetTeam << " x=" << dropX << " y=" << dropY << std::endl;
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", dropX, dropY, 0, {{"AHuman", "Green Dummy", "Base.rte"}}}});
						} else if (teamOneWindow) {
							// The tuned 2-peer fallback: keep the original blind drop when team 1's brain query misses.
							const float dropX = simTick == 80 ? 1120.0F : 1112.0F;
							const float dropY = g_SceneMan.FindAltitude(Vector(dropX, 0.0F), 2000, 20) - 60.0F;
							std::cout << "[net-match-service-e2e] brain-kill deliver: tick=" << simTick << " x=" << dropX << " y=" << dropY << std::endl;
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", dropX, dropY, 0, {{"AHuman", "Green Dummy", "Base.rte"}}}});
						}
					}
					// The poll's own bounds already skip the original 80/100 windows, so it must not be
					// an else of the (new, wider) window check or the added windows would eat poll ticks.
					if (simTick > 100 && simTick % 5 == 0) {
						if (const int64_t craftUID = g_MovableMan.GetFirstUnloadingCraftUniqueID(0)) {
							std::cout << "[net-match-service-e2e] brain-kill scuttle: tick=" << simTick << " craft=" << craftUID << std::endl;
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameScuttleCraft{craftUID, 0}});
						}
					}
				}

				// P pauses the match for every peer; the command applies on the same synced frame.
				if (ScenarioRunner::IsLockstepControllerSyncActive() && !s_netMatchServiceE2E && g_UInputMan.KeyPressedSim(SDLK_P)) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{g_NetMatchService.GetLocalTeam(), true}});
				}

				// Mid-match session upkeep: reconnect handshakes the coordinator handed over.
				g_NetMatchService.PumpSessionEvents();
				DriveModerationE2e();

				g_FrameMan.Update();

				g_MovableMan.CompleteQueuedMOIDDrawings();

				g_ConsoleMan.Update();
				{
					static const uint64_t soundPhase = Hash("Tick");
					SoundSimulationScope simulationSounds(0, soundPhase);
					g_ActivityMan.Update();

					if (g_SceneMan.GetScene()) {
						g_SceneMan.GetScene()->Update();
					}

					g_LuaMan.ClearScriptTimings();
					g_MovableMan.Update();
				}
			}
			if (ScenarioRunner::HasControllerReplayError()) {
				HandleControllerReplayFailure(returnToMenuAfterNetworkEnd);
				g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
				break;
			}
			g_PerformanceMan.UpdateSortedScriptTimings(g_LuaMan.GetScriptTimings());

			g_AudioMan.Update();
			g_MusicMan.Update();

			if (!lockstepPausedTick) {
				g_ActivityMan.LateUpdateGlobalScripts();
				// Kick the async MOID draw after the last main-thread sim mutation of the tick; it
				// completes before the render frames below, which share draw scratch state with it.
				g_MovableMan.StartMOIDDrawTask();
			}

			DumpSimStateIfArmed(simTick);
			TickProbeIfArmed(simTick);
			LocalPredictionInvarianceOnTick(simTick);
			TrackUidsIfArmed(simTick);
			DumpTerrainIfArmed(simTick);
			{
				static const std::string s_wendPath = (!ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim")) + ".wend.terrainevents.txt";
				SceneMan::FlushTerrainEventsAtWindowEnd(simTick, s_wendPath);
			}

			// Feed end-of-tick terrain state, finalize this tick's hash, and hand the result to the
			// MetricsCollector for the per-tick determinism trace (no-op without an active scenario run).
			std::optional<SimChecksum::Result> probeTickResult;
			if (hashThisTick) {
				g_SceneMan.FeedTerrainToSimChecksum();
				const auto tickResult = g_SimChecksum.EndTick();
				if (a7HashTick && ScenarioRunner::GetLockstepAppliedFrame() == simTick) {
					const uint64_t round = ScenarioRunner::GetLockstepRoundId();
					NetA7Journal::AppliedTick(round, simTick, ScenarioRunner::GetLockstepLocalPeerId(), SimChecksum::HashHex(SimChecksum::SimGatedHash(tickResult)));
					g_MovableMan.RecordA7UnitOwnership(round, simTick);
				}
				if (s_recordTickHashes && s_rbProbePhase != 3) {
					g_MetricsCollector.RecordTickHash(tickResult, lockstepPausedTick);
				}
				if (desyncSampleTick) {
					ScenarioRunner::SubmitLockstepChecksum(simTick, SimChecksum::SimGatedHash(tickResult));
				}
				if ((s_rbProbeAtTick > 0 || s_rbProbeFuzzCount > 0) && (ScenarioRunner::IsActive() || ScenarioRunner::IsLockstepReplayPlayback())) {
					probeTickResult = tickResult;
				}
			}

			// Start async GC after all main-thread Lua work for this tick is done. Earlier (inside MovableMan::Update)
			// it overlapped with LateUpdateGlobalScripts on main, opening a window for ABBA between main holding one
			// state for the global script and a worker GC __gc finalizer wanting it from another state.
			g_LuaMan.StartAsyncGarbageCollection();
			// Join before leaving the tick: an unfinished GC races the next tick's Lua for the state
			// mutexes, so collection timing (and per-peer sim state) would follow wall-clock scheduling.
			g_LuaMan.WaitForAsyncGarbageCollection();

			// This is to support hot reloading entities in SceneEditorGUI. It's a bit hacky to put it in Main like this, but PresetMan has no update in which to clear the value, and I didn't want to set up a listener for the job.
			// It's in this spot to allow it to be set by UInputMan update and ConsoleMan update, and read from ActivityMan update.
			g_PresetMan.ClearReloadEntityPresetCalledThisUpdate();

			// The MOID draw must not overlap the render frames: both rotate sprites through shared
			// scratch bitmaps, so an overlap corrupts the hit layer per-peer.
			g_MovableMan.CompleteQueuedMOIDDrawings();

			// Sim consumed this tick's accumulated input edges; clear before next tick reads
			g_UInputMan.EndSimUpdate();
			if (probeTickResult) RollbackProbeOnHashedTick(simTick, *probeTickResult);

			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);

			if (ScenarioRunner::IsLockstepControllerSyncActive()) {
				++s_paceSimTicks;
				s_paceSimUs += g_TimerMan.GetAbsoluteTime() - paceTickStartUs;
			}

			// Capture both peers after the complete tick, including global callbacks and worker joins.
			if (ScenarioRunner::GetArgs().selftestSnapshot && ScenarioRunner::IsLockstepControllerSyncActive() && simTick == 300) {
				const auto saveStart = std::chrono::steady_clock::now();
				const std::string saveName = "p5snap_p" + std::to_string(ScenarioRunner::GetLockstepLocalPeerId());
				const bool saved = g_ActivityMan.SaveCurrentGame(saveName) && g_ActivityMan.WaitForSaveGameTask();
				const auto saveMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - saveStart).count();
				std::cout << "[net-match] snapshot " << (saved ? "saved" : "FAILED") << ": " << saveName << " in " << saveMs << "ms at tick " << simTick << std::endl;
			}

			if (ScenarioRunner::FinishLockstepSimulationTick(simTick)) {
				const std::string reason = ScenarioRunner::GetLockstepStopReason();
				// A completed first round still takes the shared rematch transition below.
				if (!reason.starts_with("Complete:") || !IsFirstE2ERematchReady()) {
					ScenarioRunner::SetControllerReplayError(reason);
					HandleControllerReplayFailure(returnToMenuAfterNetworkEnd);
					break;
				}
			}

			if (s_bitmapSaveSelfTest && s_bitmapSaveSelfTestResult < 0) {
				s_bitmapSaveSelfTestResult = g_FrameMan.RunBitmapSaveSelfTest() ? 0 : 1;
				System::SetQuit(true);
				break;
			}
			if (s_saveCallbacksSelfTest && simTick > 0) {
				s_saveCallbacksSelfTestPassed = g_ActivityMan.RunSaveCallbacksSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (s_purgeSelfTest && simTick > 0) {
				s_purgeSelfTestPassed = g_MovableMan.RunPurgeSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (s_globalCallbacksSelfTest && simTick > 0) {
				s_globalCallbacksSelfTestPassed = g_ActivityMan.RunGlobalCallbacksSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (s_saveCatalogSelfTest && simTick > 0) {
				SaveLoadMenuGUI::RunCatalogSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (!s_contractAuditOperation.empty() && simTick >= s_contractAuditTick &&
			    (!s_contractAuditFinished || (ScenarioRunner::GetArgs().contractAuditContinueThrough > 0 &&
			      static_cast<uint64_t>(simTick) >= ScenarioRunner::GetArgs().contractAuditContinueThrough))) {
				const std::string base = ScenarioRunner::GetArgs().outPath + ".contract";
				ContractAudit::State pendingBefore;
				bool havePendingBefore = false;
				const auto observe = [&](const std::string& suffix) {
					std::vector<std::string> graphs, problems;
					const bool graph = ObserveScriptGraphs(graphs, problems);
					auto state = ContractAudit::Observe(base + "." + suffix + ".gaps.txt");
					ContractAudit::Write(ContractAudit::Identity(), base + "." + suffix + ".identity.txt");
					ContractAudit::Write(state, base + "." + suffix + ".state.txt");
					if (s_contractAuditOperation.starts_with("stage-")) {
						const auto pending = ContractAudit::ObservePendingCheckpoint(base + "." + suffix + ".pending.gaps.txt");
						ContractAudit::Write(pending, base + "." + suffix + ".pending.state.txt");
						if (suffix == "staged_before") { pendingBefore = pending; havePendingBefore = true; }
						else if (suffix == "staged_after" && havePendingBefore) {
							const size_t differences = ContractAudit::Compare(pendingBefore, pending, base + ".pending.diff.txt");
							std::cout << "[contract-audit-pending] differences=" << differences << " before_fields=" << pendingBefore.size()
							          << " after_fields=" << pending.size() << std::endl;
						}
					}
					for (const std::string& problem: problems) std::cout << "[contract-audit] graph-problem=" << suffix << " " << problem << std::endl;
					for (size_t index = 0; index < graphs.size(); ++index) {
						std::ofstream out(base + "." + suffix + ".lua" + std::to_string(index), std::ios::binary);
						out << graphs[index];
					}
					std::ofstream deep(base + "." + suffix + ".simstate.txt");
					g_MovableMan.DumpSimState(g_TimerMan.GetSimUpdateCount(), deep);
					for (int index = 0; index <= static_cast<int>(g_LuaMan.GetThreadedScriptStates().size()); ++index) {
						g_LuaMan.GetStateByIndex(index).RunScriptString("if _ContractAuditCheck then _ContractAuditCheck('" + suffix + "') end; "
							"ConsoleMan:PrintString('[contract-audit-marker] observation=" + suffix + " vm=" + std::to_string(index) +
							" value=' .. tostring(_ContractAuditCandidateMarker))");
					}
					std::cout << "[contract-audit] observation=" << suffix << " fields=" << state.size() << " graph=" << graph << " problems=" << problems.size() << std::endl;
					return state;
				};
				if (s_contractAuditFinished) {
					observe("continued");
					std::cout << "[contract-audit-continuation] completed_tick=" << g_TimerMan.GetSimUpdateCount()
					          << " requested_tick=" << ScenarioRunner::GetArgs().contractAuditContinueThrough << std::endl;
				} else {
				ScenarioRunner::SetContractAuditSeedMarker();
                const bool inputFixture = std::getenv("CC_CONTRACT_INPUT_FIXTURE") != nullptr;
                if (inputFixture) ContractAudit::SetInputFixture(false);
                const auto before = observe("before");
				bool prepared = true, applied = false;
				if (s_contractAuditOperation == "observe") {
					applied = true;
				} else if (ScenarioRunner::RunContractAuditLoad(s_contractAuditOperation,
				    [&](const std::string& suffix) { observe(suffix); }, prepared, applied)) {
				} else if (s_contractAuditOperation == "memory" || s_contractAuditOperation == "memory-perturb") {
					MovableMan::WorldSnapshot snapshot;
					prepared = g_MovableMan.CaptureWorld(snapshot);
					const auto captured = observe("captured");
					ContractAudit::Compare(before, captured, base + ".capture.diff.txt");
					if (prepared && s_contractAuditOperation == "memory-perturb") {
                        for (int index = 0; index <= static_cast<int>(g_LuaMan.GetThreadedScriptStates().size()); ++index) {
                            g_LuaMan.GetStateByIndex(index).RunScriptString("if _ContractAuditPerturb then _ContractAuditPerturb() end");
                        }
                        if (inputFixture) ContractAudit::SetInputFixture(true);
						const auto perturbed = observe("perturbed");
						ContractAudit::Compare(before, perturbed, base + ".perturb.diff.txt");
					}
					if (prepared) applied = g_MovableMan.RestoreWorld(snapshot);
				} else if (s_contractAuditOperation == "hold") {
                    MovableMan::WorldSetAside held;
                    prepared = g_MovableMan.SetAsideWorld(held);
                    if (prepared && inputFixture) ContractAudit::SetInputFixture(true);
                    if (prepared) applied = g_MovableMan.ReinstateWorld(held);
				} else if (s_contractAuditOperation == "preview") {
					const auto count = LocalPrediction::GetPreviewCount();
					LocalPrediction::SetCommandLineOverride(1);
					LocalPrediction::SetDepthOverride(6);
					LocalPrediction::Clear();
					LocalPrediction::RunPreview();
					LocalPrediction::Clear();
					applied = LocalPrediction::GetPreviewCount() > count;
				} else if (s_contractAuditOperation.starts_with("load:")) {
					prepared = g_ActivityMan.LoadGameToRestart(s_contractAuditOperation.substr(5));
					const auto staged = observe("staged");
					ContractAudit::Compare(before, staged, base + ".staging.diff.txt");
					if (prepared) applied = g_ActivityMan.RestartActivity();
				} else {
					prepared = g_ActivityMan.SaveCurrentGame("contract_audit") && g_ActivityMan.WaitForSaveGameTask();
					const auto saved = observe("saved");
					ContractAudit::Compare(before, saved, base + ".save.diff.txt");
                    if (prepared && s_contractAuditOperation == "file") {
                        for (int draw = 0; draw < 73; ++draw) g_SimRNG.RandomNum<uint32_t>();
                        if (inputFixture) ContractAudit::SetInputFixture(true);
						applied = g_ActivityMan.LoadAndLaunchGame("contract_audit");
					} else if (prepared && s_contractAuditOperation == "stage") {
						applied = g_ActivityMan.LoadGameToRestart("contract_audit");
					} else {
						applied = prepared;
					}
				}
				const auto after = observe("after");
				const size_t differences = ContractAudit::Compare(before, after, base + ".diff.txt");
				std::cout << "[contract-audit] complete operation=" << s_contractAuditOperation << " prepared=" << prepared << " applied=" << applied << " differences=" << differences << std::endl;
				s_contractAuditFinished = true;
				ScenarioRunner::PerturbContractAuditContinuation();
				if (ScenarioRunner::GetArgs().contractAuditContinueThrough == 0) {
					System::SetQuit(true);
					g_ActivityMan.EndActivity();
					break;
				}
				if (g_TimerMan.GetSimUpdateCount() != simTick) {
					std::cout << "[contract-audit-continuation] invalid_tick_change=1 before=" << simTick
					          << " after=" << g_TimerMan.GetSimUpdateCount() << std::endl;
					s_contractAuditFinished = false;
					System::SetQuit(true);
					g_ActivityMan.EndActivity();
					break;
				}
				std::cout << "[contract-audit-continuation] started_tick=" << simTick
				          << " requested_tick=" << ScenarioRunner::GetArgs().contractAuditContinueThrough << std::endl;
				}
			}
			if (!s_snapshotRoundtripSelfTestName.empty() && simTick > 0) {
				bool readerPassed = true;
				for (const std::string ending: {"\n", "\r\n", " \t\n", " // empty\n", " /* empty */\n"}) {
					for (bool useValueReader: {false, true}) {
						Reader reader(std::make_unique<std::stringstream>("Empty =" + ending + "Next = present\n"), "reader-empty-selftest.ini");
						const bool first = reader.ReadPropName() == "Empty";
						std::string value;
						if (useValueReader) value = reader.ReadPropValue();
						else reader >> value;
						const bool empty = value.empty();
						const bool next = reader.NextProperty() && reader.ReadPropName() == "Next";
						reader >> value;
						readerPassed = first && empty && next && value == "present" && readerPassed;
					}
				}
				std::cout << "[reader-empty-selftest] " << (readerPassed ? "PASS" : "FAIL") << " cases=10" << std::endl;
				const std::string output = s_snapshotRoundtripSelfTestName + "_roundtrip";
				g_AudioMan.SetCheckpointTraceEnabled(true);
				bool loaded = false, saved = false, audioUnchanged = true;
				{
					// The ordinary load/save path remains intact. This test holds the
					// independent mixer clock at one observation boundary.
					std::unique_ptr<AudioCheckpoint::MixerLock> audioBoundary;
					if (s_snapshotRoundtripLockAudio) audioBoundary = std::make_unique<AudioCheckpoint::MixerLock>(g_AudioMan.IsAudioEnabled() ? g_AudioMan.GetAudioSystem() : nullptr);
					g_AudioMan.TraceCheckpointBoundary("roundtrip-before-load");
					loaded = readerPassed && g_ActivityMan.LoadAndLaunchGame(s_snapshotRoundtripSelfTestName);
					g_AudioMan.TraceCheckpointBoundary("roundtrip-load-returned");
					const bool perturbed = !s_snapshotRoundtripPerturbAudio || (loaded && g_AudioMan.PerturbCheckpointCursorForSelfTest());
					const std::string audioBeforeSave = loaded && s_snapshotRoundtripLockAudio ? g_AudioMan.SaveCheckpoint() : "";
					saved = loaded && perturbed && g_ActivityMan.SaveCurrentGame(output) && g_ActivityMan.WaitForSaveGameTask();
					if (saved && s_snapshotRoundtripLockAudio) audioUnchanged = g_AudioMan.SaveCheckpoint() == audioBeforeSave;
					g_AudioMan.TraceCheckpointBoundary("roundtrip-save-returned");
					const bool audioChecked = s_snapshotRoundtripLockAudio && saved;
					std::cout << "[snapshot-audio-boundary] locked=" << s_snapshotRoundtripLockAudio << " checked=" << audioChecked << " unchanged=" << (audioChecked ? std::to_string(audioUnchanged) : "unchecked") << " perturbed=" << s_snapshotRoundtripPerturbAudio << std::endl;
				}
				g_AudioMan.TraceCheckpointBoundary("roundtrip-mixer-released");
				const bool playbackContinued = !s_snapshotRoundtripCheckPlayback || (saved && g_AudioMan.RunCheckpointPlaybackContinuationSelfTest());
				g_AudioMan.SetCheckpointTraceEnabled(false);
				s_snapshotRoundtripSelfTestPassed = loaded && saved && audioUnchanged && playbackContinued;
				std::cout << "[snapshot-roundtrip] " << (s_snapshotRoundtripSelfTestPassed ? "PASS" : "FAIL") << " save=" << output << std::endl;
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (!s_loadSelfTestName.empty() && simTick > 0) {
				s_loadSelfTestPassed = g_ActivityMan.RunLoadSelfTest(s_loadSelfTestName, s_loadSelfTestExpected);
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				// A load leaves the saved game running, so the starting scenario never reaches its
				// own verdict. The selftest owns this run's result.
				g_MetricsCollector.Record("final_tick", static_cast<double>(simTick));
				g_MetricsCollector.SetResult(s_loadSelfTestPassed);
				break;
			}
			if (s_saveIoSelfTest && simTick > 0) {
				if (s_saveMenuSelfTest) s_saveMenuSelfTestPassed = SaveLoadMenuGUI::RunSaveSelfTest(s_saveIoSelfTestName, s_saveIoSelfTestQueued);
				else s_saveIoSelfTestQueued = g_ActivityMan.SaveCurrentGame(s_saveIoSelfTestName);
				std::cout << "[save-selftest] queued=" << s_saveIoSelfTestQueued << " pending=" << g_ActivityMan.IsCurrentlySaving() << std::endl;
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}

			// Scenario direct-launch: quit when the activity reaches OVER or the -max-ticks cap hits,
			// instead of bouncing to the menu. The cap counts global sim ticks, so the trace length
			// is fixed even if the activity never sets OVER.
			if (ScenarioRunner::IsActive()) {
				static uint64_t s_scenarioStartTick = UINT64_MAX;
				const uint64_t nowTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				if (s_scenarioStartTick == UINT64_MAX) {
					s_scenarioStartTick = nowTick;
				}
				const uint64_t elapsedTicks = nowTick - s_scenarioStartTick;
				const uint64_t scenarioTickCap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 1800;
				const uint64_t tickCap = NetGameplayRequested() && s_netLockstepTicks > 0 ? s_netLockstepTicks : scenarioTickCap;
				const Activity* scenarioActivity = g_ActivityMan.GetActivity();
				if ((scenarioActivity && scenarioActivity->IsOver()) || elapsedTicks >= tickCap) {
					// Finalize so the scenario's Lua OnEnd grades the run even when the CLI tick cap
					// stops it before the scenario's own max-ticks (idempotent if it already ended).
					g_ActivityMan.EndActivity();
					System::SetQuit(true);
					break;
				}
			}

			// Playback honours -max-ticks as a bounded run: distinct from the recording's own end.
			if (!s_netReplayInPath.empty() && ScenarioRunner::GetArgs().maxTicks > 0 && !ScenarioRunner::HasControllerReplayError() &&
			    static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) >= static_cast<uint64_t>(ScenarioRunner::GetArgs().maxTicks)) {
				s_netReplayTicks = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				ScenarioRunner::SetLockstepReplayOutcome(ScenarioRunner::LockstepReplayOutcome::TickCap);
				g_ActivityMan.EndActivity();
				System::SetQuit(true);
				break;
			}
			// Stop after the last requested trace tick has completed.
			if (!ScenarioRunner::IsActive() && !s_netMatchServiceE2E && s_recordTickHashes && g_NetMatchService.WasEverStarted()) {
				const uint64_t cap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 600;
				if (g_MetricsCollector.GetTickHashCount() >= cap) {
					std::cout << "[menu-mp] trace complete at tick " << g_TimerMan.GetSimUpdateCount() << std::endl;
					// Hand over what we still owe BEFORE the goodbye, so a client one input-delay
					// behind can finish its own last tick instead of losing the round to our exit.
					(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, 0);
					g_NetMatchService.Complete("menu mp trace complete");
					(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, c_CappedStopLingerMs);
					g_ActivityMan.EndActivity();
					System::SetQuit(true);
					break;
				}
			}

			// Interactive menu-launched match: end it when the activity is over. The win condition and
			// this tick window are sim-state, so both peers finish on the same tick without a timeout.
			if (!ScenarioRunner::IsActive() && !s_netMatchServiceE2E && !s_recordTickHashes && g_NetMatchService.GetState() == NetMatchServiceState::Running) {
				static uint64_t s_matchOverTick = UINT64_MAX;
				const Activity* matchActivity = g_ActivityMan.GetActivity();
				if (matchActivity && matchActivity->IsOver()) {
					const uint64_t nowTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
					if (s_matchOverTick == UINT64_MAX) {
						s_matchOverTick = nowTick;
					}
					const uint64_t graceTicks = static_cast<uint64_t>(5.0f / g_TimerMan.GetDeltaTimeSecs());
					if (nowTick - s_matchOverTick >= graceTicks) {
						s_matchOverTick = UINT64_MAX;
						const std::string result = BuildNetMatchResultText();
						g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
						g_NetMatchService.FinishMatch(result);
						g_ActivityMan.EndActivity();
						g_ActivityMan.SetInActivity(false);
						returnToMenuAfterNetworkEnd = true;
						break;
					}
				} else {
					s_matchOverTick = UINT64_MAX;
				}
			}

			if (s_netMatchServiceE2E) {
				const uint64_t nowTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				Activity* activity = g_ActivityMan.GetActivity();
				if (!activity) {
					s_netMatchServiceE2EError = "activity ended before e2e tick cap";
					s_netMatchServiceE2EExitCode = 1;
					System::SetQuit(true);
					break;
				}
				const Activity::ActivityState activityState = activity->GetActivityState();
				if (activityState == Activity::Editing) {
					s_netMatchServiceE2EEnteredEditor = true;
					s_netMatchServiceE2EError = "activity entered unsynchronized setup editor";
					s_netMatchServiceE2EExitCode = 1;
					g_NetMatchService.ReportRuntimeError(s_netMatchServiceE2EError);
					g_ActivityMan.EndActivity();
					System::SetQuit(true);
					break;
				}
				// E2E rematch ride-through: match 1 ended, so finish it, reconvene the live session in the
				// lobby, and relaunch — round 2 is policed by the live desync exchange like any match.
				if (IsFirstE2ERematchReady()) {
					s_netMatchServiceE2ERematches = 1;
					const std::string result = BuildNetMatchResultText();
					std::cout << "[net-match-service-e2e] rematch: match 1 over (" << result << "), returning to lobby" << std::endl;
					g_NetMatchService.FinishMatch(result);
					g_ActivityMan.EndActivity();
					g_ActivityMan.SetInActivity(false);
					std::string rematchError;
					if (!g_NetMatchService.ReturnToLobby(&rematchError)) {
						s_netMatchServiceE2EError = "rematch return-to-lobby failed: " + rematchError;
						s_netMatchServiceE2EExitCode = 1;
						System::SetQuit(true);
						break;
					}
					g_NetMatchService.SetReady();
					if (s_netHost) {
						g_NetMatchService.RequestStart();
					}
					std::string rematchPreset;
					bool rematchReady = false;
					const auto rematchWaitStart = std::chrono::steady_clock::now();
					while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - rematchWaitStart).count() < 60) {
						if (g_NetMatchService.ConsumeReadyToLaunch(rematchPreset)) {
							rematchReady = true;
							break;
						}
						if (g_NetMatchService.GetState() == NetMatchServiceState::Failed) {
							break;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					}
					std::string rematchConfigureError;
					if (!rematchReady) {
						s_netMatchServiceE2EError = "rematch launch failed: " + g_NetMatchService.GetErrorText();
						s_netMatchServiceE2EExitCode = 1;
						System::SetQuit(true);
						break;
					} else if (!ConfigureNetMatchServiceE2EActivity(rematchPreset, &rematchConfigureError)) {
						s_netMatchServiceE2EError = "rematch configure failed: " + rematchConfigureError;
						s_netMatchServiceE2EExitCode = 1;
						System::SetQuit(true);
						break;
					}
					// Restart NOW: the fresh coordinator expects frame 1, so no sim tick may run before
					// RestartActivity resets the sim count (the poll above also left real-time debt in
					// the sim accumulator, which ResetTime clears).
					g_TimerMan.PauseSim(true);
					if (!g_ActivityMan.RestartActivity()) {
						s_netMatchServiceE2EError = "rematch activity restart failed";
						s_netMatchServiceE2EExitCode = 1;
						System::SetQuit(true);
						break;
					}
					std::cout << "[net-match-service-e2e] rematch: round 2 launching" << std::endl;
					// Re-anchor tick accounting; round 2 counts fresh from the zeroed sim count.
					s_netMatchE2ETicks.OnNewMatch();
					break;
				}
				// A legitimate game-over may end the activity mid-run; the sim keeps ticking to the cap so
				// the trace stays bounded. An end in the first 100 ticks still means a broken setup.
				const uint64_t earlyOverTick = ScenarioRunner::HasLockstepCoordinator()
					                               ? ScenarioRunner::GetLockstepAppliedFrame()
					                               : s_netMatchE2ETicks.Total();
				if (activityState == Activity::HasError || (activityState == Activity::Over && s_netMatchE2ETicks.EarlyOverIsSetupFailure(earlyOverTick))) {
					s_netMatchServiceE2EError = std::string("activity ended in state ") + ActivityStateName(activityState);
					s_netMatchServiceE2EExitCode = 1;
					g_NetMatchService.ReportRuntimeError(s_netMatchServiceE2EError);
					System::SetQuit(true);
					break;
				}
				if (activityState == Activity::Running || activityState == Activity::Over) {
					s_netMatchE2ETicks.NoteSimTick(nowTick);
					// In-match census; the report runs after EndActivity, which releases actors.
					s_netMatchE2EActorCensus = g_MovableMan.GetActorCount();
					s_netMatchE2EActorCensusPeak = std::max(s_netMatchE2EActorCensusPeak, s_netMatchE2EActorCensus);
					const uint64_t tickCap = s_netLockstepTicks > 0 ? s_netLockstepTicks : 600;
					if (s_netMatchE2ETicks.Total() > tickCap) {
						// A capped stop is per-peer wall clock: a peer settled behind a lagged link still
						// owes itself our in-flight tail, so hand over the forwards we hold and hold the
						// socket open before quitting drops it.
						(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, 0);
						g_NetMatchService.Complete("e2e complete");
						(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, c_CappedStopLingerMs);
						g_ActivityMan.EndActivity();
						System::SetQuit(true);
						break;
					}
				}
			}

			if (!g_ActivityMan.IsInActivity()) {
				g_TimerMan.PauseSim(true);

				if (!g_ActivityMan.ActivitySetToRestart()) {
					// Leaving a running net match: a clean leave lets N-peer survivors keep playing and,
					// with nobody left, ends their match at once - unlike a drop, which holds the seat
					// open for its reclaim window. The §7 exchange runs before the link goes down.
					if (g_NetMatchService.GetState() == NetMatchServiceState::Running) {
						g_ConsoleMan.PrintString("NETWORK: Match left");
						g_NetMatchService.LeaveMatch("Match left");
					}
					// The e2e has no menu to return to; a leaver's run ends here.
					if (s_netMatchServiceE2E) {
						System::SetQuit(true);
						break;
					}
					g_MenuMan.HandleTransitionIntoMenuLoop();
					RunMenuLoop();
				}
			}
			if (s_rbProbeMemoryRestorePending) {
				s_rbProbeMemoryRestorePending = false;
				const auto restoreStart = std::chrono::steady_clock::now();
				if (!g_MovableMan.SetAsideWorld(s_rbProbeOriginals)) {
					std::cout << "[rbprobe] FAIL: world set-aside refused" << std::endl;
					System::SetQuit(true);
					break;
				}
				const auto worldRestoreStart = std::chrono::steady_clock::now();
				if (!g_MovableMan.RestoreWorld(s_rbProbeWorld)) {
					std::cout << "[rbprobe] FAIL: world restore refused" << std::endl;
					g_MovableMan.ReinstateWorld(s_rbProbeOriginals);
					System::SetQuit(true);
					break;
				}
				const double worldRestoreMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - worldRestoreStart).count();
				std::string rewindError;
				if (ScenarioRunner::IsLockstepReplayPlayback() &&
				    !ScenarioRunner::RewindReplayForProbe(static_cast<uint64_t>(s_rbProbeSimCount) + 1, &rewindError)) {
					std::cout << "[rbprobe] FAIL: replay rewind refused: " << rewindError << std::endl;
					System::SetQuit(true);
					break;
				}
				const double restoreMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - restoreStart).count();
				std::cout << "[rbprobe] restore_ms=" << restoreMs << " world_ms=" << worldRestoreMs << std::endl;
				CheckRestoredDeepState();
				CheckRestoredScriptGraphs();
				DumpTerrainNow("rb_res");
				s_rbProbePhase = 3;
				std::cout << "[rbprobe] restored in memory and rewound to tick " << s_rbProbeSimCount << std::endl;
			}
			if (g_ActivityMan.ActivitySetToRestart() || s_rbProbeLaunchRestorePending) {
				g_LoadingScreen.DrawLoadingSplash();
				g_WindowMan.UploadFrame();
				const bool restarted = s_rbProbeLaunchRestorePending ? g_ActivityMan.LoadAndLaunchGame(RollbackProbeSaveName()) : g_ActivityMan.RestartActivity();
				if (s_rbProbeLaunchRestorePending) {
					s_rbProbeLaunchRestorePending = false;
					g_ActivityMan.RemoveSavedGame(RollbackProbeSaveName());
				}
				if (!restarted) {
					if (s_rbProbePhase == 2) {
						++s_rbProbeFailCount;
						s_rbProbeFirstFailure = "the saved activity could not restart";
						std::cout << "[rbprobe] FAIL: " << s_rbProbeFirstFailure << std::endl;
						System::SetQuit(true);
					}
					break;
				}
				if (s_rbProbePhase == 2) {
					std::string rewindError;
					if (ScenarioRunner::IsLockstepReplayPlayback() &&
					    !ScenarioRunner::RewindReplayForProbe(static_cast<uint64_t>(s_rbProbeSimCount) + 1, &rewindError)) {
						std::cout << "[rbprobe] FAIL: replay rewind refused: " << rewindError << std::endl;
						System::SetQuit(true);
						break;
					}
					CheckRestoredDeepState();
					CheckRestoredScriptGraphs();
					DumpTerrainNow("rb_res");
					s_rbProbePhase = 3;
					std::cout << "[rbprobe] restored and rewound to tick " << s_rbProbeSimCount << std::endl;
				}
			}
			if (g_ActivityMan.ActivitySetToResume()) {
				g_ActivityMan.ResumeActivity();
				g_PerformanceMan.ResetSimUpdateTimer();
				updateStartTime = g_TimerMan.GetAbsoluteTime();
			}
			if (ScenarioRunner::GetArgs().freeRunSim) {
				break;
			}
		}

		if (returnToMenuAfterNetworkEnd && !System::IsSetToQuit()) {
			g_TimerMan.PauseSim(true);
			if (!g_ActivityMan.ActivitySetToRestart()) {
				g_MenuMan.HandleTransitionIntoMenuLoop();
				RunMenuLoop();
			}
			continue;
		}

		updateEndAndDrawStartTime = g_TimerMan.GetAbsoluteTime();
		updateTotalTime = updateEndAndDrawStartTime - updateStartTime;
		drawStartTime = updateEndAndDrawStartTime;

		// Frame rendering must not advance the sim RNG stream or feed the MOID grid — its cadence is
		// host frame-rate dependent, so redirect cosmetic draws to the render RNG and suspend
		// MOID-grid registration for the frame.
		LocalPrediction::RunPreview();

		{
			RandomGenerator* prevSimRNG = t_simRNGOverride;
			t_simRNGOverride = &g_RenderRNG;
			g_SceneMan.SetRenderDrawContext(true);
			g_UInputMan.Update();
			g_MenuMan.UpdateNetworkUI();
			g_ActivityMan.RenderUpdate();
			g_UInputMan.EndFrame();
			g_SceneMan.SetRenderDrawContext(false);
			t_simRNGOverride = prevSimRNG;
		}
		if (!freeRunLockstep) {
			DrawFrameWithPreviews();
		}

		drawTotalTime = g_TimerMan.GetAbsoluteTime() - drawStartTime;
		g_PerformanceMan.UpdateMSPF(updateTotalTime, drawTotalTime);

		// Both ends of the iteration must be in a RUNNING match, or the teardown drain and
		// menu-transition iterations poison the averages.
		if (paceActiveAtIterStart && ScenarioRunner::IsLockstepControllerSyncActive()) {
			++s_paceIterations;
			s_paceUpdateUs += updateTotalTime;
			s_paceDrawUs += drawTotalTime;
		}
	}
}

/// <summary>
/// Self-invoking lambda that installs exception handlers before Main is executed.
/// </summary>
static const bool RTESetExceptionHandlers = []() {
	RTEError::SetExceptionHandlers();
	return true;
}();

bool NetSessionCliRequested() {
	return (s_netHost || !s_netJoinAddress.empty()) && !NetGameplayRequested() && !s_netMatchServiceE2E;
}

bool WriteNetSessionReport(const NetSession& session, const std::string& path, std::string* error) {
	if (path.empty()) {
		return true;
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) *error = "could not open report path '" + path + "'";
		return false;
	}
	out << session.BuildReportJson() << '\n';
	if (!out) {
		if (error) *error = "could not write report path '" + path + "'";
		return false;
	}
	return true;
}

NetSessionConfig BuildNetSessionCliConfig(const NetIdentityManifest& manifest, bool host) {
	NetSessionConfig config;
	config.localIdentity = manifest;
	config.displayName = host ? "Host" : "Client";
	config.port = s_netPort;
	config.sessionId = 0x5354414745325032ULL;
	config.localNonce = host ? 0x535441474532484FULL : 0x535441474532434CULL;
	config.maxPeers = 1;
	config.heartbeatIntervalMs = 50;
	config.timeoutMs = 5000;
	config.rejectUserdataModules = !s_netAllowUserdata && !s_netMatch;
	return config;
}

NetMatchConfig BuildNetMatchCliConfig(bool useLobbyProtocol) {
	NetMatchConfig config = NetMatchConfigUtil::MakeDefault(0x5354414745325032ULL);
	config.activityPreset = ScenarioRunner::ResolvePresetName(ScenarioRunner::GetArgs().scenario);
	config.inputDelayFrames = s_netLockstepInputDelay;
	config.modePreset = useLobbyProtocol ? "PvP" : "P3";
	if (useLobbyProtocol) {
		NetActorOwnershipPolicy policy;
		if (NetMatchConfigUtil::ParseOwnershipPolicy(s_netMatchOwnershipPolicy, policy)) {
			config.ownershipPolicy = policy;
		}
	} else {
		config.ownershipPolicy = NetActorOwnershipPolicy::UniqueIdModPeerCount;
	}
	return config;
}

int RunNetSessionCli() {
	if (s_netHost && !s_netJoinAddress.empty()) {
		std::cerr << "[net-session] choose either -net-host or -net-join, not both" << std::endl;
		return 1;
	}

	g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);

	NetIdentityManifest manifest;
	NetIdentityBuildOptions identityOptions;
	identityOptions.buildId = "stage2-p2d-local";
	identityOptions.sessionRulesTag = "stage2-p2-session-rules";
	std::string error;
	if (!NetIdentity::BuildCurrentManifest(manifest, &error, identityOptions)) {
		std::cerr << "[net-session] identity build failed: " << error << std::endl;
		return 1;
	}

	GnsTransport transport;
	NetSession session;
	NetSessionConfig config = BuildNetSessionCliConfig(manifest, s_netHost);
	const bool started = s_netHost
		? session.StartHost(transport, std::move(config), &error)
		: session.StartClient(transport, s_netJoinAddress, std::move(config), &error);

	if (!started) {
		std::cerr << "[net-session] start failed: " << error << std::endl;
		std::string reportError;
		if (!WriteNetSessionReport(session, s_netSessionReportPath, &reportError)) {
			std::cerr << "[net-session] report failed: " << reportError << std::endl;
		}
		return 1;
	}

	std::cout << "[net-session] " << (s_netHost ? "hosting" : "joining")
	          << " port=" << s_netPort
	          << " gns_compiled=" << (GnsTransport::IsCompiledIn() ? "true" : "false")
	          << " allow_userdata=" << (s_netAllowUserdata ? "true" : "false")
	          << std::endl;

	const auto startTime = std::chrono::steady_clock::now();
	constexpr uint64_t c_MaxRunMs = 15000;
	constexpr uint64_t c_ReadySettleMs = 250;
	bool sawReady = false;

	while (true) {
		const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - startTime).count());
		session.Tick(nowMs);

		if (session.IsRejected()) {
			if (s_netHost) {
				std::this_thread::sleep_for(std::chrono::milliseconds(c_ReadySettleMs));
			}
			break;
		}
		if (session.IsFailed() || session.IsClosed()) {
			break;
		}
		if (session.IsReady()) {
			if (!sawReady) {
				sawReady = true;
				std::cout << "[net-session] ready" << std::endl;
			}
			if (s_netExitAfterReady && !s_netHost) {
				std::this_thread::sleep_for(std::chrono::milliseconds(c_ReadySettleMs));
			}
			break;
		}
		if (nowMs > c_MaxRunMs) {
			std::cerr << "[net-session] timed out waiting for ready" << std::endl;
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	std::string reportError;
	if (!WriteNetSessionReport(session, s_netSessionReportPath, &reportError)) {
		std::cerr << "[net-session] report failed: " << reportError << std::endl;
		return 1;
	}

	const bool passed = session.IsReady();
	std::cout << "[net-session] final_state=" << NetSession::StateName(session.GetState())
	          << " accepted=" << (passed ? "true" : "false")
	          << " report=" << (s_netSessionReportPath.empty() ? "<none>" : s_netSessionReportPath)
	          << std::endl;
	return passed ? 0 : 1;
}

std::string JsonEscape(const std::string& value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (char c : value) {
		switch (c) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default: escaped += c; break;
		}
	}
	return escaped;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string* error) {
	if (path.empty()) {
		return true;
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) *error = "could not open report path '" + path + "'";
		return false;
	}
	out << text << '\n';
	if (!out) {
		if (error) *error = "could not write report path '" + path + "'";
		return false;
	}
	return true;
}

bool PrepareNetLockstepScenario(GnsTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, NetMatchRunner& runner, std::string* error) {
	if (!NetGameplayRequested()) {
		return true;
	}
	if (!ScenarioRunner::IsActive()) {
		if (error) *error = "network gameplay requires -scenario";
		return false;
	}
	if (s_netLockstep && s_netMatch) {
		if (error) *error = "choose either -net-lockstep or -net-match, not both";
		return false;
	}
	if (s_netHost == !s_netJoinAddress.empty()) {
		if (error) *error = "network gameplay requires exactly one of -net-host or -net-join <address>";
		return false;
	}
	if (s_netLockstepInputDelay != 0) {
		if (error) *error = s_netMatch ? "local alpha gameplay currently requires -net-match-input-delay 0" : "P3 gameplay lockstep currently requires -net-lockstep-input-delay 0";
		return false;
	}

	g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);

	NetIdentityManifest manifest;
	NetIdentityBuildOptions identityOptions;
	identityOptions.buildId = "stage2-p2d-local";
	identityOptions.sessionRulesTag = "stage2-p2-session-rules";
	if (!NetIdentity::BuildCurrentManifest(manifest, error, identityOptions)) {
		return false;
	}

	NetMatchRunnerConfig runnerConfig;
	runnerConfig.host = s_netHost;
	runnerConfig.joinAddress = s_netJoinAddress;
	runnerConfig.sessionConfig = BuildNetSessionCliConfig(manifest, s_netHost);
	runnerConfig.matchConfig = BuildNetMatchCliConfig(s_netMatch);
	runnerConfig.useLobbyProtocol = s_netMatch;
	runnerConfig.startFrame = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) + 1U;
	runnerConfig.scenario = ScenarioRunner::ResolvePresetName(ScenarioRunner::GetArgs().scenario);
	runnerConfig.lockstepWaitMs = 5000;

	const char* tag = s_netMatch ? "[net-match]" : "[net-lockstep]";
	std::cout << tag << " " << (s_netHost ? "hosting" : "joining")
	          << " port=" << s_netPort
	          << " gns_compiled=" << (GnsTransport::IsCompiledIn() ? "true" : "false")
	          << " allow_userdata=" << (s_netAllowUserdata ? "true" : "false")
	          << " lobby=" << (s_netMatch ? "true" : "false")
	          << std::endl;
	if (!runner.Start(transport, session, coordinator, runnerConfig, error)) {
		return false;
	}
	std::string reportError;
	if (!WriteNetSessionReport(session, s_netSessionReportPath, &reportError)) {
		std::cerr << tag << " session report failed: " << reportError << std::endl;
	}

	ScenarioRunner::SetLockstepCoordinator(&coordinator);
	std::cout << tag << " running local_peer=" << static_cast<int>(coordinator.GetConfig().localPeerId)
	          << " remote_peer=" << static_cast<int>(coordinator.GetConfig().remotePeerId)
	          << " start_frame=" << coordinator.GetConfig().startFrame
	          << " match_config_hash=" << NetIdentity::HashHex(runner.GetMatchConfigHash()) << std::endl;
	return true;
}

std::string BuildControllerBoundaryJson();

std::string BuildNetLockstepReportJson(const NetSession& session, const NetLockstepCoordinator& coordinator, const NetMatchRunner& runner, int scenarioExitCode, const std::string& setupError) {
	const MetricsCollector::AggregatedRun run = g_MetricsCollector.GetCurrentRun();
	const NetLockstepStats& stats = coordinator.GetStats();
	const bool completed = stats.timeoutReason.rfind("Complete:", 0) == 0;
	const char* finalState = !setupError.empty() ? "Failed" : NetLockstepCoordinator::StateName(coordinator.GetState());
	std::ostringstream out;
	out << "{";
	out << "\"final_state\":\"" << finalState << "\",";
	out << "\"setup_error\":\"" << JsonEscape(setupError) << "\",";
	out << "\"scenario_exit_code\":" << scenarioExitCode << ",";
	out << "\"session_role\":\"" << NetSession::RoleName(session.GetRole()) << "\",";
	out << "\"session_peer_id\":" << static_cast<int>(session.GetLocalPeerId()) << ",";
	out << "\"lockstep_peer_id\":" << static_cast<int>(stats.localPeerId) << ",";
	out << "\"scenario\":\"" << JsonEscape(ScenarioRunner::GetArgs().scenario) << "\",";
	out << "\"uses_lobby_protocol\":" << (runner.UsesLobbyProtocol() ? "true" : "false") << ",";
	out << "\"match_runtime_state\":\"" << NetMatchRunner::StateName(runner.GetState()) << "\",";
	out << "\"match_config_hash\":\"" << JsonEscape(NetIdentity::HashHex(runner.GetMatchConfigHash())) << "\",";
	out << "\"ownership_policy\":\"" << JsonEscape(NetMatchConfigUtil::OwnershipPolicyName(runner.GetMatchConfig().ownershipPolicy)) << "\",";
	out << "\"input_delay_frames\":" << stats.inputDelayFrames << ",";
	out << "\"controller_boundary\":" << BuildControllerBoundaryJson() << ",";
	out << "\"frames_planned\":" << (s_netLockstepTicks > 0 ? s_netLockstepTicks : ScenarioRunner::GetArgs().maxTicks) << ",";
	out << "\"frames_sent\":" << stats.framePacketsSent << ",";
	out << "\"frames_received\":" << stats.framePacketsReceived << ",";
	out << "\"frames_accepted\":" << stats.framesAccepted << ",";
	out << "\"local_controller_frames_sent\":" << stats.localControllerFramesSent << ",";
	out << "\"remote_controller_frames_received\":" << stats.remoteControllerFramesReceived << ",";
	out << "\"remote_controller_frames_accepted\":" << stats.remoteControllerFramesAccepted << ",";
	out << "\"frames_simulated\":" << run.ticks << ",";
	out << "\"duplicate_frames\":" << stats.duplicateFrames << ",";
	out << "\"out_of_order_frames\":" << stats.outOfOrderFrames << ",";
	out << "\"missing_frame_stalls\":" << stats.missingFrameStalls << ",";
	out << "\"lockstep_stop_reason\":\"" << JsonEscape(stats.timeoutReason) << "\",";
	out << "\"stall_timeout_reason\":\"" << JsonEscape(completed ? "" : stats.timeoutReason) << "\",";
	out << "\"first_desync_tick\":-1,";
	out << "\"first_desync_subsystem\":\"\",";
	out << "\"final_total_hash\":\"" << JsonEscape(run.finalTotalHashHex) << "\",";
	out << "\"controller_frame_actor_state_payload\":true,";
	out << "\"controller_frame_payload_note\":\"controls plus pose/equip/aim/facing/hand/device state\",";
	out << "\"match_config\":" << NetMatchConfigUtil::BuildReportJson(runner.GetMatchConfig()) << ",";
	out << "\"session\":" << session.BuildReportJson() << ",";
	out << "\"lockstep\":" << coordinator.BuildReportJson() << ",";
	out << "\"match_runner\":" << runner.BuildReportJson(session, coordinator);
	out << "}";
	return out.str();
}

const char* ActivityStateName(Activity::ActivityState state) {
	switch (state) {
		case Activity::NoActivity: return "NoActivity";
		case Activity::NotStarted: return "NotStarted";
		case Activity::Starting: return "Starting";
		case Activity::Editing: return "Editing";
		case Activity::PreGame: return "PreGame";
		case Activity::Running: return "Running";
		case Activity::HasError: return "HasError";
		case Activity::Over: return "Over";
	}
	return "Unknown";
}

// How this peer's AI pass crossed the controller boundary; a direct write is a boundary violation.
std::string BuildControllerBoundaryJson() {
	const MovableMan::ControllerBoundaryStats& stats = g_MovableMan.GetControllerBoundaryStats();
	std::ostringstream out;
	out << "{\"equip_commands\":" << stats.equipCommands << ",\"sound_commands\":" << stats.soundCommands << ",\"aim_intents\":" << stats.aimIntents
	    << ",\"flip_intents\":" << stats.flipIntents << ",\"direct_writes\":" << stats.directWrites << "}";
	return out.str();
}

// The in-match loop pace: wall_tps is the number that answers "does the match run at the pinned
// dt's intended rate", sim_ms_per_tick is the full-tick compute cost (and, in playback, the
// rollback re-sim cost — playback runs no AI).
std::string BuildLoopPaceJson() {
	const long long wallUs = s_paceUpdateUs + s_paceDrawUs;
	std::ostringstream out;
	out << "{";
	out << "\"iterations\":" << s_paceIterations << ",";
	out << "\"sim_ticks\":" << s_paceSimTicks << ",";
	out << "\"wall_ms\":" << wallUs / 1000 << ",";
	out << "\"sim_ms\":" << s_paceSimUs / 1000 << ",";
	out << "\"draw_ms\":" << s_paceDrawUs / 1000 << ",";
	out << "\"wall_tps\":" << (wallUs > 0 ? static_cast<double>(s_paceSimTicks) * 1000000.0 / static_cast<double>(wallUs) : 0.0) << ",";
	out << "\"sim_ms_per_tick\":" << (s_paceSimTicks > 0 ? static_cast<double>(s_paceSimUs) / 1000.0 / static_cast<double>(s_paceSimTicks) : 0.0) << ",";
	out << "\"draw_ms_per_iter\":" << (s_paceIterations > 0 ? static_cast<double>(s_paceDrawUs) / 1000.0 / static_cast<double>(s_paceIterations) : 0.0) << ",";
	out << "\"net_wait_ms\":" << ScenarioRunner::GetLockstepWaitUs() / 1000 << ",";
	const double ticksPerMs = static_cast<double>(g_TimerMan.GetTicksPerSecond()) / 1000.0;
	out << "\"accrued_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceAccruedTicks()) / ticksPerMs) << ",";
	out << "\"trimmed_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceTrimmedTicks()) / ticksPerMs) << ",";
	out << "\"wall_seen_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceWallSeenTicks()) / ticksPerMs) << ",";
	out << "\"cap_lost_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceCapLostTicks()) / ticksPerMs) << ",";
	out << "\"paused_lost_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPacePausedLostTicks()) / ticksPerMs) << ",";
	out << "\"update_calls\":" << g_TimerMan.GetPaceUpdateCalls() << ",";
	out << "\"reset_calls\":" << g_TimerMan.GetPaceResetCalls() << ",";
	out << "\"time_scale\":" << g_TimerMan.GetTimeScale();
	out << "}";
	return out.str();
}

std::string BuildNetMatchServiceE2EReportJson(int exitCode, const std::string& setupError) {
	const Activity* activity = g_ActivityMan.GetActivity();
	const Activity::ActivityState activityState = activity ? activity->GetActivityState() : Activity::NoActivity;
	std::ostringstream out;
	out << "{";
	out << "\"exit_code\":" << exitCode << ",";
	out << "\"setup_error\":\"" << JsonEscape(setupError) << "\",";
	out << "\"runtime_error\":\"" << JsonEscape(s_netMatchServiceE2EError) << "\",";
	out << "\"activity_preset\":\"" << JsonEscape(s_netMatchServiceE2EPreset) << "\",";
	out << "\"activity_state\":\"" << ActivityStateName(activityState) << "\",";
	const GameActivity* reportGameActivity = dynamic_cast<const GameActivity*>(activity);
	out << "\"winner_team\":" << (reportGameActivity ? reportGameActivity->GetWinnerTeam() : Activity::NoTeam) << ",";
	out << "\"entered_editor\":" << (s_netMatchServiceE2EEnteredEditor ? "true" : "false") << ",";
	out << "\"rematches\":" << s_netMatchServiceE2ERematches << ",";
	out << "\"resyncs\":" << s_netMatchResyncs << ",";
	// The actor census guards against sim-CONSISTENT duplication (both peers doubling identically
	// slips every divergence gate); the peak catches a double-spawn that later sheds back to normal.
	out << "\"actors\":" << s_netMatchE2EActorCensus << ",";
	out << "\"actors_peak\":" << s_netMatchE2EActorCensusPeak << ",";
	out << "\"pace\":" << BuildLoopPaceJson() << ",";
	out << "\"running_ticks\":" << s_netMatchE2ETicks.Total() << ",";
	out << "\"frames_planned\":" << (s_netLockstepTicks > 0 ? s_netLockstepTicks : 600) << ",";
	out << "\"local_prediction\":{\"enabled\":" << (LocalPrediction::IsEnabled() ? "true" : "false")
	    << ",\"previews\":" << LocalPrediction::GetPreviewCount() << ",\"actor_ticks\":" << LocalPrediction::GetPreviewTicks()
	    << ",\"ms_total\":" << LocalPrediction::GetPreviewMs() << ",\"shadows\":" << LocalPrediction::GetShadows() << ",\"taken\":" << LocalPrediction::GetTaken()
	    << ",\"violations\":" << LocalPrediction::GetViolations() << "},";
	out << "\"controller_boundary\":" << BuildControllerBoundaryJson() << ",";
	out << "\"replay_recording\":{\"frames\":" << ScenarioRunner::GetLockstepReplayRecordFrames()
	    << ",\"closed\":" << (ScenarioRunner::WasLockstepReplayRecordClosed() ? "true" : "false") << "},";
	out << "\"setup_surface\":\"fixed-alpha-duel\",";
	out << "\"unsupported_setup_surface\":\"stock pregame editor/deployment/buy-menu setup is not synchronized in P4A\",";
	out << "\"service\":" << g_NetMatchService.BuildReportJson();
	out << "}";
	return out.str();
}

bool StageResyncedMatchActivity(std::string* error) {
	return g_NetMatchService.StageResyncedMatchLaunch(error);
}

bool ConfigureNetMatchActivity(const std::string& activityPreset, int localTeam, std::string* error) {
	const Entity* presetEntity = g_PresetMan.GetEntityPreset("GAScripted", activityPreset);
	const Activity* presetActivity = dynamic_cast<const Activity*>(presetEntity);
	if (!presetActivity) {
		if (error) *error = "could not find multiplayer activity preset";
		return false;
	}
	if (!presetActivity->GetSceneName().empty()) {
		g_SceneMan.SetSceneToLoad(presetActivity->GetSceneName(), true, false);
	}
	Activity* activity = dynamic_cast<Activity*>(presetActivity->Clone());
	if (!activity) {
		if (error) *error = "could not create multiplayer activity";
		return false;
	}
	if (localTeam < Activity::TeamOne || localTeam >= Activity::MaxTeamCount) {
		delete activity;
		if (error) *error = "invalid local team";
		return false;
	}
	if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(activity)) {
		gameActivity->ClearPlayers(false);
		gameActivity->AddPlayer(Players::PlayerOne, true, localTeam, 0);
		// Activate every team in the synced roster so all peers run the identical team set.
		for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
			if (team == localTeam || ScenarioRunner::IsLockstepActiveTeam(team)) {
				gameActivity->ForceSetTeamAsActive(team);
				gameActivity->SetTeamFunds(0, team);
			}
		}
	}
	ScenarioRunner::ApplyDeterministicConfig();
	g_ActivityMan.SetStartActivity(activity);
	g_ActivityMan.SetRestartActivity(true);
	return true;
}

bool ConfigureNetMatchServiceE2EActivity(const std::string& activityPreset, std::string* error) {
	return ConfigureNetMatchActivity(activityPreset, g_NetMatchService.GetLocalTeam(), error);
}

// Drives a recorded match through the standard lockstep apply path: a no-remote coordinator over
// a dead-end transport, fed tick records by the replay reader. The deterministic sim reproduces
// the match, so a -tick-hashes trace must equal the recording peer's.
int RunNetReplayPlayback() {
	std::string setupError;
	if (!ScenarioRunner::SetLockstepReplaySource(s_netReplayInPath, &setupError)) {
		std::cerr << "[net-replay] " << setupError << std::endl;
		return 1;
	}
	const NetMatchConfig& replayConfig = ScenarioRunner::GetLockstepReplayConfig();
	std::cout << "[net-replay] playing back " << s_netReplayInPath << ": " << replayConfig.activityPreset
	          << ", " << static_cast<int>(replayConfig.peerCount) << " peers" << std::endl;

	static NullNetTransport s_nullTransport;
	static NetLockstepCoordinator s_replayCoordinator;
	NetLockstepConfig lockstepConfig;
	lockstepConfig.sessionId = replayConfig.sessionId;
	// Align to the recording's first tick, whatever sim count its match began on.
	lockstepConfig.startFrame = ScenarioRunner::GetLockstepReplayStartFrame();
	lockstepConfig.localPeerId = 1;
	lockstepConfig.peerCount = replayConfig.peerCount;
	lockstepConfig.scenario = "replay";
	lockstepConfig.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(replayConfig.ownershipPolicy);
	lockstepConfig.matchConfig = replayConfig;
	if (!s_replayCoordinator.StartReplay(s_nullTransport, lockstepConfig, &setupError)) {
		std::cerr << "[net-replay] " << setupError << std::endl;
		return 1;
	}
	ScenarioRunner::SetLockstepCoordinator(&s_replayCoordinator);

	// Watch through the first human seat; per-peer view bindings are off-sim.
	int localTeam = Activity::TeamOne;
	for (const NetMatchPlayerSlot& slot: replayConfig.players) {
		if (!slot.cpu) {
			localTeam = slot.team;
			break;
		}
	}
	if (!ConfigureNetMatchActivity(replayConfig.activityPreset, localTeam, &setupError)) {
		std::cerr << "[net-replay] setup failed: " << setupError << std::endl;
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		return 1;
	}

	const bool traceRun = s_recordTickHashes && !ScenarioRunner::GetArgs().outPath.empty();
	if (traceRun) {
		g_MetricsCollector.BeginRun("P4 Alpha Duel", ScenarioRunner::GetArgs().seed);
		g_MetricsCollector.SetRecordTickHashes(true);
	}
	// Playback is not real-time: free-run the sim as fast as it computes (the fixed dt is
	// untouched). The wall/tick numbers this yields ARE the rollback re-sim budget.
	g_TimerMan.SetFreeRunSim(true);
	const auto playbackStart = std::chrono::steady_clock::now();
	RunGameLoop();
	CheckRequiredProbesCompleted();
	const auto playbackMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - playbackStart).count();
	using ReplayOutcome = ScenarioRunner::LockstepReplayOutcome;
	const ReplayOutcome finalOutcome = ScenarioRunner::GetLockstepReplayOutcome();
	if (traceRun) {
		g_MetricsCollector.RecordString("replay_outcome", ScenarioRunner::ReplayOutcomeName(finalOutcome));
		g_MetricsCollector.Record("replay_frames_consumed", static_cast<double>(ScenarioRunner::GetLockstepReplayFramesConsumed()));
		g_MetricsCollector.Record("replay_last_tick", static_cast<double>(ScenarioRunner::GetLockstepReplayLastTick()));
		g_MetricsCollector.Record("replay_end_marker", ScenarioRunner::LockstepReplaySawEndMarker() ? 1.0 : 0.0);
		g_MetricsCollector.Record("replay_exit_code", static_cast<double>(s_netReplayExitCode));
		if (finalOutcome == ReplayOutcome::Completed || finalOutcome == ReplayOutcome::TickCap) {
			g_MetricsCollector.RecordString("controller_replay_error", "");
		}
		g_MetricsCollector.SetResult(s_netReplayExitCode == 0);
		g_MetricsCollector.EndRun();
		const std::string& tracePath = ScenarioRunner::GetArgs().outPath;
		if (!g_MetricsCollector.WriteReport(tracePath)) {
			std::cerr << "[net-replay] trace write failed: " << tracePath << std::endl;
			s_netReplayExitCode = 1;
		} else {
			std::cout << "[net-replay] wrote trace: " << tracePath << std::endl;
		}
	}
	std::cout << "[net-replay] playback " << (s_netReplayExitCode == 0 ? "finished" : "FAILED") << " in " << playbackMs
	          << "ms, ticks=" << s_netReplayTicks << " outcome=" << ScenarioRunner::ReplayOutcomeName(finalOutcome)
	          << " frames=" << ScenarioRunner::GetLockstepReplayFramesConsumed() << " last_tick=" << ScenarioRunner::GetLockstepReplayLastTick()
	          << " end_marker=" << (ScenarioRunner::LockstepReplaySawEndMarker() ? 1 : 0) << " exit=" << s_netReplayExitCode << std::endl;
	std::cout << "[pace] " << BuildLoopPaceJson() << std::endl;
	if (const std::string stats = LocalPrediction::DescribeStats(); !stats.empty()) {
		std::cout << "[localpred] " << stats << std::endl;
	}
	ScenarioRunner::SetLockstepCoordinator(nullptr);
	return s_netReplayExitCode;
}

int RunNetMatchServiceE2E() {
	std::string setupError;
	if (!NetA7Journal::StartE2E(&setupError, [] { PollSDLEvents(); return System::IsSetToQuit(); })) s_netMatchServiceE2EExitCode = 1;
	if (s_netHost == !s_netJoinAddress.empty()) {
		setupError = "-net-match-service-e2e requires exactly one of -net-host or -net-join <address>";
	}

	if (setupError.empty()) {
		ScenarioRunner::ApplyDeterministicConfig();
		NetMatchServiceRequest request;
		request.host = s_netHost;
		request.address = s_netJoinAddress.empty() ? "127.0.0.1" : s_netJoinAddress;
		request.port = s_netPort;
		request.playerName = s_netHost ? "Host" : "Client";
		request.activityPreset = s_netMatchServiceE2EPreset;
		request.ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
		request.inputDelayFrames = s_netLockstepInputDelay;
		request.peerCount = s_netMatchPeers;
		NetMatchMode parsedMode;
		if (NetMatchConfigUtil::ParseMode(s_netMatchMode, parsedMode)) {
			request.mode = parsedMode;
		}
		request.resyncOnDesync = s_netMatchResyncOnDesync;
		request.autoInputDelay = s_netMatchAutoDelay;
		if (!g_NetMatchService.Start(request, &setupError)) {
			s_netMatchServiceE2EExitCode = 1;
		}
	}

	std::string activityPreset;
	if (setupError.empty()) {
		g_NetMatchService.SetReady();
		if (s_netHost) {
			g_NetMatchService.RequestStart();
		}
		const auto waitStart = std::chrono::steady_clock::now();
		while (!g_NetMatchService.ConsumeReadyToLaunch(activityPreset)) {
			const NetMatchServiceState state = g_NetMatchService.GetState();
			if (s_netHost && ScenarioRunner::GetArgs().selftestJoinRejection && state == NetMatchServiceState::Starting) {
				const std::string rejection = g_NetMatchService.GetErrorText();
				if (!rejection.empty()) {
					setupError = rejection;
					s_netMatchServiceE2EExitCode = 1;
					break;
				}
			}
			if (state == NetMatchServiceState::Failed) {
				setupError = g_NetMatchService.GetErrorText();
				s_netMatchServiceE2EExitCode = 1;
				break;
			}
			const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - waitStart).count());
			if (nowMs > 60000) {
				setupError = "timed out waiting for service launch";
				s_netMatchServiceE2EExitCode = 1;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	if (setupError.empty()) {
		const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
		std::cout << "[net-match-service-e2e] lobby_snapshot: state=" << snapshot.serviceState
				  << " is_host=" << (snapshot.isHost ? 1 : 0) << " members=" << snapshot.members.size()
				  << " local_ready=" << (snapshot.localReady ? 1 : 0) << " remote_ready=" << (snapshot.remoteReady ? 1 : 0)
				  << " activity=" << snapshot.activityPreset << " scene=" << snapshot.sceneName << " mode=" << snapshot.modeName;
		for (const NetLobbyMember& member: snapshot.members) {
			std::cout << " | peer" << static_cast<int>(member.peerId) << "=" << member.displayName
					  << "(team" << static_cast<int>(member.team) << (member.isLocal ? ",local" : ",remote")
					  << (member.ready ? ",ready" : ",notready") << ",ping" << member.pingMs << "ms)";
		}
		std::cout << std::endl;
	}

	if (setupError.empty()) {
		// A reconnecting peer's first lobby round carried the live match's snapshot; launch from it.
		const bool staged = g_NetMatchService.HasPendingResyncLoad()
			? StageResyncedMatchActivity(&setupError)
			: ConfigureNetMatchServiceE2EActivity(activityPreset, &setupError);
		if (!staged) {
			s_netMatchServiceE2EExitCode = 1;
		}
	}

	if (setupError.empty()) {
		// Per-tick trace for the host/client sim-gated compare. Not SetActive() — that also arms the
		// -scenario stop path + perturb hook; SetRecordTickHashes arms the trace alone.
		const bool traceRun = s_recordTickHashes && !ScenarioRunner::GetArgs().outPath.empty();
		if (traceRun) {
			g_MetricsCollector.BeginRun("P4 Alpha Duel", ScenarioRunner::GetArgs().seed);
			g_MetricsCollector.SetRecordTickHashes(true);
		}
		RunGameLoop();
		// A leave is answered on the service worker, and the report below must describe the settled
		// exchange rather than one still in flight.
		g_NetMatchService.WaitForPendingWork();
		CheckRequiredProbesCompleted();
		if (s_netReplayExitCode != 0 && s_netMatchServiceE2EExitCode == 0) {
			s_netMatchServiceE2EExitCode = s_netReplayExitCode;
		}
		if (traceRun) {
			g_MetricsCollector.EndRun();
			const std::string& tracePath = ScenarioRunner::GetArgs().outPath;
			if (!g_MetricsCollector.WriteReport(tracePath)) {
				std::cerr << "[net-match-service-e2e] trace write failed: " << tracePath << std::endl;
				s_netMatchServiceE2EExitCode = 1;
			} else {
				std::cout << "[net-match-service-e2e] wrote trace: " << tracePath << std::endl;
			}
		}
	} else {
		std::cerr << "[net-match-service-e2e] setup failed: " << setupError << std::endl;
	}

	std::string reportError;
	const int exitCode = setupError.empty() ? s_netMatchServiceE2EExitCode : 1;
	const bool a7ReportSettled = !NetA7Journal::Enabled() || g_NetMatchService.CanSealA7Journal();
	if (!a7ReportSettled) {
		NetA7Journal::Gap("service worker still active before report generation");
		(void)NetA7Journal::Seal("", 1);
		return 1;
	}
	const std::string report = BuildNetMatchServiceE2EReportJson(exitCode, setupError);
	if (!WriteTextFile(s_netLockstepReportPath, report, &reportError)) {
		std::cerr << "[net-match-service-e2e] report failed: " << reportError << std::endl;
		NetA7Journal::Gap("native report write failed");
		(void)NetA7Journal::Seal("", 1);
		return 1;
	}
	if (!s_netLockstepReportPath.empty()) {
		std::cout << "[net-match-service-e2e] wrote report: " << s_netLockstepReportPath << std::endl;
		if (const std::string stats = LocalPrediction::DescribeStats(); !stats.empty()) {
			std::cout << "[localpred] " << stats << std::endl;
		}
	}
	return NetA7Journal::Seal(s_netLockstepReportPath, exitCode) ? exitCode : 1;
}

/// <summary>
/// Implementation of the main function.
/// </summary>
int main(int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		if (argv[i] != nullptr && std::string(argv[i]) == "-controller-frame-selftest") {
			return ControllerFrameSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-protocol-selftest") {
			return NetProtocolSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-identity-selftest") {
			return NetIdentitySelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-session-selftest") {
			return NetSessionSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-selftest") {
			return NetLockstepSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-match-selftest") {
			return NetMatchSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-auth-selftest") {
			return NetAuthSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-admission-selftest") {
			return NetAdmissionSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-reconnect-selftest") {
			return NetReconnectSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-reconnect-session-selftest") {
			return NetReconnectSessionSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-discovery-selftest") {
			// A beacon and a browser over the loopback broadcast: the browser must list the host.
			NetLanDiscovery beacon;
			NetLanDiscovery browser;
			std::string error;
			int exitCode = 1;
			if (!browser.StartBrowser(&error) ||
			    !beacon.StartBeacon(42120, "SelftestHost", "P4 Alpha Duel", "pvp-skirmish", 1, 4, &error)) {
				std::cerr << "[net-discovery-selftest] FAIL: " << error << std::endl;
				return 1;
			}
			for (uint64_t nowMs = 0; nowMs <= 3000 && exitCode != 0; nowMs += 50) {
				beacon.Tick(nowMs);
				browser.Tick(nowMs);
				for (const NetLanHostInfo& host: browser.GetHosts(nowMs)) {
					if (host.port == 42120 && host.hostName == "SelftestHost" && host.maxPlayers == 4 && host.mode == "pvp-skirmish") {
						exitCode = 0;
						break;
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			std::cout << "[net-discovery-selftest] " << (exitCode == 0 ? "PASS" : "FAIL") << std::endl;
			return exitCode;
		}
	}

	// Determinism-check mode is a pre-init orchestrator: it spawns child game processes and diffs
	// their JSON traces, so it must short-circuit before SDL / engine bootstrapping.
	if (DeterminismCheck::IsRequested(argc, argv)) {
		return DeterminismCheck::Run(argc, argv);
	}

	// Pick up the thread-count override before any init runs. Net-session smoke uses
	// a fixed default so identity does not depend on each platform's hardware threads.
	bool explicitLuaStateOverride = false;
	bool netSessionRequested = false;
	for (int i = 1; i < argc; ++i) {
		if (argv[i] == nullptr) {
			continue;
		}
		const std::string arg = argv[i];
		if (arg == "-num-lua-states" && i + 1 < argc) {
			s_cliNumLuaStatesOverride = static_cast<int>(std::strtol(argv[i + 1], nullptr, 10));
			explicitLuaStateOverride = true;
			++i;
		} else if (arg == "-net-host" || arg == "-net-join") {
			netSessionRequested = true;
		}
	}
	if (netSessionRequested && !explicitLuaStateOverride) {
		s_cliNumLuaStatesOverride = c_NetSessionDefaultLuaStates;
	}

	// Headless: -tick-hashes (the determinism trace mode, set on every -determinism-check child)
	// has nothing worth displaying, so create the window hidden. -headless forces it; -headed forces
	// a visible window. WindowMan reads CCCP_HEADLESS.
	{
		bool headless = false;
		for (int i = 1; i < argc; ++i) {
			if (argv[i] == nullptr) {
				continue;
			}
			const std::string arg = argv[i];
			if (arg == "-tick-hashes" || arg == "-headless" || arg == "-net-host" || arg == "-net-join" || arg == "-net-lockstep" || arg == "-net-match" || arg == "-net-match-service-e2e") {
				headless = true;
			} else if (arg == "-headed") {
				headless = false;
				break;
			}
		}
		if (headless) {
#ifdef _WIN32
			_putenv_s("CCCP_HEADLESS", "1");
			// Suppress the CRT's modal abort()/assert dialogs so an automated run exits to the log
			// instead of blocking on a message box no one is there to dismiss.
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
			_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#elif defined(__APPLE__)
			// macOS: SDL's offscreen driver loads no GL on Darwin, so use a hidden real-GL window instead.
			setenv("CCCP_HEADLESS", "1", 1);
#else
			setenv("SDL_VIDEODRIVER", "offscreen", 1);
#endif
		}
	}

	install_allegro(SYSTEM_NONE, &errno, std::atexit);
	loadpng_init();

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD );

	SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
	SDL_SetHint("SDL_ALLOW_TOPMOST", "0");
	SDL_HideCursor();

	if (std::filesystem::exists("Base.rte/gamecontrollerdb.txt")) {
		SDL_AddGamepadMappingsFromFile("Base.rte/gamecontrollerdb.txt");
	}

#ifdef WIN32
	// Stops framespiking from our child threads being sat on for too long
	// TODO: use a better thread system that'll do what we want ASAP instead of letting the OS schedule all over us
	// Disabled for now because windows is great and this means when the game lags out it freezes the entire computer. Which we wouldn't expect with anything but REALTIME priority.
	// Because apparently high priority class is preferred over "processing mouse input"?!
	// SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif // WIN32

	// argv[0] actually unreliable for exe path and name, because of course, why would it be, why would anything be simple and make sense.
	// Just use it anyway until some dumb edge case pops up and it becomes a problem.
	System::Initialize(argv[0]);
	SeedRNG();
	InstallRNGDrawTraceIfArmed();

	InitializeManagers();
	ScenarioRunner::SetStallEventPoll(&PollSDLEvents);

	if (!HandleMainArgs(argc, argv)) return ShutDown(EXIT_FAILURE);

	if (s_cameraNullSceneSelfTest) {
		// The scroll update runs from the sim tick, which keeps ticking for a frame after an activity
		// ends or an activity launch fails. With no scene it must do nothing rather than fault, and
		// this is the one moment the engine is up with nothing loaded.
		if (g_SceneMan.GetScene() != nullptr) {
			std::cerr << "[camera-null-scene-selftest] FAIL: this case needs a scene-less engine" << std::endl;
			return ShutDown(EXIT_FAILURE);
		}
		for (int screenId = 0; screenId < c_MaxScreenCount; ++screenId) {
			g_CameraMan.Update(screenId);
		}
		std::cout << "[camera-null-scene-selftest] PASS" << std::endl;
		return ShutDown(EXIT_SUCCESS);
	}

	g_PresetMan.LoadAllDataModules();
	if (!ContentFile::WaitForPendingSounds(LoadingScreen::LoadingSplashProgressReport)) return ShutDown(EXIT_FAILURE);

	if (!s_netIdentityDumpPath.empty()) {
		NetIdentityManifest manifest;
		std::string error;
		const int exitCode = NetIdentity::DumpCurrentManifestJson(s_netIdentityDumpPath, &error, &manifest) ? 0 : 1;
		if (exitCode == 0) {
			std::cout << "[net-identity-dump] wrote " << s_netIdentityDumpPath
			          << " session_identity_hash=" << NetIdentity::HashHex(manifest.sessionIdentityHash)
			          << " hash_duration_ms=" << manifest.hashDurationMs << std::endl;
		} else {
			std::cerr << "[net-identity-dump] failed: " << error << std::endl;
		}
		return ShutDown(exitCode);
	}

	if (NetSessionCliRequested()) {
		const int exitCode = RunNetSessionCli();
		return ShutDown(exitCode);
	}

	if (s_netMatchServiceE2E) {
		const int exitCode = RunNetMatchServiceE2E();
		return ShutDown(exitCode);
	}

	if (!s_netReplayVerifyPath.empty()) {
		NetReplayVerifyReport report;
		NetMatchReplayReader::Verify(s_netReplayVerifyPath, report);
		const std::string json = report.ToJson();
		std::cout << "[net-replay-verify] " << json << std::endl;
		if (s_netReplayDumpTo >= s_netReplayDumpFrom) {
			// The recorded wire, tick by tick: what every peer's sim applied.
			NetMatchReplayReader reader;
			std::string openError;
			if (reader.Open(s_netReplayVerifyPath, &openError)) {
				NetLockstepFrame record;
				bool eof = false;
				while (reader.ReadFrame(record, eof, nullptr)) {
					if (record.targetFrame < s_netReplayDumpFrom || record.targetFrame > s_netReplayDumpTo) {
						continue;
					}
					for (const ControllerFrame& frame: record.frames) {
						std::cout << "[net-replay-dump] frame=" << record.targetFrame << " uid=" << frame.actorUniqueID << " mode=" << static_cast<int>(frame.inputMode)
						          << " player=" << static_cast<int>(frame.playerRaw) << " flags=0x" << std::hex << static_cast<int>(frame.flags) << std::dec
						          << " flip=" << (frame.IsActorHFlipped() ? 1 : 0) << " aim_intent=" << (frame.HasAimIntent() ? 1 : 0) << " flip_intent=" << (frame.HasFlipIntent() ? 1 : 0)
						          << " aim=" << std::hexfloat << frame.aimAngle << std::defaultfloat << " fg=" << frame.equippedFGUniqueID << " bg=" << frame.equippedBGUniqueID
						          << " device=" << static_cast<int>(frame.deviceClass) << " aim_speed=" << frame.digitalAimSpeed << std::endl;
					}
					for (const NetGameCommand& command: record.commands) {
						std::cout << "[net-replay-dump] frame=" << record.targetFrame << " command=" << NetGameCommandTypeName(NetGameCommandTypeOf(command.payload)) << " sender=" << static_cast<int>(command.senderPeerId);
						if (const NetGameAIEquip* equip = std::get_if<NetGameAIEquip>(&command.payload)) {
							std::cout << " uid=" << equip->actorUID << " op=" << static_cast<int>(equip->op) << " group=" << equip->group << " preset=" << equip->presetName;
						} else if (const NetGameAIOrder* order = std::get_if<NetGameAIOrder>(&command.payload)) {
							std::cout << " uid=" << order->actorUID << " op=" << static_cast<int>(order->op) << " x=" << order->x << " y=" << order->y << " target=" << order->targetUID;
						}
						std::cout << std::endl;
					}
				}
			} else {
				std::cerr << "[net-replay-dump] " << openError << std::endl;
			}
		}
		if (!ScenarioRunner::GetArgs().outPath.empty()) {
			std::string writeError;
			if (!WriteTextFile(ScenarioRunner::GetArgs().outPath, json + "\n", &writeError)) {
				std::cerr << "[net-replay-verify] could not write " << ScenarioRunner::GetArgs().outPath << ": " << writeError << std::endl;
				return ShutDown(EXIT_FAILURE);
			}
		}
		return ShutDown(report.ok ? 0 : (report.truncated ? 2 : (report.corrupt ? 3 : 1)));
	}
	if (ScenarioRunner::GetArgs().scriptGraphSelfTest) {
		bool pass = g_LuaMan.RunScriptGraphSelfTest();
		pass = RunHarnessCaptureSelfTest() && pass;
		return ShutDown(pass ? 0 : 1);
	}
	if (!s_netReplayInPath.empty()) {
		const int exitCode = RunNetReplayPlayback();
		return ShutDown(exitCode);
	}

	int scenarioExitCode = 0;

	if (!System::IsInExternalModuleValidationMode()) {
		// Load the different input device icons. This can't be done during UInputMan::Create() because the icon presets don't exist so we need to do this after modules are loaded.
		g_UInputMan.LoadDeviceIcons();

		if (g_ConsoleMan.LoadWarningsExist()) {
			g_ConsoleMan.PrintString("WARNING: Encountered non-fatal errors during module loading!\nSee \"LogLoadingWarning.txt\" for information.");
			g_ConsoleMan.SaveLoadWarningLog("LogLoadingWarning.txt");
			// Open the console so the user is aware there are loading warnings.
			g_ConsoleMan.SetEnabled(true);
		} else {
			// Delete an existing log if there are no warnings so there's less junk in the root folder.
			if (std::filesystem::exists(System::GetWorkingDirectory() + "LogLoadingWarning.txt")) {
				std::remove("LogLoadingWarning.txt");
			}
		}

		if (ScenarioRunner::IsActive()) {
			// Pin the canonical deterministic sim config before anything loads or runs.
			ScenarioRunner::ApplyDeterministicConfig();
			GnsTransport netLockstepTransport;
			NetSession netLockstepSession;
			NetLockstepCoordinator netLockstepCoordinator;
			NetMatchRunner netMatchRunner;
			std::string netLockstepSetupError;
			const bool netLockstepPrepared = !NetGameplayRequested() || PrepareNetLockstepScenario(netLockstepTransport, netLockstepSession, netLockstepCoordinator, netMatchRunner, &netLockstepSetupError);
			// CLI direct-launch into a scenario: skip the menu, start the named GAScripted activity
			// directly, run the loop, then finalize the JSON report + exit code.
			std::string controllerLogError;
			const bool controllerLogPrepared = ScenarioRunner::PrepareControllerLog(&controllerLogError);
			const std::string presetName = ScenarioRunner::ResolvePresetName(ScenarioRunner::GetArgs().scenario);
			const Entity* presetEntity = g_PresetMan.GetEntityPreset("GAScripted", presetName);
			const Activity* presetActivity = dynamic_cast<const Activity*>(presetEntity);
			int startResult = -1;
			if (!controllerLogPrepared) {
				std::cerr << "[scenario] controller log setup failed: " << controllerLogError << std::endl;
			}
			if (!netLockstepPrepared) {
				std::cerr << (s_netMatch ? "[net-match]" : "[net-lockstep]") << " setup failed: " << netLockstepSetupError << std::endl;
			}
			if (controllerLogPrepared && netLockstepPrepared && presetActivity) {
				const std::string& sceneName = presetActivity->GetSceneName();
				if (!sceneName.empty()) {
					g_SceneMan.SetSceneToLoad(sceneName, true, false);
				}
				startResult = g_ActivityMan.StartActivity("GAScripted", presetName);
			} else if (controllerLogPrepared && netLockstepPrepared) {
				std::cerr << "[scenario] no preset \"" << presetName << "\" of class GAScripted" << std::endl;
			}

			if (!controllerLogPrepared || !netLockstepPrepared) {
				scenarioExitCode = 1;
			} else if (startResult < 0) {
				std::cerr << "[scenario] failed to start scenario \"" << presetName << "\"" << std::endl;
				scenarioExitCode = 1;
			} else {
				RunGameLoop();
				CheckRequiredProbesCompleted();
				scenarioExitCode = ScenarioRunner::FinalizeAndGetExitCode();
				if (s_netReplayExitCode != 0 && scenarioExitCode == 0) {
					scenarioExitCode = s_netReplayExitCode;
				}
				if (NetGameplayRequested()) {
					(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, 0);
					netLockstepCoordinator.Complete(scenarioExitCode == 0 ? "scenario complete" : "scenario failed");
				}
			}
			if (NetGameplayRequested()) {
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				std::string reportError;
				const std::string report = BuildNetLockstepReportJson(netLockstepSession, netLockstepCoordinator, netMatchRunner, scenarioExitCode, netLockstepSetupError);
				if (!WriteTextFile(s_netLockstepReportPath, report, &reportError)) {
					std::cerr << (s_netMatch ? "[net-match]" : "[net-lockstep]") << " report failed: " << reportError << std::endl;
					scenarioExitCode = 1;
				} else if (!s_netLockstepReportPath.empty()) {
					std::cout << (s_netMatch ? "[net-match]" : "[net-lockstep]") << " wrote report: " << s_netLockstepReportPath << std::endl;
					if (const std::string stats = LocalPrediction::DescribeStats(); !stats.empty()) {
						std::cout << "[localpred] " << stats << std::endl;
					}
				}
			}
		} else {
			// Interactive mode: a stalled lockstep match draws the "waiting for peer" screen.
			ScenarioRunner::SetLockstepStallOverlayEnabled(true);
			if (!g_ActivityMan.Initialize()) {
				RunMenuLoop();
			}

			// If the menu launched a multiplayer match with tracing armed, record the per-tick trace so the
			// menu-driven match can be sim-gated host vs client (same compare as the headless gate).
			const bool traceMenuMp = g_NetMatchService.WasEverStarted() && s_recordTickHashes && !ScenarioRunner::GetArgs().outPath.empty();
			if (traceMenuMp) {
				g_MetricsCollector.BeginRun("P4 Alpha Duel", ScenarioRunner::GetArgs().seed);
				g_MetricsCollector.SetRecordTickHashes(true);
			}

			RunGameLoop();

			// The menu-driven match writes the same lockstep counters the headless gates do, so a
			// lobby lane can say which side of the relay a stall was on.
			if (g_NetMatchService.WasEverStarted() && !s_netLockstepReportPath.empty()) {
				std::string reportError;
				if (!WriteTextFile(s_netLockstepReportPath, g_NetMatchService.BuildReportJson(), &reportError)) {
					std::cerr << "[menu-mp] could not write report: " << reportError << std::endl;
				} else {
					std::cout << "[menu-mp] wrote report: " << s_netLockstepReportPath << std::endl;
				}
			}

			if (traceMenuMp) {
				g_MetricsCollector.EndRun();
				const uint64_t cap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 600;
				if (!s_menuMpTraceError.empty() || g_MetricsCollector.GetTickHashCount() != cap) {
					std::cerr << "[menu-mp] FAIL: collected " << g_MetricsCollector.GetTickHashCount() << " of " << cap << " requested ticks" << std::endl;
					g_MetricsCollector.SetResult(false);
					scenarioExitCode = 1;
				}
				const std::string& tracePath = ScenarioRunner::GetArgs().outPath;
				if (g_MetricsCollector.WriteReport(tracePath)) {
					std::cout << "[menu-mp] wrote trace: " << tracePath << std::endl;
				} else {
					scenarioExitCode = 1;
				}
			}
		}
	}

	return ShutDown(scenarioExitCode);
}

#ifdef _WIN32
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) { return main(__argc, __argv); }
#endif
