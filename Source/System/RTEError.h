#pragma once

#include "allegro.h"
#include "loadpng.h"

#ifdef _WIN32
#define DebuggerBreak IsDebuggerPresent() ? __debugbreak() : std::abort();
#else
#define DebuggerBreak std::abort()
#endif

#ifndef RELEASE_BUILD
#define AbortAction DebuggerBreak
#else
#define AbortAction std::abort()
#endif

#include <source_location>
#include <string>

namespace RTE {

	/// Class for runtime error handling.
	class RTEError {

	public:
		static bool s_CurrentlyAborting; //!< Flag to prevent a potential recursive fault while attempting to save the game when aborting.
		static bool s_IgnoreAllAsserts; //!< Whether to skip the assert dialog and just let everything burn at whatever point that happens.
		static bool s_AssertFired; //!< Whether any assert fired this run on the headless-continue path.
		static int s_ShowMessageBoxCallCount; //!< ShowMessageBox calls this run, including the headless log path.
		static int s_AssertMessageBoxCallCount; //!< ShowAssertMessageBox calls that reached the app main thread this run, including the forced test path.
		static std::string s_LastIgnoredAssertDescription; //!< The last ignored assert message.
		static std::source_location s_LastIgnoredAssertLocation; //!< The last ignored assert call site.

		/// Sets custom handlers for C++ and platform specific exceptions.
		static void SetExceptionHandlers();

		/// Pops up a message box dialog in the OS. For debug purposes mostly.
		/// @param message The string that the message box should display.
		static void ShowMessageBox(const std::string& message);

		/// Abort on unhandled exception function. Will try save the current game, to dump a screenshot, dump the console log and show an abort message. Then quit the program immediately.
		/// @param description Message explaining the exception.
		/// @param callstack The call stack in string form.
		static void UnhandledExceptionFunc(const std::string& description, const std::string& callstack = "");

		/// Abort on Error function. Will try save the current game, to dump a screenshot, dump the console log and show an abort message. Then quit the program immediately.
		/// @param description Message explaining the reason for aborting.
		/// @param srcLocation std::source_location corresponding to the location of the call site.
		[[noreturn]] static void AbortFunc(const std::string& description, const std::source_location& srcLocation);

		/// An assert, which will prompt to abort or ignore it.
		/// @param description The description of the assertion.
		/// @param srcLocation std::source_location corresponding to the location of the call site.
		static void AssertFunc(const std::string& description, const std::source_location& srcLocation);

		/// Whether any assert fired this run: an ignored dialog is still a failed run.
		static bool AssertFired() { return s_AssertFired; }

		/// How many ShowMessageBox calls have been made this run (headed or headless).
		static int ShowMessageBoxCallCount() { return s_ShowMessageBoxCallCount; }

		/// Resets the ShowMessageBox counter for a native selftest.
		static void ResetShowMessageBoxCallCount() { s_ShowMessageBoxCallCount = 0; }

		/// How many ShowAssertMessageBox calls have reached the app main thread this run.
		static int AssertMessageBoxCallCount() { return s_AssertMessageBoxCallCount; }

		/// Resets the assert message box counter for a native selftest.
		static void ResetAssertMessageBoxCallCount() { s_AssertMessageBoxCallCount = 0; }

		/// How many worker-thread messages are still waiting for the app main thread to dispatch.
		static int PendingWorkerMessageCount();

		/// Shows the assert/warning/abort a worker thread recorded since the last frame, once per frame on the app main thread.
		static void DispatchPendingWorkerMessages();

		/// AssertFunc on the headed dialog/Ignore path leaves AssertFired false; the headless continue path still sets it.
		static bool RunAssertPolicySelfTest();

		/// Formats function signatures so they're slightly more sane.
		/// @param funcSig Reference to the function signature to format.
		static void FormatFunctionSignature(std::string& funcSig);

	private:
		/// Pops up the abort message box dialog in the OS, notifying the user about a runtime error.
		/// @param message The string that the message box should display.
		/// @return Whether to restart the game by launching a new instance, or proceed to exit.
		static bool ShowAbortMessageBox(const std::string& message);

		/// Pops up the assert message box dialog in the OS, notifying the user about a runtime error.
		/// @param message The string that the message box should display.
		/// @return Whether to abort, or ignore the assert and continue execution.
		static bool ShowAssertMessageBox(const std::string& message);

		/// Prints details on the user hardware to the abort log.
		/// @remark
		/// Included details:
		/// OpenGL version string, OpenGL GPU vendor string, OpenGL Renderer string.
		/// CPU vendor and brand string (as reported by cpuid on windows and linux or sysctl on macos).
		/// linux: uname sysname, release, version.
		/// macos: sysctl kern.osrelease, kern.ostype, kern.osversion.
		static void DumpHardwareInfo();

		/// Saves the current frame to a file.
		/// @return Whether the file was saved successfully.@return
		static bool DumpAbortScreen();

		/// Attempts to save the current running Activity, so the player can hopefully resume where they were.
		/// @return Whether the Activity was saved successfully.
		static bool DumpAbortSave();
	};

#define RTEAbort(description) \
	if (!RTEError::s_CurrentlyAborting) { \
		RTEError::AbortFunc(description, std::source_location::current()); \
	}

#define RTEAssert(expression, description) \
	if (!(expression)) { \
		RTEError::AssertFunc(description, std::source_location::current()); \
	}
} // namespace RTE
