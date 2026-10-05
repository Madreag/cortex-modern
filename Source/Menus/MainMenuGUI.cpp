#include "MainMenuGUI.h"
#include "NetHostOptionsText.h"
#include "NetPlayerPresentation.h"

#include "WindowMan.h"
#include "FrameMan.h"
#include "FrameRecorder.h"
#include "MenuMan.h"
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
#include "NetProtocol.h"
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
#include <set>
#include <tuple>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <format>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

using namespace RTE;

// The game's version, then the multiplayer build's own version and the network protocol it speaks.
static std::string VersionLine() {
	const std::string& build = System::GetBuildVersion();
	return "v" + c_GameVersion.str() + (build.empty() ? std::string() : ", multiplayer " + build + " (protocol " + std::to_string(NetProtocol::c_Version) + ")");
}

static std::string PlayerFacingStatus(const std::string& text) {
	const bool wireReason = text.find("ParticipantBanned") != std::string::npos || text.find("participant_identity: admitted vs banned") != std::string::npos;
	const std::string refusalPrefix = "A player could not join: ";
	const bool refusal = text.starts_with("A player could not join:");
	// The host's notice names the refused player ahead of the sentence it quotes: "<name> is banned from this session".
	const auto refusedName = [&](const std::string& sentence) {
		const size_t at = text.find(sentence);
		return at != std::string::npos && at > refusalPrefix.size() && text.starts_with(refusalPrefix) ? text.substr(refusalPrefix.size(), at - refusalPrefix.size()) : std::string();
	};
	if (text.find("removed from this session") != std::string::npos) {
		// The host's notice never speaks to the player it removed, and the player reads a whole sentence.
		if (refusal) {
			const std::string name = refusedName(" was removed from this session");
			return name.empty() ? "A removed player was refused." : name + " was refused: removed from this session.";
		}
		return "The host removed you from this session";
	}
	if (wireReason || text.find("banned") != std::string::npos) {
		// The host's notice wraps the refused player's own sentence, which is written to that player.
		if (refusal) {
			const std::string name = refusedName(" is banned from this session");
			return name.empty() ? "A banned player was refused." : name + " was refused: banned from this session.";
		}
		return wireReason ? "You are banned from this session" : text;
	}
	if (text.find("transport stopped") != std::string::npos || text == "Connection dropped") return "The host's connection was lost.";
	return text;
}

// Why a listed game cannot be joined, in the player's words.
static std::string GameRowReasonWords(const std::string& reason) {
	if (reason.empty()) return {};
	if (reason == "full") return "Full";
	if (reason == "modules") return "Different mods";
	if (reason == "address") return "No usable address";
	return "Different version";
}

// The identity a selection keeps across refreshes: a game, not a row number.
static std::string GameRowKey(const NetDirectoryClient::GameRow& row) {
	return row.source + "|" + row.sessionId + "|" + row.name + "|" + row.address + ":" + std::to_string(row.port);
}

static std::string FitDiscoveredGameRow(const NetDirectoryClient::GameRow& row, GUIFont* font, int width) {
	if (!font || row.persistentWorld) return NetDirectoryClient::DescribeGameRow(row);
	std::string name = row.name, activity = row.activity + (row.scene.empty() ? std::string() : " on " + row.scene);
	const std::string suffix = " - " + row.players + " - " + (row.source == "LAN" ? "This network" : "Internet") + (row.joinable ? std::string() : " - " + GameRowReasonWords(row.reason));
	const auto compose = [&] { return name + " - " + activity + suffix; };
	const auto shorten = [](std::string& value) {
		const size_t dots = value.find("...");
		if (dots == std::string::npos) {
			if (value.size() <= 3) { value.clear(); return; }
			value.replace((value.size() - 3) / 2, 3, "...");
		} else if (value.size() <= 4) value.clear();
		else value.erase(dots > 0 ? dots - 1 : dots + 3, 1);
	};
	while (!activity.empty() && font->CalculateWidth(compose()) > width) shorten(activity);
	while (!name.empty() && font->CalculateWidth(compose()) > width) shorten(name);
	return compose();
}

// Windows-1252 for one Unicode codepoint; 0xA0-0xFF match Latin-1.
/// Sets a path on one line of the label; one that does not fit keeps its root and as many of its last folders as fit, the middle elided.
static void SetFittedPath(GUILabel* label, const std::string& prefix, const std::filesystem::path& path) {
	const std::string full = path.generic_string();
	label->SetText(prefix + full);
	if (label->GetTextWidth() <= label->GetWidth()) return;
	std::vector<std::string> parts;
	for (size_t start = 0; start <= full.size();) {
		const size_t end = std::min(full.find('/', start), full.size());
		if (end > start) parts.push_back(full.substr(start, end - start));
		start = end + 1;
	}
	for (size_t kept = parts.size() > 1 ? parts.size() - 1 : 1; kept > 0; --kept) {
		std::string tail;
		for (size_t index = parts.size() - kept; index < parts.size(); ++index) tail += "/" + parts[index];
		label->SetText(prefix + (parts.size() > kept ? parts.front() + "/..." : std::string()) + tail);
		if (label->GetTextWidth() <= label->GetWidth()) return;
	}
	label->SetText(prefix + ".../" + parts.back());
}

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

/// A host on this machine beacons from loopback as well as its LAN interface.
static bool IsLoopbackAddress(const std::string& address) {
	return address == "::1" || address.starts_with("127.");
}

/// A seat the roster has not named yet shows the local player their own name, never the internal default.
static std::string LobbyRowName(const NetLobbyMember& member, const std::string& localName) {
	if (!member.isLocal || localName.empty()) {
		return NetPlayerPresentation::Name(member);
	}
	const bool unnamed = member.displayName.empty() || member.displayName == "Client " + std::to_string(member.peerId);
	return unnamed ? localName : NetPlayerPresentation::Name(member);
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
	m_MultiplayerLobbyChatLabels.fill(nullptr);
	m_MultiplayerLobbyChatInput = nullptr;
	m_MultiplayerLobbyVersionLabel = nullptr;
	m_MultiplayerLobbyChatLines.clear();

	m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
	m_ReconnectStatusShown.clear();
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
	m_VersionLabel->SetText("Community Project\n" + VersionLine());
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
	m_MainMenuButtons[MenuButton::MultiplayerResumeGameButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerResumeGame"));
	m_MainMenuButtons[MenuButton::ResumeStartButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonResumeStart"));
	m_MainMenuButtons[MenuButton::ResumeBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonResumeBack"));
	m_MultiplayerResumePanel = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("MultiplayerResumePanel"));
	m_ResumeMatchesList = dynamic_cast<GUIListBox*>(m_SubMenuScreenGUIControlManager->GetControl("ListResumeMatches"));
	m_ResumeSelectedLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelResumeSelected"));
	m_ResumeStatusLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelResumeStatus"));
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerReconnect"));
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerCancelReconnect"));
	m_MainMenuButtons[MenuButton::MultiplayerWaitSlotButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerWaitSlot"));
	m_MainMenuButtons[MenuButton::MultiplayerHostBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonHostBack"));
	m_MainMenuButtons[MenuButton::MultiplayerJoinBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonJoinBack"));
	m_MainMenuButtons[MenuButton::MultiplayerModerateButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonMultiplayerModerate"));
	m_MainMenuButtons[MenuButton::MultiplayerModerationBackButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonModerationBack"));
	m_MainMenuButtons[MenuButton::SaveDiagnosticsButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonSaveDiagnostics"));

	m_MultiplayerNameTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextMultiplayerName"));
	m_MultiplayerHostPortTextBox = dynamic_cast<GUITextBox*>(m_SubMenuScreenGUIControlManager->GetControl("TextHostPort"));
	m_MultiplayerHostPlayersCombo = dynamic_cast<GUIComboBox*>(m_SubMenuScreenGUIControlManager->GetControl("ComboHostPlayers"));
	m_MultiplayerHostAboutLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelHostActivityAbout"));
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
	m_JoinSelectedLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelJoinSelected"));
	m_JoinAddressDialog = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("JoinAddressDialog"));
	m_MainMenuButtons[MenuButton::JoinByAddressButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonJoinByAddress"));
	m_MainMenuButtons[MenuButton::JoinAddressGoButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonJoinAddressGo"));
	m_MainMenuButtons[MenuButton::JoinAddressCancelButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonJoinAddressCancel"));
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
	for (size_t row = 0; row < m_MultiplayerLobbyPlayerLabels.size(); ++row) {
		m_MultiplayerLobbyPlayerLabels[row] = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyPlayer" + std::to_string(row)));
	}
	m_MainMenuButtons[MenuButton::LobbyEditSetupButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonLobbyEditSetup"));
	m_LobbyLeaveDialog = dynamic_cast<GUICollectionBox*>(m_SubMenuScreenGUIControlManager->GetControl("LobbyLeaveDialog"));
	m_LobbyLeaveLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelLobbyLeave"));
	m_MainMenuButtons[MenuButton::LobbyLeaveStayButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonLobbyLeaveStay"));
	m_MainMenuButtons[MenuButton::LobbyLeaveConfirmButton] = dynamic_cast<GUIButton*>(m_SubMenuScreenGUIControlManager->GetControl("ButtonLobbyLeaveConfirm"));
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
	m_PageChatNotice = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->AddControl("LabelPageChatNotice", "LABEL", nullptr, 0, 0, 545, 12));
	if (m_PageChatNotice) {
		if (chatFont) m_PageChatNotice->SetFont(chatFont);
		m_PageChatNotice->SetHAlignment(GUIFont::Left);
		m_PageChatNotice->SetVAlignment(GUIFont::Middle);
		m_PageChatNotice->SetVisible(false);
	}
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
	m_MultiplayerLobbyVersionLabel = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->AddControl(
	    "LabelLobbyVersion", "LABEL", m_MultiplayerLobbyPanel, 8, 0, 284, 10));
	if (m_MultiplayerLobbyVersionLabel) {
		if (chatFont) m_MultiplayerLobbyVersionLabel->SetFont(chatFont);
		m_MultiplayerLobbyVersionLabel->SetVAlignment(GUIFont::Middle);
		m_MultiplayerLobbyVersionLabel->SetText(VersionLine());
		m_MultiplayerLobbyVersionLabel->SetVisible(false);
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
	m_MultiplayerJoinAddressTextBox->SetText("");
	m_MultiplayerJoinAddressTextBox->SetMaxTextLength(64);
	m_MultiplayerHostPortTextBox->SetText("41010");
	m_MultiplayerHostPortTextBox->SetNumericOnly(true);
	m_MultiplayerHostPortTextBox->SetMaxNumericValue(65535);
	m_MultiplayerHostPortTextBox->SetMaxTextLength(5);
	RefreshHostPlayersChoices();
	m_MultiplayerHostInputDelayTextBox->SetNumericOnly(true);
	m_MultiplayerHostInputDelayTextBox->SetMaxNumericValue(NetMatchConfigUtil::c_MaxInputDelayFrames);
	m_MultiplayerHostInputDelayTextBox->SetMaxTextLength(2);
	RefreshHostInputDelayControls();
	m_MultiplayerHostPortMapCheckbox->SetCheck(g_SettingsMan.GetNetworkPortMapEnable() ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	m_MultiplayerJoinPortTextBox->SetText("41010");
	m_JoinPortAutoValue = "41010";
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

void MainMenuGUI::OfferHostLeftLandingOnEntry() {
	if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen || g_NetMatchService.GetState() != NetMatchServiceState::Failed ||
	    g_NetMatchService.GetLobbySnapshot().errorText != "The host left the match") {
		return;
	}
	SetActiveMenuScreen(MenuScreen::MultiplayerScreen, false);
	m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
	m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus("The host left the match"));
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

void MainMenuGUI::JoinSelectedGame() {
	const int selected = m_MultiplayerLanGamesList ? m_MultiplayerLanGamesList->GetSelectedIndex() : -1;
	if (selected < 0 || static_cast<size_t>(selected) >= m_GameRows.size()) return;
	const NetDirectoryClient::GameRow& row = m_GameRows[static_cast<size_t>(selected)];
	if (!row.joinable) {
		m_JoinStatusText = "This game cannot be joined: " + GameRowReasonWords(row.reason) + ".";
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	m_MultiplayerJoinAddressTextBox->SetText(NetIceMenuJoinAddress(row));
	m_MultiplayerJoinPortTextBox->SetText(std::to_string(row.port == 0 ? 41010 : row.port));
	// The picked address and port stay a pair; the list no longer refills the port.
	m_JoinPortAutoValue.clear();
	m_JoinTargetName = row.name;
	m_JoinTargetPersistentWorld = row.persistentWorld || row.activity == "Persistent World";
	m_JoinTargetActivity = row.activity;
	if (m_JoinTargetPersistentWorld) {
		m_LastWorldJoinAddress = m_MultiplayerJoinAddressTextBox->GetText();
		m_LastWorldJoinPort = row.port == 0 ? 41010 : row.port;
		g_NetMatchService.NoteJoinTargetPersistentWorld(true);
	}
	StartMultiplayer(false);
}

void MainMenuGUI::ShowSetupFailure(bool host, const std::string& text) {
	if (host) {
		m_MultiplayerHostPickNotice = text;
		ApplyMultiplayerHostActivity();
		m_MultiplayerSubScreen = MultiplayerSubScreen::HostSetup;
	} else {
		m_JoinStatusText = text;
		m_JoinAttemptActive = false;
		m_MultiplayerSubScreen = MultiplayerSubScreen::JoinSetup;
	}
	g_GUISound.BackButtonPressSound()->Play();
}

void MainMenuGUI::AskToLeaveLobby() {
	if (!m_LobbyLeaveDialog) return;
	const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
	if (m_LobbyLeaveLabel) {
		m_LobbyLeaveLabel->SetText(snapshot.isHost ? "Leave and close this lobby?\nThe other players go back to the multiplayer menu."
		                                           : "Leave this lobby?\nThe host keeps the game open for the others.");
	}
	OpenMultiplayerDialog(m_LobbyLeaveDialog, m_MultiplayerLobbyPanel);
	g_GUISound.ButtonPressSound()->Play();
}

void MainMenuGUI::LeaveLobby() {
	// A join cancelled while the world's image comes goes back to where the player chose the match; the host frees the seat it offered.
	const bool cancelsTransfer = !g_NetMatchService.GetLobbySnapshot().transferLine.empty();
	g_NetMatchService.Destroy();
	m_MultiplayerSubScreen = cancelsTransfer ? MultiplayerSubScreen::JoinSetup : MultiplayerSubScreen::Landing;
	g_GUISound.BackButtonPressSound()->Play();
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
			m_HostSeatDlgRemovalSeat.reset();
			CloseMultiplayerDialog();
		} else if (m_ActiveDialogBox == m_HostBannedDialog) {
			CloseMultiplayerDialog();
		} else {
			LeaveHostOptions();
		}
		return;
	}
	if (m_ActiveMenuScreen == MenuScreen::MultiplayerScreen && !m_ActiveDialogBox && (backButtonPressed || g_UInputMan.KeyPressed(SDLK_ESCAPE))) {
		switch (m_MultiplayerSubScreen) {
			case MultiplayerSubScreen::Lobby:
				// An open lobby is the others' too: leaving it is asked, never done by a stray key.
				if (g_NetMatchService.GetState() != NetMatchServiceState::Idle) {
					AskToLeaveLobby();
				} else {
					m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
				}
				return;
			case MultiplayerSubScreen::Moderation:
				m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
				g_GUISound.BackButtonPressSound()->Play();
				return;
			case MultiplayerSubScreen::HostSetup:
			case MultiplayerSubScreen::ResumeSetup:
			case MultiplayerSubScreen::ReplayBrowser:
				m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
				g_GUISound.BackButtonPressSound()->Play();
				return;
			case MultiplayerSubScreen::JoinSetup:
				// A join on its way is the player's alone: going back cancels it and stays on the list.
				if (m_JoinAttemptActive) {
					g_NetMatchService.Destroy();
					m_JoinAttemptActive = false;
					m_JoinStatusText = "Join cancelled.";
				} else {
					m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
				}
				g_GUISound.BackButtonPressSound()->Play();
				return;
			default:
				break;
		}
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
		} else if (guiEvent.GetType() == GUIEvent::Notification && (guiEvent.GetControl() == m_MultiplayerLanGamesList || guiEvent.GetControl() == m_ReplayList ||
		                                                             (guiEvent.GetControl() == m_HostSeatDlgApplicantList && guiEvent.GetMsg() == GUIListPanel::Select))) {
			m_ListEventMsg = guiEvent.GetMsg();
			HandleMultiplayerScreenInputEvents(guiEvent.GetControl());
			m_ListEventMsg = -1;
		} else if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUIComboBox::Closed &&
		           (guiEvent.GetControl() == m_MultiplayerHostActivityCombo || guiEvent.GetControl() == m_MultiplayerHostSceneCombo ||
		            guiEvent.GetControl() == m_MultiplayerHostModeCombo || guiEvent.GetControl() == m_MultiplayerHostPlayersCombo)) {
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
		} else if (guiEvent.GetType() == GUIEvent::Notification &&
		           (guiEvent.GetMsg() == GUITextBox::Changed || guiEvent.GetMsg() == GUITextBox::Enter) &&
		           m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions &&
		           (guiEvent.GetControl() == m_HostNetSlowBoundBox || guiEvent.GetControl() == m_HostNetMinDelayBox ||
		            guiEvent.GetControl() == m_HostRecAutosaveIntervalBox ||
		            std::find(m_HostNetPeerDelayBoxes.begin(), m_HostNetPeerDelayBoxes.end(), guiEvent.GetControl()) != m_HostNetPeerDelayBoxes.end())) {
			// The refresh rewrites every box the player is not holding, so a typed row commits as it is
			// typed; waiting for Apply loses it the moment the click moves the focus. The Recovery
			// caption reads the same draft, so it follows the interval as it is typed too.
			DraftHostOptionsFromControls();
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
		StageSavedHostDefaults();
		m_MultiplayerSubScreen = MultiplayerSubScreen::HostSetup;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerJoinGameButton]) {
		m_MultiplayerLandingStatusLabel->SetText("");
		m_MultiplayerSubScreen = MultiplayerSubScreen::JoinSetup;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerResumeGameButton]) {
		m_MultiplayerLandingStatusLabel->SetText("");
		m_MultiplayerSubScreen = MultiplayerSubScreen::ResumeSetup;
		RefreshResumeList();
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_ResumeMatchesList) {
		RefreshResumeControls();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ResumeStartButton]) {
		StartSelectedResume();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::ResumeBackButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerJoinBackButton] && m_JoinAttemptActive) {
		g_NetMatchService.Destroy();
		m_JoinAttemptActive = false;
		m_JoinStatusText = "Join cancelled.";
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerHostBackButton] || guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerJoinBackButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerCreateButton]) {
		StartMultiplayer(true);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerConnectButton]) {
		JoinSelectedGame();
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
		RefreshHostPlayersChoices();
		SyncHostSetupDraft();
		ApplyMultiplayerHostActivity();
		g_GUISound.ItemChangeSound()->Play();
	} else if (guiEventControl == m_MultiplayerHostPlayersCombo) {
		const int selected = m_MultiplayerHostPlayersCombo->GetSelectedIndex();
		if (selected >= 0) m_MultiplayerHostPeerCount = static_cast<uint8_t>(NetMatchConfigUtil::c_MinPeerCount + selected);
		RefreshHostPlayersChoices();
		SyncHostSetupDraft();
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
		SyncHostSetupDraft();
		ApplyMultiplayerHostActivity();
		g_GUISound.ItemChangeSound()->Play();
	} else if (guiEventControl == m_MultiplayerHostSceneCombo) {
		const int selected = m_MultiplayerHostSceneCombo ? m_MultiplayerHostSceneCombo->GetSelectedIndex() : -1;
		if (selected >= 0 && static_cast<size_t>(selected) < m_MultiplayerHostScenes.size()) {
			m_MultiplayerHostSceneIndex = static_cast<size_t>(selected);
		}
		m_MultiplayerHostPickNotice.clear();
		SyncHostSetupDraft();
		ApplyMultiplayerHostActivity();
		g_GUISound.ItemChangeSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerReadyButton]) {
		// Ready, or the Ready taken back: the host sees either at once.
		g_NetMatchService.SetReady(!g_NetMatchService.IsReadyRequested());
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerStartButton]) {
		if (g_NetMatchService.GetLobbySnapshot().startCountdownRunning) {
			g_NetMatchService.CancelStart();
			g_GUISound.BackButtonPressSound()->Play();
		} else {
			g_NetMatchService.RequestStart();
			g_GUISound.ButtonPressSound()->Play();
		}
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerLeaveButton]) {
		LeaveLobby();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::LobbyLeaveStayButton]) {
		CloseMultiplayerDialog();
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::LobbyLeaveConfirmButton]) {
		CloseMultiplayerDialog();
		LeaveLobby();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::LobbyEditSetupButton]) {
		// The setup is ordinary: the host edits it here, everyone else reads the agreed one.
		OpenHostOptions(false);
		ShowHostOptionsPage(c_HostOptionsRulesPage);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerReconnectButton] &&
	           m_MultiplayerApplyOffered && g_NetMatchService.JoinRefusalOffer() != NetJoinRefusalOffer::None &&
	           g_NetMatchService.GetReconnectUx().GetOffer() != NetReconnectOffer::Available &&
	           !g_NetMatchService.GetReconnectUx().IsActive()) {
		// §9b: ask the host for a seat - the player's own held one when the host named it, else one whose holder is gone; the host decides.
		uint16_t ownSeat = 0;
		if (g_NetMatchService.JoinRefusalOffer(&ownSeat) == NetJoinRefusalOffer::OwnSeat) ApplyForOwnSeat(ownSeat);
		else ApplyToSubstitute();
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerWaitSlotButton] && m_MultiplayerApplyOffered &&
	           g_NetMatchService.JoinRefusalOffer() == NetJoinRefusalOffer::SlotsHeld) {
		WaitForSlot();
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]) {
		// §11's manual retry, and the same button that takes up the stored ticket after a relaunch.
		NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
		reconnect.RequestManualRetry(MenuClockMs());
		reconnect.DismissOffer();
		std::string rejoinError;
		if (!g_NetMatchService.BeginTicketRejoin(&rejoinError)) {
			reconnect.NoteAttemptFailed(MenuClockMs(), rejoinError);
			m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus(rejoinError));
		} else {
			reconnect.NoteAttemptStarted(MenuClockMs());
			m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
		}
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]) {
		// A cancel stops the automatic attempts; the recovery record survives it, so Rejoin still works.
		g_NetMatchService.GetReconnectUx().Cancel(MenuClockMs());
		g_NetMatchService.GetReconnectUx().DismissOffer();
		g_NetMatchService.GetReconnectUx().StopWatchingForHostReturn();
		m_MultiplayerApplyOffered = false;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerModerateButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Moderation;
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerModerationBackButton]) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerLobbyOptionsButton]) {
		// The host's Advanced: the same options, opened where the network tuning starts.
		OpenHostOptions(false);
		ShowHostOptionsPage(c_HostOptionsConnectionPage);
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::MultiplayerHostOptionsButton]) {
		// The setup screen's twin: the draft the next Create carries.
		OpenHostOptions(true);
	} else if (guiEventControl == m_MultiplayerLanGamesList) {
		// A click selects the game and shows it; Join Game or a double-click joins it.
		const int selected = m_MultiplayerLanGamesList->GetSelectedIndex();
		if (selected >= 0 && static_cast<size_t>(selected) < m_GameRows.size()) {
			m_SelectedGameKey = GameRowKey(m_GameRows[static_cast<size_t>(selected)]);
			if (m_ListEventMsg == GUIListPanel::DoubleClick) {
				m_JoinStatusText.clear();
				JoinSelectedGame();
			} else if (m_ListEventMsg == GUIListPanel::Select) {
				m_JoinStatusText.clear();
				g_GUISound.ItemChangeSound()->Play();
			}
		}
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::JoinByAddressButton]) {
		// The address the host gave: the box starts empty; a port a selected game named is kept.
		m_MultiplayerJoinAddressTextBox->SetText("");
		OpenMultiplayerDialog(m_JoinAddressDialog, m_MultiplayerJoinPanel);
		g_GUISound.ButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::JoinAddressCancelButton]) {
		CloseMultiplayerDialog();
		g_GUISound.BackButtonPressSound()->Play();
	} else if (guiEventControl == m_MainMenuButtons[MenuButton::JoinAddressGoButton]) {
		CloseMultiplayerDialog();
		m_JoinTargetName = m_MultiplayerJoinAddressTextBox->GetText();
		m_JoinTargetPersistentWorld = false;
		m_JoinTargetActivity.clear();
		StartMultiplayer(false);
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

void MainMenuGUI::FitClosedComboText(GUIComboBox* combo) {
	GUISkin* skin = m_SubMenuScreenGUIControlManager ? m_SubMenuScreenGUIControlManager->GetSkin() : nullptr;
	std::string fontName;
	if (!combo || !skin || !skin->GetValue("TextBox", "Font", &fontName)) {
		return;
	}
	GUIFont* font = skin->GetFont(fontName);
	if (!font) {
		return;
	}
	int kerning = 0, margin = 3;
	skin->GetValue("TextBox", "FontKerning", &kerning);
	skin->GetValue("TextBox", "WidthMargin", &margin);
	const int savedKerning = font->GetKerning();
	font->SetKerning(kerning);
	// The drop-down button covers 17 pixels of the panel the selected item reads in.
	const int room = combo->GetWidth() - 2 * margin - 17;
	const GUIListPanel::Item* item = combo->GetItem(combo->GetSelectedIndex());
	std::string text = item ? item->m_Name : combo->GetText();
	if (font->CalculateWidth(text) > room) {
		const size_t suffix = text.rfind(" - ");
		if (suffix != std::string::npos && font->CalculateWidth(text.substr(0, suffix)) <= room) {
			text = text.substr(0, suffix);
		} else {
			while (!text.empty() && font->CalculateWidth(text + "...") > room) {
				text.pop_back();
			}
			if (!text.empty()) {
				text += "...";
			}
		}
	}
	font->SetKerning(savedKerning);
	combo->SetText(text);
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
	FitClosedComboText(m_MultiplayerHostActivityCombo);
	if (m_MultiplayerHostSceneCombo) {
		fit(m_MultiplayerHostSceneCombo);
		FitClosedComboText(m_MultiplayerHostSceneCombo);
	}
	// The mode rows are short words, so the mode picker follows the activity picker's width.
	if (m_MultiplayerHostModeCombo) {
		m_MultiplayerHostModeCombo->SetPositionRel(valueX, m_MultiplayerHostModeCombo->GetRelYPos());
		if (m_MultiplayerHostModeCombo->GetWidth() != activityWidth) {
			m_MultiplayerHostModeCombo->Resize(activityWidth, m_MultiplayerHostModeCombo->GetHeight());
		}
	}
	if (m_MultiplayerHostPlayersCombo) {
		m_MultiplayerHostPlayersCombo->SetPositionRel(valueX, m_MultiplayerHostPlayersCombo->GetRelYPos());
	}
}

static std::string HostActivityType(const std::string& preset, const std::string& module);

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
		m_MultiplayerHostInfoLabel->SetText(m_MultiplayerHostPickNotice.empty() ? HostSummaryText() : m_MultiplayerHostPickNotice);
	}
	if (m_MultiplayerHostAboutLabel) {
		// The activity's own description, its first sentence, as one line under its name.
		std::string about;
		if (const Entity* activity = g_PresetMan.GetEntityPreset(HostActivityType(preset, module), preset, g_PresetMan.GetModuleID(module))) {
			about = activity->GetDescription();
			const size_t stop = about.find_first_of(".!?");
			if (stop != std::string::npos) about.resize(stop + 1);
		}
		m_MultiplayerHostAboutLabel->EnsureDrawableTextFont("FontSmall.png");
		if (m_MultiplayerLobbyPlayerRowFallbackFont) m_MultiplayerHostAboutLabel->SetFont(m_MultiplayerLobbyPlayerRowFallbackFont);
		if (GUIFont* font = m_MultiplayerLobbyPlayerRowFallbackFont; font && font->CalculateWidth(about) > m_MultiplayerHostAboutLabel->GetWidth()) {
			while (!about.empty() && font->CalculateWidth(about + "...") > m_MultiplayerHostAboutLabel->GetWidth()) about.pop_back();
			about += "...";
		}
		m_MultiplayerHostAboutLabel->SetText(about);
		m_MultiplayerHostAboutLabel->SetText(about);
	}
	// Selecting an item puts the whole name back in the closed box, so the line is refitted here.
	FitClosedComboText(m_MultiplayerHostActivityCombo);
	FitClosedComboText(m_MultiplayerHostSceneCombo);
}

// ---- §9.2/9.3 host options: six tabbed pages over the lobby, or the host setup's draft of the
// next one. The panel edits a complete NetMatchConfig draft; Apply stages it through the service. ----

void MainMenuGUI::CreateHostOptionsControls() {
	const auto get = [this](const char* name) { return m_SubMenuScreenGUIControlManager->GetControl(name); };
	m_HostOptionsPanel = dynamic_cast<GUICollectionBox*>(get("MultiplayerHostOptionsPanel"));
	m_HostOptionsTitle = dynamic_cast<GUILabel*>(get("LabelHostOptionsTitle"));
	static const char* tabNames[c_HostOptionsPageCount] = {"TabHostPageSeats", "TabHostPageRules", "TabHostPageConnection", "TabHostPageTiming", "TabHostPageRecovery", "TabHostPageFiles", "TabHostPageSession"};
	static const char* pageNames[c_HostOptionsPageCount] = {"CollectionBoxHostPageSeats", "CollectionBoxHostPageRules", "CollectionBoxHostPageConnection", "CollectionBoxHostPageTiming",
	                                                        "CollectionBoxHostPageRecovery", "CollectionBoxHostPageFiles", "CollectionBoxHostPageSession"};
	for (int i = 0; i < c_HostOptionsPageCount; ++i) {
		m_HostOptionsTabs[i] = dynamic_cast<GUITab*>(get(tabNames[i]));
		m_HostOptionsPages[i] = dynamic_cast<GUICollectionBox*>(get(pageNames[i]));
	}
	m_HostOptionsStatusLabel = dynamic_cast<GUILabel*>(get("LabelHostOptStatus"));
	// Two lines beside the buttons; a longer status scrolls through them rather than run out of the panel.
	if (m_HostOptionsStatusLabel) {
		m_HostOptionsStatusLabel->SetVAlignment(GUIFont::Top);
		m_HostOptionsStatusLabel->SetVerticalOverflowScroll(true);
		m_HostOptionsStatusLabel->ActivateDeactivateOverflowScroll(true);
	}
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
	m_HostNetSlowPolicyCombo = dynamic_cast<GUIComboBox*>(get("ComboHostNetSlowPolicy"));
	m_HostNetSlowBoundBox = dynamic_cast<GUITextBox*>(get("TextHostNetSlowBound"));
	m_HostNetSlowPolicyHintLabel = dynamic_cast<GUILabel*>(get("LabelHostNetSlowPolicyHint"));
	m_HostNetRedundancyCombo = dynamic_cast<GUIComboBox*>(get("ComboHostNetRedundancy"));
	m_HostNetMinDelayBox = dynamic_cast<GUITextBox*>(get("TextHostNetMinDelay"));
	m_HostNetEffectiveLabel = dynamic_cast<GUILabel*>(get("LabelHostNetEffective"));
	for (int peer = 0; peer < 4; ++peer) {
		const std::string n = std::to_string(peer + 1);
		m_HostNetPeerLabels[peer] = dynamic_cast<GUILabel*>(get(("LabelHostNetPeer" + n).c_str()));
		m_HostNetPeerDelayBoxes[peer] = dynamic_cast<GUITextBox*>(get(("TextHostNetPeerDelay" + n).c_str()));
	}
	m_HostNetPingLabel = dynamic_cast<GUILabel*>(get("LabelHostNetPing"));
	m_HostNetRecalcButton = dynamic_cast<GUIButton*>(get("ButtonHostNetRecalc"));
	m_HostNetModeLabel = dynamic_cast<GUILabel*>(get("LabelHostNetMode"));
	m_HostNetVisibilityCombo = dynamic_cast<GUIComboBox*>(get("ComboHostNetVisibility"));
	m_HostNetIceCombo = dynamic_cast<GUIComboBox*>(get("ComboHostNetIce"));
	m_HostNetIceHintLabel = dynamic_cast<GUILabel*>(get("LabelHostNetIceHint"));
	m_HostOptScopeLabel = dynamic_cast<GUILabel*>(get("LabelHostOptScope"));
	m_HostRulesDefaultsLabel = dynamic_cast<GUILabel*>(get("LabelHostRulesDefaults"));
	m_HostNetVisibilityHint = dynamic_cast<GUILabel*>(get("LabelHostNetVisibilityHint"));
	m_MainMenuButtons[MenuButton::HostOptionsRestoreButton] = dynamic_cast<GUIButton*>(get("ButtonHostOptRestore"));
	for (GUILabel* small : {m_HostOptScopeLabel, m_HostRulesDefaultsLabel, m_HostNetVisibilityHint}) {
		if (small) small->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
	}
	m_HostRelayCombo = dynamic_cast<GUIComboBox*>(get("ComboHostNetRelay"));
	m_HostRelayBoxes = {dynamic_cast<GUITextBox*>(get("TextHostRelayAddress")), dynamic_cast<GUITextBox*>(get("TextHostRelayUser")), dynamic_cast<GUITextBox*>(get("TextHostRelayPass"))};
	m_HostRelayLabels = {dynamic_cast<GUILabel*>(get("LabelHostRelayAddress")), dynamic_cast<GUILabel*>(get("LabelHostRelayUser")), dynamic_cast<GUILabel*>(get("LabelHostRelayPass"))};
	m_HostRelayHint = dynamic_cast<GUILabel*>(get("LabelHostRelayHint"));
	for (auto* box : m_HostRelayBoxes) if (box) box->SetMaxTextLength(1024);
	if (m_HostRelayBoxes[2]) m_HostRelayBoxes[2]->SetPasswordMask(true);
	if (m_HostRelayHint) m_HostRelayHint->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
	if (m_HostNetModeLabel) m_HostNetModeLabel->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
	if (m_HostNetEffectiveLabel) m_HostNetEffectiveLabel->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
	if (m_HostNetSlowPolicyHintLabel) m_HostNetSlowPolicyHintLabel->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
	if (auto* rangeHint = dynamic_cast<GUILabel*>(get("LabelHostRecAutosaveHint"))) rangeHint->SetText(NetAutosaveRangeHint());
	if (auto* autosaveNote = dynamic_cast<GUILabel*>(get("LabelHostRecAutosaveNote"))) {
		autosaveNote->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
		autosaveNote->SetText(NetAutosaveNote());
	}
	if (auto* autosaveCost = dynamic_cast<GUILabel*>(get("LabelHostRecAutosaveCost"))) {
		autosaveCost->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
		autosaveCost->SetText(NetAutosaveCostHint());
	}
	if (m_HostNetIceHintLabel) {
		m_HostNetIceHintLabel->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
	}
	m_HostNetPortBox = dynamic_cast<GUITextBox*>(get("TextHostNetPort"));
	m_HostRecRepairHintLabel = dynamic_cast<GUILabel*>(get("LabelHostRecRepairHint"));
	m_HostRecRepairCheck = dynamic_cast<GUICheckbox*>(get("CheckHostRecRepair"));
	m_HostRecAutosaveCheck = dynamic_cast<GUICheckbox*>(get("CheckHostRecAutosave"));
	m_HostRecAutosaveIntervalBox = dynamic_cast<GUITextBox*>(get("TextHostRecAutosaveInterval"));
	m_HostRecLastSaveLabel = dynamic_cast<GUILabel*>(get("LabelHostRecLastSave"));
	m_HostRecReturnWindowCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRecReturnWindow"));
	m_HostRecJoinHistoryCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRecJoinHistory"));
	m_HostRecJoinLagCombo = dynamic_cast<GUIComboBox*>(get("ComboHostRecJoinLag"));
	m_HostRecHintRowControls = {get("LabelHostRecReturnWindow"), m_HostRecReturnWindowCombo, get("LabelHostRecJoinHistory"), m_HostRecJoinHistoryCombo,
	                            get("LabelHostRecJoinLag"), m_HostRecJoinLagCombo};
	if ((m_HostRecOptionHintLabel = dynamic_cast<GUILabel*>(get("LabelHostRecOptionHint")))) {
		m_HostRecOptionHintLabel->SetFont(m_SubMenuScreenGUIControlManager->GetSkin()->GetFont("FontSmall.png"));
		m_HostRecOptionHintLabel->SetText(NetReturnWindowHint());
	}
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
	m_HostSeatDlgApplicantList = dynamic_cast<GUIListBox*>(get("ListHostSeatDlgApplicants"));
	m_HostSeatDlgWait = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgWait"));
	m_HostSeatDlgApprove = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgApprove"));
	m_HostSeatDlgCancel = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgCancel"));
	m_HostSeatDlgKick = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgKick"));
	m_HostSeatDlgBan = dynamic_cast<GUIButton*>(get("ButtonHostSeatDlgBan"));
	m_HostSeatDlgActionHint = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgActionHint"));
	m_HostSeatDlgStatus = dynamic_cast<GUILabel*>(get("LabelHostSeatDlgStatus"));
	m_HostBannedDialog = dynamic_cast<GUICollectionBox*>(get("HostBannedDialog"));
	m_HostBannedPick = dynamic_cast<GUIComboBox*>(get("ComboHostBannedPick"));
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
		m_HostNetPolicyCombo->AddItem("Automatic (default)");
		m_HostNetPolicyCombo->AddItem("Fixed");
	}
	if (m_HostNetSlowPolicyCombo) {
		m_HostNetSlowPolicyCombo->ClearList();
		// Only the default policy is offered: the others do not yet keep a dropped player's seat.
		m_HostNetSlowPolicyCombo->AddItem(NetSlowPlayerPolicyText(NetSlowPlayerPolicy::Substitute));
	}
	if (m_HostNetSlowBoundBox) {
		m_HostNetSlowBoundBox->SetNumericOnly(true);
		m_HostNetSlowBoundBox->SetMaxNumericValue(NetMatchConfigUtil::c_MaxSlowPlayerBoundTicks);
		m_HostNetSlowBoundBox->SetMaxTextLength(3);
	}
	if (m_HostNetRedundancyCombo) {
		m_HostNetRedundancyCombo->ClearList();
		for (int ticks = 1; ticks <= NetMatchConfigUtil::c_MaxFrameRedundancyTicks; ++ticks) {
			m_HostNetRedundancyCombo->AddItem(std::to_string(ticks) + " ticks" + (ticks == NetMatchConfigUtil::c_DefaultFrameRedundancyTicks ? " (default)" : ""));
		}
	}
	if (m_HostFilesWidgetCombo) {
		m_HostFilesWidgetCombo->ClearList();
		m_HostFilesWidgetCombo->AddItem("Off");
		m_HostFilesWidgetCombo->AddItem("When needed (default)");
		m_HostFilesWidgetCombo->AddItem("Always");
	}
	if (m_HostRecReturnWindowCombo) {
		m_HostRecReturnWindowCombo->ClearList();
		for (const int minutes : {1, 2, 5, 10, 15, 20, 30}) {
			m_HostRecReturnWindowCombo->AddItem(NetReturnWindowText(static_cast<uint8_t>(minutes)) + (minutes == NetMatchConfigUtil::c_DefaultReturnWindowMinutes ? " (default)" : ""));
		}
	}
	for (const auto& [combo, choices, standard]: {std::tuple{m_HostRecJoinHistoryCombo, std::vector<int>{60, 120, 180, 360, 600, 900, 1200, 1800}, 360},
	                                              std::tuple{m_HostRecJoinLagCombo, std::vector<int>{30, 60, 120, 300, 600, 1800}, 120}}) {
		if (!combo) continue;
		combo->ClearList();
		for (const int seconds: choices) combo->AddItem(NetJoinHistoryText(seconds) + (seconds == standard ? " (default)" : ""));
	}
	if (m_HostSessIdleCombo) {
		m_HostSessIdleCombo->ClearList();
		m_HostSessIdleCombo->AddItem("Never");
		for (int minutes : {1, 5, 10, 20, 30, 45, 60}) {
			m_HostSessIdleCombo->AddItem(std::to_string(minutes) + " minutes" + (minutes == 10 ? " (default)" : ""));
		}
	}
	if (m_HostNetMinDelayBox) {
		m_HostNetMinDelayBox->SetNumericOnly(true);
		m_HostNetMinDelayBox->SetMaxNumericValue(NetMatchConfigUtil::c_MaxInputDelayFrames);
		m_HostNetMinDelayBox->SetMaxTextLength(2);
	}
	if (m_HostNetVisibilityCombo) {
		m_HostNetVisibilityCombo->ClearList();
		m_HostNetVisibilityCombo->AddItem("Public (default)");
		m_HostNetVisibilityCombo->AddItem("Unlisted");
		m_HostNetVisibilityCombo->AddItem("Local discovery");
	}
	if (m_HostNetIceCombo) {
		m_HostNetIceCombo->ClearList();
		m_HostNetIceCombo->AddItem("On (default)");
		m_HostNetIceCombo->AddItem("Off");
	}
	if (m_HostNetPortBox) {
		m_HostNetPortBox->SetNumericOnly(true);
		m_HostNetPortBox->SetMaxNumericValue(65535);
		m_HostNetPortBox->SetMaxTextLength(5);
	}
	if (m_HostRelayCombo) {
		m_HostRelayCombo->ClearList();
		for (const char* state : {"Off", "Game service (default)", "Custom relay"}) m_HostRelayCombo->AddItem(state);
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
		m_HostRecAutosaveIntervalBox->SetMinNumericValue(NetMatchService::c_MinAutosaveIntervalSeconds);
		m_HostRecAutosaveIntervalBox->SetMaxTextLength(4);
	}
}

// The class that defines a host-pickable activity, empty when the list does not offer it.
static std::string HostActivityType(const std::string& preset, const std::string& module) {
	for (const NetHostActivityChoice& row : NetMatchService::ListHostActivities()) {
		if (row.preset == preset && row.module == module) return row.activityType;
	}
	return {};
}

// A host who staged no rules plays by the activity's own: the gold, fog of war, clear path and deployment the Scenario
// screen seeds for it. A request whose activity or scene does not resolve keeps today's path and its refusal.
static void SeedHostRulesFromActivity(NetMatchServiceRequest& request) {
	if (request.standardRules || request.persistentWorld || request.activityPreset == "Persistent World") return;
	if (!NetMatchService::SeatActivityModule(request) || !NetMatchService::SeatHostScene(request)) return;
	NetMatchStandardRules rules;
	rules.mode = request.mode;
	rules.activityPreset = request.activityPreset;
	rules.activityModule = request.activityModule;
	if (!request.activityType.empty()) rules.activityType = request.activityType;
	rules.sceneName = request.sceneName;
	if (!request.sceneModule.empty()) rules.sceneModule = request.sceneModule;
	if (NetActivitySetup::SeedRulesFromActivity(rules)) request.standardRules = rules;
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
		request.activityType = HostActivityType(request.activityPreset, request.activityModule);
	}
	if (m_MultiplayerHostSceneIndex < m_MultiplayerHostScenes.size()) {
		request.sceneName = m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex].first;
		request.sceneModule = m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex].second;
	}
	NetMatchService::ApplyHostActivityFallback(request);
	request.peerCount = m_MultiplayerHostPeerCount;
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
		request.slowPlayerBoundTicks = m_HostSetupOptions->slowPlayerBoundTicks;
		request.slowPlayerPolicy = m_HostSetupOptions->slowPlayerPolicy;
		request.idleWaitMinutes = m_HostSetupOptions->idleWaitMinutes;
		request.automaticRepair = m_HostSetupOptions->automaticRepair;
		request.autosaveSeconds = m_HostSetupOptions->autosaveEnabled ? m_HostSetupOptions->autosaveIntervalSeconds : 0;
		request.returnWindowMinutes = m_HostSetupOptions->returnWindowMinutes;
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
		request.frameRedundancyTicks = m_HostSetupOptions->frameRedundancyTicks;
	} else {
		NetHostDefaultsTemplate saved;
		if (NetHostDefaults::Load(saved, nullptr)) request.frameRedundancyTicks = saved.frameRedundancyTicks;
		SeedHostRulesFromActivity(request);
	}
	NetMatchService::SeatSavedOptions(request);
	return request;
}

void MainMenuGUI::RefreshHostPlayersChoices() {
	// Four humans leave no team for the AI when the AI plays against everyone.
	const uint8_t most = m_MultiplayerHostMode == NetMatchMode::PvPvE ? NetMatchConfigUtil::c_MaxPeerCount - 1 : NetMatchConfigUtil::c_MaxPeerCount;
	m_MultiplayerHostPeerCount = std::clamp<uint8_t>(m_MultiplayerHostPeerCount, NetMatchConfigUtil::c_MinPeerCount, most);
	if (!m_MultiplayerHostPlayersCombo) return;
	m_MultiplayerHostPlayersCombo->ClearList();
	for (uint8_t count = NetMatchConfigUtil::c_MinPeerCount; count <= most; ++count) {
		m_MultiplayerHostPlayersCombo->AddItem(std::to_string(count));
	}
	m_MultiplayerHostPlayersCombo->SetSelectedIndex(m_MultiplayerHostPeerCount - NetMatchConfigUtil::c_MinPeerCount);
}

void MainMenuGUI::SyncHostSetupDraft() {
	if (!m_HostSetupOptions) return;
	NetMatchConfig& draft = *m_HostSetupOptions;
	const NetMatchStandardRules before = draft;
	if (m_MultiplayerHostActivityIndex < m_MultiplayerHostActivities.size()) {
		draft.activityPreset = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].first;
		draft.activityModule = m_MultiplayerHostActivities[m_MultiplayerHostActivityIndex].second;
		if (std::string type = HostActivityType(draft.activityPreset, draft.activityModule); !type.empty()) draft.activityType = std::move(type);
	}
	if (m_MultiplayerHostSceneIndex < m_MultiplayerHostScenes.size()) {
		draft.sceneName = m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex].first;
		draft.sceneModule = m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex].second;
	}
	// A new activity brings its own rules, except those the host set himself.
	if (draft.activityPreset != before.activityPreset || draft.activityModule != before.activityModule) {
		if (const unsigned reseed = NetActivitySetup::AllSeededRules & ~m_HostAppliedRulesTouched; reseed != 0) NetActivitySetup::SeedRulesFromActivity(draft, reseed);
	}
	if (draft.mode == m_MultiplayerHostMode && draft.peerCount == m_MultiplayerHostPeerCount) return;
	// A new mode or player count seats the roster the mode makes of it; the rules the draft holds ride along.
	NetMatchServiceRequest request;
	request.host = true;
	request.peerCount = m_MultiplayerHostPeerCount;
	request.mode = m_MultiplayerHostMode;
	request.activityPreset = draft.activityPreset;
	request.activityModule = draft.activityModule;
	request.activityType = draft.activityType;
	request.sceneName = draft.sceneName;
	request.sceneModule = draft.sceneModule;
	NetMatchStandardRules rules = draft;
	rules.mode = m_MultiplayerHostMode;
	request.standardRules = rules;
	NetMatchConfig built;
	std::string error;
	if (!NetMatchService::BuildMatchConfig(request, draft.sessionId, built, &error)) {
		m_MultiplayerHostPickNotice = error;
		return;
	}
	draft.mode = m_MultiplayerHostMode;
	draft.modePreset = NetMatchConfigUtil::ModeName(draft.mode);
	draft.peerCount = built.peerCount;
	draft.players = built.players;
	if (!draft.peerInputDelayFrames.empty()) draft.peerInputDelayFrames.resize(draft.peerCount, draft.inputDelayFrames);
}

void MainMenuGUI::SyncHostScreenFromDraft(const NetMatchConfig& draft) {
	for (size_t i = 0; i < m_MultiplayerHostActivities.size(); ++i) {
		if (m_MultiplayerHostActivities[i].first == draft.activityPreset && m_MultiplayerHostActivities[i].second == draft.activityModule) {
			m_MultiplayerHostActivityIndex = i;
		}
	}
	RefreshMultiplayerHostScenes();
	for (size_t i = 0; i < m_MultiplayerHostScenes.size(); ++i) {
		if (m_MultiplayerHostScenes[i].first == draft.sceneName && m_MultiplayerHostScenes[i].second == draft.sceneModule) {
			m_MultiplayerHostSceneIndex = i;
		}
	}
	m_MultiplayerHostMode = draft.mode;
	uint8_t humans = 0;
	for (const NetMatchPlayerSlot& slot : draft.players) humans += slot.cpu ? 0 : 1;
	m_MultiplayerHostPeerCount = draft.dedicated ? draft.peerCount : std::max<uint8_t>(humans, NetMatchConfigUtil::c_MinPeerCount);
	RefreshHostPlayersChoices();
	ApplyMultiplayerHostActivity();
}

void MainMenuGUI::StageSavedHostDefaults() {
	if (m_HostSetupOptions) return;
	NetHostDefaultsTemplate saved;
	if (!NetHostDefaults::Load(saved, nullptr)) return;
	NetMatchConfig draft;
	if (!NetMatchService::BuildMatchConfig(HostRequestDraft(), 1, draft, nullptr) || !NetHostDefaults::ApplyTo(saved, draft, nullptr)) return;
	// The saved rules are the host's own choices: a new activity on the rows does not re-seed them.
	m_HostSetupOptions = draft;
	m_HostAppliedRulesTouched = NetActivitySetup::AllSeededRules;
	SyncHostScreenFromDraft(draft);
}

std::string MainMenuGUI::HostSummaryText() const {
	const std::string scene = m_MultiplayerHostSceneIndex < m_MultiplayerHostScenes.size() ? m_MultiplayerHostScenes[m_MultiplayerHostSceneIndex].first : std::string("Grasslands");
	unsigned people = m_MultiplayerHostPeerCount;
	unsigned ai = m_MultiplayerHostMode == NetMatchMode::PvPSkirmish ? 0 : 1;
	std::set<uint8_t> humanTeams;
	if (m_HostSetupOptions) {
		people = 0;
		ai = 0;
		for (const NetMatchPlayerSlot& slot : m_HostSetupOptions->players) {
			if (slot.cpu) {
				++ai;
			} else {
				++people;
				humanTeams.insert(slot.team);
			}
		}
	}
	const bool together = m_HostSetupOptions ? humanTeams.size() <= 1 : m_MultiplayerHostMode == NetMatchMode::CoopPvE;
	std::string seats = std::to_string(people) + (people == 1 ? " player" : " players") + (together ? " together" : " on separate teams");
	if (ai > 0) seats += ai == 1 ? " against an AI team" : " against " + std::to_string(ai) + " AI teams";
	std::string listing;
	switch (g_SettingsMan.GetNetworkHostVisibility()) {
		case SettingsMan::NetworkHostVisibility::Listed: listing = "Anyone can find and join this game in the game list."; break;
		case SettingsMan::NetworkHostVisibility::Unlisted: listing = "Not shown in the game list: friends join by your address."; break;
		case SettingsMan::NetworkHostVisibility::LAN: listing = "Only players on your network see this game."; break;
	}
	return scene + " - " + seats + ".\n" + listing;
}

void MainMenuGUI::OpenHostOptions(bool setupDraft) {
	m_HostOptionsSetupDraft = setupDraft;
	m_HostOptionsReadOnly = false;
	m_HostOptionsSeatRow = -1;
	m_HostSeatDlgModerationRow = -1;
	m_HostSeatDlgRemovalSeat.reset();
	m_HostKickBanWatch = false;
	m_HostRecRepairArmed = false;
	m_HostRecRepairRefusal.clear();
	m_HostOptionsAwaitedRevision = 0;
	// A fresh setup draft starts with no rule set by hand; any other continues from the last draft Apply accepted.
	m_HostRulesTouched = setupDraft && !m_HostSetupOptions ? 0 : m_HostAppliedRulesTouched;
	if (m_HostNetPortBox && m_MultiplayerHostPortTextBox) {
		// H34's port field edits the setup draft's port - the value the next hosted request carries.
		m_HostNetPortBox->SetText(m_MultiplayerHostPortTextBox->GetText());
	}
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
	RefreshHostOptionsActivities();
	RefreshHostOptionsScenes(false);
	LoadHostComputerDraft();
	m_HostComputerLoaded = m_HostComputerDraft;
	m_HostOptionsOpenedDraft = m_HostOptionsDraft;
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
	// An open list holds the player's pick until its close commits it, so the refresh leaves it alone.
	void HostOptSelectCombo(GUIComboBox* combo, const std::string& text) {
		if (!combo || combo->IsDropped()) return;
		for (int i = 0; i < combo->GetCount(); ++i) {
			if (combo->GetItem(i) && combo->GetItem(i)->m_Name == text) {
				combo->SetSelectedIndex(i);
				return;
			}
		}
		combo->SetText(text);
	}

	void HostOptSelectComboIndex(GUIComboBox* combo, int index) {
		if (combo && !combo->IsDropped()) combo->SetSelectedIndex(std::clamp(index, 0, std::max(0, combo->GetCount() - 1)));
	}

	void HostOptSetEditable(GUIControl* control, bool editable) {
		if (control) control->SetEnabled(editable);
	}

	/// The minutes a return window choice names; 0 when nothing is picked.
	uint8_t HostOptReturnWindowOf(GUIComboBox* combo) {
		const GUIListPanel::Item* item = combo && combo->GetSelectedIndex() >= 0 ? combo->GetItem(combo->GetSelectedIndex()) : nullptr;
		const long minutes = item ? std::strtol(item->m_Name.c_str(), nullptr, 10) : 0;
		return minutes >= NetMatchConfigUtil::c_MinReturnWindowMinutes && minutes <= NetMatchConfigUtil::c_MaxReturnWindowMinutes ? static_cast<uint8_t>(minutes) : 0;
	}

	/// Shows a return window, offering a saved value the list lacks in its place among the others.
	void HostOptSelectReturnWindow(GUIComboBox* combo, uint8_t minutes) {
		if (!combo || combo->IsDropped()) return;
		std::vector<uint8_t> offered;
		for (int i = 0; i < combo->GetCount(); ++i) {
			const GUIListPanel::Item* item = combo->GetItem(i);
			const long listed = item ? std::strtol(item->m_Name.c_str(), nullptr, 10) : 0;
			if (listed == minutes) {
				if (combo->GetSelectedIndex() != i) combo->SetSelectedIndex(i);
				return;
			}
			offered.push_back(static_cast<uint8_t>(listed));
		}
		offered.insert(std::upper_bound(offered.begin(), offered.end(), minutes), minutes);
		combo->ClearList();
		for (const uint8_t listed : offered) combo->AddItem(NetReturnWindowText(listed));
		combo->SetSelectedIndex(static_cast<int>(std::find(offered.begin(), offered.end(), minutes) - offered.begin()));
	}

	/// The seconds a world history or catch-up limit item names.
	int HostOptJoinHistorySecondsOf(const GUIListPanel::Item* item) {
		if (!item) return 0;
		const long count = std::strtol(item->m_Name.c_str(), nullptr, 10);
		return static_cast<int>(item->m_Name.find("minute") != std::string::npos ? count * 60 : count);
	}

	/// The seconds a world history or catch-up limit row has picked; 0 when nothing is picked.
	int HostOptJoinHistorySecondsOf(GUIComboBox* combo) {
		return combo && combo->GetSelectedIndex() >= 0 ? HostOptJoinHistorySecondsOf(combo->GetItem(combo->GetSelectedIndex())) : 0;
	}

	/// Shows a world history or catch-up limit, offering a saved value the list lacks in its place among the others.
	void HostOptSelectJoinHistory(GUIComboBox* combo, int seconds) {
		if (!combo || combo->IsDropped()) return;
		std::vector<int> offered;
		for (int i = 0; i < combo->GetCount(); ++i) {
			const int listed = HostOptJoinHistorySecondsOf(combo->GetItem(i));
			if (listed == seconds) {
				if (combo->GetSelectedIndex() != i) combo->SetSelectedIndex(i);
				return;
			}
			offered.push_back(listed);
		}
		offered.insert(std::upper_bound(offered.begin(), offered.end(), seconds), seconds);
		combo->ClearList();
		for (const int listed: offered) combo->AddItem(NetJoinHistoryText(listed));
		combo->SetSelectedIndex(static_cast<int>(std::find(offered.begin(), offered.end(), seconds) - offered.begin()));
	}

	bool HostOptBoxFocused(GUITextBox* box) {
		return box && box->GetPanel() && box->GetPanel()->HasFocus();
	}
}

void MainMenuGUI::RefreshHostOptionsControls(const NetLobbySnapshot& snapshot) {
	// The panel's model: every control mirrors the draft and is rewritten from it every frame; HandleHostOptionsInputEvents reads a
	// change into the draft in the frame it arrives; an open list and a box the player is typing in are never rewritten.
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
		std::string refusal = g_NetMatchService.GetErrorText();
		if (!refusal.starts_with("Host options refused: ")) refusal.clear();
		const std::optional<NetMatchConfig> pending = g_NetMatchService.GetPendingHostOptions();
		if (!refusal.empty()) {
			m_HostOptionsStatusLabel->SetText(refusal);
		} else if ((pending && pending->configRevision > adopted.configRevision) || m_HostOptionsAwaitedRevision > adopted.configRevision) {
			// A returned lobby publishes immediately even though it has played a round.
			m_HostOptionsStatusLabel->SetText(NetHostOptionsApplyText(g_NetMatchService.GetState()));
		}
		if (refusal.empty() && m_HostOptionsAwaitedRevision != 0 && adopted.configRevision >= m_HostOptionsAwaitedRevision) {
			m_HostOptionsAwaitedRevision = 0;
			m_HostOptionsStatusLabel->SetText("Applied: this lobby was republished.");
		}
	}
	// A Queued kick/ban drains on the setup worker's host pump; the applied result lands in the
	// service's last-result field, and this reads it until it stops being Queued.
	if (m_HostKickBanWatch) {
		const NetKickBanResult drained = g_NetMatchService.GetLastKickBanResult();
		if (drained != NetKickBanResult::Queued) {
			m_HostKickBanWatch = false;
			m_HostOptionsStatusLabel->SetText(m_HostKickBanVerb + ": " + NetKickBanResultName(drained));
		}
	}
	// The title names the panel and the selector names the page; the client's read-only view is the match's details.
	m_HostOptionsTitle->SetText(m_HostOptionsReadOnly ? "M A T C H   D E T A I L S" : "A D V A N C E D");
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
	const std::vector<NetMatchPlayerSlot> adoptedPlayers = m_HostOptionsSetupDraft ? std::vector<NetMatchPlayerSlot>() : g_NetMatchService.GetLobbyMatchConfig().players;
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
		if (member && (member->connected || member->dropped || member->reclaiming)) m_HostSeatNameLabels[row]->SetText(NetPlayerPresentation::Name(*member));
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
			if (member && (member->aiHeld || member->dropped || member->reclaiming || line.ends_with(": left"))) {
				state = NetPlayerPresentation::State(*member);
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
		// A row whose every other kind ChangeHostSeatType would refuse offers none: the host's own seat, and an open
		// lobby's human seat, which stays reserved for its peer until the match is over.
		const bool hostSeat = !isPlaceholder && !slot.cpu && !m_HostOptionsDraft.dedicated && slot.peerId == m_HostOptionsDraft.hostPeerId;
		const bool reservedSeat = !isPlaceholder && !slot.cpu && !m_HostOptionsSetupDraft &&
		                          std::any_of(adoptedPlayers.begin(), adoptedPlayers.end(), [&slot](const NetMatchPlayerSlot& kept) { return !kept.cpu && kept.peerId == slot.peerId; });
		HostOptSetEditable(m_HostSeatTypeCombos[row], editable && !seatedHuman && !hostSeat && !reservedSeat);
		HostOptSelectComboIndex(m_HostSeatTeamCombos[row], slot.team);
		HostOptSetEditable(m_HostSeatTeamCombos[row], editable && !isPlaceholder && !slot.cpu);
		HostOptSetEditable(m_HostSeatDetailsButtons[row], !isPlaceholder);
	}

	// Under the selector: what the shown page edits, and for whom.
	if (m_HostOptScopeLabel) {
		const bool computer = m_HostOptionsPage == c_HostOptionsConnectionPage || m_HostOptionsPage == c_HostOptionsFilesPage;
		const std::string scope = computer ? "Saved on this computer for every game you host."
		                                   : m_HostOptionsReadOnly ? "This match, as the host set it up."
		                                   : m_HostOptionsSetupDraft ? "The lobby you create next." : "This lobby's match.";
		if (m_HostOptScopeLabel->GetText() != scope) m_HostOptScopeLabel->SetText(scope);
	}
	if (m_HostRulesDefaultsLabel) {
		NetMatchStandardRules standard;
		standard.activityType = m_HostOptionsDraft.activityType;
		standard.activityModule = m_HostOptionsDraft.activityModule;
		standard.activityPreset = m_HostOptionsDraft.activityPreset;
		const bool seeded = NetActivitySetup::SeedRulesFromActivity(standard);
		const std::string gold = standard.startingGold >= NetMatchConfigUtil::c_InfiniteGold ? std::string("infinite gold") : std::to_string(standard.startingGold) + " oz";
		const std::string line = seeded ? "Defaults: difficulty 50, AI skill 50; from the activity " + gold + ", fog of war " + (standard.fogOfWar ? "on" : "off") +
		                                      ", clear path " + (standard.requireClearPathToOrbit ? "on" : "off") + ", deploy units " + (standard.deployUnits ? "on" : "off") + "."
		                                  : std::string("Defaults: difficulty 50, AI skill 50.");
		if (m_HostRulesDefaultsLabel->GetText() != line) m_HostRulesDefaultsLabel->SetText(line);
	}

	// Rules page.
	HostOptSelectCombo(m_HostRulesActivityCombo, m_HostOptionsDraft.activityPreset +
	                   (m_HostOptionsDraft.activityModule.empty() ? "" : " - " + m_HostOptionsDraft.activityModule));
	const auto draftedScene = std::find_if(m_HostOptionsScenes.begin(), m_HostOptionsScenes.end(), [this](const NetHostSceneChoice& scene) {
		return scene.name == m_HostOptionsDraft.sceneName && scene.module == m_HostOptionsDraft.sceneModule;
	});
	if (draftedScene != m_HostOptionsScenes.end()) {
		HostOptSelectComboIndex(m_HostRulesSceneCombo, static_cast<int>(draftedScene - m_HostOptionsScenes.begin()));
	} else {
		HostOptSelectCombo(m_HostRulesSceneCombo, m_HostOptionsDraft.sceneName);
	}
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
	HostOptSelectComboIndex(m_HostNetSlowPolicyCombo, 0);
	if (m_HostNetSlowBoundBox && !HostOptBoxFocused(m_HostNetSlowBoundBox)) m_HostNetSlowBoundBox->SetText(std::to_string(m_HostOptionsDraft.slowPlayerBoundTicks));
	if (m_HostNetSlowPolicyHintLabel) {
		m_HostNetSlowPolicyHintLabel->SetText(NetSlowPlayerHint(m_HostOptionsDraft.slowPlayerPolicy, m_HostOptionsDraft.slowPlayerBoundTicks, g_TimerMan.GetDeltaTimeMS()));
	}
	HostOptSelectComboIndex(m_HostNetRedundancyCombo, m_HostOptionsDraft.frameRedundancyTicks - 1);
	if (m_HostNetMinDelayBox && !HostOptBoxFocused(m_HostNetMinDelayBox)) {
		m_HostNetMinDelayBox->SetText(std::to_string(m_HostOptionsDraft.inputDelayFrames));
	}
	if (m_HostNetEffectiveLabel) {
		// H22: what an automatic delay adds, then the floor in ticks and milliseconds and the announced per-sender figure.
		const std::string floor = std::to_string(m_HostOptionsDraft.inputDelayFrames) + " ticks (" +
		                          std::to_string(static_cast<int>(m_HostOptionsDraft.inputDelayFrames * g_TimerMan.GetDeltaTimeMS())) + " ms)";
		std::string effective = m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Auto
		                            ? "Effective delay: " + NetAutoDelayText(m_HostOptionsDraft.slowPlayerBoundTicks) + ", at least " + floor
		                            : "Effective delay: " + floor;
		if (!snapshot.inputDelayText.empty()) effective += " - " + snapshot.inputDelayText;
		m_HostNetEffectiveLabel->SetText(effective);
	}
	const bool fixedPolicy = m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Fixed;
	for (int peer = 0; peer < 4; ++peer) {
		const bool used = peer < capacity;
		if (m_HostNetPeerLabels[peer]) {
			m_HostNetPeerLabels[peer]->SetVisible(used && fixedPolicy);
			m_HostNetPeerLabels[peer]->SetText("Peer " + std::to_string(peer + 1));
		}
		if (m_HostNetPeerDelayBoxes[peer]) {
			m_HostNetPeerDelayBoxes[peer]->SetVisible(used && fixedPolicy);
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
	HostOptSetEditable(m_HostNetSlowPolicyCombo, editable);
	HostOptSetEditable(m_HostNetSlowBoundBox, editable && m_HostOptionsDraft.slowPlayerPolicy == NetSlowPlayerPolicy::Substitute);
	HostOptSetEditable(m_HostNetRedundancyCombo, editable);
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
	// H34: the adopted config's own words for the host row - mode, capacity and seated humans.
	if (m_HostNetModeLabel) {
		const NetMatchConfig& adopted = g_NetMatchService.GetLobbyMatchConfig();
		int seated = 0;
		for (const NetMatchPlayerSlot& slot : adopted.players) {
			if (slot.cpu || slot.peerId == 0) continue;
			// A seat reserved for a peer that has never arrived is not a seated human; a held one is.
			for (const NetLobbyMember& member : snapshot.members) {
				if (member.peerId == slot.peerId && (member.connected || member.dropped || member.reclaiming)) {
					++seated;
					break;
				}
			}
		}
		m_HostNetModeLabel->SetText("Host mode: " + std::string(adopted.dedicated ? "Dedicated" : "Playing") +
		                            " - capacity " + std::to_string(adopted.peerCount) +
		                            " - humans seated " + std::to_string(seated) +
		                            " - " + (m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Auto ? "Auto" : "Fixed") +
		                            (m_HostOptionsDraft.slowPlayerPolicy == NetSlowPlayerPolicy::Pause ? " / pause <=20s" : " / wait " + std::to_string(m_HostOptionsDraft.slowPlayerBoundTicks) + " ticks / AI") + "\n" +
		                            (m_HostOptionsSetupDraft ? NetHostNatModeText(g_SettingsMan) : g_NetMatchService.GetNatModeText()));
	}
	// The Connection page shows this computer's draft: nothing is saved until Apply.
	const int listingIndex = m_HostComputerDraft.listing == SettingsMan::NetworkHostVisibility::Listed ? 0 : m_HostComputerDraft.listing == SettingsMan::NetworkHostVisibility::Unlisted ? 1 : 2;
	HostOptSelectComboIndex(m_HostNetVisibilityCombo, listingIndex);
	if (m_HostNetVisibilityHint) {
		static const char* meanings[] = {"Anyone finds this game in the online game list and joins it.",
		                                 "Not shown in the online game list: players join by your address.",
		                                 "Only players on your network see it: no online listing is made."};
		m_HostNetVisibilityHint->SetText(meanings[listingIndex]);
	}
	if (m_MultiplayerHostPortMapCheckbox) m_MultiplayerHostPortMapCheckbox->SetCheck(m_HostComputerDraft.portMap ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	HostOptSetEditable(m_MultiplayerHostPortMapCheckbox, editable);
	if (m_HostNetPortBox && !HostOptBoxFocused(m_HostNetPortBox)) m_HostNetPortBox->SetText(m_HostComputerDraft.port);
	if (GUILabel* portHint = dynamic_cast<GUILabel*>(m_SubMenuScreenGUIControlManager->GetControl("LabelHostNetPortHint"))) {
		// The router's own answer for the lobby being hosted, beside the port it was asked to open.
		const std::string hint = "default 41010" + (snapshot.portMap.empty() ? std::string() : " - " + snapshot.portMap);
		if (portHint->GetText() != hint) portHint->SetText(hint);
	}
	HostOptSetEditable(m_HostNetVisibilityCombo, editable);
	HostOptSelectComboIndex(m_HostNetIceCombo, m_HostComputerDraft.ice ? 0 : 1);
	HostOptSetEditable(m_HostNetIceCombo, editable);
	if (m_HostNetIceHintLabel) {
		m_HostNetIceHintLabel->SetText(NetHostNatTraversalHint(g_SettingsMan, m_HostComputerDraft.ice, m_HostOptionsSetupDraft, m_HostOptionsReadOnly, g_NetMatchService.GetIceRoute()));
	}
	HostOptSelectComboIndex(m_HostRelayCombo, static_cast<int>(m_HostComputerDraft.relay));
	HostOptSetEditable(m_HostRelayCombo, editable && m_HostOptionsSetupDraft);
	const bool fixedRelay = m_HostComputerDraft.relay == SettingsMan::NetworkHostRelayMode::Fixed;
	for (size_t i = 0; i < m_HostRelayBoxes.size(); ++i) {
		if (auto* box = m_HostRelayBoxes[i]) {
			box->SetVisible(fixedRelay);
			HostOptSetEditable(box, editable && m_HostOptionsSetupDraft);
			if (!HostOptBoxFocused(box)) box->SetText(m_HostComputerDraft.relayFields[i]);
		}
		if (m_HostRelayLabels[i]) m_HostRelayLabels[i]->SetVisible(fixedRelay);
	}
	if (m_HostRelayHint) {
		std::string hint = NetHostRelayHint(g_SettingsMan, m_HostComputerDraft.relay, m_HostComputerDraft.ice);
		const std::string relayError = g_NetMatchService.GetRelayError();
		if (!m_HostOptionsSetupDraft && !relayError.empty()) hint += "\n" + relayError;
		if (m_HostOptionsReadOnly) hint = "This row shows your saved hosting preference; only the host sets up this match.\nCurrent match: " + g_NetMatchService.GetNatModeText() + ". Choose your route in Settings > Network > Connection.\n" + relayError;
		m_HostRelayHint->SetText(hint);
	}
	// The port box stays pressable while hosted so the attempt can name the refusal.
	HostOptSetEditable(m_HostNetPortBox, editable);

	// Recovery page.
	if (m_HostRecRepairCheck) m_HostRecRepairCheck->SetCheck(m_HostOptionsDraft.automaticRepair ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	if (m_HostRecAutosaveCheck) m_HostRecAutosaveCheck->SetCheck(m_HostOptionsDraft.autosaveEnabled ? GUICheckbox::Checked : GUICheckbox::Unchecked);
	if (m_HostRecAutosaveIntervalBox && !HostOptBoxFocused(m_HostRecAutosaveIntervalBox)) {
		m_HostRecAutosaveIntervalBox->SetText(std::to_string(m_HostOptionsDraft.autosaveIntervalSeconds));
	}
	HostOptSetEditable(m_HostRecAutosaveIntervalBox, editable && m_HostOptionsDraft.autosaveEnabled);
	HostOptSelectReturnWindow(m_HostRecReturnWindowCombo, m_HostOptionsDraft.returnWindowMinutes);
	HostOptSetEditable(m_HostRecReturnWindowCombo, editable);
	HostOptSetEditable(m_HostRecRepairCheck, editable);
	HostOptSetEditable(m_HostRecAutosaveCheck, editable);
	// This host's own history options: they never ride the match config, so they show the saved settings on every pass.
	HostOptSelectJoinHistory(m_HostRecJoinHistoryCombo, m_HostComputerDraft.joinHistorySeconds);
	HostOptSelectJoinHistory(m_HostRecJoinLagCombo, m_HostComputerDraft.joinLagSeconds);
	HostOptSetEditable(m_HostRecJoinHistoryCombo, editable);
	HostOptSetEditable(m_HostRecJoinLagCombo, editable);
	// One hint area under the rows names the consequence of the row the player points at or has focused; the last one stays.
	if (m_HostRecOptionHintLabel && m_HostOptionsPages[c_HostOptionsRecoveryPage] && m_HostOptionsPages[c_HostOptionsRecoveryPage]->GetVisible()) {
		int mouseX = 0, mouseY = 0;
		m_SubMenuScreenGUIControlManager->GetManager()->GetInputController()->GetMousePosition(&mouseX, &mouseY);
		for (size_t index = 0; index < m_HostRecHintRowControls.size(); ++index) {
			GUIControl* control = m_HostRecHintRowControls[index];
			GUIPanel* panel = control ? control->GetPanel() : nullptr;
			if (panel && (panel->PointInside(mouseX, mouseY) || panel->HasFocus())) m_HostRecHintRow = static_cast<int>(index / 2);
		}
		const char* hint = m_HostRecHintRow == 1 ? NetJoinHistoryHint() : m_HostRecHintRow == 2 ? NetJoinLagHint() : NetReturnWindowHint();
		if (m_HostRecOptionHintLabel->GetText() != hint) m_HostRecOptionHintLabel->SetText(hint);
	}
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
		                                  ? (m_HostOptionsDraft.autosaveEnabled && m_HostOptionsDraft.autosaveIntervalSeconds > 0
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
	// The service gates both repair controls on the same live round.
	const bool repairLive = NetHostRepairEnabled(g_NetMatchService);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostRepairNowButton], repairLive);
	if (!repairLive) m_HostRecRepairArmed = false;
	if (m_HostRecRepairHintLabel) {
		m_HostRecRepairHintLabel->SetText(NetHostRepairHint(g_NetMatchService, m_HostRecRepairArmed, m_HostRecRepairRefusal));
	}

	// Files page: local paths, local retention, and the local status-widget preference.
	if (m_HostFilesSavePathLabel) SetFittedPath(m_HostFilesSavePathLabel, "Autosaves: ", std::filesystem::path(System::GetWorkingDirectory()) / "Autosaves");
	if (m_HostFilesDiagPathLabel) SetFittedPath(m_HostFilesDiagPathLabel, "Diagnostics: ", std::filesystem::path(System::GetWorkingDirectory()) / "Telemetry");
	HostOptSelectComboIndex(m_HostFilesWidgetCombo, static_cast<int>(m_HostComputerDraft.statusWidget));
	// A local preference, but the read-only view applies nothing: there the player sets it in Settings - Network.
	HostOptSetEditable(m_HostFilesWidgetCombo, !m_HostOptionsReadOnly);
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
	// H11: the count is the store's own rows - a Session-scope record counts when it names this
	// session's id, an Until Removed one counts beside it ("2 banned this session, 1 until removed").
	if (m_HostSessBannedLabel) {
		const uint64_t sessionId = g_NetMatchService.GetLobbyMatchConfig().sessionId;
		uint32_t sessionBans = 0, heldBans = 0;
		for (const NetHostBanRecord& record : g_NetMatchService.GetBanRecords()) {
			if (record.scope == NetHostBanScope::UntilRemoved) {
				++heldBans;
			} else if (record.sessionId == sessionId) {
				++sessionBans;
			}
		}
		m_HostSessBannedLabel->SetText(std::to_string(sessionBans) + " banned this session" +
		                               (heldBans > 0 ? ", " + std::to_string(heldBans) + " until removed" : ""));
	}
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostSessionBannedButton], true);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostSessionEndButton], editable && !m_HostOptionsSetupDraft);

	// The footer: Apply only when there is a change and the player may make one.
	bool dirty = false;
	if (m_HostOptionsSetupDraft && !m_HostSetupOptions) {
		dirty = !(m_HostOptionsDraft == m_HostOptionsOpenedDraft);
	} else if (m_HostOptionsSetupDraft) {
		dirty = static_cast<const NetMatchStandardRules&>(m_HostOptionsDraft) != static_cast<const NetMatchStandardRules&>(*m_HostSetupOptions)
		        || m_HostOptionsDraft.players != m_HostSetupOptions->players
		        || m_HostOptionsDraft.mode != m_HostSetupOptions->mode
		        || m_HostOptionsDraft.activityPreset != m_HostSetupOptions->activityPreset
		        || m_HostOptionsDraft.activityModule != m_HostSetupOptions->activityModule
		        || m_HostOptionsDraft.sceneName != m_HostSetupOptions->sceneName
		        || m_HostOptionsDraft.peerCount != m_HostSetupOptions->peerCount
		        || m_HostOptionsDraft.delayPolicy != m_HostSetupOptions->delayPolicy
		        || m_HostOptionsDraft.idleWaitMinutes != m_HostSetupOptions->idleWaitMinutes
		        || m_HostOptionsDraft.automaticRepair != m_HostSetupOptions->automaticRepair
		        || m_HostOptionsDraft.frameRedundancyTicks != m_HostSetupOptions->frameRedundancyTicks
		        || m_HostOptionsDraft.autosaveEnabled != m_HostSetupOptions->autosaveEnabled
		        || m_HostOptionsDraft.autosaveIntervalSeconds != m_HostSetupOptions->autosaveIntervalSeconds
		        || m_HostOptionsDraft.returnWindowMinutes != m_HostSetupOptions->returnWindowMinutes
		        || m_HostOptionsDraft.inputDelayFrames != m_HostSetupOptions->inputDelayFrames;
	} else {
		const NetMatchConfig adopted = g_NetMatchService.GetLobbyMatchConfig();
		dirty = !(m_HostOptionsDraft == adopted) && !(g_NetMatchService.GetPendingHostOptions() && m_HostOptionsDraft == *g_NetMatchService.GetPendingHostOptions());
	}
	dirty = dirty || !(m_HostComputerDraft == m_HostComputerLoaded);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostOptionsApplyButton], editable && dirty);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostOptionsDefaultsButton], editable);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostOptionsRestoreButton], editable);
	HostOptSetEditable(m_MainMenuButtons[MenuButton::HostOptionsBackButton], true);
	// Leaving drops what Apply has not taken, and the button says so while there is anything to drop.
	m_MainMenuButtons[MenuButton::HostOptionsBackButton]->SetText(dirty && editable ? "Cancel" : "Back");
	if (m_HostOptionsStatusLabel->GetText().empty() && dirty) {
		m_HostOptionsStatusLabel->SetText("Changes ready to apply.");
	}
	// An open seat dialog re-reads the moderation view every frame: the reclaim seconds tick and a
	// new applicant shows without the host reopening it.
	RefreshHostSeatDialog();
	// The banned dialog likewise tracks the store: a queued unban's drain lands the row's removal.
	RefreshHostBannedDialog();
}

void MainMenuGUI::RefreshHostOptionsActivities() {
	// The census the host screen offers, by module; an agreed activity outside it still displays by name.
	m_HostOptionsActivities = NetMatchService::ListHostActivities();
	if (!m_HostRulesActivityCombo) return;
	m_HostRulesActivityCombo->ClearList();
	for (const NetHostActivityChoice& row : m_HostOptionsActivities) {
		m_HostRulesActivityCombo->AddItem(row.preset + (row.module.empty() ? "" : " - " + row.module));
	}
}

void MainMenuGUI::RefreshHostOptionsScenes(bool resolve) {
	m_HostOptionsScenes = NetMatchService::ListHostScenes(m_HostOptionsDraft.activityPreset, m_HostOptionsDraft.activityModule);
	const bool runs = std::any_of(m_HostOptionsScenes.begin(), m_HostOptionsScenes.end(), [this](const NetHostSceneChoice& scene) {
		return scene.name == m_HostOptionsDraft.sceneName && scene.module == m_HostOptionsDraft.sceneModule;
	});
	// A new activity keeps the scene when it can run there, else takes its preferred one, as the host screen does.
	if (resolve && !runs) {
		NetMatchService::ResolveHostScene(m_HostOptionsDraft.activityPreset, m_HostOptionsDraft.activityModule, m_HostOptionsDraft.sceneName, m_HostOptionsDraft.sceneModule);
	}
	if (!m_HostRulesSceneCombo) return;
	m_HostRulesSceneCombo->ClearList();
	std::map<std::string, int> names;
	for (const NetHostSceneChoice& scene : m_HostOptionsScenes) ++names[scene.name];
	for (const NetHostSceneChoice& scene : m_HostOptionsScenes) {
		m_HostRulesSceneCombo->AddItem(scene.name + (names[scene.name] > 1 && !scene.module.empty() ? " - " + scene.module : ""));
	}
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
		if (picked < static_cast<int>(m_HostOptionsActivities.size())) {
			m_HostOptionsDraft.activityPreset = m_HostOptionsActivities[picked].preset;
			m_HostOptionsDraft.activityModule = m_HostOptionsActivities[picked].module;
			// The launch resolves the activity by its class too.
			m_HostOptionsDraft.activityType = m_HostOptionsActivities[picked].activityType;
		} else if (const GUIListPanel::Item* item = m_HostRulesActivityCombo->GetItem(picked)) {
			m_HostOptionsDraft.activityPreset = item->m_Name;
		}
	}
	if (m_HostRulesSceneCombo && m_HostRulesSceneCombo->GetSelectedIndex() >= 0) {
		const int picked = m_HostRulesSceneCombo->GetSelectedIndex();
		if (picked < static_cast<int>(m_HostOptionsScenes.size())) {
			m_HostOptionsDraft.sceneName = m_HostOptionsScenes[picked].name;
			m_HostOptionsDraft.sceneModule = m_HostOptionsScenes[picked].module;
		} else if (const GUIListPanel::Item* item = m_HostRulesSceneCombo->GetItem(picked)) {
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
		// A press lands between the slider's steps: a moved slider gives the nearest step, as the wheel moves it.
		const int step = std::max(1, m_HostRulesGoldSlider->GetValueResolution());
		const int shown = static_cast<int>(std::min(m_HostOptionsDraft.startingGold, static_cast<uint32_t>(31000)));
		const int raw = m_HostRulesGoldSlider->GetValue();
		const int slider = raw == shown ? raw : (raw + step / 2) / step * step;
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
	m_HostOptionsDraft.slowPlayerPolicy = NetSlowPlayerPolicy::Substitute;
	if (m_HostNetSlowBoundBox) m_HostOptionsDraft.slowPlayerBoundTicks = static_cast<uint16_t>(std::clamp<long>(std::strtol(m_HostNetSlowBoundBox->GetText().c_str(), nullptr, 10), 1, NetMatchConfigUtil::c_MaxSlowPlayerBoundTicks));
	if (m_HostNetRedundancyCombo) {
		m_HostOptionsDraft.frameRedundancyTicks = static_cast<uint8_t>(m_HostNetRedundancyCombo->GetSelectedIndex() + 1);
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
	const bool autosaveWasEnabled = m_HostOptionsDraft.autosaveEnabled;
	if (m_HostRecAutosaveCheck) m_HostOptionsDraft.autosaveEnabled = m_HostRecAutosaveCheck->GetCheck() == GUICheckbox::Checked;
	if (m_HostRecAutosaveIntervalBox) {
		const std::string typed = m_HostRecAutosaveIntervalBox->GetText();
		char* parsedEnd = nullptr;
		const long parsed = std::strtol(typed.c_str(), &parsedEnd, 10);
		// An emptied box is mid-edit and keeps the draft's interval; zero is off, and a nonzero
		// interval clamps into the announced minute-to-hour range.
		if (parsedEnd != typed.c_str()) {
			m_HostOptionsDraft.autosaveIntervalSeconds = parsed <= 0 ? 0 : static_cast<uint32_t>(std::clamp<long>(parsed, NetMatchService::c_MinAutosaveIntervalSeconds, NetMatchService::c_MaxAutosaveIntervalSeconds));
		}
	}
	if (m_HostOptionsDraft.autosaveEnabled && m_HostOptionsDraft.autosaveIntervalSeconds == 0) {
		if (autosaveWasEnabled) {
			// A typed 0 is off, so the checkbox, the caption and the published config agree.
			m_HostOptionsDraft.autosaveEnabled = false;
			if (m_HostRecAutosaveCheck) m_HostRecAutosaveCheck->SetCheck(GUICheckbox::Unchecked);
		} else {
			// Switching autosave on starts at the shortest cadence instead of reading the off zero back.
			m_HostOptionsDraft.autosaveIntervalSeconds = NetMatchService::c_MinAutosaveIntervalSeconds;
			if (m_HostRecAutosaveIntervalBox) m_HostRecAutosaveIntervalBox->SetText(std::to_string(m_HostOptionsDraft.autosaveIntervalSeconds));
		}
	}
	if (!m_HostOptionsDraft.autosaveEnabled) m_HostOptionsDraft.autosaveIntervalSeconds = 0;
	if (const uint8_t minutes = HostOptReturnWindowOf(m_HostRecReturnWindowCombo); minutes != 0) m_HostOptionsDraft.returnWindowMinutes = minutes;

	// This computer's typed choices: the port and the relay's address and login.
	if (!m_HostOptionsReadOnly) {
		if (m_HostNetPortBox) m_HostComputerDraft.port = m_HostNetPortBox->GetText();
		for (size_t i = 0; i < m_HostRelayBoxes.size() && m_HostOptionsSetupDraft; ++i) {
			if (m_HostRelayBoxes[i]) m_HostComputerDraft.relayFields[i] = m_HostRelayBoxes[i]->GetText();
		}
	}

	// Session: the idle combo's index maps onto the minutes table.
	static const uint8_t idleValues[] = {0, 1, 5, 10, 20, 30, 45, 60};
	if (m_HostSessIdleCombo && m_HostSessIdleCombo->GetSelectedIndex() >= 0) {
		m_HostOptionsDraft.idleWaitMinutes = idleValues[std::clamp(m_HostSessIdleCombo->GetSelectedIndex(), 0, 7)];
	}
}

void MainMenuGUI::FollowHostActivityDefaults(const NetMatchStandardRules& before, const GUIControl* guiEventControl) {
	// A rule the host moved himself stays his; the read is what tells a hand from the refresh writing the draft back.
	const std::pair<const GUIControl*, bool> moved[] = {
	    {m_HostRulesGoldSlider, m_HostOptionsDraft.startingGold != before.startingGold},
	    {m_HostRulesFogCheck, m_HostOptionsDraft.fogOfWar != before.fogOfWar},
	    {m_HostRulesClearPathCheck, m_HostOptionsDraft.requireClearPathToOrbit != before.requireClearPathToOrbit},
	    {m_HostRulesDeployCheck, m_HostOptionsDraft.deployUnits != before.deployUnits}};
	const unsigned rules[] = {NetActivitySetup::StartingGold, NetActivitySetup::FogOfWar, NetActivitySetup::ClearPathToOrbit, NetActivitySetup::DeployUnits};
	for (size_t i = 0; i < std::size(moved); ++i) {
		if (guiEventControl && guiEventControl == moved[i].first && moved[i].second) m_HostRulesTouched |= rules[i];
	}
	unsigned reseed = 0;
	if (m_HostOptionsDraft.activityPreset != before.activityPreset || m_HostOptionsDraft.activityModule != before.activityModule) {
		RefreshHostOptionsScenes(true);
		reseed = NetActivitySetup::AllSeededRules;
	} else if (m_HostOptionsDraft.difficulty != before.difficulty) {
		reseed = NetActivitySetup::StartingGold;
	}
	reseed &= ~m_HostRulesTouched;
	if (reseed != 0) NetActivitySetup::SeedRulesFromActivity(m_HostOptionsDraft, reseed);
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
	// This computer's choices commit on the same click; one that cannot be taken now names its reason and stays.
	if (!CommitHostComputerDraft()) {
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	std::string error;
	if (!NetMatchConfigUtil::ValidateLocalAlpha(m_HostOptionsDraft, &error)) {
		m_HostOptionsStatusLabel->SetText(error);
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	if (m_HostOptionsSetupDraft) {
		m_HostSetupOptions = m_HostOptionsDraft;
		m_HostAppliedRulesTouched = m_HostRulesTouched;
		SyncHostScreenFromDraft(*m_HostSetupOptions);
		m_HostOptionsStatusLabel->SetText("Staged for the next lobby.");
	} else {
		if (!g_NetMatchService.SubmitHostOptions(m_HostOptionsBaseRevision, m_HostOptionsDraft, &error)) {
			m_HostOptionsStatusLabel->SetText(error);
			g_GUISound.BackButtonPressSound()->Play();
			return;
		}
		// The adopted revision keeps Apply pending until the runner publishes it.
		m_HostOptionsAwaitedRevision = m_HostOptionsBaseRevision + 1;
		m_HostAppliedRulesTouched = m_HostRulesTouched;
		m_HostOptionsStatusLabel->SetText(NetHostOptionsApplyText(g_NetMatchService.GetState()));
	}
	m_HostComputerLoaded = m_HostComputerDraft;
	g_GUISound.ButtonPressSound()->Play();
}

void MainMenuGUI::LeaveHostOptions() {
	m_MultiplayerSubScreen = m_HostOptionsSetupDraft ? MultiplayerSubScreen::HostSetup : MultiplayerSubScreen::Lobby;
	if (m_MultiplayerSubScreen == MultiplayerSubScreen::HostSetup) ApplyMultiplayerHostActivity();
}

void MainMenuGUI::LoadHostComputerDraft() {
	HostComputerDraft& draft = m_HostComputerDraft;
	draft.listing = g_SettingsMan.GetNetworkHostVisibility();
	// An open lobby's listing is what its lease holds now.
	if (!m_HostOptionsSetupDraft && g_NetMatchService.IsHost()) {
		static const SettingsMan::NetworkHostVisibility leases[] = {SettingsMan::NetworkHostVisibility::LAN, SettingsMan::NetworkHostVisibility::Unlisted, SettingsMan::NetworkHostVisibility::Listed};
		draft.listing = leases[std::clamp(g_NetMatchService.GetDirectoryVisibility(), 0, 2)];
	}
	draft.portMap = g_SettingsMan.GetNetworkPortMapEnable();
	draft.port = m_MultiplayerHostPortTextBox ? m_MultiplayerHostPortTextBox->GetText() : std::string("41010");
	draft.ice = g_SettingsMan.GetNetworkIceEnableSetting();
	draft.relay = g_SettingsMan.GetNetworkHostRelayModeSetting();
	draft.relayFields = {g_SettingsMan.GetNetworkTurnServersSetting(), g_SettingsMan.GetNetworkTurnUser(), g_SettingsMan.GetNetworkTurnPass()};
	draft.joinHistorySeconds = g_SettingsMan.GetNetworkHostJoinHistorySeconds();
	draft.joinLagSeconds = g_SettingsMan.GetNetworkHostJoinLagSeconds();
	draft.statusWidget = g_SettingsMan.GetNetworkMatchStatusMode();
}

bool MainMenuGUI::CommitHostComputerDraft() {
	const HostComputerDraft& draft = m_HostComputerDraft;
	const bool hosting = g_NetMatchService.IsHost();
	if (draft.port != (m_MultiplayerHostPortTextBox ? m_MultiplayerHostPortTextBox->GetText() : draft.port)) {
		const long parsed = std::strtol(draft.port.c_str(), nullptr, 10);
		if (parsed < 1024 || parsed > 65535) {
			m_HostOptionsStatusLabel->SetText("The game port must be from 1024 to 65535.");
			ShowHostOptionsPage(c_HostOptionsConnectionPage);
			return false;
		}
		if (hosting) {
			// The bind belongs to the live session; a new port takes effect on the next lobby.
			m_HostOptionsStatusLabel->SetText("Close this lobby to change the game port.");
			ShowHostOptionsPage(c_HostOptionsConnectionPage);
			return false;
		}
		m_MultiplayerHostPortTextBox->SetText(std::to_string(static_cast<int>(parsed)));
	}
	if (hosting && !m_HostOptionsSetupDraft && draft.ice != m_HostComputerLoaded.ice) {
		// The session's routes were set up when it opened.
		m_HostOptionsStatusLabel->SetText("Close this lobby to change the direct connection.");
		ShowHostOptionsPage(c_HostOptionsConnectionPage);
		return false;
	}
	const auto vis = draft.listing == SettingsMan::NetworkHostVisibility::Listed ? 2 : draft.listing == SettingsMan::NetworkHostVisibility::Unlisted ? 1 : 0;
	if (hosting && !m_HostOptionsSetupDraft && vis != g_NetMatchService.GetDirectoryVisibility()) {
		if (vis > 0 && g_SettingsMan.GetSessionDirectoryUrl().empty()) {
			m_HostOptionsStatusLabel->SetText("An online listing needs the online game list service (Settings - Network).");
			ShowHostOptionsPage(c_HostOptionsConnectionPage);
			return false;
		}
		if (!g_NetMatchService.SetDirectoryVisibility(vis)) {
			m_HostOptionsStatusLabel->SetText("This lobby cannot be listed online again: create a new lobby to list it.");
			ShowHostOptionsPage(c_HostOptionsConnectionPage);
			return false;
		}
	}
	g_SettingsMan.SetNetworkHostVisibility(draft.listing);
	g_SettingsMan.SetNetworkPortMapEnable(draft.portMap);
	if (m_HostOptionsSetupDraft) {
		g_SettingsMan.SetNetworkIceEnable(draft.ice);
		g_SettingsMan.SetNetworkHostRelayMode(draft.relay);
		g_SettingsMan.SetNetworkTurnServers(draft.relayFields[0]);
		g_SettingsMan.SetNetworkTurnUser(draft.relayFields[1]);
		g_SettingsMan.SetNetworkTurnPass(draft.relayFields[2]);
	}
	g_SettingsMan.SetNetworkHostJoinHistorySeconds(draft.joinHistorySeconds);
	g_SettingsMan.SetNetworkHostJoinLagSeconds(draft.joinLagSeconds);
	g_SettingsMan.SetNetworkMatchStatusMode(draft.statusWidget);
	g_SettingsMan.UpdateSettingsFile();
	LogHostHistoryPolicy();
	return true;
}

void MainMenuGUI::RestoreHostOptionsPageDefaults() {
	if (m_HostOptionsReadOnly) return;
	DraftHostOptionsFromControls();
	const NetMatchConfig factory = NetMatchConfigUtil::MakeDefault(m_HostOptionsDraft.sessionId);
	switch (m_HostOptionsPage) {
		case c_HostOptionsPlayersPage:
			// The roster the mode makes of the default count, with its own teams.
			if (m_HostOptionsSetupDraft) m_HostOptionsDraft.peerCount = 2;
			m_HostOptionsDraft.players.clear();
			RederiveHostOptionsRoster();
			break;
		case c_HostOptionsRulesPage: {
			// The activity's own rules and the original defaults; the activity, scene and mode stay the host's pick.
			const std::string activityType = m_HostOptionsDraft.activityType, activityModule = m_HostOptionsDraft.activityModule, activityPreset = m_HostOptionsDraft.activityPreset;
			const std::string sceneName = m_HostOptionsDraft.sceneName, sceneModule = m_HostOptionsDraft.sceneModule;
			const NetMatchMode mode = m_HostOptionsDraft.mode;
			static_cast<NetMatchStandardRules&>(m_HostOptionsDraft) = NetMatchStandardRules{};
			m_HostOptionsDraft.activityType = activityType;
			m_HostOptionsDraft.activityModule = activityModule;
			m_HostOptionsDraft.activityPreset = activityPreset;
			m_HostOptionsDraft.sceneName = sceneName;
			m_HostOptionsDraft.sceneModule = sceneModule;
			m_HostOptionsDraft.mode = mode;
			NetActivitySetup::SeedRulesFromActivity(m_HostOptionsDraft);
			m_HostRulesTouched = 0;
			break;
		}
		case c_HostOptionsConnectionPage: {
			// The relay's own address and login stay as typed: only the choice to use it returns to the default.
			const std::array<std::string, 3> relayFields = m_HostComputerDraft.relayFields;
			const int joinHistory = m_HostComputerDraft.joinHistorySeconds, joinLag = m_HostComputerDraft.joinLagSeconds;
			const SettingsMan::NetworkMatchStatusMode widget = m_HostComputerDraft.statusWidget;
			m_HostComputerDraft = HostComputerDraft{};
			m_HostComputerDraft.relayFields = relayFields;
			m_HostComputerDraft.joinHistorySeconds = joinHistory;
			m_HostComputerDraft.joinLagSeconds = joinLag;
			m_HostComputerDraft.statusWidget = widget;
			break;
		}
		case c_HostOptionsTimingPage:
			m_HostOptionsDraft.delayPolicy = factory.delayPolicy;
			m_HostOptionsDraft.inputDelayFrames = factory.inputDelayFrames;
			m_HostOptionsDraft.peerInputDelayFrames.clear();
			m_HostOptionsDraft.slowPlayerBoundTicks = NetMatchConfigUtil::c_DefaultSlowPlayerBoundTicks;
			m_HostOptionsDraft.frameRedundancyTicks = NetMatchConfigUtil::c_DefaultFrameRedundancyTicks;
			break;
		case c_HostOptionsRecoveryPage:
			m_HostOptionsDraft.automaticRepair = factory.automaticRepair;
			m_HostOptionsDraft.autosaveEnabled = false;
			m_HostOptionsDraft.autosaveIntervalSeconds = 0;
			m_HostOptionsDraft.returnWindowMinutes = NetMatchConfigUtil::c_DefaultReturnWindowMinutes;
			m_HostComputerDraft.joinHistorySeconds = HostComputerDraft{}.joinHistorySeconds;
			m_HostComputerDraft.joinLagSeconds = HostComputerDraft{}.joinLagSeconds;
			break;
		case c_HostOptionsFilesPage:
			m_HostComputerDraft.statusWidget = HostComputerDraft{}.statusWidget;
			break;
		case c_HostOptionsSessionPage:
			m_HostOptionsDraft.idleWaitMinutes = 10;
			break;
		default:
			break;
	}
	m_HostOptionsStatusLabel->SetText("This page's defaults are staged: Apply keeps them, Cancel drops them.");
	g_GUISound.ItemChangeSound()->Play();
}

void MainMenuGUI::LogHostHistoryPolicy() const {
	NetJoinHistoryPolicy policy = NetMatchService::JoinHistoryPolicyFromSettings(g_TimerMan.GetDeltaTimeMS());
	policy.returnWindowMinutes = m_HostOptionsDraft.returnWindowMinutes;
	bool byWindow = false;
	const uint64_t retain = NetMatchService::EffectiveJoinRetention(policy, g_TimerMan.GetDeltaTimeMS(), &byWindow);
	System::PrintDiagnosticLine(std::format("[round-history] host options world_history_s={} catch_up_limit_s={} return_window_min={} retain_frames={} retain_by={} lag_limit_frames={}",
	                                        g_SettingsMan.GetNetworkHostJoinHistorySeconds(), g_SettingsMan.GetNetworkHostJoinLagSeconds(), policy.returnWindowMinutes, retain,
	                                        byWindow ? "return_window" : "world_history", policy.lagLimitFrames));
}

void MainMenuGUI::SaveHostOptionsDefaults() {
	DraftHostOptionsFromControls();
	// The settings twins hold the same fields; the draft's host-owned values become the defaults
	// every later lobby's request starts from.
	g_SettingsMan.SetNetworkHostDelayPolicy(m_HostOptionsDraft.delayPolicy == NetMatchDelayPolicy::Fixed
	                                          ? SettingsMan::NetworkHostDelayPolicy::Fixed : SettingsMan::NetworkHostDelayPolicy::Auto);
	g_SettingsMan.SetNetworkInputDelayFrames(m_HostOptionsDraft.inputDelayFrames);
	g_SettingsMan.SetNetworkSlowPlayerBoundTicks(m_HostOptionsDraft.slowPlayerBoundTicks);
	g_SettingsMan.SetNetworkSlowPlayerPolicy(m_HostOptionsDraft.slowPlayerPolicy == NetSlowPlayerPolicy::Pause ? SettingsMan::NetworkSlowPlayerPolicy::Pause : SettingsMan::NetworkSlowPlayerPolicy::Substitute);
	g_SettingsMan.SetNetworkHostIdleWaitMinutes(m_HostOptionsDraft.idleWaitMinutes);
	g_SettingsMan.SetNetworkHostReturnWindowMinutes(m_HostOptionsDraft.returnWindowMinutes);
	g_SettingsMan.SetNetworkHostAutoRepair(m_HostOptionsDraft.automaticRepair);
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
			if (m_HostOptionsDraft.dedicated && id == m_HostOptionsDraft.hostPeerId) continue;
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
			seat.displayName = NetMatchConfigUtil::UnseatedSlotName(peerId, candidate.persistentWorld);
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
			candidate.players[row].displayName = NetMatchConfigUtil::UnseatedSlotName(peerId, candidate.persistentWorld);
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
	m_HostSeatDlgActionHint->SetText("");
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
	const std::vector<NetH4ModerationSeat> seats = g_NetMatchService.GetModerationSeats();
	m_ModerationUx.Refresh(seats);
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
	// Healthy holders still need the raw admission row that removal validates.
	m_HostSeatDlgRemovalSeat.reset();
	if (slot.peerId != 0) {
		for (const NetH4ModerationSeat& seat : seats) {
			if (!seat.cpu && seat.lockstepPeerId == slot.peerId) {
				m_HostSeatDlgRemovalSeat = seat;
				break;
			}
		}
	}
	const NetModerationUx::Row* mrow = m_HostSeatDlgModerationRow >= 0 ? &m_ModerationUx.GetRow(m_HostSeatDlgModerationRow) : nullptr;
	if (mrow && (mrow->view.dropped || mrow->view.held || mrow->view.reclaiming)) {
		// A held seat waits for its player with no deadline: only the host's click gives it away.
		const std::string cause = NetModerationUx::HoldCause(mrow->view);
		m_HostSeatDlgReclaim->SetText(mrow->view.reclaiming ? std::string("Reclaim: its player is rejoining")
		                                                    : "Reclaim: seat kept for its player" + (cause.empty() ? std::string() : " - " + cause));
	} else {
		m_HostSeatDlgReclaim->SetText(mrow ? "Reclaim: seat in use" : "Reclaim: --");
	}
	if (mrow) {
		m_HostSeatDlgApplicants->SetText(mrow->view.applicants.empty() ? std::string("Nobody is asking for this seat")
		                                                             : std::string("Players asking for this seat - pick one, then Approve"));
		// One row per person asking, with what they ask for; the row the host picked is the one Approve seats.
		std::vector<std::string> asking;
		int chosen = -1;
		for (size_t i = 0; i < mrow->view.applicants.size(); ++i) {
			const NetH4ApplicantView& applicant = mrow->view.applicants[i];
			asking.push_back(applicant.displayName + " - asks to play this seat" + (applicant.approved ? std::string(" - approval sent") : std::string()));
			if (applicant.connection == mrow->applicant) chosen = static_cast<int>(i);
		}
		if (m_HostSeatDlgApplicantList) {
			bool same = m_HostSeatDlgApplicantList->GetItemList()->size() == asking.size();
			for (size_t i = 0; same && i < asking.size(); ++i) same = m_HostSeatDlgApplicantList->GetItem(static_cast<int>(i))->m_Name == asking[i];
			if (!same) {
				m_HostSeatDlgApplicantList->ClearList();
				for (const std::string& line : asking) m_HostSeatDlgApplicantList->AddItem(line);
			}
			if (m_HostSeatDlgApplicantList->GetSelectedIndex() != chosen) m_HostSeatDlgApplicantList->SetSelectedIndex(chosen);
			m_HostSeatDlgApplicantList->SetEnabled(host && !asking.empty());
		}
		HostOptSetEditable(m_HostSeatDlgWait, host && NetModerationUx::Available(*mrow, NetModerationAction::Wait));
		HostOptSetEditable(m_HostSeatDlgApprove, host && NetModerationUx::Available(*mrow, NetModerationAction::Substitute));
		HostOptSetEditable(m_HostSeatDlgCancel, host && NetModerationUx::Available(*mrow, NetModerationAction::Cancel));
	} else {
		m_HostSeatDlgApplicants->SetText("Nobody is asking for this seat");
		if (m_HostSeatDlgApplicantList) {
			m_HostSeatDlgApplicantList->ClearList();
			m_HostSeatDlgApplicantList->SetEnabled(false);
		}
		HostOptSetEditable(m_HostSeatDlgWait, false);
		HostOptSetEditable(m_HostSeatDlgApprove, false);
		HostOptSetEditable(m_HostSeatDlgCancel, false);
	}
	// H09/H10: Kick and Ban act on a human seat another peer holds - the host's own seat is never
	// kickable (the store answers ForbiddenTarget, but the button stays off first).
	const uint8_t localPeerId = g_NetMatchService.GetLobbySnapshot().localPeerId;
	const bool humanSeat = host && !slot.cpu && slot.peerId != 0 && slot.peerId != localPeerId;
	m_HostSeatDlgKick->SetEnabled(humanSeat);
	m_HostSeatDlgBan->SetEnabled(humanSeat);
	if (m_HostSeatDlgActionHint->GetText().empty() || !humanSeat) {
		m_HostSeatDlgActionHint->SetText(humanSeat ? "Seat actions apply to this player."
		                                         : (slot.cpu ? "CPU seats are the host's to retype, not to moderate."
		                                            : host && slot.peerId != 0 && slot.peerId == localPeerId ? "The host's own seat is never kicked or banned."
		                                            : "Moderation is the host's; clients watch."));
	}
}

void MainMenuGUI::RefreshHostBannedDialog() {
	if (m_ActiveDialogBox != m_HostBannedDialog || !m_HostBannedPick) {
		return;
	}
	const std::vector<NetHostBanRecord> records = g_NetMatchService.GetBanRecords();
	const uint64_t nowUnixMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	const auto describe = [&nowUnixMs](const NetHostBanRecord& record) {
		const uint64_t minutes = record.createdUnixMs < nowUnixMs ? (nowUnixMs - record.createdUnixMs) / 60000 : 0;
		return (record.displayAlias.empty() ? "(unnamed)" : record.displayAlias) + " - " +
		       (record.scope == NetHostBanScope::UntilRemoved ? "until removed" : "session") + ", " +
		       (minutes == 0 ? std::string("just now") : std::to_string(minutes) + "m ago");
	};
	// The pick list rebuilds only when the store's rows change, so an open dropdown survives.
	bool changed = records.size() != m_HostBannedRecords.size();
	for (size_t i = 0; !changed && i < records.size(); ++i) {
		changed = !(records[i].identity == m_HostBannedRecords[i].identity &&
		            records[i].scope == m_HostBannedRecords[i].scope &&
		            records[i].createdUnixMs == m_HostBannedRecords[i].createdUnixMs);
	}
	if (changed) {
		m_HostBannedRecords = records;
		m_HostBannedPick->ClearList();
		for (const NetHostBanRecord& record : m_HostBannedRecords) {
			m_HostBannedPick->AddItem(describe(record));
		}
		if (!m_HostBannedRecords.empty()) {
			m_HostBannedPick->SetSelectedIndex(0);
		}
	}
	if (m_HostBannedRecords.empty()) {
		m_HostBannedListLabel->SetText("(no banned players)");
	} else {
		std::string lines;
		const int pick = m_HostBannedPick->GetSelectedIndex();
		for (size_t i = 0; i < m_HostBannedRecords.size(); ++i) {
			const NetHostBanRecord& record = m_HostBannedRecords[i];
			if (!lines.empty()) lines += "\n";
			lines += (pick == static_cast<int>(i) ? "> " : "  ") + describe(record) +
			         (record.reason.empty() ? "" : " - " + record.reason);
		}
		m_HostBannedListLabel->SetText(lines);
	}
	const int pick = m_HostBannedPick->GetSelectedIndex();
	m_MainMenuButtons[MenuButton::HostBannedRemoveButton]->SetEnabled(
		pick >= 0 && pick < static_cast<int>(m_HostBannedRecords.size()) && g_NetMatchService.IsHost());
}

void MainMenuGUI::ShowHostBannedDialog() {
	// H11: the store's own rows - identity's public alias, the scope, the age; Remove unbans the
	// picked row through the same host pump a queued kick drains on.
	m_HostBannedRecords.clear();
	if (m_HostBannedPick) m_HostBannedPick->ClearList();
	m_HostBannedStatusLabel->SetText("");
	OpenMultiplayerDialog(m_HostBannedDialog, m_HostOptionsPanel);
	RefreshHostBannedDialog();
	m_MainMenuButtons[MenuButton::HostBannedCloseButton]->SetFocus();
}

void MainMenuGUI::HandleHostOptionsInputEvents(const GUIControl* guiEventControl) {
	// The other half of the refresh's model: a control the refresh rewrites from the draft must reach the draft from here, in the
	// frame its change arrives, or the next frame puts the old value back under the player's hand. A list still open holds an
	// uncommitted pick, and the team row's controls still show the team it left.
	if (!m_HostOptionsReadOnly && guiEventControl != m_HostRulesTeamCombo) {
		const std::vector<GUIControl*>& controls = *m_ActiveGUIControlManager->GetControlList();
		const bool listOpen = std::any_of(controls.begin(), controls.end(), [](GUIControl* control) {
			GUIComboBox* combo = dynamic_cast<GUIComboBox*>(control);
			return combo && combo->IsDropped();
		});
		// Controls that do not yet show the draft (it moved in code since the last refresh) hold nothing of the player's.
		if (!listOpen && m_HostOptionsDraft == m_HostOptionsShownDraft) {
			const NetMatchStandardRules before = m_HostOptionsDraft;
			DraftHostOptionsFromControls();
			FollowHostActivityDefaults(before, guiEventControl);
		}
	}
	if (guiEventControl == m_HostRelayCombo) {
		if (m_HostOptionsReadOnly || !m_HostOptionsSetupDraft) return;
		m_HostComputerDraft.relay = static_cast<SettingsMan::NetworkHostRelayMode>(std::clamp(m_HostRelayCombo->GetSelectedIndex(), 0, 2));
		return;
	}
	for (size_t i = 0; i < m_HostRelayBoxes.size(); ++i) {
		if (guiEventControl == m_HostRelayBoxes[i]) {
			if (!m_HostOptionsReadOnly && m_HostOptionsSetupDraft) m_HostComputerDraft.relayFields[i] = m_HostRelayBoxes[i]->GetText();
			return;
		}
	}
	if (guiEventControl == m_MultiplayerHostPortMapCheckbox) {
		if (!m_HostOptionsReadOnly) m_HostComputerDraft.portMap = m_MultiplayerHostPortMapCheckbox->GetCheck() == GUICheckbox::Checked;
		return;
	}
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
	if (guiEventControl == m_HostSeatDlgApplicantList) {
		// The model chooses by stepping through the seat's applicants; it steps until the picked person is the chosen one.
		const int picked = m_HostSeatDlgApplicantList->GetSelectedIndex();
		if (m_HostSeatDlgModerationRow >= 0 && picked >= 0) {
			const size_t row = static_cast<size_t>(m_HostSeatDlgModerationRow);
			const std::vector<NetH4ApplicantView> applicants = m_ModerationUx.GetRow(row).view.applicants;
			for (size_t step = 0; picked < static_cast<int>(applicants.size()) && step < applicants.size() &&
			                     m_ModerationUx.GetRow(row).applicant != applicants[static_cast<size_t>(picked)].connection; ++step) {
				m_ModerationUx.CycleApplicant(row);
			}
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
		const NetParticipantRemovalAction action = guiEventControl == m_HostSeatDlgKick
		                                           ? NetParticipantRemovalAction::Kick : NetParticipantRemovalAction::BanSession;
		const std::string verb = action == NetParticipantRemovalAction::Kick ? "Kick" : "Ban";
		const uint8_t peerId = m_HostOptionsSeatRow >= 0 && m_HostOptionsSeatRow < static_cast<int>(m_HostOptionsDraft.players.size())
		                           ? m_HostOptionsDraft.players[m_HostOptionsSeatRow].peerId : 0;
		const std::string refusal = NetHostSeatRemovalRefusal(g_NetMatchService.GetState(), m_HostSeatDlgRemovalSeat.has_value(), peerId, verb);
		if (!refusal.empty()) {
			m_HostSeatDlgActionHint->SetText(refusal);
			return;
		}
		const NetKickBanResult result =
			g_NetMatchService.RemoveParticipant(NetSelectModerationSeat(*m_HostSeatDlgRemovalSeat), action);
		if (result == NetKickBanResult::Ok || result == NetKickBanResult::Queued) {
			// On Ok the seat is already removed; on Queued the setup worker's host pump applies it.
			// The roster row re-reads the snapshot next frame either way.
			m_HostOptionsSeatRow = -1;
			m_HostSeatDlgModerationRow = -1;
			m_HostSeatDlgRemovalSeat.reset();
			CloseMultiplayerDialog();
			m_HostOptionsStatusLabel->SetText(verb + (result == NetKickBanResult::Queued ? " queued..." : ": Ok"));
			m_HostKickBanWatch = result == NetKickBanResult::Queued;
			m_HostKickBanVerb = verb;
			return;
		}
		// The refusal names the store's own verdict; the issue carries the peer it was refused for.
		const NetParticipantRemovalIssue issue = g_NetMatchService.GetLastRemovalIssue();
		std::string text = verb + " refused - " + NetKickBanResultName(result) +
		                   " (seat " + std::to_string(m_HostSeatDlgRemovalSeat->stableSeat) + ")";
		if (issue.lockstepPeerId != 0) {
			text += " (peer " + std::to_string(issue.lockstepPeerId) + ")";
		}
		m_HostSeatDlgActionHint->SetText(text);
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
		const int pick = m_HostBannedPick ? m_HostBannedPick->GetSelectedIndex() : -1;
		if (pick < 0 || pick >= static_cast<int>(m_HostBannedRecords.size())) {
			m_HostBannedStatusLabel->SetText("Pick a ban row first.");
			return;
		}
		// UnbanParticipant answers Queued while the setup worker owns admission; the applied result
		// replaces it at the next drain, and this dialog's rows re-read the store when it lands.
		const NetKickBanResult result = g_NetMatchService.UnbanParticipant(m_HostBannedRecords[pick].identity);
		m_HostBannedStatusLabel->SetText(std::string("Remove ban: ") + NetKickBanResultName(result));
		RefreshHostBannedDialog();
		return;
	}
	if (guiEventControl == m_HostBannedPick) {
		RefreshHostBannedDialog();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostOptionsBackButton]) {
		// Cancel discards the page's edits; the session is never touched by leaving the panel.
		LeaveHostOptions();
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostOptionsRestoreButton]) {
		RestoreHostOptionsPageDefaults();
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
		m_HostSeatDlgRemovalSeat.reset();
		CloseMultiplayerDialog();
		return;
	}
	if (guiEventControl == m_MainMenuButtons[MenuButton::HostRepairNowButton]) {
		NetHostRepairPress(g_NetMatchService, m_HostRecRepairArmed, m_HostRecRepairRefusal);
		if (m_HostRecRepairHintLabel) m_HostRecRepairHintLabel->SetText(NetHostRepairHint(g_NetMatchService, m_HostRecRepairArmed, m_HostRecRepairRefusal));
		return;
	}
	if (guiEventControl == m_HostNetVisibilityCombo) {
		static const SettingsMan::NetworkHostVisibility listings[] = {SettingsMan::NetworkHostVisibility::Listed, SettingsMan::NetworkHostVisibility::Unlisted, SettingsMan::NetworkHostVisibility::LAN};
		if (const int pick = m_HostNetVisibilityCombo->GetSelectedIndex(); pick >= 0 && pick < 3 && !m_HostOptionsReadOnly) m_HostComputerDraft.listing = listings[pick];
		return;
	}
	if (guiEventControl == m_HostNetPortBox) {
		if (!m_HostOptionsReadOnly) m_HostComputerDraft.port = m_HostNetPortBox->GetText();
		return;
	}
	if (guiEventControl == m_HostNetIceCombo) {
		if (!m_HostOptionsReadOnly) m_HostComputerDraft.ice = m_HostNetIceCombo->GetSelectedIndex() == 0;
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
		// The refresh fills the row from the team just picked; every earlier pick is already in the draft.
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
	if (guiEventControl == m_HostRecJoinHistoryCombo || guiEventControl == m_HostRecJoinLagCombo) {
		// This host's own options take effect at its next history pass; Save as host defaults keeps them.
		const bool history = guiEventControl == m_HostRecJoinHistoryCombo;
		if (const int seconds = HostOptJoinHistorySecondsOf(history ? m_HostRecJoinHistoryCombo : m_HostRecJoinLagCombo); seconds != 0) {
			(history ? m_HostComputerDraft.joinHistorySeconds : m_HostComputerDraft.joinLagSeconds) = seconds;
		}
		return;
	}
	if (guiEventControl == m_HostFilesWidgetCombo) {
		m_HostComputerDraft.statusWidget = static_cast<SettingsMan::NetworkMatchStatusMode>(std::clamp(m_HostFilesWidgetCombo->GetSelectedIndex(), 0, 2));
		return;
	}
	if (guiEventControl == m_HostNetPolicyCombo) {
		// The per-peer boxes only exist under Fixed; the policy flip redraws their state.
		m_HostOptionsDraft.delayPolicy = m_HostNetPolicyCombo->GetSelectedIndex() == 1 ? NetMatchDelayPolicy::Fixed : NetMatchDelayPolicy::Auto;
		return;
	}
	if (guiEventControl == m_HostNetRedundancyCombo) {
		m_HostOptionsDraft.frameRedundancyTicks = static_cast<uint8_t>(m_HostNetRedundancyCombo->GetSelectedIndex() + 1);
		return;
	}
	if (guiEventControl == m_HostRecReturnWindowCombo) {
		if (const uint8_t minutes = HostOptReturnWindowOf(m_HostRecReturnWindowCombo); minutes != 0) m_HostOptionsDraft.returnWindowMinutes = minutes;
		LogHostHistoryPolicy();
		return;
	}
	if (guiEventControl == m_HostNetSlowPolicyCombo) {
		m_HostOptionsDraft.slowPlayerPolicy = NetSlowPlayerPolicy::Substitute;
		return;
	}
	if (guiEventControl == m_HostNetRecalcButton) {
		// The auto policy already re-derives each sender's figure from the live link; the button is
		// the host's "look again now" - the readouts re-fill from the service snapshot this frame.
		m_HostOptionsStatusLabel->SetText("Link figures refreshed from the live snapshot.");
		g_GUISound.ItemChangeSound()->Play();
		return;
	}
	if (guiEventControl == m_HostRulesBrainlessCombo || guiEventControl == m_HostRecAutosaveCheck || guiEventControl == m_HostSessIdleCombo) {
		DraftHostOptionsFromControls();
	}
}

void MainMenuGUI::StartMultiplayer(bool host) {
	const std::string portText = (host ? m_MultiplayerHostPortTextBox : m_MultiplayerJoinPortTextBox)->GetText();
	char* parseEnd = nullptr;
	const long parsedPort = std::strtol(portText.c_str(), &parseEnd, 10);
	if (portText.empty() || *parseEnd != '\0' || parsedPort < 1 || parsedPort > 65535) {
		ShowSetupFailure(host, host ? "The game port in Advanced must be a whole number from 1 to 65535." : "The port must be a whole number from 1 to 65535.");
		return;
	}
	if (!host && m_MultiplayerJoinAddressTextBox->GetText().empty()) {
		ShowSetupFailure(host, "Enter the address the host gave you.");
		return;
	}
	// A port this machine has hosted on is the one its own join field should offer next.
	if (host) {
		SetJoinPortAuto(static_cast<uint16_t>(parsedPort));
	}
	NetMatchServiceRequest request;
	request.host = host;
	if (!host) {
		request.SetJoinAddress(m_MultiplayerJoinAddressTextBox->GetText());
	}
	request.port = static_cast<uint16_t>(parsedPort);
	// Hosting or joining under a name saves it, but saving is best effort: the wire carries more
	// bytes than the box takes typed, so a name the settings will not hold still goes out as typed.
	const std::string typedName = m_MultiplayerNameTextBox->GetText();
	// The box's typed cap is shorter than the wire's, but a pasted or scripted name skips it and a
	// name past the hello's byte cap only fails inside the encode; refuse it here in the player's words.
	if (typedName.size() > NetProtocol::c_MaxDisplayNameBytes) {
		// The name box lives on the landing: the refusal goes where the player can shorten it.
		m_MultiplayerLandingStatusLabel->SetText("This name is too long. Shorten it and try again.");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		g_GUISound.BackButtonPressSound()->Play();
		return;
	}
	request.playerName = typedName.empty() ? (host ? "Host" : "Client") : typedName;
	if (!typedName.empty() && typedName != g_SettingsMan.GetNetworkDisplayName()) {
		g_SettingsMan.SetNetworkDisplayName(typedName);
		g_SettingsMan.UpdateSettingsFile();
	}
	request.activityPreset = "P4 Alpha Duel";
	request.activityModule = "Base.rte";
	if (!host) {
		const std::string joinTarget = NetDirectoryClient::WorldJoinTarget(request.address, request.sessionId);
		bool targetWorld = m_JoinTargetPersistentWorld || m_JoinTargetActivity == "Persistent World";
		if (!targetWorld) {
			const int selected = m_MultiplayerLanGamesList ? m_MultiplayerLanGamesList->GetSelectedIndex() : -1;
			targetWorld = NetDirectoryClient::TargetsPersistentWorld(m_GameRows, selected, joinTarget, request.port,
			                                                        m_LastWorldJoinAddress, m_LastWorldJoinPort, &m_JoinTargetActivity);
		}
		if (targetWorld) {
			request.persistentWorld = true;
			m_JoinTargetPersistentWorld = true;
			m_LastWorldJoinAddress = joinTarget;
			m_LastWorldJoinPort = request.port;
			g_NetMatchService.NoteJoinTargetPersistentWorld(true);
			if (!m_JoinTargetActivity.empty()) {
				request.activityPreset = m_JoinTargetActivity;
			} else {
				request.activityPreset = "Persistent World";
			}
		}
	}
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
	// A host's Start with someone not ready counts down for everyone, as the lobby says.
	request.startCountdown = host;
	// The setup screen's staged options draft overrides every field the request carries.
	if (host && m_HostSetupOptions) {
		request.standardRules = static_cast<const NetMatchStandardRules&>(*m_HostSetupOptions);
		// BuildMatchConfig reads the mode off the request, not the rules block it carries inside.
		request.mode = m_HostSetupOptions->mode;
		request.delayPolicy = m_HostSetupOptions->delayPolicy;
		request.slowPlayerBoundTicks = m_HostSetupOptions->slowPlayerBoundTicks;
		request.slowPlayerPolicy = m_HostSetupOptions->slowPlayerPolicy;
		request.idleWaitMinutes = m_HostSetupOptions->idleWaitMinutes;
		request.automaticRepair = m_HostSetupOptions->automaticRepair;
		request.autosaveSeconds = m_HostSetupOptions->autosaveEnabled ? m_HostSetupOptions->autosaveIntervalSeconds : 0;
		request.returnWindowMinutes = m_HostSetupOptions->returnWindowMinutes;
		request.peerCount = m_HostSetupOptions->peerCount;
		request.inputDelayFrames = m_HostSetupOptions->inputDelayFrames;
		request.autoInputDelay = m_HostSetupOptions->delayPolicy == NetMatchDelayPolicy::Auto;
		uint32_t cpuSeats = 0;
		for (const NetMatchPlayerSlot& slot : m_HostSetupOptions->players) cpuSeats += slot.cpu ? 1 : 0;
		request.cpuSlots = cpuSeats;
		request.frameRedundancyTicks = m_HostSetupOptions->frameRedundancyTicks;
	} else if (host) {
		NetHostDefaultsTemplate saved;
		if (NetHostDefaults::Load(saved, nullptr)) request.frameRedundancyTicks = saved.frameRedundancyTicks;
	}
	// The host picks the roster size and the lockstep input-delay buffer; clients adopt both via
	// the lobby config sync. The delay box writes back to the setting so the choice persists.
	if (host && !m_HostSetupOptions) {
		request.peerCount = m_MultiplayerHostPeerCount;
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
		SeedHostRulesFromActivity(request);
	}

	std::string error;
	m_MultiplayerJoinRequest = request;
	m_MultiplayerApplyOffered = !host;
	const auto start = std::chrono::steady_clock::now();
	const bool started = g_NetMatchService.Start(request, &error);
	if (FrameRecorder::Instance().Enabled()) {
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		const std::string event = "menu_start host=" + std::to_string(host) + " elapsed_us=" + std::to_string(elapsed) +
		    " budget_us=16667 within_one_frame=" + std::to_string(elapsed <= 16667);
		FrameRecorder::Instance().RecordEvent(event);
		System::PrintDiagnosticLine("[video-ui] " + event);
	}
	if (started) {
		m_MultiplayerLandingStatusLabel->SetText("");
		m_ReconnectStatusShown.clear();
		// A joining peer keeps the join screen until the host admits it; the refresh switches the screen
		// the moment the lobby snapshot says the seat is real.
		m_JoinAttemptActive = !host;
		m_JoinStatusText = host ? std::string() : "Joining " + (m_JoinTargetName.empty() ? std::string("the game") : m_JoinTargetName) + "...";
		m_MultiplayerSubScreen = host ? MultiplayerSubScreen::Lobby : MultiplayerSubScreen::JoinSetup;
	} else {
		ShowSetupFailure(host, PlayerFacingStatus(error));
	}
	g_GUISound.ButtonPressSound()->Play();
}

void MainMenuGUI::ApplyForOwnSeat(uint16_t stableSeat) {
	std::string error;
	m_MultiplayerApplyOffered = false;
	if (g_NetMatchService.BeginSeatApplication(m_MultiplayerJoinRequest, stableSeat, &error)) {
		m_MultiplayerLandingStatusLabel->SetText("Asking the host for your slot...");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else {
		m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus(error));
	}
	m_ReconnectStatusShown = m_MultiplayerLandingStatusLabel->GetText();
}

void MainMenuGUI::WaitForSlot() {
	std::string error;
	// The wait is the join's own bounded wait, knocking for a slot; the same refusal offers Apply again when it runs out.
	if (g_NetMatchService.BeginSlotWait(m_MultiplayerJoinRequest, &error)) {
		m_MultiplayerLandingStatusLabel->SetText("Waiting for a slot to open...");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else {
		m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus(error));
	}
	m_ReconnectStatusShown = m_MultiplayerLandingStatusLabel->GetText();
}

void MainMenuGUI::ApplyToSubstitute() {
	std::string error;
	m_MultiplayerApplyOffered = false;
	if (g_NetMatchService.BeginSubstituteApplication(m_MultiplayerJoinRequest, &error)) {
		m_MultiplayerLandingStatusLabel->SetText("Asking the host for a seat...");
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else {
		m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus(error));
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
	if (g_NetMatchService.GetState() == NetMatchServiceState::Completed && !g_NetMatchService.NeedsHostOptionsCorrection()) {
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
	if (m_JoinAttemptActive) {
		if (snapshot.failed || g_NetMatchService.GetState() == NetMatchServiceState::Idle) {
			m_JoinAttemptActive = false;
			const std::string reason = PlayerFacingStatus(GroupDelimiterForDisplay(FormatModuleMismatchStatus(snapshot.errorText.empty() ? snapshot.statusText : snapshot.errorText)));
			// A refusal the player can answer - a held seat to apply for, a slot to wait for - is offered where its buttons are.
			if (m_MultiplayerApplyOffered && g_NetMatchService.JoinRefusalOffer() != NetJoinRefusalOffer::None) {
				m_JoinStatusText.clear();
				m_MultiplayerLandingStatusLabel->SetText(reason);
				m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
				RefreshMultiplayerScreenControls(snapshot);
				return;
			}
			m_JoinStatusText = reason;
			m_MultiplayerSubScreen = MultiplayerSubScreen::JoinSetup;
			RefreshMultiplayerScreenControls(snapshot);
			return;
		}
		if (!snapshot.running && (snapshot.lobbyPhase.empty() || snapshot.lobbyPhase == "Idle" || snapshot.lobbyPhase == "SessionStarting") && snapshot.transferLine.empty() && snapshot.waitLine.empty()) {
			m_JoinStatusText = "Joining " + (m_JoinTargetName.empty() ? std::string("the game") : m_JoinTargetName) + "..." +
			                   (snapshot.statusText.empty() ? std::string() : " " + PlayerFacingStatus(snapshot.statusText));
			m_MultiplayerSubScreen = MultiplayerSubScreen::JoinSetup;
			RefreshMultiplayerScreenControls(snapshot);
			return;
		}
		m_JoinAttemptActive = false;
		m_JoinStatusText.clear();
	}
	// The options panel opened from the lobby survives a publish tick like Moderation does; one
	// opened from the setup screen outlives the service itself, being the draft of the next one.
	if (inMatchOrLobby && m_MultiplayerSubScreen != MultiplayerSubScreen::Moderation && m_MultiplayerSubScreen != MultiplayerSubScreen::HostOptions) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	} else if (!inMatchOrLobby && (m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby || m_MultiplayerSubScreen == MultiplayerSubScreen::Moderation ||
	            (m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions && !m_HostOptionsSetupDraft))) {
		m_MultiplayerSubScreen = MultiplayerSubScreen::Landing;
		m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus(GroupDelimiterForDisplay(FormatModuleMismatchStatus(snapshot.errorText))));
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
	const NetJoinRefusalOffer refusal = m_MultiplayerApplyOffered ? g_NetMatchService.JoinRefusalOffer() : NetJoinRefusalOffer::None;
	const bool applying = !recovering && !offering && refusal != NetJoinRefusalOffer::None;
	// A world whose slots are all held offers a wait beside the application; it stands where Resume does while it is offered.
	const bool waitOffered = landing && applying && refusal == NetJoinRefusalOffer::SlotsHeld;
	m_MainMenuButtons[MenuButton::MultiplayerWaitSlotButton]->SetVisible(waitOffered);
	m_MainMenuButtons[MenuButton::MultiplayerResumeGameButton]->SetVisible(!waitOffered);
	// 7e: the match died with its host and no successor took it. The prompt stays up and waits for that
	// host to come back: enabled once its row is listed again, or at once when there is no directory to
	// watch and the only route left is the address the player types.
	const bool awaiting = reconnect.IsAwaitingHostReturn() && !recovering;
	const bool hostBack = reconnect.HasHostReturned() || !reconnect.CanWatchHostReturn();
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetVisible(landing && (offering || applying || awaiting || reconnect.CanRetryManually()));
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetEnabled(offering ? (!awaiting || hostBack) : (applying || reconnect.CanRetryManually()));
	m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->SetText(offering ? "Rejoin Match" : (applying ? NetJoinRefusalApplyCaption(refusal) : "Retry"));
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->SetVisible(landing && (offering || applying || awaiting || recovering));
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->SetEnabled(offering || applying || awaiting || reconnect.CanCancel());
	// Stopping the attempts and setting the offer aside are different acts; the record survives both.
	m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->SetText(recovering ? "Stop retrying" : "Dismiss");
	// The recovery block stands above Host and Join while it is up; the rest of the landing moves down under it.
	const bool block = m_MainMenuButtons[MenuButton::MultiplayerReconnectButton]->GetVisible() || m_MainMenuButtons[MenuButton::MultiplayerCancelReconnectButton]->GetVisible();
	const int shift = block ? 30 : 0;
	m_MainMenuButtons[MenuButton::MultiplayerHostGameButton]->SetPositionRel(20, 76 + shift);
	m_MainMenuButtons[MenuButton::MultiplayerJoinGameButton]->SetPositionRel(156, 76 + shift);
	m_MainMenuButtons[MenuButton::MultiplayerResumeGameButton]->SetPositionRel(20, 114 + shift);
	m_MainMenuButtons[MenuButton::MultiplayerWaitSlotButton]->SetPositionRel(20, 114 + shift);
	m_MainMenuButtons[MenuButton::MultiplayerReplaysButton]->SetPositionRel(156, 114 + shift);
	m_MultiplayerLandingStatusLabel->SetPositionRel(12, 142 + shift);
	if (!landing) {
		return;
	}
	// A match its host ended by leaving is over: the landing names that, and nothing is offered to reconnect to.
	if (!recovering && !offering && !awaiting && g_NetMatchService.GetState() == NetMatchServiceState::Failed &&
	    g_NetMatchService.GetLobbySnapshot().errorText == "The host left the match") {
		const std::string landed = PlayerFacingStatus("The host left the match");
		if (m_MultiplayerLandingStatusLabel->GetText() != landed) m_MultiplayerLandingStatusLabel->SetText(landed);
		m_ReconnectStatusShown.clear();
		return;
	}
	if (awaiting && offering) {
		const std::string waiting = reconnect.GetHostReturnText();
		if (waiting != m_ReconnectStatusShown) {
			m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus(waiting));
			m_ReconnectStatusShown = waiting;
		}
		return;
	}
	if (applying) {
		const std::string offer = NetJoinRefusalOfferLine(refusal);
		if (offer != m_ReconnectStatusShown) {
			m_MultiplayerLandingStatusLabel->SetText(offer);
			m_ReconnectStatusShown = offer;
		}
		return;
	}
	// One persistent line, never a toast: the status while recovering, otherwise whatever the startup
	// scan of the recovery record found - including precisely why it cannot be used. No record is the
	// absence of an offer, not a verdict: it stays silent until the player asks to rejoin.
	const std::string status = recovering || reconnect.IsRefused() ? reconnect.GetStatusText()
	                                      : (reconnect.GetOffer() == NetReconnectOffer::Missing ? std::string() : reconnect.GetOfferText());
	if (status != m_ReconnectStatusShown) {
		// A recovery in progress owns the line. What the scan of the record found does not: it clears
		// its own sentence, but never replaces a refusal or an error the screen just put there.
		if (recovering || m_MultiplayerLandingStatusLabel->GetText() == m_ReconnectStatusShown) {
			m_MultiplayerLandingStatusLabel->SetText(PlayerFacingStatus(status));
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

void MainMenuGUI::TakeLobbyChat(const NetLobbySnapshot& snapshot) {
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
}

void MainMenuGUI::RefreshMultiplayerScreenControls(const NetLobbySnapshot& snapshot) {
	NetPlayerPresentation::Remember(snapshot, m_MultiplayerNameTextBox ? m_MultiplayerNameTextBox->GetText() : SavedMultiplayerName());
	const bool savingDiagnostics = TelemetryBundle::IsBusy();
	m_MainMenuButtons[MenuButton::SaveDiagnosticsButton]->SetEnabled(!savingDiagnostics);
	m_MainMenuButtons[MenuButton::SaveDiagnosticsButton]->SetText(savingDiagnostics ? "Saving report..." : "Save Support Report");
	const bool lobby = m_MultiplayerSubScreen == MultiplayerSubScreen::Lobby;
	const auto summary = g_NetMatchService.GetLastMatchSummary();
	m_LastMatchSummaryLabel->SetText(summary ? summary->LineText() : "");
	m_LastMatchSummaryLabel->SetVisible(lobby && summary.has_value());
	m_MainMenuButtons[MenuButton::LastMatchDetailsButton]->SetVisible(lobby && summary.has_value());
	m_MainMenuButtons[MenuButton::LastMatchDetailsButton]->SetEnabled(summary.has_value());
	if (!summary) m_LastMatchDetailsLabel->SetText("");
	// One sub-screen is up at a time; the renderer draws a panel only under visible parents.
	m_MultiplayerLandingPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::Landing);
	m_MultiplayerHostPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::HostSetup);
	m_MultiplayerJoinPanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::JoinSetup);
	if (m_MultiplayerResumePanel) m_MultiplayerResumePanel->SetVisible(m_MultiplayerSubScreen == MultiplayerSubScreen::ResumeSetup);
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
	// A host page hides the lobby's chat band, so a line that lands while it is open is shown above the page instead.
	const bool pageHidesChat = m_MultiplayerSubScreen == MultiplayerSubScreen::HostOptions && g_NetMatchService.GetState() != NetMatchServiceState::Idle;
	if (pageHidesChat) TakeLobbyChat(snapshot);
	if (m_PageChatNotice) {
		const bool notice = pageHidesChat && !m_MultiplayerLobbyChatLines.empty();
		m_PageChatNotice->SetVisible(notice);
		if (notice) {
			const int x = m_HostOptionsPanel->GetXPos(), width = m_HostOptionsPanel->GetWidth();
			m_PageChatNotice->SetPositionAbs(x, std::max(0, m_HostOptionsPanel->GetYPos() - m_PageChatNotice->GetHeight() - 2));
			if (m_PageChatNotice->GetWidth() != width) m_PageChatNotice->Resize(width, m_PageChatNotice->GetHeight());
			std::string line = "Chat - " + m_MultiplayerLobbyChatLines.back();
			m_PageChatNotice->SetText(line);
			// One line: the newest message's end gives way to an ellipsis before the notice wraps.
			while (m_PageChatNotice->GetTextWidth() > width - 4 && line.size() > 8) {
				line.pop_back();
				m_PageChatNotice->SetText(line + "...");
			}
		}
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
			// The panel includes the NAT row and its routing hint at every size.
			RefreshHostOptionsControls(snapshot);
			m_HostOptionsShownDraft = m_HostOptionsDraft;
			const int panelWidth = m_HostOptionsPanel->GetWidth();
			const int panelHeight = m_HostOptionsPanel->GetHeight();
			FitMultiplayerScreen(panelWidth, panelHeight + m_MainMenuButtons[MenuButton::BackToMainButton]->GetHeight() + 5);
			LayoutMultiplayerFooter(panelWidth, panelHeight);
			// Late rows open upward so their full lists stay above the footer.
			for (GUICollectionBox* page : m_HostOptionsPages) {
				for (GUIControl* control : *page->GetChildren()) {
					if (auto* combo = dynamic_cast<GUIComboBox*>(control)) {
						const int below = combo->GetYPos() + combo->GetHeight();
						const int bottom = m_MainMenuButtons[MenuButton::HostOptionsBackButton]->GetYPos() - 4;
						combo->GetListPanel()->SetPositionAbs(combo->GetXPos(), below + combo->GetDropHeight() <= bottom
						                                                        ? below : combo->GetYPos() - combo->GetDropHeight());
					}
				}
			}
			return;
		}
		int contentWidth = 300;
		int contentHeight = 250;
		if (m_MultiplayerSubScreen == MultiplayerSubScreen::JoinSetup && m_MultiplayerLanGamesLabel) {
			// A long state line scrolls inside the list's width rather than widening the screen.
			const bool scroll = m_MultiplayerLanGamesLabel->GetTextWidth() > m_MultiplayerLanGamesLabel->GetWidth();
			m_MultiplayerLanGamesLabel->SetHorizontalOverflowScroll(scroll);
			m_MultiplayerLanGamesLabel->ActivateDeactivateOverflowScroll(scroll);
			const bool joining = m_JoinAttemptActive;
			m_MainMenuButtons[MenuButton::MultiplayerJoinBackButton]->SetText(joining ? "Cancel" : "Back");
			m_MainMenuButtons[MenuButton::JoinByAddressButton]->SetEnabled(!joining);
			const int selected = m_MultiplayerLanGamesList ? m_MultiplayerLanGamesList->GetSelectedIndex() : -1;
			const bool pickable = selected >= 0 && static_cast<size_t>(selected) < m_GameRows.size() && m_GameRows[static_cast<size_t>(selected)].joinable;
			m_MainMenuButtons[MenuButton::MultiplayerConnectButton]->SetEnabled(pickable && !joining);
		}
		if (m_MultiplayerSubScreen == MultiplayerSubScreen::HostSetup && m_MultiplayerHostPanel) {
			contentHeight = m_MultiplayerHostPanel->GetHeight();
		}
		if (m_MultiplayerSubScreen == MultiplayerSubScreen::ResumeSetup && m_MultiplayerResumePanel) {
			RefreshResumeControls();
			contentHeight = m_MultiplayerResumePanel->GetHeight();
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
	// Every seat of the roster is a row: the people, the AI teams, seats still waiting for their player and players the AI covers for.
	const bool persistentWorld = g_NetMatchService.GetLobbyMatchConfig().persistentWorld;
	const auto isOpenSeat = [persistentWorld](const NetLobbyMember& member) {
		return !member.connected && !member.cpu && !member.isLocal &&
		       member.displayName == NetMatchConfigUtil::UnseatedSlotName(member.peerId, persistentWorld);
	};
	const std::string localName = m_MultiplayerNameTextBox && !m_MultiplayerNameTextBox->GetText().empty() ? m_MultiplayerNameTextBox->GetText() : SavedMultiplayerName();
	std::vector<std::string> rowName, rowTail, rowTailCompact;
	std::vector<std::string> notReady;
	std::string hostName;
	size_t seatsToFill = 0;
	for (const NetLobbyMember& member : snapshot.members) {
		const bool open = isOpenSeat(member);
		const bool host = member.peerId == snapshot.hostPeerId && !member.cpu;
		const std::string name = open ? std::string("Open seat") : member.cpu ? std::string("AI") : LobbyRowName(member, localName);
		if (host) hostName = name;
		// The seat's own line from the return machinery ("name: state") is more exact than the lobby's word for it.
		const std::string seatLine = member.statusLine.empty() ? std::string() : member.statusLine.substr(member.statusLine.rfind(": ") == std::string::npos ? 0 : member.statusLine.rfind(": ") + 2);
		std::string state;
		if (open) {
			state = "waiting for a player";
			++seatsToFill;
		} else if (member.cpu) {
			state = "";
		} else if (!seatLine.empty()) {
			state = seatLine;
		} else if (member.dropped || member.aiHeld) {
			state = "Disconnected - AI plays";
		} else if (member.joining) {
			state = "Joining";
		} else if (host) {
			state = "Host";
		} else if (!snapshot.joiningWorld) {
			state = member.ready ? "Ready" : "Not ready";
		}
		if (!open && !member.cpu && !host && !member.isLocal && !member.ready && member.connected && !member.dropped && !member.joining) notReady.push_back(name);
		const std::string you = member.isLocal ? " (you)" : "";
		rowName.push_back(name + you);
		rowTail.push_back(" - Team " + std::to_string(member.team + 1) + (state.empty() ? std::string() : " - " + state));
		rowTailCompact.push_back(state.empty() ? " - Team " + std::to_string(member.team + 1) : " - " + state);
	}
	const int contentWidth = 300;
	const int rowBoxWidth = contentWidth - 24;
	// Four rows keep the original pitch; a fuller roster packs its rows so the screen stays inside the smallest viewport.
	const size_t rows = std::min(rowName.size(), m_MultiplayerLobbyPlayerLabels.size());
	const int rowPitch = rows > 4 ? 14 : 18;
	const int rowsExtra = std::max(0, static_cast<int>(rows) * rowPitch - 4 * 18);
	for (size_t i = 0; i < m_MultiplayerLobbyPlayerLabels.size(); ++i) {
		GUILabel* label = m_MultiplayerLobbyPlayerLabels[i];
		if (!label) continue;
		label->SetVisible(i < rows);
		if (i >= rows) {
			label->SetText("");
			continue;
		}
		label->SetPositionRel(8, 82 + static_cast<int>(i) * rowPitch);
		if (label->GetHeight() != rowPitch - 2) label->Resize(label->GetWidth(), rowPitch - 2);
	}

	// One sentence says what the lobby waits for and what to press.
	const auto seconds = [](uint32_t ms) { return std::to_string((ms + 999) / 1000); };
	const auto names = [](const std::vector<std::string>& list) {
		if (list.empty()) return std::string();
		if (list.size() == 1) return list.front();
		if (list.size() == 2) return list[0] + " and " + list[1];
		return list[0] + ", " + list[1] + " and " + std::to_string(list.size() - 2) + " more";
	};
	std::string sentence;
	if (!snapshot.transferLine.empty() || !snapshot.waitLine.empty()) {
		// A join in progress names what it waits on: the world's image, or one of its held slots and the seconds left.
		sentence = !snapshot.transferLine.empty() ? snapshot.transferLine : snapshot.waitLine;
	} else if (!snapshot.inLobby) {
		sentence = PlayerFacingStatus(snapshot.statusText);
	} else if (snapshot.isHost) {
		if (seatsToFill > 0 || !snapshot.occupancyComplete) {
			const size_t missing = std::max<size_t>(seatsToFill, 1);
			sentence = "Waiting for " + std::to_string(missing) + (missing == 1 ? " more player to join" : " more players to join");
		} else if (snapshot.startCountdownRunning) {
			sentence = notReady.empty() ? std::string("Starting the match...")
			                            : "Starting in " + seconds(snapshot.startCountdownMs) + " s - waiting for " + names(notReady) + " to press Ready";
		} else if (snapshot.remoteReady || notReady.empty()) {
			sentence = "Everyone is ready - press Start Match";
		} else {
			sentence = "Waiting for " + names(notReady) + " to press Ready - Start Match starts in 30 s";
		}
	} else if (snapshot.joiningWorld) {
		sentence = PlayerFacingStatus(snapshot.statusText);
	} else {
		const bool ready = g_NetMatchService.IsReadyRequested();
		if (snapshot.startCountdownRunning) {
			sentence = "The host is starting the match in " + seconds(snapshot.startCountdownMs) + " s" + (ready ? std::string() : " - press Ready");
		} else if (snapshot.readyClearedBySetup) {
			sentence = "The host changed the setup - check it and press Ready again";
		} else if (ready) {
			sentence = "You're ready - waiting for the host to start the match";
		} else {
			sentence = "Press Ready when you're ready to play";
		}
	}
	// A sentence wider than its row breaks at its last " - " into two lines rather than lose its end.
	if (m_MultiplayerLobbyPlayerRowFont && m_MultiplayerLobbyPlayerRowFont->CalculateWidth(sentence, m_MultiplayerLobbyPlayerRowFallbackFont) > rowBoxWidth) {
		if (const size_t dash = sentence.rfind(" - "); dash != std::string::npos) sentence.replace(dash, 3, "\n");
	}
	m_MultiplayerStatusLabel->SetText(sentence);
	m_MultiplayerStatusLabel->SetPositionRel(12, 160 + rowsExtra);
	m_MultiplayerStatusLabel->SetHorizontalOverflowScroll(false);
	m_MultiplayerStatusLabel->ActivateDeactivateOverflowScroll(false);
	// The lobby panel is one width on every peer: a longer row or status elides inside its box
	// rather than widening the panel, so host and client land on the same rectangle.
	if (m_MultiplayerLobbyPlayerRowFont) {
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
	const int statusExtra = statusHeight - 16 + rowsExtra;

	// The host's listing, in words: where a friend finds this game.
	std::string listing;
	if (snapshot.isHost && snapshot.inLobby) {
		std::string reason;
		switch (g_NetMatchService.GetListingStatus(&reason)) {
			case NetListingStatus::Listed: listing = "Listed online as " + (hostName.empty() ? localName : hostName) + " - your friend picks it in Join a Game"; break;
			case NetListingStatus::Opening: listing = "Opening the online listing..."; break;
			case NetListingStatus::Unlisted: listing = "Not in the online game list - friends join by your address"; break;
			case NetListingStatus::LocalOnly: listing = "Shown to players on your network only"; break;
			case NetListingStatus::Failed: listing = "Not listed online - " + (reason.empty() ? std::string("the online game list is unavailable") : reason); break;
			case NetListingStatus::None: break;
		}
	}
	if (listing != m_MultiplayerLobbyPortMapLabel->GetText()) m_MultiplayerLobbyPortMapLabel->SetText(listing);
	m_MultiplayerLobbyPortMapLabel->SetVisible(!listing.empty());
	if (GUIFont* font = m_MultiplayerLobbyPlayerRowFallbackFont) {
		m_MultiplayerLobbyPortMapLabel->EnsureDrawableTextFont("FontSmall.png");
		m_MultiplayerLobbyPortMapLabel->SetFont(font);
		const bool wide = font->CalculateWidth(listing) > rowBoxWidth;
		m_MultiplayerLobbyPortMapLabel->SetHorizontalOverflowScroll(wide);
		m_MultiplayerLobbyPortMapLabel->ActivateDeactivateOverflowScroll(wide);
	}
	const int summaryHeight = summary ? 20 : 0;
	m_LastMatchSummaryLabel->SetPositionRel(12, 178 + statusExtra);
	m_LastMatchSummaryLabel->Resize(contentWidth - 90, 18);
	m_MainMenuButtons[MenuButton::LastMatchDetailsButton]->SetPositionRel(contentWidth - 74, 178 + statusExtra);
	m_MultiplayerLobbyPortMapLabel->SetPositionRel(12, 178 + statusExtra + summaryHeight);
	const int portMapHeight = listing.empty() ? 0 : 14;
	m_MultiplayerErrorLabel->SetText(GroupDelimiterForDisplay(PlayerFacingStatus(snapshot.errorText)));
	m_MultiplayerErrorLabel->EnsureDrawableTextFont("FontSmall.png");
	const int desiredWidth = std::max(300, m_MultiplayerErrorLabel->GetMaxWordWidth() + 24);
	FitMultiplayerPanelWidth(m_MultiplayerLobbyPanel, m_MultiplayerErrorLabel, contentWidth, {});
	for (size_t i = 0; i < rows; ++i) {
		GUILabel* label = m_MultiplayerLobbyPlayerLabels[i];
		if (!label) continue;
		label->EnsureDrawableTextFont("FontSmall.png");
		GUIFont* font = m_MultiplayerLobbyPlayerRowFallbackFont;
		if (font) label->SetFont(font);
		const int width = std::min(rowBoxWidth, label->GetWidth());
		const auto fits = [font, width](const std::string& text) { return !font || font->CalculateWidth(text) <= width; };
		const std::string& name = rowName[i];
		std::string tail = rowTail[i];
		if (!fits(name + tail)) tail = rowTailCompact[i];
		if (!fits(name + tail)) {
			while (!tail.empty() && !fits(name + tail + "...")) tail.pop_back();
			if (!tail.empty()) tail += "...";
		}
		const bool scrollName = !fits(name);
		label->SetHorizontalOverflowScroll(scrollName);
		label->ActivateDeactivateOverflowScroll(scrollName);
		label->SetText(name + (scrollName ? std::string() : tail));
	}
	// The listing row sits under the wrapped status, so the error starts below both.
	m_MultiplayerErrorLabel->SetPositionRel(12, 178 + statusExtra + portMapHeight + summaryHeight);
	// The screen must stay inside the viewport. Error, status and listing rows keep every pixel
	// their room allows (overflow scrolls); the chat block is the one piece that yields - a row at
	// a time - before any of them do.
	const int backReserve = m_MainMenuButtons[MenuButton::BackToMainButton]->GetHeight() + 5;
	const int fixedExtra = statusExtra + portMapHeight + summaryHeight;
	const int panelCap = g_WindowMan.GetResY() - backReserve; // the Back button's band sits under the panel
	const int inputBlock = 37;                     // textbox 13 px, the 10 px version line 2 px under it, a bottom margin matching its sides
	// The Leave/Advanced row ends at rel 240; the first chat row keeps a 4px gap under it and the
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
	TakeLobbyChat(snapshot);
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
	if (m_MultiplayerLobbyVersionLabel) {
		m_MultiplayerLobbyVersionLabel->SetPositionRel(chatX, chatTop + chatRows * 10 + 15);
		if (m_MultiplayerLobbyVersionLabel->GetWidth() != chatW) {
			m_MultiplayerLobbyVersionLabel->Resize(chatW, 10);
		}
		// FontSmall, the font the line was given.
		GUIFont* font = m_MultiplayerLobbyPlayerRowFallbackFont;
		const bool wide = font && font->CalculateWidth(m_MultiplayerLobbyVersionLabel->GetText()) > chatW;
		m_MultiplayerLobbyVersionLabel->SetHorizontalOverflowScroll(wide);
		m_MultiplayerLobbyVersionLabel->ActivateDeactivateOverflowScroll(wide);
		m_MultiplayerLobbyVersionLabel->SetVisible(true);
	}

	if (m_MultiplayerLobbyPanel->GetHeight() != contentHeight) {
		m_MultiplayerLobbyPanel->Resize(contentWidth, contentHeight);
	}
	const int screenHeight = contentHeight + backReserve;
	FitMultiplayerScreen(contentWidth, screenHeight);
	// The footer is fixed: the primary button (Ready, or the host's Start Match), then Leave, the running host's Players and
	// the host's Advanced on one row.
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
	// §9b: moderation is a match feature - a lobby seat whose holder leaves goes straight back in the pool.
	const bool moderation = snapshot.isHost && snapshot.running;
	const int pairWidth = leave->GetWidth() + (snapshot.isHost ? pairGap + options->GetWidth() : 0) + (moderation ? pairGap + seats->GetWidth() : 0);
	const int pairLeft = (contentWidth - pairWidth) / 2;
	const int primaryWidth = std::max(pairWidth, leave->GetWidth() + pairGap + options->GetWidth());
	for (GUIButton* primary: {m_MainMenuButtons[MenuButton::MultiplayerReadyButton], m_MainMenuButtons[MenuButton::MultiplayerStartButton]}) {
		if (primary->GetWidth() != primaryWidth) {
			primary->Resize(primaryWidth, primary->GetHeight());
		}
		primary->SetPositionRel((contentWidth - primaryWidth) / 2, 208 + extraHeight);
	}
	leave->SetPositionRel(pairLeft, 236 + extraHeight);
	LayoutMultiplayerFooter(contentWidth, contentHeight);

	// Every player but the host readies up, and can take it back until the match starts.
	GUIButton* ready = m_MainMenuButtons[MenuButton::MultiplayerReadyButton];
	ready->SetVisible(!snapshot.isHost && !snapshot.joiningWorld);
	ready->SetEnabled(!snapshot.isHost && !snapshot.joiningWorld && snapshot.inLobby);
	ready->SetText(g_NetMatchService.IsReadyRequested() ? "Cancel Ready" : "Ready");
	// The host starts at once when everyone is ready; otherwise Start Match counts down for everyone, and the same button cancels it.
	// A seat nobody has taken keeps it disabled, and the sentence above says who is missing.
	GUIButton* start = m_MainMenuButtons[MenuButton::MultiplayerStartButton];
	start->SetVisible(snapshot.isHost);
	start->SetEnabled(snapshot.isHost && snapshot.inLobby && (snapshot.startCountdownRunning || snapshot.occupancyComplete));
	start->SetText(snapshot.startCountdownRunning ? "Cancel Start" : "Start Match");
	leave->SetEnabled(true);
	// While the world's image comes, or a held slot is waited for, the exit cancels the join, and says so.
	leave->SetText(snapshot.transferLine.empty() && snapshot.waitLine.empty() ? "Leave" : "Cancel");
	seats->SetPositionRel(pairLeft + leave->GetWidth() + pairGap, 236 + extraHeight);
	seats->SetVisible(moderation);
	seats->SetEnabled(moderation);
	// The host's network tuning sits behind one door; the setup itself is edited from the top of the lobby.
	options->SetPositionRel(pairLeft + leave->GetWidth() + pairGap + (moderation ? seats->GetWidth() + pairGap : 0), 236 + extraHeight);
	options->SetText("Advanced");
	options->SetVisible(snapshot.isHost);
	options->SetEnabled(snapshot.isHost && snapshot.inLobby);
	if (GUIButton* editSetup = m_MainMenuButtons[MenuButton::LobbyEditSetupButton]) {
		editSetup->SetText(snapshot.isHost ? "Edit setup" : "Match details");
		editSetup->SetEnabled(snapshot.inLobby);
	}
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
		m_ReplayStatusLabel->SetText(error == std::errc::no_such_file_or_directory ? "No recorded matches yet." : "The recordings could not be read: " + error.message());
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
		std::string when;
		std::string what = "Recorded match";
		std::string players;
		std::string length;
		const auto written = entry.last_write_time(fileError);
		if (!fileError) {
#ifdef _MSC_VER
			const auto convertedTime = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
			const auto convertedTime = fs::file_time_type::clock::to_sys(written);
#endif
			const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(convertedTime);
			const std::time_t time = std::chrono::system_clock::to_time_t(systemTime);
			// The player reads the file's date in their own time zone, so no zone suffix is written.
			when = System::LocalTimeText(time, "%Y-%m-%d %H:%M");
		}
		NetMatchReplayReader reader;
		if (reader.Open(row.path, &row.error)) {
			const auto& config = reader.GetConfig();
			what = config.activityPreset + " on " + config.sceneName;
			players = std::to_string(config.peerCount) + " players";
			reader.Close();
			NetReplayVerifyReport scan;
			if (NetMatchReplayReader::Verify(row.path, scan)) {
				NetMatchSummary duration;
				duration.runningTicks = scan.lastFrame;
				length = duration.DurationText();
			} else {
				row.error = scan.error;
			}
		}
		row.text = (when.empty() ? std::string() : when + " - ") + what + (players.empty() ? std::string() : " - " + players) +
		           (length.empty() ? std::string() : " - " + length) + (row.error.empty() ? std::string() : " - cannot be played");
		row.details = row.error.empty() ? what + (players.empty() ? std::string() : ", " + players) + (length.empty() ? std::string() : ", " + length) +
		                                     ". Play watches the match from its start.\nFile: " + entry.path().filename().string()
		                                   : "This recording cannot be played: " + row.error + "\nFile: " + entry.path().filename().string();
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
	m_ReplayStatusLabel->SetText(error ? "The recordings could not all be read: " + error.message()
	                                   : m_ReplayRows.empty() ? std::string("No recorded matches yet.")
	                                   : std::to_string(m_ReplayRows.size()) + (m_ReplayRows.size() == 1 ? " recorded match" : " recorded matches"));
	RefreshReplayBrowserControls();
}

void MainMenuGUI::RefreshResumeList() {
	if (!m_ResumeMatchesList) {
		return;
	}
	const int selected = m_ResumeMatchesList->GetSelectedIndex();
	const std::string keep = selected >= 0 && static_cast<size_t>(selected) < m_ResumeRows.size() ? m_ResumeRows[selected].matchId : "";
	m_ResumeRows.clear();
	m_ResumeMatchesList->ClearList();
	for (const AutosaveDescriptor& checkpoint: AutosaveStore::ListResumable()) {
		AutosaveManifest manifest;
		if (!AutosaveStore::ReadManifest(checkpoint.path.parent_path(), checkpoint.matchId, checkpoint.savedTick, manifest)) {
			continue;
		}
		ResumeRow row;
		row.matchId = checkpoint.matchId;
		row.tick = checkpoint.savedTick;
		// How far the match got, as the clock the players watched, not as a tick count.
		NetMatchSummary reached;
		reached.runningTicks = checkpoint.savedTick;
		std::string age = "just now";
		std::string savedAt;
		std::error_code status;
		const auto written = std::filesystem::last_write_time(checkpoint.path, status);
		if (!status) {
			const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::filesystem::file_time_type::clock::now() - written).count();
			age = minutes >= 1440 ? std::to_string(minutes / 1440) + "d ago" : (minutes >= 60 ? std::to_string(minutes / 60) + "h ago" : std::to_string(std::max<long long>(minutes, 0)) + "m ago");
			const auto wall = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(written - std::filesystem::file_time_type::clock::now());
			savedAt = System::LocalTimeText(std::chrono::system_clock::to_time_t(wall), "%Y-%m-%d %H:%M");
		}
		std::string playerList;
		for (size_t index = 0; index < manifest.peerNames.size(); ++index) playerList += (index == 0 ? "" : ", ") + manifest.peerNames[index];
		row.text = (savedAt.empty() ? age : savedAt) + " - " + manifest.activityPreset + " on " + manifest.scenePreset + " - " + reached.DurationText() + " played" +
		           (manifest.peerNames.empty() ? std::string() : " - " + std::to_string(manifest.peerNames.size()) + " players");
		row.details = manifest.activityPreset + " on " + manifest.scenePreset + ", " + reached.DurationText() + " played, saved " + age +
		              (manifest.savedByHost ? " by the host" : "") + "." + (playerList.empty() ? std::string() : "\nPlayers: " + playerList) +
		              "\nYou will host this saved match; the other players can rejoin with the seats they had.";
		m_ResumeRows.push_back(std::move(row));
	}
	m_ResumeMatchesList->BeginUpdate();
	int restore = m_ResumeRows.empty() ? -1 : 0;
	for (size_t index = 0; index < m_ResumeRows.size(); ++index) {
		m_ResumeMatchesList->AddItem(m_ResumeRows[index].text);
		if (m_ResumeRows[index].matchId == keep) restore = static_cast<int>(index);
	}
	m_ResumeMatchesList->EndUpdate();
	m_ResumeMatchesList->SetSelectedIndex(restore);
	if (m_ResumeStatusLabel) {
		m_ResumeStatusLabel->SetText(m_ResumeRows.empty() ? std::string("No saved matches found.")
		                                                  : std::to_string(m_ResumeRows.size()) + (m_ResumeRows.size() == 1 ? " saved match" : " saved matches"));
	}
	RefreshResumeControls();
}

void MainMenuGUI::RefreshResumeControls() {
	if (!m_ResumeMatchesList || !m_ResumeSelectedLabel) {
		return;
	}
	const int selected = m_ResumeMatchesList->GetSelectedIndex();
	const bool hasSelection = selected >= 0 && static_cast<size_t>(selected) < m_ResumeRows.size();
	m_ResumeSelectedLabel->SetText(hasSelection ? m_ResumeRows[selected].details : "Pick a saved match to host it again.");
	m_ResumeSelectedLabel->SetVerticalOverflowScroll(true);
	m_ResumeSelectedLabel->ActivateDeactivateOverflowScroll(true);
	if (m_ResumeStatusLabel) m_ResumeStatusLabel->ActivateDeactivateOverflowScroll(true);
	if (GUIButton* start = m_MainMenuButtons[MenuButton::ResumeStartButton]) {
		start->SetEnabled(hasSelection);
	}
}

void MainMenuGUI::StartSelectedResume() {
	const int selected = m_ResumeMatchesList ? m_ResumeMatchesList->GetSelectedIndex() : -1;
	if (selected < 0 || static_cast<size_t>(selected) >= m_ResumeRows.size()) {
		return;
	}
	NetMatchServiceRequest request;
	request.host = true;
	request.playerName = m_MultiplayerNameTextBox->GetText().empty() ? "Host" : m_MultiplayerNameTextBox->GetText();
	request.port = static_cast<uint16_t>(std::max(1, std::atoi(m_MultiplayerHostPortTextBox->GetText().c_str())));
	request.resyncOnDesync = true;
	// The saved policy, the way the ordinary host path reads it; the resumed configuration's own
	// policy replaces this in PrepareResume, because that is what the peers agreed to play on.
	request.autoInputDelay = g_SettingsMan.GetNetworkHostDelayPolicy() == SettingsMan::NetworkHostDelayPolicy::Auto;
	// The manifest authors the roster; the request only names which checkpoint to stand on.
	request.resumeMatchId = m_ResumeRows[selected].matchId;
	request.resumeTick = m_ResumeRows[selected].tick;
	std::string error;
	if (!g_NetMatchService.Start(request, &error)) {
		if (m_ResumeStatusLabel) m_ResumeStatusLabel->SetText(error);
		g_GUISound.UserErrorSound()->Play();
		return;
	}
	m_MultiplayerSubScreen = MultiplayerSubScreen::Lobby;
	g_GUISound.ButtonPressSound()->Play();
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
	m_ReplaySelectedLabel->SetText(hasSelection ? m_ReplayRows[selected].details : "Pick a recorded match to watch or delete it.");
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

bool MainMenuGUI::AutomationRowOf(const std::string& name, std::string& listName, int& row) const {
	// The list shows every engine beaconing on the network, so a scripted join names its own session's row by port.
	if (name.starts_with("GameRowPort")) {
		const std::string number = name.substr(11);
		if (number.empty() || number.size() > 5 || number.find_first_not_of("0123456789") != std::string::npos) return false;
		const unsigned long port = std::stoul(number);
		const auto found = std::find_if(m_GameRows.begin(), m_GameRows.end(), [port](const NetDirectoryClient::GameRow& candidate) { return candidate.port == port; });
		if (found == m_GameRows.end()) return false;
		listName = m_MultiplayerLanGamesList->GetName();
		row = static_cast<int>(found - m_GameRows.begin());
		return true;
	}
	if (name.starts_with("GameRow")) {
		const std::string number = name.substr(7);
		if (number.empty() || number.size() > 3 || number.find_first_not_of("0123456789") != std::string::npos) return false;
		listName = m_MultiplayerLanGamesList->GetName();
		row = std::stoi(number);
		return true;
	}
	if (const auto index = ReplayRowIndex(name)) {
		listName = m_ReplayList->GetName();
		row = static_cast<int>(*index);
		return true;
	}
	return false;
}

std::string MainMenuGUI::AutomationModelText() const {
	const auto draft = [](const NetMatchConfig& config) {
		std::ostringstream text;
		text << NetMatchConfigUtil::BuildReportJson(NetMatchConfigUtil::WithoutRelay(config)) << " delay=" << static_cast<int>(config.delayPolicy) << "/" << config.inputDelayFrames
		     << " slow=" << config.slowPlayerBoundTicks << "/" << static_cast<int>(config.slowPlayerPolicy) << " autosave=" << config.autosaveEnabled << "/" << config.autosaveIntervalSeconds
		     << " idle=" << static_cast<int>(config.idleWaitMinutes) << " repair=" << config.automaticRepair << " horizon=" << config.pathHorizonTicks
		     << " redundancy=" << static_cast<int>(config.frameRedundancyTicks) << " return=" << static_cast<int>(config.returnWindowMinutes) << " peers=";
		for (const uint16_t delay: config.peerInputDelayFrames) text << delay << ",";
		return text.str();
	};
	std::ostringstream text;
	text << "host activity=" << m_MultiplayerHostActivityIndex << " scene=" << m_MultiplayerHostSceneIndex << " mode=" << static_cast<int>(m_MultiplayerHostMode)
	     << " players=" << static_cast<int>(m_MultiplayerHostPeerCount) << " port=" << (m_MultiplayerHostPortTextBox ? m_MultiplayerHostPortTextBox->GetText() : "")
	     << " name=" << (m_MultiplayerNameTextBox ? m_MultiplayerNameTextBox->GetText() : "")
	     << " join=" << (m_MultiplayerJoinAddressTextBox ? m_MultiplayerJoinAddressTextBox->GetText() : "") << ":" << (m_MultiplayerJoinPortTextBox ? m_MultiplayerJoinPortTextBox->GetText() : "")
	     << "\ndraft " << draft(m_HostOptionsDraft);
	if (m_HostSetupOptions) text << "\nstaged " << draft(*m_HostSetupOptions);
	const HostComputerDraft& computer = m_HostComputerDraft;
	text << "\ncomputer listing=" << static_cast<int>(computer.listing) << " portmap=" << computer.portMap << " port=" << computer.port << " ice=" << computer.ice
	     << " relay=" << static_cast<int>(computer.relay) << " relayfields=" << computer.relayFields[0] << "|" << computer.relayFields[1] << "|" << computer.relayFields[2].size()
	     << " history=" << computer.joinHistorySeconds << " lag=" << computer.joinLagSeconds << " widget=" << static_cast<int>(computer.statusWidget)
	     << "\nlobby ready=" << g_NetMatchService.IsReadyRequested() << " countdown=" << g_NetMatchService.GetLobbySnapshot().startCountdownRunning;
	return text.str();
}

GUIControl* MainMenuGUI::AutomationModalDialog() const {
	return m_ActiveMenuScreen == MenuScreen::MultiplayerScreen ? m_ActiveDialogBox : nullptr;
}

bool MainMenuGUI::AutomationSetupHostPort(const std::string& port) {
	char* end = nullptr;
	const long parsed = std::strtol(port.c_str(), &end, 10);
	if (port.empty() || *end != '\0' || parsed < 1 || parsed > 65535 || !m_MultiplayerHostPortTextBox) return false;
	m_MultiplayerHostPortTextBox->SetText(std::to_string(parsed));
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
	if (!g_MenuMan.GetIsInMenuScreen()) return "Gameplay";
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
		case MultiplayerSubScreen::ResumeSetup: return "ResumeSetup";
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
	// A port the screen did not write is the player's, so the list stops refilling it. Read every frame:
	// the control manager drops a change event raised outside its own update.
	if (m_MultiplayerJoinPortTextBox && m_MultiplayerJoinPortTextBox->GetText() != m_JoinPortAutoValue) {
		m_JoinPortAutoValue.clear();
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
	if (m_MultiplayerLanGamesLabel) {
		// Three different states, never one sentence: still looking, nothing there, or the online list out of reach.
		std::string state;
		if (directoryUrl.empty()) {
			state = m_GameRows.empty() ? "No online game list is set. Games on this network show here." : "Games on this network:";
		} else if (!m_DirectoryBrowser.ListError().empty()) {
			state = m_GameRows.empty() ? "The online game list is unavailable. Games on this network still show here."
			                           : "The online game list is unavailable - showing games on this network.";
		} else if (m_GameRows.empty()) {
			state = m_DirectoryBrowser.ListReplies() == 0 ? "Looking for games..." : "No games found. Ask your friend to host, or host one yourself.";
		} else {
			state = m_GameRows.size() == 1 ? std::string("1 game found - pick it and press Join Game.")
			                              : std::to_string(m_GameRows.size()) + " games found - pick one and press Join Game.";
		}
		if (m_MultiplayerLanGamesLabel->GetText() != state) m_MultiplayerLanGamesLabel->SetText(state);
	}
	if (m_JoinSelectedLabel) {
		// The join's own outcome first; otherwise the selected game, whole, and why it cannot be joined.
		const int selected = m_MultiplayerLanGamesList->GetSelectedIndex();
		std::string detail = m_JoinStatusText;
		if (detail.empty() && selected >= 0 && static_cast<size_t>(selected) < m_GameRows.size()) {
			const NetDirectoryClient::GameRow& row = m_GameRows[static_cast<size_t>(selected)];
			detail = row.name + ": " + row.activity + (row.scene.empty() ? std::string() : " on " + row.scene) + ", " + row.players + " players, " +
			         (row.source == "LAN" ? "this network" : "internet") + (row.joinable ? std::string() : "\nCannot be joined: " + GameRowReasonWords(row.reason));
		}
		if (m_JoinSelectedLabel->GetText() != detail) m_JoinSelectedLabel->SetText(detail);
	}
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
		NetIdentity::StampOptionsForTarget(identityOptions, false);
		std::string identityError;
		const bool identityBuilt = NetIdentity::BuildCurrentManifest(manifest, &identityError, identityOptions);
		NetIdentityManifest worldManifest;
		NetIdentityBuildOptions worldOptions = identityOptions;
		NetIdentity::StampOptionsForTarget(worldOptions, true);
		const bool worldBuilt = NetIdentity::BuildCurrentManifest(worldManifest, &identityError, worldOptions);
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
		if (worldBuilt) {
			NetDirectoryLocalIdentity worldLocal;
			worldLocal.networkProtocolVersion = worldManifest.networkProtocolVersion;
			worldLocal.lockstepCodecVersion = worldManifest.deterministicConfig.lockstepCodecVersion;
			worldLocal.controllerFrameVersion = worldManifest.controllerFrameVersion;
			worldLocal.sessionIdentityHash = NetIdentity::HashHex(worldManifest.sessionIdentityHash);
			worldLocal.moduleManifestHash = NetIdentity::HashHex(worldManifest.moduleManifestHash);
			m_DirectoryWorldIdentity = worldLocal;
		}
	}
	const NetDirectoryLocalIdentity* worldIdent = m_DirectoryWorldIdentity ? &*m_DirectoryWorldIdentity : nullptr;
	std::vector<NetDirectoryClient::GameRow> rows = NetDirectoryClient::MergeGameLists(hosts, m_DirectoryBrowser.Rows(), m_DirectoryIdentity.value_or(NetDirectoryLocalIdentity{}), worldIdent);
	if (!m_DirectoryIdentity) {
		// Without the local identity no NET row can be proven compatible.
		for (NetDirectoryClient::GameRow& row: rows) {
			if (row.source == "NET") {
				row.joinable = false;
				row.reason = "identity";
			}
		}
	}
	// One row per session: a host beaconing on several interfaces is one game, listed at its best
	// address - loopback when the host is this machine, else the address the beacon came in on.
	std::vector<NetDirectoryClient::GameRow> listed;
	listed.reserve(rows.size());
	for (NetDirectoryClient::GameRow& row: rows) {
		const auto same = std::find_if(listed.begin(), listed.end(), [&row](const NetDirectoryClient::GameRow& kept) {
			return kept.source == row.source && kept.port == row.port && kept.name == row.name &&
			       kept.activity == row.activity && kept.mode == row.mode && kept.sessionId == row.sessionId;
		});
		if (same == listed.end()) {
			listed.push_back(std::move(row));
		} else if (IsLoopbackAddress(row.address) && !IsLoopbackAddress(same->address)) {
			same->address = row.address;
		}
	}
	rows = std::move(listed);
	const auto describe = [this](const NetDirectoryClient::GameRow& row) {
		return FitDiscoveredGameRow(row, m_MultiplayerLanGamesList->GetFont(), std::max(1, m_MultiplayerLanGamesList->GetWidth() - 29));
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
	m_MultiplayerLanGamesList->EnableScrollbars(false, true);
	for (const NetDirectoryClient::GameRow& row: m_GameRows) {
		m_MultiplayerLanGamesList->AddItem(describe(row));
	}
	// The selection is a game, not a row number: it stays on that game when the list refreshes around it.
	for (size_t i = 0; i < m_GameRows.size() && !m_SelectedGameKey.empty(); ++i) {
		if (GameRowKey(m_GameRows[i]) == m_SelectedGameKey) {
			m_MultiplayerLanGamesList->SetSelectedIndex(static_cast<int>(i));
			break;
		}
	}
	// The Port field follows what is listed rather than the stock default nobody is hosting on.
	const auto listedPort = std::find_if(m_GameRows.begin(), m_GameRows.end(), [](const NetDirectoryClient::GameRow& row) {
		return row.joinable && row.port != 0;
	});
	if (listedPort != m_GameRows.end()) {
		SetJoinPortAuto(listedPort->port);
	}
}

void MainMenuGUI::SetJoinPortAuto(uint16_t port) {
	if (!m_MultiplayerJoinPortTextBox || port == 0 || m_JoinPortAutoValue.empty() || m_MultiplayerJoinPortTextBox->GetText() != m_JoinPortAutoValue) {
		return;
	}
	m_JoinPortAutoValue = std::to_string(port);
	m_MultiplayerJoinPortTextBox->SetText(m_JoinPortAutoValue);
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
	const auto config = ScenarioRunner::GetLockstepMatchConfig();
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
