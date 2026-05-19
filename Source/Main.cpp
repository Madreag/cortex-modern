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

#include "AIDecisionChannel.h"
#include "AIDebugOverlay.h"
#include "DeterminismCheck.h"
#include "MetricsCollector.h"
#include "NetworkSimulator.h"
#include "ReplayLog.h"
#include "ScenarioRunner.h"
#include "SimChecksum.h"

#include <cstdlib>
#include <iostream>

#include "RenderTarget.h"
#include "tracy/Tracy.hpp"

#include "imgui_impl_sdl3.h"

#ifdef _WIN32
#include "windows.h"
#endif

extern "C" {
FILE __iob_func[3] = {*stdin, *stdout, *stderr};
}

using namespace RTE;

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

	// M0 observability + determinism singletons.
	AIDecisionChannel::Construct();
	AIDebugOverlay::Construct();
	MetricsCollector::Construct();
	NetworkSimulator::Construct();
	ReplayLog::Construct();
	SimChecksum::Construct();

	g_ThreadMan.Initialize();
	g_SettingsMan.Initialize();
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
	g_ReplayLog.Destroy();
	g_NetworkSimulator.Destroy();
	g_MetricsCollector.Destroy();
	g_AIDebugOverlay.Destroy();
	g_AIDecisionChannel.Destroy();
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

		// M0 scenario-runner args (CLI direct-launch into a Trust AI-NN scenario).
		// M1 Block A: -tick-hashes added (no-value flag) for the determinism CI scaffold.
		if (currentArg == "-scenario" || currentArg == "-out" || currentArg == "-seed" ||
		    currentArg == "-max-ticks" || currentArg == "-tick-hashes") {
			const int consumed = ScenarioRunner::ParseArgs(argCount, argValue, i);
			if (consumed > 0) {
				i += consumed;
				continue;
			}
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

	// Tick counter scoped to the current test-activity run, so the hard cap below covers
	// both CLI direct-launch and menu-launched test activities. Reset each time a test
	// activity ends so menu-launched back-to-back runs are independent.
	static uint64_t s_testActivityStartTick = UINT64_MAX;
	// Frame countdown between scenario completion and actual exit/teardown. Gives the user
	// ~1.5 seconds to see the AI overlay's final state + the PASS/FAIL banner before the
	// window closes (CLI mode) or bounces to the menu (Debug builds). Tick-based would have
	// been wrong here — the sim is no longer advancing in some cases, so we count frames.
	static int s_exitLingerFrames = 0;
	// Linger window: gives the user time to read the AI overlay + the [Trust] PASS/FAIL banner
	// before the window closes. 90 frames ≈ 1.5 sec at 60 fps. Adjust if scenarios end too fast
	// to read or take noticeably too long to close.
	constexpr int kExitLingerFramesMax = 90;

	while (!System::IsSetToQuit()) {
		// Trust-scenario hard auto-exit. Fires every frame (not just sim ticks), so CC's
		// DEAD overlay can never sit forever waiting for a player to press a key. Any
		// activity marked IsTestActivity() that either (a) reaches ActivityState::Over or
		// (b) exceeds its tick budget triggers a brief linger (so the user can see the result)
		// then immediate exit (CLI) or activity-end (menu).
		// Default cap is 1800 sim ticks (30 s); CLI -max-ticks N overrides.
		if (Activity* curAct = g_ActivityMan.GetActivity(); curAct && curAct->IsTestActivity()) {
			const uint64_t now = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
			if (s_testActivityStartTick == UINT64_MAX) {
				s_testActivityStartTick = now;
			}
			const uint64_t elapsed = now - s_testActivityStartTick;
			const uint64_t cap = (ScenarioRunner::IsActive() && ScenarioRunner::GetArgs().maxTicks > 0)
			                         ? ScenarioRunner::GetArgs().maxTicks
			                         : 1800;
			if (curAct->IsOver() || elapsed >= cap) {
				if (s_exitLingerFrames == 0) {
					// First time we noticed completion: open the console so the [Trust] PASS/FAIL
					// banner Lua printed is visible, then start the countdown.
					g_ConsoleMan.SetEnabled(true);
					if (ScenarioRunner::IsActive()) {
						g_ConsoleMan.PrintString("[Trust] " + curAct->GetPresetName()
						                         + " — exiting in ~1.5s");
					}
					s_exitLingerFrames = kExitLingerFramesMax;
				} else if (s_exitLingerFrames > 1) {
					--s_exitLingerFrames;
				} else {
					// Linger done — perform the actual teardown.
					s_exitLingerFrames = 0;
					// Stop all sounds + end the activity BEFORE quitting/transitioning. Without
					// this, FMOD's async update thread races against MovableMan/Activity teardown
					// in DestroyManagers and crashes on freed sound userdata.
					g_AudioMan.StopAll();
					g_ActivityMan.EndActivity();
					// EndActivity invoked Lua's EndActivity hook, which emits scenario_end. That
					// emit landed in the channel but won't be picked up by MovableMan's per-tick
					// drain because we're about to break out of the outer loop. One last manual
					// drain pulls it through so the JSON report's event_counts reflects the final
					// scenario_end event.
					{
						std::vector<AIDecisionChannel::Event> finalEvents;
						g_AIDecisionChannel.Drain(finalEvents);
						if (!finalEvents.empty()) {
							g_SimChecksum.Update("decisions",
							                     finalEvents.data(),
							                     finalEvents.size() * sizeof(AIDecisionChannel::Event));
							g_MetricsCollector.ConsumeEvents(finalEvents);
						}
					}
					if (ScenarioRunner::IsActive()) {
						// Give FMOD a moment to settle its async update before we quit.
						g_AudioMan.PauseIngameSounds(true);
						System::SetQuit(true);
						break;
					}
					g_ActivityMan.SetInActivity(false);
					s_testActivityStartTick = UINT64_MAX;
				}
			}
		} else {
			s_testActivityStartTick = UINT64_MAX;
			s_exitLingerFrames = 0;
		}

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

			// M0 observability: per-tick decision-event channel tick + SimChecksum bracket
			// + ReplayLog tick marker. Channel SetCurrentTick happens here so any emits during
			// sim are tagged with the current tick. SimChecksum::BeginTick arms the subsystem
			// accumulators; matching EndTick at the end of this block produces the per-tick
			// hash. SimChecksum::Update for the "decisions" subsystem is fed from
			// MovableMan::Update's drain. ReplayLog records the tick number + (currently empty)
			// per-player controller state so the replay file has a real frame timeline.
			// MP M1 will populate the per-player ControllerState bytes from UInputMan.
			{
				const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				g_AIDecisionChannel.SetCurrentTick(simTick);
				g_SimChecksum.BeginTick(simTick);
				// Per-tick replay frame marker. Player input is empty under CLI/trust runs
				// (no human player) but the tick numbers + scenario header + seed are enough
				// for an offline replay-verify tool (M5 stretch goal) to identify the run.
				// MP M1 will populate the per-player ControllerState bytes from UInputMan.
				if (g_ReplayLog.IsRecording()) {
					static const std::vector<ReplayLog::PlayerInput> emptyInputs;
					g_ReplayLog.RecordTick(simTick, emptyInputs);
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
			g_PerformanceMan.UpdateSortedScriptTimings(g_LuaMan.GetScriptTimings());

			g_AudioMan.Update();
			g_MusicMan.Update();

			g_ActivityMan.LateUpdateGlobalScripts();

			// M0 observability: feed the terrain subsystem hash with the sim tick number as
			// the placeholder data (the actual carve/penetrate math hash is wired at MP M2 per
			// the M0 plan). Then finalize the per-tick hash.
			//
			// M1 Block A: hand the tick result to the MetricsCollector for the per-tick hash
			// trace. The collector silently no-ops when -tick-hashes is not set, so this is
			// free for normal runs.
			{
				const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				g_SimChecksum.Update("terrain", &simTick, sizeof(simTick));
				const auto tickResult = g_SimChecksum.EndTick();
				g_MetricsCollector.RecordTickHash(tickResult);
			}

			// This is to support hot reloading entities in SceneEditorGUI. It's a bit hacky to put it in Main like this, but PresetMan has no update in which to clear the value, and I didn't want to set up a listener for the job.
			// It's in this spot to allow it to be set by UInputMan update and ConsoleMan update, and read from ActivityMan update.
			g_PresetMan.ClearReloadEntityPresetCalledThisUpdate();

			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
			g_UInputMan.EndFrame();

			if (!g_ActivityMan.IsInActivity()) {
				g_TimerMan.PauseSim(true);

				if (!g_ActivityMan.ActivitySetToRestart()) {
					if (ScenarioRunner::IsActive()) {
						System::SetQuit(true);
						break;
					}
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

		g_FrameMan.Draw();
		g_WindowMan.DrawPostProcessBuffer();
		g_WindowMan.UploadFrame();

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

/// <summary>
/// Implementation of the main function.
/// </summary>
int main(int argc, char** argv) {
	// Block A (M1): determinism-check mode is a pre-init orchestrator — it spawns child
	// game processes and diffs their JSON outputs. It must short-circuit BEFORE
	// install_allegro / SDL_Init / engine bootstrapping, so concurrent CI invocations
	// don't fight over the window/audio devices (and so the orchestrator itself stays
	// lightweight: no module load, no GL context).
	if (DeterminismCheck::IsRequested(argc, argv)) {
		return DeterminismCheck::Run(argc, argv);
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
			// CLI direct-launch into a Trust AI-NN scenario. Window stays visible — the user
			// watches the scenario play out. We skip the menu entirely and start the named
			// activity directly. RunGameLoop's "fall back to menu when activity ends" branch
			// has been taught to instead SetQuit(true) when ScenarioRunner::IsActive().
			const std::string presetName = ScenarioRunner::ResolvePresetName(ScenarioRunner::GetArgs().scenario);

			// Look up the preset so we can preload its declared SceneName. The normal
			// scenarios-menu flow does scene-selection separately; we replicate that here so
			// g_SceneMan has a scene queued by the time GAScripted::Start runs.
			const Entity* presetEntity = g_PresetMan.GetEntityPreset("GAScripted", presetName);
			const Activity* presetActivity = dynamic_cast<const Activity*>(presetEntity);
			int startResult = -1;
			if (presetActivity) {
				const std::string& sceneName = presetActivity->GetSceneName();
				if (!sceneName.empty()) {
					g_SceneMan.SetSceneToLoad(sceneName, true, false);
				}
				// Arm the ReplayLog so any input-driven changes get a trace for offline review.
				g_ReplayLog.BeginRecording(ScenarioRunner::GetArgs().scenario,
				                           ScenarioRunner::GetArgs().seed);
				startResult = g_ActivityMan.StartActivity("GAScripted", presetName);
			} else {
				std::cerr << "[scenario] no preset \"" << presetName << "\" of class GAScripted" << std::endl;
			}

			if (startResult < 0) {
				std::cerr << "[scenario] failed to start scenario \"" << presetName << "\"" << std::endl;
				scenarioExitCode = 1;
			} else {
				RunGameLoop();
				g_ReplayLog.EndRecording();
				// Persist the replay alongside the JSON report if -out was provided.
				const std::string& outPath = ScenarioRunner::GetArgs().outPath;
				if (!outPath.empty()) {
					std::filesystem::path rp = std::filesystem::path(outPath).replace_extension(".replay");
					g_ReplayLog.Write(rp.string());
				}
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
		// CLI scenario mode: the JSON report and replay log were already flushed before
		// RunGameLoop returned. Skip DestroyManagers — it races with FMOD's async update
		// thread on freed Sound userdata when actors are torn down faster than CC's normal
		// quit-from-menu flow gives FMOD time to settle. The OS reclaims everything
		// (memory, FMOD threads, file handles) when the process exits. Save the console
		// log first so the user still gets diagnostics.
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
