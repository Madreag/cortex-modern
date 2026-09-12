#include "MainMenuGUI.h"

#include "WindowMan.h"
#include "FrameMan.h"
#include "ActivityMan.h"
#include "UInputMan.h"
#include "SettingsMan.h"
#include "ConsoleMan.h"
#include "NetMatchService.h"
#include "NetConnectionQuality.h"
#include "PresetMan.h"
#include "SceneMan.h"
#include "ScenarioRunner.h"

#include "Activity.h"
#include "Entity.h"
#include "GameVersion.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUILabel.h"
#include "GUIListBox.h"
#include "GUITextBox.h"

#include "Resources/Credits.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace RTE;

void MainMenuGUI::Clear() {
	m_RootBoxMaxWidth = 0;

	m_MainMenuScreenGUIControlManager = nullptr;
	m_SubMenuScreenGUIControlManager = nullptr;
	m_ActiveGUIControlManager = nullptr;
	m_ActiveDialogBox = nullptr;

	m_ActiveMenuScreen = MenuScreen::ScreenCount;
	m_UpdateResult = MainMenuUpdateResult::NoEvent;
	m_MenuScreenChange = false;
	m_MetaGameNoticeShown = false;

	m_ResumeButtonBlinkTimer.Reset();
	m_CreditsScrollTimer.Reset();

	m_SaveLoadMenu = nullptr;
	m_SettingsMenu = nullptr;
	m_ModManagerMenu = nullptr;

	m_VersionLabel = nullptr;
	m_CreditsTextLabel = nullptr;
	m_MultiplayerStatusLabel = nullptr;
	m_MultiplayerErrorLabel = nullptr;
	m_MultiplayerLandingStatusLabel = nullptr;
	m_MultiplayerLobbyMatchLabel = nullptr;
	m_MultiplayerNameTextBox = nullptr;
	m_MultiplayerHostPortTextBox = nullptr;
	m_MultiplayerHostPlayersTextBox = nullptr;
	m_MultiplayerHostInputDelayTextBox = nullptr;
	m_MultiplayerHostModeButton = nullptr;
	m_MultiplayerHostMode = NetMatchMode::PvPSkirmish;
	m_MultiplayerJoinAddressTextBox = nullptr;
	m_MultiplayerJoinPortTextBox = nullptr;
	m_MultiplayerLanGamesList = nullptr;
	m_LanHosts.clear();
	m_LanBrowserNowMs = 0;
	m_MultiplayerLandingPanel = nullptr;
	m_MultiplayerHostPanel = nullptr;
	m_MultiplayerJoinPanel = nullptr;
	m_MultiplayerLobbyPanel = nullptr;
	m_MultiplayerModerationPanel = nullptr;
	m_MultiplayerModerationSummaryLabel = nullptr;
	m_MultiplayerModerationStatusLabel = nullptr;
	m_ModerationSeatLabels.fill(nullptr);
	m_ModerationApplicantButtons.fill(nullptr);
	m_ModerationWaitButtons.fill(nullptr);
	m_ModerationSubstituteButtons.fill(nullptr);
	m_ModerationCancelButtons.fill(nullptr);
	m_PressedModeration.clear();
	m_MultiplayerLobbyPlayerLabels.fill(nullptr);
	m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
	m_CreditsScrollPanel = nullptr;
	m_MainMenuScreens.fill(nullptr);
	m_MainMenuButtons.fill(nullptr);

	m_MainScreenButtonHoveredText.fill(std::string());
	m_MainScreenButtonUnhoveredText.fill(std::string());
	m_MainScreenHoveredButton = nullptr;
	m_MainScreenPrevHoveredButtonIndex = 0;
}

void MainMenuGUI::Create(AllegroScreen* guiScreen, GUIInputWrapper* guiInput) {
	m_MainMenuScreenGUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_MainMenuScreenGUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuScreenSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuScreenSkin.ini");
	m_MainMenuScreenGUIControlManager->Load("Base.rte/GUIs/MainMenuGUI.ini");

	m_SubMenuScreenGUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_SubMenuScreenGUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuSubMenuSkin.ini");
	m_SubMenuScreenGUIControlManager->Load("Base.rte/GUIs/MainMenuSubMenuGUI.ini");

	m_RootBoxMaxWidth = g_WindowMan.FullyCoversAllDisplays() ? g_WindowMan.GetPrimaryWindowDisplayWidth() / g_WindowMan.GetResMultiplier() : g_WindowMan.GetResX();

	GUICollectionBox* mainScreenRootBox = dynamic_cast<GUICollectionBox*>(m_MainMenuScreenGUIControlManager->GetControl("root"));
	mainScreenRootBox->Resize(m_RootBoxMaxWidth, mainScreenRootBox->GetHeight());

	GUICollectionBox* subMenuScreenRootBox = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("root"));
	subMenuScreenRootBox->Resize(m_RootBoxMaxWidth, g_WindowMan.GetResY());

	m_MainMenuButtons[MenuButton::BackToMainButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonBackToMain"));
	m_MainMenuButtons[MenuButton::BackToMainButton]->CenterInParent(true, false);

	CreateMainScreen();
	CreateMetaGameNoticeScreen();
	CreateMultiplayerScreen();
	CreateEditorsScreen();
	CreateCreditsScreen();
	CreateQuitScreen();

	m_SaveLoadMenu = std::make_unique<SaveLoadMenuGUI>(guiScreen, guiInput);
	m_SettingsMenu = std::make_unique<SettingsGUI>(guiScreen, guiInput);
	m_ModManagerMenu = std::make_unique<ModManagerGUI>(guiScreen, guiInput);

	// Set the active screen to the settings screen otherwise we're at the main screen after reinitializing.
	SetActiveMenuScreen(g_WindowMan.ResolutionChanged() ? MenuScreen::SettingsScreen : MenuScreen::MainScreen, false);
}

void MainMenuGUI::CreateMainScreen() {
	m_MainMenuScreens[MenuScreen::MainScreen] = dynamic_cast<GUICollectionBox*>(m_MainMenuScreenGUIControlManager->GetControl("MainScreen"));
	m_MainMenuScreens[MenuScreen::MainScreen]->CenterInParent(true, false);

	m_MainMenuButtons[MenuButton::MetaGameButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonMainToMetaGame"));
	m_MainMenuButtons[MenuButton::ScenarioButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonMainToSkirmish"));
	m_MainMenuButtons[MenuButton::MultiplayerButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonMainToMultiplayer"));
	m_MainMenuButtons[MenuButton::SaveOrLoadGameButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonSaveOrLoadGame"));
	m_MainMenuButtons[MenuButton::SettingsButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonMainToOptions"));
	m_MainMenuButtons[MenuButton::ModManagerButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonMainToModManager"));
	m_MainMenuButtons[MenuButton::EditorsButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonMainToEditor"));
	m_MainMenuButtons[MenuButton::CreditsButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonMainToCreds"));
	m_MainMenuButtons[MenuButton::QuitButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonQuit"));
	m_MainMenuButtons[MenuButton::ResumeButton] = dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControl("ButtonResume"));

	for (int mainScreenButton = MenuButton::MetaGameButton; mainScreenButton <= MenuButton::ResumeButton; ++mainScreenButton) {
		m_MainMenuButtons.at(mainScreenButton)->CenterInParent(true, false);
		std::string buttonText = m_MainMenuButtons.at(mainScreenButton)->GetText();
		std::transform(buttonText.begin(), buttonText.end(), buttonText.begin(), ::toupper);
		m_MainScreenButtonHoveredText.at(mainScreenButton) = buttonText;
		std::transform(buttonText.begin(), buttonText.end(), buttonText.begin(), ::tolower);
		m_MainScreenButtonUnhoveredText.at(mainScreenButton) = buttonText;

		m_MainMenuButtons.at(mainScreenButton)->SetText(m_MainScreenButtonUnhoveredText.at(mainScreenButton));
	}

	m_VersionLabel = dynamic_cast<GUILabel*>(m_MainMenuScreenGUIControlManager->GetControl("VersionLabel"));
	m_VersionLabel->SetText("Community Project\nv" + c_GameVersion.str());
	m_VersionLabel->SetPositionAbs(10, g_WindowMan.GetResY() - m_VersionLabel->GetTextHeight() - 5);
}

void MainMenuGUI::CreateMultiplayerScreen() {
	m_MainMenuScreens[MenuScreen::MultiplayerScreen] = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MultiplayerScreen"));
	m_MainMenuScreens[MenuScreen::MultiplayerScreen]->CenterInParent(true, false);

	m_MultiplayerLandingPanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MultiplayerLandingPanel"));
	m_MultiplayerHostPanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MultiplayerHostPanel"));
	m_MultiplayerJoinPanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MultiplayerJoinPanel"));
	m_MultiplayerLobbyPanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MultiplayerLobbyPanel"));
	m_MultiplayerModerationPanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MultiplayerModerationPanel"));

	m_MainMenuButtons[MenuButton::MultiplayerHostGameButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerHostGame"));
	m_MainMenuButtons[MenuButton::MultiplayerJoinGameButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerJoinGame"));
	m_MainMenuButtons[MenuButton::MultiplayerCreateButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerCreate"));
	m_MainMenuButtons[MenuButton::MultiplayerConnectButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerConnect"));
	m_MainMenuButtons[MenuButton::MultiplayerReadyButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerReady"));
	m_MainMenuButtons[MenuButton::MultiplayerStartButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerStart"));
	m_MainMenuButtons[MenuButton::MultiplayerLeaveButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerLeave"));
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerReconnect"));
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerCancelReconnect"));
	m_MainMenuButtons[MenuButton::MultiplayerHostBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostBack"));
	m_MainMenuButtons[MenuButton::MultiplayerJoinBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonJoinBack"));
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerModerate"));
	m_MainMenuButtons[MenuButton::MultiplayerModerationBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonModerationBack"));

	m_MultiplayerNameTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextMultiplayerName"));
	m_MultiplayerHostPortTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextHostPort"));
	m_MultiplayerHostPlayersTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextHostPlayers"));
	m_MultiplayerHostInputDelayTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextHostInputDelay"));
	m_MultiplayerHostModeButton = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostMode"));
	m_MultiplayerJoinAddressTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextJoinAddress"));
	m_MultiplayerJoinPortTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextJoinPort"));
	m_MultiplayerLanGamesList = dynamic_cast<GUIListBox*>(m_SubMenuScreenGUIControlManager->GetControl("ListLanGames"));

	m_MultiplayerStatusLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelMultiplayerStatus"));
	m_MultiplayerErrorLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelMultiplayerError"));
	m_MultiplayerLandingStatusLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelMultiplayerLandingStatus"));
	m_MultiplayerLobbyMatchLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyMatch"));
	m_MultiplayerLobbyPlayerLabels[0] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer0"));
	m_MultiplayerLobbyPlayerLabels[1] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer1"));
	m_MultiplayerLobbyPlayerLabels[2] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer2"));
	m_MultiplayerLobbyPlayerLabels[3] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer3"));

	m_MultiplayerModerationSummaryLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelModerationSummary"));
	m_MultiplayerModerationStatusLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelModerationStatus"));
	for (size_t row = 0; row < m_ModerationSeatLabels.size(); ++row) {
		const std::string suffix = std::to_string(row);
		m_ModerationSeatLabels[row] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelModerationSeat" + suffix));
		m_ModerationApplicantButtons[row] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonModerationApplicant" + suffix));
		m_ModerationWaitButtons[row] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonModerationWait" + suffix));
		m_ModerationSubstituteButtons[row] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonModerationSubstitute" + suffix));
		m_ModerationCancelButtons[row] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonModerationCancel" + suffix));
	}

	m_MultiplayerNameTextBox->SetText("Player");
	m_MultiplayerNameTextBox->SetMaxTextLength(24);
	m_MultiplayerJoinAddressTextBox->SetText("127.0.0.1");
	m_MultiplayerJoinAddressTextBox->SetMaxTextLength(64);
	m_MultiplayerHostPortTextBox->SetText("41010");
	m_MultiplayerHostPortTextBox->SetNumericOnly(true);
	m_MultiplayerHostPortTextBox->SetMaxNumericValue(65535);
	m_MultiplayerHostPortTextBox->SetMaxTextLength(5);
	m_MultiplayerHostPlayersTextBox->SetText("2");
	m_MultiplayerHostPlayersTextBox->SetNumericOnly(true);
	m_MultiplayerHostPlayersTextBox->SetMaxNumericValue(NetMatchConfigUtil::c_MaxPeerCount);
	m_MultiplayerHostPlayersTextBox->SetMaxTextLength(1);
	m_MultiplayerHostInputDelayTextBox->SetText(std::to_string(std::clamp(g_SettingsMan.GetNetworkInputDelayFrames(), 0, static_cast<int>(NetMatchConfigUtil::c_MaxInputDelayFrames))));
	m_MultiplayerHostInputDelayTextBox->SetNumericOnly(true);
	m_MultiplayerHostInputDelayTextBox->SetMaxNumericValue(NetMatchConfigUtil::c_MaxInputDelayFrames);
	m_MultiplayerHostInputDelayTextBox->SetMaxTextLength(2);
	m_MultiplayerJoinPortTextBox->SetText("41010");
	m_MultiplayerJoinPortTextBox->SetNumericOnly(true);
	m_MultiplayerJoinPortTextBox->SetMaxNumericValue(65535);
	m_MultiplayerJoinPortTextBox->SetMaxTextLength(5);
}

void MainMenuGUI::CreateMetaGameNoticeScreen() {
	m_MainMenuScreens[MenuScreen::MetaGameNoticeScreen] = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MetaScreen"));
	m_MainMenuScreens[MenuScreen::MetaGameNoticeScreen]->CenterInParent(true, false);

	m_MainMenuButtons[MenuButton::PlayTutorialButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonTutorial"));
	m_MainMenuButtons[MenuButton::MetaGameContinueButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonContinue"));
}

void MainMenuGUI::CreateEditorsScreen() {
	m_MainMenuScreens[MenuScreen::EditorScreen] = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("EditorScreen"));
	m_MainMenuScreens[MenuScreen::EditorScreen]->CenterInParent(true, false);

	m_MainMenuButtons[MenuButton::SceneEditorButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonSceneEditor"));
	m_MainMenuButtons[MenuButton::AreaEditorButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonAreaEditor"));
	m_MainMenuButtons[MenuButton::AssemblyEditorButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonAssemblyEditor"));
	m_MainMenuButtons[MenuButton::GibEditorButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonGibPlacement"));
	m_MainMenuButtons[MenuButton::ActorEditorButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonActorEditor"));
}

void MainMenuGUI::CreateCreditsScreen() {
	m_MainMenuScreens[MenuScreen::CreditsScreen] = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("CreditsScreen"));
	m_MainMenuScreens[MenuScreen::CreditsScreen]->Resize(m_MainMenuScreens[MenuScreen::CreditsScreen]->GetWidth(), g_WindowMan.GetResY());
	m_MainMenuScreens[MenuScreen::CreditsScreen]->CenterInParent(true, false);

	m_CreditsScrollPanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("CreditsPanel"));
	m_CreditsScrollPanel->Resize(m_CreditsScrollPanel->GetWidth(), g_WindowMan.GetResY() - m_CreditsScrollPanel->GetYPos() - 50);

	m_CreditsTextLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("CreditsLabel"));

	// TODO: Get Unicode going!
	// Hack here to change the special characters over 128 in the ANSI ASCII table to match our font files
	for (char& stringChar: s_CreditsText) {
		if (stringChar == -60) {
			stringChar = static_cast<unsigned char>(142); //'Ä'
		} else if (stringChar == -42) {
			stringChar = static_cast<unsigned char>(153); //'Ö'
		} else if (stringChar == -87) {
			stringChar = static_cast<unsigned char>(221); //'©'
		}
	}
	m_CreditsTextLabel->SetText(s_CreditsText);
	m_CreditsTextLabel->ResizeHeightToFit();
}

void MainMenuGUI::CreateQuitScreen() {
	m_MainMenuScreens[MenuScreen::QuitScreen] = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("QuitConfirmBox"));
	m_MainMenuScreens[MenuScreen::QuitScreen]->CenterInParent(true, false);

	m_MainMenuButtons[MenuButton::QuitConfirmButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("QuitConfirmButton"));
	m_MainMenuButtons[MenuButton::QuitCancelButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("QuitCancelButton"));
}

void MainMenuGUI::HideAllScreens() {
	for (GUICollectionBox* menuScreen: m_MainMenuScreens) {
		if (menuScreen) {
			menuScreen->SetVisible(false);
		}
	}
	m_MenuScreenChange = true;
}

void MainMenuGUI::SetActiveMenuScreen(MenuScreen screenToShow, bool playButtonPressSound) {
	if (screenToShow != m_ActiveMenuScreen) {
		HideAllScreens();
		m_ActiveMenuScreen = screenToShow;
		m_ActiveGUIControlManager = (m_ActiveMenuScreen == MenuScreen::MainScreen) ? m_MainMenuScreenGUIControlManager.get() : m_SubMenuScreenGUIControlManager.get();
		m_MenuScreenChange = true;

		if (screenToShow == MenuScreen::SaveOrLoadGameScreen) {
			m_SaveLoadMenu->Refresh();
		}

		if (playButtonPressSound) {
			g_GUISound.ButtonPressSound()->Play();
		}
	}
}

void MainMenuGUI::ShowMainScreen() {
	m_VersionLabel->SetVisible(true);

	m_MainMenuScreens[MenuScreen::MainScreen]->Resize(300, 220);
	m_MainMenuScreens[MenuScreen::MainScreen]->SetVisible(true);

	m_MainMenuButtons[MenuButton::BackToMainButton]->SetVisible(false);
	m_MainMenuButtons[MenuButton::ResumeButton]->SetVisible(false);

	m_MenuScreenChange = false;
}

void MainMenuGUI::ShowMultiplayerScreen() {
	m_MainMenuScreens[MenuScreen::MultiplayerScreen]->SetVisible(true);
	m_MainMenuScreens[MenuScreen::MultiplayerScreen]->GUIPanel::AddChild(m_MainMenuButtons[MenuButton::BackToMainButton]);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetVisible(true);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetPositionAbs((m_RootBoxMaxWidth - m_MainMenuButtons[MenuButton::BackToMainButton]->GetWidth()) / 2, m_MainMenuScreens[MenuScreen::MultiplayerScreen]->GetYPos() + 250);
	const NetMatchServiceState netMatchState = g_NetMatchService.GetState();
	if (netMatchState == NetMatchServiceState::Idle) {
		// A relaunch after a crash lands here with the match still running elsewhere; §11 offers it back.
		g_NetMatchService.ScanStoredTicket();
	}
	m_MultiplayerSubScreen = (netMatchState == NetMatchServiceState::Idle || netMatchState == NetMatchServiceState::Completed) ? MultiplayerSubScreen::Landing : MultiplayerSubScreen::Lobby;
	RefreshMultiplayerScreenControls(g_NetMatchService.GetLobbySnapshot());
	m_MenuScreenChange = false;
}

void MainMenuGUI::ShowMetaGameNoticeScreen() {
	m_MainMenuScreens[MenuScreen::MetaGameNoticeScreen]->SetVisible(true);
	m_MainMenuScreens[MenuScreen::MetaGameNoticeScreen]->GUIPanel::AddChild(m_MainMenuButtons[MenuButton::BackToMainButton]);

	m_MainMenuButtons[MenuButton::BackToMainButton]->SetVisible(true);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetPositionAbs((m_RootBoxMaxWidth - m_MainMenuButtons[MenuButton::BackToMainButton]->GetWidth()) / 2, m_MainMenuButtons[MenuButton::MetaGameContinueButton]->GetYPos() + 25);

	GUILabel* metaNoticeLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("MetaLabel"));

	std::string metaNotice = {
	    "- A T T E N T I O N -\n\n"
	    "Please note that Conquest mode is very INCOMPLETE and flawed, and the Cortex Command Community Project team will be remaking it from the ground-up in future. "
	    "For now though, it's fully playable, and you can absolutely enjoy playing it with the A.I. and/or up to three friends. Alternatively, check out Void Wanderers on cccp.mod.io!\n\n"
	    "Also, if you have not yet played Cortex Command, we recommend you first try the tutorial:"};
	metaNoticeLabel->SetText(metaNotice);
	metaNoticeLabel->SetVisible(true);

	// Flag that this notice has now been shown once, so no need to keep showing it
	m_MetaGameNoticeShown = true;

	m_MenuScreenChange = false;
}

void MainMenuGUI::ShowEditorsScreen() {
	m_MainMenuScreens[MenuScreen::EditorScreen]->SetVisible(true);
	m_MainMenuScreens[MenuScreen::EditorScreen]->GUIPanel::AddChild(m_MainMenuButtons[MenuButton::BackToMainButton]);

	m_MainMenuButtons[MenuButton::BackToMainButton]->SetVisible(true);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetPositionRel(4, 145);

	m_MenuScreenChange = false;
}

void MainMenuGUI::ShowCreditsScreen() {
	m_MainMenuScreens[MenuScreen::CreditsScreen]->SetVisible(true);
	m_MainMenuScreens[MenuScreen::CreditsScreen]->GUIPanel::AddChild(m_MainMenuButtons[MenuButton::BackToMainButton]);

	m_MainMenuButtons[MenuButton::BackToMainButton]->SetVisible(true);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetPositionAbs((m_RootBoxMaxWidth - m_MainMenuButtons[MenuButton::BackToMainButton]->GetWidth()) / 2, g_WindowMan.GetResY() - 35);

	m_VersionLabel->SetVisible(false);

	m_CreditsTextLabel->SetPositionRel(0, g_WindowMan.GetResY() - m_CreditsScrollPanel->GetYPos() - 50);
	m_CreditsScrollTimer.Reset();

	m_MenuScreenChange = false;
}

void MainMenuGUI::ShowQuitScreenOrQuit() {
	if (m_ActiveMenuScreen != MenuScreen::QuitScreen && g_ActivityMan.GetActivity() && (g_ActivityMan.GetActivity()->GetActivityState() == Activity::Running || g_ActivityMan.GetActivity()->GetActivityState() == Activity::Editing)) {
		SetActiveMenuScreen(MenuScreen::QuitScreen);
		m_MainMenuScreens[MenuScreen::QuitScreen]->SetVisible(true);
		m_MenuScreenChange = false;
	} else {
		m_UpdateResult = MainMenuUpdateResult::Quit;
	}
}

void MainMenuGUI::ShowAndBlinkResumeButton() {
	if (!m_MainMenuButtons[MenuButton::ResumeButton]->GetVisible()) {
		m_ResumeButtonBlinkTimer.Reset();
		if (g_ActivityMan.GetActivity() && (g_ActivityMan.GetActivity()->GetActivityState() == Activity::Running || g_ActivityMan.GetActivity()->GetActivityState() == Activity::Editing)) {
			m_MainMenuScreens[MenuScreen::MainScreen]->Resize(300, 220);
			m_MainMenuButtons[MenuButton::ResumeButton]->SetVisible(true);
		}
	} else {
		if (m_MainScreenHoveredButton && m_MainScreenHoveredButton == m_MainMenuButtons[MenuButton::ResumeButton]) {
			m_MainMenuButtons[MenuButton::ResumeButton]->SetText(m_ResumeButtonBlinkTimer.AlternateReal(500) ? m_MainScreenButtonHoveredText[MenuButton::ResumeButton] : "]" + m_MainScreenButtonHoveredText[MenuButton::ResumeButton] + "[");
		} else {
			m_MainMenuButtons[MenuButton::ResumeButton]->SetText(m_ResumeButtonBlinkTimer.AlternateReal(500) ? m_MainScreenButtonUnhoveredText[MenuButton::ResumeButton] : ">" + m_MainScreenButtonUnhoveredText[MenuButton::ResumeButton] + "<");
		}
	}
}

bool MainMenuGUI::RollCredits() {
	int scrollDuration = m_CreditsTextLabel->GetHeight() * 50;
	float scrollDist = static_cast<float>(m_CreditsScrollPanel->GetHeight() + m_CreditsTextLabel->GetHeight());
	float scrollProgress = static_cast<float>(m_CreditsScrollTimer.GetElapsedRealTimeMS()) / static_cast<float>(scrollDuration);
	m_CreditsTextLabel->SetPositionRel(0, m_CreditsScrollPanel->GetHeight() - static_cast<int>(scrollDist * scrollProgress));

	if (m_CreditsScrollTimer.IsPastRealMS(scrollDuration + 1000)) {
		return true;
	}
	return false;
}

MainMenuGUI::MainMenuUpdateResult MainMenuGUI::Update() {
	if (g_ConsoleMan.IsEnabled() && !g_ConsoleMan.IsReadOnly()) {
		return MainMenuUpdateResult::NoEvent;
	}

	bool backToMainMenu = false;

	switch (m_ActiveMenuScreen) {
		case MenuScreen::MainScreen:
			if (m_MenuScreenChange) {
				ShowMainScreen();
			}
			backToMainMenu = HandleInputEvents();
			ShowAndBlinkResumeButton();
			break;
		case MenuScreen::MetaGameNoticeScreen:
			if (m_MenuScreenChange) {
				ShowMetaGameNoticeScreen();
			}
			backToMainMenu = HandleInputEvents();
			break;
		case MenuScreen::MultiplayerScreen:
			if (m_MenuScreenChange) {
				ShowMultiplayerScreen();
			}
			backToMainMenu = HandleInputEvents();
			UpdateMultiplayerScreen();
			break;
		case MenuScreen::SaveOrLoadGameScreen:
			backToMainMenu = m_SaveLoadMenu->HandleInputEvents();
			break;
		case MenuScreen::SettingsScreen:
			backToMainMenu = m_SettingsMenu->HandleInputEvents();
			m_ActiveDialogBox = m_SettingsMenu->GetActiveDialogBox();
			break;
		case MenuScreen::ModManagerScreen:
			backToMainMenu = m_ModManagerMenu->HandleInputEvents();
			break;
		case MenuScreen::EditorScreen:
			if (m_MenuScreenChange) {
				ShowEditorsScreen();
			}
			backToMainMenu = HandleInputEvents();
			break;
		case MenuScreen::CreditsScreen:
			if (m_MenuScreenChange) {
				ShowCreditsScreen();
			}
			backToMainMenu = RollCredits() ? true : HandleInputEvents();
			break;
		case MenuScreen::QuitScreen:
			backToMainMenu = HandleInputEvents();
			m_ActiveDialogBox = m_MainMenuScreens[MenuScreen::QuitScreen]->GetVisible() ? m_MainMenuScreens[MenuScreen::QuitScreen] : nullptr;
			break;
		default:
			break;
	}
	HandleBackNavigation(backToMainMenu);

	if (m_UpdateResult == MainMenuUpdateResult::ActivityStarted || m_UpdateResult == MainMenuUpdateResult::ActivityResumed) {
		m_MainMenuButtons[MenuButton::ResumeButton]->SetVisible(false);
	}
	const MainMenuUpdateResult result = m_UpdateResult;
	m_UpdateResult = MainMenuUpdateResult::NoEvent;
	return result;
}

void MainMenuGUI::HandleBackNavigation(bool backButtonPressed) {
	if ((!m_ActiveDialogBox || m_ActiveDialogBox == m_MainMenuScreens[MenuScreen::QuitScreen]) && (backButtonPressed || g_UInputMan.KeyPressed(SDLK_ESCAPE))) {
		if (m_ActiveMenuScreen != MenuScreen::MainScreen) {
			if (m_ActiveMenuScreen == MenuScreen::SettingsScreen || m_ActiveMenuScreen == MenuScreen::ModManagerScreen) {
				if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) {
					m_SettingsMenu->RefreshActiveSettingsMenuScreen();
				}
				g_SettingsMan.UpdateSettingsFile();
			} else if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen) {
				g_NetMatchService.Destroy();
			} else if (m_ActiveMenuScreen == MenuScreen::CreditsScreen) {
				m_UpdateResult = MainMenuUpdateResult::BackToMainFromCredits;
			}
			m_ActiveDialogBox = nullptr;
			SetActiveMenuScreen(MenuScreen::MainScreen, false);
			g_GUISound.BackButtonPressSound()->Play();
		} else {
			ShowQuitScreenOrQuit();
		}
	} else if (m_ActiveMenuScreen == MenuScreen::SettingsScreen && m_ActiveDialogBox && g_UInputMan.KeyPressed(SDLK_ESCAPE)) {
		m_SettingsMenu->CloseActiveDialogBox();
	}
}

bool MainMenuGUI::HandleInputEvents() {
	if (m_ActiveMenuScreen == MenuScreen::MainScreen) {
		int mouseX = 0;
		int mouseY = 0;
		m_ActiveGUIControlManager->GetManager()->GetInputController()->GetMousePosition(&mouseX, &mouseY);
		UpdateMainScreenHoveredButton(dynamic_cast<GUIButton*>(m_MainMenuScreenGUIControlManager->GetControlUnderPoint(mouseX, mouseY, m_MainMenuScreens[MenuScreen::MainScreen], 1)));
	}
	m_ActiveGUIControlManager->Update();

	GUIEvent guiEvent;
	while (m_ActiveGUIControlManager->GetEvent(&guiEvent)) {
		if (guiEvent.GetType() == GUIEvent::Notification && dynamic_cast<GUIButton*>(guiEvent.GetControl())) {
			for (size_t row = 0; row < m_ModerationSeatLabels.size(); ++row) {
				const auto* control = guiEvent.GetControl();
				if (control != m_ModerationApplicantButtons[row] && control != m_ModerationWaitButtons[row] &&
				    control != m_ModerationSubstituteButtons[row] && control != m_ModerationCancelButtons[row]) continue;
				if (guiEvent.GetMsg() == GUIButton::Pushed && row < m_ModerationUx.RowCount()) {
					m_PressedModeration.try_emplace(control, m_ModerationUx.GetRow(row));
				} else if (guiEvent.GetMsg() == GUIButton::UnPushed && !static_cast<GUIButton*>(guiEvent.GetControl())->IsCaptured()) {
					m_PressedModeration.erase(control);
				}
			}
		}
		if (guiEvent.GetType() == GUIEvent::Command) {
			if (guiEvent.GetControl() == m_MainMenuButtons[MenuButton::BackToMainButton]) {
				return true;
			}
			switch (m_ActiveMenuScreen) {
				case MenuScreen::MainScreen:
					HandleMainScreenInputEvents(guiEvent.GetControl());
					break;
				case MenuScreen::MetaGameNoticeScreen:
					HandleMetaGameNoticeScreenInputEvents(guiEvent.GetControl());
					break;
				case MenuScreen::MultiplayerScreen:
					HandleMultiplayerScreenInputEvents(guiEvent.GetControl());
					break;
				case MenuScreen::EditorScreen:
					HandleEditorsScreenInputEvents(guiEvent.GetControl());
					break;
				case MenuScreen::QuitScreen:
					HandleQuitScreenInputEvents(guiEvent.GetControl());
					break;
				default:
					break;
			}
		} else if (guiEvent.GetType() == GUIEvent::Notification && (guiEvent.GetMsg() == GUIButton::Focused && dynamic_cast<GUIButton*>(guiEvent.GetControl()))) {
			g_GUISound.SelectionChangeSound()->Play();
		} else if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetControl() == m_MultiplayerLanGamesList) {
			HandleMultiplayerScreenInputEvents(guiEvent.GetControl());
		}
	}
	return false;
}

void MainMenuGUI::HandleMainScreenInputEvents(const GUIControl* guiEventControl) {
	if (guiEventControl == m_MainMenuButtons[MenuButton::MetaGameButton]) {
		if (!m_MetaGameNoticeShown) {
			SetActiveMenuScreen(MenuScreen::MetaGameNoticeScreen);
		} else {
			m_UpdateResult = MainMenuUpdateResult::MetaGameStarted;
			SetActiveMenuScreen(MenuScreen::MainScreen);
		}
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ScenarioButton]) {
		m_UpdateResult = MainMenuUpdateResult::ScenarioStarted;
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerButton]) {
		SetActiveMenuScreen(MenuScreen::MultiplayerScreen);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::SaveOrLoadGameButton]) {
		SetActiveMenuScreen(MenuScreen::SaveOrLoadGameScreen);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::SettingsButton]) {
		SetActiveMenuScreen(MenuScreen::SettingsScreen);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::EditorsButton]) {
		SetActiveMenuScreen(MenuScreen::EditorScreen);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ModManagerButton]) {
		SetActiveMenuScreen(MenuScreen::ModManagerScreen);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::CreditsButton]) {
		SetActiveMenuScreen(MenuScreen::CreditsScreen);
		m_UpdateResult = MainMenuUpdateResult::EnterCreditsScreen;
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::QuitButton]) {
		ShowQuitScreenOrQuit();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ResumeButton]) {
		m_UpdateResult = MainMenuUpdateResult::ActivityResumed;
		g_GUISound.BackButtonPressSound()->Play();
	}
}

void MainMenuGUI::HandleMultiplayerScreenInputEvents(const GUIControl* guiEventControl) {
	if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerHostGameButton]) {
		m_MultiplayerLandingStatusLabel->SetText("");
		m_MultiplayerSubScreen = MultiplayerSubScreen::HostSetup;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerJoinGameButton]) {
		m_MultiplayerLandingStatusLabel->SetText("");
		m_MultiplayerSubScreen = MultiplayerSubScreen::JoinSetup;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerHostBackButton] || guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerJoinBackButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerCreateButton]) {
		StartMultiplayer(true);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerConnectButton]) {
		StartMultiplayer(false);
	} else if (guiEventControl == m_MultiplayerHostModeButton) {
		// Cycle PvP -> Co-op PvE -> PvPvE. PvE modes add a CPU team the host's AI drives.
		m_MultiplayerHostMode = m_MultiplayerHostMode == NetMatchMode::PvPSkirmish ? NetMatchMode::CoopPvE : (m_MultiplayerHostMode == NetMatchMode::CoopPvE ? NetMatchMode::PvPvE : NetMatchMode::PvPSkirmish);
		const char* modeText = m_MultiplayerHostMode == NetMatchMode::PvPSkirmish ? "Mode: PvP" : (m_MultiplayerHostMode == NetMatchMode::CoopPvE ? "Mode: Co-op PvE" : "Mode: PvPvE");
		m_MultiplayerHostModeButton->SetText(modeText);
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerReadyButton]) {
		g_NetMatchService.SetReady();
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerStartButton]) {
		g_NetMatchService.RequestStart();
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]) {
		g_NetMatchService.Destroy();
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]) {
		// §11's manual retry, and the same button that takes up the stored ticket after a relaunch.
		NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
		reconnect.RequestManualRetry(MenuClockMs());
		reconnect.DismissOffer();
		std::string rejoinError;
		if (!g_NetMatchService.BeginTicketRejoin(&rejoinError)) {
			reconnect.NoteAttemptFailed(MenuClockMs(), rejoinError);
			m_MultiplayerLandingStatusLabel->SetText(rejoinError);
		} else {
			reconnect.NoteAttemptStarted(MenuClockMs());
			m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
		}
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]) {
		// A cancel stops the automatic attempts; the recovery record survives it, so Rejoin still works.
		g_NetMatchService.GetReconnectUx().Cancel(MenuClockMs());
		g_NetMatchService.GetReconnectUx().DismissOffer();
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerModerateButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Moderation;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerModerationBackButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MultiplayerLanGamesList) {
		// Clicking a discovered host fills the join fields; Connect stays the explicit action.
		const int selected = m_MultiplayerLanGamesList->GetSelectedIndex();
		if (selected >= 0 && static_cast<size_t>(selected) < m_LanHosts.size()) {
			m_MultiplayerJoinAddressTextBox->SetText(m_LanHosts[static_cast<size_t>(selected)].address);
			m_MultiplayerJoinPortTextBox->SetText(std::to_string(m_LanHosts[static_cast<size_t>(selected)].port));
			g_GUISound.ItemChangeSound()->Play();
		}
	}
	// §9b's three actions, one row per disconnected seat.
	for (size_t row = 0; row < m_ModerationSeatLabels.size(); ++row) {
		if (guiEventControl == m_ModerationApplicantButtons[row]) {
			const auto pressed = m_PressedModeration.find(guiEventControl);
			if (pressed != m_PressedModeration.end()) m_ModerationUx.CycleApplicant(pressed->second);
			g_GUISound.ItemChangeSound()->Play();
		} else if (guiEventControl == m_ModerationWaitButtons[row]) {
			ActivateModerationRow(guiEventControl, NetModerationAction::Wait);
			g_GUISound.ButtonPressSound()->Play();
		} else if (guiEventControl == m_ModerationSubstituteButtons[row]) {
			ActivateModerationRow(guiEventControl, NetModerationAction::Substitute);
			g_GUISound.ButtonPressSound()->Play();
		} else if (guiEventControl == m_ModerationCancelButtons[row]) {
			ActivateModerationRow(guiEventControl, NetModerationAction::Cancel);
			g_GUISound.BackButtonPressSound()->Play();
		}
	}
}

void MainMenuGUI::StartMultiplayer(bool host) {
	const std::string portText = (host ? m_MultiplayerHostPortTextBox : m_MultiplayerJoinPortTextBox)->GetText();
	char* parseEnd = nullptr;
	const long parsedPort = std::strtol(portText.c_str(), &parseEnd, 10);
	if (portText.empty() || *parseEnd != '\0' || parsedPort < 1 || parsedPort > 65535) {
		m_MultiplayerLandingStatusLabel->SetText("Port must be a whole number from 1 to 65535.");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		return;
	}
	if (!host && m_MultiplayerJoinAddressTextBox->GetText().empty()) {
		m_MultiplayerLandingStatusLabel->SetText("Enter the host's address to join.");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		return;
	}
	NetMatchServiceRequest request;
	request.host = host;
	if (!host) {
		request.address = m_MultiplayerJoinAddressTextBox->GetText();
	}
	request.port = static_cast<uint16_t>(parsedPort);
	request.playerName = m_MultiplayerNameTextBox->GetText().empty() ? (host ? "Host" : "Client") : m_MultiplayerNameTextBox->GetText();
	request.activityPreset = "P4 Alpha Duel";
	request.ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
	// Headed matches self-heal: a desync (or a rejoiner) reloads everyone from the host's snapshot.
	request.resyncOnDesync = true;
	// The host picks the roster size and the lockstep input-delay buffer; clients adopt both via
	// the lobby config sync. The delay box writes back to the setting so the choice persists.
	if (host) {
		const long parsedPlayers = std::strtol(m_MultiplayerHostPlayersTextBox->GetText().c_str(), nullptr, 10);
		request.peerCount = static_cast<uint8_t>(std::clamp<long>(parsedPlayers, NetMatchConfigUtil::c_MinPeerCount, NetMatchConfigUtil::c_MaxPeerCount));
		m_MultiplayerHostPlayersTextBox->SetText(std::to_string(request.peerCount));
		request.mode = m_MultiplayerHostMode;
		const long parsedDelay = std::strtol(m_MultiplayerHostInputDelayTextBox->GetText().c_str(), nullptr, 10);
		const int inputDelay = std::clamp<int>(static_cast<int>(parsedDelay), 0, NetMatchConfigUtil::c_MaxInputDelayFrames);
		m_MultiplayerHostInputDelayTextBox->SetText(std::to_string(inputDelay));
		g_SettingsMan.SetNetworkInputDelayFrames(inputDelay);
		request.inputDelayFrames = static_cast<uint16_t>(inputDelay);
		// The typed delay is the floor; the host raises it to cover the measured ping so high-RTT
		// matches run stall-free out of the box.
		request.autoInputDelay = true;
	}

	std::string error;
	if (g_NetMatchService.Start(request, &error)) {
		m_MultiplayerLandingStatusLabel->SetText("");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else {
		m_MultiplayerLandingStatusLabel->SetText(error);
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
	}
	g_GUISound.ButtonPressSound()->Play();
}

void MainMenuGUI::UpdateMultiplayerScreen() {
	g_NetMatchService.Update();
	// A completed match leaves the session connected; reconvene both peers in the lobby for a rematch.
	// On a lost session this settles the service into Failed once, so it is not retried every frame.
	if (g_NetMatchService.GetState() == NetMatchServiceState::Completed) {
		g_NetMatchService.ReturnToLobby();
	}
	const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();

	// While a connection is being established, cap the menu update rate so the GNS I/O service thread
	// isn't CPU-starved by the menu rendering flat-out (the headless path yields the same way).
	if (snapshot.inLobby) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	// Reconcile the sub-screen with the live service state. The moderation panel is a lobby screen
	// the host opened, so it stays open until the host leaves it or the match ends under it.
	const bool inMatchOrLobby = snapshot.inLobby || snapshot.running;
	if (inMatchOrLobby && m_MultiplayerSubScreen != MultiplayerSubScreen::Moderation) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else if (!inMatchOrLobby && (m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby || m_MultiplayerSubScreen == MultiplayerSubScreen::Moderation)) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		m_MultiplayerLandingStatusLabel->SetText(snapshot.errorText);
	}

	RefreshMultiplayerScreenControls(snapshot);
	MaybeLaunchMultiplayerActivity();
}

// The menu's own clock: the reconnect schedule is real time, not sim time.
uint64_t MainMenuGUI::MenuClockMs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void MainMenuGUI::RefreshReconnectControls() {
	const NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
	const bool offering = reconnect.GetOffer() == NetReconnectOffer::Available;
	const bool landing = m_MultiplayerSubScreen == MultiplayerSubScreen::Landing;
	const bool recovering = reconnect.IsActive();
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetVisible(landing && (offering || reconnect.CanRetryManually()));
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetEnabled(offering || reconnect.CanRetryManually());
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetText(offering ? "Rejoin Match" : "Retry");
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->SetVisible(landing && (offering || recovering));
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->SetEnabled(offering || reconnect.CanCancel());
	if (!landing) {
		return;
	}
	// One persistent line, never a toast: the status while recovering, otherwise whatever the startup
	// scan of the recovery record found - including precisely why it cannot be used.
	const std::string status = recovering ? reconnect.GetStatusText() : reconnect.GetOfferText();
	if (!status.empty()) {
		m_MultiplayerLandingStatusLabel->SetText(status);
	}
}

void MainMenuGUI::RefreshMultiplayerScreenControls(const NetLobbySnapshot& snapshot) {
	const bool lobby = m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby;
	m_MultiplayerLandingPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::Landing);
	m_MultiplayerHostPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::HostSetup);
	m_MultiplayerJoinPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::JoinSetup);
	m_MultiplayerLobbyPanel->SetVisible(lobby);
	const bool moderating = m_MultiplayerSubScreen == MultiplayerSubScreen::Moderation;
	m_MultiplayerModerationPanel->SetVisible(moderating);
	RefreshLanGamesList();
	RefreshReconnectControls();
	if (moderating) {
		RefreshModerationControls(snapshot);
	}
	if (!lobby) {
		if (m_MainMenuScreens[MenuScreen::MultiplayerScreen]->GetHeight() != 250) {
			m_MainMenuScreens[MenuScreen::MultiplayerScreen]->Resize(300, 250);
		}
		m_MainMenuButtons[MenuButton::BackToMainButton]->SetPositionRel((300 - m_MainMenuButtons[MenuButton::BackToMainButton]->GetWidth()) / 2, 250);
		return;
	}

	std::string matchInfo = snapshot.activityPreset;
	if (!snapshot.sceneName.empty()) {
		matchInfo += " - " + snapshot.sceneName;
	}
	if (!snapshot.modeName.empty()) {
		matchInfo += " - " + snapshot.modeName;
	}
	m_MultiplayerLobbyMatchLabel->SetText(matchInfo);
	for (size_t i = 0; i < m_MultiplayerLobbyPlayerLabels.size(); ++i) {
		GUILabel* label = m_MultiplayerLobbyPlayerLabels[i];
		if (i >= snapshot.members.size()) {
			label->SetText("");
			label->SetVisible(false);
			continue;
		}
		const NetLobbyMember& member = snapshot.members[i];
		const std::string name = member.displayName.size() > 14 ? member.displayName.substr(0, 13) + "." : member.displayName;
		std::string row = name + (member.isLocal ? " (you)" : "") + " - Team " + std::to_string(member.team + 1);
		row += member.peerId == 1 ? " - Host" : (member.ready ? " - Ready" : " - Not ready");
		// §11's persistent line for the seat, derived on this peer; the short mark while there is none.
		row += member.statusLine.empty() ? std::string(NetReconnectUx::RosterMark(member.dropped, member.reclaiming)) : " - " + member.statusLine;
		if (!member.isLocal && member.connected) {
			row += " - ";
			row += NetConnectionQualityName(ClassifyConnectionQuality(member.pingMs));
			if (member.pingMs > 0) {
				row += " (" + std::to_string(member.pingMs) + "ms)";
			}
		}
		label->SetText(row);
		label->SetVisible(true);
	}
	static std::string s_shareAddress;
	static bool s_shareResolved = false;
	if (snapshot.isHost && snapshot.inLobby && !snapshot.remoteReady) {
		size_t connectedCount = 0;
		for (const NetLobbyMember& member: snapshot.members) {
			if (member.connected) {
				++connectedCount;
			}
		}
		if (connectedCount < 2) {
			if (!s_shareResolved) {
				s_shareAddress = NetLanDiscovery::GetPrimaryLocalAddress();
				s_shareResolved = true;
			}
			m_MultiplayerStatusLabel->SetText(s_shareAddress.empty()
			                                      ? "Waiting for a player to join..."
			                                      : "Waiting for a player to join... share " + s_shareAddress + ":" + m_MultiplayerHostPortTextBox->GetText());
		} else {
			s_shareResolved = false;
			if (connectedCount < snapshot.members.size()) {
				m_MultiplayerStatusLabel->SetText("Waiting for players to join... (" + std::to_string(connectedCount) + "/" + std::to_string(snapshot.members.size()) + ")");
			} else {
				m_MultiplayerStatusLabel->SetText("Waiting for everyone to ready up...");
			}
		}
	} else {
		s_shareResolved = false;
		m_MultiplayerStatusLabel->SetText(snapshot.statusText);
	}
	m_MultiplayerErrorLabel->SetText(snapshot.errorText);
	const int errorHeight = std::max(24, m_MultiplayerErrorLabel->GetTextHeight() + 4);
	const int extraHeight = errorHeight - 24;
	if (m_MultiplayerErrorLabel->GetHeight() != errorHeight) {
		m_MultiplayerErrorLabel->Resize(m_MultiplayerErrorLabel->GetWidth(), errorHeight);
		m_MultiplayerLobbyPanel->Resize(300, 250 + extraHeight);
	}
	if (m_MainMenuScreens[MenuScreen::MultiplayerScreen]->GetHeight() != 250 + extraHeight) {
		m_MainMenuScreens[MenuScreen::MultiplayerScreen]->Resize(300, 250 + extraHeight);
	}
	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetPositionRel(55, 192 + extraHeight);
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetPositionRel(55, 192 + extraHeight);
	m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]->SetPositionRel(90, 220 + extraHeight);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetPositionRel((300 - m_MainMenuButtons[MenuButton::BackToMainButton]->GetWidth()) / 2, 250 + extraHeight);

	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetVisible(!snapshot.isHost);
	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetEnabled(!snapshot.isHost && snapshot.inLobby);
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetVisible(snapshot.isHost);
	// Start only once the remote peer is actually ready, not merely present.
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetEnabled(snapshot.isHost && snapshot.inLobby && snapshot.remoteReady);
	m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]->SetEnabled(true);
	// §9b: moderation is a match feature - a lobby seat whose holder leaves goes straight back in the pool.
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton]->SetPositionRel(212, 220 + extraHeight);
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton]->SetVisible(snapshot.isHost);
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton]->SetEnabled(snapshot.isHost && snapshot.running);
}

void MainMenuGUI::RefreshModerationControls(const NetLobbySnapshot& snapshot) {
	m_ModerationUx.Refresh(g_NetMatchService.GetModerationSeats());
	m_MultiplayerModerationSummaryLabel->SetText(snapshot.isHost ? m_ModerationUx.GetSummaryText() : "Only the host decides about seats.");
	m_MultiplayerModerationStatusLabel->SetText(m_ModerationUx.GetStatusText());
	for (size_t row = 0; row < m_ModerationSeatLabels.size(); ++row) {
		const bool used = row < m_ModerationUx.RowCount();
		m_ModerationSeatLabels[row]->SetVisible(used);
		m_ModerationApplicantButtons[row]->SetVisible(used);
		m_ModerationWaitButtons[row]->SetVisible(used);
		m_ModerationSubstituteButtons[row]->SetVisible(used);
		m_ModerationCancelButtons[row]->SetVisible(used);
		if (!used) {
			m_ModerationSeatLabels[row]->SetText("");
			continue;
		}
		const NetModerationUx::Row& seat = m_ModerationUx.GetRow(row);
		m_ModerationSeatLabels[row]->SetText(seat.text);
		m_ModerationApplicantButtons[row]->SetText(seat.applicantText);
		m_ModerationApplicantButtons[row]->SetEnabled(seat.view.actionsAvailable && (seat.applicants > 1 || (seat.applicants && seat.applicant == c_InvalidNetPeerId)));
		m_ModerationWaitButtons[row]->SetEnabled(NetModerationUx::Available(seat, NetModerationAction::Wait));
		m_ModerationSubstituteButtons[row]->SetEnabled(NetModerationUx::Available(seat, NetModerationAction::Substitute));
		m_ModerationCancelButtons[row]->SetEnabled(NetModerationUx::Available(seat, NetModerationAction::Cancel));
	}
}

void MainMenuGUI::ActivateModerationRow(const GUIControl* control, NetModerationAction action) {
	const auto pressed = m_PressedModeration.find(control);
	if (pressed == m_PressedModeration.end()) return;
	m_ModerationUx.Act(pressed->second, action);
	m_PressedModeration.erase(pressed);
	m_MultiplayerModerationStatusLabel->SetText(m_ModerationUx.GetStatusText());
}

bool MainMenuGUI::AutomationModerate(const std::string& action, int stableSeat) {
	if (m_ActiveMenuScreen != MenuScreen::MultiplayerScreen || m_MultiplayerSubScreen != MultiplayerSubScreen::Moderation ||
	    !m_MultiplayerModerationPanel->GetVisible() || !m_MultiplayerModerationPanel->GetEnabled()) return false;
	NetModerationAction verb = NetModerationAction::Wait;
	if (action == "substitute") {
		verb = NetModerationAction::Substitute;
	} else if (action == "cancel") {
		verb = NetModerationAction::Cancel;
	} else if (action != "wait") {
		return false;
	}
	m_ModerationUx.Refresh(g_NetMatchService.GetModerationSeats());
	const size_t row = stableSeat < 0 ? 0 : m_ModerationUx.FindSeat(static_cast<uint16_t>(stableSeat));
	if (row >= m_ModerationUx.RowCount()) {
		return false;
	}
	if (!NetModerationUx::Available(m_ModerationUx.GetRow(row), verb)) return false;
	return m_ModerationUx.Act(row, verb) == NetH4ModerationResult::Ok;
}

// True only if the control and every ancestor panel are enabled and visible, i.e. a human could actually click it.
static bool IsControlClickable(GUIControl* control) {
	for (GUIControl* node = control; node; node = node->GetParent()) {
		if (!node->GetEnabled() || !node->GetVisible()) {
			return false;
		}
	}
	return true;
}

bool MainMenuGUI::AutomationActivateControl(const std::string& controlName) {
	GUIControl* control = m_SubMenuScreenGUIControlManager->GetControl(controlName);
	if (!control) {
		control = m_MainMenuScreenGUIControlManager->GetControl(controlName);
	}
	if (!control || !IsControlClickable(control)) {
		return false;
	}
	switch (m_ActiveMenuScreen) {
		case MenuScreen::MainScreen: HandleMainScreenInputEvents(control); break;
		case MenuScreen::MetaGameNoticeScreen: HandleMetaGameNoticeScreenInputEvents(control); break;
		case MenuScreen::MultiplayerScreen: HandleMultiplayerScreenInputEvents(control); break;
		case MenuScreen::EditorScreen: HandleEditorsScreenInputEvents(control); break;
		case MenuScreen::QuitScreen: HandleQuitScreenInputEvents(control); break;
		default: return false;
	}
	return true;
}

bool MainMenuGUI::AutomationSetText(const std::string& controlName, const std::string& text) {
	GUITextBox* textBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl(controlName));
	if (textBox && IsControlClickable(textBox)) {
		textBox->SetText(text);
		return true;
	}
	return false;
}

std::string MainMenuGUI::AutomationActiveScreenName() const {
	switch (m_ActiveMenuScreen) {
		case MenuScreen::MainScreen: return "MainScreen";
		case MenuScreen::MetaGameNoticeScreen: return "MetaGameNoticeScreen";
		case MenuScreen::MultiplayerScreen: return "MultiplayerScreen";
		case MenuScreen::SaveOrLoadGameScreen: return "SaveOrLoadGameScreen";
		case MenuScreen::SettingsScreen: return "SettingsScreen";
		case MenuScreen::ModManagerScreen: return "ModManagerScreen";
		case MenuScreen::EditorScreen: return "EditorScreen";
		case MenuScreen::CreditsScreen: return "CreditsScreen";
		case MenuScreen::QuitScreen: return "QuitScreen";
		default: return "Unknown";
	}
}

std::string MainMenuGUI::AutomationMultiplayerStatus() const {
	if (m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby && m_MultiplayerStatusLabel) {
		return m_MultiplayerStatusLabel->GetText();
	}
	return g_NetMatchService.GetStatusText();
}

std::string MainMenuGUI::AutomationMultiplayerError() const {
	return m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby ? m_MultiplayerErrorLabel->GetText() : m_MultiplayerLandingStatusLabel->GetText();
}

std::string MainMenuGUI::AutomationMultiplayerSubScreen() const {
	switch (m_MultiplayerSubScreen) {
		case MultiplayerSubScreen::Landing: return "Landing";
		case MultiplayerSubScreen::HostSetup: return "HostSetup";
		case MultiplayerSubScreen::JoinSetup: return "JoinSetup";
		case MultiplayerSubScreen::Lobby: return "Lobby";
		case MultiplayerSubScreen::Moderation: return "Moderation";
		default: return "Unknown";
	}
}

bool MainMenuGUI::AutomationControlExists(const std::string& controlName) const {
	return m_SubMenuScreenGUIControlManager->GetControl(controlName) != nullptr ||
	       m_MainMenuScreenGUIControlManager->GetControl(controlName) != nullptr;
}

bool MainMenuGUI::AutomationControlEnabled(const std::string& controlName) const {
	GUIControl* control = m_SubMenuScreenGUIControlManager->GetControl(controlName);
	if (!control) {
		control = m_MainMenuScreenGUIControlManager->GetControl(controlName);
	}
	// "Enabled" for automation means interactable as a human would see it: enabled and visible up the chain.
	return control && IsControlClickable(control);
}

void MainMenuGUI::HandleMetaGameNoticeScreenInputEvents(const GUIControl* guiEventControl) {
	if (guiEventControl == m_MainMenuButtons[MenuButton::PlayTutorialButton]) {
		m_UpdateResult = MainMenuUpdateResult::ActivityStarted;
		SetActiveMenuScreen(MenuScreen::MainScreen);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MetaGameContinueButton]) {
		m_UpdateResult = MainMenuUpdateResult::MetaGameStarted;
		SetActiveMenuScreen(MenuScreen::MainScreen);
	}
}

void MainMenuGUI::HandleEditorsScreenInputEvents(const GUIControl* guiEventControl) {
	std::string editorToStart;
	if (guiEventControl == m_MainMenuButtons[MenuButton::SceneEditorButton]) {
		editorToStart = "SceneEditor";
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::AreaEditorButton]) {
		editorToStart = "AreaEditor";
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::AssemblyEditorButton]) {
		editorToStart = "AssemblyEditor";
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::GibEditorButton]) {
		editorToStart = "GibEditor";
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ActorEditorButton]) {
		editorToStart = "ActorEditor";
	}
	if (!editorToStart.empty()) {
		m_UpdateResult = MainMenuUpdateResult::ActivityStarted;
		SetActiveMenuScreen(MenuScreen::MainScreen, false);
		g_GUISound.ExitMenuSound()->Play();
		g_ActivityMan.SetStartEditorActivity(editorToStart);
	}
}

void MainMenuGUI::HandleQuitScreenInputEvents(const GUIControl* guiEventControl) {
	if (guiEventControl == m_MainMenuButtons[MenuButton::QuitConfirmButton]) {
		m_UpdateResult = MainMenuUpdateResult::Quit;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::QuitCancelButton]) {
		SetActiveMenuScreen(MenuScreen::MainScreen);
	}
}

void MainMenuGUI::UpdateMainScreenHoveredButton(const GUIButton* hoveredButton) {
	int hoveredButtonIndex = -1;
	if (hoveredButton) {
		hoveredButtonIndex = std::distance(m_MainMenuButtons.begin(), std::find(m_MainMenuButtons.begin(), m_MainMenuButtons.end(), hoveredButton));
		if (hoveredButton != m_MainScreenHoveredButton) {
			m_MainMenuButtons.at(hoveredButtonIndex)->SetText(m_MainScreenButtonHoveredText.at(hoveredButtonIndex));
		}
		m_MainScreenHoveredButton = m_MainMenuButtons.at(hoveredButtonIndex);
	}
	if (!hoveredButton || hoveredButtonIndex != m_MainScreenPrevHoveredButtonIndex) {
		m_MainMenuButtons.at(m_MainScreenPrevHoveredButtonIndex)->SetText(m_MainScreenButtonUnhoveredText.at(m_MainScreenPrevHoveredButtonIndex));
	}

	if (hoveredButtonIndex >= 0) {
		m_MainScreenPrevHoveredButtonIndex = hoveredButtonIndex;
	} else {
		m_MainScreenHoveredButton = nullptr;
	}
}

void MainMenuGUI::RefreshLanGamesList() {
	if (!m_MultiplayerLanGamesList) {
		return;
	}
	// The browser only runs while the join screen is up; the beacon side lives in the service.
	if (m_MultiplayerSubScreen != MultiplayerSubScreen::JoinSetup) {
		if (m_LanBrowser.IsBrowsing()) {
			m_LanBrowser.Stop();
			m_LanHosts.clear();
			m_MultiplayerLanGamesList->ClearList();
		}
		return;
	}
	std::string ignored;
	if (!m_LanBrowser.IsBrowsing() && !m_LanBrowser.StartBrowser(&ignored)) {
		return;
	}
	// A real monotonic clock, so host expiry holds at any frame rate and while minimized.
	m_LanBrowserNowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	m_LanBrowser.Tick(m_LanBrowserNowMs);
	std::vector<NetLanHostInfo> hosts = m_LanBrowser.GetHosts(m_LanBrowserNowMs);
	const auto describe = [](const NetLanHostInfo& host) {
		return host.hostName + " - " + host.activity + " (" + std::to_string(host.playerCount) + "/" + std::to_string(host.maxPlayers) + ") " + host.address;
	};
	bool changed = hosts.size() != m_LanHosts.size();
	for (size_t i = 0; !changed && i < hosts.size(); ++i) {
		changed = describe(hosts[i]) != describe(m_LanHosts[i]);
	}
	if (!changed) {
		return;
	}
	m_LanHosts = std::move(hosts);
	m_MultiplayerLanGamesList->ClearList();
	for (const NetLanHostInfo& host: m_LanHosts) {
		m_MultiplayerLanGamesList->AddItem(describe(host));
	}
}

void MainMenuGUI::MaybeLaunchMultiplayerActivity() {
	std::string activityPreset;
	if (!g_NetMatchService.ConsumeReadyToLaunch(activityPreset)) {
		return;
	}
	// A joiner whose lobby round carried a live match's snapshot launches from it instead.
	if (g_NetMatchService.HasPendingResyncLoad()) {
		std::string stageError;
		if (!g_NetMatchService.StageResyncedMatchLaunch(&stageError)) {
			m_MultiplayerErrorLabel->SetText(stageError);
			g_NetMatchService.Destroy();
			return;
		}
		m_UpdateResult = MainMenuUpdateResult::ActivityStarted;
		SetActiveMenuScreen(MenuScreen::MainScreen, false);
		g_GUISound.ExitMenuSound()->Play();
		return;
	}
	const Entity* presetEntity = g_PresetMan.GetEntityPreset("GAScripted", activityPreset);
	const Activity* presetActivity = dynamic_cast<const Activity*>(presetEntity);
	if (!presetActivity) {
		m_MultiplayerErrorLabel->SetText("Could not find multiplayer activity preset.");
		g_NetMatchService.Destroy();
		return;
	}
	if (!presetActivity->GetSceneName().empty()) {
		g_SceneMan.SetSceneToLoad(presetActivity->GetSceneName(), true, false);
	}
	Activity* activity = dynamic_cast<Activity*>(presetActivity->Clone());
	if (!activity) {
		m_MultiplayerErrorLabel->SetText("Could not create multiplayer activity.");
		g_NetMatchService.Destroy();
		return;
	}
	const int localTeam = g_NetMatchService.GetLocalTeam();
	if (localTeam >= Activity::TeamOne && localTeam < Activity::MaxTeamCount) {
		activity->ClearPlayers(false);
		activity->AddPlayer(Players::PlayerOne, true, localTeam, 0);
		activity->ForceSetTeamAsActive(Activity::TeamOne);
		activity->ForceSetTeamAsActive(Activity::TeamTwo);
		activity->SetTeamFunds(0, Activity::TeamOne);
		activity->SetTeamFunds(0, Activity::TeamTwo);
	}
	ScenarioRunner::ApplyDeterministicConfig();
	g_ActivityMan.SetStartActivity(activity);
	m_UpdateResult = MainMenuUpdateResult::ActivityStarted;
	SetActiveMenuScreen(MenuScreen::MainScreen, false);
	g_GUISound.ExitMenuSound()->Play();
}

void MainMenuGUI::Draw() {
	// Early return to avoid single frame flicker when title screen goes into transition from the meta notice screen to meta config screen.
	if (m_UpdateResult == MainMenuUpdateResult::MetaGameStarted) {
		return;
	}
	switch (m_ActiveMenuScreen) {
		case MenuScreen::SaveOrLoadGameScreen:
			m_SaveLoadMenu->Draw();
			break;
		case MenuScreen::SettingsScreen:
			m_SettingsMenu->Draw();
			break;
		case MenuScreen::ModManagerScreen:
			m_ModManagerMenu->Draw();
			break;
		default:
			m_ActiveGUIControlManager->Draw();
			break;
	}
	if (m_ActiveDialogBox) {
		set_trans_blender(128, 128, 128, 128);
		draw_trans_sprite(g_FrameMan.GetBackBuffer32(), g_FrameMan.GetOverlayBitmap32(), 0, 0);
		// Whatever this box may be at this point it's already been drawn by the owning GUIControlManager, but we need to draw it again on top of the overlay so it's not affected by it.
		m_ActiveDialogBox->Draw(m_ActiveGUIControlManager->GetScreen());
	}
	m_ActiveGUIControlManager->DrawMouse();
}
