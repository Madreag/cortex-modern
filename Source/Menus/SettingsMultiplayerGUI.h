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
		void SetEnabled(bool enable) const;

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

		/// Shows the given page and checks its selector tab.
		void SetActivePage(Page page);
	};
}
