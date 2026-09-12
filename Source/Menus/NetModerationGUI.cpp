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

#include <algorithm>

using namespace RTE;

namespace {
	std::string DisplayName(std::string text) {
		for (char& c: text) if (static_cast<unsigned char>(c) < 32) c = ' ';
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
	m_Input(std::make_unique<GUIInputWrapper>(-1, true)), m_Controls(std::make_unique<GUIControlManager>()) {
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

void NetModerationGUI::Draw() {
	const auto snapshot = g_NetMatchService.GetLobbySnapshot();
	if (snapshot.serviceState != "Running" && snapshot.serviceState != "Starting" && snapshot.serviceState != "ReadyToLaunch" && !m_Open) return;
	DrawRoster(snapshot);
	if (m_Open) {
		m_Controls->Draw();
		m_Controls->DrawMouse();
	}
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

GUIControl* NetModerationGUI::GetControl(const std::string& name) const { return m_Controls->GetControl(name); }
