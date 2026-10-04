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
		/// Missing assert_toast_band argument keeps -1.
		int ParseToastBandExpectedRows(std::istream& args);
		/// fire_assert is allowed when the recorder is on or the run is headless.
		bool FireAssertAllowed();
		/// Judges every armed text watch against what this frame shows; call once per drawn frame.
		void EvaluateWatches(GUIControlManager* menu);
		/// Every line of text the screen shows this frame, as a JSON array of {source, control, text}: a probe's label dump.
		std::string ShownTextJson(GUIControlManager* menu);
		/// Records a line this frame drew straight with a font, so the watches read it beside the controls.
		void NoteDrawnText(const std::string& source, const std::string& text);
		/// Logs each armed watch's cumulative frame and offence counts through the current sim tick: "periodic" while the
		/// scene runs, "kill" when the scene announces it drops this peer, "final" once at shutdown.
		void ReportWatches(const char* flush = "final");
		/// Sets where a probe's dumps are written: beside the probe that drives this process.
		void SetArtifactDirectory(const std::string& directory);
		bool RunSelfTest();
		/// Finishes queued readback files before the image library shuts down.
		bool FinishReadbacks();
		void Click(GUIControlManager* manager, const std::string& control);
		void BindSettingsOwner(GUIControlManager* manager, SettingsGUI* owner);
		void UnbindSettingsOwner(GUIControlManager* manager);
	}

} // namespace RTE
