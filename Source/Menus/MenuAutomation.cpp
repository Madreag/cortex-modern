#include "MenuAutomation.h"
#include "SettingsGUI.h"
#include "MenuMan.h"
#include "MainMenuGUI.h"
#include "NetModerationGUI.h"
#include "NetPlayerPresentation.h"

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
#include "GUITab.h"
#include "GUITextBox.h"
#include "FrameMan.h"
#include "FrameRecorder.h"
#include "GAScripted.h"
#include "GameActivity.h"
#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
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

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <mutex>
#include <sstream>
#include <string_view>
#include <tuple>
#include <thread>
#include <unordered_map>
#include <vector>

namespace RTE::MenuAutomation {
	using Json = nlohmann::json;
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
	// draw if its parents did. A menu frame is 1/60 s, so a quarter second covers the last one.
	constexpr double c_DrawWindowSeconds = 0.25;
	GUIControl* FirstDrawn(GUIControl* control) {
		if (!control || !control->GetPanel()) return nullptr;
		if (PanelDrewRecently(control->GetPanel(), c_DrawWindowSeconds)) return control;
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
	constexpr std::array<std::string_view, 6> c_NetworkPages{"Player", "Chat", "Recovery", "Files", "Internet", "Connection"};

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

	// A manager's Update clears its event queue, so a page request waits for the settings menu's own pass.
	static std::unordered_map<GUIControlManager*, SettingsGUI*> s_SettingsOwners;

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

	void BindSettingsOwner(GUIControlManager* manager, SettingsGUI* owner) {
		if (manager && owner) s_SettingsOwners[manager] = owner;
	}

	void UnbindSettingsOwner(GUIControlManager* manager) {
		s_SettingsOwners.erase(manager);
	}

	bool QueuePage(GUIControlManager* manager, const std::string& page) {
		auto owner = s_SettingsOwners.find(manager);
		if (owner == s_SettingsOwners.end() || !Enabled(PageTab(manager, page))) return false;
		owner->second->QueuePendingPage(page);
		return true;
	}

	void ApplyQueuedPage(GUIControlManager* manager) {
		auto owner = s_SettingsOwners.find(manager);
		if (!manager || owner == s_SettingsOwners.end() || owner->second->PendingPage().empty()) return;
		GUITab* tab = PageTab(manager, owner->second->PendingPage());
		owner->second->ClearPendingPage();
		if (!Enabled(tab)) return;
		// The settings menu switches pages on the notification a tab click raises, so raise that.
		tab->SetCheck(true);
		tab->AddEvent(GUIEvent::Notification, GUITab::UnPushed, 0);
	}
	bool Text(GUIControl* control, std::string& text) {
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
	void Click(GUIControlManager* manager, const std::string& name) {
		auto* control = manager->GetControl(name);
		if (!Enabled(control)) return;
		auto* panel = control->GetPanel();
		const auto r = Rectangle(panel);
		panel->OnMouseDown(r[0] + r[2] / 2, r[1] + r[3] / 2, GUIPanel::MOUSE_LEFT, 0);
		panel->OnMouseUp(r[0] + r[2] / 2, r[1] + r[3] / 2, GUIPanel::MOUSE_LEFT, 0);
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
		if ((!Visible(control) && !measureHiddenPreset) || !Text(control, text)) return false;
		// A closed picker is measured on the line it draws, which is not always the whole item name.
		if (auto* combo = dynamic_cast<GUIComboBox*>(control)) text = combo->GetText();
		const auto rect = Rectangle(control->GetPanel());
		observation += " text=" + Json(text).dump() + " rect=" + Json(rect).dump();
		if (auto* label = dynamic_cast<GUILabel*>(control)) {
			observation += " height=" + std::to_string(label->GetTextHeight()) + " word_width=" + std::to_string(label->GetMaxWordWidth());
			if (label->GetHorizontalOverflowScroll() && control->GetName() == "LabelMultiplayerStatus") {
				return label->GetTextHeight() <= rect[3];
			}
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
	bool Handles(const std::string& command) {
		return command == "assert_visible" || command == "assert_focus" || command == "assert_rect_inside" || command == "assert_inside_screen" || command == "assert_text_fits" || command == "assert_no_overlap" ||
			command == "dump_refresh_count" || command == "dump_enter_state" ||
			command == "dump_host_options" || command == "dump_player_options" || command == "focus_next" || command == "focus_previous" || command == "key" || command == "pad" ||
			command == "key_down" || command == "key_up" || command == "focus" ||
			command == "set_text" || command == "set_share_address" || command == "combo_drop" || command == "combo_select" ||
			command == "select_settings_page" || command == "assert_settings_page" || command == "video_mark" ||
			command == "assert_label" || command == "assert_checked" || command == "assert_vertical_scroll" ||
			command == "assert_opaque_panel" || command == "dump_network_layout" || command == "dump_match_identity" ||
			command == "assert_not_drawn" || command == "assert_toast_band" || command == "assert_word_wrap" || command == "ghost_watch" || command == "assert_list_rows" ||
			command == "assert_net_label" || command == "assert_net_label_absent" || command == "push_toast" || command == "dump_seat_state" || command == "fire_assert";
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
		if (command == "dump_seat_state") {
			const auto snapshot = g_NetMatchService.GetLobbySnapshot();
			Json members = Json::array();
			for (const auto& member: snapshot.members) members.push_back({{"peer", member.peerId}, {"name", NetPlayerPresentation::Name(member)}, {"state", NetPlayerPresentation::State(member)}, {"route", member.connectedRoute}});
			observation = Json{{"members", members}, {"private_catch_up", ScenarioRunner::WorldCatchUpActive()},
				{"resyncing", g_NetMatchService.IsMatchResyncing()}, {"slow_notice", ScenarioRunner::IsLockstepLocalMachineSlow()}}.dump();
			return true;
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
			const bool found = panel && panel->AutomationLabelText(name, text);
			const bool carries = found && text.find(expected) != std::string::npos;
			observation = name + " \"" + expected + "\" text=" + Json(text).dump();
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
				panel->DisarmGhostWatch();
				observation = Json{{"hits", hits},
					{"last", last.found ? Json{{"x", last.x}, {"y", last.y}, {"run", last.run}} : Json(nullptr)}}.dump();
				return hits == 0;
			}
			observation = "unknown ghost_watch mode";
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
				source = rest.substr(rest.find_first_not_of(' '));
				int boxWidth = 0;
				if (source.empty() || !panel->AutomationWrapLines(source, wrapped, boxWidth)) {
					observation = "probe text missing or wrap unavailable";
					return false;
				}
				textWidth = boxWidth - 12;
				capWidth = std::max(0, g_WindowMan.GetResX() - 28);
			} else {
				const auto& audit = surface == "status" ? panel->GetStatusWrap() : panel->GetRosterWrap();
				if (!audit.active) { observation = surface + " wrap surface not active"; return false; }
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
			std::istringstream tokens(source);
			std::string splitToken;
			for (std::string token; tokens >> token;) {
				const bool whole = std::any_of(rows.begin(), rows.end(), [&](const std::string& row) {
					return row.find(token) != std::string::npos; });
				if (!whole) { splitToken = token; break; }
			}
			int longestWord = 0;
			tokens.clear();
			tokens.str(source);
			for (std::string token; tokens >> token;) longestWord = std::max(longestWord, font->CalculateWidth(token));
			int widestLine = 0;
			for (const auto& row: rows) widestLine = std::max(widestLine, font->CalculateWidth(row));
			const bool capped = textWidth >= capWidth;
			const bool sized = longestWord <= textWidth || capped;
			observation = Json{{"surface", surface}, {"text_width", textWidth}, {"longest_word", longestWord},
				{"widest_line", widestLine}, {"rows", rows}, {"split_token", splitToken},
				{"capped", capped}, {"source", source}}.dump();
			return splitToken.empty() && sized;
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
				auto* box = dynamic_cast<GUICheckbox*>(manager->GetControl(name));
				const int actual = box && box->GetCheck() == GUICheckbox::Checked ? 1 : 0;
				observation = name + " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual);
				return box && Visible(box) && (expected == 0 || expected == 1) && actual == expected;
			}
			if (command == "assert_label") {
				std::string name, expected, text;
				args >> name;
				std::getline(args >> std::ws, expected);
				bool found = Text(manager->GetControl(name), text);
				MainMenuGUI* main = g_MenuMan.GetMainMenu();
				if (!found && main && manager == main->AutomationManager()) found = main->AutomationLabelText(name, text);
				observation = name + " \"" + expected + "\" text=\"" + text + "\"";
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
				auto* control = manager->GetControl(target);
				if (!control || !Enabled(control) || !control->GetPanel()) {
					observation = target + " missing or disabled";
					return false;
				}
				manager->GetManager()->SetFocus(control->GetPanel());
				const auto r = Rectangle(control->GetPanel());
				g_UInputMan.SetAbsoluteMousePosition(Vector(r[0] + r[2] / 2, r[1] + r[3] / 2) * g_WindowMan.GetResMultiplier());
				observation = target;
				return true;
			}
			if (command == "combo_drop" || command == "combo_select") {
				std::string comboName;
				args >> std::quoted(comboName);
				std::string item;
				std::getline(args >> std::ws, item);
				auto* combo = dynamic_cast<GUIComboBox*>(manager->GetControl(comboName));
				if (!combo || !Enabled(combo)) { observation = comboName + " missing or disabled"; return false; }
				observation = comboName + (item.empty() ? "" : " " + item);
				if (command == "combo_drop") {
					// The same state the text-panel click produces: the list shows, takes focus and
					// the mouse, sits topmost, and the control notifies Dropped.
					auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
					return input && input->QueueAutomationCommand([combo] {
						GUIListPanel* list = combo->GetListPanel();
						list->_SetVisible(true);
						list->SetFocus();
						list->CaptureMouse();
						list->EndUpdate();
						list->ChangeZPosition(GUIPanel::TopMost);
						combo->AddEvent(GUIEvent::Notification, GUIComboBox::Dropped, 0);
					});
				}
				if (item.empty()) { observation += " no item"; return false; }
				int index = -1;
				for (int i = 0; i < combo->GetCount(); ++i) {
					if (const GUIListPanel::Item* entry = combo->GetItem(i); entry && entry->m_Name == item) { index = i; break; }
				}
				if (index < 0) { observation += " no such item"; return false; }
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				GUIManager* gui = manager->GetManager();
				return input && input->QueueAutomationCommand([combo, index, gui] {
					GUIListPanel* list = combo->GetListPanel();
					// A scripted pick takes the row-click's close path so listeners see the same event.
					list->_SetVisible(false);
					list->ReleaseMouse();
					gui->SetFocus(nullptr);
					combo->SetSelectedIndex(index);
					combo->AddEvent(GUIEvent::Notification, GUIComboBox::Closed, 0);
				});
			}
			std::string name, argument, extra;
			args >> std::quoted(name) >> argument >> extra;
			if (!extra.empty()) { observation = "unexpected arguments"; return false; }
			auto* control = manager->GetControl(name);
			if (command == "set_share_address") {
				if (name.empty() || !argument.empty()) { observation = "need one address"; return false; }
				if (auto* menu = g_MenuMan.GetMainMenu()) {
					menu->AutomationSetShareAddress(name);
					observation = name;
					return true;
				}
				observation = "no main menu";
				return false;
			}
			if (command == "key" || command == "pad") {
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				observation = name + " " + argument;
				return input && (argument == "down" || argument == "up") && input->QueueAutomationInput(command, name, argument == "down");
			}
			if (command == "focus_next" || command == "focus_previous") {
				if (!name.empty()) return false;
				std::vector<GUIControl*> controls;
				for (auto* item : *manager->GetControlList()) if (Enabled(item) && !item->IsContainer() && !dynamic_cast<GUILabel*>(item)) controls.push_back(item);
				std::stable_sort(controls.begin(), controls.end(), [](auto* a, auto* b) {
					const auto x = Rectangle(a->GetPanel()), y = Rectangle(b->GetPanel());
					return std::tie(x[1], x[0]) < std::tie(y[1], y[0]);
				});
				if (controls.empty()) return false;
				const auto current = std::find_if(controls.begin(), controls.end(), [](auto* item) { return item->GetPanel()->HasFocus(); });
				const int count = static_cast<int>(controls.size()), index = static_cast<int>(current - controls.begin());
				const int next = current == controls.end() ? (command == "focus_next" ? 0 : count - 1) : (index + (command == "focus_next" ? 1 : count - 1)) % count;
				auto* target = controls[next];
				manager->GetManager()->SetFocus(target->GetPanel());
				const auto r = Rectangle(target->GetPanel());
				g_UInputMan.SetAbsoluteMousePosition(Vector(r[0] + r[2] / 2, r[1] + r[3] / 2) * g_WindowMan.GetResMultiplier());
				observation = target->GetName();
				return true;
			}
			if (command == "select_settings_page" || command == "assert_settings_page") {
				const std::string active = SettingsPage(manager);
				observation = name + " active=" + active;
				if (!argument.empty()) return false;
				// "Network" asserts wherever its selector sits; "Network:Chat" asserts one page.
				if (command == "assert_settings_page") {
					return active == name || (active.size() > name.size() && active.compare(0, name.size(), name) == 0 && active[name.size()] == ':');
				}
				return QueuePage(manager, name);
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
			observation = name + " " + argument;
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
				// The typed-entry seam for a settings page: the box takes the value and raises the notification a typed entry raises.
				auto* box = dynamic_cast<GUITextBox*>(control);
				if (!box || !Enabled(box) || argument.empty()) return false;
				if (box->HasPasswordMask()) observation = name + " <masked>";
				box->SetText(argument);
				// The event queue clears at the top of every Update, so the Enter has to be raised
				// inside one - the same channel a scripted click takes.
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				return input && input->QueueAutomationCommand([box] { box->AddEvent(GUIEvent::Notification, GUITextBox::Enter, 0); });
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
		std::cout << "[menu-automation-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
		return passed;
	}
}
