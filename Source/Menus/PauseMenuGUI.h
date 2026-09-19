#pragma once

#include "Timer.h"

#include <array>
#include <memory>
#include <string>

struct BITMAP;

namespace RTE {

	class AllegroScreen;
	class GUIInputWrapper;
	class GUIControlManager;
	class GUICollectionBox;
	class GUIButton;
	class GUILabel;
	class SettingsGUI;
	class ModManagerGUI;
	class SaveLoadMenuGUI;

	/// Handling for the pause menu screen composition and interaction.
	class PauseMenuGUI {

	public:
		/// Enumeration for the results of the PauseMenuGUI input and event update.
		enum class PauseMenuUpdateResult {
			NoEvent,
			BackToMain,
			ActivityResumed,
			MatchLeft,
			/// The host ended the round; the game loop's own Complete path lands every peer in the rematch lobby.
			MatchEnded
		};

#pragma region Creation
		/// Constructor method used to instantiate a PauseMenuGUI object in system memory and make it ready for use.
		/// @param guiScreen Pointer to a GUIScreen interface that will be used by this PauseMenuGUI's GUIControlManager. Ownership is NOT transferred!
		/// @param guiInput Pointer to a GUIInput interface that will be used by this PauseMenuGUI's GUIControlManager. Ownership is NOT transferred!
		PauseMenuGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput) {
			Clear();
			Create(guiScreen, guiInput);
		}

		/// Makes the PauseMenuGUI object ready for use.
		/// @param guiScreen Pointer to a GUIScreen interface that will be used by this PauseMenuGUI's GUIControlManager. Ownership is NOT transferred!
		/// @param guiInput Pointer to a GUIInput interface that will be used by this PauseMenuGUI's GUIControlManager. Ownership is NOT transferred!
		void Create(AllegroScreen* guiScreen, GUIInputWrapper* guiInput);
#pragma endregion

#pragma region Setters
		/// Sets the "Back to Main Menu" button text to the menu we will be going back to.
		/// @param menuName The target menu name, e.g. "Conquest" will result in "Back to Conquest Menu".
		void SetBackButtonTargetName(const std::string& menuName);
#pragma endregion

#pragma region Concrete Methods
		/// Enables or disables buttons depending on the current Activity.
		void EnableOrDisablePauseMenuFeatures();

		/// Shows the match rows instead of the single-player ones, for a pause menu over a running network match.
		/// @param networkMatch Whether this menu is the local menu of a network match.
		void SetNetworkMatchMode(bool networkMatch);

		/// Asks for the same back navigation the escape key does, for the pad's start button.
		void RequestBack() { m_BackRequested = true; }

		/// Updates the PauseMenuGUI state.
		/// @return The result of the PauseMenuGUI input and event update. See PauseMenuUpdateResult enumeration.
		PauseMenuUpdateResult Update();

		/// Draws the PauseMenuGUI to the screen.
		/// @param drawPostProcessBuffer Whether to present the post-process buffer first; a live match frame already has.
		void Draw(bool drawPostProcessBuffer = true);

		/// Posts a command through the visible pause menu's event queue.
		bool AutomationPostCommand(const std::string& controlName);
		GUIControlManager* AutomationManager() const;
		std::string AutomationActiveScreenName() const;
		/// Checks whether the pause menu contains a control.
		bool AutomationControlExists(const std::string& controlName) const;
		/// Checks whether a pause-menu control is visible and enabled.
		bool AutomationControlEnabled(const std::string& controlName) const;
		/// Reads the text of a pause-menu button.
		bool AutomationLabelText(const std::string& controlName, std::string& text) const;
#pragma endregion

	private:
		std::unique_ptr<GUIInputWrapper> m_AutomationInput;
		/// Enumeration for the different sub-menu screens of the pause menu.
		enum PauseMenuScreen {
			MainScreen,
			SaveOrLoadGameScreen,
			SettingsScreen,
			ModManagerScreen,
			ScreenCount
		};

		/// Enumeration for all the different buttons of the pause menu.
		enum PauseMenuButton {
			BackToMainButton,
			SaveOrLoadGameButton,
			SettingsButton,
			ModManagerButton,
			SaveDiagnosticsButton,
			PauseMatchButton,
			LeaveMatchButton,
			MatchOptionsButton,
			EndMatchButton,
			ResumeButton,
			// The confirmation's buttons follow the rows, in their own box: the row layout stops at the resume row.
			LeaveConfirmButton,
			LeaveCancelButton,
			MatchOptionsCloseButton,
			ButtonCount
		};

		std::unique_ptr<GUIControlManager> m_GUIControlManager; //!< The GUIControlManager which owns all the GUIControls of the PauseMenuGUI.
		GUICollectionBox* m_ActiveDialogBox; // The currently active GUICollectionBox in any of the pause menu screens that acts as a dialog box and requires drawing an overlay.

		BITMAP* m_BackdropBitmap; ///!< Bitmap to store half transparent black overlay.

		PauseMenuScreen m_ActiveMenuScreen; //!< The currently active pause menu screen that is being updated and drawn to the screen. See PauseMenuScreen enumeration.
		PauseMenuUpdateResult m_UpdateResult; //!< The result of the PauseMenuGUI update. See PauseMenuUpdateResult enumeration.

		Timer m_ResumeButtonBlinkTimer; //!< Activity resume button blink timer.

		std::unique_ptr<SaveLoadMenuGUI> m_SaveLoadMenu; //!< The settings menu screen.
		std::unique_ptr<SettingsGUI> m_SettingsMenu; //!< The settings menu screen.
		std::unique_ptr<ModManagerGUI> m_ModManagerMenu; //!< The mod manager menu screen.

		// TODO: Rework this hacky garbage implementation when setting button font at runtime without loading a different skin is fixed.
		// Right now the way this works is the font graphic has different character visuals for uppercase and lowercase and the visual change happens by applying the appropriate case string when hovering/unhovering.
		std::array<std::string, PauseMenuButton::ButtonCount> m_ButtonHoveredText; //!< Array containing uppercase strings of the pause menu buttons text that are used to display the larger font when a button is hovered over.
		std::array<std::string, PauseMenuButton::ButtonCount> m_ButtonUnhoveredText; //!< Array containing lowercase strings of the pause menu buttons text that are used to display the smaller font when a button is not hovered over.
		std::array<std::string, 2> m_DiagnosticsIdleText; //!< The idle hovered/unhovered texts restored after a save.
		std::array<std::string, 2> m_MatchPauseText; //!< The hovered/unhovered texts of the match pause row while the match runs.
		std::array<std::string, 2> m_MatchResumeText; //!< The hovered/unhovered texts of the match pause row while the match is paused.
		bool m_DiagnosticsBusy; //!< The last busy state applied to the arrays.
		bool m_MatchPausedShown; //!< The last shared pause state applied to the match pause row.
		GUIButton* m_HoveredButton; //!< The currently hovered pause menu button.
		std::string m_PendingAutomationCommand;
		int m_PrevHoveredButtonIndex; //!< The index of the previously hovered pause menu button in the main menu button array.

		bool m_SavingButtonsDisabled; //!< Whether the save and load buttons are disabled and hidden.
		bool m_ModManagerButtonDisabled; //!< Whether the mod manager button is disabled and hidden.
		bool m_NetworkMatchMode; //!< Whether this menu is the local menu of a running network match.
		bool m_LeaveConfirmShown; //!< Whether the leave confirmation is up instead of the menu rows.
		bool m_BackRequested; //!< A back navigation asked for by the pad's start button.

		/// GUI elements that compose the pause menu screen.
		GUICollectionBox* m_PauseMenuBox;
		std::array<GUIButton*, PauseMenuButton::ButtonCount> m_PauseMenuButtons;
		std::array<int, PauseMenuButton::ButtonCount> m_ButtonHomeY; //!< Each button's row offset inside the menu box, as the skin places it.
		int m_PauseMenuBoxHomeY; //!< The menu box position the skin places, before any row set moves it.

		/// GUI elements of the leave confirmation, which replaces the menu rows while it is up.
		GUICollectionBox* m_LeaveConfirmBox;
		GUILabel* m_LeaveConfirmLabel;

		/// The match's adopted options, shown read-only mid-match: the same panel the lobby's Details
		/// and the F6 seats panel's Options view read from.
		GUICollectionBox* m_MatchOptionsBox;
		GUILabel* m_MatchOptionsLabel;
		bool m_MatchOptionsShown;

#pragma region Menu Screen Handling
		/// Sets the PauseMenuGUI to display a menu screen.
		/// @param screenToShow Which menu screen to display. See PauseMenuScreen enumeration.
		/// @param playButtonPressSound Whether to play a sound if the menu screen change is triggered by a button press.
		void SetActiveMenuScreen(PauseMenuScreen screenToShow, bool playButtonPressSound = true);
#pragma endregion

#pragma region Update Breakdown
		/// Handles returning to the pause menu from one of the sub-menus if the player requested to return via the back button or the esc key. Also handles closing active dialog boxes with the esc key.
		/// @param backButtonPressed Whether the player requested to return to the pause menu from one of the sub-menus via back button.
		void HandleBackNavigation(bool backButtonPressed);

		/// Handles the player interaction with the PauseMenuGUI GUI elements.
		/// @return Whether the player requested to return to the main menu.
		bool HandleInputEvents();

		/// Updates the currently hovered button text to give the hovered visual and updates the previously hovered button to remove the hovered visual.
		/// @param hoveredButton Pointer to the currently hovered button, if any. Acquired by GUIControlManager::GetControlUnderPoint.
		void UpdateHoveredButton(const GUIButton* hoveredButton);

		/// Animates (blinking) the resume game button.
		void BlinkResumeButton();

		/// Puts a button's row at an offset inside the menu box.
		void PlaceButtonRow(int button, int rowOffset);

		/// Shows or hides the leave confirmation in place of the menu rows.
		void ShowLeaveConfirm(bool show);

		/// Shows or hides the match options view in place of the menu rows, refilled on open.
		void ShowMatchOptions(bool show);

		/// The one line of what leaving costs this player, from the session's own hold.
		std::string LeaveConsequenceText() const;

		/// Follows the shared pause state on the match pause row's label.
		/// @param force Whether to write the label even when the shared state has not changed.
		void UpdateMatchPauseRow(bool force = false);
#pragma endregion

		/// Clears all the member variables of this PauseMenuGUI, effectively resetting the members of this object.
		void Clear();

		// Disallow the use of some implicit methods.
		PauseMenuGUI(const PauseMenuGUI& reference) = delete;
		PauseMenuGUI& operator=(const PauseMenuGUI& rhs) = delete;
	};
} // namespace RTE
