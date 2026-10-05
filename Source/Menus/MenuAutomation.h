#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

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
		/// Expands "sweep <root> [depth=N] [restore=MACRO] [quiet=A,B] [own=A,B] [label=L] [readonly]" into the steps that change every
		/// enabled control the screen draws under root by hand, read it back frames later and put it back; buttons are pressed and the
		/// restore macro brings the screen back, or the case presses them itself (own).
		bool SweepSteps(GUIControlManager* manager, std::istream& args, std::vector<std::string>& steps, std::string& observation);
		/// Whether every button a sweep left to the case was pressed by a hand since; the missing names otherwise.
		bool OwedPressed(std::string& missing);
		/// Advances a hand's gesture by one phase; call once per drawn frame, after the frame is drawn.
		void AfterDrawnFrame();
		/// Whether a hand's gesture is still running: its driver waits for it before the next step.
		bool HandBusy();
		/// The finished gesture's verdict and record, handed out once.
		bool HandFinished(bool& passed, std::string& observation);
		/// Starts a click (or a double click) on a control the way a mouse makes one: point, press, release, a frame apart,
		/// through the GUI manager's own hit test. A control behind the modal dialog, covered, off the screen or disabled refuses.
		bool HandClick(GUIControlManager* manager, const std::string& name, GUIControl* modal, std::string& observation, int presses = 1);
		/// Clicks a point that is no control (a site on the planet), then waits until the screen shows it took the click.
		bool HandClickAt(int x, int y, const std::string& label, std::function<bool()> taken, std::string& observation);
		/// Presses and lets go of a key in play, then waits until the screen shows it took the key.
		bool HandGameKey(const std::string& key, std::function<bool()> taken, const std::string& label, std::string& observation);
		/// Points at a control; a text box is clicked into, the way a hand gives it the keyboard.
		bool HandPoint(GUIControlManager* manager, const std::string& name, std::string& observation);
		/// Clicks into a text box, selects what it holds, types the text and presses Enter when asked, with a key held (Left Ctrl) when named.
		bool HandType(GUIControlManager* manager, const std::string& name, const std::string& text, bool enter, std::string& observation, const std::string& enterWith = {});
		/// Opens a drop-down, presses its row, lifts a frame later and checks the pick holds through the next frames,
		/// or, refused, that the screen turned it down and kept its previous row.
		bool HandPick(GUIControlManager* manager, const std::string& name, const std::string& item, std::string& observation, bool refused = false);
		/// Clicks (or double-clicks) a list's row, wheeling it into view first.
		bool HandRow(GUIControlManager* manager, const std::string& listName, int index, int presses, GUIControl* modal, std::string& observation);
		/// Presses a slider's track where the value lies and wheels the rest of the way.
		bool HandDrag(GUIControlManager* manager, const std::string& name, int value, std::string& observation);
	}

} // namespace RTE
