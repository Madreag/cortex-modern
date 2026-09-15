#include "NetModerationGUI.h"

#include "ActivityMan.h"
#include "CameraMan.h"
#include "GameActivity.h"
#include "NetMatchService.h"
#include "ScenarioRunner.h"
#include "SettingsMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "UInputMan.h"
#include "GUI.h"
#include "GUIInputWrapper.h"
#include "GUIControlManager.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUILabel.h"
#include "GUISkin.h"
#include "AllegroScreen.h"
#include "AllegroBitmap.h"
#include "RTEError.h"
#include "TimerMan.h"
#include "RTETools.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

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
		int left = 0, right = 0;
	};

	/// The column the stock picker settles into (Base.rte/GUIs/ObjectPickerGUI.ini [PickerGUIBox] Width),
	/// reserved whole from the first frame of its slide so the overlay holds one place while it animates.
	constexpr int c_EditorPanelWidth = 360;

	EditorArea FreeArea(int screenWidth) {
		EditorArea area;
		area.right = screenWidth;
		const auto* game = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
		if (!game || game->GetActivityState() != Activity::ActivityState::Editing) return area;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (!(game->IsSeatActive(player) && game->IsLocalHumanSeat(player))) continue;
			area.editing = true;
			const int occlusion = g_CameraMan.GetScreenOcclusion(game->ScreenOfPlayer(player)).GetRoundIntX();
			if (occlusion < 0) {
				area.right = std::max(0, screenWidth - std::max(c_EditorPanelWidth, -occlusion));
			} else if (occlusion > 0) {
				area.left = std::min(screenWidth, std::max(c_EditorPanelWidth, occlusion));
			}
			break;
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

void NetModerationGUI::Refresh() {
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	m_Model.Refresh(g_NetMatchService.GetModerationSeats());
	// The hold is the round's, read from the same place the stall overlay reads it.
	std::string holdName;
	uint32_t holdSeconds = 0;
	const bool holdPause = ScenarioRunner::DescribeLockstepHoldPause(holdName, holdSeconds);
	m_Title->SetText(NetModerationPanelTitle(snapshot.serviceState == "Running", holdPause, DisplayName(holdName), holdSeconds));
	m_Summary->SetText(snapshot.isHost ? m_Model.GetSummaryText() : "Only the host can approve a substitute.");
	m_Status->SetText(WrapText(m_LabelFont, m_Model.GetStatusText(), m_Status->GetWidth()));
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
	std::snprintf(metrics, sizeof(metrics), "D %u ticks / %.1f ms", static_cast<unsigned>(m_MatchDelayFrames), static_cast<double>(m_MatchDelayFrames) * 1000.0 / 60.0);
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
	if (backbuffer->h < c_CompactMaxHeight) {
		// The short-screen layout is one line in the gap between the funds block and the controller icon;
		// while the editor holds the world it takes the bottom of what the picker leaves instead.
		const int freeLeft = editor.editing ? editor.left + 4 : 152;
		const int freeRight = editor.editing ? editor.right - 4 : backbuffer->w - 40;
		const int maxTextWidth = freeRight - freeLeft - 14;
		const std::string pingText = ping ? std::to_string(*ping) : "--";
		char tail[96];
		std::snprintf(tail, sizeof(tail), " / D %u / RTT %s ms / PACE %.1f tps", static_cast<unsigned>(m_MatchDelayFrames), pingText.c_str(), s_paceTps);
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
		// then the metrics tail, then the names. The count ends the line, so it outlives all of them.
		m_StripText = compose(tail, false);
		if (font->CalculateWidth(m_StripText) > maxTextWidth) {
			m_StripText = compose("", false);
		}
		if (font->CalculateWidth(m_StripText) > maxTextWidth) {
			m_StripText = compose("", true);
		}
		const int height = font->GetFontHeight() + 7;
		const int width = std::min(freeRight - freeLeft, font->CalculateWidth(m_StripText) + 14);
		const int x = freeLeft + (freeRight - freeLeft - width) / 2;
		const int y = editor.editing ? backbuffer->h - height - 2 : 2;
		m_NetStatusBox->Move(x, y);
		if (m_NetStatusBox->GetWidth() != width || m_NetStatusBox->GetHeight() != height) m_NetStatusBox->Resize(width, height);
		m_NetStatusBox->SetVisible(true);
		m_NetStatus->SetFont(font);
		// GUILabel::Move is in screen coordinates, so the inset is measured from the strip itself.
		m_NetStatus->Move(x + 7, y + 4);
		m_NetStatus->Resize(width - 14, height - 6);
		m_NetStatus->SetText(m_StripText);
		m_StatusRect = {x, y, width, height, true};
		AllegroBitmap bitmap(backbuffer);
		rectfill(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(20, 22, 27, 255));
		rect(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(59, 65, 83, 255));
		hline(backbuffer, x + 1, y + 1, x + width - 2, waiting ? makeacol32(170, 120, 0, 255) : makeacol32(108, 118, 168, 255));
		m_NetStatus->Draw(&bitmap, false);
		return;
	}
	const int available = editor.right - editor.left;
	const int width = std::min(c_StatusBoxWidth, available - 2 * c_StatusBoxMargin);
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
	const int x = editor.editing ? editor.left + (available - width) / 2 : backbuffer->w - width - c_StatusBoxMargin;
	const int y = editor.editing ? backbuffer->h - height - c_StatusBoxMargin : c_StatusBoxTop;
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
	const int available = editor.right - editor.left;
	const int width = std::min(520, available - 32);
	const int x = editor.left + (available - width) / 2;
	// The status widget takes the bottom while the editor holds the world, so the rows stack above it.
	const int bottom = editor.editing && m_StatusRect.visible ? m_StatusRect.y - 4 : backbuffer->h - 8;
	const int top = bottom - static_cast<int>(visible.size()) * rowHeight;
	if (!visible.empty()) {
		m_ToastRect = {x, top, width, static_cast<int>(visible.size()) * rowHeight - 2, true};
	}
	AllegroBitmap bitmap(backbuffer);
	for (size_t row = 0; row < m_Toasts.size(); ++row) {
		GUILabel* label = m_Toasts[row];
		const bool shown = row < visible.size();
		label->SetVisible(shown);
		label->SetText(shown ? FitToastText(font, visible[row], width - 16) : std::string());
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

GUIControl* NetModerationGUI::GetControl(const std::string& name) const {
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
