#include "NetModerationGUI.h"

#include "NetMatchService.h"
#include "ScenarioRunner.h"
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

using namespace RTE;

namespace {
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
	const int width = std::min(600, g_WindowMan.GetResX() - 12);
	m_Panel = dynamic_cast<GUICollectionBox*>(m_Controls->AddControl("NetworkSeats", "COLLECTIONBOX", nullptr,
	    (g_WindowMan.GetResX() - width) / 2, (g_WindowMan.GetResY() - 344) / 2, width, 344));
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

void NetModerationGUI::SetMatchPace(uint64_t ticks, long long wallUs) {
	m_MatchPaceTps = wallUs > 0 ? static_cast<double>(ticks) * 1000000.0 / static_cast<double>(wallUs) : 0.0;
}

void NetModerationGUI::DrawMatchStatus(const NetLobbySnapshot& snapshot) {
	CreateOverlay();
	BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	GUIFont* font = g_FrameMan.GetSmallFont(true);
	const int width = std::min(252, backbuffer->w - 16);
	const int x = backbuffer->w - width - 8;
	constexpr int y = 32;
	constexpr int height = 76;
	m_NetStatusBox->Move(x, y);
	if (m_NetStatusBox->GetWidth() != width) m_NetStatusBox->Resize(width, height);
	m_NetStatusBox->SetVisible(true);
	m_NetStatus->SetFont(font);
	m_NetStatus->Resize(width - 12, height - 12);
	if (ScenarioRunner::IsLockstepControllerSyncActive()) {
		m_MatchDelayFrames = ScenarioRunner::GetLockstepLocalInputDelay();
		m_BaseDelayFrames = ScenarioRunner::GetLockstepInputDelayFrames();
	}
	char metrics[128];
	std::snprintf(metrics, sizeof(metrics), "D %u ticks / %.1f ms", static_cast<unsigned>(m_MatchDelayFrames), static_cast<double>(m_MatchDelayFrames) * 1000.0 / 60.0);
	std::string text = std::string("NET STATUS  /  SEATS [F6]\n") + metrics;
	if (m_MatchDelayFrames != m_BaseDelayFrames) {
		text += " (base " + std::to_string(m_BaseDelayFrames) + ")";
	}
	const auto ping = g_NetMatchService.GetMatchPingMs();
	text += "\nRTT " + (ping ? std::to_string(*ping) : "--") + " ms / " + (snapshot.isHost ? "max peer" : "host link");
	std::snprintf(metrics, sizeof(metrics), "\nPACE %.1f tps", m_MatchPaceTps);
	text += metrics;
	std::string holdName;
	uint32_t holdSeconds = 0;
	bool waiting = false;
	if (g_NetMatchService.IsMatchResyncing()) {
		text += "\nRESYNCING MATCH";
		waiting = true;
	} else if (ScenarioRunner::DescribeLockstepHoldPause(holdName, holdSeconds)) {
		const int room = width - 12 - font->CalculateWidth("Waiting for  to reconnect");
		text += "\nWaiting for " + FitLine(font, holdName.empty() ? "a player" : holdName, room) + " to reconnect\n" + std::to_string(holdSeconds) + " s left";
		waiting = true;
	} else if (static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) > ScenarioRunner::GetLockstepAppliedFrame()) {
		text += "\nWAITING FOR FRAMES\n" + FitLine(font, ScenarioRunner::GetLockstepMissingPeers(), width - 12);
		waiting = true;
	} else if (ScenarioRunner::IsLockstepPaused()) {
		const int countdown = ScenarioRunner::GetLockstepResumeCountdown();
		text += countdown > 0 ? "\nResuming in " + std::to_string((countdown + 59) / 60) + " s" : "\nPAUSED / P to resume";
	} else {
		text += "\nLIVE";
	}
	m_NetStatus->SetText(text);
	AllegroBitmap bitmap(backbuffer);
	rectfill(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(20, 22, 27, 255));
	rect(backbuffer, x, y, x + width - 1, y + height - 1, makeacol32(59, 65, 83, 255));
	hline(backbuffer, x + 1, y + 1, x + width - 2, waiting ? makeacol32(170, 120, 0, 255) : makeacol32(108, 118, 168, 255));
	m_NetStatus->Draw(&bitmap, false);
}

void NetModerationGUI::DrawMatchToasts() {
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
	const int width = std::min(520, backbuffer->w - 32);
	const int x = (backbuffer->w - width) / 2;
	const int top = backbuffer->h - 8 - static_cast<int>(visible.size()) * rowHeight;
	AllegroBitmap bitmap(backbuffer);
	for (size_t row = 0; row < m_Toasts.size(); ++row) {
		GUILabel* label = m_Toasts[row];
		const bool shown = row < visible.size();
		label->SetVisible(shown);
		label->SetText(shown ? FitLine(font, ToastText(visible[row]), width - 16) : std::string());
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
	if (m_NetStatusBox) {
		m_NetStatusBox->SetVisible(false);
		m_NetStatus->SetVisible(false);
	}
	if (snapshot.serviceState != "Running" && snapshot.serviceState != "Starting" && snapshot.serviceState != "ReadyToLaunch" && !m_Open) return;
	RandomGenerator* previousRNG = t_simRNGOverride;
	t_simRNGOverride = &g_RenderRNG;
	if (ScenarioRunner::IsLockstepControllerSyncActive() || g_NetMatchService.IsMatchResyncing()) {
		DrawMatchStatus(snapshot);
		m_NetStatus->SetVisible(true);
	} else {
		DrawRoster(snapshot);
	}
	if (m_Open) {
		m_Controls->Draw();
		m_Controls->DrawMouse();
	}
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
