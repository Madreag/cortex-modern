#include "NetModerationGUIProbe.h"

#include "ActivityMan.h"
#include "FrameMan.h"
#include "GUI.h"
#include "GUIButton.h"
#include "GUILabel.h"
#include "MenuMan.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "NetModerationGUI.h"
#include "System.h"
#include "TimerMan.h"
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

namespace RTE::NetModerationGUIProbe {
namespace {
	using Json = nlohmann::json;
	using Clock = std::chrono::steady_clock;
	struct Probe {
		bool loaded = false, enabled = false, done = false, resultStarted = false;
		size_t index = 0;
		uint64_t renders = 0, stepRender = 0, stepMs = 0;
		Clock::time_point started;
		std::filesystem::path directory;
		Json script, result;
	};
	Probe probe;

	void Require(bool condition, const std::string& reason) {
		if (!condition) throw std::runtime_error(reason);
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

	Json Observe() {
		const auto snapshot = g_NetMatchService.GetLobbySnapshot();
		Json observed = {{"at_ms", NowMs()}, {"render", probe.renders}, {"sim_frame", g_TimerMan.GetSimUpdateCount()},
		    {"service", snapshot.serviceState}, {"host", snapshot.isHost}, {"panel_open", g_MenuMan.IsNetworkPanelOpen()},
		    {"paused", g_ActivityMan.ActivityPaused()}, {"seats", Json::array()}};
		for (const auto& seat: g_NetMatchService.GetModerationSeats()) {
			observed["seats"].push_back({{"seat", seat.stableSeat}, {"name", seat.displayName}, {"dropped", seat.dropped},
			    {"closed", seat.closed}, {"substituting", seat.substituting}, {"reclaiming", seat.reclaiming},
			    {"applicants", seat.applicants.size()}, {"actions_available", seat.actionsAvailable},
			    {"holder_generation", seat.holderGeneration}, {"seat_generation", seat.seatGeneration}});
		}
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
		probe.result["script"] = probe.script;
		WriteResult();
	}

	GUIControl* Control(const Json& step) {
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

	bool Step(const Json& step, bool drawn, Json& observed) {
		const std::string op = step.at("op");
		const bool drawingStep = op == "assert" || op == "assert_control" || op == "screenshot" || op == "finish";
		if (drawingStep != drawn) return false;
		if (op == "wait") {
			Require(step.contains("service") || step.contains("sim_at_least") || step.contains("renders") ||
			    step.contains("elapsed_ms") || step.contains("panel_open") || step.contains("control"), "wait has no predicate");
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
			Require(key == "F6" || key == "Escape", "unsupported probe key");
			SDL_Event event{};
			event.type = op == "key_down" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
			event.key.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			event.key.scancode = key == "F6" ? SDL_SCANCODE_F6 : SDL_SCANCODE_ESCAPE;
			event.key.key = key == "F6" ? SDLK_F6 : SDLK_ESCAPE;
			event.key.down = op == "key_down";
			Push(event);
		} else if (op == "mouse_down" || op == "mouse_up") {
			auto* control = Control(step);
			Require(g_MenuMan.IsNetworkPanelOpen() && control->GetVisible(), "mouse target is not visible");
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
		} else if (op == "screenshot") {
			auto path = Leaf(step.at("name").get<std::string>());
			path += ".png";
			Require(!std::filesystem::exists(path), "screenshot already exists");
			Require(g_FrameMan.SaveBitmapToPNG(g_FrameMan.GetBackBuffer32(), path.string().c_str()) == 0, "screenshot save failed");
			observed["screenshot"] = path.string();
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
			Require(probe.index + 1 == probe.script["steps"].size(), "finish must be last");
			probe.done = true;
			probe.result["complete"] = true;
			probe.result["pass"] = true;
		} else {
			throw std::runtime_error("unknown probe operation: " + op);
		}
		return true;
	}

	void Process(bool drawn) {
		try {
			if (!probe.loaded) Load();
			if (!probe.enabled || probe.done) return;
			if (drawn) ++probe.renders;
			Require(NowMs() <= probe.script.at("timeout_ms").get<uint64_t>(), "script deadline at step " + std::to_string(probe.index));
			Require(probe.index < probe.script["steps"].size(), "script did not finish explicitly");
			const auto& step = probe.script["steps"][probe.index];
			Json observed = Observe();
			try {
				if (!Step(step, drawn, observed)) return;
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

void BeforePoll() { Process(false); }
void AfterDraw() { Process(true); }
}
