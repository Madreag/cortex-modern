#pragma once

#include <array>
#include <utility>

namespace RTE {

	class GUIControl;

	class GUIButton;
	class GUIControlManager;
	class GUICollectionBox;
	class GUICheckbox;
	class GUIComboBox;
	class GUILabel;
	class GUIRadioButton;
	class GUITab;
	class GUITextBox;
	class GUIEvent;

	/// Handling for multiplayer network settings through the game settings user interface.
	class SettingsNetworkGUI {

	public:
#pragma region Creation
		/// Constructor method used to instantiate a SettingsNetworkGUI object in system memory and make it ready for use.
		/// @param parentControlManager Pointer to the parent GUIControlManager which owns all the GUIControls of this SettingsNetworkGUI. Ownership is NOT transferred!
		explicit SettingsNetworkGUI(GUIControlManager* parentControlManager);
#pragma endregion

#pragma region Concrete Methods
		/// Enables or disables the SettingsNetworkGUI.
		/// @param enable Show and enable or hide and disable the SettingsNetworkGUI.
		void SetEnabled(bool enable = true);

		/// Handles the player interaction with the SettingsNetworkGUI GUI elements.
		/// @param guiEvent The GUIEvent containing information about the player interaction with an element.
		void HandleInputEvents(GUIEvent& guiEvent);
#pragma endregion

	private:
		/// The pages the tab row inside the network box selects between.
		enum class Page { Player = 0, Chat, Recovery, Files, Internet, Count };

		GUIControlManager* m_GUIControlManager; //!< The GUIControlManager which holds all the GUIControls of this menu. Not owned by this.

		/// GUI elements that compose the network settings menu screen.
		GUICollectionBox* m_NetworkSettingsBox;
		std::array<GUITab*, static_cast<int>(Page::Count)> m_PageTabs; //!< The page selector row.
		std::array<GUICollectionBox*, static_cast<int>(Page::Count)> m_PageBoxes; //!< One page box per selector tab.
		Page m_ActivePage = Page::Player; //!< The page the selector currently shows.

		// Player page.
		GUITextBox* m_DisplayNameTextbox;
		GUIRadioButton* m_DelayPolicyAutoRadio;
		GUIRadioButton* m_DelayPolicyFixedRadio;
		GUILabel* m_FixedDelayLabel;
		GUITextBox* m_FixedDelayTextbox;
		GUILabel* m_FixedDelayHintLabel;
		GUILabel* m_IdleWaitLabel;
		GUILabel* m_IdleWaitHintLabel;
		GUITextBox* m_IdleWaitTextbox;
		GUICheckbox* m_AutoRepairCheckbox;
		GUICheckbox* m_ToastsCheckbox;
		GUICheckbox* m_PredictionCheckbox;
		GUIComboBox* m_StatusModeCombo;
		/// Every row drawn under the fixed-delay row, with the y it sits at while that row is drawn.
		std::array<std::pair<GUIControl*, int>, 8> m_RowsUnderFixedDelay;

		// Chat page.
		GUICheckbox* m_ChatVisibleCheckbox;
		GUICheckbox* m_ChatSoundCheckbox;
		GUICheckbox* m_ChatNotifyCheckbox;
		GUIComboBox* m_ChatScopeCombo;
		GUIComboBox* m_ChatTextSizeCombo;

		// Recovery page.
		GUICheckbox* m_AutoReconnectCheckbox;
		GUICheckbox* m_OfferRejoinCheckbox;
		GUILabel* m_LastHostLabel;
		GUILabel* m_RecoveryRecordLabel;
		GUILabel* m_RecoveryStatusLabel;
		GUILabel* m_RecoveryError;
		GUIButton* m_RejoinButton;
		GUIButton* m_CancelRecoveryButton;

		// Files page.
		GUILabel* m_AutosaveLabel;
		GUILabel* m_AutosaveIntervalLabel;
		GUILabel* m_AutosaveInfoLabel;
		GUITextBox* m_DiagDirTextbox;
		GUIButton* m_SaveDiagButton;
		GUICheckbox* m_RecordReplaysCheckbox;
		GUILabel* m_FilesMessage;

		// Internet page.
		GUITextBox* m_DirUrlTextbox;
		GUITextBox* m_DirPinTextbox;
		GUILabel* m_DirStatusLabel;
		GUILabel* m_InternetError;

#pragma region Network Settings Handling
		/// Shows what is saved, so an opened page never states anything the settings do not hold.
		void ShowSavedValues();

		/// Sends the text boxes through the settings validators and shows what they kept.
		void ApplyTextboxes();

		/// A fixed delay only applies under the fixed policy, so its row is drawn only there.
		void UpdateDelayPolicyRow();

		/// Shows the given page, checks its selector tab and commits the boxes the page leaving held.
		void SetActivePage(Page page);

		/// Refreshes the read-only lines and the state-dependent buttons of the Recovery, Files and Internet pages.
		void UpdateStatusLines();
#pragma endregion

		// Disallow the use of some implicit methods.
		SettingsNetworkGUI(const SettingsNetworkGUI& reference) = delete;
		SettingsNetworkGUI& operator=(const SettingsNetworkGUI& rhs) = delete;
	};
} // namespace RTE
