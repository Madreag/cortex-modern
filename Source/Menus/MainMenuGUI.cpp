#include "MainMenuGUI.h"

#include "WindowMan.h"
#include "FrameMan.h"
#include "ActivityMan.h"
#include "UInputMan.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "ConsoleMan.h"
#include "NetActivitySetup.h"
#include "NetMatchService.h"
#include "NetIdentity.h"
#include "NetMatchReplay.h"
#include "NetConnectionQuality.h"
#include "PresetMan.h"
#include "SceneMan.h"
#include "ScenarioRunner.h"
#include "TelemetryBundle.h"

#include "Activity.h"
#include "AllegroTools.h"
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
#include "GUICheckbox.h"
#include "GUIComboBox.h"

#include "Resources/Credits.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

using namespace RTE;

// The lobby and the network settings page show one saved name; "Player" is only the empty fallback.
static std::string SavedMultiplayerName() {
	return g_SettingsMan.GetNetworkDisplayName().empty() ? "Player" : g_SettingsMan.GetNetworkDisplayName();
}

bool StartNetReplayPlayback(const std::string& path, bool fromMenu, std::string* error);

static std::optional<size_t> ReplayRowIndex(const std::string& name) {
	const std::string prefix = "LabelReplayRow";
	if (!name.starts_with(prefix)) return std::nullopt;
	size_t index = 0;
	const auto [end, error] = std::from_chars(name.data() + prefix.size(), name.data() + name.size(), index);
	return error == std::errc{} && end == name.data() + name.size() ? std::optional<size_t>(index) : std::nullopt;
}

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
	m_LastMatchSummaryLabel = nullptr;
	m_LastMatchDetailsLabel = nullptr;
	m_LastMatchDialog = nullptr;
	m_ReplayBrowserPanel = nullptr;
	m_ReplayDeleteDialog = nullptr;
	m_ReplayList = nullptr;
	m_ReplaySelectedLabel = nullptr;
	m_ReplayStatusLabel = nullptr;
	m_ReplayDeleteLabel = nullptr;
	m_ReplayRows.clear();
	m_ReplayDeletePath.clear();
	m_MultiplayerNameTextBox = nullptr;
	m_MultiplayerHostPortTextBox = nullptr;
	m_MultiplayerHostPlayersTextBox = nullptr;
	m_MultiplayerHostInputDelayTextBox = nullptr;
	m_MultiplayerHostInputDelayPolicyLabel = nullptr;
	m_MultiplayerHostPortMapCheckbox = nullptr;
	m_MultiplayerHostModeButton = nullptr;
	m_MultiplayerHostActivityCombo = nullptr;
	m_MultiplayerHostInfoLabel = nullptr;
	m_MultiplayerHostActivities.clear();
	m_MultiplayerHostActivityIndex = 0;
	m_MultiplayerHostMode = NetMatchMode::PvPSkirmish;
	m_MultiplayerJoinAddressTextBox = nullptr;
	m_MultiplayerJoinPortTextBox = nullptr;
	m_MultiplayerLanGamesList = nullptr;
	m_MultiplayerLanGamesLabel = nullptr;
	m_LanGamesLabelText.clear();
	m_GameRows.clear();
	m_DirectoryIdentity.reset();
	m_DirectoryIdentityTried = false;
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
	m_MultiplayerLobbyPortMapLabel = nullptr;
	m_PortMapSerialShown = 0;
	m_MultiplayerLobbyChatLabels.fill(nullptr);
	m_MultiplayerLobbyChatInput = nullptr;
	m_MultiplayerLobbyChatLines.clear();

	m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
	m_ReconnectStatusShown.clear();
	m_PendingAutomationCommand.clear();
	m_CreditsScrollPanel = nullptr;
	m_MainMenuScreens.fill(nullptr);
	m_MainMenuButtons.fill(nullptr);

	m_MainScreenButtonHoveredText.fill(std::string());
	m_MainScreenButtonUnhoveredText.fill(std::string());
	m_MainScreenHoveredButton = nullptr;
	m_MainScreenPrevHoveredButtonIndex = 0;
}

void MainMenuGUI::Create(AllegroScreen* guiScreen, GUIInputWrapper* guiInput) {
	m_AutomationInput = guiInput->CreateAutomationInput();
	if (m_AutomationInput) guiInput = m_AutomationInput.get();
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
	m_MainMenuButtons[MenuButton::SaveDiagnosticsButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonSaveDiagnostics"));

	m_MultiplayerNameTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextMultiplayerName"));
	m_MultiplayerHostPortTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextHostPort"));
	m_MultiplayerHostPlayersTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextHostPlayers"));
	m_MultiplayerHostInputDelayTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextHostInputDelay"));
	m_MultiplayerHostInputDelayPolicyLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelHostInputDelayPolicy"));
	m_MultiplayerHostPortMapCheckbox = dynamic_cast<GUICheckbox*>(m_SubMenuScreenGUIControlManager->GetControl("CheckHostPortMap"));
	m_MultiplayerHostModeButton = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostMode"));
	m_MultiplayerHostActivityCombo = dynamic_cast<GUIComboBox*>(m_SubMenuScreenGUIControlManager->GetControl("ComboHostActivity"));
	m_MultiplayerHostInfoLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelHostInfo"));
	ApplyMultiplayerHostActivity();
	m_MultiplayerJoinAddressTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextJoinAddress"));
	m_MultiplayerJoinPortTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextJoinPort"));
	m_MultiplayerLanGamesList = dynamic_cast<GUIListBox*>(m_SubMenuScreenGUIControlManager->GetControl("ListLanGames"));
	m_MultiplayerLanGamesLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLanGames"));
	if (m_MultiplayerLanGamesLabel) {
		m_LanGamesLabelText = m_MultiplayerLanGamesLabel->GetText();
	}

	m_MultiplayerStatusLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelMultiplayerStatus"));
	m_MultiplayerErrorLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelMultiplayerError"));
	m_MultiplayerLandingStatusLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelMultiplayerLandingStatus"));
	m_MultiplayerLobbyMatchLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyMatch"));
	m_MultiplayerLobbyMatchModeLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyMatchMode"));
	m_LastMatchSummaryLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLastMatchSummary"));
	m_LastMatchDetailsLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLastMatchDetails"));
	m_LastMatchDialog = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("LastMatchDialog"));
	m_MainMenuButtons[MenuButton::LastMatchDetailsButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonLastMatchDetails"));
	m_MainMenuButtons[MenuButton::LastMatchCloseButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonLastMatchClose"));
	m_ReplayBrowserPanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("ReplayBrowserPanel"));
	m_ReplayDeleteDialog = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("ReplayDeleteDialog"));
	m_ReplayList = dynamic_cast<GUIListBox*>(m_SubMenuScreenGUIControlManager->GetControl("ListReplays"));
	m_ReplaySelectedLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelReplaySelected"));
	m_ReplayStatusLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelReplayStatus"));
	m_ReplayDeleteLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelReplayDelete"));
	m_MainMenuButtons[MenuButton::MultiplayerReplaysButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerReplays"));
	m_MainMenuButtons[MenuButton::ReplayPlayButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonReplayPlay"));
	m_MainMenuButtons[MenuButton::ReplayDeleteButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonReplayDelete"));
	m_MainMenuButtons[MenuButton::ReplayBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonReplayBack"));
	m_MainMenuButtons[MenuButton::ReplayDeleteConfirmButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonReplayDeleteConfirm"));
	m_MainMenuButtons[MenuButton::ReplayDeleteCancelButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonReplayDeleteCancel"));
	m_ReplayList->SetHighlightAsIfAlwaysFocused(true);
	m_MultiplayerLobbyPlayerLabels[0] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer0"));
	m_MultiplayerLobbyPlayerLabels[1] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer1"));
	m_MultiplayerLobbyPlayerLabels[2] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer2"));
	m_MultiplayerLobbyPlayerLabels[3] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer3"));
	m_MultiplayerLobbyPortMapLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPortMap"));
	m_MultiplayerLobbyPlayersHeader = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayersHeader"));

	m_MultiplayerLobbyPlayerRowFont = m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontLarge.png");
	m_MultiplayerLobbyPlayerRowFallbackFont = m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png");

	// The lobby's chat lives below the Leave/Seats row: eight FontSmall lines and one entry box.
	// They are created here rather than in the ini so the panel can grow for them without touching
	// the skin the other sub-screens share.
	GUIFont* chatFont = m_SubMenuScreenGUIControlManager->GetSkin() ? m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png") : nullptr;
	if (chatFont) {
		m_LastMatchSummaryLabel->SetFont(chatFont);
		m_LastMatchDetailsLabel->SetFont(chatFont);
		m_ReplaySelectedLabel->SetFont(chatFont);
		m_ReplayStatusLabel->SetFont(chatFont);
		m_ReplayDeleteLabel->SetFont(chatFont);
	}
	m_LastMatchSummaryLabel->SetHorizontalOverflowScroll(true);
	m_LastMatchSummaryLabel->ActivateDeactivateOverflowScroll(true);
	for (size_t row = 0; row < m_MultiplayerLobbyChatLabels.size(); ++row) {
		m_MultiplayerLobbyChatLabels[row] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->AddControl(
		    "LabelLobbyChat" + std::to_string(row), "LABEL", m_MultiplayerLobbyPanel, 8, 0, 284, 10));
		if (m_MultiplayerLobbyChatLabels[row]) {
			if (chatFont) m_MultiplayerLobbyChatLabels[row]->SetFont(chatFont);
			m_MultiplayerLobbyChatLabels[row]->SetVAlignment(GUIFont::Middle);
			m_MultiplayerLobbyChatLabels[row]->SetVisible(false);
		}
	}
	m_MultiplayerLobbyChatInput = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->AddControl(
	    "TextLobbyChat", "TEXTBOX", m_MultiplayerLobbyPanel, 8, 0, 284, 13));
	if (m_MultiplayerLobbyChatInput) {
		if (chatFont) m_MultiplayerLobbyChatInput->SetFont(chatFont);
		m_MultiplayerLobbyChatInput->SetMaxTextLength(static_cast<int>(NetProtocol::c_MaxShortTextBytes));
		m_MultiplayerLobbyChatInput->SetVisible(false);
	}

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

	m_MultiplayerNameTextBox->SetText(SavedMultiplayerName());
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
	m_MultiplayerHostInputDelayTextBox->SetNumericOnly(true);
	m_MultiplayerHostInputDelayTextBox->SetMaxNumericValue(NetMatchConfigUtil::c_MaxInputDelayFrames);
	m_MultiplayerHostInputDelayTextBox->SetMaxTextLength(2);
	RefreshHostInputDelayControls();
	m_MultiplayerHostPortMapCheckbox->SetCheck(g_SettingsMan.GetNetworkPortMapEnable() ? GUICheckbox::Checked : GUICheckbox::Unchecked);
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
	if (m_ActiveDialogBox && (m_ActiveDialogBox == m_LastMatchDialog || m_ActiveDialogBox == m_ReplayDeleteDialog)) CloseMultiplayerDialog();
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
	// The saved name is what an unconnected landing starts from; a live lobby keeps the name it joined under.
	if (m_MultiplayerSubScreen == MultiplayerSubScreen::Landing) {
		m_MultiplayerNameTextBox->SetText(SavedMultiplayerName());
	}
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

void MainMenuGUI::OfferStoredRejoinOnEntry() {
	if (g_NetMatchService.GetState() == NetMatchServiceState::Idle) {
		g_NetMatchService.ScanStoredTicket();
	}
	const NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
	if (reconnect.GetOffer() != NetReconnectOffer::Available && !reconnect.IsActive()) {
		return;
	}
	SetActiveMenuScreen(MenuScreen::MultiplayerScreen, false);
	m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
}

void MainMenuGUI::OfferRematchLobbyOnEntry() {
	if (!g_NetMatchService.NeedsCompletedLobbyPump()) {
		return;
	}
	SetActiveMenuScreen(MenuScreen::MultiplayerScreen, false);
	// UpdateMultiplayerScreen reconvenes the session and reconciles this panel from the snapshot;
	// naming it here keeps the first frame on the lobby instead of the landing panel.
	m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
}

void MainMenuGUI::HandleBackNavigation(bool backButtonPressed) {
	if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen && m_ActiveDialogBox && (backButtonPressed || g_UInputMan.KeyPressed(SDLK_ESCAPE))) {
		CloseMultiplayerDialog();
		return;
	}
	if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen && m_MultiplayerSubScreen == MultiplayerSubScreen::ReplayBrowser && g_UInputMan.KeyPressed(SDLK_ESCAPE)) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		return;
	}
	if ((!m_ActiveDialogBox || m_ActiveDialogBox == m_MainMenuScreens[MenuScreen::QuitScreen]) && (backButtonPressed || g_UInputMan.KeyPressed(SDLK_ESCAPE))) {
		if (m_ActiveMenuScreen != MenuScreen::MainScreen) {
			if (m_ActiveMenuScreen == MenuScreen::SettingsScreen || m_ActiveMenuScreen == MenuScreen::ModManagerScreen) {
				if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) {
					m_SettingsMenu->RefreshActiveSettingsMenuScreen();
				}
				g_SettingsMan.UpdateSettingsFile();
			} else if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen) {
				// A running recovery outlives the screen; Cancel is what stops it, not walking away.
				if (!g_NetMatchService.NeedsRecoveryPump()) {
					g_NetMatchService.Destroy();
				}
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
	PostPendingAutomationCommand();

	GUIEvent guiEvent;
	while (m_ActiveGUIControlManager->GetEvent(&guiEvent)) {
		if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen && m_ActiveDialogBox) {
			GUIControl* node = guiEvent.GetControl();
			while (node && node != m_ActiveDialogBox) node = node->GetParent();
			if (!node) continue;
		}
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
		} else if (guiEvent.GetType() == GUIEvent::Notification && (guiEvent.GetControl() == m_MultiplayerLanGamesList || guiEvent.GetControl() == m_ReplayList)) {
			HandleMultiplayerScreenInputEvents(guiEvent.GetControl());
		} else if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUIComboBox::Closed && guiEvent.GetControl() == m_MultiplayerHostActivityCombo) {
			HandleMultiplayerScreenInputEvents(guiEvent.GetControl());
		} else if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUICheckbox::Changed && guiEvent.GetControl() == m_MultiplayerHostPortMapCheckbox) {
			HandleMultiplayerScreenInputEvents(guiEvent.GetControl());
		} else if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUITextBox::Enter && guiEvent.GetControl() == m_MultiplayerLobbyChatInput) {
			SendLobbyChat();
		} else if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUITextBox::Changed && guiEvent.GetControl() == m_MultiplayerHostInputDelayTextBox) {
			// The label names the frames the box would send, so it follows the edit.
			if (m_MultiplayerHostInputDelayTextBox->GetEnabled()) {
				const long parsed = std::strtol(m_MultiplayerHostInputDelayTextBox->GetText().c_str(), nullptr, 10);
				const int frames = std::clamp<int>(static_cast<int>(parsed), 0, NetMatchConfigUtil::c_MaxInputDelayFrames);
				m_MultiplayerHostInputDelayPolicyLabel->SetText("(fixed, " + std::to_string(frames) + ")");
			}
		}
	}
	return false;
}

void MainMenuGUI::SendLobbyChat() {
	if (!m_MultiplayerLobbyChatInput) {
		return;
	}
	const std::string text = m_MultiplayerLobbyChatInput->GetText();
	if (text.empty()) {
		return;
	}
	const int modifier = m_SubMenuScreenGUIControlManager->GetManager()->GetInputController()->GetModifier();
	const uint8_t scope = (modifier & GUIPanel::MODI_CTRL) ? c_NetChatScopeTeam : c_NetChatScopeAll;
	if (g_NetMatchService.SendChat(scope, text)) {
		m_MultiplayerLobbyChatInput->SetText("");
	}
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
	if (guiEventControl == m_MainMenuButtons[MenuButton::SaveDiagnosticsButton]) {
		if (TelemetryBundle::RequestCapture()) g_ConsoleMan.PrintString("SYSTEM: Saving diagnostics...");
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::LastMatchDetailsButton]) {
		ShowLastMatchDetails();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::LastMatchCloseButton]) {
		CloseMultiplayerDialog();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerReplaysButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::ReplayBrowser;
		RefreshReplayBrowserControls();
		RefreshReplayList();
		m_ReplayList->SetFocus();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ReplayPlayButton]) {
		PlaySelectedReplay();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ReplayDeleteButton]) {
		const int selected = m_ReplayList->GetSelectedIndex();
		if (selected >= 0 && static_cast<size_t>(selected) < m_ReplayRows.size()) {
			m_ReplayDeletePath = m_ReplayRows[selected].path;
			m_ReplayDeleteLabel->SetText("Delete " + std::filesystem::path(m_ReplayDeletePath).filename().string() + "?\nThis removes the replay file.");
			const int width = m_ReplayBrowserPanel->GetWidth();
			m_ReplayDeleteLabel->Resize(width - 24, 88);
			const int height = std::max(104, m_ReplayDeleteLabel->GetTextHeight() + 64);
			m_ReplayDeleteDialog->Resize(width, height);
			m_ReplayDeleteLabel->Resize(width - 24, height - 64);
			m_MainMenuButtons[MenuButton::ReplayDeleteCancelButton]->SetPositionRel(width / 2 - 132, height - 34);
			m_MainMenuButtons[MenuButton::ReplayDeleteConfirmButton]->SetPositionRel(width / 2 + 12, height - 34);
			OpenMultiplayerDialog(m_ReplayDeleteDialog, m_ReplayBrowserPanel);
			m_MainMenuButtons[MenuButton::ReplayDeleteCancelButton]->SetFocus();
		}
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ReplayDeleteConfirmButton]) {
		ConfirmReplayDelete();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ReplayDeleteCancelButton]) {
		CloseMultiplayerDialog();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ReplayBackButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
	} else if (guiEventControl == m_ReplayList) {
		RefreshReplayBrowserControls();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerHostGameButton]) {
		m_MultiplayerLandingStatusLabel->SetText("");
		// The saved policy is re-read here, the way the port-map box is, so a settings change is not stale.
		RefreshHostInputDelayControls();
		m_MultiplayerHostPortMapCheckbox->SetCheck(g_SettingsMan.GetNetworkPortMapEnable() ? GUICheckbox::Checked : GUICheckbox::Unchecked);
		RefreshMultiplayerHostActivities();
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
	} else if (guiEventControl == m_MultiplayerHostPortMapCheckbox) {
		// The host panel persists nothing else; this key goes through the one settings save path.
		g_SettingsMan.SetNetworkPortMapEnable(m_MultiplayerHostPortMapCheckbox->GetCheck() == GUICheckbox::Checked);
		g_SettingsMan.UpdateSettingsFile();
		g_GUISound.ItemChangeSound()->Play();
	} else if (guiEventControl == m_MultiplayerHostModeButton) {
		// Cycle PvP -> Co-op PvE -> PvPvE. PvE modes add a CPU team the host's AI drives.
		m_MultiplayerHostMode = m_MultiplayerHostMode == NetMatchMode::PvPSkirmish ? NetMatchMode::CoopPvE : (m_MultiplayerHostMode == NetMatchMode::CoopPvE ? NetMatchMode::PvPvE : NetMatchMode::PvPSkirmish);
		const char* modeText = m_MultiplayerHostMode == NetMatchMode::PvPSkirmish ? "Mode: PvP" : (m_MultiplayerHostMode == NetMatchMode::CoopPvE ? "Mode: Co-op PvE" : "Mode: PvPvE");
		m_MultiplayerHostModeButton->SetText(modeText);
		ApplyMultiplayerHostActivity();
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MultiplayerHostActivityCombo) {
		// A closed pick-list carries its selection; the index is the request fields' source.
		const int selected = m_MultiplayerHostActivityCombo ? m_MultiplayerHostActivityCombo->GetSelectedIndex() : -1;
		if (selected >= 0 && static_cast<size_t>(selected) < m_MultiplayerHostActivities.size()) {
			m_MultiplayerHostActivityIndex = static_cast<size_t>(selected);
		}
		ApplyMultiplayerHostActivity();
		g_GUISound.ItemChangeSound()->Play();
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
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerReconnectButton] &&
	           m_MultiplayerApplyOffered && g_NetMatchService.WasJoinRefusedByALiveMatch() &&
	           g_NetMatchService.GetReconnectUx().GetOffer() != NetReconnectOffer::Available &&
	           !g_NetMatchService.GetReconnectUx().IsActive()) {
		// §9b: ask the host for a seat whose holder is gone. The host picks which one.
		ApplyToSubstitute();
		g_GUISound.ButtonPressSound()->Play();
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
		m_MultiplayerApplyOffered = false;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerModerateButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Moderation;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerModerationBackButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MultiplayerLanGamesList) {
		// Clicking a listed host fills the join fields; Connect stays the explicit action. A row the
		// merge marked non-joinable is refused here, before any connection is attempted.
		const int selected = m_MultiplayerLanGamesList->GetSelectedIndex();
		if (selected >= 0 && static_cast<size_t>(selected) < m_GameRows.size()) {
			const NetDirectoryClient::GameRow& row = m_GameRows[static_cast<size_t>(selected)];
			if (!row.joinable) {
				if (m_MultiplayerLanGamesLabel) {
					m_MultiplayerLanGamesLabel->SetText("Cannot join this game: " + row.reason);
				}
				g_GUISound.BackButtonPressSound()->Play();
			} else {
				m_MultiplayerJoinAddressTextBox->SetText(row.address);
				m_MultiplayerJoinPortTextBox->SetText(std::to_string(row.port));
				if (m_MultiplayerLanGamesLabel) {
					m_MultiplayerLanGamesLabel->SetText(m_LanGamesLabelText);
				}
				g_GUISound.ItemChangeSound()->Play();
			}
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

void MainMenuGUI::RefreshMultiplayerHostActivities() {
	// The host's picker offers the scripted activities a lockstep match can run, each pinned to the
	// module that defines it so a same-named preset elsewhere cannot swap in silently. The presets do
	// not exist until the modules load, so the list is built on entry to the host setup screen.
	const std::pair<std::string, std::string> current =
		m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size() ? m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex] : std::pair<std::string, std::string>{"P4 Alpha Duel", "Base.rte"};
	m_MultiplayerHostActivities.clear();
	std::list<Entity*> presets;
	if (g_PresetMan.GetAllOfType(presets, "Activity")) {
		for (const Entity* entity: presets) {
			const auto* activity = dynamic_cast<const Activity*>(entity);
			if (!activity || activity->GetClassName() != "GAScripted" || activity->IsTestActivity()) continue;
			m_MultiplayerHostActivities.emplace_back(activity->GetPresetName(), g_PresetMan.GetDataModuleName(activity->GetModuleID()));
		}
	}
	m_MultiplayerHostActivityIndex = 0;
	for (size_t i = 0; i < m_MultiplayerHostActivities.size(); ++i) {
		if (m_MultiplayerHostActivities[i] == current) {
			m_MultiplayerHostActivityIndex = i;
		}
	}
	if (m_MultiplayerHostActivityCombo) {
		m_MultiplayerHostActivityCombo->ClearList();
		for (const auto& [preset, module] : m_MultiplayerHostActivities) {
			m_MultiplayerHostActivityCombo->AddItem(preset + (module.empty() ? "" : " - " + module));
		}
	}
	ApplyMultiplayerHostActivity();
}

void MainMenuGUI::ApplyMultiplayerHostActivity() {
	const auto* picked = m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size()
	                         ? &m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex] : nullptr;
	const std::string preset = picked ? picked->first : "P4 Alpha Duel";
	const std::string module = picked ? picked->second : "Base.rte";
	if (m_MultiplayerHostActivityCombo) {
		if (picked) {
			m_MultiplayerHostActivityCombo->SetSelectedIndex(static_cast<int>(m_MultiplayerHostActivityIndex));
		} else {
			// No preset enumerated yet: the closed combo still names what the request would send.
			m_MultiplayerHostActivityCombo->SetText(preset + (module.empty() ? "" : " - " + module));
		}
	}
	if (m_MultiplayerHostInfoLabel) {
		m_MultiplayerHostInfoLabel->SetText(std::string("Grasslands - ") + NetMatchConfigUtil::ModeLabel(m_MultiplayerHostMode));
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
	// Hosting or joining under a name saves it, but saving is best effort: the wire carries more
	// bytes than the box takes typed, so a name the settings will not hold still goes out as typed.
	const std::string typedName = m_MultiplayerNameTextBox->GetText();
	// The box's typed cap is shorter than the wire's, but a pasted or scripted name skips it and a
	// name past the hello's byte cap only fails inside the encode; refuse it here in the player's words.
	if (typedName.size() > NetProtocol::c_MaxDisplayNameBytes) {
		m_MultiplayerLandingStatusLabel->SetText("Display names are limited to 64 bytes.");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		return;
	}
	request.playerName = typedName.empty() ? (host ? "Host" : "Client") : typedName;
	if (!typedName.empty() && typedName != g_SettingsMan.GetNetworkDisplayName()) {
		g_SettingsMan.SetNetworkDisplayName(typedName);
		g_SettingsMan.UpdateSettingsFile();
	}
	request.activityPreset = "P4 Alpha Duel";
	if (host && m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size()) {
		// The host's picker names both fields, so the lobby never resolves a bare preset name.
		request.activityPreset = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].first;
		request.activityModule = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].second;
	}
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
		// The saved policy decides it: automatic keeps the saved delay as the floor and raises it to
		// cover the measured ping; fixed hosts on the box's value, which writes back as the new floor.
		request.autoInputDelay = g_SettingsMan.GetNetworkHostDelayPolicy() == SettingsMan::NetworkHostDelayPolicy::Auto;
		if (request.autoInputDelay) {
			request.inputDelayFrames = static_cast<uint16_t>(std::clamp(g_SettingsMan.GetNetworkInputDelayFrames(), 0, static_cast<int>(NetMatchConfigUtil::c_MaxInputDelayFrames)));
		} else {
			const long parsedDelay = std::strtol(m_MultiplayerHostInputDelayTextBox->GetText().c_str(), nullptr, 10);
			const int inputDelay = std::clamp<int>(static_cast<int>(parsedDelay), 0, NetMatchConfigUtil::c_MaxInputDelayFrames);
			m_MultiplayerHostInputDelayTextBox->SetText(std::to_string(inputDelay));
			g_SettingsMan.SetNetworkInputDelayFrames(inputDelay);
			request.inputDelayFrames = static_cast<uint16_t>(inputDelay);
		}
	}

	std::string error;
	m_MultiplayerJoinRequest = request;
	m_MultiplayerApplyOffered = !host;
	if (g_NetMatchService.Start(request, &error)) {
		m_MultiplayerLandingStatusLabel->SetText("");
		m_ReconnectStatusShown.clear();
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else {
		m_MultiplayerLandingStatusLabel->SetText(error);
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
	}
	g_GUISound.ButtonPressSound()->Play();
}

void MainMenuGUI::ApplyToSubstitute() {
	std::string error;
	m_MultiplayerApplyOffered = false;
	if (g_NetMatchService.BeginSubstituteApplication(m_MultiplayerJoinRequest, &error)) {
		m_MultiplayerLandingStatusLabel->SetText("Asking the host for a seat...");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else {
		m_MultiplayerLandingStatusLabel->SetText(error);
	}
	m_ReconnectStatusShown = m_MultiplayerLandingStatusLabel->GetText();
}

// The bitmap font draws the wire delimiter's UTF-8 bytes as icon glyphs, so the GUI shows "; ".
static std::string GroupDelimiterForDisplay(const std::string& text) {
	const std::string separator = " \xC2\xB7 ";
	std::string display = text;
	for (size_t at = display.find(separator); at != std::string::npos; at = display.find(separator, at + 2)) {
		display.replace(at, separator.size(), "; ");
	}
	return display;
}

// The refusal sentence arrives as one line; the landing label shows its groups as a short list.
static std::string FormatModuleMismatchStatus(const std::string& text) {
	const std::string prefix = "This host's mods do not match yours.";
	if (text.compare(0, prefix.size(), prefix) != 0) {
		return text;
	}
	const std::string separator = " \xC2\xB7 ";
	std::string rest = text.substr(prefix.size());
	if (!rest.empty() && rest.front() != ' ') {
		return text;
	}
	if (rest.starts_with(' ')) {
		rest.erase(0, 1);
	}
	if (rest.starts_with("\xC2\xB7 ")) {
		rest.erase(0, 3);
	}
	std::string install;
	std::vector<std::string> others;
	for (size_t at = 0; at <= rest.size();) {
		const size_t next = rest.find(separator, at);
		std::string piece = next == std::string::npos ? rest.substr(at) : rest.substr(at, next - at);
		if (install.empty() && piece.starts_with("Install: ")) {
			install = std::move(piece);
		} else if (!piece.empty()) {
			others.push_back(std::move(piece));
		}
		if (next == std::string::npos) {
			break;
		}
		at = next + separator.size();
	}
	std::string formatted = prefix;
	if (!install.empty()) {
		formatted += '\n' + install;
	}
	if (!others.empty()) {
		formatted += '\n';
		for (size_t i = 0; i < others.size(); ++i) {
			formatted += (i == 0 ? "" : "; ") + others[i];
		}
	}
	return formatted;
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
		m_MultiplayerLandingStatusLabel->SetText(GroupDelimiterForDisplay(FormatModuleMismatchStatus(snapshot.errorText)));
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
	// §9b: the one refusal a joiner can answer. The same two buttons carry it, so the landing panel
	// keeps one pair of controls whatever it is offering.
	const bool applying = !recovering && !offering && m_MultiplayerApplyOffered && g_NetMatchService.WasJoinRefusedByALiveMatch();
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetVisible(landing && (offering || applying || reconnect.CanRetryManually()));
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetEnabled(offering || applying || reconnect.CanRetryManually());
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetText(offering ? "Rejoin Match" : (applying ? "Apply to Substitute" : "Retry"));
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->SetVisible(landing && (offering || applying || recovering));
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->SetEnabled(offering || applying || reconnect.CanCancel());
	if (!landing) {
		return;
	}
	if (applying) {
		const std::string offer = "The match is already in progress. Apply to substitute for a dropped player?";
		if (offer != m_ReconnectStatusShown) {
			m_MultiplayerLandingStatusLabel->SetText(offer);
			m_ReconnectStatusShown = offer;
		}
		return;
	}
	// One persistent line, never a toast: the status while recovering, otherwise whatever the startup
	// scan of the recovery record found - including precisely why it cannot be used. No record is the
	// absence of an offer, not a verdict: it stays silent until the player asks to rejoin.
	const std::string status = recovering ? reconnect.GetStatusText()
	                                      : (reconnect.GetOffer() == NetReconnectOffer::Missing ? std::string() : reconnect.GetOfferText());
	if (status != m_ReconnectStatusShown) {
		// A recovery in progress owns the line. What the scan of the record found does not: it clears
		// its own sentence, but never replaces a refusal or an error the screen just put there.
		if (recovering || m_MultiplayerLandingStatusLabel->GetText() == m_ReconnectStatusShown) {
			m_MultiplayerLandingStatusLabel->SetText(status);
		}
		m_ReconnectStatusShown = status;
	}
}

void MainMenuGUI::RefreshHostInputDelayControls() {
	const bool automatic = g_SettingsMan.GetNetworkHostDelayPolicy() == SettingsMan::NetworkHostDelayPolicy::Auto;
	const int frames = std::clamp(g_SettingsMan.GetNetworkInputDelayFrames(), 0, static_cast<int>(NetMatchConfigUtil::c_MaxInputDelayFrames));
	// Fixed pre-fills the saved frames as the per-match override; under automatic the box only
	// announces, so it goes grey with the word the label already spells.
	m_MultiplayerHostInputDelayTextBox->SetEnabled(!automatic);
	m_MultiplayerHostInputDelayTextBox->SetText(automatic ? "auto" : std::to_string(frames));
	m_MultiplayerHostInputDelayPolicyLabel->SetText(automatic ? "(auto)" : "(fixed, " + std::to_string(frames) + ")");
}

void MainMenuGUI::FitMultiplayerPanelWidth(GUICollectionBox* panel, GUILabel* diagnosticLabel, int width, const std::vector<GUILabel*>& fillLabels) {
	const int oldWidth = panel->GetWidth();
	if (oldWidth == width) {
		return;
	}
	// width/2 - oldWidth/2 telescopes; (width - oldWidth)/2 would drop half a pixel per parity step.
	const int shift = width / 2 - oldWidth / 2;
	for (GUIControl* control : *panel->GetChildren()) {
		GUIPanel* child = control->GetPanel();
		if (!child) {
			continue;
		}
		if (child == diagnosticLabel || std::find(fillLabels.begin(), fillLabels.end(), child) != fillLabels.end()) {
			child->SetPositionRel(12, child->GetRelYPos());
			control->Resize(width - 24, child->GetHeight());
		} else {
			child->SetPositionRel(child->GetRelXPos() + shift, child->GetRelYPos());
		}
	}
	panel->Resize(width, panel->GetHeight());
}

void MainMenuGUI::FitMultiplayerScreen(int width, int height) {
	GUICollectionBox* screen = m_MainMenuScreens[MenuScreen::MultiplayerScreen];
	if (screen->GetWidth() != width || screen->GetHeight() != height) {
		screen->Resize(width, height);
	}
	const int screenX = (m_RootBoxMaxWidth - width) / 2;
	// The screen floats centred; a panel taller than the viewport clamps to the top edge instead.
	const int screenY = std::max(0, (g_WindowMan.GetResY() - height) / 2);
	if (screen->GetRelXPos() != screenX || screen->GetRelYPos() != screenY) {
		screen->SetPositionRel(screenX, screenY);
	}
}

void MainMenuGUI::LayoutMultiplayerFooter(int width, int y) {
	GUIButton* back = m_MainMenuButtons[MenuButton::BackToMainButton];
	GUIButton* diagnostics = m_MainMenuButtons[MenuButton::SaveDiagnosticsButton];
	const int left = (width - back->GetWidth() - diagnostics->GetWidth() - 8) / 2;
	back->SetPositionRel(left, y);
	diagnostics->SetPositionRel(left + back->GetWidth() + 8, y);
}

void MainMenuGUI::RefreshMultiplayerScreenControls(const NetLobbySnapshot& snapshot) {
	const bool savingDiagnostics = TelemetryBundle::IsBusy();
	m_MainMenuButtons[MenuButton::SaveDiagnosticsButton]->SetEnabled(!savingDiagnostics);
	m_MainMenuButtons[MenuButton::SaveDiagnosticsButton]->SetText(savingDiagnostics ? "Saving..." : "Save Diagnostics");
	const bool lobby = m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby;
	const auto summary = g_NetMatchService.GetLastMatchSummary();
	m_LastMatchSummaryLabel->SetText(summary ? summary->LineText() : "");
	m_LastMatchSummaryLabel->SetVisible(lobby && summary.has_value());
	m_MainMenuButtons[MenuButton::LastMatchDetailsButton]->SetVisible(lobby && summary.has_value());
	m_MainMenuButtons[MenuButton::LastMatchDetailsButton]->SetEnabled(summary.has_value());
	if (!summary) m_LastMatchDetailsLabel->SetText("");
	m_MultiplayerLandingPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::Landing);
	m_MultiplayerHostPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::HostSetup);
	m_MultiplayerJoinPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::JoinSetup);
	m_MultiplayerLobbyPanel->SetVisible(lobby);
	const bool moderating = m_MultiplayerSubScreen == MultiplayerSubScreen::Moderation;
	m_MultiplayerModerationPanel->SetVisible(moderating);
	m_ReplayBrowserPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::ReplayBrowser);
	RefreshGamesList();
	RefreshReconnectControls();
	if (moderating) {
		RefreshModerationControls(snapshot);
	}
	if (!lobby) {
		for (GUILabel* label : m_MultiplayerLobbyChatLabels) {
			if (label) label->SetVisible(false);
		}
		if (m_MultiplayerLobbyChatInput) {
			m_MultiplayerLobbyChatInput->SetVisible(false);
		}
		if (m_MultiplayerSubScreen == MultiplayerSubScreen::ReplayBrowser) {
			RefreshReplayBrowserControls();
			return;
		}
		int contentWidth = 300;
		int contentHeight = 250;
		if (m_MultiplayerSubScreen == MultiplayerSubScreen::Landing) {
			// FontLarge's atlas has a few width-only blank cells (e.g. 0xDF); FontSmall's ink draws those bytes.
			m_MultiplayerLandingStatusLabel->EnsureDrawableTextFont("FontSmall.png");
			// The status wraps only on spaces; a token wider than the viewport scrolls through the label instead of growing the panel off-screen.
			const int desiredWidth = std::max(300, m_MultiplayerLandingStatusLabel->GetMaxWordWidth() + 24);
			contentWidth = std::min(desiredWidth, m_RootBoxMaxWidth - 12);
			FitMultiplayerPanelWidth(m_MultiplayerLandingPanel, m_MultiplayerLandingStatusLabel, contentWidth);
			// The status can outgrow its baseline box; it yields to whatever height the viewport leaves it.
			const int labelRelY = m_MultiplayerLandingStatusLabel->GetRelYPos();
			const int bottomPad = 250 - labelRelY - 68;
			const int backReserve = m_MainMenuButtons[MenuButton::BackToMainButton]->GetHeight() + 5;
			const int statusRoom = std::max(0, g_WindowMan.GetResY() - backReserve - labelRelY - bottomPad);
			// An unbreakable token scrolls horizontally; otherwise a too-tall status scrolls vertically.
			const bool scrollWide = desiredWidth > contentWidth;
			m_MultiplayerLandingStatusLabel->SetHorizontalOverflowScroll(scrollWide);
			const int statusHeight = std::max(10, std::min(std::max(68, m_MultiplayerLandingStatusLabel->GetTextHeight() + 4), statusRoom));
			const bool scrollTall = !scrollWide && m_MultiplayerLandingStatusLabel->GetTextHeight() + 4 > statusRoom;
			m_MultiplayerLandingStatusLabel->SetVerticalOverflowScroll(scrollTall);
			m_MultiplayerLandingStatusLabel->ActivateDeactivateOverflowScroll(scrollWide || scrollTall);
			if (m_MultiplayerLandingStatusLabel->GetHeight() != statusHeight) {
				m_MultiplayerLandingStatusLabel->Resize(m_MultiplayerLandingStatusLabel->GetWidth(), statusHeight);
			}
			const int panelHeight = std::max(250, labelRelY + statusHeight + bottomPad);
			if (m_MultiplayerLandingPanel->GetHeight() != panelHeight) {
				m_MultiplayerLandingPanel->Resize(contentWidth, panelHeight);
			}
			contentHeight = panelHeight;
		}
		const int screenHeight = contentHeight + m_MainMenuButtons[MenuButton::BackToMainButton]->GetHeight() + 5;
		FitMultiplayerScreen(contentWidth, screenHeight);
		LayoutMultiplayerFooter(contentWidth, contentHeight);
		return;
	}

	// The match header is two fixed rows: the activity with its defining module, then the scene
	// and the mode's menu label. A joiner's placeholder shows the same rows with what it has.
	std::string matchInfo = snapshot.activityPreset;
	if (!snapshot.activityModule.empty()) {
		matchInfo += matchInfo.empty() ? snapshot.activityModule : " - " + snapshot.activityModule;
	}
	m_MultiplayerLobbyMatchLabel->SetText(matchInfo);
	std::string matchMode = snapshot.sceneName;
	if (!snapshot.modeLabel.empty()) {
		matchMode += matchMode.empty() ? snapshot.modeLabel : " - " + snapshot.modeLabel;
	}
	if (m_MultiplayerLobbyMatchModeLabel) {
		m_MultiplayerLobbyMatchModeLabel->SetText(matchMode);
	}
	std::array<std::string, 4> lobbyRowName;
	std::array<std::string, 4> lobbyRowTailFull;
	std::array<std::string, 4> lobbyRowTailMarked;
	std::array<std::string, 4> lobbyRowTailBare;
	for (size_t i = 0; i < m_MultiplayerLobbyPlayerLabels.size(); ++i) {
		GUILabel* label = m_MultiplayerLobbyPlayerLabels[i];
		if (i >= snapshot.members.size()) {
			label->SetText("");
			label->SetVisible(false);
			continue;
		}
		const NetLobbyMember& member = snapshot.members[i];
		// The seat line is the verbose form of the seat mark; the row keeps whichever fits.
		const std::string seatMark = std::string(NetReconnectUx::RosterMark(member.dropped, member.reclaiming));
		const auto buildTail = [&member, &snapshot](const std::string& seat, bool withDelay = true) {
			std::string tail = member.isLocal ? " (you)" : "";
			tail += " - Team " + std::to_string(member.team + 1);
			tail += member.peerId == 1 ? " - Host" : (member.ready ? " - Ready" : " - Not ready");
			tail += seat;
			if (withDelay && member.isLocal && !snapshot.inputDelayText.empty()) {
				tail += " - " + snapshot.inputDelayText;
			}
			if (!member.isLocal && member.connected) {
				tail += " - ";
				tail += NetConnectionQualityName(ClassifyConnectionQuality(member.pingMs));
				if (member.pingMs > 0) {
					tail += " (" + std::to_string(member.pingMs) + "ms)";
				}
			}
			return tail;
		};
		lobbyRowName[i] = member.displayName;
		lobbyRowTailFull[i] = buildTail(member.statusLine.empty() ? seatMark : " - " + member.statusLine);
		lobbyRowTailMarked[i] = buildTail(seatMark);
		// The delay's own tail is the rung after the seat's: a row still too long sheds it next.
		lobbyRowTailBare[i] = buildTail(seatMark, false);
		label->SetVisible(true);
	}
	static std::string s_shareAddress;
	static bool s_shareResolved = false;
	// The host's join and ready-up hints belong to the lobby it opened itself; a lobby that follows
	// a played match already carries its own line - the result and the rematch offer - like the client's.
	if (snapshot.isHost && snapshot.inLobby && !snapshot.remoteReady && !snapshot.playedAMatch) {
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
	// The lobby panel is one width on every peer: a longer row or status elides inside its box
	// rather than widening the panel, so host and client land on the same rectangle.
	const int contentWidth = 300;
	const int rowBoxWidth = contentWidth - 24;
	if (m_MultiplayerLobbyPlayerRowFont) {
		std::string statusText = m_MultiplayerStatusLabel->GetText();
		while (!statusText.empty() &&
		       m_MultiplayerLobbyPlayerRowFont->CalculateWidth(statusText + "...", m_MultiplayerLobbyPlayerRowFallbackFont) > rowBoxWidth) {
			statusText.pop_back();
		}
		if (statusText != m_MultiplayerStatusLabel->GetText()) {
			m_MultiplayerStatusLabel->SetText(statusText + "...");
		}
	}
	// The status line can wrap; reserve its real height and let everything below slide with it.
	const int statusHeight = std::max(16, m_MultiplayerStatusLabel->GetTextHeight() + 4);
	if (m_MultiplayerStatusLabel->GetHeight() != statusHeight) {
		m_MultiplayerStatusLabel->Resize(m_MultiplayerStatusLabel->GetWidth(), statusHeight);
	}
	const int statusExtra = statusHeight - 16;
	if (snapshot.portMapSerial != m_PortMapSerialShown) {
		m_PortMapSerialShown = snapshot.portMapSerial;
		m_MultiplayerLobbyPortMapLabel->SetText(snapshot.portMap);
	}
	m_MultiplayerLobbyPortMapLabel->SetVisible(!snapshot.portMap.empty());
	const int summaryHeight = summary ? 20 : 0;
	m_LastMatchSummaryLabel->SetPositionRel(12, 178 + statusExtra);
	m_LastMatchSummaryLabel->Resize(contentWidth - 90, 18);
	m_MainMenuButtons[MenuButton::LastMatchDetailsButton]->SetPositionRel(contentWidth - 74, 178 + statusExtra);
	m_MultiplayerLobbyPortMapLabel->SetPositionRel(12, 178 + statusExtra + summaryHeight);
	const int portMapHeight = snapshot.portMap.empty() ? 0 : 14;
	m_MultiplayerErrorLabel->SetText(GroupDelimiterForDisplay(snapshot.errorText));
	m_MultiplayerErrorLabel->EnsureDrawableTextFont("FontSmall.png");
	const int desiredWidth = std::max(300, m_MultiplayerErrorLabel->GetMaxWordWidth() + 24);
	FitMultiplayerPanelWidth(m_MultiplayerLobbyPanel, m_MultiplayerErrorLabel, contentWidth, {});
	for (size_t i = 0; i < lobbyRowName.size(); ++i) {
		GUILabel* label = m_MultiplayerLobbyPlayerLabels[i];
		if (!label || lobbyRowTailFull[i].empty()) {
			continue;
		}
		const auto fitRow = [this, rowBoxWidth](const std::string& name, const std::string& tailFull, const std::string& tailMarked, const std::string& tailBare) {
			const auto elide = [this, rowBoxWidth](const std::string& name, const std::string& tail) {
				if (!m_MultiplayerLobbyPlayerRowFont || m_MultiplayerLobbyPlayerRowFont->CalculateWidth(name + tail, m_MultiplayerLobbyPlayerRowFallbackFont) <= rowBoxWidth) {
					return name + tail;
				}
				std::string trimmed = name;
				while (!trimmed.empty() && m_MultiplayerLobbyPlayerRowFont->CalculateWidth(trimmed + "..." + tail, m_MultiplayerLobbyPlayerRowFallbackFont) > rowBoxWidth) {
					trimmed.pop_back();
				}
				return trimmed + "..." + tail;
			};
			std::string row = elide(name, tailFull);
			const auto tooLong = [this, rowBoxWidth](const std::string& text) {
				return m_MultiplayerLobbyPlayerRowFont && m_MultiplayerLobbyPlayerRowFont->CalculateWidth(text, m_MultiplayerLobbyPlayerRowFallbackFont) > rowBoxWidth;
			};
			if (tooLong(row)) {
				row = elide(name, tailMarked);
			}
			// A tail still too long sheds the delay's rung before the row wraps.
			if (tooLong(row)) {
				row = elide(name, tailBare);
			}
			return row;
		};
		label->SetText(fitRow(lobbyRowName[i], lobbyRowTailFull[i], lobbyRowTailMarked[i], lobbyRowTailBare[i]));
		label->EnsureDrawableTextFont("FontSmall.png");
	}
	// The port-map row sits under the wrapped status, so the error starts below both.
	m_MultiplayerErrorLabel->SetPositionRel(12, 178 + statusExtra + portMapHeight + summaryHeight);
	// The screen must stay inside the viewport. Error, status and port-map rows keep every pixel
	// their room allows (overflow scrolls); the chat block is the one piece that yields - a row at
	// a time - before any of them do.
	const int backReserve = m_MainMenuButtons[MenuButton::BackToMainButton]->GetHeight() + 5;
	const int fixedExtra = statusExtra + portMapHeight + summaryHeight;
	const int panelCap = g_WindowMan.GetResY() - 24; // the Back button's band sits under the panel
	const int inputBlock = 25;                     // textbox 13 px + a bottom margin matching its sides
	// The Leave/Seats row ends at rel 240; the first chat row keeps a 4px gap under it and the
	// error block must not reach into that band.
	const int c_LobbyChatTop = 261;
	// Same accessibility rule as the landing status: wide token scrolls horizontally, tall text
	// vertically. The chat input row always stays, so the error's room never reaches into it.
	const int errorRoom = std::min(
	    std::max(24, g_WindowMan.GetResY() - backReserve - 266 + 24 - fixedExtra),
	    std::max(0, panelCap - (c_LobbyChatTop - 4) - fixedExtra - inputBlock));
	const bool scrollWide = desiredWidth > contentWidth;
	m_MultiplayerErrorLabel->SetHorizontalOverflowScroll(scrollWide);
	// A finished round uses the vacant error band before taking room from chat.
	const int errorHeight = summary ? (snapshot.errorText.empty() ? 0 : std::min(std::max(24, m_MultiplayerErrorLabel->GetTextHeight() + 4), errorRoom))
	                                : std::max(24, std::min(m_MultiplayerErrorLabel->GetTextHeight() + 4, errorRoom));
	m_MultiplayerErrorLabel->SetVisible(errorHeight > 0);
	const bool scrollTall = !scrollWide && m_MultiplayerErrorLabel->GetTextHeight() + 4 > errorRoom;
	m_MultiplayerErrorLabel->SetVerticalOverflowScroll(scrollTall);
	m_MultiplayerErrorLabel->ActivateDeactivateOverflowScroll(scrollWide || scrollTall);
	const int extraHeight = fixedExtra + errorHeight - 24;
	if (m_MultiplayerErrorLabel->GetHeight() != std::max(1, errorHeight)) {
		m_MultiplayerErrorLabel->Resize(m_MultiplayerErrorLabel->GetWidth(), std::max(1, errorHeight));
	}
	// A shrunken box still reads top-down: Middle would anchor a tall error on its middle lines.
	m_MultiplayerErrorLabel->SetVAlignment(
	    m_MultiplayerErrorLabel->GetTextHeight() > errorHeight ? GUIFont::Top : GUIFont::Middle);
	const int chatTop = c_LobbyChatTop + extraHeight;
	const int chatRows = std::min<int>(m_MultiplayerLobbyChatLabels.size(),
	                                   std::max(0, (panelCap - chatTop - inputBlock) / 10));
	const int contentHeight = chatTop + chatRows * 10 + inputBlock;

	// The newest chatRows lines, oldest on top; the entry box takes Enter for All, Ctrl+Enter for
	// Team. Team lines indent two cells as well as carrying their [team] mark.
	for (const NetChatEntry& entry : g_NetMatchService.TakeChatEntries()) {
		std::string name = entry.senderName;
		if (name.empty()) {
			for (const NetLobbyMember& member : snapshot.members) {
				if (member.peerId == entry.senderPeerId + 1) {
					name = member.displayName;
					break;
				}
			}
		}
		if (name.empty()) {
			name = "Player " + std::to_string(entry.senderPeerId + 1);
		}
		m_MultiplayerLobbyChatLines.push_back(std::string(entry.scope == c_NetChatScopeTeam ? "  [team] " : "") + name + ": " + entry.text);
		while (m_MultiplayerLobbyChatLines.size() > m_MultiplayerLobbyChatLabels.size()) {
			m_MultiplayerLobbyChatLines.pop_front();
		}
	}
	// Lines sit bottom-aligned above the input: the newest line is always the lowest drawn row.
	const size_t chatOffset = m_MultiplayerLobbyChatLines.size() > static_cast<size_t>(chatRows)
	                              ? m_MultiplayerLobbyChatLines.size() - chatRows : 0;
	const size_t firstLineRow = m_MultiplayerLobbyChatLines.size() - chatOffset < static_cast<size_t>(chatRows)
	                                ? static_cast<size_t>(chatRows) - (m_MultiplayerLobbyChatLines.size() - chatOffset) : 0;
	// Chat follows the player rows' convention: X=8, their ini spot, at the fixed panel width.
	const int chatX = 8;
	const int chatW = contentWidth - 2 * chatX;
	for (size_t row = 0; row < m_MultiplayerLobbyChatLabels.size(); ++row) {
		GUILabel* label = m_MultiplayerLobbyChatLabels[row];
		if (!label) continue;
		const bool drawn = row < static_cast<size_t>(chatRows) && row >= firstLineRow;
		label->SetText(drawn ? m_MultiplayerLobbyChatLines[chatOffset + row - firstLineRow] : "");
		// FontSmall's cells are 10px, so the rows pitch at the measured height, not the label's skin one.
		label->SetPositionRel(chatX, chatTop + static_cast<int>(row) * 10);
		if (label->GetWidth() != chatW) {
			label->Resize(chatW, 10);
		}
		label->SetVisible(row < static_cast<size_t>(chatRows));
	}
	if (m_MultiplayerLobbyChatInput) {
		m_MultiplayerLobbyChatInput->SetPositionRel(chatX, chatTop + chatRows * 10);
		if (m_MultiplayerLobbyChatInput->GetWidth() != chatW) {
			m_MultiplayerLobbyChatInput->Resize(chatW, 13);
		}
		m_MultiplayerLobbyChatInput->SetVisible(true);
	}

	if (m_MultiplayerLobbyPanel->GetHeight() != contentHeight) {
		m_MultiplayerLobbyPanel->Resize(contentWidth, contentHeight);
	}
	const int screenHeight = contentHeight + backReserve;
	FitMultiplayerScreen(contentWidth, screenHeight);
	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetPositionRel(55, 208 + extraHeight);
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetPositionRel(55, 208 + extraHeight);
	// The Leave/Seats pair centres on the panel the way Start does; the gap between them holds parity.
	const int leaveWidth = m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]->GetWidth();
	const int seatsWidth = m_MainMenuButtons[MenuButton::MultiplayerModerateButton]->GetWidth();
	const int pairLeft = (contentWidth - leaveWidth - 2 - seatsWidth) / 2;
	m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]->SetPositionRel(pairLeft, 236 + extraHeight);
	LayoutMultiplayerFooter(contentWidth, contentHeight);

	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetVisible(!snapshot.isHost);
	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetEnabled(!snapshot.isHost && snapshot.inLobby);
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetVisible(snapshot.isHost);
	// Start only once the remote peer is actually ready, not merely present.
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetEnabled(snapshot.isHost && snapshot.inLobby && snapshot.remoteReady);
	m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]->SetEnabled(true);
	// §9b: moderation is a match feature - a lobby seat whose holder leaves goes straight back in the pool.
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton]->SetPositionRel(contentWidth - pairLeft - seatsWidth, 236 + extraHeight);
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

void MainMenuGUI::OpenMultiplayerDialog(GUICollectionBox* dialog, const GUICollectionBox* owner) {
	dialog->SetPositionAbs(owner->GetXPos() + (owner->GetWidth() - dialog->GetWidth()) / 2,
	                       owner->GetYPos() + (owner->GetHeight() - dialog->GetHeight()) / 2);
	m_ActiveDialogBox = dialog;
	m_MainMenuScreens[MenuScreen::MultiplayerScreen]->SetEnabled(false);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetEnabled(false);
	dialog->SetVisible(true);
	dialog->SetEnabled(true);
	dialog->SetZPos(100);
}

void MainMenuGUI::CloseMultiplayerDialog() {
	if (m_ActiveDialogBox) m_ActiveDialogBox->SetVisible(false);
	m_ActiveDialogBox = nullptr;
	m_MainMenuScreens[MenuScreen::MultiplayerScreen]->SetEnabled(true);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetEnabled(true);
	m_ReplayDeletePath.clear();
	if (m_MultiplayerSubScreen == MultiplayerSubScreen::ReplayBrowser) m_ReplayList->SetFocus();
	else m_MainMenuButtons[MenuButton::LastMatchDetailsButton]->SetFocus();
}

void MainMenuGUI::ShowLastMatchDetails() {
	const auto summary = g_NetMatchService.GetLastMatchSummary();
	if (!summary) return;
	const int width = m_MultiplayerLobbyPanel->GetWidth();
	m_LastMatchDetailsLabel->SetText(summary->DetailsText());
	m_LastMatchDetailsLabel->Resize(width - 24, 240);
	const int height = std::min(g_WindowMan.GetResY() - 24, m_LastMatchDetailsLabel->GetTextHeight() + 72);
	m_LastMatchDialog->Resize(width, height);
	m_LastMatchDetailsLabel->Resize(width - 24, height - 72);
	m_LastMatchDetailsLabel->SetVerticalOverflowScroll(true);
	m_LastMatchDetailsLabel->ActivateDeactivateOverflowScroll(true);
	m_MainMenuButtons[MenuButton::LastMatchCloseButton]->SetPositionRel((width - 80) / 2, height - 32);
	OpenMultiplayerDialog(m_LastMatchDialog, m_MultiplayerLobbyPanel);
	m_MainMenuButtons[MenuButton::LastMatchCloseButton]->SetFocus();
}

void MainMenuGUI::RefreshReplayList() {
	namespace fs = std::filesystem;
	const int selected = m_ReplayList->GetSelectedIndex();
	const std::string keep = selected >= 0 && static_cast<size_t>(selected) < m_ReplayRows.size() ? m_ReplayRows[selected].path : "";
	m_ReplayRows.clear();
	m_ReplayList->ClearList();
	std::error_code error;
	const fs::path directory = fs::path("Userdata") / "Replays";
	fs::directory_iterator entries(directory, error);
	if (error) {
		m_ReplayStatusLabel->SetText(error == std::errc::no_such_file_or_directory ? "No replays in Userdata/Replays." : "Could not read Userdata/Replays: " + error.message());
		RefreshReplayBrowserControls();
		return;
	}
	for (const auto end = fs::directory_iterator(); entries != end; entries.increment(error)) {
		if (error) break;
		const auto& entry = *entries;
		std::error_code fileError;
		if (fs::is_symlink(entry.symlink_status(fileError)) || !entry.is_regular_file(fileError) || fileError) continue;
		std::string extension = entry.path().extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (extension != ".ccreplay") continue;
		ReplayRow row;
		row.path = entry.path().string();
		row.text = entry.path().filename().string();
		const auto written = entry.last_write_time(fileError);
		if (!fileError) {
#ifdef _MSC_VER
			const auto convertedTime = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
			const auto convertedTime = fs::file_time_type::clock::to_sys(written);
#endif
			const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(convertedTime);
			const std::time_t time = std::chrono::system_clock::to_time_t(systemTime) - 7 * 60 * 60;
			std::tm local{};
#ifdef _WIN32
			gmtime_s(&local, &time);
#else
			gmtime_r(&time, &local);
#endif
			std::ostringstream date;
			date << std::put_time(&local, "%Y-%m-%d %H:%M MST");
			row.text += " | " + date.str();
		}
		NetMatchReplayReader reader;
		if (reader.Open(row.path, &row.error)) {
			const auto& config = reader.GetConfig();
			row.text += " | " + config.activityPreset + " / " + config.sceneName + " | " + std::to_string(config.peerCount) + " peers";
			reader.Close();
			NetReplayVerifyReport scan;
			if (NetMatchReplayReader::Verify(row.path, scan)) {
				NetMatchSummary duration;
				duration.runningTicks = scan.lastFrame;
				row.text += " | " + duration.DurationText();
			} else {
				row.error = scan.error;
			}
		}
		if (!row.error.empty()) row.text += " | Unavailable: " + row.error;
		m_ReplayRows.push_back(std::move(row));
	}
	std::sort(m_ReplayRows.begin(), m_ReplayRows.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
	m_ReplayList->BeginUpdate();
	int restore = m_ReplayRows.empty() ? -1 : 0;
	for (size_t i = 0; i < m_ReplayRows.size(); ++i) {
		m_ReplayList->AddItem(m_ReplayRows[i].text);
		if (m_ReplayRows[i].path == keep) restore = static_cast<int>(i);
	}
	m_ReplayList->EndUpdate();
	m_ReplayList->SetSelectedIndex(restore);
	m_ReplayStatusLabel->SetText(error ? "Replay listing stopped: " + error.message() : std::to_string(m_ReplayRows.size()) + " replays in Userdata/Replays");
	RefreshReplayBrowserControls();
}

void MainMenuGUI::RefreshReplayBrowserControls() {
	const int width = std::min(600, m_RootBoxMaxWidth - 24);
	constexpr int height = 360 - 24; // The lobby's minimum-viewport budget leaves a band for Back.
	const int selected = m_ReplayList->GetSelectedIndex();
	const bool hasSelection = selected >= 0 && static_cast<size_t>(selected) < m_ReplayRows.size();
	if (m_ReplayBrowserPanel->GetWidth() != width || m_ReplayBrowserPanel->GetHeight() != height) m_ReplayBrowserPanel->Resize(width, height);
	if (m_ReplayList->GetWidth() != width - 24 || m_ReplayList->GetHeight() != height - 146) {
		m_ReplayList->Resize(width - 24, height - 146);
	}
	m_ReplaySelectedLabel->SetPositionRel(12, height - 92);
	m_ReplaySelectedLabel->Resize(width - 24, 42);
	m_ReplaySelectedLabel->SetText(hasSelection ? m_ReplayRows[selected].text : "Select a replay to play or delete.");
	m_ReplaySelectedLabel->SetVerticalOverflowScroll(true);
	m_ReplaySelectedLabel->ActivateDeactivateOverflowScroll(true);
	m_ReplayStatusLabel->Resize(width - 24, 16);
	m_ReplayStatusLabel->ActivateDeactivateOverflowScroll(true);
	m_MainMenuButtons[MenuButton::ReplayPlayButton]->SetEnabled(hasSelection && m_ReplayRows[selected].error.empty());
	m_MainMenuButtons[MenuButton::ReplayDeleteButton]->SetEnabled(hasSelection);
	m_MainMenuButtons[MenuButton::ReplayPlayButton]->SetPositionRel(12, height - 34);
	m_MainMenuButtons[MenuButton::ReplayDeleteButton]->SetPositionRel(104, height - 34);
	m_MainMenuButtons[MenuButton::ReplayBackButton]->SetPositionRel(width - 92, height - 34);
	const int backHeight = m_MainMenuButtons[MenuButton::BackToMainButton]->GetHeight();
	FitMultiplayerScreen(width, height + backHeight + 5);
	m_MainMenuButtons[MenuButton::BackToMainButton]->SetPositionRel((width - m_MainMenuButtons[MenuButton::BackToMainButton]->GetWidth()) / 2, height);
}

void MainMenuGUI::PlaySelectedReplay() {
	const int selected = m_ReplayList->GetSelectedIndex();
	if (selected < 0 || static_cast<size_t>(selected) >= m_ReplayRows.size() || !m_ReplayRows[selected].error.empty()) return;
	std::string error;
	if (!StartNetReplayPlayback(m_ReplayRows[selected].path, true, &error)) {
		m_ReplayStatusLabel->SetText(error);
		return;
	}
	m_UpdateResult = MainMenuUpdateResult::ActivityStarted;
	SetActiveMenuScreen(MenuScreen::MainScreen, false);
}

void MainMenuGUI::ReturnToReplayBrowser(const std::string& status) {
	SetActiveMenuScreen(MenuScreen::MultiplayerScreen, false);
	ShowMultiplayerScreen();
	m_MultiplayerSubScreen = MultiplayerSubScreen::ReplayBrowser;
	RefreshReplayList();
	m_ReplayStatusLabel->SetText(status);
	RefreshMultiplayerScreenControls(g_NetMatchService.GetLobbySnapshot());
	m_ReplayList->SetFocus();
}

void MainMenuGUI::ConfirmReplayDelete() {
	namespace fs = std::filesystem;
	const fs::path path(m_ReplayDeletePath);
	std::error_code error;
	const fs::path directory = fs::weakly_canonical(fs::path("Userdata") / "Replays", error);
	const fs::path parent = error ? fs::path() : fs::weakly_canonical(path.parent_path(), error);
	const auto statusBeforeDelete = error ? fs::file_status() : fs::symlink_status(path, error);
	const bool allowed = !m_ReplayDeletePath.empty() && !error && directory == parent && fs::is_regular_file(statusBeforeDelete);
	const bool removed = allowed && !error && fs::remove(path, error);
	const std::string status = removed ? "Deleted " + path.filename().string() : "Could not delete replay: " + (error ? error.message() : "file unavailable");
	CloseMultiplayerDialog();
	RefreshReplayList();
	m_ReplayStatusLabel->SetText(status);
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

// Follow the panel hierarchy used by drawing and input.
static bool IsControlClickable(GUIControl* control) {
	return MenuAutomation::Enabled(control);
}

bool MainMenuGUI::AutomationActivateControl(const std::string& controlName) {
	if (const auto index = ReplayRowIndex(controlName)) {
		if (*index >= m_ReplayRows.size() || !IsControlClickable(m_ReplayList) || m_ActiveDialogBox) return false;
		m_ReplayList->SetSelectedIndex(static_cast<int>(*index));
		HandleMultiplayerScreenInputEvents(m_ReplayList);
		return true;
	}
	if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) return m_SettingsMenu->AutomationPostCommand(controlName);
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

bool MainMenuGUI::AutomationPostCommand(const std::string& controlName) {
	if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) return m_SettingsMenu->AutomationPostCommand(controlName);
	GUIControl* control = m_SubMenuScreenGUIControlManager->GetControl(controlName);
	if (!control) {
		control = m_MainMenuScreenGUIControlManager->GetControl(controlName);
	}
	if (!control || !IsControlClickable(control)) {
		return false;
	}
	m_PendingAutomationCommand = controlName;
	return true;
}

void MainMenuGUI::PostPendingAutomationCommand() {
	if (m_PendingAutomationCommand.empty()) {
		return;
	}
	GUIControl* control = m_SubMenuScreenGUIControlManager->GetControl(m_PendingAutomationCommand);
	if (!control) {
		control = m_MainMenuScreenGUIControlManager->GetControl(m_PendingAutomationCommand);
	}
	m_PendingAutomationCommand.clear();
	if (control && IsControlClickable(control)) {
		control->AddEvent(GUIEvent::Command, 0, 0);
	}
}

bool MainMenuGUI::AutomationSetText(const std::string& controlName, const std::string& text) {
	GUITextBox* textBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl(controlName));
	if (textBox && IsControlClickable(textBox)) {
		textBox->SetText(text);
		return true;
	}
	return false;
}

bool MainMenuGUI::AutomationSetCheck(const std::string& controlName, bool checked) {
	GUICheckbox* checkbox = dynamic_cast<GUICheckbox*>(m_SubMenuScreenGUIControlManager->GetControl(controlName));
	if (!checkbox) {
		checkbox = dynamic_cast<GUICheckbox*>(m_MainMenuScreenGUIControlManager->GetControl(controlName));
	}
	if (!checkbox || !IsControlClickable(checkbox)) {
		return false;
	}
	checkbox->SetCheck(checked ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	// SetCheck raises no event; the click path is the Changed notification routed to the screen handler.
	if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen) {
		HandleMultiplayerScreenInputEvents(checkbox);
	}
	return true;
}

bool MainMenuGUI::AutomationLabelText(const std::string& controlName, std::string& text) const {
	if (const auto index = ReplayRowIndex(controlName)) {
		if (*index >= m_ReplayRows.size()) return false;
		text = m_ReplayRows[*index].text;
		return true;
	}
	if (auto* manager = AutomationManager()) {
		if (MenuAutomation::Text(manager->GetControl(controlName), text)) return true;
		if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) return false;
	}
	// The chat rows' count moves with the layout budget, so scripts address them by role, not
	// index: LabelLobbyChatNewest is the last drawn row, LabelLobbyChatAny every drawn row's
	// text joined - an assert_label substring hit proves the line reached a rendered row.
	if (controlName == "LabelLobbyChatNewest" || controlName == "LabelLobbyChatAny") {
		// Drawn rows are exactly the labels carrying text: the refresh blanks every row the
		// budget hides, and the top-gap rows never get text.
		std::string joined;
		for (const GUILabel* label : m_MultiplayerLobbyChatLabels) {
			if (label && !label->GetText().empty()) {
				if (controlName == "LabelLobbyChatNewest") {
					text = label->GetText();
				} else {
					if (!joined.empty()) joined += "\n";
					joined += label->GetText();
				}
			}
		}
		if (controlName == "LabelLobbyChatAny") text = joined;
		return !text.empty();
	}
	GUIControl* control = m_SubMenuScreenGUIControlManager->GetControl(controlName);
	if (!control) {
		control = m_MainMenuScreenGUIControlManager->GetControl(controlName);
	}
	if (!control) {
		return false;
	}
	if (const GUILabel* label = dynamic_cast<GUILabel*>(control)) {
		text = label->GetText();
		return true;
	}
	if (const GUIButton* button = dynamic_cast<GUIButton*>(control)) {
		text = button->GetText();
		return true;
	}
	if (const GUICheckbox* checkbox = dynamic_cast<GUICheckbox*>(control)) {
		text = checkbox->GetText();
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
		case MultiplayerSubScreen::ReplayBrowser: return "ReplayBrowser";
		default: return "Unknown";
	}
}

void MainMenuGUI::AutomationGoToMainScreen() {
	SetActiveMenuScreen(MenuScreen::MainScreen, false);
}

bool MainMenuGUI::AutomationControlExists(const std::string& controlName) const {
	if (const auto index = ReplayRowIndex(controlName)) return *index < m_ReplayRows.size();
	if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) return m_SettingsMenu->AutomationManager()->GetControl(controlName) != nullptr;
	return m_SubMenuScreenGUIControlManager->GetControl(controlName) != nullptr ||
	       m_MainMenuScreenGUIControlManager->GetControl(controlName) != nullptr;
}

bool MainMenuGUI::AutomationControlEnabled(const std::string& controlName) const {
	if (const auto index = ReplayRowIndex(controlName)) return *index < m_ReplayRows.size() && IsControlClickable(m_ReplayList);
	if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) return MenuAutomation::Enabled(m_SettingsMenu->AutomationManager()->GetControl(controlName));
	GUIControl* control = m_SubMenuScreenGUIControlManager->GetControl(controlName);
	if (!control) {
		control = m_MainMenuScreenGUIControlManager->GetControl(controlName);
	}
	// "Enabled" for automation means interactable as a human would see it: enabled and visible up the chain.
	return control && IsControlClickable(control);
}

GUIControlManager* MainMenuGUI::AutomationManager() const {
	if (m_ActiveMenuScreen == MenuScreen::SettingsScreen) return m_SettingsMenu->AutomationManager();
	return m_ActiveMenuScreen == MenuScreen::SaveOrLoadGameScreen || m_ActiveMenuScreen == MenuScreen::ModManagerScreen ? nullptr : m_ActiveGUIControlManager;
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

void MainMenuGUI::RefreshGamesList() {
	if (!m_MultiplayerLanGamesList) {
		return;
	}
	// The browser and the directory lister only run while the join screen is up.
	if (m_MultiplayerSubScreen != MultiplayerSubScreen::JoinSetup) {
		if (m_LanBrowser.IsBrowsing()) {
			m_LanBrowser.Stop();
			m_GameRows.clear();
			m_MultiplayerLanGamesList->ClearList();
		}
		m_DirectoryBrowser.StopBrowsing();
		if (m_MultiplayerLanGamesLabel) {
			m_MultiplayerLanGamesLabel->SetText(m_LanGamesLabelText);
		}
		return;
	}
	std::string ignored;
	(void)m_LanBrowser.StartBrowser(&ignored); // idempotent; a failed LAN half still shows NET rows
	// A real monotonic clock, so host expiry holds at any frame rate and while minimized.
	m_LanBrowserNowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	std::vector<NetLanHostInfo> hosts;
	if (m_LanBrowser.IsBrowsing()) {
		m_LanBrowser.Tick(m_LanBrowserNowMs);
		hosts = m_LanBrowser.GetHosts(m_LanBrowserNowMs);
	}
	// The directory half: the browse client issues one GET every c_ListIntervalMs while polled.
	const std::string& directoryUrl = g_SettingsMan.GetSessionDirectoryUrl();
	// Browsing is a directory use: the list GET carries the install key too.
	m_DirectoryBrowser.Configure(directoryUrl, directoryUrl.empty() ? std::string() : g_SettingsMan.GetOrCreateSessionDirectoryInstallKey(), g_SettingsMan.GetSessionDirectoryCertSha256());
	m_DirectoryBrowser.PollList(m_LanBrowserNowMs);
	// A NET row can only be judged against the local identity; build it once, on first need.
	if (!m_DirectoryIdentity && !m_DirectoryIdentityTried) {
		m_DirectoryIdentityTried = true;
		// The row's identity is what NetMatchService::Start computes at the pinned default dt; the
		// menu's own dt comes back after, so browsing never changes a single-player setting.
		const float menuDeltaTime = g_TimerMan.GetDeltaTimeSecs();
		g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);
		NetIdentityManifest manifest;
		NetIdentityBuildOptions identityOptions;
		identityOptions.buildId = "stage2-p2d-local";
		identityOptions.sessionRulesTag = "stage2-p2-session-rules";
		std::string identityError;
		const bool identityBuilt = NetIdentity::BuildCurrentManifest(manifest, &identityError, identityOptions);
		g_TimerMan.SetDeltaTimeSecs(menuDeltaTime);
		if (identityBuilt) {
			NetDirectoryLocalIdentity local;
			local.networkProtocolVersion = manifest.networkProtocolVersion;
			local.lockstepCodecVersion = manifest.deterministicConfig.lockstepCodecVersion;
			local.controllerFrameVersion = manifest.controllerFrameVersion;
			local.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
			local.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
			m_DirectoryIdentity = local;
		}
	}
	std::vector<NetDirectoryClient::GameRow> rows = NetDirectoryClient::MergeGameLists(hosts, m_DirectoryBrowser.Rows(), m_DirectoryIdentity.value_or(NetDirectoryLocalIdentity{}));
	if (!m_DirectoryIdentity) {
		// Without the local identity no NET row can be proven compatible.
		for (NetDirectoryClient::GameRow& row: rows) {
			if (row.source == "NET") {
				row.joinable = false;
				row.reason = "identity";
			}
		}
	}
	const auto describe = [](const NetDirectoryClient::GameRow& row) {
		return "[" + row.source + "] " + row.name + " - " + row.activity + " (" + row.players + ") " + row.address + ":" + std::to_string(row.port) +
		       (row.joinable ? "" : " [" + row.reason + "]");
	};
	bool changed = rows.size() != m_GameRows.size();
	for (size_t i = 0; !changed && i < rows.size(); ++i) {
		changed = describe(rows[i]) != describe(m_GameRows[i]);
	}
	if (!changed) {
		return;
	}
	m_GameRows = std::move(rows);
	m_MultiplayerLanGamesList->ClearList();
	for (const NetDirectoryClient::GameRow& row: m_GameRows) {
		m_MultiplayerLanGamesList->AddItem(describe(row));
	}
}

void MainMenuGUI::MaybeLaunchMultiplayerActivity() {
	std::string activityPreset;
	if (!g_NetMatchService.ConsumeReadyToLaunch(activityPreset)) {
		return;
	}
	m_LastMatchSummaryLabel->SetText("");
	m_LastMatchDetailsLabel->SetText("");
	std::cout << "[menu-mp] report at launch summary=\"" << m_LastMatchSummaryLabel->GetText()
	          << "\" details=\"" << m_LastMatchDetailsLabel->GetText() << "\" retained="
	          << (g_NetMatchService.GetLastMatchSummary().has_value() ? 1 : 0) << std::endl;
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
	// The agreed config the roster carries is the launch descriptor here, on every remote peer and on a
	// dedicated host, so all of them build the identical activity from the identical rules.
	const NetMatchConfig* config = ScenarioRunner::GetLockstepMatchConfig();
	if (!config) {
		m_MultiplayerErrorLabel->SetText("The launching match carries no agreed setup.");
		g_NetMatchService.Destroy();
		return;
	}
	(void)activityPreset; // ConsumeReadyToLaunch already adopted this preset into the roster.
	const int localTeam = g_NetMatchService.GetLocalTeam();
	std::string setupError;
	Activity* activity = NetActivitySetup::CreateConfiguredActivity(*config, localTeam, &setupError);
	if (!activity) {
		m_MultiplayerErrorLabel->SetText(setupError);
		g_NetMatchService.Destroy();
		return;
	}
	ScenarioRunner::ApplyDeterministicConfig();
	g_ActivityMan.SetStartActivity(activity);
	m_UpdateResult = MainMenuUpdateResult::ActivityStarted;
	std::cout << "[menu-mp] launching the match as team " << localTeam << std::endl;
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
		// The menu compositor needs the overlay's alpha as well as its black colour.
		SetTrueAlphaBlender();
		clear_to_color(g_FrameMan.GetOverlayBitmap32(), makeacol32(0, 0, 0, 128));
		draw_trans_sprite(g_FrameMan.GetBackBuffer32(), g_FrameMan.GetOverlayBitmap32(), 0, 0);
		clear_to_color(g_FrameMan.GetOverlayBitmap32(), 0);
		// Whatever this box may be at this point it's already been drawn by the owning GUIControlManager, but we need to draw it again on top of the overlay so it's not affected by it.
		m_ActiveDialogBox->Draw(m_ActiveGUIControlManager->GetScreen());
	}
	m_ActiveGUIControlManager->DrawMouse();
}
