#pragma once

#include "Controller.h"
#include "NetDirectoryClient.h"
#include "NetLanDiscovery.h"
#include "NetMatchConfig.h"
#include "NetMatchService.h"
#include "NetReconnectUx.h"

#include "SaveLoadMenuGUI.h"
#include "SettingsGUI.h"
#include "SettingsMan.h"
#include "ModManagerGUI.h"

#include <array>
#include <deque>
#include <optional>

namespace RTE {

	class AllegroScreen;
	class GUIInputWrapper;
	class GUIControlManager;
	class GUICollectionBox;
	class GUIButton;
	class GUIComboBox;
	class GUILabel;
	class GUIControl;
	class GUIFont;
	class GUITextBox;
	class GUIListBox;
	class GUICheckbox;
	struct NetLobbySnapshot;

	/// Handling for the main menu screen composition and sub-menu interaction.
	class MainMenuGUI {
		friend class GameActivity;

	public:
		/// Enumeration for the results of the MainMenuGUI input and event update.
		enum class MainMenuUpdateResult {
			NoEvent,
			MetaGameStarted,
			ScenarioStarted,
			EnterCreditsScreen,
			BackToMainFromCredits,
			ActivityStarted,
			ActivityResumed,
			Quit
		};

#pragma region Creation
		/// Constructor method used to instantiate a MainMenuGUI object in system memory and makes it ready for use.
		/// @param guiScreen Pointer to a GUIScreen interface that will be used by this MainMenuGUI's GUIControlManager. Ownership is NOT transferred!
		/// @param guiInput Pointer to a GUIInput interface that will be used by this MainMenuGUI's GUIControlManager. Ownership is NOT transferred!
		MainMenuGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput) {
			Clear();
			Create(guiScreen, guiInput);
		}

		/// Makes the MainMenuGUI object ready for use.
		/// @param guiScreen Pointer to a GUIScreen interface that will be used by this MainMenuGUI's GUIControlManager. Ownership is NOT transferred!
		/// @param guiInput Pointer to a GUIInput interface that will be used by this MainMenuGUI's GUIControlManager. Ownership is NOT transferred!
		void Create(AllegroScreen* guiScreen, GUIInputWrapper* guiInput);
#pragma endregion

#pragma region Concrete Methods
		/// Updates the MainMenuGUI state.
		/// @return The result of the MainMenuGUI input and event update. See MainMenuUpdateResult enumeration.
		MainMenuUpdateResult Update();

		/// Draws the MainMenuGUI to the screen.
		void Draw();
		/// Reopens the browser after playback releases its activity and input stream.
		void ReturnToReplayBrowser(const std::string& status);

		/// §11: reads the recovery record on the way into the main menu and, when one applies, opens the
		/// multiplayer screen's landing panel on the offer instead of leaving the player to find it.
		void OfferStoredRejoinOnEntry();

		/// Opens the multiplayer screen on the lobby panel when the match that just ended left a
		/// rematch lobby waiting, so the player comes out of the match where the rematch is.
		void OfferRematchLobbyOnEntry();

		/// Opens the multiplayer landing panel when a match fails, so its reason is the first thing the player reads.
		void OfferFailedMatchLandingOnEntry();
#pragma endregion

#pragma region Automation
		/// Gets the control manager for the screen currently drawn.
		GUIControlManager* AutomationManager() const;
		/// The list and row a scripted row name stands for: GameRow<n>, GameRowPort<port> (the joinable game on that port) or LabelReplayRow<n>.
		bool AutomationRowOf(const std::string& name, std::string& listName, int& row, NetDirectoryClient::GameRow* game = nullptr) const;
		/// The games the Join screen lists, in list order: a row shows words, the join target is here.
		const std::vector<NetDirectoryClient::GameRow>& AutomationGameRows() const { return m_GameRows; }
		static std::string DiscoveredGameRowText(const NetDirectoryClient::GameRow& row, GUIFont* font, int width);
		static std::string GameRowJoinRefusal(const NetDirectoryClient::GameRow& row);

		/// What the multiplayer screens hold for the next lobby and the host's draft, for a readback that a change reached them.
		std::string AutomationModelText() const;

		/// The dialog that takes every click while it is open, or null.
		GUIControl* AutomationModalDialog() const;

		/// Gets the port the next hosted lobby listens on, as the host's setup holds it. False when there is no setup.
		bool AutomationHostPort(std::string& port) const;

		/// Gets a named control's text (label, button or checkbox) for assert_label; false when it has none.
		bool AutomationLabelText(const std::string& controlName, std::string& text) const;

		/// Gets the name of the active menu screen.
		std::string AutomationActiveScreenName() const;

		/// Gets the multiplayer status and error label text.
		std::string AutomationMultiplayerStatus() const;
		std::string AutomationMultiplayerError() const;

		/// Gets the name of the active multiplayer sub-screen (Landing/HostSetup/JoinSetup/Lobby).
		std::string AutomationMultiplayerSubScreen() const;
		/// Takes §9b's moderation action on a seat exactly as the panel's button does: the same model,
		/// the same chosen applicant, the same service call. -1 means the first seat on the panel.
		/// @return Whether a row was found and the action ran.
		bool AutomationModerate(const std::string& action, int stableSeat);

		/// Gets whether a named button is currently enabled.
		bool AutomationControlEnabled(const std::string& controlName) const;
		/// Whether the skin defines the control at all, whatever screen is up. "Enabled" cannot answer
		/// this: a control that is merely on a hidden panel reads the same as one that does not exist.
		bool AutomationControlExists(const std::string& controlName) const;
		/// Leaves whatever sub-screen is up for the main screen, without the back button's side effects.
		void AutomationGoToMainScreen();
#pragma endregion

	private:
		friend bool TestJoiningProgress(std::string* error);
		friend bool TestLobbyChatReturn(std::string* error);
		static bool JoiningNeedsProgress(const NetLobbySnapshot& snapshot);
		std::unique_ptr<GUIInputWrapper> m_AutomationInput;
		/// Enumeration for the different sub-menu screens of the main menu.
		enum MenuScreen {
			MainScreen,
			MetaGameNoticeScreen,
			MultiplayerScreen,
			SaveOrLoadGameScreen,
			SettingsScreen,
			ModManagerScreen,
			EditorScreen,
			CreditsScreen,
			QuitScreen,
			ScreenCount
		};

		/// Enumeration for all the different buttons of the main menu and sub-menus.
		enum MenuButton {
			MetaGameButton,
			ScenarioButton,
			MultiplayerButton,
			SaveOrLoadGameButton,
			SettingsButton,
			ModManagerButton,
			EditorsButton,
			CreditsButton,
			QuitButton,
			ResumeButton,
			BackToMainButton,
			MultiplayerHostGameButton,
			MultiplayerJoinGameButton,
			MultiplayerResumeGameButton,
			ResumeStartButton,
			ResumeBackButton,
			MultiplayerCreateButton,
			MultiplayerConnectButton,
			MultiplayerReadyButton,
			MultiplayerStartButton,
			MultiplayerLeaveButton,
			MultiplayerReconnectButton,
			MultiplayerCancelReconnectButton,
			MultiplayerWaitSlotButton,
			MultiplayerHostBackButton,
			MultiplayerJoinBackButton,
			MultiplayerModerateButton,
			MultiplayerModerationBackButton,
			SaveDiagnosticsButton,
			LastMatchDetailsButton,
			LastMatchCloseButton,
			MultiplayerReplaysButton,
			ReplayPlayButton,
			ReplayDeleteButton,
			ReplayBackButton,
			ReplayDeleteConfirmButton,
			ReplayDeleteCancelButton,
			PlayTutorialButton,
			MetaGameContinueButton,
			QuitConfirmButton,
			QuitCancelButton,
			SceneEditorButton,
			AreaEditorButton,
			AssemblyEditorButton,
			GibEditorButton,
			ActorEditorButton,
			MultiplayerLobbyOptionsButton,
			MultiplayerHostOptionsButton,
			HostOptionsBackButton,
			HostOptionsApplyButton,
			HostOptionsDefaultsButton,
			HostSeatDialogCloseButton,
			HostRepairNowButton,
			HostFilesSaveDiagButton,
			HostSessionEndButton,
			HostSessionBannedButton,
			HostBannedRemoveButton,
			HostBannedCloseButton,
			LobbyEditSetupButton,
			LobbyLeaveStayButton,
			LobbyLeaveConfirmButton,
			JoinByAddressButton,
			JoinAddressGoButton,
			JoinAddressCancelButton,
			HostOptionsRestoreButton,
			ButtonCount
		};

		/// Enumeration for the sub-screens within the multiplayer menu flow.
		enum class MultiplayerSubScreen {
			Landing,
			HostSetup,
			JoinSetup,
			ResumeSetup,
			Lobby,
			Moderation,
			ReplayBrowser,
			HostOptions
		};

		int m_RootBoxMaxWidth; //!< The maximum width the root CollectionBox that holds all this menu's GUI elements. This is to constrain this menu to the primary window's display (left-most) while in multi-display fullscreen, otherwise positioning can get stupid.

		std::unique_ptr<GUIControlManager> m_MainMenuScreenGUIControlManager; //!< The GUIControlManager which owns all the GUIControls of the MainMenuGUI main screen. Alternative to changing skins at runtime which is expensive, since the main screen now has a unique skin.
		std::unique_ptr<GUIControlManager> m_SubMenuScreenGUIControlManager; //!< The GUIControlManager which owns all the GUIControls of the MainMenuGUI sub-menus.
		GUIControlManager* m_ActiveGUIControlManager; //!< The GUIControlManager that is currently being updated and drawn to the screen.
		GUICollectionBox* m_ActiveDialogBox; // The currently active GUICollectionBox in any of the main or sub-menu screens that acts as a dialog box and requires drawing an overlay.

		MenuScreen m_ActiveMenuScreen; //!< The currently active menu screen that is being updated and drawn to the screen. See MenuScreen enumeration.
		MainMenuUpdateResult m_UpdateResult; //!< The result of the MainMenuGUI update. See MainMenuUpdateResult enumeration.
		bool m_MenuScreenChange; //!< Whether the active menu screen was changed and a different one needs to be shown.
		bool m_MetaGameNoticeShown; //!< Whether the MetaGame notice and tutorial offer have been shown to the player.

		Timer m_ResumeButtonBlinkTimer; //!< Activity resume button blink timer.
		Timer m_CreditsScrollTimer; //!< Credits scrolling timer.

		std::unique_ptr<SaveLoadMenuGUI> m_SaveLoadMenu; //!< The save/load menu screen.
		std::unique_ptr<SettingsGUI> m_SettingsMenu; //!< The settings menu screen.
		std::unique_ptr<ModManagerGUI> m_ModManagerMenu; //!< The mod manager menu screen.

		// TODO: Rework this hacky garbage implementation when setting button font at runtime without loading a different skin is fixed. Would eliminate the need for a second GUIControlManager as well.
		// Right now the way this works is the font graphic has different character visuals for uppercase and lowercase and the visual change happens by applying the appropriate case string when hovering/unhovering.
		std::array<std::string, MenuButton::ResumeButton + 1> m_MainScreenButtonHoveredText; //!< Array containing uppercase strings of the main screen buttons text that are used to display the larger font when a button is hovered over.
		std::array<std::string, MenuButton::ResumeButton + 1> m_MainScreenButtonUnhoveredText; //!< Array containing lowercase strings of the main menu screen buttons text that are used to display the smaller font when a button is not hovered over.
		GUIButton* m_MainScreenHoveredButton; //!< The currently hovered main screen button.
		int m_MainScreenPrevHoveredButtonIndex; //!< The index of the previously hovered main screen button in the main menu button array.

		/// GUI elements that compose the main menu screen.
		GUILabel* m_VersionLabel;
		GUILabel* m_CreditsTextLabel;
		GUILabel* m_MultiplayerStatusLabel;
		GUILabel* m_MultiplayerErrorLabel;
		GUILabel* m_MultiplayerLandingStatusLabel;
		GUILabel* m_MultiplayerLobbyMatchLabel;
		GUILabel* m_MultiplayerLobbyMatchModeLabel = nullptr; //!< The header's second row: scene and friendly mode.
		GUILabel* m_LastMatchSummaryLabel;
		GUILabel* m_LastMatchDetailsLabel;
		GUICollectionBox* m_LastMatchDialog;
		GUICollectionBox* m_ReplayBrowserPanel;
		GUICollectionBox* m_ReplayDeleteDialog;
		GUIListBox* m_ReplayList;
		GUILabel* m_ReplaySelectedLabel;
		GUILabel* m_ReplayStatusLabel;
		GUILabel* m_ReplayDeleteLabel;
		struct ReplayRow {
			std::string path;
			std::string text;
			std::string details; //!< The selected recording, whole, and what Play does.
			std::string error;
		};
		std::vector<ReplayRow> m_ReplayRows;
		std::string m_ReplayDeletePath;
		GUICollectionBox* m_MultiplayerResumePanel = nullptr;
		GUIListBox* m_ResumeMatchesList = nullptr;
		GUILabel* m_ResumeSelectedLabel = nullptr;
		GUILabel* m_ResumeStatusLabel = nullptr;
		/// One resumable match as the screen shows it: the newest checkpoint of that match id.
		struct ResumeRow {
			std::string matchId;
			uint64_t tick = 0;
			std::string text;    //!< The list line: activity, site, how far in, how long ago.
			std::string details; //!< The selection's body: the peers the configuration names.
		};
		std::vector<ResumeRow> m_ResumeRows;
		GUITextBox* m_MultiplayerNameTextBox;
		GUITextBox* m_MultiplayerHostPortTextBox;
		GUIComboBox* m_MultiplayerHostPlayersCombo = nullptr; //!< The human players the mode accepts, the host included.
		GUILabel* m_MultiplayerHostAboutLabel = nullptr;     //!< The picked activity's description, one line.
		uint8_t m_MultiplayerHostPeerCount = 2;              //!< The human players the next lobby seats.
		GUITextBox* m_MultiplayerHostInputDelayTextBox;
		GUILabel* m_MultiplayerHostInputDelayPolicyLabel; //!< Names the saved delay policy beside the box, the same parenthetical the lobby row carries.
		GUICheckbox* m_MultiplayerHostPortMapCheckbox;
		GUIComboBox* m_MultiplayerHostModeCombo = nullptr; //!< PvP / Co-op PvE / PvPvE for request.mode.
		GUIComboBox* m_MultiplayerHostActivityCombo = nullptr; //!< The host's pick-list of lockstep-runnable activities.
		GUIComboBox* m_MultiplayerHostSceneCombo = nullptr; //!< Compatible scenes for the picked activity.
		GUILabel* m_MultiplayerHostInfoLabel;
		// (preset, defining module) for each GameActivity a lockstep match can run; the module is
		// carried so a same-named preset in another module cannot swap in silently.
		std::vector<std::pair<std::string, std::string>> m_MultiplayerHostActivities;
		size_t m_MultiplayerHostActivityIndex = 0;
		std::vector<std::pair<std::string, std::string>> m_MultiplayerHostScenes;
		size_t m_MultiplayerHostSceneIndex = 0;
		std::string m_MultiplayerHostPickNotice; //!< Vanished-pick line; empty when the current row still exists.
		NetMatchMode m_MultiplayerHostMode;
		GUITextBox* m_MultiplayerJoinAddressTextBox;
		GUITextBox* m_MultiplayerJoinPortTextBox;
		std::string m_JoinPortAutoValue; //!< The last port this screen filled in by itself; empty once the player picks a row or types a port.
		GUIListBox* m_MultiplayerLanGamesList;
		GUILabel* m_MultiplayerLanGamesLabel; //!< The line above the list; doubles as the join refusal status.
		std::string m_LanGamesLabelText;      //!< Its ini text, restored when a refusal clears.
		NetLanDiscovery m_LanBrowser; //!< Collects LAN host beacons while the join screen is up.
		NetDirectoryClient m_DirectoryBrowser; //!< A browse-only instance: GETs the session list on its poll interval.
		std::vector<NetDirectoryClient::GameRow> m_GameRows; //!< The merged LAN+NET rows, aligned with the list.
		std::optional<NetDirectoryLocalIdentity> m_DirectoryIdentity; //!< Ordinary 4/22 identity NET rows are judged against.
		std::optional<NetIdentityManifest> m_DirectoryManifest; //!< Its manifest: the game data a refusal for modules prints.
		std::optional<NetDirectoryLocalIdentity> m_DirectoryWorldIdentity; //!< World 5/23 identity for persistent_world rows.
		bool m_DirectoryIdentityTried = false;
		bool m_JoinTargetPersistentWorld = false;
		std::string m_JoinTargetActivity;
		std::string m_LastWorldJoinAddress;
		uint16_t m_LastWorldJoinPort = 0;
		uint64_t m_LanBrowserNowMs;
		GUICollectionBox* m_MultiplayerLandingPanel;
		GUICollectionBox* m_MultiplayerHostPanel;
		GUICollectionBox* m_MultiplayerJoinPanel;
		GUICollectionBox* m_MultiplayerLobbyPanel;
		GUICollectionBox* m_MultiplayerModerationPanel;
		GUILabel* m_MultiplayerModerationSummaryLabel;
		GUILabel* m_MultiplayerModerationStatusLabel;
		std::array<GUILabel*, 3> m_ModerationSeatLabels;
		std::array<GUIButton*, 3> m_ModerationApplicantButtons;
		std::array<GUIButton*, 3> m_ModerationWaitButtons;
		std::array<GUIButton*, 3> m_ModerationSubstituteButtons;
		std::array<GUIButton*, 3> m_ModerationCancelButtons;
		NetModerationUx m_ModerationUx; //!< §9b's panel model; the buttons and the headless driver share it.
		std::map<const GUIControl*, NetModerationUx::Row> m_PressedModeration;
		std::map<std::string, bool> m_ActivityNameShared; //!< Per activity name, whether two loaded activities share it.
		std::array<GUILabel*, NetMatchConfigUtil::c_MaxPlayers> m_MultiplayerLobbyPlayerLabels;
		GUIFont* m_MultiplayerLobbyPlayerRowFont = nullptr; //!< The font the player rows draw in, so the row text is measured against what draws it.
		GUIFont* m_MultiplayerLobbyPlayerRowFallbackFont = nullptr; //!< Supplies the row bytes the primary font's atlas has no ink for.
		GUILabel* m_MultiplayerLobbyPlayersHeader = nullptr; //!< The "Players" column header; it moves with the rows when the panel widens.
		GUILabel* m_MultiplayerLobbyPortMapLabel;
		// The lobby's chat is built in code so the panel can grow for it without touching the skin file.
		std::array<GUILabel*, 8> m_MultiplayerLobbyChatLabels;
		GUITextBox* m_MultiplayerLobbyChatInput;
		uint64_t m_LobbyChatRequestId = 0;
		GUILabel* m_MultiplayerLobbyVersionLabel; //!< The build's version line under the chat entry, as the main menu shows it.
		std::deque<std::string> m_MultiplayerLobbyChatLines; //!< Newest at the back; the labels show the last eight.
		MultiplayerSubScreen m_MultiplayerSubScreen;
		// §9.2/9.3's host options panel: six pages over the lobby, or the host-setup draft of the next one.
		GUICollectionBox* m_HostOptionsPanel = nullptr;
		GUILabel* m_HostOptionsTitle = nullptr;
		static constexpr int c_HostOptionsPageCount = 7;
		static constexpr int c_HostOptionsPlayersPage = 0;
		static constexpr int c_HostOptionsRulesPage = 1;
		static constexpr int c_HostOptionsConnectionPage = 2;
		static constexpr int c_HostOptionsTimingPage = 3;
		static constexpr int c_HostOptionsRecoveryPage = 4;
		static constexpr int c_HostOptionsFilesPage = 5;
		static constexpr int c_HostOptionsSessionPage = 6;
		/// This computer's own hosting choices as Advanced edits them: saved when Apply commits the page, kept as they were on Cancel.
		struct HostComputerDraft {
			SettingsMan::NetworkHostVisibility listing = SettingsMan::NetworkHostVisibility::Listed;
			bool portMap = true;
			std::string port = "41010";
			bool ice = true;
			SettingsMan::NetworkHostRelayMode relay = SettingsMan::NetworkHostRelayMode::Directory;
			std::array<std::string, 3> relayFields;
			int joinHistorySeconds = 360;
			int joinLagSeconds = 120;
			SettingsMan::NetworkMatchStatusMode statusWidget = SettingsMan::NetworkMatchStatusMode::Auto;
			bool operator==(const HostComputerDraft&) const = default;
		};
		HostComputerDraft m_HostComputerDraft;
		HostComputerDraft m_HostComputerLoaded; //!< The hosting choices as Advanced opened on them.
		NetMatchConfig m_HostOptionsOpenedDraft; //!< The match draft as Advanced opened on it, for an unstaged setup.
		GUILabel* m_HostOptScopeLabel = nullptr;       //!< Under the selector: what the shown page edits.
		GUILabel* m_HostRulesDefaultsLabel = nullptr;  //!< The Rules page's defaults, in one line.
		GUILabel* m_HostNetVisibilityHint = nullptr;   //!< What the picked game listing means for a friend.
		std::array<GUITab*, c_HostOptionsPageCount> m_HostOptionsTabs{};
		std::array<GUICollectionBox*, c_HostOptionsPageCount> m_HostOptionsPages{};
		int m_HostOptionsPage = 0;
		GUILabel* m_HostOptionsStatusLabel = nullptr;
		GUILabel* m_HostNetPortHint = nullptr;     //!< Beside the port: the default, or the router's answer for a hosted lobby.
		GUILabel* m_HostRecRejoinLabel = nullptr;  //!< Whether this build can prove who a returning player is.
		std::vector<std::string> m_MultiplayerControlsMissing; //!< Controls the multiplayer screens need that the menu file lacks.
		GUILabel* m_MultiplayerOffLabel = nullptr; //!< Under the main menu: why multiplayer is off.
		GUILabel* m_PageChatNotice = nullptr; //!< The newest lobby chat line, drawn above a host page that hides the lobby's chat band.
		/// Moves the chat the service received into the lobby's lines, whichever sub-screen is showing.
		void TakeLobbyChat(const NetLobbySnapshot& snapshot);
		GUIComboBox* m_HostSeatPlayersCombo = nullptr;
		GUILabel* m_HostSeatCapacityHint = nullptr;
		static constexpr int c_HostSeatRows = 7; //!< c_MaxPlayers: four human seats plus three CPUs.
		std::array<GUILabel*, c_HostSeatRows> m_HostSeatNameLabels{};
		std::array<GUIComboBox*, c_HostSeatRows> m_HostSeatTypeCombos{}; //!< H03: Open / Closed / CPU per seat.
		std::array<GUIComboBox*, c_HostSeatRows> m_HostSeatTeamCombos{};
		std::array<GUILabel*, c_HostSeatRows> m_HostSeatDelayLabels{};
		std::array<GUILabel*, c_HostSeatRows> m_HostSeatStateLabels{};
		std::array<GUIButton*, c_HostSeatRows> m_HostSeatDetailsButtons{};
		GUIComboBox* m_HostRulesActivityCombo = nullptr;
		GUIComboBox* m_HostRulesSceneCombo = nullptr;
		GUIComboBox* m_HostRulesModeCombo = nullptr;
		GUISlider* m_HostRulesDifficultySlider = nullptr;
		GUILabel* m_HostRulesDifficultyValue = nullptr;
		GUISlider* m_HostRulesGoldSlider = nullptr;
		GUILabel* m_HostRulesGoldValue = nullptr;
		GUICheckbox* m_HostRulesFogCheck = nullptr;
		GUICheckbox* m_HostRulesClearPathCheck = nullptr;
		GUICheckbox* m_HostRulesDeployCheck = nullptr;
		GUIComboBox* m_HostRulesBrainlessCombo = nullptr; //!< L33's "When every human brain is lost" row.
		GUIComboBox* m_HostRulesTeamCombo = nullptr;
		GUIComboBox* m_HostRulesTechCombo = nullptr;
		GUISlider* m_HostRulesSkillSlider = nullptr;
		GUILabel* m_HostRulesSkillValue = nullptr;
		GUIComboBox* m_HostNetPolicyCombo = nullptr;
		GUIComboBox* m_HostNetSlowPolicyCombo = nullptr;
		GUITextBox* m_HostNetSlowBoundBox = nullptr;
		GUILabel* m_HostNetSlowPolicyHintLabel = nullptr; //!< What the bound and the policy do to a late player, host included.
		GUIComboBox* m_HostNetRedundancyCombo = nullptr;
		GUITextBox* m_HostNetMinDelayBox = nullptr;
		GUILabel* m_HostNetEffectiveLabel = nullptr;
		std::array<GUILabel*, 4> m_HostNetPeerLabels{};
		std::array<GUITextBox*, 4> m_HostNetPeerDelayBoxes{};
		GUIButton* m_HostNetRecalcButton = nullptr;
		GUILabel* m_HostNetPingLabel = nullptr;
		GUILabel* m_HostNetModeLabel = nullptr;          //!< H34: "Host mode: <Dedicated|Playing> - capacity N - humans seated M".
		GUIComboBox* m_HostNetVisibilityCombo = nullptr; //!< H34: LAN only / Internet: Unlisted / Internet: Listed.
		GUIComboBox* m_HostNetIceCombo = nullptr;
		GUILabel* m_HostNetIceHintLabel = nullptr;
		GUIComboBox* m_HostRelayCombo = nullptr;
		std::array<GUITextBox*, 3> m_HostRelayBoxes{};
		std::array<GUILabel*, 3> m_HostRelayLabels{};
		GUILabel* m_HostRelayHint = nullptr;
		GUITextBox* m_HostNetPortBox = nullptr;          //!< H34: the next hosted session's game port (the setup draft's).
		GUICheckbox* m_HostRecRepairCheck = nullptr;
		GUICheckbox* m_HostRecAutosaveCheck = nullptr;
		GUITextBox* m_HostRecAutosaveIntervalBox = nullptr;
		GUILabel* m_HostRecLastSaveLabel = nullptr;
		GUIComboBox* m_HostRecReturnWindowCombo = nullptr; //!< How long a held seat may come back in place.
		GUIComboBox* m_HostRecJoinHistoryCombo = nullptr; //!< How long this host keeps a world's round history for joiners and returns.
		GUIComboBox* m_HostRecJoinLagCombo = nullptr;     //!< How far a watcher or returning seat may trail before it starts over.
		GUILabel* m_HostRecOptionHintLabel = nullptr;     //!< The consequence of the window or history row the player points at or has focused.
		std::array<GUIControl*, 6> m_HostRecHintRowControls{}; //!< Each of those rows' label and combo, in row order.
		int m_HostRecHintRow = 0;                         //!< The row the hint names: the last one pointed at or focused; the Return window first.
		/// Logs the history policy this host would run under the drafted return window and its own history options.
		void LogHostHistoryPolicy() const;
		GUILabel* m_HostRecWaitingLabel = nullptr;
		GUILabel* m_HostRecRepairHintLabel = nullptr;    //!< H25: repair row's own status - hint, confirm line, or live progress.
		bool m_HostRecRepairArmed = false;             //!< H25: first press arms; the second calls ResyncMatch.
		std::string m_HostRecRepairRefusal;
		GUILabel* m_HostFilesSavePathLabel = nullptr;
		GUILabel* m_HostFilesDiagPathLabel = nullptr;
		GUILabel* m_HostFilesDiagResultLabel = nullptr;
		GUIComboBox* m_HostFilesWidgetCombo = nullptr;
		GUILabel* m_HostSessHostingLabel = nullptr;
		GUILabel* m_HostSessSeatsLabel = nullptr;
		GUIComboBox* m_HostSessIdleCombo = nullptr;
		GUILabel* m_HostSessIdleStateLabel = nullptr; //!< H31: the idle window and why it is running.
		GUILabel* m_HostSessBannedLabel = nullptr;    //!< H11: the host's ban count beside its button.
		GUICollectionBox* m_HostSeatDialog = nullptr;
		GUILabel* m_HostSeatDlgName = nullptr;
		GUILabel* m_HostSeatDlgSeat = nullptr;
		GUILabel* m_HostSeatDlgTeam = nullptr;
		GUILabel* m_HostSeatDlgState = nullptr;
		GUILabel* m_HostSeatDlgReclaim = nullptr;    //!< H08: hold/reclaim seconds from the seat snapshot.
		GUILabel* m_HostSeatDlgApplicants = nullptr;
		GUIListBox* m_HostSeatDlgApplicantList = nullptr; //!< The people asking for the seat; a pick chooses whom Approve seats.
		GUIButton* m_HostSeatDlgWait = nullptr;      //!< H05.
		GUIButton* m_HostSeatDlgApprove = nullptr;   //!< H06.
		GUIButton* m_HostSeatDlgCancel = nullptr;    //!< H07.
		GUIButton* m_HostSeatDlgKick = nullptr;      //!< H09: RemoveParticipant(Kick).
		GUIButton* m_HostSeatDlgBan = nullptr;       //!< H10: RemoveParticipant(BanSession).
		GUILabel* m_HostSeatDlgActionHint = nullptr;
		GUILabel* m_HostSeatDlgStatus = nullptr;
		GUICollectionBox* m_HostBannedDialog = nullptr; //!< H11's session ban list.
		GUIComboBox* m_HostBannedPick = nullptr;          //!< Which ban row Remove acts on.
		GUILabel* m_HostBannedListLabel = nullptr;
		GUILabel* m_HostBannedStatusLabel = nullptr;
		std::vector<NetHostBanRecord> m_HostBannedRecords; //!< The store's rows, indexed like the pick combo.
		NetMatchConfig m_HostOptionsDraft;              //!< The complete config the panel edits.
		NetMatchConfig m_HostOptionsShownDraft;         //!< The draft as the controls last showed it: only then does a change event read them back.
		uint64_t m_HostOptionsBaseRevision = 0;         //!< The adopted revision the draft was seeded from.
		bool m_HostOptionsSetupDraft = false;           //!< True while the draft feeds a new lobby's request.
		bool m_HostOptionsReadOnly = false;             //!< A client reads the adopted config; it cannot edit it.
		GUICollectionBox* m_JoinAddressDialog = nullptr; //!< Join by address: the address and port boxes.
		GUILabel* m_JoinSelectedLabel = nullptr;         //!< The selected game's details, or why it cannot be joined.
		bool m_JoinAttemptActive = false;                //!< A join started from this screen and not yet in its lobby.
		std::string m_JoinStatusText;                    //!< The join's outcome on the join screen; empty when there is none.
		std::string m_JoinTargetName;                    //!< The host a join on its way is for.
		int m_ListEventMsg = -1; //!< The notification a list's event carried, while its handler runs.
		std::string m_SelectedGameKey;                   //!< The selected game's identity, kept across list refreshes.
		GUICollectionBox* m_LobbyLeaveDialog = nullptr; //!< Asks before Escape or Back closes an open lobby.
		GUILabel* m_LobbyLeaveLabel = nullptr;
		std::optional<NetMatchConfig> m_HostSetupOptions; //!< The setup draft Apply accepted; the next request carries it.
		unsigned m_HostRulesTouched = 0;        //!< The activity-seeded rules the host set himself in this draft (NetActivitySetup::SeededRule bits).
		unsigned m_HostAppliedRulesTouched = 0; //!< The same for the last draft Apply accepted, which the next open continues from.
		uint64_t m_HostOptionsAwaitedRevision = 0;        //!< The revision a live Apply waits on the adopted config to reach.
		uint64_t m_HostLastSaveScanMs = 0;                //!< H28's throttle: when the autosave directory was last re-read.
		std::string m_HostLastSaveText;                   //!< Its latest .ccsave observation, or empty for none.
		int m_HostOptionsSeatRow = -1;                  //!< The seat row the details dialog describes.
		int m_HostSeatDlgModerationRow = -1;            //!< The dialog's row in m_ModerationUx, or -1 when the seat has none.
		std::optional<NetH4ModerationSeat> m_HostSeatDlgRemovalSeat; //!< The seat's admission row a Kick/Ban selection rides, when published.
		bool m_HostKickBanWatch = false;                //!< A Queued removal's applied result lands in GetLastKickBanResult.
		std::string m_HostKickBanVerb;                  //!< "Kick"/"Ban" - the action the watch is reporting.
		std::vector<NetHostActivityChoice> m_HostOptionsActivities; //!< The activities the Rules page offers: the host screen's census.
		std::vector<NetHostSceneChoice> m_HostOptionsScenes;        //!< The scenes the Rules page offers: those the drafted activity runs.
		std::vector<std::string> m_HostOptionsTechModules; //!< Tech combo's resolved module names (-All-/-Random- first).
		std::string m_ReconnectStatusShown; //!< The last §11 line this screen wrote, so it may clear its own.
		NetMatchServiceRequest m_MultiplayerJoinRequest; //!< The join the player last asked for, so an application reuses it.
		bool m_MultiplayerApplyOffered = false;          //!< A join of this host may still be answered by applying (§9b).
		GUICollectionBox* m_CreditsScrollPanel;
		std::array<GUICollectionBox*, MenuScreen::ScreenCount> m_MainMenuScreens;
		std::array<GUIButton*, MenuButton::ButtonCount> m_MainMenuButtons;

#pragma region Create Breakdown
		/// Creates all the elements that compose the main menu screen.
		void CreateMainScreen();

		/// Creates all the elements that compose the MetaGame notice menu screen.
		/// Looks up a control the multiplayer screens use, noting it when the menu file has none of that name.
		GUIControl* MultiplayerControl(const std::string& name);

		/// Leaves multiplayer off for a menu file without its controls, and says so under the main menu.
		void TurnMultiplayerOff();

		void CreateMetaGameNoticeScreen();

		/// Creates all the elements that compose the multiplayer menu screen.
		void CreateMultiplayerScreen();

		/// Runs the LAN browser and the directory lister while the join screen is up and mirrors the
		/// merged rows into the list; a non-joinable row stays visible with its refusal reason.
		void RefreshGamesList();
		/// Fills the join Port field with a port the screen knows, until the player picks a row or types a port.
		void SetJoinPortAuto(uint16_t port);
		/// Keeps a closed picker's line inside its box: the module suffix goes first, then the tail elides.
		void FitClosedComboText(GUIComboBox* combo);

		/// Creates all the elements that compose the editor selection menu screen.
		void CreateEditorsScreen();

		/// Creates all the elements that compose the credits menu screen.
		void CreateCreditsScreen();

		/// Creates all the elements that compose the quit confirmation menu screen.
		void CreateQuitScreen();
#pragma endregion

#pragma region Menu Screen Handling
		/// Hides all main menu screens.
		void HideAllScreens();

		/// Sets the MainMenuGUI to display a menu screen.
		/// @param screenToShow Which menu screen to display. See MenuScreen enumeration.
		/// @param playButtonPressSound Whether to play a sound if the menu screen change is triggered by a button press.
		void SetActiveMenuScreen(MenuScreen screenToShow, bool playButtonPressSound = true);

		/// Makes the main menu screen visible to be interacted with by the player.
		void ShowMainScreen();

		/// Makes the MetaGame notice menu screen visible to be interacted with by the player.
		void ShowMetaGameNoticeScreen();

		/// Makes the multiplayer menu screen visible to be interacted with by the player.
		void ShowMultiplayerScreen();

		/// Makes the editor selection menu screen visible to be interacted with by the player.
		void ShowEditorsScreen();

		/// Makes the credits menu screen visible to be interacted with by the player and resets the scrolling timer for the credits.
		void ShowCreditsScreen();

		/// Makes the quit confirmation menu screen visible to be interacted with by the player if a game is in progress, or immediately sets the UpdateResult to Quit if not.
		void ShowQuitScreenOrQuit();

		/// Makes the resume game button visible to be interacted with by the player if a game is in progress and animates it (blinking).
		void ShowAndBlinkResumeButton();

		/// Progresses the credits scrolling.
		/// @return Whether the credits finished scrolling.
		bool RollCredits();
#pragma endregion

#pragma region Update Breakdown
		/// Handles returning to the main menu from one of the sub-menus if the player requested to return via the back button or the esc key. Also handles closing active dialog boxes with the esc key.
		/// @param backButtonPressed Whether the player requested to return to the main menu from one of the sub-menus via back button.
		void HandleBackNavigation(bool backButtonPressed);

		/// Asks whether to leave the open lobby, saying what leaving does for the others.
		void AskToLeaveLobby();

		/// Leaves the lobby or the match being joined, back to where the player chose it.
		void LeaveLobby();

		/// Joins the game selected in the list.
		void JoinSelectedGame();

		/// Keeps the player on the screen they set up with and says why the host or join did not start.
		void ShowSetupFailure(bool host, const std::string& text);

		/// Handles the player interaction with the MainMenuGUI GUI elements.
		/// @return Whether the player requested to return to the main menu from one of the sub-menus.
		bool HandleInputEvents();

		/// Handles the player interaction with the main screen GUI elements.
		/// @param guiEventControl Pointer to the GUI element that the player interacted with.
		void HandleMainScreenInputEvents(const GUIControl* guiEventControl);

		/// Handles the player interaction with the MetaGame notice screen GUI elements.
		/// @param guiEventControl Pointer to the GUI element that the player interacted with.
		void HandleMetaGameNoticeScreenInputEvents(const GUIControl* guiEventControl);

		/// Handles the player interaction with the multiplayer screen GUI elements.
		/// @param guiEventControl Pointer to the GUI element that the player interacted with.
		void HandleMultiplayerScreenInputEvents(const GUIControl* guiEventControl);

		/// Queues the saved audience's line (Ctrl+Enter: Team; Shift+Enter: All). Clears the draft
		/// only after the local transport accepts it.
		void SendLobbyChat();

		/// Handles the player interaction with the editor selection screen GUI elements.
		/// @param guiEventControl Pointer to the GUI element that the player interacted with.
		void HandleEditorsScreenInputEvents(const GUIControl* guiEventControl);

		/// Handles the player interaction with the quit screen GUI elements.
		/// @param guiEventControl Pointer to the GUI element that the player interacted with.
		void HandleQuitScreenInputEvents(const GUIControl* guiEventControl);

		/// Updates the currently hovered main screen button text to give the hovered visual and updates the previously hovered button to remove the hovered visual.
		/// @param hoveredButton Pointer to the currently hovered main screen button, if any. Acquired by GUIControlManager::GetControlUnderPoint.
		void UpdateMainScreenHoveredButton(const GUIButton* hoveredButton);

		/// Drives the multiplayer screen sub-screen state and refreshes its controls from the service snapshot.
		void UpdateMultiplayerScreen();

		/// Refreshes the multiplayer sub-panels, labels, and button states from the lobby snapshot.
		void RefreshMultiplayerScreenControls(const NetLobbySnapshot& snapshot);
		/// Opens the finished round's summary using the menu's modal overlay.
		void ShowLastMatchDetails();
		/// Restricts input to a multiplayer dialog until it closes.
		void OpenMultiplayerDialog(GUICollectionBox* dialog, const GUICollectionBox* owner);
		void CloseMultiplayerDialog();
		/// Enumerates replay headers and preserves the selected filename across refreshes.
		void RefreshReplayList();
		void RefreshReplayBrowserControls();
		/// Lists the matches this install can restart, newest first, and what each one stands on.
		void RefreshResumeList();
		/// The resume screen's selection, its details line and whether Resume can be pressed.
		void RefreshResumeControls();
		/// Starts the service on the selected checkpoint.
		void StartSelectedResume();
		void PlaySelectedReplay();
		void ConfirmReplayDelete();
		/// Rebuilds §9b's moderation panel from the host's live seat view.
		void RefreshModerationControls(const NetLobbySnapshot& snapshot);
		/// The one path a moderation action takes, whether a player clicked it or a gate drove it.
		void ActivateModerationRow(const GUIControl* control, NetModerationAction action);

		/// §11: shows the recovery banner and the rejoin/cancel controls the reconnect state machine says apply.
		void RefreshReconnectControls();

		/// The delay box and its policy label re-read the saved policy: fixed pre-fills the saved frames, automatic greys the box out.
		void RefreshHostInputDelayControls();

		/// Resizes a multiplayer sub-panel's width: the diagnostic label keeps its 12px side margins and every other child keeps its center offset.
		void FitMultiplayerPanelWidth(GUICollectionBox* panel, GUILabel* diagnosticLabel, int width, const std::vector<GUILabel*>& fillLabels = {});
		/// Resizes the MultiplayerScreen and keeps it centred, clamping to the top edge when the height no longer fits the viewport.
		void FitMultiplayerScreen(int width, int height);
		/// Keeps the back and diagnostics actions together below every multiplayer panel.
		void LayoutMultiplayerFooter(int width, int y);

		/// Real-time clock for the reconnect schedule; the menu runs outside the sim.
		static uint64_t MenuClockMs();

		/// Rebuilds the host activity picker's choices from the loaded presets, keeping the current pick.
		void RefreshMultiplayerHostActivities();
		/// Rebuilds the scene list for the picked activity, keeping the current scene when it is still compatible.
		void RefreshMultiplayerHostScenes();


		/// §9.2: fetches the options panel's controls and fills the fixed combo lists once.
		void CreateHostOptionsControls();
		/// Opens the panel seeded from the staged request (host setup) or the adopted config (lobby).
		void OpenHostOptions(bool setupDraft);
		/// Shows one of the six pages and checks its tab.
		void ShowHostOptionsPage(int page);
		/// Mirrors the draft into every visible control each frame; in a lobby it also re-reads the adopted state.
		void RefreshHostOptionsControls(const NetLobbySnapshot& snapshot);
		/// Reads every editable control back into the draft (Apply, and before roster re-derivation).
		void DraftHostOptionsFromControls();
		/// Fills the Rules page's activity list from the host screen's census.
		void RefreshHostOptionsActivities();
		/// Fills the Rules page's scene list with the scenes the drafted activity can run.
		/// @param resolve Whether a drafted scene the activity cannot run gives way to the activity's preferred one.
		void RefreshHostOptionsScenes(bool resolve);
		/// Keeps the activity-seeded rules on the activity's own defaults after a draft read: a new activity re-seeds
		/// them and a new difficulty re-seeds the gold, except a rule the host set himself.
		/// @param before The draft's rules before the read. @param guiEventControl The control the read answered.
		void FollowHostActivityDefaults(const NetMatchStandardRules& before, const GUIControl* guiEventControl);
		/// Rebuilds the draft's roster after a capacity/mode change, keeping the edited rules.
		void RederiveHostOptionsRoster();
		/// The request the host-setup fields would send today, so the setup draft seeds the same config.
		NetMatchServiceRequest HostRequestDraft() const;
		/// Offers the human player counts the chosen mode accepts and keeps the count inside them.
		void RefreshHostPlayersChoices();
		/// Carries the host screen's activity, scene, mode and players into the staged draft, so the rows, Advanced and the
		/// summary are one draft; a new activity re-seeds the rules the host did not set.
		void SyncHostSetupDraft();
		/// Shows a staged draft on the host screen's rows.
		void SyncHostScreenFromDraft(const NetMatchConfig& draft);
		/// The host's saved defaults become the draft the host screen opens on, when nothing is staged yet.
		void StageSavedHostDefaults();
		/// The host screen's summary: the scene, the people and AI teams and how they are arranged, and who can find the game.
		std::string HostSummaryText() const;
		/// Apply: the setup path stages the draft for the next request; the lobby path submits it.
		void ApplyHostOptions();
		/// Loads this computer's hosting choices into the draft Advanced edits.
		void LoadHostComputerDraft();
		/// Saves the edited hosting choices; false with the reason on the panel when one cannot be taken now.
		/// @param commit False checks every choice and saves none.
		bool CommitHostComputerDraft(bool commit = true);
		/// Stages the shown page's factory values; Apply commits them as any edit.
		void RestoreHostOptionsPageDefaults();
		/// Leaves Advanced for the screen it was opened from.
		void LeaveHostOptions();
		/// Writes the draft's host-owned fields to the persisted host defaults.
		void SaveHostOptionsDefaults();
		/// The seat details dialog's contents for one roster row.
		void ShowHostSeatDetails(int row);
		/// Re-fills the open seat dialog's live rows: hold seconds, applicants, action availability.
		void RefreshHostSeatDialog();
		void RefreshHostBannedDialog();
		/// H03: applies one row's Open/Closed/CPU pick to the draft roster, refusing the illegal ones.
		void ChangeHostSeatType(int row, int typeIndex);
		/// H11: opens the banned-players list dialog.
		void ShowHostBannedDialog();
		/// The panel's own event channel; only reached while the sub-screen is up.
		void HandleHostOptionsInputEvents(const GUIControl* guiEventControl);
		/// Sizes the activity and scene combos to their own longest row plus the list pad and scrollbar, clipped to the panel's right pad; the mode combo follows the activity's width.
		void FitHostActivityCombo();
		/// Writes the picked activity and scene onto the setup screen's own display.
		void ApplyMultiplayerHostActivity();
		/// Starts hosting or joining a multiplayer match from the setup screen fields.
		void StartMultiplayer(bool host);
		/// §9b: answers a live match's refusal by asking the host for a seat instead of a new one.
		void ApplyToSubstitute();
		/// Asks the host for the player's own held seat, which the host's refusal named.
		void ApplyForOwnSeat(uint16_t stableSeat);
		/// Whether two loaded activities share this name, so a header names the module to tell them apart.
		bool ActivityNameShared(const std::string& preset);
		/// Joins again and waits, knocking, for one of a world's held slots to open.
		void WaitForSlot();

		/// Launches the activity once the multiplayer runtime reaches lockstep ready.
		void MaybeLaunchMultiplayerActivity();
#pragma endregion

		/// Clears all the member variables of this MainMenuGUI, effectively resetting the members of this object.
		void Clear();

		// Disallow the use of some implicit methods.
		MainMenuGUI(const MainMenuGUI& reference) = delete;
		MainMenuGUI& operator=(const MainMenuGUI& rhs) = delete;
	};
} // namespace RTE
