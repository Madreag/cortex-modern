#pragma once

namespace RTE {

	class GUIControlManager;
	class GUICollectionBox;
	class GUICheckbox;
	class GUILabel;
	class GUIRadioButton;
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
		GUIControlManager* m_GUIControlManager; //!< The GUIControlManager which holds all the GUIControls of this menu. Not owned by this.

		/// GUI elements that compose the network settings menu screen.
		GUICollectionBox* m_NetworkSettingsBox;
		GUITextBox* m_DisplayNameTextbox;
		GUIRadioButton* m_DelayPolicyAutoRadio;
		GUIRadioButton* m_DelayPolicyFixedRadio;
		GUILabel* m_FixedDelayLabel;
		GUITextBox* m_FixedDelayTextbox;
		GUILabel* m_FixedDelayHintLabel;
		GUILabel* m_IdleWaitLabel;
		GUITextBox* m_IdleWaitTextbox;
		GUICheckbox* m_AutoRepairCheckbox;

#pragma region Network Settings Handling
		/// Shows what is saved, so an opened page never states anything the settings do not hold.
		void ShowSavedValues();

		/// Sends the text boxes through the settings validators and shows what they kept.
		void ApplyTextboxes();

		/// A fixed delay only applies under the fixed policy, so its row is drawn only there.
		void UpdateDelayPolicyRow();
#pragma endregion

		// Disallow the use of some implicit methods.
		SettingsNetworkGUI(const SettingsNetworkGUI& reference) = delete;
		SettingsNetworkGUI& operator=(const SettingsNetworkGUI& rhs) = delete;
	};
} // namespace RTE
