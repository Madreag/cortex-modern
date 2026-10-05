#include "MenuAutomation.h"
#include "MetaMan.h"
#include "MetagameGUI.h"
#include "SettingsGUI.h"
#include "MenuMan.h"
#include "MainMenuGUI.h"
#include "NetModerationGUI.h"
#include "NetHostOptionsText.h"
#include "NetPlayerPresentation.h"
#include "ActivityMan.h"

#include "GUI.h"
#include "GUIDrawRecord.h"
#include "GUIFont.h"
#include "GUIButton.h"
#include "GUICheckbox.h"
#include "GUIComboBox.h"
#include "GUICollectionBox.h"
#include "GUIListBox.h"
#include "GUIInputWrapper.h"
#include "GUILabel.h"
#include "GUIListPanel.h"
#include "GUIRadioButton.h"
#include "GUISlider.h"
#include "GUITab.h"
#include "GUITextBox.h"
#include "FrameMan.h"
#include "FrameRecorder.h"
#include "GAScripted.h"
#include "GameActivity.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "NetWorldJoin.h"
#include "NetIdentity.h"
#include "ScenarioRunner.h"
#include "PresetMan.h"
#include "Scene.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "UInputMan.h"
#include "WindowMan.h"
#include "RTEError.h"
#include "System.h"
#include "HarnessCost.h"
#include "Writer.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RTE::MenuAutomation {
	using Json = nlohmann::json;
	std::string s_ArtifactDirectory; //!< Where a probe's dumps are written, beside the probe.
	using Rect = std::array<int, 4>;
	class ReadbackWriter {
		struct Dump {
			std::filesystem::path path;
			Json controls;
			std::vector<unsigned char> pixels;
			int width = 0, height = 0;
		};
		std::mutex m_Mutex;
		std::condition_variable m_Wake;
		std::deque<Dump> m_Queue;
		bool m_Stopping = false, m_Failed = false;
		std::thread m_Worker;
		void Write() {
			for (;;) {
				Dump dump;
				{
					std::unique_lock lock(m_Mutex);
					m_Wake.wait(lock, [&] { return m_Stopping || !m_Queue.empty(); });
					if (m_Queue.empty()) return;
					dump = std::move(m_Queue.front());
					m_Queue.pop_front();
				}
				bool saved = false;
				try {
					SDL_Surface* image = SDL_CreateSurfaceFrom(dump.width, dump.height, SDL_PIXELFORMAT_RGB24, dump.pixels.data(), dump.width * 3);
					saved = image && IMG_SavePNG(image, (dump.path.string() + ".png").c_str());
					if (image) SDL_DestroySurface(image);
					if (saved) {
						const std::string temporary = dump.path.string() + ".json.tmp";
						std::ofstream output(temporary);
						output << dump.controls.dump(2) << '\n';
						output.close();
						saved = output.good();
						if (saved) std::filesystem::rename(temporary, dump.path.string() + ".json");
					}
				} catch (...) { saved = false; }
				if (!saved) {
					std::lock_guard lock(m_Mutex);
					m_Failed = true;
				}
			}
		}
	public:
		ReadbackWriter() = default;
		~ReadbackWriter() { Finish(); }
		bool Queue(const std::filesystem::path& path, Json controls, BITMAP* bitmap) {
			if (!bitmap || bitmap_color_depth(bitmap) != 32) return false;
			Dump dump{path, std::move(controls), {}, bitmap->w, bitmap->h};
			dump.pixels.resize(static_cast<size_t>(bitmap->w) * bitmap->h * 3);
			for (int y = 0; y < bitmap->h; ++y) {
				const auto* source = reinterpret_cast<const uint32_t*>(bitmap->line[y]);
				auto* target = dump.pixels.data() + static_cast<size_t>(y) * bitmap->w * 3;
				for (int x = 0; x < bitmap->w; ++x) {
					target[3 * x] = getr32(source[x]);
					target[3 * x + 1] = getg32(source[x]);
					target[3 * x + 2] = getb32(source[x]);
				}
			}
			{
				std::lock_guard lock(m_Mutex);
				if (m_Stopping || m_Failed || m_Queue.size() >= 8) return false;
				m_Queue.push_back(std::move(dump));
				if (!m_Worker.joinable()) m_Worker = std::thread([this] { Write(); });
			}
			m_Wake.notify_one();
			return true;
		}
		bool Finish() {
			{
				std::lock_guard lock(m_Mutex);
				m_Stopping = true;
			}
			m_Wake.notify_one();
			if (m_Worker.joinable()) m_Worker.join();
			return !m_Failed;
		}
	};
	ReadbackWriter& Writer() { static ReadbackWriter writer; return writer; }
	bool FinishReadbacks() { return Writer().Finish(); }
	Rect Rectangle(GUIPanel* panel) {
		Rect rect{0, 0, g_WindowMan.GetResX(), g_WindowMan.GetResY()};
		if (panel) panel->GetRect(&rect[0], &rect[1], &rect[2], &rect[3]);
		return rect;
	}
	bool Visible(GUIControl* control) {
		if (!control || !control->GetPanel()) return false;
		for (auto* node = control->GetPanel(); node; node = node->GetParentPanel()) if (!node->_GetVisible()) return false;
		return true;
	}
	// What the renderer drew, read from its own record: a visible flag only says what a panel would
	// draw if its parents did. A manager that has not drawn for a quarter second is off the screen.
	constexpr double c_DrawWindowSeconds = 0.25;
	GUIControl* FirstDrawn(GUIControl* control) {
		if (!control || !control->GetPanel()) return nullptr;
		if (PanelDrawnInLatestPass(control->GetPanel(), c_DrawWindowSeconds)) return control;
		if (std::vector<GUIControl*>* children = control->GetChildren()) {
			for (GUIControl* child: *children) {
				if (GUIControl* drawn = FirstDrawn(child)) return drawn;
			}
		}
		return nullptr;
	}
	bool Enabled(GUIControl* control) {
		if (!Visible(control)) return false;
		for (auto* node = control->GetPanel(); node; node = node->GetParentPanel()) if (!node->_GetEnabled()) return false;
		return true;
	}
	// Both settings skins name a page's tab and box after the page, so scripts address pages by name.
	constexpr std::array<std::string_view, 6> c_SettingsPages{"Video", "Audio", "Input", "Gameplay", "Misc", "Network"};
	// The network page's own selector names its sub-pages the same way; a script addresses
	// one as "Network:<page>" once the network page is up.
	constexpr std::array<std::string_view, 7> c_NetworkPages{"Basics", "Player", "Chat", "Recovery", "Files", "Internet", "Connection"};

	std::string SettingsPage(GUIControlManager* manager) {
		for (const std::string_view page: c_SettingsPages) {
			if (manager && Visible(manager->GetControl("CollectionBox" + std::string(page) + "Settings"))) {
				if (page == "Network") {
					for (const std::string_view sub: c_NetworkPages) {
						if (Visible(manager->GetControl("CollectionBoxNetPage" + std::string(sub)))) return "Network:" + std::string(sub);
					}
				}
				return std::string(page);
			}
		}
		return "";
	}

	GUITab* PageTab(GUIControlManager* manager, const std::string& page) {
		const size_t colon = page.find(':');
		if (colon != std::string::npos) {
			const std::string sub = page.substr(colon + 1);
			const bool known = page.substr(0, colon) == "Network" && std::find(c_NetworkPages.begin(), c_NetworkPages.end(), sub) != c_NetworkPages.end();
			return known && manager ? dynamic_cast<GUITab*>(manager->GetControl("TabNetPage" + sub)) : nullptr;
		}
		const bool known = std::find(c_SettingsPages.begin(), c_SettingsPages.end(), page) != c_SettingsPages.end();
		return known && manager ? dynamic_cast<GUITab*>(manager->GetControl("Tab" + page + "Settings")) : nullptr;
	}

	// A relay login is a credential, though only its password box masks itself on screen: its boxes are named by their role.
	constexpr std::array<std::string_view, 4> c_CredentialBoxes{"TextHostRelayUser", "TextHostRelayPass", "TextNetworkRelayUser", "TextNetworkRelayPass"};
	bool CredentialName(const std::string& name) {
		return std::find(c_CredentialBoxes.begin(), c_CredentialBoxes.end(), name) != c_CredentialBoxes.end();
	}
	bool Credential(GUIControl* control) {
		auto* box = dynamic_cast<GUITextBox*>(control);
		return box && (box->HasPasswordMask() || CredentialName(box->GetName()));
	}
	/// What a capture may write of a control's text: a credential's as its mask, whatever it holds.
	std::string Captured(bool credential, const std::string& text) {
		return credential ? std::string(text.size(), '*') : text;
	}
	/// A set_text step's record: a credential box's value never reaches it, whether or not the box takes it.
	std::string SetTextObservation(GUIControl* control, const std::string& name, const std::string& argument) {
		return Credential(control) || CredentialName(name) ? name + " <masked>" : name + " " + argument;
	}
	// What the control draws: only measuring and comparing read it, every capture reads Text.
	bool DisplayText(GUIControl* control, std::string& text) {
		if (auto* value = dynamic_cast<GUILabel*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIButton*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUITextBox*>(control)) text = value->GetDisplayText();
		else if (auto* value = dynamic_cast<GUICheckbox*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIRadioButton*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUITab*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIComboBox*>(control)) text = value->GetSelectedItem() ? value->GetSelectedItem()->m_Name : value->GetText();
		else return false;
		return true;
	}
	bool Text(GUIControl* control, std::string& text) {
		if (!DisplayText(control, text)) return false;
		text = Captured(Credential(control), text);
		return true;
	}
	// A hand reaches the menus the way a player does: real mouse and keyboard events go into the engine's input queue,
	// one phase per drawn frame, and every press lands where the GUI manager's own hit test says it lands.
	namespace Hand {
		enum class Beat { Next, Again, Done, Fail };
		using Phase = std::function<Beat(std::string& note)>;
		struct Gesture {
			std::string name;
			std::vector<Phase> phases;
			size_t at = 0;
			int repeats = 0; //!< How often the current phase has asked for another frame.
		};
		std::optional<Gesture> s_Gesture;
		std::optional<std::pair<bool, std::string>> s_Result;
		uint64_t s_DrawnFrames = 0, s_AdvancedFrame = 0;
		float s_WindowX = 0, s_WindowY = 0; //!< Where the pointer rests, in window pixels.
		std::deque<std::string> s_TypedText; //!< Text an input event still points at until the queue hands it on.
		constexpr int c_MaxRepeats = 240;

		bool Push(SDL_Event& event) { return SDL_PushEvent(&event); }

		/// Moves the pointer to a GUI pixel, sent in window pixels as the mouse sends it, so the window's scale is read on the way in.
		bool Move(int x, int y) {
			const float scale = g_WindowMan.GetResMultiplier();
			const float windowX = std::floor((static_cast<float>(x) + 0.5F) * scale), windowY = std::floor((static_cast<float>(y) + 0.5F) * scale);
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			event.motion.x = windowX;
			event.motion.y = windowY;
			event.motion.xrel = windowX - s_WindowX;
			event.motion.yrel = windowY - s_WindowY;
			s_WindowX = windowX;
			s_WindowY = windowY;
			return Push(event);
		}

		bool Button(bool down) {
			SDL_Event event{};
			event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = down;
			event.button.clicks = 1;
			event.button.x = s_WindowX;
			event.button.y = s_WindowY;
			return Push(event);
		}

		bool Wheel(int steps) {
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_WHEEL;
			event.wheel.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
			event.wheel.y = static_cast<float>(steps);
			event.wheel.mouse_x = s_WindowX;
			event.wheel.mouse_y = s_WindowY;
			return Push(event);
		}

		bool Type(const std::string& text) {
			// Keys come a few at a time, as a hand's do: the input reads at most 32 characters of one text event.
			constexpr size_t c_Piece = 16;
			for (size_t at = 0; at < text.size(); at += c_Piece) {
				// The queue keeps the event's pointer, not the text: the text lives until a later frame has read it.
				s_TypedText.push_back(text.substr(at, c_Piece));
				while (s_TypedText.size() > 256) s_TypedText.pop_front();
				SDL_Event event{};
				event.type = SDL_EVENT_TEXT_INPUT;
				event.text.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
				event.text.text = s_TypedText.back().c_str();
				if (!Push(event)) return false;
			}
			return true;
		}

		bool Key(const std::string& name, bool down) { return GUIInputWrapper::QueueScriptedKey(name, down); }

		bool Within(GUIPanel* panel, GUIPanel* ancestor) {
			for (; panel; panel = panel->GetParentPanel()) if (panel == ancestor) return true;
			return false;
		}

		/// What a press at this point reaches, named for the record.
		std::string Reached(GUIPanel* panel) {
			GUIControl* control = nullptr;
			for (GUIPanel* node = panel; node && !control; node = node->GetParentPanel()) control = dynamic_cast<GUIControl*>(node);
			return control ? control->GetName() : panel ? "an unnamed panel" : "nothing";
		}

		/// A point on the control that a press reaches: its centre first, then a grid over the rest of it. Empty, with the reason, when no point does.
		std::optional<std::pair<int, int>> Target(GUIControlManager* manager, GUIControl* control, GUIControl* modal, std::string& why) {
			if (!control || !control->GetPanel()) { why = "missing"; return std::nullopt; }
			GUIPanel* panel = control->GetPanel();
			if (!Enabled(control)) { why = control->GetName() + " is hidden or disabled"; return std::nullopt; }
			if (!PanelDrawnInLatestPass(panel, c_DrawWindowSeconds)) { why = control->GetName() + " is not on the screen"; return std::nullopt; }
			if (modal && !Within(panel, modal->GetPanel())) { why = control->GetName() + " is behind the open dialog " + modal->GetName(); return std::nullopt; }
			const Rect r = Rectangle(panel);
			const int width = g_WindowMan.GetResX(), height = g_WindowMan.GetResY();
			std::string covered;
			for (int row = 0; row < 5; ++row) {
				for (int column = 0; column < 5; ++column) {
					// The centre first, then the rest of a 5 x 5 grid inset from the edges.
					const bool centre = row == 0 && column == 0;
					if (!centre && row == 2 && column == 2) continue;
					const int gx = centre ? 2 : column, gy = centre ? 2 : row;
					const int x = r[0] + std::max(1, r[2] * (1 + 2 * gx) / 10), y = r[1] + std::max(1, r[3] * (1 + 2 * gy) / 10);
					if (x < 0 || y < 0 || x >= width || y >= height || x >= r[0] + r[2] || y >= r[1] + r[3]) continue;
					GUIPanel* hit = manager->GetManager()->PanelUnderMouse(x, y);
					if (Within(hit, panel)) return std::make_pair(x, y);
					if (covered.empty()) covered = Reached(hit);
				}
			}
			why = control->GetName() + (covered.empty() ? " lies off the screen" : " is covered: a press there reaches " + covered);
			if (GUIPanel* held = manager->GetManager()->GetCapturedPanel()) why += " (the mouse is held by " + Reached(held) + ")";
			// The panels a press at the centre passes through on its way down, so the one that does not hold the point shows.
			if (!covered.empty()) {
				why += "; centre " + std::to_string(r[0] + r[2] / 2) + "," + std::to_string(r[1] + r[3] / 2) + " in";
				for (GUIPanel* node = panel; node; node = node->GetParentPanel()) {
					auto* named = dynamic_cast<GUIControl*>(node);
					why += " " + (named ? named->GetName() : std::string("panel")) + Json(Rectangle(node)).dump() + (node->_GetVisible() ? "" : "(hidden)");
				}
			}
			return std::nullopt;
		}

		void Start(std::string name, std::vector<Phase> phases) {
			s_Result.reset();
			s_Gesture = Gesture{std::move(name), std::move(phases)};
		}

		/// The phases that point at a control, press it and let it go, one frame apart.
		void AppendClick(std::vector<Phase>& phases, GUIControlManager* manager, std::string name, GUIControl* modal, int presses = 1) {
			auto point = std::make_shared<std::pair<int, int>>(-1, -1);
			phases.push_back([manager, name, modal, point](std::string& note) {
				std::string why;
				const auto target = Target(manager, manager->GetControl(name), modal, why);
				if (!target) { note = why; return Beat::Fail; }
				*point = *target;
				if (Move(point->first, point->second)) return Beat::Next;
				note = "the pointer did not move";
				return Beat::Fail;
			});
			for (int press = 0; press < presses; ++press) {
				phases.push_back([manager, name, modal, point](std::string& note) {
					// The press goes where the pointer is; a control that moved under it is followed.
					GUIControl* control = manager->GetControl(name);
					if (!control || !Within(manager->GetManager()->PanelUnderMouse(point->first, point->second), control->GetPanel())) {
						std::string why;
						const auto target = Target(manager, control, modal, why);
						if (!target) { note = why; return Beat::Fail; }
						*point = *target;
						Move(point->first, point->second);
						return Beat::Again;
					}
					if (Button(true)) return Beat::Next;
					note = "the press was not sent";
					return Beat::Fail;
				});
				phases.push_back([](std::string& note) {
					if (Button(false)) return Beat::Next;
					note = "the release was not sent";
					return Beat::Fail;
				});
			}
			// The frame that delivers the release runs the control's own handlers before the next step.
			phases.push_back([](std::string&) { return Beat::Next; });
		}

		bool Busy() { return s_Gesture.has_value(); }

		void Advance() {
			if (!s_Gesture || s_AdvancedFrame == s_DrawnFrames) return;
			s_AdvancedFrame = s_DrawnFrames;
			Gesture& gesture = *s_Gesture;
			std::string note;
			Beat beat = gesture.at < gesture.phases.size() ? gesture.phases[gesture.at](note) : Beat::Done;
			if (beat == Beat::Again && ++gesture.repeats > c_MaxRepeats) {
				beat = Beat::Fail;
				note = "a phase waited " + std::to_string(c_MaxRepeats) + " frames" + (note.empty() ? "" : ": " + note);
			}
			if (beat == Beat::Next) {
				gesture.at++;
				gesture.repeats = 0;
			}
			if (beat == Beat::Fail) {
				s_Result = std::make_pair(false, gesture.name + ": " + note);
				s_Gesture.reset();
			} else if (beat == Beat::Done || gesture.at >= gesture.phases.size()) {
				s_Result = std::make_pair(true, gesture.name + (note.empty() ? "" : " - " + note));
				s_Gesture.reset();
			}
		}

		bool Finished(bool& passed, std::string& observation) {
			if (s_Gesture || !s_Result) return false;
			passed = s_Result->first;
			observation = s_Result->second;
			s_Result.reset();
			return true;
		}

		/// A list row the pointer can press: the wheel brings an unseen row into the list first.
		Beat ReachRow(GUIControlManager* manager, GUIListPanel* list, int index, std::pair<int, int>& point, std::string& note) {
			const GUIListPanel::Item* wanted = list->GetItem(index);
			if (!wanted) {
				note = "row " + std::to_string(index) + " is not in the list";
				return Beat::Fail;
			}
			const Rect r = Rectangle(list);
			int first = -1;
			for (int y = r[1] + 1; y < r[1] + r[3] - 1; ++y) {
				const GUIListPanel::Item* item = list->GetItem(r[0] + 6, y);
				if (!item) continue;
				if (item == wanted && Within(manager->GetManager()->PanelUnderMouse(r[0] + 6, y), list)) {
					int bottom = y;
					while (bottom + 1 < r[1] + r[3] - 1 && list->GetItem(r[0] + 6, bottom + 1) == wanted) ++bottom;
					point = {r[0] + 6, (y + bottom) / 2};
					return Beat::Next;
				}
				if (first < 0 || item->m_ID < first) first = item->m_ID;
			}
			if (first < 0) {
				note = "the list shows no rows";
				return Beat::Fail;
			}
			Move(r[0] + r[2] / 2, r[1] + r[3] / 2);
			Wheel(index < first ? 1 : -1);
			note = "row " + std::to_string(index) + " never came into view";
			return Beat::Again;
		}
	}

	// ---- The sweep: every enabled control a screen lists, changed by hand, read back frames later, put back ----
	std::set<std::string> s_HandReached; //!< Every control a hand's gesture reached, for the controls a case owes a press.
	std::set<std::string> s_Owed;        //!< Buttons a sweep left to the case's own later steps.
	std::string s_ModelMark, s_ScreenMark;

	void NoteReached(const std::string& name) { s_HandReached.insert(name); }

	/// What a change on a menu drives: the host's choices and draft as the menu holds them, and every setting.
	std::string ModelText() {
		std::string text;
		if (MainMenuGUI* main = g_MenuMan.GetMainMenu()) text = main->AutomationModelText();
		auto stream = std::make_unique<std::ostringstream>();
		std::ostringstream* raw = stream.get();
		{
			RTE::Writer writer(std::move(stream));
			g_SettingsMan.Save(writer);
			text += "\n" + raw->str();
		}
		return text;
	}

	/// What the screen shows: its name and every drawn control's text and state.
	std::string ScreenText(GUIControlManager* manager) {
		std::string text;
		if (MainMenuGUI* main = g_MenuMan.GetMainMenu()) text = main->AutomationActiveScreenName() + "/" + main->AutomationMultiplayerSubScreen();
		if (!manager) return text;
		for (GUIControl* control: *manager->GetControlList()) {
			if (!Visible(control) || !PanelDrawnInLatestPass(control->GetPanel(), c_DrawWindowSeconds)) continue;
			std::string shown;
			Text(control, shown);
			text += "\n" + control->GetName() + "=" + shown;
			if (auto* box = dynamic_cast<GUICheckbox*>(control)) text += box->GetCheck() == GUICheckbox::Checked ? " [x]" : " [ ]";
			if (auto* radio = dynamic_cast<GUIRadioButton*>(control)) text += radio->GetCheck() ? " (o)" : " ( )";
			if (auto* tab = dynamic_cast<GUITab*>(control)) text += tab->GetCheck() ? " <on>" : "";
			if (auto* slider = dynamic_cast<GUISlider*>(control)) text += " " + std::to_string(slider->GetValue());
			if (auto* list = dynamic_cast<GUIListBox*>(control)) text += " #" + std::to_string(list->GetSelectedIndex());
			if (!Enabled(control)) text += " (disabled)";
		}
		return text;
	}

	bool Interactive(GUIControl* control) {
		return dynamic_cast<GUIButton*>(control) || dynamic_cast<GUICheckbox*>(control) || dynamic_cast<GUIRadioButton*>(control) ||
		       dynamic_cast<GUIComboBox*>(control) || dynamic_cast<GUISlider*>(control) || dynamic_cast<GUITextBox*>(control) ||
		       dynamic_cast<GUIListBox*>(control) || dynamic_cast<GUITab*>(control);
	}

	bool Under(GUIControl* control, GUIControl* root, int depth) {
		int level = 0;
		for (GUIControl* node = control->GetParent(); node; node = node->GetParent()) {
			++level;
			if (node == root) return depth <= 0 || level <= depth;
		}
		return false;
	}

	std::set<std::string> NameList(const std::string& text) {
		std::set<std::string> names;
		std::istringstream items(text);
		for (std::string item; std::getline(items, item, ',');) if (!item.empty()) names.insert(item);
		return names;
	}

	/// A different value a text box takes from a hand: the next number in its bounds, or the text with one letter more or less.
	std::string OtherText(GUITextBox* box) {
		const std::string text = box->GetText();
		if (box->GetNumericOnly()) {
			const bool number = !text.empty() && text.size() < 10 && text.find_first_not_of("0123456789") == std::string::npos;
			const int value = number ? std::stoi(text) : 0;
			const int top = box->GetMaxNumericValue(), bottom = std::max(0, box->GetMinNumericValue());
			int other = value + 1;
			if (top > 0 && other > top) other = value - 1;
			if (other < bottom) other = bottom == value ? value + 1 : bottom;
			return std::to_string(other);
		}
		const int longest = box->GetMaxTextLength();
		if (longest > 0 && static_cast<int>(text.size()) >= longest) return text.substr(0, text.size() - 1);
		return text + "x";
	}

	bool SweepSteps(GUIControlManager* manager, std::istream& args, std::vector<std::string>& steps, std::string& observation) {
		std::string rootName;
		args >> rootName;
		int depth = 0;
		std::string restore, label = rootName;
		std::set<std::string> quiet, own;
		// A box that takes only some text (a key name, a digest) is given one it takes: with=Box:text.
		std::map<std::string, std::string> with;
		bool readOnly = false;
		for (std::string option; args >> option;) {
			const size_t equals = option.find('=');
			const std::string key = option.substr(0, equals), value = equals == std::string::npos ? std::string() : option.substr(equals + 1);
			if (key == "depth") depth = std::atoi(value.c_str());
			else if (key == "restore") restore = value;
			else if (key == "quiet") quiet = NameList(value);
			else if (key == "own") own = NameList(value);
			else if (key == "label") label = value;
			else if (key == "readonly") readOnly = true;
			else if (key == "with" && value.find(':') != std::string::npos) with[value.substr(0, value.find(':'))] = value.substr(value.find(':') + 1);
			else { observation = "unknown sweep option " + option; return false; }
		}
		GUIControl* root = manager ? manager->GetControl(rootName) : nullptr;
		if (!root || !Visible(root)) {
			observation = rootName + " is not on the screen";
			return false;
		}
		std::vector<GUIControl*> values, tabs, buttons, disabled;
		for (GUIControl* control: *manager->GetControlList()) {
			if (!Interactive(control) || !Under(control, root, depth) || !Visible(control) || !PanelDrawnInLatestPass(control->GetPanel(), c_DrawWindowSeconds)) continue;
			if (!Enabled(control)) disabled.push_back(control);
			else if (dynamic_cast<GUITab*>(control)) tabs.push_back(control);
			else if (dynamic_cast<GUIButton*>(control)) buttons.push_back(control);
			else values.push_back(control);
		}
		Json record = {{"label", label}, {"root", rootName}, {"values", Json::array()}, {"tabs", Json::array()}, {"buttons", Json::array()},
		               {"disabled", Json::array()}, {"owed", Json::array()}, {"quiet", Json::array()}};
		for (GUIControl* control: disabled) {
			record["disabled"].push_back(control->GetName());
			steps.push_back("assert_enabled " + control->GetName() + " 0");
		}
		if (readOnly) {
			const size_t enabled = values.size() + tabs.size() + buttons.size();
			record["enabled"] = enabled;
			observation = record.dump();
			System::PrintDiagnosticLine("[sweep] " + observation);
			// A screen shown to read changes nothing: every value is disabled, and its tabs and buttons only move about it.
			for (GUIControl* control: values) steps.push_back("assert_enabled " + control->GetName() + " 0");
			return true;
		}
		const auto changed = [&](const std::string& name) {
			if (quiet.count(name)) {
				record["quiet"].push_back(name);
				return;
			}
			steps.push_back("assert_model_changed " + name);
		};
		for (GUIControl* control: values) {
			const std::string name = control->GetName();
			record["values"].push_back(name);
			steps.push_back("model_mark");
			if (auto* box = dynamic_cast<GUICheckbox*>(control)) {
				const bool checked = box->GetCheck() == GUICheckbox::Checked;
				steps.push_back("setcheck " + name + (checked ? " 0" : " 1"));
				steps.push_back("wait 3");
				steps.push_back("assert_checked " + name + (checked ? " 0" : " 1"));
				changed(name);
				steps.push_back("setcheck " + name + (checked ? " 1" : " 0"));
				steps.push_back("wait 3");
				steps.push_back("assert_checked " + name + (checked ? " 1" : " 0"));
			} else if (auto* radio = dynamic_cast<GUIRadioButton*>(control)) {
				// A radio changes by its group: the checked one gives way to a sibling and comes back.
				GUIRadioButton* checked = nullptr;
				GUIRadioButton* other = nullptr;
				for (GUIControl* sibling: *control->GetParent()->GetChildren()) {
					auto* candidate = dynamic_cast<GUIRadioButton*>(sibling);
					if (!candidate || !Enabled(candidate)) continue;
					if (candidate->GetCheck()) checked = candidate;
					else if (!other || candidate == radio) other = candidate;
				}
				GUIRadioButton* target = radio->GetCheck() ? other : radio;
				if (!target || !checked) {
					observation = name + " has no other choice in its group";
					return false;
				}
				steps.push_back("activate " + target->GetName());
				steps.push_back("wait 3");
				steps.push_back("assert_checked " + target->GetName() + " 1");
				changed(name);
				steps.push_back("activate " + checked->GetName());
				steps.push_back("wait 3");
				steps.push_back("assert_checked " + checked->GetName() + " 1");
			} else if (auto* combo = dynamic_cast<GUIComboBox*>(control)) {
				const int count = combo->GetCount(), selected = combo->GetSelectedIndex();
				if (count < 1) {
					observation = name + " offers no row";
					return false;
				}
				const GUIListPanel::Item* original = combo->GetItem(std::max(0, selected));
				const GUIListPanel::Item* other = combo->GetItem(count > 1 ? (std::max(0, selected) + 1) % count : 0);
				steps.push_back("combo_select " + name + " " + other->m_Name);
				steps.push_back("wait 3");
				steps.push_back("assert_label " + name + " " + other->m_Name);
				if (count > 1) changed(name);
				if (count > 1 && selected >= 0) {
					steps.push_back("combo_select " + name + " " + original->m_Name);
					steps.push_back("wait 3");
					steps.push_back("assert_label " + name + " " + original->m_Name);
				}
			} else if (auto* slider = dynamic_cast<GUISlider*>(control)) {
				const int value = slider->GetValue();
				const int other = value != slider->GetMaximum() ? slider->GetMaximum() : slider->GetMinimum();
				steps.push_back("slider_set " + name + " " + std::to_string(other));
				steps.push_back("wait 3");
				steps.push_back("assert_value " + name + " " + std::to_string(other));
				changed(name);
				steps.push_back("slider_set " + name + " " + std::to_string(value));
				steps.push_back("wait 3");
				steps.push_back("assert_value " + name + " " + std::to_string(value));
			} else if (auto* box = dynamic_cast<GUITextBox*>(control)) {
				const std::string original = box->GetText(), other = with.count(name) ? with[name] : OtherText(box);
				// A quiet box acts on Enter (a chat line is sent and the box empties): it is typed into without Enter and read back.
				const std::string mode = quiet.count(name) ? " plain " : " enter ";
				steps.push_back("type_text " + name + mode + other);
				steps.push_back("wait 3");
				steps.push_back("assert_box_text " + name + " " + other);
				changed(name);
				steps.push_back("type_text " + name + mode + original);
				steps.push_back("wait 3");
				steps.push_back("assert_box_text " + name + " " + original);
			} else if (auto* list = dynamic_cast<GUIListBox*>(control)) {
				const int rows = static_cast<int>(list->GetItemList()->size()), selected = list->GetSelectedIndex();
				if (rows == 0) {
					// An empty list has nothing to press; the case asserts the line that says so.
					steps.push_back("assert_list_rows " + name + " 0");
					continue;
				}
				const int target = rows > 1 ? (std::max(0, selected) + 1) % rows : 0;
				steps.push_back("click_row " + name + " " + std::to_string(target));
				steps.push_back("wait 3");
				steps.push_back("assert_selected " + name + " " + std::to_string(target));
				if (selected >= 0 && selected != target) {
					steps.push_back("click_row " + name + " " + std::to_string(selected));
					steps.push_back("wait 3");
					steps.push_back("assert_selected " + name + " " + std::to_string(selected));
				}
			}
		}
		GUITab* shownTab = nullptr;
		for (GUIControl* control: tabs) {
			if (dynamic_cast<GUITab*>(control)->GetCheck()) shownTab = dynamic_cast<GUITab*>(control);
		}
		for (GUIControl* control: tabs) {
			const std::string name = control->GetName();
			record["tabs"].push_back(name);
			steps.push_back("activate " + name);
			steps.push_back("wait 3");
			steps.push_back("assert_checked " + name + " 1");
			if (shownTab && control != shownTab) {
				steps.push_back("activate " + shownTab->GetName());
				steps.push_back("wait 3");
				steps.push_back("assert_checked " + shownTab->GetName() + " 1");
			}
		}
		for (GUIControl* control: buttons) {
			const std::string name = control->GetName();
			if (own.count(name)) {
				record["owed"].push_back(name);
				s_Owed.insert(name);
				continue;
			}
			record["buttons"].push_back(name);
			if (restore.empty()) {
				observation = "pressing " + name + " needs a way back: name a restore macro or leave it to the case with own=";
				return false;
			}
			steps.push_back("screen_mark");
			steps.push_back("activate " + name);
			steps.push_back("wait 6");
			if (quiet.count(name)) record["quiet"].push_back(name);
			else steps.push_back("assert_screen_changed " + name);
			steps.push_back("run " + restore);
		}
		record["enabled"] = values.size() + tabs.size() + buttons.size() + record["owed"].size();
		observation = record.dump();
		System::PrintDiagnosticLine("[sweep] " + observation);
		return true;
	}

	bool OwedPressed(std::string& missing) {
		for (const std::string& name: s_Owed) {
			if (!s_HandReached.count(name)) missing += (missing.empty() ? "" : ",") + name;
		}
		return missing.empty();
	}

	void AfterDrawnFrame() {
		++Hand::s_DrawnFrames;
		Hand::Advance();
	}

	bool HandBusy() { return Hand::Busy(); }
	bool HandFinished(bool& passed, std::string& observation) { return Hand::Finished(passed, observation); }

	bool HandClick(GUIControlManager* manager, const std::string& name, GUIControl* modal, std::string& observation, int presses) {
		std::string why;
		if (!manager || !Hand::Target(manager, manager->GetControl(name), modal, why)) {
			observation = name + " " + why;
			return false;
		}
		std::vector<Hand::Phase> phases;
		Hand::AppendClick(phases, manager, name, modal, presses);
		Hand::Start((presses > 1 ? "double-click " : "click ") + name, std::move(phases));
		NoteReached(name);
		observation = name;
		return true;
	}

	bool HandClickAt(int x, int y, const std::string& label, std::function<bool()> taken, std::string& observation) {
		if (x < 0 || y < 0 || x >= g_WindowMan.GetResX() || y >= g_WindowMan.GetResY()) {
			observation = label + " lies off the screen";
			return false;
		}
		std::vector<Hand::Phase> phases;
		phases.push_back([x, y](std::string&) { return Hand::Move(x, y) ? Hand::Beat::Next : Hand::Beat::Fail; });
		phases.push_back([](std::string&) { return Hand::Button(true) ? Hand::Beat::Next : Hand::Beat::Fail; });
		phases.push_back([](std::string&) { return Hand::Button(false) ? Hand::Beat::Next : Hand::Beat::Fail; });
		phases.push_back([taken, label](std::string& note) {
			if (taken()) return Hand::Beat::Next;
			note = "the screen never took the click on " + label;
			return Hand::Beat::Again;
		});
		Hand::Start("click " + label, std::move(phases));
		observation = label;
		return true;
	}

	bool HandGameKey(const std::string& key, std::function<bool()> taken, const std::string& label, std::string& observation) {
		const SDL_Scancode scancode = SDL_GetScancodeFromName(key.c_str());
		if (scancode == SDL_SCANCODE_UNKNOWN) {
			observation = key + " names no key";
			return false;
		}
		// A key in play reaches the game through the event queue alone; no menu input holds it.
		const auto press = [scancode](bool down) {
			SDL_Event event{};
			event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
			event.key.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			event.key.scancode = scancode;
			event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
			event.key.down = down;
			return Hand::Push(event);
		};
		std::vector<Hand::Phase> phases;
		phases.push_back([press](std::string&) { return press(true) ? Hand::Beat::Next : Hand::Beat::Fail; });
		phases.push_back([press](std::string&) { return press(false) ? Hand::Beat::Next : Hand::Beat::Fail; });
		phases.push_back([taken, label](std::string& note) {
			if (taken()) return Hand::Beat::Next;
			note = label + " never answered the key";
			return Hand::Beat::Again;
		});
		Hand::Start("press " + key + " for " + label, std::move(phases));
		observation = key;
		return true;
	}

	bool HandPoint(GUIControlManager* manager, const std::string& name, std::string& observation) {
		std::string why;
		const auto target = manager ? Hand::Target(manager, manager->GetControl(name), nullptr, why) : std::nullopt;
		if (!target) {
			observation = name + " " + why;
			return false;
		}
		auto* box = dynamic_cast<GUITextBox*>(manager->GetControl(name));
		std::vector<Hand::Phase> phases;
		if (box) {
			// A hand gives a box the keyboard by clicking into it.
			Hand::AppendClick(phases, manager, name, nullptr);
		} else {
			const std::pair<int, int> point = *target;
			phases.push_back([point](std::string&) {
				Hand::Move(point.first, point.second);
				return Hand::Beat::Next;
			});
			phases.push_back([](std::string&) { return Hand::Beat::Next; });
		}
		Hand::Start((box ? "click into " : "point at ") + name, std::move(phases));
		NoteReached(name);
		observation = name;
		return true;
	}

	bool HandType(GUIControlManager* manager, const std::string& name, const std::string& text, bool enter, std::string& observation, const std::string& enterWith) {
		auto* box = manager ? dynamic_cast<GUITextBox*>(manager->GetControl(name)) : nullptr;
		std::string why;
		if (!box || !Hand::Target(manager, box, nullptr, why)) {
			observation = name + (box ? " " + why : " is not a text box");
			return false;
		}
		std::vector<Hand::Phase> phases;
		Hand::AppendClick(phases, manager, name, nullptr);
		const bool credential = Credential(box) || CredentialName(name);
		phases.push_back([manager, box](std::string& note) {
			if (!Hand::Within(manager->GetManager()->GetFocusPanel(), box)) {
				note = "the click left the keyboard elsewhere";
				return Hand::Beat::Fail;
			}
			// The whole text selected as a hand selects it with the keys every box reads: End, then Shift+Home.
			Hand::Key("End", true);
			return Hand::Beat::Next;
		});
		phases.push_back([](std::string&) {
			Hand::Key("End", false);
			Hand::Key("Left Shift", true);
			Hand::Key("Home", true);
			return Hand::Beat::Next;
		});
		phases.push_back([](std::string&) {
			Hand::Key("Home", false);
			Hand::Key("Left Shift", false);
			return Hand::Beat::Next;
		});
		phases.push_back([text](std::string&) { return (text.empty() ? Hand::Key("Backspace", true) : Hand::Type(text)) ? Hand::Beat::Next : Hand::Beat::Fail; });
		phases.push_back([text, enterWith](std::string&) {
			if (text.empty()) Hand::Key("Backspace", false);
			if (!enterWith.empty()) Hand::Key(enterWith, true);
			return Hand::Beat::Next;
		});
		if (enter) {
			phases.push_back([](std::string&) { return Hand::Key("Return", true) ? Hand::Beat::Next : Hand::Beat::Fail; });
			phases.push_back([enterWith](std::string&) {
				Hand::Key("Return", false);
				if (!enterWith.empty()) Hand::Key(enterWith, false);
				return Hand::Beat::Next;
			});
		}
		phases.push_back([manager, name, text, credential](std::string& note) {
			auto* typed = dynamic_cast<GUITextBox*>(manager->GetControl(name));
			const std::string shown = typed ? typed->GetText() : "";
			note = shown == text ? "the box reads what was typed" : "the box reads " + Json(Captured(credential, shown)).dump();
			return Hand::Beat::Next;
		});
		Hand::Start((enter ? "type and press Enter in " : "type into ") + name, std::move(phases));
		NoteReached(name);
		observation = credential ? name + " <masked>" : name + " " + text;
		return true;
	}

	bool HandPick(GUIControlManager* manager, const std::string& name, const std::string& item, std::string& observation, bool refused) {
		auto* combo = manager ? dynamic_cast<GUIComboBox*>(manager->GetControl(name)) : nullptr;
		if (!combo) {
			observation = name + " is not a drop-down";
			return false;
		}
		const GUIListPanel::Item* before = combo->GetSelectedItem();
		const std::string kept = before ? before->m_Name : std::string();
		if (refused && kept == item) {
			observation = name + " already reads " + Json(item).dump();
			return false;
		}
		int index = -1;
		for (int i = 0; i < combo->GetCount(); ++i) {
			if (const GUIListPanel::Item* entry = combo->GetItem(i); entry && entry->m_Name == item) {
				index = i;
				break;
			}
		}
		if (index < 0) {
			observation = name + " offers no row " + Json(item).dump();
			return false;
		}
		std::string why;
		if (!combo->IsDropped() && !Hand::Target(manager, combo, nullptr, why)) {
			observation = name + " " + why;
			return false;
		}
		std::vector<Hand::Phase> phases;
		if (!combo->IsDropped()) Hand::AppendClick(phases, manager, name, nullptr);
		phases.push_back([combo](std::string& note) {
			if (combo->IsDropped()) return Hand::Beat::Next;
			note = "the list did not open";
			return Hand::Beat::Again;
		});
		auto point = std::make_shared<std::pair<int, int>>();
		phases.push_back([manager, combo, index, point](std::string& note) { return Hand::ReachRow(manager, combo->GetListPanel(), index, *point, note); });
		phases.push_back([point](std::string&) {
			Hand::Move(point->first, point->second);
			return Hand::Beat::Next;
		});
		phases.push_back([manager, combo, point](std::string& note) {
			if (!Hand::Within(manager->GetManager()->PanelUnderMouse(point->first, point->second), combo->GetListPanel())) {
				note = "the row is covered";
				return Hand::Beat::Fail;
			}
			return Hand::Button(true) ? Hand::Beat::Next : Hand::Beat::Fail;
		});
		// A finger rests on the row for a frame before it lifts; the drop-down commits on the release.
		phases.push_back([](std::string&) { return Hand::Beat::Next; });
		phases.push_back([](std::string&) { return Hand::Button(false) ? Hand::Beat::Next : Hand::Beat::Fail; });
		phases.push_back([](std::string&) { return Hand::Beat::Next; });
		// The pick must hold on the frames after it, through the screen's own refresh; a refused one must not.
		const std::string expected = refused ? kept : item;
		for (int frame = 0; frame < 3; ++frame) {
			phases.push_back([combo, expected, frame](std::string& note) {
				const GUIListPanel::Item* shown = combo->GetSelectedItem();
				if (combo->IsDropped()) {
					note = "the list stayed open after the release";
					return Hand::Beat::Fail;
				}
				if (!shown || shown->m_Name != expected) {
					note = "the drop-down reads " + Json(shown ? shown->m_Name : std::string()).dump() + " " + std::to_string(frame + 1) + " frame(s) after the pick";
					return Hand::Beat::Fail;
				}
				return Hand::Beat::Next;
			});
		}
		Hand::Start((refused ? "pick (refused) " : "pick ") + Json(item).dump() + " in " + name, std::move(phases));
		NoteReached(name);
		observation = name + " " + item;
		return true;
	}

	bool HandRow(GUIControlManager* manager, const std::string& listName, int index, int presses, GUIControl* modal, std::string& observation) {
		GUIControl* control = manager ? manager->GetControl(listName) : nullptr;
		auto* list = dynamic_cast<GUIListPanel*>(control);
		std::string why;
		if (!list || !Hand::Target(manager, control, modal, why)) {
			observation = listName + (list ? " " + why : " is not a list");
			return false;
		}
		if (!list->GetItem(index)) {
			observation = listName + " has no row " + std::to_string(index);
			return false;
		}
		auto point = std::make_shared<std::pair<int, int>>();
		std::vector<Hand::Phase> phases;
		phases.push_back([manager, list, index, point](std::string& note) { return Hand::ReachRow(manager, list, index, *point, note); });
		phases.push_back([point](std::string&) {
			Hand::Move(point->first, point->second);
			return Hand::Beat::Next;
		});
		for (int press = 0; press < presses; ++press) {
			phases.push_back([](std::string&) { return Hand::Button(true) ? Hand::Beat::Next : Hand::Beat::Fail; });
			phases.push_back([](std::string&) { return Hand::Button(false) ? Hand::Beat::Next : Hand::Beat::Fail; });
		}
		phases.push_back([list, index](std::string& note) {
			if (list->GetSelectedIndex() == index) return Hand::Beat::Next;
			note = "the list selected row " + std::to_string(list->GetSelectedIndex());
			return Hand::Beat::Fail;
		});
		Hand::Start((presses > 1 ? "double-click row " : "click row ") + std::to_string(index) + " of " + listName, std::move(phases));
		NoteReached(listName);
		observation = listName + " row " + std::to_string(index);
		return true;
	}

	bool HandDrag(GUIControlManager* manager, const std::string& name, int value, std::string& observation) {
		auto* slider = manager ? dynamic_cast<GUISlider*>(manager->GetControl(name)) : nullptr;
		std::string why;
		if (!slider || !Hand::Target(manager, slider, nullptr, why)) {
			observation = name + (slider ? " " + why : " is not a slider");
			return false;
		}
		if (value < slider->GetMinimum() || value > slider->GetMaximum()) {
			observation = name + " runs " + std::to_string(slider->GetMinimum()) + "-" + std::to_string(slider->GetMaximum());
			return false;
		}
		const bool horizontal = slider->GetOrientation() == GUISlider::Horizontal;
		const Rect r = Rectangle(slider);
		const int length = horizontal ? r[2] : r[3];
		const int range = std::max(1, slider->GetMaximum() - slider->GetMinimum());
		// The press puts the knob where it lands; the tries walk a pixel at a time from there toward the value.
		auto offset = std::make_shared<int>(std::clamp(static_cast<int>(static_cast<long long>(value - slider->GetMinimum()) * length / range), 1, std::max(1, length - 2)));
		auto tries = std::make_shared<int>(0);
		auto stage = std::make_shared<int>(0);
		std::vector<Hand::Phase> phases;
		phases.push_back([manager, slider, value, horizontal, r, length, offset, tries, stage](std::string& note) {
			const int x = horizontal ? r[0] + *offset : r[0] + r[2] / 2, y = horizontal ? r[1] + r[3] / 2 : r[1] + *offset;
			switch ((*stage)++) {
				case 0:
					Hand::Move(x, y);
					return Hand::Beat::Again;
				case 1:
					if (!Hand::Within(manager->GetManager()->PanelUnderMouse(x, y), slider)) {
						note = "the track is covered at " + std::to_string(x) + "," + std::to_string(y);
						return Hand::Beat::Fail;
					}
					Hand::Button(true);
					return Hand::Beat::Again;
				case 2:
					Hand::Button(false);
					return Hand::Beat::Again;
				case 3:
					return Hand::Beat::Again;
				default:
					break;
			}
			*stage = 0;
			const int reached = slider->GetValue();
			if (reached == value) return Hand::Beat::Next;
			const int move = reached < value ? 1 : -1;
			if (++*tries > length || *offset + move < 1 || *offset + move > length - 2) return Hand::Beat::Next;
			*offset += move;
			return Hand::Beat::Again;
		});
		// A value no pixel lands on is reached with the wheel, a step of the slider's resolution at a time.
		phases.push_back([slider, value](std::string& note) {
			if (slider->GetValue() == value) return Hand::Beat::Next;
			note = "the wheel never reached it from " + std::to_string(slider->GetValue());
			Hand::Wheel(slider->GetValue() < value ? 1 : -1);
			return Hand::Beat::Again;
		});
		phases.push_back([slider, value](std::string& note) {
			note = "reached " + std::to_string(slider->GetValue());
			return slider->GetValue() == value ? Hand::Beat::Next : Hand::Beat::Fail;
		});
		Hand::Start("drag " + name + " to " + std::to_string(value), std::move(phases));
		NoteReached(name);
		observation = name + " " + std::to_string(value);
		return true;
	}

	bool Inside(const Rect& child, const Rect& parent) {
		return child[2] > 0 && child[3] > 0 && child[0] >= parent[0] && child[1] >= parent[1] &&
			child[0] + child[2] <= parent[0] + parent[2] && child[1] + child[3] <= parent[1] + parent[3];
	}
	bool TextFits(GUIControlManager* manager, GUIControl* control, std::string& observation) {
		if (auto* list = dynamic_cast<GUIListBox*>(control); list && Visible(control)) {
			bool fits = true;
			for (auto* item: *list->GetItemList()) {
				const std::string shown = list->RegularFittedName(item->m_Name, item->m_OffsetX);
				const int room = list->RegularItemNameRoom(item->m_OffsetX);
				const int width = list->GetFont()->CalculateWidth(shown);
				observation += " row=" + Json(shown).dump() + " width=" + std::to_string(width) + " available=" + std::to_string(room);
				fits &= width <= room;
			}
			return fits && !list->GetItemList()->empty();
		}
		std::string text;
		const bool measureHiddenPreset = control && control->GetName() == "ComboPresetResolution";
		if ((!Visible(control) && !measureHiddenPreset) || !DisplayText(control, text)) return false;
		// A closed picker is measured on the line it draws, which is not always the whole item name.
		if (auto* combo = dynamic_cast<GUIComboBox*>(control)) text = combo->GetText();
		const auto rect = Rectangle(control->GetPanel());
		observation += " text=" + Json(Captured(Credential(control), text)).dump() + " rect=" + Json(rect).dump();
		if (auto* label = dynamic_cast<GUILabel*>(control)) {
			observation += " height=" + std::to_string(label->GetTextHeight()) + " word_width=" + std::to_string(label->GetMaxWordWidth()) + " width=" + std::to_string(label->GetTextWidth());
			if (label->GetHorizontalOverflowScroll() && control->GetName() == "LabelMultiplayerStatus") {
				return label->GetTextHeight() <= rect[3];
			}
			// A label scrolling sideways draws its text on one line, which shows whole only when it is no wider than the label.
			if (label->GetHorizontalOverflowScroll() && label->GetTextWidth() > rect[2]) return false;
			return label->GetTextHeight() <= rect[3] && label->GetMaxWordWidth() <= rect[2];
		}
		std::string section = dynamic_cast<GUIButton*>(control) ? "Button_Up" : dynamic_cast<GUITab*>(control) ? "Tab" :
			dynamic_cast<GUICheckbox*>(control) ? "Checkbox" : dynamic_cast<GUIRadioButton*>(control) ? "RadioButton" :
			dynamic_cast<GUITextBox*>(control) || dynamic_cast<GUIComboBox*>(control) ? "TextBox" : "Label";
		std::string fontName;
		auto* skin = manager->GetSkin();
		if (!skin->GetValue(section, "Font", &fontName)) return false;
		auto* font = skin->GetFont(fontName);
		if (!font) return false;
		int kerning = 0, width = rect[2], height = rect[3];
		skin->GetValue(section, "FontKerning", &kerning);
		const int savedKerning = font->GetKerning();
		font->SetKerning(kerning);
		if (section == "Button_Up") {
			for (const auto* side : {"Left", "Right", "Top", "Bottom"}) {
				int border[4]{};
				skin->GetValue(section, side, border, 4);
				if (std::string_view(side) == "Left" || std::string_view(side) == "Right") width -= border[2]; else height -= border[3];
			}
			--width; --height;
		} else if (section == "Tab") { text = " " + text; width -= 4; }
		else if (section == "Checkbox" || section == "RadioButton") {
			int base[4]{}; skin->GetValue(section, "Base", base, 4);
			width -= base[2] + (section == "Checkbox" ? 2 : 0); text = " " + text;
		} else if (section == "TextBox") {
			int margin = 3, top = 0;
			skin->GetValue(section, "WidthMargin", &margin); skin->GetValue(section, "HeightMargin", &top);
			width -= 2 * margin; height -= top;
			// A combo box shows its selected item in a text panel the 17 pixel drop-down button covers.
			if (dynamic_cast<GUIComboBox*>(control)) width -= 17;
		}
		const int textWidth = font->CalculateWidth(text), textHeight = font->CalculateHeight(text);
		observation += " measured=" + Json({textWidth, textHeight}).dump() + " available=" + Json({width, height}).dump();
		const bool fits = textWidth <= width && textHeight <= height;
		font->SetKerning(savedKerning);
		return fits;
	}

	// A watch judges what the screen shows on every drawn frame between its start and its assert, so a line that is wrong
	// for a few frames cannot slip between two scripted checks.
	struct ShownLine {
		std::string source, control, text;
		bool caption = false; //!< A control's own caption - a button, a box, a tab, a choice - which repeats on every row it serves.
		bool label = false; //!< A label, whose place says which row's value it shows.
		Rect rect{}, parentRect{}; //!< Where the control and its parent stand on the screen.
		const void* parent = nullptr; //!< The parent panel, so two labels can be told to share one.
	};
	struct TextWatch {
		std::string rule, state, control, text;
		uint64_t frames = 0, active = 0, violations = 0;
		int64_t costUs = 0, worstUs = 0; //!< What judging this watch has cost the frames it ran on.
		Json first;
		std::set<std::string> offenders; //!< Each distinct offence is logged once, so one run lists them all.
	};
	std::map<std::string, TextWatch> s_Watches;
	std::vector<std::pair<std::string, std::string>> s_DrawnText; //!< This frame's font-drawn lines, cleared once judged.

	void NoteDrawnText(const std::string& source, const std::string& text) {
		if (!s_Watches.empty()) s_DrawnText.emplace_back(source, text);
	}

	double s_FrameSpanMs = c_DrawWindowSeconds * 1000;
	std::chrono::steady_clock::time_point s_LastEvaluated;

	/// Drawn during the frame being judged: since the previous frame's evaluation.
	bool Shown(GUIControl* control) {
		if (!Visible(control)) return false;
		const double age = PanelDrawAgeMs(control->GetPanel());
		return age >= 0 && age <= s_FrameSpanMs;
	}

	std::vector<std::pair<std::string, GUIControlManager*>> WatchedManagers(GUIControlManager* menu) {
		std::vector<std::pair<std::string, GUIControlManager*>> managers;
		if (menu) managers.emplace_back("menu", menu);
		if (auto* panel = g_MenuMan.GetNetworkPanel()) {
			if (panel->AutomationManager() && panel->AutomationManager() != menu) managers.emplace_back("network", panel->AutomationManager());
			if (panel->OverlayManager()) managers.emplace_back("overlay", panel->OverlayManager());
		}
		return managers;
	}

	GUIControl* WatchedControl(GUIControlManager* menu, const std::string& name) {
		for (const auto& [source, manager]: WatchedManagers(menu)) {
			if (GUIControl* control = manager->GetControl(name)) return control;
		}
		return nullptr;
	}

	/// Every line of text the screen shows this frame, one entry per drawn line of each shown control and of the game's own screen message.
	std::vector<ShownLine> ShownLines(GUIControlManager* menu) {
		std::vector<ShownLine> lines;
		const auto add = [&lines](const std::string& source, const std::string& control, const std::string& text, bool caption = false, GUIControl* owner = nullptr) {
			ShownLine line{source, control, "", caption};
			if (owner && owner->GetPanel()) {
				GUIPanel* parent = owner->GetPanel()->GetParentPanel();
				line.label = dynamic_cast<GUILabel*>(owner) != nullptr;
				line.rect = Rectangle(owner->GetPanel());
				line.parentRect = Rectangle(parent);
				line.parent = parent;
			}
			std::istringstream rows(text);
			for (std::string row; std::getline(rows, row);) {
				const auto start = row.find_first_not_of(" \t\r");
				if (start == std::string::npos) continue;
				line.text = row.substr(start, row.find_last_not_of(" \t\r") - start + 1);
				lines.push_back(line);
			}
		};
		for (const auto& [source, manager]: WatchedManagers(menu)) {
			for (GUIControl* control: *manager->GetControlList()) {
				std::string text;
				const bool caption = dynamic_cast<GUIButton*>(control) || dynamic_cast<GUICheckbox*>(control) || dynamic_cast<GUIRadioButton*>(control) ||
				                     dynamic_cast<GUITab*>(control) || dynamic_cast<GUIComboBox*>(control);
				if (Shown(control) && Text(control, text)) add(source, control->GetName(), text, caption, control);
			}
		}
		if (g_ActivityMan.IsInActivity()) add("screen", "ScreenText", g_FrameMan.GetScreenText(0));
		for (const auto& [source, text]: s_DrawnText) add("drawn", source, text);
		return lines;
	}

	std::string ShownTextJson(GUIControlManager* menu) {
		Json lines = Json::array();
		for (const ShownLine& line: ShownLines(menu)) lines.push_back({{"source", line.source}, {"control", line.control}, {"text", line.text}});
		return lines.dump();
	}

	/// A control name's words and numbers: LabelP3Sensitivity reads Label, P, 3, Sensitivity.
	std::vector<std::string> NameWords(const std::string& name) {
		std::vector<std::string> words;
		for (size_t start = 0; start < name.size();) {
			size_t end = start + 1;
			const bool digits = std::isdigit(static_cast<unsigned char>(name[start])) != 0;
			while (end < name.size() && (digits ? std::isdigit(static_cast<unsigned char>(name[end])) : std::islower(static_cast<unsigned char>(name[end]))) != 0) ++end;
			words.push_back(name.substr(start, end - start));
			start = end;
		}
		return words;
	}

	/// Two labels showing one column's values on their rows: names that differ in one word or number (LabelMasterVolume and
	/// LabelMusicVolume, LabelP3Sensitivity and LabelP4Sensitivity) standing in one column - under one parent at one x and width
	/// on different rows, or at one place in two boxes of one size.
	bool OneColumnRows(const ShownLine& a, const ShownLine& b) {
		if (!a.label || !b.label || a.source != b.source || a.control == b.control || !a.parent || !b.parent) return false;
		const std::vector<std::string> first = NameWords(a.control), second = NameWords(b.control);
		if (first.size() != second.size()) return false;
		size_t differing = 0;
		for (size_t word = 0; word < first.size(); ++word) differing += first[word] != second[word];
		if (differing != 1) return false;
		if (a.rect[0] - a.parentRect[0] != b.rect[0] - b.parentRect[0] || a.rect[2] != b.rect[2]) return false;
		if (a.parent == b.parent) return a.rect[1] != b.rect[1];
		return a.parentRect[2] == b.parentRect[2] && a.parentRect[3] == b.parentRect[3] && a.rect[1] - a.parentRect[1] == b.rect[1] - b.parentRect[1];
	}

	/// The first visible line shown twice at once, naming both controls; null when every line is shown once. A line read with or without
	/// its closing dots is one line. What may repeat: chat (players repeat themselves), a control's caption (a row's verb), one column's
	/// value on its rows (a control name differing only by its row number, or labels standing in one column), an open seat beside
	/// another, and a line with no letter in it.
	Json DuplicateLine(const std::vector<ShownLine>& lines) {
		const auto column = [](const std::string& control) { return control.substr(0, control.find_last_not_of("0123456789") + 1); };
		const auto sentence = [](const std::string& text) { return text.substr(0, text.find_last_not_of(". ") + 1); };
		std::map<std::string, std::vector<const ShownLine*>> seen;
		for (const ShownLine& line: lines) {
			if (line.caption || line.control.starts_with("LabelMatchChat") || line.control == "TextMatchChatInput" || line.text == "Open seat") continue;
			if (std::none_of(line.text.begin(), line.text.end(), [](unsigned char c) { return std::isalpha(c) != 0; })) continue;
			std::vector<const ShownLine*>& shown = seen[sentence(line.text)];
			for (const ShownLine* earlier: shown) {
				const bool rows = earlier->control != line.control && column(earlier->control) == column(line.control) && column(line.control) != line.control;
				if (!rows && !OneColumnRows(*earlier, line)) return Json{{"text", line.text}, {"controls", {earlier->source + "/" + earlier->control, line.source + "/" + line.control}}};
			}
			shown.push_back(&line);
		}
		return nullptr;
	}

	bool WatchStateHolds(const std::string& state) {
		if (state == "always") return true;
		if (state == "panel_open") return g_MenuMan.IsNetworkPanelOpen();
		if (state.starts_with("substate:")) {
			return g_MenuMan.IsMainMenuInteractive() && g_MenuMan.GetMainMenu() && g_MenuMan.GetMainMenu()->AutomationMultiplayerSubScreen() == state.substr(9);
		}
		const auto snapshot = g_NetMatchService.GetLobbySnapshot();
		if (state == "running") return snapshot.serviceState == "Running";
		if (state == "local_held" || state == "remote_held") {
			const bool local = state == "local_held";
			// A catch-up alone is no hold: a host serves one and a late joiner runs one with nobody's seat held. This peer's
			// own return is the catch-up or resync that follows the hold its host told it of.
			const uint64_t frame = ScenarioRunner::GetLockstepCompletedFrame();
			if (local && !snapshot.isHost && (ScenarioRunner::WorldCatchUpActive() || g_NetMatchService.IsMatchResyncing())) {
				const auto& log = ScenarioRunner::GetNetUiToastLog();
				if (std::any_of(log.begin(), log.end(), [](const auto& toast) { return toast.kind == "seat_held" && toast.senderPeerId == 0; })) return true;
			}
			// A seat the host opened is released, as its label reads it, though a peer still catching up replays it held.
			for (const auto& member: snapshot.members) {
				if (member.cpu || (member.peerId == snapshot.localPeerId) != local || ScenarioRunner::IsLockstepSeatReleased(member.peerId) || NetPlayerPresentation::Opened(member.peerId)) continue;
				if (member.aiHeld || member.reclaiming || ScenarioRunner::IsLockstepSeatUnderAI(member.peerId, frame)) return true;
			}
			return false;
		}
		return false;
	}

	/// The panel's round-trip summary against the per-player pings it lists beside it: the summary reads the widest link a host has, or a client's link to its host.
	std::string RttContradiction(GUIControlManager* menu) {
		GUIControl* status = WatchedControl(menu, "LabelNetMatchStatus");
		std::string text;
		if (!status || !Shown(status) || !Text(status, text)) return "";
		const auto summary = text.find("\nRTT ");
		if (summary == std::string::npos) return "";
		const std::string rest = text.substr(summary + 5);
		if (!std::isdigit(static_cast<unsigned char>(rest[0]))) return "";
		const int shown = std::atoi(rest.c_str());
		const bool host = rest.find(" ms / max peer") == rest.find(" ms");
		const auto snapshot = g_NetMatchService.GetLobbySnapshot();
		int expected = -1;
		std::istringstream rows(text);
		for (std::string row; std::getline(rows, row);) {
			if (row.size() < 4 || row[0] != 'P' || row.find(": Ping ") == std::string::npos) continue;
			const int peer = std::atoi(row.c_str() + 1);
			const std::string value = row.substr(row.find(": Ping ") + 7);
			if (!std::isdigit(static_cast<unsigned char>(value[0])) || peer == snapshot.localPeerId) continue;
			const int ping = std::atoi(value.c_str());
			if (host) expected = std::max(expected, ping);
			else if (peer == snapshot.hostPeerId) expected = ping;
		}
		if (expected < 0 || expected == shown) return "";
		return "summary RTT " + std::to_string(shown) + " ms against " + std::to_string(expected) + " ms listed";
	}

	/// Whether a text box's line is as tall as the box shows, under the box's top margin.
	bool LineFits(GUIControlManager* manager, GUIControl* control, const std::string& text, std::string& observation) {
		std::string fontName;
		GUIFont* font = manager->GetSkin()->GetValue("TextBox", "Font", &fontName) ? manager->GetSkin()->GetFont(fontName) : nullptr;
		if (!font) return false;
		int top = 0;
		manager->GetSkin()->GetValue("TextBox", "HeightMargin", &top);
		const int height = font->CalculateHeight(text), available = Rectangle(control->GetPanel())[3] - top;
		observation += " line_height=" + std::to_string(height) + " available_height=" + std::to_string(available);
		return height <= available;
	}

	/// Where a label's text lands: its alignment places it inside a rect that may be larger.
	Rect LabelTextRect(GUILabel* label, Rect rect) {
		const int width = std::min(label->GetTextWidth(), rect[2]), height = label->GetTextHeight();
		const int h = label->GetHAlignment(), v = label->GetVAlignment();
		rect[0] += h == GUIFont::Centre ? (rect[2] - width) / 2 : h == GUIFont::Right ? rect[2] - width : 0;
		rect[1] += v == GUIFont::Middle ? (rect[3] - height) / 2 : v == GUIFont::Bottom ? rect[3] - height : 0;
		rect[2] = std::max(1, width);
		rect[3] = std::max(1, height);
		return rect;
	}

	/// Where a control draws, and where its caption sits in that: a label draws only its text, a check box or a radio button its box
	/// and its caption beside it, a button its face with the caption centred on it, any other control its whole rect.
	std::pair<Rect, Rect> DrawnAndCaption(GUIControlManager* manager, GUIControl* control, const std::string& text) {
		const Rect rect = Rectangle(control->GetPanel());
		if (auto* label = dynamic_cast<GUILabel*>(control)) {
			const Rect drawn = LabelTextRect(label, rect);
			return {drawn, drawn};
		}
		const bool checkbox = dynamic_cast<GUICheckbox*>(control);
		const bool radio = dynamic_cast<GUIRadioButton*>(control);
		const bool button = dynamic_cast<GUIButton*>(control);
		if (text.empty() || !(checkbox || radio || button)) return {rect, rect};
		const std::string section = checkbox ? "Checkbox" : radio ? "RadioButton" : "Button_Up";
		std::string fontName;
		GUIFont* font = manager->GetSkin()->GetValue(section, "Font", &fontName) ? manager->GetSkin()->GetFont(fontName) : nullptr;
		if (!font) return {rect, rect};
		const int height = std::min(rect[3], font->GetFontHeight());
		const int top = rect[1] + (rect[3] - height) / 2;
		if (button) {
			const int width = std::min(rect[2], font->CalculateWidth(text));
			return {rect, {rect[0] + (rect[2] - width) / 2, top, std::max(1, width), std::max(1, height)}};
		}
		int base[4]{};
		manager->GetSkin()->GetValue(section, "Base", base, 4);
		const int box = base[2] + (checkbox ? 2 : 0);
		const int width = std::min(rect[2] - box, font->CalculateWidth(" " + text));
		// Its box and caption are centred on the rect's height, which may be taller than either.
		const int drawnHeight = std::min(rect[3], std::max(base[3], height));
		const Rect drawn{rect[0], rect[1] + (rect[3] - drawnHeight) / 2, std::min(rect[2], box + std::max(0, width)), std::max(1, drawnHeight)};
		return {drawn, {rect[0] + box, top, std::max(1, width), std::max(1, height)}};
	}

	/// Each pair of shown controls of one panel where the caption of one meets what the other draws.
	Json OverlapOffenders(GUIControlManager* menu) {
		Json offenders = Json::array();
		const auto meet = [](const Rect& a, const Rect& b) { return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3]; };
		for (const auto& [source, manager]: WatchedManagers(menu)) {
			// A box holds controls rather than covering them, so the controls each box holds are judged against each other.
			std::map<GUIPanel*, std::vector<std::tuple<GUIControl*, Rect, Rect, bool>>> panels;
			for (GUIControl* control: *manager->GetControlList()) {
				if (!Shown(control) || dynamic_cast<GUICollectionBox*>(control)) continue;
				std::string text;
				const bool captioned = Text(control, text) && !text.empty();
				// A label with nothing to say draws nothing.
				if (dynamic_cast<GUILabel*>(control) && !captioned) continue;
				const auto [drawn, caption] = DrawnAndCaption(manager, control, captioned ? text : std::string());
				panels[control->GetPanel()->GetParentPanel()].emplace_back(control, drawn, caption, captioned);
			}
			for (const auto& [parent, shown]: panels) {
				for (size_t i = 0; i < shown.size(); ++i) {
					for (size_t j = i + 1; j < shown.size(); ++j) {
						const auto& [first, firstDrawn, firstCaption, firstCaptioned] = shown[i];
						const auto& [second, secondDrawn, secondCaption, secondCaptioned] = shown[j];
						if (!(firstCaptioned && meet(firstCaption, secondDrawn)) && !(secondCaptioned && meet(secondCaption, firstDrawn))) continue;
						offenders.push_back({{"source", source}, {"control", first->GetName() + " / " + second->GetName()},
						    {"rects", Json::array({Json(firstDrawn), Json(secondDrawn)})}, {"captions", Json::array({Json(firstCaption), Json(secondCaption)})}});
					}
				}
			}
		}
		return offenders;
	}

	/// Each shown control whose text runs out of its own rect, out of the panel it sits in or off the screen.
	Json LayoutOffenders(GUIControlManager* menu) {
		Json offenders = Json::array();
		BITMAP* screen = g_FrameMan.GetBackBuffer32();
		const Rect screenRect{0, 0, screen ? screen->w : 0, screen ? screen->h : 0};
		for (const auto& [source, manager]: WatchedManagers(menu)) {
			for (GUIControl* control: *manager->GetControlList()) {
				std::string text;
				if (!Shown(control) || !Text(control, text) || text.empty()) continue;
				Rect rect = Rectangle(control->GetPanel());
				std::string fit;
				// A label that scrolls its overflow shows the whole text by design, so only its placement is judged; sideways that holds
				// only for a word wider than the label (an address or a path), since a sentence that cannot wrap is never read whole.
				auto* label = dynamic_cast<GUILabel*>(control);
				const bool scrolls = label && ((label->GetHorizontalOverflowScroll() && label->GetMaxWordWidth() > rect[2]) || label->GetVerticalOverflowScroll());
				// A text box scrolls its one line sideways under the caret, so only the line's height is judged against it.
				const bool fits = scrolls || (dynamic_cast<GUITextBox*>(control) ? LineFits(manager, control, text, fit) : TextFits(manager, control, fit));
				if (label) rect = LabelTextRect(label, rect);
				GUIPanel* parent = control->GetPanel()->GetParentPanel();
				const bool inParent = !parent || Inside(rect, Rectangle(parent));
				const bool onScreen = Inside(rect, screenRect);
				if (fits && inParent && onScreen) continue;
				offenders.push_back({{"source", source}, {"control", control->GetName()}, {"text", text.substr(0, 80)}, {"rect", rect},
				    {"parent", parent ? Json(Rectangle(parent)) : Json(nullptr)}, {"fits", fits}, {"in_parent", inParent}, {"on_screen", onScreen}, {"fit", fit}});
			}
		}
		return offenders;
	}

	void EvaluateWatches(GUIControlManager* menu) {
		// A harness arms the same watches on every peer, scripted or not: one 'start' line per watch, read on the first drawn frame.
		static bool leverRead = false;
		if (!leverRead) {
			leverRead = true;
			if (const char* path = std::getenv("CCCP_TEST_SCREEN_WATCHES"); path && *path) {
				std::ifstream lines(path);
				for (std::string line; std::getline(lines, line);) {
					if (line.empty() || line[0] == '#') continue;
					std::istringstream args("start " + line);
					std::string observation;
					if (!Execute(nullptr, "", "text_watch", args, observation)) System::PrintDiagnosticErrorLine("[text-watch] refused: " + line + " " + observation);
				}
			}
		}
		if (s_Watches.empty()) return;
		// A scene that drops this peer says so in its recorder's directory first: the summaries go out before the process does.
		static bool s_DropReported = false;
		static int s_DropChecks = 0;
		if (!s_DropReported && ++s_DropChecks % 15 == 0 && FrameRecorder::Instance().Enabled() &&
		    std::filesystem::exists(std::filesystem::path(FrameRecorder::Instance().Directory()) / "injected-drop.json")) {
			s_DropReported = true;
			ReportWatches("kill");
		}
		const auto now = std::chrono::steady_clock::now();
		const double span = s_LastEvaluated.time_since_epoch().count() == 0 ? c_DrawWindowSeconds * 1000 : std::chrono::duration<double, std::milli>(now - s_LastEvaluated).count();
		s_FrameSpanMs = std::clamp(span + 1.0, 1.0, c_DrawWindowSeconds * 1000);
		s_LastEvaluated = now;
		std::vector<ShownLine> lines;
		bool linesRead = false;
		for (auto& [name, watch]: s_Watches) {
			++watch.frames;
			struct Cost {
				TextWatch& watch;
				HarnessCost::SimulationSpan span;
				~Cost() {
					const int64_t ns = span.Stop();
					HarnessCost::Charge(HarnessCost::ScreenWatches, ns);
					const int64_t us = ns / 1000;
					watch.costUs += us;
					watch.worstUs = std::max(watch.worstUs, us);
				}
			} cost{watch, {}};
			if (!WatchStateHolds(watch.state)) continue;
			++watch.active;
			if (!linesRead && watch.rule != "layout" && watch.rule != "overlap" && watch.rule != "rtt" && watch.rule != "seat_rows") {
				lines = ShownLines(menu);
				linesRead = true;
			}
			const auto carries = [&lines](const std::string& text) {
				return std::any_of(lines.begin(), lines.end(), [&text](const ShownLine& line) { return line.text.find(text) != std::string::npos; });
			};
			Json detail;
			if (watch.rule == "require" && !carries(watch.text)) {
				detail = "no shown line carries the text";
			} else if (watch.rule == "forbid" && carries(watch.text)) {
				detail = "a shown line carries the text";
			} else if (watch.rule == "equals" || watch.rule == "shown") {
				GUIControl* control = WatchedControl(menu, watch.control);
				std::string text;
				const bool shown = control && Shown(control) && Text(control, text);
				if (!shown) detail = watch.control + " is not shown";
				else if (watch.rule == "equals" && text != watch.text) detail = watch.control + " reads " + Json(text).dump();
			} else if (watch.rule == "duplicates") {
				detail = DuplicateLine(lines);
			} else if (watch.rule == "rtt") {
				if (const std::string contradiction = RttContradiction(menu); !contradiction.empty()) detail = contradiction;
			} else if (watch.rule == "layout") {
				if (Json offenders = LayoutOffenders(menu); !offenders.empty()) detail = offenders;
			} else if (watch.rule == "overlap") {
				if (Json offenders = OverlapOffenders(menu); !offenders.empty()) detail = offenders;
			} else if (watch.rule == "seat_rows") {
				auto* panel = g_MenuMan.GetNetworkPanel();
				GUIControl* box = panel ? panel->GetControl("NetworkSeats") : nullptr;
				Json missing = Json::array();
				for (size_t row = 0; box && row < panel->AutomationSeatRowCount(); ++row) {
					const std::string name = "NetworkSeatName" + std::to_string(row);
					GUIControl* label = panel->GetControl(name);
					const bool shown = label && Shown(label);
					const Rect rect = label ? Rectangle(label->GetPanel()) : Rect{};
					if (shown && Inside(rect, Rectangle(box->GetPanel()))) continue;
					missing.push_back({{"control", name}, {"shown", shown}, {"rect", rect}, {"panel", Rectangle(box->GetPanel())}});
				}
				if (!missing.empty()) detail = missing;
			}
			if (detail.is_null()) continue;
			Json shown = Json::array();
			if (watch.violations++ == 0) {
				if (!linesRead && watch.rule != "layout" && watch.rule != "overlap") lines = ShownLines(menu), linesRead = true;
				for (size_t index = 0; index < lines.size() && index < 24; ++index) shown.push_back(lines[index].source + "/" + lines[index].control + ": " + lines[index].text);
			}
			const Json cases = detail.is_array() ? detail : Json::array({detail});
			for (const Json& offence: cases) {
				const std::string key = offence.is_object() && offence.contains("control") ? offence["control"].get<std::string>() :
				    offence.is_object() && offence.contains("text") ? offence["text"].get<std::string>() : watch.rule;
				if (watch.offenders.size() >= 40 || !watch.offenders.insert(key).second) continue;
				const Json record = {{"wall_ms", FrameRecorder::SteadyNowMS()}, {"sim_frame", g_TimerMan.GetSimUpdateCount()},
				    {"lockstep_frame", ScenarioRunner::GetLockstepCompletedFrame()}, {"detail", offence}, {"shown", shown}};
				if (watch.first.is_null()) watch.first = record;
				System::PrintDiagnosticLine("[text-watch] violation " + name + " " + watch.rule + " " + record.dump());
			}
		}
		s_DrawnText.clear();
	}

	void SetArtifactDirectory(const std::string& directory) { s_ArtifactDirectory = directory; }

	void ReportWatches(const char* flush) {
		const long long throughTick = g_TimerMan.GetSimUpdateCount();
		for (const auto& [name, watch]: s_Watches) {
			System::PrintDiagnosticLine("[text-watch] summary " + Json{{"watch", name}, {"rule", watch.rule}, {"state", watch.state}, {"frames", watch.frames},
			    {"active_frames", watch.active}, {"violations", watch.violations}, {"offences", watch.offenders.size()}, {"cost_us", watch.costUs}, {"worst_us", watch.worstUs},
			    {"through_tick", throughTick}, {"flush", flush}}.dump());
		}
	}

	bool Handles(const std::string& command) {
		return command == "assert_visible" || command == "assert_focus" || command == "assert_rect_inside" || command == "assert_inside_screen" || command == "assert_text_fits" || command == "assert_no_overlap" || command == "assert_no_overlap_within" ||
			command == "dump_refresh_count" || command == "dump_enter_state" ||
			command == "dump_host_options" || command == "dump_player_options" || command == "key" || command == "pad" ||
			command == "key_down" || command == "key_up" || command == "focus" ||
			command == "set_text" || command == "setup_host_port" || command == "combo_drop" || command == "combo_select" || command == "combo_refused" || command == "assert_combo_items" ||
			command == "slider_set" || command == "model_mark" || command == "assert_model_changed" || command == "screen_mark" || command == "assert_screen_changed" ||
			command == "assert_box_text" || command == "type_text" || command == "assert_value" || command == "assert_selected" || command == "click_row" ||
			command == "select_settings_page" || command == "assert_settings_page" || command == "video_mark" ||
			command == "assert_label" || command == "assert_checked" || command == "assert_vertical_scroll" ||
			command == "assert_opaque_panel" || command == "dump_network_layout" || command == "dump_match_identity" ||
			command == "assert_not_drawn" || command == "assert_toast_band" || command == "assert_word_wrap" || command == "assert_roster_fits" || command == "status_line" || command == "ghost_watch" || command == "text_watch" || command == "assert_list_rows" ||
			command == "assert_net_label" || command == "assert_net_label_absent" || command == "push_toast" || command == "dump_seat_state" || command == "dump_world_ownership" || command == "fire_assert" || command == "fire_abort" || command == "fire_worker_throw" ||
			command == "window_event" || command == "assert_window_focus" || command == "game_key" || command == "assert_game_input" || command == "open_local_pause" || command == "meta_command";
	}
	Json PanelCoverage(GUIControl* control) {
		const auto rect = Rectangle(control ? control->GetPanel() : nullptr);
		BITMAP* bitmap = g_FrameMan.GetBackBuffer32();
		int uncovered = 0;
		for (int y = rect[1]; y < rect[1] + rect[3]; ++y) {
			for (int x = rect[0]; x < rect[0] + rect[2]; ++x) {
				if (x < 0 || y < 0 || x >= bitmap->w || y >= bitmap->h) { ++uncovered; continue; }
				const int pixel = getpixel(bitmap, x, y);
				uncovered += geta32(pixel) != 255 && getr32(pixel) == 0 && getg32(pixel) == 0 && getb32(pixel) == 0;
			}
		}
		return {{"rect", rect}, {"uncovered_pixels", uncovered}, {"pixels", rect[2] * rect[3]}};
	}
	bool Execute(GUIControlManager* manager, const std::string& screen, const std::string& command, std::istream& args, std::string& observation) {
		if (command == "meta_command") {
			std::string name;
			args >> name;
			if (!FireAssertAllowed() || !MetaMan::IsConstructed() || !g_MetaMan.GetGUI()) return false;
			GUIControlManager* controls = g_MetaMan.GetGUI()->GetGUIControlManager();
			GUIControl* control = controls ? controls->GetControl(name) : nullptr;
			auto* input = controls ? dynamic_cast<GUIInputWrapper*>(controls->GetInput()) : nullptr;
			observation = name + " game=" + g_MetaMan.GetGameName();
			if (!input || !control) return false;
			return HandClick(controls, name, nullptr, observation);
		}
		if (command == "open_local_pause") {
			// A player opens the match's own menu with Escape.
			if (!FireAssertAllowed() || g_MenuMan.IsLocalPauseMenuOpen()) return false;
			return HandGameKey("Escape", [] { return g_MenuMan.IsLocalPauseMenuOpen(); }, "the match menu", observation);
		}
		if (command == "window_event") {
			std::string kind;
			args >> kind;
			if (!FireAssertAllowed() || !g_WindowMan.GetWindow()) return false;
			SDL_Event event{};
			if (kind == "focus_lost") event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
			else if (kind == "focus_gained") event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
			else if (kind == "minimized") event.type = SDL_EVENT_WINDOW_MINIMIZED;
			else if (kind == "restored") event.type = SDL_EVENT_WINDOW_RESTORED;
			else if (kind == "mouse_enter") event.type = SDL_EVENT_WINDOW_MOUSE_ENTER;
			else return false;
			event.window.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			observation = kind;
			return SDL_PushEvent(&event);
		}
		if (command == "assert_window_focus") {
			int expected = -1;
			args >> expected;
			const bool focused = g_WindowMan.AnyWindowHasFocus();
			observation = "focus=" + std::to_string(focused);
			return (expected == 0 || expected == 1) && focused == (expected == 1);
		}
		if (command == "game_key") {
			std::string key, state;
			args >> key >> state;
			const SDL_Scancode scancode = SDL_GetScancodeFromName(key.c_str());
			if (!FireAssertAllowed() || scancode == SDL_SCANCODE_UNKNOWN || (state != "down" && state != "up")) return false;
			SDL_Event event{};
			event.type = state == "down" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
			event.key.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
			event.key.scancode = scancode;
			event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
			event.key.down = state == "down";
			observation = key + " " + state;
			return SDL_PushEvent(&event);
		}
		if (command == "assert_game_input") {
			int seat = -1, expected = -1;
			std::string element;
			args >> seat >> element >> expected;
			if (seat < Players::PlayerOne || seat >= Players::MaxPlayerCount || (expected != 0 && expected != 1)) return false;
			const int input = element == "L_UP" ? InputElements::INPUT_L_UP : element == "L_RIGHT" ? InputElements::INPUT_L_RIGHT : -1;
			if (input < 0) return false;
			const bool held = g_UInputMan.ElementHeld(seat, input);
			observation = "held=" + std::to_string(held) + " typing=" + std::to_string(g_UInputMan.SeatInputTypedInto());
			return held == (expected == 1);
		}
		if (command == "dump_seat_state") {
			const auto snapshot = g_NetMatchService.GetLobbySnapshot();
			Json members = Json::array();
			for (const auto& member: snapshot.members) members.push_back({{"peer", member.peerId}, {"name", NetPlayerPresentation::Name(member)}, {"state", NetPlayerPresentation::State(member)}, {"route", member.connectedRoute}});
			observation = Json{{"members", members}, {"private_catch_up", ScenarioRunner::WorldCatchUpActive()},
				{"resyncing", g_NetMatchService.IsMatchResyncing()}, {"slow_notice", ScenarioRunner::IsLockstepLocalMachineSlow()}}.dump();
			return true;
		}
		if (command == "dump_world_ownership") {
			// Who holds which world seat at a probe's mark, from the roster, the world's slots and this process's own receipts: written
			// beside the probe as <mark>.ownership.json, never from a requested seat or a scripted expectation.
			std::string mark;
			args >> mark;
			const bool named = !mark.empty() && mark.size() <= 100 && std::all_of(mark.begin(), mark.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
			if (s_ArtifactDirectory.empty() || !named) return false;
			const Json facts = Json::parse(g_NetMatchService.GetWorldOwnershipFacts(), nullptr, false);
			if (!facts.is_object()) return false;
			const auto receipt = [](const char* name) {
				const std::string text = ScenarioRunner::GetHarnessReceipt(name);
				const Json value = text.empty() ? Json(nullptr) : Json::parse(text, nullptr, false);
				return value.is_object() ? value : Json(nullptr);
			};
			// A watcher replays under its authority's round, so its own id is the service's.
			const uint8_t serviceLocal = facts.value("local_peer", static_cast<uint8_t>(0));
			const uint8_t local = serviceLocal != 0 ? serviceLocal : ScenarioRunner::GetLockstepLocalPeerId();
			const Json seat = facts.value("seat_of_peer", Json::object()).value(std::to_string(local), Json(nullptr));
			const Json ticketIncarnation = facts.value("ticket_incarnation", Json(nullptr));
			const Json firstLive = receipt("first_live_tick");
			const uint64_t frame = ScenarioRunner::GetLockstepCompletedFrame();
			const char* instance = std::getenv("CC_TEST_CROSS_INSTANCE");
			Json dump = {{"schema", 1}, {"mark", mark}, {"process", instance && *instance ? Json(instance) : Json(System::GetProcessID())}, {"pid", System::GetProcessID()},
			    {"round", ScenarioRunner::GetLockstepRoundId()}, {"local_peer", local}, {"sim_tick", g_TimerMan.GetSimUpdateCount()}, {"lockstep_frame", frame},
			    {"configuration", facts.value("configuration", Json::object())}, {"seated_first_tick", firstLive.is_object() ? firstLive.value("tick", Json(nullptr)) : Json(nullptr)}};
			Json ownership = {{"seat", seat}, {"local_peer", local}, {"ticket_incarnation", ticketIncarnation}};
			if (Activity* activity = g_ActivityMan.GetActivity()) {
				for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
					const Actor* actor = activity->GetControlledActor(player);
					if (!actor || ScenarioRunner::GetLockstepActorOwner(actor->GetUniqueID(), actor->GetTeam(), false) != local) continue;
					ownership["actor"] = actor->GetUniqueID();
					break;
				}
			}
			dump["ownership"] = ownership;
			// A watcher plays on a lobby id above every seat's and watches the ticks it replayed; a seat plays its round's.
			const bool watcher = local >= c_WorldSpectatorLobbyPeerFirst;
			const Json replayed = facts.value("replayed", Json(nullptr));
			dump["watch"] = {{"role", watcher ? "Spectator" : "Seated"}, {"image_received", facts.value("image_received", false)},
			    {"first", watcher && replayed.is_object() ? replayed.value("first", Json(nullptr)) : firstLive.is_object() ? firstLive.value("tick", Json(nullptr)) : Json(nullptr)},
			    {"last", watcher && replayed.is_object() ? replayed.value("last", Json(nullptr)) : Json(frame)}};
			if (const Json window = receipt("world_spectator_cost_window"); window.is_object()) dump["sim_cost"] = window;
			if (facts.value("is_host", false)) {
				if (facts.contains("promotion")) dump["promotion"] = facts["promotion"];
				if (facts.contains("release")) dump["release"] = facts["release"];
			} else if (const Json reclaim = receipt("ownership_reclaim"); reclaim.is_object()) {
				// A promoted watcher's own side: the seat its slot is, the actor it plays from its activation, and its first fresh input.
				Json promotion = {{"seat", seat}, {"freed_seat", seat}, {"actor", reclaim.value("actor", Json(nullptr))}, {"ticket_incarnation", ticketIncarnation},
				    {"activation_tick", facts.contains("watcher_seated_at") ? facts["watcher_seated_at"] : reclaim.value("activation_tick", Json(nullptr))}};
				if (const Json first = receipt("first_controllable_input"); first.is_object()) {
					const Json input = first.value("input", Json::object());
					promotion["input_tick"] = first.value("wire_tick", Json(nullptr));
					promotion["input_created_tick"] = input.value("produced_tick", Json(nullptr));
					promotion["applied_actor"] = input.value("actor", Json(nullptr));
					promotion["applied_seat"] = facts.value("seat_of_peer", Json::object()).value(std::to_string(local), Json(nullptr));
					promotion["applied_incarnation"] = first.value("seat_incarnation", Json(nullptr));
					promotion["applied_input"] = input;
				}
				dump["promotion"] = promotion;
			}
			std::ofstream output(std::filesystem::path(s_ArtifactDirectory) / (mark + ".ownership.json"));
			output << dump.dump(2) << '\n';
			observation = dump.dump();
			return static_cast<bool>(output);
		}
		if (command == "fire_assert") {
			if (!FireAssertAllowed()) return false;
			// The assert seam the harness needs: a scripted run must be able to answer a real assert the way
			// a player does, and prove the run went on.
			std::string reason{std::istreambuf_iterator<char>(args), std::istreambuf_iterator<char>()};
			if (reason.empty()) return false;
			observation = reason;
			RTEAssert(false, reason);
			return true;
		}
		if (command == "fire_abort") {
			if (!FireAssertAllowed()) return false;
			// A real abort for a scripted run: its reason, its dumps and its exit are what the run proves on the box.
			std::string reason{std::istreambuf_iterator<char>(args), std::istreambuf_iterator<char>()};
			if (reason.empty()) return false;
			observation = reason;
			RTEAbort(reason);
			return true;
		}
		if (command == "fire_worker_throw") {
			if (!FireAssertAllowed()) return false;
			// An allocation failure on a worker no code catches: the run proves the engine ends naming it, on any box.
			observation = "std::bad_alloc on a worker thread";
			RTEError::ThrowOnWorkerThread();
			return true;
		}
		if (command == "push_toast") {
			std::string kind, text;
			args >> kind;
			std::getline(args >> std::ws, text);
			if (!FireAssertAllowed() || kind.empty() || text.empty()) return false;
			ScenarioRunner::PushNetUiToast(kind, text);
			observation = kind + " " + text;
			return true;
		}
		if (command == "dump_match_identity") {
			std::string path;
			args >> std::quoted(path);
			const auto snapshot = g_NetMatchService.GetLobbySnapshot();
			const auto config = g_NetMatchService.GetLobbyMatchConfig();
			const uint64_t round = ScenarioRunner::GetLockstepRoundId();
			if (path.empty() || !FrameRecorder::Instance().Enabled() || snapshot.serviceState != "Running" || !round) return false;
			const Json identity = {{"schema", 1}, {"match_id", g_NetMatchService.GetAutosaveMatchId()}, {"session_id", config.sessionId},
				{"round", round}, {"config_hash", NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(config))}, {"peer_id", snapshot.localPeerId}, {"host", snapshot.isHost}};
			std::ofstream output(path);
			output << identity.dump(2) << '\n';
			observation = identity.dump();
			return output.good();
		}
		if (command == "dump_network_layout") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			if (!panel) return false;
			const auto& roster = panel->GetRosterRect();
			const int fundsBottom = g_FrameMan.GetLargeFont()->GetFontHeight();
			observation = Json{{"roster", {roster.x, roster.y, roster.width, roster.height}}, {"roster_visible", roster.visible},
				{"funds_bottom", fundsBottom}, {"roster_below_funds", roster.y >= fundsBottom}, {"local_pause", g_MenuMan.IsLocalPauseMenuOpen()}}.dump();
			return true;
		}
		if (command == "assert_net_label" || command == "assert_net_label_absent") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			std::string name, expected, text;
			args >> name;
			std::getline(args >> std::ws, expected);
			bool found = panel && panel->AutomationLabelText(name, text);
			if (!found) if (auto* main = g_MenuMan.GetMainMenu()) found = main->AutomationLabelText(name, text);
			const bool carries = found && text.find(expected) != std::string::npos;
			observation = name + " \"" + Captured(CredentialName(name), expected) + "\" text=" + Json(Captured(CredentialName(name), text)).dump();
			return found && carries == (command == "assert_net_label");
		}
		if (command == "assert_toast_band") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			if (!panel) return false;
			const auto& toasts = panel->GetToastRect();
			const auto& seats = panel->GetSeatsPanelRect();
			int expectedRows = ParseToastBandExpectedRows(args);
			// The `single` argument counts painted bands, not just the live rect: a band that
			// moved must not leave its pixels behind on the GUI layer (ENGINE 195's second band).
			// A non-numeric first token fails the int read; clear() lets the rest of the line parse.
			args.clear();
			bool single = false;
			for (std::string token; args >> token;) single = single || token == "single";
			int rows = 0;
			for (int row = 0; row < 3; ++row) {
				auto* label = panel->GetControl("LabelNetMatchToast" + std::to_string(row));
				if (Visible(label)) ++rows;
			}
			BITMAP* screen = g_FrameMan.GetBackBuffer32();
			const bool inside = !toasts.visible || (toasts.x >= 0 && toasts.y >= 0 &&
			                                        toasts.x + toasts.width <= screen->w && toasts.y + toasts.height <= screen->h);
			const bool clearOfSeats = !toasts.visible || !seats.visible ||
			                          toasts.y + toasts.height <= seats.y || toasts.y >= seats.y + seats.height ||
			                          toasts.x + toasts.width <= seats.x || toasts.x >= seats.x + seats.width;
			// A band that moved must not leave its pixels behind: the panel's own scan finds a
			// toast-fill run no live rect covers. The local pause menu's backdrop rewrites the
			// whole layer each frame, so the scan only runs on the game screen.
			NetModerationGUI::GhostBandHit ghost;
			if (single && !g_MenuMan.IsLocalPauseMenuOpen()) {
				ghost = panel->ScanGhostBand(100);
			}
			const int ghostRun = ghost.found ? ghost.run : -1;
			const auto& chat = panel->GetChatRect();
			const auto& status = panel->GetStatusRect();
			const auto& roster = panel->GetRosterRect();
			observation = Json{{"screen", {screen->w, screen->h}}, {"toasts", {toasts.x, toasts.y, toasts.width, toasts.height}},
				{"toasts_visible", toasts.visible}, {"seats", {seats.x, seats.y, seats.width, seats.height}}, {"seats_visible", seats.visible},
				{"chat", {chat.x, chat.y, chat.width, chat.height}}, {"chat_visible", chat.visible},
				{"status", {status.x, status.y, status.width, status.height}}, {"status_visible", status.visible},
				{"roster", {roster.x, roster.y, roster.width, roster.height}}, {"roster_visible", roster.visible},
				{"inside_screen", inside}, {"clear_of_seats", clearOfSeats}, {"rows", rows}, {"expected_rows", expectedRows},
				{"single", single}, {"ghost_band", ghostRun < 0 ? Json(nullptr) : Json{{"x", ghost.x}, {"y", ghost.y}, {"run", ghostRun}}}}.dump();
			return inside && clearOfSeats && (expectedRows < 0 || rows == expectedRows) && ghostRun < 0;
		}
		if (command == "text_watch") {
			std::string mode, name;
			args >> mode >> name;
			if (name.empty()) return false;
			if (mode == "start") {
				TextWatch watch;
				args >> watch.rule >> watch.state;
				if (watch.rule == "equals" || watch.rule == "shown") args >> watch.control;
				std::getline(args >> std::ws, watch.text);
				const bool textRule = watch.rule == "require" || watch.rule == "forbid" || watch.rule == "equals";
				const bool known = textRule || watch.rule == "shown" || watch.rule == "duplicates" || watch.rule == "rtt" || watch.rule == "layout" || watch.rule == "overlap" || watch.rule == "seat_rows";
				if (!known || watch.state.empty() || (textRule && watch.text.empty()) || ((watch.rule == "equals" || watch.rule == "shown") && watch.control.empty())) {
					observation = "unknown or incomplete watch";
					return false;
				}
				observation = Json{{"armed", name}, {"rule", watch.rule}, {"state", watch.state}, {"control", watch.control}, {"text", watch.text}}.dump();
				System::PrintDiagnosticLine("[text-watch] armed " + observation);
				s_Watches[name] = std::move(watch);
				return true;
			}
			// An assert fails the script on an offence; a close ends the window and leaves the verdict to the review that reads the log.
			if (mode == "assert" || mode == "close") {
				const auto found = s_Watches.find(name);
				if (found == s_Watches.end()) { observation = name + " was never armed"; return false; }
				const TextWatch watch = std::move(found->second);
				s_Watches.erase(found);
				observation = Json{{"watch", name}, {"rule", watch.rule}, {"state", watch.state}, {"text", watch.text}, {"frames", watch.frames},
				    {"active_frames", watch.active}, {"violations", watch.violations}, {"offences", watch.offenders.size()}}.dump();
				System::PrintDiagnosticLine("[text-watch] summary " + observation);
				// A watch whose state never held judged nothing.
				return mode == "close" || (watch.active > 0 && watch.violations == 0);
			}
			observation = Json{{"mode", mode}}.dump();
			return false;
		}
		if (command == "ghost_watch") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			if (!panel) return false;
			std::string mode;
			args >> mode;
			if (mode == "start") {
				// Arms the panel's per-draw scan: every DrawMatchToasts frame counts a stale band,
				// so a ghost visible only between a band's move and the next wipe is still caught.
				panel->ArmGhostWatch();
				observation = "armed";
				return true;
			}
			if (mode == "assert") {
				const auto& last = panel->GhostWatchLast();
				const int hits = panel->GhostWatchHits();
				const int probe = panel->GhostWatchProbe();
				panel->DisarmGhostWatch();
				observation = Json{{"hits", hits}, {"probe_pixel", probe},
					{"last", last.found ? Json{{"x", last.x}, {"y", last.y}, {"run", last.run}} : Json(nullptr)}}.dump();
				return hits == 0;
			}
			observation = Json{{"mode", mode}}.dump();
			return false;
		}
		if (command == "assert_word_wrap") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			auto* font = g_FrameMan.GetSmallFont(true);
			if (!panel || !font) return false;
			std::string surface = "roster";
			args >> surface;
			std::string source, wrapped;
			int textWidth = 0, capWidth = 0;
			if (surface == "probe") {
				// The same helper and width rule the roster box runs, on a script-chosen text - a
				// long token exercises the wrap without depending on match state.
				const std::string rest{std::istreambuf_iterator<char>(args), std::istreambuf_iterator<char>()};
				const auto start = rest.find_first_not_of(' ');
				source = start == std::string::npos ? std::string() : rest.substr(start);
				int boxWidth = 0;
				const bool textMissing = source.empty();
				const bool wrapUnavailable = !textMissing && !panel->AutomationWrapLines(source, wrapped, boxWidth);
				if (textMissing || wrapUnavailable) {
					observation = Json{{"text_missing", textMissing}, {"wrap_unavailable", wrapUnavailable}, {"source", source}}.dump();
					return false;
				}
				textWidth = boxWidth - 12;
				capWidth = std::max(0, g_WindowMan.GetResX() - 28);
			} else {
				const auto& audit = surface == "status" ? panel->GetStatusWrap() : panel->GetRosterWrap();
				if (!audit.active) {
					if (surface == "status") { observation = "status: no wrap surface"; return true; }
					observation = surface + " wrap surface not active";
					return false;
				}
				source = audit.source;
				wrapped = audit.wrapped;
				textWidth = audit.textWidth;
				capWidth = audit.capWidth;
			}
			// A mid-word break leaves a token's fragment on a line of its own; requiring every
			// whitespace-delimited source token to land inside one wrapped line catches it.
			std::istringstream lines(wrapped);
			std::vector<std::string> rows;
			for (std::string row; std::getline(lines, row);) rows.push_back(row);
			auto tokenLanded = [&](const std::string& token) {
				for (const auto& row: rows) {
					if (row.find(token) != std::string::npos) return true;
					std::istringstream words(row);
					for (std::string word; words >> word;) {
						if (word.size() <= 3 || !word.ends_with("...")) continue;
						const std::string prefix = word.substr(0, word.size() - 3);
						if (!prefix.empty() && token.size() > prefix.size() && token.starts_with(prefix)) return true;
					}
				}
				return false;
			};
			std::istringstream tokens(source);
			std::string splitToken;
			for (std::string token; tokens >> token;) {
				if (!tokenLanded(token)) { splitToken = token; break; }
			}
			int longestWord = 0;
			tokens.clear();
			tokens.str(source);
			for (std::string token; tokens >> token;) longestWord = std::max(longestWord, font->CalculateWidth(token));
			int widestLine = 0;
			for (const auto& row: rows) widestLine = std::max(widestLine, font->CalculateWidth(row));
			const bool capped = textWidth >= capWidth;
			const bool sized = longestWord <= textWidth || capped;
			const bool linesFit = widestLine <= textWidth;
			observation = Json{{"surface", surface}, {"text_width", textWidth}, {"longest_word", longestWord},
				{"widest_line", widestLine}, {"rows", rows}, {"split_token", splitToken},
				{"capped", capped}, {"lines_fit", linesFit}, {"source", source}}.dump();
			return splitToken.empty() && sized && (surface != "status" || linesFit);
		}
		if (command == "status_line") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			if (!panel) return false;
			const std::string rest{std::istreambuf_iterator<char>(args), std::istreambuf_iterator<char>()};
			std::string text = rest;
			if (!text.empty() && text[0] == ' ') text.erase(0, 1);
			panel->SetStatusProbeLine(text);
			observation = "status line " + std::to_string(text.size());
			return true;
		}
		if (command == "assert_roster_fits") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			auto* font = g_FrameMan.GetSmallFont(true);
			if (!panel || !font) return false;
			const std::string rest{std::istreambuf_iterator<char>(args), std::istreambuf_iterator<char>()};
			std::string text = rest;
			if (!text.empty() && text[0] == ' ') text.erase(0, 1);
			std::string wrapped;
			int boxWidth = 0;
			if (text.empty() || !panel->AutomationWrapLines(text, wrapped, boxWidth)) {
				observation = "roster fit text missing or wrap unavailable";
				return false;
			}
			const int column = boxWidth - 12;
			int widestToken = 0;
			std::istringstream tokens(wrapped);
			for (std::string token; tokens >> token;) widestToken = std::max(widestToken, font->CalculateWidth(token));
			int widestLine = 0;
			std::istringstream lines(wrapped);
			for (std::string row; std::getline(lines, row);) widestLine = std::max(widestLine, font->CalculateWidth(row));
			auto* large = g_FrameMan.GetLargeFont();
			const int y = std::max(8, (large ? large->GetFontHeight() : 8) + 4);
			const int height = font->CalculateHeight(wrapped) + 12;
			const int right = 8 + boxWidth + 1;
			const int bottom = y + height + 1;
			const bool inside = right <= g_WindowMan.GetResX() && bottom <= g_WindowMan.GetResY();
			const auto& live = panel->GetRosterRect();
			const bool fits = widestToken <= column && widestLine <= column;
			observation = Json{{"surface", "roster_fits"}, {"widest_token", widestToken}, {"widest_line", widestLine},
				{"column", column}, {"rect", Json::array({8, y, boxWidth + 1, height + 1})},
				{"roster_rect", Json::array({live.x, live.y, live.width, live.height})},
				{"inside", inside}, {"ellipsis", wrapped.find("...") != std::string::npos}, {"drawn", wrapped}}.dump();
			return fits && inside;
		}
		if (command == "assert_no_overlap") {
			const std::string arguments{std::istreambuf_iterator<char>(args), std::istreambuf_iterator<char>()};
			std::istringstream names(arguments);
			std::string first, second;
			names >> first >> second;
			auto* network = g_MenuMan.GetNetworkPanel();
			auto find = [&](const std::string& name) {
				auto* control = manager ? manager->GetControl(name) : nullptr;
				return control ? control : network ? network->GetControl(name) : nullptr;
			};
			auto* aControl = find(first);
			auto* bControl = find(second);
			if (!Visible(aControl) || !Visible(bControl)) { observation = "both controls must be visible"; return false; }
			const auto a = Rectangle(aControl->GetPanel()), b = Rectangle(bControl->GetPanel());
			observation = first + " " + second + " rect=" + Json(a).dump() + " other=" + Json(b).dump();
			return a[0] + a[2] <= b[0] || b[0] + b[2] <= a[0] || a[1] + a[3] <= b[1] || b[1] + b[3] <= a[1];
		}
		if (command == "assert_no_overlap_within") {
			// No two visible controls of one panel overlap, so the panel's layout leaves every one readable.
			std::string parentName;
			args >> parentName;
			auto* network = g_MenuMan.GetNetworkPanel();
			GUIControl* parent = manager ? manager->GetControl(parentName) : nullptr;
			if (!parent && network) parent = network->GetControl(parentName);
			std::vector<GUIControl*>* children = parent ? parent->GetChildren() : nullptr;
			if (!Visible(parent) || !children) { observation = parentName + " missing or hidden"; return false; }
			std::vector<std::pair<std::string, Rect>> shown;
			for (GUIControl* child: *children)
				if (Visible(child)) shown.emplace_back(child->GetName(), Rectangle(child->GetPanel()));
			Json overlaps = Json::array();
			for (size_t i = 0; i < shown.size(); ++i) {
				for (size_t j = i + 1; j < shown.size(); ++j) {
					const Rect& a = shown[i].second;
					const Rect& b = shown[j].second;
					if (!(a[0] + a[2] <= b[0] || b[0] + b[2] <= a[0] || a[1] + a[3] <= b[1] || b[1] + b[3] <= a[1])) overlaps.push_back({shown[i].first, shown[j].first, a, b});
				}
			}
			observation = Json{{"parent", parentName}, {"visible_children", shown.size()}, {"overlaps", overlaps}}.dump();
			return overlaps.empty();
		}
		if (command == "assert_inside_screen") {
			std::string name;
			args >> name;
			auto* network = g_MenuMan.GetNetworkPanel();
			auto* control = manager ? manager->GetControl(name) : nullptr;
			if (!control && network) control = network->GetControl(name);
			if (!control || !control->GetPanel()) { observation = name + " missing"; return false; }
			BITMAP* screen = g_FrameMan.GetBackBuffer32();
			const auto rect = Rectangle(control->GetPanel());
			const bool inside = screen && rect[0] >= 0 && rect[1] >= 0 &&
			                    rect[0] + rect[2] <= screen->w && rect[1] + rect[3] <= screen->h;
			observation = Json{{"control", name}, {"rect", rect}, {"screen", {screen ? screen->w : 0, screen ? screen->h : 0}}, {"inside", inside}}.dump();
			return inside;
		}
		if (command == "dump_refresh_count") {
			auto* panel = g_MenuMan.GetNetworkPanel();
			if (!panel) return false;
			observation = Json{{"refresh_count", panel->RefreshCount()}, {"refresh_changes", panel->RefreshChangeCount()}}.dump();
			System::PrintDiagnosticLine("[refresh-count] " + observation);
			return true;
		}
		if (command == "dump_enter_state") {
			if (!manager || !manager->GetInput()) return false;
			const int state = manager->GetInput()->GetAsciiState(static_cast<unsigned char>(GUIInput::Key_Enter));
			const char* name = state == GUIInput::Pushed ? "Pushed" : state == GUIInput::Released ? "Released" : state == GUIInput::Repeat ? "Repeat" : "None";
			observation = Json{{"key_enter", state}, {"name", name}}.dump();
			System::PrintDiagnosticLine("[enter-state] " + observation);
			return true;
		}
		if (command == "assert_vertical_scroll") {
			std::string name;
			args >> name;
			auto* panel = g_MenuMan.GetNetworkPanel();
			auto* label = dynamic_cast<GUILabel*>(panel ? panel->GetControl(name) : nullptr);
			const bool enabled = label && label->GetVerticalOverflowScroll();
			const bool active = label && label->OverflowScrollIsActivated();
			observation = name + " enabled=" + std::to_string(enabled) + " active=" + std::to_string(active);
			return label && Visible(label) && enabled && active;
		}
		if (command == "video_mark") {
			args >> observation;
			if (observation.empty()) return false;
			FrameRecorder::Instance().RecordEvent("video_mark " + observation);
			// Each mark closes a span of the scene: the watches say what they judged up to it.
			ReportWatches("periodic");
			return true;
		}
		try {
			if (!manager) { observation = "no active control manager"; return false; }
			if (command == "assert_list_rows") {
				std::string name;
				int expected = -1;
				args >> name >> expected;
				auto* list = dynamic_cast<GUIListBox*>(manager->GetControl(name));
				const int actual = list && list->GetItemList() ? static_cast<int>(list->GetItemList()->size()) : -1;
				observation = name + " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual);
				return list != nullptr && actual == expected;
			}
			if (command == "assert_not_drawn") {
				std::string name;
				args >> name;
				auto* control = manager->GetControl(name);
				GUIControl* drawn = FirstDrawn(control);
				observation = name + (control ? "" : " missing") + " drawn=" + (drawn ? drawn->GetName() : std::string("none"));
				if (control) System::PrintDiagnosticLine("[draw-record] " + name + " last_drawn_ms=" + std::to_string(static_cast<long long>(PanelDrawAgeMs(control->GetPanel()))) + " in_latest_pass=" + (drawn == control ? "1" : "0"));
				return control != nullptr && drawn == nullptr;
			}
			if (command == "assert_opaque_panel") {
				std::string name;
				args >> name;
				auto* control = manager->GetControl(name);
				if (!control || !Visible(control)) return false;
				const Json coverage = PanelCoverage(control);
				observation = coverage.dump();
				return coverage["uncovered_pixels"] == 0;
			}
			if (command == "assert_checked") {
				std::string name;
				int expected = -1;
				args >> name >> expected;
				GUIControl* control = manager->GetControl(name);
				auto* box = dynamic_cast<GUICheckbox*>(control);
				auto* radio = dynamic_cast<GUIRadioButton*>(control);
				auto* tab = dynamic_cast<GUITab*>(control);
				const int actual = box ? box->GetCheck() == GUICheckbox::Checked : radio ? radio->GetCheck() : tab ? tab->GetCheck() : 0;
				observation = name + " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual);
				return (box || radio || tab) && Visible(control) && (expected == 0 || expected == 1) && actual == expected;
			}
			if (command == "assert_label") {
				std::string name, expected, text;
				args >> name;
				std::getline(args >> std::ws, expected);
				bool found = DisplayText(manager->GetControl(name), text);
				MainMenuGUI* main = g_MenuMan.GetMainMenu();
				if (!found && main && manager == main->AutomationManager()) found = main->AutomationLabelText(name, text);
				const bool credential = CredentialName(name) || Credential(manager->GetControl(name));
				observation = name + " \"" + Captured(credential, expected) + "\" text=\"" + Captured(credential, text) + "\"";
				return found && text.find(expected) != std::string::npos;
			}
			if (command == "key_down" || command == "key_up") {
				std::string key;
				args >> std::quoted(key);
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				observation = key;
				return input && !key.empty() && input->QueueAutomationInput("key", key, command == "key_down");
			}
			if (command == "focus") {
				std::string target;
				args >> std::quoted(target);
				return HandPoint(manager, target, observation);
			}
			if (command == "assert_combo_items") {
				// The items a combo offers, in order, separated by '|'.
				std::string comboName, expected;
				args >> std::quoted(comboName);
				std::getline(args >> std::ws, expected);
				auto* combo = dynamic_cast<GUIComboBox*>(manager->GetControl(comboName));
				if (!combo) { observation = comboName + " missing"; return false; }
				std::string items;
				for (int i = 0; i < combo->GetCount(); ++i) {
					if (const GUIListPanel::Item* entry = combo->GetItem(i)) items += (i == 0 ? "" : "|") + entry->m_Name;
				}
				observation = comboName + " items=\"" + items + "\"";
				return items == expected;
			}
			if (command == "combo_drop" || command == "combo_select" || command == "combo_refused") {
				std::string comboName, item;
				args >> std::quoted(comboName);
				std::getline(args >> std::ws, item);
				auto* combo = dynamic_cast<GUIComboBox*>(manager->GetControl(comboName));
				if (!combo) { observation = comboName + " is not a drop-down"; return false; }
				if (command == "combo_drop") {
					if (!item.empty()) { observation = comboName + " takes no row"; return false; }
					return HandClick(manager, comboName, nullptr, observation);
				}
				if (item.empty()) { observation = comboName + " no item"; return false; }
				return HandPick(manager, comboName, item, observation, command == "combo_refused");
			}
			if (command == "model_mark") {
				s_ModelMark = ModelText();
				observation = std::to_string(s_ModelMark.size()) + " bytes";
				return true;
			}
			if (command == "assert_model_changed") {
				// The change reached what the control drives: the menu's own choices or a saved setting.
				std::string control;
				args >> control;
				observation = control;
				return !s_ModelMark.empty() && ModelText() != s_ModelMark;
			}
			if (command == "screen_mark") {
				s_ScreenMark = ScreenText(manager);
				observation = std::to_string(s_ScreenMark.size()) + " bytes";
				return true;
			}
			if (command == "assert_screen_changed") {
				std::string control;
				args >> control;
				observation = control + " screen=" + ScreenText(manager).substr(0, 60);
				return !s_ScreenMark.empty() && ScreenText(manager) != s_ScreenMark;
			}
			if (command == "type_text") {
				// Typed by a hand, Enter or not; the text may be empty.
				std::string target, mode, text;
				args >> std::quoted(target) >> mode;
				std::getline(args, text);
				if (!text.empty() && text[0] == ' ') text.erase(0, 1);
				if (mode != "enter" && mode != "plain") { observation = target + " needs enter or plain"; return false; }
				return HandType(manager, target, text, mode == "enter", observation);
			}
			if (command == "assert_box_text") {
				std::string target, expected;
				args >> std::quoted(target);
				std::getline(args, expected);
				if (!expected.empty() && expected[0] == ' ') expected.erase(0, 1);
				auto* box = dynamic_cast<GUITextBox*>(manager->GetControl(target));
				const bool credential = Credential(box) || CredentialName(target);
				const std::string text = box ? box->GetText() : "";
				observation = target + " \"" + Captured(credential, expected) + "\" text=\"" + Captured(credential, text) + "\"";
				return box && text == expected;
			}
			if (command == "assert_value") {
				std::string target;
				int expected = 0;
				args >> std::quoted(target) >> expected;
				auto* slider = dynamic_cast<GUISlider*>(manager->GetControl(target));
				observation = target + " expected=" + std::to_string(expected) + " actual=" + (slider ? std::to_string(slider->GetValue()) : std::string("none"));
				return slider && slider->GetValue() == expected;
			}
			if (command == "assert_selected" || command == "click_row") {
				std::string target;
				int row = -1;
				args >> std::quoted(target) >> row;
				if (command == "click_row") {
					// A row named by the game it lists (wait_row's name) is clicked in the list that shows it now.
					std::string listName;
					if (row < 0 && g_MenuMan.GetMainMenu() && g_MenuMan.GetMainMenu()->AutomationRowOf(target, listName, row)) return HandRow(manager, listName, row, 1, nullptr, observation);
					return HandRow(manager, target, row, 1, nullptr, observation);
				}
				auto* list = dynamic_cast<GUIListBox*>(manager->GetControl(target));
				observation = target + " expected=" + std::to_string(row) + " actual=" + (list ? std::to_string(list->GetSelectedIndex()) : std::string("none"));
				return list && list->GetSelectedIndex() == row;
			}
			if (command == "slider_set") {
				std::string target;
				int value = 0;
				args >> std::quoted(target);
				if (!(args >> value)) { observation = target + " needs a value"; return false; }
				return HandDrag(manager, target, value, observation);
			}
			std::string name, argument, extra;
			args >> std::quoted(name) >> argument >> extra;
			if (!extra.empty()) { observation = "unexpected arguments"; return false; }
			auto* control = manager->GetControl(name);
			if (command == "setup_host_port") {
				// SETUP, not a gesture: the port a scripted host listens on, which a player sets in Advanced. A run picks its own
				// so two runs on one machine never meet.
				if (name.empty() || !argument.empty()) { observation = "need one port"; return false; }
				if (auto* menu = g_MenuMan.GetMainMenu(); menu && menu->AutomationSetupHostPort(name)) {
					observation = name;
					return true;
				}
				observation = name + " is not a port";
				return false;
			}
			if (command == "key" || command == "pad") {
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				observation = name + " " + argument;
				return input && (argument == "down" || argument == "up") && input->QueueAutomationInput(command, name, argument == "down");
			}
			if (command == "select_settings_page" || command == "assert_settings_page") {
				const std::string active = SettingsPage(manager);
				observation = name + " active=" + active;
				if (!argument.empty()) return false;
				// "Network" asserts wherever its selector sits; "Network:Chat" asserts one page.
				if (command == "assert_settings_page") {
					return active == name || (active.size() > name.size() && active.compare(0, name.size(), name) == 0 && active[name.size()] == ':');
				}
				// A page opens the way a player opens it: a click on its tab.
				GUITab* tab = PageTab(manager, name);
				if (!tab) return false;
				return HandClick(manager, tab->GetName(), nullptr, observation);
			}
			if (command == "dump_host_options" || command == "dump_player_options") {
				if (!name.empty()) return false;
				static unsigned int capture = 0;
				std::filesystem::path path;
				do {
					path = std::filesystem::path("ScreenShots") / (command + "_" + std::to_string(capture++));
				} while (std::filesystem::exists(path.string() + ".json") || std::filesystem::exists(path.string() + ".png") || std::filesystem::exists(path.string() + ".json.tmp"));
				const auto lobby = g_NetMatchService.GetLobbySnapshot();
				Json table = Json::array();
				for (const NetHostActivityChoice& activity : NetMatchService::ListHostActivities()) {
					Json scenes = Json::array();
					for (const NetHostSceneChoice& scene : activity.scenes) {
						scenes.push_back({{"name", scene.name}, {"module", scene.module}});
					}
					table.push_back({{"preset", activity.preset}, {"module", activity.module},
						{"activity_type", activity.activityType}, {"scenes", scenes}});
				}
				Json loaded = Json::array();
				std::list<Entity*> activityPresets;
				g_PresetMan.GetAllOfType(activityPresets, "Activity");
				for (Entity* entity : activityPresets) {
					auto* activity = dynamic_cast<GameActivity*>(entity);
					if (!activity || activity->IsTestActivity()) {
						continue;
					}
					Json required = Json::array();
					if (auto* scripted = dynamic_cast<GAScripted*>(activity)) {
						for (const std::string& area : scripted->GetRequiredAreas()) {
							required.push_back(area);
						}
					}
					loaded.push_back({{"preset", activity->GetPresetName()},
						{"module", g_PresetMan.GetDataModuleName(activity->GetModuleID())},
						{"activity_type", activity->GetClassName()},
						{"min_teams", activity->GetMinTeamsRequired()},
						{"required_areas", required}});
				}
				Json loadedScenes = Json::array();
				std::list<Entity*> scenePresets;
				g_PresetMan.GetAllOfType(scenePresets, "Scene");
				for (Entity* entity : scenePresets) {
					auto* scene = dynamic_cast<Scene*>(entity);
					if (!scene) {
						continue;
					}
					Json areas = Json::array();
					for (const Scene::Area* area : scene->GetAreas()) {
						if (area) {
							areas.push_back(area->GetName());
						}
					}
					loadedScenes.push_back({{"name", scene->GetPresetName()},
						{"module", g_PresetMan.GetDataModuleName(scene->GetModuleID())},
						{"areas", areas},
						{"location_zero", scene->GetLocation().IsZero()},
						{"metagame_internal", scene->IsMetagameInternal()},
						{"saved_game_internal", scene->IsSavedGameInternal()},
						{"metascene_parent", scene->GetMetasceneParent()}});
				}
				Json result = {{"schema", 1}, {"screen", screen}, {"settings_page", SettingsPage(manager)}, {"viewport", Rectangle(nullptr)}, {"service", lobby.serviceState},
					{"phase", "after_draw"}, {"sim_frame", g_TimerMan.GetSimUpdateCount()}, {"host", lobby.isHost}, {"peer_id", lobby.localPeerId},
					{"activity_preset", lobby.activityPreset}, {"activity_module", lobby.activityModule},
					{"scene_name", lobby.sceneName}, {"scene_module", lobby.sceneModule},
					{"activity_table", table}, {"game_activities", loaded}, {"loaded_scenes", loadedScenes},
					// The video page hides the preset box when the window's aspect is not the display's, so a
					// reader needs the display the engine itself measured.
					{"display", {{"res_x", g_WindowMan.GetResX()}, {"res_y", g_WindowMan.GetResY()},
						{"max_res_x", g_WindowMan.GetMaxResX()}, {"max_res_y", g_WindowMan.GetMaxResY()},
						{"fullscreen", g_WindowMan.IsFullscreen()}}},
					{"show_metascenes", g_SettingsMan.ShowMetascenes()}, {"controls", Json::array()}};
				// The games the Join screen lists, with the join target each row stands for: the rows show words, not addresses.
				if (const MainMenuGUI* main = g_MenuMan.GetMainMenu()) {
					Json rows = Json::array();
					for (const NetDirectoryClient::GameRow& game : main->AutomationGameRows()) {
						rows.push_back({{"name", game.name}, {"source", game.source}, {"address", game.address}, {"port", game.port}, {"joinable", game.joinable}});
					}
					result["game_rows"] = rows;
				}
				for (auto* item : *manager->GetControlList()) {
					const bool dumpHiddenPreset = item->GetName() == "ComboPresetResolution";
					if (!Visible(item) && !dumpHiddenPreset) continue;
					auto* panel = item->GetPanel();
					auto* parent = dynamic_cast<GUIControl*>(panel->GetParentPanel());
					std::string text;
					Json row = {{"name", item->GetName()}, {"rect", Rectangle(panel)}, {"parent", parent ? parent->GetName() : ""},
						{"parent_rect", Rectangle(panel->GetParentPanel())}, {"text", Text(item, text) ? text : ""}, {"enabled", Enabled(item) || dumpHiddenPreset}, {"visible", Visible(item)}, {"focus", panel->HasFocus()}};
					if (auto* label = dynamic_cast<GUILabel*>(item)) {
						row["overflow_scroll"] = label->GetHorizontalOverflowScroll();
						row["word_width"] = label->GetMaxWordWidth();
						row["row_width"] = row["rect"][2];
					}
					if (item->GetName() == "MatchOptionsBox" || item->GetName() == "LeaveConfirmBox") row["coverage"] = PanelCoverage(item);
					if (auto* list = dynamic_cast<GUIListBox*>(item)) {
						row["items"] = Json::array();
						for (auto* entry: *list->GetItemList()) {
							const std::string shown = list->RegularFittedName(entry->m_Name, entry->m_OffsetX);
							row["items"].push_back({{"text", entry->m_Name}, {"display", shown},
								{"drawn_width", list->GetFont()->CalculateWidth(shown)}, {"name_room", list->RegularItemNameRoom(entry->m_OffsetX)}});
						}
					}
					// The combo's open list is a panel, not a control, so its state rides its owner's row.
					if (auto* combo = dynamic_cast<GUIComboBox*>(item)) {
						row["dropped"] = combo->IsDropped();
						row["item_count"] = combo->GetCount();
						Json items = Json::array();
						GUIListPanel* list = combo->GetListPanel();
						GUIFont* listFont = list ? list->GetFont() : nullptr;
						int longest = 0;
						for (int i = 0; i < combo->GetCount(); ++i) {
							const GUIListPanel::Item* entry = combo->GetItem(i);
							if (!entry) continue;
							const int nameRoom = std::max(1, list ? list->RegularItemNameRoom(entry->m_OffsetX) : combo->GetWidth() - 8);
							const std::string display = list ? list->RegularFittedName(entry->m_Name, entry->m_OffsetX) : entry->m_Name;
							const int rawWidth = listFont ? listFont->CalculateWidth(entry->m_Name) : 0;
							const int drawnWidth = listFont ? listFont->CalculateWidth(display) : 0;
							longest = std::max(longest, rawWidth);
							items.push_back({{"text", entry->m_Name}, {"display", display}, {"text_fits", drawnWidth <= nameRoom},
								{"name_room", nameRoom}, {"drawn_width", drawnWidth}, {"raw_width", rawWidth}});
						}
						row["items"] = items;
						row["selected_index"] = combo->GetSelectedIndex();
						constexpr int namePad = 8;
						constexpr int scrollThickness = 17;
						constexpr int panelPad = 12;
						const int stackHeight = list ? list->GetStackHeight() : 0;
						const int scroll = stackHeight > combo->GetDropHeight() ? scrollThickness : 0;
						const int needed = longest > 0 ? longest + namePad + scroll : combo->GetWidth();
						GUIPanel* parentPanel = panel->GetParentPanel();
						const int valueX = parentPanel ? combo->GetRelXPos() : 0;
						const int clamp = std::max(80, (parentPanel ? parentPanel->GetWidth() : combo->GetWidth()) - valueX - panelPad);
						row["fit_longest"] = longest;
						row["fit_needed"] = needed;
						row["fit_clamp"] = clamp;
						row["fit_width"] = combo->GetWidth();
						// The line the closed box draws, which a fit elides: it must follow every pick.
						row["drawn"] = combo->GetText();
					}
					// Measure every drawn caption here so a layout review reads the whole page, not the named controls.
					std::string measured;
					if (!text.empty()) { row["text_fits"] = TextFits(manager, item, measured); row["text_measure"] = measured; }
					result["controls"].push_back(row);
				}
				observation = result.dump();
				return Writer().Queue(path, std::move(result), g_FrameMan.GetBackBuffer32());
			}
			observation = command == "set_text" ? SetTextObservation(control, name, argument) : name + " " + argument;
			if (!control) { observation += " missing control"; return false; }
			if (command == "assert_visible") {
				observation += " actual=" + std::to_string(Visible(control));
				return (argument == "0" || argument == "1") && Visible(control) == (argument == "1");
			}
			if (command == "assert_focus") {
				observation += " actual=" + std::to_string(Visible(control) && control->GetPanel()->HasFocus());
				return argument.empty() && Visible(control) && control->GetPanel()->HasFocus();
			}
			if (command == "set_text") {
				// A settings box takes a typed value and Enter, the way a player commits one.
				if (!dynamic_cast<GUITextBox*>(control) || argument.empty()) return false;
				std::string typed;
				const bool started = HandType(manager, name, argument, true, typed);
				if (!started) observation += " " + typed;
				return started;
			}
			if (command == "assert_text_fits") return argument.empty() && TextFits(manager, control, observation);
			if (command == "assert_rect_inside") {
				auto* parent = argument == "parent" ? control->GetPanel()->GetParentPanel() : manager->GetControl(argument) ? manager->GetControl(argument)->GetPanel() : nullptr;
				observation += " rect=" + Json(Rectangle(control->GetPanel())).dump() + " bounds=" + Json(Rectangle(parent)).dump();
				return (parent || argument == "viewport") && Inside(Rectangle(control->GetPanel()), Rectangle(parent));
			}
			return false;
		} catch (const std::exception& error) { observation = error.what(); return false; }
	}

	int ParseToastBandExpectedRows(std::istream& args) {
		int expectedRows = -1;
		if (!(args >> expectedRows)) {
			expectedRows = -1;
		}
		return expectedRows;
	}

	bool FireAssertAllowed() {
		return FrameRecorder::Instance().Enabled() || SDL_getenv("CCCP_HEADLESS") != nullptr;
	}

	bool RunSelfTest() {
		bool passed = true;
		const auto check = [&passed](const char* label, bool value, const std::string& detail) {
			passed = passed && value;
			std::cout << "[menu-automation-selftest] " << (value ? "PASS" : "FAIL") << " " << label << " " << detail << std::endl;
		};
		{
			std::istringstream missing("");
			const int rows = ParseToastBandExpectedRows(missing);
			check("missing_toast_band_arg_is_minus_one", rows == -1, "expectedRows=" + std::to_string(rows));
		}
		{
			std::istringstream present("3");
			const int rows = ParseToastBandExpectedRows(present);
			check("toast_band_arg_is_n", rows == 3, "expectedRows=" + std::to_string(rows));
		}
		{
			const char* runner = SDL_getenv("CCCP_HEADLESS");
			const bool runnerHad = runner != nullptr;
			const std::string runnerValue = runnerHad ? runner : "";
			SDL_unsetenv_unsafe("CCCP_HEADLESS");
			const bool refused = !FireAssertAllowed();
			check("fire_assert_refused_when_headed", refused, refused ? "refused" : "allowed");
			check("headless_unset_stays_unset", SDL_getenv("CCCP_HEADLESS") == nullptr,
			      SDL_getenv("CCCP_HEADLESS") ? "set" : "unset");
			if (runnerHad) {
				SDL_setenv_unsafe("CCCP_HEADLESS", runnerValue.c_str(), 1);
			}
		}
		{
			// A panel its manager skipped on the latest pass is off the screen, however recent the pass before was.
			SetPanelDrawRecording(false);
			int unrecorded = 0;
			RecordPanelDraw(&unrecorded);
			check("draw_record_off_records_nothing", PanelDrawAgeMs(&unrecorded) < 0, "age_ms=" + std::to_string(PanelDrawAgeMs(&unrecorded)));
			SetPanelDrawRecording(true);
			int manager = 0, shown = 0, replaced = 0, loose = 0;
			const void* previous = BeginPanelDrawPass(&manager);
			RecordPanelDraw(&shown);
			RecordPanelDraw(&replaced);
			EndPanelDrawPass(previous);
			previous = BeginPanelDrawPass(&manager);
			RecordPanelDraw(&shown);
			EndPanelDrawPass(previous);
			RecordPanelDraw(&loose);
			const std::string age = "replaced_age_ms=" + std::to_string(PanelDrawAgeMs(&replaced));
			check("draw_record_latest_pass_drawn", PanelDrawnInLatestPass(&shown, c_DrawWindowSeconds), age);
			check("draw_record_skipped_panel_not_drawn", !PanelDrawnInLatestPass(&replaced, c_DrawWindowSeconds), age);
			check("draw_record_outside_pass_uses_window", PanelDrawnInLatestPass(&loose, c_DrawWindowSeconds), age);
			ClearPanelDrawRecord();
			check("draw_record_cleared", !PanelDrawnInLatestPass(&shown, c_DrawWindowSeconds) && PanelDrawAgeMs(&shown) < 0, age);
		}
		{
			// The option pages' hints say what a late player, a checkpoint and a route choice do, the host included.
			const double tickMs = 1000.0 / 60.0;
			const auto same = [&check](const char* label, const std::string& actual, const std::string& expected) {
				check(label, actual == expected, "\"" + actual + "\"");
			};
			same("hint_policy_substitute", NetSlowPlayerPolicyText(NetSlowPlayerPolicy::Substitute), "Give the seat to the AI (host too) until they catch up");
			same("hint_policy_pause", NetSlowPlayerPolicyText(NetSlowPlayerPolicy::Pause), "Pause for them (up to 20 s)");
			same("hint_bound_substitute", NetSlowPlayerHint(NetSlowPlayerPolicy::Substitute, 3, tickMs),
			     "A player late past 3 ticks (50 ms), host too, is held to the AI while others play on. Other policies return in a later version.");
			same("hint_bound_one_tick", NetSlowPlayerHint(NetSlowPlayerPolicy::Substitute, 1, tickMs),
			     "A player late past 1 tick (17 ms), host too, is held to the AI while others play on. Other policies return in a later version.");
			same("hint_bound_pause", NetSlowPlayerHint(NetSlowPlayerPolicy::Pause, 3, tickMs), "Everyone waits for a late player, host included, for up to 20 s.");
			same("hint_auto_delay", NetAutoDelayText(3), "ping plus a 3-tick margin, raised live if inputs arrive late");
			same("hint_autosave_range", NetAutosaveRangeHint(), "Every 60 s to 60 min, or off (default)");
			same("hint_autosave_note", NetAutosaveNote(), "Every player takes each checkpoint at the same tick; a player who rejoins starts from one.");
			same("hint_autosave_cost", NetAutosaveCostHint(), "Saving may cause a brief pause for other players on slower hosts");
			same("hint_connection_automatic", NetConnectionModeHint(SettingsMan::NetworkConnectionMode::Automatic), "Direct first: lowest latency; relay adds a round trip if direct fails.");
			same("hint_connection_direct", NetConnectionModeHint(SettingsMan::NetworkConnectionMode::DirectOnly), "Direct only: lowest latency; fails when routers block a direct route.");
			same("hint_connection_relay", NetConnectionModeHint(SettingsMan::NetworkConnectionMode::RelayOnly), "Relay only: every packet uses the relay and adds its round trip.");
			NetMatchConfig config;
			config.delayPolicy = NetMatchDelayPolicy::Auto;
			config.slowPlayerPolicy = NetSlowPlayerPolicy::Substitute;
			config.slowPlayerBoundTicks = 3;
			NetLobbySnapshot snapshot;
			snapshot.inputDelayText = "Input delay: 4 (auto, 50ms ping)";
			const std::string summary = NetHostOptionsSummary(config, snapshot);
			check("hint_summary_policy", summary.find("\nWhen a player falls behind: Give the seat to the AI (host too) until they catch up\n") != std::string::npos, summary);
			check("hint_summary_delay", summary.find("\nInput delay: Automatic, ping plus a 3-tick margin, raised live if inputs arrive late - now 4 (auto, 50ms ping)\n") != std::string::npos, summary);
			check("hint_summary_no_rejoin", summary.find("rejoin") == std::string::npos, summary);
			check("summary_short_rows_share_lines", summary.find(" ticks   Slow player bound: 3 ticks\n") != std::string::npos && summary.find("\nDifficulty: ") != std::string::npos &&
			      summary.find("   Starting gold: ") != std::string::npos, summary);
			same("transfer_line_measured", NetImageTransferLine(3250585, 8598323, 1468006.0), "Receiving the world: 3.1 of 8.2 MB - 1.4 MB/s - 4 s left");
			same("transfer_line_before_a_rate", NetImageTransferLine(0, 8598323, 0.0), "Receiving the world: 0.0 of 8.2 MB");
			same("transfer_line_complete", NetImageTransferLine(8598323, 8598323, 1468006.0), "Receiving the world: 8.2 of 8.2 MB");
			same("catch_up_line_to_an_activation", NetCatchUpLine(1200, 2400, 300.0, 0.0), "Catching up with the world: frame 1200 of 2400 - 300 frames/s - 4 s left");
			same("catch_up_line_to_a_moving_round", NetCatchUpLine(1200, 2400, 300.0, 60.0), "Catching up with the world: frame 1200 of 2400 - 300 frames/s - 5 s left");
			same("catch_up_line_before_a_rate", NetCatchUpLine(1200, 2400, 0.0, 60.0), "Catching up with the world: frame 1200 of 2400");
			same("seat_join_receiving", NetSeatJoinProgress(3250585, 8598323, false), "receiving the world 3.1 of 8.2 MB");
			same("seat_join_catching_up", NetSeatJoinProgress(0, 8598323, true), "catching up");
			same("changing_host_line", c_NetMatchChangingHostLine, "The match is changing host - try again in a moment");
			// The repair toast names why and whose game, never the repair screen's own line.
			const std::string diverged = "tick 243 lockstep stopped: Desync:sim state diverged at tick 240 (Client)";
			same("repair_toast_names_the_player", NetRepairStartLine(diverged, "Host", "Host", true), "Match repair started: Client fell out of step at frame 240");
			same("repair_toast_on_the_named_screen", NetRepairStartLine(diverged, "Client", "Host", false), "Match repair started: your game fell out of step at frame 240");
			same("repair_toast_naming_the_host", NetRepairStartLine("Desync:sim state diverged at tick 60 (Host)", "Ana", "Host", false), "Match repair started: out of step with the host at frame 60");
			same("repair_toast_on_the_host_it_names", NetRepairStartLine("Desync:sim state diverged at tick 60 (Host)", "Host", "Host", true), "Match repair started: a player fell out of step at frame 60");
			same("repair_toast_host_request", NetRepairStartLine("ResyncRequested:host requested repair", "Ana", "Host", false), "Match repair started by the host");
			same("repair_toast_own_request", NetRepairStartLine("ResyncRequested:host requested repair", "Host", "Host", true), "Match repair started by you");
			same("repair_toast_rejoin", NetRepairStartLine("tick 900 lockstep stopped: ResyncRequested:player rejoined", "Ana", "Host", false), "Match repair started: a player rejoined");
			same("repair_toast_unnamed", NetRepairStartLine("Desync", "Ana", "Host", false), "Match repair started");
			check("repair_toast_never_the_screen_line", NetRepairStartLine(diverged, "Host", "Host", true).find("Resyncing") == std::string::npos, "");
		}
		{
			// A state line shown twice is caught wherever the two copies are; what a screen repeats by design is not.
			const auto line = [](const char* source, const char* control, const char* text, bool caption = false) { return ShownLine{source, control, text, caption}; };
			const Json acrossMenu = DuplicateLine({line("menu", "LobbyStatusLabel", "leaver: Held - AI in control"), line("overlay", "NetworkStatus", "leaver: Held - AI in control")});
			check("duplicates_menu_and_overlay", !acrossMenu.is_null(), acrossMenu.dump());
			const Json withinOne = DuplicateLine({line("network", "NetworkRoster", "leaver: Held - AI in control"), line("network", "NetworkRoster", "leaver: Held - AI in control")});
			check("duplicates_within_one_control", !withinOne.is_null(), withinOne.dump());
			const Json byDesign = DuplicateLine({line("network", "NetworkSeatDetail0", "Connected"), line("network", "NetworkSeatDetail1", "Connected"),
			                                     line("network", "NetworkRoster", "Open seat"), line("network", "NetworkRoster", "Open seat"),
			                                     line("network", "NetworkSeatKick0", "Kick", true), line("network", "NetworkSeatKick1", "Kick", true),
			                                     line("menu", "LabelMatchChat0", "gg"), line("menu", "LabelMatchChat1", "gg"), line("overlay", "NetworkPing", "--"), line("overlay", "NetworkLoss", "--")});
			check("duplicates_by_design_pass", byDesign.is_null(), byDesign.dump());
			const Json closingDots = DuplicateLine({line("overlay", "LabelNetMatchToast0", "Held - AI in control - rejoining"), line("drawn", "RejoinOverlay", "Held - AI in control - rejoining...")});
			check("duplicates_closing_dots_are_one_line", !closingDots.is_null(), closingDots.dump());
			// The settings pages' value columns, laid out as the game lays them out: the audio channels in one box, the players' boxes alike.
			const int boxes[5] = {};
			const void* const audioBox = &boxes[0];
			const void* const boxP3 = &boxes[1];
			const void* const boxP4 = &boxes[2];
			const void* const filesBox = &boxes[3];
			const void* const lobbyBox = &boxes[4];
			const auto placed = [](const char* source, const char* control, const char* text, Rect rect, Rect parentRect, const void* parent) {
				ShownLine shown{source, control, text};
				shown.label = true;
				shown.rect = rect;
				shown.parentRect = parentRect;
				shown.parent = parent;
				return shown;
			};
			const Rect audio{100, 80, 480, 230}, p3{110, 216, 470, 47}, p4{110, 269, 470, 47}, files{100, 136, 480, 210};
			const Json valueColumns = DuplicateLine({placed("menu", "LabelMasterVolume", "Volume: 0", {150, 130, 80, 20}, audio, audioBox),
			                                         placed("menu", "LabelMusicVolume", "Volume: 0", {150, 180, 80, 20}, audio, audioBox),
			                                         placed("menu", "LabelSoundVolume", "Volume: 0", {150, 230, 80, 20}, audio, audioBox),
			                                         placed("menu", "LabelP3Sensitivity", "Stick Deadzone: 1", {120, 241, 120, 20}, p3, boxP3),
			                                         placed("menu", "LabelP4Sensitivity", "Stick Deadzone: 1", {120, 294, 120, 20}, p4, boxP4)});
			check("duplicates_value_columns_pass", valueColumns.is_null(), valueColumns.dump());
			const Json twoNotes = DuplicateLine({placed("menu", "LabelNetAutosaveHost", "Set by the host", {400, 148, 165, 20}, files, filesBox),
			                                     placed("menu", "LabelNetAutosaveIntervalHost", "Set by the host", {400, 168, 165, 20}, files, filesBox)});
			check("duplicates_note_repeated_down_rows", !twoNotes.is_null(), twoNotes.dump());
			const Json offColumn = DuplicateLine({placed("menu", "LabelLobbyStatus", "Match ended", {100, 100, 200, 20}, audio, lobbyBox),
			                                      placed("menu", "LabelMatchStatus", "Match ended", {140, 140, 200, 20}, audio, lobbyBox)});
			check("duplicates_off_one_column", !offColumn.is_null(), offColumn.dump());
			const Json acrossSources = DuplicateLine({placed("menu", "LabelMultiplayerStatus", "Match ended", {100, 100, 200, 20}, audio, lobbyBox),
			                                          placed("overlay", "LabelNetMatchStatus", "Match ended", {100, 140, 200, 20}, audio, lobbyBox)});
			check("duplicates_one_column_never_spans_two_surfaces", !acrossSources.is_null(), acrossSources.dump());
		}
		{
			// A relay login's boxes capture as their mask and their set_text record is masked, on the host's page and the player's; each
			// box keeps what was typed. The boxes need the timer manager and have no control manager, so a bare panel takes their events.
			if (!TimerMan::IsConstructed()) TimerMan::Construct();
			GUIPanel signals;
			std::vector<std::unique_ptr<GUITextBox>> boxes;
			const auto box = [&boxes, &signals](const char* name, const char* value, bool masked) {
				auto& made = boxes.emplace_back(std::make_unique<GUITextBox>(nullptr, nullptr));
				made->GUIControl::Create(name, 0, 0, 120, 16);
				made->SetSignalTarget(&signals);
				made->SetPasswordMask(masked);
				made->SetText(value);
				return made.get();
			};
			const std::string value = "synthetic-login-3f9a";
			GUITextBox* plain = box("TextHostRelayAddress", "relay.example.test:3478", false);
			for (GUITextBox* credential: {box("TextHostRelayUser", value.c_str(), false), box("TextHostRelayPass", value.c_str(), true),
			                              box("TextNetworkRelayUser", value.c_str(), false), box("TextNetworkRelayPass", value.c_str(), true)}) {
				const std::string name = credential->GetName();
				std::string captured, drawn;
				const bool read = Text(credential, captured) && DisplayText(credential, drawn);
				check(("credential_capture_masked " + name).c_str(), read && captured == std::string(value.size(), '*'), "captured=" + captured);
				const std::string record = SetTextObservation(credential, name, "synthetic-typed-5519");
				check(("credential_set_text_record_masked " + name).c_str(), record == name + " <masked>", "record=" + record);
				const bool drawnRight = drawn == (credential->HasPasswordMask() ? std::string(value.size(), '*') : value);
				check(("credential_box_keeps_its_value " + name).c_str(), credential->GetText() == value && drawnRight, drawnRight ? "drawn as before" : "drawn differently");
			}
			std::string plainText;
			const bool plainRead = Text(plain, plainText);
			check("plain_box_captured_as_shown", plainRead && plainText == "relay.example.test:3478", "captured=" + plainText);
			const std::string plainRecord = SetTextObservation(plain, "TextHostRelayAddress", "relay.example.test:3478");
			check("plain_set_text_record_kept", plainRecord == "TextHostRelayAddress relay.example.test:3478", "record=" + plainRecord);
		}
		std::cout << "[menu-automation-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
		return passed;
	}
}
