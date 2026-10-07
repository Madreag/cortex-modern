#include "NetModerationGUIProbe.h"

#include "Actor.h"
#include "ActivityMan.h"
#include "CameraMan.h"
#include "Controller.h"
#include "FrameMan.h"
#include "GameActivity.h"
#include "GUI.h"
#include "GUIButton.h"
#include "GUIFont.h"
#include "GUILabel.h"
#include "GnsTransport.h"
#include "GUIListBox.h"
#include "GUITextBox.h"
#include "GUIInputWrapper.h"
#include "MainMenuGUI.h"
#include "PauseMenuGUI.h"
#include "MenuMan.h"
#include "MenuAutomation.h"
#include "Scene.h"
#include "SceneMan.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "NetModerationGUI.h"
#include "NetProtocol.h"
#include "ScenarioRunner.h"
#include "System.h"
#include "TimerMan.h"
#include "UInputMan.h"
#include "WindowMan.h"
#include "BuyMenuGUI.h"
#include "PieMenu.h"
#include "PieSlice.h"
#include "MetricsCollector.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
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
		size_t index = 0, gestureIndex = SIZE_MAX, handIndex = SIZE_MAX; //!< handIndex: the step whose hand gesture is still running.
		uint64_t renders = 0, stepRender = 0, stepMs = 0, simTick = 0, resultWrittenMs = 0;
		Clock::time_point started;
		Clock::time_point loadedAt; //!< The label dump's one clock: the script's load, which the activation and a round's reset never move.
		uint64_t labelDumpMs = 0, labelWrittenMs = 0;
		std::string labelBoundary;
		std::filesystem::path directory;
		Json script, result;
		std::string hintAtLoad;
		bool hintAtLoadPresent = false;
		bool holdsPad = false;
		uint64_t round = 0;
		bool phaseArmed = false;
		bool roundEndArmed = false;
		std::vector<std::string> roundEndSignals;
		bool pageDown = false; //!< show_row holds the More players press it made.
		uint64_t pageRender = 0; //!< The render show_row acts again at.
		int pageTurns = 0; //!< Pages show_row has turned for the row it looks for.
	};
	Probe probe;
	std::atomic<uint64_t> rendezvousCount{0};

	// The moment this peer's script stops waiting on another peer: named on its own line, and
	// counted so an engine watchdog can measure the wait that follows it, not the one before.
	void NoteRendezvous(const std::string& name) {
		const uint64_t count = rendezvousCount.fetch_add(1) + 1;
		System::PrintDiagnosticLine("[net-ui-probe] rendezvous " + name + " count=" + std::to_string(count) +
		                            " tick=" + std::to_string(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount())));
	}

	GUIControlManager* MenuControls() {
		if (auto* panel = g_MenuMan.GetNetworkPanel(); panel && panel->IsChatEntryOpen()) return panel->OverlayManager();
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

	void ScriptedPadCensus(int& scripted, bool& wrapper, bool& leftoverProbe) {
		scripted = 0;
		wrapper = false;
		leftoverProbe = false;
		int count = 0;
		SDL_JoystickID* ids = SDL_GetJoysticks(&count);
		for (int i = 0; i < count; ++i) {
			const char* name = SDL_GetJoystickNameForID(ids[i]);
			if (!name) continue;
			if (std::string(name) == "Menu script controller") {
				++scripted;
				wrapper = true;
			} else if (std::string(name) == "Net UI probe controller") {
				++scripted;
				leftoverProbe = true;
			}
		}
		SDL_free(ids);
	}

	int ScriptedPadCount() {
		int scripted = 0;
		bool wrapper = false, leftoverProbe = false;
		ScriptedPadCensus(scripted, wrapper, leftoverProbe);
		return scripted;
	}

	void ReleaseProbePad() {
		if (!probe.holdsPad) return;
		GUIInputWrapper::ReleaseScriptedPad();
		probe.holdsPad = false;
	}

	uint64_t NowMs() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - probe.started).count());
	}

	// The wait for a script's activation tick or phase has its own deadline, counted from the load or the round's reset.
	constexpr uint64_t c_DefaultActivationTimeoutMs = 600000;
	bool ActivationPending(const Json& script, bool pending, uint64_t waitedMs) {
		if (!pending) return false;
		const uint64_t deadline = script.value("activation_timeout_ms", c_DefaultActivationTimeoutMs);
		Require(waitedMs <= deadline, "script deadline waiting " + std::to_string(waitedMs) + " ms for activation");
		return true;
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
		// sim_frame counts every sim update since launch, lobby ticks included; lockstep_frame is the round's own.
		const uint64_t lockstepFrame = ScenarioRunner::HasLockstepCoordinator() ? ScenarioRunner::GetLockstepCompletedFrame() : 0;
		Json observed = {{"at_ms", NowMs()}, {"render", probe.renders}, {"sim_frame", g_TimerMan.GetSimUpdateCount()},
		    {"lockstep_frame", lockstepFrame},
		    {"screen", MenuScreen()},
		    {"service", snapshot.serviceState}, {"host", snapshot.isHost}, {"activity_preset", snapshot.activityPreset},
		    {"panel_open", g_MenuMan.IsNetworkPanelOpen()},
		    {"paused", g_ActivityMan.ActivityPaused()}, {"seats", Json::array()}};
		observed["chat_history"] = Json::array();
		for (const auto& line: g_NetMatchService.ChatHistory()) observed["chat_history"].push_back({{"sender", line.senderPeerId}, {"scope", line.scope}, {"text", line.text}});
		for (const auto& seat: g_NetMatchService.GetModerationSeats()) {
			observed["seats"].push_back({{"seat", seat.stableSeat}, {"name", seat.displayName}, {"dropped", seat.dropped},
			    {"closed", seat.closed}, {"substituting", seat.substituting}, {"reclaiming", seat.reclaiming},
			    {"applicants", seat.applicants.size()}, {"actions_available", seat.actionsAvailable},
			    {"holder_generation", seat.holderGeneration}, {"seat_generation", seat.seatGeneration}});
		}
		// This peer's own player plays on only while its controlled actor is there and alive.
		bool localActorAlive = false;
		if (Activity* activity = g_ActivityMan.GetActivity()) {
			for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
				if (!activity->IsLocalHumanSeat(player)) continue;
				const Actor* actor = activity->GetControlledActor(player);
				localActorAlive = actor && !actor->IsDead();
				break;
			}
		}
		observed["local_actor_alive"] = localActorAlive;
		observed["local_peer"] = snapshot.localPeerId;
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
		// The heights the band laid itself out with, so a failure names them instead of only the rectangle.
		const NetModerationGUI::ChatBand chatBand = panel ? panel->GetChatBand() : NetModerationGUI::ChatBand{};
		observed["net_ui"] = {{"status", panel ? OverlayRect(panel->GetStatusRect()) : Rect(0, 0, 0, 0, false)},
		    {"toasts", panel ? OverlayRect(panel->GetToastRect()) : Rect(0, 0, 0, 0, false)},
		    {"chat", panel ? OverlayRect(panel->GetChatRect()) : Rect(0, 0, 0, 0, false)},
		    {"roster", panel ? OverlayRect(panel->GetRosterRect()) : Rect(0, 0, 0, 0, false)},
		    {"chat_entry_open", panel && panel->IsChatEntryOpen()},
		    {"chat_rows", chatBand.rows},
		    {"chat_row_height", chatBand.rowHeight},
		    {"chat_entry_height", chatBand.entryHeight},
		    {"chat_history_visible", chatBand.historyVisible},
		    {"chat_text_size_reduced", chatBand.reducedTextSize},
		    {"seat_input_typed_into", g_UInputMan.SeatInputTypedInto()},
		    {"seats_panel", seats}};
		observed["controllers"] = Json::array();
		const int probePad = probe.holdsPad ? static_cast<int>(GUIInputWrapper::ScriptedPadId()) : 0;
		observed["probe_pad"] = probePad;
		int boundSeat = 0;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			const InputDevice device = g_UInputMan.GetControlScheme(player)->GetDevice();
			const int joystickId = static_cast<int>(g_UInputMan.GetGamepadID(device));
			Controller controller;
			controller.Create(Controller::CIM_PLAYER, player);
			controller.Update();
			bool any = false;
			for (int state = 0; state < CONTROLSTATECOUNT; ++state) {
				if (controller.IsState(static_cast<ControlState>(state))) { any = true; break; }
			}
			const bool startHeld = g_UInputMan.ElementHeld(player, InputElements::INPUT_START);
			const int moved = (any || startHeld) ? 1 : 0;
			if (probePad && joystickId == probePad) boundSeat = player + 1;
			observed["controllers"].push_back({{"player", player}, {"seat", player + 1}, {"device", static_cast<int>(device)},
			    {"joystick_id", joystickId}, {"moved", moved}, {"start", startHeld},
			    {"primary", controller.IsState(PRIMARY_ACTION)}});
		}
		observed["bound_seat"] = boundSeat;
		return observed;
	}

	void WriteSignal(const std::string& name, const Json& observed) {
		auto path = Leaf(name);
		path += ".json";
		Require(!std::filesystem::exists(path), "signal already exists");
		std::ofstream output(path);
		output << observed.dump() << '\n';
		Require(static_cast<bool>(output), "cannot write signal");
		NoteRendezvous(name);
	}

	/// Whether the round is over by its own rules or its service's end rather than by a step still ahead.
	bool RoundEnded() {
		const Activity* activity = g_ActivityMan.GetActivity();
		return !activity || activity->IsOver() || g_NetMatchService.GetState() != NetMatchServiceState::Running;
	}

	void WriteResult();

	/// A script's "label_dump": {"every_ms": N} writes every line the screen shows - the menus, the network panel and overlay, the
	/// game's own screen message, the text drawn by hand - every N ms of this peer's clock (5000 by default) and at every change of
	/// screen, service state or image transfer, from the script's load (before its activation) to its end: what a joiner reads while
	/// a world's image comes, while it loads and while it catches up.
	void LabelDump() {
		const Json config = probe.script.value("label_dump", Json());
		if (!config.is_object()) return;
		const auto snapshot = g_NetMatchService.GetLobbySnapshot();
		const std::string screen = MenuScreen();
		const bool catchingUp = ScenarioRunner::WorldCatchUpActive();
		const std::string boundary = screen + '|' + snapshot.serviceState + '|' + (snapshot.transferTotalBytes != 0 ? "transfer" : "") + '|' + (catchingUp ? "catchup" : "");
		const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - probe.loadedAt).count());
		const bool atBoundary = boundary != probe.labelBoundary;
		if (!atBoundary && now < probe.labelDumpMs + config.value("every_ms", uint64_t{5000})) return;
		probe.labelBoundary = boundary;
		probe.labelDumpMs = now;
		const uint64_t unixMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		Json record = {{"at_ms", now}, {"unix_ms", unixMs}, {"why", atBoundary ? "boundary" : "cadence"}, {"screen", screen}, {"service", snapshot.serviceState},
		    {"transfer", {{"received_bytes", snapshot.transferReceivedBytes}, {"total_bytes", snapshot.transferTotalBytes}}}, {"catching_up", catchingUp},
		    {"sim_frame", g_TimerMan.GetSimUpdateCount()}, {"lockstep_frame", ScenarioRunner::HasLockstepCoordinator() ? ScenarioRunner::GetLockstepCompletedFrame() : 0},
		    {"lines", Json::parse(MenuAutomation::ShownTextJson(MenuControls()))}};
		if (!probe.result.contains("label_dumps")) probe.result["label_dumps"] = Json::array();
		probe.result["label_dumps"].push_back(std::move(record));
		System::PrintDiagnosticLine("[net-ui-probe] label dump at_ms=" + std::to_string(now) + " screen=" + screen + " service=" + snapshot.serviceState +
		                            " transfer=" + std::to_string(snapshot.transferReceivedBytes) + "/" + std::to_string(snapshot.transferTotalBytes) + (atBoundary ? " boundary" : ""));
		// A dump before the script's steps begin is the only record of what came before them, so it is written within a second.
		if (now >= probe.labelWrittenMs + 1000) {
			WriteResult();
			probe.labelWrittenMs = now;
		}
	}

	/// Completes a script whose round ended while finish_on_round_end was armed: the steps it had left are recorded as
	/// skipped and the signals it named are written, so a peer waiting on them goes on.
	void FinishOnRoundEnd(const Json& observed) {
		GUIInputWrapper::SetAutomationDriving(false);
		ReleaseProbePad();
		for (const std::string& name: probe.roundEndSignals) {
			if (!std::filesystem::exists(Leaf(name).string() + ".json")) WriteSignal(name, observed);
		}
		const auto& steps = probe.script["steps"];
		const size_t skipped = steps.size() - std::min(probe.index, steps.size());
		probe.result["finished_on_round_end"] = {{"at_step", probe.index}, {"op", probe.index < steps.size() ? steps[probe.index].value("op", "") : ""},
		    {"skipped", skipped}, {"signals", probe.roundEndSignals}, {"observed", observed}};
		probe.roundEndArmed = false;
		probe.done = true;
		probe.result["complete"] = true;
		probe.result["pass"] = true;
		WriteResult();
		System::PrintDiagnosticLine("[net-ui-probe] PASS: completed on the round's end at step " + std::to_string(probe.index) + " (" +
		                            std::to_string(skipped) + " steps skipped)");
	}

	void WriteResult() {
		const bool repeat = probe.script.value("repeat_rounds", false);
		// A probe that repeats per round reports per round: before its first round it has only a failure to report.
		if (repeat && probe.round == 0 && !probe.result.contains("error")) return;
		const std::string name = repeat ? "net-ui-result.round" + std::to_string(probe.round) + ".json" : "net-ui-result.json";
		std::ofstream output(probe.directory / name);
		output << probe.result.dump(2) << '\n';
		Require(static_cast<bool>(output), "cannot write probe result");
	}

	void Load() {
		probe.loaded = true;
		const char* path = std::getenv("CC_TEST_NET_UI_SCRIPT");
		if (!path || !*path) return;
		probe.enabled = true;
		probe.started = probe.loadedAt = Clock::now();
		const char* hint = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
		probe.hintAtLoadPresent = hint != nullptr;
		probe.hintAtLoad = hint ? hint : "";
		probe.directory = std::filesystem::absolute(path).parent_path();
		MenuAutomation::SetArtifactDirectory(probe.directory.string());
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
		const auto activation = probe.script.value("activation_timeout_ms", c_DefaultActivationTimeoutMs);
		Require(activation > 0 && activation <= c_DefaultActivationTimeoutMs, "invalid activation deadline");
		for (const auto& step: probe.script["steps"]) {
			const std::string op = step.value("op", "");
			if (op != "key_down" && op != "key_up") continue;
			if (SimRateKey(step.value("key", ""))) {
				const bool exact = step.contains("sim_at") && step["sim_at"].is_number_unsigned();
				const bool least = step.contains("sim_at_least") && step["sim_at_least"].is_number_unsigned();
				Require(exact ^ least, "sim-rate probe key needs sim_at or sim_at_least");
			} else {
				Require(!step.contains("sim_at") && !step.contains("sim_at_least"), "sim_at and sim_at_least are only for sim-rate probe keys");
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
		} else if (auto* box = dynamic_cast<GUITextBox*>(control)) {
			value["text"] = box->GetText();
		} else if (auto* list = dynamic_cast<GUIListBox*>(control)) {
			// A list reads as its rows, one per line, and the row it has selected.
			std::string text;
			value["items"] = Json::array();
			for (const auto* item: *list->GetItemList()) {
				text += (text.empty() ? "" : "\n") + item->m_Name;
				value["items"].push_back(item->m_Name);
			}
			value["text"] = text;
			value["selected"] = list->GetSelectedIndex();
		}
		return value;
	}

	/// The route the transport holds now for this peer's connection to its host: GNS's live flag, not the panel's copy of it.
	std::string LiveRoute() {
		for (const GnsProcessConnection& connection: GnsTransport::GetProcessConnections()) {
			if (connection.p2p && !connection.host && !connection.info.connectedRoute.empty()) return connection.info.connectedRoute;
		}
		return {};
	}

	/// "watch_route": every frame the panel's "via <route>" is read beside the transport's live route. A route that moved may take the
	/// panel up to grace_ms (2000) to follow; past that, or a route other than "expect" when one is named, fails the script. Each move
	/// of either is recorded with its time, so a run that moved shows the panel at both routes.
	void WatchRoute(const Json& step) {
		const std::string text = ReadControl(Control(step)).at("text").get<std::string>();
		const size_t at = text.find(" via ");
		std::string shown = at == std::string::npos ? std::string() : text.substr(at + 5);
		shown = shown.substr(0, shown.find_first_of(" \n"));
		const std::string live = LiveRoute();
		const uint64_t now = NowMs();
		Json& watch = probe.result["route_watch"];
		if (!watch.is_object()) watch = {{"samples", 0}, {"moves", Json::array()}, {"live", live}, {"shown", shown}, {"live_since_ms", now}, {"first_ms", now}};
		for (const auto& [what, value]: {std::pair<const char*, const std::string*>{"live", &live}, {"shown", &shown}}) {
			if (watch[what] == *value) continue;
			watch["moves"].push_back({{"at_ms", now}, {"what", what}, {"from", watch[what]}, {"to", *value}});
			watch[what] = *value;
			if (std::string(what) == "live") watch["live_since_ms"] = now;
			System::PrintDiagnosticLine("[net-ui-probe] route " + std::string(what) + " -> " + *value + " at_ms=" + std::to_string(now));
		}
		watch["samples"] = watch["samples"].get<uint64_t>() + 1;
		watch["last_ms"] = now;
		const std::string expect = step.value("expect", "");
		Require(!live.empty(), "the transport holds no live route to the host");
		Require(expect.empty() || live == expect, "the transport's route is " + live + ", the script expects " + expect);
		Require(shown == live || now - watch["live_since_ms"].get<uint64_t>() <= step.value("grace_ms", uint64_t{2000}),
		        "the panel says via " + shown + " while the transport's route has been " + live + " for " + std::to_string(now - watch["live_since_ms"].get<uint64_t>()) + " ms");
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
		    op == "assert_buy" || op == "assert_pie" || op == "assert_window" ||
		    op == "screenshot" || op == "screenshot_pair" || op == "finish" || op == "watch_route") return Phase::Draw;
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
		if (op == "assert_buy" || op == "assert_pie" || op == "assert_window") {
			Json scope;
			int player = step.value("player", 0);
			if (step.contains("input_player")) {
				const auto* activity = g_ActivityMan.GetActivity();
				player = -1;
				for (int seat = 0; activity && seat < Players::MaxPlayerCount; ++seat)
					if (activity->LocalInputOfPlayer(seat) == step["input_player"].get<int>()) { player = seat; break; }
			}
			if (op == "assert_buy") {
				auto* activity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity());
				auto* buy = activity && player >= 0 && player < Players::MaxPlayerCount ? activity->GetBuyGUI(player) : nullptr;
				Require(buy != nullptr, "buy scope has no local seat manager");
				std::list<const SceneObject*> order;
				buy->GetOrderList(order);
				Json cart = Json::array();
				for (const auto* item: order) cart.push_back(item->GetModuleAndPresetName());
				const auto* craft = buy->GetDeliveryCraftPreset();
				scope = {{"player", player}, {"visible", buy->IsVisible()}, {"enabled", buy->IsEnabled()},
				    {"buy_allowed", activity->GetBuyMenuEnabled()}, {"cart", std::move(cart)},
				    {"craft", craft ? craft->GetModuleAndPresetName() : ""}, {"cost", buy->GetTotalOrderCost()},
				    {"mass", buy->GetTotalOrderMass()}, {"passengers", buy->GetTotalOrderPassengers()}};
			} else if (op == "assert_pie") {
				auto* activity = g_ActivityMan.GetActivity();
				auto* actor = activity && player >= 0 && player < Players::MaxPlayerCount ? activity->GetControlledActor(player) : nullptr;
				auto* pie = actor ? actor->GetPieMenu() : nullptr;
				Require(pie != nullptr, "pie scope has no controlled actor");
				Json commands = Json::array();
				for (const auto* slice: pie->GetPieSlices()) commands.push_back(static_cast<int>(slice->GetType()));
				scope = {{"player", player}, {"actor", actor->GetUniqueID()}, {"visible", pie->IsVisible()},
				    {"enabled", pie->IsEnabled()}, {"command", static_cast<int>(pie->GetPieCommand())}, {"commands", std::move(commands)}};
			} else {
				const auto flags = SDL_GetWindowFlags(g_WindowMan.GetWindow());
				scope = {{"width", g_WindowMan.GetResX()}, {"height", g_WindowMan.GetResY()}, {"fullscreen", g_WindowMan.IsFullscreen()},
				    {"minimized", (flags & SDL_WINDOW_MINIMIZED) != 0}, {"hidden", (flags & SDL_WINDOW_HIDDEN) != 0}};
			}
			observed["scope"] = scope;
			const Json expectedValues = step.value("equals", Json::object());
			for (const auto& [key, expected]: expectedValues.items()) Require(scope.at(key) == expected, op + " differs: " + key);
			g_MetricsCollector.WriteObservation({{"type", "probe_scope"}, {"scope", op}, {"observed", scope}});
		} else if (op == "remove_participant") {
			const uint16_t seat = step.at("stable_seat");
			const auto seats = g_NetMatchService.GetModerationSeats();
			const auto found = std::find_if(seats.begin(), seats.end(), [=](const auto& row) { return row.stableSeat == seat; });
			Require(found != seats.end(), "participant selection is absent");
			const auto selection = NetSelectModerationSeat(*found);
			const auto action = step.value("ban", false) ? NetParticipantRemovalAction::BanUntilRemoved : NetParticipantRemovalAction::Kick;
			const auto result = g_NetMatchService.RemoveParticipant(selection, action);
			observed["removal"] = {{"seat", seat}, {"incarnation", selection.incarnation}, {"result", NetKickBanResultName(result)}};
			Require(std::string(NetKickBanResultName(result)) == step.value("expected", "Ok"), "participant removal result differs");
			g_MetricsCollector.WriteObservation({{"type", "participant_removal"}, {"observed", observed["removal"]}});
		} else if (op == "wait") {
			Require(step.contains("service") || step.contains("sim_at_least") || step.contains("lockstep_frame_at_least") || step.contains("renders") ||
			    step.contains("elapsed_ms") || step.contains("panel_open") || step.contains("control") || step.contains("screen") ||
			    step.contains("editing") || step.contains("seat_ready") || step.contains("seat_text_contains") ||
			    step.contains("picker_open") || step.contains("chat_entry_open") || step.contains("local_peer_at_most") || step.contains("paused"),
			    "wait has no predicate");
			// The title screen's own scene reads as Gameplay too, paused; a started game runs.
			if (step.contains("paused") && observed["paused"] != step["paused"]) return false;
			if (step.contains("chat_entry_open") && observed["net_ui"].at("chat_entry_open") != step["chat_entry_open"]) return false;
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
			if (step.contains("sim_at_least")) {
				const char* clock = probe.script.value("sim_clock", "process") == "lockstep" ? "lockstep_frame" : "sim_frame";
				if (observed[clock].get<long long>() < step["sim_at_least"].get<long long>()) return false;
			}
			// A watcher plays a seat once its own id is a seat's.
			if (step.contains("local_peer_at_most") && (observed["local_peer"].get<int>() == 0 || observed["local_peer"].get<int>() > step["local_peer_at_most"].get<int>())) return false;
			if (step.contains("lockstep_frame_at_least") && observed["lockstep_frame"].get<uint64_t>() < step["lockstep_frame_at_least"].get<uint64_t>()) return false;
			if (step.contains("renders") && probe.renders - probe.stepRender < step["renders"].get<uint64_t>()) return false;
			if (step.contains("elapsed_ms") && NowMs() - probe.stepMs < step["elapsed_ms"].get<uint64_t>()) return false;
			if (step.contains("panel_open") && observed["panel_open"] != step["panel_open"]) return false;
			if (step.contains("control")) {
				observed["control"] = ReadControl(Control(step));
				if (step.contains("equals")) {
					for (auto it = step.at("equals").begin(); it != step["equals"].end(); ++it) {
						if (observed["control"].at(it.key()) != it.value()) return false;
					}
				}
				if (step.contains("text_contains")) {
					if (observed["control"].at("text").get<std::string>().find(step["text_contains"].get<std::string>()) == std::string::npos) return false;
				}
			}
		} else if (op == "key_down" || op == "key_up") {
			const std::string key = step.at("key");
			// The movement letters drive a seat's actor the way a player's keyboard does.
			const bool movement = key == "A" || key == "D" || key == "W" || key == "S";
			Require(movement || key == "F6" || key == "Escape" || key == "P" || key == "CHAT" || key == "F5" || key == "F9" || key == "RCtrl+F9" || key == "RAlt+F9", "unsupported probe key");
			if (SimRateKey(key)) {
				if (step.contains("sim_at_least")) {
					if (probe.simTick < step["sim_at_least"].get<uint64_t>()) return false;
					g_UInputMan.SetProbeKeySim(SDLK_P, op == "key_down");
					return true;
				}
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
			// The chat key is whatever the settings name resolves to, so the script never hardcodes it.
			const SDL_Scancode chatScancode = static_cast<SDL_Scancode>(NetModerationGUI::ChatKeyScancode());
			const bool f9 = key == "F9" || key == "RCtrl+F9" || key == "RAlt+F9";
			if (movement) {
				event.key.scancode = key == "A" ? SDL_SCANCODE_A : key == "D" ? SDL_SCANCODE_D : key == "W" ? SDL_SCANCODE_W : SDL_SCANCODE_S;
				event.key.key = SDL_GetKeyFromScancode(event.key.scancode, SDL_KMOD_NONE, false);
			} else {
				event.key.scancode = key == "F6" ? SDL_SCANCODE_F6 : key == "P" ? SDL_SCANCODE_P : key == "CHAT" ? chatScancode : key == "F5" ? SDL_SCANCODE_F5 : f9 ? SDL_SCANCODE_F9 : SDL_SCANCODE_ESCAPE;
				event.key.key = key == "F6" ? SDLK_F6 : key == "P" ? SDLK_P : key == "CHAT" ? SDL_GetKeyFromScancode(chatScancode, SDL_KMOD_NONE, false) : key == "F5" ? SDLK_F5 : f9 ? SDLK_F9 : SDLK_ESCAPE;
			}
			event.key.down = op == "key_down";
			// The engine's hotkeys read modifiers from SDL's state, so a combo holds its modifier from its down step to its up step.
			const SDL_Keymod modifier = static_cast<SDL_Keymod>(key == "RCtrl+F9" ? SDL_KMOD_RCTRL : key == "RAlt+F9" ? SDL_KMOD_RALT : SDL_KMOD_NONE);
			if (modifier != SDL_KMOD_NONE) {
				SDL_SetModState(static_cast<SDL_Keymod>(event.key.down ? SDL_GetModState() | modifier : SDL_GetModState() & ~modifier));
			}
			Push(event);
		} else if (op == "pad_down" || op == "pad_up") {
			const std::string name = step.at("button");
			if (!probe.holdsPad) {
				Require(GUIInputWrapper::AcquireScriptedPad(), "probe could not acquire the shared scripted pad");
				probe.holdsPad = true;
			}
			Require(GUIInputWrapper::QueueScriptedPad(name, op == "pad_down"), "probe pad " + name);
			observed["pad"] = GUIInputWrapper::ScriptedPadId();
			int pads = 0;
			bool wrapper = false, leftoverProbe = false;
			ScriptedPadCensus(pads, wrapper, leftoverProbe);
			Require(pads == 1, "scripted pads attached: " + std::to_string(pads));
			if (wrapper && leftoverProbe) Require(false, "wrapper and leftover probe pads both attached");
		} else if (op == "input_scope") {
			GUIInputWrapper::SetAutomationDriving(step.at("enabled").get<bool>());
		} else if (op == "menu") {
			std::istringstream args(step.at("command").get<std::string>());
			std::string command, name, detail;
			args >> command;
			bool accepted = false;
			if (probe.handIndex == probe.index) {
				// The step's gesture runs a phase a frame; the step ends with its verdict.
				if (!MenuAutomation::HandFinished(accepted, detail)) return false;
				probe.handIndex = SIZE_MAX;
			} else if (MenuAutomation::HandBusy()) {
				return false;
			} else if (command == "activate" || command == "post_command") {
				args >> name;
				auto* main = g_MenuMan.IsMainMenuInteractive() ? g_MenuMan.GetMainMenu() : nullptr;
				accepted = MenuControls() && MenuAutomation::HandClick(MenuControls(), name, !g_MenuMan.GetActivePauseMenu() && main ? main->AutomationModalDialog() : nullptr, detail);
			} else if (command == "assert_enabled") {
				int expected = -1; args >> name >> expected;
				accepted = MenuControls() && (expected == 0 || expected == 1) && MenuControls()->GetControl(name) && MenuAutomation::Enabled(MenuControls()->GetControl(name)) == (expected == 1);
			} else {
				Require(MenuAutomation::Handles(command), "unknown menu operation: " + command);
				accepted = MenuAutomation::Execute(MenuControls(), MenuScreen(), command, args, detail);
			}
			if (accepted && MenuAutomation::HandBusy() && probe.handIndex != probe.index) {
				probe.handIndex = probe.index;
				return false;
			}
			observed["accepted"] = accepted;
			observed["menu_observation"] = detail;
			Require(accepted == step.value("accepted", true), "menu operation refused: " + step.at("command").get<std::string>() + " " + detail);
		} else if (op == "show_row") {
			// A player's row on the open host panel, reached as a hand reaches it: More players, its press and its release on
			// separate frames, until the row shows or every page has been seen.
			auto* menu = g_MenuMan.GetNetworkPanel();
			Require(menu != nullptr && g_MenuMan.IsNetworkPanelOpen(), "the players panel is not open");
			const std::string name = step.at("name").get<std::string>();
			if (GUIControl* row = menu->GetControl("NetworkSeatName@" + name); row && row->GetVisible() && !probe.pageDown) {
				probe.pageTurns = 0;
				observed["control"] = ReadControl(row);
				observed["page_turns"] = probe.pageTurns;
				return true;
			}
			if (probe.renders < probe.pageRender) return false;
			GUIControl* more = menu->GetControl("NetworkSeatsMore");
			Require(more != nullptr && (probe.pageDown || more->GetVisible()), "no row for " + name + " and no more players to turn to");
			Require(probe.pageTurns < 4, "every page turned and no row for " + name);
			int x, y, w, h;
			more->GetControlRect(&x, &y, &w, &h);
			const float mouseX = static_cast<float>((x + w / 2) * g_WindowMan.GetResMultiplier());
			const float mouseY = static_cast<float>((y + h / 2) * g_WindowMan.GetResMultiplier());
			SDL_Event motion{};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			motion.motion.x = mouseX;
			motion.motion.y = mouseY;
			Push(motion);
			SDL_Event event{};
			event.type = probe.pageDown ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.windowID = motion.motion.windowID;
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = !probe.pageDown;
			event.button.x = mouseX;
			event.button.y = mouseY;
			Push(event);
			if (probe.pageDown) ++probe.pageTurns;
			probe.pageDown = !probe.pageDown;
			probe.pageRender = probe.renders + (probe.pageDown ? 3 : 4);
			return false;
		} else if (op == "mouse_down" || op == "mouse_up" || op == "mouse_move") {
			auto* control = Control(step);
			Require((step.value("scope", "") == "menu" ? MenuAutomation::Visible(control) : g_MenuMan.IsNetworkPanelOpen() && control->GetVisible()), "mouse target is not visible");
			int x, y, w, h;
			control->GetControlRect(&x, &y, &w, &h);
			int pointY = y + h / 2;
			if (step.contains("item")) {
				// A list row is pressed on the row itself: the list's own top, the rows above it and half its height.
				auto* list = dynamic_cast<GUIListBox*>(control);
				Require(list != nullptr, "an item press needs a list");
				auto* item = list->GetItem(step["item"].get<int>());
				Require(item != nullptr, "the list has no such row");
				pointY = y + 1 + list->GetStackHeight(item) + list->GetItemHeight(item) / 2 - list->GetScrollVerticalValue();
			}
			const float mouseX = static_cast<float>((x + w / 2) * g_WindowMan.GetResMultiplier());
			const float mouseY = static_cast<float>(pointY * g_WindowMan.GetResMultiplier());
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
			if (step.contains("name")) {
				const std::string name = step.at("name").get<std::string>();
				if (name == "pad_held") {
					const int seat = observed.value("bound_seat", 0);
					int start = 0, moved = 0;
					for (const auto& row: observed["controllers"]) {
						if (seat && row.at("seat") == seat) {
							start = row.at("start").get<bool>() ? 1 : 0;
							moved = row.at("moved").get<int>();
						}
					}
					std::cout << "[pad] seat=" << seat << " moved=" << moved << " start=" << start
					          << " sim=" << observed.at("sim_frame").get<long long>() << std::endl;
				}
			}
		} else if (op == "assert_control") {
			observed["control"] = ReadControl(Control(step));
			const auto& value = observed["control"];
			if (step.contains("equals")) {
				for (auto it = step.at("equals").begin(); it != step["equals"].end(); ++it) {
					Require(value.at(it.key()) == it.value(), "control assertion differs: " + it.key());
				}
			}
			if (step.contains("text_contains")) {
				const std::string text = value.at("text").get<std::string>();
				const std::string needle = step["text_contains"].get<std::string>();
				Require(text.find(needle) != std::string::npos, "control text '" + text + "' does not contain '" + needle + "'");
			}
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
		} else if (op == "send_chat") {
			const std::string text = step.at("text").get<std::string>();
			const uint8_t scope = step.value("scope", std::string("all")) == "team" ? c_NetChatScopeTeam : c_NetChatScopeAll;
			Require(g_NetMatchService.SendChat(scope, text), "SendChat refused the probe line");
		} else if (op == "screen_message") {
			// A seat's own message band is an occupier the overlay must clear; a running match has none
			// of its own, so the script puts one on the seat it is about to measure against.
			g_FrameMan.SetScreenText(step.at("text").get<std::string>(), step.value("screen", 0), 0, step.value("duration_ms", 10000), true);
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
			for (const std::string& element: {"status", "toasts", "seats_panel", "chat"}) {
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
			if (step.value("chat_layout", false)) {
				Require(observed["net_ui"]["chat"].at("visible") == true, "the match chat band is not on screen");
				if (step.contains("entry_open")) {
					Require(observed["net_ui"].at("chat_entry_open") == step["entry_open"],
					    std::string("the chat entry is ") + (observed["net_ui"].at("chat_entry_open").get<bool>() ? "open" : "closed") + " and the step wanted the other");
					// The entry is what consumes the seat's own input, and only for as long as it is open.
					Require(observed["net_ui"].at("seat_input_typed_into") == step["entry_open"],
					    std::string("the seats' input is ") + (observed["net_ui"].at("seat_input_typed_into").get<bool>() ? "consumed" : "free") +
					        " while the entry is " + (observed["net_ui"].at("chat_entry_open").get<bool>() ? "open" : "closed"));
					if (step["entry_open"].get<bool>()) {
						// The entry shrinks before the history yields, so one row rides above it at every supported
						// size - 640x360 included, where the two pixels it gives up are the whole margin.
						Require(observed["net_ui"].at("chat_history_visible") == true,
						    "the chat history is switched off, so the band's rows prove nothing about the entry's size");
						const int bandHeight = observed["net_ui"]["chat"].at("h").get<int>();
						const int rowHeight = observed["net_ui"].at("chat_row_height").get<int>();
						const int entryHeight = observed["net_ui"].at("chat_entry_height").get<int>();
						const int bandRows = observed["net_ui"].at("chat_rows").get<int>();
						Require(bandRows >= 1 && bandHeight >= rowHeight + entryHeight,
						    "the chat band drew " + std::to_string(bandRows) + " history rows above the entry: band h=" +
						        std::to_string(bandHeight) + ", row h=" + std::to_string(rowHeight) + ", entry h=" +
						        std::to_string(entryHeight) + " at " + std::to_string(backbuffer->w) + "x" + std::to_string(backbuffer->h) +
						        (observed["net_ui"].at("chat_text_size_reduced").get<bool>() ? " with the text size already dropped" : ""));
					}
				}
				// A band measured against nothing proves nothing: the step names the occupiers that had to
				// be on screen for this reading to count.
				for (const std::string& element: step.value("occupiers", std::vector<std::string>{})) {
					if (element == "picker" || element == "text_band") {
						const auto seat = std::find_if(observed["editor_seats"].begin(), observed["editor_seats"].end(),
						    [&](const Json& row) { return row.at("player") == step.value("player", 0); });
						Require(seat != observed["editor_seats"].end() && seat->at(element).value("visible", false),
						    "the seat's " + element + " is not on screen, so the chat layout reading proves nothing about it");
						continue;
					}
					Require(observed["net_ui"].at(element).value("visible", false),
					    "the network " + element + " is not on screen, so the chat layout reading proves nothing about it");
				}
				const std::string occupiers[] = {"status", "toasts", "seats_panel", "chat"};
				for (size_t i = 0; i < 4; ++i) {
					for (size_t j = i + 1; j < 4; ++j) {
						Require(!Overlaps(observed["net_ui"].at(occupiers[i]), observed["net_ui"].at(occupiers[j])),
						    "the network " + occupiers[i] + " overlaps " + occupiers[j]);
					}
				}
				for (const auto& other: observed["editor_seats"]) {
					for (const std::string& area: {"picker", "screen_text_rect", "text_band"}) {
						if (!other.contains(area)) continue;
						Require(!Overlaps(observed["net_ui"].at("chat"), other.at(area)),
						    "the match chat band overlaps the editor's " + area);
					}
				}
				return true;
			}
			if (match) {
				Require(seatsPanel.at("visible") == true, "the seats panel is not open");
				Require(observed["net_ui"]["toasts"].at("visible") == true, "no toast is live for the reserved row");
				for (const std::string& element: {"status", "toasts", "chat"}) {
					Require(!Overlaps(observed["net_ui"].at(element), seatsPanel),
					    "the network " + element + " overlaps the seats panel");
				}
				for (const auto& other: observed["editor_seats"]) {
					if (!other.contains("text_band") || !other.at("text_band").value("visible", false)) continue;
					const Json& band = other.at("text_band");
					Require(!Overlaps(seatsPanel, band), "the seats panel overlaps a seat message band");
					for (const std::string& element: {"status", "toasts", "chat"}) {
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
			for (const std::string& element: {"status", "toasts", "seats_panel", "chat"}) {
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
			WriteSignal(step.at("name").get<std::string>(), observed);
		} else if (op == "finish_on_round_end") {
			// From here a round that ends before the script does completes it; "armed": false hands the end back to the script.
			probe.roundEndArmed = step.value("armed", true);
			probe.roundEndSignals.clear();
			for (const auto& name: step.value("signals", Json::array())) {
				(void)Leaf(name.get<std::string>());
				probe.roundEndSignals.push_back(name.get<std::string>());
			}
		} else if (op == "watch_route") {
			WatchRoute(step);
			// It samples every frame until the round's end completes the script.
			return false;
		} else if (op == "wait_file") {
			const std::string path = step.at("path").get<std::string>();
			if (!std::filesystem::is_regular_file(path)) return false;
			NoteRendezvous(std::filesystem::path(path).stem().generic_string());
		} else if (op == "finish") {
			GUIInputWrapper::SetAutomationDriving(false);
			ReleaseProbePad();
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
			if (!probe.enabled) return;
			if (phase == Phase::Draw && !probe.done) LabelDump();
			const uint64_t round = ScenarioRunner::GetLockstepRoundId();
			if (probe.script.value("repeat_rounds", false) && round > 0 && round != probe.round) {
				probe.round = round; probe.index = 0; probe.done = false; probe.phaseArmed = false;
				probe.result["steps"] = Json::array(); probe.result["complete"] = false; probe.result["pass"] = false;
				probe.result["round"] = round; probe.started = Clock::now();
				probe.stepMs = probe.resultWrittenMs = 0; probe.gestureIndex = SIZE_MAX; probe.handIndex = SIZE_MAX;
				probe.roundEndArmed = false; probe.roundEndSignals.clear();
			}
			if (probe.done) return;
			if (!probe.phaseArmed) {
				const bool tickPending = probe.script.contains("activate_at_tick") && g_TimerMan.GetSimUpdateCount() < probe.script["activate_at_tick"].get<uint64_t>();
				const bool phasePending = !tickPending && probe.script.contains("activate_phase") && g_NetMatchService.GetLobbySnapshot().serviceState != probe.script["activate_phase"].get<std::string>();
				if (ActivationPending(probe.script, tickPending || phasePending, NowMs())) return;
				probe.phaseArmed = true; probe.started = Clock::now();
			}
			if (phase != Phase::Sim && probe.roundEndArmed && RoundEnded()) {
				FinishOnRoundEnd(Observe());
				return;
			}
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
			// The result grows by a full observation per step; rewriting all of it every frame made a long script its own
			// peer's slowest work, so it is written at most once a second and always at the end.
			if (probe.done || probe.stepMs >= probe.resultWrittenMs + 1000) {
				WriteResult();
				probe.resultWrittenMs = probe.stepMs;
			}
			if (probe.done) System::PrintDiagnosticLine("[net-ui-probe] PASS: completed " + std::to_string(probe.index) + " steps");
		} catch (const std::exception& error) {
			GUIInputWrapper::SetAutomationDriving(false);
			ReleaseProbePad();
			probe.done = true;
			probe.result["pass"] = false;
			probe.result["error"] = error.what();
			probe.result["failed_step"] = probe.index;
			try { if (probe.resultStarted) WriteResult(); } catch (...) {}
			System::PrintDiagnosticErrorLine(std::string("[net-ui-probe] FAIL: ") + error.what());
			System::SetQuit(true);
		}
	}
}

uint64_t RendezvousCount() { return rendezvousCount.load(); }
bool Running() { return probe.loaded && probe.enabled && !probe.done; }
void WriteUnfinished() {
	// The steps since the last once-a-second write are otherwise lost with the process.
	if (Running() && probe.resultStarted) WriteResult();
}

bool RunCrossScopeSelfTest(std::string* error) {
	bool passed = true;
	for (const char* name: {"assert_buy", "assert_pie", "assert_window"}) {
		const bool good = StepPhase({{"op", name}}) == Phase::Draw;
		passed &= good;
		System::PrintDiagnosticLine("[net-match-selftest] " + std::string(good ? "PASS " : "FAIL ") + name + "_is_scheduled_after_draw");
	}
	const auto expired = [](const Json& script, uint64_t waitedMs) {
		try { ActivationPending(script, true, waitedMs); } catch (const std::exception&) { return true; }
		return false;
	};
	const bool bounded = !expired({{"activation_timeout_ms", 100}}, 100) && expired({{"activation_timeout_ms", 100}}, 101) &&
	    !expired(Json::object(), c_DefaultActivationTimeoutMs) && expired(Json::object(), c_DefaultActivationTimeoutMs + 1) &&
	    !ActivationPending({{"activation_timeout_ms", 1}}, false, UINT64_MAX);
	passed &= bounded;
	System::PrintDiagnosticLine("[net-match-selftest] " + std::string(bounded ? "PASS" : "FAIL") + " probe_activation_wait_has_a_deadline");

	// Lobby ticks cannot satisfy an opt-in round wait. The legacy clock remains the default.
	{
		Probe saved = std::move(probe);
		probe = Probe{};
		probe.script = {{"sim_clock", "lockstep"}};
		const Json wait = {{"op", "wait"}, {"service", "Running"}, {"sim_at_least", 450}};
		Json observed = {{"service", "Running"}, {"sim_frame", 5000}, {"lockstep_frame", 100}};
		const bool lobbyDidNotSettleRound = !Step(wait, observed);
		observed["lockstep_frame"] = 450;
		const bool settledRound = Step(wait, observed);
		observed["lockstep_frame"] = 100;
		probe.script = Json::object();
		const bool legacyClock = Step(wait, observed);
		const bool roundClock = lobbyDidNotSettleRound && settledRound && legacyClock;
		probe = std::move(saved);
		passed &= roundClock;
		if (!roundClock) *error = "lobby simulation ticks satisfied a lockstep round wait";
		System::PrintDiagnosticLine("[net-match-selftest] " + std::string(roundClock ? "PASS" : "FAIL") +
		                            " probe_round_clock_ignores_lobby_ticks_and_preserves_the_default");
	}

	// finish_on_round_end: armed, a round end completes the script with its signals; disarmed or never armed, it is the script's.
	{
		Probe saved = std::move(probe);
		const auto root = std::filesystem::temp_directory_path() / ("net-ui-probe-round-end-" + std::to_string(System::GetProcessID()));
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
		const Json arm = {{"op", "finish_on_round_end"}, {"signals", {"done"}}};
		const Json wait = {{"op", "wait"}, {"elapsed_ms", 100000}};
		const Json finish = {{"op", "finish"}};
		const auto script = [&](const std::string& name, const Json& steps, size_t ran) {
			probe = Probe{};
			probe.loaded = probe.enabled = true;
			probe.directory = root / name;
			std::filesystem::create_directories(probe.directory);
			probe.script = {{"schema", 1}, {"timeout_ms", 180000}, {"steps", steps}};
			probe.result = {{"schema", 1}, {"pass", false}, {"complete", false}, {"steps", Json::array()}};
			Json stepObserved;
			for (; probe.index < ran; ++probe.index) Step(steps[probe.index], stepObserved);
		};
		bool roundEnd = false;
		std::string detail;
		try {
			script("armed", {arm, wait, wait, finish}, 1);
			const bool armed = probe.roundEndArmed;
			FinishOnRoundEnd({{"service", "Completed"}});
			const Json& ended = probe.result["finished_on_round_end"];
			roundEnd = armed && probe.done && probe.result["complete"] == true && probe.result["pass"] == true && !Running() &&
			    ended.value("at_step", 0) == 1 && ended.value("skipped", 0) == 3 && std::filesystem::is_regular_file(root / "armed" / "done.json") &&
			    std::filesystem::is_regular_file(root / "armed" / "net-ui-result.json");
			if (!roundEnd) detail = "an armed script was not completed with its signal: " + probe.result.dump();
			script("disarmed", {arm, Json{{"op", "finish_on_round_end"}, {"armed", false}}, wait, finish}, 2);
			if (roundEnd && (probe.roundEndArmed || !probe.roundEndSignals.empty())) { roundEnd = false; detail = "armed: false left the watch armed"; }
			script("unarmed", {wait, finish}, 0);
			if (roundEnd && probe.roundEndArmed) { roundEnd = false; detail = "a script without the op was armed"; }
			script("bad-name", {Json{{"op", "finish_on_round_end"}, {"signals", {"../done"}}}, finish}, 0);
			bool refused = false;
			try { Json unused; Step(probe.script["steps"][0], unused); } catch (const std::exception&) { refused = true; }
			if (roundEnd && !refused) { roundEnd = false; detail = "a signal name outside the probe directory was accepted"; }
		} catch (const std::exception& failure) {
			roundEnd = false;
			detail = failure.what();
		}
		probe = std::move(saved);
		std::filesystem::remove_all(root, ignored);
		passed &= roundEnd;
		if (!roundEnd) *error = "the probe's round-end finish: " + detail;
		System::PrintDiagnosticLine("[net-match-selftest] " + std::string(roundEnd ? "PASS" : "FAIL") + " probe_finishes_on_round_end_when_armed" +
		                            (roundEnd ? "" : " (" + detail + ")"));
	}
	Json observed;
	const bool alreadyConstructed = NetMatchService::IsConstructed();
	if (!alreadyConstructed) NetMatchService::Construct();
	std::string rejection;
	try { Step({{"op", "remove_participant"}, {"stable_seat", 65535}}, observed); }
	catch (const std::exception& rejected) { rejection = rejected.what(); }
	if (!alreadyConstructed) NetMatchService::Destruct();
	if (rejection == "participant selection is absent") {
		System::PrintDiagnosticLine("[net-match-selftest] PASS participant_probe_refuses_an_absent_selection");
		if (!passed && error->empty()) *error = bounded ? "one or more gameplay scopes were not scheduled after drawing" : "the probe's activation wait has no deadline";
		return passed;
	}
	System::PrintDiagnosticLine("[net-match-selftest] FAIL participant_probe_refuses_an_absent_selection");
	*error = rejection.empty() ? "the participant probe accepted an absent selection" : rejection; return false;
}

void BeforePoll() { Process(Phase::Poll); }
void AfterDraw() {
	MenuAutomation::AfterDrawnFrame();
	MenuAutomation::EvaluateWatches(MenuControls());
	Process(Phase::Draw);
}
void AfterMenuDraw() {
	MenuAutomation::AfterDrawnFrame();
	MenuAutomation::EvaluateWatches(MenuControls());
	Process(Phase::Draw, true);
}

void OnSimTick(uint64_t simUpdateCount) {
	if (!probe.enabled || probe.done) return;
	probe.simTick = simUpdateCount;
	Process(Phase::Sim);
}
}
