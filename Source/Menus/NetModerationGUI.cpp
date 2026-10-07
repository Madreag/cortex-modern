#include "NetModerationGUI.h"

#include "ActivityMan.h"
#include "Constants.h"
#include "CameraMan.h"
#include "GameActivity.h"
#include "MainMenuGUI.h"
#include "MenuMan.h"
#include "PauseMenuGUI.h"
#include "NetMatchService.h"
#include "NetSession.h"
#include "NetHostOptionsText.h"
#include "NetPlayerPresentation.h"
#include "NetChatPresentation.h"
#include "GUISound.h"
#include "ScenarioRunner.h"
#include "SettingsMan.h"
#include "System.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "FrameRecorder.h"
#include "UInputMan.h"
#include "GUI.h"
#include "GUIDrawRecord.h"
#include <chrono>
#include <array>
#include "GUIEvent.h"
#include "GUIManager.h"
#include "GUIInputWrapper.h"
#include "GUIControlManager.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUILabel.h"
#include "GUIListBox.h"
#include "GUITextBox.h"
#include "GUISkin.h"
#include "NetProtocol.h"
#include "InputScript.h"
#include "ConsoleMan.h"
#include <SDL3/SDL.h>
#include "AllegroScreen.h"
#include "AllegroBitmap.h"
#include "RTEError.h"
#include "TimerMan.h"
#include "RTETools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <map>
#include <set>
#include <vector>

using namespace RTE;

namespace {
	/// Screens shorter than this get the single-line strip instead of the box.
	constexpr int c_CompactMaxHeight = 480;
	/// The status box on a tall screen, and the rows the compact strip owns at the top of a short one.
	constexpr int c_StatusBoxTop = 32, c_StatusBoxHeight = 76, c_StatusBoxWidth = 252, c_StatusBoxMargin = 8;
	constexpr int c_StripBandBottom = 20;
	/// The seats panel, and the gap it keeps from the screen edges.
	constexpr int c_PanelWidth = 600, c_PanelHeight = 344, c_PanelGap = 4;
	/// The rows the panel may give up: one more puts the close row's rel-Y below 0.
	constexpr int c_PanelMaxLost = 318;

	/// The panel's top row: centred, but under the status widget's band on a screen with the rows for both.
	int PanelTop(int screenHeight, int wantedHeight = c_PanelHeight) {
		const int band = (screenHeight < c_CompactMaxHeight ? c_StripBandBottom : c_StatusBoxTop + c_StatusBoxHeight) + c_PanelGap;
		const int lowest = std::max(0, screenHeight - wantedHeight - c_PanelGap);
		return std::clamp((screenHeight - wantedHeight) / 2, std::min(band, lowest), lowest);
	}

	/// What the overlay may use while the synchronized setup editor holds the world. The editor owns the top
	/// band (team icon, funds, the seat's own message) and the column its picker slides into, which every
	/// editor reports as the seat's screen occlusion; the picker is 360 px of a window never under 640.
	struct EditorArea {
		bool editing = false;
		/// One local seat's picker column in window space. A split screen parks each seat's framebuffer at
		/// its own offset, so every local seat contributes the column its picker reports, translated here.
		struct Column { int x = 0, y = 0, w = 0, h = 0; };
		std::vector<Column> columns;
		/// The seat's own message band in the same window space: the overlay keeps above or below it,
		/// never across it.
		std::vector<Column> textBands;

		/// Extra window-space occupiers (the status widget) that carve the toast span the same way columns do.
		std::vector<Column> occupiers;

		/// The widest column-free gap in [0, screenWidth) across every occupier that crosses the band.
		void FreeSpan(int bandTop, int bandBottom, int screenWidth, int& left, int& right) const {
			std::vector<std::pair<int, int>> occupied;
			auto consider = [&](const Column& column) {
				if (column.w <= 0 || column.h <= 0) return;
				if (column.y >= bandBottom || column.y + column.h <= bandTop) return;
				const int x0 = std::max(0, column.x);
				const int x1 = std::min(screenWidth, column.x + column.w);
				if (x1 > x0) occupied.push_back({x0, x1});
			};
			for (const Column& column: columns) consider(column);
			for (const Column& column: occupiers) consider(column);
			std::sort(occupied.begin(), occupied.end());
			std::vector<std::pair<int, int>> merged;
			for (const auto& span: occupied) {
				if (merged.empty() || span.first > merged.back().second) merged.push_back(span);
				else merged.back().second = std::max(merged.back().second, span.second);
			}
			int bestLeft = 0, bestRight = 0, bestWidth = -1;
			int cursor = 0;
			auto keep = [&](int gapLeft, int gapRight) {
				const int width = gapRight - gapLeft;
				if (width > bestWidth) {
					bestWidth = width;
					bestLeft = gapLeft;
					bestRight = gapRight;
				}
			};
			for (const auto& span: merged) {
				if (span.first > cursor) keep(cursor, span.first);
				cursor = std::max(cursor, span.second);
			}
			if (cursor < screenWidth) keep(cursor, screenWidth);
			if (bestWidth < 0) {
				left = 0;
				right = 0;
			} else {
				left = bestLeft;
				right = bestRight;
			}
		}
	};

	/// A seat-space rect translated into the window, the same offset the picker column already uses.
	EditorArea::Column SeatWindowRect(int screen, int x, int y, int width, int height) {
		Vector offset;
		g_FrameMan.GetScreenOffsetForSplitScreen(screen, offset);
		return {x + offset.GetRoundIntX(), y + offset.GetRoundIntY(), width, height};
	}

	/// The column the stock picker settles into (Base.rte/GUIs/ObjectPickerGUI.ini [PickerGUIBox] Width),
	/// reserved whole from the first frame of its slide so the overlay holds one place while it animates.
	constexpr int c_EditorPanelWidth = 360;

	EditorArea FreeArea(int screenWidth) {
		EditorArea area;
		const auto* game = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
		if (!game) return area;
		area.editing = game->GetActivityState() == Activity::ActivityState::Editing;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (!(game->IsSeatActive(player) && game->IsLocalHumanSeat(player))) continue;
			const int screen = game->ScreenOfPlayer(player);
			const FrameMan::ScreenTextLayout text = g_FrameMan.GetScreenTextLayout(screen, true);
			if (text.height > 0) {
				area.textBands.push_back(SeatWindowRect(screen, text.x, text.y, text.width, text.height));
			}
			const int occlusion = g_CameraMan.GetScreenOcclusion(screen).GetRoundIntX();
			if (occlusion == 0) continue;
			// The picker measured itself against the seat's own framebuffer, which a split screen parks
			// at a window offset - the column it owns is translated, not read off the window's width.
			const int columnW = std::max(c_EditorPanelWidth, occlusion < 0 ? -occlusion : occlusion);
			const int frameWidth = g_FrameMan.GetPlayerFrameBufferWidth(player);
			const int columnX = occlusion < 0 ? frameWidth - columnW : 0;
			area.columns.push_back(SeatWindowRect(screen, columnX, 0, columnW, g_FrameMan.GetPlayerFrameBufferHeight(player)));
		}
		return area;
	}

	/// The chat band's row height for the text size the settings ask for.
	int ChatLineHeight(bool large) {
		GUIFont* font = large ? g_FrameMan.GetLargeFont(true) : g_FrameMan.GetSmallFont(true);
		if (!font) font = g_FrameMan.GetSmallFont(true);
		return std::max(12, font ? font->GetFontHeight() : 12) + 4;
	}

	/// The first row the chat may use: under every message band that sits in the top half, which the band
	/// keeps clear of rather than draw across, and the window's own margin where there is no message.
	int ChatTopLimit(const EditorArea& area, int screenHeight) {
		int top = 4;
		for (const auto& band: area.textBands) {
			if (band.y + band.h <= screenHeight / 2) top = std::max(top, band.y + band.h + 4);
		}
		return top;
	}

	/// The run an open entry needs above the seats panel: one history row and the tight entry.
	int ChatEntryMinimum(int lineHeight) { return 4 * lineHeight + 28; }

	std::string DisplayName(std::string text) {
		for (char& c: text) if (static_cast<unsigned char>(c) < 32) c = ' ';
		return text;
	}

	std::string FitLine(GUIFont* font, std::string text, int width) {
		text = DisplayName(std::move(text));
		if (font->CalculateWidth(text) <= width) return text;
		while (!text.empty() && font->CalculateWidth(text + "...") > width) text.pop_back();
		return text + "...";
	}

	/// A token wider than the column keeps a prefix and "...", so the glyph run stays inside the column.
	std::string FitTokens(GUIFont* font, const std::string& text, int width) {
		if (!font || width < 1) return text;
		std::string fitted, token;
		auto emit = [&]() {
			if (token.empty()) return;
			fitted += FitLine(font, std::move(token), width);
			token.clear();
		};
		for (char c: text) {
			if (c == ' ' || c == '\n') {
				emit();
				fitted += c;
			} else {
				token += c;
			}
		}
		emit();
		return fitted;
	}

	/// A measured round trip in milliseconds; a link under one reads as such, never as unmeasured.
	std::string PingWords(uint32_t milliseconds) { return milliseconds == 0 ? "<1" : std::to_string(milliseconds); }

	/// Whether a member's link has a round trip to show.
	bool PingKnown(const NetLobbyMember& member) { return member.pingMs > 0 || member.pingMeasured; }

	/// A name shortened to its room keeps the seat number that tells it apart from a namesake.
	std::string FitName(GUIFont* font, const std::string& name, int width) {
		const std::string text = DisplayName(name);
		const auto seat = text.rfind(" (seat ");
		if (font->CalculateWidth(text) <= width || seat == std::string::npos || !text.ends_with(")")) return FitLine(font, text, width);
		const std::string number = text.substr(seat);
		return FitLine(font, text.substr(0, seat), std::max(0, width - font->CalculateWidth(number))) + number;
	}

	/// The lines a text takes wrapped into a column, every word whole.
	std::string WrapWhole(GUIFont* font, const std::string& text, int width);

	SDL_Scancode ChatScancode() {
		const std::string& key = g_SettingsMan.GetNetworkChatKey();
		if (key == "ENTER" || key == "RETURN") return SDL_SCANCODE_RETURN;
		if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
			return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key[0] - 'A'));
		}
		return SDL_SCANCODE_T;
	}

	int TeamBarColor(uint8_t team, int alpha) {
		switch (team) {
			case 0: return makeacol32(220, 70, 70, alpha);
			case 1: return makeacol32(70, 120, 220, alpha);
			case 2: return makeacol32(70, 200, 90, alpha);
			case 3: return makeacol32(220, 200, 70, alpha);
			default: return makeacol32(190, 190, 190, alpha);
		}
	}

	// A seat's own state names the seat; only a toast about an action names who did it.
	bool ToastNamesTheSeat(const std::string& kind) { return kind == "seat_held"; }
	bool ToastNamesNobody(const std::string& kind) { return kind == "slow_machine" || kind == "catch_up"; }

	// The roster names this machine's own seat held or rejoining until the host says it is back.
	bool OwnRosterSeatHeld() {
		const auto snapshot = g_NetMatchService.GetLobbySnapshot();
		if (const auto view = g_NetMatchService.GetSeatView(snapshot.localPeerId)) return view->seat.owner != 0 && (view->state == "Held" || view->state == "Reconnecting");
		return std::any_of(snapshot.members.begin(), snapshot.members.end(), [&snapshot](const auto& member) {
			return member.peerId == snapshot.localPeerId && !member.cpu && (member.aiHeld || member.reclaiming) && !ScenarioRunner::IsLockstepSeatReleased(member.peerId);
		});
	}

	/// This player's own seat on its way back: the roster holds it, or this peer replays its hold while it catches up.
	bool OwnSeatReturning() {
		return OwnRosterSeatHeld() || (ScenarioRunner::IsLockstepOwnSeatHeld() && (ScenarioRunner::WorldCatchUpActive() || g_NetMatchService.IsMatchResyncing()));
	}

	bool ToastStillApplies(const ScenarioRunner::NetUiToastRecord& toast) {
		if (toast.kind == "slow_machine") return ScenarioRunner::IsLockstepLocalMachineSlow();
		if (toast.kind != "seat_held") return true;
		if (toast.text.ends_with("joining")) {
			return ScenarioRunner::WorldCatchUpActive() || g_NetMatchService.IsMatchResyncing() || ScenarioRunner::IsLockstepOwnSeatHeld() || OwnRosterSeatHeld();
		}
		const uint8_t peer = toast.senderPeerId ? toast.senderPeerId : ScenarioRunner::GetLockstepLocalPeerId();
		return ScenarioRunner::IsLockstepSeatUnderAI(peer, ScenarioRunner::GetLockstepCompletedFrame());
	}

	/// Names two or more of this round's players share; their lines carry the player's number so they read apart.
	std::set<std::string> s_SharedNames;

	void NoteSharedNames(const NetLobbySnapshot& snapshot) {
		std::map<std::string, int> counts;
		for (const auto& member: snapshot.members) {
			if (!member.cpu) ++counts[NetPlayerPresentation::Name(member)];
		}
		s_SharedNames.clear();
		for (const auto& [name, count]: counts) {
			if (count > 1) s_SharedNames.insert(name);
		}
	}

	std::string ShownName(uint8_t peer, const std::string& name) {
		return s_SharedNames.contains(name) ? name + " (seat " + std::to_string(peer) + ")" : name;
	}

	std::string ShownName(const NetLobbyMember& member) { return ShownName(member.peerId, NetPlayerPresentation::Name(member)); }

	std::string ShownRow(const NetLobbyMember& member) {
		std::string row = NetPlayerPresentation::Row(member);
		const std::string name = NetPlayerPresentation::Name(member);
		if (s_SharedNames.contains(name) && row.starts_with(name)) row.insert(name.size(), " (seat " + std::to_string(member.peerId) + ")");
		return row;
	}

	std::string ToastText(const ScenarioRunner::NetUiToastRecord& toast) {
		uint8_t sender = toast.senderPeerId;
		if (ToastNamesNobody(toast.kind)) return toast.text;
		if (ToastNamesTheSeat(toast.kind)) {
			// This player's own seat speaks to the player, not about them.
			if (!sender || sender == ScenarioRunner::GetLockstepLocalPeerId()) {
				if (toast.text.find("rejoining") != std::string::npos) return "Rejoining - the AI plays your units until you are back";
				if (toast.text.find("joining") != std::string::npos) return "Joining - catching up with the match";
				return "The AI is playing for you until you are back";
			}
			const auto snapshot = g_NetMatchService.GetLobbySnapshot();
			const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(), [&](const auto& row) { return row.peerId == sender; });
			const std::string state = member != snapshot.members.end() ? NetPlayerPresentation::State(*member) : std::string("Held - AI in control");
			const bool joining = member != snapshot.members.end() && member->joining;
			return ShownName(sender, NetPlayerPresentation::Name(sender, g_NetMatchService.GetPeerDisplayName(sender))) + ": " +
			    (toast.text.find("rejoining") != std::string::npos ? (joining ? "Held - AI in control - joining" : "Held - AI in control - rejoining") : state);
		}
		if (toast.kind == "resumed" && sender == 0) {
			const auto& events = ScenarioRunner::GetNetUiToastLog();
			auto event = std::find_if(events.rbegin(), events.rend(), [&](const auto& entry) {
				return entry.tick == toast.tick && entry.kind == toast.kind && entry.text == toast.text;
			});
			if (event != events.rend()) {
				for (++event; event != events.rend(); ++event) {
					if (event->kind == "resuming") { sender = event->senderPeerId; break; }
					if (event->kind == "paused" || event->kind == "resumed") break;
				}
			}
		}
		std::string text = toast.text;
		for (const auto& [peer, name]: NetPlayerPresentation::names) {
			for (const std::string prefix: {"Client " + std::to_string(peer), "Player " + std::to_string(peer)}) {
				if (text.starts_with(prefix + " ")) text.replace(0, prefix.size(), name);
			}
		}
		return text + (sender ? " by " + ShownName(sender, NetPlayerPresentation::Name(sender, g_NetMatchService.GetPeerDisplayName(sender))) : std::string());
	}

	// The player_* toasts read "<name> <verb>"; on a short line the name elides, never the verb.
	std::string FitToastText(GUIFont* font, const ScenarioRunner::NetUiToastRecord& toast, int width) {
		const char* verb = nullptr;
		if (toast.kind == "player_joined") {
			verb = " joined";
		} else if (toast.kind == "player_dropped") {
			verb = " dropped";
		} else if (toast.kind == "player_left") {
			verb = " left";
		} else if (toast.kind == "player_rejoined") {
			verb = " rejoined";
		} else if (toast.kind == "player_substituted") {
			verb = " joined as substitute";
		}
		std::string text = ToastText(toast);
		const size_t verbLen = verb ? std::strlen(verb) : 0;
		if (verbLen && text.size() > verbLen && text.compare(text.size() - verbLen, verbLen, verb) == 0) {
			return FitLine(font, text.substr(0, text.size() - verbLen), width - font->CalculateWidth(verb)) + verb;
		}
		return FitLine(font, std::move(text), width);
	}

	/// The widest whitespace-delimited token, so a panel's wrap column is never narrower than the
	/// longest word it must show.
	int LongestWordWidth(GUIFont* font, const std::string& text) {
		int widest = 0, run = 0;
		for (char c: text) {
			if (c == ' ' || c == '\n') {
				widest = std::max(widest, run);
				run = 0;
				continue;
			}
			run += font->CalculateWidth(c);
		}
		return std::max(widest, run);
	}

	/// "Ana", "Ana and Ben", "Ana, Ben and Cleo".
	std::string NamesInWords(const std::vector<std::string>& names) {
		std::string words;
		for (size_t index = 0; index < names.size(); ++index) {
			words += (index == 0 ? "" : index + 1 == names.size() ? " and " : ", ") + names[index];
		}
		return words;
	}

	std::string SecondsInWords(long long milliseconds) {
		char text[32];
		std::snprintf(text, sizeof(text), "%.1f s", static_cast<double>(std::max(0LL, milliseconds)) / 1000.0);
		return text;
	}

	std::string WrapText(GUIFont* font, const std::string& text, int width) {
		std::string wrapped, line;
		for (char c: text) {
			if (c == '\n') {
				wrapped += line + '\n';
				line.clear();
				continue;
			}
			if (!line.empty() && font->CalculateWidth(line + c) > width) {
				const auto space = line.find_last_of(' ');
				if (space != std::string::npos && space > 0) {
					wrapped += line.substr(0, space) + '\n';
					line.erase(0, space + 1);
				}
				// No boundary on the line: the word runs whole and the panel's width rule holds it.
			}
			line += c;
		}
		return wrapped + line;
	}

	std::string WrapWhole(GUIFont* font, const std::string& text, int width) {
		return WrapText(font, FitTokens(font, text, width), width);
	}

	int LineCount(const std::string& text) {
		return text.empty() ? 0 : 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
	}
}

NetModerationGUI::NetModerationGUI(AllegroScreen* screen) :
	m_Screen(screen), m_Input(std::make_unique<GUIInputWrapper>(-1, true)), m_Controls(std::make_unique<GUIControlManager>()) {
	RTEAssert(m_Controls->Create(screen, m_Input.get(), "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Could not create the network seat panel");
	std::string fontName;
	m_Controls->GetSkin()->GetValue("Label", "Font", &fontName);
	m_LabelFont = m_Controls->GetSkin()->GetFont(fontName);
	const int width = std::min(c_PanelWidth, g_WindowMan.GetResX() - 12);
	m_Panel = dynamic_cast<GUICollectionBox*>(m_Controls->AddControl("NetworkSeats", "COLLECTIONBOX", nullptr,
	    (g_WindowMan.GetResX() - width) / 2, PanelTop(g_WindowMan.GetResY()), width, c_PanelHeight));
	m_Panel->SetDrawBackground(true);
	m_Panel->SetDrawType(GUICollectionBox::Image);
	auto label = [&](const std::string& name, int y, int height) {
		auto* result = dynamic_cast<GUILabel*>(m_Controls->AddControl(name, "LABEL", m_Panel, 10, y, width - 20, height));
		result->SetHAlignment(GUIFont::Left);
		result->SetVAlignment(GUIFont::Top);
		return result;
	};
	auto button = [&](const std::string& name, const std::string& text, int x, int y, int w) {
		auto* result = dynamic_cast<GUIButton*>(m_Controls->AddControl(name, "BUTTON", m_Panel, x, y, w, 20));
		result->SetText(text);
		return result;
	};
	m_Title = label("NetworkSeatsTitle", 8, 16);
	m_Summary = label("NetworkSeatsSummary", 24, 16);
	m_Status = label("NetworkSeatsStatus", 282, 30);
	m_Roster = label("NetworkSeatsRoster", 40, 240);
	m_Close = button("NetworkSeatsClose", "Close  [F6 / Esc]", width - 224, 318, 214);
	m_OptionsToggle = button("NetworkSeatsOptions", "Rules for this round", 10, 318, 180);
	m_More = button("NetworkSeatsMore", "More players", 196, 318, 160);
	m_More->SetVisible(false);
	m_Options = label("NetworkSeatsOptionsText", 40, 240);
	m_Options->SetVisible(false);
	for (size_t row = 0; row < m_Seats.size(); ++row) {
		const std::string suffix = std::to_string(row);
		auto& seat = m_Seats[row];
		seat.name = label("NetworkSeatName" + suffix, 40, 16);
		seat.detail = label("NetworkSeatDetail" + suffix, 40, 16);
		seat.hint = label("NetworkSeatHint" + suffix, 40, 16);
		seat.requests = dynamic_cast<GUIListBox*>(m_Controls->AddControl("NetworkSeatApplicant" + suffix, "LISTBOX", m_Panel, 10, 40, 180, 44));
		seat.actions[0] = button("NetworkSeatWait" + suffix, "", 10, 40, 120);
		seat.actions[1] = button("NetworkSeatSubstitute" + suffix, "", 10, 40, 120);
		seat.actions[2] = button("NetworkSeatCancel" + suffix, "Cancel this approval", 10, 40, 150);
		seat.remove = button("NetworkSeatRemove" + suffix, "", 10, 40, 120);
		seat.ban = button("NetworkSeatBan" + suffix, "", 10, 40, 180);
		seat.declineApplicant = button("NetworkSeatDeclineApplicant" + suffix, "", 10, 40, 150);
		seat.banApplicant = button("NetworkSeatBanApplicant" + suffix, "", 10, 40, 180);
		HideRow(seat);
	}
	m_Panel->SetVisible(false);
	if (const char* lever = std::getenv("CC_TEST_PANEL_COST"); lever && *lever) m_Cost = std::make_unique<FrameCost>();
}

NetModerationGUI::~NetModerationGUI() = default;

void NetModerationGUI::CreateOverlay() {
	if (m_OverlayControls) return;
	m_OverlayControls = std::make_unique<GUIControlManager>();
	RTEAssert(m_OverlayControls->Create(m_Screen, m_Input.get(), "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Could not create the network status widget");
	m_NetStatusBox = dynamic_cast<GUICollectionBox*>(m_OverlayControls->AddControl("BoxNetMatchStatus", "COLLECTIONBOX", nullptr, 0, 32, 252, 76));
	m_NetStatus = dynamic_cast<GUILabel*>(m_OverlayControls->AddControl("LabelNetMatchStatus", "LABEL", m_NetStatusBox, 6, 6, 240, 64));
	m_NetStatus->SetVAlignment(GUIFont::Top);
	m_NetStatusBox->SetVisible(false);
	for (size_t row = 0; row < m_Toasts.size(); ++row) {
		m_Toasts[row] = dynamic_cast<GUILabel*>(m_OverlayControls->AddControl("LabelNetMatchToast" + std::to_string(row), "LABEL", nullptr, 0, 0, 20, 12));
		m_Toasts[row]->SetHAlignment(GUIFont::Centre);
		m_Toasts[row]->SetVAlignment(GUIFont::Top);
		m_Toasts[row]->SetVisible(false);
	}
	for (size_t row = 0; row < m_MatchChat.size(); ++row) {
		m_MatchChat[row] = dynamic_cast<GUILabel*>(m_OverlayControls->AddControl("LabelMatchChat" + std::to_string(row), "LABEL", nullptr, 0, 0, 20, 12));
		m_MatchChat[row]->SetHAlignment(GUIFont::Left);
		m_MatchChat[row]->SetVAlignment(GUIFont::Top);
		m_MatchChat[row]->SetVisible(false);
	}
	m_MatchChatInput = dynamic_cast<GUITextBox*>(m_OverlayControls->AddControl("TextMatchChatInput", "TEXTBOX", nullptr, 0, 0, 20, 16));
	m_MatchChatInput->SetMaxTextLength(static_cast<int>(NetProtocol::c_MaxShortTextBytes));
	m_MatchChatInput->SetVisible(false);
	m_MatchChatCaption = dynamic_cast<GUILabel*>(m_OverlayControls->AddControl("LabelMatchChatAudience", "LABEL", nullptr, 0, 0, 20, 16));
	m_MatchChatCaption->SetHAlignment(GUIFont::Left);
	m_MatchChatCaption->SetVAlignment(GUIFont::Top);
	m_ChatOlder = dynamic_cast<GUIButton*>(m_OverlayControls->AddControl("ButtonMatchChatOlder", "BUTTON", nullptr, 0, 0, 96, 20));
	m_ChatOlder->SetText("Older messages");
	m_ChatNewer = dynamic_cast<GUIButton*>(m_OverlayControls->AddControl("ButtonMatchChatNewer", "BUTTON", nullptr, 0, 0, 96, 20));
	m_ChatNewer->SetText("Newer messages");
	m_MatchChatCaption->SetVisible(false);
	m_ChatOlder->SetVisible(false);
	m_ChatNewer->SetVisible(false);
}

bool NetModerationGUI::SetOpen(bool open) {
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	if (open && snapshot.serviceState != "Running" && snapshot.serviceState != "Starting" && snapshot.serviceState != "ReadyToLaunch") return false;
	if (open == m_Open) return true;
	m_Open = open;
	m_Panel->SetVisible(open);
	m_Press.reset();
	if (open) {
		m_Input->SetKeyJoyMouseCursor(true);
		g_UInputMan.TrapMousePos(false);
		Refresh();
	} else {
		m_Panel->ReleaseMouse();
		m_Close->SetPushed(false);
		m_OptionsToggle->SetPushed(false);
		m_More->SetPushed(false);
		m_OptionsView = false;
		m_PageFirstPeer = 0;
		for (auto& seat: m_Seats) {
			for (auto* button: seat.actions) button->SetPushed(false);
			seat.remove->SetPushed(false);
			seat.ban->SetPushed(false);
		}
		// An armed action never outlives the panel it was armed on.
		m_Armed.reset();
		m_PanelStatus.clear();
		g_UInputMan.EndSimUpdate();
	}
	return true;
}

NetModerationGUI::PanelPlacement NetModerationGUI::PlaceSeatsPanel(int highestTop, int bottomLimit, int wantedHeight, int minHeight, const std::vector<PanelBand>& bands) {
	std::vector<PanelBand> blocked;
	for (const PanelBand& band: bands) {
		const PanelBand clipped{std::max(band.top, highestTop), std::min(band.bottom, bottomLimit)};
		if (clipped.bottom > clipped.top) blocked.push_back(clipped);
	}
	std::sort(blocked.begin(), blocked.end(), [](const PanelBand& a, const PanelBand& b) { return a.top < b.top; });
	std::vector<PanelBand> open;
	int cursor = highestTop;
	for (const PanelBand& band: blocked) {
		if (band.top > cursor) open.push_back({cursor, band.top});
		cursor = std::max(cursor, band.bottom);
	}
	if (cursor < bottomLimit) open.push_back({cursor, bottomLimit});
	const PanelBand* chosen = nullptr;
	for (const PanelBand& run: open) {
		if (run.bottom - run.top >= wantedHeight) {
			chosen = &run;
			break;
		}
	}
	// Nothing holds the whole panel: the longest run takes it and the roster scrolls for the rest.
	if (!chosen) {
		for (const PanelBand& run: open) {
			if (!chosen || run.bottom - run.top > chosen->bottom - chosen->top) chosen = &run;
		}
	}
	// Every row is under a band: the panel keeps its smallest height against the bottom edge.
	if (!chosen) return {std::max(0, bottomLimit - minHeight), minHeight};
	const int height = std::max(minHeight, std::min(wantedHeight, chosen->bottom - chosen->top));
	return {std::max(0, std::min(chosen->top, bottomLimit - height)), height};
}

NetModerationGUI::PanelPlacement NetModerationGUI::PlaceSeatsPanelOnScreen(int screenHeight, int rowHeight, const std::vector<PanelBand>& textBands, int reservedTop, int wantedHeight) {
	const int minHeight = c_PanelHeight - c_PanelMaxLost;
	wantedHeight = std::clamp(wantedHeight, minHeight, c_PanelHeight);
	const int top = PanelTop(screenHeight, wantedHeight);
	const int height = std::max(minHeight, std::min(wantedHeight, screenHeight - c_PanelGap - top));
	if (screenHeight >= c_CompactMaxHeight) {
		const int fittedTop = std::max(top, reservedTop);
		return {fittedTop, std::max(minHeight, std::min(height, screenHeight - c_PanelGap - fittedTop))};
	}
	// A compact screen keeps the strip band and one toast row above the panel's top: the panel sits
	// under them and loses the rows off its height, so its bottom edge - and the roster - stay put.
	// An open chat entry's run sits above the panel too, so its reservation is a floor for the top.
	int highestTop = std::max({top, c_StripBandBottom + rowHeight + c_PanelGap, reservedTop});
	highestTop = std::min(highestTop, screenHeight - c_PanelGap - minHeight);
	// A seat's message owns its own rows and the toast row under them, wherever on the screen it sits.
	std::vector<PanelBand> bands;
	for (const PanelBand& band: textBands) {
		bands.push_back({band.top, band.bottom + rowHeight + 2 * c_PanelGap});
	}
	const int wanted = std::max(minHeight, std::min(wantedHeight, screenHeight - c_PanelGap - highestTop));
	return PlaceSeatsPanel(highestTop, screenHeight - c_PanelGap, wanted, minHeight, bands);
}

bool NetModerationGUI::MatchSurfacesDrawn(bool controllerSyncActive, bool matchResyncing, bool hostLost, bool lockstepAttached, bool matchEnded, bool activityInMatch, bool postMatchLobby, bool lobbyMenuActive) {
	return controllerSyncActive || matchResyncing || hostLost || ((lockstepAttached || matchEnded) && activityInMatch) ||
	    (postMatchLobby && lobbyMenuActive);
}

namespace {
	/// The multiplayer lobby is the menu up in the menu loop: the title screen, settings and every
	/// other menu leave the post-match arm off. The lobby's own sub-state does not gate it - a
	/// survivor of a lost host reads the same surfaces on the landing it lands on.
	bool LobbyMenuUp() {
		const MainMenuGUI* mainMenu = g_MenuMan.GetMainMenu();
		return g_MenuMan.GetIsInMenuScreen() && g_MenuMan.IsMainMenuInteractive() && mainMenu &&
		    mainMenu->AutomationActiveScreenName() == "MultiplayerScreen";
	}

	/// The finished match's lobby on this peer: owed its pump while the peers settle, then standing on
	/// the snapshot's own playedAMatch mark until the peer leaves or relaunches. The pump alone cannot
	/// say it - its marker clears the moment the lobby seats, and on a Failed landing at once.
	bool PostMatchLobbyAlive() {
		const auto snapshot = g_NetMatchService.GetLobbySnapshot();
		return g_NetMatchService.NeedsCompletedLobbyPump() ||
		    (snapshot.playedAMatch && snapshot.active && !snapshot.leftMatch);
	}

	/// The lobby screen's box is the one column the menu-loop surfaces lay out beside - the same rule
	/// the editor's picker column follows - so the roster, the status and the bands keep off its controls.
	void LobbyMenuColumn(EditorArea& area) {
		const MainMenuGUI* menu = g_MenuMan.GetMainMenu();
		GUIControlManager* controls = menu ? menu->AutomationManager() : nullptr;
		GUIControl* screen = controls ? controls->GetControl("MultiplayerScreen") : nullptr;
		int x = 0, y = 0, w = 0, h = 0;
		if (screen) screen->GetControlRect(&x, &y, &w, &h);
		if (w > 0 && h > 0) area.columns.push_back({x - 2, y - 2, w + 4, h + 4});
	}
}

bool NetModerationGUI::PostMatchLobbySurfaces() {
	return LobbyMenuUp() && PostMatchLobbyAlive();
}

bool NetModerationGUI::ActivityInMatch() {
	// Not ActivityRunning(): that reads false the moment the pause menu opens, and the host's End Match
	// pauses the activity on its way out.
	const Activity* activity = g_ActivityMan.GetActivity();
	return activity && !activity->IsOver();
}

void NetModerationGUI::LayoutPanel() {
	const int screenHeight = g_WindowMan.GetResY();
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	const int rowHeight = std::max(12, font ? font->GetFontHeight() : 12) + 8;
	int wantedWidth = 430;
	if (m_OptionsView || m_Rows.size() > m_Seats.size() || std::any_of(m_Rows.begin(), m_Rows.end(), [](const PanelRow& row) { return row.decision.has_value(); })) wantedWidth = c_PanelWidth;
	for (const auto& row: m_Rows) wantedWidth = std::max(wantedWidth, std::min(c_PanelWidth, m_LabelFont->CalculateWidth(row.name + "  /  " + row.state) + 28));
	const int width = std::min(wantedWidth, g_WindowMan.GetResX() - 12);
	const EditorArea area = FreeArea(g_WindowMan.GetResX());
	std::vector<PanelBand> textBands;
	for (const auto& band: area.textBands) {
		textBands.push_back({band.y, band.y + band.h});
	}
	// The panel gives up rows to its compact form before an open entry's history gives up its last one.
	const int toastRows = static_cast<int>(std::min<size_t>(3, ScenarioRunner::GetVisibleNetUiToasts().size()));
	const int statusBottom = m_StatusRect.visible ? m_StatusRect.y + m_StatusRect.height : c_StripBandBottom;
	int reservedTop = statusBottom + c_PanelGap + (toastRows ? toastRows * rowHeight + c_PanelGap : 0);
	if (m_ChatEntryOpen && g_SettingsMan.GetNetworkChatVisible()) {
		const int lineHeight = ChatLineHeight(g_SettingsMan.GetNetworkChatTextSize() == SettingsMan::NetworkChatTextSize::Large);
		reservedTop = std::max(reservedTop, ChatTopLimit(area, screenHeight) + ChatEntryMinimum(lineHeight) + c_PanelGap);
	}
	int wantedHeight = 144;
	if (m_OptionsView) {
		wantedHeight = 76 + m_Options->GetTextHeight();
	} else if (g_NetMatchService.IsHost()) {
		wantedHeight = 40 + rowHeight + 4 + 36 + 32;
		for (size_t row = m_PageStart; row < m_Rows.size() && row < m_PageStart + m_Seats.size(); ++row) wantedHeight += RowHeight(m_Rows[row], width - 20);
	} else {
		wantedHeight = 76 + m_Roster->GetTextHeight();
	}
	const PanelPlacement placed = PlaceSeatsPanelOnScreen(screenHeight, rowHeight, textBands, reservedTop, std::clamp(wantedHeight, 144, c_PanelHeight));
	const int top = placed.top;
	const int height = placed.height;
	const int lost = std::min(c_PanelMaxLost, c_PanelHeight - height);
	int x, y, w, h;
	m_Panel->GetControlRect(&x, &y, &w, &h);
	if (x != (g_WindowMan.GetResX() - width) / 2 || y != top || w != width || h != height) {
		m_Panel->Move((g_WindowMan.GetResX() - width) / 2, top);
		m_Panel->Resize(width, height);
	}
	for (GUILabel* label: {m_Title, m_Summary, m_Roster, m_Options, m_Status}) {
		if (label->GetWidth() != width - 20) label->Resize(width - 20, label->GetHeight());
	}
	// The roster yields the reserved rows and scrolls for what no longer fits; the status row gives up
	// its second line first, then moves up with the close row instead of clipping at the panel's bottom.
	// The options view shares that band, so it takes the same shrink and the same scroll.
	const int textHeight = std::max(0, height - 76);
	if (m_Options->GetHeight() != textHeight) {
		m_Options->Resize(m_Options->GetWidth(), textHeight);
	}
	if (!g_NetMatchService.IsHost() && m_Roster->GetHeight() != textHeight) {
		m_Roster->Resize(m_Roster->GetWidth(), textHeight);
	}
	m_Roster->SetVerticalOverflowScroll(lost != 0);
	m_Roster->ActivateDeactivateOverflowScroll(lost != 0);
	m_Options->SetVerticalOverflowScroll(true);
	m_Options->ActivateDeactivateOverflowScroll(true);
	const int closeY = std::max(0, 318 - lost);
	// The status keeps two lines at every size, so a consequence is never cut; it keeps its gap above the close row.
	const int statusHeight = std::max(30, 2 * m_LabelFont->GetFontHeight());
	const int statusY = std::max(0, std::min(282, closeY - 6 - statusHeight));
	if (m_Status->GetRelYPos() != statusY) {
		m_Status->SetPositionRel(10, statusY);
	}
	if (m_Status->GetHeight() != statusHeight) {
		m_Status->Resize(m_Status->GetWidth(), statusHeight);
	}
	if (m_Close->GetRelYPos() != closeY || m_Close->GetRelXPos() != width - 224) {
		m_Close->SetPositionRel(width - 224, closeY);
		m_OptionsToggle->SetPositionRel(10, closeY);
	}
}

namespace {
	/// A button sized to its caption, the name in it shortened first when the caption would pass the room it has.
	void Caption(GUIButton* button, GUIFont* font, const std::string& before, const std::string& name, const std::string& after, int room) {
		std::string text = before + name + after;
		const int fixed = font->CalculateWidth(before + after) + 20;
		if (font->CalculateWidth(text) + 20 > room) text = before + FitName(font, name, std::max(24, room - fixed)) + after;
		if (button->GetText() != text) button->SetText(text);
		const int width = std::min(room, std::max(60, font->CalculateWidth(text) + 20));
		if (button->GetWidth() != width) button->Resize(width, 20);
	}

	void Place(GUIControl* control, int x, int y) {
		if (control->GetPanel()->GetRelXPos() != x || control->GetPanel()->GetRelYPos() != y) control->GetPanel()->SetPositionRel(x, y);
	}

	void Show(GUIControl* control, bool shown) {
		if (control->GetVisible() != shown) control->SetVisible(shown);
	}

	uint64_t PanelNowMs() { return static_cast<uint64_t>(SDL_GetTicks()); }

	/// A refused removal's reason in the host's words.
	std::string RemovalWords(NetKickBanResult result) {
		switch (result) {
			case NetKickBanResult::NotHosting: return "only the host can do that";
			case NetKickBanResult::ForbiddenTarget: return "the host's own place cannot be taken away";
			case NetKickBanResult::UnknownSeat:
			case NetKickBanResult::StaleSelection: return "that place changed - try again";
			case NetKickBanResult::UnknownIdentity: return "the player's identity is not verified";
			case NetKickBanResult::PersistenceFailed: return "the ban list could not be saved";
			default: return "that is not possible right now";
		}
	}

	/// Why a held row's give-away cannot name anyone yet, or nothing when it can.
	std::string RowWhy(const NetModerationGUI::PanelRow& row) {
		if (!row.decision) return {};
		const NetH4ModerationSeat& view = row.decision->view;
		if (!view.actionsAvailable) return "Changes are paused for a moment; try again shortly";
		if (view.substituting) return "Waiting for " + NetModerationUx::ApplicantName(*row.decision) + " to finish joining";
		if (view.reclaiming) return row.name + " is rejoining";
		if (!view.substitutable) return row.name + "'s place cannot be kept or given away right now";
		if (view.applicants.empty()) return "Nobody has asked for " + row.name + "'s place yet";
		if (row.decision->applicant == c_InvalidNetPeerId) return "Pick a player from the list to let them join";
		return {};
	}

	int RequestListHeight(GUIFont* font, size_t requests) {
		return static_cast<int>(std::min<size_t>(3, std::max<size_t>(1, requests))) * (font->GetFontHeight() + 2) + 4;
	}

	/// Who a row's controls act on: the player, its place and the request Let names.
	bool SameIdentity(const NetModerationGUI::PanelRow& shown, const NetModerationGUI::PanelRow& pressed) {
		auto place = [](const NetModerationGUI::PanelRow& row) {
			return row.seat ? std::make_pair(row.seat->stableSeat, row.seat->incarnation) : std::make_pair(uint16_t{0}, uint32_t{0});
		};
		auto request = [](const NetModerationGUI::PanelRow& row) { return row.decision ? row.decision->selection : NetModerationSelection{}; };
		return shown.peer == pressed.peer && shown.opened == pressed.opened && place(shown) == place(pressed) && request(shown) == request(pressed);
	}

	/// The host's line when a press or an armed action lost its player before it could act.
	constexpr const char* c_ListChanged = "The list changed, so nothing was done - check the names and press again";
}

int NetModerationGUI::RowHeight(const PanelRow& row, int inner) const {
	const int lineHeight = std::max(14, m_LabelFont->GetFontHeight() + 2);
	if (!row.decision) return 22 + lineHeight + 6;
	const size_t requests = row.decision->view.applicants.size();
	const int listWidth = std::min(220, inner / 3);
	// The reason an action is off runs whole: beside the buttons while nobody asks, under them beside the list otherwise.
	const std::string why = RowWhy(row);
	if (requests == 0) {
		const int whyLines = LineCount(WrapWhole(m_LabelFont, why, listWidth));
		return std::max(22 + lineHeight + 4 + 22 + 6, 22 + lineHeight + 2 + whyLines * lineHeight + 6);
	}
	const int below = std::max(6, RequestListHeight(m_LabelFont, requests) - 22 + 6);
	const int whyLines = LineCount(WrapWhole(m_LabelFont, why, inner - listWidth - 12));
	return 22 + lineHeight + 4 + 44 + std::max(below, whyLines * lineHeight + 6);
}

void NetModerationGUI::HideRow(Controls& controls) {
	for (GUIControl* control: std::initializer_list<GUIControl*>{controls.name, controls.detail, controls.hint, controls.requests, controls.remove, controls.ban, controls.declineApplicant, controls.banApplicant}) Show(control, false);
	for (auto* action: controls.actions) Show(action, false);
}

std::vector<NetModerationGUI::PanelRow> NetModerationGUI::BuildRows(const NetLobbySnapshot& snapshot) {
	const std::vector<NetH4ModerationSeat> seats = g_NetMatchService.GetModerationSeats();
	m_Model.Refresh(seats);
	std::vector<PanelRow> held, playing;
	for (const auto& member: snapshot.members) {
		if (!member.cpu && (member.isLocal || member.peerId == snapshot.localPeerId)) continue;
		PanelRow row;
		row.peer = member.peerId;
		row.team = member.team;
		row.cpu = member.cpu;
		for (size_t index = 0; index < m_Model.RowCount(); ++index) {
			if (!member.cpu && m_Model.GetRow(index).lockstepPeerId == member.peerId) row.decision = m_Model.GetRow(index);
		}
		// A place nobody holds any more is a line on the roster, unless a newcomer asks for it: then it is the host's to give.
		row.opened = !member.cpu && (ShownRow(member) == "Open seat" || NetPlayerPresentation::Opened(member.peerId));
		if (row.opened && !(row.decision && !row.decision->view.applicants.empty())) continue;
		row.name = row.opened ? "Open place (seat " + std::to_string(member.peerId) + ")" : DisplayName(ShownName(member));
		// The roster's own reading after the name: the state, and the route while the link is live.
		const std::string full = DisplayName(ShownRow(member));
		const std::string prefix = DisplayName(ShownName(member)) + "  /  ";
		row.state = full.starts_with(prefix) ? full.substr(prefix.size()) : NetPlayerPresentation::State(member);
		for (const NetH4ModerationSeat& seat: seats) {
			if (!member.cpu && !seat.cpu && seat.lockstepPeerId == member.peerId) row.seat = seat;
		}
		(row.decision ? held : playing).push_back(std::move(row));
	}
	held.insert(held.end(), playing.begin(), playing.end());
	// The count the roster calls for, read apart from the rows above so a check can hold one against the other: every other
	// player, and an opened place while somebody asks for it.
	m_RowsImplied = 0;
	for (const auto& member: snapshot.members) {
		if (!member.cpu && (member.isLocal || member.peerId == snapshot.localPeerId)) continue;
		const bool open = !member.cpu && (NetPlayerPresentation::Row(member) == "Open seat" || NetPlayerPresentation::Opened(member.peerId));
		const bool asked = std::any_of(seats.begin(), seats.end(), [&](const NetH4ModerationSeat& seat) {
			return !seat.cpu && seat.lockstepPeerId == member.peerId && !seat.applicants.empty();
		});
		if (!open || asked) ++m_RowsImplied;
	}
	return held;
}

int NetModerationGUI::FillRow(Controls& controls, const PanelRow& row, size_t slot, int top) {
	const int inner = m_Panel->GetWidth() - 20;
	const int lineHeight = std::max(14, m_LabelFont->GetFontHeight() + 2);
	const bool running = g_NetMatchService.GetState() == NetMatchServiceState::Running;
	const bool armed = m_Armed && m_Armed->slot == slot && m_Armed->peer == row.peer && row.seat && m_Armed->stableSeat == row.seat->stableSeat &&
	                   m_Armed->incarnation == row.seat->incarnation;
	// The removal pair sits at the right of the name line, Ban outermost; an opened place has nobody in it to remove.
	Caption(controls.ban, m_LabelFont, armed && m_Armed->kind == Armed::Kind::Ban ? "Confirm: ban " : "Ban ", row.name,
	        armed && m_Armed->kind == Armed::Kind::Ban ? "" : " for this session", inner / 3);
	Caption(controls.remove, m_LabelFont, armed && m_Armed->kind == Armed::Kind::Remove ? "Confirm: remove " : "Remove ", row.name, "", inner / 4);
	const bool removable = running && row.seat.has_value() && row.seat->actionsAvailable;
	controls.remove->SetEnabled(removable);
	controls.ban->SetEnabled(removable);
	const int banX = 10 + inner - controls.ban->GetWidth();
	const int removeX = banX - 6 - controls.remove->GetWidth();
	Place(controls.ban, banX, top);
	Place(controls.remove, removeX, top);
	Show(controls.ban, !row.opened && !row.cpu);
	Show(controls.remove, !row.opened && !row.cpu);
	const int nameWidth = row.opened || row.cpu ? inner : std::max(40, removeX - 16);
	controls.name->Resize(nameWidth, 20);
	Place(controls.name, 10, top + 2);
	controls.name->SetText(FitName(m_LabelFont, row.name, nameWidth));
	Show(controls.name, true);
	std::string detail = row.state;
	if (row.decision) {
		const NetH4ModerationSeat& view = row.decision->view;
		const std::string cause = NetModerationUx::HoldCause(view);
		detail = "Team " + std::to_string(view.team + 1) + "  /  " + NetPlayerPresentation::State(row.peer, false, view.dropped, view.reclaiming) + (cause.empty() ? "" : "  /  " + cause) +
		         (view.joinProgress.empty() ? "" : "  /  " + view.joinProgress);
	}
	if (!removable && running && !row.opened && !row.cpu) detail += "  /  changes are paused for a moment";
	controls.detail->Resize(inner, lineHeight);
	Place(controls.detail, 10, top + 22);
	controls.detail->SetText(FitLine(m_LabelFont, detail, inner));
	Show(controls.detail, true);
	if (!row.decision) {
		Show(controls.declineApplicant, false);
		Show(controls.banApplicant, false);
		Show(controls.hint, false);
		Show(controls.requests, false);
		for (auto* action: controls.actions) Show(action, false);
		return RowHeight(row, inner);
	}
	const NetModerationUx::Row& model = *row.decision;
	const NetH4ModerationSeat& view = model.view;
	const std::string applicant = NetModerationUx::ApplicantName(model);
	const bool chosen = model.applicant != c_InvalidNetPeerId;
	const int buttonsTop = top + 22 + lineHeight + 4;
	const int listWidth = std::min(220, inner / 3);
	const int buttonRoom = inner - listWidth - 12;
	Caption(controls.actions[0], m_LabelFont, "Keep for ", row.name, "", buttonRoom / 3);
	const bool letArmed = armed && m_Armed->kind == Armed::Kind::Let;
	Caption(controls.actions[1], m_LabelFont, letArmed ? "Confirm: let " : "Let ", chosen ? applicant : std::string("someone"), " join", buttonRoom / 3);
	Caption(controls.actions[2], m_LabelFont, "Cancel this approval", "", "", buttonRoom / 3);
	int x = 10;
	for (size_t action = 0; action < controls.actions.size(); ++action) {
		// Cancel is there while an approval is on its way, and only then; an opened place has nobody to keep it for.
		const bool shown = static_cast<NetModerationAction>(action) == NetModerationAction::Cancel ? view.substituting
		                   : static_cast<NetModerationAction>(action) == NetModerationAction::Wait ? !row.opened : true;
		controls.actions[action]->SetEnabled(NetModerationUx::Available(model, static_cast<NetModerationAction>(action)));
		Place(controls.actions[action], x, buttonsTop);
		Show(controls.actions[action], shown);
		if (shown) x += controls.actions[action]->GetWidth() + 6;
	}
	const bool requestActions = chosen && view.actionsAvailable && !view.substituting;
	Caption(controls.declineApplicant, m_LabelFont, armed && m_Armed->kind == Armed::Kind::DeclineApplicant ? "Confirm: decline " : "Decline ", chosen ? applicant : "request", "", buttonRoom / 2);
	Caption(controls.banApplicant, m_LabelFont, armed && m_Armed->kind == Armed::Kind::BanApplicant ? "Confirm: ban " : "Ban ", chosen ? applicant : "requester", "", buttonRoom / 2);
	Place(controls.declineApplicant, 10, buttonsTop + 22);
	Place(controls.banApplicant, 16 + controls.declineApplicant->GetWidth(), buttonsTop + 22);
	controls.declineApplicant->SetEnabled(requestActions);
	controls.banApplicant->SetEnabled(requestActions);
	Show(controls.declineApplicant, !view.applicants.empty());
	Show(controls.banApplicant, !view.applicants.empty());
	// The people asking for this place, or why the give-away has nobody to name.
	const std::string why = RowWhy(row);
	const int listX = 10 + inner - listWidth;
	const int listTop = top + 22 + lineHeight + 2;
	if (!view.applicants.empty()) {
		std::vector<std::string> items;
		// An item fits its list, or the list scrolls sideways under the pick: the place's name goes first, then the words, the newcomer's
		// own name last.
		const int itemRoom = listWidth - 12;
		for (const NetH4ApplicantView& request: view.applicants) {
			std::string marks;
			if (view.held && request.displayName == view.displayName) marks += " (same name)";
			if (request.approved) marks += " - approved";
			std::string item = request.displayName + (row.opened ? " - asks for this place" : " - asks for " + row.name + "'s place") + marks;
			if (m_LabelFont->CalculateWidth(item) > itemRoom) item = request.displayName + " - asks for this place" + marks;
			if (m_LabelFont->CalculateWidth(item) > itemRoom) item = FitLine(m_LabelFont, request.displayName + marks, itemRoom);
			items.push_back(item);
		}
		std::vector<std::string> listed;
		for (const auto* entry: *controls.requests->GetItemList()) listed.push_back(entry->m_Name);
		if (listed != items) {
			controls.requests->ClearList();
			for (const std::string& item: items) controls.requests->AddItem(item);
		}
		int selected = -1;
		for (size_t index = 0; index < view.applicants.size(); ++index) {
			if (view.applicants[index].connection == model.applicant) selected = static_cast<int>(index);
		}
		if (controls.requests->GetSelectedIndex() != selected) controls.requests->SetSelectedIndex(selected);
		const int listHeight = RequestListHeight(m_LabelFont, items.size());
		if (controls.requests->GetWidth() != listWidth || controls.requests->GetHeight() != listHeight) controls.requests->Resize(listWidth, listHeight);
		Place(controls.requests, listX, listTop);
		Show(controls.requests, true);
	} else {
		Show(controls.requests, false);
	}
	// The reason runs whole in its column, a line under another, never cut.
	const int hintWidth = view.applicants.empty() ? listWidth : inner - listWidth - 12;
	const std::string wrapped = WrapWhole(m_LabelFont, why, hintWidth);
	if (controls.hint->GetText() != wrapped) controls.hint->SetText(wrapped);
	const int hintHeight = std::max(1, LineCount(wrapped)) * lineHeight;
	if (controls.hint->GetWidth() != hintWidth || controls.hint->GetHeight() != hintHeight) controls.hint->Resize(hintWidth, hintHeight);
	Place(controls.hint, view.applicants.empty() ? listX : 10, view.applicants.empty() ? listTop : buttonsTop + 44);
	Show(controls.hint, !why.empty());
	return RowHeight(row, inner);
}

void NetModerationGUI::Refresh() {
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	if (snapshot.isHost && !m_OptionsView) m_Rows = BuildRows(snapshot);
	LayoutPanel();
	// The hold is the round's, read from the same place the stall overlay reads it.
	std::string holdName;
	uint32_t holdSeconds = 0;
	const bool holdPause = ScenarioRunner::DescribeLockstepHoldPause(holdName, holdSeconds);
	m_Title->SetText(m_OptionsView ? "MATCH DETAILS" :
	    NetModerationPanelTitle(snapshot.serviceState == "Running", holdPause, DisplayName(holdName), holdSeconds, ScenarioRunner::IsLockstepPaused()));
	m_OptionsToggle->SetText(m_OptionsView ? (m_ConnectionView ? "Back to players" : "Connection details") : "Rules for this round");
	m_Options->SetVisible(m_OptionsView);
	if (m_Armed && PanelNowMs() > m_Armed->untilMs) {
		m_Armed.reset();
		m_PanelStatus.clear();
	}
	if (m_OptionsView) {
		// The adopted config every peer runs this round by - read-only here the way the lobby's
		// Details reads it for a client; the editable pages are the lobby's own.
		std::string options = m_ConnectionView ? NetHostConnectionSummary(g_NetMatchService.GetLobbyMatchConfig(), snapshot) : NetHostOptionsSummary(g_NetMatchService.GetLobbyMatchConfig(), snapshot);
		if (snapshot.isHost && m_ConnectionView) {
			options += "\nRepair match: " + std::string(NetHostRepairEnabled(g_NetMatchService)
			    ? "Ready - pause menu > Match Details" : NetHostRepairHint(g_NetMatchService));
		}
		m_Options->SetText(WrapText(m_LabelFont, FitTokens(m_LabelFont, options, m_Options->GetWidth()), m_Options->GetWidth()));
		m_Summary->SetText(m_ConnectionView ? "Connection details" : "Rules for this round");
		LayoutPanel();
		m_Status->SetVisible(false);
		m_Roster->SetVisible(false);
		m_RowsShown = 0;
		for (auto& seat: m_Seats) HideRow(seat);
		Show(m_More, false);
		return;
	}
	const int lineHeight = std::max(14, m_LabelFont->GetFontHeight() + 2);
	if (!snapshot.isHost) {
		m_Rows.clear();
		m_RowsShown = 0;
		m_RowsImplied = 0;
		for (auto& seat: m_Seats) HideRow(seat);
		Show(m_More, false);
		m_Summary->SetText("Only the host can keep, give away or remove a player's place");
		std::string roster;
		for (const auto& member: snapshot.members) {
			std::string line = DisplayName(ShownRow(member));
			if (!member.cpu && (member.isLocal || member.peerId == snapshot.localPeerId)) line.insert(DisplayName(ShownName(member)).size(), " (you)");
			roster += (roster.empty() ? "" : "\n\n") + line;
		}
		Place(m_Roster, 10, 40);
		m_Roster->SetText(WrapText(m_LabelFont, FitTokens(m_LabelFont, roster, m_Roster->GetWidth()), m_Roster->GetWidth()));
		m_Roster->SetVisible(true);
		m_Status->SetVisible(false);
		LayoutPanel();
		return;
	}
	size_t away = 0, requests = 0;
	// An opened place leads the rows but nobody is away from it: the sentence names the player who is.
	const PanelRow* awayRow = nullptr;
	for (const PanelRow& row: m_Rows) {
		if (row.decision) {
			if (!row.opened) {
				++away;
				awayRow = &row;
			}
			requests += row.decision->applicants;
		}
	}
	const bool ownHeld = OwnRosterSeatHeld() || ScenarioRunner::IsLockstepOwnSeatHeld();
	std::string summary = ownHeld ? "The AI is playing for you" : away == 0 ? "Everyone is playing" : away == 1 ? awayRow->name + " is away" : std::to_string(away) + " players are away";
	if (requests) summary += requests == 1 ? " - 1 request to join" : " - " + std::to_string(requests) + " requests to join";
	m_Summary->SetText(FitLine(m_LabelFont, summary, m_Summary->GetWidth()));
	// The host's own line leads, then a page of the other players' rows, above the status line that says what an action will do
	// and did. Players on the other pages are read as lines under the host's own, and More players turns to them.
	const int inner = m_Panel->GetWidth() - 20;
	const int statusTop = m_Status->GetRelYPos();
	const int room = std::max(0, statusTop - 2 - (40 + lineHeight + 4));
	std::vector<size_t> pageStarts{0};
	int used = 0;
	size_t onPage = 0;
	for (size_t index = 0; index < m_Rows.size(); ++index) {
		const int height = RowHeight(m_Rows[index], inner);
		if (onPage > 0 && (onPage == m_Seats.size() || used + height > room)) {
			pageStarts.push_back(index);
			used = 0;
			onPage = 0;
		}
		used += height;
		++onPage;
	}
	// The page shown is the one that holds the player it began with, so it stays put while rows come and go around it.
	size_t page = 0;
	for (size_t index = 0; index < m_Rows.size(); ++index) {
		if (m_Rows[index].peer != m_PageFirstPeer || m_Rows[index].team != m_PageFirstTeam || m_Rows[index].cpu != m_PageFirstCpu) continue;
		page = static_cast<size_t>(std::upper_bound(pageStarts.begin(), pageStarts.end(), index) - pageStarts.begin()) - 1;
		break;
	}
	if (m_PageTurn) page = (page + 1) % pageStarts.size();
	m_PageTurn = false;
	m_PageStart = m_Rows.empty() ? 0 : pageStarts[page];
	m_PageFirstPeer = m_Rows.empty() ? 0 : m_Rows[m_PageStart].peer;
	m_PageFirstTeam = m_Rows.empty() ? 0 : m_Rows[m_PageStart].team;
	m_PageFirstCpu = !m_Rows.empty() && m_Rows[m_PageStart].cpu;
	const size_t shown = (page + 1 < pageStarts.size() ? pageStarts[page + 1] : m_Rows.size()) - m_PageStart;
	std::string own;
	std::vector<std::string> lines;
	for (const auto& member: snapshot.members) {
		const bool self = !member.cpu && (member.isLocal || member.peerId == snapshot.localPeerId);
		const bool listed = std::any_of(m_Rows.begin(), m_Rows.end(), [&](const PanelRow& row) { return row.peer == member.peerId; });
		if (self) {
			own = DisplayName(ShownName(member)) + " (you)  /  Team " + std::to_string(member.team + 1) + "  /  " + NetPlayerPresentation::State(member);
			if (const auto view = g_NetMatchService.GetSeatView(member.peerId); view && view->seat.holdCause != NetSeatHoldCause::None) {
				NetH4ModerationSeat held;
				held.holdCause = view->seat.holdCause;
				own += "  /  " + NetModerationUx::HoldCause(held);
			}
		} else if (!listed) {
			lines.push_back(DisplayName(ShownRow(member)));
		}
	}
	for (size_t index = 0; index < m_Rows.size(); ++index) {
		if (index < m_PageStart || index >= m_PageStart + shown) lines.push_back(m_Rows[index].name + "  /  " + m_Rows[index].state);
	}
	int rowsHeight = 0;
	for (size_t slot = 0; slot < shown; ++slot) rowsHeight += RowHeight(SlotRow(slot), inner);
	const int rosterLines = 1 + static_cast<int>(lines.size());
	std::string roster = FitLine(m_LabelFont, own, m_Roster->GetWidth());
	for (const std::string& line: lines) roster += "\n" + FitLine(m_LabelFont, line, m_Roster->GetWidth());
	// The roster takes what the page leaves, its first line at least, and scrolls for the rest.
	const int rosterHeight = std::max(lineHeight, std::min(rosterLines * lineHeight, statusTop - 2 - 40 - 4 - rowsHeight));
	Place(m_Roster, 10, 40);
	if (m_Roster->GetHeight() != rosterHeight) m_Roster->Resize(m_Roster->GetWidth(), rosterHeight);
	const bool scrolls = rosterLines * lineHeight > rosterHeight;
	m_Roster->SetVerticalOverflowScroll(scrolls);
	m_Roster->ActivateDeactivateOverflowScroll(scrolls);
	if (m_Roster->GetText() != roster) m_Roster->SetText(roster);
	m_Roster->SetVisible(true);
	int cursor = 40 + rosterHeight + 4;
	for (size_t slot = 0; slot < m_Seats.size(); ++slot) {
		if (slot < shown) {
			cursor += FillRow(m_Seats[slot], SlotRow(slot), slot, cursor);
		} else {
			HideRow(m_Seats[slot]);
		}
	}
	m_RowsShown = shown;
	// More players turns the page; it names where the host is, so a newcomer sees there is more.
	const bool paged = pageStarts.size() > 1;
	if (paged) {
		const int moreX = 10 + m_OptionsToggle->GetWidth() + 6;
		const int moreRoom = std::max(60, m_Close->GetRelXPos() - 6 - moreX);
		const std::string where = " (" + std::to_string(page + 1) + " of " + std::to_string(pageStarts.size()) + ")";
		Caption(m_More, m_LabelFont, "More players", "", m_LabelFont->CalculateWidth("More players" + where) + 20 <= moreRoom ? where : "", moreRoom);
		Place(m_More, moreX, m_Close->GetRelYPos());
	}
	Show(m_More, paged);
	DropStaleArmed();
	DropStalePress(true);
	const std::string status = m_PanelStatus.empty() ? m_Model.GetStatusText() : m_PanelStatus;
	const std::string wrapped = WrapWhole(m_LabelFont, status, m_Status->GetWidth());
	if (m_Status->GetText() != wrapped) m_Status->SetText(wrapped);
	m_Status->SetVisible(!status.empty());
}

void NetModerationGUI::DropStaleArmed() {
	if (!m_Armed) return;
	const bool here = m_Armed->slot < m_RowsShown && SlotRow(m_Armed->slot).peer == m_Armed->peer && SlotRow(m_Armed->slot).seat &&
	                  SlotRow(m_Armed->slot).seat->stableSeat == m_Armed->stableSeat && SlotRow(m_Armed->slot).seat->incarnation == m_Armed->incarnation &&
	                  NetSelectModerationSeat(*SlotRow(m_Armed->slot).seat, m_Armed->selection.applicant) == m_Armed->selection;
	if (here) return;
	// The confirming press would land on someone else now: the action is off, and the host is told why.
	m_Armed.reset();
	m_PanelStatus = c_ListChanged;
}

void NetModerationGUI::DropStalePress(bool mouseDown) {
	if (!m_Press) return;
	if (mouseDown && m_Press->slot < m_RowsShown && SameIdentity(SlotRow(m_Press->slot), m_Press->row)) return;
	if (auto* button = dynamic_cast<GUIButton*>(const_cast<GUIControl*>(m_Press->button))) button->SetPushed(false);
	m_Press.reset();
}

void NetModerationGUI::PressArmed(const PanelRow& row, size_t slot, Armed::Kind kind) {
	if (!row.seat) return;
	const NetH4ModerationSeat& seat = *row.seat;
	const NetPeerId applicant = row.decision ? row.decision->applicant : c_InvalidNetPeerId;
	const bool targetsApplicant = kind == Armed::Kind::DeclineApplicant || kind == Armed::Kind::BanApplicant;
	const auto selection = NetSelectModerationSeat(seat, (targetsApplicant || kind == Armed::Kind::Let) ? applicant : c_InvalidNetPeerId);
	if (targetsApplicant && selection.applicant == c_InvalidNetPeerId) return;
	const bool second = m_Armed && m_Armed->kind == kind && m_Armed->slot == slot && m_Armed->peer == row.peer && m_Armed->stableSeat == seat.stableSeat &&
	                    m_Armed->incarnation == seat.incarnation && m_Armed->applicant == applicant && m_Armed->selection == selection && PanelNowMs() <= m_Armed->untilMs;
	if (!second) {
		m_Armed = Armed{kind, slot, row.peer, seat.stableSeat, seat.incarnation, applicant, PanelNowMs() + 6000};
		m_Armed->selection = selection;
		const std::string name = row.name;
		if (targetsApplicant) {
			const auto newcomer = NetModerationUx::ApplicantName(*row.decision);
			m_PanelStatus = kind == Armed::Kind::DeclineApplicant ? "Decline " + newcomer + "'s request; " + name + " keeps this place - press again"
			    : "Ban " + newcomer + " from this session; " + name + " keeps this place - press again";
		} else if (kind == Armed::Kind::Let) {
			const std::string newcomer = row.decision ? NetModerationUx::ApplicantName(*row.decision) : std::string("the player");
			m_PanelStatus = row.opened ? newcomer + " takes this open place - press again to confirm"
			                           : newcomer + " takes " + name + "'s place and " + name + " cannot return to it - press again to confirm";
		} else if (kind == Armed::Kind::Remove) {
			m_PanelStatus = name + " loses the place and the AI keeps playing it - press again to remove";
		} else {
			m_PanelStatus = name + " loses the place and cannot join this session again - press again to ban";
		}
		return;
	}
	m_Armed.reset();
	if (kind == Armed::Kind::Let) {
		m_PanelStatus.clear();
		m_ActionResult = m_Model.Act(*row.decision, NetModerationAction::Substitute);
		return;
	}
	const NetParticipantRemovalAction action = kind == Armed::Kind::Remove || kind == Armed::Kind::DeclineApplicant ? NetParticipantRemovalAction::Kick : NetParticipantRemovalAction::BanSession;
	const NetKickBanResult result = g_NetMatchService.RemoveParticipant(selection, action);
	if (targetsApplicant) {
		const auto newcomer = NetModerationUx::ApplicantName(*row.decision);
		m_PanelStatus = result == NetKickBanResult::Ok ? (kind == Armed::Kind::DeclineApplicant ? newcomer + "'s request was declined." : newcomer + " was banned from this session.")
		    : "Request unchanged: " + RemovalWords(result) + ".";
		return;
	}
	const char* done = kind == Armed::Kind::Remove ? " was removed from the match." : " was banned from this session.";
	if (result == NetKickBanResult::Ok) {
		m_PanelStatus = row.name + done;
	} else if (result == NetKickBanResult::Queued) {
		m_PanelStatus = row.name + (kind == Armed::Kind::Remove ? " will be removed as the match starts." : " will be banned as the match starts.");
	} else {
		m_PanelStatus = row.name + (kind == Armed::Kind::Remove ? " was not removed: " : " was not banned: ") + RemovalWords(result) + ".";
	}
	System::PrintDiagnosticLine("[net-moderation] " + std::string(kind == Armed::Kind::Remove ? "remove" : "ban") + " seat=" + std::to_string(seat.stableSeat) +
	                            " result=" + NetKickBanResultName(result));
}

bool NetModerationGUI::HandleEvents() {
	// The manager hands its events out newest first; read them as they happened, so a release's command comes before its unpush.
	std::vector<GUIEvent> events;
	for (GUIEvent event; m_Controls->GetEvent(&event);) events.push_back(event);
	for (auto it = events.rbegin(); it != events.rend(); ++it) {
		GUIEvent& event = *it;
		const auto* control = event.GetControl();
		if (event.GetType() == GUIEvent::Command && control == m_Close) { SetOpen(false); continue; }
		if (event.GetType() == GUIEvent::Command && control == m_OptionsToggle) {
			// A view that closes takes its press and its armed action with it.
			if (!m_OptionsView) { m_OptionsView = true; m_ConnectionView = false; }
			else if (!m_ConnectionView) m_ConnectionView = true;
			else { m_OptionsView = false; m_ConnectionView = false; }
			DropStalePress(false);
			m_Armed.reset();
			m_PanelStatus.clear();
			continue;
		}
		if (event.GetType() == GUIEvent::Command && control == m_More) {
			m_PageTurn = true;
			continue;
		}
		if (!m_Open) continue;
		for (size_t slot = 0; slot < m_Seats.size() && slot < m_RowsShown; ++slot) {
			auto& widgets = m_Seats[slot];
			if (control == widgets.requests) {
				// A pick from the list is the request Let names.
				if (event.GetType() == GUIEvent::Notification && event.GetMsg() == GUIListBox::Select && SlotRow(slot).decision) {
					const int picked = widgets.requests->GetSelectedIndex();
					if (picked >= 0) m_Model.ChooseApplicant(*SlotRow(slot).decision, static_cast<size_t>(picked));
					m_Armed.reset();
					m_PanelStatus.clear();
				}
				continue;
			}
			const bool action = std::find(widgets.actions.begin(), widgets.actions.end(), control) != widgets.actions.end();
			if (!action && control != widgets.remove && control != widgets.ban && control != widgets.declineApplicant && control != widgets.banApplicant) continue;
			if (event.GetType() == GUIEvent::Notification && event.GetMsg() == GUIButton::Pushed) {
				// A new press replaces any kept one: only the player this control shows now can be acted on.
				m_Press = Press{control, slot, SlotRow(slot)};
			} else if (event.GetType() == GUIEvent::Command && m_Press && m_Press->button == control) {
				const Press pressed = *m_Press;
				m_Press.reset();
				// The release acts on the player the control shows at the release, and only if it is the one pressed.
				if (pressed.slot != slot || !SameIdentity(SlotRow(slot), pressed.row)) {
					m_Armed.reset();
					m_PanelStatus = c_ListChanged;
					continue;
				}
				const PanelRow row = SlotRow(slot);
				if (control == widgets.declineApplicant) {
					PressArmed(row, slot, Armed::Kind::DeclineApplicant);
				} else if (control == widgets.banApplicant) {
					PressArmed(row, slot, Armed::Kind::BanApplicant);
				} else if (control == widgets.remove) {
					if (!row.opened) PressArmed(row, slot, Armed::Kind::Remove);
				} else if (control == widgets.ban) {
					if (!row.opened) PressArmed(row, slot, Armed::Kind::Ban);
				} else if (control == widgets.actions[static_cast<size_t>(NetModerationAction::Substitute)]) {
					if (row.decision) PressArmed(row, slot, Armed::Kind::Let);
				} else if (row.decision) {
					m_Armed.reset();
					m_PanelStatus.clear();
					for (size_t verb = 0; verb < widgets.actions.size(); ++verb) {
						if (control == widgets.actions[verb]) m_ActionResult = m_Model.Act(*row.decision, static_cast<NetModerationAction>(verb));
					}
				}
			} else if (event.GetType() == GUIEvent::Notification && event.GetMsg() == GUIButton::UnPushed &&
			           !static_cast<GUIButton*>(event.GetControl())->IsCaptured()) {
				m_Press.reset();
			}
		}
	}
	return !events.empty();
}

void NetModerationGUI::Update() {
	// Without the cost lever the panel reads no clock for it.
	struct FrameTime {
		FrameCost* cost;
		std::chrono::steady_clock::time_point started = cost ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		~FrameTime() {
			if (!cost) return;
			const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
			cost->frameNs += ns;
			cost->framePartsUs += ns / 1000;
		}
	} frameTime{m_Cost.get()};
	if (m_Cost) ++m_Cost->updates;
	// The loop comes by many times a drawn frame while it waits: the panel follows the match once a drawn frame, from the snapshot
	// that frame read, or every 16 ms from its own when nothing draws.
	const uint64_t nowMs = PanelNowMs();
	const bool frameDue = !m_FrameSnapshot || m_ReadDrawSerial != m_DrawSerial || nowMs - m_ReadMs >= 16;
	if (frameDue) {
		if (!m_FrameSnapshot || m_ReadDrawSerial == m_DrawSerial) {
			m_FrameSnapshot = std::make_unique<NetLobbySnapshot>(g_NetMatchService.GetLobbySnapshot());
			NetPlayerPresentation::Remember(*m_FrameSnapshot, g_SettingsMan.GetNetworkDisplayName());
		}
		m_ReadDrawSerial = m_DrawSerial;
		m_ReadMs = nowMs;
	}
	const NetLobbySnapshot& snapshot = *m_FrameSnapshot;
	if (frameDue) {
		NoteSharedNames(snapshot);
		const uint64_t frame = ScenarioRunner::GetLockstepCompletedFrame();
		if (frame < m_DepartureFrame || snapshot.serviceState != "Running") m_AnnouncedAISeats.clear();
		m_DepartureFrame = frame;
		if (snapshot.serviceState == "Running") {
			for (const auto& member: snapshot.members) {
				if (member.cpu || !NetPlayerPresentation::Seated(member.peerId) || !ScenarioRunner::IsLockstepPeerGone(member.peerId, frame)) continue;
				if (std::find(m_AnnouncedAISeats.begin(), m_AnnouncedAISeats.end(), member.peerId) != m_AnnouncedAISeats.end()) continue;
				m_AnnouncedAISeats.push_back(member.peerId);
				ScenarioRunner::PushNetUiToast("seat_left_ai", DisplayName(ShownName(member)) + ": " + NetPlayerPresentation::State(member));
			}
		}
		if (m_Open && snapshot.serviceState != "Running" && snapshot.serviceState != "Starting" && snapshot.serviceState != "ReadyToLaunch") SetOpen(false);
	}
	UpdateMatchChat(snapshot, frameDue);
	if (!m_Open) return;
	// The controls take every pass the mouse changed on, since a click's edge lasts one pass, and otherwise one a drawn frame.
	if (!MouseChanged() && !frameDue) return;
	g_UInputMan.TrapMousePos(false);
	m_Controls->Update();
	const bool acted = HandleEvents();
	// A press the mouse let go of without its command acts on nobody, wherever the release landed.
	int mouseEvents[3] = {}, mouseStates[3] = {};
	m_Input->GetMouseButtons(mouseEvents, mouseStates);
	const bool pressed = m_Press.has_value();
	DropStalePress(mouseStates[0] == GUIInput::Down);
	// The rows are laid out again at once only after the host did something; the draw lays them out when the match changes them.
	if (acted || pressed != m_Press.has_value()) Refresh();
}

bool NetModerationGUI::MouseChanged() {
	const Vector mouse = g_UInputMan.GetAbsoluteMousePosition(-1);
	bool changed = mouse.GetFloorIntX() != m_LastMouseX || mouse.GetFloorIntY() != m_LastMouseY;
	m_LastMouseX = mouse.GetFloorIntX();
	m_LastMouseY = mouse.GetFloorIntY();
	changed = changed || g_UInputMan.MouseWheelMovedByPlayer(-1) != 0;
	const auto& change = g_UInputMan.GetMouseChange(-1);
	for (int button = 1; button <= 3; ++button) {
		changed = changed || change[button] || g_UInputMan.MouseButtonPressedSim(button, -1) || g_UInputMan.MouseButtonReleasedSim(button, -1);
	}
	return changed;
}

void NetModerationGUI::DrawRoster(const NetLobbySnapshot& snapshot) {
	NoteSharedNames(snapshot);
	AllegroBitmap bitmap(g_FrameMan.GetBackBuffer32());
	auto* font = g_FrameMan.GetSmallFont(true);
	const bool menuLobby = PostMatchLobbySurfaces();
	int x = 8;
	int widest = std::max(1, g_WindowMan.GetResX() - 16);
	if (menuLobby) {
		// In the menu loop the lobby's own box is the column: the seats reading takes the widest
		// gutter beside it rather than the screen's corner.
		EditorArea area;
		LobbyMenuColumn(area);
		int freeLeft = 0, freeRight = g_WindowMan.GetResX();
		area.FreeSpan(0, g_WindowMan.GetResY(), g_WindowMan.GetResX(), freeLeft, freeRight);
		x = freeLeft + 8;
		widest = std::min(widest, std::max(120, freeRight - freeLeft - 16));
	}
	const int y = std::max(menuLobby ? c_StripBandBottom + c_PanelGap : 8, g_FrameMan.GetLargeFont()->GetFontHeight() + 4);
	std::string text = "Players  [F6]";
	// The announced input delay rides the corner box so the HUD shows what the lobby showed.
	if (!snapshot.inputDelayText.empty()) text += "\n" + snapshot.inputDelayText;
	for (const auto& member: snapshot.members) {
		text += "\n" + DisplayName(ShownRow(member));
	}
	// The cap column: a token the box cannot grow past is ellipsized before the width rule measures it.
	const int column = std::max(1, widest - 12);
	text = FitTokens(font, text, column);
	const int width = std::min(widest, RosterBoxWidth(font, text));
	m_RosterWrap.source = text;
	m_RosterWrap.textWidth = width - 12;
	m_RosterWrap.longestWord = LongestWordWidth(font, text);
	m_RosterWrap.capWidth = std::max(0, widest - 12);
	m_RosterWrap.active = true;
	text = WrapText(font, text, width - 12);
	m_RosterWrap.wrapped = text;
	const int height = font->CalculateHeight(text) + 12;
	m_RosterRect = {x, y, width + 1, height + 1, true};
	rectfill(g_FrameMan.GetBackBuffer32(), x, y, x + width + 8, y + height, makeacol32(20, 22, 27, 255));
	font->DrawAligned(&bitmap, x + 6, y + 6, text, GUIFont::Left, GUIFont::Top);
}

int NetModerationGUI::RosterBoxWidth(GUIFont* font, const std::string& text) {
	// The stock width, grown so the longest word the box must show lands whole on a line of its own.
	const int cap = std::max(1, g_WindowMan.GetResX() - 16);
	return std::min(cap, std::max(std::min(412, cap), LongestWordWidth(font, text) + 12));
}

bool NetModerationGUI::AutomationWrapLines(const std::string& text, std::string& wrapped, int& boxWidth) const {
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	if (!font) return false;
	const int column = std::max(1, g_WindowMan.GetResX() - 28);
	const std::string fitted = FitTokens(font, text, column);
	boxWidth = RosterBoxWidth(font, fitted);
	wrapped = WrapText(font, fitted, boxWidth - 12);
	return true;
}

bool NetModerationGUI::MatchStatusWanted() const {
	if (g_SettingsMan.GetNetworkShowDiagnostics() || ScenarioRunner::IsLockstepLocalMachineSlow() || OwnRosterSeatHeld() || ScenarioRunner::IsLockstepOwnSeatHeld()) return true;
	switch (g_SettingsMan.GetNetworkMatchStatusMode()) {
		case SettingsMan::NetworkMatchStatusMode::Off: return false;
		case SettingsMan::NetworkMatchStatusMode::Always: return true;
		default: break;
	}
	// The countdown is still a pause state, so it must not blink the widget off.
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	bool active = m_Open || snapshot.statusText.starts_with("Host lost") || g_NetMatchService.IsMatchResyncing() || ScenarioRunner::IsLockstepPaused() || ScenarioRunner::GetLockstepResumeCountdown() > 0;
	if (!active) {
		// The seats placing their brains hold the world too, and the player is waiting on exactly that.
		std::string placementNames;
		int placed = 0, seats = 0;
		const auto* setupActivity = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
		active = setupActivity && setupActivity->DescribeLockstepPlacementWait(placementNames, placed, seats);
	}
	if (!active && ScenarioRunner::IsLockstepControllerSyncActive()) {
		if (static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) > ScenarioRunner::GetLockstepCompletedFrame()) {
			active = true;
		} else {
			std::string holdWho;
			uint32_t holdSeconds = 0;
			active = ScenarioRunner::DescribeLockstepHoldPause(holdWho, holdSeconds);
		}
	}
	const long long nowUs = g_TimerMan.GetAbsoluteTime();
	if (active) {
		m_AutoShowUntilUs = nowUs + 3000000;
	}
	return active || nowUs < m_AutoShowUntilUs;
}

void NetModerationGUI::DrawMatchStatus(const NetLobbySnapshot& snapshot) {
	NoteSharedNames(snapshot);
	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	if (ScenarioRunner::IsLockstepControllerSyncActive()) {
		m_MatchDelayFrames = ScenarioRunner::GetLockstepLocalInputDelay();
		m_BaseDelayFrames = ScenarioRunner::GetLockstepInputDelayFrames();
	}
	char metrics[128];
	std::snprintf(metrics, sizeof(metrics), "delay %u ticks / %.1f ms", static_cast<unsigned>(m_MatchDelayFrames), m_MatchDelayFrames * g_TimerMan.GetDeltaTimeMS());
	// The summary reads the links the per-player lines list - the widest a host has, a client's own to its host - so the
	// two never disagree; a link not measured yet reads as unknown in both.
	std::optional<uint32_t> ping;
	for (const auto& member: snapshot.members) {
		if (member.cpu || member.isLocal || !PingKnown(member) || (!snapshot.isHost && member.peerId != snapshot.hostPeerId)) continue;
		ping = std::max(ping.value_or(0), member.pingMs);
	}
	const bool hostLost = snapshot.statusText.starts_with("Host lost") || snapshot.serviceState == "HostLost" || snapshot.serviceState == "Migrating";
	// Sim updates against wall time over the last second, so a stalled or paused match reads its true pace
	static long long s_paceMarkUs = 0;
	static uint64_t s_paceMarkUpdates = 0;
	static double s_paceTps = 0.0;
	const long long paceNowUs = g_TimerMan.GetAbsoluteTime();
	const uint64_t paceUpdates = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	const long long paceSpanUs = paceNowUs - s_paceMarkUs;
	if (s_paceMarkUs <= 0 || paceSpanUs < 0) {
		s_paceMarkUs = paceNowUs;
		s_paceMarkUpdates = paceUpdates;
	} else if (paceSpanUs >= 1000000 || (s_paceTps == 0.0 && paceSpanUs > 0)) {
		s_paceTps = static_cast<double>(paceUpdates - s_paceMarkUpdates) * 1000000.0 / static_cast<double>(paceSpanUs);
		if (paceSpanUs >= 1000000) {
			s_paceMarkUs = paceNowUs;
			s_paceMarkUpdates = paceUpdates;
		}
	}
	std::string holdName;
	uint32_t holdSeconds = 0;
	const bool resyncing = g_NetMatchService.IsMatchResyncing();
	// The seats that still owe the synchronized setup editor a brain: the world is held while they place.
	std::string placementNames;
	int placed = 0, seats = 0;
	// The lobby arm has no coordinator: the lockstep reads are dead there, so the status line shows
	// the service's own text instead of a wait the match no longer owes.
	const bool menuLobby = PostMatchLobbySurfaces();
	const auto* setupActivity = menuLobby ? nullptr : dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
	const bool placing = !resyncing && setupActivity && setupActivity->DescribeLockstepPlacementWait(placementNames, placed, seats);
	const bool holdPause = !menuLobby && !resyncing && !placing && ScenarioRunner::DescribeLockstepHoldPause(holdName, holdSeconds);
	const bool missingFrames = !menuLobby && !resyncing && !placing && !holdPause && (hostLost ||
	    static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) > ScenarioRunner::GetLockstepCompletedFrame());
	const bool paused = !menuLobby && !resyncing && !placing && !holdPause && !missingFrames && ScenarioRunner::IsLockstepPaused();
	const int countdown = paused ? ScenarioRunner::GetLockstepResumeCountdown() : 0;
	const bool waiting = resyncing || placing || holdPause || missingFrames;
	// A wait that began before this seat was reclaimed is not the wait the player is in now: the round
	// was stopped for the rejoin, so the clock would read the whole absence back to them.
	if (const uint32_t reclaims = ScenarioRunner::GetLockstepSeatReclaimEpoch(); reclaims != m_StatusWaitReclaimEpoch) {
		m_StatusWaitReclaimEpoch = reclaims;
		m_StatusWaitStartedUs = 0;
	}
	if (!waiting) m_StatusWaitStartedUs = 0;
	else if (m_StatusWaitStartedUs == 0) m_StatusWaitStartedUs = paceNowUs;
	const long long currentWaitMs = waiting ? (paceNowUs - m_StatusWaitStartedUs) / 1000 : 0;
	EditorArea editor = FreeArea(backbuffer->w);
	if (menuLobby) LobbyMenuColumn(editor);
	const std::string countOnly = std::to_string(placed) + " of " + std::to_string(seats);
	const int countNeed = font->CalculateWidth(countOnly) + 14;
	const bool diagnostics = g_SettingsMan.GetNetworkShowDiagnostics();
	// What the player reads first is what is happening and who must act, in the player's words; the numbers follow behind
	// the detailed statistics setting.
	bool localUnplaced = false;
	std::vector<std::string> othersPlacing;
	if (placing) {
		// The activity names a seat by the roster's non-CPU slots in order, so the same names read here.
		std::vector<std::string> slotNames;
		if (const auto config = ScenarioRunner::GetLockstepMatchConfig()) {
			for (const NetMatchPlayerSlot& slot: config->players) {
				if (!slot.cpu) slotNames.push_back(slot.displayName);
			}
		}
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (!(setupActivity->IsSeatActive(player) && setupActivity->IsHumanSeat(player)) || setupActivity->IsReadyToStart(player)) continue;
			if (setupActivity->IsLocalHumanSeat(player)) {
				localUnplaced = true;
			} else {
				const size_t slot = static_cast<size_t>(player);
				othersPlacing.push_back(DisplayName(slot < slotNames.size() && !slotNames[slot].empty() ? slotNames[slot] : "Player " + std::to_string(player + 1)));
			}
		}
	}
	// This player's own seat on its way back, and the other seats the AI plays while their players are away.
	const bool ownRejoin = !menuLobby && !hostLost && (resyncing || ScenarioRunner::WorldCatchUpActive()) &&
	                       (OwnRosterSeatHeld() || ScenarioRunner::IsLockstepOwnSeatHeld());
	const bool ownHeld = !menuLobby && !hostLost && (OwnRosterSeatHeld() || ScenarioRunner::IsLockstepOwnSeatHeld());
	std::string ownHeldCause;
	if (ownHeld) {
		if (const auto view = g_NetMatchService.GetSeatView(snapshot.localPeerId)) {
			NetH4ModerationSeat held;
			held.holdCause = view->seat.holdCause;
			ownHeldCause = NetModerationUx::HoldCause(held);
		}
	}
	std::vector<std::string> heldNames, heldCauses, returningNames, lostNames;
	if (!menuLobby && !hostLost) {
		for (const auto& member: snapshot.members) {
			if (member.cpu || member.isLocal || member.peerId == snapshot.localPeerId) continue;
			// A place its player gave up and nobody plays is no longer a player of this match.
			const std::string state = NetPlayerPresentation::State(member);
			const std::string name = DisplayName(ShownName(member));
			if (state.find("AI in control") != std::string::npos) heldNames.push_back(name);
			else if (state == "Rejoining" || state == "Joining") returningNames.push_back(name);
			else if (state == "Disconnected") lostNames.push_back(name);
		}
		if (!heldNames.empty() && snapshot.isHost) {
			for (const NetH4ModerationSeat& seat: g_NetMatchService.GetModerationSeats()) {
				const std::string cause = NetModerationUx::HoldCause(seat);
				const std::string name = DisplayName(ShownName(seat.lockstepPeerId, NetPlayerPresentation::Name(seat.lockstepPeerId, seat.displayName)));
				if (!seat.cpu && !cause.empty() && std::find(heldNames.begin(), heldNames.end(), name) != heldNames.end()) heldCauses.push_back(name + ": " + cause);
			}
		}
	}
	const std::string missingPeers = missingFrames && !hostLost ? DisplayName(ScenarioRunner::GetLockstepMissingPeers()) : std::string();
	// The headline names whoever the reading is about; a short line fits each name into nameRoom pixels, 0 keeps them whole.
	const auto headline = [&](int nameRoom) -> std::string {
		const auto fit = [&](const std::string& name) { return nameRoom > 0 ? FitLine(font, name, nameRoom) : DisplayName(name); };
		const auto fitAll = [&](const std::vector<std::string>& names) {
			std::vector<std::string> fitted;
			for (const std::string& name: names) fitted.push_back(fit(name));
			return NamesInWords(fitted);
		};
		if (menuLobby) return "In the lobby";
		if (hostLost) return snapshot.statusText.starts_with("Changing hosts") ? "Changing hosts - the match picks up in a moment" : "Host lost - contacting the next host...";
		// The toast and the full-screen wait say the rest; the box's line never repeats theirs.
		if (ownRejoin) return "Rejoining - catching up with the match";
		if (ownHeld) return "The AI is playing for you";
		if (resyncing) return "Match repair in progress";
		if (placing) {
			if (localUnplaced) return "Place your brain";
			if (othersPlacing.empty()) return "All brains placed";
			return "Waiting for " + fitAll(othersPlacing) + (othersPlacing.size() == 1 ? " to place their brain" : " to place their brains");
		}
		if (holdPause) return "Waiting for " + fit(holdName.empty() ? std::string("a player") : holdName) + " to return - " + std::to_string(holdSeconds) + " s left";
		if (missingFrames) {
			if (missingPeers.empty()) return "Waiting for the other players' connections";
			return missingPeers.find(", ") == std::string::npos ? "Waiting for " + fit(missingPeers) + "'s connection" : "Waiting for the connections of " + fit(missingPeers);
		}
		if (paused) return countdown > 0 ? "Resuming in " + std::to_string((countdown + 59) / 60) + " s" : "Match paused - press P to resume";
		if (!heldNames.empty()) return "The AI is playing for " + fitAll(heldNames);
		if (!returningNames.empty()) return fitAll(returningNames) + (returningNames.size() == 1 ? " is joining the match" : " are joining the match");
		if (!lostNames.empty()) return fitAll(lostNames) + " lost connection";
		return "Everyone is connected";
	};
	// The lines under the headline: the wait so far, who else is placing, why a seat is held and how each player is connected.
	const auto details = [&](int textWidth) {
		std::vector<std::string> lines;
		if (!ownHeldCause.empty()) lines.push_back("Your seat: " + ownHeldCause);
		if (hostLost || (missingFrames && !paused)) lines.push_back("Waiting " + SecondsInWords(currentWaitMs));
		if (placing) {
			if (localUnplaced && !othersPlacing.empty()) lines.push_back(FitLine(font, "Also placing: " + NamesInWords(othersPlacing), textWidth));
			lines.push_back(countOnly + " brains placed");
		}
		if (!resyncing && !placing && !hostLost) {
			for (const std::string& cause: heldCauses) lines.push_back(cause);
		}
		if (!diagnostics && !hostLost && !menuLobby) {
			for (const auto& member: snapshot.members) {
				if (member.cpu || member.isLocal || member.connectedRoute.empty() || !member.connected || NetPlayerPresentation::Departed(member.peerId)) continue;
				lines.push_back(FitName(font, ShownName(member), textWidth / 2) +
				                (member.connectedRoute == "relay" ? ": connected through a relay" : ": direct connection"));
			}
		}
		return lines;
	};
	// The box is laid out again only when an input it reads changes; otherwise the kept box and line are drawn as they were.
	const auto layoutKey = [&](char mode, std::initializer_list<long long> geometry) {
		char pace[32];
		std::snprintf(pace, sizeof(pace), "%.1f", s_paceTps);
		std::string key{mode};
		for (const long long value: {static_cast<long long>(backbuffer->w), static_cast<long long>(backbuffer->h), static_cast<long long>(reinterpret_cast<intptr_t>(font)),
		                             static_cast<long long>(m_Open), static_cast<long long>(diagnostics), static_cast<long long>(menuLobby),
		                             static_cast<long long>(hostLost), static_cast<long long>(resyncing), static_cast<long long>(placing), static_cast<long long>(placed),
		                             static_cast<long long>(seats), static_cast<long long>(holdPause), static_cast<long long>(holdSeconds), static_cast<long long>(missingFrames),
		                             static_cast<long long>(paused), static_cast<long long>((countdown + 59) / 60), static_cast<long long>(waiting),
		                             hostLost || missingFrames ? currentWaitMs / 100 : 0LL, static_cast<long long>(m_MatchDelayFrames), static_cast<long long>(m_BaseDelayFrames),
		                             ping ? static_cast<long long>(*ping) : -1LL, static_cast<long long>(snapshot.isHost), static_cast<long long>(snapshot.hostPeerId),
		                             static_cast<long long>(localUnplaced), static_cast<long long>(ownRejoin), static_cast<long long>(ownHeld)}) {
			key += ' ' + std::to_string(value);
		}
		for (const long long value: geometry) key += ' ' + std::to_string(value);
		key += '|' + std::string(pace) + '|' + placementNames + '|' + holdName + '|' + snapshot.statusText + '|' + m_StatusProbeLine + '|' + missingPeers;
		for (const std::string& name: othersPlacing) key += '|' + name;
		for (const std::string& cause: heldCauses) key += '|' + cause;
		key += "|own " + ownHeldCause;
		for (const std::string& name: heldNames) key += "|ai " + name;
		for (const auto& member: snapshot.members) {
			key += '|' + std::to_string(member.peerId) + ',' + member.displayName + ',' + std::to_string(member.team) + ',' + std::to_string(member.cpu) + ',' +
			       std::to_string(member.isLocal) + ',' + std::to_string(member.ready) + ',' + std::to_string(member.connected) + ',' + std::to_string(member.pingMs) + ',' + std::to_string(member.pingMeasured) + ',' +
			       std::to_string(member.inputDelayFrames) + ',' + std::to_string(member.waits) + ',' + std::to_string(member.longestWaitMs) + ',' +
			       std::to_string(member.aiHeld) + ',' + std::to_string(member.dropped) + ',' + std::to_string(member.reclaiming) + ',' + member.connectedRoute + ',' +
			       member.statusLine + ',' + ShownName(member) + ',' + NetPlayerPresentation::State(member);
		}
		return key;
	};
	// Test lever: lay the box out every frame and name any frame whose kept key would have drawn a different text.
	static const bool s_CheckStatusCache = [] { const char* value = std::getenv("CCCP_TEST_STATUS_CACHE_CHECK"); return value && std::string(value) == "1"; }();
	const auto checkKept = [&](bool keyKept, const std::string& keptText) {
		if (s_CheckStatusCache && keyKept && m_NetStatus->GetText() != keptText) {
			System::PrintDiagnosticLine("[status-cache] stale tick=" + std::to_string(g_TimerMan.GetSimUpdateCount()) + " kept=\"" + keptText + "\" fresh=\"" + m_NetStatus->GetText() + "\"");
		}
	};
	const auto drawKept = [&] {
		const OverlayRect& kept = m_StatusLayoutRect;
		m_NetStatusBox->SetVisible(true);
		m_StatusRect = kept;
		AllegroBitmap bitmap(backbuffer);
		rectfill(backbuffer, kept.x, kept.y, kept.x + kept.width - 1, kept.y + kept.height - 1, makeacol32(20, 22, 27, 255));
		rect(backbuffer, kept.x, kept.y, kept.x + kept.width - 1, kept.y + kept.height - 1, makeacol32(59, 65, 83, 255));
		hline(backbuffer, kept.x + 1, kept.y + 1, kept.x + kept.width - 2, waiting ? makeacol32(170, 120, 0, 255) : makeacol32(108, 118, 168, 255));
		m_NetStatus->Draw(&bitmap, false);
		m_StatusWrap = m_StatusLayoutWrap;
		RecordStatusObservation(snapshot, hostLost, currentWaitMs);
	};
	if (backbuffer->h < c_CompactMaxHeight && (m_Open || !g_SettingsMan.GetNetworkShowDiagnostics())) {
		// The short-screen layout is one line in the gap between the funds block and the controller icon;
		// while the editor holds the world it takes the widest column-free gap, or the top band when none fits.
		const int fullHeight = font->GetFontHeight() + 7;
		int height = fullHeight;
		int y = editor.editing && !m_Open ? backbuffer->h - height - 2 : 2;
		bool topBand = false;

		int freeLeft = 152, freeRight = backbuffer->w - 40;
		if (editor.editing || menuLobby) {
			editor.FreeSpan(y, y + height, backbuffer->w, freeLeft, freeRight);
			freeLeft += 4;
			freeRight -= 4;
			if (freeRight - freeLeft < countNeed) {
				topBand = true;
				y = 2;
				height = fullHeight;
				freeLeft = 0;
				freeRight = backbuffer->w;
			}
		}
		const int maxTextWidth = std::max(0, freeRight - freeLeft - 14);
		const std::string stripKey = layoutKey('c', {y, height, fullHeight, freeLeft, freeRight, topBand, maxTextWidth});
		const bool stripKept = stripKey == m_StatusLayoutKey;
		if (stripKept && !s_CheckStatusCache) {
			drawKept();
			return;
		}
		const std::string stripKeptText = s_CheckStatusCache ? m_NetStatus->GetText() : std::string();
		const std::string pingText = !hostLost && ping ? PingWords(*ping) : "--";
		char tail[96];
		std::snprintf(tail, sizeof(tail), " / delay %u / RTT %s ms / PACE %.1f tps", static_cast<unsigned>(m_MatchDelayFrames), pingText.c_str(), s_paceTps);
		auto compose = [&](bool withTail, bool withKey, bool shortenNames) {
			const std::string count = placing ? " / " + countOnly : std::string();
			const std::string key = withKey ? " / F6: Players" : std::string();
			const std::string numbers = withTail && diagnostics ? std::string(tail) : std::string();
			const int room = maxTextWidth - font->CalculateWidth(headline(1) + count + key + numbers) + font->CalculateWidth("...");
			return headline(shortenNames ? std::max(24, room) : 0) + count + key + numbers;
		};
		// An open picker leaves a short screen 280 px, so the line gives way in this order: the whole line,
		// then a shortening pass, then whatever remains. The count ends the line, so it outlives all of
		// them - the last resort drops everything but it rather than clip it off the end.
		if (topBand) {
			m_StripText = countOnly;
		} else {
			m_StripText = compose(true, true, false);
			if (font->CalculateWidth(m_StripText) > maxTextWidth) {
				m_StripText = compose(false, true, false);
			}
			if (font->CalculateWidth(m_StripText) > maxTextWidth) {
				m_StripText = compose(false, false, false);
			}
			if (font->CalculateWidth(m_StripText) > maxTextWidth) {
				m_StripText = compose(false, false, true);
			}
			if (placing && font->CalculateWidth(m_StripText) > maxTextWidth) {
				m_StripText = font->CalculateWidth(countOnly) <= maxTextWidth ? countOnly : m_StripText;
			}
		}
		const int width = std::max(1, std::min(std::max(0, freeRight - freeLeft), font->CalculateWidth(m_StripText) + 14));
		const int x = std::max(0, std::min(backbuffer->w - width, freeLeft + std::max(0, (freeRight - freeLeft - width) / 2)));
		m_NetStatusBox->Move(x, y);
		if (m_NetStatusBox->GetWidth() != width || m_NetStatusBox->GetHeight() != height) m_NetStatusBox->Resize(width, height);
		m_NetStatusBox->SetVisible(true);
		m_NetStatus->SetFont(font);
		// GUILabel::Move is in screen coordinates, so the inset is measured from the strip itself. A strip
		// squeezed under the seats panel centres its line instead of keeping the usual padding.
		const bool squeezed = height < fullHeight;
		const int textPad = squeezed ? std::max(0, (height - font->GetFontHeight()) / 2) : 4;
		m_NetStatus->Move(x + 7, y + textPad);
		m_NetStatus->Resize(width - 14, squeezed ? height - 2 * textPad : height - 6);
		m_NetStatus->SetText(m_StripText);
		m_StatusRect = {x, y, width, height, true};
		AllegroBitmap bitmap(backbuffer);
		rectfill(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(20, 22, 27, 255));
		rect(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(59, 65, 83, 255));
		hline(backbuffer, x + 1, y + 1, x + width - 2, waiting ? makeacol32(170, 120, 0, 255) : makeacol32(108, 118, 168, 255));
		m_NetStatus->Draw(&bitmap, false);
		checkKept(stripKept, stripKeptText);
		m_StatusLayoutKey = stripKey;
		m_StatusLayoutRect = m_StatusRect;
		m_StatusLayoutWrap = {};
		RecordStatusObservation(snapshot, hostLost, currentWaitMs);
		return;
	}
	// The box is tall enough that a picker column beside it matters wherever the rows land, so its span
	// keeps every column out rather than only the ones crossing the box's own band.
	int freeLeft = 0, freeRight = backbuffer->w;
	editor.FreeSpan(0, backbuffer->h, backbuffer->w, freeLeft, freeRight);
	int available = freeRight - freeLeft;
	bool topBand = (editor.editing || menuLobby) && available < countNeed;
	if (topBand) {
		freeLeft = 0;
		freeRight = backbuffer->w;
		available = backbuffer->w;
	}
	const int maxPanelWidth = std::max(1, available - 2 * c_StatusBoxMargin);
	const std::string panelKey = layoutKey('f', {freeLeft, freeRight, available, topBand, maxPanelWidth, editor.editing});
	const bool panelKept = panelKey == m_StatusLayoutKey;
	if (panelKept && !s_CheckStatusCache) {
		drawKept();
		return;
	}
	const std::string panelKeptText = s_CheckStatusCache ? m_NetStatus->GetText() : std::string();
	m_NetStatusBox->SetVisible(true);
	m_NetStatus->SetFont(font);
	const auto compose = [&](int textWidth) {
		std::string composed = headline(0);
		for (const std::string& line: details(textWidth)) composed += "\n" + line;
		composed += "\nF6: Players";
		if (diagnostics) {
			composed += "\n" + std::string(metrics);
			if (m_MatchDelayFrames != m_BaseDelayFrames) {
				composed += " (base " + std::to_string(m_BaseDelayFrames) + ")";
			}
			for (const auto& member: snapshot.members) {
				if (member.cpu || member.isLocal || member.connectedRoute.empty()) continue;
				composed += "\n" + FitName(font, ShownName(member), textWidth) + " / via " + member.connectedRoute;
			}
			composed += "\nRTT " + (!hostLost && ping ? PingWords(*ping) : "--") + " ms / " + (hostLost ? "host lost" : snapshot.isHost ? "max peer" : "host link");
			char pace[32];
			std::snprintf(pace, sizeof(pace), "\nPACE %.1f tps", s_paceTps);
			composed += pace;
			for (const auto& member: snapshot.members) {
				if (member.cpu) continue;
				composed += "\nP" + std::to_string(member.peerId) + ": Ping " + ((hostLost && member.peerId == snapshot.hostPeerId) || !PingKnown(member) ? "--" : PingWords(member.pingMs)) + " ms / delay " + std::to_string(member.inputDelayFrames) + " frames";
				// Each player's second row names them too, or two players' equal numbers read as one line twice.
				composed += "\nP" + std::to_string(member.peerId) + " waits " + std::to_string(member.waits) + " / longest " + std::to_string(member.longestWaitMs) + " ms";
				if (member.reclaiming || member.aiHeld || !member.connected) composed += " / " + NetPlayerPresentation::State(member);
			}
		}
		if (!m_StatusProbeLine.empty()) composed += "\n" + m_StatusProbeLine;
		return composed;
	};
	int width = std::max(1, std::min(c_StatusBoxWidth, maxPanelWidth));
	std::string text = compose(width - 12);
	// A word wider than the stock column must not hang over the panel edge: grow to hold it whole.
	if (const int need = LongestWordWidth(font, text) + 12; need > width && width < maxPanelWidth) {
		width = std::min(need, maxPanelWidth);
		text = compose(width - 12);
	}
	// Measured at the final width, so the height below is the height these rows really need.
	m_NetStatus->Resize(width - 12, backbuffer->h);
	const int column = std::max(0, width - 12);
	const std::string drawn = WrapText(font, FitTokens(font, text, std::max(1, column)), std::max(1, column));
	m_StatusWrap.source = text;
	m_StatusWrap.wrapped = drawn;
	m_StatusWrap.textWidth = column;
	m_StatusWrap.longestWord = LongestWordWidth(font, text);
	m_StatusWrap.capWidth = maxPanelWidth - 12;
	m_StatusWrap.active = true;
	m_NetStatus->SetText(drawn);
	// The box grows for a state that needs more rows than the metric ones; those keep the stock height.
	const int height = std::max(c_StatusBoxHeight, m_NetStatus->GetTextHeight() + 12);
	// The editor's own top band and picker column are its own, so the box takes the bottom of the rest.
	// A collapsed span takes the window's top band instead of a zero-width box.
	const int x = topBand ? std::max(0, (backbuffer->w - width) / 2) :
	    (editor.editing || menuLobby ? freeLeft + std::max(0, (available - width) / 2) : backbuffer->w - width - c_StatusBoxMargin);
	// In the lobby the box takes the bottom of the gutter the lobby's own box leaves, the same bottom
	// slot it takes beside the editor's column.
	int y = topBand ? 2 : ((editor.editing || menuLobby) && !m_Open ? backbuffer->h - height - c_StatusBoxMargin : c_StatusBoxTop);

	m_NetStatusBox->Move(x, y);
	if (m_NetStatusBox->GetWidth() != width || m_NetStatusBox->GetHeight() != height) m_NetStatusBox->Resize(width, height);
	m_NetStatus->Move(x + 6, y + 6);
	m_NetStatus->Resize(width - 12, height - 12);
	m_StatusRect = {x, y, width, height, true};
	AllegroBitmap bitmap(backbuffer);
	rectfill(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(20, 22, 27, 255));
	rect(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(59, 65, 83, 255));
	hline(backbuffer, x + 1, y + 1, x + width - 2, waiting ? makeacol32(170, 120, 0, 255) : makeacol32(108, 118, 168, 255));
	m_NetStatus->Draw(&bitmap, false);
	checkKept(panelKept, panelKeptText);
	m_StatusLayoutKey = panelKey;
	m_StatusLayoutRect = m_StatusRect;
	m_StatusLayoutWrap = m_StatusWrap;
	RecordStatusObservation(snapshot, hostLost, currentWaitMs);
}

void NetModerationGUI::RecordStatusObservation(const NetLobbySnapshot& snapshot, bool hostLost, long long currentWaitMs) {
	if (!FrameRecorder::Instance().Enabled()) return;
	const long long now = FrameRecorder::SteadyNowMS();
	const bool slow = ScenarioRunner::IsLockstepLocalMachineSlow();
	if (now - m_LastStatusObservationMs < 250 && slow == m_LastSlowNotice && hostLost == m_LastHostLost) return;
	m_LastStatusObservationMs = now;
	m_LastSlowNotice = slow;
	m_LastHostLost = hostLost;
	uint64_t longest = 0;
	for (const auto& member: snapshot.members) longest = std::max(longest, member.longestWaitMs);
	FrameRecorder::Instance().RecordEvent("net_status peer=" + std::to_string(snapshot.localPeerId) + " host_lost=" + std::to_string(hostLost) +
	    " local_slow=" + std::to_string(slow) + " current_wait_ms=" + std::to_string(currentWaitMs) + " longest_ms=" + std::to_string(longest));
}

void NetModerationGUI::UpdateMatchChat(const NetLobbySnapshot& snapshot, bool frameDue) {
	// Chat follows the status: the end of the round is when players say gg and agree a rematch, so it
	// lives as long as the surfaces around it do.
	if (frameDue) {
		m_ChatInMatch = MatchSurfacesDrawn(ScenarioRunner::IsLockstepControllerSyncActive(), g_NetMatchService.IsMatchResyncing(),
		    snapshot.statusText.starts_with("Host lost"), ScenarioRunner::HasLockstepCoordinator(),
		    snapshot.serviceState == "Completed" && ActivityInMatch(), ActivityInMatch(),
		    PostMatchLobbyAlive(), LobbyMenuUp());
	}
	const bool inMatch = m_ChatInMatch;
	if (!inMatch) {
		m_MatchChatLines.clear();
		m_ChatWrappedLines.clear();
		m_ChatWrapKey.clear();
		m_ChatHistoryInitialized = false;
		m_ChatNotifyUntilUs = 0;
		if (m_ChatEntryOpen) {
			m_ChatEntryOpen = false;
			if (m_MatchChatInput) {
				m_MatchChatInput->SetText("");
				m_MatchChatInput->SetVisible(false);
			}
			if (m_ChatDisabledKeys) {
				g_UInputMan.DisableKeys(false);
				g_UInputMan.TypeIntoSeatInput(false);
				m_ChatDisabledKeys = false;
			}
			if (!m_Open) m_Input->SetKeyJoyMouseCursor(false);
		}
		return;
	}

	// The lines are read again once a drawn frame; the key below lasts one pass and is read on every one.
	if (frameDue) {
		const auto history = g_NetMatchService.ChatHistory();
		std::deque<MatchChatLine> next;
		const long long nowUs = g_TimerMan.GetAbsoluteTime();
		for (const NetChatEntry& entry: history) {
			MatchChatLine line;
			line.historyId = entry.historyId;
			line.receivedTick = entry.receivedTick;
			line.senderPeerId = entry.senderPeerId;
			line.scope = entry.scope;
			line.name = entry.senderName;
			line.text = entry.text;
			line.seenUs = nowUs;
			for (const auto& member: snapshot.members) {
				if (member.cpu || member.peerId != NetChatRosterPeer(entry.senderPeerId)) continue;
				line.team = member.team;
				if (line.name.empty()) line.name = member.displayName;
				break;
			}
			bool known = false;
			for (const auto& prior: m_MatchChatLines) {
				if (prior.historyId == line.historyId) {
					line.seenUs = prior.seenUs;
					line.team = prior.team;
					known = true;
					if (line.name.empty()) line.name = prior.name;
					break;
				}
			}
			if (!known && m_ChatHistoryInitialized && NetChatRosterPeer(entry.senderPeerId) != snapshot.localPeerId) {
				if (g_SettingsMan.GetNetworkChatNotify()) m_ChatNotifyUntilUs = nowUs + 6000000;
				if (g_SettingsMan.GetNetworkChatSound()) {
					RandomGenerator* previous = t_simRNGOverride;
					t_simRNGOverride = &g_RenderRNG;
					g_GUISound.SelectionChangeSound()->Play();
					t_simRNGOverride = previous;
				}
			}
			next.push_back(std::move(line));
		}
		m_MatchChatLines = std::move(next);
		m_ChatHistoryInitialized = true;
	}

	const bool consoleOpen = g_ConsoleMan.IsEnabled() && !g_ConsoleMan.IsReadOnly();
	bool scriptChat = false;
	if (InputScript::IsActive()) {
		const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
		for (int player = 0; player < Players::MaxPlayerCount; ++player) {
			if (InputScript::HeldAt(player, InputScript::c_ChatAction, tick)) {
				scriptChat = true;
				break;
			}
		}
	}
	const SDL_Scancode chatKey = ChatScancode();
	// In the lobby the lobby's own chat box is the entry; the overlay's would only steal its keys.
	const bool otherMenu = g_MenuMan.IsLocalPauseMenuOpen() || g_MenuMan.IsNetworkPanelOpen() || PostMatchLobbySurfaces();
	const bool keyChat = !m_ChatEntryOpen && !consoleOpen && !otherMenu && g_UInputMan.KeyPressed(chatKey);
	if ((scriptChat || keyChat) && !m_ChatKeysHeld && !m_ChatEntryOpen && !consoleOpen && !otherMenu) {
		CreateOverlay();
		m_ChatEntryOpen = true;
		m_ChatScroll = 0;
		if (m_MatchChatInput) {
			m_MatchChatInput->SetEnabled(m_ChatRequestId == 0);
			m_MatchChatInput->SetVisible(true);
			if (GUIPanel* panel = m_MatchChatInput->GetPanel()) panel->SetFocus();
		}
		if (!m_ChatDisabledKeys) {
			g_UInputMan.DisableKeys(true);
			// The entry takes the seats' own gameplay mappings too, which losing the window must not.
			g_UInputMan.TypeIntoSeatInput(true);
			m_ChatDisabledKeys = true;
		}
		m_Input->SetKeyJoyMouseCursor(true);
	}
	m_ChatKeysHeld = scriptChat || g_UInputMan.KeyHeld(chatKey);

	if (!m_ChatEntryOpen) return;
	if (m_ChatRequestId) {
		const auto result = g_NetMatchService.ChatSendResult(m_ChatRequestId);
		if (result.state != NetChatSendState::Queued) {
			m_ChatRequestId = 0;
			m_MatchChatInput->SetEnabled(true);
			m_ChatSendStatus = result.state == NetChatSendState::Unknown ? "Not sent: the chat session changed. Your draft is kept." : result.detail;
			if (result.state == NetChatSendState::Sent) {
				m_ChatSendStatus.clear();
				m_MatchChatInput->SetText("");
				m_ChatEntryOpen = false;
				m_MatchChatInput->SetVisible(false);
				if (m_ChatDisabledKeys) { g_UInputMan.DisableKeys(false); g_UInputMan.TypeIntoSeatInput(false); m_ChatDisabledKeys = false; }
				m_Input->SetKeyJoyMouseCursor(false);
				return;
			}
			if (GUIPanel* panel = m_MatchChatInput->GetPanel()) panel->SetFocus();
		}
	}
	if (g_UInputMan.KeyPressed(SDL_SCANCODE_PAGEUP)) m_ChatScroll = std::min(m_ChatVisualRows, m_ChatScroll + 4);
	if (g_UInputMan.KeyPressed(SDL_SCANCODE_PAGEDOWN)) m_ChatScroll = m_ChatScroll > 4 ? m_ChatScroll - 4 : 0;

	// An entry that outlived its match holds the lobby's keys hostage; it closes the same way Escape does.
	if (g_UInputMan.KeyPressed(SDL_SCANCODE_ESCAPE) || PostMatchLobbySurfaces()) {
		m_ChatEntryOpen = false;
		if (m_MatchChatInput) {
			m_MatchChatInput->SetVisible(false);
		}
		if (m_ChatDisabledKeys) {
			g_UInputMan.DisableKeys(false);
			g_UInputMan.TypeIntoSeatInput(false);
			m_ChatDisabledKeys = false;
		}
		if (!m_Open) m_Input->SetKeyJoyMouseCursor(false);
		return;
	}

	if (m_OverlayControls) {
		m_OverlayControls->Update();
		GUIEvent event;
		while (m_OverlayControls->GetEvent(&event)) {
			if (event.GetType() == GUIEvent::Command && event.GetControl() == m_ChatOlder) m_ChatScroll = std::min(m_ChatVisualRows, m_ChatScroll + 4);
			if (event.GetType() == GUIEvent::Command && event.GetControl() == m_ChatNewer) m_ChatScroll = m_ChatScroll > 4 ? m_ChatScroll - 4 : 0;
			if (event.GetType() == GUIEvent::Notification && event.GetMsg() == GUITextBox::Enter && event.GetControl() == m_MatchChatInput) {
				if (m_ChatRequestId) continue;
				const std::string text = m_MatchChatInput->GetText();
				if (!text.empty()) {
					const int modifier = m_OverlayControls->GetManager()->GetInputController()->GetModifier();
					const uint8_t scope = NetChatAudience(g_SettingsMan.GetNetworkChatDefaultScope(), modifier & GUIPanel::MODI_CTRL, modifier & GUIPanel::MODI_SHIFT);
					m_ChatPendingScope = scope;
					if (!g_NetMatchService.SendChat(scope, text, &m_ChatRequestId)) { m_ChatSendStatus = "Not sent: chat is unavailable or the message was refused. Your draft is kept."; continue; }
					m_ChatSendStatus = std::string("Sending to ") + (scope == c_NetChatScopeTeam ? "Team" : "All") + "...";
					m_MatchChatInput->SetEnabled(false);
					continue;
				}
				m_ChatEntryOpen = false;
				m_MatchChatInput->SetText("");
				m_MatchChatInput->SetVisible(false);
				if (m_ChatDisabledKeys) {
					g_UInputMan.DisableKeys(false);
					g_UInputMan.TypeIntoSeatInput(false);
					m_ChatDisabledKeys = false;
				}
				if (!m_Open) m_Input->SetKeyJoyMouseCursor(false);
			}
		}
	}
}

void NetModerationGUI::DrawMatchChat(const NetLobbySnapshot& snapshot) {
	(void)snapshot;
	m_ChatBand = {};
	if (!m_OverlayControls) {
		m_ChatRect = {};
		return;
	}
	const bool showHistory = g_SettingsMan.GetNetworkChatVisible();
	if (!showHistory && !m_ChatEntryOpen && (!g_SettingsMan.GetNetworkChatNotify() || g_TimerMan.GetAbsoluteTime() >= m_ChatNotifyUntilUs)) {
		for (GUILabel* label: m_MatchChat) {
			if (!label) continue;
			label->SetVisible(false);
			label->SetText("");
		}
		if (m_MatchChatInput) m_MatchChatInput->SetVisible(false);
		m_MatchChatCaption->SetVisible(false);
		m_ChatOlder->SetVisible(false);
		m_ChatNewer->SetVisible(false);
		m_ChatRect = {};
		return;
	}

	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	const bool wantsLarge = g_SettingsMan.GetNetworkChatTextSize() == SettingsMan::NetworkChatTextSize::Large;
	GUIFont* font = wantsLarge ? g_FrameMan.GetLargeFont(true) : g_FrameMan.GetSmallFont(true);
	if (!font) font = g_FrameMan.GetSmallFont(true);
	int lineH = std::max(12, font->GetFontHeight()) + 4;
	EditorArea area = FreeArea(backbuffer->w);
	if (PostMatchLobbySurfaces()) LobbyMenuColumn(area);
	if (m_StatusRect.visible) {
		area.occupiers.push_back({m_StatusRect.x, m_StatusRect.y, m_StatusRect.width, m_StatusRect.height});
	}
	if (m_ToastRect.visible) {
		area.occupiers.push_back({m_ToastRect.x, m_ToastRect.y, m_ToastRect.width, m_ToastRect.height});
	}
	int bottom = backbuffer->h - 6;
	auto lower = [&](int y) {
		if (y > 0) bottom = std::min(bottom, y);
	};
	if (m_StatusRect.visible && m_StatusRect.y + m_StatusRect.height > backbuffer->h / 2) {
		lower(m_StatusRect.y - 4);
	}
	if (m_ToastRect.visible && m_ToastRect.y + m_ToastRect.height > backbuffer->h / 2) {
		lower(m_ToastRect.y - 4);
	}
	if (m_Open) {
		int panelX, panelTop, panelWidth, panelHeight;
		m_Panel->GetControlRect(&panelX, &panelTop, &panelWidth, &panelHeight);
		area.occupiers.push_back({panelX, panelTop, panelWidth, panelHeight});
		lower(panelTop - 4);
	}
	// A message band is a ceiling as much as a floor: the chat stays under a high one and above a low one.
	for (const auto& band: area.textBands) {
		if (band.y + band.h > backbuffer->h / 2) lower(band.y - 4);
		area.occupiers.push_back(band);
	}
	const int topLimit = ChatTopLimit(area, backbuffer->h);

	const int available = std::max(0, bottom - topLimit);
	bool reducedTextSize = false;
	if (wantsLarge && m_ChatEntryOpen && available < ChatEntryMinimum(lineH)) {
		if (GUIFont* small = g_FrameMan.GetSmallFont(true)) {
			font = small;
			lineH = std::max(12, font->GetFontHeight()) + 4;
			reducedTextSize = true;
		}
	}
	int freeLeft = 8, freeRight = backbuffer->w - 8;
	// Measuring the whole available run keeps a wrapped line clear of every editor column it could reach.
	area.FreeSpan(topLimit, bottom, backbuffer->w, freeLeft, freeRight);
	const int width = std::min(420, freeRight - freeLeft - 8);
	const auto hide = [&]() {
		for (auto* label: m_MatchChat) { label->SetVisible(false); label->SetText(""); }
		m_MatchChatInput->SetVisible(false);
		m_MatchChatCaption->SetVisible(false);
		m_ChatOlder->SetVisible(false);
		m_ChatNewer->SetVisible(false);
		m_ChatRect = {};
	};
	if (width < 80) { hide(); return; }
	const auto wrap = [&](const std::string& text) { return NetChatWrap(text, width - 18, [&](const std::string& part) { return font->CalculateWidth(part); }); };
	const bool notification = g_SettingsMan.GetNetworkChatNotify() && g_TimerMan.GetAbsoluteTime() < m_ChatNotifyUntilUs;
	std::string wrapKey;
	for (size_t index = 0; index < m_MatchChatLines.size(); ++index) {
		if (!showHistory && !m_ChatEntryOpen && (!notification || index + 1 != m_MatchChatLines.size())) continue;
		const auto& line = m_MatchChatLines[index];
		const std::string text = (line.scope == c_NetChatScopeTeam ? "[TEAM] " : "[ALL] ") + DisplayName(line.name.empty() ? "Player" : line.name) + ": " + DisplayName(line.text);
		wrapKey += std::to_string(line.historyId) + ":" + std::to_string(line.team) + ":" + text + '\n';
	}
	if (wrapKey != m_ChatWrapKey || width != m_ChatWrapWidth || font != m_ChatWrapFont) {
		m_ChatWrappedLines.clear();
		for (size_t index = 0; index < m_MatchChatLines.size(); ++index) {
			if (!showHistory && !m_ChatEntryOpen && (!notification || index + 1 != m_MatchChatLines.size())) continue;
			const auto& line = m_MatchChatLines[index];
			const std::string text = (line.scope == c_NetChatScopeTeam ? "[TEAM] " : "[ALL] ") + DisplayName(line.name.empty() ? "Player" : line.name) + ": " + DisplayName(line.text);
			for (auto& part: wrap(text)) m_ChatWrappedLines.push_back({std::move(part), line.team, line.seenUs});
		}
		m_ChatWrapKey = std::move(wrapKey);
		m_ChatWrapWidth = width;
		m_ChatWrapFont = font;
	}
	const auto& visual = m_ChatWrappedLines;
	m_ChatVisualRows = visual.size();
	const int modifier = m_OverlayControls->GetManager()->GetInputController()->GetModifier();
	const auto audience = m_ChatRequestId ? m_ChatPendingScope : NetChatAudience(g_SettingsMan.GetNetworkChatDefaultScope(), modifier & GUIPanel::MODI_CTRL, modifier & GUIPanel::MODI_SHIFT);
	std::string caption = std::string("To ") + (audience == c_NetChatScopeTeam ? "Team" : "All") + " | Ctrl+Enter: Team | Shift+Enter: All";
	if (!m_ChatSendStatus.empty()) caption += "\n" + m_ChatSendStatus;
	const auto captionLines = m_ChatEntryOpen ? wrap(caption) : std::vector<std::string>{};
	std::string wrappedCaption;
	for (const auto& part: captionLines) wrappedCaption += (wrappedCaption.empty() ? "" : "\n") + part;
	const int captionH = static_cast<int>(captionLines.size()) * lineH;
	const int navigationH = m_ChatEntryOpen ? 22 : 0;
	const int inputH = m_ChatEntryOpen ? lineH + 6 : 0;
	const int footerH = inputH + captionH + navigationH;
	const int maxRows = std::min(static_cast<int>(m_MatchChat.size()), std::max(0, (available - footerH) / lineH));
	const int rows = std::min(maxRows, static_cast<int>(visual.size()));
	const int height = rows * lineH + footerH;
	if (height <= 0 || height > available) { hide(); return; }
	const int top = bottom - height;
	const int x = freeLeft + (freeRight - freeLeft - width) / 2;
	m_ChatRect = {x, top, width, height, true};
	m_ChatBand = {lineH, footerH, rows, showHistory || m_ChatEntryOpen || notification, reducedTextSize};
	m_ChatScroll = std::min(m_ChatScroll, visual.size() > static_cast<size_t>(rows) ? visual.size() - rows : 0);
	const size_t first = visual.size() - rows - (m_ChatEntryOpen ? m_ChatScroll : 0);
	const long long nowUs = g_TimerMan.GetAbsoluteTime();
	AllegroBitmap bitmap(backbuffer);
	for (size_t row = 0; row < m_MatchChat.size(); ++row) {
		GUILabel* label = m_MatchChat[row];
		label->SetVisible(row < static_cast<size_t>(rows));
		if (row >= static_cast<size_t>(rows)) { label->SetText(""); continue; }
		const auto& line = visual[first + row];
		const long long ageUs = std::max(0LL, nowUs - line.seenUs);
		int alpha = 255;
		if (!m_ChatEntryOpen && ageUs > 8000000) alpha = static_cast<int>(255 - std::min(4000000LL, ageUs - 8000000) * 165 / 4000000);
		const int y = top + static_cast<int>(row) * lineH;
		label->SetFont(font);
		label->Move(x + 8, y + 2);
		label->Resize(width - 12, lineH - 2);
		label->SetText(line.text);
		rectfill(backbuffer, x, y, x + width - 1, y + lineH - 2, makeacol32(20, 22, 27, alpha));
		rectfill(backbuffer, x, y, x + 2, y + lineH - 2, TeamBarColor(line.team, alpha));
		label->Draw(&bitmap, false);
	}
	m_MatchChatCaption->SetVisible(m_ChatEntryOpen);
	m_MatchChatInput->SetVisible(m_ChatEntryOpen);
	m_ChatOlder->SetVisible(m_ChatEntryOpen);
	m_ChatNewer->SetVisible(m_ChatEntryOpen);
	if (m_ChatEntryOpen) {
		int y = top + rows * lineH;
		rectfill(backbuffer, x, y, x + width - 1, bottom - 1, makeacol32(20, 22, 27, 255));
		m_MatchChatCaption->SetFont(font);
		m_MatchChatCaption->Move(x + 8, y + 2);
		m_MatchChatCaption->Resize(width - 12, captionH);
		m_MatchChatCaption->SetText(wrappedCaption);
		m_MatchChatCaption->Draw(&bitmap, false);
		y += captionH;
		const int navigationWidth = std::min(96, (width - 12) / 2);
		m_ChatOlder->Resize(navigationWidth, 20);
		m_ChatNewer->Resize(navigationWidth, 20);
		m_ChatOlder->SetText("Older");
		m_ChatNewer->SetText("Newer");
		m_ChatOlder->Move(x + 4, y);
		m_ChatNewer->Move(x + width - navigationWidth - 4, y);
		m_ChatOlder->SetEnabled(first > 0);
		m_ChatNewer->SetEnabled(m_ChatScroll > 0);
		m_ChatOlder->Draw(m_Screen);
		m_ChatNewer->Draw(m_Screen);
		RecordPanelDraw(m_ChatOlder->GetPanel());
		RecordPanelDraw(m_ChatNewer->GetPanel());
		y += navigationH;
		m_MatchChatInput->Move(x + 4, y + 2);
		m_MatchChatInput->Resize(width - 8, inputH - 4);
		m_MatchChatInput->Draw(m_Screen);
		RecordPanelDraw(m_MatchChatInput->GetPanel());
	}
}

void NetModerationGUI::DrawMatchToasts(const std::string& screenLine) {
	m_ToastRect = {};
	m_SeatsPanelRect = {};
	const bool menuLobby = PostMatchLobbySurfaces();
	// A relaunched peer replaying its private catch-up is in the match before its round's controllers sync: its held line shows then too.
	if (!ScenarioRunner::IsLockstepControllerSyncActive() && !g_NetMatchService.IsMatchResyncing() && !ScenarioRunner::WorldCatchUpActive() && !menuLobby) {
		for (GUILabel* label: m_Toasts) {
			if (label) {
				label->SetVisible(false);
				label->SetText("");
			}
		}
		return;
	}
	RandomGenerator* previousRNG = t_simRNGOverride;
	t_simRNGOverride = &g_RenderRNG;
	CreateOverlay();
	auto queued = ScenarioRunner::GetVisibleNetUiToasts();
	// The own seat's line lasts as long as the roster's held or rejoining reading, which the host ends a round trip after the player's input applies,
	// and as long as this peer catches up through a hold its roster no longer shows (a host lost meanwhile).
	if (OwnSeatReturning() && std::none_of(queued.begin(), queued.end(), [](const auto& toast) { return toast.kind == "seat_held" && toast.text.find("rejoining") != std::string::npos; }))
		queued.push_back({ScenarioRunner::GetLockstepCompletedFrame(), "seat_held", "Held - AI in control - rejoining", ScenarioRunner::GetLockstepLocalPeerId()});
	std::vector<ScenarioRunner::NetUiToastRecord> visible;
	std::vector<size_t> indices;
	std::vector<std::string> lines;
	const auto sentence = [](const std::string& text) { return text.substr(0, text.find_last_not_of(". ") + 1); };
	const uint8_t localPeer = ScenarioRunner::GetLockstepLocalPeerId();
	const PauseMenuGUI* pauseMenu = g_MenuMan.GetActivePauseMenu();
	const std::string menuSaveLine = pauseMenu ? pauseMenu->GetShownSaveLine() : std::string();
	for (size_t index = 0; index < queued.size(); ++index) {
		if (!ToastStillApplies(queued[index])) continue;
		// A seat's toast reads its current state, so two events about one seat can read alike: the band shows that line once.
		std::string line = ToastText(queued[index]);
		if (std::find(lines.begin(), lines.end(), line) != lines.end()) continue;
		// The screen beneath shows its own line; the band does not read it out again, nor this seat's own state when that is the line.
		const bool ownSeat = queued[index].senderPeerId == 0 || queued[index].senderPeerId == localPeer;
		// The wait's own screen already says this seat is on its way back.
		if (!screenLine.empty() && (sentence(line) == sentence(screenLine) ||
		                            (ownSeat && (queued[index].kind == "seat_held" || sentence(queued[index].text) == sentence(screenLine))))) continue;
		// The open pause menu reads the save under its Save Match row; the band does not say it a second time.
		if (!menuSaveLine.empty() && queued[index].kind == "match_save" && sentence(line) == sentence(menuSaveLine)) continue;
		lines.push_back(std::move(line));
		visible.push_back(queued[index]);
		indices.push_back(index);
	}
	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	const int rowHeight = std::max(12, font->GetFontHeight()) + 8;
	EditorArea editor = FreeArea(backbuffer->w);
	if (menuLobby) LobbyMenuColumn(editor);
	// The status widget takes the bottom while the editor holds the world, so the rows stack above it.
	int bottom = editor.editing && m_StatusRect.visible ? m_StatusRect.y - 4 : backbuffer->h - 8;
	// The stack never crosses a seat's own message band, which owns the rows it draws in.
	int topLimit = 2;
	for (const auto& band: editor.textBands) {
		if (band.y + band.h <= backbuffer->h / 2) topLimit = std::max(topLimit, band.y + band.h + 4);
		else bottom = std::min(bottom, band.y - 4);
	}
	if (m_Open) {
		// The seats panel owns its rows too: the stack takes the larger free band beside it, and takes
		// none at all when the panel leaves no room - a row laid out off the screen is not a reading.
		int panelX, panelTop, panelWidth, panelHeight;
		m_Panel->GetControlRect(&panelX, &panelTop, &panelWidth, &panelHeight);
		m_SeatsPanelRect = {panelX, panelTop, panelWidth, panelHeight, true};
		const int panelBottom = panelTop + panelHeight;
		if (panelTop < bottom && panelBottom > topLimit) {
			const int above = panelTop - 4 - topLimit, below = bottom - (panelBottom + 4);
			if (above >= below) bottom = panelTop - 4;
			else topLimit = panelBottom + 4;
		}
	}
	size_t rowCount = std::min<size_t>(visible.size(), 3);
	while (rowCount > 0 && bottom - static_cast<int>(rowCount) * rowHeight < topLimit) --rowCount;
	// A short band keeps the oldest waiting toasts; a full one keeps the newest.
	size_t firstRow = rowCount < 3 ? 0 : visible.size() - rowCount;
	int top = bottom - static_cast<int>(rowCount) * rowHeight;
	EditorArea toastArea = editor;
	if (m_StatusRect.visible) {
		toastArea.occupiers.push_back({m_StatusRect.x, m_StatusRect.y, m_StatusRect.width, m_StatusRect.height});
	}
	if (m_ChatRect.visible) {
		toastArea.occupiers.push_back({m_ChatRect.x, m_ChatRect.y, m_ChatRect.width, m_ChatRect.height});
	}
	int freeLeft = 0, freeRight = backbuffer->w;
	toastArea.FreeSpan(top, bottom, backbuffer->w, freeLeft, freeRight);
	const int countNeed = font->CalculateWidth(std::string("0 of 0")) + 14;
	if (freeRight - freeLeft < countNeed) {
		// No column-free span wide enough: the oldest toast takes the band's first row, the rest wait.
		firstRow = 0;
		rowCount = visible.empty() || bottom - rowHeight < topLimit ? 0 : 1;
		top = topLimit;
		bottom = topLimit + static_cast<int>(rowCount) * rowHeight;
		freeLeft = 0;
		freeRight = backbuffer->w;
	}
	const int available = freeRight - freeLeft;
	const int width = std::max(1, std::min(520, std::max(1, available - 32)));
	const int x = std::max(0, std::min(backbuffer->w - width, freeLeft + std::max(0, (available - width) / 2)));
	if (rowCount) {
		m_ToastRect = {x, top, width, static_cast<int>(rowCount) * rowHeight - 2, true};
	}
	AllegroBitmap bitmap(backbuffer);
	for (size_t row = 0; row < m_Toasts.size(); ++row) {
		GUILabel* label = m_Toasts[row];
		const bool shown = row < rowCount;
		label->SetVisible(shown);
		label->SetText(shown ? FitToastText(font, visible[firstRow + row], width - 16) : std::string());
		if (!shown) continue;
		const int y = top + static_cast<int>(row) * rowHeight;
		label->SetFont(font);
		label->Move(x + 8, y + 4);
		label->Resize(width - 16, rowHeight - 8);
		rectfill(backbuffer, x, y, x + width - 1, y + rowHeight - 3, makeacol32(20, 22, 27, 255));
		label->Draw(&bitmap, false);
		const std::string& message = label->GetText();
		const uint64_t round = ScenarioRunner::GetLockstepRoundId();
		if (FrameRecorder::Instance().Enabled() && message.find(" is now hosting") != std::string::npos &&
		    (message != m_LastHandoverToast || round != m_LastHandoverRound)) {
			m_LastHandoverToast = message;
			m_LastHandoverRound = round;
			const auto snapshot = g_NetMatchService.GetLobbySnapshot();
			uint64_t longest = 0;
			for (const auto& member: snapshot.members) longest = std::max(longest, member.longestWaitMs);
			FrameRecorder::Instance().RecordEvent("handover_toast peer=" + std::to_string(snapshot.localPeerId) + " longest_ms=" + std::to_string(longest) + " text=" + message);
		}
	}
	if (rowCount) {
		const size_t first = indices[firstRow], last = indices[firstRow + rowCount - 1];
		ScenarioRunner::NoteNetUiToastsDrawn(first, last - first + 1);
	} else if (visible.empty()) {
		ScenarioRunner::NoteNetUiToastsDrawn(0, queued.size());
	}
	t_simRNGOverride = previousRNG;
	GhostWatchTick();
}

NetModerationGUI::GhostBandHit NetModerationGUI::ScanGhostBand(const int minRunPx) const {
	GhostBandHit hit;
	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	if (!backbuffer) {
		return hit;
	}
	// Every overlay surface fills (20,22,27,255); a run of those pixels that no reported rect
	// covers is a band painted where nothing drew this frame. The seats panel's skin never reaches
	// that exact colour run-wide, so an uncovered run can only be stale paint.
	const int bandFill = makeacol32(20, 22, 27, 255);
	if (m_ToastRect.visible) {
		hit.probePixel = getpixel(backbuffer, m_ToastRect.x + 2, m_ToastRect.y + 2);
	}
	const OverlayRect* rects[] = {&m_ToastRect, &m_SeatsPanelRect, &m_StatusRect, &m_ChatRect, &m_RosterRect};
	for (int y = 0; y < backbuffer->h && !hit.found; ++y) {
		int run = 0;
		for (int x = 0; x < backbuffer->w; ++x) {
			bool covered = getpixel(backbuffer, x, y) != bandFill;
			for (const auto* rect: rects) {
				covered = covered || (rect->visible && x >= rect->x && x < rect->x + rect->width &&
				                      y >= rect->y && y < rect->y + rect->height);
			}
			run = covered ? 0 : run + 1;
			if (run >= minRunPx) {
				hit.found = true;
				hit.x = x - run + 1;
				hit.y = y;
				hit.run = run;
				break;
			}
		}
	}
	return hit;
}

void NetModerationGUI::GhostWatchTick() {
	// The resync overlay paints the whole layer the fill colour - a full fill is a deliberate
	// screen, not a stale band, so those frames do not count.
	if (!m_GhostWatchArmed || g_NetMatchService.IsMatchResyncing()) {
		return;
	}
	const GhostBandHit hit = ScanGhostBand(100);
	m_GhostWatchProbe = hit.probePixel;
	if (hit.found) {
		++m_GhostWatchHits;
		m_GhostWatchLast = hit;
	}
}

// Test lever CCCP_TEST_DRAW_PHASES: the overlay's own stages, their means every 600 frames.
namespace {
	struct OverlayPhases {
		const bool armed = std::getenv("CCCP_TEST_DRAW_PHASES") != nullptr;
		std::array<double, 5> totalUs{};
		uint64_t frames = 0;
		std::chrono::steady_clock::time_point lap;
		void Begin() { if (armed) lap = std::chrono::steady_clock::now(); }
		void Lap(size_t phase) {
			if (!armed) return;
			const auto now = std::chrono::steady_clock::now();
			totalUs[phase] += std::chrono::duration<double, std::micro>(now - lap).count();
			lap = now;
		}
		void End() {
			if (!armed || ++frames < 600) return;
			System::PrintDiagnosticLine("[overlay-phase] frames=600 mean_us: snapshot=" + std::to_string(totalUs[0] / 600) + " surfaces=" + std::to_string(totalUs[1] / 600) +
			                            " status=" + std::to_string(totalUs[2] / 600) + " panel=" + std::to_string(totalUs[3] / 600) + " chat=" + std::to_string(totalUs[4] / 600));
			totalUs = {};
			frames = 0;
		}
	};
	OverlayPhases s_OverlayPhases;
}

void NetModerationGUI::Draw() {
	struct DrawTime {
		NetModerationGUI& panel;
		std::chrono::steady_clock::time_point started = panel.m_Cost ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		~DrawTime() {
			if (panel.m_Cost) panel.NoteFrameCost(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
		}
	} drawTime{*this};
	s_OverlayPhases.Begin();
	// The panel's update follows the match from this frame's read until the next frame draws.
	m_FrameSnapshot = std::make_unique<NetLobbySnapshot>(g_NetMatchService.GetLobbySnapshot());
	++m_DrawSerial;
	const NetLobbySnapshot& snapshot = *m_FrameSnapshot;
	NetPlayerPresentation::Remember(snapshot, g_SettingsMan.GetNetworkDisplayName());
	s_OverlayPhases.Lap(0);
	m_StatusRect = {};
	m_ChatRect = {};
	m_RosterRect = {};
	m_RosterWrap = {};
	m_StatusWrap = {};
	if (m_NetStatusBox) {
		m_NetStatusBox->SetVisible(false);
		m_NetStatus->SetVisible(false);
	}
	// A completed round's peer is still in its match until the activity is over, and it still needs its
	// surfaces to read the result and leave. The menu-loop arm is the lobby's own version of that:
	// the rematch lobby keeps the surfaces while the pump is owed.
	const bool matchEnded = snapshot.serviceState == "Completed" && ActivityInMatch();
	const bool menuLobby = PostMatchLobbySurfaces();
	if (snapshot.serviceState != "Running" && snapshot.serviceState != "Starting" && snapshot.serviceState != "ReadyToLaunch" &&
	    !matchEnded && !snapshot.hostLost && !snapshot.migrating && !m_Open && !menuLobby) return;
	RandomGenerator* previousRNG = t_simRNGOverride;
	t_simRNGOverride = &g_RenderRNG;
	const bool inMatch = MatchSurfacesDrawn(ScenarioRunner::IsLockstepControllerSyncActive(), g_NetMatchService.IsMatchResyncing(),
	    snapshot.statusText.starts_with("Host lost"), ScenarioRunner::HasLockstepCoordinator(), matchEnded, ActivityInMatch(),
	    PostMatchLobbyAlive(), LobbyMenuUp());
	if (inMatch) {
		CreateOverlay();
	} else {
		DrawRoster(snapshot);
	}
	// The lobby arm sits inside inMatch: the rematch lobby keeps the seats reading beside its own box.
	if (menuLobby) DrawRoster(snapshot);
	s_OverlayPhases.Lap(1);
	if (inMatch && (menuLobby || MatchStatusWanted())) {
		DrawMatchStatus(snapshot);
		m_NetStatus->SetVisible(true);
	}
	s_OverlayPhases.Lap(2);
	if (m_Open) {
		uint64_t hash = std::hash<std::string>{}(snapshot.serviceState);
		hash ^= std::hash<std::string>{}(snapshot.statusText) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
		hash ^= std::hash<bool>{}(snapshot.isHost) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
		for (const auto& member: snapshot.members) {
			hash ^= std::hash<unsigned>{}(member.peerId) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
			hash ^= std::hash<bool>{}(member.dropped) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
			hash ^= std::hash<bool>{}(member.reclaiming) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
			hash ^= std::hash<bool>{}(member.connected) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
			hash ^= std::hash<std::string>{}(member.connectedRoute);
			hash ^= std::hash<std::string>{}(member.statusLine);
		}
		const long long nowMs = static_cast<long long>(SDL_GetTicks());
		const bool changed = hash != m_LastRefreshHash;
		const bool due = m_LastRefreshMs == 0 || (nowMs - m_LastRefreshMs) >= 100;
		if (changed || due) {
			if (changed) {
				++m_RefreshChangeCount;
			}
			m_LastRefreshHash = hash;
			m_LastRefreshMs = nowMs;
			++m_RefreshCount;
			Refresh();
		}
		m_Controls->Draw();
	}
	s_OverlayPhases.Lap(3);
	if (inMatch) {
		DrawMatchChat(snapshot);
	} else {
		for (GUILabel* label: m_MatchChat) {
			if (!label) continue;
			label->SetVisible(false);
			label->SetText("");
		}
		if (m_MatchChatInput) m_MatchChatInput->SetVisible(false);
	}
	if (m_Open) m_Controls->DrawMouse();
	t_simRNGOverride = previousRNG;
	s_OverlayPhases.Lap(4);
	s_OverlayPhases.End();
}

void NetModerationGUI::NoteFrameCost(long long drawNs) {
	std::vector<long long>& samples = m_Open ? m_Cost->openNs : m_Cost->closedNs;
	std::vector<long long>& updates = m_Open ? m_Cost->openUpdateNs : m_Cost->closedUpdateNs;
	std::vector<long long>& parts = m_Open ? m_Cost->openPartsUs : m_Cost->closedPartsUs;
	samples.push_back(m_Cost->frameNs + drawNs);
	updates.push_back(m_Cost->frameNs);
	parts.push_back(m_Cost->framePartsUs + drawNs / 1000);
	m_Cost->frameNs = 0;
	m_Cost->framePartsUs = 0;
	if (samples.size() < 600) return;
	std::vector<long long> sorted = samples;
	std::sort(sorted.begin(), sorted.end());
	const auto median = [](std::vector<long long> values) {
		std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
		return values[values.size() / 2];
	};
	const auto at = [&sorted](double share) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(share * static_cast<double>(sorted.size())))]; };
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	const auto players = std::count_if(snapshot.members.begin(), snapshot.members.end(), [](const auto& member) { return !member.cpu; });
	const auto us = [](long long ns) {
		char text[32];
		std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(ns) / 1000.0);
		return std::string(text);
	};
	// parts_median_us is the same frames with every pass rounded down first, to show what such a sum leaves out.
	System::PrintDiagnosticLine("[panel-cost] " + std::string(m_Open ? "open" : "closed") + " players=" + std::to_string(players) + " frames=" +
	                            std::to_string(sorted.size()) + " median_us=" + us(at(0.5)) + " p99_us=" + us(at(0.99)) +
	                            " status_shown=" + (m_NetStatus && m_NetStatus->GetVisible() ? "1" : "0") + " update_median_us=" + us(median(updates)) +
	                            " updates=" + std::to_string(m_Cost->updates) + " parts_median_us=" + std::to_string(median(parts)));
	m_Cost->updates = 0;
	samples.clear();
	updates.clear();
	parts.clear();
}

bool NetModerationGUI::AutomationModerate(const std::string& action, int stableSeat) {
	if (g_NetMatchService.GetState() != NetMatchServiceState::Running) return false;
	if (!m_Open && !SetOpen(true)) return false;
	Refresh();
	size_t row = m_RowsShown;
	for (size_t index = 0; index < m_RowsShown; ++index) {
		if (SlotRow(index).decision && (stableSeat < 0 || SlotRow(index).decision->stableSeat == static_cast<uint16_t>(stableSeat))) {
			row = index;
			break;
		}
	}
	if (row >= m_RowsShown) return false;
	NetModerationAction verb;
	if (action == "wait") verb = NetModerationAction::Wait;
	else if (action == "substitute") verb = NetModerationAction::Substitute;
	else if (action == "cancel") verb = NetModerationAction::Cancel;
	else return false;
	if (!NetModerationUx::Available(*SlotRow(row).decision, verb)) return false;
	auto* button = m_Seats[row].actions[static_cast<size_t>(verb)];
	if (!button->GetEnabled() || !button->GetVisible()) return false;
	m_ActionResult.reset();
	for (int press = 0; press < (verb == NetModerationAction::Substitute ? 2 : 1); ++press) {
		int x, y, width, height;
		button->GetControlRect(&x, &y, &width, &height);
		button->OnMouseDown(x + width / 2, y + height / 2, GUIPanel::MOUSE_LEFT, 0);
		HandleEvents();
		button->OnMouseUp(x + width / 2, y + height / 2, GUIPanel::MOUSE_LEFT, 0);
		HandleEvents();
		Refresh();
	}
	return m_ActionResult == NetH4ModerationResult::Ok;
}

size_t NetModerationGUI::AutomationRowsImplied() const {
	return m_Open && !m_OptionsView ? m_RowsImplied : 0;
}

std::vector<std::string> NetModerationGUI::AutomationRowControls(size_t slot) const {
	if (slot >= m_RowsShown) return {};
	const std::string suffix = std::to_string(slot);
	const PanelRow& row = SlotRow(slot);
	std::vector<std::string> controls{"NetworkSeatName" + suffix, "NetworkSeatDetail" + suffix};
	if (!row.opened && !row.cpu) controls.insert(controls.end(), {"NetworkSeatRemove" + suffix, "NetworkSeatBan" + suffix});
	if (row.decision) {
		if (!row.opened) controls.push_back("NetworkSeatWait" + suffix);
		controls.push_back("NetworkSeatSubstitute" + suffix);
		if (row.decision->view.substituting) controls.push_back("NetworkSeatCancel" + suffix);
		if (!row.decision->view.applicants.empty()) controls.insert(controls.end(), {"NetworkSeatApplicant" + suffix, "NetworkSeatDeclineApplicant" + suffix, "NetworkSeatBanApplicant" + suffix});
	}
	return controls;
}

int NetModerationGUI::ChatKeyScancode() {
	return static_cast<int>(ChatScancode());
}

bool NetModerationGUI::AutomationPostCommand(const std::string& name) {
	GUIControl* control = GetControl(name);
	if (!control || !control->GetEnabled() || !control->GetVisible()) return false;
	int x = 0, y = 0, width = 0, height = 0;
	control->GetControlRect(&x, &y, &width, &height);
	GUIPanel* panel = control->GetPanel();
	panel->OnMouseDown(x + width / 2, y + height / 2, GUIPanel::MOUSE_LEFT, 0);
	HandleEvents();
	panel->OnMouseUp(x + width / 2, y + height / 2, GUIPanel::MOUSE_LEFT, 0);
	HandleEvents();
	Refresh();
	return true;
}

bool NetModerationGUI::AutomationLabelText(const std::string& name, std::string& text) const {
	auto* label = dynamic_cast<GUILabel*>(GetControl(name));
	if (!label) return false;
	text = label->GetText();
	return true;
}

GUIControl* NetModerationGUI::GetControl(const std::string& name) const {
	if (const auto at = name.find('@'); at != std::string::npos && name.starts_with("NetworkSeat")) {
		const std::string part = name.substr(11, at - 11), player = name.substr(at + 1);
		for (size_t row = 0; row < m_RowsShown && row < m_Seats.size(); ++row) {
			if (SlotRow(row).name != player) continue;
			const Controls& controls = m_Seats[row];
			if (part == "Name") return controls.name;
			if (part == "Detail") return controls.detail;
			if (part == "Hint") return controls.hint;
			if (part == "Applicant") return controls.requests;
			if (part == "Wait") return controls.actions[0];
			if (part == "Substitute") return controls.actions[1];
			if (part == "Cancel") return controls.actions[2];
			if (part == "Remove") return controls.remove;
			if (part == "Ban") return controls.ban;
			if (part == "DeclineApplicant") return controls.declineApplicant;
			if (part == "BanApplicant") return controls.banApplicant;
		}
		return nullptr;
	}
	if (name.starts_with("NetworkPeerDetail") && name.size() == 18 && name.back() >= '1' && name.back() <= '4') {
		const uint8_t peer = static_cast<uint8_t>(name.back() - '0');
		for (size_t row = 0; row < m_RowsShown && row < m_Seats.size(); ++row) {
			if (SlotRow(row).peer == peer) return m_Seats[row].detail;
		}
		return nullptr;
	}
	if (m_OverlayControls && name == "LabelMatchChatNewest") {
		for (auto row = m_MatchChat.rbegin(); row != m_MatchChat.rend(); ++row) {
			if (*row && (*row)->GetVisible()) return *row;
		}
		return m_MatchChat.front();
	}
	if (m_OverlayControls && name == "LabelNetMatchToastNewest") {
		for (auto row = m_Toasts.rbegin(); row != m_Toasts.rend(); ++row) {
			if ((*row)->GetVisible()) return *row;
		}
		return m_Toasts.front();
	}
	if (m_OverlayControls && name == "LabelNetMatchHandoverToast") {
		for (auto* row : m_Toasts) {
			if (row->GetVisible() && row->GetText().find(" is now hosting") != std::string::npos) return row;
		}
		return m_Toasts.front();
	}
	if (m_OverlayControls) {
		if (GUIControl* overlay = m_OverlayControls->GetControl(name)) return overlay;
	}
	return m_Controls->GetControl(name);
}
