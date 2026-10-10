/*          ______   ______   ______  ______  ______  __  __       ______   ______   __    __   __    __   ______   __   __   _____
           /\  ___\ /\  __ \ /\  == \/\__  _\/\  ___\/\_\_\_\     /\  ___\ /\  __ \ /\ "-./  \ /\ "-./  \ /\  __ \ /\ "-.\ \ /\  __-.
           \ \ \____\ \ \/\ \\ \  __<\/_/\ \/\ \  __\\/_/\_\/_    \ \ \____\ \ \/\ \\ \ \-./\ \\ \ \-./\ \\ \  __ \\ \ \-.  \\ \ \/\ \
            \ \_____\\ \_____\\ \_\ \_\ \ \_\ \ \_____\/\_\/\_\    \ \_____\\ \_____\\ \_\ \ \_\\ \_\ \ \_\\ \_\ \_\\ \_\\"\_\\ \____-
             \/_____/ \/_____/ \/_/ /_/  \/_/  \/_____/\/_/\/_/     \/_____/ \/_____/ \/_/  \/_/ \/_/  \/_/ \/_/\/_/ \/_/ \/_/ \/____/
   ______   ______   __    __   __    __   __  __   __   __   __   ______  __  __       ______  ______   ______      __   ______   ______   ______
  /\  ___\ /\  __ \ /\ "-./  \ /\ "-./  \ /\ \/\ \ /\ "-.\ \ /\ \ /\__  _\/\ \_\ \     /\  == \/\  == \ /\  __ \    /\ \ /\  ___\ /\  ___\ /\__  _\
  \ \ \____\ \ \/\ \\ \ \-./\ \\ \ \-./\ \\ \ \_\ \\ \ \-.  \\ \ \\/_/\ \/\ \____ \    \ \  _-/\ \  __< \ \ \/\ \  _\_\ \\ \  __\ \ \ \____\/_/\ \/
   \ \_____\\ \_____\\ \_\ \ \_\\ \_\ \ \_\\ \_____\\ \_\\"\_\\ \_\  \ \_\ \/\_____\    \ \_\   \ \_\ \_\\ \_____\/\_____\\ \_____\\ \_____\  \ \_\
    \/_____/ \/_____/ \/_/  \/_/ \/_/  \/_/ \/_____/ \/_/ \/_/ \/_/   \/_/  \/_____/     \/_/    \/_/ /_/ \/_____/\/_____/ \/_____/ \/_____/   \/_/

/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\/////\\\\\*/

/// <summary>
/// Main driver implementation of the Retro Terrain Engine.
/// Data Realms, LLC - http://www.datarealms.com
/// Cortex Command Community Project - https://github.com/cortex-command-community
/// Cortex Command Community Project Discord - https://discord.gg/TSU6StNQUG
/// </summary>

#include "allegro.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include "GUI.h"
#include "GUICheckbox.h"
#include "GUIInputWrapper.h"
#include "CaptureSentinel.h"
#include "FloatText.h"
#include "MainMenuGUI.h"
#include "NetModerationGUI.h"
#include "NetChatPresentation.h"
#include "NetModerationGUIProbe.h"
#include "Icon.h"
#include "AllegroScreen.h"
#include "AllegroBitmap.h"

#include "MainMenuGUI.h"
#include "ScenarioGUI.h"
#include "PauseMenuGUI.h"
#include "TitleScreen.h"
#include "LoadingScreen.h"

#include "MenuMan.h"
#include "SaveLoadMenuGUI.h"
#include "ConsoleMan.h"
#include "Constants.h"
#include "SettingsMan.h"
#include "TimerMan.h"
#include "PresetMan.h"
#include "UInputMan.h"
#include "PerformanceMan.h"
#include "FrameMan.h"
#include "PostProcessMan.h"
#include "SceneMan.h"
#include "SLTerrain.h"
#include "MetaMan.h"
#include "WindowMan.h"
#include "GLResourceMan.h"
#include "CameraMan.h"
#include "ActivityMan.h"
#include "AutosaveStore.h"
#include "Actor.h"
#include "AHuman.h"
#include "Attachable.h"
#include "GameActivity.h"
#include "NetActivitySetup.h"
#include "MovableObject.h"
#include "MOPixel.h"
#include "RTETools.h"
#include "FloatingPointEnvironment.h"
#include "RotatePrimitiveSelfTest.h"
#include "FrameRecorder.h"
#include "ScenarioGUI.h"
#include "FloatTextSelfTest.h"
#include "CheckpointImage.h"
#include "PrimitiveMan.h"
#include "ThreadMan.h"
#include "LuaMan.h"
#include "MusicMan.h"
#include "Atom.h"
#include "AudioMan.h"
#include "SoundContainer.h"
#include "SoundSimulation.h"
#include "AudioCheckpoint.h"
#include "System.h"
#include "RTEError.h"
#include "DataModule.h"
#include "MenuAutomation.h"
#include "GUIDrawRecord.h"
#ifdef __APPLE__
#include "AppleApplication.h"
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#endif

#include "Controller.h"
#include "ControllerFrame.h"
#include "GnsP2PSelfTest.h"
#include "GnsTransport.h"
#include "NetAdmissionSelfTest.h"
#include "NetAuthSelfTest.h"
#include "NetDirectoryClient.h"
#include "NetDirectoryCodec.h"
#include "NetDirectorySignalChannel.h"
#include "NetHttpClient.h"
#include "NetIdentity.h"
#include "NetIdentitySelfTest.h"
#include "NetLanDiscovery.h"
#include "NetLockstep.h"
#include "NetLobbyProtocol.h"
#include "NetMatchReplay.h"
#include "TelemetryBundle.h"
#include "NetLockstepSelfTest.h"
#include "NetSessionPlaneSelfTest.h"
#include "NetMatchRunner.h"
#include "NetMatchService.h"
#include "NetMatchSelfTest.h"
#include "NetPortMap.h"
#include "NetProtocolSelfTest.h"
#include "NetReconnectSelfTest.h"
#include "NetReconnectSessionSelfTest.h"
#include "NetReconnectUx.h"
#include "NetSession.h"
#include "NetSessionSelfTest.h"
#include "NetRejoinMatrixSelfTest.h"
#include "NetSeatRoster.h"
#include "NetWorldJoinSelfTest.h"
#ifdef CCCP_WITH_GNS
#include <steam/isteamnetworkingutils.h>
#endif
#include "SimChecksum.h"
#include "NetA7Journal.h"
#include "ScenarioRunner.h"
#include "NetActorOwnership.h"
#include "InputScript.h"
#include "AIWriteScript.h"
#include "FaultInjection.h"
#include "LocalPrediction.h"
#include "SimDumpTape.h"
#include "LocalPredictionHudSelfTest.h"
#include "OwnedMovableObjects.h"
#include "PreviewEventLedger.h"
#include "PreviewScriptSelfTest.h"
#include "LuaBindingExhaustiveSelfTest.h"
#include "TerrainLayerSnapshot.h"
#include "DeterminismCheck.h"
#include "MetricsCollector.h"
#include "AsyncLineWriter.h"
#include "StallStackSampler.h"
#include "HarnessCost.h"
#include "ContractAudit.h"

#include "RenderTarget.h"
#include "tracy/Tracy.hpp"

#include "imgui_impl_sdl3.h"

#include "nlohmann/json.hpp"

#ifdef _WIN32
#include "windows.h"
#include <crtdbg.h>
#include <processsnapshot.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <bit>
#include <cfloat>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <fstream>
#include <filesystem>
#include <format>
#include <iostream>
#include <iomanip>
#include <random>
#include <condition_variable>
#include <future>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include <list>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
FILE __iob_func[3] = {*stdin, *stdout, *stderr};
}

namespace RTE {
	bool RunModApiShimsSelfTest();
	bool ApplyCrossTransportFault(int lagMs, float lossPercent, float jitterMs, uint64_t durationMs);
	bool RunCrossRosterSelfTest(std::string* error);
	bool RunCrossEndSignalSelfTest(std::string* error);
	bool RunCrossLobbyResourcesSelfTest(std::string* error);
	bool RunCrossExecutionPhaseSelfTest(std::string* error);
	bool RunCrossAuthorityRecordSelfTest(std::string* error);
	bool RunCrossHistoryRecordSelfTest(std::string* error);
	bool RunCrossInPlaceHistorySelfTest(std::string* error);
	bool RunCrossReadyRevisionSelfTest(std::string* error);
}

using namespace RTE;

// Per-tick state hashing — armed by the -tick-hashes CLI flag, off in normal play.
static bool s_recordTickHashes = false;

/// Test lever CCCP_TEST_MINIMIZE_TICKS=<from>:<to>[:fullscreen]: the window is minimized at the first tick and restored at the second.
/// With ':fullscreen' it is shown in the game's own fullscreen from startup, before any round, and its state is written every 60 ticks
/// from 60 before the minimize to 60 after the restore, so a run proves the whole minimized span was a fullscreen one.
struct MinimizeLever { uint64_t from = 0, to = 0; bool fullscreen = false; };

static const MinimizeLever& TestMinimizeLever() {
	static const MinimizeLever s_lever = [] {
		unsigned long long from = 0, to = 0;
		char form[16] = {};
		const char* text = std::getenv("CCCP_TEST_MINIMIZE_TICKS");
		const int fields = text ? std::sscanf(text, "%llu:%llu:%15s", &from, &to, form) : 0;
		if (fields < 2 || to <= from) return MinimizeLever{};
		return MinimizeLever{from, to, fields == 3 && std::string(form) == "fullscreen"};
	}();
	return s_lever;
}

static void WriteTestWindowState(uint64_t tick) {
	const SDL_WindowFlags flags = SDL_GetWindowFlags(g_WindowMan.GetWindow());
	const MinimizeLever& lever = TestMinimizeLever();
	const nlohmann::json state{{"tick", tick}, {"process", System::GetProcessID()}, {"round", ScenarioRunner::GetLockstepRoundId()},
	                           {"fullscreen", g_WindowMan.IsFullscreen() && (flags & SDL_WINDOW_FULLSCREEN) != 0}, {"minimized", (flags & SDL_WINDOW_MINIMIZED) != 0},
	                           {"hidden", (flags & SDL_WINDOW_HIDDEN) != 0}, {"input_focus", (flags & SDL_WINDOW_INPUT_FOCUS) != 0},
	                           {"minimize_from", lever.from}, {"minimize_to", lever.to}};
	System::PrintDiagnosticLine("[window-state] " + state.dump());
}
static std::string s_netLiveTickHashPath;

// Written every tick, so the disk never holds the simulation.
static AsyncLineWriter s_netLiveTickHashes;

static AsyncLineWriter& LiveTickHashStream() {
	static bool attempted = false;
	if (!attempted) {
		attempted = true;
		(void)s_netLiveTickHashes.Open(s_netLiveTickHashPath);
	}
	return s_netLiveTickHashes;
}

static nlohmann::json s_crossContext;

// Ticks a held seat ran off the round leave both hash records; the live stream names the round they belong to and the process that ran them.
static void RetractAbandonedTickHashes() {
	uint64_t round = 0;
	uint64_t ranThrough = 0;
	const uint64_t abandoned = ScenarioRunner::TakeAbandonedTicksFrom(round, ranThrough);
	if (abandoned == 0) {
		return;
	}
	// Ticks run past the hold were live on this seat's own screen until it heard of the hold; none run retracts nothing it showed.
	const bool ran = ranThrough >= abandoned;
	if (ran) g_MetricsCollector.RetractTickHashesFrom(abandoned);
	if (!s_netLiveTickHashPath.empty()) {
		nlohmann::json receipt{{"abandon_from", abandoned}, {"round", round}, {"ran_through", ran ? ranThrough : 0}, {"private", !ran}, {"player_visible", ran},
		                       {"process", System::GetProcessID()}};
		for (const char* key: {"instance", "execution", "incarnation"})
			if (s_crossContext.is_object() && s_crossContext.contains(key)) receipt[key] = s_crossContext[key];
		LiveTickHashStream().Write(receipt.dump());
	}
}

static uint64_t s_memoryCensusTicks = 0; //!< Every this many ticks one line names what each record holds; 0 = never.
static bool s_memoryCensusHistogram = false; //!< The census also sums the process heaps, walks the default one and names the block sizes holding the most.
static size_t s_memoryCensusProbeSize = 0; //!< Blocks of this size have their first bytes printed, so a leaked object can be named.

// Private bytes, and what the process heaps hold allocated and committed, for the memory census; costs gets each part's microseconds.
static std::string ProcessHeapCensus([[maybe_unused]] std::string& costs) {
#ifdef _WIN32
	auto lapStart = std::chrono::steady_clock::now();
	const auto lap = [&costs, &lapStart](const char* name) {
		const auto now = std::chrono::steady_clock::now();
		costs += std::string(",") + name + ":" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now - lapStart).count());
		lapStart = now;
	};
	PROCESS_MEMORY_COUNTERS_EX counters{};
	K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
	lap("heap_counters");
	// Summing a heap holds its lock for as long as the heap is large, and every thread that allocates waits on it, the simulation's
	// included: the heaps are summed only with the census's heap walk.
	std::string heapFigures;
	if (s_memoryCensusHistogram) {
		HANDLE heaps[256];
		const DWORD count = std::min<DWORD>(GetProcessHeaps(256, heaps), 256);
		unsigned long long allocated = 0, committed = 0;
		for (DWORD index = 0; index < count; ++index) {
			HEAP_SUMMARY summary{};
			summary.cb = sizeof(summary);
			if (HeapSummary(heaps[index], 0, &summary)) {
				allocated += summary.cbAllocated;
				committed += summary.cbCommitted;
			}
		}
		heapFigures = std::format(" heaps={} heap_allocated_mb={} heap_committed_mb={}", count, allocated >> 20, committed >> 20);
		lap("heap_summary");
	}
	DWORD handles = 0;
	GetProcessHandleCount(GetCurrentProcess(), &handles);
	// This process's threads alone: a thread snapshot of the whole system costs tens of milliseconds on a busy desktop.
	size_t threads = 0;
	if (HPSS snapshot = nullptr; PssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_THREADS, 0, &snapshot) == ERROR_SUCCESS) {
		PSS_THREAD_INFORMATION captured{};
		if (PssQuerySnapshot(snapshot, PSS_QUERY_THREAD_INFORMATION, &captured, sizeof(captured)) == ERROR_SUCCESS) threads = captured.ThreadsCaptured;
		PssFreeSnapshot(GetCurrentProcess(), snapshot);
	}
	lap("heap_threads");
	std::string histogram;
	if (s_memoryCensusHistogram) {
		// Busy blocks by exact size below 64 KB, larger ones by power of two; the walk allocates nothing while the heap is locked.
		static std::vector<uint64_t> small(65536), large(64);
		std::fill(small.begin(), small.end(), 0);
		std::fill(large.begin(), large.end(), 0);
		HANDLE heap = GetProcessHeap();
		PROCESS_HEAP_ENTRY walk{};
		std::array<std::array<uint8_t, 64>, 4> probes{};
		size_t probed = 0;
		HeapLock(heap);
		while (HeapWalk(heap, &walk)) {
			if (!(walk.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
			if (walk.cbData < small.size()) ++small[walk.cbData]; else ++large[std::bit_width(static_cast<uint64_t>(walk.cbData))];
			if (s_memoryCensusProbeSize != 0 && walk.cbData == s_memoryCensusProbeSize && walk.cbData >= 64 && small[walk.cbData] % 997 == 1) {
				std::memcpy(probes[probed % probes.size()].data(), walk.lpData, 64);
				++probed;
			}
		}
		HeapUnlock(heap);
		const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		for (size_t index = 0; index < std::min(probed, probes.size()); ++index) {
			std::string text = " probe" + std::to_string(index) + "=";
			for (size_t word = 0; word < 64; word += 8) {
				uint64_t value = 0;
				std::memcpy(&value, probes[index].data() + word, 8);
				text += value >= imageBase && value < imageBase + 0x3000000 ? std::format("exe+0x{:X}|", value - imageBase) : std::format("{:X}|", value);
			}
			for (uint8_t byte: probes[index]) text += byte >= 0x21 && byte < 0x7F ? static_cast<char>(byte) : '.';
			histogram += text;
		}
		std::vector<std::pair<uint64_t, std::string>> top;
		for (size_t size = 0; size < small.size(); ++size) if (small[size]) top.emplace_back(small[size] * size, std::to_string(size) + ":" + std::to_string(small[size]));
		for (size_t bit = 0; bit < large.size(); ++bit) if (large[bit]) top.emplace_back(large[bit] << bit, "2^" + std::to_string(bit) + ":" + std::to_string(large[bit]));
		std::partial_sort(top.begin(), top.begin() + std::min<size_t>(top.size(), 12), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		// The sizes whose busy blocks grew most since the last census line, by the bytes they added.
		static std::vector<uint64_t> lastSmall(65536), lastLarge(64);
		std::vector<std::pair<int64_t, std::string>> grew;
		for (size_t size = 0; size < small.size(); ++size) {
			const int64_t added = static_cast<int64_t>(small[size]) - static_cast<int64_t>(lastSmall[size]);
			if (added > 0) grew.emplace_back(added * static_cast<int64_t>(size), std::to_string(size) + ":+" + std::to_string(added));
		}
		for (size_t bit = 0; bit < large.size(); ++bit) {
			const int64_t added = static_cast<int64_t>(large[bit]) - static_cast<int64_t>(lastLarge[bit]);
			if (added > 0) grew.emplace_back(added << bit, "2^" + std::to_string(bit) + ":+" + std::to_string(added));
		}
		lastSmall = small;
		lastLarge = large;
		std::partial_sort(grew.begin(), grew.begin() + std::min<size_t>(grew.size(), 10), grew.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		histogram += " heap_grew=";
		for (size_t index = 0; index < std::min<size_t>(grew.size(), 10); ++index) histogram += (index ? "," : "") + grew[index].second;
		histogram += " heap_top=";
		for (size_t index = 0; index < std::min<size_t>(top.size(), 12); ++index) histogram += (index ? "," : "") + top[index].second;
	}
	return std::format(" private_mb={}{} threads={} handles={}{}", counters.PrivateUsage >> 20, heapFigures, threads, handles, histogram);
#elif defined(__APPLE__)
	mach_task_basic_info_data_t info{};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return {};
	return std::format(" resident_mb={}", info.resident_size >> 20);
#elif defined(__linux__)
	unsigned long long pages = 0, resident = 0;
	std::ifstream statm("/proc/self/statm");
	if (!(statm >> pages >> resident)) return {};
	std::string heap;
#if defined(__GLIBC__)
	// What the allocator hands the program and what it keeps free, so a resident climb names which of the two it is.
	const struct mallinfo2 info = mallinfo2();
	heap = std::format(" heap_in_use_mb={} heap_free_mb={} heap_mmap_mb={}", (info.uordblks + info.hblkhd) >> 20, info.fordblks >> 20, info.hblkhd >> 20);
#endif
	return std::format(" resident_mb={}{}", (resident * static_cast<unsigned long long>(sysconf(_SC_PAGESIZE))) >> 20, heap);
#else
	return {};
#endif
}

// The census's clock: the process's start, so its instants line up across rounds that restart their ticks.
static const std::chrono::steady_clock::time_point s_CensusProcessStart = std::chrono::steady_clock::now();

// The memory census's process figures are summed here, off the simulation thread: on a large heap they cost hundreds of milliseconds.
class CensusWorker {
public:
	static CensusWorker& Get() {
		// Never destroyed: a job still running at exit is left to finish, never joined past the exit's wait.
		static CensusWorker* worker = [] {
			auto* created = new CensusWorker();
			std::atexit([] { Get().Stop(); });
			return created;
		}();
		return *worker;
	}

	void Post(std::function<void()> job) {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Jobs.push_back(std::move(job));
		}
		m_Wake.notify_one();
	}

	void Stop() {
		std::unique_lock<std::mutex> lock(m_Mutex);
		m_Stopping = true;
		m_Wake.notify_one();
		const bool finished = m_Done.wait_for(lock, std::chrono::seconds(2), [this] { return m_Finished; });
		lock.unlock();
		if (finished) m_Thread.join();
		else m_Thread.detach();
	}

private:
	CensusWorker() :
	    m_Thread(FloatingPointEnvironment::StartThread([this] { Run(); })) {}

	// Every posted line is printed before the worker stops.
	void Run() {
		for (;;) {
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(m_Mutex);
				m_Wake.wait(lock, [this] { return m_Stopping || !m_Jobs.empty(); });
				if (m_Jobs.empty()) {
					m_Finished = true;
					m_Done.notify_all();
					return;
				}
				job = std::move(m_Jobs.front());
				m_Jobs.pop_front();
			}
			job();
		}
	}

	std::mutex m_Mutex;
	std::condition_variable m_Wake;
	std::deque<std::function<void()>> m_Jobs;
	std::condition_variable m_Done;
	bool m_Stopping = false;
	bool m_Finished = false;
	std::thread m_Thread;
};
// Test lever: every N committed lockstep ticks each peer hashes its whole capture; 0 is off.
static uint32_t s_netFullStateEvery = 0;
static std::string s_netFullStateDump;
static nlohmann::json s_crossSchedule = nlohmann::json::array();
static uint64_t s_crossBudget = 0;
static uint64_t s_crossOwnIdentity = 0; //!< This peer's stable identity as its seat's owner, last seen live.
static std::map<uint64_t, uint64_t> s_crossLastCommitted;
static std::map<uint64_t, uint64_t> s_crossFirstGameplayTick;
static std::set<std::string> s_crossFired;
static unsigned s_crossRematches = 0;
static bool s_crossTicketRejoin = false;
static bool s_crossLeaveRequested = false;
static nlohmann::json s_crossHostOptions = nlohmann::json::array();

static std::string CrossEnvironment(const char* name, const char* fallback = "") {
	const char* value = std::getenv(name);
	return value ? value : fallback;
}

static const char* CrossTickPhase(bool catchup, uint64_t tick, uint64_t previousCommitted, [[maybe_unused]] uint64_t execution) {
	return catchup ? "catchup" : tick <= previousCommitted ? "reexecution" : "live";
}

static const nlohmann::json* CrossMatchingLockstep(const nlohmann::json& report, uint64_t session, uint8_t host) {
	if (!report.is_object() || !report.contains("runner") || !report["runner"].is_object() || !report["runner"].contains("lockstep")) return nullptr;
	const auto& value = report["runner"]["lockstep"];
	if (!value.is_object() || !value.contains("session_id") || !value.contains("host_peer_id") || value["session_id"] != session || value["host_peer_id"] != host) return nullptr;
	return &value;
}

static nlohmann::json CrossAuthorityFromReport(const nlohmann::json& report, uint64_t session, uint8_t host) {
	const auto* value = CrossMatchingLockstep(report, session, host);
	return value && value->contains("migration_phase") && (*value)["migration_phase"] == 0 && value->contains("migration_generation") &&
	    (*value)["migration_generation"].is_number_unsigned() ? (*value)["migration_generation"] : nlohmann::json(nullptr);
}

// Whether this tick leaves the initial history: a catch-up onto another world, a round begun past frame 1 other than
// the return of an in-place catch-up, a ticket rejoin or a re-executed tick.
static bool CrossHistoryRestored(bool catchup, bool inPlace, uint64_t configuredStart, const std::set<uint64_t>& inPlaceReturns, bool ticketRejoin, bool reexecuted) {
	return (catchup && !inPlace) || (configuredStart > 1 && !inPlaceReturns.contains(configuredStart)) || ticketRejoin || reexecuted;
}

// A relaunched process rejoins one round, the first it sees; every later round it starts from the round's first frame like the others.
static bool CrossTicketRejoinRound(bool ticketRejoin, uint64_t round, uint64_t& firstRound) {
	if (firstRound == 0) firstRound = round;
	return ticketRejoin && round == firstRound;
}

static nlohmann::json CrossHistoryBranch(uint64_t configuredStart, bool restored, [[maybe_unused]] uint64_t nextFrameCursor, const std::set<uint64_t>& inPlaceReturns = {}) {
	const bool started = configuredStart == 1 || (configuredStart > 1 && inPlaceReturns.contains(configuredStart));
	return started && !restored ? nlohmann::json("initial") : nlohmann::json(nullptr);
}

// A restored or re-run history is the round's own: a catch-up replay or a re-execution runs its committed ticks, and a restored
// process runs them live from the frame it lands on, so each such row carries the round's branch.
static nlohmann::json CrossLandedBranch(const nlohmann::json& branch, bool catchup, std::string_view phase, uint64_t configuredStart, uint64_t tick) {
	if (!branch.is_null()) return branch;
	const bool replayed = catchup || phase == "catchup" || phase == "reexecution";
	const bool landed = phase == "live" && configuredStart != 0 && tick >= configuredStart;
	return replayed || landed ? nlohmann::json("initial") : branch;
}

static uint64_t CrossRecordRound(uint64_t nativeRound, [[maybe_unused]] uint64_t sourceRound) { return nativeRound; }

static bool CrossReadyRevision(uint64_t attemptWindow, uint64_t& previous) {
	if (attemptWindow == previous) return false;
	previous = attemptWindow;
	return true;
}

bool RTE::RunCrossReadyRevisionSelfTest(std::string* error) {
	uint64_t last = UINT64_MAX;
	if (!CrossReadyRevision(1, last) || CrossReadyRevision(1, last) || !CrossReadyRevision(4, last) || last != 4) {
		*error = "ready/start intent is not re-armed once per retry window"; return false;
	}
	std::cout << "[net-match-selftest] PASS cross_ready_start_retries_across_configuration_changes" << std::endl;
	return true;
}

static void CrossReadyForCurrentConfig(uint64_t& previous) {
	if (s_crossHostOptions.empty() || g_NetMatchService.GetState() != NetMatchServiceState::Starting) return;
	const auto attemptWindow = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 100;
	if (!CrossReadyRevision(attemptWindow, previous)) return;
	g_NetMatchService.SetReady();
	if (g_NetMatchService.IsHost()) g_NetMatchService.RequestStart();
	System::PrintDiagnosticLine("[cross-ready] retry_window=" + std::to_string(attemptWindow) + " host=" + std::to_string(g_NetMatchService.IsHost()));
}

bool RTE::RunCrossHistoryRecordSelfTest(std::string* error) {
	const auto roundKey = std::getenv("CC_TEST_CROSS_MATCH_IDENTITY_RED") ? +[](uint64_t, uint64_t sourceRound) { return sourceRound; } : &CrossRecordRound;
	std::map<uint64_t, uint64_t> last;
	uint64_t budget = 0;
	for (const auto [round, tick]: {std::pair<uint64_t,uint64_t>{101,1}, {101,2}, {102,1}, {102,2}}) {
		auto& previous = last[roundKey(round, 1)];
		if (tick > previous) { budget += tick - previous; previous = tick; }
	}
	if (budget != 4 || roundKey(101, 1) == roundKey(102, 1)) { *error = "distinct rematches alias their checkpoint source round"; return false; }
	if (CrossHistoryBranch(1, false, 601) != "initial" || !CrossHistoryBranch(600, false, 601).is_null() ||
	    !CrossHistoryBranch(1, true, 601).is_null() || !CrossHistoryBranch(0, false, 601).is_null()) {
		*error = "a next-frame cursor is mistaken for a checkpoint, or unknown restoration is mapped as initial"; return false;
	}
	// l4p-38: a restored history's catch-up rows and its live rows after the landing carried no branch, so none of them was compared.
	if (CrossLandedBranch(nullptr, false, "live", 1663, 1700) != "initial" || CrossLandedBranch(nullptr, true, "catchup", 0, 400) != "initial" ||
	    CrossLandedBranch(nullptr, false, "reexecution", 357, 400) != "initial") {
		*error = "a row a restored or re-run history wrote carries no branch"; return false;
	}
	if (!CrossLandedBranch(nullptr, false, "live", 1663, 1600).is_null() || !CrossLandedBranch(nullptr, false, "live", 0, 400).is_null()) {
		*error = "a live row before its process's start took the round's branch"; return false;
	}
	std::cout << "[net-match-selftest] PASS initial_history_uses_configured_start_not_the_next_frame_cursor" << std::endl;
	return true;
}

bool RTE::RunCrossInPlaceHistorySelfTest(std::string* error) {
	// A held seat that catches up in place replays its own world, so it and its return stay on the round's history.
	const std::set<uint64_t> returns{671, 884};
	if (CrossHistoryRestored(true, true, 1, returns, false, false) || CrossHistoryRestored(true, true, 671, returns, false, false) ||
	    CrossHistoryRestored(false, false, 671, returns, false, false) || CrossHistoryBranch(671, false, 672, returns) != "initial" ||
	    CrossHistoryBranch(1, false, 601, returns) != "initial") {
		*error = "an in-place catch-up or its return is recorded as a restored history"; return false;
	}
	if (!CrossHistoryRestored(true, false, 1, returns, false, false) || !CrossHistoryRestored(false, false, 700, returns, false, false) ||
	    !CrossHistoryRestored(false, false, 1, returns, true, false) || !CrossHistoryRestored(false, false, 1, returns, false, true) ||
	    !CrossHistoryBranch(700, false, 701, returns).is_null() || !CrossHistoryBranch(671, true, 672, returns).is_null() ||
	    !CrossHistoryBranch(671, false, 672, {}).is_null()) {
		*error = "an image catch-up, a late start, a ticket rejoin or a re-executed tick keeps the initial history"; return false;
	}
	// l4p-31: the second machine's relaunch rejoined round 2 by ticket, then played rounds 3-6 from their first frames; all of them were recorded as restored.
	uint64_t firstRound = 0;
	const bool rejoined = CrossTicketRejoinRound(true, 1542469911966388394ull, firstRound), later = CrossTicketRejoinRound(true, 12545044123032358889ull, firstRound);
	if (!rejoined || later || CrossTicketRejoinRound(false, 1542469911966388394ull, firstRound)) {
		*error = std::string("a relaunched process's ticket rejoin marks ") + (later ? "a later round it played from its start" : "the wrong round") + " as restored";
		return false;
	}
	std::cout << "[net-match-selftest] PASS in_place_catch_up_keeps_the_initial_history" << std::endl;
	return true;
}

bool RTE::RunCrossAuthorityRecordSelfTest(std::string* error) {
	const nlohmann::json source{{"runner", {{"lockstep", {{"session_id", uint64_t{81}}, {"host_peer_id", 2}, {"migration_generation", uint64_t{7}}, {"migration_phase", 0}}}}}};
	if (CrossAuthorityFromReport(source, 81, 2) != 7 || !CrossAuthorityFromReport(source, 82, 2).is_null() ||
	    !CrossAuthorityFromReport(source, 81, 3).is_null() || !CrossAuthorityFromReport(nlohmann::json::object(), 81, 2).is_null()) {
		*error = "authority record is absent or accepts another session/host"; return false;
	}
	auto election = source; election["runner"]["lockstep"]["migration_phase"] = 1;
	if (!CrossAuthorityFromReport(election, 81, 2).is_null()) { *error = "an election candidate is reported as settled authority"; return false; }
	std::cout << "[net-match-selftest] PASS cross_authority_record_is_bound_to_its_session_and_host" << std::endl;
	return true;
}

bool RTE::RunCrossExecutionPhaseSelfTest(std::string* error) {
	if (std::string(CrossTickPhase(true, 99, 100, 1)) != "catchup" || std::string(CrossTickPhase(false, 99, 100, 1)) != "reexecution" ||
	    std::string(CrossTickPhase(false, 101, 100, 1)) != "live" || std::string(CrossTickPhase(false, 1, 0, 1)) != "live") {
		*error = "new committed ticks or a new round remain labelled as old replay work"; return false;
	}
	std::cout << "[net-match-selftest] PASS cross_phase_returns_to_live_after_reexecution_or_new_round" << std::endl;
	return true;
}

static nlohmann::json s_crossRecoveryStarts = nlohmann::json::object();
static nlohmann::json s_crossRecoveryCases = nlohmann::json::array();
static nlohmann::json s_crossEndCases = nlohmann::json::array();
static std::set<std::string> s_crossRecoveryDone;
static std::map<std::string, std::string> s_crossRecoveryPhase;
static std::map<uint64_t, uint64_t> s_crossRestoredInputThrough;

static void CrossRememberRestoredInput() {
	if (!g_MetricsCollector.EventsEnabled() || !ScenarioRunner::WorldCatchUpActive()) return;
	// ReleaseWorldCatchUp clears the public cursor; retain its observed bound for
	// this round so restored future inputs cannot be called newly queued input.
	auto& through = s_crossRestoredInputThrough[ScenarioRunner::GetLockstepRoundId()];
	through = std::max(through, ScenarioRunner::WorldCatchUpPriorInputThrough());
}

static bool EnsureCrossEventsOpen() {
	static const bool armed = !CrossEnvironment("CC_TEST_CROSS_RECORDS").empty();
	if (!armed) return false;
	static bool opened = false;
	if (!opened) {
		opened = true;
		s_crossBudget = std::stoull(CrossEnvironment("CC_TEST_CROSS_BUDGET_BASE", "0"));
		const auto eventBudget = static_cast<size_t>(std::stoull(CrossEnvironment("CC_TEST_CROSS_EVENT_RAW_LIMIT", "268435456")));
		if (!g_MetricsCollector.OpenEvents(CrossEnvironment("CC_TEST_CROSS_RECORDS"), eventBudget)) {
			System::PrintDiagnosticLine("[cross-record] FAIL cannot open event file");
			return false;
		}
		g_MetricsCollector.UpdateEventContext({{"run", CrossEnvironment("CC_TEST_CROSS_RUN")}, {"instance", CrossEnvironment("CC_TEST_CROSS_INSTANCE")},
		    {"process", System::GetProcessID()}, {"execution", CrossEnvironment("CC_TEST_CROSS_EXECUTION") + "/0"},
		    {"incarnation", std::stoul(CrossEnvironment("CC_TEST_CROSS_INCARNATION", "0"))}, {"phase", "setup"}, {"tick", 0}});
		const std::string recoveryPath = CrossEnvironment("CC_TEST_CROSS_RECOVERIES");
		if (!recoveryPath.empty()) {
			try {
				std::ifstream input(recoveryPath); const auto document = nlohmann::json::parse(input);
				s_crossRecoveryStarts = document.value("starts", nlohmann::json::object());
				s_crossRecoveryCases = document.value("cases", nlohmann::json::array());
				s_crossEndCases = document.value("forced_ends", nlohmann::json::array());
			} catch (const std::exception& error) { System::PrintDiagnosticLine("[cross-record] FAIL recovery input: " + std::string(error.what())); }
		}
	}
	return g_MetricsCollector.EventsEnabled();
}

static void BeginCrossTick(uint64_t tick) {
	if (!EnsureCrossEventsOpen()) return;
	const auto config = ScenarioRunner::GetLockstepMatchConfig();
	if (!config) {
		s_crossContext = {{"tick", tick}, {"phase", "unmapped"}, {"history_branch", nullptr}};
		g_MetricsCollector.BeginEventTick(s_crossContext);
		return;
	}
	static uint64_t previousRound = 0, previousTick = 0, execution = 0;
	const uint64_t round = ScenarioRunner::GetLockstepRoundId();
	if (round == previousRound && tick <= previousTick) ++execution;
	const bool newRound = previousRound != round;
	const bool catchup = ScenarioRunner::WorldCatchUpActive();
	CrossRememberRestoredInput();
	const auto host = ScenarioRunner::GetLockstepHostPeerId();
	static nlohmann::json authority = nullptr;
	static uint8_t priorHost = 0;
	static bool priorCatchup = false;
	static uint64_t authorityObservedTick = 0;
	static uint64_t configuredStart = 0;
	static std::set<std::string> unmappedHistories;
	static std::map<std::string, std::set<uint64_t>> inPlaceReturns;
	// An unresolved authority is asked again twice a second, not every tick: the report is built under the service's lock.
	const bool transition = newRound || host != priorHost || priorCatchup;
	const bool unresolved = configuredStart == 0 || authority.is_null();
	if (!catchup && (transition || (unresolved && (authorityObservedTick == 0 || tick < authorityObservedTick || tick >= authorityObservedTick + 30)))) {
		const auto diagnostic = nlohmann::json::parse(g_NetMatchService.BuildReportJson(), nullptr, false);
		authority = CrossAuthorityFromReport(diagnostic, config->sessionId, host);
		const auto* lockstep = CrossMatchingLockstep(diagnostic, config->sessionId, host);
		configuredStart = lockstep && lockstep->contains("configured_start_frame") && (*lockstep)["configured_start_frame"].is_number_unsigned() ? (*lockstep)["configured_start_frame"].get<uint64_t>() : 0;
		authorityObservedTick = tick;
	}
	const std::string historyKey = std::to_string(config->sessionId) + "/" + std::to_string(CrossRecordRound(round, config->roundId));
	const bool inPlace = catchup && g_NetMatchService.CatchingUpInPlace();
	auto& returns = inPlaceReturns[historyKey];
	// The frame an in-place catch-up returns at is where this process starts its round again on its own world.
	if (inPlace && ScenarioRunner::WorldCatchUpActivationTick() != 0) returns.insert(ScenarioRunner::WorldCatchUpActivationTick());
	static uint64_t processFirstRound = 0;
	const bool ticketRound = CrossTicketRejoinRound(s_crossTicketRejoin, round, processFirstRound);
	if (CrossHistoryRestored(catchup, inPlace, configuredStart, returns, ticketRound, round == previousRound && tick <= previousTick)) unmappedHistories.insert(historyKey);
	priorHost = host; priorCatchup = catchup;
	// The process's own names and the config's hash change rarely; each tick reads them from here.
	static const std::string run = CrossEnvironment("CC_TEST_CROSS_RUN"), instance = CrossEnvironment("CC_TEST_CROSS_INSTANCE");
	static const std::string executionBase = CrossEnvironment("CC_TEST_CROSS_EXECUTION") + "/";
	static const unsigned long incarnation = std::stoul(CrossEnvironment("CC_TEST_CROSS_INCARNATION", "0"));
	static std::tuple<uint64_t, uint64_t, uint64_t> hashedConfig{0, 0, 0};
	static std::string configHash;
	if (const auto key = std::make_tuple(static_cast<uint64_t>(config->sessionId), static_cast<uint64_t>(config->roundId), static_cast<uint64_t>(config->configRevision));
	    configHash.empty() || key != hashedConfig) {
		hashedConfig = key;
		configHash = NetMatchConfigUtil::StoredConfigHash(*config);
	}
	const char* tickPhase = CrossTickPhase(catchup, tick, s_crossLastCommitted.contains(round) ? s_crossLastCommitted.at(round) : 0, execution);
	// A world's replay runs under its image's round number; its rows name the round their frames were committed in.
	const uint64_t replayedRound = catchup ? ScenarioRunner::WorldCatchUpRoundAt(tick) : 0;
	s_crossContext = {{"run", run}, {"instance", instance},
	    {"process", System::GetProcessID()}, {"execution", executionBase + std::to_string(execution)},
	    {"incarnation", incarnation}, {"seat_incarnation", nullptr},
	    {"authority_generation", catchup ? nlohmann::json(nullptr) : authority}, {"authority_generation_observed_at_tick", authorityObservedTick},
	    {"authority_generation_source", "service.runner.lockstep; refreshed on round/host/catch-up transition"},
	    {"session", std::to_string(config->sessionId)}, {"match", std::to_string(replayedRound != 0 ? replayedRound : CrossRecordRound(round, config->roundId))},
	    {"round", round}, {"source_round", config->roundId}, {"tick", tick}, {"peer", ScenarioRunner::GetLockstepLocalPeerId()},
	    {"history_branch", CrossLandedBranch(CrossHistoryBranch(configuredStart, unmappedHistories.contains(historyKey), ScenarioRunner::GetLockstepResumeFrame(), returns), catchup, tickPhase, configuredStart, tick)},
	    {"configured_start_frame", catchup ? nlohmann::json(nullptr) : nlohmann::json(configuredStart)},
	    {"checkpoint_digest", nullptr}, {"config_revision", config->configRevision},
	    {"config_hash", configHash}, {"host_peer", ScenarioRunner::GetLockstepHostPeerId()},
	    {"phase", tickPhase}, {"gameplay_tick", g_ActivityMan.ActivityRunning()},
	    {"wall_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count()}};
	if (const auto view = g_NetMatchService.GetSeatView(ScenarioRunner::GetLockstepLocalPeerId())) {
		s_crossContext["seat_incarnation"] = view->seat.incarnation;
		s_crossContext["seat_revision"] = view->revision;
		if (view->seat.owner != 0) s_crossOwnIdentity = view->seat.owner;
	}
	g_MetricsCollector.BeginEventTick(s_crossContext);
	// The moderation state a lost host hands over: every applied tick on the host, and once each handover on every survivor.
	if (!catchup && authority.is_number_unsigned()) {
		const uint64_t generation = authority.get<uint64_t>();
		static uint64_t firstGeneration = UINT64_MAX, afterGeneration = UINT64_MAX;
		if (firstGeneration == UINT64_MAX) firstGeneration = generation;
		const auto emit = [&](const char* stage, const std::string& state) {
			const auto parsed = nlohmann::json::parse(state, nullptr, false);
			if (parsed.is_discarded()) return;
			g_MetricsCollector.WriteObservation({{"type", "moderation_snapshot"}, {"stage", stage}, {"tick", tick}, {"host_peer", host}, {"authority_generation", generation}, {"state", parsed}});
		};
		if (host == ScenarioRunner::GetLockstepLocalPeerId()) emit("before", g_NetMatchService.GetModerationSnapshotState(false));
		if (generation > firstGeneration && generation != afterGeneration) {
			afterGeneration = generation;
			emit("after", g_NetMatchService.GetModerationSnapshotState(true));
		}
	}
	// A silence another peer's schedule declared, once its seat is held here: the hold the host committed for it and what it measured.
	if (!catchup && host == ScenarioRunner::GetLockstepLocalPeerId()) {
		static std::set<std::string> namedHolds;
		for (const auto& entry: s_crossSchedule) {
			const std::string id = entry.value("id", std::string());
			const std::string target = entry.value("peer", std::string());
			const bool silence = entry.value("action", std::string()) == "silence" || entry.value("declared_action", std::string()) == "silence";
			if (!silence || target.empty() || target == instance || namedHolds.contains(id) || s_crossBudget < entry.value("tick", uint64_t{0})) continue;
			for (const NetH4ModerationSeat& seat: g_NetMatchService.GetModerationSeats()) {
				if (seat.cpu || !seat.held || seat.displayName != target) continue;
				const auto fact = ScenarioRunner::GetLockstepLastHold(seat.lockstepPeerId);
				if (!fact) continue;
				namedHolds.insert(id);
				g_MetricsCollector.WriteObservation({{"type", "scheduled_hold"}, {"id", id}, {"peer", seat.lockstepPeerId}, {"cause", "silent"}, {"hold_cause", fact->cause},
				    {"roster_cause", NetSeatHoldCauseName(seat.holdCause)}, {"ai_in_control", true}, {"tick", fact->frame}, {"silence_ms", fact->silenceMs},
				    {"bound_ms", fact->boundMs}});
			}
		}
	}
	// This peer's seat comes back - a return, a held seat's reclaim, a stall's in-place catch-up, an applicant seated: the roster's committed
	// owner, the actor it plays from this tick, and a recovery the fresh-input receipt answers, whether or not a schedule asked for one.
	{
		const uint8_t local = ScenarioRunner::GetLockstepLocalPeerId();
		// A return is owed from the first tick the seat is away until its player plays an actor this peer owns: a relaunched peer's catch-up is
		// away before it knows its seat, and the committed binding names the player's actor only once its first frame lands.
		static bool returnOwed = false;
		static bool lastCatchup = false;
		static uint64_t awayRound = 0;
		const bool away = catchup || (local != 0 && ScenarioRunner::IsLockstepSeatUnderAI(local, tick));
		if (round != awayRound) {
			if (!lastCatchup) returnOwed = false;
			awayRound = round;
		}
		lastCatchup = catchup;
		if (away) returnOwed = true;
		Activity* activity = g_ActivityMan.GetActivity();
		const auto view = local != 0 ? g_NetMatchService.GetSeatView(local) : std::nullopt;
		if (returnOwed && !away && activity && view && view->seat.owner != 0) {
			for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
				const Actor* actor = activity->IsLocalHumanSeat(player) ? activity->GetLocallyControlledActor(player) : nullptr;
				if (!actor || !g_MovableMan.IsActor(actor)) continue;
				const uint8_t owner = ScenarioRunner::GetLockstepActorOwner(actor->GetUniqueID(), actor->GetTeam(), false);
				if (owner != local) continue;
				const nlohmann::json reclaim ={{"type", "ownership_reclaim"}, {"round", round}, {"peer", local}, {"stable_seat", view->stableSeat},
				    {"actor", actor->GetUniqueID()}, {"owner_peer", owner}, {"ticket_incarnation", view->seat.incarnation},
				    {"seat_incarnation", view->seat.incarnation}, {"activation_tick", tick}, {"tick", tick}, {"committed", true}};
				g_MetricsCollector.WriteObservation(reclaim);
				ScenarioRunner::NoteHarnessReceipt("ownership_reclaim", reclaim.dump());
				const std::string id = "own-return-" + std::to_string(round) + "-" + std::to_string(tick);
				s_crossRecoveryCases.push_back({{"id", id}, {"return_incarnation", incarnation}, {"deadline_ms", 60000}});
				s_crossRecoveryStarts[id] = {{"effect_finished", true},
				    {"engine_after_wall_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count()}};
				returnOwed = false;
				break;
			}
		}
	}
	if (newRound) {
		nlohmann::json roster = nlohmann::json::array();
		for (const auto& slot: config->players) roster.push_back({{"peer", slot.peerId}, {"team", slot.team}, {"human", !slot.cpu}});
		g_MetricsCollector.WriteObservation({{"type", "adopted_config"}, {"peer_limit", NetMatchConfigUtil::c_MaxPeerCount},
		    {"peer_count", config->peerCount}, {"players", std::move(roster)}, {"difficulty", config->difficulty},
		    {"fog", config->fogOfWar}, {"config", nlohmann::json::parse(NetMatchConfigUtil::BuildReportJson(*config))},
		    {"sim_tick_ms", g_TimerMan.GetDeltaTimeSecs() * 1000.0}});
		System::PrintDiagnosticLine("[cross-context] round=" + std::to_string(round) + " source_round=" + std::to_string(config->roundId) +
		    " config=" + NetMatchConfigUtil::StoredConfigHash(*config) + " peer_limit=" + std::to_string(NetMatchConfigUtil::c_MaxPeerCount));
	}
	previousRound = round; previousTick = tick;
}

// A peer the host banned ends on the ban: the entry that declared it, its identity and the last tick the host played it live.
static void CrossNoteOwnBan() {
	static bool noted = false;
	const auto removal = !noted && g_MetricsCollector.EventsEnabled() ? g_NetMatchService.GetOwnRemoval() : std::nullopt;
	if (!removal || removal->reason != NetRejectReason::ParticipantBanned || removal->boundary == 0 || s_crossOwnIdentity == 0) return;
	noted = true;
	const std::string instance = CrossEnvironment("CC_TEST_CROSS_INSTANCE");
	for (const auto& entry: s_crossSchedule) {
		if (entry.value("action", std::string()) != "moderation-ban" || entry.value("target_peer", std::string()) != instance) continue;
		g_MetricsCollector.WriteObservation({{"type", "moderation_terminal"}, {"id", entry.value("id", std::string())}, {"result", "ParticipantBanned"}, {"terminal", true},
		    {"identity_sha256", NetMatchService::IdentityDigest(s_crossOwnIdentity)}, {"last_live_tick", removal->boundary - 1}, {"tick", removal->boundary - 1},
		    {"simulated_through", g_TimerMan.GetSimUpdateCount()}});
	}
}

static void ApplyCrossSchedule() {
	if (s_crossSchedule.empty() || !ScenarioRunner::IsLockstepControllerSyncActive() || ScenarioRunner::WorldCatchUpActive()) return;
	static uint64_t resetAt = 0;
	if (resetAt && s_crossBudget >= resetAt) {
		const bool accepted = ApplyCrossTransportFault(0, 0, 0, 0);
		g_MetricsCollector.WriteObservation({{"type", "fault_reset"}, {"send_recv_armed", accepted}, {"budget_tick", s_crossBudget}});
		const double finished = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
		for (auto& [id, start]: s_crossRecoveryStarts.items()) if (start.value("waiting_for_reset", false)) {
			start["waiting_for_reset"] = false; start["effect_finished"] = accepted; start["engine_after_wall_ms"] = finished;
			g_MetricsCollector.WriteObservation({{"type", "fault_reset"}, {"id", id}, {"send_recv_armed", accepted}, {"budget_tick", s_crossBudget}});
		}
		resetAt = 0;
	}
	static const std::string instance = CrossEnvironment("CC_TEST_CROSS_INSTANCE");
	for (const auto& fault: s_crossSchedule) {
		const std::string id = fault.at("id");
		if (s_crossFired.contains(id) || s_crossBudget < fault.at("tick").get<uint64_t>()) continue;
		// An entry naming another peer is that peer's fault, declared here so this one can name what it does about it.
		if (const std::string owner = fault.value("peer", std::string()); !owner.empty() && owner != instance) continue;
		const std::string action = fault.at("action");
		if (action == "crash-restart" || action == "brain-eliminate") continue;
		const auto stamp = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
		nlohmann::json receipt{{"type", "fault"}, {"id", id}, {"action", action}, {"requested_budget_tick", fault.at("tick")},
		    {"budget_tick", s_crossBudget}, {"applied_wall_ms", stamp()}, {"applied", false}};
		g_MetricsCollector.WriteObservation({{"type", "fault_begin"}, {"id", id}, {"action", action}, {"budget_tick", s_crossBudget},
		    {"intent_wall_ms", receipt["applied_wall_ms"]}});
		receipt["applied_wall_ms"] = stamp();
		s_crossFired.insert(id);
		if (action == "live-stall" || action == "draw-stall" || action == "late-script-stall") {
			const auto duration = fault.value("duration_ms", 0u);
			if (duration > 0 && duration <= 20000 && action == "live-stall") {
				const uint64_t before = NetLockstepPlane::Ticks();
				{ NetLockstepPlane::Window window("scheduled live stall"); std::this_thread::sleep_for(std::chrono::milliseconds(duration)); }
				receipt["applied"] = true;
				receipt["plane_pumps"] = NetLockstepPlane::Ticks() - before;
			}
		} else if (action == "loss" || action == "lag" || action == "jitter" || action == "outage") {
			const float loss = action == "outage" ? 100.0F : fault.value("percent", 0.0F);
			receipt["applied"] = ApplyCrossTransportFault(fault.value("lag_ms", 0), loss, fault.value("jitter_ms", 0.0F),
			    fault.value("duration_ms", fault.value("deadline_ms", uint64_t{120000})));
			receipt["send_recv_armed"] = receipt["applied"];
			receipt["direction"] = "send_and_receive_all_GNS_connections";
			resetAt = s_crossBudget + fault.value("duration_ticks", uint64_t{1800});
		} else if (action == "ack-drop" || action == "ack-duplicate" || action == "commit-drop") {
			NetH4SetFault(NetH4FaultFromName(action));
			receipt["applied"] = true;
			receipt["effect_observed"] = false;
			receipt["scope"] = "H4_acknowledgement_fault";
		} else if (action == "announced-leave-rejoin") {
			s_crossLeaveRequested = true;
			receipt["applied"] = true;
			receipt["scope"] = "announced_leave_at_next_committed_tick_end";
		} else if (action == "moderation-ban") {
			const std::string target = fault.value("target_peer", std::string());
			receipt["target_peer"] = target;
			for (const NetH4ModerationSeat& seat: g_NetMatchService.GetModerationSeats()) {
				if (seat.cpu || seat.displayName != target) continue;
				const auto view = g_NetMatchService.GetSeatView(seat.lockstepPeerId);
				const uint64_t identity = view ? view->seat.owner : 0;
				const NetKickBanResult result = g_NetMatchService.RemoveParticipant(NetSelectModerationSeat(seat), NetParticipantRemovalAction::BanSession);
				receipt["result"] = NetKickBanResultName(result);
				receipt["target_seat"] = seat.lockstepPeerId;
				receipt["applied"] = result == NetKickBanResult::Ok && identity != 0;
				if (receipt["applied"] == true) {
					g_MetricsCollector.WriteObservation({{"type", "moderation_action"}, {"id", id}, {"action", "Ban"}, {"applied", true}, {"target_peer", target},
					    {"target_seat", seat.lockstepPeerId}, {"identity_sha256", NetMatchService::IdentityDigest(identity)}, {"budget_tick", s_crossBudget},
					    {"tick", g_NetMatchService.GetLastRemovalBoundary()}});
				}
				break;
			}
		}
		receipt["completed_wall_ms"] = stamp();
		if (receipt["applied"] == true) {
			const bool timed = action == "loss" || action == "lag" || action == "jitter" || action == "outage";
			const bool h4 = action == "ack-drop" || action == "ack-duplicate" || action == "commit-drop";
			s_crossRecoveryStarts[id] = {{"engine_after_wall_ms", receipt["completed_wall_ms"]}, {"effect_finished", !timed && !h4},
			    {"waiting_for_reset", timed}, {"start_record", receipt}};
		}
		g_MetricsCollector.WriteObservation(receipt);
		System::PrintDiagnosticLine("[cross-fault] " + receipt.dump());
	}
}

// The harness writes h4-effects.json beside the records; a watcher thread re-reads it when it changes, so no committed
// tick stats or parses it.
static bool CrossEffectsChanged(uint64_t& seenGeneration, nlohmann::json& effects) {
	static std::mutex mutex;
	static nlohmann::json latest = nlohmann::json::array();
	static std::atomic<uint64_t> generation{0};
	static std::jthread watcher([](std::stop_token stop) {
		FloatingPointEnvironment::Initialize();
		const FloatingPointEnvironment::Scope floatingPointScope("diagnostic watcher");
		const auto path = std::filesystem::path(CrossEnvironment("CC_TEST_CROSS_RECORDS")).parent_path() / "h4-effects.json";
		std::filesystem::file_time_type seenTime{};
		uintmax_t seenSize = UINTMAX_MAX;
		while (!stop.stop_requested()) {
			std::error_code error;
			const auto time = std::filesystem::last_write_time(path, error);
			const uintmax_t size = error ? 0 : std::filesystem::file_size(path, error);
			if (!error && (time != seenTime || size != seenSize)) {
				std::ifstream input(path);
				auto parsed = nlohmann::json::parse(input, nullptr, false);
				if (parsed.is_array()) {
					seenTime = time;
					seenSize = size;
					std::lock_guard<std::mutex> lock(mutex);
					latest = std::move(parsed);
					generation.fetch_add(1);
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	});
	if (generation.load() == seenGeneration) return false;
	std::lock_guard<std::mutex> lock(mutex);
	effects = latest;
	seenGeneration = generation.load();
	return true;
}

static void CrossConfirmLocalControllerInputs(uint64_t tick) {
	if (!g_MetricsCollector.EventsEnabled() || !ScenarioRunner::IsLockstepControllerSyncActive() || ScenarioRunner::WorldCatchUpActive()) return;
	const uint64_t target = tick + ScenarioRunner::GetLockstepInputDelayFrames();
	const uint64_t round = ScenarioRunner::GetLockstepRoundId();
	std::vector<ControllerFrame> queued;
	std::vector<long> actors;
	if (ScenarioRunner::PeekLockstepLocalControllerFrames(target, queued)) {
		for (const ControllerFrame& input: queued) {
			if (input.inputMode != Controller::CIM_PLAYER || input.playerRaw < Players::PlayerOne || input.playerRaw >= Players::MaxPlayerCount) continue;
			const long actor = static_cast<long>(input.actorUniqueID);
			actors.push_back(actor);
			// A returning player's first sample precedes its first committed human frame.
			if (g_MetricsCollector.ProducedControllerFor(round, target, actor).empty())
				g_MetricsCollector.RecordProducedController(round, tick, target, actor, input.playerRaw);
		}
	}
	g_MetricsCollector.ConfirmProducedControllers(round, tick, target, actors, s_crossRestoredInputThrough[round]);
}

static void CrossRecoveryAtCommittedTick(uint64_t tick, bool paused = false) {
	CrossRememberRestoredInput();
	if (!g_MetricsCollector.EventsEnabled() || s_crossRecoveryStarts.empty()) return;
	const unsigned incarnation = std::stoul(CrossEnvironment("CC_TEST_CROSS_INCARNATION", "0"));
	static nlohmann::json effects = nlohmann::json::array();
	static uint64_t effectsGeneration = 0;
	CrossEffectsChanged(effectsGeneration, effects);
	if (effects.is_array()) {
		for (const auto& effect: effects) {
			const std::string id = effect.value("id", "");
			if (effect.value("incarnation", ~0u) != incarnation || !s_crossRecoveryStarts.contains(id)) continue;
			auto& start = s_crossRecoveryStarts[id];
			if (start.value("effect_finished", false)) continue;
			NetH4SetFault(NetH4Fault::None);
			start["effect_finished"] = true;
			start["engine_after_wall_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
			g_MetricsCollector.WriteObservation({{"type", "fault_effect"}, {"id", id}, {"effect_observed", true}, {"reset", true}, {"evidence", effect}});
		}
	}
	const uint64_t round = ScenarioRunner::GetLockstepRoundId();
	const uint8_t local = ScenarioRunner::GetLockstepLocalPeerId();
	const bool catchup = ScenarioRunner::WorldCatchUpActive();
	const bool held = local && ScenarioRunner::IsLockstepSeatUnderAI(local, tick);
	uint64_t goodbyeFrame = 0;
	const bool goodbye = g_NetMatchService.HostGoodbyeSeen(goodbyeFrame) && goodbyeFrame > 0;
	for (const auto& recovery: s_crossRecoveryCases) {
		const std::string id = recovery.at("id");
		if (!s_crossRecoveryStarts.contains(id) || s_crossRecoveryDone.contains(id) ||
		    recovery.value("return_incarnation", 0u) != incarnation) continue;
		const auto& start = s_crossRecoveryStarts.at(id);
		const std::string phase = goodbye ? "match_over_goodbye" : catchup ? "catch_up" : held ? "held" : "awaiting_fresh_input";
		if (s_crossRecoveryPhase[id] != phase) {
			s_crossRecoveryPhase[id] = phase;
			g_MetricsCollector.WriteObservation({{"type", "recovery"}, {"id", id}, {"recovery_phase", phase},
			    {"terminal", goodbye}, {"goodbye_frame", goodbyeFrame}, {"deadline_ms", recovery.at("deadline_ms")},
			    {"proof", goodbye ? "NetMatchService::HostGoodbyeSeen" : "committed seat/catch-up observation"}});
		}
		if (goodbye) { s_crossRecoveryDone.insert(id); continue; }
		if (paused || !start.value("effect_finished", false)) continue;
		Activity* activity = g_ActivityMan.GetActivity();
		if (!activity || !g_ActivityMan.ActivityRunning()) continue;
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			Actor* actor = activity->GetControlledActor(player);
			if (!actor) continue;
			const Controller* controller = actor->GetController();
			const bool controllable = controller && actor->IsPlayerControlled() && !controller->IsDisabled() &&
			    !g_MenuMan.IsLiveMenuOwningInput() && !(g_ConsoleMan.IsEnabled() && !g_ConsoleMan.IsReadOnly()) &&
			    ScenarioRunner::GetLockstepActorOwner(actor->GetUniqueID(), actor->GetTeam(), false) == local;
			const auto sample = g_MetricsCollector.ProducedControllerFor(round, tick, actor->GetUniqueID());
			if (!controller || !MetricsCollector::IsFreshControllerRecovery(sample, round, tick, actor->GetUniqueID(),
			        controller->GetWireApplyTick(), controllable, held, catchup, start.value("engine_after_wall_ms", 0.0))) continue;
			const nlohmann::json first = {{"type", "recovery"}, {"id", id}, {"recovery_phase", "first_controllable_input"},
			    {"terminal", true}, {"deadline_ms", recovery.at("deadline_ms")}, {"input", sample}, {"wire_tick", controller->GetWireApplyTick()},
			    {"controllable", controllable}, {"held", held}, {"catchup", catchup}, {"actor", actor->GetUniqueID()}, {"player", player}, {"tick", tick},
			    {"seat_incarnation", s_crossContext.is_object() ? s_crossContext.value("seat_incarnation", nlohmann::json(nullptr)) : nlohmann::json(nullptr)}};
			g_MetricsCollector.WriteObservation(first);
			ScenarioRunner::NoteHarnessReceipt("first_controllable_input", first.dump());
			s_crossRecoveryDone.insert(id);
			break;
		}
	}
}

static void CrossHostOptionFields(NetMatchConfig& draft, const nlohmann::json& options) {
	for (bool cpu: {false, true}) {
		const char* key = cpu ? "cpu_teams" : "human_teams";
		if (!options.contains(key)) continue;
		const auto teams = options.at(key).get<std::vector<int>>();
		const auto expected = std::count_if(draft.players.begin(), draft.players.end(), [=](const auto& slot) { return slot.cpu == cpu; });
		if (teams.size() != static_cast<size_t>(expected)) throw std::runtime_error(std::string(key) + " count differs from the adopted roster");
		if (std::any_of(teams.begin(), teams.end(), [](int team) { return team < 0 || team >= Activity::MaxTeamCount; }))
			throw std::runtime_error(std::string(key) + " contains an invalid team");
		size_t index = 0;
		for (auto& slot: draft.players) if (slot.cpu == cpu) slot.team = static_cast<uint8_t>(teams[index++]);
	}
	if (options.contains("difficulty")) draft.difficulty = options["difficulty"].get<uint8_t>();
	if (options.contains("ai_skill")) for (auto& team: draft.teamRules) team.aiSkill = options["ai_skill"].get<uint8_t>();
	if (options.contains("fog")) draft.fogOfWar = options["fog"].get<bool>();
	if (options.contains("scene")) draft.sceneName = options["scene"].get<std::string>();
	if (options.contains("scene_module")) draft.sceneModule = options["scene_module"].get<std::string>();
}

bool RTE::RunCrossRosterSelfTest(std::string* error) {
	auto config = NetMatchConfigUtil::MakeDefault(0x43524f5353ULL);
	config.peerCount = 3; config.mode = NetMatchMode::PvPvE; config.modePreset = "pvpve";
	config.players = {{1, 0, false, "one"}, {2, 1, false, "two"}, {3, 2, false, "three"}, {0, 3, true, "CPU"}};
	CrossHostOptionFields(config, {{"human_teams", {0, 0, 1}}, {"cpu_teams", {2}}, {"difficulty", 100}, {"ai_skill", 100}});
	if (config.players[1].team != 0 || config.players[2].team != 1 || config.players[3].team != 2 || config.players[3].peerId != 0 ||
	    config.difficulty != 100 || config.teamRules[2].aiSkill != 100 || !NetMatchConfigUtil::ValidateLocalAlpha(config, error)) {
		if (error->empty()) *error = "mixed roster does not preserve three humans plus a peerless CPU on distinct team";
		return false;
	}
	config.players[3].team = 0;
	if (NetMatchConfigUtil::ValidateLocalAlpha(config, nullptr)) { *error = "CPU slot sharing a human team was accepted"; return false; }
	bool refused = false;
	try { CrossHostOptionFields(config, {{"human_teams", {0, 0}}}); } catch (const std::exception&) { refused = true; }
	if (!refused) { *error = "team edit changed the roster size silently"; return false; }
	std::cout << "[net-match-selftest] PASS cross_mixed_roster_preserves_seats_and_cpu_rules" << std::endl;
	return true;
}

static bool CrossHostOptions(unsigned match, std::string* error) {
	if (s_crossHostOptions.empty() || !g_NetMatchService.IsHost()) return true;
	auto draft = g_NetMatchService.GetLobbyMatchConfig();
	const auto& options = s_crossHostOptions[(match / 2) % s_crossHostOptions.size()];
	try { CrossHostOptionFields(draft, options); }
	catch (const std::exception& failure) { if (error) *error = failure.what(); return false; }
	const bool accepted = g_NetMatchService.SubmitHostOptions(draft.configRevision, draft, error);
	if (accepted) System::PrintDiagnosticLine("[cross-host-options] match=" + std::to_string(match) + " accepted=1 revision=" +
	    std::to_string(draft.configRevision) + " intended_config=" + NetMatchConfigUtil::StoredConfigHash(draft));
	return accepted;
}

static bool CrossEndSignalMatches(const std::string& text, uint8_t sender, uint8_t target, const std::string& id,
    uint64_t session, uint64_t sourceRound, unsigned incarnation) {
	if (!target || sender != target || !text.starts_with("[cross-end] ")) return false;
	try {
		const auto value = nlohmann::json::parse(text.substr(12), nullptr, false);
		return value.is_object() && value.value("id", "") == id && value.value("session", "") == std::to_string(session) &&
		    value.value("source_round", uint64_t{0}) == sourceRound && value.value("incarnation", ~0u) == incarnation && value.value("phase", "") == "catch_up";
	} catch (const std::exception&) { return false; }
}

bool RTE::RunCrossEndSignalSelfTest(std::string* error) {
	const std::string message = "[cross-end] " + nlohmann::json{{"id", "end"}, {"session", "81"}, {"source_round", 9},
	    {"incarnation", 1}, {"phase", "catch_up"}}.dump();
	if (!CrossEndSignalMatches(message, 3, 3, "end", 81, 9, 1) || CrossEndSignalMatches(message, 2, 3, "end", 81, 9, 1) ||
	    CrossEndSignalMatches(message, 3, 3, "end", 82, 9, 1) || CrossEndSignalMatches(message, 3, 3, "end", 81, 10, 1) ||
	    CrossEndSignalMatches(message, 3, 3, "end", 81, 9, 2) || CrossEndSignalMatches(message, 3, 3, "old", 81, 9, 1) ||
	    CrossEndSignalMatches("[cross-end] broken", 3, 3, "end", 81, 9, 1)) {
		*error = "forced-end phase signal accepted a stale round, incarnation, sender or id"; return false;
	}
	System::PrintDiagnosticLine("[net-match-selftest] PASS forced_end_phase_signal_requires_sender_round_incarnation_and_id");
	return true;
}

static void CrossEndTargetObservation(uint64_t tick) {
	CrossRememberRestoredInput();
	if (s_crossEndCases.empty() || !ScenarioRunner::WorldCatchUpActive()) return;
	const auto config = ScenarioRunner::GetLockstepMatchConfig();
	if (!config) return;
	static std::set<std::string> signalled, ended;
	const unsigned incarnation = std::stoul(CrossEnvironment("CC_TEST_CROSS_INCARNATION", "0"));
	for (const auto& fault: s_crossEndCases) {
		if (fault.value("phase", "") != "catch_up" || fault.value("target_peer", "") != CrossEnvironment("CC_TEST_CROSS_INSTANCE") ||
		    fault.value("target_incarnation", ~0u) != incarnation) continue;
		const std::string id = fault.at("id");
		const std::string key = id + "/" + std::to_string(config->roundId);
		if (!signalled.contains(key)) {
			const nlohmann::json value{{"id", id}, {"session", std::to_string(config->sessionId)}, {"source_round", config->roundId},
			    {"incarnation", incarnation}, {"phase", "catch_up"}};
			if (g_NetMatchService.SendChat(c_NetChatScopeAll, "[cross-end] " + value.dump())) {
				signalled.insert(key);
				g_MetricsCollector.WriteObservation({{"type", "elimination_phase"}, {"id", id}, {"recovery_id", fault.at("recovery_id")},
				    {"observed_phase", "catch_up"}, {"signal", value}});
			}
		}
		if (g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->IsOver() && ended.insert(key).second)
			g_MetricsCollector.WriteObservation({{"type", "recovery_match_end"}, {"id", id}, {"recovery_id", fault.at("recovery_id")},
			    {"source_round", config->roundId}, {"catchup_at_end", true}, {"observed_tick", tick}});
	}
}

static void CrossEliminationAtCommittedTick(uint64_t tick) {
	if (s_crossSchedule.empty() || !g_NetMatchService.IsHost() || ScenarioRunner::WorldCatchUpActive()) return;
	static std::map<std::string, std::set<long>> issued;
	// Why a forced end has not fired yet, named once its window closes unfired.
	static std::map<std::string, std::string> heldBack;
	static std::set<std::string> closed;
	for (const auto& fault: s_crossSchedule) {
		if (fault.at("action") != "brain-eliminate" || s_crossBudget < fault.at("tick").get<uint64_t>()) continue;
		const std::string id = fault.at("id");
		if (s_crossFired.contains(id)) continue;
		if (s_crossBudget > fault.at("tick").get<uint64_t>() + fault.value("phase_window_ticks", uint64_t{6000})) {
			if (issued[id].empty() && closed.insert(id).second) {
				const std::string reason = heldBack.contains(id) ? heldBack.at(id) : "never evaluated";
				System::PrintDiagnosticLine("[cross-end] " + id + " window closed unfired at budget " + std::to_string(s_crossBudget) + ": " + reason);
				g_MetricsCollector.WriteObservation({{"type", "elimination_window_closed"}, {"id", id}, {"reason", reason}, {"budget_tick", s_crossBudget}});
			}
			continue;
		}
		const auto config = ScenarioRunner::GetLockstepMatchConfig();
		if (!config) continue;
		uint8_t targetPeer = 0;
		for (const auto& slot: config->players) if (slot.displayName == fault.value("target_peer", "") ||
		    (slot.peerId && g_NetMatchService.GetPeerDisplayName(slot.peerId) == fault.value("target_peer", ""))) targetPeer = slot.peerId;
		const std::string phase = fault.value("phase", "hold");
		const bool held = targetPeer && ScenarioRunner::IsLockstepSeatUnderAI(targetPeer, tick);
		const auto& chats = g_NetMatchService.ChatHistory();
		const bool phaseSignalled = std::any_of(chats.begin(), chats.end(), [&](const auto& chat) {
			return CrossEndSignalMatches(chat.text, chat.senderPeerId, targetPeer, id, config->sessionId, config->roundId, fault.value("target_incarnation", ~0u));
		});
		if (issued[id].empty() && !(phase == "hold" ? held : phaseSignalled)) { heldBack[id] = "target " + std::to_string(targetPeer) + " not in its " + phase + " phase at tick " + std::to_string(tick); continue; }
		if (issued[id].empty() && g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->IsOver()) { heldBack[id] = "activity over at tick " + std::to_string(tick); continue; }
		const int survivor = fault.value("survivor_team", 0);
		Actor* writer = g_MovableMan.GetFirstBrainActor(survivor);
		if (!writer) { heldBack[id] = "survivor team " + std::to_string(survivor) + " has no brain at tick " + std::to_string(tick); continue; }
		if (!ScenarioRunner::IsLockstepAIWriteAuthorized(ScenarioRunner::GetLockstepLocalPeerId(), writer->GetTeam(), writer->GetUniqueID(), writer->GetUniqueID())) {
			heldBack[id] = "this peer may not write for survivor team " + std::to_string(survivor) + " at tick " + std::to_string(tick);
			continue;
		}
		unsigned enemies = 0;
		for (int team = 0; team < Activity::MaxTeamCount; ++team) if (team != survivor) {
			if (Actor* brain = g_MovableMan.GetFirstBrainActor(team)) {
				++enemies;
				if (issued[id].insert(brain->GetUniqueID()).second) {
					if (issued[id].size() == 1) g_MetricsCollector.WriteObservation({{"type", "fault"}, {"id", id}, {"action", "brain-eliminate"},
					    {"applied", true}, {"budget_tick", s_crossBudget}, {"recovery_id", fault.at("recovery_id")}});
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameAIGib{writer->GetUniqueID(), brain->GetUniqueID(), 0, writer->GetTeam(), 0, 0}});
					g_MetricsCollector.WriteObservation({{"type", "elimination_request"}, {"id", id}, {"trigger_phase", phase},
					    {"observed_hold", held}, {"phase_signalled", phaseSignalled}, {"target_actor", brain->GetUniqueID()},
					    {"target_team", team}, {"writer", writer->GetUniqueID()}, {"requested_at_tick", tick}});
				}
			}
		}
		if (!issued[id].empty() && !enemies && g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->IsOver()) {
			s_crossFired.insert(id);
			g_MetricsCollector.WriteObservation({{"type", "elimination_outcome"}, {"id", id}, {"result", "activity_over"},
			    {"survivor_team", survivor}, {"overlap_hold_at_end", held}, {"trigger_phase", phase}, {"final_tick", tick},
			    {"recovery_id", fault.at("recovery_id")}, {"target_peer", fault.at("target_peer")}, {"budget_tick", s_crossBudget}});
		}
	}
}
// The live desync check. On everywhere by default; -net-desync-check off opts a measurement run out.
static bool s_netDesyncCheck = true;
static bool s_telemetryBundleOnExit = false;
static std::string s_menuMpTraceError;
// The menu trace's tick coverage: a held seat's trace skips the ticks it was away and resumes at the image it loaded.
struct MenuTraceCoverage {
	uint64_t lastTick = 0;
	uint64_t heldResume = 0;
	unsigned heldGaps = 0;
	bool unexplained = false;
	bool heldReplay = false;

	void NoteRecorded(uint64_t tick, bool firstOfRun) {
		if (firstOfRun) {
			*this = MenuTraceCoverage{};
		} else if (tick != lastTick + 1) {
			// A held rejoin resumes at the image it loaded, past its gap or over ticks an abandoned catch-up recorded.
			if (heldResume != 0 && tick == heldResume) ++heldGaps; else unexplained = true;
		}
		heldResume = 0;
		heldReplay = false;
		lastTick = tick;
	}
	// A held rejoin that resumes at an image behind its last recorded tick replays ticks the trace already holds; they are
	// not recorded twice, so the trace's record budget reaches the cap tick.
	bool SkipsReplayedTick(uint64_t tick) const { return heldReplay && tick <= lastTick; }
	// Past a held rejoin the count also holds the ticks it skipped or replayed twice, so only the cap tick itself counts.
	bool ReachedCap(size_t count, uint64_t cap) const { return heldGaps > 0 ? lastTick >= cap : count >= cap; }
	bool CoversCap(size_t count, uint64_t cap) const { return heldGaps > 0 ? !unexplained && lastTick == cap : count == cap; }
};
static MenuTraceCoverage s_menuTraceCoverage;
static bool s_cowCheckpointAutosave = false;
static bool s_checkpointFixturePrimeScripts = false;
static bool s_checkpointAudioEffects = false;
static bool s_checkpointAudioEffectsPassed = false;
static bool s_checkpointWorldAudio = false;
static bool s_checkpointWorldAudioPassed = false;
static bool s_checkpointCaptureSelfTest = false;
static unsigned s_checkpointCapturePasses = 0;
static int s_cowCheckpointCaptures = 0;
static std::string s_loadGameName;
static bool s_loadGameFailed = false;
static bool s_bitmapSaveSelfTest = false;
static int s_bitmapSaveSelfTestResult = -1;
static bool s_cameraNullSceneSelfTest = false;
static std::string s_recordVideoDirectory;
static int s_recordVideoFps = FrameRecorder::c_DefaultFps;
static bool s_frameRecorderSelfTest = false;
static bool s_saveIoSelfTest = false;
static bool s_saveIoSelfTestQueued = false;
static std::string s_saveIoSelfTestName;
static uint64_t s_saveIoSelfTestAfter = 0;
static uint64_t s_saveIoSelfTestFirstTick = 0;
static bool s_saveMenuSelfTest = false;
static bool s_saveMenuSelfTestPassed = true;
static bool s_menuScriptFailed = false;
static bool s_menuScriptObserveStep = false;
static bool s_menuHashCapture = false;
static bool s_menuScriptComplete = false;
static std::string s_menuScriptHandStep; //!< The step whose hand gesture is still running, as the log names it.
static bool s_menuScriptHandObserve = false;
static bool s_menuScriptHoldE2ePause = false;
static std::string s_snapshotRoundtripSelfTestName;
static bool s_snapshotRoundtripSelfTestPassed = false;
static bool s_snapshotRoundtripLockAudio = false;
static bool s_snapshotRoundtripPerturbAudio = false;
static bool s_snapshotRoundtripCheckPlayback = false;
static std::string s_contractAuditOperation;
static long long s_contractAuditTick = 50;
static bool s_contractAuditFinished = false;
static std::string s_loadSelfTestName;
static bool s_loadSelfTestExpected = false;
static bool s_loadSelfTestPassed = false;
static uint64_t s_netAutosaveRestoreTick = 0; //!< The committed tick to restore; 0 when the checkpoint is named by its place.
static std::string s_netAutosaveRestoreWhich; //!< "oldest" or "newest" of the retained set, for a caller that cannot know the cadence's ticks.
static std::string s_netResumeMatchId; //!< -net-resume-match: restart that match from its newest resumable checkpoint.
static uint64_t s_netResumeTick = 0;   //!< -net-resume-tick: the checkpoint to stand on; 0 takes the newest.
static uint64_t s_netAutosaveRestoreAtTick = 0; //!< The sim tick the restore check runs at; the restored tick by default.
static bool s_netAutosaveRestorePassed = false;
static bool s_saveCatalogSelfTest = false;
static bool s_saveCallbacksSelfTest = false;
static bool s_saveCallbacksSelfTestPassed = false;
static bool s_purgeSelfTest = false;
static bool s_purgeSelfTestPassed = false;
static bool s_globalCallbacksSelfTest = false;
static bool s_globalCallbacksSelfTestPassed = false;

// -selftest-frame-stall <tick>:<ms>: hold one frame, so a sim tick reading the wall clock is observable.
static bool s_frameStallArmed = false;
static bool s_frameStallFired = false;
static bool s_scriptedLeaveDue = false; //!< The -net-match-e2e-leave tick has run; the leave follows at its end.
static long long s_frameStallTick = 0;
static int s_frameStallMs = 0;
static long long s_frameStallAgainTick = 0; //!< A second frame stall, for a seat held again soon after its return.
static int s_frameStallAgainMs = 0;
static bool s_frameStallAgainFired = false;
static long long s_drawStallTick = 0; //!< A present that blocks the main thread at this tick, for the plane's draw window.
static int s_drawStallMs = 0;
static bool s_drawStallFired = false;
static long long s_lateScriptStallTick = 0; //!< A late global script that blocks the main thread at this tick, for the plane's window over it.
static int s_lateScriptStallMs = 0;
static bool s_lateScriptStallFired = false;
// An each-round stall fires once in every round of a rematch chain, keyed by the rematch count.
struct NetLiveStall { uint64_t tick; int milliseconds; bool fired = false; bool eachRound = false; int firedRound = -1; };
static std::vector<NetLiveStall> s_netLiveStalls;
// Test lever: how many ticks the e2e synced pause lasts before its unpause.
static uint64_t s_netTestPauseTicks = 180;
static std::optional<uint64_t> s_netLiveStallActivation;
static bool s_netPerturbWhenLive = false;

// The retired -num-lua-states flag: parsed so old command lines still run, and reported once.
static bool s_retiredLuaStateCountFlag = false;

// -selftest-prematch-history <objects>: spend that many objects' unique IDs and script-state
// assignments before anything else, the way a session that played a scene before hosting has. The
// two-peer rows that prove the assignment needs no agreement start one runtime with a history and
// the other without.
static int s_preMatchHistoryObjects = 0;
static std::string s_preMatchActivity;

// Post-module-load diagnostic. Empty means disabled.
static std::string s_netIdentityDumpPath;

// Headless directory probe: register -> heartbeat -> list -> delete -> list, then exit.
static std::string s_netDirectoryProbeUrl;
static std::string s_netDirectoryProbeCertSha256;
static std::string s_netDirectorySignalProbeUrl;
static std::string s_netDirectorySignalProbeCertSha256;
static bool s_netDirectoryList = false;

// Debug-only transport/session smoke. This exits before gameplay starts.
static bool s_netHost = false;
static std::string s_netJoinAddress;
static uint16_t s_netPort = 41010;
static std::string s_netSessionReportPath;
static bool s_netExitAfterReady = false;
static bool s_netAllowUserdata = false;
// Phase B, unattended gates: the host stands in for a moderator on the seat it is told to watch.
static bool s_netH4Substitute = false;
static uint16_t s_netH4SubstituteSeat = 0;
static uint64_t s_netH4SubstituteDelayMs = 0;
static bool s_netH4SubstituteCancel = false;
static bool s_netLockstep = false;
static bool s_netMatch = false;
static bool s_netMatchServiceE2E = false;
static bool s_netDedicated = false;
static bool s_netWorldDaemon = false;
static bool s_netPersistentWorld = false;
static bool s_netWorldFresh = false; //!< -net-world-fresh: open a new round from the scene instead of the world's newest checkpoint.
static bool s_netMatchServicePresetExplicit = false;
static bool s_netMatchTicksExplicit = false;
static std::string s_netMatchServiceE2EPreset = "P4 Alpha Duel";
static std::string s_netMatchServiceE2EModule;
static std::string s_netMatchServiceE2EScene;
static std::string s_netMatchServiceE2ESceneModule;
static std::string s_netMatchServiceConfigPath;
static std::string s_netPlayerName;

// The W97 router port-mapping feature: opt-in by setting or flag, probed headless.
static int s_netPortMapCli = -1; // -1 unset; -net-port-map off|on forces 0/1 over the setting.
static bool s_netPortMapProbe = false;
static std::string s_netPortMapGateway; // Test seam: the gateway as a.b.c.d[:port].
static std::string s_netPortMapIgd;     // Test seam: the IGD description URL.
// Named host-issued spawn: -net-match-e2e-spawn Class:Preset:Module:x:y:tick[:team]
struct E2eNamedSpawn {
	std::string className;
	std::string preset;
	std::string module;
	float x = 0.0F;
	float y = 0.0F;
	uint64_t tick = 50;
	int32_t team = 0;
};
static std::vector<E2eNamedSpawn> s_e2eNamedSpawns;

static bool ParseE2eSpawnSpec(const std::string& spec, E2eNamedSpawn& out) {
	std::vector<std::string> parts;
	std::string cur;
	for (char c: spec) {
		if (c == ':') {
			parts.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	parts.push_back(cur);
	if (parts.size() < 6 || parts[0].empty() || parts[1].empty() || parts[2].empty()) {
		return false;
	}
	out.className = parts[0];
	out.preset = parts[1];
	out.module = parts[2];
	// A fixture's coordinates read the same in every process locale.
	(void)ParseNumberExact(parts[3].data(), parts[3].data() + parts[3].size(), out.x);
	(void)ParseNumberExact(parts[4].data(), parts[4].data() + parts[4].size(), out.y);
	out.tick = static_cast<uint64_t>(std::strtoull(parts[5].c_str(), nullptr, 10));
	if (parts.size() >= 7) {
		out.team = static_cast<int32_t>(std::strtol(parts[6].c_str(), nullptr, 10));
	}
	return true;
}
static std::string s_netLockstepReportPath;
static std::string s_netJoinSessionId; //!< -net-join-session: the directory session a client joins instead of an address.
// A capped stop holds the link while the relay host hands over what it still owes; a client one
// input-delay behind needs those forwards to finish its own last tick.
static constexpr uint32_t c_CappedStopDrainMs = 8000;
static constexpr uint32_t c_CappedStopLingerMs = 1500;
static constexpr uint64_t c_NetMatchE2EEditorTickCap = 120; //!< A synchronized setup editor that has not finished by here is stuck, not slow.
static constexpr uint64_t c_NetMatchE2EAdmissionWaitTicks = 1800; //!< How long past its cap a host waits for a seat still coming into the round.
static uint64_t s_netMatchE2EOwedSampleFrame = 0; //!< The full-state sample frame a round owes a seat admitted late; 0 when none.
static uint64_t s_netLockstepTicks = 0;
static std::unordered_set<uint64_t> s_netMatchScreenshotTicks;
static uint16_t s_netLockstepInputDelay = 1;
static uint8_t s_netMatchPeers = 2;
static std::optional<bool> s_netMatchBrainlessSpectate;
static std::optional<uint32_t> s_netMatchHumans;
static std::optional<uint32_t> s_netMatchCPUSlots;
static std::string s_netMatchMode = "pvp";
static std::string s_netMatchOwnershipPolicy = "team-owner";
static bool s_netMatchServiceE2EEnteredEditor = false;
static bool s_netMatchE2EBrainPlacement = false; //!< -net-match-e2e-brain-placement: this peer places its own seats' brains in the synchronized setup editor.
static uint64_t s_netMatchE2EEditorTicks = 0; //!< Ticks the activity has spent in the setup editor since the last rendezvous, so a match that never leaves it fails instead of idling.
static uint64_t s_netMatchE2ERendezvousSeen = 0; //!< Rendezvous points the UI probe has passed, so a deliberate wait on another peer does not spend the cap.
static NetMatchE2ETickClock s_netMatchE2ETicks;
static long s_netMatchE2EActorCensus = -1;
static long s_netMatchE2EActorCensusPeak = -1; //!< The max actor count seen, so a transient heal double-spawn that later sheds back to normal is still visible.
static uint64_t s_netMatchE2eSwitchControlTick = 0;
static uint64_t s_netMatchE2eBrainDamageTick = 0;
static uint64_t s_netMatchE2eBrainReseatTick = 0;
static int64_t s_netMatchE2eSwitchUid = 0;
static bool s_netMatchE2eSwitchIssued = false;
static bool s_netMatchE2eSwitchHandedBack = false;
struct E2eOwnerLogEntry {
	uint64_t tick = 0;
	int64_t uid = 0;
	int owner = 0;
	int mode = 0;
};
static std::vector<E2eOwnerLogEntry> s_netMatchE2eOwnerLog;
// Loop-pace accounting, accumulated only while a lockstep match or playback runs: the honest
// wall-tps and per-tick sim cost that steer the pace and rollback work.
static uint64_t s_paceIterations = 0;
static uint64_t s_paceSimTicks = 0;
static long long s_paceSimUs = 0;
static std::atomic<float> s_paceExecutionAverageMs{0.0F};
static std::deque<float> s_paceTickCostsMs; //!< The paced round's recent tick costs, whose median caps what the clock owes.
static long long s_paceUpdateUs = 0;
static long long s_paceDrawUs = 0;
static long long s_paceTrailingUs = 0; //!< Time since the round's last simulated tick, which the round did not run.
static long long s_pacePreviewUs = 0; //!< The draw's share spent in the local prediction preview.
static long long s_paceInterfaceUs = 0; //!< The draw's share spent on input, the menus and the activity's render update.
static uint64_t s_paceFramesDrawn = 0; //!< Frames the presentation cap let through.
static uint64_t s_paceFramesShed = 0; //!< Frames a paced round skipped because it still owed ticks.
static long long s_paceLastPresentUs = 0; //!< When the paced round last drew a frame.
static long long s_paceFrameDrawUs = 0; //!< Time spent drawing and presenting those frames.
static long long s_paceMaxDrawUs = 0; //!< The longest single iteration's draw since the previous census line.

// The loop pace since the previous memory census line, with the draw split where it spends its time.
static std::string PaceCensusSinceLast() {
	struct Mark { uint64_t iterations = 0, ticks = 0; long long simUs = 0, updateUs = 0, drawUs = 0, previewUs = 0, interfaceUs = 0, waitUs = 0, trimmedTicks = 0; uint64_t previews = 0, framesDrawn = 0; long long frameDrawUs = 0; uint64_t framesShed = 0; };
	static Mark last;
	const Mark now{s_paceIterations, s_paceSimTicks, s_paceSimUs, s_paceUpdateUs, s_paceDrawUs, s_pacePreviewUs, s_paceInterfaceUs, ScenarioRunner::GetLockstepWaitUs(), g_TimerMan.GetPaceTrimmedTicks(), LocalPrediction::GetPreviewCount(), s_paceFramesDrawn, s_paceFrameDrawUs, s_paceFramesShed};
	// A new round restarts the counters.
	if (now.iterations < last.iterations || now.ticks < last.ticks) last = Mark{};
	const double ticks = static_cast<double>(std::max<uint64_t>(1, now.ticks - last.ticks));
	const long long wallUs = (now.updateUs - last.updateUs) + (now.drawUs - last.drawUs);
	// Per simulation tick: an idle iteration that neither simulates nor draws would dilute a per-iteration average.
	std::ostringstream out;
	out << std::fixed << std::setprecision(3) << " pace: iterations=" << now.iterations - last.iterations << " sim_ticks=" << now.ticks - last.ticks
	    << " wall_tps=" << (wallUs > 0 ? static_cast<double>(now.ticks - last.ticks) * 1000000.0 / static_cast<double>(wallUs) : 0.0)
	    << " sim_ms_per_tick=" << static_cast<double>(now.simUs - last.simUs) / 1000.0 / ticks
	    << " update_ms_per_tick=" << static_cast<double>(now.updateUs - last.updateUs) / 1000.0 / ticks
	    << " draw_ms_per_tick=" << static_cast<double>(now.drawUs - last.drawUs) / 1000.0 / ticks
	    << " preview_ms_per_tick=" << static_cast<double>(now.previewUs - last.previewUs) / 1000.0 / ticks
	    << " interface_ms_per_tick=" << static_cast<double>(now.interfaceUs - last.interfaceUs) / 1000.0 / ticks
	    << " net_wait_ms=" << (now.waitUs - last.waitUs) / 1000 << " previews=" << now.previews - last.previews
	    << " trimmed_ms=" << static_cast<double>(now.trimmedTicks - last.trimmedTicks) * 1000.0 / static_cast<double>(g_TimerMan.GetTicksPerSecond())
	    << " mspsu_average=" << g_PerformanceMan.GetMSPSUAverage() << " frames_drawn=" << now.framesDrawn - last.framesDrawn
	    << " frames_shed=" << now.framesShed - last.framesShed
	    << " ms_per_frame_drawn=" << static_cast<double>(now.frameDrawUs - last.frameDrawUs) / 1000.0 / static_cast<double>(std::max<uint64_t>(1, now.framesDrawn - last.framesDrawn))
	    << " max_iteration_draw_ms=" << static_cast<double>(s_paceMaxDrawUs) / 1000.0 << " preview_phase_ms=" << LocalPrediction::DescribePhasesSinceLastCall();
	s_paceMaxDrawUs = 0;
	last = now;
	return out.str();
}

// One memory census line: its counts are read on the sim thread between ticks and each part's cost goes on its line; the census worker sums
// the process heaps and prints it.
static void PostMemoryCensus(uint64_t simTick, const std::string& when = std::string()) {
	std::string costs;
	const auto timed = [&costs](const char* name, const auto& part) {
		const HarnessCost::SimulationSpan span;
		std::ostringstream text;
		text << part();
		const int64_t ns = span.Stop();
		HarnessCost::Charge(HarnessCost::Census, ns);
		costs += std::string(costs.empty() ? "" : ",") + name + ":" + std::to_string(ns / 1000);
		return text.str();
	};
	const std::string lua = timed("lua", [] { return g_LuaMan.GetTotalHeapBytes(); });
	// The capture keeps its last image for the next one to share; that image is the full-state instrument's own memory.
	const std::string cow = timed("cow", [] { return CheckpointCow::Get().Cache().Census() + " last_image_mb=" + std::to_string(CheckpointCow::Get().LastImageBytes() >> 20) + " " + CheckpointCow::Get().PartCensus(); });
	const std::string movable = timed("movable", [] { return g_MovableMan.Census(); });
	const std::string atoms = timed("atoms", [] { return Atom::SampledConstructionStacks(); });
	const std::string audio = timed("audio", [] { return g_AudioMan.Census(); });
	const std::string runner = timed("runner", [] { return ScenarioRunner::MemoryCensus(); });
	const std::string console = timed("console", [] { return g_ConsoleMan.LogCensus(); });
	const std::string pace = timed("pace", [] { return PaceCensusSinceLast(); });
	const std::string world = timed("world", [] { return g_NetMatchService.MemoryCensus(); });
	std::ostringstream rest;
	// Rounds restart their ticks, so the census names its own instant for a slope across a rematching run.
	rest << " uptime_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_CensusProcessStart).count();
	// A capture still writing holds its whole frozen image, so a line taken beside one counts it.
	rest << " in_flight=autosave:" << g_ActivityMan.UnwrittenAutosaves() << ",fullstate:" << g_ActivityMan.UnfinishedFullStateCaptures() << when;
	rest << " tick_hashes=" << g_MetricsCollector.GetTickHashCount() << " lua_bytes=" << lua
	     << " actors=" << g_MovableMan.GetActorCount() << " particles=" << g_MovableMan.GetParticleCount() << " cow: " << cow
	     << " movable: " << movable << ' ' << atoms << " audio: " << audio << ' ' << runner << ' ' << console << ' ' << world << pace;
	CensusWorker::Get().Post([simTick, rest = std::move(rest).str(), costs = std::move(costs)]() mutable {
		// The worker's whole job is the census's cost too, charged to the frame it ends in.
		const auto began = std::chrono::steady_clock::now();
		const std::string heap = ProcessHeapCensus(costs);
		System::PrintDiagnosticLine("[mem-census] tick=" + std::to_string(simTick) + heap + rest + " census_us=" + costs);
		HarnessCost::Charge(HarnessCost::Census, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
	});
}

// The census on the process's own clock: a line in every slot of this many seconds from its start, before its first tick and through
// its lobby, loads and catch-up, never more than one a slot; 0 = never.
static uint64_t s_memoryCensusSeconds = 0;
static void MemoryCensusByUptime() {
	if (s_memoryCensusSeconds == 0) return;
	static uint64_t s_nextSlot = 0;
	static std::optional<std::chrono::steady_clock::time_point> s_dueSince;
	const auto now = std::chrono::steady_clock::now();
	const uint64_t slot = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - s_CensusProcessStart).count()) / s_memoryCensusSeconds;
	if (slot < s_nextSlot) return;
	// A line waits out the captures still writing, so it reads the game's own memory and not a frozen image on its way to disk;
	// past a tenth of its slot, or into the next slot, it is taken anyway and says it waited.
	if (!s_dueSince) s_dueSince = now;
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - *s_dueSince).count();
	const bool busy = g_ActivityMan.UnwrittenAutosaves() + g_ActivityMan.UnfinishedFullStateCaptures() != 0;
	const uint64_t dueSlot = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(*s_dueSince - s_CensusProcessStart).count()) / s_memoryCensusSeconds;
	if (busy && waited < static_cast<int64_t>(s_memoryCensusSeconds) * 100 && slot == dueSlot) return;
	s_nextSlot = slot + 1;
	s_dueSince.reset();
	PostMemoryCensus(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), " census_waited_ms=" + std::to_string(waited) + (busy ? " census_busy=1" : ""));
}
// Rollback fidelity probe: capture at tick T, record K hashed ticks, restore + rewind,
// re-run the SAME ticks, compare. Green = the restore layer reproduces the sim byte-exactly.
static long long s_rbProbeAtTick = 0;
static long long s_rbProbeWindow = 30;
static bool s_rbProbeRequested = false;
static int s_rbProbePhase = 0; //!< 0 idle, 1 first pass, 2 restore staged, 3 re-run pass.
static std::vector<SimChecksum::Result> s_rbProbeFirst;
static std::vector<SimChecksum::Result> s_rbProbeSecond;
static long long s_rbProbeSimCount = 0;
static long long s_rbProbeSimTimeTicks = 0;
static bool s_rbProbeInMemory = true;
static bool s_rbProbeUseLoadGame = false;
static bool s_rbProbeLaunchRestorePending = false;
static bool s_rbProbeMemoryRestorePending = false;
static MovableMan::WorldSnapshot s_rbProbeWorld;
static MovableMan::WorldSetAside s_rbProbeOriginals;
static std::string s_rbProbeLuaIdentityAtCapture;
static std::vector<std::string> s_rbProbeLuaGraphsAtCapture;
static std::deque<long long> s_rbProbeSchedule;
static int s_rbProbeFuzzCount = 0;
static uint64_t s_rbProbeFuzzSeed = 1;
static int s_rbProbePassCount = 0;
static int s_rbProbeFailCount = 0;
static std::string s_rbProbeFirstFailure;
static std::string s_rbProbeCapturedDeep;
static std::vector<std::string> s_rbProbeFirstDeep;
static long long s_rbProbeDeepDivergence = -1;
static bool s_rbProbeRestoreMismatch = false;

/// The longest a completed e2e round waits for a menu probe to read its still-drawn pause menu.
static constexpr int64_t c_CompletedProbeHoldMs = 12000;
static int64_t s_netMatchE2ECompletedMs = 0;
static int64_t s_netMatchE2ELeftMs = 0;
static int64_t SteadyMilliseconds() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/// Whether an e2e run that ended at endedMs still draws frames for a probe that is mid-script.
static bool ProbeHoldsE2eEnd(int64_t endedMs) {
	return NetModerationGUIProbe::Running() && SteadyMilliseconds() - endedMs < c_CompletedProbeHoldMs;
}
static int s_netMatchServiceE2EExitCode = 0;
static int s_netMatchServiceE2ERematches = 0;
// A held seat that could not get back in before the host said goodbye finished the match it was in.
static bool s_netMatchCompletedByHostGoodbye = false;
static uint64_t s_netMatchHeldFromTick = 0;
static uint64_t s_netMatchGoodbyeFinalFrame = 0;

/// Whether the host's goodbye, not a broken link, ended this seat's rejoin.
static bool NetMatchHostGoodbyeEndedTheRejoin(const std::string& resyncError) {
	uint64_t finalFrame = 0;
	if (!g_NetMatchService.HostGoodbyeSeen(finalFrame) && resyncError.rfind("match over", 0) != 0) {
		return false;
	}
	s_netMatchGoodbyeFinalFrame = finalFrame;
	return true;
}
static NetMatchHealWindow s_netMatchHeals;
static bool s_netMatchResyncOnDesync = false;
static bool s_netMatchAutoDelay = false;
static std::string s_netMatchServiceE2EError;
// -net-chat-script <file>: lines "tick all|team text" this peer sends at those sim ticks.
// Presentation-only traffic — the sends never touch the command stream or a tick hash.
struct NetChatScriptLine {
	uint64_t tick = 0;
	uint8_t scope = c_NetChatScopeAll;
	std::string text;
};
static std::string s_netChatScriptPath;
static std::vector<NetChatScriptLine> s_netChatScript;
static size_t s_netChatScriptNext = 0;
static void NetChatScriptOnSimTick(uint64_t simTick);
static std::string s_netReplayInPath;
static std::string s_netReplayVerifyPath;
static uint64_t s_netReplayDumpFrom = 1;
static uint64_t s_netReplayDumpTo = 0;

// The launch target decides whether the bundled test module belongs in the session's identity.
bool HarnessMatchRunActive() {
	// This explicit checkpoint fixture flag also restores archives without an
	// active scenario runner. Their saved baseline still includes Tests.rte.
	if (s_cowCheckpointAutosave) return true;
	std::string type = "GAScripted", preset = s_netMatchServiceE2EPreset, module = s_netMatchServiceE2EModule;
	const auto selected = [&](const NetMatchConfig& config) {
		type = config.activityType; preset = config.activityPreset; module = config.activityModule;
	};
	if (!s_netReplayInPath.empty()) {
		NetMatchReplayReader replay;
		if (!replay.Open(s_netReplayInPath)) return false;
		selected(replay.GetConfig());
	} else if (!s_netMatchServiceE2E) {
		return false;
	} else if (!s_netMatchServiceConfigPath.empty()) {
		std::ifstream input(s_netMatchServiceConfigPath, std::ios::binary);
		std::vector<uint8_t> bytes(NetLobbyProtocol::c_HeaderBytes + NetLobbyProtocol::c_MaxPayloadBytes + 1);
		input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		bytes.resize(static_cast<size_t>(input.gcount()));
		const auto decoded = NetLobbyProtocol::Decode(bytes);
		if (const auto* payload = decoded.ok ? std::get_if<NetLobbyMatchConfig>(&decoded.message.payload) : nullptr) selected(payload->config);
	}
	if (!module.empty()) return module == "Tests.rte";
	return g_PresetMan.GetEntityPreset(type, preset) == nullptr;
}
static long long s_lpInvarianceTick = 0;
static std::vector<int> s_lpInvarianceDepths;
static std::vector<int> s_lpInvarianceRepeats;
static std::string s_lpOverlayLinkModes; //!< -lpinv-overlay-links: the overlay link arms run after the depth cases, in order.
static int s_lpInvarianceFailures = -1; //!< -1 = not run, else the count of failed checks.
static std::string s_lpExpectEquip; //!< -lpinv-expect: the preset the previews must hold once their horizon passes its pickup tick.
static long long s_lpExpectEquipTick = 0;
static long long s_lpExpectEquipSlack = 0; //!< Ticks the preview's pickup may lag the canonical one (the reach ray is a random cast).
static long long s_lpExpectFireTick = 0;
static long long s_lpExpectFireSlack = 0;
static long long s_eventLedgerPressTick = 0; //!< -local-prediction-event-ledger: the tick the tracked press is sampled at.
static bool s_eventLedgerChecked = false;
static long long s_eventLedgerFlashTick = -1; //!< The committed tick a preview first drew the muzzle flash on.
static uint64_t s_eventLedgerLuaEmitterUID = 0;
static std::string s_eventLedgerLuaPreset;
static std::unordered_set<uint64_t> s_eventLedgerGlowUIDs;
struct GhostSample {
	uint64_t tick = 0;
	PreviewEventLedger::Key key;
	bool any = false;
	Vector pos;
	Vector vel;
	float globalAccScalar = 1.0F;
	float airResistance = 0;
	float airThreshold = 0;
	bool adopted = false;     //!< A canonical spawn is waiting behind this ghost.
	bool adopteeHeld = false; //!< That spawn is still off the frame.
	uint64_t poseTick = 0;
	uint64_t adoptionTick = 0;
	long adopteeUID = 0;
	Vector adopteePos;
	Vector adopteeVel;
	float adopteeGlobalAccScalar = 1.0F;
	float adopteeAirResistance = 0;
	float adopteeAirThreshold = 0;
};
// One row per seamless swap: the ghost went and the spawn it led took the frame.
struct SwapRecord {
	uint64_t adoptionTick = 0;
	uint64_t tick = 0;
	uint64_t leadTicks = 0;
	float poseDelta = 0;
	long adopteeUID = 0;
	bool adopteeAlive = false;
	bool adopteeHeldAfter = false;
	Vector adopteePos;
	Vector ghostPos;
};
static std::vector<SwapRecord> s_eventLedgerSwaps;
static uint64_t s_eventLedgerSwapCount = 0;
static int s_eventLedgerHoldDumpPoints = 0; //!< Ticks whose dump was taken both with and without the adoption hold.
static int s_eventLedgerHoldDumpDiffs = 0;
static int s_eventLedgerHoldSampleCandidates = 0; //!< Held ghosts the sampler reached at an adoption or last-led tick.
static bool s_eventLedgerSwapDumpT0 = false; //!< The three cross-run dump probes: adoption, last led tick, swap.
static bool s_eventLedgerSwapDumpMid = false;
static bool s_eventLedgerSwapDumpEnd = false;
static std::vector<GhostSample> s_eventLedgerGhostSamples; //!< Per-committed-tick ghost kinematics keyed to the ledger event.
static bool s_eventLedgerGhostRegistered = false;
static bool s_eventLedgerGhostDumpTaken = false;
static bool s_eventLedgerGhostDumpIdentical = false;
static std::vector<PreviewEventLedger::Key> s_eventLedgerGhostMovedKeys;
static bool s_eventLedgerExpireDroppedGhost = false;
static std::string s_eventLedgerExpireDetail;
static long long s_fundsPreviewPress = 0;
static const int s_fundsPreviewTeam = Activity::TeamOne; //!< The team -net-match-e2e-buy-command grants and buys for.
static std::string s_netReplayOutPath;
static int s_netReplayExitCode = 0;
static uint64_t s_netReplayTicks = 0;
static uint64_t s_netReplaySegmentTick = 0; //!< The world checkpoint a played segment stands on; 0 for an ordinary recording.
static bool s_netReplayFromMenu = false;
static bool s_netReplayReturnPending = false;
static std::string s_netReplayReturnStatus;
static float s_netReplayPreviousDeltaTime = 0.0F;
static bool s_netReplayPreviousFreeRun = false;
static void CloseNetReplayPlayback();
bool ConfigureNetMatchServiceE2EActivity(const std::string& activityPreset, std::string* error);
bool StageResyncedMatchActivity(std::string* error);

/// Restores the checkpoint of this match the run was told to restore and reports whether the restored
/// world is the one that checkpoint recorded. Ends the run, so it only ever serves a driver.
/// @param tick The committed tick to restore, or 0 to take the oldest or newest checkpoint still retained.
static bool RunAutosaveRestoreCheck(uint64_t tick, const std::string& which) {
	g_ActivityMan.WaitForAutosaveTasks();
	const std::string matchId = g_NetMatchService.GetAutosaveMatchId();
	std::string refusal;
	std::optional<AutosaveDescriptor> named;
	if (tick > 0) {
		named = AutosaveStore::Find(matchId, tick, &refusal);
	} else if (const std::vector<AutosaveDescriptor> held = AutosaveStore::ListRestorable(matchId); !held.empty()) {
		// A caller that cannot know the cadence's exact ticks names the checkpoint by its place; the
		// restore below still goes through the tick that names it.
		named = which == "oldest" ? held.back() : held.front();
	} else {
		refusal = "no restorable checkpoint";
	}
	if (!named) {
		{
			std::ostringstream line;
			line << "[autosave] restore_check FAIL match=" << matchId << " tick=" << tick << " reason=" << refusal;
			System::PrintDiagnosticLine(line.str());
		}
		return false;
	}
	const bool policy = AutosaveStore::RunSelfTest(matchId);
	if (!g_ActivityMan.LoadAutosaveToRestart(matchId, named->savedTick) || !g_ActivityMan.RestartActivity()) {
		{
			std::ostringstream line;
			line << "[autosave] restore_check FAIL match=" << matchId << " tick=" << named->savedTick << " reason=restore refused";
			System::PrintDiagnosticLine(line.str());
		}
		return false;
	}
	const std::string worldHash = NetIdentity::HashHex(NetIdentity::HashCanonicalText("autosave-world", {{"structure", g_MovableMan.SaveWorldStructure()}}));
	const auto restoredTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	const bool passed = policy && worldHash == named->worldStructureHash && restoredTick == named->savedTick;
	// The flag prints as a number, the way the store's own self-test line reports its results.
	System::PrintDiagnosticLine(std::format("[autosave] restore_check {} match={} tick={} sim_update_count={} world_hash={} expected={} policy={}\n",
	                                        passed ? "PASS" : "FAIL", matchId, named->savedTick, restoredTick, worldHash, named->worldStructureHash, static_cast<int>(policy)));
	return passed;
}

// A dead-end transport for replay playback: nothing to poll, nowhere to send.
class NullNetTransport final : public INetTransport {
public:
	bool StartHost(uint16_t, std::string*) override { return true; }
	bool Connect(const std::string&, uint16_t, std::string*) override { return true; }
	bool Send(NetPeerId, NetTransportLane, const std::vector<uint8_t>&, std::string*, bool*) override { return true; }
	void Disconnect(NetPeerId, const std::string&) override {}
	void Stop() override {}
	std::vector<NetTransportEvent> PollEvents() override { return {}; }
};
static std::string s_menuScriptPath;
static std::string s_menuScriptOutDir;
// §9b's moderation panel, driven headless: the gate names the actions, the seat and how long to wait
// before each. They take the panel's own path, so a gate exercises what a host clicks.
static std::vector<std::string> s_netMatchE2eModerate;
static size_t s_netMatchE2eModerateAt = 0;
static int s_netMatchE2eModerateSeat = -1;
static uint64_t s_netMatchE2eModerateDelayMs = 0;
static uint64_t s_netMatchE2eModerateReadyMs = 0;

bool NetGameplayRequested() {
	return s_netLockstep || s_netMatch;
}

const char* ActivityStateName(Activity::ActivityState state);

/// <summary>
/// Initializes all the essential managers.
/// </summary>
void InitializeManagers() {
	ThreadMan::Construct();
	TimerMan::Construct();
	PresetMan::Construct();
	SettingsMan::Construct();
	WindowMan::Construct();
	GLResourceMan::Construct();
	LuaMan::Construct();
	FrameMan::Construct();
	PerformanceMan::Construct();
	PostProcessMan::Construct();
	PrimitiveMan::Construct();
	AudioMan::Construct();
	GUISound::Construct();
	MusicMan::Construct();
	UInputMan::Construct();
	ConsoleMan::Construct();
	SceneMan::Construct();
	MovableMan::Construct();
	MetaMan::Construct();
	MenuMan::Construct();
	CameraMan::Construct();
	ActivityMan::Construct();
	LoadingScreen::Construct();
	MetricsCollector::Construct();
	SimChecksum::Construct();
	NetMatchService::Construct();

	g_ThreadMan.Initialize();
	g_SettingsMan.Initialize();

	// Say once that neither the flag nor the settings line picks the count any more.
	if (s_retiredLuaStateCountFlag || g_SettingsMan.GetRetiredLuaStateCountOverride() != -1) {
		std::cout << "[lua] the threaded Lua state count is fixed at " << c_LuaStateCount
		          << "; -num-lua-states and NumberOfLuaStatesOverride are retired" << std::endl;
	}
	g_WindowMan.Initialize();
	g_GLResourceMan.Initialize();

	g_LuaMan.Initialize();
	g_TimerMan.Initialize();
	g_FrameMan.Initialize();
	g_PostProcessMan.Initialize();
	g_PerformanceMan.Initialize();
	// The minimize lever's fullscreen form enters the game's own fullscreen before any round, as a player who plays fullscreen starts.
	if (TestMinimizeLever().fullscreen && g_WindowMan.GetWindow()) {
		SDL_ShowWindow(g_WindowMan.GetWindow());
		if (!g_WindowMan.IsFullscreen()) g_WindowMan.ToggleFullscreen();
		WriteTestWindowState(0);
	}

	if (g_AudioMan.Initialize()) {
		g_GUISound.Initialize();
		g_MusicMan.Initialize();
		if (std::getenv("CCCP_HEADLESS") != nullptr) {
			g_AudioMan.SetOutputSilenced(true);
			{
				std::ostringstream line;
				line << "[audio] output silenced for the headless run";
				System::PrintDiagnosticLine(line.str());
			}
		}
	}

	g_UInputMan.Initialize();
	g_ConsoleMan.Initialize();
	g_SceneMan.Initialize();
	g_MovableMan.Initialize();
	g_MetaMan.Initialize();
	g_MenuMan.Initialize();

	// Overwrite Settings.ini after all the managers are created to fully populate the file. Up until this moment Settings.ini is populated only with minimal required properties to run.
	// If Settings.ini already exists and is fully populated, this will deal with overwriting it to apply any overrides performed by the managers at boot (e.g resolution validation).
	if (g_SettingsMan.SettingsNeedOverwrite()) {
		g_SettingsMan.UpdateSettingsFile();
	}
}

/// <summary>
/// Destroys all the managers and frees all loaded data before termination.
/// </summary>
void DestroyManagers() {
	g_SimChecksum.Destroy();
	g_NetMatchService.Destroy();
	g_MetricsCollector.Destroy();
	g_MetaMan.Destroy();
	g_PerformanceMan.Destroy();
	g_MovableMan.Destroy();
	g_SceneMan.Destroy();
	g_ActivityMan.Destroy();
	g_GUISound.Destroy();
	g_AudioMan.Destroy();
	g_MusicMan.Destroy();
	g_PresetMan.Destroy();
	g_UInputMan.Destroy();
	g_PostProcessMan.Destroy();
	g_FrameMan.Destroy();
	g_TimerMan.Destroy();
	g_LuaMan.Destroy();
	ContentFile::FreeAllLoaded();
	g_ConsoleMan.Destroy();
	g_GLResourceMan.Destroy();
	g_WindowMan.Destroy();

#ifdef DEBUG_BUILD
	Entity::ClassInfo::DumpPoolMemoryInfo(Writer("MemCleanupInfo.txt"));
#endif
}

int ShutDown(int exitCode) {
	// A quit during the identity walk must not sit through the rest of the disk pass; what it finished
	// is kept. This runs before the statics are torn down, where the future would wait unasked.
	NetIdentity::StopManifestPriming();
	// The writer holds frames the run has already presented, so it drains while SDL is still up.
	FrameRecorder::Instance().Finish();
	MenuAutomation::ReportWatches();
	if (!MenuAutomation::FinishReadbacks()) {
		System::PrintDiagnosticErrorLine("[menu-readback] queued snapshot write failed");
		exitCode = EXIT_FAILURE;
	}
	// An assert a player would have had to dismiss is a failed run, whichever way it was answered.
	if (RTEError::AssertFired()) {
		System::PrintDiagnosticErrorLine("[assert] the run continued past an assert; see the RTE Assert line above");
		exitCode = EXIT_FAILURE;
	}
	if (!s_contractAuditOperation.empty() && !s_contractAuditFinished) exitCode = EXIT_FAILURE;
	if (s_menuScriptFailed) exitCode = EXIT_FAILURE;
	if (s_checkpointAudioEffects && !s_checkpointAudioEffectsPassed) exitCode = EXIT_FAILURE;
	if (s_checkpointWorldAudio && !s_checkpointWorldAudioPassed) exitCode = EXIT_FAILURE;
	if (s_checkpointCaptureSelfTest && s_checkpointCapturePasses != 2) exitCode = EXIT_FAILURE;
	if (s_bitmapSaveSelfTest && s_bitmapSaveSelfTestResult != 0) exitCode = EXIT_FAILURE;
	if (!s_snapshotRoundtripSelfTestName.empty() && !s_snapshotRoundtripSelfTestPassed) exitCode = EXIT_FAILURE;
	if (!s_loadSelfTestName.empty() && !s_loadSelfTestPassed) exitCode = EXIT_FAILURE;
	if ((s_netAutosaveRestoreTick > 0 || !s_netAutosaveRestoreWhich.empty()) && !s_netAutosaveRestorePassed) exitCode = EXIT_FAILURE;
	if (s_saveCallbacksSelfTest && !s_saveCallbacksSelfTestPassed) exitCode = EXIT_FAILURE;
	if (s_purgeSelfTest && !s_purgeSelfTestPassed) exitCode = EXIT_FAILURE;
	if (s_globalCallbacksSelfTest && !s_globalCallbacksSelfTestPassed) exitCode = EXIT_FAILURE;
	if (s_saveIoSelfTest) {
		const bool saved = s_saveIoSelfTestQueued && g_ActivityMan.WaitForSaveGameTask();
		{
			std::ostringstream line;
			line << "[save-selftest] completed=" << saved;
			System::PrintDiagnosticLine(line.str());
		}
		if (!saved || !s_saveMenuSelfTestPassed) exitCode = EXIT_FAILURE;
	}
	g_ThreadMan.GetPriorityThreadPool().wait_for_tasks();
	g_ThreadMan.GetBackgroundThreadPool().wait_for_tasks();
	g_ActivityMan.WaitForAutosaveTasks();
	LocalPrediction::Clear();
	PreviewEventLedger::Clear();
	if (s_rbProbeOriginals.held) {
		// These originals outlive the managers, so a refusal here is discarded while there is still an engine to do it.
		if (!g_MovableMan.ReinstateWorld(s_rbProbeOriginals)) {
			g_MovableMan.DiscardWorld(s_rbProbeOriginals);
		}
		exitCode = EXIT_FAILURE;
	}
	s_rbProbeWorld.Clear();
	ScenarioRunner::CloseLockstepReplayRecord();
	if (s_telemetryBundleOnExit) {
		TelemetryBundle::Flush();
		TelemetryBundle::RequestCapture();
		if (!TelemetryBundle::CaptureAtTickBoundary()) exitCode = EXIT_FAILURE;
	} else {
		TelemetryBundle::CaptureAtTickBoundary();
	}
	if (!TelemetryBundle::Flush()) exitCode = EXIT_FAILURE;
	g_ConsoleMan.SaveAllText("LogConsole.txt");
	UInputMan::StopJoystickUpdater();
	DestroyManagers();
	TelemetryBundle::Finish();
	allegro_exit();
	SDL_Quit();
	std::cout.flush();
	std::cerr.flush();
	return exitCode;
}

/// <summary>
/// Command-line argument handling.
/// </summary>
/// <param name="argCount">Argument count.</param>
/// <param name="argValue">Argument values.</param>
bool HandleMainArgs(int argCount, char** argValue) {
	// Discard the first argument because it's always the executable path/name
	argCount--;
	argValue++;
	if (argCount == 0) {
		return true;
	}
	bool launchModeSet = false;
	bool singleModuleSet = false;

	for (int i = 0; i < argCount;) {
		std::string currentArg = argValue[i];
		bool lastArg = i + 1 == argCount;
		if (currentArg == "-net-cross-ticket-rejoin") {
			if (CrossEnvironment("CCCP_HEADLESS") != "1") { std::cerr << "[cross-ticket-rejoin] FAIL cross ticket rejoin requires headless" << std::endl; return false; }
			s_crossTicketRejoin = true; ++i; continue;
		}
		if (currentArg == "-net-cross-host-options" && !lastArg) {
			try {
				if (CrossEnvironment("CCCP_HEADLESS") != "1") throw std::runtime_error("cross options require headless");
				std::ifstream input(argValue[i + 1]); s_crossHostOptions = nlohmann::json::parse(input);
				if (!s_crossHostOptions.is_array() || s_crossHostOptions.empty() || s_crossHostOptions.size() > 32) throw std::runtime_error("invalid host options list");
			} catch (const std::exception& error) { std::cerr << "[cross-host-options] FAIL " << error.what() << std::endl; return false; }
			i += 2; continue;
		}
		if (currentArg == "-net-cross-schedule" && !lastArg) {
			try {
				if (CrossEnvironment("CCCP_HEADLESS") != "1") throw std::runtime_error("cross schedule requires headless");
				std::ifstream input(argValue[i + 1]);
				s_crossSchedule = nlohmann::json::parse(input);
				if (!s_crossSchedule.is_array() || s_crossSchedule.size() > 256) throw std::runtime_error("invalid cross schedule length");
				std::set<std::string> ids;
				for (const auto& fault: s_crossSchedule) {
					if (!fault.at("tick").is_number_unsigned() || fault.at("tick").get<uint64_t>() == 0 || !ids.insert(fault.at("id").get<std::string>()).second)
						throw std::runtime_error("invalid cross fault tick or duplicate id");
				}
			} catch (const std::exception& error) { std::cerr << "[cross-schedule] FAIL " << error.what() << std::endl; return false; }
			i += 2; continue;
		}
		if (currentArg == "-net-cross-rematches" && !lastArg) {
			const std::string count = argValue[i + 1];
			if (count.empty() || !std::all_of(count.begin(), count.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }) || count.size() > 4) return false;
			// The rematch budget counts committed ticks through the cross records; without them it never advances.
			if (CrossEnvironment("CCCP_HEADLESS") != "1" || CrossEnvironment("CC_TEST_CROSS_RECORDS").empty()) {
				std::cerr << "[cross-rematches] FAIL cross rematches require headless and CC_TEST_CROSS_RECORDS" << std::endl;
				return false;
			}
			s_crossRematches = static_cast<unsigned>(std::stoul(count));
			i += 2; continue;
		}

		if (currentArg == "-cout") {
			System::EnableLoggingToCLI();
		}

		if (currentArg == "-ext-validate") {
			System::EnableExternalModuleValidationMode();
		}

		// Arm per-tick state hashing for the determinism trace.
		if (currentArg == "-tick-hashes") {
			s_recordTickHashes = true;
			// Deterministic runs drain async path solves each frame so they can't race the node-cost rewrite.
			g_SettingsMan.SetForceImmediatePathingRequestCompletion(true);
		}
		if (currentArg == "-memory-census-probe-size") {
			if (lastArg) return false;
			s_memoryCensusProbeSize = std::strtoull(argValue[i + 1], nullptr, 10);
			s_memoryCensusHistogram = true;
			i += 2;
			continue;
		}
		if (currentArg == "-memory-census-atom-stacks") {
			Atom::SampleConstructionStacks(2000);
			++i;
			continue;
		}
		if (currentArg == "-memory-census-histogram") {
			s_memoryCensusHistogram = true;
			++i;
			continue;
		}
		if (currentArg == "-memory-census-seconds") {
			if (lastArg) return false;
			s_memoryCensusSeconds = std::strtoull(argValue[i + 1], nullptr, 10);
			i += 2;
			continue;
		}
		if (currentArg == "-memory-census-ticks") {
			if (lastArg) return false;
			s_memoryCensusTicks = std::strtoull(argValue[i + 1], nullptr, 10);
			i += 2;
			continue;
		}
		if (currentArg == "-net-live-tick-hashes") {
			if (lastArg) return false;
			s_netLiveTickHashPath = argValue[i + 1];
			i += 2;
			continue;
		}
		if (currentArg == "-net-fullstate-hash-every") {
			uint32_t every = 0;
			const std::string value = lastArg ? "" : argValue[i + 1];
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), every);
			if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || every == 0) {
				System::PrintDiagnosticErrorLine("[fullstate] -net-fullstate-hash-every requires a positive 32-bit integer");
				return false;
			}
			s_netFullStateEvery = every;
			i += 2;
			continue;
		}
		if (currentArg == "-net-fullstate-dump") {
			if (lastArg) return false;
			s_netFullStateDump = argValue[i + 1];
			i += 2;
			continue;
		}
		if (currentArg == "-cow-checkpoint-autosave") {
			s_cowCheckpointAutosave = true;
			++i;
			continue;
		}
		if (currentArg == "-checkpoint-fixture-prime-scripts") {
			s_checkpointFixturePrimeScripts = true;
			++i;
			continue;
		}
		// Reports every engine object a capture's worker threads make; Debug and ASan builds report from the start.
		if (currentArg == "-checkpoint-sentinel") {
			CaptureSentinel::Enable();
			++i;
			continue;
		}
		if (currentArg == "-checkpoint-audio-effects-selftest") {
			s_checkpointAudioEffects = true;
			++i;
			continue;
		}
		if (currentArg == "-checkpoint-capture-selftest") {
			s_checkpointCaptureSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-checkpoint-audio-world-selftest") {
			s_checkpointWorldAudio = true;
			++i;
			continue;
		}
		if (currentArg == "-load-game") {
			if (lastArg) {
				{
					std::ostringstream line;
					line << "[load-game] usage: -load-game <SaveName>, the name the load menu shows; add -max-ticks N to stop the run after N ticks";
					System::PrintDiagnosticLine(line.str());
				}
				++i;
				continue;
			}
			s_loadGameName = argValue[++i];
			++i;
			continue;
		}
		if (currentArg == "-bitmap-save-selftest") {
			s_bitmapSaveSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-camera-null-scene-selftest") {
			s_cameraNullSceneSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-frame-recorder-selftest") {
			s_frameRecorderSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-record-video") {
			if (lastArg) {
				{
					std::ostringstream line;
					line << "[record-video] usage: -record-video <existing empty directory>; add -record-video-fps N for a rate other than " << FrameRecorder::c_DefaultFps;
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_recordVideoDirectory = argValue[++i];
			++i;
			continue;
		}
		if (currentArg == "-record-video-fps") {
			if (lastArg) {
				{
					std::ostringstream line;
					line << "[record-video] usage: -record-video-fps <1-" << FrameRecorder::c_MaxFps << ">";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			const std::string rate = argValue[++i];
			++i;
			const long value = rate.empty() || rate.find_first_not_of("0123456789") != std::string::npos ? 0 : std::strtol(rate.c_str(), nullptr, 10);
			if (value < 1 || value > FrameRecorder::c_MaxFps) {
				{
					std::ostringstream line;
					line << "[record-video] frame rate '" << rate << "' is outside 1-" << FrameRecorder::c_MaxFps;
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_recordVideoFps = static_cast<int>(value);
			continue;
		}
		if (currentArg == "-save-callback-selftest") {
			s_saveCallbacksSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-global-callback-selftest") {
			s_globalCallbacksSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-purge-selftest") {
			s_purgeSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-save-catalog-selftest") {
			s_saveCatalogSelfTest = true;
			++i;
			continue;
		}
		if (currentArg == "-selftest-preallocate-sound-identities" && i + 1 < argCount) {
			const uint64_t count = std::strtoull(argValue[i + 1], nullptr, 10);
			for (uint64_t n = 0; n < count; ++n) {
				SoundContainer scratch;
			}
			{
				std::ostringstream line;
				line << "[selftest] preallocated " << count << " sound identities cursor=" << g_AudioMan.GetCheckpointSoundContainerCursor();
				System::PrintDiagnosticLine(line.str());
			}
			i += 2;
			continue;
		}
		if (currentArg == "-net-test-perturb-when-live") { s_netPerturbWhenLive = true; ++i; continue; }
		if (currentArg == "-net-test-pause-ticks" && i + 1 < argCount) {
			const std::string value = argValue[++i];
			uint64_t ticks = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), ticks);
			if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && ticks > 0) s_netTestPauseTicks = ticks;
			++i;
			continue;
		}
		if ((currentArg == "-net-test-live-stall" || currentArg == "-net-test-live-stall-each-round") && i + 1 < argCount) {
			const std::string spec = argValue[++i];
			const size_t separator = spec.find(':');
			NetLiveStall stall{};
			stall.eachRound = currentArg == "-net-test-live-stall-each-round";
			const char* headless = std::getenv("CCCP_HEADLESS");
			if (stall.eachRound && (headless == nullptr || std::string(headless) != "1")) {
				System::PrintDiagnosticLine("[net-test] -net-test-live-stall-each-round requires CCCP_HEADLESS=1: ignored");
			} else if (separator != std::string::npos) {
				const auto tick = std::from_chars(spec.data(), spec.data() + separator, stall.tick);
				const auto duration = std::from_chars(spec.data() + separator + 1, spec.data() + spec.size(), stall.milliseconds);
				if (tick.ec == std::errc{} && tick.ptr == spec.data() + separator && duration.ec == std::errc{} &&
				    duration.ptr == spec.data() + spec.size() && stall.tick > 0 && stall.milliseconds > 0 && stall.milliseconds <= 20000)
					s_netLiveStalls.push_back(stall);
			}
			++i;
			continue;
		}
		if (currentArg == "-selftest-prematch-activity" && i + 1 < argCount) {
			s_preMatchActivity = argValue[i + 1];
			++i;
			continue;
		}
		if (currentArg == "-selftest-prematch-history" && i + 1 < argCount) {
			s_preMatchHistoryObjects = static_cast<int>(std::strtol(argValue[i + 1], nullptr, 10));
			++i;
			continue;
		}
		if (currentArg == "-selftest-draw-stall" && i + 1 < argCount) {
			const std::string spec = argValue[i + 1];
			const size_t separator = spec.find(':');
			if (separator != std::string::npos) {
				s_drawStallTick = std::strtoll(spec.substr(0, separator).c_str(), nullptr, 10);
				s_drawStallMs = static_cast<int>(std::strtol(spec.substr(separator + 1).c_str(), nullptr, 10));
			}
			if (s_drawStallMs <= 0) System::PrintDiagnosticErrorLine("[selftest] draw stall expected <tick>:<ms>, got " + spec);
			i += 2;
			continue;
		}
		if (currentArg == "-selftest-late-script-stall" && i + 1 < argCount) {
			const std::string spec = argValue[i + 1];
			const size_t separator = spec.find(':');
			if (separator != std::string::npos) {
				s_lateScriptStallTick = std::strtoll(spec.substr(0, separator).c_str(), nullptr, 10);
				s_lateScriptStallMs = static_cast<int>(std::strtol(spec.substr(separator + 1).c_str(), nullptr, 10));
			}
			if (s_lateScriptStallMs <= 0) System::PrintDiagnosticErrorLine("[selftest] late script stall expected <tick>:<ms>, got " + spec);
			i += 2;
			continue;
		}
		if (currentArg == "-selftest-frame-stall-again" && i + 1 < argCount) {
			const std::string spec = argValue[i + 1];
			const size_t separator = spec.find(':');
			if (separator != std::string::npos) {
				s_frameStallAgainTick = std::strtoll(spec.substr(0, separator).c_str(), nullptr, 10);
				s_frameStallAgainMs = static_cast<int>(std::strtol(spec.substr(separator + 1).c_str(), nullptr, 10));
			}
			if (s_frameStallAgainMs <= 0) System::PrintDiagnosticErrorLine("[selftest] second frame stall expected <tick>:<ms>, got " + spec);
			i += 2;
			continue;
		}
		if (currentArg == "-selftest-frame-stall" && i + 1 < argCount) {
			const std::string spec = argValue[i + 1];
			const size_t separator = spec.find(':');
			if (separator != std::string::npos) {
				s_frameStallTick = std::strtoll(spec.substr(0, separator).c_str(), nullptr, 10);
				s_frameStallMs = static_cast<int>(std::strtol(spec.substr(separator + 1).c_str(), nullptr, 10));
				s_frameStallArmed = s_frameStallMs > 0;
			}
			if (!s_frameStallArmed) {
				{
					std::ostringstream line;
					line << "[selftest] frame stall expected <tick>:<ms>, got " << spec;
					System::PrintDiagnosticErrorLine(line.str());
				}
			}
			i += 2;
			continue;
		}
		if (currentArg == "-contract-audit" && i + 1 < argCount) {
			s_contractAuditOperation = argValue[i + 1];
			i += 2;
			continue;
		}
		if (currentArg == "-contract-audit-tick" && i + 1 < argCount) {
			s_contractAuditTick = std::stoll(argValue[i + 1]);
			i += 2;
			continue;
		}
		if (currentArg == "-snapshot-roundtrip-lock-audio" || currentArg == "-snapshot-roundtrip-perturb-audio" || currentArg == "-snapshot-roundtrip-check-playback") {
			if (currentArg == "-snapshot-roundtrip-lock-audio") s_snapshotRoundtripLockAudio = true;
			if (currentArg == "-snapshot-roundtrip-perturb-audio") s_snapshotRoundtripPerturbAudio = true;
			if (currentArg == "-snapshot-roundtrip-check-playback") s_snapshotRoundtripCheckPlayback = true;
			++i;
			continue;
		}
		if (currentArg == "-snapshot-roundtrip-selftest" && i + 1 < argCount) {
			s_snapshotRoundtripSelfTestName = argValue[i + 1];
			i += 2;
			continue;
		}
		if ((currentArg == "-load-io-selftest" || currentArg == "-load-io-success-selftest") && i + 1 < argCount) {
			s_loadSelfTestName = argValue[i + 1];
			s_loadSelfTestExpected = currentArg == "-load-io-success-selftest";
			i += 2;
			continue;
		}
		// The save waits this many ticks past the first one this run simulates.
		if (currentArg == "-save-io-selftest-after" && i + 1 < argCount) {
			s_saveIoSelfTestAfter = std::strtoull(argValue[i + 1], nullptr, 10);
			i += 2;
			continue;
		}
		if ((currentArg == "-save-io-selftest" || currentArg == "-save-menu-selftest") && i + 1 < argCount) {
			s_saveIoSelfTest = true;
			s_saveMenuSelfTest = currentArg == "-save-menu-selftest";
			s_saveIoSelfTestName = argValue[i + 1];
			i += 2;
			continue;
		}

		// Scenario direct-launch + determinism flags (-scenario, -seed, -max-ticks, ...).
		if (int consumed = ScenarioRunner::ParseArgs(argCount, argValue, i); consumed > 0) {
			i += consumed;
			continue;
		}

		if (!lastArg && currentArg == "-net-identity-dump") {
			s_netIdentityDumpPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-directory-probe") {
			s_netDirectoryProbeUrl = argValue[++i];
			if (i + 1 < argCount && argValue[i + 1][0] != '-') {
				s_netDirectoryProbeCertSha256 = argValue[++i];
			}
			continue;
		}

		if (!lastArg && currentArg == "-net-directory-signal-probe") {
			s_netDirectorySignalProbeUrl = argValue[++i];
			if (i + 1 < argCount && argValue[i + 1][0] != '-') {
				s_netDirectorySignalProbeCertSha256 = argValue[++i];
			}
			continue;
		}

		if (currentArg == "-net-directory-list") {
			s_netDirectoryList = true;
			++i;
			continue;
		}

		if (currentArg == "-net-host") {
			s_netHost = true;
			++i;
			continue;
		}

		if (currentArg == "-net-dedicated") {
			s_netDedicated = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-join") {
			s_netJoinAddress = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-join-session") {
			s_netJoinSessionId = argValue[++i];
			continue;
		}

		// Run overrides: they decide this run and are never written back to Settings.ini.
		if (!lastArg && currentArg == "-net-ice") {
			g_SettingsMan.SetNetworkIceEnableOverride(std::string(argValue[++i]) == "on");
			continue;
		}

		if (!lastArg && currentArg == "-net-stun") {
			g_SettingsMan.SetNetworkStunServersOverride(argValue[++i]);
			continue;
		}

		if (!lastArg && currentArg == "-net-turn") {
			g_SettingsMan.SetNetworkTurnServersOverride(argValue[++i]);
			continue;
		}

		if (!lastArg && currentArg == "-net-port") {
			const long parsedPort = std::strtol(argValue[++i], nullptr, 10);
			if (parsedPort > 0 && parsedPort <= 65535) {
				s_netPort = static_cast<uint16_t>(parsedPort);
			}
			continue;
		}

		if (!lastArg && currentArg == "-net-port-map") {
			const std::string value = argValue[++i];
			s_netPortMapCli = (value == "on" || value == "true" || value == "1") ? 1 : 0;
			continue;
		}

		if (currentArg == "-net-port-map-probe") {
			s_netPortMapProbe = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-port-map-gateway") {
			s_netPortMapGateway = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-port-map-igd") {
			s_netPortMapIgd = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-session-report") {
			s_netSessionReportPath = argValue[++i];
			continue;
		}

		if (currentArg == "-net-no-reconnect-admission") {
			NetMatchService::SetAdmissionEnabled(false);
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-reconnect-ticket") {
			NetMatchService::SetTicketStorePath(argValue[++i]);
			continue;
		}

		if (!lastArg && currentArg == "-net-host-bans") {
			NetMatchService::SetHostBanStorePath(argValue[++i]);
			continue;
		}

		if (!lastArg && currentArg == "-net-join-wait-for") {
			NetMatchService::SetJoinWaitPath(argValue[++i]);
			continue;
		}

		// Phase B, unattended gates: the joiner asks the host for a seat instead of joining one, and
		// the host approves the first applicant for that seat the way a moderator would.
		if (!lastArg && currentArg == "-net-h4-apply") {
			NetMatchService::SetApplyForSeat(true, static_cast<uint16_t>(std::stoi(argValue[++i])));
			continue;
		}

		if (!lastArg && currentArg == "-net-h4-substitute") {
			s_netH4Substitute = true;
			s_netH4SubstituteSeat = static_cast<uint16_t>(std::stoi(argValue[++i]));
			NetMatchService::SetAutoSubstitute(s_netH4Substitute, s_netH4SubstituteSeat, s_netH4SubstituteDelayMs, s_netH4SubstituteCancel);
			continue;
		}

		if (!lastArg && currentArg == "-net-h4-substitute-delay") {
			s_netH4SubstituteDelayMs = static_cast<uint64_t>(std::stoll(argValue[++i]));
			NetMatchService::SetAutoSubstitute(s_netH4Substitute, s_netH4SubstituteSeat, s_netH4SubstituteDelayMs, s_netH4SubstituteCancel);
			continue;
		}

		if (currentArg == "-net-h4-substitute-cancel") {
			s_netH4SubstituteCancel = true;
			NetMatchService::SetAutoSubstitute(s_netH4Substitute, s_netH4SubstituteSeat, s_netH4SubstituteDelayMs, s_netH4SubstituteCancel);
			++i;
			continue;
		}

		if (currentArg == "-net-exit-after-ready") {
			s_netExitAfterReady = true;
			++i;
			continue;
		}

		if (currentArg == "-net-allow-userdata") {
			s_netAllowUserdata = true;
			++i;
			continue;
		}

		if (currentArg == "-net-lockstep") {
			s_netLockstep = true;
			++i;
			continue;
		}

		if (currentArg == "-net-match") {
			s_netMatch = true;
			++i;
			continue;
		}

		if (currentArg == "-net-match-service-e2e") {
			s_netMatchServiceE2E = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-match-service-preset") {
			s_netMatchServiceE2EPreset = argValue[++i];
			s_netMatchServicePresetExplicit = true;
			continue;
		}
		// The module the preset belongs to; without it the service resolves the preset's own module.
		if (!lastArg && currentArg == "-net-match-service-module") {
			s_netMatchServiceE2EModule = argValue[++i];
			continue;
		}
		if (!lastArg && currentArg == "-net-match-service-config") {
			s_netMatchServiceConfigPath = argValue[++i];
			continue;
		}
		if (!lastArg && currentArg == "-net-match-service-scene") {
			s_netMatchServiceE2EScene = argValue[++i];
			continue;
		}
		if (!lastArg && currentArg == "-net-match-service-scene-module") {
			s_netMatchServiceE2ESceneModule = argValue[++i];
			continue;
		}
		// The seat name this peer announces; without it the e2e path still defaults to Host/Client.
		if (!lastArg && currentArg == "-net-player-name") {
			const std::string name = argValue[++i];
			// Past the hello's byte cap the encode would refuse it mid-start, so the flag is refused here.
			if (name.size() > NetLobbyProtocol::c_MaxDisplayNameBytes) {
				std::cerr << "-net-player-name over the 64-byte cap; the peer announces its default seat name" << std::endl;
			} else {
				s_netPlayerName = name;
			}
			continue;
		}

		if (!lastArg && currentArg == "-menu-script") {
			s_menuScriptPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-menu-script-out") {
			s_menuScriptOutDir = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-lockstep-report") {
			s_netLockstepReportPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-match-report") {
			s_netLockstepReportPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-lockstep-ticks") {
			s_netLockstepTicks = static_cast<uint64_t>(std::strtoull(argValue[++i], nullptr, 10));
			s_netMatchTicksExplicit = true;
			continue;
		}

		if (!lastArg && currentArg == "-net-match-ticks") {
			s_netLockstepTicks = static_cast<uint64_t>(std::strtoull(argValue[++i], nullptr, 10));
			s_netMatchTicksExplicit = true;
			continue;
		}

		if (currentArg == "-net-persistent-world") {
			s_netPersistentWorld = true;
			++i;
			continue;
		}

		if (currentArg == "-net-world-fresh") {
			s_netWorldFresh = true;
			++i;
			continue;
		}

		if (currentArg == "-net-match-e2e-brain-placement") {
			// Stand in for each local player's DONE in the setup editor: place this peer's own seats' brains.
			s_netMatchE2EBrainPlacement = true;
			++i;
			continue;
		}

		if (currentArg == "-net-match-screenshot-ticks") {
			const std::string list = lastArg ? "" : argValue[++i];
			std::istringstream entries(list);
			std::string entry;
			bool valid = !list.empty() && list.back() != ',';
			while (std::getline(entries, entry, ',')) {
				uint64_t tick = 0;
				std::istringstream number(entry);
				if (entry.empty() || !std::all_of(entry.begin(), entry.end(), [](char c) { return c >= '0' && c <= '9'; }) || !(number >> tick) || !number.eof() || tick == 0) {
					valid = false;
					break;
				}
				s_netMatchScreenshotTicks.insert(tick);
			}
			if (!valid || s_netMatchScreenshotTicks.size() > 32) {
				{
					std::ostringstream line;
					line << "[net-match-screenshot] expected 1-32 positive, comma-separated applied ticks";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			continue;
		}

		if (!lastArg && currentArg == "-net-lockstep-input-delay") {
			const unsigned long parsedDelay = std::strtoul(argValue[++i], nullptr, 10);
			s_netLockstepInputDelay = static_cast<uint16_t>(std::min<unsigned long>(parsedDelay, NetLockstepCodec::c_MaxInputDelayFrames));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-input-delay") {
			const unsigned long parsedDelay = std::strtoul(argValue[++i], nullptr, 10);
			s_netLockstepInputDelay = static_cast<uint16_t>(std::min<unsigned long>(parsedDelay, NetLockstepCodec::c_MaxInputDelayFrames));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-spawn") {
			E2eNamedSpawn spec;
			if (!ParseE2eSpawnSpec(argValue[++i], spec)) {
				{
					std::ostringstream line;
					line << "[net-match-e2e-spawn] expected Class:Preset:Module:x:y:tick[:team]";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_e2eNamedSpawns.push_back(spec);
			continue;
		}

		if (!lastArg && currentArg == "-net-chat-script") {
			s_netChatScriptPath = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-match-ownership-policy") {
			s_netMatchOwnershipPolicy = argValue[++i];
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-switch-control") {
			s_netMatchE2eSwitchControlTick = std::strtoull(argValue[++i], nullptr, 10);
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-brain-damage") {
			s_netMatchE2eBrainDamageTick = std::strtoull(argValue[++i], nullptr, 10);
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-brain-reseat") {
			s_netMatchE2eBrainReseatTick = std::strtoull(argValue[++i], nullptr, 10);
			continue;
		}

		if (!lastArg && currentArg == "-net-match-peers") {
			const unsigned long parsedPeers = std::strtoul(argValue[++i], nullptr, 10);
			s_netMatchPeers = static_cast<uint8_t>(std::clamp<unsigned long>(parsedPeers, 2, NetMatchConfigUtil::c_MaxPeerCount));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-mode") {
			s_netMatchMode = argValue[++i];
			continue;
		}
		if (currentArg == "-net-match-humans" || currentArg == "-net-match-cpu-slots") {
			uint32_t count = 0;
			const std::string value = lastArg ? "" : argValue[i + 1];
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
			if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
				{
					std::ostringstream line;
					line << "[net-match-service-e2e] " << currentArg << " requires a nonnegative 32-bit integer";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			(currentArg == "-net-match-humans" ? s_netMatchHumans : s_netMatchCPUSlots) = count;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-match-brainless-spectate") {
			const std::string value = argValue[++i];
			if (value != "0" && value != "1") {
				{
					std::ostringstream line;
					line << "[net-match] -net-match-brainless-spectate requires 0 or 1";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_netMatchBrainlessSpectate = value == "1";
			continue;
		}

		if (currentArg == "-net-match-e2e-resync") {
			s_netMatchResyncOnDesync = true;
			++i;
			continue;
		}

		// A measurement run that must play past a divergence instead of stopping on it opts out here.
		// Never a default and never passed by a gate script: an unarmed detector is a silent desync.
		if (!lastArg && currentArg == "-net-desync-check") {
			s_netDesyncCheck = std::string(argValue[++i]) != "off";
			continue;
		}

		if (!lastArg && currentArg == "-net-h4-fault") {
			const std::string kind = argValue[++i];
			NetH4SetFault(NetH4FaultFromName(kind));
			{
				std::ostringstream line;
				line << "[net-h4-fault] armed " << kind;
				System::PrintDiagnosticLine(line.str());
			}
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-moderate") {
			// Repeatable: the actions run in order, one per delay, on the one seat.
			s_netMatchE2eModerate.emplace_back(argValue[++i]);
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-moderate-seat") {
			s_netMatchE2eModerateSeat = static_cast<int>(std::strtol(argValue[++i], nullptr, 10));
			continue;
		}

		if (!lastArg && currentArg == "-net-match-e2e-moderate-delay") {
			s_netMatchE2eModerateDelayMs = std::strtoull(argValue[++i], nullptr, 10);
			continue;
		}

		if (currentArg == "-telemetry-bundle") {
			s_telemetryBundleOnExit = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-replay-out") {
			s_netReplayOutPath = argValue[++i];
			ScenarioRunner::ArmLockstepReplayRecord(s_netReplayOutPath);
			continue;
		}
		if (currentArg == "-net-autosave-restore") {
			const std::string value = lastArg ? "" : argValue[i + 1];
			if (value == "oldest" || value == "newest") {
				// Named by place, for a caller that cannot know which ticks the cadence lands on; it must
				// say when to restore with -net-autosave-restore-at.
				s_netAutosaveRestoreWhich = value;
				i += 2;
				continue;
			}
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), s_netAutosaveRestoreTick);
			if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || s_netAutosaveRestoreTick == 0) {
				{
					std::ostringstream line;
					line << "[autosave] -net-autosave-restore requires the committed tick to restore, or oldest or newest";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			i += 2;
			continue;
		}
		if (currentArg == "-net-resume-match") {
			// Restart a match that ended with its host, from the checkpoints and manifest it left behind.
			const std::string value = lastArg ? "" : argValue[i + 1];
			if (value.empty()) {
				{
					std::ostringstream line;
					line << "[autosave] -net-resume-match requires the match id to restart";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_netResumeMatchId = value;
			++i;
			continue;
		}
		if (currentArg == "-net-resume-tick") {
			const std::string value = lastArg ? "" : argValue[i + 1];
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), s_netResumeTick);
			if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || s_netResumeTick == 0) {
				{
					std::ostringstream line;
					line << "[autosave] -net-resume-tick requires the committed tick to resume from";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			i += 2;
			continue;
		}
		if (currentArg == "-net-autosave-restore-at") {
			// The tick the restore runs at, so a run can restore an older checkpoint after later ones exist.
			const std::string value = lastArg ? "" : argValue[i + 1];
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), s_netAutosaveRestoreAtTick);
			if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || s_netAutosaveRestoreAtTick == 0) {
				{
					std::ostringstream line;
					line << "[autosave] -net-autosave-restore-at requires the positive sim tick to restore at";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			i += 2;
			continue;
		}
		if (currentArg == "-net-autosave-seconds") {
			uint32_t seconds = 0;
			const std::string value = lastArg ? "" : argValue[i + 1];
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
			if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
				{
					std::ostringstream line;
					line << "[autosave] -net-autosave-seconds requires a nonnegative 32-bit integer";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			NetMatchService::SetAutosaveSeconds(seconds);
			i += 2;
			continue;
		}

		if (!lastArg && currentArg == "-net-fake-lag") {
			GnsTransport::SetSimulatedLagMs(static_cast<int>(std::strtol(argValue[++i], nullptr, 10)));
			continue;
		}
		if (!lastArg && currentArg == "-net-fake-jitter") {
			GnsTransport::SetSimulatedJitterMs(static_cast<int>(std::strtol(argValue[++i], nullptr, 10)));
			continue;
		}
		if (!lastArg && currentArg == "-net-fake-reorder") {
			GnsTransport::SetSimulatedReorderPercent(std::strtof(argValue[++i], nullptr));
			continue;
		}
		if (!lastArg && currentArg == "-net-fake-dup") {
			GnsTransport::SetSimulatedDuplicatePercent(std::strtof(argValue[++i], nullptr));
			continue;
		}
		if (!lastArg && currentArg == "-net-rendezvous-log") {
			GnsTransport::SetRendezvousLogLevel(static_cast<int>(std::strtol(argValue[++i], nullptr, 10)));
			continue;
		}
		if (!lastArg && currentArg == "-feel-render-settings") {
			if (!FrameMan::SetFeelRenderSettings(argValue[++i])) {
				{
					std::ostringstream line;
					line << "[feel] invalid render settings: expected RenderCapHz = 0 or 60";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			continue;
		}
		if (!lastArg && currentArg == "-feel-measure") {
			if (!FrameMan::SetFeelRecordDirectory(argValue[++i])) {
				{
					std::ostringstream line;
					line << "[feel] recording requires CCCP_HEADLESS=1 and a fresh existing output directory";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			continue;
		}

		if (!lastArg && currentArg == "-net-local-prediction") {
			LocalPrediction::SetCommandLineOverride(std::string(argValue[++i]) == "off" ? 0 : 1);
			continue;
		}

		if (!lastArg && currentArg == "-rollback-fidelity-probe") {
			// T:K — capture after tick T, gate K re-run ticks against the first pass.
			const std::string probeSpec = argValue[++i];
			const size_t colon = probeSpec.find(':');
			s_rbProbeAtTick = std::strtoll(probeSpec.c_str(), nullptr, 10);
			s_rbProbeRequested = true;
			if (colon != std::string::npos) {
				s_rbProbeWindow = std::strtoll(probeSpec.c_str() + colon + 1, nullptr, 10);
			}
			continue;
		}

		if (!lastArg && currentArg == "-rollback-fidelity-probe-mode") {
			const std::string mode = argValue[++i];
			s_rbProbeUseLoadGame = mode == "launch";
			s_rbProbeInMemory = mode != "file" && !s_rbProbeUseLoadGame;
			continue;
		}

		if (!lastArg && currentArg == "-rollback-fidelity-fuzz") {
			// seed:count:K — count random capture ticks in one process, each gated over K re-run ticks.
			const std::string spec = argValue[++i];
			const size_t first = spec.find(':');
			const size_t second = first == std::string::npos ? std::string::npos : spec.find(':', first + 1);
			s_rbProbeFuzzSeed = std::strtoull(spec.c_str(), nullptr, 10);
			s_rbProbeRequested = true;
			if (first != std::string::npos) {
				s_rbProbeFuzzCount = static_cast<int>(std::strtol(spec.c_str() + first + 1, nullptr, 10));
			}
			if (second != std::string::npos) {
				s_rbProbeWindow = std::strtoll(spec.c_str() + second + 1, nullptr, 10);
			}
			continue;
		}

		if (currentArg == "-net-match-auto-delay") {
			s_netMatchAutoDelay = true;
			++i;
			continue;
		}

		if (!lastArg && currentArg == "-net-replay") {
			s_netReplayInPath = argValue[++i];
			continue;
		}
		if (!lastArg && currentArg == "-net-replay-verify") {
			s_netReplayVerifyPath = argValue[++i];
			continue;
		}
		if (!lastArg && currentArg == "-net-replay-dump") {
			// <from>:<to> — with -net-replay-verify, print the recorded frames and commands of those ticks.
			const std::string spec = argValue[++i];
			const size_t colon = spec.find(':');
			if (colon == std::string::npos) {
				{
					std::ostringstream line;
					line << "[net-replay-dump] bad range '" << spec << "': expected <from>:<to>";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_netReplayDumpFrom = std::strtoull(spec.c_str(), nullptr, 10);
			s_netReplayDumpTo = std::strtoull(spec.c_str() + colon + 1, nullptr, 10);
			continue;
		}
		if (!lastArg && currentArg == "-input-script") {
			// A fixture's inputs stand in for the player's devices at the UInputMan boundary.
			std::string scriptError;
			if (!InputScript::Load(argValue[++i], &scriptError)) {
				{
					std::ostringstream line;
					line << "[input-script] " << scriptError;
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			continue;
		}
		if (!lastArg && currentArg == "-ai-write-script") {
			// A fixture's direct AI writes on a local actor, made inside the owner's AI pass.
			std::string scriptError;
			if (!AIWriteScript::Load(argValue[++i], &scriptError)) {
				{
					std::ostringstream line;
					line << "[ai-write-script] " << scriptError;
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			continue;
		}
		if (!lastArg && currentArg == "-digital-aim-speed") {
			// <player>:<multiplier> — this machine's digital aim speed for the player, a per-machine setting the sim may only read off the wire.
			const std::string spec = argValue[++i];
			const size_t colon = spec.find(':');
			float speed = 0.0F;
			const bool whole = colon != std::string::npos && ParseNumberExact(spec.data() + colon + 1, spec.data() + spec.size(), speed).ptr == spec.data() + spec.size();
			const int player = colon == std::string::npos ? -1 : std::atoi(spec.substr(0, colon).c_str());
			if (colon == std::string::npos || player < 0 || player >= Players::MaxPlayerCount || !whole || !(speed > 0.0F)) {
				{
					std::ostringstream line;
					line << "[digital-aim-speed] bad spec '" << spec << "': expected <player>:<multiplier>";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			g_UInputMan.GetControlScheme(player)->SetDigitalAimSpeed(speed);
			{
				std::ostringstream line;
				line << "[digital-aim-speed] player " << player << " -> " << speed;
				System::PrintDiagnosticLine(line.str());
			}
			continue;
		}
		if (!lastArg && currentArg == "-local-prediction-depth") {
			const std::string text = argValue[++i];
			if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
				{
					std::ostringstream line;
					line << "[localpred] bad depth '" << text << "': expected a whole number";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			LocalPrediction::SetDepthOverride(static_cast<int>(std::strtol(text.c_str(), nullptr, 10)));
			continue;
		}
		if (!lastArg && currentArg == "-local-prediction-event-ledger") {
			// The tick the tracked press is sampled at; the shot's sound must be audible by the next one.
			const std::string text = argValue[++i];
			if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
				{
					std::ostringstream line;
					line << "[preview-event-selftest] bad press tick '" << text << "': expected a whole number";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_eventLedgerPressTick = std::strtoll(text.c_str(), nullptr, 10);
			continue;
		}
		if (currentArg == "-local-prediction-subtree-emitter") {
			PreviewScriptSelfTest::SetSubtreeProbe(true);
		}
		if (currentArg == "-local-prediction-shared-slot") {
			PreviewScriptSelfTest::SetSharedSlot(true);
		}
		if (!lastArg && currentArg == "-local-prediction-hud") {
			const std::string text = argValue[++i];
			if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
				{
					std::ostringstream line;
					line << "[preview-hud-selftest] bad press tick '" << text << "': expected a whole number";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			LocalPredictionHudSelfTest::g_PressTick = std::strtoll(text.c_str(), nullptr, 10);
			continue;
		}
		if (!lastArg && currentArg == "-local-prediction-funds-preview") {
			const std::string text = argValue[++i];
			if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
				{
					std::ostringstream line;
					line << "[preview-funds-driver] bad press tick '" << text << "': expected a whole number";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_fundsPreviewPress = std::strtoll(text.c_str(), nullptr, 10);
			continue;
		}
		if (!lastArg && currentArg == "-local-prediction-invariance") {
			// T:d1,d2,...:r1,r2,... — at tick T run previews of each depth, each repeat count, and prove the canonical world untouched.
			const std::string spec = argValue[++i];
			const auto parsePositive = [](const std::string& text, long long& out) {
				if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
					return false;
				}
				out = std::strtoll(text.c_str(), nullptr, 10);
				return out > 0;
			};
			const auto parseList = [&parsePositive](const std::string& text, std::vector<int>& out) {
				size_t start = 0;
				while (true) {
					const size_t comma = text.find(',', start);
					long long value = 0;
					if (!parsePositive(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start), value)) {
						return false;
					}
					out.push_back(static_cast<int>(value));
					if (comma == std::string::npos) {
						return true;
					}
					start = comma + 1;
				}
			};
			const size_t first = spec.find(':');
			const size_t second = first == std::string::npos ? std::string::npos : spec.find(':', first + 1);
			long long tick = 0;
			bool ok = parsePositive(spec.substr(0, first), tick);
			if (ok && first != std::string::npos) {
				const std::string depths = spec.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
				ok = depths.empty() || parseList(depths, s_lpInvarianceDepths);
			}
			if (ok && second != std::string::npos) {
				const std::string repeats = spec.substr(second + 1);
				ok = repeats.empty() || parseList(repeats, s_lpInvarianceRepeats);
			}
			if (!ok) {
				{
					std::ostringstream line;
					line << "[lpinv] bad spec '" << spec << "': expected T:d1,d2,...:r1,r2,... with positive whole numbers";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_lpInvarianceTick = tick;
			if (s_lpInvarianceDepths.empty()) {
				s_lpInvarianceDepths = {1, 3, 8, 14};
			}
			if (s_lpInvarianceRepeats.empty()) {
				s_lpInvarianceRepeats = {1, 3};
			}
			continue;
		}
		if (!lastArg && currentArg == "-lpinv-expect") {
			// equip=<preset>@<tick>[~<slack>],fire@<tick>[~<slack>] — the canonical ticks the previews must reproduce once their horizon passes them.
			const std::string spec = argValue[++i];
			bool ok = !spec.empty();
			size_t start = 0;
			while (ok && start < spec.size()) {
				const size_t comma = spec.find(',', start);
				const std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				const size_t at = item.rfind('@');
				const size_t tilde = item.find('~', at == std::string::npos ? 0 : at);
				const std::string tickText = at == std::string::npos ? std::string() : item.substr(at + 1, tilde == std::string::npos ? std::string::npos : tilde - at - 1);
				const std::string slackText = tilde == std::string::npos ? std::string("0") : item.substr(tilde + 1);
				if (at == std::string::npos || tickText.empty() || tickText.find_first_not_of("0123456789") != std::string::npos || slackText.empty() || slackText.find_first_not_of("0123456789") != std::string::npos) {
					ok = false;
					break;
				}
				const long long tick = std::strtoll(tickText.c_str(), nullptr, 10);
				const long long slack = std::strtoll(slackText.c_str(), nullptr, 10);
				if (item.rfind("equip=", 0) == 0 && at > 6) {
					s_lpExpectEquip = item.substr(6, at - 6);
					s_lpExpectEquipTick = tick;
					s_lpExpectEquipSlack = slack;
				} else if (item.substr(0, at) == "fire") {
					s_lpExpectFireTick = tick;
					s_lpExpectFireSlack = slack;
				} else {
					ok = false;
				}
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
			if (!ok || (s_lpExpectEquip.empty() && s_lpExpectFireTick <= 0)) {
				{
					std::ostringstream line;
					line << "[lpinv] bad expectation '" << spec << "': expected equip=<preset>@<tick>,fire@<tick>";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			continue;
		}
		if (currentArg == "-preview-binding-exhaustive-selftest") {
			// Fixtures join the world at 150 and the walk runs at 154, off the 30-tick desync sample; the row plays the pickup_fire replay.
			LuaBindingExhaustiveSelfTest::Arm(150, 154);
		}
		if (!lastArg && currentArg == "-lpinv-overlay-links") {
			// b spawn and shadow links, r a shadow item in reach, c spawn parts, wounds and a shadow part.
			const std::string modes = argValue[++i];
			if (modes.empty() || modes.find_first_not_of("brcitmqxonlap") != std::string::npos) {
				{
					std::ostringstream line;
					line << "[lpinv] bad overlay-link modes '" << modes << "': expected letters from brcitmqxonlap";
					System::PrintDiagnosticErrorLine(line.str());
				}
				return false;
			}
			s_lpOverlayLinkModes = modes;
			continue;
		}

		if (!lastArg && !singleModuleSet && currentArg == "-module") {
			std::string moduleToLoad = argValue[++i];
			if (moduleToLoad.find(System::GetModulePackageExtension()) == moduleToLoad.length() - System::GetModulePackageExtension().length()) {
				g_PresetMan.SetSingleModuleToLoad(moduleToLoad);
				singleModuleSet = true;
			}
		}
		if (!launchModeSet) {
			if (!lastArg && currentArg == "-editor") {
				g_ActivityMan.SetEditorToLaunch(argValue[++i]);
				launchModeSet = true;
			}
		}
		++i;
	}
	if (launchModeSet) {
		g_SettingsMan.SetSkipIntro(true);
	}
	if (s_globalCallbacksSelfTest && s_netReplayInPath.empty() && !ScenarioRunner::IsActive()) {
		{
			std::ostringstream line;
			line << "[global-callback-selftest] REFUSE needs -net-replay <recording> or -scenario, and UserScenes.rte Checkpoint Global";
			System::PrintDiagnosticLine(line.str());
		}
		return false;
	}
	return true;
}

static bool FrameRecorderSelfTestFail(const std::string& reason) {
	std::ostringstream line;
	line << "[frame-recorder-selftest] FAIL: " << reason;
	System::PrintDiagnosticErrorLine(line.str());
	return false;
}

/// Feeds an argument vector through the real parser, so a handler that does not advance the loop
/// is caught where the loop lives rather than in a run that hangs.
static bool FrameRecorderParse(std::vector<std::string> arguments) {
	std::vector<char*> argv;
	argv.reserve(arguments.size());
	for (std::string& argument: arguments) argv.push_back(argument.data());
	return HandleMainArgs(static_cast<int>(argv.size()), argv.data());
}

/// The recorder's own rows: the flag parses and advances, and the capture rate and the queue bound
/// account for every frame the manifest reports.
static bool RunFrameRecorderSelfTest() {
	const std::string savedDirectory = s_recordVideoDirectory;
	const int savedFps = s_recordVideoFps;
	const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "frame-recorder-selftest";
	std::error_code code;
	std::filesystem::remove_all(scratch, code);
	if (!std::filesystem::create_directories(scratch, code) || code) return FrameRecorderSelfTestFail("could not create " + scratch.string());

	const std::string armed = (scratch / "armed").generic_string();
	s_recordVideoDirectory.clear();
	s_recordVideoFps = FrameRecorder::c_DefaultFps;
	if (!FrameRecorderParse({"exe", "-record-video", armed, "-record-video-fps", "5", "-record-video-fps", "7"})) {
		return FrameRecorderSelfTestFail("the parser refused a well-formed -record-video pair");
	}
	if (s_recordVideoDirectory != armed) return FrameRecorderSelfTestFail("-record-video kept '" + s_recordVideoDirectory + "' instead of '" + armed + "'");
	// The second rate is only reached if the first pair advanced the loop past its own value.
	if (s_recordVideoFps != 7) return FrameRecorderSelfTestFail("-record-video-fps kept " + std::to_string(s_recordVideoFps) + " instead of 7");
	for (const std::string& rate: {std::string("0"), std::string("61"), std::string("half")}) {
		if (FrameRecorderParse({"exe", "-record-video-fps", rate})) return FrameRecorderSelfTestFail("-record-video-fps accepted '" + rate + "'");
	}
	if (FrameRecorderParse({"exe", "-record-video"})) return FrameRecorderSelfTestFail("-record-video accepted a missing directory");
	s_recordVideoDirectory = savedDirectory;
	s_recordVideoFps = savedFps;

	const int width = 4;
	const int height = 2;
	const std::size_t bytes = static_cast<std::size_t>(width) * height * 3;
	// One synthetic frame per fake 100 ms, ten of them across one fake second.
	const auto feed = [&](FrameRecorder& recorder) {
		for (int index = 0; index < 10; ++index) {
			FrameRecorder::FrameMeta meta;
			meta.wallMS = index * 100;
			meta.simTick = static_cast<unsigned long long>(index);
			meta.screen = "game";
			meta.width = width;
			meta.height = height;
			if (unsigned char* pixels = recorder.BeginFrame(meta.wallMS, bytes)) {
				std::fill(pixels, pixels + bytes, static_cast<unsigned char>(index * 20));
				recorder.EndFrame(meta);
			}
		}
	};
	const auto manifestOf = [](const std::filesystem::path& directory, nlohmann::json& parsed) {
		std::ifstream in(directory / "manifest.json");
		if (!in) return false;
		parsed = nlohmann::json::parse(in, nullptr, false);
		return !parsed.is_discarded();
	};

	const std::filesystem::path paced = scratch / "paced";
	if (!std::filesystem::create_directory(paced, code) || code) return FrameRecorderSelfTestFail("could not create " + paced.string());
	std::string error;
	FrameRecorder pacedRecorder;
	if (!pacedRecorder.Start(paced.string(), 5, &error)) return FrameRecorderSelfTestFail("the paced recorder refused to start: " + error);
	if (pacedRecorder.Start(paced.string(), 5, &error)) return FrameRecorderSelfTestFail("a second Start on the same recorder was accepted");
	// A recording whose engine is killed never reaches Finish: its manifest is on disk from the start.
	nlohmann::json started;
	if (!manifestOf(paced, started) || started.value("fps", 0) != 5) return FrameRecorderSelfTestFail("no manifest with the capture rate before Finish in " + paced.string());
	int finishActions = 0;
	pacedRecorder.SetFinishAction([&finishActions] { ++finishActions; });
	feed(pacedRecorder);
	pacedRecorder.Finish();
	pacedRecorder.Finish();
	if (finishActions != 1) return FrameRecorderSelfTestFail("the finish action must run exactly once");
	nlohmann::json manifest;
	if (!manifestOf(paced, manifest)) return FrameRecorderSelfTestFail("no readable manifest in " + paced.string());
	if (manifest.value("schema", 0) != 1 || manifest.value("fps", 0) != 5) return FrameRecorderSelfTestFail("manifest schema/fps: " + manifest.dump());
	if (manifest.value("frames_saved", -1) != 5) return FrameRecorderSelfTestFail("at 5 fps over a fake second the manifest saved " + manifest.dump());
	if (manifest.value("frames_rate_limited", -1) != 5) return FrameRecorderSelfTestFail("the rate limit did not account for the rest: " + manifest.dump());
	if (manifest.value("frames_dropped", -1) != 0 || manifest.value("write_failures", -1) != 0) return FrameRecorderSelfTestFail("unexpected drops or write failures: " + manifest.dump());
	if (manifest.value("first_sim_tick", -1) != 0 || manifest.value("last_sim_tick", -1) != 8) return FrameRecorderSelfTestFail("sim tick span: " + manifest.dump());
	if (manifest.value("exe_sha256", std::string()).size() != 64) return FrameRecorderSelfTestFail("manifest carries no executable hash: " + manifest.dump());
	std::size_t pngs = 0;
	for (const auto& entry: std::filesystem::directory_iterator(paced / "frames")) pngs += entry.path().extension() == ".png" ? 1 : 0;
	if (pngs != 5) return FrameRecorderSelfTestFail("wrote " + std::to_string(pngs) + " PNGs, expected 5");
	std::ifstream index(paced / "frames.jsonl");
	std::size_t lines = 0;
	for (std::string line; std::getline(index, line);) {
		const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
		if (parsed.is_discarded() || !parsed.contains("frame") || !parsed.contains("wall_ms") || !parsed.contains("sim_tick") ||
		    !parsed.contains("screen") || !parsed.contains("resolution")) {
			return FrameRecorderSelfTestFail("frame index line " + std::to_string(lines) + " is incomplete: " + line);
		}
		if (parsed.value("frame", std::size_t(99)) != lines) return FrameRecorderSelfTestFail("frame numbers are not contiguous at " + line);
		++lines;
	}
	if (lines != 5) return FrameRecorderSelfTestFail("frame index has " + std::to_string(lines) + " lines, expected 5");

	// A recorder with no room to fall behind drops every frame it admits, and says so.
	const std::filesystem::path starved = scratch / "starved";
	if (!std::filesystem::create_directory(starved, code) || code) return FrameRecorderSelfTestFail("could not create " + starved.string());
	FrameRecorder starvedRecorder;
	if (!starvedRecorder.Start(starved.string(), FrameRecorder::c_MaxFps, 0, &error)) return FrameRecorderSelfTestFail("the starved recorder refused to start: " + error);
	feed(starvedRecorder);
	starvedRecorder.Finish();
	if (!manifestOf(starved, manifest)) return FrameRecorderSelfTestFail("no readable manifest in " + starved.string());
	if (manifest.value("frames_saved", -1) != 0 || manifest.value("frames_dropped", -1) != 10) return FrameRecorderSelfTestFail("a full queue must drop and count: " + manifest.dump());

	FrameRecorder refuser;
	if (refuser.Start((scratch / "missing").string(), 30, &error)) return FrameRecorderSelfTestFail("a missing directory was accepted");
	if (error.find((scratch / "missing").string()) == std::string::npos) return FrameRecorderSelfTestFail("the refusal does not name the directory: " + error);
	if (refuser.Start(paced.string(), 30, &error)) return FrameRecorderSelfTestFail("a non-empty directory was accepted");
	if (error.find(paced.string()) == std::string::npos) return FrameRecorderSelfTestFail("the refusal does not name the directory: " + error);

	// A line writer behind a stalled disk holds its bound, drops the rest and says so in its own file, in order.
	{
		const std::filesystem::path bounded = scratch / "bounded.txt";
		AsyncLineWriter writer(4, size_t{1} << 20);
		if (!writer.Open(bounded.string())) return FrameRecorderSelfTestFail("the bounded writer could not open " + bounded.string());
		std::promise<void> released;
		std::shared_future<void> release = released.get_future().share();
		std::promise<void> entered;
		std::future<void> stalled = entered.get_future();
		writer.WriteMade([release, &entered] { entered.set_value(); release.wait(); return std::string("stalled\n"); });
		stalled.wait();
		for (int line = 0; line < 10; ++line) writer.Write("line " + std::to_string(line));
		const unsigned long long dropped = writer.Dropped();
		released.set_value();
		// The writer takes what waited, and the report of what it dropped, before anything newer queues.
		writer.Flush();
		writer.Write("after");
		writer.Flush();
		writer.Close();
		std::ifstream in(bounded);
		std::vector<std::string> lines;
		for (std::string line; std::getline(in, line);) lines.push_back(line);
		const std::vector<std::string> expected = {"stalled", "line 0", "line 1", "line 2", "line 3", "[async-writer] dropped 6 entries (42 bytes) while the disk fell behind", "after"};
		if (dropped != 6 || lines != expected) {
			std::string seen;
			for (const std::string& line: lines) seen += "|" + line;
			return FrameRecorderSelfTestFail("a line writer behind a stalled disk dropped " + std::to_string(dropped) + " and wrote " + seen);
		}
	}

	// Every thread that logs an event - the menu's and each writer's - leaves whole lines, all of them, in the event index.
	const std::filesystem::path logged = scratch / "events";
	if (!std::filesystem::create_directory(logged, code) || code) return FrameRecorderSelfTestFail("could not create " + logged.string());
	{
		FrameRecorder eventRecorder;
		if (!eventRecorder.Start(logged.string(), 5, &error)) return FrameRecorderSelfTestFail("the event recorder refused to start: " + error);
		constexpr int c_Threads = 8;
		constexpr int c_PerThread = 2000;
		std::vector<std::thread> loggers;
		for (int thread = 0; thread < c_Threads; ++thread) {
			loggers.push_back(FloatingPointEnvironment::StartThread([&eventRecorder, thread] {
				for (int event = 0; event < c_PerThread; ++event) eventRecorder.RecordEvent("thread " + std::to_string(thread) + " event " + std::to_string(event) + " " + std::string(64, 'x'));
			}));
		}
		for (std::thread& logger: loggers) logger.join();
		eventRecorder.Finish();
		std::ifstream events(logged / "events.jsonl");
		std::size_t whole = 0, torn = 0;
		for (std::string line; std::getline(events, line);) {
			const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
			(parsed.is_discarded() || !parsed.contains("message") ? torn : whole) += 1;
		}
		if (whole != static_cast<std::size_t>(c_Threads * c_PerThread) || torn != 0) {
			return FrameRecorderSelfTestFail("events logged from " + std::to_string(c_Threads) + " threads at once left " + std::to_string(whole) + " whole lines of " +
			                                 std::to_string(c_Threads * c_PerThread) + " and " + std::to_string(torn) + " torn");
		}
	}

#ifndef _WIN32
	// The encoder starts behind a shell here: every argument reaches it whole, and one that stops reading fails the
	// recorder's writes instead of ending the process.
	{
		const char* savedEncoder = std::getenv("CCCP_TEST_RECORD_ENCODER");
		const std::optional<std::string> restoreEncoder = savedEncoder ? std::optional<std::string>(savedEncoder) : std::nullopt;
		const auto encodeWith = [&](const std::string& name, const std::string& body, int side, nlohmann::json& parsed) {
			const std::filesystem::path directory = scratch / name;
			const std::filesystem::path script = scratch / (name + ".sh");
			if (!std::filesystem::create_directory(directory, code) || code) return false;
			{
				std::ofstream out(script);
				out << "#!/bin/sh\n" << body << "\n";
			}
			std::filesystem::permissions(script, std::filesystem::perms::owner_all, code);
			setenv("CCCP_TEST_RECORD_ENCODER", script.c_str(), 1);
			FrameRecorder recorder;
			const bool started = recorder.Start(directory.string(), 5, &error);
			const std::size_t sideBytes = static_cast<std::size_t>(side) * side * 3;
			for (int index = 0; started && index < 10; ++index) {
				FrameRecorder::FrameMeta meta;
				meta.wallMS = index * 100;
				meta.simTick = static_cast<unsigned long long>(index);
				meta.screen = "game";
				meta.width = side;
				meta.height = side;
				if (unsigned char* pixels = recorder.BeginFrame(meta.wallMS, sideBytes)) {
					std::fill(pixels, pixels + sideBytes, static_cast<unsigned char>(index * 20));
					recorder.EndFrame(meta);
				}
			}
			recorder.Finish();
			if (restoreEncoder) {
				setenv("CCCP_TEST_RECORD_ENCODER", restoreEncoder->c_str(), 1);
			} else {
				unsetenv("CCCP_TEST_RECORD_ENCODER");
			}
			return started && manifestOf(directory, parsed);
		};
		const std::string reader = (scratch / "reader").string();
		nlohmann::json encoded;
		if (!encodeWith("reader", "for argument in \"$@\"; do printf '%s\\n' \"$argument\"; done > '" + reader + "/args.txt'\ncat > '" + reader + "/frames.raw'", 4, encoded)) {
			return FrameRecorderSelfTestFail("the encoder recording did not start or left no manifest: " + error);
		}
		std::ifstream argsIn(scratch / "reader" / "args.txt");
		bool padWhole = false;
		for (std::string argument; std::getline(argsIn, argument);) padWhole |= argument == "pad=ceil(iw/2)*2:ceil(ih/2)*2";
		std::error_code sizeCode;
		const auto rawBytes = std::filesystem::file_size(scratch / "reader" / "frames.raw", sizeCode);
		if (!padWhole || sizeCode || rawBytes != 5 * 4 * 4 * 3 || encoded.value("frames_saved", -1) != 5 || encoded.value("write_failures", -1) != 0) {
			return FrameRecorderSelfTestFail("the shell split the encoder's arguments or the frames never reached it: pad_whole=" + std::to_string(padWhole) +
			                                 " raw_bytes=" + std::to_string(sizeCode ? -1 : static_cast<long long>(rawBytes)) + " manifest=" + encoded.dump());
		}
		nlohmann::json stopped;
		if (!encodeWith("stopped", "exit 0", 128, stopped)) return FrameRecorderSelfTestFail("the stopped encoder's recording left no manifest: " + error);
		const bool named = stopped.contains("encoder") && stopped["encoder"].is_object() && stopped["encoder"].value("error", std::string()) == "the encoder stopped reading";
		if (stopped.value("write_failures", -1) < 1 || !named) {
			return FrameRecorderSelfTestFail("an encoder that stopped reading was not reported: " + stopped.dump());
		}
	}
#endif

	std::filesystem::remove_all(scratch, code);
	{
		std::ostringstream line;
		line << "[frame-recorder-selftest] PASS";
		System::PrintDiagnosticLine(line.str());
	}
	return true;
}

/// <summary>
/// Polls the SDL event queue and passes events to be handled by the relevant managers.
/// </summary>
void PollSDLEvents() {
	NetModerationGUIProbe::BeforePoll();
	// A device discovery can block for hundreds of milliseconds, so a lockstep match runs it on the joystick updater thread.
	UInputMan::SetJoystickUpdaterRunning(ScenarioRunner::HasLockstepCoordinator());
	UInputMan::UpdateJoysticksBeforePoll();
	SDL_Event sdlEvent;
	while (SDL_PollEvent(&sdlEvent)) {
		switch (sdlEvent.type) {
			case SDL_EVENT_QUIT :
				System::SetQuit(true);
				return;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				System::SetQuit(true);
				return;
			case SDL_EVENT_KEY_UP :
			case SDL_EVENT_KEY_DOWN :
			case SDL_EVENT_TEXT_INPUT :
			case SDL_EVENT_MOUSE_MOTION :
			case SDL_EVENT_MOUSE_BUTTON_UP :
			case SDL_EVENT_MOUSE_BUTTON_DOWN :
			case SDL_EVENT_MOUSE_WHEEL :
			case SDL_EVENT_GAMEPAD_AXIS_MOTION :
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN :
			case SDL_EVENT_GAMEPAD_BUTTON_UP :
			case SDL_EVENT_JOYSTICK_AXIS_MOTION :
			case SDL_EVENT_JOYSTICK_BUTTON_DOWN :
			case SDL_EVENT_JOYSTICK_BUTTON_UP :
			case SDL_EVENT_JOYSTICK_ADDED :
			case SDL_EVENT_JOYSTICK_REMOVED :
				g_UInputMan.HandleInputEvent(sdlEvent);
				break;
			default:
				break;
		}
		ImGui_ImplSDL3_ProcessEvent(&sdlEvent);
		if (sdlEvent.type >= SDL_EVENT_WINDOW_FIRST && sdlEvent.type <= SDL_EVENT_WINDOW_LAST) {
			g_WindowMan.QueueWindowEvent(sdlEvent);
		}
	}
}

// The e2e driver uses the same live panel as the host.
static void DriveModerationE2e() {
	if (s_netMatchE2eModerateAt >= s_netMatchE2eModerate.size()) {
		return;
	}
	NetModerationGUI* menu = g_MenuMan.GetNetworkPanel();
	if (!menu) {
		return;
	}
	const std::vector<NetH4ModerationSeat> seats = g_NetMatchService.GetModerationSeats();
	const bool anythingToDecide = std::any_of(seats.begin(), seats.end(), [](const NetH4ModerationSeat& seat) {
		return seat.dropped || seat.closed || seat.substituting;
	});
	if (!anythingToDecide) {
		return;
	}
	const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	if (s_netMatchE2eModerateReadyMs == 0) {
		s_netMatchE2eModerateReadyMs = nowMs + s_netMatchE2eModerateDelayMs;
		{
			std::ostringstream line;
			line << "[net-match-e2e] moderate armed actions=" << s_netMatchE2eModerate.size()
			     << " seat=" << s_netMatchE2eModerateSeat << " delay_ms=" << s_netMatchE2eModerateDelayMs;
			System::PrintDiagnosticLine(line.str());
		}
	}
	if (nowMs < s_netMatchE2eModerateReadyMs) {
		return;
	}
	// A substitution needs an applicant; until one turns up the panel's own button is disabled too,
	// so the driver waits exactly as a host would rather than pressing a dead button.
	const std::string& action = s_netMatchE2eModerate[s_netMatchE2eModerateAt];
	if (!menu->AutomationModerate(action, s_netMatchE2eModerateSeat)) {
		return;
	}
	{
		std::ostringstream line;
		line << "[net-match-e2e] moderate " << action << " seat=" << s_netMatchE2eModerateSeat << " done";
		System::PrintDiagnosticLine(line.str());
	}
	++s_netMatchE2eModerateAt;
	s_netMatchE2eModerateReadyMs = nowMs + s_netMatchE2eModerateDelayMs;
}

/// <summary>
/// Game menus loop.
/// </summary>
static uint64_t MenuScriptNowMs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void ConfigureMenuScriptInput(int argc, char** argv) {
	const char* probe = std::getenv("CC_TEST_NET_UI_SCRIPT");
	bool driving = probe && *probe;
	for (int i = 1; i + 1 < argc; ++i) {
		if (argv[i] && std::string_view(argv[i]) == "-menu-script" && argv[i + 1] && *argv[i + 1]) driving = true;
	}
	GUIInputWrapper::SetAutomationDriving(driving);
}

// One write per diagnostic line: a worker thread's own line can never land inside a menu-script line.
static void MenuScriptPrint(const std::string& line) {
	FrameRecorder::Instance().RecordEvent(line);
	System::PrintDiagnosticLine("[menu-script] " + line);
}

// A scripted-menu step failed: print it and exit non-zero so the automation harness can't false-green.
static void MenuScriptFail(const std::string& reason) {
	FrameRecorder::Instance().RecordEvent("FAILED: " + reason);
	System::PrintDiagnosticErrorLine("[menu-script] FAILED: " + reason);
	s_menuScriptFailed = true;
	if (!s_menuScriptObserveStep) {
		GUIInputWrapper::SetAutomationDriving(false);
		System::SetQuit(true);
	}
}

static void CompleteMenuScript() {
	MenuScriptPrint("complete");
	GUIInputWrapper::SetAutomationDriving(false);
	s_menuScriptComplete = true;
	if (!s_menuScriptHoldE2ePause) System::SetQuit(true);
}

static bool MenuScriptFileExists(const std::string& pattern) {
	const std::filesystem::path path(pattern);
	const std::string name = path.filename().string();
	const size_t star = name.find('*');
	std::error_code error;
	if (star == std::string::npos) return std::filesystem::is_regular_file(path, error);
	const std::string prefix = name.substr(0, star), suffix = name.substr(star + 1);
	std::filesystem::directory_iterator entry(path.has_parent_path() ? path.parent_path() : ".", error), end;
	for (; !error && entry != end; entry.increment(error)) {
		const std::string candidate = entry->path().filename().string();
		if (candidate.size() >= prefix.size() + suffix.size() && candidate.starts_with(prefix) && candidate.ends_with(suffix) && entry->is_regular_file(error)) return true;
	}
	return false;
}

static void FinishMenuTickHashes() {
	if (!s_menuHashCapture) return;
	g_MetricsCollector.SetNativeOutcome(!s_menuScriptFailed, s_menuScriptFailed ? "the menu script failed" : "the menu recording finished",
	                                   static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
	g_MetricsCollector.EndRun();
	const bool saved = g_MetricsCollector.WriteReport(ScenarioRunner::GetArgs().outPath);
	s_recordTickHashes = false;
	s_menuHashCapture = false;
	g_MetricsCollector.SetRecordTickHashes(false);
	MenuScriptPrint("finish_tick_hashes ticks=" + std::to_string(g_MetricsCollector.GetTickHashCount()) + " saved=" + std::to_string(saved));
	if (!saved) s_menuScriptFailed = true;
}

// The menu a hand reaches now: the pause menu over a match, the scenario picker, or the main menu's active screen.
static GUIControlManager* MenuScriptHandManager(GUIControl** modal) {
	*modal = nullptr;
	if (PauseMenuGUI* pause = g_MenuMan.GetActivePauseMenu()) return pause->AutomationManager();
	if (ScenarioGUI* scenario = ScenarioGUI::AutomationActive()) return scenario->AutomationManager();
	MainMenuGUI* menu = g_MenuMan.GetMainMenu();
	*modal = menu->AutomationModalDialog();
	return menu->AutomationManager();
}

// A press of a named control, or of a named list row, the way a mouse makes it.
static bool StartMenuScriptClick(const std::string& control, std::string& observation) {
	GUIControl* modal = nullptr;
	GUIControlManager* manager = MenuScriptHandManager(&modal);
	std::string list;
	int row = -1;
	if (!g_MenuMan.GetActivePauseMenu() && !ScenarioGUI::AutomationActive() && g_MenuMan.GetMainMenu()->AutomationRowOf(control, list, row)) {
		return MenuAutomation::HandRow(manager, list, row, 1, modal, observation);
	}
	return MenuAutomation::HandClick(manager, control, modal, observation);
}

// Menu scripts use real controls and the normal screenshot render path.
void ProcessMenuScript() {
	s_menuScriptObserveStep = false;
	ScenarioGUI* scenarioMenu = ScenarioGUI::AutomationActive();
	if (FrameRecorder::Instance().Enabled()) {
		PauseMenuGUI* pause = g_MenuMan.GetActivePauseMenu();
		g_FrameMan.RecordVideoFrame(pause ? pause->AutomationActiveScreenName() : scenarioMenu ? scenarioMenu->AutomationScreen() : g_MenuMan.GetMainMenu()->AutomationActiveScreenName(),
		                           g_NetMatchService.GetLobbySnapshot().serviceState);
	}
	NetModerationGUIProbe::AfterMenuDraw();
	static std::vector<std::string> steps;
	static std::map<std::string, std::vector<std::string>> macros; //!< "define NAME" ... "end" blocks, run by "run NAME".
	static size_t stepIndex = 0;
	static int waitFrames = 0;
	static bool loaded = false;
	static std::string waitCond;
	static int waitCondTimeout = 0;
	static uint64_t waitCondDeadlineMs = 0;
	static uint64_t waitUntilMs = 0;

	if (!loaded) {
		std::ifstream in(s_menuScriptPath);
		if (!in) {
			return MenuScriptFail("could not open menu-script file: " + s_menuScriptPath);
		}
		std::string line;
		std::vector<std::string>* defining = nullptr;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') { line.pop_back(); }
			if (line.empty() || line[0] == '#') { continue; }
			if (line.starts_with("define ")) {
				defining = &macros[line.substr(7)];
				continue;
			}
			if (defining && line == "end") {
				defining = nullptr;
				continue;
			}
			(defining ? *defining : steps).push_back(line);
		}
		loaded = true;
		MenuScriptPrint("loaded " + std::to_string(steps.size()) + " steps");
		if (steps.empty()) {
			return MenuScriptFail("menu-script has no steps: " + s_menuScriptPath);
		}
	}
	static bool introSkipped = false;
	PauseMenuGUI* pauseMenu = g_MenuMan.GetActivePauseMenu();
	if (!g_MenuMan.IsMainMenuInteractive() && !pauseMenu && !scenarioMenu) {
		// A pause transition follows the title state machine even after a long match.
		if (!introSkipped && !g_ActivityMan.ActivityPaused()) {
			g_MenuMan.SkipTitleIntroForAutomation();
		}
		return;
	}
	introSkipped = true;
	// A hand's gesture runs a phase a drawn frame; its step ends with the gesture.
	if (MenuAutomation::HandBusy()) return;
	if (!s_menuScriptHandStep.empty()) {
		bool passed = false;
		std::string observation;
		const std::string step = std::exchange(s_menuScriptHandStep, {});
		if (!MenuAutomation::HandFinished(passed, observation)) observation = "the gesture left no verdict";
		s_menuScriptObserveStep = s_menuScriptHandObserve;
		MenuScriptPrint(step + " " + observation + " " + (passed ? "PASS" : "FAIL"));
		if (!passed) return MenuScriptFail(step + " " + observation);
	}
	if (waitFrames > 0) {
		--waitFrames;
		return;
	}
	if (waitUntilMs > 0) {
		if (MenuScriptNowMs() < waitUntilMs) {
			return;
		}
		waitUntilMs = 0;
	}
	// Condition wait: block until the service reaches a member count / state, robust to variable FPS across
	// two contending instances (frame-count waits can't synchronize two real-time peers reliably).
	if (!waitCond.empty()) {
		const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
		bool met = false;
		std::string seen;
		if (waitCond.starts_with("file:")) {
			met = MenuScriptFileExists(waitCond.substr(5));
		} else if (waitCond.starts_with("row:")) {
			std::string listName;
			int row = -1;
			met = !pauseMenu && g_MenuMan.GetMainMenu()->AutomationRowOf(waitCond.substr(4), listName, row);
		} else if (waitCond.starts_with("label:")) {
			std::istringstream label(waitCond.substr(6));
			std::string control, expected, actual;
			label >> control;
			std::getline(label >> std::ws, expected);
			const bool found = pauseMenu ? pauseMenu->AutomationLabelText(control, actual) : g_MenuMan.GetMainMenu()->AutomationLabelText(control, actual);
			met = found && actual.find(expected) != std::string::npos;
			seen = " text=\"" + actual + "\"";
		} else if (waitCond.starts_with("substate:")) {
			seen = pauseMenu ? std::string(" screen=pause") : " screen=" + g_MenuMan.GetMainMenu()->AutomationMultiplayerSubScreen();
			met = !pauseMenu && g_MenuMan.GetMainMenu()->AutomationMultiplayerSubScreen() == waitCond.substr(9);
		} else if (waitCond.rfind("members:", 0) == 0) {
			met = static_cast<int>(snapshot.members.size()) >= std::atoi(waitCond.c_str() + 8);
		} else if (waitCond.rfind("connected:", 0) == 0) {
			met = std::count_if(snapshot.members.begin(), snapshot.members.end(), [](const NetLobbyMember& member) { return member.connected; }) == std::atoi(waitCond.c_str() + 10);
		} else if (waitCond == "allready") {
			met = snapshot.members.size() > 1 && std::all_of(snapshot.members.begin(), snapshot.members.end(), [](const NetLobbyMember& member) { return member.connected && member.ready; });
		} else if (waitCond.rfind("state:", 0) == 0) {
			met = snapshot.serviceState == waitCond.substr(6);
		} else if (waitCond == "remoteready") {
			met = snapshot.remoteReady;
		} else if (waitCond.starts_with("error:")) {
			met = snapshot.errorText.find(waitCond.substr(6)) != std::string::npos;
		} else if (waitCond.starts_with("activity:")) {
			met = snapshot.activityPreset.find(waitCond.substr(9)) != std::string::npos;
		} else if (waitCond.rfind("attempts:", 0) == 0) {
			met = g_NetMatchService.GetReconnectUx().GetAttempts() >= static_cast<uint32_t>(std::atoi(waitCond.c_str() + 9));
		}
		// The retry schedule is real time, so its wait is bounded in real time; every other condition
		// keeps the frame budget it has always had.
		const bool expired = waitCondDeadlineMs != 0 ? MenuScriptNowMs() >= waitCondDeadlineMs : --waitCondTimeout <= 0;
		if (met || expired) {
			MenuScriptPrint(std::format("{} -> {} (members={} state={}){}", waitCond, met ? "OK" : "TIMEOUT", snapshot.members.size(), snapshot.serviceState, met ? std::string() : seen));
			if (!met) { return MenuScriptFail("condition wait timed out: " + waitCond); }
			waitCond.clear();
			waitCondDeadlineMs = 0;
		}
		return;
	}
	if (stepIndex >= steps.size()) {
		std::string owed;
		if (!MenuAutomation::OwedPressed(owed)) return MenuScriptFail("the script never pressed what its sweeps left to it: " + owed);
		CompleteMenuScript();
		return;
	}
	std::istringstream iss(steps[stepIndex++]);
	std::string cmd;
	iss >> cmd;
	if (cmd == "observe") {
		s_menuScriptObserveStep = true;
		iss >> cmd;
	}
	MainMenuGUI* menu = g_MenuMan.GetMainMenu();
	if (MenuAutomation::Handles(cmd)) {
		std::string observation;
		const bool pass = MenuAutomation::Execute(pauseMenu ? pauseMenu->AutomationManager() : scenarioMenu ? scenarioMenu->AutomationManager() : menu->AutomationManager(),
			pauseMenu ? pauseMenu->AutomationActiveScreenName() : scenarioMenu ? scenarioMenu->AutomationScreen() : menu->AutomationActiveScreenName(), cmd, iss, observation);
		if (pass && MenuAutomation::HandBusy()) {
			s_menuScriptHandStep = cmd + " " + observation;
			s_menuScriptHandObserve = s_menuScriptObserveStep;
			return;
		}
		MenuScriptPrint(cmd + " " + observation + " " + (pass ? "PASS" : "FAIL"));
		if (!pass) return MenuScriptFail(cmd + " " + observation);
	} else if (cmd == "net_panel") {
		// The F6 seats/options panel owns its own control manager, so the script's assert_* commands
		// need this re-aim: "net_panel open" raises it, every other word runs on its controls.
		NetModerationGUI* panel = g_MenuMan.GetNetworkPanel();
		std::string inner;
		iss >> inner;
		std::string observation;
		bool pass = panel != nullptr;
		if (pass && inner == "activate") {
			std::string control;
			iss >> control;
			pass = MenuAutomation::HandClick(panel->AutomationManager(), control, nullptr, observation);
		} else if (pass && inner == "assert_label") {
			std::string control, sub, text;
			iss >> control;
			std::getline(iss, sub);
			if (!sub.empty() && sub[0] == ' ') { sub.erase(0, 1); }
			pass = panel->AutomationLabelText(control, text) && text.find(sub) != std::string::npos;
			observation = control + " \"" + sub + "\" text=\"" + text + "\"";
		} else if (pass && inner == "assert_label_absent") {
			std::string control, sub, text;
			iss >> control;
			std::getline(iss, sub);
			if (!sub.empty() && sub[0] == ' ') { sub.erase(0, 1); }
			pass = panel->AutomationLabelText(control, text) && text.find(sub) == std::string::npos;
			observation = control + " \"" + sub + "\" text=\"" + text + "\"";
		} else if (pass) {
			pass = MenuAutomation::Handles(inner) &&
			       MenuAutomation::Execute(panel->AutomationManager(), "NetSeats", inner, iss, observation);
		}
		if (pass && MenuAutomation::HandBusy()) {
			s_menuScriptHandStep = "net_panel " + inner + " " + observation;
			s_menuScriptHandObserve = s_menuScriptObserveStep;
			return;
		}
		{
			std::ostringstream line;
			line << "[menu-script] net_panel " << inner << " " << observation << " " << (pass ? "PASS" : "FAIL");
			System::PrintDiagnosticLine(line.str());
		}
		if (!pass) return MenuScriptFail("net_panel " + inner + " " + observation);
	} else if (cmd == "run") {
		std::string name;
		iss >> name;
		const auto macro = macros.find(name);
		if (macro == macros.end()) return MenuScriptFail("run names no defined macro: " + name);
		steps.insert(steps.begin() + static_cast<std::ptrdiff_t>(stepIndex), macro->second.begin(), macro->second.end());
		MenuScriptPrint("run " + name + " steps=" + std::to_string(macro->second.size()));
	} else if (cmd == "sweep") {
		// The screen's own control list becomes the steps that change each control by hand, read it back and put it back.
		std::string observation;
		std::vector<std::string> generated;
		GUIControl* modal = nullptr;
		if (!MenuAutomation::SweepSteps(MenuScriptHandManager(&modal), iss, generated, observation)) return MenuScriptFail("sweep " + observation);
		steps.insert(steps.begin() + static_cast<std::ptrdiff_t>(stepIndex), generated.begin(), generated.end());
		MenuScriptPrint("sweep steps=" + std::to_string(generated.size()) + " " + observation);
	} else if (cmd == "setup_host_port") {
		// The port a scripted host listens on, set as a player sets it: typed into Advanced's port box, taken by Apply and its
		// check, read back on the setup screen. A run picks its own so two runs on one machine never meet.
		std::string port;
		iss >> port;
		if (port.empty()) return MenuScriptFail("setup_host_port needs a port");
		const std::vector<std::string> generated = {"activate ButtonHostOptions", "wait 10", "assert_substate HostOptions", "activate TabHostPageConnection", "wait 4",
		                                            "settext TextHostNetPort " + port, "wait 4", "activate ButtonHostOptApply", "wait 6", "activate ButtonHostOptBack",
		                                            "wait 6", "assert_substate HostSetup", "assert_host_port " + port};
		steps.insert(steps.begin() + static_cast<std::ptrdiff_t>(stepIndex), generated.begin(), generated.end());
		MenuScriptPrint("setup_host_port " + port + " steps=" + std::to_string(generated.size()));
	} else if (cmd == "wait") {
		iss >> waitFrames;
	} else if (cmd == "wait_ms") {
		int milliseconds = 0;
		iss >> milliseconds;
		waitUntilMs = MenuScriptNowMs() + static_cast<uint64_t>(std::max(0, milliseconds));
	} else if (cmd == "wait_file") {
		std::string path;
		int seconds = 30;
		iss >> path >> std::ws;
		if (path.empty() || (!iss.eof() && !(iss >> seconds)) || seconds <= 0) return MenuScriptFail("wait_file requires a path and positive timeout");
		waitCond = "file:" + path;
		waitCondDeadlineMs = MenuScriptNowMs() + static_cast<uint64_t>(seconds) * 1000ULL;
	} else if (cmd == "wait_substate") {
		// A hand presses on a screen once it sees it: a join shows the lobby a few frames after its connection is up.
		std::string screen;
		int seconds = 30;
		iss >> screen >> std::ws;
		if (screen.empty() || (!iss.eof() && !(iss >> seconds)) || seconds <= 0) return MenuScriptFail("wait_substate requires a screen name and positive timeout");
		waitCond = "substate:" + screen;
		waitCondDeadlineMs = MenuScriptNowMs() + static_cast<uint64_t>(seconds) * 1000ULL;
	} else if (cmd == "wait_row") {
		// A listed game a hand will click: the join list shows it once its host's beacon or listing arrives.
		std::string row;
		int seconds = 60;
		iss >> row >> std::ws;
		if (row.empty() || (!iss.eof() && !(iss >> seconds)) || seconds <= 0) return MenuScriptFail("wait_row requires a row name and positive timeout");
		waitCond = "row:" + row;
		waitCondDeadlineMs = MenuScriptNowMs() + static_cast<uint64_t>(seconds) * 1000ULL;
	} else if (cmd == "touch_file") {
		// The other engine's wait_file: one run tells another it has seen what it waited for.
		std::string path;
		iss >> path;
		std::error_code madeError;
		if (!path.empty()) std::filesystem::create_directories(std::filesystem::path(path).parent_path(), madeError);
		std::ofstream marker(path);
		if (path.empty() || !marker) return MenuScriptFail("touch_file could not write " + path);
		MenuScriptPrint("touch_file " + path);
	} else if (cmd == "select_scene") {
		// A scene is picked the way a player picks it: a click on its site on the planet.
		std::string name, observation;
		std::getline(iss >> std::ws, name);
		const auto site = scenarioMenu ? scenarioMenu->AutomationScenePoint(name) : std::nullopt;
		if (!site || !MenuAutomation::HandClickAt(site->first, site->second, "the site of " + name, [scenarioMenu, name] { return scenarioMenu == ScenarioGUI::AutomationActive() && scenarioMenu->AutomationSceneSelected(name); }, observation)) {
			return MenuScriptFail("select_scene " + name + (site ? " " + observation : " offers no such site"));
		}
		s_menuScriptHandStep = "select_scene " + observation;
		s_menuScriptHandObserve = s_menuScriptObserveStep;
	} else if (cmd == "host_world_lobby") {
		unsigned port = 0;
		if (!(iss >> port) || port == 0 || port > UINT16_MAX) return MenuScriptFail("host_world_lobby requires a port");
		NetMatchServiceRequest request;
		request.host = request.dedicated = request.persistentWorld = request.worldFresh = true;
		request.port = static_cast<uint16_t>(port);
		request.playerName = "World host";
		request.activityPreset = "Persistent World";
		request.activityModule = "Base.rte";
		request.sceneName = "Grasslands";
		request.sceneModule = "Base.rte";
		request.peerCount = 4;
		request.humans = 1;
		request.cpuSlots = 1;
		request.autosaveSeconds = 0;
		std::string error;
		if (!g_NetMatchService.Start(request, &error)) return MenuScriptFail("host_world_lobby: " + error);
		MenuScriptPrint("host_world_lobby started");
	} else if (cmd == "wait_members") {
		int n = 0;
		iss >> n;
		waitCond = "members:" + std::to_string(n);
		waitCondTimeout = 4000;
	} else if (cmd == "wait_label") {
		std::string control, expected;
		iss >> control;
		std::getline(iss >> std::ws, expected);
		if (control.empty() || expected.empty()) return MenuScriptFail("wait_label requires a control and nonempty text");
		waitCond = "label:" + control + " " + expected;
		waitCondDeadlineMs = MenuScriptNowMs() + 60000;
	} else if (cmd == "wait_state") {
		std::string s;
		int seconds = 0;
		iss >> s;
		waitCond = "state:" + s;
		waitCondTimeout = 4000;
		// A wait that has to survive a whole match cannot be counted in menu frames: none run while
		// the match does, and a fade-out at menu frame rates burns the budget on its own.
		if ((iss >> seconds) && seconds > 0) {
			waitCondDeadlineMs = MenuScriptNowMs() + static_cast<uint64_t>(seconds) * 1000ULL;
		}
	} else if (cmd == "wait_error") {
		std::string text;
		std::getline(iss >> std::ws, text);
		if (text.empty()) return MenuScriptFail("wait_error requires a nonempty substring");
		waitCond = "error:" + text;
		waitCondTimeout = 4000;
	} else if (cmd == "wait_remote_ready") {
		waitCond = "remoteready";
		int seconds = 60;
		iss >> seconds;
		waitCondDeadlineMs = MenuScriptNowMs() + static_cast<uint64_t>(std::max(1, seconds)) * 1000ULL;
	} else if (cmd == "wait_connected") {
		int n = 0;
		iss >> n;
		waitCond = "connected:" + std::to_string(n);
		int seconds = 60;
		iss >> seconds;
		waitCondDeadlineMs = MenuScriptNowMs() + static_cast<uint64_t>(std::max(1, seconds)) * 1000ULL;
	} else if (cmd == "wait_activity") {
		std::string text;
		std::getline(iss >> std::ws, text);
		if (text.empty()) return MenuScriptFail("wait_activity requires a nonempty substring");
		// A joiner's own placeholder config already fills the roster; only a synced activity name
		// proves the host's match config actually landed.
		waitCond = "activity:" + text;
		waitCondTimeout = 4000;
	} else if (cmd == "wait_all_ready") {
		waitCond = "allready";
		waitCondTimeout = 4000;
	} else if (cmd == "wait_attempts") {
		int attempts = 0;
		int seconds = 0;
		iss >> attempts;
		if (!(iss >> seconds) || seconds <= 0) { seconds = 60; }
		waitCond = "attempts:" + std::to_string(attempts);
		waitCondDeadlineMs = MenuScriptNowMs() + static_cast<uint64_t>(seconds) * 1000ULL;
	} else if (cmd == "screenshot") {
		std::string name;
		iss >> name;
		// SaveScreenToPNG prepends System::GetScreenshotDirectory() ("ScreenShots/"); use a plain name.
		g_FrameMan.SaveScreenToPNG(name.c_str());
		MenuScriptPrint("screenshot ScreenShots/" + name + " screen=" + (pauseMenu ? std::string("Pause") : menu->AutomationActiveScreenName()));
	} else if (cmd == "activate" || cmd == "post_command") {
		std::string control, observation;
		iss >> control;
		if (!StartMenuScriptClick(control, observation)) {
			const auto lobby = g_NetMatchService.GetLobbySnapshot();
			MenuScriptPrint(cmd + " " + observation + " FAIL service=" + lobby.serviceState + " in_lobby=" + std::to_string(lobby.inLobby) +
			                " remote_ready=" + std::to_string(lobby.remoteReady) + " host=" + std::to_string(lobby.isHost));
			return MenuScriptFail(cmd + " " + observation);
		}
		s_menuScriptHandStep = cmd + " " + observation;
		s_menuScriptHandObserve = s_menuScriptObserveStep;
	} else if (cmd == "assert_control") {
		std::string control;
		iss >> control;
		const bool exists = pauseMenu ? pauseMenu->AutomationControlExists(control) : menu->AutomationControlExists(control);
		MenuScriptPrint("assert_control " + control + " " + (exists ? "PASS" : "FAIL"));
		if (!exists) { return MenuScriptFail("assert_control names no control in the skin: " + control); }
	} else if (cmd == "settext") {
		// Typed by a hand: a click into the box, the old text selected, the new text typed.
		std::string control, text, observation;
		iss >> control;
		std::getline(iss, text);
		if (!text.empty() && text[0] == ' ') { text.erase(0, 1); }
		GUIControl* modal = nullptr;
		if (!MenuAutomation::HandType(MenuScriptHandManager(&modal), control, text, false, observation)) { return MenuScriptFail("settext " + observation); }
		s_menuScriptHandStep = "settext " + observation;
		s_menuScriptHandObserve = s_menuScriptObserveStep;
	} else if (cmd == "setcheck") {
		std::string control, observation;
		int checked = 0;
		iss >> control >> checked;
		GUIControl* modal = nullptr;
		GUIControlManager* manager = MenuScriptHandManager(&modal);
		auto* box = manager ? dynamic_cast<GUICheckbox*>(manager->GetControl(control)) : nullptr;
		if (!box) { return MenuScriptFail("setcheck " + control + " is not a checkbox on this screen"); }
		if (!MenuAutomation::Visible(box)) { return MenuScriptFail("setcheck " + control + " is not on the screen"); }
		if ((box->GetCheck() == GUICheckbox::Checked) == (checked != 0)) {
			// A hand leaves a box that already reads right alone.
			MenuScriptPrint("setcheck " + control + " " + std::to_string(checked) + " already PASS");
		} else {
			if (!MenuAutomation::HandClick(manager, control, modal, observation)) { return MenuScriptFail("setcheck " + observation); }
			s_menuScriptHandStep = "setcheck " + control + " " + std::to_string(checked);
			s_menuScriptHandObserve = s_menuScriptObserveStep;
		}
	} else if (cmd == "assert_label") {
		std::string control;
		std::string sub;
		iss >> control;
		std::getline(iss, sub);
		if (!sub.empty() && sub[0] == ' ') { sub.erase(0, 1); }
		std::string text;
		const bool found = pauseMenu ? pauseMenu->AutomationLabelText(control, text) : menu->AutomationLabelText(control, text);
		const bool pass = found && text.find(sub) != std::string::npos;
		MenuScriptPrint("assert_label " + control + " \"" + sub + "\" text=\"" + text + "\" " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail("assert_label " + control + " missing substring: " + sub); }
	} else if (cmd == "assert_label_absent") {
		std::string control;
		std::string sub;
		iss >> control;
		std::getline(iss, sub);
		if (!sub.empty() && sub[0] == ' ') { sub.erase(0, 1); }
		std::string text;
		const bool found = pauseMenu ? pauseMenu->AutomationLabelText(control, text) : menu->AutomationLabelText(control, text);
		const bool pass = found && text.find(sub) == std::string::npos;
		MenuScriptPrint("assert_label_absent " + control + " \"" + sub + "\" text=\"" + text + "\" " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail("assert_label_absent " + control + " carries: " + sub); }
	} else if (cmd == "assert_screen") {
		std::string expected;
		iss >> expected;
		const std::string actual = pauseMenu ? pauseMenu->AutomationActiveScreenName() : scenarioMenu ? scenarioMenu->AutomationScreen() : menu->AutomationActiveScreenName();
		const bool pass = actual == expected;
		MenuScriptPrint("assert_screen expected=" + expected + " actual=" + actual + " " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail("assert_screen expected " + expected + " got " + actual); }
	} else if (cmd == "assert_status" || cmd == "assert_error") {
		std::string sub;
		std::getline(iss, sub);
		if (!sub.empty() && sub[0] == ' ') { sub.erase(0, 1); }
		const std::string status = cmd == "assert_error" ? menu->AutomationMultiplayerError() : menu->AutomationMultiplayerStatus();
		const bool pass = status.find(sub) != std::string::npos;
		MenuScriptPrint(cmd + " \"" + sub + "\" status=\"" + status + "\" " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail(cmd + " missing substring: " + sub); }
	} else if (cmd == "assert_substate") {
		std::string expected;
		iss >> expected;
		const std::string actual = menu->AutomationMultiplayerSubScreen();
		const bool pass = actual == expected;
		MenuScriptPrint("assert_substate expected=" + expected + " actual=" + actual + " " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail("assert_substate expected " + expected + " got " + actual); }
	} else if (cmd == "chat") {
		// The lobby's chat line, typed and sent as a player sends it: Enter for everyone, Ctrl+Enter for the team.
		std::string scope, text, observation;
		iss >> scope;
		std::getline(iss, text);
		if (!text.empty() && text[0] == ' ') { text.erase(0, 1); }
		if (text.empty() || (scope != "all" && scope != "team")) { return MenuScriptFail("chat needs all or team and a line"); }
		GUIControl* modal = nullptr;
		if (!MenuAutomation::HandType(MenuScriptHandManager(&modal), "TextLobbyChat", text, true, observation, scope == "team" ? "Left Ctrl" : "")) { return MenuScriptFail("chat " + observation); }
		s_menuScriptHandStep = "chat scope=" + scope + " " + observation;
		s_menuScriptHandObserve = s_menuScriptObserveStep;
	} else if (cmd == "record_tick_hashes") {
		if (s_recordTickHashes || !FrameRecorder::Instance().Enabled() || g_ActivityMan.IsInActivity() ||
		    g_NetMatchService.GetState() != NetMatchServiceState::Starting ||
		    ScenarioRunner::GetArgs().outPath.empty() || ScenarioRunner::GetArgs().maxTicks <= 0) {
			return MenuScriptFail("record_tick_hashes needs a lobby, -out and a positive -max-ticks");
		}
		s_recordTickHashes = true;
		s_menuHashCapture = true;
		g_MetricsCollector.BeginHostRun("Menu round", ScenarioRunner::GetArgs().seed);
		g_MetricsCollector.SetRecordTickHashes(true);
		FrameRecorder::Instance().SetFinishAction(FinishMenuTickHashes);
		MenuScriptPrint("record_tick_hashes armed for the next round");
	} else if (cmd == "dump_lobby") {
		const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
		std::string line = "dump_lobby state=" + snapshot.serviceState + " members=" + std::to_string(snapshot.members.size()) +
		                   " activity=\"" + snapshot.activityPreset + "\" module=\"" + snapshot.activityModule + "\"" +
		                   " scene=\"" + snapshot.sceneName + "\" scene_module=\"" + snapshot.sceneModule + "\"" +
		                   " error=\"" + snapshot.errorText + "\" status=\"" + snapshot.statusText + "\"" +
		                   " input_delay=\"" + snapshot.inputDelayText + "\"" +
		                   " port_map=\"" + snapshot.portMap + "\"";
		for (const NetLobbyMember& member: snapshot.members) {
			line += " | " + member.displayName + "(team" + std::to_string(static_cast<int>(member.team)) +
			        (member.isLocal ? ",local" : ",remote") + ",ping" + std::to_string(member.pingMs) + ")";
		}
		MenuScriptPrint(line);
	} else if (cmd == "goto_main") {
		menu->AutomationGoToMainScreen();
		MenuScriptPrint("goto_main screen=" + menu->AutomationActiveScreenName());
	} else if (cmd == "dump_reconnect") {
		const NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
		MenuScriptPrint("dump_reconnect screen=" + menu->AutomationActiveScreenName() +
		                " state=" + std::string(NetReconnectUx::StateName(reconnect.GetState())) +
		                " attempts=" + std::to_string(reconnect.GetAttempts()) +
		                " service=" + g_NetMatchService.GetLobbySnapshot().serviceState +
		                " status=\"" + reconnect.GetStatusText() + "\"" +
		                " offer=\"" + reconnect.GetOfferText() + "\"");
	} else if (cmd == "assert_console") {
		int expected = 0;
		iss >> expected;
		const int actual = g_ConsoleMan.IsEnabled() ? 1 : 0;
		const bool pass = actual == expected;
		MenuScriptPrint("assert_console expected=" + std::to_string(expected) + " actual=" + std::to_string(actual) + " " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail("assert_console expected " + std::to_string(expected)); }
	} else if (cmd == "assert_landing_empty") {
		const std::string status = menu->AutomationMultiplayerError();
		const bool pass = status.empty();
		MenuScriptPrint("assert_landing_empty status=\"" + status + "\" " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail("assert_landing_empty found: " + status); }
	} else if (cmd == "assert_enabled") {
		std::string control;
		int expected = 0;
		iss >> control >> expected;
		const int actual = (pauseMenu ? pauseMenu->AutomationControlEnabled(control) : menu->AutomationControlEnabled(control)) ? 1 : 0;
		// A control a player cannot see is neither usable nor unusable to them: the read fails.
		GUIControlManager* manager = pauseMenu ? pauseMenu->AutomationManager() : menu->AutomationManager();
		GUIControl* shown = manager ? manager->GetControl(control) : nullptr;
		const bool onScreen = !shown || MenuAutomation::Visible(shown);
		const bool pass = onScreen && actual == expected;
		MenuScriptPrint("assert_enabled " + control + " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual) + (onScreen ? "" : " (not on the screen)") + " " + (pass ? "PASS" : "FAIL"));
		if (!pass) { return MenuScriptFail("assert_enabled " + control + (onScreen ? " is " + std::string(actual ? "enabled" : "disabled") : std::string(" is not on the screen"))); }
	} else if (cmd == "exit") {
		std::string owed;
		if (!MenuAutomation::OwedPressed(owed)) return MenuScriptFail("the script never pressed what its sweeps left to it: " + owed);
		CompleteMenuScript();
	} else {
		return MenuScriptFail("unknown command: " + cmd);
	}
}

static void BeginHarnessCostMenuStay();
static void WriteHarnessCostMenuFrame();

void RunMenuLoop() {
	g_MenuMan.SetIsInMenuScreen(true);
	g_UInputMan.DisableKeys(false);
	g_UInputMan.TrapMousePos(false);
	BeginHarnessCostMenuStay();

	while (!System::IsSetToQuit()) {
		HarnessCost::BeginFrame();
		g_WindowMan.ClearBackbuffer();
		PollSDLEvents();

		g_WindowMan.Update();
		RTEError::DispatchPendingWorkerMessages();

		g_UInputMan.Update();
		g_TimerMan.Update();
		MemoryCensusByUptime();
		g_TimerMan.UpdateSim();
		g_AudioMan.Update();
		g_MusicMan.Update();

		if (g_WindowMan.ResolutionChanged()) {
			g_MenuMan.Reinitialize();
			g_ConsoleMan.Destroy();
			g_ConsoleMan.Initialize();
			g_LoadingScreen.CreateLoadingSplash();
			g_WindowMan.CompleteResolutionChange();
		}

		if (g_MenuMan.Update()) {
			g_UInputMan.EndFrame();
			break;
		}
		if (s_netReplayReturnPending && g_MenuMan.IsMainMenuInteractive()) {
			// Apply the playback destination after the menu-entry offers, before drawing.
			s_netReplayReturnPending = false;
			g_MenuMan.GetMainMenu()->ReturnToReplayBrowser(s_netReplayReturnStatus);
		}

		g_ConsoleMan.Update();

		TelemetryBundle::CaptureAtTickBoundary();
		g_UInputMan.EndFrame();
		g_WindowMan.GetScreenBuffer()->Begin();
		g_MenuMan.Draw();
		g_ConsoleMan.Draw(g_FrameMan.GetBackBuffer32());
		g_WindowMan.GetScreenBuffer()->End();
		g_WindowMan.UploadFrame();

		if (!s_menuScriptPath.empty()) {
			ProcessMenuScript();
		}
		WriteHarnessCostMenuFrame();
		if (!s_menuScriptPath.empty() && s_menuScriptHoldE2ePause && s_menuScriptComplete) break;
	}

	g_MenuMan.SetIsInMenuScreen(false);
}

/// <summary>
/// Local-perspective result text for a finished network match.
/// </summary>
static std::string BuildNetMatchResultText() {
	const GameActivity* gameActivity = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
	const int winnerTeam = gameActivity ? gameActivity->GetWinnerTeam() : Activity::NoTeam;
	if (winnerTeam == Activity::NoTeam) {
		return "Match over: draw";
	}
	if (g_NetMatchService.GetLocalTeam() == Activity::NoTeam) {
		return "Match over";
	}
	return winnerTeam == g_NetMatchService.GetLocalTeam() ? "Victory!" : "Defeat";
}

static std::string NetMatchEndReason(const Activity* activity) {
	const std::string stopReason = ScenarioRunner::GetLockstepStopReason();
	if (stopReason.starts_with("Complete:") && stopReason.size() > 9) {
		return stopReason.substr(9);
	}
	return (activity && activity->IsOver()) ? BuildNetMatchResultText() : "The other player left the match";
}

/// <summary>
/// Game simulation loop.
// CC_SIM_DUMP=<from>:<to> writes every MO's exact-bit state per tick beside the -out trace
// (".simdump.txt"), for host-vs-client divergence forensics.
static void DumpSimStateIfArmed(uint64_t simTick) {
	static uint64_t s_from = 1;
	static uint64_t s_to = 0;
	// Every tick's dump is written and formatted by a writer thread; the simulation only records its values.
	static AsyncLineWriter s_out;
	static bool s_checked = false;
	if (!s_checked) {
		s_checked = true;
		const char* env = std::getenv("CC_SIM_DUMP");
		unsigned long long from = 0;
		unsigned long long to = 0;
		if (env && std::sscanf(env, "%llu:%llu", &from, &to) == 2 && to >= from) {
			const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
			if (s_out.Open(base + ".simdump.txt")) {
				s_from = from;
				s_to = to;
			}
		}
	}
	if (!s_out.IsOpen() || simTick < s_from || simTick > s_to) {
		return;
	}
	const HarnessCost::SimulationSpan span;
	auto tape = std::make_shared<SimDumpTape>();
	g_MovableMan.CaptureSimState(simTick, *tape);
	// What the dump costs the simulation thread, so a harness cost is never read as the engine's own.
	static uint64_t s_ticks = 0, s_over2 = 0, s_over50 = 0, s_maxTick = 0;
	static double s_totalMs = 0, s_maxMs = 0;
	// Test lever: the simulation also writes the text itself, and the writer compares it with the tape's.
	static const bool s_compare = std::getenv("CCCP_TEST_SIM_DUMP_COMPARE") != nullptr;
	std::shared_ptr<const std::string> inlineText;
	if (s_compare) {
		std::ostringstream text;
		g_MovableMan.DumpSimState(simTick, text);
		inlineText = std::make_shared<const std::string>(std::move(text).str());
	}
	const int64_t simulationNs = span.Stop();
	HarnessCost::Charge(HarnessCost::SimDump, simulationNs);
	const double ms = static_cast<double>(simulationNs) / 1e6;
	s_out.WriteMade([tape, inlineText, simTick, last = s_to]() {
		// The writer's own work is the dump's too, charged to the frame it ends in.
		const auto began = std::chrono::steady_clock::now();
		struct Charge {
			std::chrono::steady_clock::time_point began;
			~Charge() { HarnessCost::Charge(HarnessCost::SimDump, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count()); }
		} charge{began};
		std::ostringstream text;
		tape->Replay(text);
		std::string made = std::move(text).str();
		if (inlineText) {
			static uint64_t s_compared = 0, s_mismatched = 0;
			++s_compared;
			if (made != *inlineText && ++s_mismatched <= 3) System::PrintDiagnosticLine("[sim-dump] tape mismatch tick=" + std::to_string(simTick));
			if (simTick == last) System::PrintDiagnosticLine("[sim-dump] tape compared=" + std::to_string(s_compared) + " mismatched=" + std::to_string(s_mismatched));
		}
		return made;
	});
	++s_ticks;
	s_totalMs += ms;
	s_over2 += ms > 2 ? 1 : 0;
	s_over50 += ms > 50 ? 1 : 0;
	if (ms > s_maxMs) {
		s_maxMs = ms;
		s_maxTick = simTick;
	}
	if (ms > 50) System::PrintDiagnosticLine("[sim-dump] slow tick=" + std::to_string(simTick) + " ms=" + std::to_string(ms));
	if (s_ticks % 600 == 0 || simTick == s_to) {
		std::ostringstream line;
		line << "[sim-dump] ticks=" << s_ticks << " mean_ms=" << s_totalMs / static_cast<double>(s_ticks) << " max_ms=" << s_maxMs << " max_tick=" << s_maxTick
		     << " over_2ms=" << s_over2 << " over_50ms=" << s_over50;
		System::PrintDiagnosticLine(line.str());
	}
}

// The instruments' cost receipts: one scope per process, round and unbroken run of simulated frames, naming every instrument's
// state, and inside it one line per frame with what each running instrument cost that frame.
namespace {
	struct HarnessCostScope {
		bool open = false;
		uint64_t round = 0, first = 0, last = 0;
		uint32_t segment = 0;
	};
	std::mutex s_harnessCostMutex;
	HarnessCostScope s_harnessCostScope;

	nlohmann::json HarnessCostInstruments() {
		nlohmann::json instruments = nlohmann::json::object();
		for (size_t instrument = 0; instrument < HarnessCost::InstrumentCount; ++instrument)
			instruments[HarnessCost::c_Names[instrument]] = HarnessCost::Enabled(static_cast<HarnessCost::Instrument>(instrument));
		return instruments;
	}

	nlohmann::json HarnessCostMs(const std::array<int64_t, HarnessCost::InstrumentCount>& charged) {
		nlohmann::json costs = nlohmann::json::object();
		for (size_t instrument = 0; instrument < HarnessCost::InstrumentCount; ++instrument)
			if (HarnessCost::Enabled(static_cast<HarnessCost::Instrument>(instrument))) costs[HarnessCost::c_Names[instrument]] = static_cast<double>(charged[instrument]) / 1e6;
		return costs;
	}

	void CloseHarnessCostScopeLocked() {
		if (!s_harnessCostScope.open) return;
		s_harnessCostScope.open = false;
		static const unsigned long incarnation = std::stoul(CrossEnvironment("CC_TEST_CROSS_INCARNATION", "0"));
		System::PrintDiagnosticLine("[harness-cost-scope] " + nlohmann::json{{"version", HarnessCost::c_ReceiptVersion}, {"process", System::GetProcessID()}, {"incarnation", incarnation},
		    {"round", s_harnessCostScope.round}, {"segment", s_harnessCostScope.segment}, {"first_frame", s_harnessCostScope.first},
		    {"last_frame", s_harnessCostScope.last}, {"instruments", HarnessCostInstruments()}}.dump());
	}
}

static void WriteHarnessCostFrameOf(uint64_t round, uint64_t frame) {
	if (!HarnessCost::AnyEnabled()) return;
	std::lock_guard<std::mutex> lock(s_harnessCostMutex);
	static const unsigned long incarnation = std::stoul(CrossEnvironment("CC_TEST_CROSS_INCARNATION", "0"));
	static std::map<uint64_t, uint32_t> s_segments;
	HarnessCostScope& scope = s_harnessCostScope;
	// A tick that simulated no new frame leaves its costs to the frame that follows.
	if (scope.open && scope.round == round && frame == scope.last) return;
	if (scope.open && (scope.round != round || frame != scope.last + 1)) CloseHarnessCostScopeLocked();
	auto charged = HarnessCost::TakeFrame();
	const auto before = HarnessCost::TakeBeforeFrame();
	if (!scope.open) {
		static const bool s_flushAtExit = [] {
			std::atexit([] {
				std::lock_guard<std::mutex> exitLock(s_harnessCostMutex);
				CloseHarnessCostScopeLocked();
			});
			return true;
		}();
		(void)s_flushAtExit;
		scope = {true, round, frame, frame, s_segments[round]++};
		// Declared as it opens, so a process ended before its close still owns the frames it wrote.
		System::PrintDiagnosticLine("[harness-cost-scope-open] " + nlohmann::json{{"version", HarnessCost::c_ReceiptVersion}, {"process", System::GetProcessID()}, {"incarnation", incarnation},
		    {"round", round}, {"segment", scope.segment}, {"first_frame", frame}, {"instruments", HarnessCostInstruments()}}.dump());
		// What the instruments did before this run of frames began (a lobby, a loading screen) is no frame's cost.
		System::PrintDiagnosticLine("[harness-cost-outside] " + nlohmann::json{{"process", System::GetProcessID()}, {"incarnation", incarnation}, {"round", round},
		    {"segment", scope.segment}, {"before_frame", frame}, {"costs_ms", HarnessCostMs(before)}}.dump());
	} else {
		for (size_t instrument = 0; instrument < HarnessCost::InstrumentCount; ++instrument) charged[instrument] += before[instrument];
	}
	scope.last = frame;
	System::PrintDiagnosticLine("[harness-cost-frame] " + nlohmann::json{{"process", System::GetProcessID()}, {"incarnation", incarnation}, {"round", round},
	    {"segment", scope.segment}, {"frame", frame}, {"partition_valid", true}, {"costs_ms", HarnessCostMs(charged)}}.dump());
}

static void WriteHarnessCostFrame(uint64_t frame) {
	WriteHarnessCostFrameOf(ScenarioRunner::GetLockstepRoundId(), frame);
}

// The menus' frames carry their instruments' costs too (the recorder's readback, the menu script's watches); each stay in the
// menu loop is a round of its own, numbered apart from any match round, its frames counted from one.
static uint64_t s_harnessMenuStay = 0;
static uint64_t s_harnessMenuFrame = 0;
static constexpr uint64_t c_HarnessMenuRounds = uint64_t{1} << 62;

static void BeginHarnessCostMenuStay() {
	++s_harnessMenuStay;
	s_harnessMenuFrame = 0;
}

static void WriteHarnessCostMenuFrame() {
	WriteHarnessCostFrameOf(c_HarnessMenuRounds | s_harnessMenuStay, ++s_harnessMenuFrame);
}

// CC_TERRAIN_DUMP=<tick> saves the material and FG color bitmaps beside the -out trace at that tick
// (and on a runtime Desync stop), for host-vs-client terrain-layer divergence forensics.
static void DumpTerrainNow(const std::string& suffix) {
	if (!g_SceneMan.GetScene() || !g_SceneMan.GetScene()->GetTerrain()) {
		return;
	}
	const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
	auto dumpRaw = [&](const char* name, BITMAP* bitmap) {
		std::ofstream out(base + "." + suffix + "." + name + ".bin", std::ios::binary | std::ios::trunc);
		const int32_t dims[2] = {bitmap->w, bitmap->h};
		out.write(reinterpret_cast<const char*>(dims), sizeof(dims));
		for (int y = 0; y < bitmap->h; ++y) {
			out.write(reinterpret_cast<const char*>(bitmap->line[y]), bitmap->w);
		}
	};
	dumpRaw("mat", g_SceneMan.GetScene()->GetTerrain()->GetMaterialBitmap());
	dumpRaw("fg", g_SceneMan.GetScene()->GetTerrain()->GetFGColorBitmap());
	{
		std::ostringstream line;
		line << "[terrain-dump] " << suffix << " saved";
		System::PrintDiagnosticLine(line.str());
	}
}

static bool TerrainDumpArmed() {
	static const bool s_armed = std::getenv("CC_TERRAIN_DUMP") != nullptr;
	return s_armed;
}

// CC_RNG_DRAW_TRACE=1 emits one "rng" tracer event per g_SimRNG draw inside the CC_TERRAIN_EVENTS
// window: the running draw index plus the travel context that consumed it.
static void InstallRNGDrawTraceIfArmed() {
	if (!std::getenv("CC_RNG_DRAW_TRACE")) {
		return;
	}
	g_RNGDrawHook = [](uint64_t drawCount) {
		SceneMan::TraceTerrainEvent("rng", static_cast<int>(drawCount & 0xFFFFFFFFu), static_cast<int>(drawCount >> 32), 0, 0, static_cast<int>(SceneMan::GetTerrainEventContext()));
	};
	g_SimRNG.SetDrawTraceEnabled(true);
}

// CC_TICK_PROBE=1 appends one counts+RNG line per tick beside the -out trace; light enough not to
// disturb the pacing the desync hunt depends on.
// CC_TICK_PROBE_BOX=<x1>:<y1>:<x2>:<y2> adds per-tick FNV hashes of the boxed terrain mat+fg bytes.
static uint64_t BoxedTerrainHash(BITMAP* bitmap, int x1, int y1, int x2, int y2) {
	uint64_t hash = 1469598103934665603ULL;
	if (!bitmap) {
		return hash;
	}
	x1 = std::max(0, x1);
	y1 = std::max(0, y1);
	x2 = std::min(bitmap->w - 1, x2);
	y2 = std::min(bitmap->h - 1, y2);
	for (int y = y1; y <= y2; ++y) {
		for (int x = x1; x <= x2; ++x) {
			hash = (hash ^ static_cast<uint64_t>(bitmap->line[y][x])) * 1099511628211ULL;
		}
	}
	return hash;
}

static void TickProbeIfArmed(uint64_t simTick) {
	static std::ofstream s_out;
	static int s_state = 0;
	static int s_box[4] = {0, 0, -1, -1};
	static bool s_boxArmed = false;
	if (s_state == 0) {
		s_state = std::getenv("CC_TICK_PROBE") ? 1 : -1;
		if (s_state == 1) {
			const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
			s_out.open(base + ".tickprobe.txt", std::ios::trunc);
			if (const char* boxEnv = std::getenv("CC_TICK_PROBE_BOX")) {
				s_boxArmed = std::sscanf(boxEnv, "%d:%d:%d:%d", &s_box[0], &s_box[1], &s_box[2], &s_box[3]) == 4;
			}
		}
	}
	if (s_state != 1 || !s_out.is_open()) {
		return;
	}
	const std::string rngState = g_SimRNG.SerializeStateForHashing();
	s_out << simTick << " a=" << g_MovableMan.GetActorCount() << " p=" << g_MovableMan.GetParticleCount() << " rng=" << std::hash<std::string>{}(rngState) << " draws=" << g_SimRNG.GetDrawCount();
	if (s_boxArmed && g_SceneMan.GetScene() && g_SceneMan.GetScene()->GetTerrain()) {
		s_out << " tm=" << std::hex
		      << BoxedTerrainHash(g_SceneMan.GetScene()->GetTerrain()->GetMaterialBitmap(), s_box[0], s_box[1], s_box[2], s_box[3])
		      << " tf=" << BoxedTerrainHash(g_SceneMan.GetScene()->GetTerrain()->GetFGColorBitmap(), s_box[0], s_box[1], s_box[2], s_box[3])
		      << std::dec;
	}
	s_out << "\n";
}

// CC_TRACK_UID=<uid>[,<uid>...] appends per-tick bit-exact pose/vel/rest rows for those MOs into the
// terrain event trace (tags trk/trk2); in-memory, so it keeps the pacing the desync hunt depends on.
static void TrackUidsIfArmed(uint64_t simTick) {
	static std::vector<long> s_uids;
	static int s_state = 0;
	if (s_state == 0) {
		s_state = -1;
		if (const char* env = std::getenv("CC_TRACK_UID")) {
			const std::string list(env);
			size_t start = 0;
			while (start < list.size()) {
				size_t end = list.find(',', start);
				if (end == std::string::npos) {
					end = list.size();
				}
				const long uid = std::strtol(list.substr(start, end - start).c_str(), nullptr, 10);
				if (uid > 0) {
					s_uids.push_back(uid);
				}
				start = end + 1;
			}
			if (!s_uids.empty()) {
				s_state = 1;
			}
		}
	}
	if (s_state != 1) {
		return;
	}
	// One FP-state row per tick: a driver flipping the FP control state (FTZ/DAZ) mid-run would fork denormal math.
#if defined(__aarch64__)
	uint64_t fpcr = 0;
	asm volatile("mrs %0, fpcr" : "=r"(fpcr));
	SceneMan::TraceTerrainEvent("fpu", static_cast<int32_t>(fpcr), 0, 0, 0, 0);
#elif defined(_MSC_VER)
	SceneMan::TraceTerrainEvent("fpu", static_cast<int32_t>(_mm_getcsr()), static_cast<int32_t>(_control87(0, 0)), 0, 0, 0);
#else
	SceneMan::TraceTerrainEvent("fpu", static_cast<int32_t>(_mm_getcsr()), 0, 0, 0, 0);
#endif
	for (long uid: s_uids) {
		const MovableObject* mo = g_MovableMan.FindObjectByUniqueID(uid);
		if (!mo) {
			continue;
		}
		const auto bits = [](float value) { return std::bit_cast<int32_t>(value); };
		SceneMan::TraceTerrainEvent("trk", bits(mo->GetPos().m_X), bits(mo->GetPos().m_Y), bits(mo->GetVel().m_X), bits(mo->GetVel().m_Y), static_cast<int>(uid));
		float rotAngle = 0.0F;
		float angVel = 0.0F;
		if (const MOSprite* sprite = dynamic_cast<const MOSprite*>(mo)) {
			rotAngle = sprite->GetRotAngle();
			angVel = sprite->GetAngularVel();
		}
		SceneMan::TraceTerrainEvent("trk2", bits(rotAngle), bits(angVel), static_cast<int>(mo->GetRestTimerElapsedSimMS()), mo->GetVelOscillations(), static_cast<int>(uid));
		const int flags = (mo->GetsHitByMOs() ? 1 : 0) | (mo->IgnoresAtomGroupHits() ? 2 : 0) | (mo->GetTraveling() ? 4 : 0) | (mo->ToSettle() ? 8 : 0) | (mo->ToDelete() ? 16 : 0) | (mo->HitsMOs() ? 32 : 0);
		SceneMan::TraceTerrainEvent("trk3", flags, bits(mo->GetPrevPos().m_X), bits(mo->GetPrevPos().m_Y), 0, static_cast<int>(uid));
	}
}

// One-shot per-MO state dump for the Desync stop; pacing-neutral, unlike the per-tick CC_SIM_DUMP.
static void DumpSimStateNow(const std::string& suffix) {
	const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
	std::ofstream out(base + "." + suffix + ".simstate.txt", std::ios::binary | std::ios::trunc);
	if (out.is_open()) {
		g_MovableMan.DumpSimState(g_TimerMan.GetSimUpdateCount(), out);
		{
			std::ostringstream line;
			line << "[sim-dump] " << suffix << " saved";
			System::PrintDiagnosticLine(line.str());
		}
	}
}

// The full per-MO dump as text; the fidelity probe compares this, not just the checksum.
static std::string DumpSimStateToString() {
	std::ostringstream out;
	g_MovableMan.DumpSimState(g_TimerMan.GetSimUpdateCount(), out);
	return out.str();
}

static void WriteProbeText(const std::string& suffix, const std::string& text) {
	const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
	std::ofstream out(base + "." + suffix + ".simstate.txt", std::ios::binary | std::ios::trunc);
	out << text;
}

// Compares the restored world's dump against the captured one; a mismatch is a fidelity hole the hash may miss.
static void CheckRestoredDeepState() {
	const std::string restored = DumpSimStateToString();
	WriteProbeText("rb_restored", restored);
	s_rbProbeRestoreMismatch = restored != s_rbProbeCapturedDeep;
	if (s_rbProbeRestoreMismatch) {
		WriteProbeText("rb_restored_" + std::to_string(s_rbProbeAtTick), restored);
		{
			std::ostringstream line;
			line << "[rbprobe] RESTORE MISMATCH: the restored world's dump differs from the captured one (rb_captured vs rb_restored)";
			System::PrintDiagnosticLine(line.str());
		}
	}
}

// Every capture takes the settle first, as CaptureWorld and SetAsideWorld do: a graph taken in front of it names the objects it sweeps.
static void SettleBeforeCapture() {
	g_LuaMan.CollectGarbageForCheckpoint();
}

// The probe's comparison capture; CheckRestoredScriptGraphs holds the result against the restored world.
static bool CaptureProbeScriptGraphs() {
	SettleBeforeCapture();
	std::vector<std::string> problems;
	if (!g_MovableMan.SerializeScriptGraphs(s_rbProbeLuaGraphsAtCapture, problems)) {
		for (const std::string& problem: problems) {
			{
				std::ostringstream line;
				line << "[rbprobe] FAIL: script graph capture refused: " << problem;
				System::PrintDiagnosticLine(line.str());
			}
		}
		return false;
	}
	return true;
}

// The contract audit's observation is a capture too.
static bool ObserveScriptGraphs(std::vector<std::string>& graphs, std::vector<std::string>& problems) {
	SettleBeforeCapture();
	return g_MovableMan.SerializeScriptGraphs(graphs, problems);
}

// The restored Lua state must serialize exactly as the captured one did: the graph text is canonical.
static void CheckRestoredScriptGraphs() {
	std::vector<std::string> restored;
	std::vector<std::string> problems;
	if (!g_MovableMan.SerializeScriptGraphs(restored, problems)) {
		s_rbProbeRestoreMismatch = true;
		for (const std::string& problem: problems) {
			{
				std::ostringstream line;
				line << "[rbprobe] RESTORE MISMATCH: the restored Lua state cannot be carried: " << problem;
				System::PrintDiagnosticLine(line.str());
			}
		}
	}
	if (!g_MovableMan.GetScriptGraphFailure().empty()) {
		s_rbProbeRestoreMismatch = true;
		{
			std::ostringstream line;
			line << "[rbprobe] RESTORE MISMATCH: the set-aside could not carry the Lua state: " << g_MovableMan.GetScriptGraphFailure();
			System::PrintDiagnosticLine(line.str());
		}
	}
	const size_t count = std::max(restored.size(), s_rbProbeLuaGraphsAtCapture.size());
	for (size_t i = 0; i < count; ++i) {
		const std::string& before = i < s_rbProbeLuaGraphsAtCapture.size() ? s_rbProbeLuaGraphsAtCapture[i] : std::string();
		const std::string& after = i < restored.size() ? restored[i] : std::string();
		if (before != after) {
			s_rbProbeRestoreMismatch = true;
			WriteProbeText("rb_luagraph_" + std::to_string(i) + "_capture", before);
			WriteProbeText("rb_luagraph_" + std::to_string(i) + "_restored", after);
			{
				std::ostringstream line;
				line << "[rbprobe] RESTORE MISMATCH: Lua state " << i << " serializes differently after the restore (rb_luagraph_" << i << "_capture vs _restored)";
				System::PrintDiagnosticLine(line.str());
			}
		}
	}
}

// Everything a preview may touch besides the MO dump: clocks, RNG, identity counter, queues, activity
// scalars, terrain layers and the Lua bindings. The camera and the previews themselves are the only
// presentation-side changes a preview is allowed to make.
// What a session leaves behind before a match: unique IDs drawn and script states handed out. The
// objects are made and destroyed here, so nothing of them reaches the match but the counters.
static void SpendPreMatchHistory(int objects) {
	if (objects <= 0) {
		return;
	}
	const std::string scriptPath = g_PresetMan.GetFullModulePath("Tests.rte/PreviewCompat.lua");
	int loaded = 0;
	for (int index = 0; index < objects; ++index) {
		auto* object = new MOPixel;
		if (object->Create() >= 0 && object->LoadScript(scriptPath, true) == 0 && object->AdoptScriptObject() == 0) {
			++loaded;
		}
		object->DestroyScriptState();
		object->Destroy();
		delete object;
	}
	std::ostringstream line;
	line << "[selftest] pre-match history: objects=" << objects << " scripted=" << loaded
	     << " uid_counter=" << MovableObject::GetUniqueIDCounter();
	System::PrintDiagnosticLine(line.str());
}

// A single-player game played before a match: the activity starts, runs a few updates and ends, leaving its scripts'
// history in the Lua states as a player's earlier game would.
static void PlayPreMatchActivity(const std::string& preset) {
	if (preset.empty()) {
		return;
	}
	const Activity* activity = dynamic_cast<const Activity*>(g_PresetMan.GetEntityPreset("GAScripted", preset));
	if (activity && !activity->GetSceneName().empty()) {
		g_SceneMan.SetSceneToLoad(activity->GetSceneName(), true, false);
	}
	const int started = activity ? g_ActivityMan.StartActivity("GAScripted", preset) : -1;
	int updates = 0;
	for (; started >= 0 && updates < 30; ++updates) {
		g_ActivityMan.Update();
		g_MovableMan.Update();
	}
	g_ActivityMan.EndActivity();
	std::ostringstream line;
	line << "[selftest] pre-match activity: preset=" << preset << " started=" << started << " updates=" << updates
	     << " lua_births=" << g_LuaMan.GetTableBirthCount();
	System::PrintDiagnosticLine(line.str());
}

static std::string DescribeCanonicalExtras(std::vector<std::string>& problems) {
	std::ostringstream out;
	out << "sim_count=" << g_TimerMan.GetSimUpdateCount() << " sim_ticks=" << g_TimerMan.GetSimTimeTicks() << " accumulator=" << g_TimerMan.GetSimAccumulator() << "\n";
	out << "rng_draws=" << g_SimRNG.GetDrawCount() << " rng_state=" << g_SimRNG.GetEngineState() << "\n";
	out << "sound_cursor=" << g_AudioMan.GetCheckpointSoundContainerCursor() << "\n";
	out << "uid_counter=" << MovableObject::GetUniqueIDCounter() << "\n";
	const MovableMan::AddQueueMark mark = g_MovableMan.MarkAddQueues();
	out << "queues actors=" << mark.actors << " items=" << mark.items << " particles=" << mark.particles << " alarms=" << mark.alarms << "\n";
	if (const Activity* activity = g_ActivityMan.GetActivity()) {
		Activity::RollbackState state;
		activity->CaptureRollbackState(state);
		out << "activity state=" << static_cast<int>(state.state);
		for (int team = 0; team < Activity::MaxTeamCount; ++team) {
			out << " t" << team << "=" << std::hexfloat << state.teamFunds[team] << std::defaultfloat << "/" << state.teamDeaths[team] << (state.teamActive[team] ? "a" : "");
		}
		out << "\n";
		out << "players";
		for (int player = 0; player < Players::MaxPlayerCount; ++player) {
			out << " p" << player << "=" << state.controlledActorUID[player] << "/" << state.brainUID[player] << (state.brainEvacuated[player] ? "e" : "");
		}
		out << "\n";
	}
	out << "rosters\n" << g_MovableMan.DescribeTeamRosters();
	out << "render_hidden=" << g_MovableMan.GetRenderHiddenCount() << " speculative=" << (g_MovableMan.IsSpeculative() ? 1 : 0) << " registry=" << g_MovableMan.GetKnownObjectsCount() << "\n";
	TerrainLayerSnapshot terrain;
	if (terrain.Capture()) {
		auto fnv = [](const std::vector<uint8_t>& bytes) {
			uint64_t h = 1469598103934665603ULL;
			for (const uint8_t b: bytes) {
				h = (h ^ b) * 1099511628211ULL;
			}
			return h;
		};
		out << "terrain mat=" << std::hex << fnv(terrain.mat) << " fg=" << fnv(terrain.fg) << " bg=" << fnv(terrain.bg) << std::dec << "\n";
	}
	out << "scripts\n" << g_MovableMan.DescribeScriptBindings();
	if (const Scene* scene = g_SceneMan.GetScene()) {
		auto stream = std::make_unique<std::stringstream>();
		std::stringstream* raw = stream.get();
		Writer writer(std::move(stream));
		for (const Scene::Area* area: scene->GetAreas()) {
			writer.NewProperty("Area");
			area->SaveSnapshot(writer);
			writer.ObjectEnd();
		}
		out << "scene_areas\n" << raw->str();
	}
	std::vector<std::string> graphs;
	g_MovableMan.SerializeScriptGraphs(graphs, problems);
	for (size_t index = 0; index < graphs.size(); ++index) {
		out << "lua_graph " << index << " " << graphs[index].size() << "\n" << graphs[index] << "\n";
	}
	return out.str();
}

// One rendered frame with the previews standing in for their actors, on the render RNG and off the MOID grid.
// The probe's save file is per process, so concurrent probes never read each other's world.
static std::string RollbackProbeSaveName() {
	return "rbprobe_" + std::to_string(System::GetProcessID());
}

/// Whether a resync is taking the coordinator down and rebuilding it: the snapshot is being made, sent or
/// loaded, or the stop that starts all that is still waiting to be read. The world holds meanwhile.
static bool NetMatchResyncRebuilding() {
	if (g_NetMatchService.IsMatchResyncing() || g_ActivityMan.LockstepRelaunchInProgress()) {
		return true;
	}
	// The stop that starts a resync is read a tick or two after the coordinator goes down, and the peer that
	// only hears about it reads it later still. A match that heals in place is given that window; the editor's
	// own tick cap still bounds it, and a match that does not heal fails the moment its coordinator stops.
	return g_NetMatchService.IsResyncOnDesyncEnabled() && ScenarioRunner::HasLockstepCoordinator();
}

/// Whether this completed update batch has a harness frame to present.
static bool NetMatchScreenshotDue() {
	return !s_netMatchScreenshotTicks.empty() && ScenarioRunner::IsLockstepControllerSyncActive() &&
	       s_netMatchScreenshotTicks.contains(ScenarioRunner::GetLockstepCompletedFrame());
}

/// The screen name a recorded frame is stamped with, read from the seam the menu probes read.
static std::string RecordedScreenName() {
	if (PauseMenuGUI* pause = g_MenuMan.GetActivePauseMenu()) {
		const std::string screen = pause->AutomationActiveScreenName();
		return screen == "Gameplay" ? "game" : screen;
	}
	if (!g_MenuMan.GetIsInMenuScreen()) return "game";
	if (!g_MenuMan.IsMainMenuInteractive()) return "game";
	MainMenuGUI* menu = g_MenuMan.GetMainMenu();
	return menu ? menu->AutomationActiveScreenName() : "game";
}

static void PollStallEventsForCapture() {
	// The preceding stall presentation is complete when its next event poll begins.
	if (FrameRecorder::Instance().Enabled()) {
		g_FrameMan.RecordVideoFrame("LockstepWaitOverlay", g_NetMatchService.GetLobbySnapshot().serviceState);
	}
	PollSDLEvents();
}

// Test lever CCCP_TEST_DRAW_PHASES: each stage of the frame's draw, its mean, median and 99th percentile, once every 600 frames.
struct DrawPhases {
	static constexpr const char* c_Names[] = {"previews_in", "scene", "net_ui", "toasts", "post", "pause_menu"};
	static constexpr size_t c_Count = std::size(c_Names);
	const bool armed = std::getenv("CCCP_TEST_DRAW_PHASES") != nullptr;
	std::array<std::vector<float>, c_Count> samples;
	long long lapUs = 0;
	void Begin() { if (armed) lapUs = g_TimerMan.GetAbsoluteTime(); }
	void Lap(size_t phase) {
		if (!armed) return;
		const long long now = g_TimerMan.GetAbsoluteTime();
		samples[phase].push_back(static_cast<float>(now - lapUs));
		lapUs = now;
		if (phase + 1 == c_Count && samples[phase].size() == 600) {
			std::ostringstream line;
			line << "[draw-phase] frames=600 us(mean/p50/p99):";
			for (size_t i = 0; i < c_Count; ++i) {
				std::vector<float>& values = samples[i];
				double total = 0;
				for (float value: values) total += value;
				std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
				const float median = values[values.size() / 2];
				std::nth_element(values.begin(), values.begin() + values.size() * 99 / 100, values.end());
				line << " " << c_Names[i] << "=" << total / values.size() << "/" << median << "/" << values[values.size() * 99 / 100];
				values.clear();
			}
			System::PrintDiagnosticLine(line.str());
		}
	}
};
static DrawPhases s_drawPhases;

static void DrawFrameWithPreviews() {
	if (!FrameMan::FeelBeginDraw()) return;
	const long long drawBeganUs = g_TimerMan.GetAbsoluteTime();
	s_drawPhases.Begin();
	RandomGenerator* prevSimRNG = t_simRNGOverride;
	t_simRNGOverride = &g_RenderRNG;
	g_SceneMan.SetRenderDrawContext(true);
	LocalPredictionHudSelfTest::SampleBeforeRender();
	LocalPrediction::BeginRender();
	LocalPredictionHudSelfTest::SampleDuringRender();
	s_drawPhases.Lap(0);
	std::array<bool, c_MaxScreenCount> hudDisabled;
	const bool localPause = g_MenuMan.IsLocalPauseMenuOpen();
	for (int screen = 0; screen < c_MaxScreenCount; ++screen) {
		hudDisabled[screen] = g_FrameMan.IsHudDisabled(screen);
		if (localPause) g_FrameMan.SetHudDisabled(true, screen);
	}
	{
		NetLockstepPlane::Window sceneDraw("scene draw");
		g_FrameMan.Draw();
	}
	s_drawPhases.Lap(1);
	// Test lever: a slower machine's draw cost, spent inside the frame's draw on this one.
	static const long long s_testDrawCostUs = [] { const char* text = std::getenv("CCCP_TEST_DRAW_COST_US"); return text ? std::atoll(text) : 0LL; }();
	if (s_testDrawCostUs > 0) {
		for (const long long until = g_TimerMan.GetAbsoluteTime() + s_testDrawCostUs; g_TimerMan.GetAbsoluteTime() < until;) {}
	}
	for (int screen = 0; screen < c_MaxScreenCount; ++screen) g_FrameMan.SetHudDisabled(hudDisabled[screen], screen);
	LocalPredictionHudSelfTest::SampleAfterDraw();
	{
		// The overlays read the match service, which reaches the round without the plane's lock.
		NetLockstepPlane::Gap plane("overlay draw");
		s_drawPhases.Begin();
		g_MenuMan.DrawNetworkUI();
		s_drawPhases.Lap(2);
		ScenarioRunner::DrawNetUiToasts();
		s_drawPhases.Lap(3);
		g_WindowMan.DrawPostProcessBuffer();
		s_drawPhases.Lap(4);
		g_MenuMan.DrawLocalPauseMenu();
		s_drawPhases.Lap(5);
	}
	FrameMan::FeelBeforePresent();
	{
		NetLockstepPlane::Window present("present");
		if (s_drawStallMs > 0 && !s_drawStallFired && g_TimerMan.GetSimUpdateCount() >= s_drawStallTick) {
			// A present the driver holds: the main thread is away inside the frame's draw, not its simulation.
			s_drawStallFired = true;
			System::PrintDiagnosticLine("[selftest] draw stall tick=" + std::to_string(g_TimerMan.GetSimUpdateCount()) + " ms=" + std::to_string(s_drawStallMs));
			const uint64_t planeTicksBefore = NetLockstepPlane::Ticks();
			std::this_thread::sleep_for(std::chrono::milliseconds(s_drawStallMs));
			System::PrintDiagnosticLine("[selftest] draw stall done plane_ticks=" + std::to_string(NetLockstepPlane::Ticks() - planeTicksBefore));
		}
		g_WindowMan.UploadFrame();
	}
	g_FrameMan.FeelAfterPresent();
	++s_paceFramesDrawn;
	s_paceFrameDrawUs += g_TimerMan.GetAbsoluteTime() - drawBeganUs;
	if (FrameRecorder::Instance().Enabled()) {
		std::string screen;
		const std::string serviceState = [&screen] {
			// The screen's name comes from the menus, which read the match service.
			NetLockstepPlane::Gap plane("recorder's service state");
			screen = RecordedScreenName();
			return g_NetMatchService.GetLobbySnapshot().serviceState;
		}();
		NetLockstepPlane::Window recorder("recorder");
		g_FrameMan.RecordVideoFrame(screen, serviceState);
	}
	if (NetMatchScreenshotDue()) {
		const uint64_t tick = ScenarioRunner::GetLockstepCompletedFrame();
		const std::string name = "net_match_tick_" + std::to_string(tick) + "_round_" + std::to_string(ScenarioRunner::GetLockstepRoundId());
		const int result = g_FrameMan.SaveScreenToPNG(name.c_str());
		{
			std::ostringstream line;
			line << "[net-match-screenshot] applied_tick=" << tick << " name=" << name << " queued=" << (result == 0);
			System::PrintDiagnosticLine(line.str());
		}
		s_netMatchScreenshotTicks.erase(tick);
	}
	LocalPrediction::EndRender();
	LocalPredictionHudSelfTest::SampleAfterRender();
	if (s_fundsPreviewPress > 0) {
		const long long tick = g_TimerMan.GetSimUpdateCount();
		const long long delay = std::max<long long>(static_cast<long long>(ScenarioRunner::GetLockstepLocalInputDelay()), 7);
		if (tick == s_fundsPreviewPress + 1 || tick == s_fundsPreviewPress + delay) {
			const Activity* activity = g_ActivityMan.GetActivity();
			// Each peer presents its own seat, so sample that seat and the buying team as this peer shows it.
			const int seat = activity ? activity->PlayerOfScreen(0) : Players::NoPlayer;
			const auto oz = [](float funds) {
				char text[64];
				std::snprintf(text, sizeof(text), "%.10g", std::floor(funds));
				return std::string(text);
			};
			const std::string& readout = GameActivity::GetLastFundsReadout(seat);
			std::vector<std::string> problems;
			const std::string extras = DescribeCanonicalExtras(problems);
			// The peers compare the world dump; the extras carry this process's own accumulator, so they stay local.
			const std::string suffix = tick == s_fundsPreviewPress + 1 ? std::string("funds_p1") : std::string("funds_pd");
			WriteProbeText(suffix, DumpSimStateToString());
			WriteProbeText(suffix + "_extras", extras);
			{
				std::ostringstream line;
				line << "[preview-funds-driver] tick=" << tick << " seat=" << seat << " seat_team=" << (activity ? activity->GetTeamOfPlayer(seat) : static_cast<int>(Activity::NoTeam))
				     << " buy_team=" << s_fundsPreviewTeam
				     << " buy_team_oz=" << (activity ? activity->DescribeFundsReadout(s_fundsPreviewTeam, seat) : std::string("EMPTY"))
				     << " buy_team_committed=" << (activity ? oz(activity->GetTeamFunds(s_fundsPreviewTeam)) : std::string("EMPTY"))
				     << " peek_tick=" << LocalPrediction::GetLastFillTick() << " problems=" << problems.size()
				     << " readout=" << (readout.empty() ? "EMPTY" : readout);
				System::PrintDiagnosticLine(line.str());
			}
		}
	}
	g_SceneMan.SetRenderDrawContext(false);
	t_simRNGOverride = prevSimRNG;
	NetModerationGUIProbe::AfterDraw();
}

/// Draws the network wait; returns whether the player explicitly left through the local menu.
static bool UpdateResyncUI(uint32_t elapsedSeconds, bool heldRejoin = false, const std::string& heldLine = {}) {
	PollSDLEvents();
	g_UInputMan.Update(false);
	// Escape opens or backs out of the local menu; only its explicit Leave/End action exits the wait.
	const bool leave = g_MenuMan.UpdateNetworkWaitInput();
	g_WindowMan.ClearBackbuffer();
	clear_to_color(g_FrameMan.GetBackBuffer32(), makeacol32(20, 22, 27, 255));
	AllegroBitmap bitmap(g_FrameMan.GetBackBuffer32());
	const int centerX = g_WindowMan.GetResX() / 2;
	const int centerY = g_WindowMan.GetResY() / 2;
	const std::string resyncTitle = heldRejoin ? (heldLine.empty() ? std::string("Rejoining the match...") : heldLine) : std::string("Restoring the shared match state...");
	g_FrameMan.GetLargeFont(true)->DrawAligned(&bitmap, centerX, centerY - 12, resyncTitle, GUIFont::Centre);
	MenuAutomation::NoteDrawnText(heldRejoin ? "RejoinOverlay" : "ResyncOverlay", resyncTitle);
	// A held player's units are the AI's until the player is back; a repair pauses every player at once.
	const std::string resyncLine = heldRejoin ? (ScenarioRunner::IsLockstepHoldNoticeVisible() ? NetReconnectUx::HeldSeatReturnNotice() : std::string())
	                                          : "Every player waits while the match is reloaded  /  " + std::to_string(elapsedSeconds) + " s  /  F6: Players";
	g_FrameMan.GetSmallFont(true)->DrawAligned(&bitmap, centerX, centerY + 8, resyncLine, GUIFont::Centre);
	MenuAutomation::NoteDrawnText(heldRejoin ? "RejoinOverlay" : "ResyncOverlay", resyncLine);
	if (heldRejoin) {
		const std::string controls = std::to_string(elapsedSeconds) + " s  /  F6: Players  /  Esc: pause menu";
		g_FrameMan.GetSmallFont(true)->DrawAligned(&bitmap, centerX, centerY + 10 + g_FrameMan.GetSmallFont(true)->GetFontHeight(), controls, GUIFont::Centre);
		MenuAutomation::NoteDrawnText("RejoinOverlay", controls);
	}
	g_MenuMan.DrawNetworkUI();
	ScenarioRunner::DrawNetUiToasts(resyncTitle);
	ScenarioRunner::NoteResyncOverlayFrame();
	g_MenuMan.DrawLocalPauseMenu();
	g_WindowMan.UploadFrame();
	if (FrameRecorder::Instance().Enabled()) {
		g_FrameMan.RecordVideoFrame(heldRejoin ? "RejoinOverlay" : "ResyncOverlay", g_NetMatchService.GetLobbySnapshot().serviceState);
	}
	NetModerationGUIProbe::AfterDraw();
	g_UInputMan.EndFrame();
	g_UInputMan.EndSimUpdate();
	return leave;
}

// The previews' gameplay against -lpinv-expect. A preview that starts before the canonical pickup must
// have picked up once its horizon passes that tick (plus the slack of the reach ray's random cast) and
// never before it; one that starts after already holds the item and takes nothing. The shot likewise.
static std::string CheckPreviewOutcome(long long startTick, long long horizon) {
	const LocalPrediction::Outcome& outcome = LocalPrediction::GetLastOutcome();
	if (outcome.violations > 0) {
		return "the previews wrote to the world " + std::to_string(outcome.violations) + " time(s)";
	}
	const bool holdsExpected = !s_lpExpectEquip.empty() && outcome.equipped == s_lpExpectEquip;
	const bool pickedUp = holdsExpected && outcome.taken == 1;
	if (!s_lpExpectEquip.empty()) {
		if (startTick >= s_lpExpectEquipTick) {
			if (outcome.taken != 0 || !holdsExpected) {
				return "after the canonical pickup the preview should hold '" + s_lpExpectEquip + "' and take nothing, but it took " + std::to_string(outcome.taken) + " and holds '" + outcome.equipped + "'";
			}
		} else if (horizon < s_lpExpectEquipTick) {
			if (outcome.taken != 0) {
				return "a pickup before its tick " + std::to_string(s_lpExpectEquipTick) + " (horizon " + std::to_string(horizon) + ", took " + std::to_string(outcome.taken) + ")";
			}
		} else if (horizon >= s_lpExpectEquipTick + s_lpExpectEquipSlack) {
			if (!pickedUp) {
				return "expected the pickup of '" + s_lpExpectEquip + "' by tick " + std::to_string(horizon) + " but the preview took " + std::to_string(outcome.taken) + " resident(s) and holds '" + outcome.equipped + "'";
			}
		} else if (outcome.taken > 1) {
			return "the pickup took " + std::to_string(outcome.taken) + " residents, expected at most 1";
		}
	}
	if (s_lpExpectFireTick > 0 && startTick < s_lpExpectFireTick) {
		const bool fired = holdsExpected && (outcome.firedOnce || (outcome.takenRounds >= 0 && outcome.roundsInMag >= 0 && outcome.roundsInMag < outcome.takenRounds));
		if (horizon < s_lpExpectFireTick) {
			if (fired) {
				return "a shot before its tick " + std::to_string(s_lpExpectFireTick) + " (horizon " + std::to_string(horizon) + ")";
			}
		} else if (horizon >= s_lpExpectFireTick + s_lpExpectFireSlack && !fired) {
			return "expected a shot from '" + s_lpExpectEquip + "' by tick " + std::to_string(horizon) + " but rounds went " + std::to_string(outcome.takenRounds) + " -> " + std::to_string(outcome.roundsInMag) + " and fired_once=" + std::to_string(outcome.firedOnce ? 1 : 0);
		}
	}
	return "";
}

// The first differing lines of two canonical-state captures, so a failure names what moved.
static std::string DescribeStateDifference(const std::string& before, const std::string& after) {
	const auto split = [](const std::string& text) {
		std::vector<std::string> lines;
		std::istringstream stream(text);
		std::string line;
		while (std::getline(stream, line)) {
			lines.push_back(line);
		}
		return lines;
	};
	const std::vector<std::string> first = split(before);
	const std::vector<std::string> second = split(after);
	std::string detail;
	size_t differing = 0;
	size_t shown = 0;
	for (size_t index = 0; index < std::min(first.size(), second.size()); ++index) {
		if (first[index] == second[index]) {
			continue;
		}
		++differing;
		if (shown < 3) {
			++shown;
			detail += " line " + std::to_string(index + 1) + ": '" + first[index].substr(0, 60) + "' -> '" + second[index].substr(0, 60) + "'";
		}
	}
	return "lines " + std::to_string(first.size()) + "->" + std::to_string(second.size()) + " differing=" + std::to_string(differing) + detail;
}

// One depth-1 preview per overlay-link mode; survivor links must miss retired objects and the canonical world must stay identical.
static void RunOverlayLinkArm(char mode, int& cases, int& failures) {
	const std::string label = std::string("overlay-links ") + mode;
	bool caseFailed = false;
	const auto fail = [&caseFailed, &label](const std::string& what) {
		caseFailed = true;
		{
			std::ostringstream line;
			line << "[lpinv] FAIL " << label << ": " << what;
			System::PrintDiagnosticLine(line.str());
		}
	};
	const auto pass = [&label](const std::string& what) {
		{
			std::ostringstream line;
			line << "[lpinv] PASS " << label << ": " << what;
			System::PrintDiagnosticLine(line.str());
		}
	};
	++cases;
	// The letters run in sequence in one process, so each is compared against the state it started from.
	LocalPrediction::Clear();
	g_MovableMan.DropAllPreviewGhosts();
	std::vector<std::string> problems;
	const std::string before = DumpSimStateToString() + DescribeCanonicalExtras(problems);
	for (const std::string& problem: problems) {
		fail("cannot capture canonical Lua state: " + problem);
	}
	if (std::string("itmqxonlap").find(mode) != std::string::npos) {
		if (!PreviewScriptSelfTest::RunRetirementArm(mode)) {
			caseFailed = true;
		}
		problems.clear();
		const std::string after = DumpSimStateToString() + DescribeCanonicalExtras(problems);
		for (const std::string& problem: problems) {
			fail("cannot capture canonical Lua state: " + problem);
		}
		if (after != before) {
			WriteProbeText(std::string("lpinv_before_overlay_") + mode, before);
			WriteProbeText(std::string("lpinv_after_overlay_") + mode, after);
			fail(std::string("canonical state changed after retirement arm (lpinv_before_overlay_") + mode + " vs lpinv_after_overlay_" + mode + ")");
		}
		if (caseFailed) {
			++failures;
		} else {
			pass("canonical state byte-identical");
		}
		return;
	}
	Activity* activity = g_ActivityMan.GetActivity();
	// Clear item-in-reach and arm support so faithful resolution cannot overwrite the probe's links.
	std::vector<std::pair<Actor*, HeldDevice*>> reach;
	std::vector<std::pair<Arm*, HeldDevice*>> support;
	const Actor* original = nullptr;
	for (int player = Players::PlayerOne; activity && player < Players::MaxPlayerCount; ++player) {
		if (Actor* actor = activity->GetLocallyControlledActor(player)) {
			if (!original && activity->IsLocalHumanSeat(player)) {
				original = actor;
			}
			reach.emplace_back(actor, actor->GetItemInReach());
			actor->SetItemInReach(nullptr);
			if (const AHuman* human = dynamic_cast<const AHuman*>(actor)) {
				for (Arm* arm: {human->GetFGArm(), human->GetBGArm()}) {
					if (arm) {
						support.emplace_back(arm, arm->GetHeldDeviceThisArmIsTryingToSupport());
						arm->SetHeldDeviceThisArmIsTryingToSupport(nullptr);
					}
				}
			}
		}
	}
	// The resident anchors: the nearest other actor with parts and the nearest item lying in the world.
	const Actor* residentActor = nullptr;
	const MovableObject* residentItem = nullptr;
	if (original) {
		const float radius = static_cast<float>(std::max(g_SceneMan.GetSceneWidth(), g_SceneMan.GetSceneHeight()));
		const std::vector<MovableObject*>* nearby = g_MovableMan.GetMOsInRadius(original->GetPos(), radius);
		float actorDistance = 0.0F;
		float itemDistance = 0.0F;
		for (MovableObject* mo: *nearby) {
			if (!mo) {
				continue;
			}
			const float distance = g_SceneMan.ShortestDistance(original->GetPos(), mo->GetPos(), g_SceneMan.SceneWrapsX()).GetMagnitude();
			const Actor* actor = dynamic_cast<const Actor*>(mo);
			if (actor && actor != original && g_MovableMan.IsActor(actor) && !actor->GetAttachableList().empty()) {
				if (!residentActor || distance < actorDistance) {
					residentActor = actor;
					actorDistance = distance;
				}
			} else if (dynamic_cast<const HeldDevice*>(mo) && g_MovableMan.IsDevice(mo) && (!residentItem || distance < itemDistance)) {
				residentItem = mo;
				itemDistance = distance;
			}
		}
		delete nearby;
	}

	PreviewScriptSelfTest::ArmOverlayLinkProbe(mode, residentActor, residentItem);
	const size_t ghostsBefore = g_MovableMan.GetPreviewGhostCount();
	LocalPrediction::SetDepthOverride(1);
	LocalPrediction::Clear();
	LocalPrediction::RunPreview();
	const PreviewScriptSelfTest::OverlayLinkProbe& probe = PreviewScriptSelfTest::GetOverlayLinkProbe();
	// Links are compared by address only: a target the overlay retired may already be deleted.
	const auto describe = [&probe](const MovableObject* link) -> std::string {
		if (!link) {
			return "nothing";
		}
		if (link == probe.spawn) {
			return "the retired drop " + probe.spawnPreset + " uid=" + std::to_string(probe.spawnUID);
		}
		if (link == probe.spawnPart) {
			return "a part of the retired drop";
		}
		if (link == probe.spawnWound) {
			return "a wound of the retired drop";
		}
		if (link == probe.residentActor) {
			return "the resident actor";
		}
		if (link == probe.residentItem) {
			return "the resident item";
		}
		if (link == probe.residentPart) {
			return "the resident part";
		}
		return "another object";
	};
	const auto expect = [&describe, &pass, &fail](const std::string& check, const MovableObject* link, const MovableObject* wanted) {
		if (link == wanted) {
			pass(check + ": " + describe(link));
		} else {
			fail(check + ": points at " + describe(link) + ", expected " + describe(wanted));
		}
	};
	if (!probe.ran) {
		fail("not armed: no preview clone reached the probe");
	} else if (!probe.failure.empty()) {
		fail("not armed: " + probe.failure);
	} else {
		{
			std::ostringstream line;
			line << "[lpinv] ARMED " << label << ": clone uid=" << probe.cloneUID << (probe.spawn ? " drop=" + probe.spawnPreset + " uid=" + std::to_string(probe.spawnUID) : std::string())
			     << " ghosts " << ghostsBefore << "->" << g_MovableMan.GetPreviewGhostCount() << " [" << LocalPrediction::DescribeLastOutcome() << "]";
			System::PrintDiagnosticLine(line.str());
		}
		const Actor* clone = dynamic_cast<const Actor*>(probe.clone);
		if (mode == 'b') {
			expect("item_in_reach_to_a_retired_spawn", clone->GetItemInReach(), nullptr);
			expect("mo_to_not_hit_to_a_retired_spawn", clone->GetWhichMOToNotHit(), nullptr);
			expect("mo_to_not_hit_to_an_in_world_shadow", probe.shadowLinkPart->GetWhichMOToNotHit(), probe.residentActor);
		} else if (mode == 'r') {
			expect("item_in_reach_to_an_in_world_shadow", clone->GetItemInReach(), probe.residentItem);
		} else if (mode == 'c') {
			expect("mo_to_not_hit_to_a_retired_spawn_part", probe.spawnPartLinkPart->GetWhichMOToNotHit(), nullptr);
			expect("mo_to_not_hit_to_a_retired_spawn_wound", probe.spawnWoundLinkPart->GetWhichMOToNotHit(), nullptr);
			expect("survivor_wound_mo_to_not_hit_to_a_retired_spawn", probe.cloneWound->GetWhichMOToNotHit(), nullptr);
			expect("mo_to_not_hit_to_an_in_world_shadow_part", probe.shadowPartLinkPart->GetWhichMOToNotHit(), probe.residentPart);
		}
	}
	LocalPrediction::Clear();
	g_MovableMan.DropAllPreviewGhosts();
	PreviewScriptSelfTest::DisarmOverlayLinkProbe();
	for (auto entry = reach.rbegin(); entry != reach.rend(); ++entry) {
		entry->first->SetItemInReach(entry->second);
	}
	for (auto entry = support.rbegin(); entry != support.rend(); ++entry) {
		entry->first->SetHeldDeviceThisArmIsTryingToSupport(entry->second);
	}
	problems.clear();
	const std::string after = DumpSimStateToString() + DescribeCanonicalExtras(problems);
	for (const std::string& problem: problems) {
		fail("cannot capture canonical Lua state: " + problem);
	}
	if (after != before) {
		WriteProbeText(std::string("lpinv_before_overlay_") + mode, before);
		WriteProbeText(std::string("lpinv_after_overlay_") + mode, after);
		fail(std::string("canonical state changed after the overlay-link preview (lpinv_before_overlay_") + mode + " vs lpinv_after_overlay_" + mode + ")");
	}
	if (caseFailed) {
		++failures;
	} else {
		{
			std::ostringstream line;
			line << "[lpinv] ok " << label << ": canonical state byte-identical";
			System::PrintDiagnosticLine(line.str());
		}
	}
}

// -local-prediction-invariance: at tick T, run and discard previews of every depth and repeat count and
// require the canonical world (dump + extras) byte-identical afterwards. The run then continues, so the
// trace compare against a no-preview reference closes the resume half of the guarantee.
static void LocalPredictionInvarianceOnTick(uint64_t simTick) {
	if (s_lpInvarianceTick <= 0 || simTick != static_cast<uint64_t>(s_lpInvarianceTick)) {
		return;
	}
	const int savedDepth = LocalPrediction::GetDepthOverride();
	g_MovableMan.WaitForActorsSeeTask();
	g_MovableMan.CompleteQueuedMOIDDrawings();
	PreviewScriptSelfTest::SetStrideCounter(true);
	if (Activity* activity = g_ActivityMan.GetActivity()) {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (Actor* actor = activity->GetLocallyControlledActor(player)) {
				PreviewScriptSelfTest::InstallStrideCounter(actor);
			}
		}
	}
	std::vector<std::string> problems;
	const std::string before = DumpSimStateToString() + DescribeCanonicalExtras(problems);
	if (!problems.empty()) {
		for (const std::string& problem: problems) {
			{
				std::ostringstream line;
				line << "[lpinv] FAIL: cannot capture canonical Lua state: " << problem;
				System::PrintDiagnosticLine(line.str());
			}
		}
		s_lpInvarianceFailures = 1;
		s_netReplayExitCode = 5;
		g_MetricsCollector.RecordString("lpinv_result", "fail");
		return;
	}
	WriteProbeText("lpinv_before", before);
	// What a checkpoint taken with previews outstanding would see: the per-state bindings and the graph roots.
	struct Registrations {
		std::string bindings;
		std::vector<std::string> graphs;
		std::vector<std::vector<long>> roots;
	};
	// The uids SerializeScriptGraph keys its roots by: registered plus pending, scripts initialized, one per uid.
	const auto rootUIDs = [](const LuaStateWrapper& state) {
		std::vector<long> uids;
		for (const MovableObject* mo: state.GetRegisteredMOs()) {
			if (mo->ObjectScriptsInitialized()) {
				uids.push_back(mo->GetUniqueID());
			}
		}
		for (const MovableObject* mo: state.GetPendingRegisteredMOs()) {
			if (mo->ObjectScriptsInitialized()) {
				uids.push_back(mo->GetUniqueID());
			}
		}
		std::sort(uids.begin(), uids.end());
		uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
		return uids;
	};
	const auto captureRegistrations = [&rootUIDs](std::vector<std::string>& into) {
		Registrations capture;
		capture.bindings = g_MovableMan.DescribeScriptBindings();
		g_MovableMan.SerializeScriptGraphs(capture.graphs, into);
		capture.roots.push_back(rootUIDs(g_LuaMan.GetMasterScriptState()));
		for (const LuaStateWrapper& state: g_LuaMan.GetThreadedScriptStates()) {
			capture.roots.push_back(rootUIDs(state));
		}
		return capture;
	};
	const auto describeDrift = [](const Registrations& was, const Registrations& now) {
		const auto bindingLines = [](const std::string& text) {
			std::map<std::string, std::string> byState;
			std::istringstream stream(text);
			for (std::string line; std::getline(stream, line);) {
				const size_t split = line.find(' ');
				byState[line.substr(0, split)] = split == std::string::npos ? std::string() : line.substr(split + 1);
			}
			return byState;
		};
		const auto tokensOnlyIn = [](const std::string& left, const std::string& right) {
			std::map<std::string, int> pool;
			std::istringstream rightTokens(right);
			for (std::string token; rightTokens >> token;) {
				++pool[token];
			}
			std::string out;
			std::istringstream leftTokens(left);
			for (std::string token; leftTokens >> token;) {
				if (const auto found = pool.find(token); found != pool.end() && found->second > 0) {
					--found->second;
					continue;
				}
				out += (out.empty() ? "" : " ") + token;
			}
			return out;
		};
		const auto uidsOnlyIn = [](const std::vector<long>& left, const std::vector<long>& right) {
			std::string out;
			for (const long uid: left) {
				if (std::find(right.begin(), right.end(), uid) == right.end()) {
					out += (out.empty() ? "" : " ") + std::to_string(uid);
				}
			}
			return out;
		};
		std::vector<std::string> drift;
		const std::map<std::string, std::string> wasLines = bindingLines(was.bindings);
		const std::map<std::string, std::string> nowLines = bindingLines(now.bindings);
		std::vector<std::string> states;
		for (const auto& [name, tokens]: wasLines) {
			states.push_back(name);
		}
		for (const auto& [name, tokens]: nowLines) {
			if (wasLines.find(name) == wasLines.end()) {
				states.push_back(name);
			}
		}
		for (const std::string& name: states) {
			const std::string wasTokens = wasLines.find(name) != wasLines.end() ? wasLines.at(name) : std::string();
			const std::string nowTokens = nowLines.find(name) != nowLines.end() ? nowLines.at(name) : std::string();
			const std::string extra = tokensOnlyIn(nowTokens, wasTokens);
			const std::string missing = tokensOnlyIn(wasTokens, nowTokens);
			if (!extra.empty() || !missing.empty()) {
				drift.push_back("scripts " + name + ": extra [" + extra + "] missing [" + missing + "]; measured before [" + wasTokens + "] now [" + nowTokens + "]");
			}
		}
		for (size_t index = 0; index < std::max(was.roots.size(), now.roots.size()); ++index) {
			const std::vector<long> wasRoots = index < was.roots.size() ? was.roots[index] : std::vector<long>();
			const std::vector<long> nowRoots = index < now.roots.size() ? now.roots[index] : std::vector<long>();
			const std::string wasGraph = index < was.graphs.size() ? was.graphs[index] : std::string();
			const std::string nowGraph = index < now.graphs.size() ? now.graphs[index] : std::string();
			const std::string extra = uidsOnlyIn(nowRoots, wasRoots);
			const std::string missing = uidsOnlyIn(wasRoots, nowRoots);
			if (!extra.empty() || !missing.empty() || wasGraph != nowGraph) {
				drift.push_back("lua_graph " + std::to_string(index) + " (" + (index == 0 ? std::string("master") : "thread" + std::to_string(index - 1)) + "): extra roots [" + extra + "] missing roots [" + missing + "]; measured roots " + std::to_string(wasRoots.size()) + " -> " + std::to_string(nowRoots.size()) + ", graph bytes " + std::to_string(wasGraph.size()) + " -> " + std::to_string(nowGraph.size()));
			}
		}
		return drift;
	};
	std::vector<std::string> registrationProblems;
	const Registrations registrationsBefore = captureRegistrations(registrationProblems);
	if (!registrationProblems.empty()) {
		for (const std::string& problem: registrationProblems) {
			{
				std::ostringstream line;
				line << "[lpinv] FAIL: cannot capture canonical script registrations: " << problem;
				System::PrintDiagnosticLine(line.str());
			}
		}
		s_lpInvarianceFailures = 1;
		s_netReplayExitCode = 5;
		g_MetricsCollector.RecordString("lpinv_result", "fail");
		return;
	}
	const uint64_t soundCursorBefore = g_AudioMan.GetCheckpointSoundContainerCursor();
	int failures = 0;
	int cases = 0;
	for (const int depth: s_lpInvarianceDepths) {
		for (const int repeats: s_lpInvarianceRepeats) {
			const std::string label = "depth " + std::to_string(depth) + " x" + std::to_string(repeats);
			bool caseFailed = false;
			const auto fail = [&caseFailed, &label](const std::string& what) {
				caseFailed = true;
				{
					std::ostringstream line;
					line << "[lpinv] FAIL " << label << ": " << what;
					System::PrintDiagnosticLine(line.str());
				}
			};
			LocalPrediction::SetDepthOverride(depth);
			const uint64_t previewsBefore = LocalPrediction::GetPreviewCount();
			const uint64_t armsBefore = PreviewEventLedger::GetArmCount();
			std::string outcomeFailure;
			for (int n = 0; n < repeats; ++n) {
				LocalPrediction::Clear();
				LocalPrediction::RunPreview();
				// A real frame: the substitution, the HUD and the hidden residents all go through the draw.
				DrawFrameWithPreviews();
				if (const std::string failure = CheckPreviewOutcome(static_cast<long long>(simTick), static_cast<long long>(simTick) + depth); !failure.empty() && outcomeFailure.empty()) {
					outcomeFailure = failure;
				}
			}
			// The previews are still outstanding: a checkpoint here must find the registrations unchanged.
			std::vector<std::string> outstandingProblems;
			const Registrations registrationsOutstanding = captureRegistrations(outstandingProblems);
			for (const std::string& problem: outstandingProblems) {
				fail("cannot capture the script registrations while the previews are outstanding: " + problem);
			}
			if (const std::vector<std::string> drift = describeDrift(registrationsBefore, registrationsOutstanding); !drift.empty()) {
				std::string probe = "scripts\n" + registrationsOutstanding.bindings;
				for (size_t index = 0; index < registrationsOutstanding.graphs.size(); ++index) {
					probe += "lua_graph " + std::to_string(index) + " " + std::to_string(registrationsOutstanding.graphs[index].size()) + "\n" + registrationsOutstanding.graphs[index] + "\n";
				}
				WriteProbeText("lpinv_previews_d" + std::to_string(depth) + "_x" + std::to_string(repeats), probe);
				for (const std::string& line: drift) {
					fail("the outstanding previews left the Lua states changed - " + line);
				}
			}
			const uint64_t previewsRun = LocalPrediction::GetPreviewCount() - previewsBefore;
			const std::string outcome = LocalPrediction::DescribeLastOutcome();
			if (FaultInjected("preview_mutate_canonical") && depth == s_lpInvarianceDepths.front() && repeats == s_lpInvarianceRepeats.front()) {
				if (Actor* victim = g_MovableMan.GetFirstBrainActor(0)) {
					victim->SetVel(victim->GetVel() + Vector(0.001F, 0.0F));
				}
			}
			if (FaultInjected("preview_mutate_lua") && depth == s_lpInvarianceDepths.front() && repeats == s_lpInvarianceRepeats.front()) {
				g_LuaMan.GetMasterScriptState().RunScriptString("_InvarianceFault = { changed = true }");
			}
			LocalPrediction::Clear();
			problems.clear();
			const std::string after = DumpSimStateToString() + DescribeCanonicalExtras(problems);
			++cases;
			for (const std::string& problem: problems) {
				fail("cannot capture canonical Lua state: " + problem);
			}
			if (previewsRun != static_cast<uint64_t>(repeats)) {
				fail(std::to_string(previewsRun) + " previews ran, expected " + std::to_string(repeats) + " (no local actor to preview?)");
			}
			if (const uint64_t soundCursorAfter = g_AudioMan.GetCheckpointSoundContainerCursor(); soundCursorAfter != soundCursorBefore) {
				fail("the audio checkpoint identity cursor moved " + std::to_string(soundCursorBefore) + " -> " + std::to_string(soundCursorAfter) + " across discarded previews");
			}
			if (const uint64_t armed = PreviewEventLedger::GetArmCount() - armsBefore; armed != previewsRun) {
				fail("the event ledger armed " + std::to_string(armed) + " times for " + std::to_string(previewsRun) + " previews");
			}
			if (PreviewEventLedger::IsArmed()) {
				fail("the event ledger is still armed after the previews");
			}
			if (!outcomeFailure.empty()) {
				fail(outcomeFailure + " [" + outcome + "]");
			}
			if (after != before) {
				WriteProbeText("lpinv_after_d" + std::to_string(depth) + "_x" + std::to_string(repeats), after);
				fail("canonical state changed after discarded previews (lpinv_before vs lpinv_after_d" + std::to_string(depth) + "_x" + std::to_string(repeats) + ")");
			}
			if (caseFailed) {
				++failures;
			} else {
				{
					std::ostringstream line;
					line << "[lpinv] ok " << label << ": " << previewsRun << " previews, " << outcome << ", canonical state byte-identical";
					System::PrintDiagnosticLine(line.str());
				}
			}
		}
	}
	if (!s_lpOverlayLinkModes.empty()) {
		// Each letter is compared against its own baseline, so the set keeps its own end-to-end compare for drift across letters.
		problems.clear();
		const std::string setBefore = DumpSimStateToString() + DescribeCanonicalExtras(problems);
		for (const char mode: s_lpOverlayLinkModes) {
			RunOverlayLinkArm(mode, cases, failures);
		}
		++cases;
		const std::string setAfter = DumpSimStateToString() + DescribeCanonicalExtras(problems);
		if (!problems.empty()) {
			++failures;
			for (const std::string& problem: problems) {
				{
					std::ostringstream line;
					line << "[lpinv] FAIL overlay-links set: cannot capture canonical Lua state: " << problem;
					System::PrintDiagnosticLine(line.str());
				}
			}
		} else if (setAfter != setBefore) {
			WriteProbeText("lpinv_before_overlay_set", setBefore);
			WriteProbeText("lpinv_after_overlay_set", setAfter);
			++failures;
			{
				std::ostringstream line;
				line << "[lpinv] FAIL overlay-links set: canonical state changed: " << DescribeStateDifference(setBefore, setAfter);
				System::PrintDiagnosticLine(line.str());
			}
		} else {
			{
				std::ostringstream line;
				line << "[lpinv] PASS overlay-links set: canonical state byte-identical";
				System::PrintDiagnosticLine(line.str());
			}
		}
	}
	LocalPrediction::SetDepthOverride(savedDepth);
	PreviewScriptSelfTest::SetStrideCounter(false);
	s_lpInvarianceFailures = failures;
	{
		std::ostringstream line;
		line << "[lpinv] " << (failures == 0 ? "PASS" : "FAIL") << " tick " << simTick << ": " << (cases - failures) << "/" << cases << " cases passed invariance and link checks";
		System::PrintDiagnosticLine(line.str());
	}
	g_MetricsCollector.RecordString("lpinv_result", failures == 0 ? "pass" : "fail");
	g_MetricsCollector.Record("lpinv_cases", cases);
	g_MetricsCollector.Record("lpinv_failures", failures);
	if (failures > 0) {
		s_netReplayExitCode = 5;
	}
}

// -local-prediction-event-ledger drives one preview and one frame per sim tick, the cadence a played
// match has; a replay run pumps its ticks without frames, so nothing would preview at all.
// The seamless swap: the ghost's lead, the particle that takes its pose, and the dumps around the handover.
static void SampleSeamlessSwap(uint64_t tick, const std::vector<MovableMan::PreviewGhostState>& ghosts) {
	const MovableMan::PreviewSwap& swap = g_MovableMan.GetLastPreviewSwap();
	if (swap.count > s_eventLedgerSwapCount) {
		s_eventLedgerSwapCount = swap.count;
		SwapRecord record;
		record.adoptionTick = swap.adoptionTick;
		record.tick = swap.tick;
		record.leadTicks = swap.leadTicks;
		record.poseDelta = swap.poseDelta;
		record.adopteeUID = swap.adopteeUID;
		for (const GhostSample& sample: s_eventLedgerGhostSamples) {
			if (sample.any && sample.adopted && sample.adoptionTick == swap.adoptionTick && sample.adopteeUID == swap.adopteeUID) {
				record.ghostPos = sample.pos;
			}
		}
		if (MovableObject* adoptee = g_MovableMan.FindObjectByUniqueID(swap.adopteeUID)) {
			record.adopteeAlive = true;
			record.adopteeHeldAfter = adoptee->IsHeldForPreviewAdoption();
			record.adopteePos = adoptee->GetPos();
		}
		s_eventLedgerSwaps.push_back(record);
		if (!s_eventLedgerSwapDumpEnd) {
			WriteProbeText("event_ledger_swap_end", DumpSimStateToString());
			s_eventLedgerSwapDumpEnd = true;
		}
	}
	for (const MovableMan::PreviewGhostState& ghost: ghosts) {
		if (!ghost.adopteeHeld) {
			continue;
		}
		const bool atAdoption = tick == ghost.adoptionTick;
		const bool atLastLedTick = tick + 1 == ghost.poseTick;
		if (!atAdoption && !atLastLedTick) {
			continue;
		}
		++s_eventLedgerHoldSampleCandidates;
		if (MovableObject* adoptee = g_MovableMan.FindObjectByUniqueID(ghost.adopteeUID)) {
			// Presentation only: the same world dumps the same bytes with the hold on and with it off.
			const std::string held = DumpSimStateToString();
			const MovableObject::PreviewAdoption tag = adoptee->GetPreviewAdoption();
			adoptee->ReleasePreviewAdoptionHold();
			const std::string freed = DumpSimStateToString();
			adoptee->HoldForPreviewAdoption(tag.key, tag.revealTick);
			++s_eventLedgerHoldDumpPoints;
			if (held != freed) {
				++s_eventLedgerHoldDumpDiffs;
			}
			if (atAdoption && !s_eventLedgerSwapDumpT0) {
				WriteProbeText("event_ledger_swap_t0", held);
				s_eventLedgerSwapDumpT0 = true;
			}
			if (atLastLedTick && !s_eventLedgerSwapDumpMid) {
				WriteProbeText("event_ledger_swap_mid", held);
				s_eventLedgerSwapDumpMid = true;
			}
		}
		break;
	}
}

static void PreviewEventLedgerFrameOnTick() {
	if (s_eventLedgerPressTick <= 0 && LocalPredictionHudSelfTest::g_PressTick <= 0 && s_fundsPreviewPress <= 0) {
		return;
	}
	if (g_TimerMan.GetSimUpdateCount() == s_eventLedgerPressTick && s_eventLedgerLuaEmitterUID == 0) {
		if (Activity* activity = g_ActivityMan.GetActivity()) {
			if (Actor* actor = activity->GetLocallyControlledActor(activity->PlayerOfScreen(0))) {
				if (const AHuman* human = dynamic_cast<const AHuman*>(actor)) {
					if (const HeldDevice* held = human->GetEquippedItem()) {
						s_eventLedgerLuaPreset = held->GetPresetName();
						s_eventLedgerLuaEmitterUID = static_cast<uint64_t>(held->GetUniqueID());
						s_eventLedgerGlowUIDs.clear();
						s_eventLedgerGlowUIDs.insert(static_cast<uint64_t>(actor->GetUniqueID()));
						std::unordered_set<const Entity*> visited;
						std::unordered_set<const MovableObject*> objects;
						CollectOwnedMovableObjects(held, visited, objects);
						for (const MovableObject* mo: objects) {
							if (mo) {
								s_eventLedgerGlowUIDs.insert(static_cast<uint64_t>(mo->GetUniqueID()));
							}
						}
					}
				}
			}
		}
	}
	std::string dumpBeforePreview;
	std::vector<std::string> dumpProblemsBefore;
	std::vector<MovableMan::PreviewGhostState> ghostsBeforePreview;
	const bool snapshotGhostWindow = s_eventLedgerPressTick > 0 && !s_eventLedgerGhostDumpTaken;
	if (snapshotGhostWindow) {
		ghostsBeforePreview = g_MovableMan.GetPreviewGhostStates();
		dumpBeforePreview = DumpSimStateToString() + DescribeCanonicalExtras(dumpProblemsBefore);
	}
	LocalPrediction::RunPreview();
	if (snapshotGhostWindow && g_MovableMan.GetPreviewGhostCount() > 0) {
		// A ghost that this preview installed or carried forward: the dump around that window must not move.
		std::vector<PreviewEventLedger::Key> moved;
		for (const MovableMan::PreviewGhostState& ghost: g_MovableMan.GetPreviewGhostStates()) {
			const auto before = std::find_if(ghostsBeforePreview.begin(), ghostsBeforePreview.end(), [&ghost](const MovableMan::PreviewGhostState& was) {
				return was.key.kind == ghost.key.kind && was.key.emitterUID == ghost.key.emitterUID && was.key.presetHash == ghost.key.presetHash && was.key.tick == ghost.key.tick && was.key.seq == ghost.key.seq;
			});
			if (before == ghostsBeforePreview.end() || (ghost.pos - before->pos).GetMagnitude() > 0.01) {
				moved.push_back(ghost.key);
			}
		}
		if (!moved.empty()) {
			s_eventLedgerGhostMovedKeys = moved;
			std::vector<std::string> dumpProblemsAfter;
			const std::string dumpAfterPreview = DumpSimStateToString() + DescribeCanonicalExtras(dumpProblemsAfter);
			WriteProbeText("event_ledger_ghost_travel", dumpAfterPreview);
			s_eventLedgerGhostDumpIdentical = dumpProblemsBefore.empty() && dumpProblemsAfter.empty() && dumpBeforePreview == dumpAfterPreview;
			s_eventLedgerGhostDumpTaken = true;
		}
	}
	if (s_eventLedgerPressTick > 0) {
		const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
		const std::vector<MovableMan::PreviewGhostState> ghosts = g_MovableMan.GetPreviewGhostStates();
		if (ghosts.empty()) {
			GhostSample sample;
			sample.tick = tick;
			s_eventLedgerGhostSamples.push_back(sample);
		} else {
			for (const MovableMan::PreviewGhostState& ghost: ghosts) {
				GhostSample sample;
				sample.tick = tick;
				sample.key = ghost.key;
				sample.any = true;
				sample.pos = ghost.pos;
				sample.vel = ghost.vel;
				sample.globalAccScalar = ghost.globalAccScalar;
				sample.airResistance = ghost.airResistance;
				sample.airThreshold = ghost.airThreshold;
				sample.adopted = ghost.adopted;
				sample.adopteeHeld = ghost.adopteeHeld;
				sample.poseTick = ghost.poseTick;
				sample.adoptionTick = ghost.adoptionTick;
				sample.adopteeUID = ghost.adopteeUID;
				sample.adopteePos = ghost.adopteePos;
				sample.adopteeVel = ghost.adopteeVel;
				sample.adopteeGlobalAccScalar = ghost.adopteeGlobalAccScalar;
				sample.adopteeAirResistance = ghost.adopteeAirResistance;
				sample.adopteeAirThreshold = ghost.adopteeAirThreshold;
				s_eventLedgerGhostSamples.push_back(sample);
			}
		}
		if (g_MovableMan.GetPreviewGhostCount() > 0 && !g_MovableMan.PreviewGhostsAreUnregistered()) {
			s_eventLedgerGhostRegistered = true;
		}
		SampleSeamlessSwap(tick, ghosts);
	}
	if (s_eventLedgerFlashTick < 0 && LocalPrediction::GetLastOutcome().firedFrame) {
		s_eventLedgerFlashTick = g_TimerMan.GetSimUpdateCount();
	}
	DrawFrameWithPreviews();
}

// -local-prediction-event-ledger: the shot a previewed actor fires must be audible on the preview that
// runs it, not D ticks later at its committed tick, and it must reach the output exactly once.
static void CheckPreviewEventLedgerSelfTest() {
	if (s_eventLedgerPressTick <= 0 || s_eventLedgerChecked) {
		return;
	}
	s_eventLedgerChecked = true;
	bool passed = true;
	const auto check = [&passed](const char* name, bool ok, const std::string& detail) {
		{
			std::ostringstream line;
			line << "[preview-event-selftest] " << (ok ? "PASS " : "FAIL ") << name << ": " << detail;
			System::PrintDiagnosticLine(line.str());
		}
		passed = passed && ok;
	};
	const uint64_t press = static_cast<uint64_t>(s_eventLedgerPressTick);
	const std::vector<PreviewEventLedger::EventStart>& starts = PreviewEventLedger::GetEventStarts();
	const auto firstAfterPress = [&starts, press](uint8_t kind) -> const PreviewEventLedger::EventStart* {
		for (const PreviewEventLedger::EventStart& start: starts) {
			if (start.kind == kind && start.committedTick >= press) return &start;
		}
		return nullptr;
	};
	const PreviewEventLedger::EventStart* tracked = firstAfterPress(PreviewEventLedger::Sound);
	if (!tracked) {
		check("a_previewed_actor_played_a_sound", false, "no physical voice from a previewed actor at or after tick " + std::to_string(press) + " (" + std::to_string(PreviewEventLedger::GetEventStartCount()) + " events recorded in the run)");
	} else {
		size_t sameKey = 0;
		for (const PreviewEventLedger::EventStart& start: starts) {
			if (start.kind == tracked->kind && start.emitterUID == tracked->emitterUID && start.eventTick == tracked->eventTick && start.seq == tracked->seq) {
				++sameKey;
			}
		}
		check("the_sound_starts_on_the_preview_tick", tracked->committedTick <= press + 1,
		      "first physical voice for the press at committed tick " + std::to_string(tracked->committedTick) + " (event tick " + std::to_string(tracked->eventTick) + ", seq " + std::to_string(tracked->seq) + ", predicted=" + std::to_string(tracked->predicted ? 1 : 0) + "), expected <= " + std::to_string(press + 1));
		check("the_event_reaches_the_output_once", sameKey == 1, std::to_string(sameKey) + " physical starts for that event");
	}
	const uint64_t glowWindow = press + static_cast<uint64_t>(std::max(0, LocalPrediction::GetDepthOverride())) + 1;
	const PreviewEventLedger::EventStart* glow = nullptr;
	for (const PreviewEventLedger::EventStart& start: starts) {
		if (start.kind == PreviewEventLedger::PostEffect && start.committedTick >= press && start.committedTick <= glowWindow && s_eventLedgerGlowUIDs.count(start.emitterUID)) {
			glow = &start;
			break;
		}
	}
	if (!glow) {
		check("the_glow_starts_on_the_preview_tick", true, "no post effect belongs to " + (s_eventLedgerLuaPreset.empty() ? std::string("the firearm") : s_eventLedgerLuaPreset));
	} else {
		check("the_glow_starts_on_the_preview_tick", glow->committedTick <= press + 1,
		      "first post effect for the press at committed tick " + std::to_string(glow->committedTick) + " (event tick " + std::to_string(glow->eventTick) + ", predicted=" + std::to_string(glow->predicted ? 1 : 0) + "), expected <= " + std::to_string(press + 1));
	}
	if (s_eventLedgerLuaPreset == "AK-47" && s_eventLedgerLuaEmitterUID != 0) {
		const PreviewEventLedger::EventStart* luaFire = nullptr;
		for (const PreviewEventLedger::EventStart& start: starts) {
			if (start.kind == PreviewEventLedger::Sound && start.emitterUID == s_eventLedgerLuaEmitterUID && start.committedTick >= press) {
				luaFire = &start;
				break;
			}
		}
		check("the_lua_fire_sound_starts_on_the_preview_tick", luaFire && luaFire->committedTick <= press + 1 && luaFire->predicted,
		      luaFire ? "first Mech Ronin AK-47 voice at committed tick " + std::to_string(luaFire->committedTick) + " (event tick " + std::to_string(luaFire->eventTick) + ", seq " + std::to_string(luaFire->seq) + ", predicted=" + std::to_string(luaFire->predicted ? 1 : 0) + "), expected <= " + std::to_string(press + 1)
		              : "no physical voice from equipped AK-47 uid=" + std::to_string(s_eventLedgerLuaEmitterUID) + " at or after tick " + std::to_string(press));
		if (luaFire) {
			size_t luaSameKey = 0;
			for (const PreviewEventLedger::EventStart& start: starts) {
				if (start.kind == luaFire->kind && start.emitterUID == luaFire->emitterUID && start.eventTick == luaFire->eventTick && start.seq == luaFire->seq) {
					++luaSameKey;
				}
			}
			check("the_event_reaches_the_output_once", luaSameKey == 1, std::to_string(luaSameKey) + " physical starts for that event");
		}
	}
	const uint64_t pressEmitter = tracked ? tracked->emitterUID : 0;
	const PreviewEventLedger::EventStart* round = nullptr;
	for (const PreviewEventLedger::EventStart& start: starts) {
		if (start.kind == PreviewEventLedger::Projectile && start.committedTick >= press && (!pressEmitter || start.emitterUID == pressEmitter)) {
			round = &start;
			break;
		}
	}
	check("the_first_round_is_visible_on_the_preview_tick", round && round->committedTick <= press + 1 && round->predicted,
	      round ? "first projectile for the press at committed tick " + std::to_string(round->committedTick) + " (event tick " + std::to_string(round->eventTick) + ", seq " + std::to_string(round->seq) + ", predicted=" + std::to_string(round->predicted ? 1 : 0) + "), expected <= " + std::to_string(press + 1)
	           : "no projectile from the press's emitter at or after tick " + std::to_string(press));
	size_t projectileAdoptions = 0;
	if (round) {
		for (const PreviewEventLedger::EventStart& start: starts) {
			if (start.kind == round->kind && start.emitterUID == round->emitterUID && start.eventTick == round->eventTick && start.seq == round->seq && !start.predicted) {
				++projectileAdoptions;
			}
		}
	}
	check("the_projectile_is_adopted_once", projectileAdoptions == 1, std::to_string(projectileAdoptions) + " adoptions of that projectile");
	// The ghost the previewed shot left behind holds the last preview pose until the canonical particle adopts it.
	uint64_t adoptionTick = 0;
	PreviewEventLedger::Key trackedKey;
	if (round) {
		trackedKey.kind = PreviewEventLedger::Projectile;
		trackedKey.emitterUID = round->emitterUID;
		trackedKey.tick = round->eventTick;
		trackedKey.seq = round->seq;
		for (const PreviewEventLedger::EventStart& start: starts) {
			if (start.kind == round->kind && start.emitterUID == round->emitterUID && start.eventTick == round->eventTick && start.seq == round->seq && !start.predicted) {
				adoptionTick = start.committedTick;
				break;
			}
		}
	}
	const auto isTracked = [&trackedKey, round](const GhostSample& sample) {
		return sample.any && round && sample.key.kind == trackedKey.kind && sample.key.emitterUID == trackedKey.emitterUID && sample.key.tick == trackedKey.tick && sample.key.seq == trackedKey.seq;
	};
	bool sawGhost = false;
	uint64_t firstGhostTick = 0;
	uint64_t lastGhostTick = 0;
	Vector lastGhostVel;
	bool ghostAtOrAfterAdoption = false;
	bool prevValid = false;
	GhostSample prev;
	size_t travelTicks = 0;
	size_t frozenTicks = 0;
	size_t heldTicks = 0;
	double worstMotionError = 0.0;
	const float sampleDt = g_TimerMan.GetDeltaTimeSecs();
	const Vector gravity = g_SceneMan.GetGlobalAcc();
	// The canonical motion the adopted particle will run: gravity, air drag, wrap, terrain stop-and-hold.
	const auto applyForcesCopy = [&](GhostSample state) {
		Vector vel = state.vel + gravity * state.globalAccScalar * sampleDt;
		if (state.airResistance > 0 && vel.GetLargest() >= state.airThreshold) {
			vel *= 1.0F - (state.airResistance * sampleDt);
		}
		Vector pos = state.pos + vel * sampleDt;
		g_SceneMan.WrapPosition(pos);
		const int pixelX = static_cast<int>(std::floor(pos.m_X));
		const int pixelY = static_cast<int>(std::floor(pos.m_Y));
		if (g_SceneMan.IsWithinBounds(pixelX, pixelY, 0) && g_SceneMan.GetTerrMatter(pixelX, pixelY) != g_MaterialAir) {
			state.vel = Vector(0, 0);
			return state;
		}
		state.vel = vel;
		state.pos = pos;
		return state;
	};
	const double gravityTerm = gravity.GetMagnitude() * static_cast<double>(sampleDt) * static_cast<double>(sampleDt);
	const double motionSlack = gravityTerm * 0.5;
	for (const GhostSample& sample: s_eventLedgerGhostSamples) {
		if (!isTracked(sample)) {
			if (!sample.any) {
				prevValid = false;
			}
			continue;
		}
		if (!sawGhost) {
			sawGhost = true;
			firstGhostTick = sample.tick;
		}
		lastGhostTick = sample.tick;
		lastGhostVel = sample.vel;
		if (adoptionTick != 0 && sample.tick >= adoptionTick) {
			ghostAtOrAfterAdoption = true;
		}
		if (sample.adopted) {
			// Past its adoption the ghost holds the pose it is handing over; only the lead is travelled.
			prevValid = false;
			continue;
		}
		if (prevValid && sample.tick == prev.tick + 1) {
			const GhostSample expected = applyForcesCopy(prev);
			worstMotionError = std::max(worstMotionError, static_cast<double>((sample.pos - expected.pos).GetMagnitude()));
			if ((sample.pos - prev.pos).GetMagnitude() > 0.01) {
				++travelTicks;
			} else if (sample.vel.GetMagnitude() <= 0.01 && travelTicks >= 1) {
				++heldTicks;
			} else {
				++frozenTicks;
			}
		}
		prev = sample;
		prevValid = true;
	}
	// The seamless swap: the ghost keeps the pixel through its lead, then hands it to the particle it led.
	const SwapRecord* swap = nullptr;
	for (const SwapRecord& record: s_eventLedgerSwaps) {
		if (adoptionTick != 0 && record.adoptionTick == adoptionTick) {
			swap = &record;
		}
	}
	size_t heldSamples = 0;
	size_t heldSteps = 0;
	bool ghostAtSwapTick = false;
	bool bothAtLastLedTick = false;
	double worstHeldMotionError = 0.0;
	bool prevHeldValid = false;
	GhostSample prevHeld;
	for (const GhostSample& sample: s_eventLedgerGhostSamples) {
		if (!isTracked(sample) || !sample.adopted) {
			continue;
		}
		if (sample.adopteeHeld) {
			++heldSamples;
		}
		if (swap && sample.tick == swap->tick) {
			ghostAtSwapTick = true;
		}
		if (swap && sample.tick + 1 == swap->tick && sample.adopteeHeld) {
			bothAtLastLedTick = true;
		}
		// The held particle's own motion, scored against the same ApplyForces copy the ghost rows use.
		if (prevHeldValid && sample.tick == prevHeld.tick + 1) {
			GhostSample from = prevHeld;
			from.pos = prevHeld.adopteePos;
			from.vel = prevHeld.adopteeVel;
			from.globalAccScalar = prevHeld.adopteeGlobalAccScalar;
			from.airResistance = prevHeld.adopteeAirResistance;
			from.airThreshold = prevHeld.adopteeAirThreshold;
			const GhostSample expected = applyForcesCopy(from);
			worstHeldMotionError = std::max(worstHeldMotionError, static_cast<double>((sample.adopteePos - expected.pos).GetMagnitude()));
			++heldSteps;
		}
		prevHeld = sample;
		prevHeldValid = sample.adopteeHeld;
	}
	const auto poseText = [](const Vector& pose) { return std::to_string(pose.m_X) + "," + std::to_string(pose.m_Y); };
	const std::string swapDetail = swap ? "lead " + std::to_string(swap->leadTicks) + " ticks, adopted at " + std::to_string(swap->adoptionTick) + ", swapped at " + std::to_string(swap->tick) +
	                                          ", ghost pose " + poseText(swap->ghostPos) + ", particle pose " + poseText(swap->adopteePos) + ", delta " + std::to_string(swap->poseDelta) + " px, particle " +
	                                          (swap->adopteeAlive ? (swap->adopteeHeldAfter ? "still held" : "shown") : "gone")
	                                    : "no swap recorded for the tracked round: the ghost went at adoption and the pixel jumped back to the muzzle";
	bool trackedMoved = false;
	for (const PreviewEventLedger::Key& key: s_eventLedgerGhostMovedKeys) {
		GhostSample moved;
		moved.any = true;
		moved.key = key;
		if (isTracked(moved)) {
			trackedMoved = true;
		}
	}
	check("the_ghost_appears_on_the_preview_tick", sawGhost && firstGhostTick <= press + 1,
	      sawGhost ? "first ghost at committed tick " + std::to_string(firstGhostTick) + ", expected <= " + std::to_string(press + 1) : "no ghost sampled in the run");
	check("the_ghost_travels_every_committed_tick", sawGhost && travelTicks >= 1 && frozenTicks == 0,
	      !sawGhost ? "no ghost sampled in the run" : std::to_string(travelTicks) + " travelled ticks, " + std::to_string(heldTicks) + " stop-and-hold ticks, " + std::to_string(frozenTicks) + " frozen ticks (a frozen tick is a ghost that neither moved nor stopped on terrain), last vel " + std::to_string(lastGhostVel.GetMagnitude()));
	check("the_ghost_matches_the_canonical_motion", sawGhost && travelTicks >= 1 && worstMotionError < motionSlack,
	      "worst |sample - ApplyForces copy of the previous sample| " + std::to_string(worstMotionError) + " px over " + std::to_string(travelTicks + heldTicks + frozenTicks) + " scored steps, slack " + std::to_string(motionSlack) + " (half |g|dt^2=" + std::to_string(gravityTerm) + "; a gravity-less step differs by that term and fails)");
	check("the_ghost_hands_the_pixel_over_at_the_swap", adoptionTick != 0 && sawGhost && ghostAtOrAfterAdoption && swap != nullptr && lastGhostTick + 1 == swap->tick,
	      "last ghost at committed tick " + std::to_string(lastGhostTick) + ", adoption at " + std::to_string(adoptionTick) + "; " + swapDetail);
	check("the_adoption_does_not_snap",
	      swap != nullptr && swap->leadTicks > 0 && heldSamples == swap->leadTicks && bothAtLastLedTick && !ghostAtSwapTick && swap->adopteeAlive && !swap->adopteeHeldAfter && static_cast<double>(swap->poseDelta) <= motionSlack,
	      swapDetail + "; the particle was held on " + std::to_string(heldSamples) + " of the " + (swap ? std::to_string(swap->leadTicks) : std::string("0")) + " led ticks, both drawn at the last led tick " + std::to_string(bothAtLastLedTick ? 1 : 0) + ", a ghost was still there at the swap tick " + std::to_string(ghostAtSwapTick ? 1 : 0) + ", slack " + std::to_string(motionSlack) + " px");
	check("hidden_adoptee_still_simulates", swap != nullptr && heldSteps >= 1 && worstHeldMotionError < motionSlack,
	      "the held particle's worst |sample - ApplyForces copy of the previous sample| " + std::to_string(worstHeldMotionError) + " px over " + std::to_string(heldSteps) + " held steps, slack " + std::to_string(motionSlack) + " (the unhidden control run is the no-prediction compare of the three swap dump probes)");
	check("dumps_byte_identical_across_the_swap", s_eventLedgerHoldDumpPoints >= 1 && s_eventLedgerHoldDumpDiffs == 0 && s_eventLedgerSwapDumpT0 && s_eventLedgerSwapDumpMid && s_eventLedgerSwapDumpEnd,
	      std::to_string(s_eventLedgerHoldDumpPoints) + " ticks dumped with the hold on and off, " + std::to_string(s_eventLedgerHoldDumpDiffs) + " of them differed; probes written adoption=" + std::to_string(s_eventLedgerSwapDumpT0 ? 1 : 0) + " last_led=" + std::to_string(s_eventLedgerSwapDumpMid ? 1 : 0) + " swap=" + std::to_string(s_eventLedgerSwapDumpEnd ? 1 : 0));
	check("the_ghost_stays_off_the_moid_grid", !s_eventLedgerGhostRegistered,
	      s_eventLedgerGhostRegistered ? "a ghost had a MOID or stayed in the world lists the dump walks" : "ghosts stayed unregistered");
	check("ghost_travel_leaves_dumps_byte_identical", s_eventLedgerGhostDumpTaken && trackedMoved && s_eventLedgerGhostDumpIdentical,
	      !s_eventLedgerGhostDumpTaken ? "no dump snapshot around a preview that moved a ghost" : (!trackedMoved ? "the tracked round's ghost did not move in that window" : (s_eventLedgerGhostDumpIdentical ? "dump+extras unchanged after the ghost moved" : "dump or extras changed after the ghost moved")));
	if (!s_eventLedgerExpireDroppedGhost && MovableMan::IsConstructed()) {
		PreviewEventLedger::Key expireKey;
		expireKey.kind = PreviewEventLedger::Projectile;
		expireKey.emitterUID = 1;
		expireKey.tick = 1;
		expireKey.seq = 99;
		PreviewEventLedger::Insert(expireKey, {});
		MovableMan::InstallPreviewGhostForSelfTest(new MOPixel(), expireKey, expireKey.tick);
		const uint64_t expiredBefore = PreviewEventLedger::GetCounters().expired;
		PreviewEventLedger::ExpireForTick(expireKey.tick + 2);
		const std::vector<MovableMan::PreviewGhostState> left = g_MovableMan.GetPreviewGhostStates();
		const bool plantedGone = std::none_of(left.begin(), left.end(), [&expireKey](const MovableMan::PreviewGhostState& ghost) {
			return ghost.key.kind == expireKey.kind && ghost.key.emitterUID == expireKey.emitterUID && ghost.key.tick == expireKey.tick && ghost.key.seq == expireKey.seq;
		});
		s_eventLedgerExpireDroppedGhost = PreviewEventLedger::GetCounters().expired > expiredBefore && plantedGone;
		s_eventLedgerExpireDetail = "expired " + std::to_string(expiredBefore) + " -> " + std::to_string(PreviewEventLedger::GetCounters().expired) +
		    ", planted ghost " + std::string(plantedGone ? "dropped" : "still installed") + ", " + std::to_string(left.size()) + " ghosts left";
	}
	check("expired_ghost_vanishes", MovableMan::IsConstructed() && s_eventLedgerExpireDroppedGhost,
	      !MovableMan::IsConstructed() ? "MovableMan is not constructed in this host, so no ghost could be planted" : s_eventLedgerExpireDetail);
	if (MovableMan::IsConstructed()) {
		// One live ghost per key: a second install would leave a ghost nothing reposes, adopts or drops.
		PreviewEventLedger::Key twinKey;
		twinKey.kind = PreviewEventLedger::Projectile;
		twinKey.emitterUID = 2;
		twinKey.tick = 3;
		twinKey.seq = 98;
		const size_t ghostsBefore = g_MovableMan.GetPreviewGhostStates().size();
		const bool firstInstalled = MovableMan::InstallPreviewGhostForSelfTest(new MOPixel(), twinKey, twinKey.tick + 4);
		MOPixel* twin = new MOPixel();
		const bool secondInstalled = MovableMan::InstallPreviewGhostForSelfTest(twin, twinKey, twinKey.tick + 4);
		if (!secondInstalled) {
			delete twin;
		}
		const size_t ghostsAfter = g_MovableMan.GetPreviewGhostStates().size();
		g_MovableMan.DropPreviewGhost(twinKey);
		const size_t ghostsDropped = g_MovableMan.GetPreviewGhostStates().size();
		check("one_live_ghost_per_key", firstInstalled && !secondInstalled && ghostsAfter == ghostsBefore + 1 && ghostsDropped == ghostsBefore,
		      "installs " + std::to_string(firstInstalled ? 1 : 0) + "/" + std::to_string(secondInstalled ? 1 : 0) + ", ghosts " +
		          std::to_string(ghostsBefore) + " -> " + std::to_string(ghostsAfter) + " -> " + std::to_string(ghostsDropped) + " after the drop");
	}
	{
		// Two held ghosts, the first of them mid-lead: the sampler must still reach the second.
		const int candidatesBefore = s_eventLedgerHoldSampleCandidates;
		std::vector<MovableMan::PreviewGhostState> twoGhosts(2);
		twoGhosts[0].adopteeHeld = true;
		twoGhosts[0].adoptionTick = 1;
		twoGhosts[0].poseTick = 10;
		twoGhosts[1].adopteeHeld = true;
		twoGhosts[1].adoptionTick = 5;
		twoGhosts[1].poseTick = 20;
		SampleSeamlessSwap(5, twoGhosts);
		check("every_held_ghost_is_sampled", s_eventLedgerHoldSampleCandidates == candidatesBefore + 1,
		      "the sampler reached " + std::to_string(s_eventLedgerHoldSampleCandidates - candidatesBefore) + " of the 1 ghost at its tick behind a ghost that was not");
	}
	// A guard, not a detector: the muzzle flash sprite is already drawn on the preview that fires.
	check("the_flash_sprite_stays_on_the_preview_tick", s_eventLedgerFlashTick > 0 && static_cast<uint64_t>(s_eventLedgerFlashTick) <= press + 1,
	      "the previewed firearm's flash frame is first set at committed tick " + std::to_string(s_eventLedgerFlashTick) + ", expected <= " + std::to_string(press + 1));
	const PreviewEventLedger::Counters& counters = PreviewEventLedger::GetCounters();
	check("the_counters_balance", counters.playedAtPreview == counters.adoptedAtCommit + counters.expired + PreviewEventLedger::GetLiveEntryCount(),
	      PreviewEventLedger::Describe() + " live=" + std::to_string(PreviewEventLedger::GetLiveEntryCount()));
	{
		std::ostringstream line;
		line << "[preview-event-selftest] " << (passed ? "PASS" : "FAIL") << " press tick " << press;
		System::PrintDiagnosticLine(line.str());
	}
	if (!passed) {
		s_netReplayExitCode = 5;
	}
}

// A requested test that never reached its tick is a failed test; stopping early cannot pass it.
static void CheckRequiredProbesCompleted() {
	const long long stoppedAt = g_TimerMan.GetSimUpdateCount();
	CheckPreviewEventLedgerSelfTest();
	if ((s_eventLedgerPressTick > 0 || s_lpInvarianceTick > 0) && !PreviewScriptSelfTest::CheckNestedHookScope()) {
		s_netReplayExitCode = 5;
	}
	if (PreviewScriptSelfTest::SubtreeProbeEnabled() && !PreviewScriptSelfTest::CheckSubtreeEmitter(s_eventLedgerPressTick)) {
		s_netReplayExitCode = 5;
	}
	if (LocalPredictionHudSelfTest::g_PressTick > 0) {
		if (!LocalPredictionHudSelfTest::g_Sampled && !LocalPredictionHudSelfTest::g_Checked) {
			{
				std::ostringstream line;
				line << "[preview-hud-selftest] FAIL: hud sample at tick " << LocalPredictionHudSelfTest::g_PressTick << " never executed (the run stopped at tick " << stoppedAt << ")";
				System::PrintDiagnosticLine(line.str());
			}
			s_netReplayExitCode = 5;
			LocalPredictionHudSelfTest::g_Checked = true;
		} else if (!LocalPredictionHudSelfTest::Check()) {
			s_netReplayExitCode = 5;
		}
	}
	if (s_lpInvarianceTick > 0 && s_lpInvarianceFailures < 0) {
		{
			std::ostringstream line;
			line << "[lpinv] FAIL: invariance test at tick " << s_lpInvarianceTick << " never executed (the run stopped at tick " << stoppedAt << ")";
			System::PrintDiagnosticLine(line.str());
		}
		g_MetricsCollector.RecordString("lpinv_result", "not_run");
		s_netReplayExitCode = 5;
	}
	if (s_rbProbeRequested && s_rbProbePhase != 4) {
		{
			std::ostringstream line;
			line << "[rbprobe] FIDELITY FAIL: the probe at tick " << s_rbProbeAtTick << " did not complete (phase " << s_rbProbePhase << " when the run stopped at tick " << stoppedAt << ")";
			System::PrintDiagnosticLine(line.str());
		}
		g_MetricsCollector.RecordString("rbprobe_result", "incomplete");
		if (s_netReplayExitCode == 0) {
			s_netReplayExitCode = 1;
		}
	}
}

static void DumpTerrainIfArmed(uint64_t simTick) {
	static uint64_t s_tick = 0;
	static bool s_checked = false;
	if (!s_checked) {
		s_checked = true;
		if (const char* env = std::getenv("CC_TERRAIN_DUMP")) {
			s_tick = std::strtoull(env, nullptr, 10);
		}
	}
	if (s_tick == 0 || simTick != s_tick) {
		return;
	}
	DumpTerrainNow("t" + std::to_string(simTick));
}

// Every object registered at the capture that is still registered afterwards must hold the same Lua object.
static bool LuaIdentityPreserved(const std::string& atCapture, const std::string& now) {
	std::map<std::string, std::string> before;
	std::istringstream in(atCapture);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string state;
		words >> state;
		std::string entry;
		while (words >> entry) {
			before[state + ":" + entry.substr(0, entry.find('@'))] = entry;
		}
	}
	std::istringstream after(now);
	while (std::getline(after, line)) {
		std::istringstream words(line);
		std::string state;
		words >> state;
		std::string entry;
		while (words >> entry) {
			const auto it = before.find(state + ":" + entry.substr(0, entry.find('@')));
			if (it != before.end() && it->second != entry) {
				return false;
			}
		}
	}
	return true;
}

// The harness's own captures, checked the way the engine's are: a root that has lost its last reference
// must not survive into a capture the settle is about to sweep.
static bool RunHarnessCaptureSelfTest() {
	// A Lua-owned scripted object with its last reference dropped and nothing collected yet.
	const auto park = [](long& uid) -> MovableObject* {
		LuaStateWrapper& master = g_LuaMan.GetMasterScriptState();
		const long first = MovableObject::GetUniqueIDCounter();
		if (master.RunScriptString("_HarnessCaptureParked = CreateMOPixel(\"Spark Yellow 1\", \"Base.rte\")") != 0) {
			return nullptr;
		}
		MovableObject* parked = nullptr;
		for (long candidate = first; candidate <= MovableObject::GetUniqueIDCounter() && !parked; ++candidate) {
			parked = g_MovableMan.FindObjectByUniqueID(candidate);
			uid = candidate;
		}
		if (parked) {
			// Registered with initialized scripts is what makes it a graph root.
			parked->MoveScriptsToState(master);
			parked->AdoptScriptObject();
		}
		master.RunScriptString("_HarnessCaptureParked = nil");
		return parked;
	};
	const auto settleThroughAHold = []() {
		MovableMan::WorldSetAside aside;
		return g_MovableMan.SetAsideWorld(aside, false) && g_MovableMan.ReinstateWorld(aside);
	};
	bool probeOrder = false;
	long probeUID = 0;
	if (park(probeUID) && probeUID > 0) {
		const std::vector<std::string> heldCapture = s_rbProbeLuaGraphsAtCapture;
		const bool heldMismatch = s_rbProbeRestoreMismatch;
		s_rbProbeRestoreMismatch = false;
		const bool captured = CaptureProbeScriptGraphs();
		const bool settled = settleThroughAHold();
		CheckRestoredScriptGraphs();
		const bool swept = g_MovableMan.FindObjectByUniqueID(probeUID) == nullptr;
		probeOrder = captured && settled && swept && !s_rbProbeRestoreMismatch;
		{
			std::ostringstream line;
			line << "[harness-order] probe uid=" << probeUID << " captured=" << captured << " settled=" << settled
			     << " swept=" << swept << " mismatch=" << s_rbProbeRestoreMismatch;
			System::PrintDiagnosticLine(line.str());
		}
		s_rbProbeLuaGraphsAtCapture = heldCapture;
		s_rbProbeRestoreMismatch = heldMismatch;
	}
	{
		std::ostringstream line;
		line << "[script-graph-selftest] " << (probeOrder ? "PASS" : "FAIL") << " rollback_probe_capture_settles_first";
		System::PrintDiagnosticLine(line.str());
	}
	bool observeOrder = false;
	long observeUID = 0;
	if (park(observeUID) && observeUID > 0) {
		std::vector<std::string> before, after, problems;
		const bool first = ObserveScriptGraphs(before, problems);
		const bool settled = settleThroughAHold();
		const bool second = ObserveScriptGraphs(after, problems);
		const bool swept = g_MovableMan.FindObjectByUniqueID(observeUID) == nullptr;
		observeOrder = first && settled && second && swept && before == after;
		{
			std::ostringstream line;
			line << "[harness-order] observe uid=" << observeUID << " swept=" << swept
			     << " graphs_equal=" << (before == after);
			System::PrintDiagnosticLine(line.str());
		}
	}
	{
		std::ostringstream line;
		line << "[script-graph-selftest] " << (observeOrder ? "PASS" : "FAIL") << " contract_audit_observation_settles_first";
		System::PrintDiagnosticLine(line.str());
	}
	// A scenario script starting under a CLI trace run must join it: its own BeginRun would drop the
	// armed tick-hash trace and leave the trace file without hashes.
	bool scriptJoinsTheHostRun = false;
	{
		g_MetricsCollector.BeginHostRun("HostOwnedRun", 7);
		g_MetricsCollector.SetRecordTickHashes(true);
		SimChecksum::Result sample;
		sample.tick = 1;
		g_MetricsCollector.RecordTickHash(sample);
		const size_t armed = g_MetricsCollector.GetTickHashCount();
		const int scriptError = g_LuaMan.GetMasterScriptState().RunScriptString(
		    "MetricsCollector:BeginRun(\"ScriptOwnedRun\", 0); MetricsCollector:EndRun();");
		const MetricsCollector::AggregatedRun joined = g_MetricsCollector.GetCurrentRun();
		const auto named = joined.stringValues.find("scenario");
		scriptJoinsTheHostRun = scriptError == 0 && armed == 1 && joined.tickHashCount == 1 &&
		                        g_MetricsCollector.IsRecordingTickHashes() && joined.scenario == "HostOwnedRun" &&
		                        named != joined.stringValues.end() && named->second == "ScriptOwnedRun";
		{
			std::ostringstream line;
			line << "[harness-order] metrics armed=" << armed << " after_script=" << joined.tickHashCount
			     << " recording=" << g_MetricsCollector.IsRecordingTickHashes() << " run=" << joined.scenario
			     << " script=" << (named != joined.stringValues.end() ? named->second : std::string("-"));
			System::PrintDiagnosticLine(line.str());
		}
		g_MetricsCollector.Destroy();
	}
	{
		std::ostringstream line;
		line << "[script-graph-selftest] " << (scriptJoinsTheHostRun ? "PASS" : "FAIL") << " scenario_script_joins_the_host_metrics_run";
		System::PrintDiagnosticLine(line.str());
	}
	// Last in the run: a build that fails this one leaves a world held.
	bool refusalKeepsTheNextHold = false;
	{
		bool refused = false;
		bool stillHeld = false;
		{
			// A caller that gives up on a refused reinstate, exactly as RestartActivity's local record does.
			MovableMan::WorldSetAside aside;
			if (g_MovableMan.SetAsideWorld(aside, false)) {
				aside.runtimeGlobals = "not a runtime globals archive";
				refused = !g_MovableMan.ReinstateWorld(aside);
				stillHeld = aside.held && g_MovableMan.HasWorldSetAside();
			}
		}
		MovableMan::WorldSetAside next;
		const bool nextHold = g_MovableMan.SetAsideWorld(next, false);
		refusalKeepsTheNextHold = refused && stillHeld && nextHold && g_MovableMan.ReinstateWorld(next) && !g_MovableMan.HasWorldSetAside();
		{
			std::ostringstream line;
			line << "[setaside-latch] refused=" << refused << " still_held=" << stillHeld << " next_hold=" << nextHold;
			System::PrintDiagnosticLine(line.str());
		}
	}
	{
		std::ostringstream line;
		line << "[script-graph-selftest] " << (refusalKeepsTheNextHold ? "PASS" : "FAIL") << " refused_reinstate_leaves_the_next_hold_possible";
		System::PrintDiagnosticLine(line.str());
	}
	return probeOrder && observeOrder && scriptJoinsTheHostRun && refusalKeepsTheNextHold;
}

// Observes completed tick boundaries. All state restoration belongs to the production
// checkpoint APIs; the probe only rewinds its recorded input stream and compares observations.
void RollbackProbeOnHashedTick(uint64_t simTick, const SimChecksum::Result& tickResult) {
	if (s_rbProbeFuzzCount > 0 && s_rbProbeSchedule.empty() && s_rbProbeAtTick <= 0 && s_rbProbePhase == 0) {
		// Lay out the random capture ticks once the run's cap is known; cycles never overlap.
		const long long maxTick = static_cast<long long>(ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 1800);
		const long long first = std::max<long long>(40, static_cast<long long>(simTick) + 2);
		const long long last = maxTick - s_rbProbeWindow - 4;
		std::mt19937_64 rng(s_rbProbeFuzzSeed);
		std::vector<long long> picks;
		for (int n = 0; n < s_rbProbeFuzzCount && last > first; ++n) {
			picks.push_back(first + static_cast<long long>(rng() % static_cast<uint64_t>(last - first + 1)));
		}
		std::sort(picks.begin(), picks.end());
		long long floor = first;
		for (long long pick: picks) {
			const long long tick = std::max(pick, floor);
			if (tick > last) {
				break;
			}
			s_rbProbeSchedule.push_back(tick);
			floor = tick + s_rbProbeWindow + 3;
		}
		if (s_rbProbeSchedule.empty()) {
			{
				std::ostringstream line;
				line << "[rbfuzz] no room for probes under the tick cap";
				System::PrintDiagnosticLine(line.str());
			}
			s_rbProbeFuzzCount = 0;
			return;
		}
		s_rbProbeAtTick = s_rbProbeSchedule.front();
		s_rbProbeSchedule.pop_front();
		{
			std::ostringstream line;
			line << "[rbfuzz] " << (s_rbProbeSchedule.size() + 1) << " probes scheduled, window " << s_rbProbeWindow << ", first at " << s_rbProbeAtTick;
			System::PrintDiagnosticLine(line.str());
		}
	}
	if (s_rbProbePhase == 0 && simTick == static_cast<uint64_t>(s_rbProbeAtTick)) {
		const auto captureStart = std::chrono::steady_clock::now();
		double worldCaptureMs = 0.0;
		if (!CaptureProbeScriptGraphs()) {
			System::SetQuit(true);
			return;
		}
		if (s_rbProbeInMemory) {
			const auto worldStart = std::chrono::steady_clock::now();
			if (!g_MovableMan.CaptureWorld(s_rbProbeWorld)) {
				{
					std::ostringstream line;
					line << "[rbprobe] FAIL: world capture refused (add queues not drained)";
					System::PrintDiagnosticLine(line.str());
				}
				System::SetQuit(true);
				return;
			}
			worldCaptureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - worldStart).count();
		} else if (!g_ActivityMan.SaveCurrentGame(RollbackProbeSaveName())) {
			{
				std::ostringstream line;
				line << "[rbprobe] FAIL: the capture save was refused";
				System::PrintDiagnosticLine(line.str());
			}
			System::SetQuit(true);
			return;
		}
		s_rbProbeSimCount = g_TimerMan.GetSimUpdateCount();
		s_rbProbeSimTimeTicks = g_TimerMan.GetSimTimeTicks();
		if (ScenarioRunner::IsLockstepReplayPlayback()) {
			ScenarioRunner::ArmReplayRewindBuffer(simTick + 1, static_cast<uint64_t>(s_rbProbeWindow));
		}
		const double captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count();
		{
			std::ostringstream line;
			line << "[rbprobe] capture_ms=" << captureMs << " world_ms=" << worldCaptureMs << " mos=" << (s_rbProbeWorld.actors.size() + s_rbProbeWorld.items.size() + s_rbProbeWorld.particles.size());
			System::PrintDiagnosticLine(line.str());
		}
		if (FaultInjected("snapshot_skew") && !s_rbProbeWorld.actors.empty()) {
			// A deliberately wrong captured field: the restore must be caught by the deep compare.
			Actor* skewed = s_rbProbeWorld.actors.front();
			skewed->SetVel(skewed->GetVel() + Vector(0.001F, 0.0F));
			{
				std::ostringstream line;
				line << "[rbprobe] fault injected: snapshot_skew on uid " << skewed->GetUniqueID();
				System::PrintDiagnosticLine(line.str());
			}
		}
		s_rbProbeCapturedDeep = DumpSimStateToString();
		s_rbProbeLuaIdentityAtCapture = g_MovableMan.DescribeLuaIdentity();
		WriteProbeText("rb_captured", s_rbProbeCapturedDeep);
		WriteProbeText("rb_captured_" + std::to_string(simTick), s_rbProbeCapturedDeep);
		s_rbProbeFirstDeep.clear();
		s_rbProbeDeepDivergence = -1;
		s_rbProbeRestoreMismatch = false;
		DumpTerrainNow("rb_cap");
		s_rbProbePhase = 1;
		{
			std::ostringstream line;
			line << "[rbprobe] captured at tick " << simTick;
			System::PrintDiagnosticLine(line.str());
		}
	} else if (s_rbProbePhase == 1 && simTick > static_cast<uint64_t>(s_rbProbeAtTick)) {
		s_rbProbeFirst.push_back(tickResult);
		s_rbProbeFirstDeep.push_back(DumpSimStateToString());
		if (static_cast<long long>(s_rbProbeFirst.size()) >= s_rbProbeWindow) {
			if (!s_rbProbeInMemory && !g_ActivityMan.WaitForSaveGameTask()) {
				{
					std::ostringstream line;
					line << "[rbprobe] FAIL: the capture save did not complete";
					System::PrintDiagnosticLine(line.str());
				}
				System::SetQuit(true);
				return;
			}
			if (s_rbProbeInMemory) {
				s_rbProbeMemoryRestorePending = true;
			} else if (s_rbProbeUseLoadGame) {
				s_rbProbeLaunchRestorePending = true;
			} else if (!g_ActivityMan.LoadGameToRestart(RollbackProbeSaveName())) {
				{
					std::ostringstream line;
					line << "[rbprobe] FAIL: the restore load was refused";
					System::PrintDiagnosticLine(line.str());
				}
				System::SetQuit(true);
				return;
			} else {
				g_ActivityMan.RemoveSavedGame(RollbackProbeSaveName());
			}
			s_rbProbePhase = 2;
			{
				std::ostringstream line;
				line << "[rbprobe] window recorded; restore staged";
				System::PrintDiagnosticLine(line.str());
			}
		}
	} else if (s_rbProbePhase == 3 && simTick > static_cast<uint64_t>(s_rbProbeAtTick)) {
		s_rbProbeSecond.push_back(tickResult);
		if (s_rbProbeDeepDivergence < 0 && s_rbProbeSecond.size() <= s_rbProbeFirstDeep.size()) {
			const std::string secondDeep = DumpSimStateToString();
			const std::string& firstDeep = s_rbProbeFirstDeep[s_rbProbeSecond.size() - 1];
			if (secondDeep != firstDeep) {
				s_rbProbeDeepDivergence = s_rbProbeAtTick + static_cast<long long>(s_rbProbeSecond.size());
				WriteProbeText("rb_deep_pass1", firstDeep);
				WriteProbeText("rb_deep_pass2", secondDeep);
				WriteProbeText("rb_deep_" + std::to_string(s_rbProbeAtTick) + "_pass1", firstDeep);
				WriteProbeText("rb_deep_" + std::to_string(s_rbProbeAtTick) + "_pass2", secondDeep);
			}
		}
		if (static_cast<long long>(s_rbProbeSecond.size()) >= s_rbProbeWindow) {
			long long firstDivergence = -1;
			size_t divergentIndex = 0;
			for (size_t i = 0; i < s_rbProbeFirst.size(); ++i) {
				if (SimChecksum::SimGatedHash(s_rbProbeFirst[i]) != SimChecksum::SimGatedHash(s_rbProbeSecond[i])) {
					firstDivergence = s_rbProbeAtTick + 1 + static_cast<long long>(i);
					divergentIndex = i;
					break;
				}
			}
			if (s_rbProbeInMemory) {
				// The re-run is discarded; the run continues on the originals, whose Lua objects never left.
				if (!g_MovableMan.ReinstateWorld(s_rbProbeOriginals)) {
					s_rbProbeRestoreMismatch = true;
					System::SetQuit(true);
				}
				const std::string identityNow = g_MovableMan.DescribeLuaIdentity();
				if (!LuaIdentityPreserved(s_rbProbeLuaIdentityAtCapture, identityNow)) {
					s_rbProbeRestoreMismatch = true;
					WriteProbeText("rb_lua_identity_capture", s_rbProbeLuaIdentityAtCapture);
					WriteProbeText("rb_lua_identity_after", identityNow);
					{
						std::ostringstream line;
						line << "[rbprobe] FIDELITY FAIL: a Lua object identity changed across the probe (capture " << s_rbProbeAtTick << ")";
						System::PrintDiagnosticLine(line.str());
					}
				}
			}
			if (firstDivergence < 0 && s_rbProbeDeepDivergence < 0 && !s_rbProbeRestoreMismatch) {
				++s_rbProbePassCount;
				{
					std::ostringstream line;
					line << "[rbprobe] FIDELITY PASS: " << s_rbProbeWindow << " ticks byte-identical after the restore, hash and full dump (capture " << s_rbProbeAtTick << ")";
					System::PrintDiagnosticLine(line.str());
				}
			} else if (firstDivergence < 0) {
				++s_rbProbeFailCount;
				const std::string where = s_rbProbeRestoreMismatch ? "restore mismatch at capture " + std::to_string(s_rbProbeAtTick) : "dump divergence at tick " + std::to_string(s_rbProbeDeepDivergence);
				{
					std::ostringstream line;
					line << "[rbprobe] FIDELITY FAIL: hashes identical but " << where << " (capture " << s_rbProbeAtTick << ")";
					System::PrintDiagnosticLine(line.str());
				}
				if (s_rbProbeFirstFailure.empty()) {
					s_rbProbeFirstFailure = "capture " + std::to_string(s_rbProbeAtTick) + ": " + where;
				}
			} else {
				++s_rbProbeFailCount;
				std::string divergentSubsystems;
				for (const auto& [name, hash]: s_rbProbeFirst[divergentIndex].per_subsystem) {
					if (name == "controller") {
						continue;
					}
					const auto secondIt = s_rbProbeSecond[divergentIndex].per_subsystem.find(name);
					if (secondIt == s_rbProbeSecond[divergentIndex].per_subsystem.end() || secondIt->second != hash) {
						divergentSubsystems += (divergentSubsystems.empty() ? "" : ",") + name;
					}
				}
				{
					std::ostringstream line;
					line << "[rbprobe] FIDELITY FAIL: first divergence at tick " << firstDivergence
					     << " subsystems=" << divergentSubsystems << " dump=" << (s_rbProbeRestoreMismatch ? std::string("restore mismatch") : std::to_string(s_rbProbeDeepDivergence)) << " (capture " << s_rbProbeAtTick << ")";
					System::PrintDiagnosticLine(line.str());
				}
				if (s_rbProbeFirstFailure.empty()) {
					s_rbProbeFirstFailure = "capture " + std::to_string(s_rbProbeAtTick) + " diverged at " + std::to_string(firstDivergence) + " [" + divergentSubsystems + "]";
				}
			}
			// Both passes' tracked-UID rows are in the tracer buffer; flush them for the fidelity diff.
			SceneMan::FlushTerrainEvents((!ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim")) + ".rbprobe.terrainevents.txt");
			s_rbProbeFirst.clear();
			s_rbProbeSecond.clear();
			s_rbProbeFirstDeep.clear();
			if (!s_rbProbeSchedule.empty()) {
				s_rbProbeAtTick = s_rbProbeSchedule.front();
				s_rbProbeSchedule.pop_front();
				s_rbProbePhase = 0;
				return;
			}
			s_rbProbePhase = 4;
			g_MetricsCollector.RecordString("rbprobe_result", s_rbProbeFailCount == 0 ? "pass" : "fail");
			if (s_rbProbeFuzzCount > 0) {
				{
					std::ostringstream line;
					line << "[rbfuzz] " << (s_rbProbeFailCount == 0 ? "PASS" : "FAIL") << " " << s_rbProbePassCount << "/" << (s_rbProbePassCount + s_rbProbeFailCount) << " probes byte-identical";
					if (s_rbProbeFailCount > 0) {
						line << "; first failure: " << s_rbProbeFirstFailure;
					}
					System::PrintDiagnosticLine(line.str());
				}
			}
			if (s_rbProbeFailCount > 0) {
				s_netReplayExitCode = 1;
				System::SetQuit(true);
			}
		}
	}
}

/// </summary>
/// Whether the activity ended with a winning team: a won round, however short.
static bool NetMatchActivityHasWinner(const Activity* activity) {
	const GameActivity* game = dynamic_cast<const GameActivity*>(activity);
	return game && game->GetWinnerTeam() != Activity::NoTeam;
}

static bool E2ERematchesLeft() {
	const auto& args = ScenarioRunner::GetArgs();
	// The cross driver's rematch budget counts here as it does for a round that ends in play, so a seat whose round ended while it caught up follows the match.
	const int rematches = s_crossRematches ? static_cast<int>(s_crossRematches)
	                                       : static_cast<int>(std::max<uint32_t>(args.selftestRematch ? 1 : 0, args.selftestRematches));
	return s_netMatchServiceE2ERematches < rematches;
}

static bool IsE2ERematchReady() {
	const Activity* activity = g_ActivityMan.GetActivity();
	const auto& args = ScenarioRunner::GetArgs();
	const int rematches = s_crossRematches ? static_cast<int>(s_crossRematches)
	                                       : static_cast<int>(std::max<uint32_t>(args.selftestRematch ? 1 : 0, args.selftestRematches));
	return s_netMatchServiceE2E && s_netMatchServiceE2ERematches < rematches && activity && activity->IsOver() &&
	       (s_crossRematches || !s_netMatchE2ETicks.EarlyOverIsSetupFailure(s_netMatchE2ETicks.Total(), NetMatchActivityHasWinner(activity)));
}

static bool CrossWinSurfaceReady() {
	if (!s_crossRematches) return true;
	static uint64_t round = 0, began = 0;
	const uint64_t current = ScenarioRunner::GetLockstepRoundId(), tick = g_TimerMan.GetSimUpdateCount();
	if (round != current) { round = current; began = tick; }
	if (tick - began < 300) return false;
	const auto path = std::filesystem::path(CrossEnvironment("CC_TEST_CROSS_RECORDS")).parent_path() /
	    ("round-" + std::to_string(current) + "-win.png");
	const bool saved = g_FrameMan.SaveBitmapToPNG(g_FrameMan.GetBackBuffer32(), path.string().c_str()) == 0;
	g_MetricsCollector.WriteObservation({{"type", "win_surface"}, {"result", BuildNetMatchResultText()},
	    {"screen_text", g_FrameMan.GetScreenText(0)}, {"grace_ticks", tick - began}, {"screenshot", path.string()}, {"saved", saved}});
	return true;
}

static bool PrepareCrossLobbyResources(std::string* error) {
	// Service-E2E returns before normal menu startup loads these preset icons.
	// the second machine can have virtual gamepads even when the test supplies scripted input.
	g_UInputMan.LoadDeviceIcons();
	for (int device = InputDevice::DEVICE_KEYB_ONLY; device < InputDevice::DEVICE_COUNT; ++device) {
		const Icon* icon = g_UInputMan.GetDeviceIcon(device);
		if (!icon || icon->GetBitmaps32().empty() || !icon->GetBitmaps32().front()) {
			*error = "post-match lobby device icon is not loaded: " + std::to_string(device); return false;
		}
	}
	return true;
}

bool RTE::RunCrossLobbyResourcesSelfTest(std::string* error) {
	if (!PrepareCrossLobbyResources(error)) return false;
	System::PrintDiagnosticLine("[net-match-selftest] PASS cross_lobby_loads_keyboard_mouse_and_all_gamepad_icons_before_draw");
	return true;
}

static bool CrossDrawLobbySurface(std::string* error) {
	if (!s_crossRematches) return true;
	if (!PrepareCrossLobbyResources(error)) return false;
	g_TimerMan.PauseSim(true);
	g_MenuMan.HandleTransitionIntoMenuLoop();
	g_MenuMan.SkipTitleIntroForAutomation();
	g_MenuMan.SetIsInMenuScreen(true);
	g_UInputMan.DisableKeys(false);
	g_UInputMan.TrapMousePos(false);
	auto* menu = g_MenuMan.GetMainMenu();
	menu->OfferRematchLobbyOnEntry();
	const auto draw = [&] {
		PollSDLEvents(); g_WindowMan.Update(); g_UInputMan.Update(); g_TimerMan.Update();
		g_WindowMan.ClearBackbuffer();
		g_MenuMan.Update();
		g_WindowMan.GetScreenBuffer()->Begin(); g_MenuMan.Draw(); g_WindowMan.GetScreenBuffer()->End();
		g_WindowMan.UploadFrame(); g_UInputMan.EndFrame();
	};
	// The details open and close the way a host opens them: a click, a phase a drawn frame.
	SetPanelDrawRecording(true);
	const auto click = [&](const std::string& name) {
		std::string observation;
		bool passed = false;
		if (!MenuAutomation::HandClick(menu->AutomationManager(), name, menu->AutomationModalDialog(), observation)) return false;
		for (unsigned frame = 0; frame < 120 && MenuAutomation::HandBusy() && !System::IsSetToQuit(); ++frame) {
			draw();
			MenuAutomation::AfterDrawnFrame();
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
		return MenuAutomation::HandFinished(passed, observation) && passed;
	};
	for (unsigned frame = 0; frame < 20 && !System::IsSetToQuit(); ++frame) { draw(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
	std::string summary, details;
	const bool summaryRead = menu->AutomationLabelText("LabelLastMatchSummary", summary);
	const bool opened = click("ButtonLastMatchDetails");
	draw();
	const bool detailsRead = menu->AutomationLabelText("LabelLastMatchDetails", details);
	const auto path = std::filesystem::path(CrossEnvironment("CC_TEST_CROSS_RECORDS")).parent_path() /
	    ("match-" + std::to_string(s_netMatchServiceE2ERematches) + "-lobby.png");
	const bool saved = g_FrameMan.SaveBitmapToPNG(g_FrameMan.GetBackBuffer32(), path.string().c_str()) == 0;
	const auto summaryRecord = g_NetMatchService.GetLastMatchSummary();
	const bool equal = summaryRecord && summary == summaryRecord->LineText() && details == summaryRecord->DetailsText();
	g_MetricsCollector.WriteObservation({{"type", "lobby_surface"}, {"summary", summary}, {"details", details},
	    {"screen", menu->AutomationActiveScreenName()}, {"subscreen", menu->AutomationMultiplayerSubScreen()},
	    {"summary_matches", equal}, {"details_opened", opened}, {"screenshot", path.string()}, {"saved", saved}});
	click("ButtonLastMatchClose");
	g_MenuMan.SetIsInMenuScreen(false);
	if (!summaryRead || !detailsRead || !opened || !equal || !saved) { *error = "the drawn lobby summary/details did not match the retained result"; return false; }
	return true;
}

// The e2e round boundary: the finished round's count and result.
static void NoteNetMatchE2ERoundOver(const std::string& result) {
	++s_netMatchServiceE2ERematches;
	if (s_crossRematches) {
		g_MetricsCollector.WriteObservation({{"type", "match_boundary"}, {"result", result},
		    {"final_tick", static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount())}, {"rematch", s_netMatchServiceE2ERematches}, {"budget_tick", s_crossBudget}});
	}
	std::ostringstream line;
	line << "[net-match-service-e2e] rematch: match " << s_netMatchServiceE2ERematches << " over (" << result << "), returning to lobby";
	System::PrintDiagnosticLine(line.str());
}

// Builds and starts the round the lobby launched; false with the e2e error set when any step fails.
static bool LaunchNetMatchE2ERound(const std::string& preset) {
	// A seat returning to a round the host already plays loads that round's image instead of building a fresh one.
	const bool fresh = g_NetMatchService.LaunchedFreshRound();
	std::string configureError;
	if (!(fresh ? ConfigureNetMatchServiceE2EActivity(preset, &configureError) : StageResyncedMatchActivity(&configureError))) {
		s_netMatchServiceE2EError = "rematch configure failed: " + configureError;
		s_netMatchServiceE2EExitCode = 1;
		System::SetQuit(true);
		return false;
	}
	// Restart NOW: the fresh coordinator expects frame 1, so no sim tick may run before
	// RestartActivity resets the sim count (the poll above also left real-time debt in
	// the sim accumulator, which ResetTime clears).
	g_TimerMan.PauseSim(true);
	if (!g_ActivityMan.RestartActivity()) {
		s_netMatchServiceE2EError = "rematch activity restart failed";
		s_netMatchServiceE2EExitCode = 1;
		System::SetQuit(true);
		return false;
	}
	if (!fresh) {
		System::PrintDiagnosticLine("[net-match] held client: replaying the private committed tail");
		g_NetMatchService.NoteResyncRelaunched();
		const uint64_t lockstepResume = ScenarioRunner::HasLockstepCoordinator() ? ScenarioRunner::GetLockstepResumeFrame() : 0;
		s_netMatchE2ETicks.OnResyncRelaunch(lockstepResume > 0 ? lockstepResume : static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) + 1);
		s_netMatchE2EEditorTicks = 0;
		return true;
	}
	{
		std::ostringstream line;
		line << "[net-match-service-e2e] rematch: round " << (s_netMatchServiceE2ERematches + 1) << " launching";
		System::PrintDiagnosticLine(line.str());
	}
	// Re-anchor tick accounting; round 2 counts fresh from the zeroed sim count.
	s_netMatchE2ETicks.OnNewMatch();
	s_netMatchE2EOwedSampleFrame = 0;
	return true;
}

// The e2e rematch ride-through: the finished round's result is logged, the live session reconvenes in the lobby and the next
// round launches; false with the e2e error set when any step fails.
static bool RunNetMatchE2ERematch(const std::string& result, bool finished) {
	NoteNetMatchE2ERoundOver(result);
	if (!finished) {
		g_NetMatchService.FinishMatch(result);
		g_ActivityMan.EndActivity();
		g_ActivityMan.SetInActivity(false);
	}
	std::string rematchError;
	// The cross driver draws and checks the post-match lobby first; its offered lobby may already have returned the session.
	bool lobbyLost = false;
	if (!CrossDrawLobbySurface(&rematchError) ||
	    ((!s_crossRematches || g_NetMatchService.GetState() == NetMatchServiceState::Completed) && !g_NetMatchService.ReturnToLobby(&rematchError))) {
		// A seat whose link the host closed or lost keeps its place: it returns through its ticket below.
		lobbyLost = g_NetMatchService.RematchReturnOwed();
		if (!lobbyLost) {
			s_netMatchServiceE2EError = "rematch return-to-lobby failed: " + rematchError;
			s_netMatchServiceE2EExitCode = 1;
			System::SetQuit(true);
			return false;
		}
	}
	std::string rematchPreset;
	bool rematchReady = false;
	uint64_t crossReadyRevision = UINT64_MAX;
	if (!lobbyLost) {
		g_NetMatchService.SetReady();
		// The peer that hosts the match now asks for the next one: after a migration that is the successor.
		if (g_NetMatchService.IsHost() || s_netDedicated) {
			if (!CrossHostOptions(s_netMatchServiceE2ERematches, &rematchError)) {
				s_netMatchServiceE2EError = "rematch host options: " + rematchError;
				s_netMatchServiceE2EExitCode = 1;
				System::SetQuit(true);
				return false;
			}
			g_NetMatchService.RequestStart();
		}
		// The service's own deadlines end a lobby that never starts; no wall clock here does.
		for (;;) {
			CrossReadyForCurrentConfig(crossReadyRevision);
			if (g_NetMatchService.ConsumeReadyToLaunch(rematchPreset)) {
				rematchReady = true;
				break;
			}
			if (g_NetMatchService.GetState() == NetMatchServiceState::Failed) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	if (!rematchReady && g_NetMatchService.RematchReturnOwed()) {
		// The link to the host closed in the lobby: the host plays the round with this seat held, and the seat returns to it.
		System::PrintDiagnosticLine("[net-match] rematch lobby link lost: rejoining the host");
		std::string rejoinError;
		if (g_NetMatchService.BeginHeldRejoin(&rejoinError)) {
			for (;;) {
				CrossReadyForCurrentConfig(crossReadyRevision);
				if (g_NetMatchService.ConsumeReadyToLaunch(rematchPreset)) {
					rematchReady = true;
					break;
				}
				if (g_NetMatchService.PumpHeldRejoin(&rejoinError)) {
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					continue;
				}
				// A failed attempt is asked again after the roster's backoff, until its bound or the host's final word.
				if (g_NetMatchService.GetState() == NetMatchServiceState::Failed && !g_NetMatchService.BeginHeldRejoinOnNextHost(&rejoinError)) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		} else {
			System::PrintDiagnosticLine("[net-match] rematch return could not start: " + rejoinError);
		}
	}
	if (!rematchReady) {
		s_netMatchServiceE2EError = "rematch launch failed: " + g_NetMatchService.GetErrorText();
		s_netMatchServiceE2EExitCode = 1;
		System::SetQuit(true);
		return false;
	}
	return LaunchNetMatchE2ERound(rematchPreset);
}

/// Whether a controller stop carries a Complete stop, as the finish path passes it or as the poll and the controller path prefix it ("tick N lockstep stopped: Complete:...").
static bool IsCompleteControllerStop(const std::string& error) {
	if (error.starts_with("Complete:")) {
		return true;
	}
	if (!error.starts_with("tick ")) {
		return false;
	}
	constexpr std::string_view c_StopPrefix = " lockstep stopped: Complete:";
	size_t digits = 5;
	while (digits < error.size() && error[digits] >= '0' && error[digits] <= '9') {
		++digits;
	}
	return digits > 5 && error.compare(digits, c_StopPrefix.size(), c_StopPrefix) == 0;
}

static void HandleControllerReplayFailure(bool& returnToMenuAfterNetworkEnd) {
	const std::string error = ScenarioRunner::GetControllerReplayError();
	if (error.find("Desync") != std::string::npos) {
		if (TerrainDumpArmed()) {
			DumpTerrainNow("desync");
			DumpSimStateNow("desync");
		}
		const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
		SceneMan::FlushTerrainEvents(base + ".desync.terrainevents.txt");
	}
	if (ScenarioRunner::IsActive()) {
		{
			std::ostringstream line;
			line << "[scenario] controller replay failed: " << error;
			System::PrintDiagnosticErrorLine(line.str());
		}
		System::SetQuit(true);
	} else if (ScenarioRunner::IsLockstepReplayPlayback()) {
		// Playback ends when the recording's marker does; every other stop is a distinct, named failure.
		s_netReplayTicks = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
		using Outcome = ScenarioRunner::LockstepReplayOutcome;
		Outcome outcome = ScenarioRunner::GetLockstepReplayOutcome();
		if (outcome == Outcome::Playing || outcome == Outcome::None) {
			outcome = Outcome::SimFailure;
			ScenarioRunner::SetLockstepReplayOutcome(outcome);
		}
		if (outcome != Outcome::Completed) {
			{
				std::ostringstream line;
				line << "[net-replay] playback stopped: " << ScenarioRunner::ReplayOutcomeName(outcome) << ": " << error;
				System::PrintDiagnosticErrorLine(line.str());
			}
			s_netReplayExitCode = outcome == Outcome::Truncated ? 2 : (outcome == Outcome::Corrupt ? 3 : 4);
		}
		g_ActivityMan.EndActivity();
		ScenarioRunner::ClearControllerReplayError();
		if (s_netReplayFromMenu) {
			{
				std::ostringstream line;
				line << "[net-replay] playback " << (outcome == Outcome::Completed ? "finished" : "FAILED")
				     << ", ticks=" << s_netReplayTicks << " outcome=" << ScenarioRunner::ReplayOutcomeName(outcome)
				     << " frames=" << ScenarioRunner::GetLockstepReplayFramesConsumed()
				     << " end_marker=" << (ScenarioRunner::LockstepReplaySawEndMarker() ? 1 : 0);
				System::PrintDiagnosticLine(line.str());
			}
			s_netReplayReturnStatus = outcome == Outcome::Completed ? "Playback finished: " + std::to_string(ScenarioRunner::GetLockstepReplayFramesConsumed()) + " ticks"
			                                                      : "Playback failed: " + error;
			CloseNetReplayPlayback();
			g_ActivityMan.SetInActivity(false);
			s_netReplayReturnPending = true;
			returnToMenuAfterNetworkEnd = true;
		} else {
			System::SetQuit(true);
		}
	} else {
		const uint64_t e2eTickBudget = s_netLockstepTicks > 0 ? s_netLockstepTicks : 600;
		const uint64_t e2eTickCap = s_netMatchE2ETicks.matchFirstFrame != UINT64_MAX
		    ? e2eTickBudget + s_netMatchE2ETicks.matchFirstFrame - 1 : e2eTickBudget;
		const uint64_t matchTick = ParseLockstepStopTick(error, static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
		const bool e2ePeerStoppedAfterCap = s_netMatchServiceE2E &&
			NetMatchE2ERoundReachedPlannedEnd(error, s_netMatchE2ETicks.Total(), matchTick, e2eTickCap);
		// A member hears its host's End Match as the host's own Complete stop; the local stop of a failed handover is not one.
		const bool e2eHostEndedRound = s_netMatchServiceE2E && IsCompleteControllerStop(error) && error.find("e2e complete") == std::string::npos &&
			error.find("match over") == std::string::npos && error.find("host handover ended") == std::string::npos &&
			!g_NetMatchService.IsHost() && g_NetMatchService.GetState() == NetMatchServiceState::Running;
		// The host's own End Match played its round to the agreed end frame: that clean stop is the end it asked for.
		const bool hostEndedAtAgreedFrame = IsCompleteControllerStop(error) && g_NetMatchService.IsHost() && g_NetMatchService.EndsAtAgreedFrame();
		// A held seat's rejoin is the product's own recovery too: the observed trace follows it instead of stopping.
		const bool observeTraceRecovery = !s_netMatchServiceE2E && s_recordTickHashes &&
		    (error.find("ResyncRequested") != std::string::npos || (error.find("PeerHeld:") != std::string::npos && g_SettingsMan.GetNetworkAutoReconnect()));
		if (!s_netMatchServiceE2E && s_recordTickHashes && g_NetMatchService.WasEverStarted() && !observeTraceRecovery) {
			const uint64_t cap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 600;
			if (!s_menuTraceCoverage.ReachedCap(g_MetricsCollector.GetTickHashCount(), cap) || error.find("Complete:") == std::string::npos) {
				s_menuMpTraceError = error;
				{
					std::ostringstream line;
					line << "[menu-mp] trace stopped: " << error;
					System::PrintDiagnosticErrorLine(line.str());
				}
			}
			g_ActivityMan.EndActivity();
			ScenarioRunner::ClearControllerReplayError();
			System::SetQuit(true);
		} else if (s_netMatchServiceE2E && e2ePeerStoppedAfterCap) {
			g_NetMatchService.Complete("e2e complete");
			// A probe still reading the pause menu the round ended under keeps it for a bounded window; the
			// game loop ends the activity as soon as the probe is done (ENGINE 200).
			if (NetModerationGUIProbe::Running()) {
				s_netMatchE2ECompletedMs = SteadyMilliseconds();
			} else {
				g_ActivityMan.EndActivity();
				ScenarioRunner::ClearControllerReplayError();
				System::SetQuit(true);
			}
		} else if (error.find("MatchOver:") != std::string::npos) {
			// The round this seat was rejoining is finished: it completes on what it holds. Without this the
			// rejoin's own link failure reads as a broken match instead of a played one.
			uint64_t goodbyeFinal = 0;
			(void)g_NetMatchService.HostGoodbyeSeen(goodbyeFinal);
			s_netMatchCompletedByHostGoodbye = true;
			s_netMatchGoodbyeFinalFrame = goodbyeFinal;
			// A seat the host sent its end record reads the round's result from it and stays for the rematch.
			uint64_t endRecord = 0;
			const bool endedByRecord = g_NetMatchService.TakeRoundEndRecord(endRecord);
			// The round ends before another tick would write what the hold abandoned.
			RetractAbandonedTickHashes();
			{
				std::ostringstream line;
				line << (endedByRecord ? "[net-match] completed_by_end_record=1 held_from=" : "[net-match] completed_by_host_goodbye=1 held_from=") << s_netMatchHeldFromTick
				     << " final=" << s_netMatchGoodbyeFinalFrame;
				System::PrintDiagnosticLine(line.str());
			}
			const std::string result = endedByRecord ? NetMatchService::RoundEndResultText(RoundEndedWinnerTeam(endRecord), g_NetMatchService.GetLocalTeam())
			                                         : NetMatchEndReason(g_ActivityMan.GetActivity());
			g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
			g_NetMatchService.FinishMatch(result);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			if (endedByRecord && s_netMatchServiceE2E && E2ERematchesLeft()) {
				(void)RunNetMatchE2ERematch(result, true);
			} else if (s_netMatchServiceE2E) {
				System::SetQuit(true);
			} else {
				returnToMenuAfterNetworkEnd = true;
			}
		} else if (const size_t refused = error.find("WorldJoinRefused:"); refused != std::string::npos) {
			const std::string reason = error.substr(refused + std::string("WorldJoinRefused:").size());
			g_ConsoleMan.PrintString("NETWORK: " + reason);
			g_NetMatchService.ReportRuntimeError(reason);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			if (s_netMatchServiceE2E) {
				s_netMatchServiceE2EError = reason;
				s_netMatchServiceE2EExitCode = 1;
				System::SetQuit(true);
			} else returnToMenuAfterNetworkEnd = true;
		} else if (error.find("PeerLeft:") != std::string::npos && g_NetMatchService.GetState() == NetMatchServiceState::Running) {
			// The last peer announced its leave, so the match is over rather than broken: it ends the
			// way a finished one does, which keeps the seats and the admission counters in the report.
			const std::string result = NetMatchEndReason(g_ActivityMan.GetActivity());
			g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
			g_NetMatchService.FinishMatch(result);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			if (s_netMatchServiceE2E) {
				System::SetQuit(true);
			} else {
				returnToMenuAfterNetworkEnd = true;
			}
		} else if (!s_netMatchServiceE2E && error.find("Complete:") != std::string::npos && g_NetMatchService.GetState() == NetMatchServiceState::Running) {
			// The peer's clean stop ends this match before the local activity catches up.
			const std::string result = NetMatchEndReason(g_ActivityMan.GetActivity());
			g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
			g_NetMatchService.FinishMatch(result);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			returnToMenuAfterNetworkEnd = true;
		} else if (error.find("Complete:") != std::string::npos &&
		           error.find("e2e complete") == std::string::npos &&
		           (g_NetMatchService.GetState() == NetMatchServiceState::Completed ||
		            error.find("match over") != std::string::npos || e2eHostEndedRound || hostEndedAtAgreedFrame)) {
			if (e2eHostEndedRound) {
				// The round ends on the host's word, as the product's clean stop does.
				const std::string result = NetMatchEndReason(g_ActivityMan.GetActivity());
				System::PrintDiagnosticLine("[net-match] completed_by_host_end=1 result=" + result);
				g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
				g_NetMatchService.FinishMatch(result);
			} else if (g_NetMatchService.GetState() == NetMatchServiceState::Running) {
				g_NetMatchService.FinishMatch(BuildNetMatchResultText());
			}
			g_ActivityMan.EndActivity();
			ScenarioRunner::ClearControllerReplayError();
			if (s_netMatchServiceE2E) {
				System::SetQuit(true);
			} else {
				returnToMenuAfterNetworkEnd = true;
			}
		} else if ((error.find("PeerHeld:") != std::string::npos && g_SettingsMan.GetNetworkAutoReconnect()) ||
		          ((error.find("Desync") != std::string::npos || error.find("ResyncRequested") != std::string::npos) &&
		           g_NetMatchService.IsResyncOnDesyncEnabled() &&
		           (s_netMatchHeals.Allowed(g_TimerMan.GetSimTimeTicks(), g_TimerMan.GetTicksPerSecond()) || g_NetMatchService.IsHostMigrationRepairPending()) &&
		           g_NetMatchService.GetState() == NetMatchServiceState::Running)) {
			const bool heldRejoin = error.find("PeerHeld:") != std::string::npos;
			System::PrintDiagnosticLine("[net-match] recovery requested tick=" + std::to_string(matchTick) +
			    " catch_up=" + std::to_string(ScenarioRunner::WorldCatchUpActive()) + " reason=" + error);
			static unsigned int traceRecoveryCount = 0;
			const std::string traceGapKey = observeTraceRecovery ? "menu_trace_gap_" + std::to_string(++traceRecoveryCount) : std::string();
			if (observeTraceRecovery) {
				g_MetricsCollector.RecordString(traceGapKey, nlohmann::json{{"stopped_at", matchTick}, {"reason", error}}.dump());
				System::PrintDiagnosticLine("[menu-mp] trace recovery gap frame=" + std::to_string(matchTick) + " reason=" + error);
				FrameRecorder::Instance().RecordEvent("trace recovery gap frame=" + std::to_string(matchTick));
				// Save the observed prefix before recovery can leave the gameplay loop.
				const std::string& tracePath = ScenarioRunner::GetArgs().outPath;
				if (!tracePath.empty() && !g_MetricsCollector.WriteReport(tracePath)) {
					System::PrintDiagnosticErrorLine("[menu-mp] could not retain trace before recovery: " + tracePath);
				}
			}
			if (heldRejoin) {
				s_netMatchHeldFromTick = matchTick;
				if (ScenarioRunner::IsLockstepHoldNoticeVisible()) g_ConsoleMan.PrintString("NETWORK: Held - AI in control - rejoining");
				ScenarioRunner::PushNetUiToast("seat_held", "Held - AI in control - rejoining");
			} else {
				s_netMatchHeals.Note(g_TimerMan.GetSimTimeTicks(), g_TimerMan.GetTicksPerSecond());
				g_ConsoleMan.PrintString("NETWORK: Resyncing from the host (" + std::to_string(s_netMatchHeals.Total()) + "): " + error);
				System::PrintDiagnosticLine("[net-match] resync: reloading from the host snapshot");
				const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
				const auto seatName = [&snapshot](uint8_t peer) {
					if (const auto view = g_NetMatchService.GetSeatView(peer); view && !view->name.empty()) return view->name;
					const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(), [peer](const NetLobbyMember& row) { return row.peerId == peer; });
					return member != snapshot.members.end() ? member->displayName : std::string();
				};
				ScenarioRunner::PushNetUiToast("resync_start", NetRepairStartLine(error, seatName(snapshot.localPeerId), seatName(snapshot.hostPeerId), snapshot.isHost));
			}
			ScenarioRunner::ClearControllerReplayError();
			std::string resyncError;
			bool resyncOk = false;
			uint64_t endRecord = 0;
			bool endedByRecord = false;
			bool leftTheWait = false;
			// Without a majority the seat waits for its host and the screen says why.
			const size_t heldAt = error.find("PeerHeld:");
			const std::string stopLine = heldAt == std::string::npos ? std::string() : error.substr(heldAt + 9);
			const std::string unreachableAtStop = stopLine.rfind("The host is unreachable", 0) == 0 ? stopLine : std::string();
			// The wait's screen goes up before the host's snapshot save holds this thread, so the stopped match says why at once.
			const bool leaveAtOnce = UpdateResyncUI(0, heldRejoin, unreachableAtStop);
			if (leaveAtOnce) {
				leftTheWait = true;
				resyncOk = false;
			} else if (heldRejoin) {
				resyncOk = g_NetMatchService.BeginHeldRejoin(&resyncError);
			} else resyncOk = g_NetMatchService.ResyncMatch(&resyncError);
			std::string launchPreset;
			for (bool attempt = resyncOk; attempt;) {
				attempt = false;
				const auto resyncWaitStart = std::chrono::steady_clock::now();
				while (!g_NetMatchService.ConsumeReadyToLaunch(launchPreset)) {
					// The round ended while this seat was on its way back: the host's end record is the result, and the session stays.
					if (g_NetMatchService.TakeRoundEndRecord(endRecord)) {
						endedByRecord = true;
						resyncOk = false;
						break;
					}
					const std::string unreachable = heldRejoin ? g_NetMatchService.GetHostUnreachableLine() : std::string();
					if (UpdateResyncUI(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - resyncWaitStart).count()), heldRejoin,
					                   unreachable.empty() ? unreachableAtStop : unreachable)) {
						leftTheWait = true;
						resyncOk = false;
						break;
					}
					if (System::IsSetToQuit()) {
						resyncError = "quit requested during resync";
						resyncOk = false;
						break;
					}
					// A held rejoin's next attempt is armed in the service and begins there once its backoff is over.
					if (heldRejoin && g_NetMatchService.PumpHeldRejoin(&resyncError)) {
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
						continue;
					}
					if (g_NetMatchService.GetState() == NetMatchServiceState::Failed) {
						resyncError = g_NetMatchService.GetErrorText();
						resyncOk = false;
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				// A held seat whose host is gone rejoins the peer that hosts the match now, through its private rejoin.
				if (!resyncOk && !endedByRecord && !leftTheWait && heldRejoin && !System::IsSetToQuit()) {
					std::string nextError;
					if (g_NetMatchService.BeginHeldRejoinOnNextHost(&nextError)) {
						resyncOk = true;
						attempt = true;
					}
				}
			}
			// The held round ended while this seat was on its way back and the host's next round took it in: it plays that round.
			const bool landedInNextRound = resyncOk && heldRejoin && g_NetMatchService.LaunchedFreshRound();
			if (landedInNextRound) resyncOk = false;
			if (resyncOk) {
				resyncOk = StageResyncedMatchActivity(&resyncError);
			}
			if (resyncOk) {
				g_TimerMan.PauseSim(true);
				if (!g_ActivityMan.RestartActivity()) {
					resyncError = "resync activity restart failed";
					resyncOk = false;
				}
			}
			if (resyncOk) {
				{
					std::ostringstream line;
					line << (heldRejoin ? "[net-match] held client: replaying the private committed tail" : "[net-match] resync: match relaunched from the snapshot");
					System::PrintDiagnosticLine(line.str());
				}
				g_NetMatchService.NoteResyncRelaunched();
				if (observeTraceRecovery) {
					const uint64_t resumedAt = ScenarioRunner::GetLockstepResumeFrame();
					g_MetricsCollector.RecordString(traceGapKey, nlohmann::json{{"stopped_at", matchTick}, {"reason", error}, {"resumed_at", resumedAt}}.dump());
					System::PrintDiagnosticLine("[menu-mp] trace observation resumed frame=" + std::to_string(resumedAt));
					if (heldRejoin) {
						s_menuTraceCoverage.heldResume = resumedAt;
						s_menuTraceCoverage.heldReplay = true;
					}
				}
				// The relaunch drops the queue; the healed round has not applied a frame yet, so the
				// toast names the frame it resumes on.
				ScenarioRunner::ClearNetUiToasts();
				ScenarioRunner::PushNetUiToast(heldRejoin ? "seat_held" : "resync_finish", heldRejoin ? "Held - AI in control - rejoining" : "Match resynced (healed at frame " + std::to_string(ScenarioRunner::GetLockstepResumeFrame()) + ")");
				if (s_netMatchServiceE2E) {
					// A world rejoin names no lockstep resume frame: its budget runs from the image it loaded, as a private return's does.
					const uint64_t lockstepResume = ScenarioRunner::HasLockstepCoordinator() ? ScenarioRunner::GetLockstepResumeFrame() : 0;
					const uint64_t resumeFrame = lockstepResume > 0 ? lockstepResume : static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) + 1;
					s_netMatchE2ETicks.OnResyncRelaunch(resumeFrame);
					// The relaunch restarts the editor phase, so its budget restarts.
					s_netMatchE2EEditorTicks = 0;
				}
			} else if (landedInNextRound) {
				RetractAbandonedTickHashes();
				System::PrintDiagnosticLine("[net-match] completed_by_next_round=1 held_from=" + std::to_string(s_netMatchHeldFromTick));
				g_ConsoleMan.PrintString("NETWORK: The round ended while you were rejoining - the next round starts");
				g_NetMatchService.EndHeldRejoinInNextRound();
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				ScenarioRunner::ClearControllerReplayError();
				ScenarioRunner::ClearNetUiToasts();
				if (s_netMatchServiceE2E) {
					NoteNetMatchE2ERoundOver("Match over");
					(void)LaunchNetMatchE2ERound(launchPreset);
				} else {
					std::string launchError;
					g_TimerMan.PauseSim(true);
					if (!ConfigureNetMatchServiceE2EActivity(launchPreset, &launchError) || !g_ActivityMan.RestartActivity()) {
						g_ConsoleMan.PrintString("NETWORK: The next round could not start: " + launchError);
						g_NetMatchService.ReportRuntimeError("next round launch failed: " + launchError);
						returnToMenuAfterNetworkEnd = true;
					}
				}
			} else if (endedByRecord) {
				// The round ended while this seat was held or rejoining: it shows the host's result and stays for the rematch.
				const std::string result = NetMatchService::RoundEndResultText(RoundEndedWinnerTeam(endRecord), g_NetMatchService.GetLocalTeam());
				s_netMatchCompletedByHostGoodbye = heldRejoin;
				s_netMatchGoodbyeFinalFrame = RoundEndedFinalFrame(endRecord);
				RetractAbandonedTickHashes();
				{
					std::ostringstream line;
					line << "[net-match] completed_by_end_record=1 held_from=" << s_netMatchHeldFromTick << " final=" << s_netMatchGoodbyeFinalFrame << " result=" << result;
					System::PrintDiagnosticLine(line.str());
				}
				g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
				g_NetMatchService.FinishMatch(result);
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				ScenarioRunner::ClearControllerReplayError();
				if (s_netMatchServiceE2E && E2ERematchesLeft()) {
					(void)RunNetMatchE2ERematch(result, true);
				} else if (s_netMatchServiceE2E) {
					System::SetQuit(true);
				} else {
					returnToMenuAfterNetworkEnd = true;
				}
			} else if (leftTheWait) {
				System::PrintDiagnosticLine("[net-match] held client: left the wait for its host; the seat and its ticket are kept");
				g_ConsoleMan.PrintString("NETWORK: Left the match - seat held until the host reassigns it; use Rejoin Match to try to return");
				g_NetMatchService.LeaveHeldWait();
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				ScenarioRunner::ClearControllerReplayError();
				if (s_netMatchServiceE2E) {
					System::SetQuit(true);
				} else {
					returnToMenuAfterNetworkEnd = true;
				}
			} else if (NetMatchHostGoodbyeEndedTheRejoin(resyncError)) {
				// The host's goodbye ends this seat's match at the frame the round ended on: the rejoin had
				// nothing left to return to, so the seat completes with what it holds instead of failing.
				s_netMatchCompletedByHostGoodbye = heldRejoin;
				// The round ends before another tick would write what the hold abandoned.
				RetractAbandonedTickHashes();
				if (heldRejoin) {
					std::ostringstream line;
					line << "[net-match] completed_by_host_goodbye=1 held_from=" << s_netMatchHeldFromTick
					     << " final=" << s_netMatchGoodbyeFinalFrame;
					System::PrintDiagnosticLine(line.str());
				}
				g_NetMatchService.FinishMatch("match over");
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				ScenarioRunner::ClearControllerReplayError();
				// An observed trace of a held seat ends with its match, as the trace cap ends one that played on.
				if (s_netMatchServiceE2E || (heldRejoin && observeTraceRecovery)) {
					System::SetQuit(true);
				} else {
					returnToMenuAfterNetworkEnd = true;
				}
			} else {
				{
					std::ostringstream line;
					line << "[net-match] resync failed: " << resyncError;
					System::PrintDiagnosticErrorLine(line.str());
				}
				g_ConsoleMan.PrintString("NETWORK: Resync failed: " + resyncError);
				g_NetMatchService.ReportRuntimeError("resync failed: " + resyncError);
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
				if (s_netMatchServiceE2E) {
					s_netMatchServiceE2EError = "resync failed: " + resyncError;
					s_netMatchServiceE2EExitCode = 1;
					System::SetQuit(true);
				} else {
					returnToMenuAfterNetworkEnd = true;
				}
			}
		} else {
			{
				std::ostringstream line;
				line << "[net-match] controller sync failed: " << error;
				System::PrintDiagnosticErrorLine(line.str());
			}
			CrossNoteOwnBan();
			g_ConsoleMan.PrintString("NETWORK: Match stopped: " + error);
			g_NetMatchService.ReportRuntimeError(error);
			g_ActivityMan.EndActivity();
			g_ActivityMan.SetInActivity(false);
			ScenarioRunner::ClearControllerReplayError();
			if (s_netMatchServiceE2E) {
				s_netMatchServiceE2EError = error;
				s_netMatchServiceE2EExitCode = 1;
				System::SetQuit(true);
			} else {
				returnToMenuAfterNetworkEnd = true;
			}
		}
	}
}

/// A launch that fails leaves no scene, so the loop below has nothing to run against - a resync whose
/// snapshot will not apply lands exactly here. The recovery text rides the service, so §11's state
/// machine says why the seat was lost.
/// @return Whether the game loop may still be entered.
static bool HandleFailedActivityLaunch() {
	const std::string reason = "could not launch the activity";
	const bool menuReplayFailed = s_netReplayFromMenu;
	{
		std::ostringstream line;
		line << (menuReplayFailed ? "[net-replay] " : "[net-match] ") << reason;
		System::PrintDiagnosticErrorLine(line.str());
	}
	g_ConsoleMan.PrintString("ERROR: " + reason);
	if (!menuReplayFailed && g_NetMatchService.GetState() != NetMatchServiceState::Idle) {
		g_NetMatchService.ReportRuntimeError(reason);
	}
	g_ActivityMan.EndActivity();
	g_ActivityMan.SetInActivity(false);
	if (menuReplayFailed) {
		ScenarioRunner::SetLockstepReplayOutcome(ScenarioRunner::LockstepReplayOutcome::SimFailure);
		CloseNetReplayPlayback();
		g_ActivityMan.ClearEndedReplayActivity();
		s_netReplayReturnStatus = "Playback failed: " + reason;
		s_netReplayReturnPending = true;
	}
	if (s_netMatchServiceE2E) {
		if (s_netMatchServiceE2EExitCode == 0) {
			s_netMatchServiceE2EError = reason;
			s_netMatchServiceE2EExitCode = 1;
		}
		System::SetQuit(true);
		return false;
	}
	g_TimerMan.PauseSim(true);
	g_MenuMan.HandleTransitionIntoMenuLoop();
	RunMenuLoop();
	return !System::IsSetToQuit();
}

static Actor* FindE2eSwitchControlTarget(Activity* activity, int player) {
	if (!activity) {
		return nullptr;
	}
	const int team = activity->GetTeamOfPlayer(player);
	Actor* brain = activity->GetPlayerBrain(player);
	if (!brain) {
		brain = g_MovableMan.GetFirstBrainActor(team);
	}
	Actor* best = nullptr;
	if (std::list<Actor*>* roster = g_MovableMan.GetTeamRoster(team)) {
		for (Actor* actor: *roster) {
			if (!actor || actor == brain || actor->IsInGroup("Brains") || actor->IsPlayerControlled()) {
				continue;
			}
			if (!best || actor->GetUniqueID() < best->GetUniqueID()) {
				best = actor;
			}
		}
	}
	return best;
}

static void NoteE2eSwitchOwnerLog(uint64_t tick) {
	if (!s_netMatchServiceE2E || !ScenarioRunner::IsLockstepControllerSyncActive()) {
		return;
	}
	if (s_netMatchE2eSwitchUid == 0) {
		s_netMatchE2eSwitchUid = ScenarioRunner::GetE2eOwnerTransferUid();
	}
	if (s_netMatchE2eSwitchUid == 0) {
		for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
			std::list<Actor*>* roster = g_MovableMan.GetTeamRoster(team);
			if (!roster) {
				continue;
			}
			for (Actor* actor: *roster) {
				if (!actor) {
					continue;
				}
				const int64_t uid = static_cast<int64_t>(actor->GetUniqueID());
				const uint8_t seeded = NetActorOwnership::GetSeededOwner(uid);
				if (seeded == 0) {
					continue;
				}
				const uint8_t owner = ScenarioRunner::GetLockstepActorOwner(uid, actor->GetTeam(), !actor->IsPlayerControlled());
				if (owner != seeded) {
					s_netMatchE2eSwitchUid = uid;
					break;
				}
			}
			if (s_netMatchE2eSwitchUid != 0) {
				break;
			}
		}
	}
	// A peer present from the start waits for the transfer; a late joiner is already past this tick.
	if (s_netMatchE2eSwitchUid == 0 && s_netMatchE2eSwitchControlTick > 0 && g_TimerMan.GetSimUpdateCount() >= s_netMatchE2eSwitchControlTick) {
		if (Activity* activity = g_ActivityMan.GetActivity()) {
			const int player = activity->PlayerOfScreen(0);
			if (Actor* target = FindE2eSwitchControlTarget(activity, player)) {
				s_netMatchE2eSwitchUid = static_cast<int64_t>(target->GetUniqueID());
			}
		}
	}
	if (s_netMatchE2eSwitchUid == 0) {
		return;
	}
	Actor* actor = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long int>(s_netMatchE2eSwitchUid)));
	if (!actor) {
		return;
	}
	const uint8_t owner = ScenarioRunner::GetLockstepActorOwner(s_netMatchE2eSwitchUid, actor->GetTeam(), !actor->IsPlayerControlled());
	s_netMatchE2eOwnerLog.push_back({tick, s_netMatchE2eSwitchUid, static_cast<int>(owner), static_cast<int>(actor->GetController()->GetInputMode())});
}

void RunGameLoop() {
	if (System::IsSetToQuit()) {
		return;
	}
	StallStackSampler::ArmForCurrentThread();
	g_TimerMan.PauseSim(false);

	if (g_ActivityMan.ActivitySetToRestart()) {
		g_LoadingScreen.DrawLoadingSplash();
		g_WindowMan.UploadFrame();
		if (!g_ActivityMan.RestartActivity() && !HandleFailedActivityLaunch()) {
			return;
		}
	}

	g_NetMatchService.PreparePrivateRejoinCheckpoint();

	long long updateStartTime = 0;
	long long updateTotalTime = 0;
	long long updateEndAndDrawStartTime = 0;
	long long drawStartTime = 0;
	long long drawTotalTime = 0;

	struct FrameStallSample { ~FrameStallSample() { StallStackSampler::TickEnd(); } };
	while (!System::IsSetToQuit()) {
		StallStackSampler::TickBegin(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
		const FrameStallSample frameStallSample;
		bool returnToMenuAfterNetworkEnd = false;
		// The completed round's held pause menu ends the moment its probe does, or when the window runs out.
		if (s_netMatchE2ECompletedMs && !ProbeHoldsE2eEnd(s_netMatchE2ECompletedMs)) {
			s_netMatchE2ECompletedMs = 0;
			NetModerationGUIProbe::WriteUnfinished();
			g_ActivityMan.EndActivity();
			ScenarioRunner::ClearControllerReplayError();
			System::SetQuit(true);
			break;
		}
		// A run that left its match ends as a leaver's run does, once its probe is done or the window runs out.
		if (s_netMatchE2ELeftMs && !ProbeHoldsE2eEnd(s_netMatchE2ELeftMs)) {
			NetModerationGUIProbe::WriteUnfinished();
			System::SetQuit(true);
			break;
		}
		static uint64_t lossArmRound = UINT64_MAX;
		if (ScenarioRunner::IsLockstepControllerSyncActive() && lossArmRound != ScenarioRunner::GetLockstepRoundId()) {
			lossArmRound = ScenarioRunner::GetLockstepRoundId();
			const char* lossText = std::getenv("CC_TEST_GNS_LOSS_PERCENT");
			const char* headless = std::getenv("CCCP_HEADLESS");
			if (lossText && headless && std::strcmp(headless, "1") == 0) {
				float percent = -1.0F;
				const char* lossEnd = lossText + std::strlen(lossText);
				const char* end = ParseNumberExact(lossText, lossEnd, percent).ptr;
				bool applied = false;
#ifdef CCCP_WITH_GNS
				if (end != lossText && *end == '\0' && percent >= 0 && percent <= 100) {
					applied = SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, percent) &&
					    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, percent);
				}
#endif
				std::cout << "[net-transport-loss] percent=" << percent << " send_recv_armed=" << applied << " round=" << lossArmRound << std::endl;
			}
		}
		FrameMan::FeelBeginIteration();
		ApplyCrossSchedule();
		updateStartTime = g_TimerMan.GetAbsoluteTime();

		// The host's session plane receives, relays and commits for every stretch of the frame the simulation spends away from its round.
		std::optional<NetLockstepPlane::Window> frameHeadWindow;
		frameHeadWindow.emplace("frame head");
		PollSDLEvents();
		g_WindowMan.Update();
		g_WindowMan.ClearBackbuffer();

		RTEError::DispatchPendingWorkerMessages();

		if (s_frameStallArmed && !s_frameStallFired && g_TimerMan.GetSimUpdateCount() >= s_frameStallTick) {
			s_frameStallFired = true;
			{
				std::ostringstream line;
				line << "[selftest] frame stall tick=" << g_TimerMan.GetSimUpdateCount() << " ms=" << s_frameStallMs << " requested_tick=" << s_frameStallTick;
				System::PrintDiagnosticLine(line.str());
			}
			// The session plane keeps the round's frames moving while this machine's simulation is away.
			NetLockstepPlane::Window planeWindow;
			const uint64_t planeTicksBefore = NetLockstepPlane::Ticks();
			std::this_thread::sleep_for(std::chrono::milliseconds(s_frameStallMs));
			System::PrintDiagnosticLine("[selftest] frame stall done plane_ticks=" + std::to_string(NetLockstepPlane::Ticks() - planeTicksBefore));
		}
		if (s_frameStallAgainMs > 0 && s_frameStallFired && !s_frameStallAgainFired && g_TimerMan.GetSimUpdateCount() >= s_frameStallAgainTick) {
			s_frameStallAgainFired = true;
			System::PrintDiagnosticLine("[selftest] frame stall again tick=" + std::to_string(g_TimerMan.GetSimUpdateCount()) + " ms=" + std::to_string(s_frameStallAgainMs));
			NetLockstepPlane::Window planeWindow;
			const uint64_t planeTicksBefore = NetLockstepPlane::Ticks();
			std::this_thread::sleep_for(std::chrono::milliseconds(s_frameStallAgainMs));
			System::PrintDiagnosticLine("[selftest] frame stall again done plane_ticks=" + std::to_string(NetLockstepPlane::Ticks() - planeTicksBefore));
		}
		if (ScenarioRunner::IsLockstepControllerSyncActive() && !ScenarioRunner::WorldCatchUpActive()) {
			for (auto& stall: s_netLiveStalls) if ((stall.eachRound ? stall.firedRound != s_netMatchServiceE2ERematches : !stall.fired) && static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) >= stall.tick) {
				if (stall.eachRound) {
					stall.firedRound = s_netMatchServiceE2ERematches;
				} else {
					const uint64_t activation = ScenarioRunner::WorldCatchUpActivationTick();
					if (ScenarioRunner::GetLockstepLocalPeerId() != ScenarioRunner::GetLockstepHostPeerId() &&
					    s_netLiveStallActivation && activation <= *s_netLiveStallActivation) break;
					stall.fired = true;
					s_netLiveStallActivation = activation;
				}
				System::PrintDiagnosticLine("[net-test] live stall frame=" + std::to_string(g_TimerMan.GetSimUpdateCount()) + " ms=" + std::to_string(stall.milliseconds) +
				                            (stall.eachRound ? " round_index=" + std::to_string(s_netMatchServiceE2ERematches) : std::string()));
				{
					NetLockstepPlane::Window planeWindow;
					std::this_thread::sleep_for(std::chrono::milliseconds(stall.milliseconds));
				}
				break;
			}
		}
		frameHeadWindow.reset();

		g_TimerMan.Update();
		MemoryCensusByUptime();

		if (!g_ActivityMan.ActivityRunning()) {
			LocalPrediction::Clear();
			PreviewEventLedger::Clear();
		}

		const bool paceActiveAtIterStart = ScenarioRunner::IsLockstepControllerSyncActive();
		static bool s_pacePrevActive = false;
		if (paceActiveAtIterStart && !s_pacePrevActive) {
			// A fresh pace window per round, so multi-round runs don't blend their numbers.
			s_paceIterations = 0;
			s_paceSimTicks = 0;
			s_paceSimUs = 0;
			s_paceExecutionAverageMs.store(0.0F, std::memory_order_relaxed);
			s_paceTickCostsMs.clear();
			s_paceUpdateUs = 0;
			s_paceDrawUs = 0;
			s_paceTrailingUs = 0;
			s_pacePreviewUs = 0;
			s_paceInterfaceUs = 0;
			s_paceFramesDrawn = 0;
			s_paceFramesShed = 0;
			s_paceFrameDrawUs = 0;
			ScenarioRunner::ResetLockstepWaitUs();
			g_TimerMan.ResetPaceCounters();
		}
		s_pacePrevActive = paceActiveAtIterStart;
		const uint64_t paceTicksAtIterStart = s_paceSimTicks;

		// A free-running lockstep match takes one tick per iteration, so the preview and the polls still run per tick.
		const bool freeRunLockstep = ScenarioRunner::GetArgs().freeRunSim && ScenarioRunner::IsLockstepControllerSyncActive();
		if (ScenarioRunner::GetArgs().freeRunSim) {
			g_TimerMan.SetFreeRunSim(freeRunLockstep);
		}

		// A world joiner applies the committed tail faster than real time. 16 is a ceiling, and a
		// tick is granted only when the tail still holds that next frame.
		ScenarioRunner::BeginWorldCatchUpFrame();
		CrossEndTargetObservation(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));

		// A lockstep peer with owed ticks presents at least once per tick length while it catches up, the round's first ticks
		// included (the round starts inside this frame's first poll); a world joiner keeps its own ceiling.
		// A paced round sheds frames, never ticks: it keeps up to a second of what it owes, runs it back to back and presents
		// at least 15 times a second. One whose frames fit spreads a catch-up burst over frames a tick apart.
		const bool pacedRound = ScenarioRunner::HasLockstepCoordinator() && !freeRunLockstep && !ScenarioRunner::WorldCatchUpActive();
		const bool shedsFrames = pacedRound && g_PerformanceMan.GetMSPFAverage() > g_TimerMan.GetDeltaTimeMS();
		constexpr double c_FloorFrameMs = 1000.0 / 15.0;
		constexpr double c_OwedKeptMs = 1000.0;
		g_TimerMan.SetOwedTicksKept(pacedRound ? static_cast<int>(c_OwedKeptMs / g_TimerMan.GetDeltaTimeMS()) : 0);
		// A paced round caps what it owes by the median of its last 15 ticks, so a capture or a first-tick load cannot drop owed time.
		float owedCapTickCostMs = 0;
		if (pacedRound && !s_paceTickCostsMs.empty()) {
			std::vector<float> costs(s_paceTickCostsMs.begin(), s_paceTickCostsMs.end());
			std::nth_element(costs.begin(), costs.begin() + costs.size() / 2, costs.end());
			owedCapTickCostMs = costs[costs.size() / 2];
		}
		g_TimerMan.SetOwedCapTickCostMS(owedCapTickCostMs);
		g_TimerMan.BeginSimFrame(!pacedRound ? 0 : !shedsFrames ? g_TimerMan.GetDeltaTimeTicks() :
		                         std::max(g_TimerMan.GetDeltaTimeTicks(), static_cast<long long>((c_FloorFrameMs - g_PerformanceMan.GetMSPDAverage()) * 1000.0)));
		// Simulation update, as many times as the fixed update step allows in the span since last frame draw.
		while (true) {
			if (g_TimerMan.SimFrameBudgetSpent()) {
				break;
			}
			// A joiner launched from the menu restores its base before asking for the tick after it, as a launch at the loop's start does:
			// the counter still reads the round this process left, whose next tick the catch-up never grants.
			if (ScenarioRunner::WorldCatchUpActive() && !g_ActivityMan.IsInActivity() && g_ActivityMan.ActivitySetToRestart()) {
				g_TimerMan.PauseSim(false);
				g_LoadingScreen.DrawLoadingSplash();
				g_WindowMan.UploadFrame();
				if (!g_ActivityMan.RestartActivity() && !HandleFailedActivityLaunch()) return;
				g_NetMatchService.PreparePrivateRejoinCheckpoint();
				break;
			}
			const uint64_t nextSimTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) + 1;
			if (ScenarioRunner::WorldCatchUpActive()) {
				if (!ScenarioRunner::TakeWorldCatchUpGrant(nextSimTick)) {
					// A joiner that cannot advance still has to receive: the tail's next frame and the
					// lockstep start both arrive on this pump, which otherwise runs per sim tick only.
					g_NetMatchService.PumpSessionEvents();
					if (!ScenarioRunner::GetControllerReplayError().empty()) HandleControllerReplayFailure(returnToMenuAfterNetworkEnd);
					break;
				}
				if (!g_TimerMan.TimeForSimUpdate()) {
					g_TimerMan.GrantSimUpdates(1);
				}
			} else if (!g_TimerMan.TimeForSimUpdate()) {
				if (!ScenarioRunner::TakeOwnSeatCatchUpGrant(nextSimTick)) break;
				g_TimerMan.GrantSimUpdates(1);
			}
			if (!ScenarioRunner::WorldCatchUpActive() && !ScenarioRunner::PollLockstepSimulationTick(nextSimTick)) {
				if (ScenarioRunner::HasControllerReplayError()) HandleControllerReplayFailure(returnToMenuAfterNetworkEnd);
				break;
			}
			ZoneScopedN("Simulation Update");
			const FloatingPointEnvironment::Scope floatingPointScope("simulation tick");
			HarnessCost::BeginFrame();

			// The probe's sim-rate keys land before the update that reads them; SDL events only arrive per frame.
			NetModerationGUIProbe::OnSimTick(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));

			const long long paceTickStartUs = g_TimerMan.GetAbsoluteTime();
			const long long paceWaitStartUs = ScenarioRunner::GetLockstepWaitUs();
			const bool measureLockstepCost = ScenarioRunner::IsLockstepControllerSyncActive();
			g_PerformanceMan.NewPerformanceSample();
			if (!measureLockstepCost) g_PerformanceMan.UpdateMSPSU();
			g_TimerMan.UpdateSim();
			// Test lever: a slower machine's sim cost, spent inside the tick on this one - in the simulation's update, where a slower
			// machine spends it; CCCP_TEST_SIM_COST_OUTSIDE_WINDOW spends it before the update instead, where the plane cannot tick.
			static const long long s_testSimCostUs = [] { const char* text = std::getenv("CCCP_TEST_SIM_COST_US"); return text ? std::atoll(text) : 0LL; }();
			static const bool s_testSimCostOutside = std::getenv("CCCP_TEST_SIM_COST_OUTSIDE_WINDOW") != nullptr;
			const auto spendTestSimCost = [] {
				static const long long s_fromTick = [] { const char* text = std::getenv("CCCP_TEST_SIM_COST_FROM_TICK"); return text ? std::atoll(text) : 0LL; }();
				static const long long s_untilTick = [] { const char* text = std::getenv("CCCP_TEST_SIM_COST_UNTIL_TICK"); return text ? std::atoll(text) : 0LL; }();
				const long long tick = g_TimerMan.GetSimUpdateCount();
				// The window's receipt: which process crawled, over which ticks, from when to when on the steady clock the cross records read.
				struct Window {
					bool open = false, closed = false;
					long long firstTick = 0, lastTick = 0;
					double startMs = 0, endMs = 0;
				};
				static Window s_window;
				const auto steadyMs = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
				const auto receipt = [](const char* stage) {
					return std::string("[sim-cost] ") + stage + " " + nlohmann::json{{"process", System::GetProcessID()}, {"instance", CrossEnvironment("CC_TEST_CROSS_INSTANCE")},
					    {"incarnation", std::stoul(CrossEnvironment("CC_TEST_CROSS_INCARNATION", "0"))}, {"cost_us", s_testSimCostUs}, {"first_tick", s_window.firstTick},
					    {"last_tick", s_window.lastTick}, {"start_ms", s_window.startMs}, {"end_ms", s_window.endMs}}.dump();
				};
				// The same window for a probe's dump: the world_spectator_cost_window a scripted watcher's crawl is proven by.
				const auto keep = [] {
					const std::string instance = CrossEnvironment("CC_TEST_CROSS_INSTANCE");
					ScenarioRunner::NoteHarnessReceipt("world_spectator_cost_window", nlohmann::json{{"receipt", "world_spectator_cost_window"},
					    {"process", instance.empty() ? nlohmann::json(System::GetProcessID()) : nlohmann::json(instance)}, {"pid", System::GetProcessID()},
					    {"sim_cost_us", s_testSimCostUs}, {"first_tick", s_window.firstTick}, {"last_tick", s_window.lastTick}, {"start_ms", s_window.startMs},
					    {"end_ms", s_window.endMs}, {"closed", s_window.closed}}.dump());
				};
				if (tick >= s_fromTick && (s_untilTick == 0 || tick < s_untilTick)) {
					if (!s_window.open) {
						s_window.open = true;
						s_window.firstTick = tick;
						s_window.startMs = steadyMs();
						System::PrintDiagnosticLine(receipt("window_open"));
						std::atexit([] {
							if (s_window.open && !s_window.closed) {
								s_window.closed = true;
								System::PrintDiagnosticLine(std::string("[sim-cost] window_closed_at_exit ") + nlohmann::json{{"process", System::GetProcessID()},
								    {"first_tick", s_window.firstTick}, {"last_tick", s_window.lastTick}, {"start_ms", s_window.startMs}, {"end_ms", s_window.endMs}}.dump());
							}
						});
					}
					for (const long long until = g_TimerMan.GetAbsoluteTime() + s_testSimCostUs; g_TimerMan.GetAbsoluteTime() < until;) {}
					s_window.lastTick = tick;
					s_window.endMs = steadyMs();
					keep();
				} else if (s_window.open && !s_window.closed) {
					s_window.closed = true;
					System::PrintDiagnosticLine(receipt("window_closed"));
					keep();
				}
			};
			if (s_testSimCostUs > 0 && s_testSimCostOutside) spendTestSimCost();
			g_AudioMan.RetireFinishedSimulationSounds();
			const bool watchLedgerExpiry = s_eventLedgerPressTick > 0;
			const uint64_t expiredBefore = watchLedgerExpiry ? PreviewEventLedger::GetCounters().expired : 0;
			const size_t ghostsBeforeExpire = watchLedgerExpiry ? g_MovableMan.GetPreviewGhostCount() : 0;
			PreviewEventLedger::ExpireForTick(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
			if (watchLedgerExpiry && PreviewEventLedger::GetCounters().expired > expiredBefore && g_MovableMan.GetPreviewGhostCount() < ghostsBeforeExpire) {
				s_eventLedgerExpireDroppedGhost = true;
				s_eventLedgerExpireDetail = "the run's own expiry dropped a ghost at committed tick " + std::to_string(g_TimerMan.GetSimUpdateCount());
			}
			if (Activity* activity = g_ActivityMan.GetActivity()) {
				activity->ExpirePresentationViews(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
			}

			g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::SimTotal);

			const uint64_t simTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
			BeginCrossTick(simTick);
			if (s_checkpointFixturePrimeScripts && simTick == 1) {
				// Both arms of the checkpoint fixture start with the same scripted
				// actors. Priming does not run an extra Update on a live actor.
				std::list<SceneObject*> actors;
				g_MovableMan.GetAllActors(false, actors);
				for (SceneObject* object: actors) if (auto* actor = dynamic_cast<Actor*>(object)) actor->InitializeObjectScriptsIfNeeded();
			}
			if (!s_loadGameName.empty() && ScenarioRunner::GetArgs().maxTicks > 0 &&
			    simTick >= static_cast<uint64_t>(ScenarioRunner::GetArgs().maxTicks)) {
				System::SetQuit(true);
			}
			if (s_cowCheckpointAutosave && g_ActivityMan.ActivityRunning() && simTick == 1) {
				// The isolation and same-tick rows need a live scene, which the standalone flag has not got.
				RTE::RunCheckpointSceneRows();
			}
			// A match a menu script started reports its rules too: what the menus chose is what the round plays by.
			if (simTick == 1 && (s_netMatchServiceE2E || !s_netReplayInPath.empty() || ScenarioRunner::IsActive() || !s_menuScriptPath.empty())) {
				if (auto* activity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity())) {
					{
						std::ostringstream line;
						line << "[e2e] rules tick=" << simTick << " difficulty=" << activity->GetDifficulty()
						     << " gold=" << activity->GetStartingGold() << " fog=" << activity->GetFogOfWarEnabled()
						     << " orbit=" << activity->GetRequireClearPathToOrbit() << " deploy=" << g_SceneMan.GetPlaceUnitsOnLoad();
						for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
							line << " team" << team << ".tech=" << std::quoted(activity->GetTeamTech(team))
							     << " team" << team << ".ai=" << activity->GetTeamAISkill(team)
							     << " team" << team << ".funds=" << activity->GetTeamFunds(team);
						}
						const Scene* loadedScene = g_SceneMan.GetScene();
						line << " cpu_team=" << activity->GetCPUTeam() << " activity=" << std::quoted(activity->GetModuleAndPresetName())
						     << " scene=" << std::quoted(loadedScene ? loadedScene->GetModuleAndPresetName() : std::string()) << std::endl;
						System::PrintDiagnosticLine(line.str());
					}
				}
			}
			// Sample the sim hash on an interval during live lockstep for the runtime desync check. It runs
			// under -tick-hashes too: an offline trace compare only reads the divergence after the run, so
			// skipping it there left the live check dead in every two-peer harness arm.
			constexpr uint64_t c_DesyncCheckIntervalTicks = 30;
			const bool desyncSampleTick = s_netDesyncCheck && ScenarioRunner::IsLockstepControllerSyncActive() &&
			                              (simTick % c_DesyncCheckIntervalTicks == 0);
			const bool a7HashTick = NetA7Journal::Enabled() && ScenarioRunner::IsLockstepControllerSyncActive();
			const bool liveHashTick = !s_netLiveTickHashPath.empty() && (ScenarioRunner::IsLockstepControllerSyncActive() || ScenarioRunner::IsActive() || ScenarioRunner::WorldCatchUpActive());
			const bool hashThisTick = s_recordTickHashes || desyncSampleTick || a7HashTick || liveHashTick;
			if (hashThisTick) {
				g_SimChecksum.BeginTick(simTick);
			}

			// Positive control — inject one genuine non-determinism at a fixed tick so the offline determinism
			// gate OR the runtime desync detector sees a guaranteed divergence. One-shot: a resynced
			// match reuses tick numbers, and the healed round must NOT be re-poisoned.
			static bool s_perturbFired = false;
			bool perturbDue = simTick == ScenarioRunner::GetArgs().selftestPerturbTick;
			if (s_netPerturbWhenLive) {
				perturbDue = simTick >= ScenarioRunner::GetArgs().selftestPerturbTick && simTick % c_DesyncCheckIntervalTicks == 0 && ScenarioRunner::IsLockstepControllerSyncActive() && !ScenarioRunner::WorldCatchUpActive();
				const auto match = ScenarioRunner::GetLockstepMatchConfig();
				if (!match) perturbDue = false;
				else for (uint8_t peer = 1; peer <= match->peerCount; ++peer)
					if (ScenarioRunner::IsLockstepPeerGone(peer, simTick) || ScenarioRunner::IsLockstepSeatReclaimGap(peer, simTick)) perturbDue = false;
			}
			if ((ScenarioRunner::IsActive() || s_netMatchServiceE2E) && ScenarioRunner::GetArgs().selftestPerturb && perturbDue && !s_perturbFired) {
				s_perturbFired = true;
				// Named in both modes: a run's oracle excuses only the desync it can see was injected.
				System::PrintDiagnosticLine("[net-test] live perturb frame=" + std::to_string(simTick));
				std::random_device perturbDevice;
				const unsigned perturbAdvance = (perturbDevice() % 64u) + 1u;
				for (unsigned k = 0; k < perturbAdvance; ++k) {
					g_SimRNG.RandomNum<uint32_t>();
				}
			}

			// E2E control: the host pauses at the flag's tick (default 250) and unpauses 180 ticks
			// later; both sims must stop and resume on the same frame with sim time frozen across the gap.
			if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestPauseCommand) {
				const uint64_t pauseTick = ScenarioRunner::GetArgs().selftestPauseTick;
				if (simTick == pauseTick) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{0, true}});
				} else if (simTick == pauseTick + s_netTestPauseTicks) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{0, false}});
				}
			}
			// E2E control: at the flag's tick every armed peer ends the round through the natural
			// end path - SetWinnerTeam + ActivityMan::EndActivity() -> Activity::End() -> Over, the
			// same entry the scripted activity's Lua calls. Deliberately not gated on
			// lockstepPausedTick: a tick inside the pause window must still end the round so the
			// rematch detector below sees it.
			if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestEndRoundTick > 0 &&
			    ScenarioRunner::IsLockstepControllerSyncActive() && simTick == ScenarioRunner::GetArgs().selftestEndRoundTick) {
				if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity());
				    gameActivity && gameActivity->GetActivityState() != Activity::Over) {
					{
						std::ostringstream line;
						line << "[net-match-service-e2e] end-round: ending the round at tick " << simTick;
						System::PrintDiagnosticLine(line.str());
					}
					gameActivity->SetWinnerTeam(Activity::TeamOne);
					g_ActivityMan.EndActivity();
				}
			}
			// E2E control: stand in for each local player's DONE in the synchronized setup editor. The two
			// seats place different brains at different spots, so a placement that failed to cross the wire
			// leaves the peers holding different brains and the shared tick hashes part.
			if (s_netMatchServiceE2E && s_netMatchE2EBrainPlacement && ScenarioRunner::IsLockstepControllerSyncActive()) {
				if (auto* placementActivity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity());
				    placementActivity && placementActivity->GetActivityState() == Activity::Editing) {
					static const std::pair<const char*, const char*> s_e2eBrains[] = {{"Actor", "Brain Case"}, {"AHuman", "Brain Robot"}, {"Actor", "Brain Case"}, {"AHuman", "Brain Robot"}};
					for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
						if (!placementActivity->IsSeatActive(player) || !placementActivity->IsLocalHumanSeat(player)) {
							continue;
						}
						if (placementActivity->PlaceAndSubmitLockstepBrain(player, s_e2eBrains[player].first, s_e2eBrains[player].second, "Base.rte")) {
							{
								std::ostringstream line;
								line << "[net-match-service-e2e] brain placement submitted: seat=" << player
								     << " preset=" << s_e2eBrains[player].second << " tick=" << simTick;
								System::PrintDiagnosticLine(line.str());
							}
						}
					}
				}
			}

			const bool lockstepPaused = ScenarioRunner::IsLockstepPaused();
			// The synchronized setup editor holds the world still while the seats place their brains, so a
			// per-machine editor never edits a sim that is advancing and every peer holds the same ticks.
			// The editor itself still runs, and the placements ride the held tick's own frame exchange.
			const Activity* setupActivity = ScenarioRunner::IsLockstepControllerSyncActive() ? g_ActivityMan.GetActivity() : nullptr;
			const bool lockstepSetupHold = !lockstepPaused && setupActivity && setupActivity->GetActivityState() == Activity::Editing;
			const bool lockstepPausedTick = lockstepPaused || lockstepSetupHold;
			if (lockstepPausedTick) {
				// The sim holds still: read the sim-rate resume key, exchange an empty frame so
				// commands and stops still flow, and step the shared resume countdown.
				if (!s_netMatchServiceE2E && lockstepPaused && g_UInputMan.KeyPressedSim(SDLK_P)) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{g_NetMatchService.GetLocalTeam(), false}});
				}
				if (lockstepSetupHold) {
					g_ActivityMan.Update();
				}
				g_MovableMan.RunLockstepPausedTick();
				if (lockstepPaused) {
					ScenarioRunner::AdvanceLockstepPausedTick();
				}
				// The session plane keeps running through a pause: a seat held during it is served its image and its tail.
				g_NetMatchService.PumpSessionEvents();
				if (const NetMatchServiceState netServiceState = g_NetMatchService.GetState(); netServiceState == NetMatchServiceState::Running) {
					g_NetMatchService.Update();
				}
			}
			if (!lockstepPausedTick) {
				{
					// The scripts' own housekeeping reads the round only through guarded calls, as the update below does.
					NetLockstepPlane::Window planeWindow("lua update");
					g_LuaMan.Update();
				}

				// E2E control: host-issued funds command at tick 50; both peers must apply it identically.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestFundsCommand && static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) == 50) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSetTeamFunds{0, 5000}});
				}
				// E2E control: host-issued spawn command at tick 50; both peers must clone the identical actor.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestSpawnCommand && static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) == 50) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSpawnActor{"AHuman", "Green Dummy", "Base.rte", 1000.0F, 200.0F, 0}});
				}
				for (const E2eNamedSpawn& spec: s_e2eNamedSpawns) {
					if (s_netMatchServiceE2E && simTick == spec.tick) {
						{
							std::ostringstream line;
							line << "[net-match-service-e2e] spawn " << spec.className << " " << spec.preset << " at " << spec.x << "," << spec.y << " tick " << spec.tick;
							System::PrintDiagnosticLine(line.str());
						}
						ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSpawnActor{spec.className, spec.preset, spec.module, spec.x, spec.y, spec.team}});
					}
				}
				// E2E control: host-issued delivery at tick 50; both peers must build the identical craft, hold, and flight.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestDeliverCommand && static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) == 50) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameDeliverCargo{"ACDropShip", "Dropship MK1", "Base.rte", 880.0F, 100.0F, 0, {{"AHuman", "Green Dummy", "Base.rte"}, {"AHuman", "Green Dummy", "Base.rte"}}}});
				}
				// The same delivery with no match around it: a scenario run issues it at the tick the
				// match's delivery lands on, so a single-player dump can be read beside a lockstep one.
				if (!ScenarioRunner::HasLockstepCoordinator() && ScenarioRunner::GetArgs().scenarioDeliverCommandTick >= 0 &&
				    simTick == static_cast<uint64_t>(ScenarioRunner::GetArgs().scenarioDeliverCommandTick)) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameDeliverCargo{"ACDropShip", "Dropship MK1", "Base.rte", 880.0F, 100.0F, 0, {{"AHuman", "Green Dummy", "Base.rte"}, {"AHuman", "Green Dummy", "Base.rte"}}}});
				}
				// E2E control: the host orders its dummy and its brain at fixed ticks; both peers must hold the identical AI mode, waypoints and squad.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestAIOrderCommand && (simTick == 50 || simTick == 200 || simTick == 400 || simTick == 600)) {
					std::vector<Actor*> units;
					for (Actor* actor: *g_MovableMan.GetTeamRoster(0)) {
						if (!actor->IsInGroup("Brains") && dynamic_cast<AHuman*>(actor)) {
							units.push_back(actor);
						}
					}
					std::sort(units.begin(), units.end(), [](const Actor* lhs, const Actor* rhs) { return lhs->GetUniqueID() < rhs->GetUniqueID(); });
					Actor* brain = g_MovableMan.GetFirstBrainActor(0);
					if (!units.empty() && brain) {
						Actor* unit = units.front();
						const auto order = [](const Actor* actor, uint8_t op, const Vector& point, const Actor* target) {
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameAIOrder{static_cast<int64_t>(actor->GetUniqueID()), actor->GetTeam(), op, point.m_X, point.m_Y, target ? static_cast<int64_t>(target->GetUniqueID()) : 0}});
						};
						if (simTick == 50) {
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSetActorAIMode{static_cast<int64_t>(unit->GetUniqueID()), 0, static_cast<uint8_t>(Actor::AIMODE_GOTO)}});
							order(unit, NetGameAIOrder::SceneWaypoint, unit->GetPos() + Vector(300.0F, 0.0F), nullptr);
						} else if (simTick == 200) {
							order(unit, NetGameAIOrder::MOWaypoint, unit->GetPos(), brain);
						} else if (simTick == 400) {
							order(brain, NetGameAIOrder::FormSquad, brain->GetPos() + Vector(600.0F, 0.0F), nullptr);
						} else {
							order(brain, NetGameAIOrder::DisbandSquad, brain->GetPos(), nullptr);
						}
						{
							std::ostringstream line;
							line << "[net-match-service-e2e] ai order issued at tick " << simTick << " unit " << unit->GetUniqueID() << " brain " << brain->GetUniqueID();
							System::PrintDiagnosticLine(line.str());
						}
					}
				}
				if (s_netMatchServiceE2E && s_netMatchE2eSwitchControlTick > 0 && ScenarioRunner::IsLockstepControllerSyncActive()) {
					if (!s_netMatchE2eSwitchIssued && simTick == s_netMatchE2eSwitchControlTick) {
						if (Activity* activity = g_ActivityMan.GetActivity()) {
							const int player = activity->PlayerOfScreen(0);
							if (Actor* target = FindE2eSwitchControlTarget(activity, player)) {
								s_netMatchE2eSwitchUid = static_cast<int64_t>(target->GetUniqueID());
								activity->SwitchToActor(target, player, activity->GetTeamOfPlayer(player));
								{
									std::ostringstream line;
									line << "[net-match] e2e switch-control: uid=" << s_netMatchE2eSwitchUid << " at tick " << simTick;
									System::PrintDiagnosticLine(line.str());
								}
							} else {
								{
									std::ostringstream line;
									line << "[net-match] e2e switch-control: uid=0 at tick " << simTick;
									System::PrintDiagnosticLine(line.str());
								}
							}
							s_netMatchE2eSwitchIssued = true;
						}
					} else if (s_netMatchE2eSwitchIssued && !s_netMatchE2eSwitchHandedBack && simTick == s_netMatchE2eSwitchControlTick + 10) {
						if (Activity* activity = g_ActivityMan.GetActivity()) {
							const int player = activity->PlayerOfScreen(0);
							if (Actor* brain = activity->GetPlayerBrain(player)) {
								activity->SwitchToActor(brain, player, activity->GetTeamOfPlayer(player));
								{
									std::ostringstream line;
									line << "[net-match] e2e switch-control: hand-back at tick " << simTick;
									System::PrintDiagnosticLine(line.str());
								}
							}
							s_netMatchE2eSwitchHandedBack = true;
						}
					}
				}
				// E2E control: host-issued inventory ops on its brain at fixed ticks; both peers must mutate identically.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestInventoryCommand &&
				    (simTick == 210 || simTick == 240 || simTick == 270 || simTick == 300)) {
					if (const Actor* brain = g_MovableMan.GetFirstBrainActor(0)) {
						NetGameInventoryOp op;
						op.actorUID = static_cast<int64_t>(brain->GetUniqueID());
						op.team = 0;
						if (simTick == 210) {
							op.op = NetGameInventoryOp::Reorder;
							op.a = 0;
							op.b = 1;
						} else if (simTick == 240) {
							op.op = NetGameInventoryOp::SwapEquipped;
							op.a = 0;
							op.b = 0;
						} else if (simTick == 270) {
							op.op = NetGameInventoryOp::Reload;
							op.a = 0;
							op.b = -1;
						} else {
							op.op = NetGameInventoryOp::Drop;
							op.a = -1;
							op.b = 0;
							op.hasDropDirection = true;
							op.dirX = 0.7F;
							op.dirY = -0.7F;
						}
						{
							std::ostringstream line;
							line << "[net-match-service-e2e] inventory op " << static_cast<int>(op.op) << " at tick " << simTick << " actor " << op.actorUID;
							System::PrintDiagnosticLine(line.str());
						}
						ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, op});
					}
				}
				// E2E control: the host grants funds then places a REAL buy order through GameActivity::CreateDelivery,
				// exercising the confirm seam -> wire -> queued arrival -> identical funds deduction on both peers.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestBuyCommand) {
					if (simTick == 50) {
						ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSetTeamFunds{0, 5000}});
					} else if (simTick == 80) {
						if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity())) {
							const SceneObject* craft = dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset("ACDropShip", "Dropship MK1", "Base.rte"));
							const SceneObject* dummy = dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset("AHuman", "Green Dummy", "Base.rte"));
							if (craft && dummy) {
								gameActivity->AddOverridePurchase(craft, 0);
								gameActivity->AddOverridePurchase(dummy, 0);
								gameActivity->AddOverridePurchase(dummy, 0);
								gameActivity->SetLandingZone(Vector(900.0F, 0.0F), 0);
								const bool ordered = gameActivity->CreateDelivery(0);
								{
									std::ostringstream line;
									line << "[net-match-service-e2e] buy order placed: " << (ordered ? "ok" : "FAILED");
									System::PrintDiagnosticLine(line.str());
								}
							}
						}
					} else if (simTick == 700) {
						if (const Activity* activity = g_ActivityMan.GetActivity()) {
							{
								std::ostringstream line;
								line << "[net-match-service-e2e] team 0 funds at tick 700: " << activity->GetTeamFunds(0);
								System::PrintDiagnosticLine(line.str());
							}
						}
					}
				}
				// E2E control: host scuttles the delivered craft at tick 100; both peers must gib it identically.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestScuttleCommand && simTick == 100) {
					if (const int64_t craftUID = g_MovableMan.GetFirstCraftUniqueID(0)) {
						ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameScuttleCraft{craftUID, 0}});
					}
				}
				// Test control: fake a hung peer — this peer stops producing frames for 8s; the other side
				// must ride out the stall within the grace window and both must still finish identical.
				// Works in e2e AND interactive matches so the headed stall overlay can be exercised.
				if (ScenarioRunner::GetArgs().selftestStall && ScenarioRunner::IsLockstepControllerSyncActive() && simTick == 300) {
					{
						std::ostringstream line;
						line << "[net-match] stall: sleeping 8s at tick 300";
						System::PrintDiagnosticLine(line.str());
					}
					std::this_thread::sleep_for(std::chrono::seconds(8));
				}
				// Test control: leave the match at the flag's tick (default 300) like a pause-menu quit, which happens between
				// ticks: the leave waits for this tick's end, so the tick's hash and checksum are the ones every peer computes.
				if (ScenarioRunner::GetArgs().selftestLeave && ScenarioRunner::IsLockstepControllerSyncActive() && simTick == ScenarioRunner::GetArgs().selftestLeaveTick) {
					s_scriptedLeaveDue = true;
				}
				// E2E control: this peer spawns a SECOND brain for its own team; the win condition must ride
				// through the original brain's death because the team still has the spawned one.
				if (s_netMatchServiceE2E && ScenarioRunner::GetArgs().selftestBrainSpawnCommand && simTick == 40) {
					{
						std::ostringstream line;
						line << "[net-match-service-e2e] brain spawn: team 1 at 1250,700 (sentry)";
						System::PrintDiagnosticLine(line.str());
					}
					// Spawn the spare as SENTRY like the real brains (P4AlphaDuel), so it holds position and
					// survives the original's death instead of wandering into the kill zone on BRAINHUNT.
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameSpawnActor{"AHuman", "Brain Robot", "Base.rte", 1250.0F, 700.0F, 1, Actor::AIMODE_SENTRY}});
				}
				// Test control: the host delivers two crafts just above the enemy brain and scuttles each as
				// its hatch opens; both peers must trace the identical game-over transition. The spawn height
				// is computed from the terrain and rides the synced command, so both peers see the same drop.
				// Works in interactive matches too, so a headed match can be ended deterministically.
				if (ScenarioRunner::GetArgs().selftestBrainKillCommand && ScenarioRunner::IsLockstepControllerSyncActive()) {
					// Not before tick 80: an activity Over inside the first 100 running ticks reads as a broken setup.
					// Teams 2/3 exist only in 3/4-peer matches; their kill windows are query-gated no-ops otherwise.
					const bool teamOneWindow = simTick == 80 || simTick == 100;
					const bool teamTwoWindow = simTick == 130 || simTick == 150;
					const bool teamThreeWindow = simTick == 180 || simTick == 200;
					if (teamOneWindow || teamTwoWindow || teamThreeWindow) {
						const int targetTeam = teamOneWindow ? 1 : (teamTwoWindow ? 2 : 3);
						const bool firstDrop = simTick == 80 || simTick == 130 || simTick == 180;
						// Aim at the live brain and drop low; static coordinates drift off on a different sim's physics.
						if (const Actor* enemyBrain = g_MovableMan.GetFirstBrainActor(targetTeam)) {
							const float dropX = enemyBrain->GetPos().m_X + (firstDrop ? 4.0F : -4.0F);
							const float dropY = g_SceneMan.FindAltitude(Vector(dropX, 0.0F), 2000, 20) - 60.0F;
							{
								std::ostringstream line;
								line << "[net-match-service-e2e] brain-kill deliver: tick=" << simTick << " team=" << targetTeam << " x=" << dropX << " y=" << dropY;
								System::PrintDiagnosticLine(line.str());
							}
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", dropX, dropY, 0, {{"AHuman", "Green Dummy", "Base.rte"}}}});
						} else if (teamOneWindow) {
							// The tuned 2-peer fallback: keep the original blind drop when team 1's brain query misses.
							const float dropX = simTick == 80 ? 1120.0F : 1112.0F;
							const float dropY = g_SceneMan.FindAltitude(Vector(dropX, 0.0F), 2000, 20) - 60.0F;
							{
								std::ostringstream line;
								line << "[net-match-service-e2e] brain-kill deliver: tick=" << simTick << " x=" << dropX << " y=" << dropY;
								System::PrintDiagnosticLine(line.str());
							}
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", dropX, dropY, 0, {{"AHuman", "Green Dummy", "Base.rte"}}}});
						}
					}
					// The poll's own bounds already skip the original 80/100 windows, so it must not be
					// an else of the (new, wider) window check or the added windows would eat poll ticks.
					if (simTick > 100 && simTick % 5 == 0) {
						if (const int64_t craftUID = g_MovableMan.GetFirstUnloadingCraftUniqueID(0)) {
							{
								std::ostringstream line;
								line << "[net-match-service-e2e] brain-kill scuttle: tick=" << simTick << " craft=" << craftUID;
								System::PrintDiagnosticLine(line.str());
							}
							ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGameScuttleCraft{craftUID, 0}});
						}
					}
				}
				// Test control: every peer hurts every team's brain on the same applied frame, so a
				// brain-damage reaction only one peer takes stands out as a shared-state difference.
				// The damage rides an attachable's damage counter because Actor::Update only sees a
				// drop it collects itself, between saving the previous health and reacting to it.
				if (s_netMatchE2eBrainDamageTick > 0 && simTick == s_netMatchE2eBrainDamageTick && ScenarioRunner::IsLockstepControllerSyncActive()) {
					for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
						Actor* brain = g_MovableMan.GetFirstBrainActor(team);
						if (!brain) {
							continue;
						}
						Attachable* target = nullptr;
						for (Attachable* attachable: brain->GetAttachables()) {
							if (attachable->GetDamageMultiplier() > 0.0F && (!target || attachable->GetUniqueID() < target->GetUniqueID())) {
								target = attachable;
							}
						}
						if (!target) {
							continue;
						}
						target->AddDamage(2.0F / target->GetDamageMultiplier());
						{
							std::ostringstream line;
							line << "[net-match-service-e2e] brain-damage: tick=" << simTick << " team=" << team << " brain=" << brain->GetUniqueID()
							     << " attachable=" << target->GetUniqueID() << " health=" << brain->GetHealth();
							System::PrintDiagnosticLine(line.str());
						}
					}
				}

				// Test control: the seating peer moves its seat to its team's LAST brain, the shape a script
				// takes when a team has more than one brain and the seat is not at the first of them.
				if (s_netMatchE2eBrainReseatTick > 0 && simTick == s_netMatchE2eBrainReseatTick && ScenarioRunner::IsLockstepControllerSyncActive()) {
					if (Activity* activity = g_ActivityMan.GetActivity()) {
						for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
							if (!activity->PlayerActive(player) || !activity->PlayerHuman(player)) {
								continue;
							}
							const int team = activity->GetTeamOfPlayer(player);
							Actor* lastBrain = nullptr;
							for (Actor* actor: *g_MovableMan.GetTeamRoster(team)) {
								if (actor->HasObjectInGroup("Brains")) {
									lastBrain = actor;
								}
							}
							if (lastBrain && lastBrain != activity->GetPlayerBrain(player)) {
								{
									std::ostringstream line;
									line << "[net-match-service-e2e] brain-reseat: tick=" << simTick << " team=" << team
									     << " brain=" << lastBrain->GetUniqueID();
									System::PrintDiagnosticLine(line.str());
								}
								activity->SetPlayerBrain(lastBrain, player);
							}
						}
					}
				}

				// P pauses the match for every peer; the command applies on the same synced frame.
				if (ScenarioRunner::IsLockstepControllerSyncActive() && !s_netMatchServiceE2E && g_UInputMan.KeyPressedSim(SDLK_P)) {
					ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{0, NetGamePauseMatch{g_NetMatchService.GetLocalTeam(), true}});
				}

				// Mid-match session upkeep: reconnect handshakes the coordinator handed over.
				g_NetMatchService.PumpSessionEvents();
				// Chat script sends and the receive drain ride the sim tick so [chat] lines order with the match.
				if (s_netMatchServiceE2E) {
					NetChatScriptOnSimTick(simTick);
				}
				// The session-directory heartbeat rides Update on the game thread, never the pump.
				if (const NetMatchServiceState netServiceState = g_NetMatchService.GetState();
				    netServiceState == NetMatchServiceState::Starting || netServiceState == NetMatchServiceState::ReadyToLaunch ||
				    netServiceState == NetMatchServiceState::Running || netServiceState == NetMatchServiceState::Completed) {
					g_NetMatchService.Update();
				}
				DriveModerationE2e();

				g_FrameMan.Update();

				g_MovableMan.CompleteQueuedMOIDDrawings();

				g_ConsoleMan.Update();
				{
					static const uint64_t soundPhase = Hash("Tick");
					SoundSimulationScope simulationSounds(0, soundPhase);
					// A long update is this machine's own: the plane commits for the round meanwhile, and the update reads the coordinator only through guarded calls.
					NetLockstepPlane::Window planeWindow("activity update");
					if (s_testSimCostUs > 0 && !s_testSimCostOutside) spendTestSimCost();
					g_ActivityMan.Update();

					if (g_SceneMan.GetScene()) {
						g_SceneMan.GetScene()->Update();
					}

					g_LuaMan.ClearScriptTimings();
					g_MovableMan.Update();
				}
			}
			if (ScenarioRunner::HasControllerReplayError()) {
				HandleControllerReplayFailure(returnToMenuAfterNetworkEnd);
				g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
				break;
			}
			g_PerformanceMan.UpdateSortedScriptTimings(g_LuaMan.GetScriptTimings());

			g_AudioMan.Update();
			g_MusicMan.Update();

			if (!lockstepPausedTick) {
				// A long late script is this machine's own like the update's: the plane commits for the round meanwhile.
				NetLockstepPlane::Window planeWindow("late scripts");
				if (s_lateScriptStallMs > 0 && !s_lateScriptStallFired && g_TimerMan.GetSimUpdateCount() >= s_lateScriptStallTick) {
					// A late global script that runs long: the main thread is inside the tick's scripts, not waiting on the round.
					s_lateScriptStallFired = true;
					System::PrintDiagnosticLine("[selftest] late script stall tick=" + std::to_string(g_TimerMan.GetSimUpdateCount()) + " ms=" + std::to_string(s_lateScriptStallMs));
					const uint64_t planeTicksBefore = NetLockstepPlane::Ticks();
					std::this_thread::sleep_for(std::chrono::milliseconds(s_lateScriptStallMs));
					System::PrintDiagnosticLine("[selftest] late script stall done plane_ticks=" + std::to_string(NetLockstepPlane::Ticks() - planeTicksBefore));
				}
				g_ActivityMan.LateUpdateGlobalScripts();
				g_MovableMan.CapturePhysicsHistory(simTick, 3);
				// Kick the async MOID draw after the last main-thread sim mutation of the tick; it
				// completes before the render frames below, which share draw scratch state with it.
				g_MovableMan.StartMOIDDrawTask();
			}

			DumpSimStateIfArmed(simTick);
			LocalPrediction::CompareFidelityAtTick(simTick);
			NoteE2eSwitchOwnerLog(simTick);
			TickProbeIfArmed(simTick);
			LocalPredictionInvarianceOnTick(simTick);
			if (LuaBindingExhaustiveSelfTest::OnTick(simTick) > 0) {
				s_netReplayExitCode = 5;
			}
			PreviewEventLedgerFrameOnTick();
			TrackUidsIfArmed(simTick);
			DumpTerrainIfArmed(simTick);
			{
				static const std::string s_wendPath = (!ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim")) + ".wend.terrainevents.txt";
				SceneMan::FlushTerrainEventsAtWindowEnd(simTick, s_wendPath);
			}

			// Feed end-of-tick terrain state, finalize this tick's hash, and hand the result to the
			// MetricsCollector for the per-tick determinism trace (no-op without an active scenario run).
			std::optional<SimChecksum::Result> probeTickResult;
			if (g_MetricsCollector.EventsEnabled()) {
				if (const auto config = ScenarioRunner::GetLockstepMatchConfig()) {
					s_crossContext["config_revision"] = config->configRevision;
					s_crossContext["config_hash"] = NetMatchConfigUtil::StoredConfigHash(*config);
				}
				s_crossContext["applied_frame"] = ScenarioRunner::GetLockstepAppliedFrame();
				s_crossContext["effective_start_frame"] = ScenarioRunner::GetLockstepEffectiveStartFrame();
				s_crossContext["wall_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
				g_MetricsCollector.UpdateEventContext(s_crossContext);
			}
			CrossConfirmLocalControllerInputs(simTick);
			g_MetricsCollector.FlushEventTick();
			if (g_MetricsCollector.EventsEnabled() && !lockstepPausedTick && !ScenarioRunner::WorldCatchUpActive() && s_crossContext.value("gameplay_tick", false)) {
				const uint64_t sourceRound = CrossRecordRound(s_crossContext.value("round", uint64_t{0}), s_crossContext.value("source_round", uint64_t{0}));
				if (!s_crossLastCommitted.contains(sourceRound)) {
					const uint64_t first = s_crossLastCommitted.empty() && s_crossTicketRejoin ? std::stoull(CrossEnvironment("CC_TEST_CROSS_MATCH_FIRST_TICK", "1")) : simTick;
					s_crossFirstGameplayTick[sourceRound] = first;
					s_crossLastCommitted[sourceRound] = first - 1;
				}
				auto& last = s_crossLastCommitted[sourceRound];
				if (simTick > last) { s_crossBudget += simTick - last; last = simTick; }
				const uint64_t first = s_crossFirstGameplayTick[sourceRound];
				g_MetricsCollector.WriteObservation({{"type", "progress"}, {"budget_tick", s_crossBudget}, {"first_gameplay_tick", first},
				    {"budget_base", s_crossBudget - (last - first + 1)}, {"budget_basis", "canonical_frame_advance_not_observation_coverage"}});
			}
			CrossEliminationAtCommittedTick(simTick);
			CrossEndTargetObservation(simTick);
			CrossRecoveryAtCommittedTick(simTick, lockstepPausedTick);
			if (hashThisTick) {
				const HarnessCost::SimulationSpan harnessSpan;
				// The object census goes in here, not inside MovableMan::Update: the checkpoint
				// archive below writes the same deques, so both have to read one instant.
				g_MovableMan.FeedTickEndChecksum();
				g_SceneMan.FeedTerrainToSimChecksum();
				const auto tickResult = g_SimChecksum.EndTick();
				RetractAbandonedTickHashes();
				if (liveHashTick) {
					nlohmann::json subsystems = nlohmann::json::object();
					// A subsystem with nothing to hash this tick (no actors left) still has its row entry, at the empty value.
					for (const auto& [name, hash]: SimChecksum::CompleteSubsystems(tickResult)) subsystems[name] = SimChecksum::HashHex(hash);
					nlohmann::json observation = s_crossContext.is_object() ? s_crossContext : nlohmann::json::object();
					// A catch-up replays committed ticks behind its overlay: the player plays none of them, so they are its private history.
					const bool catchUpTick = ScenarioRunner::WorldCatchUpActive();
					if (!observation.contains("phase")) observation["phase"] = catchUpTick ? "catchup" : "live";
					observation["player_visible"] = !catchUpTick;
					observation.update(nlohmann::json{{"round", ScenarioRunner::GetLockstepRoundId()}, {"tick", simTick},
					    {"wall_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count()},
					    {"peer", ScenarioRunner::GetLockstepLocalPeerId()}, {"paused", lockstepPausedTick},
					    {"total", SimChecksum::HashHex(tickResult.total)}, {"sim_gated", SimChecksum::HashHex(SimChecksum::SimGatedHash(tickResult))},
					    {"subsystems", std::move(subsystems)}});
					LiveTickHashStream().Write(observation.dump());
					// The first tick this process recorded in each round: where its comparable history begins, for a probe's dump.
					static uint64_t s_firstLiveRound = 0;
					if (const uint64_t round = ScenarioRunner::GetLockstepRoundId(); round != s_firstLiveRound) {
						s_firstLiveRound = round;
						ScenarioRunner::NoteHarnessReceipt("first_live_tick", nlohmann::json{{"round", round}, {"tick", simTick}}.dump());
					}
					// The harness's own tick-end cost on this machine, beside the capacity it publishes: the share of a slow seat that is the harness's.
					static std::vector<double> s_harnessTickUs;
					const int64_t harnessNs = harnessSpan.Stop();
					HarnessCost::Charge(HarnessCost::TickEnd, harnessNs);
					s_harnessTickUs.push_back(static_cast<double>(harnessNs) / 1000.0);
					if (s_harnessTickUs.size() == 600) {
						std::sort(s_harnessTickUs.begin(), s_harnessTickUs.end());
						// Whole microseconds cut down, never rounded up: a summary must not read above the frame records it summarises.
						const auto us = [](double value) { return static_cast<long long>(value); };
						System::PrintDiagnosticLine(std::format("[harness-cost] tick={} window=600 tick_end_us p50={} p95={} max={}", simTick, us(s_harnessTickUs[300]),
						                                        us(s_harnessTickUs[570]), us(s_harnessTickUs.back())));
						s_harnessTickUs.clear();
					}
				}
				if (a7HashTick && ScenarioRunner::GetLockstepAppliedFrame() == simTick) {
					const uint64_t round = ScenarioRunner::GetLockstepRoundId();
					NetA7Journal::AppliedTick(round, simTick, ScenarioRunner::GetLockstepLocalPeerId(), SimChecksum::HashHex(SimChecksum::SimGatedHash(tickResult)));
					g_MovableMan.RecordA7UnitOwnership(round, simTick);
				}
				if (s_recordTickHashes && s_rbProbePhase != 3 && !s_menuTraceCoverage.SkipsReplayedTick(simTick)) {
					const size_t recordedBefore = g_MetricsCollector.GetTickHashCount();
					g_MetricsCollector.RecordTickHash(tickResult, lockstepPausedTick);
					if (const size_t recorded = g_MetricsCollector.GetTickHashCount(); recorded != recordedBefore) {
						s_menuTraceCoverage.NoteRecorded(simTick, recorded == 1);
					}
				}
				// Key the exchange on the frame the sim applied, not on the local tick: after a resync
				// relaunch a peer can tick past the round's stop, and a hash labelled with that tick
				// would file two different states under one frame number.
				if (const uint64_t appliedFrame = ScenarioRunner::GetLockstepAppliedFrame();
				    desyncSampleTick && appliedFrame == simTick) {
					ScenarioRunner::SubmitLockstepChecksum(appliedFrame, SimChecksum::SimGatedHash(tickResult));
				}
				if ((s_rbProbeAtTick > 0 || s_rbProbeFuzzCount > 0) && (ScenarioRunner::IsActive() || ScenarioRunner::IsLockstepReplayPlayback())) {
					probeTickResult = tickResult;
				}
			}

			// Start async GC after all main-thread Lua work for this tick is done. Earlier (inside MovableMan::Update)
			// it overlapped with LateUpdateGlobalScripts on main, opening a window for ABBA between main holding one
			// state for the global script and a worker GC __gc finalizer wanting it from another state.
			// A tick the round named for a capture collects every state, on every peer, before the capture reads the world.
			g_LuaMan.StartAsyncGarbageCollection(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), g_NetMatchService.IsNamedCaptureTick(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount())));
			// Join before leaving the tick: an unfinished GC races the next tick's Lua for the state
			// mutexes, so collection timing (and per-peer sim state) would follow wall-clock scheduling.
			{
				NetLockstepPlane::Window collectionWindow("tick-end collection wait");
				g_LuaMan.WaitForAsyncGarbageCollection();
			}
			if (s_crossLeaveRequested) { s_crossLeaveRequested = false; s_scriptedLeaveDue = true; }
			if (const MinimizeLever& lever = TestMinimizeLever(); lever.to != 0 && g_WindowMan.GetWindow()) {
				SDL_Window* window = g_WindowMan.GetWindow();
				const uint64_t tick = static_cast<uint64_t>(simTick);
				if (tick == lever.from) SDL_MinimizeWindow(window);
				if (tick == lever.to) {
					// SDL minimizes an unfocused fullscreen window again, and a private desktop gives none focus, so it comes back as a window.
					if (lever.fullscreen && g_WindowMan.IsFullscreen()) g_WindowMan.ToggleFullscreen();
					SDL_RestoreWindow(window);
				}
				if (tick == lever.from || tick == lever.from + 60 || tick == lever.to || tick == lever.to + 60) {
					const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
					System::PrintDiagnosticLine("[selftest] window tick=" + std::to_string(tick) + " minimized=" + std::to_string((flags & SDL_WINDOW_MINIMIZED) != 0) +
					                            " hidden=" + std::to_string((flags & SDL_WINDOW_HIDDEN) != 0));
				}
				if (lever.fullscreen && tick + 60 >= lever.from && tick <= lever.to + 60 && (tick + 60 - lever.from) % 60 == 0) WriteTestWindowState(tick);
			}
			if (s_memoryCensusTicks != 0 && simTick % s_memoryCensusTicks == 0) PostMemoryCensus(simTick);
			if (s_scriptedLeaveDue) {
				s_scriptedLeaveDue = false;
				{
					std::ostringstream line;
					line << "[net-match] leave: quitting to menu at tick " << simTick;
					System::PrintDiagnosticLine(line.str());
				}
				g_ActivityMan.EndActivity();
				g_ActivityMan.SetInActivity(false);
			}

			// This is to support hot reloading entities in SceneEditorGUI. It's a bit hacky to put it in Main like this, but PresetMan has no update in which to clear the value, and I didn't want to set up a listener for the job.
			// It's in this spot to allow it to be set by UInputMan update and ConsoleMan update, and read from ActivityMan update.
			g_PresetMan.ClearReloadEntityPresetCalledThisUpdate();

			// The MOID draw must not overlap the render frames: both rotate sprites through shared
			// scratch bitmaps, so an overlap corrupts the hit layer per-peer.
			g_MovableMan.CompleteQueuedMOIDDrawings();

			// Sim consumed this tick's accumulated input edges; clear before next tick reads
			g_UInputMan.EndSimUpdate();
			if (probeTickResult) RollbackProbeOnHashedTick(simTick, *probeTickResult);
			if (s_checkpointAudioEffects && simTick == 1) {
				s_checkpointAudioEffectsPassed = g_AudioMan.RunCheckpointEffectsSelfTest();
				g_MetricsCollector.SetResult(s_checkpointAudioEffectsPassed);
				System::SetQuit(true);
			}
			if (s_checkpointWorldAudio && simTick == 60) {
				s_checkpointWorldAudioPassed = g_AudioMan.RunCheckpointWorldEffectsSelfTest();
				g_MetricsCollector.SetResult(s_checkpointWorldAudioPassed);
				System::SetQuit(true);
			}
			if (s_checkpointCaptureSelfTest && (simTick == 60 || simTick == 120)) {
				if (g_ActivityMan.RunCheckpointCaptureSelfTest(simTick)) ++s_checkpointCapturePasses;
				if (simTick == 120) {
					g_MetricsCollector.SetResult(s_checkpointCapturePasses == 2);
					System::SetQuit(true);
				}
			}
			// The self-test's capture rides the same tick boundary the live autosave uses, after the
			// census above: a capture taken at the top of the frame describes a different instant.
			if (s_cowCheckpointAutosave && g_ActivityMan.ActivityRunning() && simTick > 0 && (simTick == 1 || simTick % 60 == 0)) {
				// The capture freezes the sim thread, so its budget is one sim tick.
				constexpr int64_t captureBudgetUs = 16700;
				const bool saved = g_ActivityMan.SaveAutosaveSnapshot("c0de-a1", simTick);
				const int64_t freezeUs = CheckpointCow::Get().LastFreezeUs();
				const bool under = saved && freezeUs > 0 && freezeUs < captureBudgetUs;
				{
					std::ostringstream line;
					line << "[cow-checkpoint-selftest] " << (under ? "PASS" : "FAIL")
					     << " freeze_240_actors_under_one_tick freeze_us=" << freezeUs
					     << " saved=" << saved
					     << " (limit < 16700 us / one sim tick; RED today is the ~870 ms sim-thread stall of Scene::CaptureSavedScene plus Lua graph capture)";
					System::PrintDiagnosticLine(line.str());
				}
				// A capture must leave an archive behind and a second one must follow it in the same
				// process. The first capture walks every table once, so it is held to the stall the
				// design exists to remove; every capture after it owes the one-tick freeze budget.
				constexpr double firstWalkBudgetMs = 870.0;
				const double captureMs = g_ActivityMan.LastAutosaveCaptureMs();
				const bool firstCapture = s_cowCheckpointCaptures == 0;
				const double budgetMs = firstCapture ? firstWalkBudgetMs : static_cast<double>(captureBudgetUs) / 1000.0;
				const bool archived = saved && g_ActivityMan.LastAutosaveBytes() > 0 && g_ActivityMan.LastAutosaveTick() == simTick &&
				                      captureMs > 0.0 && captureMs < budgetMs;
				// The image and the world structure must name ONE frozen instant: every object the
				// structure's cohorts carry has to be in the scene the image wrote, and no other.
				if (archived) {
					const auto image = CheckpointCow::Get().Last();
					std::set<long> cohortUIDs, sceneUIDs;
					std::string membershipDetail;
					if (image) {
						// "<len> WorldStructure3 " then the six cohorts, each a count and that many ids.
						std::istringstream structure(image->structure.Text());
						std::string headerSize, headerTag;
						structure >> headerSize >> headerTag;
						for (int cohort = 0; cohort < 6 && structure; ++cohort) {
							size_t count = 0;
							structure >> count;
							for (size_t index = 0; index < count && structure; ++index) {
								long uid = 0;
								structure >> uid;
								cohortUIDs.insert(uid);
							}
						}
						membershipDetail = headerTag;
						const std::string& sceneText = image->scene.Text();
						for (size_t at = sceneText.find("PlaceSceneObject"); at != std::string::npos; at = sceneText.find("PlaceSceneObject", at + 1)) {
							const size_t next = sceneText.find("PlaceSceneObject", at + 1);
							const size_t id = sceneText.find("UniqueID = ", at);
							if (id != std::string::npos && (next == std::string::npos || id < next)) sceneUIDs.insert(std::strtol(sceneText.c_str() + id + 11, nullptr, 10));
						}
					}
					std::set<long> missing, extra;
					std::set_difference(cohortUIDs.begin(), cohortUIDs.end(), sceneUIDs.begin(), sceneUIDs.end(), std::inserter(missing, missing.end()));
					std::set_difference(sceneUIDs.begin(), sceneUIDs.end(), cohortUIDs.begin(), cohortUIDs.end(), std::inserter(extra, extra.end()));
					std::ostringstream line;
					line << "[cow-checkpoint-selftest] " << (image && !cohortUIDs.empty() && missing.empty() && extra.empty() ? "PASS" : "FAIL")
					     << " image_membership_matches_the_world_structure tick=" << simTick
					     << " cohorts=" << cohortUIDs.size() << " scene=" << sceneUIDs.size()
					     << " cohorts_only=" << (missing.empty() ? 0 : *missing.begin())
					     << " scene_only=" << (extra.empty() ? 0 : *extra.begin()) << " tag=" << membershipDetail;
					System::PrintDiagnosticLine(line.str());
				}
				// The codec's reader has only ever been held to hand-built fixtures. Parse the graphs
				// this capture actually wrote, in the states that wrote them, so writer and reader
				// are held to each other on a live match's own data.
				if (archived) {
					const auto image = CheckpointCow::Get().Last();
					size_t parsed = 0;
					std::string firstProblem;
					if (image) {
						std::vector<LuaStateWrapper*> states{&g_LuaMan.GetMasterScriptState()};
						for (LuaStateWrapper& threaded: g_LuaMan.GetThreadedScriptStates()) states.push_back(&threaded);
						for (size_t index = 0; index < image->graphs.size() && index < states.size(); ++index) {
							const std::string text = image->graphs[index].Text();
							if (text.empty()) continue;
							std::vector<std::string> problems;
							if (!states[index]->ValidateScriptGraph(text, problems) && firstProblem.empty()) {
								firstProblem = problems.empty() ? "refused without a reason" : problems.front();
							}
							++parsed;
						}
					}
					std::ostringstream line;
					line << "[cow-checkpoint-selftest] " << (parsed > 0 && firstProblem.empty() ? "PASS" : "FAIL")
					     << " captured_graphs_parse_in_their_own_state tick=" << simTick
					     << " graphs=" << parsed << " problem=" << (firstProblem.empty() ? "none" : firstProblem);
					System::PrintDiagnosticLine(line.str());
				}
				// The archive has to describe the instant the tick's hash was taken at: the same tick,
				// and exactly the objects the census fed. A capture taken anywhere else in the frame
				// carries a population no peer's hash ever covered.
				if (archived) {
					std::set<long> cohortUIDs;
					if (const auto image = CheckpointCow::Get().Last()) {
						std::istringstream structure(image->structure.Text());
						std::string headerSize, headerTag;
						structure >> headerSize >> headerTag;
						for (int cohort = 0; cohort < 6 && structure; ++cohort) {
							size_t count = 0;
							structure >> count;
							for (size_t index = 0; index < count && structure; ++index) {
								long uid = 0;
								structure >> uid;
								cohortUIDs.insert(uid);
							}
						}
					}
					const std::vector<long>& census = g_MovableMan.GetLastChecksumCensus();
					const std::set<long> hashed(census.begin(), census.end());
					std::set<long> unhashed, uncaptured;
					std::set_difference(cohortUIDs.begin(), cohortUIDs.end(), hashed.begin(), hashed.end(), std::inserter(unhashed, unhashed.end()));
					std::set_difference(hashed.begin(), hashed.end(), cohortUIDs.begin(), cohortUIDs.end(), std::inserter(uncaptured, uncaptured.end()));
					const uint64_t censusTick = g_MovableMan.GetLastChecksumCensusTick();
					const bool sameInstant = !census.empty() && censusTick == simTick && unhashed.empty() && uncaptured.empty();
					std::ostringstream line;
					line << "[cow-checkpoint-selftest] " << (sameInstant ? "PASS" : "FAIL")
					     << " archive_describes_the_hashed_instant tick=" << simTick
					     << " census_tick=" << censusTick << " census=" << hashed.size() << " cohorts=" << cohortUIDs.size()
					     << " archived_but_unhashed=" << (unhashed.empty() ? 0 : *unhashed.begin())
					     << " hashed_but_uncaptured=" << (uncaptured.empty() ? 0 : *uncaptured.begin());
					System::PrintDiagnosticLine(line.str());
				}
				if (++s_cowCheckpointCaptures <= 2) {
					std::ostringstream line;
					line << "[cow-checkpoint-selftest] " << (archived ? "PASS" : "FAIL")
					     << (s_cowCheckpointCaptures == 1 ? " autosave_capture_leaves_an_archive" : " autosave_capture_follows_another_in_the_same_process")
					     << " tick=" << simTick << " saved=" << saved << " bytes=" << g_ActivityMan.LastAutosaveBytes()
					     << " capture_ms=" << captureMs << " budget_ms=" << budgetMs << " first_walk=" << firstCapture;
					System::PrintDiagnosticLine(line.str());
				}
				// A capture is no sim event: a second one at the same tick leaves the world's structure, both RNG
				// streams and every allocation counter as they were, and the serializers draw and allocate nothing
				// while they run.
				if (s_cowCheckpointCaptures == 1) {
					const auto fingerprint = [] {
						return g_MovableMan.SaveWorldStructure() + "|" + g_SimRNG.SerializeStateForHashing() + "|" + g_RenderRNG.SerializeStateForHashing()
						     + "|" + std::to_string(MovableObject::GetUniqueIDCounter())
						     + "|" + std::to_string(g_AudioMan.GetCheckpointSoundContainerCursor()) + "|" + std::to_string(g_SimRNG.GetDrawCount());
					};
					const std::string before = fingerprint();
					const long uidBefore = MovableObject::GetUniqueIDCounter();
					const bool again = g_ActivityMan.SaveAutosaveSnapshot("c0de-a2", simTick);
					const ActivityMan::CaptureEffects effects = g_ActivityMan.LastCaptureEffects();
					const std::string after = fingerprint();
					const bool untouched = again && before == after && uidBefore == MovableObject::GetUniqueIDCounter();
					const bool pure = effects.uidsAllocated == 0 && effects.simDraws == 0 && effects.renderDraws == 0 && effects.soundCursorMoves == 0;
					std::ostringstream line;
					line << "[cow-checkpoint-selftest] " << (untouched && pure ? "PASS" : "FAIL") << " capture_is_side_effect_free tick=" << simTick
					     << " second_saved=" << again << " digest_same=" << (before == after) << " uid_counter=" << uidBefore << "->" << MovableObject::GetUniqueIDCounter()
					     << " during: uids_allocated=" << effects.uidsAllocated << " sim_draws=" << effects.simDraws << " render_draws=" << effects.renderDraws
					     << " sound_cursor_moves=" << effects.soundCursorMoves;
					System::PrintDiagnosticLine(line.str());
				}
			}
			// Test lever: the full-state oracle samples the same boundary the autosave captures at, on every live peer alike.
			// A round's first tick with committed input is sampled too, so a round shorter than the interval still has a sample
			// its peers share; the startup ticks before it run each machine's own seat bindings.
			const long long crossCaptureStartUs = g_TimerMan.GetAbsoluteTime();
			const long long crossCaptureWaitStartUs = ScenarioRunner::GetLockstepWaitUs();
			if (s_netFullStateEvery > 0) ScenarioRunner::SetLockstepAnnouncedCaptureEvery(s_netFullStateEvery);
			// A peer still catching up takes the labelled samples its replay passes, so every peer compares the ticks around a return.
			const bool catchingUp = ScenarioRunner::WorldCatchUpActive();
			if (s_netFullStateEvery > 0 && !lockstepPausedTick && ScenarioRunner::IsLockstepControllerSyncActive() &&
			    ScenarioRunner::GetLockstepAppliedFrame() == simTick && g_ActivityMan.ActivityRunning()) {
				static uint64_t s_fullStateSampledRound = 0;
				const uint64_t round = ScenarioRunner::GetLockstepRoundId();
				const uint64_t effectiveStart = ScenarioRunner::GetLockstepEffectiveStartFrame();
				const bool roundStart = !catchingUp && round != s_fullStateSampledRound && effectiveStart > 0 && simTick >= effectiveStart;
				if (roundStart) s_fullStateSampledRound = round;
				// A seat's reclaim frame is sampled on every peer alike, so its returner shares a sample even when the round ends first;
				// the labelled capture is never coalesced behind a busy writer.
				bool reclaimStart = false;
				for (uint8_t peer = 1; peer <= NetLockstepCodec::c_MaxPeerCount && !reclaimStart && simTick > 0; ++peer)
					reclaimStart = ScenarioRunner::IsLockstepSeatReclaimGap(peer, simTick) && !ScenarioRunner::IsLockstepSeatReclaimGap(peer, simTick - 1);
				if (reclaimStart) g_NetMatchService.CaptureFullStateHash(simTick, round, s_netFullStateDump, "reclaim");
				// Sixty ticks after a returner's reclaim gap closes it plays live on every peer, so that tick is sampled too.
				bool landed = false;
				for (uint8_t peer = 1; peer <= NetLockstepCodec::c_MaxPeerCount && !landed && simTick > 60; ++peer)
					landed = ScenarioRunner::IsLockstepSeatReclaimGap(peer, simTick - 60) && !ScenarioRunner::IsLockstepSeatReclaimGap(peer, simTick - 59);
				// A landed tick owes its sample even when another seat's gap opens on it; each label dumps into a folder of its own.
				if (landed) g_NetMatchService.CaptureFullStateHash(simTick, round, s_netFullStateDump, "landed");
				if (!catchingUp && ((roundStart && !reclaimStart) || simTick % s_netFullStateEvery == 0)) g_NetMatchService.CaptureFullStateHash(simTick, round, s_netFullStateDump);
			}
			g_NetMatchService.AutosaveAtTickBoundary(simTick, lockstepPausedTick);
			TelemetryBundle::CaptureAtTickBoundary();
			const long long crossCaptureUs = g_TimerMan.GetAbsoluteTime() - crossCaptureStartUs;
			const long long crossCaptureWaitUs = ScenarioRunner::GetLockstepWaitUs() - crossCaptureWaitStartUs;
			WriteHarnessCostFrame(static_cast<uint64_t>(simTick));
			// The watches' running totals, so a peer a scene kills has reported what it judged.
			if (simTick % 600 == 0) MenuAutomation::ReportWatches("periodic");

			// The paced round estimates execution cost without counting its idle interval.
			if (measureLockstepCost) {
				const float tickCostMs = static_cast<float>(std::max(0LL, g_TimerMan.GetAbsoluteTime() - paceTickStartUs)) / 1000.0F;
				g_PerformanceMan.UpdateMSPSU(tickCostMs);
				s_paceExecutionAverageMs.store(g_PerformanceMan.GetMSPSUAverage(), std::memory_order_relaxed);
				s_paceTickCostsMs.push_back(tickCostMs);
				if (s_paceTickCostsMs.size() > 15) s_paceTickCostsMs.pop_front();
			}
			g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::SimTotal);
			if (ScenarioRunner::WorldCatchUpActive()) ScenarioRunner::NoteWorldCatchUpTickCost(simTick, static_cast<uint64_t>(std::max(0LL, g_TimerMan.GetAbsoluteTime() - paceTickStartUs)), static_cast<uint64_t>(g_TimerMan.GetAbsoluteTime()));

			if (ScenarioRunner::IsLockstepControllerSyncActive()) {
				++s_paceSimTicks;
				const long long elapsedUs = g_TimerMan.GetAbsoluteTime() - paceTickStartUs;
				s_paceSimUs += elapsedUs;
				if (!lockstepPausedTick) ScenarioRunner::NoteLockstepLocalTickCost(simTick,
				    std::max(0LL, elapsedUs - (ScenarioRunner::GetLockstepWaitUs() - paceWaitStartUs)) / 1000.0);
				if (g_MetricsCollector.EventsEnabled()) {
					const long long waitUs = ScenarioRunner::GetLockstepWaitUs() - paceWaitStartUs;
					auto timing = MetricsCollector::TickTiming(elapsedUs, waitUs, crossCaptureUs, crossCaptureWaitUs);
					timing.update(nlohmann::json{
					    {"paused", lockstepPausedTick}, {"actors_alive", g_MovableMan.GetActorCount()},
					    {"particles_alive", g_MovableMan.GetParticleCount()}, {"record_bytes", g_MetricsCollector.EventBytes()},
					    {"trace_vector_payload_bytes", g_MetricsCollector.InstrumentationBytes()}, {"budget_tick", s_crossBudget}});
					g_MetricsCollector.WriteObservation(timing);
				}
			}

			// Capture both peers after the complete tick, including global callbacks and worker joins.
			if (ScenarioRunner::GetArgs().selftestSnapshot && ScenarioRunner::IsLockstepControllerSyncActive() && simTick == 300) {
				const auto saveStart = std::chrono::steady_clock::now();
				const std::string saveName = "p5snap_p" + std::to_string(ScenarioRunner::GetLockstepLocalPeerId());
				const bool saved = g_ActivityMan.SaveCurrentGame(saveName) && g_ActivityMan.WaitForSaveGameTask();
				const auto saveMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - saveStart).count();
				{
					std::ostringstream line;
					line << "[net-match] snapshot " << (saved ? "saved" : "FAILED") << ": " << saveName << " in " << saveMs << "ms at tick " << simTick;
					System::PrintDiagnosticLine(line.str());
				}
			}

			// Count the executed tick before the round's stop can break out of the loop below.
			if (s_netMatchServiceE2E) {
				const Activity* countedActivity = g_ActivityMan.GetActivity();
				const Activity::ActivityState countedState = countedActivity ? countedActivity->GetActivityState() : Activity::NotStarted;
				if (countedState == Activity::Running || countedState == Activity::Over) {
					s_netMatchE2ETicks.NoteSimTick(simTick);
				}
			}

			if (ScenarioRunner::FinishLockstepSimulationTick(simTick)) {
				const std::string reason = ScenarioRunner::GetLockstepStopReason();
				// A completed first round still takes the shared rematch transition below.
				if (!reason.starts_with("Complete:") || !IsE2ERematchReady()) {
					ScenarioRunner::SetControllerReplayError(reason);
					HandleControllerReplayFailure(returnToMenuAfterNetworkEnd);
					break;
				}
			}

			g_NetMatchService.PreparePrivateRejoinCheckpoint();

			if (s_bitmapSaveSelfTest && s_bitmapSaveSelfTestResult < 0) {
				s_bitmapSaveSelfTestResult = g_FrameMan.RunBitmapSaveSelfTest() ? 0 : 1;
				System::SetQuit(true);
				break;
			}
			if (s_saveCallbacksSelfTest && simTick > 0) {
				s_saveCallbacksSelfTestPassed = g_ActivityMan.RunSaveCallbacksSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (s_purgeSelfTest && simTick > 0) {
				s_purgeSelfTestPassed = g_MovableMan.RunPurgeSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (s_globalCallbacksSelfTest && simTick > 0) {
				s_globalCallbacksSelfTestPassed = g_ActivityMan.RunGlobalCallbacksSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (s_saveCatalogSelfTest && simTick > 0) {
				SaveLoadMenuGUI::RunCatalogSelfTest();
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			if (!s_contractAuditOperation.empty() && simTick >= s_contractAuditTick &&
			    (!s_contractAuditFinished || (ScenarioRunner::GetArgs().contractAuditContinueThrough > 0 &&
			      static_cast<uint64_t>(simTick) >= ScenarioRunner::GetArgs().contractAuditContinueThrough))) {
				const std::string base = ScenarioRunner::GetArgs().outPath + ".contract";
				ContractAudit::State pendingBefore;
				bool havePendingBefore = false;
				const auto observe = [&](const std::string& suffix) {
					std::vector<std::string> graphs, problems;
					const bool graph = ObserveScriptGraphs(graphs, problems);
					auto state = ContractAudit::Observe(base + "." + suffix + ".gaps.txt");
					ContractAudit::Write(ContractAudit::Identity(), base + "." + suffix + ".identity.txt");
					ContractAudit::Write(state, base + "." + suffix + ".state.txt");
					if (s_contractAuditOperation.starts_with("stage-")) {
						const auto pending = ContractAudit::ObservePendingCheckpoint(base + "." + suffix + ".pending.gaps.txt");
						ContractAudit::Write(pending, base + "." + suffix + ".pending.state.txt");
						if (suffix == "staged_before") { pendingBefore = pending; havePendingBefore = true; }
						else if (suffix == "staged_after" && havePendingBefore) {
							const size_t differences = ContractAudit::Compare(pendingBefore, pending, base + ".pending.diff.txt");
							{
								std::ostringstream line;
								line << "[contract-audit-pending] differences=" << differences << " before_fields=" << pendingBefore.size()
								     << " after_fields=" << pending.size();
								System::PrintDiagnosticLine(line.str());
							}
						}
					}
					for (const std::string& problem: problems) {
						std::ostringstream line;
						line << "[contract-audit] graph-problem=" << suffix << " " << problem;
						System::PrintDiagnosticLine(line.str());
					}
					for (size_t index = 0; index < graphs.size(); ++index) {
						std::ofstream out(base + "." + suffix + ".lua" + std::to_string(index), std::ios::binary);
						out << graphs[index];
					}
					std::ofstream deep(base + "." + suffix + ".simstate.txt");
					g_MovableMan.DumpSimState(g_TimerMan.GetSimUpdateCount(), deep);
					for (int index = 0; index <= static_cast<int>(g_LuaMan.GetThreadedScriptStates().size()); ++index) {
						g_LuaMan.GetStateByIndex(index).RunScriptString("if _ContractAuditCheck then _ContractAuditCheck('" + suffix + "') end; "
							"ConsoleMan:PrintString('[contract-audit-marker] observation=" + suffix + " vm=" + std::to_string(index) +
							" value=' .. tostring(_ContractAuditCandidateMarker))");
					}
					{
						std::ostringstream line;
						line << "[contract-audit] observation=" << suffix << " fields=" << state.size() << " graph=" << graph << " problems=" << problems.size();
						System::PrintDiagnosticLine(line.str());
					}
					return state;
				};
				if (s_contractAuditFinished) {
					observe("continued");
					{
						std::ostringstream line;
						line << "[contract-audit-continuation] completed_tick=" << g_TimerMan.GetSimUpdateCount()
						     << " requested_tick=" << ScenarioRunner::GetArgs().contractAuditContinueThrough;
						System::PrintDiagnosticLine(line.str());
					}
				} else {
				ScenarioRunner::SetContractAuditSeedMarker();
                const bool inputFixture = std::getenv("CC_CONTRACT_INPUT_FIXTURE") != nullptr;
                if (inputFixture) ContractAudit::SetInputFixture(false);
                const auto before = observe("before");
				bool prepared = true, applied = false;
				if (s_contractAuditOperation == "observe") {
					applied = true;
				} else if (ScenarioRunner::RunContractAuditLoad(s_contractAuditOperation,
				    [&](const std::string& suffix) { observe(suffix); }, prepared, applied)) {
				} else if (s_contractAuditOperation == "memory" || s_contractAuditOperation == "memory-perturb") {
					MovableMan::WorldSnapshot snapshot;
					prepared = g_MovableMan.CaptureWorld(snapshot);
					const auto captured = observe("captured");
					ContractAudit::Compare(before, captured, base + ".capture.diff.txt");
					if (prepared && s_contractAuditOperation == "memory-perturb") {
                        for (int index = 0; index <= static_cast<int>(g_LuaMan.GetThreadedScriptStates().size()); ++index) {
                            g_LuaMan.GetStateByIndex(index).RunScriptString("if _ContractAuditPerturb then _ContractAuditPerturb() end");
                        }
                        if (inputFixture) ContractAudit::SetInputFixture(true);
						const auto perturbed = observe("perturbed");
						ContractAudit::Compare(before, perturbed, base + ".perturb.diff.txt");
					}
					if (prepared) applied = g_MovableMan.RestoreWorld(snapshot);
				} else if (s_contractAuditOperation == "hold") {
                    MovableMan::WorldSetAside held;
                    prepared = g_MovableMan.SetAsideWorld(held);
                    if (prepared && inputFixture) ContractAudit::SetInputFixture(true);
                    if (prepared) applied = g_MovableMan.ReinstateWorld(held);
				} else if (s_contractAuditOperation == "preview") {
					const auto count = LocalPrediction::GetPreviewCount();
					LocalPrediction::SetCommandLineOverride(1);
					LocalPrediction::SetDepthOverride(6);
					LocalPrediction::Clear();
					LocalPrediction::RunPreview();
					LocalPrediction::Clear();
					applied = LocalPrediction::GetPreviewCount() > count;
				} else if (s_contractAuditOperation.starts_with("load:")) {
					prepared = g_ActivityMan.LoadGameToRestart(s_contractAuditOperation.substr(5));
					const auto staged = observe("staged");
					ContractAudit::Compare(before, staged, base + ".staging.diff.txt");
					if (prepared) applied = g_ActivityMan.RestartActivity();
				} else {
					prepared = g_ActivityMan.SaveCurrentGame("contract_audit") && g_ActivityMan.WaitForSaveGameTask();
					const auto saved = observe("saved");
					ContractAudit::Compare(before, saved, base + ".save.diff.txt");
                    if (prepared && s_contractAuditOperation == "file") {
                        for (int draw = 0; draw < 73; ++draw) g_SimRNG.RandomNum<uint32_t>();
                        if (inputFixture) ContractAudit::SetInputFixture(true);
						applied = g_ActivityMan.LoadAndLaunchGame("contract_audit");
					} else if (prepared && s_contractAuditOperation == "stage") {
						applied = g_ActivityMan.LoadGameToRestart("contract_audit");
					} else {
						applied = prepared;
					}
				}
				const auto after = observe("after");
				const size_t differences = ContractAudit::Compare(before, after, base + ".diff.txt");
				{
					std::ostringstream line;
					line << "[contract-audit] complete operation=" << s_contractAuditOperation << " prepared=" << prepared << " applied=" << applied << " differences=" << differences;
					System::PrintDiagnosticLine(line.str());
				}
				s_contractAuditFinished = true;
				ScenarioRunner::PerturbContractAuditContinuation();
				if (ScenarioRunner::GetArgs().contractAuditContinueThrough == 0) {
					System::SetQuit(true);
					g_ActivityMan.EndActivity();
					break;
				}
				if (g_TimerMan.GetSimUpdateCount() != simTick) {
					{
						std::ostringstream line;
						line << "[contract-audit-continuation] invalid_tick_change=1 before=" << simTick
							 << " after=" << g_TimerMan.GetSimUpdateCount();
						System::PrintDiagnosticLine(line.str());
					}
					s_contractAuditFinished = false;
					System::SetQuit(true);
					g_ActivityMan.EndActivity();
					break;
				}
				{
					std::ostringstream line;
					line << "[contract-audit-continuation] started_tick=" << simTick
						 << " requested_tick=" << ScenarioRunner::GetArgs().contractAuditContinueThrough;
					System::PrintDiagnosticLine(line.str());
				}
				}
			}
			if (!s_snapshotRoundtripSelfTestName.empty() && simTick > 0) {
				bool readerPassed = true;
				for (const std::string ending: {"\n", "\r\n", " \t\n", " // empty\n", " /* empty */\n"}) {
					for (bool useValueReader: {false, true}) {
						Reader reader(std::make_unique<std::stringstream>("Empty =" + ending + "Next = present\n"), "reader-empty-selftest.ini");
						const bool first = reader.ReadPropName() == "Empty";
						std::string value;
						if (useValueReader) value = reader.ReadPropValue();
						else reader >> value;
						const bool empty = value.empty();
						const bool next = reader.NextProperty() && reader.ReadPropName() == "Next";
						reader >> value;
						readerPassed = first && empty && next && value == "present" && readerPassed;
					}
				}
				{
					std::ostringstream line;
					line << "[reader-empty-selftest] " << (readerPassed ? "PASS" : "FAIL") << " cases=10";
					System::PrintDiagnosticLine(line.str());
				}
				const std::string output = s_snapshotRoundtripSelfTestName + "_roundtrip";
				g_AudioMan.SetCheckpointTraceEnabled(true);
				bool loaded = false, saved = false, audioUnchanged = true;
				{
					// The ordinary load/save path remains intact. This test holds the
					// independent mixer clock at one observation boundary.
					std::unique_ptr<AudioCheckpoint::MixerLock> audioBoundary;
					if (s_snapshotRoundtripLockAudio) audioBoundary = std::make_unique<AudioCheckpoint::MixerLock>(g_AudioMan.IsAudioEnabled() ? g_AudioMan.GetAudioSystem() : nullptr);
					g_AudioMan.TraceCheckpointBoundary("roundtrip-before-load");
					loaded = readerPassed && g_ActivityMan.LoadAndLaunchGame(s_snapshotRoundtripSelfTestName);
					g_AudioMan.TraceCheckpointBoundary("roundtrip-load-returned");
					const bool perturbed = !s_snapshotRoundtripPerturbAudio || (loaded && g_AudioMan.PerturbCheckpointCursorForSelfTest());
					const std::string audioBeforeSave = loaded && s_snapshotRoundtripLockAudio ? g_AudioMan.SaveCheckpoint() : "";
					saved = loaded && perturbed && g_ActivityMan.SaveCurrentGame(output) && g_ActivityMan.WaitForSaveGameTask();
					if (saved && s_snapshotRoundtripLockAudio) audioUnchanged = g_AudioMan.SaveCheckpoint() == audioBeforeSave;
					g_AudioMan.TraceCheckpointBoundary("roundtrip-save-returned");
					const bool audioChecked = s_snapshotRoundtripLockAudio && saved;
					{
						std::ostringstream line;
						line << "[snapshot-audio-boundary] locked=" << s_snapshotRoundtripLockAudio << " checked=" << audioChecked << " unchanged=" << (audioChecked ? std::to_string(audioUnchanged) : "unchecked") << " perturbed=" << s_snapshotRoundtripPerturbAudio;
						System::PrintDiagnosticLine(line.str());
					}
				}
				g_AudioMan.TraceCheckpointBoundary("roundtrip-mixer-released");
				const bool playbackContinued = !s_snapshotRoundtripCheckPlayback || (saved && g_AudioMan.RunCheckpointPlaybackContinuationSelfTest());
				g_AudioMan.SetCheckpointTraceEnabled(false);
				s_snapshotRoundtripSelfTestPassed = loaded && saved && audioUnchanged && playbackContinued;
				{
					std::ostringstream line;
					line << "[snapshot-roundtrip] " << (s_snapshotRoundtripSelfTestPassed ? "PASS" : "FAIL") << " save=" << output;
					System::PrintDiagnosticLine(line.str());
				}
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}
			const uint64_t restoreCheckTick = std::max(s_netAutosaveRestoreTick, s_netAutosaveRestoreAtTick);
			if (restoreCheckTick > 0 && simTick >= restoreCheckTick &&
			    (s_netAutosaveRestoreTick > 0 || !s_netAutosaveRestoreWhich.empty())) {
				(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, 0);
				s_netAutosaveRestorePassed = RunAutosaveRestoreCheck(s_netAutosaveRestoreTick, s_netAutosaveRestoreWhich);
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				g_MetricsCollector.Record("final_tick", static_cast<double>(simTick));
				g_MetricsCollector.SetResult(s_netAutosaveRestorePassed);
				break;
			}
			if (!s_loadSelfTestName.empty() && simTick > 0) {
				s_loadSelfTestPassed = g_ActivityMan.RunLoadSelfTest(s_loadSelfTestName, s_loadSelfTestExpected);
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				// A load leaves the saved game running, so the starting scenario never reaches its
				// own verdict. The selftest owns this run's result.
				g_MetricsCollector.Record("final_tick", static_cast<double>(simTick));
				g_MetricsCollector.SetResult(s_loadSelfTestPassed);
				break;
			}
			if (s_saveIoSelfTest && simTick > 0 && s_saveIoSelfTestFirstTick == 0) s_saveIoSelfTestFirstTick = simTick;
			if (s_saveIoSelfTest && simTick > 0 && simTick >= s_saveIoSelfTestFirstTick + s_saveIoSelfTestAfter) {
				if (s_saveIoSelfTestAfter > 0) {
					System::PrintDiagnosticLine("[save-selftest] played " + std::to_string(simTick - s_saveIoSelfTestFirstTick) + " ticks from " + std::to_string(s_saveIoSelfTestFirstTick));
				}
				if (s_saveMenuSelfTest) s_saveMenuSelfTestPassed = SaveLoadMenuGUI::RunSaveSelfTest(s_saveIoSelfTestName, s_saveIoSelfTestQueued);
				else s_saveIoSelfTestQueued = g_ActivityMan.SaveCurrentGame(s_saveIoSelfTestName);
				{
					std::ostringstream line;
					line << "[save-selftest] queued=" << s_saveIoSelfTestQueued << " pending=" << g_ActivityMan.IsCurrentlySaving();
					System::PrintDiagnosticLine(line.str());
				}
				System::SetQuit(true);
				g_ActivityMan.EndActivity();
				break;
			}

			// Scenario direct-launch: quit when the activity reaches OVER or the -max-ticks cap hits,
			// instead of bouncing to the menu. The cap counts global sim ticks, so the trace length
			// is fixed even if the activity never sets OVER.
			if (ScenarioRunner::IsActive()) {
				static uint64_t s_scenarioStartTick = UINT64_MAX;
				const uint64_t nowTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				if (s_scenarioStartTick == UINT64_MAX) {
					s_scenarioStartTick = nowTick;
				}
				const uint64_t elapsedTicks = nowTick - s_scenarioStartTick;
				const uint64_t scenarioTickCap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 1800;
				const uint64_t tickCap = NetGameplayRequested() && s_netLockstepTicks > 0 ? s_netLockstepTicks : scenarioTickCap;
				const Activity* scenarioActivity = g_ActivityMan.GetActivity();
				// A match keeps simulating its remaining ticks after the round is decided; a scenario
				// run that has to be read beside one needs the same window.
				const bool activityDecided = scenarioActivity && scenarioActivity->IsOver() && !ScenarioRunner::GetArgs().scenarioRunPastEnd;
				if (activityDecided || elapsedTicks >= tickCap) {
					// Finalize so the scenario's Lua OnEnd grades the run even when the CLI tick cap
					// stops it before the scenario's own max-ticks (idempotent if it already ended).
					g_ActivityMan.EndActivity();
					System::SetQuit(true);
					break;
				}
			}

			// Playback honours -max-ticks as a bounded run: distinct from the recording's own end.
			if (!s_netReplayInPath.empty() && ScenarioRunner::GetArgs().maxTicks > 0 && !ScenarioRunner::HasControllerReplayError() &&
			    static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) >= static_cast<uint64_t>(ScenarioRunner::GetArgs().maxTicks)) {
				s_netReplayTicks = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				ScenarioRunner::SetLockstepReplayOutcome(ScenarioRunner::LockstepReplayOutcome::TickCap);
				g_ActivityMan.EndActivity();
				System::SetQuit(true);
				break;
			}
			// Stop after the last requested trace tick has completed.
			if (!ScenarioRunner::IsActive() && !s_netMatchServiceE2E && s_recordTickHashes && g_NetMatchService.WasEverStarted()) {
				const uint64_t cap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 600;
				if (s_menuTraceCoverage.ReachedCap(g_MetricsCollector.GetTickHashCount(), cap)) {
					{
						std::ostringstream line;
						line << "[menu-mp] trace complete at tick " << g_TimerMan.GetSimUpdateCount();
						System::PrintDiagnosticLine(line.str());
					}
					// Hand over what we still owe BEFORE the goodbye, so a client one input-delay
					// behind can finish its own last tick instead of losing the round to our exit.
					FrameRecorder::Instance().RecordEvent("capped stop");
					(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, 0);
					g_NetMatchService.Complete("menu mp trace complete");
					(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, c_CappedStopLingerMs);
					g_ActivityMan.EndActivity();
					System::SetQuit(true);
					break;
				}
			}

			// Interactive menu-launched match: end it when the activity is over. The win condition and
			// this tick window are sim-state, so both peers finish on the same tick without a timeout.
			if (!ScenarioRunner::IsActive() && !s_netMatchServiceE2E && !s_recordTickHashes && g_NetMatchService.GetState() == NetMatchServiceState::Running) {
				static uint64_t s_matchOverTick = UINT64_MAX;
				const Activity* matchActivity = g_ActivityMan.GetActivity();
				// -net-match-ticks ends a menu-launched match at a played frame every peer reaches,
				// so an unattended run finishes one the same way a win condition does.
				const bool cappedEnd = s_netLockstepTicks > 0 && ScenarioRunner::HasLockstepCoordinator() &&
				                       LockstepPlayedFrame() >= s_netLockstepTicks;
				if ((matchActivity && matchActivity->IsOver()) || cappedEnd) {
					const uint64_t nowTick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
					if (s_matchOverTick == UINT64_MAX) {
						s_matchOverTick = nowTick;
					}
					// The grace lets a win play out on screen; a capped end has nothing to show.
					const uint64_t graceTicks = cappedEnd ? 0 : static_cast<uint64_t>(5.0f / g_TimerMan.GetDeltaTimeSecs());
					if (nowTick - s_matchOverTick >= graceTicks) {
						s_matchOverTick = UINT64_MAX;
						const std::string result = BuildNetMatchResultText();
						g_ConsoleMan.PrintString("NETWORK: Match complete: " + result);
						g_NetMatchService.FinishMatch(result);
						g_ActivityMan.EndActivity();
						g_ActivityMan.SetInActivity(false);
						returnToMenuAfterNetworkEnd = true;
						break;
					}
				} else {
					s_matchOverTick = UINT64_MAX;
				}
			}

			if (s_netMatchServiceE2E) {
				Activity* activity = g_ActivityMan.GetActivity();
				if (!activity) {
					s_netMatchServiceE2EError = "activity ended before e2e tick cap";
					s_netMatchServiceE2EExitCode = 1;
					System::SetQuit(true);
					break;
				}
				const Activity::ActivityState activityState = activity->GetActivityState();
				if (activityState == Activity::Editing) {
					s_netMatchServiceE2EEnteredEditor = true;
					// A lockstep match's setup editor is synchronized: seats commit their placements over the
					// wire and every peer starts on the same frame. Only an unsynchronized one is an error.
					std::string editorError;
					// The cap is a watchdog on a stuck editor, not a limit on the phase: a script that waits
					// on another peer's probe signal ends its wait at a rendezvous, and the count starts there.
					const uint64_t rendezvous = NetModerationGUIProbe::RendezvousCount();
					if (rendezvous != s_netMatchE2ERendezvousSeen) {
						s_netMatchE2ERendezvousSeen = rendezvous;
						s_netMatchE2EEditorTicks = 0;
					}
					// A resync takes the coordinator down and rebuilds it around the host's snapshot, and the
					// editor holds while that happens. An editor nobody synchronizes never gets one back.
					if (!ScenarioRunner::IsLockstepControllerSyncActive() && !NetMatchResyncRebuilding()) {
						editorError = "activity entered unsynchronized setup editor";
					} else if (++s_netMatchE2EEditorTicks > c_NetMatchE2EEditorTickCap) {
						editorError = "setup editor did not finish within " + std::to_string(c_NetMatchE2EEditorTickCap) + " ticks";
					}
					if (!editorError.empty()) {
						s_netMatchServiceE2EError = editorError;
						s_netMatchServiceE2EExitCode = 1;
						g_NetMatchService.ReportRuntimeError(s_netMatchServiceE2EError);
						g_ActivityMan.EndActivity();
						System::SetQuit(true);
						break;
					}
				}
				// E2E rematch ride-through: match 1 ended, so finish it, reconvene the live session in the
				// lobby, and relaunch — round 2 is policed by the live desync exchange like any match.
				if (IsE2ERematchReady() && CrossWinSurfaceReady() && (!s_crossRematches || s_crossBudget <= s_netLockstepTicks)) {
					(void)RunNetMatchE2ERematch(BuildNetMatchResultText(), false);
					break;
				}
				// A legitimate game-over may end the activity mid-run; the sim keeps ticking to the cap so
				// the trace stays bounded. An end in the first 100 ticks with no winner still means a broken setup.
				const uint64_t earlyOverTick = ScenarioRunner::HasLockstepCoordinator()
					                               ? ScenarioRunner::GetLockstepAppliedFrame()
					                               : s_netMatchE2ETicks.Total();
				if (activityState == Activity::HasError || (activityState == Activity::Over && !s_crossRematches && s_netMatchE2ETicks.EarlyOverIsSetupFailure(earlyOverTick, NetMatchActivityHasWinner(activity)))) {
					s_netMatchServiceE2EError = std::string("activity ended in state ") + ActivityStateName(activityState);
					s_netMatchServiceE2EExitCode = 1;
					g_NetMatchService.ReportRuntimeError(s_netMatchServiceE2EError);
					System::SetQuit(true);
					break;
				}
				if (activityState == Activity::Running || activityState == Activity::Over) {
					// In-match census; the report runs after EndActivity, which releases actors.
					s_netMatchE2EActorCensus = g_MovableMan.GetActorCount();
					s_netMatchE2EActorCensusPeak = std::max(s_netMatchE2EActorCensusPeak, s_netMatchE2EActorCensus);
					const bool unlimitedWorld = (s_netWorldDaemon || s_netPersistentWorld) && !s_netMatchTicksExplicit;
					const uint64_t roundTicks = s_netLockstepTicks > 0 ? s_netLockstepTicks : 600;
					// A peer counts its cap from its own first tick, a world joiner too.
					const uint64_t completedTicks = s_crossRematches ? s_crossBudget : s_netMatchE2ETicks.Total();
					// The round never ends with a seat mid-admission and unsampled: the host waits for a seat still coming in,
					// then for the first full-state sample after it, so the returner shares one with the round (bounded).
					uint64_t tickCap = roundTicks;
					if (s_netFullStateEvery > 0 && ScenarioRunner::HasLockstepCoordinator() && activityState == Activity::Over) {
						// A finished activity takes no capture, so it owes no sample and the round ends at its own cap.
						static uint64_t s_activityOverLoggedRound = 0;
						if (s_activityOverLoggedRound != ScenarioRunner::GetLockstepRoundId()) {
							s_activityOverLoggedRound = ScenarioRunner::GetLockstepRoundId();
							System::PrintDiagnosticLine("[net-match-service-e2e] activity over at frame " + std::to_string(ScenarioRunner::GetLockstepAppliedFrame()) +
							                            ": no full-state sample follows");
						}
						s_netMatchE2EOwedSampleFrame = 0;
					} else if (s_netFullStateEvery > 0 && ScenarioRunner::HasLockstepCoordinator()) {
						const uint64_t applied = ScenarioRunner::GetLockstepAppliedFrame();
						// The host sees the seat coming in; the seat sees its own catch-up. Both end on the same frame before its activation.
						const bool comingIn = g_NetMatchService.IsHost() ? g_NetMatchService.SeatMidAdmission(applied) : ScenarioRunner::WorldCatchUpActive();
						if (completedTicks + s_netFullStateEvery >= roundTicks && comingIn)
							s_netMatchE2EOwedSampleFrame = (applied / s_netFullStateEvery + 1) * s_netFullStateEvery;
						if (s_netMatchE2EOwedSampleFrame >= applied)
							tickCap = std::min(roundTicks + c_NetMatchE2EAdmissionWaitTicks, std::max(roundTicks, completedTicks + (s_netMatchE2EOwedSampleFrame - applied) + 1));
						static uint64_t s_admissionWaitLogged = 0;
						if (tickCap > roundTicks && completedTicks >= roundTicks && s_admissionWaitLogged != s_netMatchE2EOwedSampleFrame) {
							s_admissionWaitLogged = s_netMatchE2EOwedSampleFrame;
							System::PrintDiagnosticLine("[net-match-service-e2e] cap waits for a seat coming in: sample owed at " + std::to_string(s_netMatchE2EOwedSampleFrame) +
							                            " (applied " + std::to_string(applied) + ")");
						}
					}
					// Every peer stops at the cap, so the round knows the last frame anyone will feed.
					if (!unlimitedWorld && completedTicks <= tickCap && ScenarioRunner::HasLockstepCoordinator())
						ScenarioRunner::SetLockstepFinalFrame(ScenarioRunner::GetLockstepAppliedFrame() + (tickCap + 1 - completedTicks));
					if (!unlimitedWorld && completedTicks > tickCap) {
						// A capped stop is per-peer wall clock: a peer settled behind a lagged link still
						// owes itself our in-flight tail, so hand over the forwards we hold and hold the
						// socket open before quitting drops it.
						if (!s_netMatchE2ECompletedMs) {
							// The screen holds its last picture while the harness drains, lingers and writes its records.
							FrameRecorder::Instance().RecordEvent("capped stop");
							(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, 0);
							g_NetMatchService.Complete("e2e complete");
							s_netMatchE2ECompletedMs = SteadyMilliseconds();
						}
						// A player reading the pause menu when the round ends keeps seeing it; a probe that is
						// still mid-script gets that same window before the harness ends the activity.
						if (ProbeHoldsE2eEnd(s_netMatchE2ECompletedMs)) {
							// Leave the update loop for this frame: the drawn pause menu is what the probe reads.
							break;
						}
						(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, c_CappedStopLingerMs);
						g_ActivityMan.EndActivity();
						System::SetQuit(true);
						break;
					}
				}
			}

			if (!g_ActivityMan.IsInActivity()) {
				g_TimerMan.PauseSim(true);

				if (!g_ActivityMan.ActivitySetToRestart()) {
					// Leaving a running net match: a clean leave lets N-peer survivors keep playing and,
					// with nobody left, ends their match at once - unlike a drop, which holds the seat
					// open for its reclaim window. The §7 exchange runs before the link goes down.
					const bool networkMatchLeft = g_NetMatchService.GetState() == NetMatchServiceState::Running;
					if (networkMatchLeft) {
						g_ConsoleMan.PrintString("NETWORK: Match left");
						g_NetMatchService.LeaveMatch("Match left");
					}
					if (s_netMatchServiceE2E && !s_menuScriptPath.empty() && !s_menuScriptComplete && !s_menuScriptFailed) {
						s_menuScriptHoldE2ePause = true;
						g_MenuMan.HandleTransitionIntoMenuLoop(networkMatchLeft);
						RunMenuLoop();
						s_menuScriptHoldE2ePause = false;
						if (!s_menuScriptComplete && !System::IsSetToQuit()) continue;
					}
					// The e2e has no menu to return to; a leaver's run ends here, once a probe still mid-script has drawn its last steps.
					if (s_netMatchServiceE2E) {
						if (NetModerationGUIProbe::Running()) {
							if (!s_netMatchE2ELeftMs) s_netMatchE2ELeftMs = SteadyMilliseconds();
							break;
						}
						System::SetQuit(true);
						break;
					}
					g_MenuMan.HandleTransitionIntoMenuLoop(networkMatchLeft);
					RunMenuLoop();
				}
			}
			if (s_rbProbeMemoryRestorePending) {
				s_rbProbeMemoryRestorePending = false;
				const auto restoreStart = std::chrono::steady_clock::now();
				if (!g_MovableMan.SetAsideWorld(s_rbProbeOriginals)) {
					{
						std::ostringstream line;
						line << "[rbprobe] FAIL: world set-aside refused";
						System::PrintDiagnosticLine(line.str());
					}
					System::SetQuit(true);
					break;
				}
				const auto worldRestoreStart = std::chrono::steady_clock::now();
				if (!g_MovableMan.RestoreWorld(s_rbProbeWorld)) {
					{
						std::ostringstream line;
						line << "[rbprobe] FAIL: world restore refused";
						System::PrintDiagnosticLine(line.str());
					}
					g_MovableMan.ReinstateWorld(s_rbProbeOriginals);
					System::SetQuit(true);
					break;
				}
				const double worldRestoreMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - worldRestoreStart).count();
				std::string rewindError;
				if (ScenarioRunner::IsLockstepReplayPlayback() &&
				    !ScenarioRunner::RewindReplayForProbe(static_cast<uint64_t>(s_rbProbeSimCount) + 1, &rewindError)) {
					{
						std::ostringstream line;
						line << "[rbprobe] FAIL: replay rewind refused: " << rewindError;
						System::PrintDiagnosticLine(line.str());
					}
					System::SetQuit(true);
					break;
				}
				const double restoreMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - restoreStart).count();
				{
					std::ostringstream line;
					line << "[rbprobe] restore_ms=" << restoreMs << " world_ms=" << worldRestoreMs;
					System::PrintDiagnosticLine(line.str());
				}
				CheckRestoredDeepState();
				CheckRestoredScriptGraphs();
				DumpTerrainNow("rb_res");
				s_rbProbePhase = 3;
				{
					std::ostringstream line;
					line << "[rbprobe] restored in memory and rewound to tick " << s_rbProbeSimCount;
					System::PrintDiagnosticLine(line.str());
				}
			}
			if (g_ActivityMan.ActivitySetToRestart() || s_rbProbeLaunchRestorePending) {
				g_LoadingScreen.DrawLoadingSplash();
				g_WindowMan.UploadFrame();
				const bool restarted = s_rbProbeLaunchRestorePending ? g_ActivityMan.LoadAndLaunchGame(RollbackProbeSaveName()) : g_ActivityMan.RestartActivity();
				if (s_rbProbeLaunchRestorePending) {
					s_rbProbeLaunchRestorePending = false;
					g_ActivityMan.RemoveSavedGame(RollbackProbeSaveName());
				}
				if (!restarted) {
					if (s_rbProbePhase == 2) {
						++s_rbProbeFailCount;
						s_rbProbeFirstFailure = "the saved activity could not restart";
						{
							std::ostringstream line;
							line << "[rbprobe] FAIL: " << s_rbProbeFirstFailure;
							System::PrintDiagnosticLine(line.str());
						}
						System::SetQuit(true);
					}
					break;
				}
				if (s_rbProbePhase == 2) {
					std::string rewindError;
					if (ScenarioRunner::IsLockstepReplayPlayback() &&
					    !ScenarioRunner::RewindReplayForProbe(static_cast<uint64_t>(s_rbProbeSimCount) + 1, &rewindError)) {
						{
							std::ostringstream line;
							line << "[rbprobe] FAIL: replay rewind refused: " << rewindError;
							System::PrintDiagnosticLine(line.str());
						}
						System::SetQuit(true);
						break;
					}
					CheckRestoredDeepState();
					CheckRestoredScriptGraphs();
					DumpTerrainNow("rb_res");
					s_rbProbePhase = 3;
					{
						std::ostringstream line;
						line << "[rbprobe] restored and rewound to tick " << s_rbProbeSimCount;
						System::PrintDiagnosticLine(line.str());
					}
				}
			}
			if (g_ActivityMan.ActivitySetToResume()) {
				g_ActivityMan.ResumeActivity();
				g_PerformanceMan.ResetSimUpdateTimer();
				updateStartTime = g_TimerMan.GetAbsoluteTime();
			}
			// A capture ends only the update batch; the next simulation tick keeps its time debt.
			if (ScenarioRunner::GetArgs().freeRunSim || NetMatchScreenshotDue()) {
				break;
			}
		}
		// The frame's budget ran out with ticks still due: the round is behind its schedule, not waiting on a peer.
		const bool ticksOwed = pacedRound && g_TimerMan.SimFrameBudgetSpent();
		g_TimerMan.BeginSimFrame(0);

		if (returnToMenuAfterNetworkEnd && !System::IsSetToQuit()) {
			g_TimerMan.PauseSim(true);
			if (s_netReplayReturnPending) {
				// CC_FAULT_INJECT=queued_restart_at_replay_end presses the rematch on the frame the replay's
				// end clears: the guard has to keep the queue, and the run goes on the way it would have.
				const bool injectedRestart = FaultInjected("queued_restart_at_replay_end");
				const bool restartBeforeInjection = g_ActivityMan.ActivitySetToRestart();
				if (injectedRestart) g_ActivityMan.SetRestartActivity(true);
				g_ActivityMan.ClearEndedReplayActivity();
				if (injectedRestart) {
					System::PrintDiagnosticLine(std::string("[replay-end] injected queued restart kept=") + (g_ActivityMan.ActivitySetToRestart() ? "1" : "0"));
					// The guard held, so the clear did nothing: put the queue back and clear for real, or the
					// armed run would carry the ended replay activity into the menu loop.
					g_ActivityMan.SetRestartActivity(restartBeforeInjection);
					g_ActivityMan.ClearEndedReplayActivity();
				}
			}
			if (!g_ActivityMan.ActivitySetToRestart()) {
				g_MenuMan.HandleTransitionIntoMenuLoop();
				RunMenuLoop();
			}
			if (!System::IsSetToQuit()) {
				g_TimerMan.PauseSim(false);
				s_pacePrevActive = false;
				if (g_ActivityMan.ActivitySetToRestart()) {
					s_netReplayReturnPending = false;
					s_netReplayReturnStatus.clear();
					g_LoadingScreen.DrawLoadingSplash();
					g_WindowMan.UploadFrame();
					if (!g_ActivityMan.RestartActivity() && !HandleFailedActivityLaunch()) return;
				}
			}
			continue;
		}

		updateEndAndDrawStartTime = g_TimerMan.GetAbsoluteTime();
		updateTotalTime = updateEndAndDrawStartTime - updateStartTime;
		drawStartTime = updateEndAndDrawStartTime;

		// Frame rendering must not advance the sim RNG stream or feed the MOID grid — its cadence is
		// host frame-rate dependent, so redirect cosmetic draws to the render RNG and suspend
		// MOID-grid registration for the frame.
		// Presentation, input and the menus are this machine's own; the round goes on through them.
		std::optional<NetLockstepPlane::Window> drawWindow;
		drawWindow.emplace("frame draw");
		FrameMan::FeelBeforePreview();
		// Behind its schedule, a paced round skips this frame's preview and draw so the owed ticks run first, down to the floor.
		const bool shedFrame = ticksOwed && !NetMatchScreenshotDue() &&
		                       g_TimerMan.GetAbsoluteTime() - s_paceLastPresentUs < static_cast<long long>(c_FloorFrameMs * 1000.0);
		const long long previewStartTime = g_TimerMan.GetAbsoluteTime();
		if (!shedFrame) LocalPrediction::RunPreview();
		const long long interfaceStartTime = g_TimerMan.GetAbsoluteTime();

		{
			RandomGenerator* prevSimRNG = t_simRNGOverride;
			t_simRNGOverride = &g_RenderRNG;
			g_SceneMan.SetRenderDrawContext(true);
			g_UInputMan.Update();
			{
				NetLockstepPlane::Gap plane("network UI update");
				g_ActivityMan.RenderUpdate();
				g_MenuMan.UpdateNetworkUI();
			}
			g_MenuMan.UpdateLocalPauseMenu();
			g_UInputMan.EndFrame();
			g_SceneMan.SetRenderDrawContext(false);
			t_simRNGOverride = prevSimRNG;
		}
		const long long frameDrawStartTime = g_TimerMan.GetAbsoluteTime();
		if (shedFrame) {
			++s_paceFramesShed;
		} else if (!freeRunLockstep || NetMatchScreenshotDue()) {
			const uint64_t framesBefore = s_paceFramesDrawn;
			DrawFrameWithPreviews();
			if (s_paceFramesDrawn != framesBefore) s_paceLastPresentUs = g_TimerMan.GetAbsoluteTime();
		}
		drawWindow.reset();

		drawTotalTime = g_TimerMan.GetAbsoluteTime() - drawStartTime;
		g_PerformanceMan.UpdateMSPF(updateTotalTime, drawTotalTime);
		// A frame that kept the simulation away half a second names where the time went.
		if (ScenarioRunner::IsLockstepControllerSyncActive() && updateTotalTime + drawTotalTime >= 500000)
			System::PrintDiagnosticLine("[main-loop] slow frame update_ms=" + std::to_string(updateTotalTime / 1000) + " draw_ms=" + std::to_string(drawTotalTime / 1000) + " tick=" + std::to_string(g_TimerMan.GetSimUpdateCount()));

		// Both ends of the iteration must be in a RUNNING match, or the teardown drain and
		// menu-transition iterations poison the averages.
		if (paceActiveAtIterStart && ScenarioRunner::IsLockstepControllerSyncActive()) {
			++s_paceIterations;
			s_paceUpdateUs += updateTotalTime;
			s_paceDrawUs += drawTotalTime;
			// A run that ends while this machine waits on a frame past its last tick ran no round in that wait.
			s_paceTrailingUs = s_paceSimTicks != paceTicksAtIterStart ? 0 : s_paceTrailingUs + updateTotalTime + drawTotalTime;
			s_pacePreviewUs += interfaceStartTime - previewStartTime;
			s_paceInterfaceUs += frameDrawStartTime - interfaceStartTime;
			s_paceMaxDrawUs = std::max(s_paceMaxDrawUs, drawTotalTime);
		}
		FrameMan::FeelEndIteration(s_paceSimTicks, s_paceSimUs, s_paceUpdateUs, s_paceDrawUs);
	}
	FrameMan::FeelFinish();
}

/// <summary>
/// Self-invoking lambda that installs exception handlers before Main is executed.
/// </summary>
static const bool RTESetExceptionHandlers = []() {
	RTEError::SetExceptionHandlers();
	return true;
}();

bool NetSessionCliRequested() {
	return (s_netHost || !s_netJoinAddress.empty()) && !NetGameplayRequested() && !s_netMatchServiceE2E;
}

bool WriteNetSessionReport(const NetSession& session, const std::string& path, std::string* error) {
	if (path.empty()) {
		return true;
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) *error = "could not open report path '" + path + "'";
		return false;
	}
	out << session.BuildReportJson() << '\n';
	if (!out) {
		if (error) *error = "could not write report path '" + path + "'";
		return false;
	}
	return true;
}

NetSessionConfig BuildNetSessionCliConfig(const NetIdentityManifest& manifest, bool host) {
	NetSessionConfig config;
	config.localIdentity = manifest;
	config.displayName = host ? "Host" : "Client";
	config.port = s_netPort;
	config.sessionId = 0x5354414745325032ULL;
	config.localNonce = host ? 0x535441474532484FULL : 0x535441474532434CULL;
	config.maxPeers = 1;
	config.heartbeatIntervalMs = 50;
	config.timeoutMs = 5000;
	config.rejectUserdataModules = !s_netAllowUserdata && !s_netMatch;
	return config;
}

NetMatchConfig BuildNetMatchCliConfig(bool useLobbyProtocol) {
	NetMatchConfig config = NetMatchConfigUtil::MakeDefault(0x5354414745325032ULL);
	config.activityPreset = ScenarioRunner::ResolvePresetName(ScenarioRunner::GetArgs().scenario);
	config.inputDelayFrames = s_netLockstepInputDelay;
	config.modePreset = useLobbyProtocol ? "PvP" : "P3";
	if (useLobbyProtocol) {
		NetActorOwnershipPolicy policy;
		if (NetMatchConfigUtil::ParseOwnershipPolicy(s_netMatchOwnershipPolicy, policy)) {
			config.ownershipPolicy = policy;
		}
	} else {
		config.ownershipPolicy = NetActorOwnershipPolicy::UniqueIdModPeerCount;
	}
	return config;
}

int RunNetSessionCli() {
	if (s_netHost && !s_netJoinAddress.empty()) {
		{
			std::ostringstream line;
			line << "[net-session] choose either -net-host or -net-join, not both";
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}

	g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);

	NetIdentityManifest manifest;
	NetIdentityBuildOptions identityOptions;
	identityOptions.buildId = "stage2-p2d-local";
	identityOptions.sessionRulesTag = "peer-session-plane-v1";
	std::string error;
	if (!NetIdentity::BuildCurrentManifest(manifest, &error, identityOptions)) {
		{
			std::ostringstream line;
			line << "[net-session] identity build failed: " << error;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}

	GnsTransport transport;
	NetSession session;
	NetSessionConfig config = BuildNetSessionCliConfig(manifest, s_netHost);
	const bool started = s_netHost
		? session.StartHost(transport, std::move(config), &error)
		: session.StartClient(transport, s_netJoinAddress, std::move(config), &error);

	if (!started) {
		{
			std::ostringstream line;
			line << "[net-session] start failed: " << error;
			System::PrintDiagnosticErrorLine(line.str());
		}
		std::string reportError;
		if (!WriteNetSessionReport(session, s_netSessionReportPath, &reportError)) {
			{
				std::ostringstream line;
				line << "[net-session] report failed: " << reportError;
				System::PrintDiagnosticErrorLine(line.str());
			}
		}
		return 1;
	}

	{
		std::ostringstream line;
		line << "[net-session] " << (s_netHost ? "hosting" : "joining")
		     << " port=" << s_netPort
		     << " gns_compiled=" << (GnsTransport::IsCompiledIn() ? "true" : "false")
		     << " allow_userdata=" << (s_netAllowUserdata ? "true" : "false");
		System::PrintDiagnosticLine(line.str());
	}

	const auto startTime = std::chrono::steady_clock::now();
	constexpr uint64_t c_MaxRunMs = 15000;
	constexpr uint64_t c_ReadySettleMs = 250;
	bool sawReady = false;

	while (true) {
		const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - startTime).count());
		session.Tick(nowMs);

		if (session.IsRejected()) {
			if (s_netHost) {
				std::this_thread::sleep_for(std::chrono::milliseconds(c_ReadySettleMs));
			}
			break;
		}
		if (session.IsFailed() || session.IsClosed()) {
			break;
		}
		if (session.IsReady()) {
			if (!sawReady) {
				sawReady = true;
				{
					std::ostringstream line;
					line << "[net-session] ready";
					System::PrintDiagnosticLine(line.str());
				}
			}
			if (s_netExitAfterReady && !s_netHost) {
				std::this_thread::sleep_for(std::chrono::milliseconds(c_ReadySettleMs));
			}
			break;
		}
		if (nowMs > c_MaxRunMs) {
			{
				std::ostringstream line;
				line << "[net-session] timed out waiting for ready";
				System::PrintDiagnosticErrorLine(line.str());
			}
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	std::string reportError;
	if (!WriteNetSessionReport(session, s_netSessionReportPath, &reportError)) {
		{
			std::ostringstream line;
			line << "[net-session] report failed: " << reportError;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}

	const bool passed = session.IsReady();
	{
		std::ostringstream line;
		line << "[net-session] final_state=" << NetSession::StateName(session.GetState())
		     << " accepted=" << (passed ? "true" : "false")
		     << " report=" << (s_netSessionReportPath.empty() ? "<none>" : s_netSessionReportPath);
		System::PrintDiagnosticLine(line.str());
	}
	return passed ? 0 : 1;
}

std::string JsonEscape(const std::string& value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (char c : value) {
		switch (c) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default: escaped += c; break;
		}
	}
	return escaped;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string* error) {
	if (path.empty()) {
		return true;
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) *error = "could not open report path '" + path + "'";
		return false;
	}
	out << text << '\n';
	if (!out) {
		if (error) *error = "could not write report path '" + path + "'";
		return false;
	}
	return true;
}

bool PrepareNetLockstepScenario(GnsTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, NetMatchRunner& runner, std::string* error) {
	if (!NetGameplayRequested()) {
		return true;
	}
	if (!ScenarioRunner::IsActive()) {
		if (error) *error = "network gameplay requires -scenario";
		return false;
	}
	if (s_netLockstep && s_netMatch) {
		if (error) *error = "choose either -net-lockstep or -net-match, not both";
		return false;
	}
	if (s_netHost == !s_netJoinAddress.empty()) {
		if (error) *error = "network gameplay requires exactly one of -net-host or -net-join <address>";
		return false;
	}
	if (s_netLockstepInputDelay == 0) {
		if (error) *error = "network gameplay requires at least 1 frame of input delay before its committed simulation tick";
		return false;
	}

	g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);

	NetIdentityManifest manifest;
	NetIdentityBuildOptions identityOptions;
	identityOptions.buildId = "stage2-p2d-local";
	identityOptions.sessionRulesTag = "peer-session-plane-v1";
	if (!NetIdentity::BuildCurrentManifest(manifest, error, identityOptions)) {
		return false;
	}

	NetMatchRunnerConfig runnerConfig;
	runnerConfig.host = s_netHost;
	runnerConfig.joinAddress = s_netJoinAddress;
	runnerConfig.sessionConfig = BuildNetSessionCliConfig(manifest, s_netHost);
	runnerConfig.matchConfig = BuildNetMatchCliConfig(s_netMatch);
	runnerConfig.useLobbyProtocol = s_netMatch;
	runnerConfig.requirePublishedStart = true;
	runnerConfig.startFrame = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) + 1U;
	runnerConfig.scenario = ScenarioRunner::ResolvePresetName(ScenarioRunner::GetArgs().scenario);
	runnerConfig.lockstepWaitMs = 5000;

	const char* tag = s_netMatch ? "[net-match]" : "[net-lockstep]";
	{
		std::ostringstream line;
		line << tag << " " << (s_netHost ? "hosting" : "joining")
		     << " port=" << s_netPort
		     << " gns_compiled=" << (GnsTransport::IsCompiledIn() ? "true" : "false")
		     << " allow_userdata=" << (s_netAllowUserdata ? "true" : "false")
		     << " lobby=" << (s_netMatch ? "true" : "false");
		System::PrintDiagnosticLine(line.str());
	}
	if (!runner.Start(transport, session, coordinator, runnerConfig, error)) {
		return false;
	}
	std::string reportError;
	if (!WriteNetSessionReport(session, s_netSessionReportPath, &reportError)) {
		std::ostringstream line;
		line << tag << " session report failed: " << reportError;
		System::PrintDiagnosticErrorLine(line.str());
	}

	ScenarioRunner::SetLockstepCoordinator(&coordinator);
	{
		std::ostringstream line;
		line << tag << " running local_peer=" << static_cast<int>(coordinator.GetConfig().localPeerId)
		     << " remote_peer=" << static_cast<int>(coordinator.GetConfig().remotePeerId)
		     << " start_frame=" << coordinator.GetConfig().startFrame
		     << " match_config_hash=" << NetIdentity::HashHex(runner.GetMatchConfigHash());
		System::PrintDiagnosticLine(line.str());
	}
	return true;
}

std::string BuildControllerBoundaryJson();
std::string BuildDesyncCheckJson();

std::string BuildNetLockstepReportJson(const NetSession& session, const NetLockstepCoordinator& coordinator, const NetMatchRunner& runner, int scenarioExitCode, const std::string& setupError) {
	const MetricsCollector::AggregatedRun run = g_MetricsCollector.GetCurrentRun();
	const NetLockstepStats& stats = coordinator.GetStats();
	const bool completed = stats.timeoutReason.rfind("Complete:", 0) == 0;
	const char* finalState = !setupError.empty() ? "Failed" : NetLockstepCoordinator::StateName(coordinator.GetState());
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << "{";
	out << "\"final_state\":\"" << finalState << "\",";
	out << "\"setup_error\":\"" << JsonEscape(setupError) << "\",";
	out << "\"scenario_exit_code\":" << scenarioExitCode << ",";
	out << "\"session_role\":\"" << NetSession::RoleName(session.GetRole()) << "\",";
	out << "\"session_peer_id\":" << static_cast<int>(session.GetLocalPeerId()) << ",";
	out << "\"lockstep_peer_id\":" << static_cast<int>(stats.localPeerId) << ",";
	out << "\"scenario\":\"" << JsonEscape(ScenarioRunner::GetArgs().scenario) << "\",";
	out << "\"uses_lobby_protocol\":" << (runner.UsesLobbyProtocol() ? "true" : "false") << ",";
	out << "\"match_runtime_state\":\"" << NetMatchRunner::StateName(runner.GetState()) << "\",";
	out << "\"match_config_hash\":\"" << JsonEscape(NetIdentity::HashHex(runner.GetMatchConfigHash())) << "\",";
	out << "\"ownership_policy\":\"" << JsonEscape(NetMatchConfigUtil::OwnershipPolicyName(runner.GetMatchConfig().ownershipPolicy)) << "\",";
	out << "\"input_delay_frames\":" << stats.inputDelayFrames << ",";
	out << "\"controller_boundary\":" << BuildControllerBoundaryJson() << ",";
	out << "\"desync_check\":" << BuildDesyncCheckJson() << ",";
	out << "\"frames_planned\":" << (s_netLockstepTicks > 0 ? s_netLockstepTicks : ScenarioRunner::GetArgs().maxTicks) << ",";
	out << "\"frames_sent\":" << stats.framePacketsSent << ",";
	out << "\"frames_received\":" << stats.framePacketsReceived << ",";
	out << "\"frames_accepted\":" << stats.framesAccepted << ",";
	out << "\"local_controller_frames_sent\":" << stats.localControllerFramesSent << ",";
	out << "\"remote_controller_frames_received\":" << stats.remoteControllerFramesReceived << ",";
	out << "\"remote_controller_frames_accepted\":" << stats.remoteControllerFramesAccepted << ",";
	out << "\"frames_simulated\":" << run.ticks << ",";
	out << "\"duplicate_frames\":" << stats.duplicateFrames << ",";
	out << "\"out_of_order_frames\":" << stats.outOfOrderFrames << ",";
	out << "\"missing_frame_stalls\":" << stats.missingFrameStalls << ",";
	out << "\"lockstep_stop_reason\":\"" << JsonEscape(stats.timeoutReason) << "\",";
	out << "\"stall_timeout_reason\":\"" << JsonEscape(completed ? "" : stats.timeoutReason) << "\",";
	out << "\"first_desync_tick\":-1,";
	out << "\"first_desync_subsystem\":\"\",";
	out << "\"final_total_hash\":\"" << JsonEscape(run.finalTotalHashHex) << "\",";
	out << "\"controller_frame_actor_state_payload\":true,";
	out << "\"controller_frame_payload_note\":\"controls plus pose/equip/aim/facing/hand/device state\",";
	out << "\"match_config\":" << NetMatchConfigUtil::BuildReportJson(runner.GetMatchConfig()) << ",";
	out << "\"session\":" << session.BuildReportJson() << ",";
	out << "\"lockstep\":" << coordinator.BuildReportJson() << ",";
	out << "\"match_runner\":" << runner.BuildReportJson(session, coordinator);
	out << "}";
	return out.str();
}

const char* ActivityStateName(Activity::ActivityState state) {
	switch (state) {
		case Activity::NoActivity: return "NoActivity";
		case Activity::NotStarted: return "NotStarted";
		case Activity::Starting: return "Starting";
		case Activity::Editing: return "Editing";
		case Activity::PreGame: return "PreGame";
		case Activity::Running: return "Running";
		case Activity::HasError: return "HasError";
		case Activity::Over: return "Over";
	}
	return "Unknown";
}

// How this peer's AI pass crossed the controller boundary; a direct write is a boundary violation.
std::string BuildControllerBoundaryJson() {
	const MovableMan::ControllerBoundaryStats& stats = g_MovableMan.GetControllerBoundaryStats();
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << "{\"equip_commands\":" << stats.equipCommands << ",\"sound_commands\":" << stats.soundCommands << ",\"aim_intents\":" << stats.aimIntents
	    << ",\"flip_intents\":" << stats.flipIntents << ",\"direct_writes\":" << stats.directWrites
	    << ",\"local_script_messages\":" << stats.localScriptMessages << "}";
	return out.str();
}

// What the runtime desync check did this match. Zero submissions in a finished lockstep match means
// the detector never ran, which every other number in the report would have hidden.
std::string BuildDesyncCheckJson() {
	const ScenarioRunner::LockstepChecksumCounters counters = ScenarioRunner::GetLockstepChecksumCounters();
	std::ostringstream out;
	out.imbue(std::locale::classic());
	const uint64_t ticks = ScenarioRunner::GetLockstepAppliedFrame();
	const int64_t compareFloor = static_cast<int64_t>(ticks / 30) - 1;
	const int64_t compareMargin = static_cast<int64_t>(counters.compares) - compareFloor;
	out << "{\"submissions\":" << counters.submissions << ",\"sends\":" << counters.sends
	    << ",\"compares\":" << counters.compares << ",\"mismatches\":" << counters.mismatches
	    << ",\"compare_floor\":" << compareFloor << ",\"compare_margin\":" << compareMargin << "}";
	return out.str();
}

// The in-match loop pace: wall_tps is the number that answers "does the match run at the pinned
// dt's intended rate", sim_ms_per_tick is the full-tick compute cost (and, in playback, the
// rollback re-sim cost — playback runs no AI).
std::string BuildLoopPaceJson() {
	const long long wallUs = s_paceUpdateUs + s_paceDrawUs - s_paceTrailingUs;
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << "{";
	out << "\"iterations\":" << s_paceIterations << ",";
	out << "\"sim_ticks\":" << s_paceSimTicks << ",";
	out << "\"wall_ms\":" << wallUs / 1000 << ",";
	out << "\"trailing_ms\":" << s_paceTrailingUs / 1000 << ",";
	out << "\"sim_ms\":" << s_paceSimUs / 1000 << ",";
	out << "\"draw_ms\":" << s_paceDrawUs / 1000 << ",";
	out << "\"wall_tps\":" << (wallUs > 0 ? static_cast<double>(s_paceSimTicks) * 1000000.0 / static_cast<double>(wallUs) : 0.0) << ",";
	out << "\"sim_ms_per_tick\":" << (s_paceSimTicks > 0 ? static_cast<double>(s_paceSimUs) / 1000.0 / static_cast<double>(s_paceSimTicks) : 0.0) << ",";
	out << "\"sim_execution_average_ms\":" << s_paceExecutionAverageMs.load(std::memory_order_relaxed) << ",";
	out << "\"draw_ms_per_iter\":" << (s_paceIterations > 0 ? static_cast<double>(s_paceDrawUs) / 1000.0 / static_cast<double>(s_paceIterations) : 0.0) << ",";
	out << "\"net_wait_ms\":" << ScenarioRunner::GetLockstepWaitUs() / 1000 << ",";
	const double ticksPerMs = static_cast<double>(g_TimerMan.GetTicksPerSecond()) / 1000.0;
	out << "\"accrued_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceAccruedTicks()) / ticksPerMs) << ",";
	out << "\"trimmed_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceTrimmedTicks()) / ticksPerMs) << ",";
	out << "\"wall_seen_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceWallSeenTicks()) / ticksPerMs) << ",";
	out << "\"cap_lost_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPaceCapLostTicks()) / ticksPerMs) << ",";
	out << "\"paused_lost_ms\":" << static_cast<long long>(static_cast<double>(g_TimerMan.GetPacePausedLostTicks()) / ticksPerMs) << ",";
	out << "\"update_calls\":" << g_TimerMan.GetPaceUpdateCalls() << ",";
	out << "\"reset_calls\":" << g_TimerMan.GetPaceResetCalls() << ",";
	out << "\"time_scale\":" << g_TimerMan.GetTimeScale();
	out << "}";
	return out.str();
}

// What a previewed actor put on the output and when, so a fixture can measure press to sound.
std::string BuildPreviewEventStartsJson() {
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << "[";
	const std::vector<PreviewEventLedger::EventStart>& starts = PreviewEventLedger::GetEventStarts();
	for (size_t index = 0; index < starts.size(); ++index) {
		const PreviewEventLedger::EventStart& start = starts[index];
		out << (index ? "," : "") << "{\"committed_tick\":" << start.committedTick << ",\"event_tick\":" << start.eventTick << ",\"emitter\":" << start.emitterUID
		    << ",\"kind\":" << static_cast<int>(start.kind) << ",\"seq\":" << start.seq << ",\"predicted\":" << (start.predicted ? "true" : "false") << "}";
	}
	out << "]";
	return out.str();
}

std::string BuildNetMatchServiceE2EReportJson(int exitCode, const std::string& setupError) {
	const Activity* activity = g_ActivityMan.GetActivity();
	const Activity::ActivityState activityState = activity ? activity->GetActivityState() : Activity::NoActivity;
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << "{";
	out << "\"exit_code\":" << exitCode << ",";
	out << "\"setup_error\":\"" << JsonEscape(setupError) << "\",";
	out << "\"runtime_error\":\"" << JsonEscape(s_netMatchServiceE2EError) << "\",";
	out << "\"activity_preset\":\"" << JsonEscape(s_netMatchServiceE2EPreset) << "\",";
	out << "\"activity_state\":\"" << ActivityStateName(activityState) << "\",";
	const GameActivity* reportGameActivity = dynamic_cast<const GameActivity*>(activity);
	out << "\"winner_team\":" << (reportGameActivity ? reportGameActivity->GetWinnerTeam() : Activity::NoTeam) << ",";
	out << "\"entered_editor\":" << (s_netMatchServiceE2EEnteredEditor ? "true" : "false") << ",";
	out << "\"rematches\":" << s_netMatchServiceE2ERematches << ",";
	out << "\"completed_by_host_goodbye\":" << (s_netMatchCompletedByHostGoodbye ? 1 : 0) << ",";
	out << "\"held_from\":" << s_netMatchHeldFromTick << ",";
	out << "\"goodbye_final_frame\":" << s_netMatchGoodbyeFinalFrame << ",";
	out << "\"resyncs\":" << s_netMatchHeals.Total() << ",";
	out << "\"resyncs_in_window\":" << s_netMatchHeals.InWindow() << ",";
	out << "\"stale_activity_slots\":" << g_ActivityMan.StaleActivitySlotCount() << ",";
	// The actor census guards against sim-CONSISTENT duplication (both peers doubling identically
	// slips every divergence gate); the peak catches a double-spawn that later sheds back to normal.
	out << "\"actors\":" << s_netMatchE2EActorCensus << ",";
	out << "\"actors_peak\":" << s_netMatchE2EActorCensusPeak << ",";
	out << "\"owner_log\":[";
	for (size_t i = 0; i < s_netMatchE2eOwnerLog.size(); ++i) {
		if (i) {
			out << ",";
		}
		const E2eOwnerLogEntry& entry = s_netMatchE2eOwnerLog[i];
		out << "{\"tick\":" << entry.tick << ",\"uid\":" << entry.uid << ",\"owner\":" << entry.owner << ",\"mode\":" << entry.mode << "}";
	}
	out << "],";
	out << "\"pace\":" << BuildLoopPaceJson() << ",";
	out << "\"running_ticks\":" << s_netMatchE2ETicks.Total() << ",";
	out << "\"frames_planned\":" << (s_netLockstepTicks > 0 ? s_netLockstepTicks : 600) << ",";
	out << "\"local_prediction\":{\"enabled\":" << (LocalPrediction::IsEnabled() ? "true" : "false")
	    << ",\"previews\":" << LocalPrediction::GetPreviewCount() << ",\"actor_ticks\":" << LocalPrediction::GetPreviewTicks()
	    << ",\"ms_total\":" << LocalPrediction::GetPreviewMs() << ",\"shadows\":" << LocalPrediction::GetShadows() << ",\"taken\":" << LocalPrediction::GetTaken()
	    << ",\"violations\":" << LocalPrediction::GetViolations()
	    << ",\"events_played_at_preview\":" << PreviewEventLedger::GetCounters().playedAtPreview
	    << ",\"events_suppressed_at_commit\":" << PreviewEventLedger::GetCounters().adoptedAtCommit
	    << ",\"events_expired\":" << PreviewEventLedger::GetCounters().expired
	    << ",\"events_retimed\":" << PreviewEventLedger::GetCounters().retimed
	    << ",\"event_starts\":" << BuildPreviewEventStartsJson() << "},";
	out << "\"controller_boundary\":" << BuildControllerBoundaryJson() << ",";
	out << "\"desync_check\":" << BuildDesyncCheckJson() << ",";
	out << "\"replay_recording\":{\"frames\":" << ScenarioRunner::GetLockstepReplayRecordFrames()
	    << ",\"closed\":" << (ScenarioRunner::WasLockstepReplayRecordClosed() ? "true" : "false") << "},";
	out << "\"setup_surface\":\"fixed-alpha-duel\",";
	out << "\"unsupported_setup_surface\":\"stock pregame editor/deployment/buy-menu setup is not synchronized in P4A\",";
	// Presentation-only instrumentation: banners queued, wait-screen frames drawn, the announced
	// input-delay line. None of it touches sim state, tick hashes or saves.
	out << "\"ui\":{\"toasts\":[";
	const std::vector<ScenarioRunner::NetUiToastRecord>& uiToasts = ScenarioRunner::GetNetUiToastLog();
	for (size_t i = 0; i < uiToasts.size(); ++i) {
		if (i) out << ",";
		out << "{\"tick\":" << uiToasts[i].tick << ",\"kind\":\"" << JsonEscape(uiToasts[i].kind)
		    << "\",\"text\":\"" << JsonEscape(uiToasts[i].text) << "\"}";
	}
	out << "],\"resync_overlay_frames\":" << ScenarioRunner::GetResyncOverlayFrames()
	    << ",\"input_delay_text\":\"" << JsonEscape(g_NetMatchService.GetInputDelayText()) << "\"},";
	out << "\"service\":" << g_NetMatchService.BuildReportJson();
	out << "}";
	return out.str();
}

bool StageResyncedMatchActivity(std::string* error) {
	return g_NetMatchService.StageResyncedMatchLaunch(error);
}

bool ConfigureNetMatchActivity(const NetMatchConfig& config, int localTeam, std::string* error) {
	Activity* activity = NetActivitySetup::CreateConfiguredActivity(config, localTeam, error);
	if (!activity) {
		return false;
	}
	if (s_netMatchServiceE2E) {
		if (const GameActivity* gameActivity = dynamic_cast<const GameActivity*>(activity)) {
			for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
				if (team == localTeam || ScenarioRunner::IsLockstepActiveTeam(team)) {
					{
						std::ostringstream line;
						line << "[e2e] TeamIsCPU team=" << team << " value=" << (gameActivity->TeamIsCPU(team) ? 1 : 0);
						System::PrintDiagnosticLine(line.str());
					}
				}
			}
		}
	}
	ScenarioRunner::ApplyDeterministicConfig();
	// Before any image of the round is applied: the image carries the round's own readings.
	g_AudioMan.BeginLockstepRound();
	g_ActivityMan.SetStartActivity(activity);
	g_ActivityMan.SetRestartActivity(true);
	return true;
}

bool ConfigureNetMatchServiceE2EActivity(const std::string& activityPreset, std::string* error) {
	// The roster's agreed config is the launch descriptor on every peer, the dedicated host and here.
	const auto config = ScenarioRunner::GetLockstepMatchConfig();
	if (!config) {
		if (error) *error = "the launching match carries no agreed config";
		return false;
	}
	(void)activityPreset; // The roster already carries the preset ConsumeReadyToLaunch handed up.
	return ConfigureNetMatchActivity(*config, g_NetMatchService.GetLocalTeam(), error);
}

// A world segment stands on a checkpoint, so the sim is loaded from that archive and given the
// manifest's agreed lockstep state, exactly as a resumed host stands its round up.
static bool StageWorldSegmentActivity(const NetMatchService::WorldSegmentPlayback& staged, std::string* error) {
	if (!g_ActivityMan.LoadAutosaveToRestart(staged.checkpoint.matchId, staged.checkpoint.savedTick)) {
		if (error) *error = "segment refused: checkpoint " + std::to_string(staged.checkpoint.savedTick) +
		                    " of world " + staged.checkpoint.matchId + " could not be loaded";
		return false;
	}
	g_ActivityMan.NoteLockstepRelaunch();
	const auto state = std::make_shared<NetResyncState>(staged.resumeState);
	if (!g_ActivityMan.SetPendingCheckpointCallbacks([] { return true; }, [state](Activity& activity) {
		    if (static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) != state->savedTick) return false;
		    // The restored world already carries the seats it had at that tick; they are read back out
		    // of it rather than derived from the roster.
		    NetGamePlayerBindings own;
		    activity.CaptureNetPlayerBindings(own);
		    if (!activity.ApplyNetPlayerBindings(own)) return false;
		    return ScenarioRunner::RestoreNetResyncState(*state);
	    })) {
		if (error) *error = "segment refused: the checkpoint could not be staged for restoration";
		return false;
	}
	ScenarioRunner::ApplyDeterministicConfig();
	return true;
}

// Drives a recorded match through the standard lockstep apply path: a no-remote coordinator over
// a dead-end transport, fed tick records by the replay reader. The deterministic sim reproduces
// the match, so a -tick-hashes trace must equal the recording peer's.
bool StartNetReplayPlayback(const std::string& path, bool fromMenu, std::string* error) {
	std::string setupError;
	if (ScenarioRunner::HasLockstepCoordinator()) {
		if (error) *error = "Leave the current match before playing a replay.";
		return false;
	}
	if (!ScenarioRunner::SetLockstepReplaySource(path, &setupError)) {
		if (error) *error = setupError;
		return false;
	}
	s_netReplayExitCode = 0;
	s_netReplayTicks = 0;
	s_netReplaySegmentTick = 0;
	s_netReplayFromMenu = fromMenu;
	s_netReplayPreviousDeltaTime = g_TimerMan.GetDeltaTimeSecs();
	s_netReplayPreviousFreeRun = g_TimerMan.IsFreeRunSim();
	ScenarioRunner::ClearControllerReplayError();
	const NetMatchConfig& replayConfig = ScenarioRunner::GetLockstepReplayConfig();
	{
		std::ostringstream line;
		line << "[net-replay] playing back " << path << ": " << replayConfig.activityPreset
		     << ", " << static_cast<int>(replayConfig.peerCount) << " peers";
		System::PrintDiagnosticLine(line.str());
	}
	// A world segment boots the world's own snapshot instead of the preset: its records begin one tick
	// after the checkpoint, so the sim has to already stand on it.
	const bool worldSegment = ScenarioRunner::IsLockstepReplayWorldSegment();
	NetMatchService::WorldSegmentPlayback staged;
	if (worldSegment) {
		staged = NetMatchService::PrepareWorldSegmentPlayback(AutosaveStore::Directory(), ScenarioRunner::GetLockstepReplayWorldSegment(),
		                                                      replayConfig, ScenarioRunner::GetLockstepReplayStartFrame());
		if (!staged.refusal.empty()) {
			if (error) *error = staged.refusal;
			CloseNetReplayPlayback();
			return false;
		}
		s_netReplaySegmentTick = staged.checkpoint.savedTick;
		std::ostringstream line;
		line << "[net-replay] segment stands on checkpoint tick=" << staged.checkpoint.savedTick
		     << " world=" << staged.checkpoint.matchId << " round=" << staged.manifest.roundId;
		System::PrintDiagnosticLine(line.str());
	}

	static NullNetTransport s_nullTransport;
	static NetLockstepCoordinator s_replayCoordinator;
	NetLockstepConfig lockstepConfig;
	lockstepConfig.sessionId = replayConfig.sessionId;
	// Align to the recording's first tick, whatever sim count its match began on.
	lockstepConfig.startFrame = worldSegment ? staged.startFrame : ScenarioRunner::GetLockstepReplayStartFrame();
	lockstepConfig.localPeerId = 1;
	lockstepConfig.peerCount = replayConfig.peerCount;
	lockstepConfig.scenario = "replay";
	lockstepConfig.authorityPeerId = ScenarioRunner::GetLockstepReplayStartAuthorityPeerId();
	lockstepConfig.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(replayConfig.ownershipPolicy);
	lockstepConfig.matchConfig = replayConfig;
	if (!s_replayCoordinator.StartReplay(s_nullTransport, lockstepConfig, &setupError)) {
		if (error) *error = setupError;
		CloseNetReplayPlayback();
		return false;
	}
	// A segment stands on its checkpoint: the round's opening start boundary lies before its first record and would hold the
	// playback at a frame the segment never carries.
	if (const auto& agreed = ScenarioRunner::GetLockstepReplayAgreedStart(); agreed && !(worldSegment && agreed->agreedFirstFrame < lockstepConfig.startFrame)) {
		if (!s_replayCoordinator.ApplyReplayAgreedStart(*agreed, &setupError)) {
			if (error) *error = setupError;
			CloseNetReplayPlayback();
			return false;
		}
	}
	ScenarioRunner::SetLockstepCoordinator(&s_replayCoordinator);

	// Watch through the first human seat; per-peer view bindings are off-sim.
	int localTeam = Activity::TeamOne;
	for (const NetMatchPlayerSlot& slot: replayConfig.players) {
		if (!slot.cpu) {
			localTeam = slot.team;
			break;
		}
	}
	if (!(worldSegment ? StageWorldSegmentActivity(staged, &setupError) : ConfigureNetMatchActivity(replayConfig, localTeam, &setupError))) {
		if (error) *error = (fromMenu ? "" : "setup failed: ") + setupError;
		CloseNetReplayPlayback();
		return false;
	}
	return true;
}

static void CloseNetReplayPlayback() {
	ScenarioRunner::SetLockstepCoordinator(nullptr);
	ScenarioRunner::CloseLockstepReplayPlayback();
	ScenarioRunner::ClearControllerReplayError();
	g_TimerMan.SetDeltaTimeSecs(s_netReplayPreviousDeltaTime);
	g_TimerMan.SetFreeRunSim(s_netReplayPreviousFreeRun);
	s_netReplayFromMenu = false;
}

int RunNetReplayPlayback() {
	std::string setupError;
	if (!StartNetReplayPlayback(s_netReplayInPath, false, &setupError)) {
		{
			std::ostringstream line;
			line << "[net-replay] " << setupError;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}

	const bool traceRun = s_recordTickHashes && !ScenarioRunner::GetArgs().outPath.empty();
	if (traceRun) {
		g_MetricsCollector.BeginHostRun("P4 Alpha Duel", ScenarioRunner::GetArgs().seed);
		g_MetricsCollector.SetRecordTickHashes(true);
	}
	// Playback is not real-time: free-run the sim as fast as it computes (the fixed dt is
	// untouched). The wall/tick numbers this yields ARE the rollback re-sim budget.
	g_TimerMan.SetFreeRunSim(true);
	const auto playbackStart = std::chrono::steady_clock::now();
	RunGameLoop();
	CheckRequiredProbesCompleted();
	const auto playbackMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - playbackStart).count();
	using ReplayOutcome = ScenarioRunner::LockstepReplayOutcome;
	const ReplayOutcome finalOutcome = ScenarioRunner::GetLockstepReplayOutcome();
	if (traceRun) {
		g_MetricsCollector.RecordString("replay_outcome", ScenarioRunner::ReplayOutcomeName(finalOutcome));
		g_MetricsCollector.Record("replay_frames_consumed", static_cast<double>(ScenarioRunner::GetLockstepReplayFramesConsumed()));
		g_MetricsCollector.Record("replay_last_tick", static_cast<double>(ScenarioRunner::GetLockstepReplayLastTick()));
		g_MetricsCollector.Record("replay_end_marker", ScenarioRunner::LockstepReplaySawEndMarker() ? 1.0 : 0.0);
		g_MetricsCollector.Record("replay_exit_code", static_cast<double>(s_netReplayExitCode));
		if (finalOutcome == ReplayOutcome::Completed || finalOutcome == ReplayOutcome::TickCap) {
			g_MetricsCollector.RecordString("controller_replay_error", "");
		}
		g_MetricsCollector.SetResult(s_netReplayExitCode == 0);
		g_MetricsCollector.SetNativeOutcome(s_netReplayExitCode == 0, std::string("replay ") + ScenarioRunner::ReplayOutcomeName(finalOutcome) + ", exit code " + std::to_string(s_netReplayExitCode),
		                                   ScenarioRunner::GetLockstepReplayLastTick());
		g_MetricsCollector.EndRun();
		const std::string& tracePath = ScenarioRunner::GetArgs().outPath;
		if (!g_MetricsCollector.WriteReport(tracePath)) {
			{
				std::ostringstream line;
				line << "[net-replay] trace write failed: " << tracePath;
				System::PrintDiagnosticErrorLine(line.str());
			}
			s_netReplayExitCode = 1;
		} else {
			{
				std::ostringstream line;
				line << "[net-replay] wrote trace: " << tracePath;
				System::PrintDiagnosticLine(line.str());
			}
		}
	}
	{
		std::ostringstream line;
		line << "[net-replay] playback " << (s_netReplayExitCode == 0 ? "finished" : "FAILED") << " in " << playbackMs
		     << "ms, ticks=" << s_netReplayTicks << " segment_tick=" << s_netReplaySegmentTick
		     << " outcome=" << ScenarioRunner::ReplayOutcomeName(finalOutcome)
		     << " frames=" << ScenarioRunner::GetLockstepReplayFramesConsumed() << " last_tick=" << ScenarioRunner::GetLockstepReplayLastTick()
		     << " end_marker=" << (ScenarioRunner::LockstepReplaySawEndMarker() ? 1 : 0) << " exit=" << s_netReplayExitCode;
		System::PrintDiagnosticLine(line.str());
	}
	{
		std::ostringstream line;
		line << "[pace] " << BuildLoopPaceJson();
		System::PrintDiagnosticLine(line.str());
	}
	if (const std::string stats = LocalPrediction::DescribeStats(); !stats.empty()) {
		{
			std::ostringstream line;
			line << "[localpred] " << stats;
			System::PrintDiagnosticLine(line.str());
		}
	}
	CloseNetReplayPlayback();
	return s_netReplayExitCode;
}

// Loads the -net-chat-script file: one "tick all|team text" line per scheduled send.
static bool LoadNetChatScript(const std::string& path, std::string* error) {
	std::ifstream in(path);
	if (!in) {
		*error = "cannot open chat script: " + path;
		return false;
	}
	s_netChatScript.clear();
	s_netChatScriptNext = 0;
	std::string line;
	uint64_t lineNo = 0;
	while (std::getline(in, line)) {
		++lineNo;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}
		std::istringstream ls(line);
		uint64_t tick = 0;
		std::string scope;
		if (!(ls >> tick >> scope) || (scope != "all" && scope != "team")) {
			*error = "chat script line " + std::to_string(lineNo) + ": expected '<tick> all|team <text>'";
			return false;
		}
		std::string text;
		std::getline(ls, text);
		if (!text.empty() && text[0] == ' ') {
			text.erase(0, 1);
		}
		s_netChatScript.push_back({tick, static_cast<uint8_t>(scope == "team" ? c_NetChatScopeTeam : c_NetChatScopeAll), text});
	}
	std::stable_sort(s_netChatScript.begin(), s_netChatScript.end(), [](const NetChatScriptLine& a, const NetChatScriptLine& b) { return a.tick < b.tick; });
	return true;
}

// Fires the due chat sends on this peer's tick, then drains what arrived — the [chat] lines are the
// driver's oracle, so they print in sink order exactly once each.
static void NetChatScriptOnSimTick(uint64_t simTick) {
	while (s_netChatScriptNext < s_netChatScript.size() && s_netChatScript[s_netChatScriptNext].tick <= simTick) {
		const NetChatScriptLine& line = s_netChatScript[s_netChatScriptNext++];
		const bool sent = g_NetMatchService.SendChat(line.scope, line.text);
		{
			std::ostringstream diag;
			diag << "[chat-send] tick=" << simTick << " scope=" << (line.scope == c_NetChatScopeTeam ? "team" : "all")
			     << " ok=" << (sent ? 1 : 0) << " text=" << line.text;
			System::PrintDiagnosticLine(diag.str());
		}
	}
	for (const NetChatEntry& entry : g_NetMatchService.TakeChatEntries()) {
		{
			std::ostringstream line;
			line << "[chat] tick=" << entry.receivedTick
			     << " from=" << static_cast<int>(entry.senderPeerId)
			     << " scope=" << (entry.scope == c_NetChatScopeTeam ? "team" : "all")
			     << " text=" << entry.text;
			System::PrintDiagnosticLine(line.str());
		}
	}
}

#ifdef _WIN32
static BOOL WINAPI WorldDaemonCtrlHandler(DWORD) {
	System::SetQuit(true);
	return TRUE;
}
#else
static void WorldDaemonSignal(int) {
	System::SetQuit(true);
}
#endif

int RunNetMatchServiceE2E() {
	EnsureCrossEventsOpen();
	std::string setupError;
	if (!NetA7Journal::StartE2E(&setupError, [] { PollSDLEvents(); return System::IsSetToQuit(); })) s_netMatchServiceE2EExitCode = 1;
	const bool e2eHost = s_netHost || s_netDedicated;
	const bool e2eJoiner = !s_netJoinAddress.empty() || !s_netJoinSessionId.empty();
	if (s_netDedicated && e2eJoiner) {
		setupError = "-net-dedicated cannot be combined with -net-join <address>";
	} else if (e2eHost == e2eJoiner) {
		setupError = "-net-match-service-e2e requires exactly one of -net-host, -net-dedicated, -net-join <address> or -net-join-session <id>";
	}
	if (setupError.empty() && !s_netChatScriptPath.empty() && !LoadNetChatScript(s_netChatScriptPath, &setupError)) {
		s_netMatchServiceE2EExitCode = 1;
	}

	if (s_netWorldDaemon || s_netPersistentWorld) {
#ifdef _WIN32
		SetConsoleCtrlHandler(WorldDaemonCtrlHandler, TRUE);
#else
		std::signal(SIGINT, WorldDaemonSignal);
		std::signal(SIGTERM, WorldDaemonSignal);
#endif
	}

	if (setupError.empty()) {
		ScenarioRunner::ApplyDeterministicConfig();
		NetMatchServiceRequest request;
		request.host = e2eHost;
		request.dedicated = s_netDedicated;
		request.address = s_netJoinAddress.empty() ? "127.0.0.1" : s_netJoinAddress;
		request.sessionId = s_netJoinSessionId;
		request.port = s_netPort;
		request.playerName = s_netPlayerName.empty() ? (e2eHost ? "Host" : "Client") : s_netPlayerName;
		request.activityPreset = s_netMatchServiceE2EPreset;
		request.activityModule = s_netMatchServiceE2EModule;
		if (e2eHost && !s_netMatchServiceConfigPath.empty()) {
			std::ifstream input(s_netMatchServiceConfigPath, std::ios::binary);
			std::vector<uint8_t> bytes(NetLobbyProtocol::c_HeaderBytes + NetLobbyProtocol::c_MaxPayloadBytes + 1);
			input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			bytes.resize(static_cast<size_t>(input.gcount()));
			const auto decoded = NetLobbyProtocol::Decode(bytes);
			const auto* payload = decoded.ok ? std::get_if<NetLobbyMatchConfig>(&decoded.message.payload) : nullptr;
			if (!payload || payload->config.dedicated != request.dedicated) {
				setupError = payload ? "launch config dedicated role differs from command line" : "launch config: " + decoded.error.message;
			} else {
				request.standardRules = payload->config;
				request.activityPreset = payload->config.activityPreset;
				request.activityModule = payload->config.activityModule;
				request.sceneName = payload->config.sceneName;
				request.sceneModule = payload->config.sceneModule;
			}
		}
		if (!s_netMatchServiceE2EScene.empty()) {
			request.sceneName = s_netMatchServiceE2EScene;
			request.sceneModule = s_netMatchServiceE2ESceneModule;
		}
		// The e2e honours -net-match-ownership-policy; team-owner is the default so the flagless path is unchanged.
		NetActorOwnershipPolicy e2ePolicy;
		request.ownershipPolicy = NetMatchConfigUtil::ParseOwnershipPolicy(s_netMatchOwnershipPolicy, e2ePolicy) ? e2ePolicy : NetActorOwnershipPolicy::TeamOwner;
		request.inputDelayFrames = s_netLockstepInputDelay;
		request.brainlessHumansSpectate = s_netMatchBrainlessSpectate;
		request.peerCount = s_netMatchPeers;
		request.humans = s_netMatchHumans;
		request.cpuSlots = s_netMatchCPUSlots;
		NetMatchMode parsedMode;
		if (NetMatchConfigUtil::ParseMode(s_netMatchMode, parsedMode)) {
			request.mode = parsedMode;
		}
		request.resyncOnDesync = s_netMatchResyncOnDesync;
		if (s_crossTicketRejoin) { request.rejoin = true; request.resyncOnDesync = true; }
		request.autoInputDelay = s_netMatchAutoDelay;
		if (s_netPersistentWorld && e2eHost) {
			request.persistentWorld = true;
			request.dedicated = true;
			request.worldFresh = s_netWorldFresh;
		}
		if (e2eHost && !s_netResumeMatchId.empty()) {
			// The checkpoint's own manifest authors the roster, so nothing above steers this round.
			request.resumeMatchId = s_netResumeMatchId;
			request.resumeTick = s_netResumeTick;
		}
		if (!setupError.empty() || !g_NetMatchService.Start(request, &setupError)) {
			s_netMatchServiceE2EExitCode = 1;
		}
	}

	bool setupCancelled = false;
	std::string activityPreset;
	// Match setup needs every admitted peer's readiness.
	const bool waitPeers = std::getenv("CC_TEST_NET_MATCH_E2E_WAIT_PEERS") != nullptr;
	if (setupError.empty()) {
		g_NetMatchService.SetReady();
		bool crossOptionsApplied = s_crossHostOptions.empty() || !e2eHost;
		uint64_t crossReadyRevision = UINT64_MAX;
		const auto peersReady = [waitPeers] {
			if (!waitPeers) return true;
			const auto snapshot = g_NetMatchService.GetLobbySnapshot();
			const auto ready = std::count_if(snapshot.members.begin(), snapshot.members.end(), [](const NetLobbyMember& member) {
				return !member.cpu && member.connected && member.ready;
			});
			return ready >= s_netMatchPeers - (s_netDedicated ? 1 : 0);
		};
		if (e2eHost && crossOptionsApplied && peersReady()) {
			g_NetMatchService.RequestStart();
		}
		bool roundEndedOnTheWay = false;
		while (true) {
			PollSDLEvents();
			CrossRecoveryAtCommittedTick(0);
			if (waitPeers && g_NetMatchService.GetState() == NetMatchServiceState::Starting) g_NetMatchService.SetReady();
			if (!crossOptionsApplied) {
				std::string optionsError;
				if (CrossHostOptions(0, &optionsError)) { crossOptionsApplied = true; g_NetMatchService.SetReady(); if (peersReady()) g_NetMatchService.RequestStart(); }
			}
			if (crossOptionsApplied && peersReady()) {
				if (e2eHost && waitPeers) g_NetMatchService.RequestStart();
				CrossReadyForCurrentConfig(crossReadyRevision);
			}
			if (System::IsSetToQuit()) {
				setupCancelled = true;
				g_NetMatchService.Destroy();
				break;
			}
			if (g_NetMatchService.ConsumeReadyToLaunch(activityPreset)) break;
			// The directory row is the game thread's to drive, and a session-id join waits on it.
			g_NetMatchService.Update();
			const NetMatchServiceState state = g_NetMatchService.GetState();
			if (e2eHost && ScenarioRunner::GetArgs().selftestJoinRejection && state == NetMatchServiceState::Starting) {
				const std::string rejection = g_NetMatchService.GetErrorText();
				if (!rejection.empty()) {
					setupError = rejection;
					s_netMatchServiceE2EExitCode = 1;
					break;
				}
			}
			// A seat whose round ended while it rejoined completes on the host's end record and takes its seat into the next round's lobby.
			if (!e2eHost && state == NetMatchServiceState::Completed && !roundEndedOnTheWay) {
				roundEndedOnTheWay = true;
				System::PrintDiagnosticLine("[net-match-service-e2e] the round ended while this seat rejoined: joining the next round's lobby");
				std::string lobbyError;
				if (g_NetMatchService.ReturnToLobby(&lobbyError)) {
					g_NetMatchService.SetReady();
				} else if (!g_NetMatchService.RematchReturnOwed() || !g_NetMatchService.BeginHeldRejoin(&lobbyError)) {
					setupError = "the round ended while this seat rejoined and its seat could not follow: " + lobbyError;
					s_netMatchServiceE2EExitCode = 1;
					break;
				}
				continue;
			}
			if (state == NetMatchServiceState::Failed) {
				setupError = g_NetMatchService.GetErrorText();
				s_netMatchServiceE2EExitCode = 1;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	if (setupError.empty() && !setupCancelled) {
		// The report names the adopted activity, which a joining peer's own request does not carry.
		if (!activityPreset.empty()) {
			s_netMatchServiceE2EPreset = activityPreset;
		}
		const NetLobbySnapshot snapshot = g_NetMatchService.GetLobbySnapshot();
		{
			std::ostringstream line;
			line << "[net-match-service-e2e] lobby_snapshot: state=" << snapshot.serviceState
			     << " is_host=" << (snapshot.isHost ? 1 : 0) << " members=" << snapshot.members.size()
			     << " local_ready=" << (snapshot.localReady ? 1 : 0) << " remote_ready=" << (snapshot.remoteReady ? 1 : 0)
			     << " activity=" << snapshot.activityPreset << " scene=" << snapshot.sceneName << " mode=" << snapshot.modeName;
			for (const NetLobbyMember& member: snapshot.members) {
				line << " | peer" << static_cast<int>(member.peerId) << "=" << member.displayName
				     << "(team" << static_cast<int>(member.team) << (member.isLocal ? ",local" : ",remote")
				     << (member.ready ? ",ready" : ",notready") << ",ping" << member.pingMs << "ms)";
			}
			System::PrintDiagnosticLine(line.str());
		}
	}

	if (setupError.empty() && !setupCancelled) {
		// A reconnecting peer's first lobby round carried the live match's snapshot; launch from it.
		if (const auto config = ScenarioRunner::GetLockstepMatchConfig()) {
			{
				std::ostringstream line;
				line << "[net-match-service-e2e] roster:";
				for (const NetMatchPlayerSlot& slot: config->players) {
					line << " peer" << static_cast<int>(slot.peerId) << "=" << slot.displayName
					     << "(team" << static_cast<int>(slot.team) << (slot.cpu ? ",cpu)" : ",human)");
				}
				System::PrintDiagnosticLine(line.str());
			}
		}
		const bool staged = g_NetMatchService.HasPendingResyncLoad()
			? StageResyncedMatchActivity(&setupError)
			: ConfigureNetMatchServiceE2EActivity(activityPreset, &setupError);
		if (!staged) {
			s_netMatchServiceE2EExitCode = 1;
		}
	}

	if (setupError.empty() && !setupCancelled) {
		// Per-tick trace for the host/client sim-gated compare. Not SetActive() — that also arms the
		// -scenario stop path + perturb hook; SetRecordTickHashes arms the trace alone.
		const bool traceRun = (s_recordTickHashes || !CrossEnvironment("CC_TEST_CROSS_RECORDS").empty()) && !ScenarioRunner::GetArgs().outPath.empty();
		if (traceRun) {
			g_MetricsCollector.BeginHostRun("P4 Alpha Duel", ScenarioRunner::GetArgs().seed);
			g_MetricsCollector.SetRecordTickHashes(s_recordTickHashes);
		}
		RunGameLoop();
		// A leave is answered on the service worker, and the report below must describe the settled
		// exchange rather than one still in flight.
		g_NetMatchService.WaitForPendingWork();
		CrossRecoveryAtCommittedTick(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
		CheckRequiredProbesCompleted();
		if (s_netReplayExitCode != 0 && s_netMatchServiceE2EExitCode == 0) {
			s_netMatchServiceE2EExitCode = s_netReplayExitCode;
		}
		if (traceRun) {
			// The match's own ending, once the pending work settled and the exit codes combined.
			g_MetricsCollector.SetNativeOutcome(s_netMatchServiceE2EExitCode == 0,
			                                   s_netMatchServiceE2EExitCode == 0 ? "the match ran to its end" : "exit code " + std::to_string(s_netMatchServiceE2EExitCode) + (setupError.empty() ? "" : ": " + setupError),
			                                   static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
			// A round no scenario judges (a dedicated world) carries its own verdict: how it ended and the ticks it played.
			if (!g_MetricsCollector.HasResult()) {
				g_MetricsCollector.SetResult(s_netMatchServiceE2EExitCode == 0);
				g_MetricsCollector.RecordString("verdict_source", "round");
			}
			if (!g_MetricsCollector.HasNumeric("final_tick")) g_MetricsCollector.Record("final_tick", static_cast<double>(g_TimerMan.GetSimUpdateCount()));
			RetractAbandonedTickHashes();
			if (s_netLiveTickHashes.IsOpen()) s_netLiveTickHashes.Flush();
			g_MetricsCollector.EndRun();
			const std::string& tracePath = ScenarioRunner::GetArgs().outPath;
			if (!g_MetricsCollector.WriteReport(tracePath)) {
				{
					std::ostringstream line;
					line << "[net-match-service-e2e] trace write failed: " << tracePath;
					System::PrintDiagnosticErrorLine(line.str());
				}
				s_netMatchServiceE2EExitCode = 1;
			} else {
				{
					std::ostringstream line;
					line << "[net-match-service-e2e] wrote trace: " << tracePath;
					System::PrintDiagnosticLine(line.str());
				}
			}
		}
	} else if (!setupCancelled) {
		{
			std::ostringstream line;
			line << "[net-match-service-e2e] setup failed: " << setupError;
			System::PrintDiagnosticErrorLine(line.str());
		}
	}

	CrossRecoveryAtCommittedTick(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
	CrossNoteOwnBan();
	std::string reportError;
	const int exitCode = setupError.empty() ? s_netMatchServiceE2EExitCode : 1;
	const bool a7ReportSettled = !NetA7Journal::Enabled() || g_NetMatchService.CanSealA7Journal();
	if (!a7ReportSettled) {
		NetA7Journal::Gap("service worker still active before report generation");
		(void)NetA7Journal::Seal("", 1);
		return 1;
	}
	const std::string report = BuildNetMatchServiceE2EReportJson(exitCode, setupError);
	if (!WriteTextFile(s_netLockstepReportPath, report, &reportError)) {
		{
			std::ostringstream line;
			line << "[net-match-service-e2e] report failed: " << reportError;
			System::PrintDiagnosticErrorLine(line.str());
		}
		NetA7Journal::Gap("native report write failed");
		(void)NetA7Journal::Seal("", 1);
		return 1;
	}
	if (!s_netLockstepReportPath.empty()) {
		{
			std::ostringstream line;
			line << "[net-match-service-e2e] wrote report: " << s_netLockstepReportPath;
			System::PrintDiagnosticLine(line.str());
		}
		if (const std::string stats = LocalPrediction::DescribeStats(); !stats.empty()) {
			{
				std::ostringstream line;
				line << "[localpred] " << stats;
				System::PrintDiagnosticLine(line.str());
			}
		}
	}
	return NetA7Journal::Seal(s_netLockstepReportPath, exitCode) ? exitCode : 1;
}

/// <summary>
/// The headless directory probe: register -> heartbeat -> list -> delete -> list against a live
/// session-directory service. Returns 0 only when every step succeeded.
/// </summary>
int RunNetDirectoryProbe(const std::string& baseUrlArg, const std::string& certPin) {
	std::string baseUrl = baseUrlArg;
	while (!baseUrl.empty() && baseUrl.back() == '/') {
		baseUrl.pop_back();
	}
	const std::string installKey = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
	auto request = [&](const std::string& method, const std::string& path, const std::string& body, NetHttpClient::Response& out) {
		NetHttpClient client;
		const std::vector<std::pair<std::string, std::string>> headers = {
			{"X-Install-Key", installKey},
			{"Content-Type", "application/json"},
		};
		client.Start(method, baseUrl + path, headers, body, certPin);
		while (client.Poll() == NetHttpClient::PollResult::Pending) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		out = client.GetResponse();
		{
			std::ostringstream line;
			line << "[net-directory-probe] " << method << " " << path << " -> status=" << out.statusCode;
			if (!out.error.empty()) {
				line << " error=" << out.error;
			}
			line << " body=" << out.body << std::endl;
			System::PrintDiagnosticLine(line.str());
		}
		return out.error.empty();
	};

	std::string reason;
	NetIdentityManifest manifest;
	NetIdentityBuildOptions identityOptions;
	identityOptions.buildId = "stage2-p2d-local";
	identityOptions.sessionRulesTag = "peer-session-plane-v1";
	if (!NetIdentity::BuildCurrentManifest(manifest, &reason, identityOptions)) {
		{
			std::ostringstream line;
			line << "[net-directory-probe] identity manifest failed: " << reason;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}

	NetDirectoryRegisterRequest row;
	row.name = "probe";
	row.activity = "probe";
	row.scene = "probe";
	row.mode = "probe";
	row.peerCount = 1;
	row.seatsFree = 1;
	row.gameVersion = manifest.gameVersion;
	row.buildId = manifest.buildId;
	row.networkProtocolVersion = manifest.networkProtocolVersion;
	row.lockstepCodecVersion = manifest.deterministicConfig.lockstepCodecVersion;
	row.controllerFrameVersion = manifest.controllerFrameVersion;
	row.matchConfigHash = NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(NetMatchConfigUtil::MakeDefault(0x5354414745325032ULL)));
	row.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
	row.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
	row.listenPort = 41010;
	row.listenAddrs = {"127.0.0.1"};
	row.joinMode = "ip";

	NetDirectoryLocalIdentity local;
	local.networkProtocolVersion = row.networkProtocolVersion;
	local.lockstepCodecVersion = row.lockstepCodecVersion;
	local.controllerFrameVersion = row.controllerFrameVersion;
	local.sessionIdentityHash = row.sessionIdentityHash;
	local.moduleManifestHash = row.moduleManifestHash;

	NetHttpClient::Response resp;
	if (!request("POST", "/v1/sessions", NetDirectoryCodec::EncodeRegisterRequest(row), resp)) {
		return 1;
	}
	NetDirectoryRegisterResponse created;
	if (resp.statusCode != 200 || !NetDirectoryCodec::DecodeRegisterResponse(resp.body, created, reason)) {
		{
			std::ostringstream line;
			line << "[net-directory-probe] register refused: status=" << resp.statusCode << " reason=" << reason;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}
	const std::string sessionPath = "/v1/sessions/" + created.sessionId;

	NetDirectoryHeartbeatRequest beat;
	beat.token = created.token;
	beat.peerCount = row.peerCount;
	beat.seatsFree = row.seatsFree;
	if (!request("POST", sessionPath + "/heartbeat", NetDirectoryCodec::EncodeHeartbeatRequest(beat), resp) || resp.statusCode != 200) {
		return 1;
	}

	if (!request("GET", "/v1/sessions", "", resp) || resp.statusCode != 200) {
		return 1;
	}
	NetDirectoryListResponse listed;
	if (!NetDirectoryCodec::DecodeListResponse(resp.body, listed, reason)) {
		{
			std::ostringstream line;
			line << "[net-directory-probe] list decode failed: " << reason;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}
	const NetDirectorySessionRow* mine = nullptr;
	for (const NetDirectorySessionRow& listedRow : listed.sessions) {
		if (listedRow.sessionId == created.sessionId) {
			mine = &listedRow;
		}
	}
	if (mine == nullptr) {
		{
			std::ostringstream line;
			line << "[net-directory-probe] registered row not listed";
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}
	std::string joinReason;
	const bool joinable = NetDirectoryCodec::IsJoinable(*mine, local, &joinReason);
	{
		std::ostringstream line;
		line << "[net-directory-probe] listed row joinable=" << (joinable ? "true" : "false") << (joinReason.empty() ? "" : " reason=" + joinReason);
		System::PrintDiagnosticLine(line.str());
	}
	if (!joinable) {
		return 1;
	}

	NetDirectoryDeleteRequest del;
	del.token = created.token;
	if (!request("DELETE", sessionPath, NetDirectoryCodec::EncodeDeleteRequest(del), resp) || resp.statusCode != 200) {
		return 1;
	}

	if (!request("GET", "/v1/sessions", "", resp) || resp.statusCode != 200) {
		return 1;
	}
	NetDirectoryListResponse after;
	if (!NetDirectoryCodec::DecodeListResponse(resp.body, after, reason)) {
		{
			std::ostringstream line;
			line << "[net-directory-probe] list decode failed: " << reason;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}
	for (const NetDirectorySessionRow& listedRow : after.sessions) {
		if (listedRow.sessionId == created.sessionId) {
			{
				std::ostringstream line;
				line << "[net-directory-probe] row still listed after delete";
				System::PrintDiagnosticErrorLine(line.str());
			}
			return 1;
		}
	}
	{
		std::ostringstream line;
		line << "[net-directory-probe] deleted row absent from the list";
		System::PrintDiagnosticLine(line.str());
	}
	{
		std::ostringstream line;
		line << "[net-directory-probe] PASS";
		System::PrintDiagnosticLine(line.str());
	}
	return 0;
}

/// <summary>
/// The headless signal-channel probe against a live session-directory service: register a row, a
/// joiner channel posts three signals, the host channel polls them with the session token and echoes
/// two back, the joiner polls those, both drain, the row is deleted, and a joiner on the deleted row
/// fails with "session gone". Returns 0 only when every step held.
/// </summary>
int RunNetDirectorySignalProbe(const std::string& baseUrlArg, const std::string& certPin) {
	using Channel = NetDirectorySignalChannel;
	std::string baseUrl = baseUrlArg;
	while (!baseUrl.empty() && baseUrl.back() == '/') {
		baseUrl.pop_back();
	}
	const std::string installKey = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
	auto fail = [](const std::string& why) {
		{
			std::ostringstream line;
			line << "[net-directory-signal-probe] FAIL: " << why;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	};
	auto request = [&](const std::string& method, const std::string& path, const std::string& body, NetHttpClient::Response& out) {
		NetHttpClient client;
		client.Start(method, baseUrl + path, {{"X-Install-Key", installKey}, {"Content-Type", "application/json"}}, body, certPin);
		while (client.Poll() == NetHttpClient::PollResult::Pending) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		out = client.GetResponse();
		{
			std::ostringstream line;
			line << "[net-directory-signal-probe] " << method << " " << path << " -> status=" << out.statusCode << (out.error.empty() ? "" : " error=" + out.error) << " body=" << out.body;
			System::PrintDiagnosticLine(line.str());
		}
		return out.error.empty();
	};
	const auto nowMs = [] {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	};
	// Runs one channel until done() holds, the channel leaves Open, or 10 s pass.
	auto pump = [&](Channel& channel, auto done) {
		const uint64_t begin = nowMs();
		while (!done() && channel.GetState() == Channel::State::Open && nowMs() - begin < 10000) {
			channel.Update(nowMs());
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return done();
	};
	auto headerNames = [](const Channel& channel) {
		std::string names;
		for (const auto& [name, value] : channel.RequestHeaders()) {
			names += (names.empty() ? "" : ",") + name;
		}
		return names;
	};

	NetDirectoryRegisterRequest row;
	row.name = "signal-probe";
	row.activity = "probe";
	row.scene = "probe";
	row.mode = "probe";
	row.peerCount = 1;
	row.seatsFree = 1;
	row.gameVersion = "probe";
	row.buildId = "probe";
	row.matchConfigHash = std::string(64, '0');
	row.sessionIdentityHash = std::string(64, '0');
	row.moduleManifestHash = std::string(64, '0');
	row.listenPort = 41010;
	row.joinMode = "ice";
	NetHttpClient::Response resp;
	NetDirectoryRegisterResponse created;
	std::string reason;
	if (!request("POST", "/v1/sessions", NetDirectoryCodec::EncodeRegisterRequest(row), resp) || resp.statusCode != 200 || !NetDirectoryCodec::DecodeRegisterResponse(resp.body, created, reason)) {
		return fail("register refused: status=" + std::to_string(resp.statusCode) + " " + reason);
	}

	Channel client;
	client.ConfigureClient(baseUrl, installKey, certPin, created.sessionId);
	const std::string nonce = client.GetJoinNonce();
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] joiner nonce=" << nonce << " (" << nonce.size() << " chars, printed so the service log can be searched for it) headers=" << headerNames(client);
		System::PrintDiagnosticLine(line.str());
	}
	Channel host;
	host.ConfigureHost(baseUrl, installKey, certPin, created.sessionId, created.token);
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] host headers=" << headerNames(host);
		System::PrintDiagnosticLine(line.str());
	}
	if (client.GetState() != Channel::State::Open || host.GetState() != Channel::State::Open || nonce.size() != Channel::c_JoinNonceChars) {
		return fail("a channel did not open");
	}

	const std::vector<std::string> posted = {"probe-signal-1", "probe-signal-2", "probe-signal-3"};
	for (const std::string& bytes : posted) {
		if (!client.Post("host", bytes)) {
			return fail("the joiner refused to queue " + bytes);
		}
	}
	if (!pump(client, [&] { return client.PendingPosts() == 0; })) {
		return fail("the joiner's posts did not go: " + client.BuildReportJson());
	}
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] joiner posted 3 signals: " << client.BuildReportJson();
		System::PrintDiagnosticLine(line.str());
	}

	std::vector<Channel::Signal> hostGot;
	host.SetSink([&hostGot](const Channel::Signal& signal) {
		hostGot.push_back(signal);
		return true;
	});
	host.SetPolling(true);
	if (!pump(host, [&] { return hostGot.size() >= posted.size(); })) {
		return fail("the host did not receive 3 signals: " + host.BuildReportJson());
	}
	for (size_t i = 0; i < hostGot.size(); ++i) {
		{
			std::ostringstream line;
			line << "[net-directory-signal-probe] host received seq=" << hostGot[i].seq << " from=" << hostGot[i].from << " bytes=" << hostGot[i].bytes;
			System::PrintDiagnosticLine(line.str());
		}
	}
	for (size_t i = 0; i < posted.size(); ++i) {
		if (hostGot.size() != posted.size() || hostGot[i].seq != static_cast<int64_t>(i + 1) || hostGot[i].from != client.GetLocalPeer() || hostGot[i].bytes != posted[i]) {
			return fail("the host received the signals changed or out of order");
		}
	}

	for (size_t i = 0; i < 2; ++i) {
		if (!host.Post(hostGot[i].from, "echo:" + hostGot[i].bytes)) {
			return fail("the host refused to queue an echo");
		}
	}
	if (!pump(host, [&] { return host.PendingPosts() == 0; })) {
		return fail("the host's echoes did not go: " + host.BuildReportJson());
	}
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] host echoed 2 signals: " << host.BuildReportJson();
		System::PrintDiagnosticLine(line.str());
	}

	std::vector<Channel::Signal> clientGot;
	client.SetSink([&clientGot](const Channel::Signal& signal) {
		clientGot.push_back(signal);
		return true;
	});
	client.SetPolling(true);
	if (!pump(client, [&] { return clientGot.size() >= 2; })) {
		return fail("the joiner did not receive the 2 echoes: " + client.BuildReportJson());
	}
	for (size_t i = 0; i < clientGot.size(); ++i) {
		{
			std::ostringstream line;
			line << "[net-directory-signal-probe] joiner received seq=" << clientGot[i].seq << " from=" << clientGot[i].from << " bytes=" << clientGot[i].bytes;
			System::PrintDiagnosticLine(line.str());
		}
	}
	for (size_t i = 0; i < 2; ++i) {
		if (clientGot.size() != 2 || clientGot[i].seq != static_cast<int64_t>(i + 1) || clientGot[i].from != "host" || clientGot[i].bytes != "echo:" + posted[i]) {
			return fail("the joiner received the echoes changed or out of order");
		}
	}

	host.Drain();
	client.Drain();
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] host drained: cursor=" << host.GetCursor() << " " << host.BuildReportJson();
		System::PrintDiagnosticLine(line.str());
	}
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] joiner drained: cursor=" << client.GetCursor() << " " << client.BuildReportJson();
		System::PrintDiagnosticLine(line.str());
	}
	if (host.GetState() != Channel::State::Closed || client.GetState() != Channel::State::Closed || host.GetCursor() != 3 || client.GetCursor() != 2) {
		return fail("a channel did not drain and close");
	}

	NetDirectoryDeleteRequest del;
	del.token = created.token;
	if (!request("DELETE", "/v1/sessions/" + created.sessionId, NetDirectoryCodec::EncodeDeleteRequest(del), resp) || resp.statusCode != 200) {
		return fail("delete refused");
	}

	Channel late;
	late.ConfigureClient(baseUrl, installKey, certPin, created.sessionId);
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] late joiner nonce=" << late.GetJoinNonce() << " polls the deleted row";
		System::PrintDiagnosticLine(line.str());
	}
	late.SetPolling(true);
	(void)pump(late, [] { return false; });
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] late joiner: " << late.BuildReportJson();
		System::PrintDiagnosticLine(line.str());
	}
	if (late.GetState() != Channel::State::Failed || late.GetLastError() != "session gone") {
		return fail("a joiner on the deleted row did not fail with \"session gone\"");
	}
	{
		std::ostringstream line;
		line << "[net-directory-signal-probe] PASS";
		System::PrintDiagnosticLine(line.str());
	}
	return 0;
}

/// The headless half of the join list: one directory GET plus one 2 s LAN browse window, then the
/// merged rows exactly as the join screen would render them. Exit 1 when the directory is
/// configured but no reply arrived.
int RunNetDirectoryList() {
	std::string reason;
	// Rows carry the identity NetMatchService::Start computes, which pins the default dt first.
	g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);
	NetIdentityManifest manifest;
	NetIdentityBuildOptions identityOptions;
	identityOptions.buildId = "stage2-p2d-local";
	identityOptions.sessionRulesTag = "peer-session-plane-v1";
	NetIdentity::StampOptionsForTarget(identityOptions, false);
	if (!NetIdentity::BuildCurrentManifest(manifest, &reason, identityOptions)) {
		{
			std::ostringstream line;
			line << "[net-directory-list] identity manifest failed: " << reason;
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}
	NetDirectoryLocalIdentity local;
	local.networkProtocolVersion = manifest.networkProtocolVersion;
	local.lockstepCodecVersion = manifest.deterministicConfig.lockstepCodecVersion;
	local.controllerFrameVersion = manifest.controllerFrameVersion;
	local.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
	local.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
	NetIdentityManifest worldManifest;
	NetIdentityBuildOptions worldOptions = identityOptions;
	NetIdentity::StampOptionsForTarget(worldOptions, true);
	NetDirectoryLocalIdentity worldLocal;
	if (NetIdentity::BuildCurrentManifest(worldManifest, &reason, worldOptions)) {
		worldLocal.networkProtocolVersion = worldManifest.networkProtocolVersion;
		worldLocal.lockstepCodecVersion = worldManifest.deterministicConfig.lockstepCodecVersion;
		worldLocal.controllerFrameVersion = worldManifest.controllerFrameVersion;
		worldLocal.sessionIdentityHash = NetIdentity::HashHex(worldManifest.sessionIdentityHash);
		worldLocal.moduleManifestHash = NetIdentity::HashHex(worldManifest.moduleManifestHash);
	}

	const std::string& baseUrl = g_SettingsMan.GetSessionDirectoryUrl();
	NetDirectoryClient directory;
	directory.Configure(baseUrl, baseUrl.empty() ? std::string() : g_SettingsMan.GetOrCreateSessionDirectoryInstallKey(), g_SettingsMan.GetSessionDirectoryCertSha256());

	NetLanDiscovery browser;
	std::string browseError;
	(void)browser.StartBrowser(&browseError);

	const auto nowMs = [] {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	};
	const uint64_t begin = nowMs();
	while (nowMs() - begin < 2000) {
		const uint64_t now = nowMs();
		if (browser.IsBrowsing()) {
			browser.Tick(now);
		}
		directory.PollList(now);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const std::vector<NetLanHostInfo> lan = browser.IsBrowsing() ? browser.GetHosts(nowMs()) : std::vector<NetLanHostInfo>{};
	browser.Stop();

	if (!baseUrl.empty() && (directory.ListReplies() == 0 || !directory.ListError().empty())) {
		{
			std::ostringstream line;
			line << "[net-directory-list] directory unreachable: " << (directory.ListError().empty() ? "no reply in the window" : directory.ListError());
			System::PrintDiagnosticErrorLine(line.str());
		}
		return 1;
	}

	const std::vector<NetDirectoryClient::GameRow> rows = NetDirectoryClient::MergeGameLists(lan, directory.Rows(), local, worldLocal.lockstepCodecVersion != 0 ? &worldLocal : nullptr);
	for (const NetDirectoryClient::GameRow& row : rows) {
		{
			std::ostringstream line;
			line << "[net-directory-list] source=" << row.source << " name=\"" << row.name << "\" activity=\"" << row.activity << "\" mode=\"" << row.mode
			     << "\" players=" << row.players << " address=" << row.address << ":" << row.port
			     << " joinable=" << (row.joinable ? "yes" : "no") << " reason=" << (row.reason.empty() ? "-" : row.reason);
			System::PrintDiagnosticLine(line.str());
		}
	}
	return 0;
}

/// Whether an argument starts a run whose Lua-side world must agree with another run's: a net session, a hash trace, a replay, a controller log or the determinism check.
static bool IsDeterministicRunArgument(const std::string& argument) {
	static const std::array<std::string, 13> c_Arguments = {"-deterministic-gc", "-tick-hashes", "-net-host", "-net-join", "-net-dedicated", "-net-lockstep", "-net-match", "-net-match-service-e2e", "-net-replay", "-net-replay-out", "-controller-log-out", "-controller-log-in", "-determinism-selftest-perturb"};
	return std::find(c_Arguments.begin(), c_Arguments.end(), argument) != c_Arguments.end();
}

/// <summary>
/// The headless port-map probe: request a UDP mapping for the game port through the
/// NAT-PMP -> PCP -> UPnP chain (optionally against the -net-port-map-gateway/-net-port-map-igd
/// test seams), print the result, then delete the mapping. Returns 0 only when one method mapped.
/// </summary>
int RunNetPortMapProbe() {
	NetPortMap mapper;
	mapper.Request(s_netPort, NetPortMap::c_DefaultLeaseS, NetPortMap::ProbeOverrides());
	const auto nowMs = [] {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	};
	const uint64_t begin = nowMs();
	while (!mapper.Done() && nowMs() - begin < NetPortMap::c_MapBudgetMs + NetPortMap::c_ReleaseBudgetMs) {
		mapper.Update(nowMs());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const NetPortMap::Result& result = mapper.GetResult();
	const bool mapped = mapper.Mapped();
	if (mapped) {
		{
			std::ostringstream line;
			line << "[net-port-map-probe] mapped " << result.externalIp << ":" << result.externalPort
			     << " via " << NetPortMap::MethodName(result.method) << " lease_s=" << result.leaseS;
			System::PrintDiagnosticLine(line.str());
		}
	} else {
		{
			std::ostringstream line;
			line << "[net-port-map-probe] no mapping: " << (result.error.empty() ? "still pending at the budget" : result.error);
			System::PrintDiagnosticLine(line.str());
		}
	}
	mapper.Release();
	{
		std::ostringstream line;
		line << "[net-port-map-probe] " << (mapped ? "PASS" : "FAIL");
		System::PrintDiagnosticLine(line.str());
	}
	return mapped ? 0 : 1;
}

/// <summary>
/// Implementation of the main function.
/// </summary>
int main(int argc, char** argv) {
	FloatingPointEnvironment::Initialize();
	bool netMatchSelfTest = false;
	bool netSeatSuccessionSelfTest = false;
	bool netSeatAdmissionSelfTest = false;
	bool netRejoinGridSelfTest = false;
	bool netMatchLobbyLifecycleSelfTest = false;
	bool netMatchLeaveCatchUpSelfTest = false;
	for (int i = 1; i < argc; ++i) {
		if (argv[i] != nullptr && std::string(argv[i]) == "-rotate-primitive-selftest") {
			return RotatePrimitiveSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-lua-numeric-policy-selftest") {
			return LuaStateWrapper::RunNumericPolicySelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-fp-environment-selftest") {
			return FloatingPointEnvironment::RunSelfTest() && LuaStateWrapper::RunFloatingPointCallbackSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-deterministic-math-selftest") {
			return LuaStateWrapper::RunDeterministicMathSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-fp-environment-native-drift-selftest") {
			return LuaStateWrapper::RunFloatingPointCallbackSelfTest(1) ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-fp-environment-error-drift-selftest") {
			return LuaStateWrapper::RunFloatingPointCallbackSelfTest(2) ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-fp-environment-capture-drift-selftest") {
			return LuaStateWrapper::RunFloatingPointCallbackSelfTest(3) ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-sim-checksum-selftest") {
			return SimChecksum::RunRowBlockSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-cow-checkpoint-selftest") {
			return RTE::RunCheckpointImageSelfTest() ? 0 : 1;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-float-text-selftest") {
			return FloatTextSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-combo-key-selftest") {
			return GUIManager::RunComboKeyCommitSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-rteerror-selftest") {
			return RTEError::RunAssertPolicySelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-module-version-selftest") {
			return DataModule::RunVersionGuardSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-ext-validate-version-selftest") {
			return DataModule::RunExtValidateVersionSelfTest();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-path-prefix-selftest") {
			return PresetMan::RunPathPrefixSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		// -limb-path-selftest runs after modules load: AHuman/LimbPath construction needs the entity pools.
		if (argv[i] != nullptr && std::string(argv[i]) == "-menu-automation-selftest") {
			return MenuAutomation::RunSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-settings-preferences-selftest") {
			return SettingsMan::RunNetworkPreferencesSelfTest();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-controller-frame-selftest") {
			return ControllerFrameSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-preview-event-ledger-selftest") {
			return PreviewEventLedger::RunSelfTest() ? 0 : 1;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-protocol-selftest") {
			return NetProtocolSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-identity-selftest") {
			return NetIdentitySelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-session-selftest") {
			return NetSessionSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-selftest") {
			return NetLockstepSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-session-plane-selftest") {
			return NetSessionPlaneSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-input-acceptance-selftest") {
			return NetLockstepSelfTest::RunAcceptance();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-input-acceptance-steady-selftest") {
			return NetLockstepSelfTest::RunAcceptanceSteady();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-first-start-selftest") {
			return NetLockstepSelfTest::RunFirstStart();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-ordering-selftest") {
			return NetLockstepSelfTest::RunOrdering();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-hold-heartbeat-selftest") {
			return NetLockstepSelfTest::RunHoldHeartbeat();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-seat-log-selftest") {
			return NetLockstepSelfTest::RunSeatLog();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-released-claims-selftest") {
			return NetLockstepSelfTest::RunReleasedClaims();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-release-paths-selftest") {
			return NetLockstepSelfTest::RunReleasePaths();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-seat-succession-selftest") {
			netSeatSuccessionSelfTest = true;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-lockstep-seat-admission-selftest") {
			netSeatAdmissionSelfTest = true;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-match-selftest") {
			if (NetMatchSelfTest::RunBeforeInitialization() != 0) return EXIT_FAILURE;
			netMatchSelfTest = true;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-cross-capabilities") {
			std::cout << "[cross-capabilities] " << nlohmann::json{{"peer_limit", NetMatchConfigUtil::c_MaxPeerCount},
			    {"player_slots", Players::MaxPlayerCount}, {"team_members", false}, {"schema", 1}}.dump() << std::endl;
			return EXIT_SUCCESS;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-match-leave-catch-up-selftest") {
			netMatchSelfTest = true;
			netMatchLeaveCatchUpSelfTest = true;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-match-lobby-lifecycle-selftest") {
			netMatchSelfTest = true;
			netMatchLobbyLifecycleSelfTest = true;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-auth-selftest") {
			return NetAuthSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-admission-selftest") {
			return NetAdmissionSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-reconnect-selftest") {
			return NetReconnectSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-reconnect-session-selftest") {
			return NetReconnectSessionSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-directory-selftest") {
			return NetDirectorySelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-rejoin-matrix-selftest") {
			return NetRejoinMatrixSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-roster-selftest") {
			return NetSeatRosterSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-rejoin-grid-selftest") {
			netRejoinGridSelfTest = true;
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-world-join-selftest") {
			return NetWorldJoinSelfTest::Run();
		}
		if (argv[i] != nullptr) {
			const std::string flag = argv[i];
			if (flag.rfind("-net-world-", 0) == 0 && flag.size() > 12 && flag.find("-selftest") != std::string::npos) {
				return NetWorldJoinSelfTest::RunCase(flag.c_str());
			}
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-p2p-selftest") {
			return GnsP2PSelfTest::Run(std::vector<std::string>(argv + i + 1, argv + argc));
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-port-map-selftest") {
			return NetPortMapSelfTest::Run();
		}
		if (argv[i] != nullptr && std::string(argv[i]) == "-net-discovery-selftest") {
			// A beacon and a browser over the loopback broadcast: the browser must list the host.
			NetLanDiscovery beacon;
			NetLanDiscovery browser;
			std::string error;
			int exitCode = 1;
			if (!browser.StartBrowser(&error) ||
			    !beacon.StartBeacon(42120, "SelftestHost", "P4 Alpha Duel", "pvp-skirmish", 1, 4, &error)) {
				{
					std::ostringstream line;
					line << "[net-discovery-selftest] FAIL: " << error;
					System::PrintDiagnosticErrorLine(line.str());
				}
				return 1;
			}
			for (uint64_t nowMs = 0; nowMs <= 3000 && exitCode != 0; nowMs += 50) {
				beacon.Tick(nowMs);
				browser.Tick(nowMs);
				for (const NetLanHostInfo& host: browser.GetHosts(nowMs)) {
					if (host.port == 42120 && host.hostName == "SelftestHost" && host.maxPlayers == 4 && host.mode == "pvp-skirmish") {
						exitCode = 0;
						break;
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			{
				std::ostringstream line;
				line << "[net-discovery-selftest] " << (exitCode == 0 ? "PASS" : "FAIL");
				System::PrintDiagnosticLine(line.str());
			}
			return exitCode;
		}
	}

	// Determinism-check mode is a pre-init orchestrator: it spawns child game processes and diffs
	// their JSON traces, so it must short-circuit before SDL / engine bootstrapping.
	if (DeterminismCheck::IsRequested(argc, argv)) {
		return DeterminismCheck::Run(argc, argv);
	}

	// The Lua state count is a build constant, so the flag only reports that it no longer chooses one.
	for (int i = 1; i < argc; ++i) {
		if (argv[i] != nullptr && std::string(argv[i]) == "-num-lua-states" && i + 1 < argc) {
			s_retiredLuaStateCountFlag = true;
			++i;
		}
	}

	// Decided before LuaMan starts, so its startup line names the collector this run uses.
	for (int i = 1; i < argc; ++i) {
		if (argv[i] != nullptr && IsDeterministicRunArgument(argv[i])) {
			LuaMan::SetDeterministicCollection(true);
			break;
		}
	}

	// Headless: -tick-hashes (the determinism trace mode, set on every -determinism-check child)
	// has nothing worth displaying, so create the window hidden. -headless forces it; -headed forces
	// a visible window. WindowMan reads CCCP_HEADLESS.
	{
		bool headless = false;
		for (int i = 1; i < argc; ++i) {
			if (argv[i] == nullptr) {
				continue;
			}
			const std::string arg = argv[i];
			if (arg == "-tick-hashes" || arg == "-headless" || arg == "-net-host" || arg == "-net-dedicated" || arg == "-net-join" || arg == "-net-lockstep" || arg == "-net-match" || arg == "-net-match-service-e2e" || arg == "-net-directory-probe" || arg == "-net-directory-signal-probe" || arg == "-net-directory-list" || arg == "-net-directory-selftest" || arg == "-net-port-map-probe" || arg == "-net-join-session") {
				headless = true;
			} else if (arg.size() > 9 && arg.compare(arg.size() - 9, 9, "-selftest") == 0) {
				// A selftest never needs a visible window; a bare launch from a worker shell must not raise one.
				headless = true;
			} else if (arg == "-headed") {
				headless = false;
				break;
			}
		}
		if (headless) {
#ifdef _WIN32
			_putenv_s("CCCP_HEADLESS", "1");
			// Suppress the CRT's modal abort()/assert dialogs so an automated run exits to the log
			// instead of blocking on a message box no one is there to dismiss.
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
			_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#elif defined(__APPLE__)
			// macOS: SDL's offscreen driver loads no GL on Darwin, so use a hidden real-GL window instead.
			setenv("CCCP_HEADLESS", "1", 1);
#else
			setenv("SDL_VIDEODRIVER", "offscreen", 1);
#endif
		}
		// Every harness and self-test run proves the session plane's lock discipline as it plays.
		if (headless || std::getenv("CCCP_HEADLESS") != nullptr) NetLockstepPlane::ArmChecks(true);
	}

#ifdef __APPLE__
	// Before SDL creates NSApp: its first event pump handles the launch event that decides window restoration.
	AppleRegisterApplicationDefaults();
#endif

	install_allegro(SYSTEM_NONE, &errno, std::atexit);
	loadpng_init();

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD );
	for (int i = 1; i < argc; ++i) {
		if (argv[i] != nullptr && std::string(argv[i]) == "-joystick-updater-selftest") {
			const bool passed = UInputMan::RunJoystickUpdaterSelfTest();
			UInputMan::StopJoystickUpdater();
			SDL_Quit();
			return passed ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
	SDL_SetHint("SDL_ALLOW_TOPMOST", "0");
	SDL_HideCursor();

	if (std::filesystem::exists("Base.rte/gamecontrollerdb.txt")) {
		SDL_AddGamepadMappingsFromFile("Base.rte/gamecontrollerdb.txt");
	}

#ifdef WIN32
	// Stops framespiking from our child threads being sat on for too long
	// TODO: use a better thread system that'll do what we want ASAP instead of letting the OS schedule all over us
	// Disabled for now because windows is great and this means when the game lags out it freezes the entire computer. Which we wouldn't expect with anything but REALTIME priority.
	// Because apparently high priority class is preferred over "processing mouse input"?!
	// SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif // WIN32

	// argv[0] actually unreliable for exe path and name, because of course, why would it be, why would anything be simple and make sense.
	// Just use it anyway until some dumb edge case pops up and it becomes a problem.
	System::Initialize(argv[0]);
	SeedRNG();
	InstallRNGDrawTraceIfArmed();

	TelemetryBundle::Initialize("unavailable");
	ConfigureMenuScriptInput(argc, argv);
	InitializeManagers();
	ScenarioRunner::SetStallEventPoll(&PollStallEventsForCapture);
	// Same arming condition as the probe itself, so only a probe run pumps the panel from a stall.
	const char* netUiProbeScript = std::getenv("CC_TEST_NET_UI_SCRIPT");
	ScenarioRunner::SetLockstepStallUIProbeArmed(netUiProbeScript != nullptr && *netUiProbeScript != '\0');

	const bool mainArgsValid = HandleMainArgs(argc, argv);
	// Only a menu script, the UI probe or a harness's screen watches read what the renderer drew.
	const char* screenWatches = std::getenv("CCCP_TEST_SCREEN_WATCHES");
	SetPanelDrawRecording(!s_menuScriptPath.empty() || (netUiProbeScript != nullptr && *netUiProbeScript != '\0') || (screenWatches != nullptr && *screenWatches != '\0'));
	// The instruments this process runs, from the levers it was launched with: every frame's cost receipt names each one's measured cost.
	{
		const auto armed = [](const char* name) { const char* value = std::getenv(name); return value && *value; };
		unsigned long long dumpFrom = 0, dumpTo = 0;
		const char* dump = std::getenv("CC_SIM_DUMP");
		HarnessCost::SetEnabled(HarnessCost::SimDump, dump && std::sscanf(dump, "%llu:%llu", &dumpFrom, &dumpTo) == 2 && dumpTo >= dumpFrom);
		HarnessCost::SetEnabled(HarnessCost::TickEnd, !s_netLiveTickHashPath.empty());
		HarnessCost::SetEnabled(HarnessCost::FullState, s_netFullStateEvery != 0);
		HarnessCost::SetEnabled(HarnessCost::Census, s_memoryCensusTicks != 0 || s_memoryCensusSeconds != 0);
		// A preview's steps are recorded only for the feel recorder or the fidelity probe; the player's own prediction setting arms neither.
		HarnessCost::SetEnabled(HarnessCost::PreviewFidelity, armed("CCCP_TEST_PREVIEW_FIDELITY") || FrameMan::FeelRecordingEnabled());
		HarnessCost::SetEnabled(HarnessCost::ScreenWatches, armed("CCCP_TEST_SCREEN_WATCHES") || !s_menuScriptPath.empty() || (netUiProbeScript != nullptr && *netUiProbeScript != '\0'));
		HarnessCost::SetEnabled(HarnessCost::Recorder, !s_recordVideoDirectory.empty());
		HarnessCost::SetEnabled(HarnessCost::ControllerTrace, ScenarioRunner::IsControllerDebugDumpEnabled());
		HarnessCost::SetEnabled(HarnessCost::FeelRecorder, FrameMan::FeelRecordingEnabled());
	}
	if (CaptureSentinel::Enabled()) CaptureSentinel::Enable();
	if (s_netDedicated && !s_netMatchServiceE2E) {
		s_netWorldDaemon = true;
		s_netMatchServiceE2E = true;
		if (!s_netMatchServicePresetExplicit) {
			s_netMatchServiceE2EPreset = "Persistent World";
		}
	}
	if (s_netMatchServiceE2EPreset == "Persistent World") {
		s_netPersistentWorld = true;
	}
	const auto* gpu = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
	TelemetryBundle::SetGpuDescription(gpu ? gpu : "unavailable");
	if (!mainArgsValid) return ShutDown(EXIT_FAILURE);
	FrameMan::ApplyHeadlessPresentationDefault();

	// The managers are up, so the recorder takes the run's own resolution from the first frame on.
	if (!s_recordVideoDirectory.empty()) {
		std::string recorderError;
		if (!FrameRecorder::Instance().Start(s_recordVideoDirectory, s_recordVideoFps, &recorderError)) {
			{
				std::ostringstream line;
				line << "[record-video] refused: " << recorderError;
				System::PrintDiagnosticErrorLine(line.str());
			}
			return ShutDown(EXIT_FAILURE);
		}
		{
			std::ostringstream line;
			line << "[record-video] recording to " << s_recordVideoDirectory << " at " << s_recordVideoFps << " fps";
			System::PrintDiagnosticLine(line.str());
		}
	}

	// The chat script drives session traffic — only a headless e2e match may carry it.
	if (!s_netChatScriptPath.empty() && !s_netMatchServiceE2E) {
		{
			std::ostringstream line;
			line << "[net-chat-script] requires -net-match-service-e2e";
			System::PrintDiagnosticErrorLine(line.str());
		}
		return ShutDown(EXIT_FAILURE);
	}

	// The -net-port-map flags are only parsed by HandleMainArgs, so the run override and the
	// probe seams take effect here, before the probe dispatch and any match Start can read them.
	g_SettingsMan.SetNetworkPortMapEnableOverride(s_netPortMapCli);
	NetPortMap::SetProbeOverrides(s_netPortMapGateway, s_netPortMapIgd);

	if (ScenarioRunner::GetArgs().renderWindowScriptsSelfTest) {
		return ShutDown(LocalPrediction::RunRenderWindowScriptsSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	if (ScenarioRunner::GetArgs().textWrapSelfTest) {
		return ShutDown(g_FrameMan.RunTextWrapSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	if (s_frameRecorderSelfTest) {
		return ShutDown(RunFrameRecorderSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	if (s_cameraNullSceneSelfTest) {
		// The scroll update runs from the sim tick, which keeps ticking for a frame after an activity
		// ends or an activity launch fails. With no scene it must do nothing rather than fault, and
		// this is the one moment the engine is up with nothing loaded.
		if (g_SceneMan.GetScene() != nullptr) {
			{
				std::ostringstream line;
				line << "[camera-null-scene-selftest] FAIL: this case needs a scene-less engine";
				System::PrintDiagnosticErrorLine(line.str());
			}
			return ShutDown(EXIT_FAILURE);
		}
		for (int screenId = 0; screenId < c_MaxScreenCount; ++screenId) {
			g_CameraMan.Update(screenId);
		}
		{
			std::ostringstream line;
			line << "[camera-null-scene-selftest] PASS";
			System::PrintDiagnosticLine(line.str());
		}
		return ShutDown(EXIT_SUCCESS);
	}

	g_PresetMan.LoadAllDataModules();
	// The device icons are presets, so they load once the modules have, before any path that can draw a menu.
	if (!System::IsInExternalModuleValidationMode()) g_UInputMan.LoadDeviceIcons();
	SpendPreMatchHistory(s_preMatchHistoryObjects);
	PlayPreMatchActivity(s_preMatchActivity);
	// The modules are loaded and will not change under this process: read them once here, off the game
	// thread, so the multiplayer landing and Create Lobby do not each walk every module on their frame.
	NetIdentity::PrimeManifest();
	if (!ContentFile::WaitForPendingSounds(LoadingScreen::LoadingSplashProgressReport)) return ShutDown(EXIT_FAILURE);
	if (netRejoinGridSelfTest) {
		NetMatchService::Destruct();
		const int result = NetRejoinMatrixSelfTest::RunGrid();
		NetMatchService::Construct();
		return ShutDown(result);
	}
	if (netSeatSuccessionSelfTest) return ShutDown(NetLockstepSelfTest::RunSeatSuccession());
	if (netSeatAdmissionSelfTest) return ShutDown(NetLockstepSelfTest::RunSeatAdmission());
	if (netMatchSelfTest) {
		NetMatchService::Destruct();
		const int result = netMatchLeaveCatchUpSelfTest ? NetMatchSelfTest::RunLeaveCatchUp() :
		                   netMatchLobbyLifecycleSelfTest ? NetMatchSelfTest::RunLobbyLifecycle() : NetMatchSelfTest::Run();
		NetMatchService::Construct();
		return ShutDown(result);
	}

	if (!s_netIdentityDumpPath.empty()) {
		NetIdentityManifest manifest;
		std::string error;
		const int exitCode = NetIdentity::DumpCurrentManifestJson(s_netIdentityDumpPath, &error, &manifest) ? 0 : 1;
		if (exitCode == 0) {
			{
				std::ostringstream line;
				line << "[net-identity-dump] wrote " << s_netIdentityDumpPath
				     << " session_identity_hash=" << NetIdentity::HashHex(manifest.sessionIdentityHash)
				     << " hash_duration_ms=" << manifest.hashDurationMs;
				System::PrintDiagnosticLine(line.str());
			}
		} else {
			{
				std::ostringstream line;
				line << "[net-identity-dump] failed: " << error;
				System::PrintDiagnosticErrorLine(line.str());
			}
		}
		return ShutDown(exitCode);
	}

	if (!s_netDirectoryProbeUrl.empty()) {
		const int exitCode = RunNetDirectoryProbe(s_netDirectoryProbeUrl, s_netDirectoryProbeCertSha256);
		return ShutDown(exitCode);
	}

	if (!s_netDirectorySignalProbeUrl.empty()) {
		return ShutDown(RunNetDirectorySignalProbe(s_netDirectorySignalProbeUrl, s_netDirectorySignalProbeCertSha256));
	}

	if (s_netDirectoryList) {
		return ShutDown(RunNetDirectoryList());
	}

	if (s_netPortMapProbe) {
		return ShutDown(RunNetPortMapProbe());
	}

	if (NetSessionCliRequested()) {
		const int exitCode = RunNetSessionCli();
		return ShutDown(exitCode);
	}

	if (s_netMatchServiceE2E) {
		const int exitCode = RunNetMatchServiceE2E();
		return ShutDown(exitCode);
	}

	if (!s_netReplayVerifyPath.empty()) {
		NetReplayVerifyReport report;
		NetMatchReplayReader::Verify(s_netReplayVerifyPath, report);
		const std::string json = report.ToJson();
		{
			std::ostringstream line;
			line << "[net-replay-verify] " << json;
			System::PrintDiagnosticLine(line.str());
		}
		if (s_netReplayDumpTo >= s_netReplayDumpFrom) {
			// The recorded wire, tick by tick: what every peer's sim applied.
			NetMatchReplayReader reader;
			std::string openError;
			if (reader.Open(s_netReplayVerifyPath, &openError)) {
				NetLockstepFrame record;
				bool eof = false;
				while (reader.ReadFrame(record, eof, nullptr)) {
					if (record.targetFrame < s_netReplayDumpFrom || record.targetFrame > s_netReplayDumpTo) {
						continue;
					}
					for (const ControllerFrame& frame: record.frames) {
						{
							std::ostringstream line;
							line << "[net-replay-dump] frame=" << record.targetFrame << " uid=" << frame.actorUniqueID << " mode=" << static_cast<int>(frame.inputMode)
							     << " player=" << static_cast<int>(frame.playerRaw) << " flags=0x" << std::hex << static_cast<int>(frame.flags) << std::dec
							     << " flip=" << (frame.IsActorHFlipped() ? 1 : 0) << " aim_intent=" << (frame.HasAimIntent() ? 1 : 0) << " flip_intent=" << (frame.HasFlipIntent() ? 1 : 0)
							     << " aim=" << std::hexfloat << frame.aimAngle << std::defaultfloat << " fg=" << frame.equippedFGUniqueID << " bg=" << frame.equippedBGUniqueID
							     << " device=" << static_cast<int>(frame.deviceClass) << " aim_speed=" << frame.digitalAimSpeed;
							System::PrintDiagnosticLine(line.str());
						}
					}
					for (const NetGameCommand& command: record.commands) {
						{
							std::ostringstream line;
							line << "[net-replay-dump] frame=" << record.targetFrame << " command=" << NetGameCommandTypeName(NetGameCommandTypeOf(command.payload)) << " sender=" << static_cast<int>(command.senderPeerId);
							if (const NetGameAIEquip* equip = std::get_if<NetGameAIEquip>(&command.payload)) {
								line << " uid=" << equip->actorUID << " op=" << static_cast<int>(equip->op) << " group=" << equip->group << " preset=" << equip->presetName;
							} else if (const NetGameAIOrder* order = std::get_if<NetGameAIOrder>(&command.payload)) {
								line << " uid=" << order->actorUID << " op=" << static_cast<int>(order->op) << " x=" << order->x << " y=" << order->y << " target=" << order->targetUID;
							}
							System::PrintDiagnosticLine(line.str());
						}
					}
				}
			} else {
				{
					std::ostringstream line;
					line << "[net-replay-dump] " << openError;
					System::PrintDiagnosticErrorLine(line.str());
				}
			}
		}
		if (!ScenarioRunner::GetArgs().outPath.empty()) {
			std::string writeError;
			if (!WriteTextFile(ScenarioRunner::GetArgs().outPath, json + "\n", &writeError)) {
				{
					std::ostringstream line;
					line << "[net-replay-verify] could not write " << ScenarioRunner::GetArgs().outPath << ": " << writeError;
					System::PrintDiagnosticErrorLine(line.str());
				}
				return ShutDown(EXIT_FAILURE);
			}
		}
		return ShutDown(report.ok ? 0 : (report.truncated ? 2 : (report.corrupt ? 3 : 1)));
	}
	if (ScenarioRunner::GetArgs().scriptGraphSelfTest) {
		bool pass = g_LuaMan.RunScriptGraphSelfTest();
		pass = RunHarnessCaptureSelfTest() && pass;
		return ShutDown(pass ? 0 : 1);
	}
	if (ScenarioRunner::GetArgs().limbPathSelfTest) {
		return ShutDown(AHuman::RunLimbPathTravelSpeedSelfTest() ? 0 : 1);
	}
	if (ScenarioRunner::GetArgs().modApiShimsSelfTest) {
		return ShutDown(RunModApiShimsSelfTest() ? 0 : 1);
	}
	if (ScenarioRunner::GetArgs().saveRefusalDiagnosisSelfTest) {
		return ShutDown(g_ActivityMan.RunSaveRefusalDiagnosisSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	if (!s_netReplayInPath.empty()) {
		const int exitCode = RunNetReplayPlayback();
		return ShutDown(exitCode);
	}

	int scenarioExitCode = 0;

	if (!System::IsInExternalModuleValidationMode()) {
		if (g_ConsoleMan.LoadWarningsExist()) {
			g_ConsoleMan.PrintString("WARNING: Encountered non-fatal errors during module loading!\nSee \"LogLoadingWarning.txt\" for information.");
			g_ConsoleMan.SaveLoadWarningLog("LogLoadingWarning.txt");
			// Open the console so the user is aware there are loading warnings.
			g_ConsoleMan.SetEnabled(true);
		} else {
			// Delete an existing log if there are no warnings so there's less junk in the root folder.
			if (std::filesystem::exists(System::GetWorkingDirectory() + "LogLoadingWarning.txt")) {
				std::remove("LogLoadingWarning.txt");
			}
		}

		if (ScenarioRunner::IsActive()) {
			// Pin the canonical deterministic sim config before anything loads or runs.
			ScenarioRunner::ApplyDeterministicConfig();
			GnsTransport netLockstepTransport;
			NetSession netLockstepSession;
			NetLockstepCoordinator netLockstepCoordinator;
			NetMatchRunner netMatchRunner;
			std::string netLockstepSetupError;
			const bool netLockstepPrepared = !NetGameplayRequested() || PrepareNetLockstepScenario(netLockstepTransport, netLockstepSession, netLockstepCoordinator, netMatchRunner, &netLockstepSetupError);
			// CLI direct-launch into a scenario: skip the menu, start the named GAScripted activity
			// directly, run the loop, then finalize the JSON report + exit code.
			std::string controllerLogError;
			const bool controllerLogPrepared = ScenarioRunner::PrepareControllerLog(&controllerLogError);
			const std::string presetName = ScenarioRunner::ResolvePresetName(ScenarioRunner::GetArgs().scenario);
			const Entity* presetEntity = g_PresetMan.GetEntityPreset("GAScripted", presetName);
			const Activity* presetActivity = dynamic_cast<const Activity*>(presetEntity);
			int startResult = -1;
			if (!controllerLogPrepared) {
				{
					std::ostringstream line;
					line << "[scenario] controller log setup failed: " << controllerLogError;
					System::PrintDiagnosticErrorLine(line.str());
				}
			}
			if (!netLockstepPrepared) {
				{
					std::ostringstream line;
					line << (s_netMatch ? "[net-match]" : "[net-lockstep]") << " setup failed: " << netLockstepSetupError;
					System::PrintDiagnosticErrorLine(line.str());
				}
			}
			if (controllerLogPrepared && netLockstepPrepared && presetActivity) {
				const std::string& sceneName = presetActivity->GetSceneName();
				if (!sceneName.empty()) {
					g_SceneMan.SetSceneToLoad(sceneName, true, false);
				}
				startResult = g_ActivityMan.StartActivity("GAScripted", presetName);
			} else if (controllerLogPrepared && netLockstepPrepared) {
				{
					std::ostringstream line;
					line << "[scenario] no preset \"" << presetName << "\" of class GAScripted";
					System::PrintDiagnosticErrorLine(line.str());
				}
			}

			if (!controllerLogPrepared || !netLockstepPrepared) {
				scenarioExitCode = 1;
			} else if (startResult < 0) {
				{
					std::ostringstream line;
					line << "[scenario] failed to start scenario \"" << presetName << "\"";
					System::PrintDiagnosticErrorLine(line.str());
				}
				scenarioExitCode = 1;
			} else {
				// A fixture may have no metrics calls of its own. Explicit tick-hash
				// collection still records that run; scripts which began one keep it.
				if (ScenarioRunner::GetArgs().tickHashes && g_MetricsCollector.GetCurrentRun().scenario.empty()) {
					g_MetricsCollector.BeginHostRun(ScenarioRunner::GetArgs().scenario, ScenarioRunner::GetArgs().seed);
				}
				RunGameLoop();
				CheckRequiredProbesCompleted();
				scenarioExitCode = ScenarioRunner::FinalizeAndGetExitCode();
				if (s_netReplayExitCode != 0 && scenarioExitCode == 0) {
					scenarioExitCode = s_netReplayExitCode;
				}
				if (NetGameplayRequested()) {
					(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, 0);
					netLockstepCoordinator.Complete(scenarioExitCode == 0 ? "scenario complete" : "scenario failed");
					// The completion needs the same goodbye flush as an interactive capped round.
					(void)ScenarioRunner::DrainLockstepRelay(c_CappedStopDrainMs, c_CappedStopLingerMs);
				}
			}
			if (NetGameplayRequested()) {
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				std::string reportError;
				const std::string report = BuildNetLockstepReportJson(netLockstepSession, netLockstepCoordinator, netMatchRunner, scenarioExitCode, netLockstepSetupError);
				if (!WriteTextFile(s_netLockstepReportPath, report, &reportError)) {
					{
						std::ostringstream line;
						line << (s_netMatch ? "[net-match]" : "[net-lockstep]") << " report failed: " << reportError;
						System::PrintDiagnosticErrorLine(line.str());
					}
					scenarioExitCode = 1;
				} else if (!s_netLockstepReportPath.empty()) {
					{
						std::ostringstream line;
						line << (s_netMatch ? "[net-match]" : "[net-lockstep]") << " wrote report: " << s_netLockstepReportPath;
						System::PrintDiagnosticLine(line.str());
					}
					if (const std::string stats = LocalPrediction::DescribeStats(); !stats.empty()) {
						{
							std::ostringstream line;
							line << "[localpred] " << stats;
							System::PrintDiagnosticLine(line.str());
						}
					}
				}
			}
		} else {
			// Interactive mode: a stalled lockstep match draws the "waiting for peer" screen.
			ScenarioRunner::SetLockstepStallOverlayEnabled(true);
			bool loadedSavedGame = false;
			if (!s_loadGameName.empty()) {
				loadedSavedGame = g_ActivityMan.LoadAndLaunchGame(s_loadGameName);
				{
					std::ostringstream line;
					line << "[load-game] " << (loadedSavedGame ? "loaded " : "could not load ") << std::quoted(s_loadGameName);
					System::PrintDiagnosticLine(line.str());
				}
				if (!loadedSavedGame) {
					s_loadGameFailed = true;
					System::SetQuit(true);
				}
				if (loadedSavedGame && s_cowCheckpointAutosave) {
					// Recapture the restored instant before simulation advances. The
					// fixture's absolute two-tick cap can precede the saved tick.
					const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
					if (!g_ActivityMan.SaveAutosaveSnapshot("c0de-a1", tick) || !g_ActivityMan.WaitForAutosaveVerdict()) s_loadGameFailed = true;
					System::SetQuit(true);
				}
			}
			if (!loadedSavedGame && !g_ActivityMan.Initialize()) {
				RunMenuLoop();
			}

			// If the menu launched a multiplayer match with tracing armed, record the per-tick trace so the
			// menu-driven match can be sim-gated host vs client (same compare as the headless gate).
			const bool traceMenuMp = g_NetMatchService.WasEverStarted() && s_recordTickHashes && !ScenarioRunner::GetArgs().outPath.empty();
			if (traceMenuMp) {
				g_MetricsCollector.BeginHostRun("P4 Alpha Duel", ScenarioRunner::GetArgs().seed);
				g_MetricsCollector.SetRecordTickHashes(true);
			}

			RunGameLoop();

			// The menu-driven match writes the same lockstep counters the headless gates do, so a
			// lobby lane can say which side of the relay a stall was on.
			if (g_NetMatchService.WasEverStarted() && !s_netLockstepReportPath.empty()) {
				std::string reportError;
				if (!WriteTextFile(s_netLockstepReportPath, g_NetMatchService.BuildReportJson(), &reportError)) {
					{
						std::ostringstream line;
						line << "[menu-mp] could not write report: " << reportError;
						System::PrintDiagnosticErrorLine(line.str());
					}
				} else {
					{
						std::ostringstream line;
						line << "[menu-mp] wrote report: " << s_netLockstepReportPath;
						System::PrintDiagnosticLine(line.str());
					}
				}
			}

			if (traceMenuMp) {
				g_MetricsCollector.EndRun();
				const uint64_t cap = ScenarioRunner::GetArgs().maxTicks > 0 ? ScenarioRunner::GetArgs().maxTicks : 600;
				if (!s_menuMpTraceError.empty() || !s_menuTraceCoverage.CoversCap(g_MetricsCollector.GetTickHashCount(), cap)) {
					{
						std::ostringstream line;
						line << "[menu-mp] FAIL: collected " << g_MetricsCollector.GetTickHashCount() << " of " << cap << " requested ticks";
						System::PrintDiagnosticErrorLine(line.str());
					}
					g_MetricsCollector.SetResult(false);
					scenarioExitCode = 1;
				}
				g_MetricsCollector.SetNativeOutcome(scenarioExitCode == 0,
				                                   scenarioExitCode == 0 ? "the menu round ran to its cap" : "exit code " + std::to_string(scenarioExitCode) + (s_menuMpTraceError.empty() ? "" : ": " + s_menuMpTraceError),
				                                   static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
				const std::string& tracePath = ScenarioRunner::GetArgs().outPath;
				if (g_MetricsCollector.WriteReport(tracePath)) {
					{
						std::ostringstream line;
						line << "[menu-mp] wrote trace: " << tracePath;
						System::PrintDiagnosticLine(line.str());
					}
				} else {
					scenarioExitCode = 1;
				}
			}
		}
	}

	if (s_loadGameFailed) {
		scenarioExitCode = 1;
	}
	return ShutDown(scenarioExitCode);
}

#ifdef _WIN32
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) { return main(__argc, __argv); }
#endif
