#include "NetModerationGUI.h"

#include "ActivityMan.h"
#include "Constants.h"
#include "CameraMan.h"
#include "GameActivity.h"
#include "NetMatchService.h"
#include "NetSession.h"
#include "ScenarioRunner.h"
#include "SettingsMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "UInputMan.h"
#include "GUI.h"
#include "GUIEvent.h"
#include "GUIManager.h"
#include "GUIInputWrapper.h"
#include "GUIControlManager.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUILabel.h"
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
#include <cstring>
#include <utility>
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

	/// The panel's top row: centred, but under the status widget's band on a screen with the rows for both.
	int PanelTop(int screenHeight) {
		const int band = (screenHeight < c_CompactMaxHeight ? c_StripBandBottom : c_StatusBoxTop + c_StatusBoxHeight) + c_PanelGap;
		const int lowest = std::max(0, screenHeight - c_PanelHeight - c_PanelGap);
		return std::clamp((screenHeight - c_PanelHeight) / 2, std::min(band, lowest), lowest);
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

	std::string ToastText(const ScenarioRunner::NetUiToastRecord& toast) {
		uint8_t sender = toast.senderPeerId;
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
		return toast.text + (sender ? " by " + g_NetMatchService.GetPeerDisplayName(sender) : std::string());
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
				} else {
					wrapped += line + '\n';
					line.clear();
				}
			}
			line += c;
		}
		return wrapped + line;
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
	m_Close = button("NetworkSeatsClose", "Close seats  [F6 / Esc]", width - 224, 318, 214);
	for (size_t row = 0; row < m_Seats.size(); ++row) {
		const std::string suffix = std::to_string(row);
		const int y = 40 + static_cast<int>(row) * 80;
		auto& seat = m_Seats[row];
		seat.name = label("NetworkSeatName" + suffix, y, 24);
		seat.detail = label("NetworkSeatDetail" + suffix, y + 24, 16);
		seat.applicant = button("NetworkSeatApplicant" + suffix, "No applicants", 10, y + 40, width - 20);
		seat.applicant->SetHorizontalOverflowScroll(true);
		seat.actions[0] = button("NetworkSeatWait" + suffix, "Wait for player", 10, y + 60, 140);
		seat.actions[1] = button("NetworkSeatSubstitute" + suffix, "Approve substitute", 158, y + 60, 160);
		seat.actions[2] = button("NetworkSeatCancel" + suffix, "Cancel approval", 326, y + 60, 140);
	}
	m_Panel->SetVisible(false);
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
		for (auto& seat: m_Seats) {
			seat.applicant->SetPushed(false);
			for (auto* button: seat.actions) button->SetPushed(false);
		}
		g_UInputMan.EndSimUpdate();
	}
	return true;
}

void NetModerationGUI::LayoutPanel() {
	// A compact screen keeps the strip band and one toast row above the panel's top: the panel sits
	// under them and loses the rows off its height, so its bottom edge - and the roster - stay put.
	const int screenHeight = g_WindowMan.GetResY();
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	const int rowHeight = std::max(12, font ? font->GetFontHeight() : 12) + 8;
	const int width = std::min(c_PanelWidth, g_WindowMan.GetResX() - 12);
	int top = PanelTop(screenHeight);
	if (screenHeight < c_CompactMaxHeight) {
		int reserved = c_StripBandBottom + rowHeight + c_PanelGap;
		// While the editor holds the world its seat message bands own their top rows too: a band
		// crossing the reservation pushes the toast row - and the panel - under it.
		for (const auto& band: FreeArea(g_WindowMan.GetResX()).textBands) {
			// Every seat's message band is in window space: the panel sits under the lowest one.
			reserved = std::max(reserved, band.y + band.h + rowHeight + 2 * c_PanelGap);
		}
		top = std::max(top, reserved);
	}
	// Keep the close row on the panel: a height that loses more than that row puts its rel-Y below 0.
	const int maxLost = 318;
	int height = std::min(c_PanelHeight, screenHeight - c_PanelGap - top);
	height = std::max(height, c_PanelHeight - maxLost);
	if (top + height > screenHeight - c_PanelGap) {
		top = std::max(0, screenHeight - c_PanelGap - height);
	}
	const int lost = std::min(maxLost, c_PanelHeight - height);
	int x, y, w, h;
	m_Panel->GetControlRect(&x, &y, &w, &h);
	if (x != (g_WindowMan.GetResX() - width) / 2 || y != top || w != width || h != height) {
		m_Panel->Move((g_WindowMan.GetResX() - width) / 2, top);
		m_Panel->Resize(width, height);
	}
	// The roster yields the reserved rows and scrolls for what no longer fits; the status row gives up
	// its second line first, then moves up with the close row instead of clipping at the panel's bottom.
	if (m_Roster->GetHeight() != 240 - lost) {
		m_Roster->Resize(m_Roster->GetWidth(), 240 - lost);
	}
	m_Roster->SetVerticalOverflowScroll(lost != 0);
	m_Roster->ActivateDeactivateOverflowScroll(lost != 0);
	const int statusY = std::max(0, 282 - std::max(0, lost - 20));
	if (m_Status->GetRelYPos() != statusY) {
		m_Status->SetPositionRel(10, statusY);
	}
	const int statusHeight = 30 - std::min(lost, 20);
	if (m_Status->GetHeight() != statusHeight) {
		m_Status->Resize(m_Status->GetWidth(), statusHeight);
	}
	const int closeY = std::max(0, 318 - lost);
	if (m_Close->GetRelYPos() != closeY) {
		m_Close->SetPositionRel(width - 224, closeY);
	}
}

void NetModerationGUI::Refresh() {
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	LayoutPanel();
	m_Model.Refresh(g_NetMatchService.GetModerationSeats());
	// The hold is the round's, read from the same place the stall overlay reads it.
	std::string holdName;
	uint32_t holdSeconds = 0;
	const bool holdPause = ScenarioRunner::DescribeLockstepHoldPause(holdName, holdSeconds);
	m_Title->SetText(NetModerationPanelTitle(snapshot.serviceState == "Running", holdPause, DisplayName(holdName), holdSeconds));
	m_Summary->SetText(snapshot.isHost ? m_Model.GetSummaryText() : "Only the host can approve a substitute.");
	m_Status->SetText(WrapText(m_LabelFont, m_Model.GetStatusText(), m_Status->GetWidth()));
	// A compact panel that lost its rows to the toast reservation has no room for the status line
	// under a full seat list; the seat rows already carry the same state.
	const int statusY = 282 - std::max(0, (c_PanelHeight - m_Panel->GetHeight()) - 20);
	m_Status->SetVisible(40 + static_cast<int>(std::min<size_t>(m_Model.RowCount(), m_Seats.size())) * 80 <= statusY);
	m_Roster->SetVisible(!snapshot.isHost);
	if (!snapshot.isHost) {
		std::string roster;
		for (const auto& member: snapshot.members) {
			if (!member.cpu) roster += (roster.empty() ? "" : "\n\n") +
			    (member.statusLine.empty() ? DisplayName(member.displayName) + "  /  Connected" : DisplayName(member.statusLine));
		}
		m_Roster->SetText(WrapText(m_LabelFont, roster, m_Roster->GetWidth()));
	}
	for (size_t row = 0; row < m_Seats.size(); ++row) {
		const bool used = row < m_Model.RowCount();
		auto& controls = m_Seats[row];
		controls.name->SetVisible(used);
		controls.detail->SetVisible(used);
		controls.applicant->SetVisible(used);
		for (auto* action: controls.actions) action->SetVisible(used);
		if (!used) continue;
		const auto& seat = m_Model.GetRow(row);
		controls.name->SetText(WrapText(m_LabelFont, "Seat " + std::to_string(seat.stableSeat) + "  /  " + DisplayName(seat.view.displayName), controls.name->GetWidth()));
		controls.detail->SetText((seat.view.reclaiming ? "Reconnecting" : seat.view.dropped ? "Disconnected" : "Left") +
		    std::string("  /  away ") + std::to_string(seat.view.droppedForMs / 1000) + "s  /  sim hold " +
		    std::to_string(NetSeatPresence::HoldSeconds(seat.view.holdFramesRemaining)) + "s");
		controls.applicant->SetText(DisplayName(seat.applicantText));
		controls.applicant->SetEnabled(seat.view.actionsAvailable && (seat.applicants > 1 || (seat.applicants && seat.applicant == c_InvalidNetPeerId)));
		for (size_t action = 0; action < controls.actions.size(); ++action) {
			controls.actions[action]->SetEnabled(NetModerationUx::Available(seat, static_cast<NetModerationAction>(action)));
		}
	}
}

void NetModerationGUI::HandleEvents() {
	GUIEvent event;
	while (m_Controls->GetEvent(&event)) {
		const auto* control = event.GetControl();
		if (event.GetType() == GUIEvent::Command && control == m_Close) { SetOpen(false); continue; }
		if (!m_Open) continue;
		for (size_t row = 0; row < m_Seats.size(); ++row) {
			const auto& widgets = m_Seats[row];
			if (control != widgets.applicant && std::find(widgets.actions.begin(), widgets.actions.end(), control) == widgets.actions.end()) continue;
			if (event.GetType() == GUIEvent::Notification && event.GetMsg() == GUIButton::Pushed && !m_Press && row < m_Model.RowCount()) {
				m_Press = Press{control, m_Model.GetRow(row)};
			} else if (event.GetType() == GUIEvent::Command && m_Press && m_Press->button == control) {
				if (control == widgets.applicant) m_Model.CycleApplicant(m_Press->row);
				for (size_t action = 0; action < widgets.actions.size(); ++action) {
					if (control == widgets.actions[action]) m_ActionResult = m_Model.Act(m_Press->row, static_cast<NetModerationAction>(action));
				}
				m_Press.reset();
			} else if (event.GetType() == GUIEvent::Notification && event.GetMsg() == GUIButton::UnPushed &&
			           !static_cast<GUIButton*>(event.GetControl())->IsCaptured()) {
				m_Press.reset();
			}
		}
	}
}

void NetModerationGUI::Update() {
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	if (m_Open && snapshot.serviceState != "Running" && snapshot.serviceState != "Starting" && snapshot.serviceState != "ReadyToLaunch") SetOpen(false);
	UpdateMatchChat(snapshot);
	if (!m_Open) return;
	g_UInputMan.TrapMousePos(false);
	m_Controls->Update();
	HandleEvents();
	Refresh();
}

void NetModerationGUI::DrawRoster(const NetLobbySnapshot& snapshot) {
	AllegroBitmap bitmap(g_FrameMan.GetBackBuffer32());
	auto* font = g_FrameMan.GetSmallFont(true);
	const int width = std::min(412, g_WindowMan.GetResX() - 16);
	int y = 8;
	std::string text = "Seats  [F6]";
	// The announced input delay rides the corner box so the HUD shows what the lobby showed.
	if (!snapshot.inputDelayText.empty()) text += "\n" + snapshot.inputDelayText;
	for (const auto& member: snapshot.members) {
		if (member.cpu) continue;
		text += "\n" + (member.statusLine.empty() ? DisplayName(member.displayName) + "  /  Connected" : DisplayName(member.statusLine));
	}
	text = WrapText(font, text, width - 12);
	const int height = font->CalculateHeight(text) + 12;
	rectfill(g_FrameMan.GetBackBuffer32(), 8, y, width + 8, y + height, makeacol32(20, 22, 27, 255));
	font->DrawAligned(&bitmap, 14, y + 6, text, GUIFont::Left, GUIFont::Top);
}

bool NetModerationGUI::MatchStatusWanted() const {
	switch (g_SettingsMan.GetNetworkMatchStatusMode()) {
		case SettingsMan::NetworkMatchStatusMode::Off: return false;
		case SettingsMan::NetworkMatchStatusMode::Always: return true;
		default: break;
	}
	// The countdown is still a pause state, so it must not blink the widget off.
	bool active = m_Open || g_NetMatchService.IsMatchResyncing() || ScenarioRunner::IsLockstepPaused() || ScenarioRunner::GetLockstepResumeCountdown() > 0;
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
	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	if (ScenarioRunner::IsLockstepControllerSyncActive()) {
		m_MatchDelayFrames = ScenarioRunner::GetLockstepLocalInputDelay();
		m_BaseDelayFrames = ScenarioRunner::GetLockstepInputDelayFrames();
	}
	char metrics[128];
	std::snprintf(metrics, sizeof(metrics), "delay %u ticks / %.1f ms", static_cast<unsigned>(m_MatchDelayFrames), static_cast<double>(m_MatchDelayFrames) * 1000.0 / 60.0);
	const auto ping = g_NetMatchService.GetMatchPingMs();
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
	const auto* setupActivity = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
	const bool placing = !resyncing && setupActivity && setupActivity->DescribeLockstepPlacementWait(placementNames, placed, seats);
	const bool holdPause = !resyncing && !placing && ScenarioRunner::DescribeLockstepHoldPause(holdName, holdSeconds);
	const bool missingFrames = !resyncing && !placing && !holdPause &&
	    static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) > ScenarioRunner::GetLockstepCompletedFrame();
	const bool paused = !resyncing && !placing && !holdPause && !missingFrames && ScenarioRunner::IsLockstepPaused();
	const int countdown = paused ? ScenarioRunner::GetLockstepResumeCountdown() : 0;
	const bool waiting = resyncing || placing || holdPause || missingFrames;
	const EditorArea editor = FreeArea(backbuffer->w);
	const std::string countOnly = std::to_string(placed) + " of " + std::to_string(seats);
	const int countNeed = font->CalculateWidth(countOnly) + 14;
	if (backbuffer->h < c_CompactMaxHeight) {
		// The short-screen layout is one line in the gap between the funds block and the controller icon;
		// while the editor holds the world it takes the widest column-free gap, or the top band when none fits.
		const int fullHeight = font->GetFontHeight() + 7;
		int height = fullHeight;
		int y = editor.editing ? backbuffer->h - height - 2 : 2;
		bool topBand = false;
		if (m_Open) {
			// An open seats panel reaches the top of a compact screen, so a strip crossing its rows lifts
			// above it - shrinking to a bare line of them when that is all the room there is. The toast
			// row the panel's band reserves sits under the strip, so a live toast lowers the ceiling to it,
			// and the editor's own seat message bands lower it further still.
			int panelX, panelTop, panelWidth, panelHeight;
			m_Panel->GetControlRect(&panelX, &panelTop, &panelWidth, &panelHeight);
			const int rowHeight = std::max(12, font->GetFontHeight()) + 8;
			int ceiling = ScenarioRunner::GetVisibleNetUiToasts().empty() ? panelTop : panelTop - 4 - rowHeight;
			if (editor.editing) {
				for (const auto& band: editor.textBands) {
					if (band.y < ceiling) ceiling = band.y;
				}
			}
			if (y < panelTop + panelHeight && y + height > ceiling) {
				y = ceiling - height;
				if (y < 0) {
					y = 0;
					height = std::min(height, ceiling);
				}
			}
		}
		int freeLeft = 152, freeRight = backbuffer->w - 40;
		if (editor.editing) {
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
		const std::string pingText = ping ? std::to_string(*ping) : "--";
		char tail[96];
		std::snprintf(tail, sizeof(tail), " / delay %u / RTT %s ms / PACE %.1f tps", static_cast<unsigned>(m_MatchDelayFrames), pingText.c_str(), s_paceTps);
		auto compose = [&](const std::string& metrics, bool shortenNames) {
			std::string line = "NET [F6] / ";
			if (resyncing) {
				line += "RESYNCING MATCH";
			} else if (placing) {
				const std::string count = " / " + std::to_string(placed) + " of " + std::to_string(seats);
				const int room = maxTextWidth - font->CalculateWidth(line + "WAITING FOR  TO PLACE" + count + metrics);
				const std::string names = shortenNames ? FitLine(font, placementNames, room) : DisplayName(placementNames);
				line += placementNames.empty() ? "ALL BRAINS PLACED" : "WAITING FOR " + names + " TO PLACE";
				line += count;
			} else if (holdPause) {
				const std::string who = holdName.empty() ? "a player" : holdName;
				const int room = maxTextWidth - font->CalculateWidth(line + "WAITING FOR  " + metrics) - font->CalculateWidth(" (999 s)");
				line += "WAITING FOR " + (shortenNames ? FitLine(font, who, room) : DisplayName(who)) + " (" + std::to_string(holdSeconds) + " s)";
			} else if (missingFrames) {
				line += "WAITING FOR FRAMES";
			} else if (paused) {
				line += countdown > 0 ? "RESUMING IN " + std::to_string((countdown + 59) / 60) + " S" : "PAUSED / P RESUMES";
			} else {
				line += "LIVE";
			}
			return line + metrics;
		};
		// An open picker leaves a short screen 280 px, so the line gives way in this order: the whole line,
		// then a shortening pass, then whatever remains. The count ends the line, so it outlives all of
		// them - the last resort drops everything but it rather than clip it off the end.
		if (topBand) {
			m_StripText = countOnly;
		} else {
			m_StripText = compose(tail, false);
			if (font->CalculateWidth(m_StripText) > maxTextWidth) {
				m_StripText = compose("", false);
			}
			if (font->CalculateWidth(m_StripText) > maxTextWidth) {
				m_StripText = compose("", true);
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
		return;
	}
	// The box is tall enough that a picker column beside it matters wherever the rows land, so its span
	// keeps every column out rather than only the ones crossing the box's own band.
	int freeLeft = 0, freeRight = backbuffer->w;
	editor.FreeSpan(0, backbuffer->h, backbuffer->w, freeLeft, freeRight);
	int available = freeRight - freeLeft;
	bool topBand = editor.editing && available < countNeed;
	if (topBand) {
		freeLeft = 0;
		freeRight = backbuffer->w;
		available = backbuffer->w;
	}
	const int width = std::max(1, std::min(c_StatusBoxWidth, std::max(1, available - 2 * c_StatusBoxMargin)));
	m_NetStatusBox->SetVisible(true);
	m_NetStatus->SetFont(font);
	// Measured at the final width, so the height below is the height these rows really need.
	m_NetStatus->Resize(width - 12, backbuffer->h);
	std::string text = std::string("NET STATUS  /  SEATS [F6]\n") + metrics;
	if (m_MatchDelayFrames != m_BaseDelayFrames) {
		text += " (base " + std::to_string(m_BaseDelayFrames) + ")";
	}
	text += "\nRTT " + (ping ? std::to_string(*ping) : "--") + " ms / " + (snapshot.isHost ? "max peer" : "host link");
	std::snprintf(metrics, sizeof(metrics), "\nPACE %.1f tps", s_paceTps);
	text += metrics;
	if (resyncing) {
		text += "\nRESYNCING MATCH";
	} else if (placing) {
		const int room = width - 12 - font->CalculateWidth("Waiting for  to place their brains");
		text += placementNames.empty() ? "\nAll brains placed" : "\nWaiting for " + FitLine(font, placementNames, room) + " to place their brains";
		text += "\n" + std::to_string(placed) + " of " + std::to_string(seats) + " placed";
	} else if (holdPause) {
		const int room = width - 12 - font->CalculateWidth("Waiting for  to reconnect");
		text += "\nWaiting for " + FitLine(font, holdName.empty() ? "a player" : holdName, room) + " to reconnect\n" + std::to_string(holdSeconds) + " s left";
	} else if (missingFrames) {
		text += "\nWAITING FOR FRAMES\n" + FitLine(font, ScenarioRunner::GetLockstepMissingPeers(), width - 12);
	} else if (paused) {
		text += countdown > 0 ? "\nResuming in " + std::to_string((countdown + 59) / 60) + " s" : "\nPAUSED / P to resume";
	} else {
		text += "\nLIVE";
	}
	m_NetStatus->SetText(text);
	// The box grows for a state that needs more rows than the metric ones; those keep the stock height.
	const int height = std::max(c_StatusBoxHeight, m_NetStatus->GetTextHeight() + 12);
	// The editor's own top band and picker column are its own, so the box takes the bottom of the rest.
	// A collapsed span takes the window's top band instead of a zero-width box.
	const int x = topBand ? std::max(0, (backbuffer->w - width) / 2) :
	    (editor.editing ? freeLeft + std::max(0, (available - width) / 2) : backbuffer->w - width - c_StatusBoxMargin);
	int y = topBand ? 2 : (editor.editing ? backbuffer->h - height - c_StatusBoxMargin : c_StatusBoxTop);
	if (m_Open) {
		// The same rule the compact strip follows: an open seats panel owns its rows, so a box crossing
		// them lifts above it.
		int panelX, panelTop, panelWidth, panelHeight;
		m_Panel->GetControlRect(&panelX, &panelTop, &panelWidth, &panelHeight);
		if (y < panelTop + panelHeight && y + height > panelTop) {
			y = std::max(0, panelTop - height);
		}
	}
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
}

void NetModerationGUI::UpdateMatchChat(const NetLobbySnapshot& snapshot) {
	const bool inMatch = ScenarioRunner::IsLockstepControllerSyncActive() || g_NetMatchService.IsMatchResyncing();
	if (!inMatch) {
		m_MatchChatLines.clear();
		if (m_ChatEntryOpen) {
			m_ChatEntryOpen = false;
			if (m_MatchChatInput) {
				m_MatchChatInput->SetText("");
				m_MatchChatInput->SetVisible(false);
			}
			if (m_ChatDisabledKeys) {
				g_UInputMan.DisableKeys(false);
				m_ChatDisabledKeys = false;
			}
			if (!m_Open) m_Input->SetKeyJoyMouseCursor(false);
		}
		return;
	}

	const auto history = g_NetMatchService.ChatHistory();
	std::deque<MatchChatLine> next;
	const long long nowUs = g_TimerMan.GetAbsoluteTime();
	for (const NetChatEntry& entry: history) {
		MatchChatLine line;
		line.receivedTick = entry.receivedTick;
		line.senderPeerId = entry.senderPeerId;
		line.scope = entry.scope;
		line.name = entry.senderName;
		line.text = entry.text;
		line.seenUs = nowUs;
		for (const auto& member: snapshot.members) {
			if (member.peerId != entry.senderPeerId) continue;
			line.team = member.team;
			if (line.name.empty()) line.name = member.displayName;
			break;
		}
		for (const auto& prior: m_MatchChatLines) {
			if (prior.receivedTick == line.receivedTick && prior.senderPeerId == line.senderPeerId && prior.text == line.text) {
				line.seenUs = prior.seenUs;
				line.team = prior.team ? prior.team : line.team;
				if (line.name.empty()) line.name = prior.name;
				break;
			}
		}
		next.push_back(std::move(line));
	}
	m_MatchChatLines = std::move(next);

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
	const bool keyChat = !m_ChatEntryOpen && !consoleOpen && g_UInputMan.KeyPressed(chatKey);
	if ((scriptChat || keyChat) && !m_ChatKeysHeld && !m_ChatEntryOpen && !consoleOpen) {
		CreateOverlay();
		m_ChatEntryOpen = true;
		if (m_MatchChatInput) {
			m_MatchChatInput->SetText("");
			m_MatchChatInput->SetVisible(true);
			if (GUIPanel* panel = m_MatchChatInput->GetPanel()) panel->SetFocus();
		}
		if (!m_ChatDisabledKeys) {
			g_UInputMan.DisableKeys(true);
			m_ChatDisabledKeys = true;
		}
		m_Input->SetKeyJoyMouseCursor(true);
	}
	m_ChatKeysHeld = scriptChat || g_UInputMan.KeyHeld(chatKey);

	if (!m_ChatEntryOpen) return;

	if (g_UInputMan.KeyPressed(SDL_SCANCODE_ESCAPE)) {
		m_ChatEntryOpen = false;
		if (m_MatchChatInput) {
			m_MatchChatInput->SetText("");
			m_MatchChatInput->SetVisible(false);
		}
		if (m_ChatDisabledKeys) {
			g_UInputMan.DisableKeys(false);
			m_ChatDisabledKeys = false;
		}
		if (!m_Open) m_Input->SetKeyJoyMouseCursor(false);
		return;
	}

	if (m_OverlayControls) {
		m_OverlayControls->Update();
		GUIEvent event;
		while (m_OverlayControls->GetEvent(&event)) {
			if (event.GetType() == GUIEvent::Notification && event.GetMsg() == GUITextBox::Enter && event.GetControl() == m_MatchChatInput) {
				const std::string text = m_MatchChatInput->GetText();
				if (!text.empty()) {
					const int modifier = m_OverlayControls->GetManager()->GetInputController()->GetModifier();
					const uint8_t scope = (modifier & GUIPanel::MODI_CTRL) ? c_NetChatScopeTeam : c_NetChatScopeAll;
					if (!g_NetMatchService.SendChat(scope, text)) continue;
				}
				m_ChatEntryOpen = false;
				m_MatchChatInput->SetText("");
				m_MatchChatInput->SetVisible(false);
				if (m_ChatDisabledKeys) {
					g_UInputMan.DisableKeys(false);
					m_ChatDisabledKeys = false;
				}
				if (!m_Open) m_Input->SetKeyJoyMouseCursor(false);
			}
		}
	}
}

void NetModerationGUI::DrawMatchChat(const NetLobbySnapshot& snapshot) {
	(void)snapshot;
	if (!m_OverlayControls) {
		m_ChatRect = {};
		return;
	}
	const bool showHistory = g_SettingsMan.GetNetworkChatVisible();
	if (!showHistory && !m_ChatEntryOpen) {
		for (GUILabel* label: m_MatchChat) {
			if (!label) continue;
			label->SetVisible(false);
			label->SetText("");
		}
		if (m_MatchChatInput) m_MatchChatInput->SetVisible(false);
		m_ChatRect = {};
		return;
	}

	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	GUIFont* font = g_SettingsMan.GetNetworkChatTextSize() == SettingsMan::NetworkChatTextSize::Large ?
	    g_FrameMan.GetLargeFont(true) : g_FrameMan.GetSmallFont(true);
	if (!font) font = g_FrameMan.GetSmallFont(true);
	const int lineH = std::max(12, font->GetFontHeight()) + 4;
	const int inputH = m_ChatEntryOpen ? lineH + 6 : 0;
	EditorArea area = FreeArea(backbuffer->w);
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
	for (const auto& band: area.textBands) {
		if (band.y + band.h > backbuffer->h / 2) lower(band.y - 4);
	}

	int rows = showHistory ? static_cast<int>(std::min(m_MatchChat.size(), m_MatchChatLines.size())) : 0;
	if (m_ChatEntryOpen && rows == 0) rows = 0;
	int available = std::max(0, bottom - 4);
	int maxRows = std::min(static_cast<int>(m_MatchChat.size()), std::max(0, (available - inputH) / lineH));
	if (rows > maxRows) rows = maxRows;
	// A 640x360 match still owes the player one yielded row before the band clips off.
	if (rows == 0 && showHistory && !m_MatchChatLines.empty() && maxRows == 0 && available >= lineH + inputH) {
		rows = 1;
	}
	const int height = rows * lineH + inputH;
	// An open entry that the free area cannot hold gives way rather than drawing over an occupier.
	if (height <= 0 || height > available) {
		for (GUILabel* label: m_MatchChat) {
			if (!label) continue;
			label->SetVisible(false);
			label->SetText("");
		}
		if (m_MatchChatInput) m_MatchChatInput->SetVisible(false);
		m_ChatRect = {};
		return;
	}
	int top = std::max(0, bottom - height);
	int freeLeft = 8, freeRight = backbuffer->w - 8;
	area.FreeSpan(top, bottom, backbuffer->w, freeLeft, freeRight);
	if (freeRight - freeLeft < 80) {
		freeLeft = 8;
		freeRight = backbuffer->w - 8;
		area.FreeSpan(top, bottom, backbuffer->w, freeLeft, freeRight);
	}
	const int width = std::max(1, std::min(420, std::max(80, freeRight - freeLeft - 8)));
	const int x = std::max(0, std::min(backbuffer->w - width, freeLeft + std::max(0, (freeRight - freeLeft - width) / 2)));
	m_ChatRect = {x, top, width, height, true};

	const long long nowUs = g_TimerMan.GetAbsoluteTime();
	const size_t first = m_MatchChatLines.size() > static_cast<size_t>(rows) ? m_MatchChatLines.size() - static_cast<size_t>(rows) : 0;
	AllegroBitmap bitmap(backbuffer);
	for (size_t row = 0; row < m_MatchChat.size(); ++row) {
		GUILabel* label = m_MatchChat[row];
		const bool shown = row < static_cast<size_t>(rows);
		label->SetVisible(shown);
		if (!shown) {
			label->SetText("");
			continue;
		}
		const MatchChatLine& line = m_MatchChatLines[first + row];
		long long ageUs = nowUs - line.seenUs;
		if (ageUs < 0) ageUs = 0;
		int alpha = 255;
		if (!m_ChatEntryOpen && ageUs > 8000000) {
			const long long fade = ageUs - 8000000;
			alpha = fade >= 4000000 ? 90 : static_cast<int>(255 - (fade * 165) / 4000000);
		}
		std::string prefix = line.scope == c_NetChatScopeTeam ? "[TEAM] " : "";
		std::string text = prefix + DisplayName(line.name.empty() ? "Player" : line.name) + ": " + DisplayName(line.text);
		text = FitLine(font, std::move(text), width - 18);
		const int y = top + static_cast<int>(row) * lineH;
		label->SetFont(font);
		label->Move(x + 8, y + 2);
		label->Resize(width - 12, lineH - 2);
		label->SetText(text);
		rectfill(backbuffer, x, y, x + width - 1, y + lineH - 2, makeacol32(20, 22, 27, alpha));
		rectfill(backbuffer, x, y, x + 2, y + lineH - 2, TeamBarColor(line.team, alpha));
		label->Draw(&bitmap, false);
	}
	if (m_ChatEntryOpen && m_MatchChatInput) {
		const int y = top + rows * lineH;
		m_MatchChatInput->SetVisible(true);
		m_MatchChatInput->Move(x + 4, y + 2);
		m_MatchChatInput->Resize(width - 8, inputH - 4);
		rectfill(backbuffer, x, y, x + width - 1, y + inputH - 1, makeacol32(20, 22, 27, 230));
		rect(backbuffer, x, y, x + width - 1, y + inputH - 1, makeacol32(59, 65, 83, 255));
		m_MatchChatInput->Draw(m_Screen);
	} else if (m_MatchChatInput) {
		m_MatchChatInput->SetVisible(false);
	}
}

void NetModerationGUI::DrawMatchToasts() {
	m_ToastRect = {};
	if (!ScenarioRunner::IsLockstepControllerSyncActive() && !g_NetMatchService.IsMatchResyncing()) {
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
	const auto visible = ScenarioRunner::GetVisibleNetUiToasts();
	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	const int rowHeight = std::max(12, font->GetFontHeight()) + 8;
	const EditorArea editor = FreeArea(backbuffer->w);
	// The status widget takes the bottom while the editor holds the world, so the rows stack above it.
	int bottom = editor.editing && m_StatusRect.visible ? m_StatusRect.y - 4 : backbuffer->h - 8;
	// A compact screen with the seats panel open reserves one toast row under the strip band: the
	// newest toast takes it and the rest of the stack waits for the room to come back.
	const bool reserved = m_Open && backbuffer->h < c_CompactMaxHeight;
	if (m_Open) {
		// The seats panel owns its rows too: a stack that would cross them piles up above it instead.
		// On a compact screen the reservation is the only band the stack gets: the editing anchor
		// points at a strip the open panel already lifted off the bottom, so the reservation wins.
		int panelX, panelTop, panelWidth, panelHeight;
		m_Panel->GetControlRect(&panelX, &panelTop, &panelWidth, &panelHeight);
		bottom = reserved ? panelTop - 4 : std::min(bottom, panelTop - 4);
	}
	size_t firstRow = reserved && !visible.empty() ? visible.size() - 1 : 0;
	size_t rowCount = visible.size() - firstRow;
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
		// No column-free span wide enough: the newest toast takes the top band, the rest wait.
		firstRow = visible.empty() ? 0 : visible.size() - 1;
		rowCount = visible.empty() ? 0 : 1;
		top = 2;
		bottom = 2 + static_cast<int>(rowCount) * rowHeight;
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
	}
	t_simRNGOverride = previousRNG;
}

void NetModerationGUI::Draw() {
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	m_StatusRect = {};
	m_ChatRect = {};
	if (m_NetStatusBox) {
		m_NetStatusBox->SetVisible(false);
		m_NetStatus->SetVisible(false);
	}
	if (snapshot.serviceState != "Running" && snapshot.serviceState != "Starting" && snapshot.serviceState != "ReadyToLaunch" && !m_Open) return;
	RandomGenerator* previousRNG = t_simRNGOverride;
	t_simRNGOverride = &g_RenderRNG;
	const bool inMatch = ScenarioRunner::IsLockstepControllerSyncActive() || g_NetMatchService.IsMatchResyncing();
	if (inMatch) {
		CreateOverlay();
	} else {
		DrawRoster(snapshot);
	}
	if (m_Open) m_Controls->Draw();
	// Opening the panel is one of the status widget's triggers, so the status draws over it: a screen too
	// short for both still owes the player the reading it just asked for.
	if (inMatch && MatchStatusWanted()) {
		DrawMatchStatus(snapshot);
		m_NetStatus->SetVisible(true);
	}
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
}

bool NetModerationGUI::AutomationModerate(const std::string& action, int stableSeat) {
	if (g_NetMatchService.GetState() != NetMatchServiceState::Running) return false;
	if (!m_Open && !SetOpen(true)) return false;
	Refresh();
	const size_t row = stableSeat < 0 ? 0 : m_Model.FindSeat(static_cast<uint16_t>(stableSeat));
	if (row >= m_Model.RowCount()) return false;
	NetModerationAction verb;
	if (action == "wait") verb = NetModerationAction::Wait;
	else if (action == "substitute") verb = NetModerationAction::Substitute;
	else if (action == "cancel") verb = NetModerationAction::Cancel;
	else return false;
	if (!NetModerationUx::Available(m_Model.GetRow(row), verb)) return false;
	auto* button = m_Seats[row].actions[static_cast<size_t>(verb)];
	if (!button->GetEnabled() || !button->GetVisible()) return false;
	int x, y, width, height;
	button->GetControlRect(&x, &y, &width, &height);
	m_ActionResult.reset();
	button->OnMouseDown(x + width / 2, y + height / 2, GUIPanel::MOUSE_LEFT, 0);
	HandleEvents();
	button->OnMouseUp(x + width / 2, y + height / 2, GUIPanel::MOUSE_LEFT, 0);
	HandleEvents();
	Refresh();
	return m_ActionResult == NetH4ModerationResult::Ok;
}

int NetModerationGUI::ChatKeyScancode() {
	return static_cast<int>(ChatScancode());
}

GUIControl* NetModerationGUI::GetControl(const std::string& name) const {
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
	if (m_OverlayControls) {
		if (GUIControl* overlay = m_OverlayControls->GetControl(name)) return overlay;
	}
	return m_Controls->GetControl(name);
}
