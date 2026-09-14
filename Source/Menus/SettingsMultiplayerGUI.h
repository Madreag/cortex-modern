#pragma once

#include "GUI.h"

namespace RTE {

	class GUIControlManager;
	class GUIEvent;
	class GUIButton;
	class GUICheckbox;
	class GUICollectionBox;
	class GUIComboBox;
	class GUILabel;
	class GUITab;
	class GUITextBox;

	/// Contents of the settings screen's Multiplayer tab: the Player / Chat / Recovery / Files /
	/// Internet pages that stage the network preferences and the per-page actions behind them.
	/// Edits are staged in the controls and only reach SettingsMan through Apply.
	class SettingsMultiplayerGUI {
	public:
		/// Constructor method used to instantiate a SettingsMultiplayerGUI object in system memory and make it ready for use.
		/// @param parentControlManager The parent GUIControlManager which owns all the child controls of this SettingsMultiplayerGUI. Ownership is NOT transferred!
		explicit SettingsMultiplayerGUI(GUIControlManager* parentControlManager);

		/// Enables or disables the menu. This is done by hiding or showing all the controls in this menu.
		/// @param enable Show and enable this menu (true) or hide and disable it (false).
		void SetEnabled(bool enable);

		/// Handles the player interaction with the SettingsMultiplayerGUI GUI elements.
		/// @param guiEvent The GUIEvent containing information about the player interaction with an element.
		void HandleInputEvents(GUIEvent& guiEvent);

	private:
		enum class Page { Player = 0, Chat, Recovery, Files, Internet, Count };

		GUIControlManager* m_GUIControlManager = nullptr; //!< The GUIControlManager which holds all the controls of this menu. Not owned by this.

		GUICollectionBox* m_MultiplayerSettingsBox = nullptr; //!< The settings container box.
		GUITab* m_PageTabs[static_cast<int>(Page::Count)] = {}; //!< The page selector tabs.
		GUICollectionBox* m_PageBoxes[static_cast<int>(Page::Count)] = {}; //!< The five page boxes.
		Page m_ActivePage = Page::Player; //!< The page currently shown.

		GUIButton* m_ApplyButton = nullptr; //!< Applies the staged edits on every page; lives beside the Back button.
		bool m_PendingSave = false; //!< The last apply wrote the settings but the file write failed; Apply retries the save.

		// Player page.
		GUITextBox* m_DisplayNameBox = nullptr;
		GUIComboBox* m_StatusModeCombo = nullptr;
		GUICheckbox* m_NotificationsCheckbox = nullptr;
		GUICheckbox* m_PredictionCheckbox = nullptr;
		GUILabel* m_PlayerError = nullptr;

		// Chat page.
		GUICheckbox* m_ChatVisibleCheckbox = nullptr;
		GUICheckbox* m_ChatSoundCheckbox = nullptr;
		GUICheckbox* m_ChatNotifyCheckbox = nullptr;
		GUIComboBox* m_ChatScopeCombo = nullptr;
		GUIComboBox* m_ChatTextSizeCombo = nullptr;
		GUIButton* m_MutedPlayersButton = nullptr;
		GUILabel* m_ChatError = nullptr;

		// Recovery page.
		GUICheckbox* m_AutoReconnectCheckbox = nullptr;
		GUICheckbox* m_OfferRejoinCheckbox = nullptr;
		GUILabel* m_LastHostLabel = nullptr;
		GUILabel* m_RecoveryRecordLabel = nullptr;
		GUILabel* m_RecoveryStatusLabel = nullptr;
		GUIButton* m_RejoinButton = nullptr;
		GUIButton* m_CancelRecoveryButton = nullptr;
		GUILabel* m_RecoveryError = nullptr;

		// Files page.
		GUILabel* m_AutosaveLabel = nullptr;
		GUILabel* m_AutosaveIntervalLabel = nullptr;
		GUILabel* m_AutosaveInfoLabel = nullptr;
		GUITextBox* m_DiagDirBox = nullptr;
		GUIButton* m_SaveDiagButton = nullptr;
		GUIButton* m_ReplaysButton = nullptr;
		GUICheckbox* m_RecordReplaysCheckbox = nullptr;
		GUILabel* m_FilesMessage = nullptr;

		// Internet page.
		GUITextBox* m_DirUrlBox = nullptr;
		GUITextBox* m_DirPinBox = nullptr;
		GUILabel* m_DirStatusLabel = nullptr;
		GUILabel* m_InternetError = nullptr;

		/// Shows the given page and checks its selector tab.
		void SetActivePage(Page page);

		/// Loads every staged control from the current SettingsMan values.
		void ResetDraft();

		/// Resets the staged controls of one page to the defaults.
		void ResetPageDefaults(Page page);

		/// Validates the staged edits, writes them to SettingsMan and saves the settings file.
		void ApplyDraft();

		/// @return Whether every staged control already matches the persisted settings.
		bool DraftMatchesSettings() const;

		/// Refreshes the read-only lines (recovery record, autosave mirror, directory status) and
		/// the state-dependent buttons, and enables Apply only while edits or a failed save are pending.
		void UpdateStatusLines();

		/// Refreshes the diagnostics capture status line while a bundle request is in flight.
		void UpdateDiagnosticsStatus();

		/// Shows an inline error on the given page and switches to it.
		void FailOnPage(Page page, const std::string& text, GUIControl* focus);

		/// Fills the match status combo with the mode entries and selects the persisted mode.
		void CreateStatusModeCombo();

		/// Handles the match status combo closing; the selection is staged until Apply.
		void OnStatusModeEvent(GUIEvent& guiEvent);

		bool m_DiagWasBusy = false; //!< Tracks the diagnostics capture so its status line can report completion.
	};
}
