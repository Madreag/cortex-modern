#pragma once

#include "Controller.h"
#include "NetLanDiscovery.h"
#include "NetMatchConfig.h"
#include "NetReconnectUx.h"

#include "SaveLoadMenuGUI.h"
#include "SettingsGUI.h"
#include "ModManagerGUI.h"

#include <array>

namespace RTE {

	class AllegroScreen;
	class GUIInputWrapper;
	class GUIControlManager;
	class GUICollectionBox;
	class GUIButton;
	class GUILabel;
	class GUIControl;
	class GUITextBox;
	class GUIListBox;
	struct NetLobbySnapshot;

	/// Handling for the main menu screen composition and sub-menu interaction.
	class MainMenuGUI {

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
#pragma endregion

#pragma region Automation
		/// Activates a control by name, dispatching to the active screen handler exactly as a click does.
		/// @return Whether the control was found.
		bool AutomationActivateControl(const std::string& controlName);

		/// Sets a text box's text by control name.
		bool AutomationSetText(const std::string& controlName, const std::string& text);

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
#pragma endregion

	private:
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
			MultiplayerCreateButton,
			MultiplayerConnectButton,
			MultiplayerReadyButton,
			MultiplayerStartButton,
			MultiplayerLeaveButton,
			MultiplayerReconnectButton,
			MultiplayerCancelReconnectButton,
			MultiplayerHostBackButton,
			MultiplayerJoinBackButton,
			MultiplayerModerateButton,
			MultiplayerModerationBackButton,
			PlayTutorialButton,
			MetaGameContinueButton,
			QuitConfirmButton,
			QuitCancelButton,
			SceneEditorButton,
			AreaEditorButton,
			AssemblyEditorButton,
			GibEditorButton,
			ActorEditorButton,
			ButtonCount
		};

		/// Enumeration for the sub-screens within the multiplayer menu flow.
		enum class MultiplayerSubScreen {
			Landing,
			HostSetup,
			JoinSetup,
			Lobby,
			Moderation
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
		GUITextBox* m_MultiplayerNameTextBox;
		GUITextBox* m_MultiplayerHostPortTextBox;
		GUITextBox* m_MultiplayerHostPlayersTextBox;
		GUITextBox* m_MultiplayerHostInputDelayTextBox;
		GUIButton* m_MultiplayerHostModeButton;
		NetMatchMode m_MultiplayerHostMode;
		GUITextBox* m_MultiplayerJoinAddressTextBox;
		GUITextBox* m_MultiplayerJoinPortTextBox;
		GUIListBox* m_MultiplayerLanGamesList;
		NetLanDiscovery m_LanBrowser; //!< Collects LAN host beacons while the join screen is up.
		std::vector<NetLanHostInfo> m_LanHosts; //!< The listed hosts, aligned with the list rows.
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
		std::array<GUILabel*, 4> m_MultiplayerLobbyPlayerLabels;
		MultiplayerSubScreen m_MultiplayerSubScreen;
		GUICollectionBox* m_CreditsScrollPanel;
		std::array<GUICollectionBox*, MenuScreen::ScreenCount> m_MainMenuScreens;
		std::array<GUIButton*, MenuButton::ButtonCount> m_MainMenuButtons;

#pragma region Create Breakdown
		/// Creates all the elements that compose the main menu screen.
		void CreateMainScreen();

		/// Creates all the elements that compose the MetaGame notice menu screen.
		void CreateMetaGameNoticeScreen();

		/// Creates all the elements that compose the multiplayer menu screen.
		void CreateMultiplayerScreen();

		/// Runs the LAN browser while the join screen is up and mirrors fresh hosts into the list.
		void RefreshLanGamesList();

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
		/// Rebuilds §9b's moderation panel from the host's live seat view.
		void RefreshModerationControls(const NetLobbySnapshot& snapshot);
		/// The one path a moderation action takes, whether a player clicked it or a gate drove it.
		void ActivateModerationRow(size_t row, NetModerationAction action);

		/// §11: shows the recovery banner and the rejoin/cancel controls the reconnect state machine says apply.
		void RefreshReconnectControls();

		/// Real-time clock for the reconnect schedule; the menu runs outside the sim.
		static uint64_t MenuClockMs();

		/// Starts hosting or joining a multiplayer match from the setup screen fields.
		void StartMultiplayer(bool host);

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
