#pragma once

#include <iosfwd>
#include <string>

namespace RTE {

	class GUIControl;
	class GUIControlManager;
	class SettingsGUI;

	namespace MenuAutomation {
		bool Visible(GUIControl* control);
		bool Enabled(GUIControl* control);
		bool Text(GUIControl* control, std::string& text);
		/// Name of the settings page the manager currently shows, empty when it shows no settings page.
		std::string SettingsPage(GUIControlManager* manager);
		/// Asks the settings menu owning the manager to show a page on its next event pass.
		bool QueuePage(GUIControlManager* manager, const std::string& page);
		/// Raises the queued page's tab notification, which the settings menu answers in the same pass.
		void ApplyQueuedPage(GUIControlManager* manager);
		bool Handles(const std::string& command);
		bool Execute(GUIControlManager* manager, const std::string& screen, const std::string& command, std::istream& args, std::string& observation);
		void Click(GUIControlManager* manager, const std::string& control);
		void BindSettingsOwner(GUIControlManager* manager, SettingsGUI* owner);
		void UnbindSettingsOwner(GUIControlManager* manager);
	}

} // namespace RTE
