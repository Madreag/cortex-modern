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
#include "System.h"
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
#include "GUIListPanel.h"
#include "GUISlider.h"
#include "GUITab.h"

#include "Resources/Credits.h"

#include <algorithm>
#include <map>
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

// Windows-1252 for one Unicode codepoint; 0xA0-0xFF match Latin-1.
static bool Cp1252FromCodepoint(int codepoint, char& out) {
	if (codepoint < 0) {
		return false;
	}
	if (codepoint < 0x80 || (codepoint >= 0xA0 && codepoint <= 0xFF)) {
		out = static_cast<char>(codepoint);
		return true;
	}
	switch (codepoint) {
		case 0x20AC: out = static_cast<char>(0x80); return true;
		case 0x201A: out = static_cast<char>(0x82); return true;
		case 0x0192: out = static_cast<char>(0x83); return true;
		case 0x201E: out = static_cast<char>(0x84); return true;
		case 0x2026: out = static_cast<char>(0x85); return true;
		case 0x2020: out = static_cast<char>(0x86); return true;
		case 0x2021: out = static_cast<char>(0x87); return true;
		case 0x02C6: out = static_cast<char>(0x88); return true;
		case 0x2030: out = static_cast<char>(0x89); return true;
		case 0x0160: out = static_cast<char>(0x8A); return true;
		case 0x2039: out = static_cast<char>(0x8B); return true;
		case 0x0152: out = static_cast<char>(0x8C); return true;
		case 0x017D: out = static_cast<char>(0x8E); return true;
		case 0x2018: out = static_cast<char>(0x91); return true;
		case 0x2019: out = static_cast<char>(0x92); return true;
		case 0x201C: out = static_cast<char>(0x93); return true;
		case 0x201D: out = static_cast<char>(0x94); return true;
		case 0x2022: out = static_cast<char>(0x95); return true;
		case 0x2013: out = static_cast<char>(0x96); return true;
		case 0x2014: out = static_cast<char>(0x97); return true;
		case 0x02DC: out = static_cast<char>(0x98); return true;
		case 0x2122: out = static_cast<char>(0x99); return true;
		case 0x0161: out = static_cast<char>(0x9A); return true;
		case 0x203A: out = static_cast<char>(0x9B); return true;
		case 0x0153: out = static_cast<char>(0x9C); return true;
		case 0x017E: out = static_cast<char>(0x9E); return true;
		case 0x0178: out = static_cast<char>(0x9F); return true;
		default: return false;
	}
}

static int NextUtf8Codepoint(const std::string& text, size_t& index) {
	const unsigned char lead = static_cast<unsigned char>(text[index]);
	if (lead < 0x80) {
		++index;
		return lead;
	}
	size_t need = 0;
	int codepoint = 0;
	if ((lead & 0xE0) == 0xC0) {
		need = 1;
		codepoint = lead & 0x1F;
	} else if ((lead & 0xF0) == 0xE0) {
		need = 2;
		codepoint = lead & 0x0F;
	} else if ((lead & 0xF8) == 0xF0) {
		need = 3;
		codepoint = lead & 0x07;
	} else {
		++index;
		return -1;
	}
	if (index + 1 + need > text.size()) {
		++index;
		return -1;
	}
	for (size_t trail = 1; trail <= need; ++trail) {
		const unsigned char byte = static_cast<unsigned char>(text[index + trail]);
		if ((byte & 0xC0) != 0x80) {
			++index;
			return -1;
		}
		codepoint = (codepoint << 6) | (byte & 0x3F);
	}
	index += 1 + need;
	return codepoint;
}

static std::string Utf8ToCp1252(const std::string& utf8) {
	std::string out;
	out.reserve(utf8.size());
	for (size_t index = 0; index < utf8.size();) {
		char mapped = '?';
		if (!Cp1252FromCodepoint(NextUtf8Codepoint(utf8, index), mapped)) {
			mapped = '?';
		}
		out.push_back(mapped);
	}
	return out;
}

// The lobby and the network settings page show one saved name; "Player" is only the empty fallback.
static std::string SavedMultiplayerName() {
	return g_SettingsMan.GetNetworkDisplayName().empty() ? "Player" : g_SettingsMan.GetNetworkDisplayName();
}

bool StartNetReplayPlayback(const std::string& path, bool fromMenu, std::string* error);

static std::string s_ShareAddress;
static bool s_ShareResolved = false;

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
	m_MultiplayerHostModeCombo = nullptr;
	m_MultiplayerHostActivityCombo = nullptr;
	m_MultiplayerHostSceneCombo = nullptr;
	m_MultiplayerHostInfoLabel = nullptr;
	m_MultiplayerHostActivities.clear();
	m_MultiplayerHostActivityIndex = 0;
	m_MultiplayerHostScenes.clear();
	m_MultiplayerHostSceneIndex = 0;
	m_MultiplayerHostPickNotice.clear();
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
	m_MultiplayerHostModeCombo = dynamic_cast<GUIComboBox*>(m_SubMenuScreenGUIControlManager->GetControl("ComboHostMode"));
	m_MultiplayerHostActivityCombo = dynamic_cast<GUIComboBox*>(m_SubMenuScreenGUIControlManager->GetControl("ComboHostActivity"));
	m_MultiplayerHostSceneCombo = dynamic_cast<GUIComboBox*>(m_SubMenuScreenGUIControlManager->GetControl("ComboHostScene"));
	m_MultiplayerHostInfoLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelHostInfo"));
	if (m_MultiplayerHostModeCombo) {
		m_MultiplayerHostModeCombo->ClearList();
		m_MultiplayerHostModeCombo->AddItem(NetMatchConfigUtil::ModeLabel(NetMatchMode::PvPSkirmish));
		m_MultiplayerHostModeCombo->AddItem(NetMatchConfigUtil::ModeLabel(NetMatchMode::CoopPvE));
		m_MultiplayerHostModeCombo->AddItem(NetMatchConfigUtil::ModeLabel(NetMatchMode::PvPvE));
		m_MultiplayerHostModeCombo->SetSelectedIndex(0);
	}
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

	m_MainMenuButtons[MenuButton::MultiplayerLobbyOptionsButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonLobbyOptions"));
	m_MainMenuButtons[MenuButton::MultiplayerHostOptionsButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostOptions"));
	m_MainMenuButtons[MenuButton::HostOptionsBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostOptBack"));
	m_MainMenuButtons[MenuButton::HostOptionsApplyButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostOptApply"));
	m_MainMenuButtons[MenuButton::HostOptionsDefaultsButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostOptDefaults"));
	m_MainMenuButtons[MenuButton::HostSeatDialogCloseButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostSeatDlgClose"));
	m_MainMenuButtons[MenuButton::HostRepairNowButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostRecRepairNow"));
	m_MainMenuButtons[MenuButton::HostFilesSaveDiagButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostFilesSaveDiag"));
	m_MainMenuButtons[MenuButton::HostSessionEndButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostSessEnd"));
	m_MainMenuButtons[MenuButton::HostSessionBannedButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostSessBanned"));
	m_MainMenuButtons[MenuButton::HostBannedRemoveButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostBannedRemove"));
	m_MainMenuButtons[MenuButton::HostBannedCloseButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostBannedClose"));
	CreateHostOptionsControls();
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

	// Credits.h is UTF-8; transcode to cp1252 so the menu Latin-1 atlas draws the letters.
	static bool s_CreditsAreCp1252 = false;
	if (!s_CreditsAreCp1252) {
		s_CreditsText = Utf8ToCp1252(s_CreditsText);
		s_CreditsAreCp1252 = true;
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
	if (m_ActiveDialogBox && (m_ActiveDialogBox == m_LastMatchDialog || m_ActiveDialogBox == m_ReplayDeleteDialog || m_ActiveDialogBox == m_HostSeatDialog || m_ActiveDialogBox == m_HostBannedDialog)) CloseMultiplayerDialog();
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
	if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen && m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions && g_UInputMan.KeyPressed(SDLK_ESCAPE)) {
		// Esc is the panel's Back: local navigation only, never a session action. An open dialog
		// eats it first the way every other screen's dialog does.
		if (m_ActiveDialogBox == m_HostSeatDialog) {
			m_HostOptionsSeatRow = -1;
			m_HostSeatDlgModerationRow = -1;
			CloseMultiplayerDialog();
		} else if (m_ActiveDialogBox == m_HostBannedDialog) {
			CloseMultiplayerDialog();
		} else {
			m_MultiplayerSubScreen = m_HostOptionsSetupDraft ? MultiplayerSubScreen::HostSetup : MultiplayerSubScreen::Lobby;
		}
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
		} else if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUIComboBox::Closed &&
		           (guiEvent.GetControl() == m_MultiplayerHostActivityCombo || guiEvent.GetControl() == m_MultiplayerHostSceneCombo ||
		            guiEvent.GetControl() == m_MultiplayerHostModeCombo)) {
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
		} else if (guiEvent.GetType() == GUIEvent::Notification && m_ActiveMenuScreen == MenuScreen::MultiplayerScreen &&
		           m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions &&
		           (guiEvent.GetMsg() == GUITab::UnPushed || guiEvent.GetMsg() == GUIComboBox::Closed ||
		            guiEvent.GetMsg() == GUICheckbox::Changed || guiEvent.GetMsg() == GUISlider::Changed ||
		            guiEvent.GetMsg() == GUITextBox::Enter)) {
			// Tabs, combos, checks and sliders on the options pages notify rather than Command; only
			// the commit-flavoured ones reach the handler (a hover must never switch the page).
			HandleMultiplayerScreenInputEvents(guiEvent.GetControl());
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
	if (m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions) {
		HandleHostOptionsInputEvents(guiEventControl);
		return;
	}
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
	} else if (guiEventControl == m_MultiplayerHostModeCombo) {
		const int selected = m_MultiplayerHostModeCombo ? m_MultiplayerHostModeCombo->GetSelectedIndex() : -1;
		static const NetMatchMode modes[] = {NetMatchMode::PvPSkirmish, NetMatchMode::CoopPvE, NetMatchMode::PvPvE};
		if (selected >= 0 && selected < 3) {
			m_MultiplayerHostMode = modes[selected];
		}
		ApplyMultiplayerHostActivity();
		g_GUISound.ItemChangeSound()->Play();
	} else if (guiEventControl == m_MultiplayerHostActivityCombo) {
		// A closed pick-list carries its selection; the index is the request fields' source.
		const int selected = m_MultiplayerHostActivityCombo ? m_MultiplayerHostActivityCombo->GetSelectedIndex() : -1;
		if (selected >= 0 && static_cast<size_t>(selected) < m_MultiplayerHostActivities.size()) {
			m_MultiplayerHostActivityIndex = static_cast<size_t>(selected);
		}
		m_MultiplayerHostPickNotice.clear();
		RefreshMultiplayerHostScenes();
		g_GUISound.ItemChangeSound()->Play();
	} else if (guiEventControl == m_MultiplayerHostSceneCombo) {
		const int selected = m_MultiplayerHostSceneCombo ? m_MultiplayerHostSceneCombo->GetSelectedIndex() : -1;
		if (selected >= 0 && static_cast<size_t>(selected) < m_MultiplayerHostScenes.size()) {
			m_MultiplayerHostSceneIndex = static_cast<size_t>(selected);
		}
		m_MultiplayerHostPickNotice.clear();
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
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerLobbyOptionsButton]) {
		// The lobby's options entry: the host edits, a client reads the same adopted config.
		OpenHostOptions(false);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerHostOptionsButton]) {
		// The setup screen's twin: the draft the next Create carries.
		OpenHostOptions(true);
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
	// Same walk as the scenario menu: every GameActivity that is not a test and has a compatible scene.
	// H12's first-run default is Skirmish Defense - the same preset a fresh NetMatchServiceRequest
	// names - until the host's saved template (or an earlier pick) says otherwise.
	const std::pair<std::string, std::string> current =
		m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size() ? m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex] : std::pair<std::string, std::string>{"Skirmish Defense", "Base.rte"};
	const bool hadPick = !m_MultiplayerHostActivities.empty();
	m_MultiplayerHostActivities.clear();
	for (const NetHostActivityChoice& row: g_NetMatchService.ListHostActivities()) {
		m_MultiplayerHostActivities.emplace_back(row.preset, row.module);
	}
	m_MultiplayerHostActivityIndex = 0;
	bool found = false;
	for (size_t i = 0; i < m_MultiplayerHostActivities.size(); ++i) {
		if (m_MultiplayerHostActivities[i] == current) {
			m_MultiplayerHostActivityIndex = i;
			found = true;
		}
	}
	if (hadPick && !found && !m_MultiplayerHostActivities.empty()) {
		m_MultiplayerHostPickNotice = current.first + " is no longer loaded; picked " +
		                             m_MultiplayerHostActivities[0].first +
		                             (m_MultiplayerHostActivities[0].second.empty() ? "" : " - " + m_MultiplayerHostActivities[0].second);
	} else if (found) {
		m_MultiplayerHostPickNotice.clear();
	}
	if (m_MultiplayerHostActivityCombo) {
		m_MultiplayerHostActivityCombo->ClearList();
		for (const auto& [preset, module] : m_MultiplayerHostActivities) {
			m_MultiplayerHostActivityCombo->AddItem(preset + (module.empty() ? "" : " - " + module));
		}
	}
	RefreshMultiplayerHostScenes();
}

void MainMenuGUI::RefreshMultiplayerHostScenes() {
	const std::pair<std::string, std::string> current =
		m_MultiplayerHostSceneIndex < m_MultiplayerHostScenes.size() ? m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex] : std::pair<std::string, std::string>{};
	const std::string preset = m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size() ? m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].first : "P4 Alpha Duel";
	const std::string module = m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size() ? m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].second : "Base.rte";
	m_MultiplayerHostScenes.clear();
	for (const NetHostSceneChoice& row: g_NetMatchService.ListHostScenes(preset, module)) {
		m_MultiplayerHostScenes.emplace_back(row.name, row.module);
	}
	m_MultiplayerHostSceneIndex = 0;
	bool found = false;
	for (size_t i = 0; i < m_MultiplayerHostScenes.size(); ++i) {
		if (!current.first.empty() && m_MultiplayerHostScenes[i] == current) {
			m_MultiplayerHostSceneIndex = i;
			found = true;
		}
	}
	if (!found) {
		for (size_t i = 0; i < m_MultiplayerHostScenes.size(); ++i) {
			if (m_MultiplayerHostScenes[i].first == "Grasslands") {
				m_MultiplayerHostSceneIndex = i;
				break;
			}
		}
	}
	if (m_MultiplayerHostSceneCombo) {
		m_MultiplayerHostSceneCombo->ClearList();
		std::map<std::string, int> names;
		for (const auto& [name, sceneModule] : m_MultiplayerHostScenes) {
			++names[name];
		}
		for (const auto& [name, sceneModule] : m_MultiplayerHostScenes) {
			m_MultiplayerHostSceneCombo->AddItem(name + (names[name] > 1 && !sceneModule.empty() ? " - " + sceneModule : ""));
		}
	}
	FitHostActivityCombo();
	ApplyMultiplayerHostActivity();
}

void MainMenuGUI::FitHostActivityCombo() {
	if (!m_MultiplayerHostActivityCombo || !m_MultiplayerHostPortTextBox || !m_MultiplayerHostPanel) {
		return;
	}
	constexpr int namePad = 8;
	constexpr int scrollThickness = 17;
	constexpr int panelPad = 12;
	const int valueX = m_MultiplayerHostPortTextBox->GetRelXPos();
	const int maxWidth = std::max(80, m_MultiplayerHostPanel->GetWidth() - valueX - panelPad);
	// Every picker in the value column is measured the same way: its own longest row plus the list
	// pad and, when the rows overflow the drop, the scrollbar.
	const auto fit = [&](GUIComboBox* combo) {
		GUIListPanel* list = combo->GetListPanel();
		GUIFont* font = list ? list->GetFont() : nullptr;
		if (!font && m_SubMenuScreenGUIControlManager && m_SubMenuScreenGUIControlManager->GetSkin()) {
			GUISkin* skin = m_SubMenuScreenGUIControlManager->GetSkin();
			std::string fontName;
			if (skin->GetValue("Listbox", "Font", &fontName)) {
				font = skin->GetFont(fontName);
			}
			if (!font) {
				font = skin->GetFont("FontLarge.png");
			}
		}
		int longest = 0;
		if (font) {
			for (int i = 0; i < combo->GetCount(); ++i) {
				if (const GUIListPanel::Item* item = combo->GetItem(i)) {
					longest = std::max(longest, font->CalculateWidth(item->m_Name));
				}
			}
		}
		const int rowHeight = font ? font->GetFontHeight() : 0;
		const int stackHeight = std::max(list ? list->GetStackHeight() : 0, rowHeight * combo->GetCount());
		const int scroll = (rowHeight > 0 && stackHeight > combo->GetDropHeight()) ? scrollThickness : 0;
		const int needed = longest > 0 ? longest + namePad + scroll : combo->GetWidth();
		const int width = std::clamp(std::max(needed, 80), 80, maxWidth);
		combo->SetPositionRel(valueX, combo->GetRelYPos());
		if (combo->GetWidth() != width) {
			combo->Resize(width, combo->GetHeight());
		}
		return width;
	};
	const int activityWidth = fit(m_MultiplayerHostActivityCombo);
	if (m_MultiplayerHostSceneCombo) {
		fit(m_MultiplayerHostSceneCombo);
	}
	// The mode rows are short words, so the mode picker follows the activity picker's width.
	if (m_MultiplayerHostModeCombo) {
		m_MultiplayerHostModeCombo->SetPositionRel(valueX, m_MultiplayerHostModeCombo->GetRelYPos());
		if (m_MultiplayerHostModeCombo->GetWidth() != activityWidth) {
			m_MultiplayerHostModeCombo->Resize(activityWidth, m_MultiplayerHostModeCombo->GetHeight());
		}
	}
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
	const auto* scene = m_MultiplayerHostSceneIndex < m_MultiplayerHostScenes.size()
	                        ? &m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex] : nullptr;
	const std::string sceneName = scene ? scene->first : "Grasslands";
	if (m_MultiplayerHostSceneCombo) {
		if (scene) {
			m_MultiplayerHostSceneCombo->SetSelectedIndex(static_cast<int>(m_MultiplayerHostSceneIndex));
		} else {
			m_MultiplayerHostSceneCombo->SetText(sceneName);
		}
	}
	if (m_MultiplayerHostModeCombo) {
		const int modeIndex = m_MultiplayerHostMode == NetMatchMode::CoopPvE ? 1 : (m_MultiplayerHostMode == NetMatchMode::PvPvE ? 2 : 0);
		m_MultiplayerHostModeCombo->SetSelectedIndex(modeIndex);
	}
	if (m_MultiplayerHostInfoLabel) {
		if (!m_MultiplayerHostPickNotice.empty()) {
			m_MultiplayerHostInfoLabel->SetText(m_MultiplayerHostPickNotice);
		} else {
			m_MultiplayerHostInfoLabel->SetText(sceneName + " - " + NetMatchConfigUtil::ModeLabel(m_MultiplayerHostMode));
		}
	}
}

// ---- §9.2/9.3 host options: six tabbed pages over the lobby, or the host setup's draft of the
// next one. The panel edits a complete NetMatchConfig draft; Apply stages it through the service. ----

void MainMenuGUI::CreateHostOptionsControls() {
	const auto get = [this](const char* name) { return m_SubMenuScreenGUIControlManager->GetControl(name); };
	m_HostOptionsPanel = dynamic_cast<GUICollectionBox*>(get("MultiplayerHostOptionsPanel"));
	m_HostOptionsTitle = dynamic_cast<GUILabel*>(get("LabelHostOptionsTitle"));
	static const char* tabNames[c_HostOptionsPageCount] = {"TabHostPageSeats", "TabHostPageRules", "TabHostPageNetwork", "TabHostPageRecovery", "TabHostPageFiles", "TabHostPageSession"};
	static const char* pageNames[c_HostOptionsPageCount] = {"CollectionBoxHostPageSeats", "CollectionBoxHostPageRules", "CollectionBoxHostPageNetwork", "CollectionBoxHostPageRecovery", "CollectionBoxHostPageFiles", "CollectionBoxHostPageSession"};
	for (int i = 0; i < c_HostOptionsPageCount; ++i) {
		m_HostOptionsTabs[i] = dynamic_cast<GUITab*>(get(tabNames[i]));
		m_HostOptionsPages[i] = dynamic_cast<GUICollectionBox*>(get(pageNames[i]));
	}
	m_HostOptionsStatusLabel = dynamic_cast<GUILabel*>(get("LabelHostOptStatus"));
	m_HostSeatPlayersCombo = dynamic_cast<GUIComboBox*>(get("ComboHostSeatPlayers"));
	m_HostSeatCapacityHint = dynamic_cast<GUILabel*>(get("LabelHostSeatCapacityHint"));
	for (int row = 0; row < c_HostSeatRows; ++row) {
		const std::string n = std::to_string(row);
		m_HostSeatNameLabels[row] = dynamic_cast<GUILabel*>(get(("LabelHostSeatName" + n).c_str()));
		m_HostSeatTypeCombos[row] = dynamic_cast<GUIComboBox*>(get(("ComboHostSeatType" + n).c_str()));
		m_HostSeatTeamCombos[row] = dynamic_cast<GUIComboBox*>(get(("ComboHostSeatTeam" + n).c_str()));
		m_HostSeatDelayLabels[row] = dynamic_cast<GUILabel*>(get(("LabelHostSeatDelay" + n).c_str()));
		m_HostSeatStateLabels[row] = dynamic_cast<GUILabel*>(get(("LabelHostSeatState" + n).c_str()));
		m_HostSeatDetailsButtons[row] = dynamic_cast<GUIButton*>(get(("ButtonHostSeatDetails" + n).c_str()));
	}
	m_HostRulesActivityCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRulesActivity"));
	if (m_HostRulesActivityCombo) {
		// The same module-qualified census the setup picker's combo carries; a staged preset the
		// census lacks still displays by name through HostOptSelectCombo.
		m_HostRulesActivityCombo->ClearList();
		for (const auto& [preset, module] : m_MultiplayerHostActivities) {
			m_HostRulesActivityCombo->AddItem(preset + (module.empty() ? "" : " - " + module));
		}
	}
	m_HostRulesSceneCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRulesScene"));
	m_HostRulesModeCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRulesMode"));
	m_HostRulesDifficultySlider = dynamic_cast<GUISlider*>(get("SliderHostRulesDifficulty"));
	m_HostRulesDifficultyValue = dynamic_cast<GUILabel*>(get("LabelHostRulesDifficultyValue"));
	m_HostRulesGoldSlider = dynamic_cast<GUISlider*>(get("SliderHostRulesGold"));
	m_HostRulesGoldValue = dynamic_cast<GUILabel*>(get("LabelHostRulesGoldValue"));
	m_HostRulesFogCheck = dynamic_cast<GUICheckbox*>(get("CheckHostRulesFog"));
	m_HostRulesClearPathCheck = dynamic_cast<GUICheckbox*>(get("CheckHostRulesClearPath"));
	m_HostRulesDeployCheck = dynamic_cast<GUICheckbox*>(get("CheckHostRulesDeploy"));
	m_HostRulesBrainlessCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRulesBrainless"));
	m_HostRulesTeamCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRulesTeam"));
	m_HostRulesTechCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRulesTech"));
	m_HostRulesSkillSlider = dynamic_cast<GUISlider*>(get("SliderHostRulesSkill"));
	m_HostRulesSkillValue = dynamic_cast<GUILabel*>(get("LabelHostRulesSkillValue"));
	m_HostNetPolicyCombo = dynamic_cast<GUIComboBox*>(get("ComboHostNetPolicy"));
	m_HostNetMinDelayBox = dynamic_cast<GUITextBox*>(get("TextHostNetMinDelay"));
	m_HostNetEffectiveLabel = dynamic_cast<GUILabel*>(get("LabelHostNetEffective"));
	for (int peer = 0; peer < 4; ++peer) {
		const std::string n = std::to_string(peer + 1);
		m_HostNetPeerLabels[peer] = dynamic_cast<GUILabel*>(get(("LabelHostNetPeer" + n).c_str()));
		m_HostNetPeerDelayBoxes[peer] = dynamic_cast<GUITextBox*>(get(("TextHostNetPeerDelay" + n).c_str()));
	}
	m_HostNetPingLabel = dynamic_cast<GUILabel*>(get("LabelHostNetPing"));
	m_HostNetRecalcButton = dynamic_cast<GUIButton*>(get("ButtonHostNetRecalc"));
	m_HostRecRepairCheck = dynamic_cast<GUICheckbox*>(get("CheckHostRecRepair"));
	m_HostRecAutosaveCheck = dynamic_cast<GUICheckbox*>(get("CheckHostRecAutosave"));
	m_HostRecAutosaveIntervalBox = dynamic_cast<GUITextBox*>(get("TextHostRecAutosaveInterval"));
	m_HostRecLastSaveLabel = dynamic_cast<GUILabel*>(get("LabelHostRecLastSave"));
	m_HostRecWaitingLabel = dynamic_cast<GUILabel*>(get("LabelHostRecWaiting"));
	m_HostFilesSavePathLabel = dynamic_cast<GUILabel*>(get("LabelHostFilesSavePath"));
	m_HostFilesDiagPathLabel = dynamic_cast<GUILabel*>(get("LabelHostFilesDiagPath"));
	m_HostFilesDiagResultLabel = dynamic_cast<GUILabel*>(get("LabelHostFilesDiagResult"));
	m_HostFilesWidgetCombo = dynamic_cast<GUIComboBox*>(get("ComboHostFilesWidget"));
	m_HostSessHostingLabel = dynamic_cast<GUILabel*>(get("LabelHostSessHosting"));
	m_HostSessSeatsLabel = dynamic_cast<GUILabel*>(get("LabelHostSessSeats"));
	m_HostSessIdleCombo = dynamic_cast<GUIComboBox*>(get("ComboHostSessIdle"));
	m_HostSessIdleStateLabel = dynamic_cast<GUILabel*>(get("LabelHostSessIdleState"));
	m_HostSessBannedLabel = dynamic_cast<GUILabel*>(get("LabelHostSessBanned"));
	m_HostSeatDialog = dynamic_cast<GUICollectionBox*>(get("HostSeatDialog"));
	m_HostSeatDlgName = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgName"));
	m_HostSeatDlgSeat = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgSeat"));
	m_HostSeatDlgTeam = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgTeam"));
	m_HostSeatDlgState = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgState"));
	m_HostSeatDlgReclaim = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgReclaim"));
	m_HostSeatDlgApplicants = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgApplicants"));
	m_HostSeatDlgApplicant = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgApplicant"));
	m_HostSeatDlgWait = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgWait"));
	m_HostSeatDlgApprove = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgApprove"));
	m_HostSeatDlgCancel = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgCancel"));
	m_HostSeatDlgKick = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgKick"));
	m_HostSeatDlgBan = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgBan"));
	m_HostSeatDlgActionHint = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgActionHint"));
	m_HostSeatDlgStatus = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgStatus"));
	m_HostBannedDialog = dynamic_cast<GUICollectionBox*>(get("HostBannedDialog"));
	m_HostBannedListLabel = dynamic_cast<GUILabel*>(get("LabelHostBannedList"));
	m_HostBannedStatusLabel = dynamic_cast<GUILabel*>(get("LabelHostBannedStatus"));

	if (m_HostSeatPlayersCombo) {
		m_HostSeatPlayersCombo->ClearList();
		for (int seats = NetMatchConfigUtil::c_MinPeerCount; seats <= NetMatchConfigUtil::c_MaxPeerCount; ++seats) {
			m_HostSeatPlayersCombo->AddItem(std::to_string(seats));
		}
	}
	if (m_HostRulesModeCombo) {
		m_HostRulesModeCombo->ClearList();
		m_HostRulesModeCombo->AddItem(NetMatchConfigUtil::ModeLabel(NetMatchMode::PvPSkirmish));
		m_HostRulesModeCombo->AddItem(NetMatchConfigUtil::ModeLabel(NetMatchMode::CoopPvE));
		m_HostRulesModeCombo->AddItem(NetMatchConfigUtil::ModeLabel(NetMatchMode::PvPvE));
	}
	// L33's pair of values, in the host's words.
	if (m_HostRulesBrainlessCombo) {
		m_HostRulesBrainlessCombo->ClearList();
		m_HostRulesBrainlessCombo->AddItem("Keep playing, humans spectate");
		m_HostRulesBrainlessCombo->AddItem("End the match");
	}
	if (m_HostRulesTeamCombo) {
		m_HostRulesTeamCombo->ClearList();
		for (int team = 1; team <= 4; ++team) {
			m_HostRulesTeamCombo->AddItem("Team " + std::to_string(team));
		}
	}
	for (GUIComboBox* combo : m_HostSeatTeamCombos) {
		if (!combo) continue;
		combo->ClearList();
		for (int team = 1; team <= 4; ++team) {
			combo->AddItem("Team " + std::to_string(team));
		}
	}
	// H03's seat kinds, in the order the status column reads them.
	for (GUIComboBox* combo : m_HostSeatTypeCombos) {
		if (!combo) continue;
		combo->ClearList();
		combo->AddItem("Open");
		combo->AddItem("Closed");
		combo->AddItem("CPU");
	}
	if (m_HostRulesTechCombo) {
		m_HostRulesTechCombo->ClearList();
		m_HostOptionsTechModules.clear();
		m_HostOptionsTechModules.push_back("-All-");
		m_HostOptionsTechModules.push_back("-Random-");
		for (int moduleId = 0; moduleId < g_PresetMan.GetTotalModuleCount(); ++moduleId) {
			m_HostOptionsTechModules.push_back(g_PresetMan.GetDataModuleName(moduleId));
		}
		for (const std::string& module : m_HostOptionsTechModules) {
			m_HostRulesTechCombo->AddItem(module);
		}
	}
	if (m_HostNetPolicyCombo) {
		m_HostNetPolicyCombo->ClearList();
		m_HostNetPolicyCombo->AddItem("Automatic");
		m_HostNetPolicyCombo->AddItem("Fixed");
	}
	if (m_HostFilesWidgetCombo) {
		m_HostFilesWidgetCombo->ClearList();
		m_HostFilesWidgetCombo->AddItem("Off");
		m_HostFilesWidgetCombo->AddItem("Auto");
		m_HostFilesWidgetCombo->AddItem("Always");
	}
	if (m_HostSessIdleCombo) {
		m_HostSessIdleCombo->ClearList();
		m_HostSessIdleCombo->AddItem("Never");
		for (int minutes : {1, 5, 10, 20, 30, 45, 60}) {
			m_HostSessIdleCombo->AddItem(std::to_string(minutes) + " minutes");
		}
	}
	if (m_HostNetMinDelayBox) {
		m_HostNetMinDelayBox->SetNumericOnly(true);
		m_HostNetMinDelayBox->SetMaxNumericValue(NetMatchConfigUtil::c_MaxInputDelayFrames);
		m_HostNetMinDelayBox->SetMaxTextLength(2);
	}
	for (GUITextBox* box : m_HostNetPeerDelayBoxes) {
		if (!box) continue;
		box->SetNumericOnly(true);
		box->SetMaxNumericValue(NetMatchConfigUtil::c_MaxInputDelayFrames);
		box->SetMaxTextLength(2);
	}
	if (m_HostRecAutosaveIntervalBox) {
		m_HostRecAutosaveIntervalBox->SetNumericOnly(true);
		m_HostRecAutosaveIntervalBox->SetMaxNumericValue(NetMatchService::c_MaxAutosaveIntervalSeconds);
		m_HostRecAutosaveIntervalBox->SetMaxTextLength(4);
	}
	// The scene list is a preset census like the host picker's activity one; the draft's own scene
	// always stays selectable even when no preset of that name loads.
	m_HostOptionsScenes.clear();
	std::list<Entity*> scenes;
	if (g_PresetMan.GetAllOfType(scenes, "Scene")) {
		for (const Entity* scene : scenes) {
			m_HostOptionsScenes.push_back(scene->GetPresetName());
		}
	}
	if (m_HostRulesSceneCombo) {
		m_HostRulesSceneCombo->ClearList();
		for (const std::string& name : m_HostOptionsScenes) {
			m_HostRulesSceneCombo->AddItem(name);
		}
	}
}

NetMatchServiceRequest MainMenuGUI::HostRequestDraft() const {
	NetMatchServiceRequest request;
	request.host = true;
	const std::string typedName = m_MultiplayerNameTextBox->GetText();
	request.playerName = typedName.empty() ? "Host" : typedName;
	const long parsedPort = std::strtol(m_MultiplayerHostPortTextBox->GetText().c_str(), nullptr, 10);
	request.port = static_cast<uint16_t>(std::clamp<long>(parsedPort, 1, 65535));
	request.activityPreset = "P4 Alpha Duel";
	if (m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size()) {
		request.activityPreset = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].first;
		request.activityModule = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].second;
	}
	const long parsedPlayers = std::strtol(m_MultiplayerHostPlayersTextBox->GetText().c_str(), nullptr, 10);
	request.peerCount = static_cast<uint8_t>(std::clamp<long>(parsedPlayers, NetMatchConfigUtil::c_MinPeerCount, NetMatchConfigUtil::c_MaxPeerCount));
	request.mode = m_MultiplayerHostMode;
	request.ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
	request.resyncOnDesync = true;
	request.autoInputDelay = g_SettingsMan.GetNetworkHostDelayPolicy() == SettingsMan::NetworkHostDelayPolicy::Auto;
	const long parsedDelay = std::strtol(m_MultiplayerHostInputDelayTextBox->GetText().c_str(), nullptr, 10);
	request.inputDelayFrames = static_cast<uint16_t>(std::clamp<int>(static_cast<int>(parsedDelay), 0, NetMatchConfigUtil::c_MaxInputDelayFrames));
	// The staged options draft overrides every field the request names; a field it leaves keeps the
	// setup controls' (then the saved settings') values.
	if (m_HostSetupOptions) {
		request.standardRules = static_cast<const NetMatchStandardRules&>(*m_HostSetupOptions);
		// BuildMatchConfig reads the mode off the request, not the rules block it carries inside.
		request.mode = m_HostSetupOptions->mode;
		request.delayPolicy = m_HostSetupOptions->delayPolicy;
		request.idleWaitMinutes = m_HostSetupOptions->idleWaitMinutes;
		request.automaticRepair = m_HostSetupOptions->automaticRepair;
		request.autosaveSeconds = m_HostSetupOptions->autosaveEnabled ? m_HostSetupOptions->autosaveIntervalSeconds : 0;
		request.inputDelayFrames = m_HostSetupOptions->inputDelayFrames;
		request.autoInputDelay = m_HostSetupOptions->delayPolicy == NetMatchDelayPolicy::Auto;
		request.peerCount = m_HostSetupOptions->peerCount;
		// H03's kind edits ride the request: the drafted human/CPU split, not the mode's default.
		uint32_t humanSeats = 0, cpuSeats = 0;
		for (const NetMatchPlayerSlot& slot : m_HostSetupOptions->players) {
			(slot.cpu ? cpuSeats : humanSeats)++;
		}
		request.humans = humanSeats;
		request.cpuSlots = cpuSeats;
	}
	NetMatchService::SeatSavedOptions(request);
	return request;
}

void MainMenuGUI::OpenHostOptions(bool setupDraft) {
	m_HostOptionsSetupDraft = setupDraft;
	m_HostOptionsReadOnly = false;
	m_HostOptionsSeatRow = -1;
	m_HostSeatDlgModerationRow = -1;
	m_HostOptionsAwaitedRevision = 0;
	if (setupDraft) {
		// The staged draft applies where one exists; the request the Create button would send builds the rest.
		NetMatchServiceRequest request = HostRequestDraft();
		std::string error;
		// A setup draft has no session yet, so it is built under a placeholder id: the real one is
		// assigned when the lobby opens, and a zero id is not a configuration the validator accepts.
		constexpr uint64_t setupDraftSessionId = 1;
		if (!NetMatchService::BuildMatchConfig(request, setupDraftSessionId, m_HostOptionsDraft, &error)) {
			m_HostOptionsDraft = NetMatchConfigUtil::MakeDefault(setupDraftSessionId);
			m_HostOptionsStatusLabel->SetText(error);
		} else if (m_HostSetupOptions) {
			m_HostOptionsStatusLabel->SetText("Staged options are loaded.");
		} else {
			// Nothing staged this run: the host's saved defaults are what a new lobby proposes.
			NetHostDefaultsTemplate saved;
			std::string templateError;
			if (NetHostDefaults::Load(saved, &templateError) && NetHostDefaults::ApplyTo(saved, m_HostOptionsDraft, &templateError)) {
				m_HostOptionsStatusLabel->SetText("Host defaults loaded.");
			} else {
				m_HostOptionsStatusLabel->SetText(templateError);
			}
		}
	} else {
		m_HostOptionsDraft = g_NetMatchService.GetLobbyMatchConfig();
		m_HostOptionsReadOnly = !g_NetMatchService.GetLobbySnapshot().isHost;
		m_HostOptionsStatusLabel->SetText(m_HostOptionsReadOnly ? "Only the host edits these." : "");
	}
	m_HostOptionsBaseRevision = m_HostOptionsDraft.configRevision;
	m_MultiplayerSubScreen = MultiplayerSubScreen::HostOptions;
	ShowHostOptionsPage(m_HostOptionsPage);
	g_GUISound.ButtonPressSound()->Play();
}

void MainMenuGUI::ShowHostOptionsPage(int page) {
	m_HostOptionsPage = std::clamp(page, 0, c_HostOptionsPageCount - 1);
	for (int i = 0; i < c_HostOptionsPageCount; ++i) {
		if (m_HostOptionsPages[i]) m_HostOptionsPages[i]->SetVisible(i == m_HostOptionsPage);
		if (m_HostOptionsTabs[i]) m_HostOptionsTabs[i]->SetCheck(i == m_HostOptionsPage);
	}
}

namespace {
	void HostOptSelectCombo(GUIComboBox* combo, const std::string& text) {
		if (!combo) return;
		for (int i = 0; i < combo->GetCount(); ++i) {
			if (combo->GetItem(i) && combo->GetItem(i)->m_Name == text) {
				combo->SetSelectedIndex(i);
				return;
			}
		}
		combo->SetText(text);
	}

	void HostOptSelectComboIndex(GUIComboBox* combo, int index) {
		if (combo) combo->SetSelectedIndex(std::clamp(index, 0, std::max(0, combo->GetCount() - 1)));
	}

	void HostOptSetEditable(GUIControl* control, bool editable) {
		if (control) control->SetEnabled(editable);
	}

	bool HostOptBoxFocused(GUITextBox* box) {
		return box && box->GetPanel() && box->GetPanel()->HasFocus();
	}
}

void MainMenuGUI::RefreshHostOptionsControls(const NetLobbySnapshot& snapshot) {
	if (!m_HostOptionsPanel) return;
	// A live lobby re-seeds the draft whenever the adopted config advances past the base revision;
	// a staged host draft or a local edit never loses the player's text mid-typing.
	if (!m_HostOptionsSetupDraft) {
		const NetMatchConfig adopted = g_NetMatchService.GetLobbyMatchConfig();
		if (adopted.configRevision != m_HostOptionsDraft.configRevision || m_HostOptionsReadOnly) {
			// A client always mirrors the adopted config; a host re-seeds only from an older base.
			if (m_HostOptionsReadOnly || adopted.configRevision > m_HostOptionsBaseRevision) {
				m_HostOptionsDraft = adopted;
				m_HostOptionsBaseRevision = adopted.configRevision;
			}
		}
		const std::optional<NetMatchConfig> pending = g_NetMatchService.GetPendingHostOptions();
		if (pending || m_HostOptionsAwaitedRevision > adopted.configRevision) {
			// A rematch lobby's draft is staged for the next match; an open lobby's Apply is a live
			// republish the peers acknowledge before it counts. "Applied" lands when the adopted
			// mirror reaches the awaited revision - the runner queue publishes it for every peer.
			m_HostOptionsStatusLabel->SetText(snapshot.playedAMatch
			                                      ? "Options staged for the next match."
			                                      : "Waiting for peers to confirm the new options...");
		}
		if (m_HostOptionsAwaitedRevision != 0 && adopted.configRevision >= m_HostOptionsAwaitedRevision) {
			m_HostOptionsAwaitedRevision = 0;
			m_HostOptionsStatusLabel->SetText("Applied.");
		}
	}
	// Each page names itself in the title band the way the design's page mocks do; the client's
	// read-only view keeps the details title on every page.
	static const char* pageTitles[c_HostOptionsPageCount] = {
		"H O S T   O P T I O N S", "M A T C H   R U L E S", "N E T W O R K   O P T I O N S",
		"M A T C H   R E C O V E R Y", "F I L E S   A N D   S T A T U S", "S E S S I O N"};
	m_HostOptionsTitle->SetText(m_HostOptionsReadOnly ? "M A T C H   D E T A I L S" : pageTitles[m_HostOptionsPage]);
	const bool editable = !m_HostOptionsReadOnly;

	// Seats page.
	const int capacity = std::clamp<int>(m_HostOptionsDraft.peerCount, NetMatchConfigUtil::c_MinPeerCount, NetMatchConfigUtil::c_MaxPeerCount);
	HostOptSelectComboIndex(m_HostSeatPlayersCombo, capacity - NetMatchConfigUtil::c_MinPeerCount);
	// Capacity is a session property once the lobby is open; only the setup draft may move it.
	HostOptSetEditable(m_HostSeatPlayersCombo, editable && m_HostOptionsSetupDraft);
	if (m_HostSeatCapacityHint) {
		m_HostSeatCapacityHint->SetText(m_HostOptionsSetupDraft ? "Peers the lobby will seat" : "Fixed while the lobby is open");
	}
	// Each rostered slot is a row; while the draft can still take a seat a trailing "Closed"
	// placeholder row is where the host opens one back up. peerCount itself is transport
	// capacity, fixed once the lobby is open, so a closed seat keeps its row instead of
	// shrinking the session.
	int humans = 0;
	for (const NetMatchPlayerSlot& slot : m_HostOptionsDraft.players) humans += slot.cpu ? 0 : 1;
	const int humanCapacity = m_HostOptionsDraft.peerCount - (m_HostOptionsDraft.dedicated ? 1 : 0);
	const int slots = static_cast<int>(m_HostOptionsDraft.players.size());
	const bool placeholder = editable && slots < c_HostSeatRows &&
	                         (humans < humanCapacity || slots < static_cast<int>(NetMatchConfigUtil::c_MaxPlayers));
	const int seatRows = std::min(slots + (placeholder ? 1 : 0), c_HostSeatRows);
	for (int row = 0; row < c_HostSeatRows; ++row) {
		const bool used = row < seatRows;
		for (GUIControl* control : {static_cast<GUIControl*>(m_HostSeatNameLabels[row]), static_cast<GUIControl*>(m_HostSeatTypeCombos[row]),
		                            static_cast<GUIControl*>(m_HostSeatTeamCombos[row]),
		                            static_cast<GUIControl*>(m_HostSeatDelayLabels[row]), static_cast<GUIControl*>(m_HostSeatStateLabels[row]),
		                            static_cast<GUIControl*>(m_HostSeatDetailsButtons[row])}) {
			if (control) control->SetVisible(used);
		}
		if (!used) continue;
		const bool isPlaceholder = row >= slots;
		static const NetMatchPlayerSlot c_EmptySlot;
		const NetMatchPlayerSlot& slot = isPlaceholder ? c_EmptySlot : m_HostOptionsDraft.players[row];
		m_HostSeatNameLabels[row]->SetText(isPlaceholder ? "--" : slot.displayName);
		const NetLobbyMember* member = nullptr;
		if (!isPlaceholder && slot.peerId != 0) {
			for (const NetLobbyMember& m : snapshot.members) {
				if (m.peerId == slot.peerId) {
					member = &m;
					break;
				}
			}
		}
		// The delay column reads the agreed per-sender value; automatic reads "Auto" since each
		// sender's own link decides its figure there.
		std::string delay = "--";
		if (!isPlaceholder && !slot.cpu && slot.peerId != 0) {
			delay = m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Fixed
			            ? std::to_string(NetMatchConfigUtil::PeerInputDelay(m_HostOptionsDraft, slot.peerId)) + " ticks"
			            : "Auto";
		}
		m_HostSeatDelayLabels[row]->SetText(delay);
		// A departed human's seat shows the leave consequence, not the CPU's: its units are already
		// AI-driven, while a merely dropped holder is still reclaiming and stays a held seat.
		std::string state = "Open";
		if (isPlaceholder) {
			state = "Closed";
		} else if (slot.cpu) {
			state = "CPU / Skill " + std::to_string(m_HostOptionsDraft.teamRules[slot.team < 4 ? slot.team : 0].aiSkill);
		} else {
			const std::string line = member ? member->statusLine : std::string();
			if (line.size() >= 5 && line.compare(line.size() - 5, 5, ": left") == 0) {
				state = "AI in control";
			} else if (line.find("reconnecting") != std::string::npos) {
				state = "Reclaiming";
			} else if (line.find("disconnected") != std::string::npos) {
				state = "Held";
			} else if (member && member->connected) {
				state = member->ready ? "Ready" : "Not ready";
				if (member->isLocal) state += " (you)";
			} else if (member && member->dropped) {
				state = "Held";
			}
		}
		m_HostSeatStateLabels[row]->SetText(state);
		// H03: the kind column — Open / Closed / CPU. A seated human's kind never moves: the seat
		// belongs to its player, and SubmitHostOptions refuses the drop a stale UI would still try.
		const int typeIndex = isPlaceholder ? 1 : (slot.cpu ? 2 : 0);
		HostOptSelectComboIndex(m_HostSeatTypeCombos[row], typeIndex);
		const bool seatedHuman = !isPlaceholder && !slot.cpu && slot.peerId != 0 &&
		                         member && (member->connected || member->dropped || member->reclaiming);
		HostOptSetEditable(m_HostSeatTypeCombos[row], editable && !seatedHuman);
		HostOptSelectComboIndex(m_HostSeatTeamCombos[row], slot.team);
		HostOptSetEditable(m_HostSeatTeamCombos[row], editable && !isPlaceholder && !slot.cpu);
		HostOptSetEditable(m_HostSeatDetailsButtons[row], !isPlaceholder);
	}

	// Rules page.
	HostOptSelectCombo(m_HostRulesActivityCombo, m_HostOptionsDraft.activityPreset +
	                   (m_HostOptionsDraft.activityModule.empty() ? "" : " - " + m_HostOptionsDraft.activityModule));
	HostOptSelectCombo(m_HostRulesSceneCombo, m_HostOptionsDraft.sceneName);
	const int modeIndex = m_HostOptionsDraft.mode == NetMatchMode::CoopPvE ? 1 : (m_HostOptionsDraft.mode == NetMatchMode::PvPvE ? 2 : 0);
	HostOptSelectComboIndex(m_HostRulesModeCombo, modeIndex);
	if (m_HostRulesDifficultySlider) m_HostRulesDifficultySlider->SetValue(m_HostOptionsDraft.difficulty);
	if (m_HostRulesDifficultyValue) m_HostRulesDifficultyValue->SetText(std::to_string(m_HostOptionsDraft.difficulty));
	const uint32_t goldSlider = std::min(m_HostOptionsDraft.startingGold, static_cast<uint32_t>(31000));
	if (m_HostRulesGoldSlider) m_HostRulesGoldSlider->SetValue(static_cast<int>(goldSlider));
	if (m_HostRulesGoldValue) {
		m_HostRulesGoldValue->SetText(m_HostOptionsDraft.startingGold >= NetMatchConfigUtil::c_InfiniteGold
		                                  ? "Infinite" : (std::to_string(m_HostOptionsDraft.startingGold) + " oz"));
	}
	if (m_HostRulesFogCheck) m_HostRulesFogCheck->SetCheck(m_HostOptionsDraft.fogOfWar ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	if (m_HostRulesClearPathCheck) m_HostRulesClearPathCheck->SetCheck(m_HostOptionsDraft.requireClearPathToOrbit ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	if (m_HostRulesDeployCheck) m_HostRulesDeployCheck->SetCheck(m_HostOptionsDraft.deployUnits ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	// L33: index 0 keeps the humans spectating; index 1 ends the match.
	HostOptSelectComboIndex(m_HostRulesBrainlessCombo, m_HostOptionsDraft.brainlessHumansSpectate ? 0 : 1);
	const int rulesTeam = std::clamp<int>(m_HostRulesTeamCombo ? m_HostRulesTeamCombo->GetSelectedIndex() : 0, 0, 3);
	const NetMatchTeamRules& teamRules = m_HostOptionsDraft.teamRules[rulesTeam];
	HostOptSelectComboIndex(m_HostRulesTeamCombo, rulesTeam);
	{
		const std::string tech = teamRules.technologyModule.empty() ? teamRules.technologyIntent
		                                                            : teamRules.technologyIntent + " - " + teamRules.technologyModule;
		HostOptSelectCombo(m_HostRulesTechCombo, tech);
	}
	if (m_HostRulesSkillSlider) m_HostRulesSkillSlider->SetValue(teamRules.aiSkill);
	if (m_HostRulesSkillValue) m_HostRulesSkillValue->SetText(std::to_string(teamRules.aiSkill));
	for (GUIControl* control : {static_cast<GUIControl*>(m_HostRulesActivityCombo), static_cast<GUIControl*>(m_HostRulesSceneCombo),
	                            static_cast<GUIControl*>(m_HostRulesModeCombo), static_cast<GUIControl*>(m_HostRulesDifficultySlider),
	                            static_cast<GUIControl*>(m_HostRulesGoldSlider), static_cast<GUIControl*>(m_HostRulesFogCheck),
	                            static_cast<GUIControl*>(m_HostRulesClearPathCheck), static_cast<GUIControl*>(m_HostRulesDeployCheck),
	                            static_cast<GUIControl*>(m_HostRulesBrainlessCombo), static_cast<GUIControl*>(m_HostRulesTeamCombo),
	                            static_cast<GUIControl*>(m_HostRulesTechCombo), static_cast<GUIControl*>(m_HostRulesSkillSlider)}) {
		HostOptSetEditable(control, editable);
	}

	// Network page.
	HostOptSelectComboIndex(m_HostNetPolicyCombo, m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Fixed ? 1 : 0);
	if (m_HostNetMinDelayBox && !HostOptBoxFocused(m_HostNetMinDelayBox)) {
		m_HostNetMinDelayBox->SetText(std::to_string(m_HostOptionsDraft.inputDelayFrames));
	}
	if (m_HostNetEffectiveLabel) {
		// H22: the floor in ticks and in milliseconds, then the announced per-sender figure.
		std::string effective = "Effective delay: " + std::to_string(m_HostOptionsDraft.inputDelayFrames) + " ticks (" +
		                        std::to_string(m_HostOptionsDraft.inputDelayFrames * 1000 / 60) + " ms)";
		if (m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Auto) effective += " (auto, follows ping)";
		if (!snapshot.inputDelayText.empty()) effective += " - " + snapshot.inputDelayText;
		m_HostNetEffectiveLabel->SetText(effective);
	}
	const bool fixedPolicy = m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Fixed;
	for (int peer = 0; peer < 4; ++peer) {
		const bool used = peer < capacity;
		if (m_HostNetPeerLabels[peer]) {
			m_HostNetPeerLabels[peer]->SetVisible(used);
			m_HostNetPeerLabels[peer]->SetText("Peer " + std::to_string(peer + 1));
		}
		if (m_HostNetPeerDelayBoxes[peer]) {
			m_HostNetPeerDelayBoxes[peer]->SetVisible(used);
			const uint16_t delay = peer < static_cast<int>(m_HostOptionsDraft.peerInputDelayFrames.size())
			                           ? m_HostOptionsDraft.peerInputDelayFrames[peer]
			                           : m_HostOptionsDraft.inputDelayFrames;
			if (!HostOptBoxFocused(m_HostNetPeerDelayBoxes[peer])) {
				m_HostNetPeerDelayBoxes[peer]->SetText(std::to_string(delay));
			}
			HostOptSetEditable(m_HostNetPeerDelayBoxes[peer], editable && fixedPolicy);
		}
	}
	HostOptSetEditable(m_HostNetPolicyCombo, editable);
	HostOptSetEditable(m_HostNetMinDelayBox, editable);
	// Recalculate means "re-sample the link for the automatic policy"; under Fixed the host's own
	// figures are the answer, so the button stays off there.
	HostOptSetEditable(m_HostNetRecalcButton, editable && !fixedPolicy);
	if (m_HostNetPingLabel) {
		std::string ping;
		for (const NetLobbyMember& m : snapshot.members) {
			if (m.peerId == snapshot.localPeerId || !m.connected || m.pingMs == 0) continue;
			ping += ping.empty() ? "" : ", ";
			ping += m.displayName + " " + std::to_string(m.pingMs) + "ms";
		}
		m_HostNetPingLabel->SetText(ping.empty() ? "No peer ping yet" : ping);
	}

	// Recovery page.
	if (m_HostRecRepairCheck) m_HostRecRepairCheck->SetCheck(m_HostOptionsDraft.automaticRepair ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	if (m_HostRecAutosaveCheck) m_HostRecAutosaveCheck->SetCheck(m_HostOptionsDraft.autosaveEnabled ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	if (m_HostRecAutosaveIntervalBox && !HostOptBoxFocused(m_HostRecAutosaveIntervalBox)) {
		m_HostRecAutosaveIntervalBox->SetText(std::to_string(m_HostOptionsDraft.autosaveIntervalSeconds));
	}
	HostOptSetEditable(m_HostRecAutosaveIntervalBox, editable && m_HostOptionsDraft.autosaveEnabled);
	HostOptSetEditable(m_HostRecRepairCheck, editable);
	HostOptSetEditable(m_HostRecAutosaveCheck, editable);
	if (m_HostRecLastSaveLabel) {
		// H28: the service exposes no last-autosave tick getter, so the observation is the local
		// filesystem's own newest .ccsave - the same place the [autosave] log line's file lands.
		// It is this peer's disk only; a client sees its own (usually empty) directory.
		const uint64_t nowMs = MenuClockMs();
		if (nowMs - m_HostLastSaveScanMs >= 1000) {
			m_HostLastSaveScanMs = nowMs;
			m_HostLastSaveText.clear();
			std::error_code dirError;
			const std::string dir = System::GetWorkingDirectory() + "Autosaves";
			std::string latest;
			std::filesystem::file_time_type latestTime{};
			for (const auto& entry : std::filesystem::directory_iterator(dir, dirError)) {
				if (!entry.is_regular_file() || entry.path().extension() != ".ccsave") continue;
				const auto written = entry.last_write_time(dirError);
				if (dirError) continue;
				if (latest.empty() || written > latestTime) {
					latestTime = written;
					latest = entry.path().filename().string();
				}
			}
			if (!latest.empty()) {
				// The name is <matchId>-<tick>.ccsave; the tick trails the last dash.
				const size_t dash = latest.rfind('-');
				const std::string tick = dash == std::string::npos ? "" : latest.substr(dash + 1, latest.size() - dash - 8);
				m_HostLastSaveText = tick.empty() || tick.find_first_not_of("0123456789") != std::string::npos
				                         ? "Last checkpoint: " + latest
				                         : "Last checkpoint: tick " + tick + " (" + latest + ")";
			}
		}
		m_HostRecLastSaveLabel->SetText(m_HostLastSaveText.empty()
		                                  ? (m_HostOptionsDraft.autosaveEnabled
		                                         ? "Checkpoint every " + std::to_string(m_HostOptionsDraft.autosaveIntervalSeconds) + " sim seconds - none saved yet"
		                                         : "No autosaves while this is off")
		                                  : m_HostLastSaveText);
	}
	if (m_HostRecWaitingLabel) {
		std::string waiting;
		for (const NetLobbyMember& m : snapshot.members) {
			if (m.reclaiming) waiting += (waiting.empty() ? "" : ", ") + m.displayName;
		}
		m_HostRecWaitingLabel->SetText(waiting.empty() ? "" : ("Waiting on: " + waiting));
	}
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostRepairNowButton], false); // repair now needs the match's snapshot path, which is an open seam

	// Files page: local paths, local retention, and the local status-widget preference.
	if (m_HostFilesSavePathLabel) {
		m_HostFilesSavePathLabel->SetText("Autosaves: " + (std::filesystem::path(System::GetWorkingDirectory()) / "Autosaves").generic_string());
	}
	if (m_HostFilesDiagPathLabel) {
		m_HostFilesDiagPathLabel->SetText("Diagnostics: " + (std::filesystem::path(System::GetWorkingDirectory()) / "Telemetry").generic_string());
	}
	HostOptSelectComboIndex(m_HostFilesWidgetCombo, static_cast<int>(g_SettingsMan.GetNetworkMatchStatusMode()));
	HostOptSetEditable(m_HostFilesWidgetCombo, true); // local preference, editable on every peer
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostFilesSaveDiagButton], true);

	// Session page.
	if (m_HostSessHostingLabel) {
		m_HostSessHostingLabel->SetText(m_HostOptionsSetupDraft
		                                  ? "Hosting: not open yet"
		                                  : ("Hosting: " + snapshot.serviceState + " - " + snapshot.lobbyPhase));
	}
	if (m_HostSessSeatsLabel) {
		int humans = 0;
		for (const NetMatchPlayerSlot& slot : m_HostOptionsDraft.players) humans += slot.cpu ? 0 : 1;
		m_HostSessSeatsLabel->SetText("Human seats: " + std::to_string(humans) + " of " + std::to_string(m_HostOptionsDraft.players.size()));
	}
	static const uint8_t idleValues[] = {0, 1, 5, 10, 20, 30, 45, 60};
	int idleIndex = 0;
	for (int i = 0; i < 8; ++i) {
		if (m_HostOptionsDraft.idleWaitMinutes == idleValues[i]) idleIndex = i;
	}
	HostOptSelectComboIndex(m_HostSessIdleCombo, idleIndex);
	HostOptSetEditable(m_HostSessIdleCombo, editable);
	if (m_HostSessIdleStateLabel) {
		// H31's countdown and reason: the configured window plus why it is running. The snapshot
		// carries no published deadline, so the live seconds stay the service's - the label names
		// the phase the peers are actually in instead of inventing a clock of its own.
		std::string idle = m_HostOptionsDraft.idleWaitMinutes == 0
		                       ? "Idle wait: Never - the lobby stays open"
		                       : "Idle wait: " + std::to_string(m_HostOptionsDraft.idleWaitMinutes) + " min";
		if (m_HostOptionsSetupDraft) {
			idle += " (applies once the lobby opens)";
		} else if (!snapshot.lobbyPhase.empty()) {
			idle += " - " + snapshot.lobbyPhase;
			if (!snapshot.statusText.empty()) idle += ": " + snapshot.statusText;
		}
		m_HostSessIdleStateLabel->SetText(idle);
	}
	// H11: the ban list is the L20 store's; until it lands the row reports an empty list and the
	// dialog's removal stays off, so the control is honest about the contract it needs.
	if (m_HostSessBannedLabel) {
		m_HostSessBannedLabel->SetText("0 banned this session");
	}
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostSessionBannedButton], true);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostSessionEndButton], editable && !m_HostOptionsSetupDraft);

	// The footer: Apply only when there is a change and the player may make one.
	bool dirty = false;
	if (m_HostOptionsSetupDraft) {
		dirty = !m_HostSetupOptions || static_cast<const NetMatchStandardRules&>(m_HostOptionsDraft) != static_cast<const NetMatchStandardRules&>(*m_HostSetupOptions)
		        || m_HostOptionsDraft.players != m_HostSetupOptions->players
		        || m_HostOptionsDraft.mode != m_HostSetupOptions->mode
		        || m_HostOptionsDraft.activityPreset != m_HostSetupOptions->activityPreset
		        || m_HostOptionsDraft.activityModule != m_HostSetupOptions->activityModule
		        || m_HostOptionsDraft.sceneName != m_HostSetupOptions->sceneName
		        || m_HostOptionsDraft.peerCount != m_HostSetupOptions->peerCount
		        || m_HostOptionsDraft.delayPolicy != m_HostSetupOptions->delayPolicy
		        || m_HostOptionsDraft.idleWaitMinutes != m_HostSetupOptions->idleWaitMinutes
		        || m_HostOptionsDraft.automaticRepair != m_HostSetupOptions->automaticRepair
		        || m_HostOptionsDraft.autosaveEnabled != m_HostSetupOptions->autosaveEnabled
		        || m_HostOptionsDraft.autosaveIntervalSeconds != m_HostSetupOptions->autosaveIntervalSeconds
		        || m_HostOptionsDraft.inputDelayFrames != m_HostSetupOptions->inputDelayFrames;
	} else {
		const NetMatchConfig adopted = g_NetMatchService.GetLobbyMatchConfig();
		dirty = !(m_HostOptionsDraft == adopted) && !(g_NetMatchService.GetPendingHostOptions() && m_HostOptionsDraft == *g_NetMatchService.GetPendingHostOptions());
	}
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostOptionsApplyButton], editable && dirty);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostOptionsDefaultsButton], editable);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostOptionsBackButton], true);
	if (m_HostOptionsStatusLabel->GetText().empty() && dirty) {
		m_HostOptionsStatusLabel->SetText("Unsaved changes");
	}
	// An open seat dialog re-reads the moderation view every frame: the reclaim seconds tick and a
	// new applicant shows without the host reopening it.
	RefreshHostSeatDialog();
}

void MainMenuGUI::DraftHostOptionsFromControls() {
	// Seats: team picks only; the kind column commits in ChangeHostSeatType the moment it moves,
	// so a row past the roster (the closed-seat placeholder) has nothing to read back.
	for (int row = 0; row < c_HostSeatRows && row < static_cast<int>(m_HostOptionsDraft.players.size()); ++row) {
		if (m_HostSeatTeamCombos[row] && m_HostSeatTeamCombos[row]->GetSelectedIndex() >= 0) {
			m_HostOptionsDraft.players[row].team = static_cast<uint8_t>(m_HostSeatTeamCombos[row]->GetSelectedIndex());
		}
	}
	if (m_HostOptionsSetupDraft && m_HostSeatPlayersCombo && m_HostSeatPlayersCombo->GetSelectedIndex() >= 0) {
		m_HostOptionsDraft.peerCount = static_cast<uint8_t>(NetMatchConfigUtil::c_MinPeerCount + m_HostSeatPlayersCombo->GetSelectedIndex());
	}
	// Rules.
	if (m_HostRulesActivityCombo && m_HostRulesActivityCombo->GetSelectedIndex() >= 0) {
		const int picked = m_HostRulesActivityCombo->GetSelectedIndex();
		if (picked < static_cast<int>(m_MultiplayerHostActivities.size())) {
			m_HostOptionsDraft.activityPreset = m_MultiplayerHostActivities[picked].first;
			m_HostOptionsDraft.activityModule = m_MultiplayerHostActivities[picked].second;
		} else if (const GUIListPanel::Item* item = m_HostRulesActivityCombo->GetItem(picked)) {
			m_HostOptionsDraft.activityPreset = item->m_Name;
		}
	}
	if (m_HostRulesSceneCombo && m_HostRulesSceneCombo->GetSelectedIndex() >= 0) {
		if (const GUIListPanel::Item* item = m_HostRulesSceneCombo->GetItem(m_HostRulesSceneCombo->GetSelectedIndex())) {
			m_HostOptionsDraft.sceneName = item->m_Name;
		}
	}
	if (m_HostRulesModeCombo) {
		static const NetMatchMode modes[] = {NetMatchMode::PvPSkirmish, NetMatchMode::CoopPvE, NetMatchMode::PvPvE};
		const int picked = m_HostRulesModeCombo->GetSelectedIndex();
		if (picked >= 0 && picked < 3) {
			m_HostOptionsDraft.mode = modes[picked];
			m_HostOptionsDraft.modePreset = NetMatchConfigUtil::ModeName(m_HostOptionsDraft.mode);
		}
	}
	if (m_HostRulesDifficultySlider) m_HostOptionsDraft.difficulty = static_cast<uint8_t>(std::clamp(m_HostRulesDifficultySlider->GetValue(), 0, 100));
	if (m_HostRulesGoldSlider) {
		const int slider = m_HostRulesGoldSlider->GetValue();
		m_HostOptionsDraft.startingGold = slider >= 31000 ? NetMatchConfigUtil::c_InfiniteGold
		                                                : static_cast<uint32_t>(std::clamp(slider, 0, static_cast<int>(NetMatchConfigUtil::c_MaxFiniteStartingGold)));
	}
	if (m_HostRulesFogCheck) m_HostOptionsDraft.fogOfWar = m_HostRulesFogCheck->GetCheck() == GUICheckbox::Checked;
	if (m_HostRulesClearPathCheck) m_HostOptionsDraft.requireClearPathToOrbit = m_HostRulesClearPathCheck->GetCheck() == GUICheckbox::Checked;
	if (m_HostRulesDeployCheck) m_HostOptionsDraft.deployUnits = m_HostRulesDeployCheck->GetCheck() == GUICheckbox::Checked;
	if (m_HostRulesBrainlessCombo && m_HostRulesBrainlessCombo->GetSelectedIndex() >= 0) {
		m_HostOptionsDraft.brainlessHumansSpectate = m_HostRulesBrainlessCombo->GetSelectedIndex() == 0;
	}
	const int rulesTeam = std::clamp<int>(m_HostRulesTeamCombo ? m_HostRulesTeamCombo->GetSelectedIndex() : 0, 0, 3);
	if (m_HostRulesTechCombo && m_HostRulesTechCombo->GetSelectedIndex() >= 0) {
		const int picked = m_HostRulesTechCombo->GetSelectedIndex();
		if (picked < static_cast<int>(m_HostOptionsTechModules.size())) {
			m_HostOptionsDraft.teamRules[rulesTeam].technologyIntent = m_HostOptionsTechModules[picked];
			m_HostOptionsDraft.teamRules[rulesTeam].technologyModule.clear();
		} else if (const GUIListPanel::Item* item = m_HostRulesTechCombo->GetItem(picked)) {
			m_HostOptionsDraft.teamRules[rulesTeam].technologyIntent = item->m_Name;
		}
	}
	if (m_HostRulesSkillSlider) m_HostOptionsDraft.teamRules[rulesTeam].aiSkill = static_cast<uint8_t>(std::clamp(m_HostRulesSkillSlider->GetValue(), 1, 100));

	// Network.
	if (m_HostNetPolicyCombo) {
		m_HostOptionsDraft.delayPolicy = m_HostNetPolicyCombo->GetSelectedIndex() == 1 ? NetMatchDelayPolicy::Fixed : NetMatchDelayPolicy::Auto;
	}
	if (m_HostNetMinDelayBox) {
		const long parsed = std::strtol(m_HostNetMinDelayBox->GetText().c_str(), nullptr, 10);
		m_HostOptionsDraft.inputDelayFrames = static_cast<uint16_t>(std::clamp<int>(static_cast<int>(parsed), 0, NetMatchConfigUtil::c_MaxInputDelayFrames));
	}
	m_HostOptionsDraft.peerInputDelayFrames.clear();
	for (int peer = 0; peer < 4 && peer < m_HostOptionsDraft.peerCount; ++peer) {
		if (!m_HostNetPeerDelayBoxes[peer]) continue;
		const long parsed = std::strtol(m_HostNetPeerDelayBoxes[peer]->GetText().c_str(), nullptr, 10);
		m_HostOptionsDraft.peerInputDelayFrames.push_back(static_cast<uint16_t>(std::clamp<int>(static_cast<int>(parsed), 0, NetMatchConfigUtil::c_MaxInputDelayFrames)));
	}
	if (m_HostOptionsDraft.delayPolicy != NetMatchDelayPolicy::Fixed) {
		m_HostOptionsDraft.peerInputDelayFrames.clear();
	}

	// Recovery.
	if (m_HostRecRepairCheck) m_HostOptionsDraft.automaticRepair = m_HostRecRepairCheck->GetCheck() == GUICheckbox::Checked;
	if (m_HostRecAutosaveCheck) m_HostOptionsDraft.autosaveEnabled = m_HostRecAutosaveCheck->GetCheck() == GUICheckbox::Checked;
	if (m_HostRecAutosaveIntervalBox) {
		const long parsed = std::strtol(m_HostRecAutosaveIntervalBox->GetText().c_str(), nullptr, 10);
		m_HostOptionsDraft.autosaveIntervalSeconds = static_cast<uint32_t>(std::clamp<long>(parsed, 0, NetMatchService::c_MaxAutosaveIntervalSeconds));
	}
	if (!m_HostOptionsDraft.autosaveEnabled) m_HostOptionsDraft.autosaveIntervalSeconds = 0;

	// Session: the idle combo's index maps onto the minutes table.
	static const uint8_t idleValues[] = {0, 1, 5, 10, 20, 30, 45, 60};
	if (m_HostSessIdleCombo && m_HostSessIdleCombo->GetSelectedIndex() >= 0) {
		m_HostOptionsDraft.idleWaitMinutes = idleValues[std::clamp(m_HostSessIdleCombo->GetSelectedIndex(), 0, 7)];
	}
}

void MainMenuGUI::RederiveHostOptionsRoster() {
	// Rebuild the seat list for the drafted capacity/mode, keeping the rules the panel edited.
	NetMatchServiceRequest request = HostRequestDraft();
	request.peerCount = m_HostOptionsDraft.peerCount;
	request.mode = m_HostOptionsDraft.mode;
	NetMatchStandardRules rules = static_cast<const NetMatchStandardRules&>(m_HostOptionsDraft);
	request.standardRules = rules;
	// The kind column's edits ride the rebuild: the humans and CPUs the draft seats now, not the
	// mode's own default split, so closing or CPU-filling a seat survives a mode pick.
	uint32_t humanSeats = 0, cpuSeats = 0;
	for (const NetMatchPlayerSlot& slot : m_HostOptionsDraft.players) {
		(slot.cpu ? cpuSeats : humanSeats)++;
	}
	request.humans = humanSeats;
	request.cpuSlots = cpuSeats;
	NetMatchConfig built;
	std::string error;
	if (NetMatchService::BuildMatchConfig(request, m_HostOptionsDraft.sessionId, built, &error)) {
		built.configRevision = m_HostOptionsDraft.configRevision;
		// Teams the panel already picked on the old roster carry over where the seat survives.
		for (size_t i = 0; i < built.players.size() && i < m_HostOptionsDraft.players.size(); ++i) {
			if (built.players[i].peerId == m_HostOptionsDraft.players[i].peerId) {
				built.players[i].team = m_HostOptionsDraft.players[i].team;
			}
		}
		m_HostOptionsDraft.players = std::move(built.players);
	} else if (m_HostOptionsStatusLabel) {
		// H13: the refused mode+roster pair says why on the panel, in the validator's own words.
		m_HostOptionsStatusLabel->SetText(error);
	}
}

void MainMenuGUI::ApplyHostOptions() {
	if (m_HostOptionsReadOnly) return;
	DraftHostOptionsFromControls();
	std::string error;
	if (!NetMatchConfigUtil::ValidateLocalAlpha(m_HostOptionsDraft, &error)) {
		m_HostOptionsStatusLabel->SetText(error);
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	if (m_HostOptionsSetupDraft) {
		m_HostSetupOptions = m_HostOptionsDraft;
		m_HostOptionsStatusLabel->SetText("Staged for the next lobby.");
	} else {
		if (!g_NetMatchService.SubmitHostOptions(m_HostOptionsBaseRevision, m_HostOptionsDraft, &error)) {
			m_HostOptionsStatusLabel->SetText(error);
			g_GUISound.BackButtonPressSound()->Play();
			return;
		}
		// In an open lobby the submission is a live republish: the peers' adopted configs move to
		// the next revision, and the status row reads their acks until it lands. In the rematch
		// (closed) lobby the same draft is the staged next-match config instead.
		m_HostOptionsAwaitedRevision = m_HostOptionsBaseRevision + 1;
		const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
		m_HostOptionsStatusLabel->SetText(snapshot.playedAMatch
		                                      ? "Options staged for the next match."
		                                      : "Waiting for peers to confirm the new options...");
	}
	g_GUISound.ButtonPressSound()->Play();
}

void MainMenuGUI::SaveHostOptionsDefaults() {
	DraftHostOptionsFromControls();
	// The settings twins hold the same fields; the draft's host-owned values become the defaults
	// every later lobby's request starts from.
	g_SettingsMan.SetNetworkHostDelayPolicy(m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Fixed
	                                          ? SettingsMan::NetworkHostDelayPolicy::Fixed : SettingsMan::NetworkHostDelayPolicy::Auto);
	g_SettingsMan.SetNetworkInputDelayFrames(m_HostOptionsDraft.inputDelayFrames);
	g_SettingsMan.SetNetworkHostIdleWaitMinutes(m_HostOptionsDraft.idleWaitMinutes);
	g_SettingsMan.SetNetworkHostAutoRepair(m_HostOptionsDraft.automaticRepair);
	g_SettingsMan.SetAutosaveSeconds(m_HostOptionsDraft.autosaveEnabled ? m_HostOptionsDraft.autosaveIntervalSeconds : 0);
	g_SettingsMan.SetBrainlessHumansSpectate(m_HostOptionsDraft.brainlessHumansSpectate);
	g_SettingsMan.UpdateSettingsFile();
	// The scalars above are this installation's preferences; the whole match intent - activity, site,
	// rules, capacity and each seat's team - goes to the versioned template a new lobby seeds from.
	std::string templateError;
	if (!NetHostDefaults::Save(NetHostDefaults::FromConfig(m_HostOptionsDraft), &templateError)) {
		m_HostOptionsStatusLabel->SetText(templateError);
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	m_HostOptionsStatusLabel->SetText("Saved as the host defaults.");
	g_GUISound.ItemChangeSound()->Play();
}

void MainMenuGUI::ChangeHostSeatType(int row, int typeIndex) {
	if (m_HostOptionsReadOnly || row < 0 || row >= c_HostSeatRows || typeIndex < 0 || typeIndex > 2) return;
	DraftHostOptionsFromControls();
	auto refuse = [this](const std::string& reason) {
		m_HostOptionsStatusLabel->SetText(reason);
		g_GUISound.BackButtonPressSound()->Play();
	};
	const size_t slots = m_HostOptionsDraft.players.size();
	const bool isPlaceholder = row >= static_cast<int>(slots);
	const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
	auto memberSeated = [&snapshot](uint8_t peerId) {
		if (peerId == 0) return false;
		for (const NetLobbyMember& m : snapshot.members) {
			if (m.peerId == peerId) return true;
		}
		return false;
	};
	// The smallest peer id the roster leaves unclaimed; 0 when the peer capacity is fully seated.
	auto freePeerId = [this]() -> uint8_t {
		for (uint8_t id = 1; id <= m_HostOptionsDraft.peerCount; ++id) {
			bool used = false;
			for (const NetMatchPlayerSlot& s : m_HostOptionsDraft.players) {
				if (!s.cpu && s.peerId == id) {
					used = true;
					break;
				}
			}
			if (!used) return id;
		}
		return 0;
	};
	// A CPU seat wants a team no human seat holds and no other CPU seat claims.
	auto freeCpuTeam = [this]() -> int {
		for (int team = 0; team < 4; ++team) {
			bool humanTeam = false, cpuTeam = false;
			for (const NetMatchPlayerSlot& s : m_HostOptionsDraft.players) {
				if (s.team != team) continue;
				(s.cpu ? cpuTeam : humanTeam) = true;
			}
			if (!humanTeam && !cpuTeam) return team;
		}
		return -1;
	};
	const auto currentType = [this, isPlaceholder](const NetMatchPlayerSlot& slot) {
		return isPlaceholder ? 1 : (slot.cpu ? 2 : 0);
	};
	if (isPlaceholder && typeIndex == 1) return; // Closing a seat that does not exist is a no-op.

	NetMatchConfig candidate = m_HostOptionsDraft;
	if (isPlaceholder) {
		// Opening the closed tail: a new seat joins the roster within the capacity the mode allows.
		if (typeIndex == 0) {
			const uint8_t peerId = freePeerId();
			if (peerId == 0) {
				return refuse("No free peer seat - raise the Players count first");
			}
			NetMatchPlayerSlot seat;
			seat.peerId = peerId;
			seat.team = 0;
			seat.cpu = false;
			seat.displayName = "Client " + std::to_string(peerId);
			candidate.players.push_back(seat);
		} else {
			const int team = freeCpuTeam();
			if (team < 0) {
				return refuse("No free team for a CPU seat");
			}
			NetMatchPlayerSlot seat;
			seat.peerId = 0;
			seat.team = static_cast<uint8_t>(team);
			seat.cpu = true;
			seat.displayName = "CPU";
			candidate.players.push_back(seat);
		}
	} else {
		const NetMatchPlayerSlot slot = candidate.players[row];
		const int kind = currentType(slot);
		if (typeIndex == kind) return;
		if (!slot.cpu && memberSeated(slot.peerId)) {
			// The rule the submission path enforces, surfaced at the row: a live holder's seat is
			// never reallocated by an options edit, whatever kind it becomes.
			return refuse("A seated player is never dropped by an options edit");
		}
		if (!slot.cpu && !m_HostOptionsSetupDraft) {
			// Every human slot in the adopted config is a seat the open lobby reserved for its peer,
			// claimed or not; SubmitHostOptions refuses its removal, so the row refuses it here with
			// the same reason rather than letting Apply surprise the host.
			const NetMatchConfig adopted = g_NetMatchService.GetLobbyMatchConfig();
			for (const NetMatchPlayerSlot& kept : adopted.players) {
				if (!kept.cpu && kept.peerId == slot.peerId) {
					return refuse("An open lobby's human seat stays open for its peer - close it after the match");
				}
			}
		}
		if (typeIndex == 1) {
			candidate.players.erase(candidate.players.begin() + row);
		} else if (typeIndex == 2) {
			const int team = freeCpuTeam();
			if (team < 0) {
				return refuse("No free team for a CPU seat - every team is taken");
			}
			candidate.players[row].peerId = 0;
			candidate.players[row].cpu = true;
			candidate.players[row].team = static_cast<uint8_t>(team);
			candidate.players[row].displayName = "CPU";
		} else {
			const uint8_t peerId = freePeerId();
			if (peerId == 0) {
				return refuse("No free peer seat - raise the Players count first");
			}
			candidate.players[row].peerId = peerId;
			candidate.players[row].cpu = false;
			candidate.players[row].team = 0;
			candidate.players[row].displayName = "Client " + std::to_string(peerId);
		}
	}
	std::string error;
	if (!NetMatchConfigUtil::ValidateLocalAlpha(candidate, &error)) {
		// H13: the validator's own words explain the refused roster - "cpu slot shares a human
		// team" tells the host which pick to move, not just that the pick is wrong.
		return refuse(error);
	}
	m_HostOptionsDraft.players = std::move(candidate.players);
	m_HostOptionsStatusLabel->SetText("Unsaved changes");
	g_GUISound.ItemChangeSound()->Play();
}

void MainMenuGUI::ShowHostSeatDetails(int row) {
	if (row < 0 || row >= static_cast<int>(m_HostOptionsDraft.players.size())) return;
	m_HostOptionsSeatRow = row;
	m_HostSeatDlgModerationRow = -1;
	RefreshHostSeatDialog();
	OpenMultiplayerDialog(m_HostSeatDialog, m_HostOptionsPanel);
	m_MainMenuButtons[MenuButton::HostSeatDialogCloseButton]->SetFocus();
}

void MainMenuGUI::RefreshHostSeatDialog() {
	if (m_ActiveDialogBox != m_HostSeatDialog || m_HostOptionsSeatRow < 0 ||
	    m_HostOptionsSeatRow >= static_cast<int>(m_HostOptionsDraft.players.size())) {
		return;
	}
	const NetMatchPlayerSlot& slot = m_HostOptionsDraft.players[m_HostOptionsSeatRow];
	m_HostSeatDlgName->SetText("Name: " + slot.displayName);
	m_HostSeatDlgSeat->SetText(slot.peerId == 0 ? "Seat: peerless" : ("Seat: peer " + std::to_string(slot.peerId)));
	m_HostSeatDlgTeam->SetText("Team: Team " + std::to_string(slot.team + 1));
	m_HostSeatDlgState->SetText(slot.cpu ? "Kind: CPU player" : "Kind: human seat");
	const NetMatchTeamRules& rules = m_HostOptionsDraft.teamRules[slot.team < 4 ? slot.team : 0];
	m_HostSeatDlgStatus->SetText("Tech: " + rules.technologyIntent +
	                             (rules.technologyModule.empty() ? "" : " - " + rules.technologyModule) +
	                             "  AI skill " + std::to_string(rules.aiSkill));

	// H04-H08: the host's moderation view holds the reclaim clock and the applicant list; the
	// dialog maps its roster row to that view by the lockstep peer id, never the list index.
	m_ModerationUx.Refresh(g_NetMatchService.GetModerationSeats());
	m_HostSeatDlgModerationRow = -1;
	if (slot.peerId != 0) {
		for (size_t i = 0; i < m_ModerationUx.RowCount(); ++i) {
			if (m_ModerationUx.GetRow(i).lockstepPeerId == slot.peerId) {
				m_HostSeatDlgModerationRow = static_cast<int>(i);
				break;
			}
		}
	}
	const bool host = !m_HostOptionsReadOnly && !m_HostOptionsSetupDraft && g_NetMatchService.IsHost();
	const NetModerationUx::Row* mrow = m_HostSeatDlgModerationRow >= 0 ? &m_ModerationUx.GetRow(m_HostSeatDlgModerationRow) : nullptr;
	if (mrow && (mrow->view.dropped || mrow->view.heldForReclaim || mrow->view.reclaiming)) {
		// H08's countdown is the snapshot's own figure: frames the round still holds, in seconds.
		const uint64_t seconds = NetSeatPresence::HoldSeconds(mrow->view.holdFramesRemaining);
		m_HostSeatDlgReclaim->SetText(mrow->view.holdFramesRemaining > 0
		                                  ? "Reclaim: seat held " + std::to_string(seconds) + "s for the original holder"
		                                  : "Reclaim: the hold has run out");
	} else {
		m_HostSeatDlgReclaim->SetText(mrow ? "Reclaim: seat in use" : "Reclaim: --");
	}
	if (mrow) {
		m_HostSeatDlgApplicants->SetText("Applicants: " + std::to_string(mrow->applicants));
		m_HostSeatDlgApplicant->SetText(mrow->applicantText);
		m_HostSeatDlgApplicant->SetEnabled(host && mrow->applicants > 1);
		HostOptSetEditable(m_HostSeatDlgWait, host && NetModerationUx::Available(*mrow, NetModerationAction::Wait));
		HostOptSetEditable(m_HostSeatDlgApprove, host && NetModerationUx::Available(*mrow, NetModerationAction::Substitute));
		HostOptSetEditable(m_HostSeatDlgCancel, host && NetModerationUx::Available(*mrow, NetModerationAction::Cancel));
	} else {
		m_HostSeatDlgApplicants->SetText("Applicants: none");
		m_HostSeatDlgApplicant->SetText("No applicant");
		m_HostSeatDlgApplicant->SetEnabled(false);
		HostOptSetEditable(m_HostSeatDlgWait, false);
		HostOptSetEditable(m_HostSeatDlgApprove, false);
		HostOptSetEditable(m_HostSeatDlgCancel, false);
	}
	// H09/H10: Kick and Ban are host powers over a peer's seat; the buttons stay pressable so the
	// click can say why nothing happened when the action cannot run.
	const bool humanSeat = host && !slot.cpu && slot.peerId != 0;
	m_HostSeatDlgKick->SetEnabled(humanSeat);
	m_HostSeatDlgBan->SetEnabled(humanSeat);
	if (m_HostSeatDlgActionHint->GetText().empty() || !humanSeat) {
		m_HostSeatDlgActionHint->SetText(humanSeat ? "Seat actions apply to this player."
		                                         : (slot.cpu ? "CPU seats are the host's to retype, not to moderate."
		                                            : "Moderation is the host's; clients watch."));
	}
}

void MainMenuGUI::ShowHostBannedDialog() {
	// H11: the list is the session ban store's. This branch has no store yet, so the dialog opens
	// on its empty state and the remove button stays off rather than pretending to act on one.
	m_HostBannedListLabel->SetText("(no banned players this session)");
	m_HostBannedStatusLabel->SetText("The session ban list lands with the moderation store.");
	m_MainMenuButtons[MenuButton::HostBannedRemoveButton]->SetEnabled(false);
	OpenMultiplayerDialog(m_HostBannedDialog, m_HostOptionsPanel);
	m_MainMenuButtons[MenuButton::HostBannedCloseButton]->SetFocus();
}

void MainMenuGUI::HandleHostOptionsInputEvents(const GUIControl* guiEventControl) {
	for (int i = 0; i < c_HostOptionsPageCount; ++i) {
		if (guiEventControl == m_HostOptionsTabs[i]) {
			// The tab un-pushes after a pick; commit the page's text boxes before it hides.
			DraftHostOptionsFromControls();
			ShowHostOptionsPage(i);
			g_GUISound.ItemChangeSound()->Play();
			return;
		}
	}
	for (int row = 0; row < c_HostSeatRows; ++row) {
		if (guiEventControl == m_HostSeatDetailsButtons[row]) {
			ShowHostSeatDetails(row);
			return;
		}
		if (guiEventControl == m_HostSeatTypeCombos[row]) {
			ChangeHostSeatType(row, m_HostSeatTypeCombos[row]->GetSelectedIndex());
			return;
		}
		if (guiEventControl == m_HostSeatTeamCombos[row] && row < static_cast<int>(m_HostOptionsDraft.players.size())) {
			m_HostOptionsDraft.players[row].team = static_cast<uint8_t>(m_HostSeatTeamCombos[row]->GetSelectedIndex());
			return;
		}
	}
	// The seat dialog's moderation row: applicant cycling and the three live actions all go through
	// the shared model, so a click here is the same selection the seats panel would send.
	if (guiEventControl == m_HostSeatDlgApplicant) {
		if (m_HostSeatDlgModerationRow >= 0) {
			m_ModerationUx.CycleApplicant(m_HostSeatDlgModerationRow);
		}
		return;
	}
	if (guiEventControl == m_HostSeatDlgWait || guiEventControl == m_HostSeatDlgApprove || guiEventControl == m_HostSeatDlgCancel) {
		if (m_HostSeatDlgModerationRow >= 0) {
			const NetModerationAction action = guiEventControl == m_HostSeatDlgWait    ? NetModerationAction::Wait
			                                   : guiEventControl == m_HostSeatDlgApprove ? NetModerationAction::Substitute
			                                                                             : NetModerationAction::Cancel;
			m_ModerationUx.Act(m_HostSeatDlgModerationRow, action);
			m_HostSeatDlgActionHint->SetText(m_ModerationUx.GetStatusText());
		}
		return;
	}
	if (guiEventControl == m_HostSeatDlgKick || guiEventControl == m_HostSeatDlgBan) {
		// L20 contract: NetMatchService::RemoveParticipant / BanSession return Queued while the
		// lobby is Starting and drain through the host pump; until that store lands the click names
		// the seam instead of faking the action.
		m_HostSeatDlgActionHint->SetText(guiEventControl == m_HostSeatDlgKick
		                                     ? "Kick needs the session store (lands with the moderation seam)."
		                                     : "Ban needs the session store (lands with the moderation seam).");
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostSessionBannedButton]) {
		ShowHostBannedDialog();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostBannedCloseButton]) {
		CloseMultiplayerDialog();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostBannedRemoveButton]) {
		// L20 contract: UnbanParticipant drains through the same host pump; nothing to remove while
		// the store is absent.
		m_HostBannedStatusLabel->SetText("Unban lands with the moderation store.");
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostOptionsBackButton]) {
		// Back is local navigation only: the session is never touched by leaving the panel.
		DraftHostOptionsFromControls();
		m_MultiplayerSubScreen = m_HostOptionsSetupDraft ? MultiplayerSubScreen::HostSetup : MultiplayerSubScreen::Lobby;
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostOptionsApplyButton]) {
		ApplyHostOptions();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostOptionsDefaultsButton]) {
		SaveHostOptionsDefaults();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostSeatDialogCloseButton]) {
		m_HostOptionsSeatRow = -1;
		m_HostSeatDlgModerationRow = -1;
		CloseMultiplayerDialog();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostSessionEndButton]) {
		// Same end the lobby's Leave runs; the panel closes with it.
		g_NetMatchService.Destroy();
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostFilesSaveDiagButton]) {
		if (TelemetryBundle::RequestCapture()) {
			m_HostFilesDiagResultLabel->SetText("Saving diagnostics...");
		}
		return;
	}
	if (guiEventControl == m_HostRulesTeamCombo) {
		// Switching the team re-reads its rules row; the prior pick is already in the draft.
		DraftHostOptionsFromControls();
		return;
	}
	if (guiEventControl == m_HostSeatPlayersCombo) {
		DraftHostOptionsFromControls();
		RederiveHostOptionsRoster();
		return;
	}
	if (guiEventControl == m_HostRulesModeCombo) {
		DraftHostOptionsFromControls();
		RederiveHostOptionsRoster();
		return;
	}
	if (guiEventControl == m_HostFilesWidgetCombo) {
		g_SettingsMan.SetNetworkMatchStatusMode(static_cast<SettingsMan::NetworkMatchStatusMode>(std::clamp(m_HostFilesWidgetCombo->GetSelectedIndex(), 0, 2)));
		g_SettingsMan.UpdateSettingsFile();
		return;
	}
	if (guiEventControl == m_HostNetPolicyCombo) {
		// The per-peer boxes only exist under Fixed; the policy flip redraws their state.
		m_HostOptionsDraft.delayPolicy = m_HostNetPolicyCombo->GetSelectedIndex() == 1 ? NetMatchDelayPolicy::Fixed : NetMatchDelayPolicy::Auto;
		return;
	}
	if (guiEventControl == m_HostNetRecalcButton) {
		// The auto policy already re-derives each sender's figure from the live link; the button is
		// the host's "look again now" - the readouts re-fill from the service snapshot this frame.
		m_HostOptionsStatusLabel->SetText("Link figures refreshed from the live snapshot.");
		g_GUISound.ItemChangeSound()->Play();
		return;
	}
	// Every other editable control marks the draft; DraftHostOptionsFromControls reads them on Apply.
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
	request.activityModule = "Base.rte";
	if (host && m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size()) {
		// The host's picker names both fields, so the lobby never resolves a bare preset name.
		request.activityPreset = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].first;
		request.activityModule = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].second;
		for (const NetHostActivityChoice& row: g_NetMatchService.ListHostActivities()) {
			if (row.preset == request.activityPreset && row.module == request.activityModule) {
				request.activityType = row.activityType;
				break;
			}
		}
	}
	if (host && m_MultiplayerHostSceneIndex < m_MultiplayerHostScenes.size()) {
		request.sceneName = m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex].first;
		request.sceneModule = m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex].second;
	}
	if (host) {
		NetMatchService::ApplyHostActivityFallback(request);
	}
	request.ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
	// Headed matches self-heal: a desync (or a rejoiner) reloads everyone from the host's snapshot.
	request.resyncOnDesync = true;
	// The setup screen's staged options draft overrides every field the request carries.
	if (host && m_HostSetupOptions) {
		request.standardRules = static_cast<const NetMatchStandardRules&>(*m_HostSetupOptions);
		// BuildMatchConfig reads the mode off the request, not the rules block it carries inside.
		request.mode = m_HostSetupOptions->mode;
		request.delayPolicy = m_HostSetupOptions->delayPolicy;
		request.idleWaitMinutes = m_HostSetupOptions->idleWaitMinutes;
		request.automaticRepair = m_HostSetupOptions->automaticRepair;
		request.autosaveSeconds = m_HostSetupOptions->autosaveEnabled ? m_HostSetupOptions->autosaveIntervalSeconds : 0;
		request.peerCount = m_HostSetupOptions->peerCount;
		request.inputDelayFrames = m_HostSetupOptions->inputDelayFrames;
		request.autoInputDelay = m_HostSetupOptions->delayPolicy == NetMatchDelayPolicy::Auto;
		uint32_t cpuSeats = 0;
		for (const NetMatchPlayerSlot& slot : m_HostSetupOptions->players) cpuSeats += slot.cpu ? 1 : 0;
		request.cpuSlots = cpuSeats;
	}
	// The host picks the roster size and the lockstep input-delay buffer; clients adopt both via
	// the lobby config sync. The delay box writes back to the setting so the choice persists.
	if (host && !m_HostSetupOptions) {
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
	// The options panel opened from the lobby survives a publish tick like Moderation does; one
	// opened from the setup screen outlives the service itself, being the draft of the next one.
	if (inMatchOrLobby && m_MultiplayerSubScreen != MultiplayerSubScreen::Moderation && m_MultiplayerSubScreen != MultiplayerSubScreen::HostOptions) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else if (!inMatchOrLobby && (m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby || m_MultiplayerSubScreen == MultiplayerSubScreen::Moderation ||
	            (m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions && !m_HostOptionsSetupDraft))) {
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
	m_HostOptionsPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions);
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
		if (m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions) {
			// The options panel's own geometry: 545x246 at every size, like the design's H01 frame.
			RefreshHostOptionsControls(snapshot);
			constexpr int panelHeight = 246;
			FitMultiplayerScreen(545, panelHeight + m_MainMenuButtons[MenuButton::BackToMainButton]->GetHeight() + 5);
			LayoutMultiplayerFooter(545, panelHeight);
			return;
		}
		int contentWidth = 300;
		int contentHeight = 250;
		if (m_MultiplayerSubScreen == MultiplayerSubScreen::HostSetup && m_MultiplayerHostPanel) {
			contentHeight = m_MultiplayerHostPanel->GetHeight();
		}
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
	const int contentWidth = 300;
	const int rowBoxWidth = contentWidth - 24;
	bool addressOnOwnRow = false;
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
			if (!s_ShareResolved) {
				s_ShareAddress = NetLanDiscovery::GetPrimaryLocalAddress();
				s_ShareResolved = true;
			}
			if (s_ShareAddress.empty()) {
				m_MultiplayerStatusLabel->SetText("Waiting for a player to join...");
			} else {
				const std::string address = s_ShareAddress + ":" + m_MultiplayerHostPortTextBox->GetText();
				std::string prose = "Waiting for a player to join... share";
				// The status label's skin font is FontLarge; draw and measure the share row in FontSmall.
				if (m_MultiplayerLobbyPlayerRowFallbackFont) {
					m_MultiplayerStatusLabel->EnsureDrawableTextFont("FontSmall.png");
					m_MultiplayerStatusLabel->SetFont(m_MultiplayerLobbyPlayerRowFallbackFont);
				}
				if (GUIFont* font = m_MultiplayerLobbyPlayerRowFallbackFont) {
					if (font->CalculateWidth(prose) > rowBoxWidth) {
						while (!prose.empty() && font->CalculateWidth(prose + "...") > rowBoxWidth) {
							prose.pop_back();
						}
						prose += "...";
					}
				}
				m_MultiplayerStatusLabel->SetText(prose + "\n" + address);
				addressOnOwnRow = true;
				const bool addressScrolls = m_MultiplayerStatusLabel->GetMaxWordWidth() > rowBoxWidth;
				m_MultiplayerStatusLabel->SetHorizontalOverflowScroll(addressScrolls);
				m_MultiplayerStatusLabel->ActivateDeactivateOverflowScroll(addressScrolls);
			}
		} else {
			s_ShareResolved = false;
			if (connectedCount < snapshot.members.size()) {
				m_MultiplayerStatusLabel->SetText("Waiting for players to join... (" + std::to_string(connectedCount) + "/" + std::to_string(snapshot.members.size()) + ")");
			} else {
				m_MultiplayerStatusLabel->SetText("Waiting for everyone to ready up...");
			}
		}
	} else {
		s_ShareResolved = false;
		m_MultiplayerStatusLabel->SetText(snapshot.statusText);
	}
	if (!addressOnOwnRow) {
		m_MultiplayerStatusLabel->SetHorizontalOverflowScroll(false);
		m_MultiplayerStatusLabel->ActivateDeactivateOverflowScroll(false);
	}
	// The lobby panel is one width on every peer: a longer row or status elides inside its box
	// rather than widening the panel, so host and client land on the same rectangle.
	if (!addressOnOwnRow && m_MultiplayerLobbyPlayerRowFont) {
		m_MultiplayerStatusLabel->SetFont(m_MultiplayerLobbyPlayerRowFont);
		std::string statusText = m_MultiplayerStatusLabel->GetText();
		while (!statusText.empty() &&
		       m_MultiplayerLobbyPlayerRowFont->CalculateWidth(statusText + "...", m_MultiplayerLobbyPlayerRowFallbackFont) > rowBoxWidth) {
			statusText.pop_back();
		}
		if (statusText != m_MultiplayerStatusLabel->GetText()) {
			m_MultiplayerStatusLabel->SetText(statusText + "...");
		}
	}
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
	// Leave 100 + Seats 82 + 8 matches Start Match's 190: Leave keeps the wider half as the exit.
	GUIButton* leave = m_MainMenuButtons[MenuButton::MultiplayerLeaveButton];
	GUIButton* seats = m_MainMenuButtons[MenuButton::MultiplayerModerateButton];
	GUIButton* options = m_MainMenuButtons[MenuButton::MultiplayerLobbyOptionsButton];
	constexpr int pairGap = 8;
	if (leave->GetWidth() != 90) {
		leave->Resize(90, leave->GetHeight());
	}
	if (seats->GetWidth() != 74) {
		seats->Resize(74, seats->GetHeight());
	}
	if (options->GetWidth() != 74) {
		options->Resize(74, options->GetHeight());
	}
	const int pairLeft = (contentWidth - leave->GetWidth() - pairGap - seats->GetWidth() - pairGap - options->GetWidth()) / 2;
	leave->SetPositionRel(pairLeft, 236 + extraHeight);
	LayoutMultiplayerFooter(contentWidth, contentHeight);

	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetVisible(!snapshot.isHost);
	m_MainMenuButtons[MenuButton::MultiplayerReadyButton]->SetEnabled(!snapshot.isHost && snapshot.inLobby);
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetVisible(snapshot.isHost);
	// Start only once the remote peer is actually ready, not merely present.
	m_MainMenuButtons[MenuButton::MultiplayerStartButton]->SetEnabled(snapshot.isHost && snapshot.inLobby && snapshot.remoteReady);
	m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]->SetEnabled(true);
	// §9b: moderation is a match feature - a lobby seat whose holder leaves goes straight back in the pool.
	seats->SetPositionRel(pairLeft + leave->GetWidth() + pairGap, 236 + extraHeight);
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton]->SetVisible(snapshot.isHost);
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton]->SetEnabled(snapshot.isHost && snapshot.running);
	// Options sits after Seats on the same row: the host's edit surface, the client's details view.
	// A client sees no Seats button, so Options closes up next to Leave instead of floating.
	options->SetPositionRel(pairLeft + leave->GetWidth() + pairGap + (snapshot.isHost ? seats->GetWidth() + pairGap : 0), 236 + extraHeight);
	options->SetText(snapshot.isHost ? "Options" : "Details");
	options->SetEnabled(snapshot.inLobby);
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
	else if (m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions) m_MainMenuButtons[MenuButton::HostOptionsBackButton]->SetFocus();
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

void MainMenuGUI::AutomationSetShareAddress(const std::string& address) {
	s_ShareAddress = address;
	s_ShareResolved = true;
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
		case MultiplayerSubScreen::HostOptions: return "HostOptions";
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
