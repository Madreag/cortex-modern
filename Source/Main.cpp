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
#include "AllegroScreen.h"
#include "AllegroBitmap.h"

#include "MainMenuGUI.h"
#include "ScenarioGUI.h"
#include "PauseMenuGUI.h"
#include "TitleScreen.h"
#include "LoadingScreen.h"

#include "MenuMan.h"
#include "ConsoleMan.h"
#include "SettingsMan.h"
#include "PresetMan.h"
#include "UInputMan.h"
#include "PerformanceMan.h"
#include "FrameMan.h"
#include "PostProcessMan.h"
#include "SceneMan.h"
#include "MetaMan.h"
#include "WindowMan.h"
#include "GLResourceMan.h"
#include "CameraMan.h"
#include "ActivityMan.h"
#include "PrimitiveMan.h"
#include "ThreadMan.h"
#include "LuaMan.h"
#include "MusicMan.h"
#include "System.h"

#include "ControllerFrame.h"
#include "GnsTransport.h"
#include "NetIdentity.h"
#include "NetIdentitySelfTest.h"
#include "NetProtocolSelfTest.h"
#include "NetSession.h"
#include "NetSessionSelfTest.h"
#include "SimChecksum.h"
#include "ScenarioRunner.h"
#include "DeterminismCheck.h"
#include "MetricsCollector.h"

#include "RenderTarget.h"
#include "tracy/Tracy.hpp"

#include "imgui_impl_sdl3.h"

#ifdef _WIN32
#include "windows.h"
#include <crtdbg.h>
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <utility>

extern "C" {
FILE __iob_func[3] = {*stdin, *stdout, *stderr};
}

using namespace RTE;

// Per-tick state hashing — armed by the -tick-hashes CLI flag, off in normal play.
static bool s_recordTickHashes = false;

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

/// <summary>
/// Command-line argument handling.
/// </summary>
/// <param name="argCount">Argument count.</param>
/// <param name="argValue">Argument values.</param>
void HandleMainArgs(int argCount, char** argValue) {
	// Discard the first argument because it's always the executable path/name
	argCount--;
	argValue++;
	if (argCount == 0) {
		return;
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
}

/// <summary>
/// Polls the SDL event queue and passes events to be handled by the relevant managers.
/// </summary>
void PollSDLEvents() {
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

/// <summary>
/// Game menus loop.
/// </summary>
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
	}

	g_MenuMan.SetIsInMenuScreen(false);
}

/// <summary>
/// Game simulation loop.
/// </summary>
void RunGameLoop() {
	if (System::IsSetToQuit()) {
		return;
	}
	g_TimerMan.PauseSim(false);

	if (g_ActivityMan.ActivitySetToRestart()) {
		g_LoadingScreen.DrawLoadingSplash();
		g_WindowMan.UploadFrame();
		if (!g_ActivityMan.RestartActivity()) {
			// This doesn't work.
			// Somewhat related to https://github.com/cortex-command-community/Cortex-Command-Community-Project-Source/issues/472
			// Deal with later.
			// g_MenuMan.GetTitleScreen()->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeIn);
		}
	}

	long long updateStartTime = 0;
	long long updateTotalTime = 0;
	long long updateEndAndDrawStartTime = 0;
	long long drawStartTime = 0;
	long long drawTotalTime = 0;

	while (!System::IsSetToQuit()) {
		bool serverUpdated = false;
		updateStartTime = g_TimerMan.GetAbsoluteTime();

		PollSDLEvents();
		g_WindowMan.Update();
		g_WindowMan.ClearBackbuffer();

		g_TimerMan.Update();

		// Simulation update, as many times as the fixed update step allows in the span since last frame draw.
		while (g_TimerMan.TimeForSimUpdate()) {
			ZoneScopedN("Simulation Update");

			serverUpdated = false;

			g_PerformanceMan.NewPerformanceSample();
			g_PerformanceMan.UpdateMSPSU();
			g_TimerMan.UpdateSim();

			g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::SimTotal);

			if (s_recordTickHashes) {
				const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				g_SimChecksum.BeginTick(simTick);

				// Positive control — inject exactly one genuine non-determinism at a fixed tick
				// so the determinism check's selftest sees a guaranteed divergence.
				if (ScenarioRunner::IsActive() && ScenarioRunner::GetArgs().selftestPerturb && simTick == 50) {
					std::random_device perturbDevice;
					const unsigned perturbAdvance = (perturbDevice() % 64u) + 1u;
					for (unsigned k = 0; k < perturbAdvance; ++k) {
						g_SimRNG.RandomNum<uint32_t>();
					}
				}
			}

			g_LuaMan.Update();

			g_UInputMan.Update();

			g_FrameMan.Update();

			g_MovableMan.CompleteQueuedMOIDDrawings();

			g_ConsoleMan.Update();
			g_ActivityMan.Update();

			if (g_SceneMan.GetScene()) {
				g_SceneMan.GetScene()->Update();
			}

			g_LuaMan.ClearScriptTimings();
			g_MovableMan.Update();
			if (ScenarioRunner::HasControllerReplayError()) {
				std::cerr << "[scenario] controller replay failed: " << ScenarioRunner::GetControllerReplayError() << std::endl;
				System::SetQuit(true);
				g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
				g_UInputMan.EndFrame();
				break;
			}
			g_PerformanceMan.UpdateSortedScriptTimings(g_LuaMan.GetScriptTimings());

			g_AudioMan.Update();
			g_MusicMan.Update();

			g_ActivityMan.LateUpdateGlobalScripts();

			// Feed end-of-tick terrain state, finalize this tick's hash, and hand the result to the
			// MetricsCollector for the per-tick determinism trace (no-op without an active scenario run).
			if (s_recordTickHashes) {
				g_SceneMan.FeedTerrainToSimChecksum();
				const auto tickResult = g_SimChecksum.EndTick();
				g_MetricsCollector.RecordTickHash(tickResult);
			}

			// This is to support hot reloading entities in SceneEditorGUI. It's a bit hacky to put it in Main like this, but PresetMan has no update in which to clear the value, and I didn't want to set up a listener for the job.
			// It's in this spot to allow it to be set by UInputMan update and ConsoleMan update, and read from ActivityMan update.
			g_PresetMan.ClearReloadEntityPresetCalledThisUpdate();

			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
			g_UInputMan.EndFrame();

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
				const uint64_t tickCap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 1800;
				const Activity* scenarioActivity = g_ActivityMan.GetActivity();
				if ((scenarioActivity && scenarioActivity->IsOver()) || elapsedTicks >= tickCap) {
					// Finalize so the scenario's Lua OnEnd grades the run even when the CLI tick cap
					// stops it before the scenario's own max-ticks (idempotent if it already ended).
					g_ActivityMan.EndActivity();
					System::SetQuit(true);
					break;
				}
			}

			if (!g_ActivityMan.IsInActivity()) {
				g_TimerMan.PauseSim(true);

				if (!g_ActivityMan.ActivitySetToRestart()) {
					g_MenuMan.HandleTransitionIntoMenuLoop();
					RunMenuLoop();
				}
			}
			if (g_ActivityMan.ActivitySetToRestart()) {
				g_LoadingScreen.DrawLoadingSplash();
				g_WindowMan.UploadFrame();
				if (!g_ActivityMan.RestartActivity()) {
					break;
				}
			}
			if (g_ActivityMan.ActivitySetToResume()) {
				g_ActivityMan.ResumeActivity();
				g_PerformanceMan.ResetSimUpdateTimer();
				updateStartTime = g_TimerMan.GetAbsoluteTime();
			}
		}

		updateEndAndDrawStartTime = g_TimerMan.GetAbsoluteTime();
		updateTotalTime = updateEndAndDrawStartTime - updateStartTime;
		drawStartTime = updateEndAndDrawStartTime;

		// Frame rendering must not advance the sim RNG stream — its cadence is host frame-rate
		// dependent, so redirect any cosmetic draws here to the render RNG.
		RandomGenerator* prevSimRNG = t_simRNGOverride;
		t_simRNGOverride = &g_RenderRNG;
		g_FrameMan.Draw();
		g_WindowMan.DrawPostProcessBuffer();
		g_WindowMan.UploadFrame();
		t_simRNGOverride = prevSimRNG;

		drawTotalTime = g_TimerMan.GetAbsoluteTime() - drawStartTime;
		g_PerformanceMan.UpdateMSPF(updateTotalTime, drawTotalTime);
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
	return s_netHost || !s_netJoinAddress.empty();
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
	config.rejectUserdataModules = !s_netAllowUserdata;
	return config;
}

int RunNetSessionCli() {
	if (s_netHost && !s_netJoinAddress.empty()) {
		std::cerr << "[net-session] choose either -net-host or -net-join, not both" << std::endl;
		return 1;
	}

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
			if (arg == "-tick-hashes" || arg == "-headless" || arg == "-net-host" || arg == "-net-join") {
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

	InitializeManagers();

	HandleMainArgs(argc, argv);

	g_PresetMan.LoadAllDataModules();

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
		std::cout.flush();
		std::cerr.flush();
		std::_Exit(exitCode);
	}

	if (NetSessionCliRequested()) {
		const int exitCode = RunNetSessionCli();
		std::cout.flush();
		std::cerr.flush();
		std::_Exit(exitCode);
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
			} else if (presetActivity) {
				const std::string& sceneName = presetActivity->GetSceneName();
				if (!sceneName.empty()) {
					g_SceneMan.SetSceneToLoad(sceneName, true, false);
				}
				startResult = g_ActivityMan.StartActivity("GAScripted", presetName);
			} else {
				std::cerr << "[scenario] no preset \"" << presetName << "\" of class GAScripted" << std::endl;
			}

			if (!controllerLogPrepared) {
				scenarioExitCode = 1;
			} else if (startResult < 0) {
				std::cerr << "[scenario] failed to start scenario \"" << presetName << "\"" << std::endl;
				scenarioExitCode = 1;
			} else {
				RunGameLoop();
				scenarioExitCode = ScenarioRunner::FinalizeAndGetExitCode();
			}
		} else {
			if (!g_ActivityMan.Initialize()) {
				RunMenuLoop();
			}

			RunGameLoop();
		}
	}

	g_ThreadMan.GetPriorityThreadPool().wait_for_tasks();
	g_ThreadMan.GetBackgroundThreadPool().wait_for_tasks();

	if (ScenarioRunner::IsActive()) {
		// Scenario mode: the JSON report was flushed before RunGameLoop returned. Skip
		// DestroyManagers — it races FMOD's async update thread on freed Sound userdata when actors
		// tear down faster than the normal quit flow. The OS reclaims everything on process exit.
		g_ConsoleMan.SaveAllText("LogConsole.txt");
		std::_Exit(scenarioExitCode);
	}

	DestroyManagers();

	allegro_exit();
	SDL_Quit();

	return EXIT_SUCCESS;
}

#ifdef _WIN32
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) { return main(__argc, __argv); }
#endif
