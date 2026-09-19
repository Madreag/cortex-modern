#include "PauseMenuGUI.h"

#include "WindowMan.h"
#include "FrameMan.h"
#include "ConsoleMan.h"
#include "ActivityMan.h"
#include "UInputMan.h"
#include "SettingsMan.h"
#include "TelemetryBundle.h"

#include "SaveLoadMenuGUI.h"
#include "SettingsGUI.h"
#include "ModManagerGUI.h"

#include "NetLockstep.h"
#include "NetMatchService.h"
#include "NetHostOptionsText.h"
#include "ScenarioRunner.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUILabel.h"

#include "AllegroTools.h"

using namespace RTE;

void PauseMenuGUI::Clear() {
	m_GUIControlManager = nullptr;
	m_ActiveDialogBox = nullptr;

	m_BackdropBitmap = nullptr;

	m_ActiveMenuScreen = PauseMenuScreen::MainScreen;
	m_UpdateResult = PauseMenuUpdateResult::NoEvent;
	m_ResumeButtonBlinkTimer.Reset();

	m_SaveLoadMenu = nullptr;
	m_SettingsMenu = nullptr;
	m_ModManagerMenu = nullptr;

	m_ButtonHoveredText.fill(std::string());
	m_ButtonUnhoveredText.fill(std::string());
	m_DiagnosticsIdleText.fill(std::string());
	m_MatchPauseText.fill(std::string());
	m_MatchResumeText.fill(std::string());
	m_DiagnosticsBusy = false;
	m_MatchPausedShown = false;
	m_HoveredButton = nullptr;
	m_PendingAutomationCommand.clear();
	m_PrevHoveredButtonIndex = 0;

	m_SavingButtonsDisabled = false;
	m_ModManagerButtonDisabled = false;
	m_NetworkMatchMode = false;
	m_LeaveConfirmShown = false;
	m_BackRequested = false;

	m_PauseMenuBox = nullptr;
	m_PauseMenuButtons.fill(nullptr);
	m_ButtonHomeY.fill(0);
	m_PauseMenuBoxHomeY = 0;

	m_LeaveConfirmBox = nullptr;
	m_LeaveConfirmLabel = nullptr;

	m_MatchOptionsBox = nullptr;
	m_MatchOptionsLabel = nullptr;
	m_MatchOptionsShown = false;
}

void PauseMenuGUI::Create(AllegroScreen* guiScreen, GUIInputWrapper* guiInput) {
	m_AutomationInput = guiInput->CreateAutomationInput();
	if (m_AutomationInput) guiInput = m_AutomationInput.get();
	m_GUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_GUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuScreenSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuScreenSkin.ini");
	m_GUIControlManager->Load("Base.rte/GUIs/PauseMenuGUI.ini");

	int rootBoxMaxWidth = g_WindowMan.FullyCoversAllDisplays() ? g_WindowMan.GetPrimaryWindowDisplayWidth() / g_WindowMan.GetResMultiplier() : g_WindowMan.GetResX();

	dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("root"))->Resize(rootBoxMaxWidth, g_WindowMan.GetResY());

	m_PauseMenuBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("PauseScreen"));
	m_PauseMenuBox->CenterInParent(true, true);

	m_PauseMenuButtons[PauseMenuButton::BackToMainButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonBackToMain"));
	m_PauseMenuButtons[PauseMenuButton::SaveOrLoadGameButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonSaveOrLoadGame"));
	m_PauseMenuButtons[PauseMenuButton::SettingsButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonSettings"));
	m_PauseMenuButtons[PauseMenuButton::ModManagerButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonModManager"));
	m_PauseMenuButtons[PauseMenuButton::SaveDiagnosticsButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonSaveDiagnostics"));
	m_PauseMenuButtons[PauseMenuButton::PauseMatchButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonPauseMatch"));
	m_PauseMenuButtons[PauseMenuButton::LeaveMatchButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonLeaveMatch"));
	m_PauseMenuButtons[PauseMenuButton::MatchOptionsButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMatchOptions"));
	m_PauseMenuButtons[PauseMenuButton::EndMatchButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonEndMatch"));
	m_PauseMenuButtons[PauseMenuButton::ResumeButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonResume"));
	m_PauseMenuButtons[PauseMenuButton::LeaveConfirmButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonLeaveConfirm"));
	m_PauseMenuButtons[PauseMenuButton::LeaveCancelButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonLeaveCancel"));
	m_PauseMenuButtons[PauseMenuButton::MatchOptionsCloseButton] = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMatchOptionsClose"));

	for (size_t pauseMenuButton = 0; pauseMenuButton < m_PauseMenuButtons.size(); ++pauseMenuButton) {
		std::string buttonText = m_PauseMenuButtons[pauseMenuButton]->GetText();

		std::transform(buttonText.begin(), buttonText.end(), buttonText.begin(), ::toupper);
		m_ButtonHoveredText[pauseMenuButton] = buttonText;
		std::transform(buttonText.begin(), buttonText.end(), buttonText.begin(), ::tolower);
		m_ButtonUnhoveredText[pauseMenuButton] = buttonText;

		m_PauseMenuButtons[pauseMenuButton]->SetText(m_ButtonUnhoveredText[pauseMenuButton]);
		m_PauseMenuButtons[pauseMenuButton]->CenterInParent(true, false);
	}
	m_DiagnosticsIdleText = {m_ButtonHoveredText[PauseMenuButton::SaveDiagnosticsButton], m_ButtonUnhoveredText[PauseMenuButton::SaveDiagnosticsButton]};
	m_MatchPauseText = {m_ButtonHoveredText[PauseMenuButton::PauseMatchButton], m_ButtonUnhoveredText[PauseMenuButton::PauseMatchButton]};
	m_MatchResumeText = {"RESUME MATCH", "resume match"};

	m_LeaveConfirmBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("LeaveConfirmBox"));
	m_LeaveConfirmBox->CenterInParent(true, true);
	m_LeaveConfirmLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelLeaveConfirm"));

	m_MatchOptionsBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("MatchOptionsBox"));
	m_MatchOptionsBox->CenterInParent(true, true);
	m_MatchOptionsLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMatchOptions"));

	int boxPosX = 0;
	int boxPosY = 0;
	int boxWidth = 0;
	int boxHeight = 0;
	m_PauseMenuBox->GetControlRect(&boxPosX, &boxPosY, &boxWidth, &boxHeight);
	m_PauseMenuBoxHomeY = boxPosY;
	for (int pauseMenuButton = 0; pauseMenuButton < PauseMenuButton::LeaveConfirmButton; ++pauseMenuButton) {
		int buttonPosX = 0;
		int buttonPosY = 0;
		int buttonWidth = 0;
		int buttonHeight = 0;
		m_PauseMenuButtons[pauseMenuButton]->GetControlRect(&buttonPosX, &buttonPosY, &buttonWidth, &buttonHeight);
		m_ButtonHomeY[pauseMenuButton] = buttonPosY - boxPosY;
	}

	if (m_BackdropBitmap) {
		destroy_bitmap(m_BackdropBitmap);
	}
	const BITMAP* backbuffer = g_FrameMan.GetBackBuffer32();
	m_BackdropBitmap = create_bitmap_ex(FrameMan::c_BPP, backbuffer->w, backbuffer->h);
	unsigned int halfTransBlack = makeacol32(0, 0, 0, 96);
	clear_to_color(m_BackdropBitmap, halfTransBlack);

	m_SaveLoadMenu = std::make_unique<SaveLoadMenuGUI>(guiScreen, guiInput, true);
	m_SettingsMenu = std::make_unique<SettingsGUI>(guiScreen, guiInput, true);
	m_ModManagerMenu = std::make_unique<ModManagerGUI>(guiScreen, guiInput, true);
}

void PauseMenuGUI::SetBackButtonTargetName(const std::string& menuName) {
	std::string newButtonText = "Back to " + menuName + " Menu";

	std::transform(newButtonText.begin(), newButtonText.end(), newButtonText.begin(), ::toupper);
	m_ButtonHoveredText[PauseMenuButton::BackToMainButton] = newButtonText;
	std::transform(newButtonText.begin(), newButtonText.end(), newButtonText.begin(), ::tolower);
	m_ButtonUnhoveredText[PauseMenuButton::BackToMainButton] = newButtonText;

	int newButtonWidth = m_GUIControlManager->GetSkin()->GetFont("FontMainMenu.png")->CalculateWidth(newButtonText) + 50;

	m_PauseMenuButtons[PauseMenuButton::BackToMainButton]->SetSize(newButtonWidth, m_PauseMenuButtons[PauseMenuButton::BackToMainButton]->GetHeight());
	m_PauseMenuButtons[PauseMenuButton::BackToMainButton]->SetText(m_ButtonUnhoveredText[PauseMenuButton::BackToMainButton]);
	m_PauseMenuButtons[PauseMenuButton::BackToMainButton]->CenterInParent(true, false);
}

void PauseMenuGUI::EnableOrDisablePauseMenuFeatures() {
	bool disableModManager = true;

	if (const Activity* activity = g_ActivityMan.GetActivity(); activity) {
		disableModManager = activity->GetClassName() != "GAScripted";
	}

	if (m_ModManagerButtonDisabled != disableModManager) {
		GUIButton* modManagerButton = m_PauseMenuButtons[PauseMenuButton::ModManagerButton];

		modManagerButton->SetEnabled(!disableModManager);
		modManagerButton->SetVisible(!disableModManager);

		int yOffset = m_PauseMenuButtons[PauseMenuButton::ModManagerButton]->GetHeight();

		m_PauseMenuButtons[PauseMenuButton::ResumeButton]->MoveRelative(0, yOffset * (disableModManager ? -1 : 1));
		m_PauseMenuButtons[PauseMenuButton::SaveDiagnosticsButton]->MoveRelative(0, yOffset * (disableModManager ? -1 : 1));
		m_PauseMenuBox->MoveRelative(0, yOffset / 2 * -1);

		m_ModManagerButtonDisabled = disableModManager;
	}
}

void PauseMenuGUI::PlaceButtonRow(int button, int rowOffset) {
	int boxPosX = 0;
	int boxPosY = 0;
	int boxWidth = 0;
	int boxHeight = 0;
	m_PauseMenuBox->GetControlRect(&boxPosX, &boxPosY, &boxWidth, &boxHeight);

	int buttonPosX = 0;
	int buttonPosY = 0;
	int buttonWidth = 0;
	int buttonHeight = 0;
	m_PauseMenuButtons[button]->GetControlRect(&buttonPosX, &buttonPosY, &buttonWidth, &buttonHeight);
	m_PauseMenuButtons[button]->MoveRelative(0, boxPosY + rowOffset - buttonPosY);
}

void PauseMenuGUI::SetNetworkMatchMode(bool networkMatch) {
	if (networkMatch == m_NetworkMatchMode) {
		return;
	}
	m_NetworkMatchMode = networkMatch;
	ShowLeaveConfirm(false);
	ShowMatchOptions(false);
	SetActiveMenuScreen(PauseMenuScreen::MainScreen, false);

	// The match rows take the slots and the half row shift of the pause menu without its mod manager row.
	const auto matchRowOffset = [](int button) {
		switch (button) {
			case PauseMenuButton::LeaveMatchButton:
				return 0;
			case PauseMenuButton::MatchOptionsButton:
				return 20;
			case PauseMenuButton::PauseMatchButton:
				return 40;
			case PauseMenuButton::SettingsButton:
				return 60;
			case PauseMenuButton::SaveDiagnosticsButton:
				return 80;
			case PauseMenuButton::EndMatchButton:
				return 100;
			case PauseMenuButton::ResumeButton:
				return 120;
			default:
				return -1;
		}
	};

	int boxPosX = 0;
	int boxPosY = 0;
	int boxWidth = 0;
	int boxHeight = 0;
	m_PauseMenuBox->GetControlRect(&boxPosX, &boxPosY, &boxWidth, &boxHeight);
	m_PauseMenuBox->MoveRelative(0, m_PauseMenuBoxHomeY + (networkMatch ? -10 : 0) - boxPosY);

	for (int button = 0; button < PauseMenuButton::LeaveConfirmButton; ++button) {
		const int matchRow = matchRowOffset(button);
		const bool onMenu = networkMatch ? matchRow >= 0
		                                 : button != PauseMenuButton::PauseMatchButton && button != PauseMenuButton::LeaveMatchButton &&
		                                       button != PauseMenuButton::MatchOptionsButton && button != PauseMenuButton::EndMatchButton;
		PlaceButtonRow(button, networkMatch && onMenu ? matchRow : m_ButtonHomeY[button]);
		m_PauseMenuButtons[button]->SetEnabled(onMenu);
		m_PauseMenuButtons[button]->SetVisible(onMenu);
	}
	// The single-player pass owns the mod manager row again and re-derives it from the Activity.
	m_ModManagerButtonDisabled = false;
	UpdateMatchPauseRow(true);
}

void PauseMenuGUI::UpdateMatchPauseRow(bool force) {
	if (!m_NetworkMatchMode) {
		return;
	}
	const bool matchPaused = ScenarioRunner::IsLockstepPaused();
	// End Match is the host's: it finishes the round the way the match's own end would.
	m_PauseMenuButtons[PauseMenuButton::EndMatchButton]->SetEnabled(g_NetMatchService.IsHost() && g_NetMatchService.GetState() == NetMatchServiceState::Running);
	if (!force && matchPaused == m_MatchPausedShown) {
		return;
	}
	m_MatchPausedShown = matchPaused;
	GUIButton* pauseMatchButton = m_PauseMenuButtons[PauseMenuButton::PauseMatchButton];
	m_ButtonHoveredText[PauseMenuButton::PauseMatchButton] = matchPaused ? m_MatchResumeText[0] : m_MatchPauseText[0];
	m_ButtonUnhoveredText[PauseMenuButton::PauseMatchButton] = matchPaused ? m_MatchResumeText[1] : m_MatchPauseText[1];
	pauseMatchButton->SetText(m_HoveredButton == pauseMatchButton ? m_ButtonHoveredText[PauseMenuButton::PauseMatchButton] : m_ButtonUnhoveredText[PauseMenuButton::PauseMatchButton]);
}

std::string PauseMenuGUI::LeaveConsequenceText() const {
	if (g_NetMatchService.IsHost()) {
		return "Leave the match?\nThe match ends for everyone.";
	}
	// An announced leave holds nothing: the drop window is for peers that vanish, not for this one.
	return "Leave the match?\nThe others play on; your units fall to a teammate or to the AI. Your seat cannot be reclaimed.";
}

void PauseMenuGUI::ShowLeaveConfirm(bool show) {
	m_LeaveConfirmShown = show;
	if (show) {
		m_LeaveConfirmLabel->SetText(LeaveConsequenceText());
	}
	m_LeaveConfirmBox->SetVisible(show);
	m_LeaveConfirmBox->SetEnabled(show);
	m_PauseMenuBox->SetVisible(!show);
	m_PauseMenuBox->SetEnabled(!show);
}

void PauseMenuGUI::ShowMatchOptions(bool show) {
	m_MatchOptionsShown = show;
	if (show) {
		// Mid-match every peer reads the adopted config: the same panel the lobby's Details shows,
		// read-only because the open round's edits live in the lobby that follows.
		m_MatchOptionsLabel->SetText(NetHostOptionsSummary(g_NetMatchService.GetLobbyMatchConfig(),
		                                                 g_NetMatchService.GetLobbySnapshot()));
	}
	m_MatchOptionsBox->SetVisible(show);
	m_MatchOptionsBox->SetEnabled(show);
	m_PauseMenuBox->SetVisible(!show);
	m_PauseMenuBox->SetEnabled(!show);
}

void PauseMenuGUI::SetActiveMenuScreen(PauseMenuScreen screenToShow, bool playButtonPressSound) {
	if (screenToShow != m_ActiveMenuScreen) {
		m_ActiveMenuScreen = screenToShow;
		if (screenToShow == PauseMenuScreen::SaveOrLoadGameScreen) {
			m_SaveLoadMenu->Refresh();
		}
		if (playButtonPressSound) {
			g_GUISound.ButtonPressSound()->Play();
		}
	}
}

PauseMenuGUI::PauseMenuUpdateResult PauseMenuGUI::Update() {
	m_UpdateResult = PauseMenuUpdateResult::NoEvent;
	const bool savingDiagnostics = TelemetryBundle::IsBusy();
	GUIButton* diagnosticsButton = m_PauseMenuButtons[PauseMenuButton::SaveDiagnosticsButton];
	diagnosticsButton->SetEnabled(!savingDiagnostics);
	if (savingDiagnostics != m_DiagnosticsBusy) {
		m_DiagnosticsBusy = savingDiagnostics;
		m_ButtonHoveredText[PauseMenuButton::SaveDiagnosticsButton] = savingDiagnostics ? "SAVING" : m_DiagnosticsIdleText[0];
		m_ButtonUnhoveredText[PauseMenuButton::SaveDiagnosticsButton] = savingDiagnostics ? "saving" : m_DiagnosticsIdleText[1];
		diagnosticsButton->SetText(m_HoveredButton == diagnosticsButton ? m_ButtonHoveredText[PauseMenuButton::SaveDiagnosticsButton] : m_ButtonUnhoveredText[PauseMenuButton::SaveDiagnosticsButton]);
	}

	UpdateMatchPauseRow();

	if (g_ConsoleMan.IsEnabled() && !g_ConsoleMan.IsReadOnly()) {
		return m_UpdateResult;
	}

	bool backToMainScreen = false;

	switch (m_ActiveMenuScreen) {
		case PauseMenuScreen::MainScreen:
			backToMainScreen = HandleInputEvents();
			BlinkResumeButton();
			break;
		case PauseMenuScreen::SaveOrLoadGameScreen:
			backToMainScreen = m_SaveLoadMenu->HandleInputEvents(this);
			break;
		case PauseMenuScreen::SettingsScreen:
			backToMainScreen = m_SettingsMenu->HandleInputEvents();
			m_ActiveDialogBox = m_SettingsMenu->GetActiveDialogBox();
			break;
		case PauseMenuScreen::ModManagerScreen:
			backToMainScreen = m_ModManagerMenu->HandleInputEvents();
			break;
		default:
			break;
	}
	HandleBackNavigation(backToMainScreen);

	return m_UpdateResult;
}

void PauseMenuGUI::HandleBackNavigation(bool backButtonPressed) {
	// The pad's start button asks for the same step back the escape key takes.
	backButtonPressed = backButtonPressed || m_BackRequested;
	m_BackRequested = false;

	if (m_LeaveConfirmShown) {
		if (backButtonPressed || g_UInputMan.KeyPressed(SDLK_ESCAPE)) {
			ShowLeaveConfirm(false);
			g_GUISound.BackButtonPressSound()->Play();
		}
		return;
	}
	if (m_MatchOptionsShown) {
		if (backButtonPressed || g_UInputMan.KeyPressed(SDLK_ESCAPE)) {
			ShowMatchOptions(false);
			g_GUISound.BackButtonPressSound()->Play();
		}
		return;
	}
	if (!m_ActiveDialogBox && (backButtonPressed || g_UInputMan.KeyPressed(SDLK_ESCAPE))) {
		if (m_ActiveMenuScreen != PauseMenuScreen::MainScreen) {
			if (m_ActiveMenuScreen == PauseMenuScreen::SettingsScreen || m_ActiveMenuScreen == PauseMenuScreen::ModManagerScreen) {
				if (m_ActiveMenuScreen == PauseMenuScreen::SettingsScreen) {
					m_SettingsMenu->RefreshActiveSettingsMenuScreen();
				}
				g_SettingsMan.UpdateSettingsFile();
			}
			m_ActiveDialogBox = nullptr;
			SetActiveMenuScreen(PauseMenuScreen::MainScreen, false);
		} else {
			m_UpdateResult = PauseMenuUpdateResult::ActivityResumed;
		}
		g_GUISound.BackButtonPressSound()->Play();
	} else if (m_ActiveMenuScreen == PauseMenuScreen::SettingsScreen && m_ActiveDialogBox && g_UInputMan.KeyPressed(SDLK_ESCAPE)) {
		m_SettingsMenu->CloseActiveDialogBox();
	}
}

bool PauseMenuGUI::HandleInputEvents() {
	if (m_ActiveMenuScreen == PauseMenuScreen::MainScreen) {
		int mousePosX;
		int mousePosY;
		m_GUIControlManager->GetManager()->GetInputController()->GetMousePosition(&mousePosX, &mousePosY);
		UpdateHoveredButton(dynamic_cast<GUIButton*>(m_GUIControlManager->GetControlUnderPoint(mousePosX, mousePosY, m_LeaveConfirmShown ? m_LeaveConfirmBox : (m_MatchOptionsShown ? m_MatchOptionsBox : m_PauseMenuBox), 1)));
	}
	m_GUIControlManager->Update();
	if (!m_PendingAutomationCommand.empty()) {
		GUIControl* control = m_GUIControlManager->GetControl(m_PendingAutomationCommand);
		const bool enabled = AutomationControlEnabled(m_PendingAutomationCommand);
		m_PendingAutomationCommand.clear();
		if (control && enabled) control->AddEvent(GUIEvent::Command, 0, 0);
	}

	GUIEvent guiEvent;
	while (m_GUIControlManager->GetEvent(&guiEvent)) {
		if (guiEvent.GetType() == GUIEvent::Command) {
			if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::ResumeButton]) {
				return true;
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::SaveOrLoadGameButton]) {
				SetActiveMenuScreen(PauseMenuScreen::SaveOrLoadGameScreen);
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::SettingsButton]) {
				SetActiveMenuScreen(PauseMenuScreen::SettingsScreen);
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::ModManagerButton]) {
				SetActiveMenuScreen(PauseMenuScreen::ModManagerScreen);
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::SaveDiagnosticsButton]) {
				if (TelemetryBundle::RequestCapture()) g_ConsoleMan.PrintString("SYSTEM: Saving diagnostics...");
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::PauseMatchButton]) {
				// The one pause every peer shares, on this player's own team authority, like the P shortcut.
				ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{g_NetMatchService.GetLocalTeam(), !ScenarioRunner::IsLockstepPaused()}});
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::LeaveMatchButton]) {
				ShowLeaveConfirm(true);
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::MatchOptionsButton]) {
				ShowMatchOptions(true);
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::MatchOptionsCloseButton]) {
				ShowMatchOptions(false);
				g_GUISound.BackButtonPressSound()->Play();
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::EndMatchButton]) {
				// H33: the host's End Match is the round's own completion, so every peer takes the
				// same rematch path a played-out match takes. Leave stays the session's way out.
				m_UpdateResult = PauseMenuUpdateResult::MatchEnded;
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::LeaveConfirmButton]) {
				m_UpdateResult = PauseMenuUpdateResult::MatchLeft;
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::LeaveCancelButton]) {
				g_GUISound.BackButtonPressSound()->Play();
				ShowLeaveConfirm(false);
			} else if (guiEvent.GetControl() == m_PauseMenuButtons[PauseMenuButton::BackToMainButton]) {
				g_GUISound.BackButtonPressSound()->Play();
				m_UpdateResult = PauseMenuUpdateResult::BackToMain;
			}
		}
		if (guiEvent.GetType() == GUIEvent::Notification && (guiEvent.GetMsg() == GUIButton::Focused && dynamic_cast<GUIButton*>(guiEvent.GetControl()))) {
			g_GUISound.SelectionChangeSound()->Play();
		}
	}
	return false;
}

bool PauseMenuGUI::AutomationPostCommand(const std::string& controlName) {
	if (m_ActiveMenuScreen == PauseMenuScreen::SettingsScreen) return m_SettingsMenu->AutomationPostCommand(controlName);
	if (!AutomationControlEnabled(controlName)) return false;
	m_PendingAutomationCommand = controlName;
	return true;
}

bool PauseMenuGUI::AutomationControlExists(const std::string& controlName) const {
	return AutomationManager() && AutomationManager()->GetControl(controlName) != nullptr;
}

bool PauseMenuGUI::AutomationControlEnabled(const std::string& controlName) const {
	if (m_ActiveMenuScreen == PauseMenuScreen::SettingsScreen) return MenuAutomation::Enabled(AutomationManager()->GetControl(controlName));
	GUIControl* control = m_GUIControlManager->GetControl(controlName);
	if (!control || m_ActiveMenuScreen != PauseMenuScreen::MainScreen || m_ActiveDialogBox) return false;
	for (GUIControl* node = control; node; node = node->GetParent()) {
		if (!node->GetEnabled() || !node->GetVisible()) return false;
	}
	return true;
}

bool PauseMenuGUI::AutomationLabelText(const std::string& controlName, std::string& text) const {
	return AutomationManager() && MenuAutomation::Text(AutomationManager()->GetControl(controlName), text);
}

GUIControlManager* PauseMenuGUI::AutomationManager() const {
	if (m_ActiveMenuScreen == PauseMenuScreen::SettingsScreen) return m_SettingsMenu->AutomationManager();
	return m_ActiveMenuScreen == PauseMenuScreen::MainScreen ? m_GUIControlManager.get() : nullptr;
}

std::string PauseMenuGUI::AutomationActiveScreenName() const {
	if (m_ActiveMenuScreen == PauseMenuScreen::MainScreen) {
		if (m_MatchOptionsShown) {
			return "PauseMatchOptions";
		}
		return m_LeaveConfirmShown ? "PauseLeaveConfirm" : "Pause";
	}
	return m_ActiveMenuScreen == PauseMenuScreen::SettingsScreen ? "PauseSettings" : "PauseOther";
}

void PauseMenuGUI::UpdateHoveredButton(const GUIButton* hoveredButton) {
	int hoveredButtonIndex = -1;
	if (hoveredButton) {
		hoveredButtonIndex = std::distance(m_PauseMenuButtons.begin(), std::find(m_PauseMenuButtons.begin(), m_PauseMenuButtons.end(), hoveredButton));
		if (hoveredButton != m_HoveredButton) {
			m_PauseMenuButtons[hoveredButtonIndex]->SetText(m_ButtonHoveredText[hoveredButtonIndex]);
		}
		m_HoveredButton = m_PauseMenuButtons[hoveredButtonIndex];
	}
	if (!hoveredButton || hoveredButtonIndex != m_PrevHoveredButtonIndex) {
		m_PauseMenuButtons[m_PrevHoveredButtonIndex]->SetText(m_ButtonUnhoveredText[m_PrevHoveredButtonIndex]);
	}

	if (hoveredButtonIndex >= 0) {
		m_PrevHoveredButtonIndex = hoveredButtonIndex;
	} else {
		m_HoveredButton = nullptr;
	}
}

void PauseMenuGUI::BlinkResumeButton() {
	if (m_HoveredButton && m_HoveredButton == m_PauseMenuButtons[PauseMenuButton::ResumeButton]) {
		m_PauseMenuButtons[PauseMenuButton::ResumeButton]->SetText(m_ResumeButtonBlinkTimer.AlternateReal(500) ? m_ButtonHoveredText[PauseMenuButton::ResumeButton] : "]" + m_ButtonHoveredText[PauseMenuButton::ResumeButton] + "[");
	} else {
		m_PauseMenuButtons[PauseMenuButton::ResumeButton]->SetText(m_ResumeButtonBlinkTimer.AlternateReal(500) ? m_ButtonUnhoveredText[PauseMenuButton::ResumeButton] : ">" + m_ButtonUnhoveredText[PauseMenuButton::ResumeButton] + "<");
	}
}

void PauseMenuGUI::Draw(bool drawPostProcessBuffer) {
	if (drawPostProcessBuffer) {
		g_WindowMan.DrawPostProcessBuffer();
	}
	blit(m_BackdropBitmap, g_FrameMan.GetBackBuffer32(), 0, 0, 0, 0, m_BackdropBitmap->w, m_BackdropBitmap->h);

	switch (m_ActiveMenuScreen) {
		case PauseMenuScreen::SaveOrLoadGameScreen:
			m_SaveLoadMenu->Draw();
			break;
		case PauseMenuScreen::SettingsScreen:
			m_SettingsMenu->Draw();
			break;
		case PauseMenuScreen::ModManagerScreen:
			m_ModManagerMenu->Draw();
			break;
		default:
			m_GUIControlManager->Draw();
			break;
	}
	if (m_ActiveDialogBox) {
		SetTrueAlphaBlender();
		// Whatever this box may be at this point it's already been drawn by the owning GUIControlManager, but we need to draw it again on top of the overlay so it's not affected by it.
		m_ActiveDialogBox->Draw(m_GUIControlManager->GetScreen());
	}
	m_GUIControlManager->DrawMouse();
}
