#include "NetModerationGUIProbe.h"

#include "ActivityMan.h"
#include "CameraMan.h"
#include "FrameMan.h"
#include "GameActivity.h"
#include "GUI.h"
#include "GUIButton.h"
#include "GUIFont.h"
#include "GUILabel.h"
#include "GUIInputWrapper.h"
#include "MainMenuGUI.h"
#include "PauseMenuGUI.h"
#include "MenuMan.h"
#include "Scene.h"
#include "SceneMan.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "NetModerationGUI.h"
#include "System.h"
#include "TimerMan.h"
#include "UInputMan.h"
#include "WindowMan.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace RTE::NetModerationGUIProbe {
namespace {
	using Json = nlohmann::json;
	using Clock = std::chrono::steady_clock;
	enum class Phase {
		Poll,
		Draw,
		Sim
	};
	struct Probe {
		bool loaded = false, enabled = false, done = false, resultStarted = false;
		size_t index = 0, gestureIndex = SIZE_MAX;
		uint64_t renders = 0, stepRender = 0, stepMs = 0, simTick = 0;
		Clock::time_point started;
		std::filesystem::path directory;
		Json script, result;
		std::string hintAtLoad;
		bool hintAtLoadPresent = false;
	};
	Probe probe;

	GUIControlManager* MenuControls() {
		if (auto* pause = g_MenuMan.GetActivePauseMenu()) return pause->AutomationManager();
		return g_MenuMan.IsMainMenuInteractive() ? g_MenuMan.GetMainMenu()->AutomationManager() : nullptr;
	}
	std::string MenuScreen() {
		if (auto* pause = g_MenuMan.GetActivePauseMenu()) return pause->AutomationActiveScreenName();
		return g_MenuMan.IsMainMenuInteractive() ? g_MenuMan.GetMainMenu()->AutomationActiveScreenName() : "Gameplay";
	}

	/// P is read inside the sim tick (KeyPressedSim); F6 and Escape are menu keys read per render frame.
	bool SimRateKey(const std::string& key) { return key == "P"; }

	void Require(bool condition, const std::string& reason) {
		if (!condition) throw std::runtime_error(reason);
	}

	int ScriptedPadCount() {
		int count = 0;
		SDL_JoystickID* ids = SDL_GetJoysticks(&count);
		int scripted = 0;
		for (int i = 0; i < count; ++i) {
			const char* name = SDL_GetJoystickNameForID(ids[i]);
			if (name && (std::string(name) == "Menu script controller" || std::string(name) == "Net UI probe controller")) {
				++scripted;
			}
		}
		SDL_free(ids);
		return scripted;
	}

	uint64_t NowMs() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - probe.started).count());
	}

	std::filesystem::path Leaf(const std::string& name) {
		Require(!name.empty() && name.size() <= 100 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		}), "invalid artifact name");
		return probe.directory / name;
	}

	Json Rect(int x, int y, int width, int height, bool visible) {
		return {{"x", x}, {"y", y}, {"w", width}, {"h", height}, {"visible", visible && width > 0 && height > 0}};
	}

	bool Overlaps(const Json& left, const Json& right) {
		return left.at("visible").get<bool>() && right.at("visible").get<bool>() &&
		    left["x"].get<int>() < right["x"].get<int>() + right["w"].get<int>() &&
		    right["x"].get<int>() < left["x"].get<int>() + left["w"].get<int>() &&
		    left["y"].get<int>() < right["y"].get<int>() + right["h"].get<int>() &&
		    right["y"].get<int>() < left["y"].get<int>() + left["h"].get<int>();
	}

	/// The slide-in panel's visible column, which every editor reports as the seat's screen occlusion.
	/// The occlusion is in the seat's framebuffer space, so a split screen's own offset translates it
	/// into the window space the overlay rects already live in.
	Json PickerRect(int screen) {
		const int occlusion = g_CameraMan.GetScreenOcclusion(screen).GetRoundIntX();
		Vector offset;
		g_FrameMan.GetScreenOffsetForSplitScreen(screen, offset);
		const int x = offset.GetRoundIntX(), y = offset.GetRoundIntY();
		const int height = g_FrameMan.GetPlayerScreenHeight();
		if (occlusion < 0) return Rect(x + g_FrameMan.GetPlayerScreenWidth() + occlusion, y, -occlusion, height, true);
		return Rect(x, y, occlusion, height, true);
	}

	/// The band the seat's own message occupies, read from the manager that lays it out. Measured in its
	/// blinking form whether or not this frame draws it, so the rect does not pulse.
	Json ScreenTextRect(int screen) {
		const FrameMan::ScreenTextLayout layout = g_FrameMan.GetScreenTextLayout(screen, true);
		Vector offset;
		g_FrameMan.GetScreenOffsetForSplitScreen(screen, offset);
		return Rect(layout.x + offset.GetRoundIntX(), layout.y + offset.GetRoundIntY(), layout.width, layout.height, !layout.text.empty());
	}

	Json OverlayRect(const NetModerationGUI::OverlayRect& rect) {
		return Rect(rect.x, rect.y, rect.width, rect.height, rect.visible);
	}

	Json Observe() {
		const auto snapshot = g_NetMatchService.GetLobbySnapshot();
		Json observed = {{"at_ms", NowMs()}, {"render", probe.renders}, {"sim_frame", g_TimerMan.GetSimUpdateCount()},
		    {"screen", MenuScreen()},
		    {"service", snapshot.serviceState}, {"host", snapshot.isHost}, {"activity_preset", snapshot.activityPreset},
		    {"panel_open", g_MenuMan.IsNetworkPanelOpen()},
		    {"paused", g_ActivityMan.ActivityPaused()}, {"seats", Json::array()}};
		for (const auto& seat: g_NetMatchService.GetModerationSeats()) {
			observed["seats"].push_back({{"seat", seat.stableSeat}, {"name", seat.displayName}, {"dropped", seat.dropped},
			    {"closed", seat.closed}, {"substituting", seat.substituting}, {"reclaiming", seat.reclaiming},
			    {"applicants", seat.applicants.size()}, {"actions_available", seat.actionsAvailable},
			    {"holder_generation", seat.holderGeneration}, {"seat_generation", seat.seatGeneration}});
		}
		// The setup editor a lockstep match holds in, so a script can drive and read this peer's own seats.
		auto* game = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity());
		observed["editing"] = game && game->GetActivityState() == Activity::Editing;
		observed["editor_seats"] = Json::array();
		const Scene* scene = game ? g_SceneMan.GetScene() : nullptr;
		for (int player = 0; game && player < Players::MaxPlayerCount; ++player) {
			if (!(game->IsSeatActive(player) && game->IsLocalHumanSeat(player))) continue;
			const Json textBand = ScreenTextRect(game->ScreenOfPlayer(player));
			observed["editor_seats"].push_back({{"player", player}, {"ready", game->IsReadyToStart(player)},
			    {"resident", scene && scene->GetResidentBrain(player) != nullptr},
			    {"submitted", game->HasSubmittedLockstepPlacement(player)}, {"mode", game->SetupEditorMode(player)},
			    {"gesture", GameActivity::SetupEditorGestureStatus(player)},
			    {"placement_refused", GameActivity::EditorWriteWasRefused(player)},
			    {"screen_text", g_FrameMan.GetScreenText(game->ScreenOfPlayer(player))},
			    {"picker", PickerRect(game->ScreenOfPlayer(player))},
			    {"screen_text_rect", textBand}, {"text_band", textBand}});
		}
		// What the network overlay drew this frame, so a script can require it to stay off the editor's own UI.
		const NetModerationGUI* panel = g_MenuMan.GetNetworkPanel();
		Json seats = Rect(0, 0, 0, 0, false);
		if (panel) {
			if (GUIControl* box = panel->GetControl("NetworkSeats")) {
				int x, y, w, h;
				box->GetControlRect(&x, &y, &w, &h);
				seats = Rect(x, y, w, h, g_MenuMan.IsNetworkPanelOpen());
			}
		}
		observed["net_ui"] = {{"status", panel ? OverlayRect(panel->GetStatusRect()) : Rect(0, 0, 0, 0, false)},
		    {"toasts", panel ? OverlayRect(panel->GetToastRect()) : Rect(0, 0, 0, 0, false)}, {"seats_panel", seats}};
		return observed;
	}

	void WriteResult() {
		std::ofstream output(probe.directory / "net-ui-result.json");
		output << probe.result.dump(2) << '\n';
		Require(static_cast<bool>(output), "cannot write probe result");
	}

	void Load() {
		probe.loaded = true;
		const char* path = std::getenv("CC_TEST_NET_UI_SCRIPT");
		if (!path || !*path) return;
		probe.enabled = true;
		probe.started = Clock::now();
		const char* hint = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
		probe.hintAtLoadPresent = hint != nullptr;
		probe.hintAtLoad = hint ? hint : "";
		probe.directory = std::filesystem::absolute(path).parent_path();
		probe.result = {{"schema", 1}, {"pass", false}, {"complete", false}, {"steps", Json::array()}, {"pid", System::GetProcessID()}};
		Require(!std::filesystem::exists(probe.directory / "net-ui-result.json"), "probe result already exists");
		probe.resultStarted = true;
		std::ifstream input(path);
		Require(static_cast<bool>(input), "cannot read input script");
		probe.script = Json::parse(input);
		Require(probe.script.at("schema") == 1 && probe.script.at("steps").is_array(), "invalid script schema");
		Require(!probe.script["steps"].empty() && probe.script["steps"].size() <= 256, "invalid script length");
		const auto timeout = probe.script.at("timeout_ms").get<uint64_t>();
		Require(timeout > 0 && timeout <= 180000, "invalid script deadline");
		for (const auto& step: probe.script["steps"]) {
			const std::string op = step.value("op", "");
			if (op != "key_down" && op != "key_up") continue;
			if (SimRateKey(step.value("key", ""))) {
				Require(step.contains("sim_at") && step["sim_at"].is_number_unsigned(), "sim-rate probe key needs an integer sim_at");
			} else {
				Require(!step.contains("sim_at"), "sim_at is only for sim-rate probe keys");
			}
		}
		probe.result["script"] = probe.script;
		WriteResult();
	}

	GUIControl* Control(const Json& step) {
		if (step.value("scope", "") == "menu") {
			auto* manager = MenuControls();
			auto* control = manager ? manager->GetControl(step.at("control").get<std::string>()) : nullptr;
			Require(control != nullptr, "unknown active menu control: " + step.at("control").get<std::string>());
			return control;
		}
		auto* menu = g_MenuMan.GetNetworkPanel();
		Require(menu != nullptr, "network panel has not been constructed");
		auto* control = menu->GetControl(step.at("control").get<std::string>());
		Require(control != nullptr, "unknown control: " + step.at("control").get<std::string>());
		return control;
	}

	Json ReadControl(GUIControl* control) {
		int x, y, w, h;
		control->GetControlRect(&x, &y, &w, &h);
		Json value = {{"rect", {x, y, w, h}}, {"visible", control->GetVisible()}, {"enabled", control->GetEnabled()}};
		value["focus"] = control->GetPanel() && control->GetPanel()->HasFocus();
		if (auto* label = dynamic_cast<GUILabel*>(control)) {
			value["text"] = label->GetText();
			value["text_height"] = label->GetTextHeight();
		} else if (auto* button = dynamic_cast<GUIButton*>(control)) {
			value["text"] = button->GetText();
			value["pushed"] = button->IsPushed();
		}
		return value;
	}

	void Push(SDL_Event& event) {
		Require(SDL_PushEvent(&event), std::string("SDL_PushEvent: ") + SDL_GetError());
	}

	Phase StepPhase(const Json& step) {
		const std::string op = step.at("op");
		if (op == "menu") {
			const std::string command = step.at("command");
			return command.starts_with("assert_") || command.starts_with("dump_") ? Phase::Draw : Phase::Poll;
		}
		if (op == "assert" || op == "assert_control" || op == "assert_editor" || op == "assert_net_ui_clear" ||
		    op == "screenshot" || op == "screenshot_pair" || op == "finish") return Phase::Draw;
		if ((op == "key_down" || op == "key_up") && SimRateKey(step.value("key", ""))) return Phase::Sim;
		return Phase::Poll;
	}

	/// A step the menus can serve on their own. The overlay and the seats panel exist only from the first
	/// in-match draw, so their steps wait for it; `finish` ends a script that never leaves the menus.
	bool MenuScopeStep(const Json& step) {
		const std::string op = step.value("op", "");
		return op == "menu" || op == "finish" || step.value("scope", "") == "menu";
	}

	bool Step(const Json& step, Json& observed) {
		const std::string op = step.at("op");
		if (op == "wait") {
			Require(step.contains("service") || step.contains("sim_at_least") || step.contains("renders") ||
			    step.contains("elapsed_ms") || step.contains("panel_open") || step.contains("control") || step.contains("screen") ||
			    step.contains("editing") || step.contains("seat_ready") || step.contains("seat_text_contains") ||
			    step.contains("picker_open"), "wait has no predicate");
			if (step.contains("screen") && observed["screen"] != step["screen"]) return false;
			if (step.contains("picker_open")) {
				const auto seat = std::find_if(observed["editor_seats"].begin(), observed["editor_seats"].end(),
				    [&](const Json& row) { return row.at("player") == step.value("player", 0); });
				if (seat == observed["editor_seats"].end() || seat->at("picker").at("visible") != step["picker_open"]) return false;
			}
			if (step.contains("seat_text_contains")) {
				// A seat's screen carries both its editor's line and its activity's, so wait for the one asked for.
				const auto seat = std::find_if(observed["editor_seats"].begin(), observed["editor_seats"].end(),
				    [&](const Json& row) { return row.at("player") == step.value("player", 0); });
				if (seat == observed["editor_seats"].end() ||
				    seat->at("screen_text").get<std::string>().find(step["seat_text_contains"].get<std::string>()) == std::string::npos) return false;
			}
			if (step.contains("editing") && observed["editing"] != step["editing"]) return false;
			if (step.contains("seat_ready")) {
				const auto seat = std::find_if(observed["editor_seats"].begin(), observed["editor_seats"].end(),
				    [&](const Json& row) { return row.at("player") == step["seat_ready"]; });
				if (seat == observed["editor_seats"].end() || seat->at("ready") != true) return false;
			}
			if (step.contains("service") && observed["service"] != step["service"]) return false;
			if (step.contains("sim_at_least") && observed["sim_frame"].get<long long>() < step["sim_at_least"].get<long long>()) return false;
			if (step.contains("renders") && probe.renders - probe.stepRender < step["renders"].get<uint64_t>()) return false;
			if (step.contains("elapsed_ms") && NowMs() - probe.stepMs < step["elapsed_ms"].get<uint64_t>()) return false;
			if (step.contains("panel_open") && observed["panel_open"] != step["panel_open"]) return false;
			if (step.contains("control")) {
				observed["control"] = ReadControl(Control(step));
				for (auto it = step.at("equals").begin(); it != step["equals"].end(); ++it) {
					if (observed["control"].at(it.key()) != it.value()) return false;
				}
			}
		} else if (op == "key_down" || op == "key_up") {
			const std::string key = step.at("key");
			Require(key == "F6" || key == "Escape" || key == "P", "unsupported probe key");
			if (SimRateKey(key)) {
				Require(step.contains("sim_at") && step["sim_at"].is_number_unsigned(), "sim-rate probe key needs an integer sim_at");
				const uint64_t simAt = step["sim_at"].get<uint64_t>();
				if (probe.simTick < simAt) return false;
				Require(probe.simTick == simAt, "sim-rate probe key missed sim update " + std::to_string(simAt) + ", the sim is at " + std::to_string(probe.simTick));
				g_UInputMan.SetProbeKeySim(SDLK_P, op == "key_down");
				return true;
			}
			SDL_Event event{};
			event.type = op == "key_down" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
			event.key.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			event.key.scancode = key == "F6" ? SDL_SCANCODE_F6 : key == "P" ? SDL_SCANCODE_P : SDL_SCANCODE_ESCAPE;
			event.key.key = key == "F6" ? SDLK_F6 : key == "P" ? SDLK_P : SDLK_ESCAPE;
			event.key.down = op == "key_down";
			Push(event);
		} else if (op == "pad_down" || op == "pad_up") {
			const std::string name = step.at("button");
			Require(GUIInputWrapper::QueueScriptedPad(name, op == "pad_down"), "probe pad " + name);
			observed["pad"] = GUIInputWrapper::ScriptedPadId();
			Require(ScriptedPadCount() == 1, "probe pad must be the one shared scripted device");
		} else if (op == "input_scope") {
			GUIInputWrapper::SetAutomationDriving(step.at("enabled").get<bool>());
		} else if (op == "menu") {
			std::istringstream args(step.at("command").get<std::string>());
			std::string command, name, detail;
			args >> command;
			bool accepted = false;
			if (command == "activate" || command == "post_command") {
				args >> name;
				if (auto* pause = g_MenuMan.GetActivePauseMenu()) accepted = pause->AutomationPostCommand(name);
				else if (g_MenuMan.IsMainMenuInteractive()) accepted = g_MenuMan.GetMainMenu()->AutomationPostCommand(name);
			} else if (command == "assert_enabled") {
				int expected = -1; args >> name >> expected;
				accepted = MenuControls() && (expected == 0 || expected == 1) && MenuControls()->GetControl(name) && MenuAutomation::Enabled(MenuControls()->GetControl(name)) == (expected == 1);
			} else {
				Require(MenuAutomation::Handles(command), "unknown menu operation: " + command);
				accepted = MenuAutomation::Execute(MenuControls(), MenuScreen(), command, args, detail);
			}
			observed["accepted"] = accepted;
			observed["menu_observation"] = detail;
			Require(accepted == step.value("accepted", true), "menu operation refused: " + step.at("command").get<std::string>() + " " + detail);
		} else if (op == "mouse_down" || op == "mouse_up" || op == "mouse_move") {
			auto* control = Control(step);
			Require((step.value("scope", "") == "menu" ? MenuAutomation::Visible(control) : g_MenuMan.IsNetworkPanelOpen() && control->GetVisible()), "mouse target is not visible");
			int x, y, w, h;
			control->GetControlRect(&x, &y, &w, &h);
			const float mouseX = static_cast<float>((x + w / 2) * g_WindowMan.GetResMultiplier());
			const float mouseY = static_cast<float>((y + h / 2) * g_WindowMan.GetResMultiplier());
			SDL_Event motion{};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			motion.motion.x = mouseX;
			motion.motion.y = mouseY;
			Push(motion);
			if (op == "mouse_move") return true;
			SDL_Event event{};
			event.type = op == "mouse_down" ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = motion.motion.windowID;
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = op == "mouse_down";
			event.button.x = mouseX;
			event.button.y = mouseY;
			Push(event);
			observed["control"] = ReadControl(control);
		} else if (op == "assert") {
			for (auto it = step.at("equals").begin(); it != step["equals"].end(); ++it) {
				Require(observed.at(it.key()) == it.value(), "assertion differs: " + it.key());
			}
			if (step.contains("sim_at_least")) Require(observed["sim_frame"].get<long long>() >= step["sim_at_least"].get<long long>(), "simulation did not advance");
		} else if (op == "assert_control") {
			observed["control"] = ReadControl(Control(step));
			const auto& value = observed["control"];
			for (auto it = step.at("equals").begin(); it != step["equals"].end(); ++it) {
				Require(value.at(it.key()) == it.value(), "control assertion differs: " + it.key());
			}
			if (step.contains("text_contains")) Require(value.at("text").get<std::string>().find(step["text_contains"].get<std::string>()) != std::string::npos, "control text is missing expected content");
			if (step.value("fits", false)) {
				const auto& rect = value["rect"];
				Require(rect[0].get<int>() >= 0 && rect[1].get<int>() >= 0 && rect[0].get<int>() + rect[2].get<int>() <= g_WindowMan.GetResX() &&
				    rect[1].get<int>() + rect[3].get<int>() <= g_WindowMan.GetResY(), "control exceeds viewport");
				if (value.contains("text_height")) Require(value["text_height"].get<int>() <= rect[3].get<int>(), "label text exceeds its height");
			}
		} else if (op == "place_brain_command") {
			// A placement exactly as issued, for the commands every peer has to refuse.
			Require(GameActivity::EnqueueRawBrainPlacement(step.value("player", 0), step.value("team", 0),
			            step.value("x", 0.0F), step.value("y", 0.0F), step.value("class", std::string("Actor")),
			            step.value("preset", std::string("Brain Case")), step.value("module", std::string("Base.rte"))),
			    "the match cannot take a placement command");
		} else if (op == "editor_place_brain" || op == "editor_done" || op == "editor_place" || op == "actor_select") {
			// The seat's own editor does the work: the gesture is queued once and the step waits it out.
			const int player = step.value("player", 0);
			if (probe.gestureIndex != probe.index) {
				if (op != "actor_select") {
					Require(observed["editing"] == true, "the activity is not in the setup editor");
				}
				const std::string kind = op == "editor_done" ? "done" : op == "editor_place" ? "place_object" :
				    op == "actor_select" ? "actor_select" : "place_brain";
				const bool object = kind == "place_object";
				Require(GameActivity::QueueSetupEditorGesture(player, kind,
				            step.value("x_fraction", 0.5F),
				            step.value("class", object ? std::string("HDFirearm") : std::string("Actor")),
				            step.value("preset", object ? std::string("Pistol") : std::string("Brain Case")),
				            step.value("module", std::string("Base.rte"))),
				    "the seat cannot take an editor gesture");
				probe.gestureIndex = probe.index;
			}
			const int status = GameActivity::SetupEditorGestureStatus(player);
			Require(status != 2, "the seat could not carry out its editor gesture");
			if (status == 1) return false;
		} else if (op == "assert_editor") {
			const int player = step.value("player", 0);
			const auto seat = std::find_if(observed["editor_seats"].begin(), observed["editor_seats"].end(),
			    [player](const Json& row) { return row.at("player") == player; });
			Require(seat != observed["editor_seats"].end(), "seat " + std::to_string(player) + " is not a local editor seat");
			if (step.contains("equals")) {
				for (auto it = step["equals"].begin(); it != step["equals"].end(); ++it) {
					Require(seat->at(it.key()) == it.value(), "editor seat assertion differs: " + it.key());
				}
			}
			if (step.contains("screen_text_contains")) {
				Require(seat->at("screen_text").get<std::string>().find(step["screen_text_contains"].get<std::string>()) != std::string::npos,
				    "the seat's screen does not carry the expected message");
			}
		} else if (op == "assert_net_ui_clear") {
			// The network overlay owes the stock setup editor its own surfaces: the picker and the seat's
			// message band. In a running match ("match") the open seats panel is the surface it owes instead.
			const int player = step.value("player", 0);
			const bool match = step.value("match", false);
			const auto seat = std::find_if(observed["editor_seats"].begin(), observed["editor_seats"].end(),
			    [player](const Json& row) { return row.at("player") == player; });
			if (!match) {
				Require(seat != observed["editor_seats"].end(), "seat " + std::to_string(player) + " is not a local editor seat");
				Require(observed["editing"] == true, "the activity is not in the setup editor");
			}
			// The status widget is the overlay's own surface wherever its mode lets it draw; an Off
			// match honestly has none, so a step may say so rather than fake one.
			if (step.value("status", true)) {
				Require(observed["net_ui"]["status"].at("visible") == true, "the network status widget is not on screen");
			}
			const BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
			// No overlay rectangle ever leaves the window, and a visible one keeps a positive area.
			for (const std::string& element: {"status", "toasts", "seats_panel"}) {
				const Json& r = observed["net_ui"].at(element);
				if (!r.at("visible").get<bool>()) continue;
				Require(r["w"].get<int>() > 0 && r["h"].get<int>() > 0,
				    "the network " + element + " has no area");
				Require(r["x"].get<int>() >= 0 && r["y"].get<int>() >= 0 &&
				    r["x"].get<int>() + r["w"].get<int>() <= backbuffer->w &&
				    r["y"].get<int>() + r["h"].get<int>() <= backbuffer->h,
				    "the network " + element + " leaves the window");
			}
			const Json& seatsPanel = observed["net_ui"].at("seats_panel");
			if (match) {
				Require(seatsPanel.at("visible") == true, "the seats panel is not open");
				Require(observed["net_ui"]["toasts"].at("visible") == true, "no toast is live for the reserved row");
				for (const std::string& element: {"status", "toasts"}) {
					Require(!Overlaps(observed["net_ui"].at(element), seatsPanel),
					    "the network " + element + " overlaps the seats panel");
				}
				for (const auto& other: observed["editor_seats"]) {
					if (!other.contains("text_band") || !other.at("text_band").value("visible", false)) continue;
					const Json& band = other.at("text_band");
					Require(!Overlaps(seatsPanel, band), "the seats panel overlaps a seat message band");
					for (const std::string& element: {"status", "toasts"}) {
						Require(!Overlaps(observed["net_ui"].at(element), band),
						    "the network " + element + " overlaps a seat message band");
					}
				}
				return true; // a match-mode step ends here; the editor checks are the non-match path's
			}
			if (step.value("picker_open", false)) Require(seat->at("picker").at("visible") == true, "the editor's object picker is not open");
			if (step.value("screen_text", false)) Require(seat->at("screen_text_rect").at("visible") == true, "the seat's screen carries no editor message");
			const Json& band = seat->at("screen_text_rect");
			if (band.at("visible").get<bool>()) {
				// The band reads in window space, so it is held against the seat's own framebuffer rect.
				const auto* game = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
				Vector offset;
				g_FrameMan.GetScreenOffsetForSplitScreen(game ? game->ScreenOfPlayer(player) : 0, offset);
				Require(band["x"].get<int>() >= offset.GetRoundIntX() && band["y"].get<int>() >= offset.GetRoundIntY() &&
				    band["x"].get<int>() + band["w"].get<int>() <= offset.GetRoundIntX() + g_FrameMan.GetPlayerScreenWidth() &&
				    band["y"].get<int>() + band["h"].get<int>() <= offset.GetRoundIntY() + g_FrameMan.GetPlayerScreenHeight(),
				    "the seat's message is drawn off its own screen");
			}
			for (const std::string& element: {"status", "toasts", "seats_panel"}) {
				if (element == "seats_panel" && !seatsPanel.at("visible").get<bool>()) continue;
				if (element != "seats_panel") {
					// An open seats panel owns its rows too - the overlay lifts above it rather than draw over it.
					Require(!Overlaps(observed["net_ui"].at(element), seatsPanel),
					    "the network " + element + " overlaps the seats panel");
				}
				for (const auto& other: observed["editor_seats"]) {
					for (const std::string& area: {"picker", "screen_text_rect", "text_band"}) {
						if (!other.contains(area)) continue;
						Require(!Overlaps(observed["net_ui"].at(element), other.at(area)),
						    "the network " + element + " overlaps the editor's " + area);
					}
				}
			}
		} else if (op == "screenshot_pair") {
			// Both shots of one capture inside a single step, so the overlay readback and the composited
			// screen buffer are the same rendered frame.
			const std::string name = step.at("name").get<std::string>();
			auto path = Leaf(name);
			path += ".png";
			Require(!std::filesystem::exists(path), "screenshot already exists");
			Require(g_FrameMan.SaveBitmapToPNG(g_FrameMan.GetBackBuffer32(), path.string().c_str()) == 0, "screenshot save failed");
			const std::string composited = step.value("composited_name", name + "_composited");
			(void)Leaf(composited);
			Require(g_FrameMan.SaveScreenToPNG(composited.c_str()) == 0, "composited screenshot save failed");
			observed["screenshot"] = path.string();
			observed["screenshot_composited"] = composited;
		} else if (op == "screenshot") {
			const std::string name = step.at("name").get<std::string>();
			if (step.value("composited", false)) {
				// The frame as it reaches the screen - world, editor and overlay - read back from the screen
				// buffer into the run's ScreenShots directory, where the harness collects it.
				(void)Leaf(name); // Validates the name's charset; the file lands under the run's ScreenShots.
				Require(g_FrameMan.SaveScreenToPNG(name.c_str()) == 0, "composited screenshot save failed");
				observed["screenshot"] = name;
			} else {
				auto path = Leaf(name);
				path += ".png";
				Require(!std::filesystem::exists(path), "screenshot already exists");
				Require(g_FrameMan.SaveBitmapToPNG(g_FrameMan.GetBackBuffer32(), path.string().c_str()) == 0, "screenshot save failed");
				observed["screenshot"] = path.string();
			}
		} else if (op == "signal") {
			auto path = Leaf(step.at("name").get<std::string>());
			path += ".json";
			Require(!std::filesystem::exists(path), "signal already exists");
			std::ofstream output(path);
			output << observed.dump() << '\n';
			Require(static_cast<bool>(output), "cannot write signal");
		} else if (op == "wait_file") {
			if (!std::filesystem::is_regular_file(step.at("path").get<std::string>())) return false;
		} else if (op == "finish") {
			GUIInputWrapper::SetAutomationDriving(false);
			GUIInputWrapper::ReleaseScriptedPad();
			Require(ScriptedPadCount() == 0, "a scripted pad remained after finish");
			const char* hint = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
			const std::string now = hint ? hint : "";
			Require(now == probe.hintAtLoad && (hint != nullptr) == probe.hintAtLoadPresent,
			    "joystick background hint left at \"" + now + "\"");
			Require(probe.index + 1 == probe.script["steps"].size(), "finish must be last");
			probe.done = true;
			probe.result["complete"] = true;
			probe.result["pass"] = true;
		} else {
			throw std::runtime_error("unknown probe operation: " + op);
		}
		return true;
	}

	void Process(Phase phase, bool menuScopeOnly = false) {
		try {
			if (!probe.loaded) {
				if (phase == Phase::Sim) return;
				Load();
			}
			if (!probe.enabled || probe.done) return;
			if (phase == Phase::Draw) ++probe.renders;
			Require(NowMs() <= probe.script.at("timeout_ms").get<uint64_t>(), "script deadline at step " + std::to_string(probe.index));
			Require(probe.index < probe.script["steps"].size(), "script did not finish explicitly");
			const auto& step = probe.script["steps"][probe.index];
			if (StepPhase(step) != phase) return;
			if (menuScopeOnly && !MenuScopeStep(step)) return;
			Json observed = Observe();
			try {
				if (!Step(step, observed)) return;
			} catch (...) {
				probe.result["failed_observation"] = observed;
				throw;
			}
			probe.result["steps"].push_back({{"index", probe.index}, {"op", step.at("op")}, {"observed", observed}});
			++probe.index;
			probe.stepRender = probe.renders;
			probe.stepMs = NowMs();
			WriteResult();
			if (probe.done) std::cout << "[net-ui-probe] PASS: completed " << probe.index << " steps" << std::endl;
		} catch (const std::exception& error) {
			GUIInputWrapper::SetAutomationDriving(false);
			GUIInputWrapper::ReleaseScriptedPad();
			probe.done = true;
			probe.result["pass"] = false;
			probe.result["error"] = error.what();
			probe.result["failed_step"] = probe.index;
			try { if (probe.resultStarted) WriteResult(); } catch (...) {}
			std::cerr << "[net-ui-probe] FAIL: " << error.what() << std::endl;
			System::SetQuit(true);
		}
	}
}

void BeforePoll() { Process(Phase::Poll); }
void AfterDraw() { Process(Phase::Draw); }
void AfterMenuDraw() { Process(Phase::Draw, true); }

void OnSimTick(uint64_t simUpdateCount) {
	if (!probe.enabled || probe.done) return;
	probe.simTick = simUpdateCount;
	Process(Phase::Sim);
}
}
