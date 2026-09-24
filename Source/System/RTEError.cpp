#include "RTEError.h"

#include "WindowMan.h"
#include "FrameMan.h"
#include "ConsoleMan.h"
#include "ActivityMan.h"
#include "System.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_messagebox.h>

#ifdef _WIN32
#include "Windows.h"
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <iostream>
#include <mutex>
#include <regex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#elif defined(__linux__)
#include <cpuid.h>
#endif

#ifdef __linux__
#include <sys/utsname.h>
#include <fstream>
#include <filesystem>
#elif defined(__APPLE__) && defined(__MACH__)
#include <sys/sysctl.h>
#include <pthread.h>
#endif

#ifdef _WIN32
static std::atomic<unsigned long> s_AppMainThreadId{0};

static bool IsOnAppMainThread() {
	const unsigned long current = GetCurrentThreadId();
	unsigned long expected = 0;
	s_AppMainThreadId.compare_exchange_strong(expected, current);
	return current == s_AppMainThreadId.load();
}
#else
// SDL answers true on every thread until it has recorded its own main thread, so the app's is kept here.
static std::atomic<std::thread::id> s_AppMainThreadId{};

static bool IsOnAppMainThread() {
	const std::thread::id current = std::this_thread::get_id();
	std::thread::id expected{};
	s_AppMainThreadId.compare_exchange_strong(expected, current);
	return current == s_AppMainThreadId.load();
}
#endif

#include "backward/backward.hpp"


using namespace RTE;

bool RTEError::s_CurrentlyAborting = false;
bool RTEError::s_IgnoreAllAsserts = false;
bool RTEError::s_AssertFired = false;
int RTEError::s_ShowMessageBoxCallCount = 0;
int RTEError::s_AssertMessageBoxCallCount = 0;
static bool s_ForceAssertDialogPathForTest = false;
std::string RTEError::s_LastIgnoredAssertDescription = "";
SourceLocation RTEError::s_LastIgnoredAssertLocation = {};

/// What a worker thread leaves for the app main thread to surface at its next frame: the text the
/// dialog would have shown, plus what a dispatched Abort needs to abort the way AssertFunc would.
enum class WorkerMessageKind : uint8_t { Assert, Warning, Abort };
struct PendingWorkerMessage {
	WorkerMessageKind kind = WorkerMessageKind::Assert;
	std::string message;
	std::string description;
	SourceLocation location = {};
};
static std::mutex s_PendingWorkerMessageMutex;
static PendingWorkerMessage s_PendingWorkerMessageRecord;
static std::atomic<bool> s_PendingWorkerMessage{false};
static std::atomic<int> s_PendingWorkerMessageDropped{0};

// The first pending record is kept until the main thread takes it; later ones are only counted.
static void RecordPendingWorkerMessage(WorkerMessageKind kind, const std::string& message, const std::string& description, const SourceLocation& location) {
	std::lock_guard<std::mutex> lock(s_PendingWorkerMessageMutex);
	if (s_PendingWorkerMessage.load(std::memory_order_relaxed)) {
		++s_PendingWorkerMessageDropped;
		return;
	}
	s_PendingWorkerMessageRecord = PendingWorkerMessage{kind, message, description, location};
	s_PendingWorkerMessage.store(true, std::memory_order_release);
}

#if (defined(__linux__) || (defined(__APPLE__) && defined(__MACH__)))
backward::SignalHandling sh;
#endif
#ifdef _WIN32
/// <summary>
/// Custom exception handler for Windows SEH.
/// Unfortunately this also intercepts any C++ exceptions and turns them into SE bullshit, meaning we can't get and rethrow the current C++ exception to get what() from it.
/// Even if we "translate" SE exceptions to C++ exceptions it's still ass and doesn't really work, so this is what it is and it is good enough.
/// </summary>
/// <param name="exceptPtr">Struct containing information about the exception. This will be provided by the OS exception handler.</param>
static LONG WINAPI RTEWindowsExceptionHandler([[maybe_unused]] EXCEPTION_POINTERS* exceptPtr) {
	// This sorta half-assedly works in x86 because exception handling is slightly different, but since the main target is x64 we can just not care about it.
	// Something something ESP. ESP is a guitar brand.
#ifndef TARGET_MACHINE_X86

	// Returns the last Win32 error in string format. Returns an empty string if there is no error.
	static auto getLastWinErrorAsString = []() -> std::string {
		DWORD errorMessageID = GetLastError();
		if (errorMessageID == 0) {
			return "";
		}
		LPSTR messageBuffer = nullptr;
		DWORD messageFlags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;

		// This bullshit makes the error string and returns the size because we can't know it in advance. Don't think we actually care about the size when we construct string from a buffer but whatever.
		size_t messageSize = FormatMessage(messageFlags, nullptr, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&messageBuffer), 0, nullptr);
		std::string message(messageBuffer, messageSize);
		LocalFree(messageBuffer);
		return message;
	};

	// Returns a string with the type of the exception from the passed in code.
	static auto getExceptionDescriptionFromCode = [](const DWORD& exceptCode) -> std::string {
		switch (exceptCode) {
			case EXCEPTION_ACCESS_VIOLATION:
				return "EXCEPTION_ACCESS_VIOLATION";
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
				return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
			case EXCEPTION_BREAKPOINT:
				return "EXCEPTION_BREAKPOINT";
			case EXCEPTION_DATATYPE_MISALIGNMENT:
				return "EXCEPTION_DATATYPE_MISALIGNMENT";
			case EXCEPTION_FLT_DENORMAL_OPERAND:
				return "EXCEPTION_FLT_DENORMAL_OPERAND";
			case EXCEPTION_FLT_DIVIDE_BY_ZERO:
				return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
			case EXCEPTION_FLT_INEXACT_RESULT:
				return "EXCEPTION_FLT_INEXACT_RESULT";
			case EXCEPTION_FLT_INVALID_OPERATION:
				return "EXCEPTION_FLT_INVALID_OPERATION";
			case EXCEPTION_FLT_OVERFLOW:
				return "EXCEPTION_FLT_OVERFLOW";
			case EXCEPTION_FLT_STACK_CHECK:
				return "EXCEPTION_FLT_STACK_CHECK";
			case EXCEPTION_FLT_UNDERFLOW:
				return "EXCEPTION_FLT_UNDERFLOW";
			case EXCEPTION_ILLEGAL_INSTRUCTION:
				return "EXCEPTION_ILLEGAL_INSTRUCTION";
			case EXCEPTION_IN_PAGE_ERROR:
				return "EXCEPTION_IN_PAGE_ERROR";
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
				return "EXCEPTION_INT_DIVIDE_BY_ZERO";
			case EXCEPTION_INT_OVERFLOW:
				return "EXCEPTION_INT_OVERFLOW";
			case EXCEPTION_INVALID_DISPOSITION:
				return "EXCEPTION_INVALID_DISPOSITION";
			case EXCEPTION_NONCONTINUABLE_EXCEPTION:
				return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
			case EXCEPTION_PRIV_INSTRUCTION:
				return "EXCEPTION_PRIV_INSTRUCTION";
			case EXCEPTION_SINGLE_STEP:
				return "EXCEPTION_SINGLE_STEP";
			case EXCEPTION_STACK_OVERFLOW:
				return "EXCEPTION_STACK_OVERFLOW";
			default:
				return "UNKNOWN EXCEPTION";
		}
	};

	// Attempts to get a symbol name from the exception address.
	static auto getSymbolNameFromAddress = [](HANDLE& procHandle, const size_t& exceptAddr) {
		if (SymInitialize(procHandle, nullptr, TRUE)) {
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);

			char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
			PSYMBOL_INFO symbolInfo = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);

			symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbolInfo->MaxNameLen = MAX_SYM_NAME;

			if (SymFromAddr(procHandle, exceptAddr, nullptr, symbolInfo)) {
				std::string symbolName = symbolInfo->Name;
				return "The symbol name at this address is" + (symbolName.empty() ? " empty for reasons unknown to man." : ": \"" + symbolName + "\"");
			} else {
				return "Unable to get symbol name at address because:\n\n" + getLastWinErrorAsString();
			}
		}
		std::string error = getLastWinErrorAsString();
		return "Unable to get symbol name at address.\nSymbol Handler failed to initialize " + (error.empty() ? "for reasons unknown to man." : "because\n\n" + error);
	};

	HANDLE processHandle = GetCurrentProcess();

	std::stringstream exceptionDescription;
	DWORD exceptionCode = exceptPtr->ExceptionRecord->ExceptionCode;
	size_t exceptionAddress = reinterpret_cast<size_t>(exceptPtr->ExceptionRecord->ExceptionAddress);

	if (exceptionCode == EXCEPTION_BREAKPOINT) {
		// Advance to the next instruction otherwise this handler will be called for all eternity.
		exceptPtr->ContextRecord->Rip++;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	// A durable minimal record FIRST: everything below (symbols, stack walk, abort save,
	// screenshot) can nest-fault and kill the process before any output lands. The once-guard
	// keeps a second thread faulting at the same moment from racing the same file open.
	static std::atomic_flag s_minimalRecordWritten = ATOMIC_FLAG_INIT;
	if (!s_minimalRecordWritten.test_and_set()) {
		char minimalRecord[128];
		const size_t imageBase = reinterpret_cast<size_t>(GetModuleHandleW(nullptr));
		size_t imageSize = 0;
		if (imageBase != 0) {
			const IMAGE_NT_HEADERS* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase)->e_lfanew);
			imageSize = ntHeaders->OptionalHeader.SizeOfImage;
		}
		// A fault outside the executable keeps the raw VA only.
		const bool inImage = imageBase != 0 && exceptionAddress >= imageBase && exceptionAddress < imageBase + imageSize;
		const int recordLength = inImage
			? std::snprintf(minimalRecord, sizeof(minimalRecord), "FATAL: unhandled exception 0x%08lX at 0x%zX (exe+0x%zX)\n", static_cast<unsigned long>(exceptionCode), exceptionAddress, exceptionAddress - imageBase)
			: std::snprintf(minimalRecord, sizeof(minimalRecord), "FATAL: unhandled exception 0x%08lX at 0x%zX\n", static_cast<unsigned long>(exceptionCode), exceptionAddress);
		if (recordLength > 0) {
			std::fwrite(minimalRecord, 1, static_cast<size_t>(recordLength), stderr);
			std::fflush(stderr);
			if (std::FILE* recordFile = std::fopen("AbortCode.txt", "w")) {
				std::fwrite(minimalRecord, 1, static_cast<size_t>(recordLength), recordFile);
				std::fclose(recordFile);
			}
		}
		wchar_t dumpPath[MAX_PATH];
		const DWORD dumpPathLength = GetEnvironmentVariableW(L"CC_TEST_CRASH_DUMP", dumpPath, MAX_PATH);
		wchar_t fullDumpFlag[16];
		const DWORD fullDumpLength = GetEnvironmentVariableW(L"CC_TEST_CRASH_DUMP_FULL", fullDumpFlag, 16);
		if (dumpPathLength > 0 && dumpPathLength < MAX_PATH) {
			HANDLE dumpFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (dumpFile != INVALID_HANDLE_VALUE) {
				MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{GetCurrentThreadId(), exceptPtr, FALSE};
				const MINIDUMP_TYPE dumpType = fullDumpLength > 0
					? MiniDumpWithFullMemory
					: static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithDataSegs);
				MiniDumpWriteDump(processHandle, GetCurrentProcessId(), dumpFile, dumpType, &exceptionInfo, nullptr, nullptr);
				CloseHandle(dumpFile);
			}
		}
	}

	// A call through a corrupted pointer faults outside every module; the return addresses still on the stack name the caller.
	if (std::FILE* recordFile = std::fopen("AbortCode.txt", "a")) {
		const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
		const IMAGE_NT_HEADERS* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase)->e_lfanew);
		const uintptr_t imageEnd = imageBase + ntHeaders->OptionalHeader.SizeOfImage;
		const bool symbols = SymInitialize(processHandle, nullptr, TRUE);
		if (symbols) {
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		}
		const uintptr_t* stackWord = reinterpret_cast<const uintptr_t*>(exceptPtr->ContextRecord->Rsp);
		for (int i = 0; i < 1024; ++i) {
			MEMORY_BASIC_INFORMATION region{};
			if (VirtualQuery(stackWord + i, &region, sizeof(region)) == 0 || region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				break;
			}
			const uintptr_t word = stackWord[i];
			if (word < imageBase || word >= imageEnd) {
				continue;
			}
			char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
			PSYMBOL_INFO symbolInfo = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
			symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbolInfo->MaxNameLen = MAX_SYM_NAME;
			DWORD64 displacement = 0;
			const char* name = (symbols && SymFromAddr(processHandle, word, &displacement, symbolInfo)) ? symbolInfo->Name : "?";
			std::fprintf(recordFile, "stack[%d] exe+0x%zX %s+0x%llX\n", i, static_cast<size_t>(word - imageBase), name, static_cast<unsigned long long>(displacement));
		}
		if (symbols) {
			SymCleanup(processHandle);
		}
		std::fclose(recordFile);
	}

	std::string symbolNameAtAddress = getSymbolNameFromAddress(processHandle, exceptionAddress);
	RTEError::FormatFunctionSignature(symbolNameAtAddress);

	exceptionDescription << getExceptionDescriptionFromCode(exceptionCode) << " at address 0x" << std::uppercase << std::hex << exceptionAddress << ".\n\n"
	                     << symbolNameAtAddress << std::endl;

	backward::StackTrace st;
	st.load_here(32, exceptPtr->ContextRecord);
	backward::Printer printer;
	std::ostringstream stack;
	printer.print(st, stack);

	RTEError::UnhandledExceptionFunc(exceptionDescription.str(), stack.str());
	return EXCEPTION_EXECUTE_HANDLER;
#endif
}
#endif

void RTEError::SetExceptionHandlers() {
#ifdef _WIN32
	s_AppMainThreadId.store(GetCurrentThreadId());
#else
	s_AppMainThreadId.store(std::this_thread::get_id());
#endif
	// Basic handling for C++ exceptions. Doesn't give us much meaningful information.
	[[maybe_unused]] static const std::terminate_handler terminateHandler = []() {
		std::exception_ptr currentException = std::current_exception();

		if (currentException) {
			try {
				std::rethrow_exception(currentException);
			} catch (const std::bad_exception& exception) {
				RTEError::UnhandledExceptionFunc("Unable to get exception description because: " + std::string(exception.what()) + ".\n");
			} catch (const std::exception& exception) {
				RTEError::UnhandledExceptionFunc(std::string(exception.what()) + ".\n");
			}
		} else {
			RTEError::UnhandledExceptionFunc("Terminate was called without an exception.\nMay god have mercy on us all.");
		}
	};

#ifdef _WIN32
#ifndef TARGET_MACHINE_X86
	// Reserve emergency stack so the handler can still write its minimal record after a
	// stack-overflow fault leaves almost no stack.
	ULONG stackGuaranteeBytes = 32U * 1024U;
	SetThreadStackGuarantee(&stackGuaranteeBytes);
	SetUnhandledExceptionFilter(RTEWindowsExceptionHandler);
#else
	// This only works for C++ exceptions and doesn't catch and access violations and such, or provide much meaningful info.
	std::set_terminate(terminateHandler);
#endif
#else
	// TODO: Deal with segfaults and such on other systems. Probably need to use Unix signal junk to get any meaningful information. Good luck and godspeed to whoever deals with this.
	std::set_terminate(terminateHandler);
#endif
}

void RTEError::ShowMessageBox(const std::string& message) {
	s_ShowMessageBoxCallCount++;
	if (!IsOnAppMainThread()) {
		RecordPendingWorkerMessage(WorkerMessageKind::Warning, message, "", {});
		System::PrintFaultLine("RTE Warning (from worker thread): " + message);
		return;
	}
	if (SDL_getenv("CCCP_HEADLESS") != nullptr) {
		System::PrintFaultLine("RTE Warning (headless): " + message);
		return;
	}
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "RTE Warning! (>_<)", message.c_str(), nullptr);
}

bool RTEError::ShowAbortMessageBox(const std::string& message) {
	if (!IsOnAppMainThread()) {
		System::PrintFaultLine("RTE Abort (from worker thread): " + message);
		return false;
	}
	// Headless / automated runs can't dismiss a modal dialog — log + proceed to exit.
	if (SDL_getenv("CCCP_HEADLESS") != nullptr) {
		System::PrintFaultLine("RTE Abort (headless): " + message);
		return false;
	}
	enum AbortMessageButton {
		ButtonInvalid,
		ButtonExit,
		ButtonRestart
	};

	std::vector<SDL_MessageBoxButtonData> abortMessageBoxButtons = {
	    SDL_MessageBoxButtonData(SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, AbortMessageButton::ButtonExit, "OK")};

	// Don't even show the restart button in debug builds.
#ifdef RELEASE_BUILD
	// Getting a junk path from argv[0] is, or should be, impossible but check anyway.
	if (std::filesystem::exists(System::GetThisExePathAndName())) {
		abortMessageBoxButtons.emplace_back(0, AbortMessageButton::ButtonRestart, "Restart Game");
	}
#endif

	SDL_MessageBoxData abortMessageBox = {
	    SDL_MESSAGEBOX_ERROR,
	    g_WindowMan.GetWindow(),
	    "RTE Aborted! (x_x)",
	    message.c_str(),
	    static_cast<int>(abortMessageBoxButtons.size()),
	    abortMessageBoxButtons.data(),
	    nullptr};

	int pressedButton = AbortMessageButton::ButtonInvalid;
	SDL_ShowMessageBox(&abortMessageBox, &pressedButton);

	return pressedButton == AbortMessageButton::ButtonRestart;
}

bool RTEError::ShowAssertMessageBox(const std::string& message) {
	if (!IsOnAppMainThread()) {
		if (SDL_getenv("CCCP_HEADLESS") != nullptr) {
			s_AssertFired = true;
		}
		System::PrintFaultLine("RTE Assert (from worker thread): " + message);
		return false;
	}
	// The headed dialog path, forced or shown: tests count the box here without opening SDL.
	s_AssertMessageBoxCallCount++;
	if (s_ForceAssertDialogPathForTest) {
		return false;
	}
	// A headless run answers the dialog the way a player does: Ignore, and carry on. The fired assert is
	// remembered so the run still ends non-zero and the reviewer reads the line the player would have read.
	if (SDL_getenv("CCCP_HEADLESS") != nullptr) {
		s_AssertFired = true;
		System::PrintFaultLine("RTE Assert (headless, continued like Ignore): " + message);
		return false;
	}
	enum AssertMessageButton {
		ButtonInvalid,
		ButtonAbort,
		ButtonIgnore,
		ButtonIgnoreAll
	};

	std::vector<SDL_MessageBoxButtonData> assertMessageBoxButtons = {
	    SDL_MessageBoxButtonData(SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, AssertMessageButton::ButtonAbort, "Abort"),
	    SDL_MessageBoxButtonData(0, AssertMessageButton::ButtonIgnore, "Ignore"),
	    SDL_MessageBoxButtonData(0, AssertMessageButton::ButtonIgnoreAll, "Ignore All")};

	SDL_MessageBoxData assertMessageBox = {
	    SDL_MESSAGEBOX_ERROR,
	    g_WindowMan.GetWindow(),
	    "RTE Assert! (x_x)",
	    message.c_str(),
	    static_cast<int>(assertMessageBoxButtons.size()),
	    assertMessageBoxButtons.data(),
	    nullptr};

	int pressedButton = AssertMessageButton::ButtonInvalid;
	SDL_ShowMessageBox(&assertMessageBox, &pressedButton);

	if (pressedButton == AssertMessageButton::ButtonIgnoreAll) {
		s_IgnoreAllAsserts = true;
	}

	return pressedButton == AssertMessageButton::ButtonAbort;
}

void RTEError::UnhandledExceptionFunc(const std::string& description, const std::string& callstack) {
	s_CurrentlyAborting = true;

	std::string exceptionMessage = "Runtime Error due to unhandled exception!\n\n" + description;

	if (!s_LastIgnoredAssertDescription.empty()) {
		exceptionMessage += "\nThe last ignored Assertion was: " + s_LastIgnoredAssertDescription;
	}

	if (s_LastIgnoredAssertLocation.line() > 0) {
		// This typically contains the absolute path to the file on whatever machine this was compiled on, so in that case get only the file name.
		std::filesystem::path filePath = s_LastIgnoredAssertLocation.file_name();
		std::string fileName = (filePath.has_root_name() || filePath.has_root_directory()) ? filePath.filename().generic_string() : s_LastIgnoredAssertLocation.file_name();
		std::string srcLocation = "file '" + fileName + "', line " + std::to_string(s_LastIgnoredAssertLocation.line()) + ",\nin function '" + s_LastIgnoredAssertLocation.function_name() + "'";

		if (!s_LastIgnoredAssertDescription.empty()) {
			exceptionMessage += "\nIn " + srcLocation + ".\n";
		} else {
			exceptionMessage += "\nThe last ignored Assertion was in " + srcLocation + ".\n";
		}
	}

	if (DumpAbortSave()) {
		exceptionMessage += "\nThe game has saved to 'AbortSave'.";
	}
	if (DumpAbortScreen()) {
		exceptionMessage += "\nThe last frame has been dumped to 'AbortScreen.png'.";
	}

	g_ConsoleMan.PrintString(exceptionMessage);

	std::string consoleSaveMsg;
	if (!callstack.empty()) {
		g_ConsoleMan.PrintString(callstack);
		consoleSaveMsg = "\nThe console and callstack have been dumped to 'AbortLog.txt'.";
	} else {
		consoleSaveMsg = "\nThe console has been dumped to 'AbortLog.txt'.";
	}
	if (g_ConsoleMan.SaveAllText("AbortLog.txt")) {
		exceptionMessage += consoleSaveMsg;
	}
	System::PrintFaultToCLI(exceptionMessage);

	// Ditch the video mode so the message box appears without problems.
	if (g_WindowMan.GetWindow()) {
		SDL_SetWindowFullscreen(g_WindowMan.GetWindow(), 0);
	}

	// Headless / automated runs can't dismiss a modal dialog — the CLI print above suffices.
	if (SDL_getenv("CCCP_HEADLESS") == nullptr) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "RTE CATASTROPHIC ERROR!!! (X_X)", exceptionMessage.c_str(), nullptr);
	}
	AbortAction;
}

void RTEError::AbortFunc(const std::string& description, const SourceLocation& srcLocation) {
	s_CurrentlyAborting = true;

	if (!System::IsInExternalModuleValidationMode()) {
		// This typically contains the absolute path to the file on whatever machine this was compiled on, so in that case get only the file name.
		std::filesystem::path filePath = srcLocation.file_name();
		std::string fileName = (filePath.has_root_name() || filePath.has_root_directory()) ? filePath.filename().generic_string() : srcLocation.file_name();

		std::string lineNum = std::to_string(srcLocation.line());
		std::string funcName = srcLocation.function_name();
		FormatFunctionSignature(funcName);

		std::string abortMessage = "Runtime Error in file '" + fileName + "', line " + lineNum + ",\nin function '" + funcName + "'\nbecause:\n\n" + description + "\n";

		if (DumpAbortSave()) {
			abortMessage += "\nThe game has saved to 'AbortSave'.";
		}
		if (DumpAbortScreen()) {
			abortMessage += "\nThe last frame has been dumped to 'AbortScreen.png'.";
		}

		g_ConsoleMan.PrintString(abortMessage);

		DumpHardwareInfo();

		std::string callstack = "";

		backward::StackTrace st;
		st.load_here();
		backward::Printer printer;
		std::ostringstream stack;
		printer.print(st, stack);
		callstack = stack.str();

		std::string consoleSaveMsg;
		if (!callstack.empty()) {
			g_ConsoleMan.PrintString(callstack);
			consoleSaveMsg = "\nThe console and callstack have been dumped to 'AbortLog.txt'.";
		} else {
			consoleSaveMsg = "\nThe console has been dumped to 'AbortLog.txt'.";
		}

		if (g_ConsoleMan.SaveAllText("AbortLog.txt")) {
			abortMessage += consoleSaveMsg;
		}
		System::PrintFaultToCLI(abortMessage);

		// Ditch the video mode so the message box appears without problems.
		if (g_WindowMan.GetWindow()) {
			SDL_SetWindowFullscreen(g_WindowMan.GetWindow(), 0);
		}

		// A worker-thread abort still exits here; the record stands in case the process survives it.
		if (!IsOnAppMainThread()) {
			RecordPendingWorkerMessage(WorkerMessageKind::Abort, abortMessage, description, srcLocation);
		}
		if (ShowAbortMessageBox(abortMessage)) {
			// Enable restarting in release builds only.
			// Once this exits the debugger is detached and while there does seem to be a way to programatically re-attach it to the new instance (at least in Windows), it is so incredibly ass and I cannot even begin to can.
			// This will prevent your day from being ruined when your breakpoints don't trigger during a meltdown because you launched a new instance and didn't realize you're not attached to it.
#ifdef RELEASE_BUILD
#ifdef _WIN32
			std::system(std::string(R"(start "" ")" + System::GetThisExePathAndName() + "\"").c_str());
#else
			std::system(std::string("\"" + System::GetThisExePathAndName() + "\"").c_str());
#endif
#endif
		}
	}
	s_CurrentlyAborting = false;
	AbortAction;
}

void RTEError::AssertFunc(const std::string& description, const SourceLocation& srcLocation) {
	if (System::IsInExternalModuleValidationMode()) {
		AbortFunc(description, srcLocation);
	}

	// This typically contains the absolute path to the file on whatever machine this was compiled on, so in that case get only the file name.
	std::filesystem::path filePath = srcLocation.file_name();
	std::string fileName = (filePath.has_root_name() || filePath.has_root_directory()) ? filePath.filename().generic_string() : srcLocation.file_name();

	std::string lineNum = std::to_string(srcLocation.line());
	std::string funcName = srcLocation.function_name();

	g_ConsoleMan.PrintString("ERROR: Assertion in file '" + fileName + "', line " + lineNum + ", in function '" + funcName + "' because: " + description);

	bool storeAssertInfo = false;

	if (!s_IgnoreAllAsserts) {
		std::string assertMessage =
		    "Assertion in file '" + fileName + "', line " + lineNum + ",\nin function '" + funcName + "'\nbecause:\n\n" + description + "\n\n" +
		    "You may choose to ignore this and crash immediately\nor at some unexpected point later on.\n\nProceed at your own risk!";

		if (!IsOnAppMainThread()) {
			RecordPendingWorkerMessage(WorkerMessageKind::Assert, assertMessage, description, srcLocation);
		}
		if (ShowAssertMessageBox(assertMessage)) {
			AbortFunc(description, srcLocation);
		} else {
			storeAssertInfo = true;
		}
	} else {
		if (SDL_getenv("CCCP_HEADLESS") != nullptr) {
			s_AssertFired = true;
		}
		storeAssertInfo = true;
	}

	if (storeAssertInfo) {
		s_LastIgnoredAssertDescription = description;
		s_LastIgnoredAssertLocation = srcLocation;
	}
}

int RTEError::PendingWorkerMessageCount() {
	return (s_PendingWorkerMessage.load(std::memory_order_acquire) ? 1 : 0) + s_PendingWorkerMessageDropped.load(std::memory_order_acquire);
}

void RTEError::DispatchPendingWorkerMessages() {
	if (!IsOnAppMainThread() || !s_PendingWorkerMessage.load(std::memory_order_acquire)) {
		return;
	}
	PendingWorkerMessage record;
	int dropped = 0;
	{
		std::lock_guard<std::mutex> lock(s_PendingWorkerMessageMutex);
		record = std::move(s_PendingWorkerMessageRecord);
		s_PendingWorkerMessageRecord = {};
		dropped = s_PendingWorkerMessageDropped.exchange(0);
		s_PendingWorkerMessage.store(false, std::memory_order_release);
	}
	const char* kindName = record.kind == WorkerMessageKind::Assert ? "Assert" : (record.kind == WorkerMessageKind::Abort ? "Abort" : "Warning");
	const std::string more = dropped > 0 ? " (+" + std::to_string(dropped) + " more worker messages)" : "";
	if (SDL_getenv("CCCP_HEADLESS") != nullptr && !s_ForceAssertDialogPathForTest) {
		// The worker already printed and set AssertFired; the record discharges as one line.
		System::PrintFaultLine("RTE " + std::string(kindName) + " (worker, dispatched): " + record.message + more);
		return;
	}
	switch (record.kind) {
		case WorkerMessageKind::Warning:
			ShowMessageBox(record.message + more);
			break;
		case WorkerMessageKind::Abort:
			AbortFunc(record.description, record.location);
			break;
		default:
			if (ShowAssertMessageBox(record.message + more)) {
				AbortFunc(record.description, record.location);
			}
			break;
	}
}

void RTEError::DumpHardwareInfo() {
	std::string glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
	std::string glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
	std::string glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));

	std::string glExtentions = "";
	GLint numExt = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
	for(GLint i = 0; i < numExt; i++) {
		glExtentions += "\t";
		glExtentions += reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
		glExtentions += "\n";
	}

	std::string hwInfo = "GL Version: " + glVersion + "\n" +
	                     "GL Vendor: " + glVendor + "\n" +
	                     "GL Renderer: " + glRenderer + "\n" +
	                     "Available Extensions: \n" + glExtentions + "\n";

#if defined(_MSC_VER) || defined(__linux__)
	int vendorRegs[4] = {0};
#ifdef _MSC_VER
	__cpuid(vendorRegs, 0);
#else
	__cpuid(0, vendorRegs[0], vendorRegs[1], vendorRegs[2], vendorRegs[3]);
#endif

	std::string cpuVendor(reinterpret_cast<const char*>(&vendorRegs[1]), 4);
	cpuVendor += std::string(reinterpret_cast<const char*>(&vendorRegs[3]), 4);
	cpuVendor += std::string(reinterpret_cast<const char*>(&vendorRegs[2]), 4);

	hwInfo += "CPU Manufacturer ID: " + cpuVendor + "\n";

	std::string cpuModel;
	int modelRegs[12];
#ifdef _MSC_VER
	__cpuid(modelRegs, 0x80000000);
#else
	__cpuid(0x80000000, modelRegs[0], modelRegs[1], modelRegs[2], modelRegs[3]);
#endif
	if (modelRegs[0] >= 0x80000004) {
		for (size_t i = 0; i <= 2; ++i) {
#ifdef _MSC_VER
			__cpuid(&modelRegs[0] + i * 4, i + 0x80000002);
#else
			__cpuid(i + 0x80000002, modelRegs[0 + i * 4], modelRegs[1 + i * 4], modelRegs[2 + i * 4], modelRegs[3 + i * 4]);
#endif
		}
		for (size_t i = 0; i < 12; ++i) {
			cpuModel += std::string(reinterpret_cast<const char*>(&modelRegs[i]), 4);
		}

		hwInfo += "CPU Model: " + cpuModel + "\n";
	}
#elif defined(__APPLE__) && defined(__MACH__)
	char vendor[1024];
	size_t vendorSize = sizeof(vendor);
	int error = sysctlbyname("machdep.cpu.vendor", &vendor, &vendorSize, nullptr, 0);
	if (!error) {
		hwInfo += "CPU Vendor: " + std::string(vendor) + "\n";
	}
	char brand[1024];
	size_t brandSize = sizeof(brand);
	error = sysctlbyname("machdep.cpu.brand_string", &brand, &brandSize, nullptr, 0);
	if (!error) {
		hwInfo += "CPU Model: " + std::string(brand) + "\n";
	}
#endif

	g_ConsoleMan.PrintString(hwInfo);

#ifdef __unix__
	struct utsname unameData;
	if (uname(&unameData) == 0) {
		std::string osInfo = "uname: " + std::string(unameData.sysname) + " " + std::string(unameData.release) + " " + std::string(unameData.version);
		g_ConsoleMan.PrintString(osInfo);
	}
#endif

#ifdef _MSC_VER
	g_ConsoleMan.PrintString("OS: Windows");
#endif

#ifdef __linux__
	// Read distribution info from /etc/os-release
	if (std::filesystem::exists("/etc/os-release")) {
		std::ifstream osReleaseFile("/etc/os-release");
		if (osReleaseFile.is_open()) {
			std::string line;
			while (std::getline(osReleaseFile, line)) {
				if (line.find("PRETTY_NAME") != std::string::npos) {
					g_ConsoleMan.PrintString("OS: " + line.substr(line.find_first_of('"') + 1, line.find_last_of('"') - line.find_first_of('"') - 1));
					break;
				}
			}
			osReleaseFile.close();
		}
	} else {
		g_ConsoleMan.PrintString("OS: Unknown Linux (/etc/os-release not found)");
	}
#endif

#if defined(__APPLE__) && defined(__MACH__)
	char osType[1024];
	size_t osTypeSize = sizeof(osType);
	error = sysctlbyname("kern.ostype", &osType, &osTypeSize, nullptr, 0);
	if (!error) {
		g_ConsoleMan.PrintString("OS Type: " + std::string(osType));
	}

	char osRelease[1024];
	size_t osReleaseSize = sizeof(osRelease);
	error = sysctlbyname("kern.osrelease", &osRelease, &osReleaseSize, nullptr, 0);
	if (!error) {
		g_ConsoleMan.PrintString("OS Release: " + std::string(osRelease));
	}

	char osVersion[1024];
	size_t osVersionSize = sizeof(osVersion);
	error = sysctlbyname("kern.osversion", &osVersion, &osVersionSize, nullptr, 0);
	if (!error) {
		g_ConsoleMan.PrintString("OS Version: " + std::string(osVersion));
	}
#endif
}

bool RTEError::DumpAbortScreen() {
	int success = -1;
	if (glReadPixels != nullptr) {
		int w, h;
		SDL_GetWindowSizeInPixels(g_WindowMan.GetWindow(), &w, &h);
		if (!(w > 0 && h > 0)) {
			return false;
		}
		BITMAP* readBuffer = create_bitmap_ex(24, w, h);
		// Read screen from the front buffer since that is the only framebuffer guaranteed to exist at this point.
		// Read twice because front buffer content is technically undefined, but most drivers still eventually give up the contents correctly.
		glReadBuffer(GL_FRONT);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, readBuffer->line[0]);
		glFinish();
		glReadBuffer(GL_BACK);
		glReadBuffer(GL_FRONT);
		glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, readBuffer->line[0]);
		glFinish();

		BITMAP* flipBuffer = create_bitmap_ex(24, w, h);
		draw_sprite_v_flip(flipBuffer, readBuffer, 0, 0);

		success = save_png("AbortScreen.png", flipBuffer, nullptr);
	}
	return success == 0;
}

bool RTEError::DumpAbortSave() {
	bool success = false;
	if (g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->CanBeUserSaved()) {
		success = g_ActivityMan.SaveCurrentGame("AbortSave") && g_ActivityMan.WaitForSaveGameTask();
	}
	return success;
}

void RTEError::FormatFunctionSignature(std::string& symbolName) {
	// TODO: Expand this with more dumb signatures, or make something that makes more sense.
	static const std::array<std::pair<std::regex, std::string>, 3> stlSigs{
	    {{std::regex("( >)"), ">"},
	     {std::regex("(std::basic_string<char,std::char_traits<char>,std::allocator<char>>)"), "std::string"},
	     {std::regex("(class ?std::basic_string<char,struct ?std::char_traits<char>,class ?std::allocator<char>>)"), "std::string"}}};
	for (const auto& [fullSig, simpleSig]: stlSigs) {
		symbolName = std::regex_replace(symbolName, fullSig, simpleSig);
	}
	for (size_t pos = 0;;) {
		pos += 100;
		if (pos < symbolName.size()) {
			if (size_t lastCommaPos = symbolName.find_last_of(',', pos); lastCommaPos != std::string::npos) {
				symbolName.insert(lastCommaPos + 1, "\n");
			}
		} else {
			break;
		}
	}
}

bool RTEError::RunAssertPolicySelfTest() {
	// A static hook selects the headed dialog/Ignore branch without opening SDL.
	if (!ConsoleMan::IsConstructed()) {
		ConsoleMan::Construct();
	}
#ifdef _WIN32
	s_AppMainThreadId.store(GetCurrentThreadId());
#else
	s_AppMainThreadId.store(std::this_thread::get_id());
#endif
	s_AssertFired = false;
	s_ForceAssertDialogPathForTest = true;
	AssertFunc("dialog-path probe", RTECurrentSourceLocation);
	s_ForceAssertDialogPathForTest = false;
	const bool dialogLeftUnfired = !s_AssertFired;
	std::cout << "[rteerror-selftest] " << (dialogLeftUnfired ? "PASS" : "FAIL")
	          << " dialog_path_leaves_assert_fired_false AssertFired=" << (s_AssertFired ? "true" : "false") << std::endl;

	s_AssertFired = false;
	SDL_setenv_unsafe("CCCP_HEADLESS", "1", 1);
	std::thread worker([] {
		AssertFunc("worker-thread probe", RTECurrentSourceLocation);
	});
	worker.join();
	const bool workerFired = s_AssertFired;
	std::cout << "[rteerror-selftest] " << (workerFired ? "PASS" : "FAIL")
	          << " worker_thread_headless_sets_assert_fired AssertFired=" << (workerFired ? "true" : "false") << std::endl;

	s_AssertFired = false;
	s_IgnoreAllAsserts = true;
	AssertFunc("ignore-all probe", RTECurrentSourceLocation);
	s_IgnoreAllAsserts = false;
	const bool ignoreAllFired = s_AssertFired;
	std::cout << "[rteerror-selftest] " << (ignoreAllFired ? "PASS" : "FAIL")
	          << " ignore_all_headless_sets_assert_fired AssertFired=" << (ignoreAllFired ? "true" : "false") << std::endl;

	// A worker-thread assert is recorded and the main thread's next dispatch shows its dialog; the
	// forced path counts the box without opening one. The forced drain clears what the earlier
	// worker probe left pending.
	s_AssertFired = false;
	s_ForceAssertDialogPathForTest = true;
	DispatchPendingWorkerMessages();
	ResetAssertMessageBoxCallCount();
	std::thread dispatchWorker([] {
		AssertFunc("worker dispatch probe", RTECurrentSourceLocation);
	});
	dispatchWorker.join();
	const int pendingBefore = PendingWorkerMessageCount();
	const int boxesBefore = AssertMessageBoxCallCount();
	DispatchPendingWorkerMessages();
	const int pendingAfter = PendingWorkerMessageCount();
	const int boxesAfter = AssertMessageBoxCallCount();
	s_ForceAssertDialogPathForTest = false;
	const bool dispatched = pendingBefore == 1 && boxesBefore == 0 && pendingAfter == 0 && boxesAfter == 1;
	std::cout << "[rteerror-selftest] " << (dispatched ? "PASS" : "FAIL")
	          << " worker_assert_reaches_dialog pending=" << pendingBefore << " boxes=" << boxesBefore
	          << " after_dispatch pending=" << pendingAfter << " boxes=" << boxesAfter << std::endl;

	const bool passed = dialogLeftUnfired && workerFired && ignoreAllFired && dispatched;
	std::cout << "[rteerror-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
	return passed;
}
