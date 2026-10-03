#include "FrameRecorder.h"

#include "System.h"

#include "png.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace RTE {

	/// One encoder process reading raw frames on its standard input; nothing else of this process reaches it.
	class EncoderPipe {
	public:
		~EncoderPipe() { Close(0); }

		bool Open(const std::string& command, const std::string& logPath, std::string& error) {
#ifdef _WIN32
			SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
			HANDLE readEnd = nullptr;
			if (!CreatePipe(&readEnd, &m_Write, &inherit, 1 << 22)) return Fail(error, "CreatePipe");
			SetHandleInformation(m_Write, HANDLE_FLAG_INHERIT, 0);
			HANDLE log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			// Only the pipe and the log are handed down: an inherited socket would outlive this process in the encoder.
			HANDLE handles[2] = {readEnd, log};
			const DWORD handleCount = log != INVALID_HANDLE_VALUE ? 2 : 1;
			SIZE_T attributeBytes = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
			std::vector<unsigned char> attributeList(attributeBytes);
			auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeList.data());
			STARTUPINFOEXA startup{};
			startup.StartupInfo.cb = sizeof(startup);
			startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			startup.StartupInfo.hStdInput = readEnd;
			startup.StartupInfo.hStdOutput = log != INVALID_HANDLE_VALUE ? log : nullptr;
			startup.StartupInfo.hStdError = startup.StartupInfo.hStdOutput;
			PROCESS_INFORMATION process{};
			std::string mutableCommand = command;
			const bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) &&
			    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, handleCount * sizeof(HANDLE), nullptr, nullptr);
			startup.lpAttributeList = listed ? attributes : nullptr;
			const bool started = listed && CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
			    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
			if (listed) DeleteProcThreadAttributeList(attributes);
			CloseHandle(readEnd);
			if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
			if (!started) return Fail(error, "CreateProcess");
			CloseHandle(process.hThread);
			m_Process = process.hProcess;
			return true;
#else
			// An encoder that stops reading fails the next write instead of ending the process.
			std::signal(SIGPIPE, SIG_IGN);
			m_Pipe = popen((command + " 2>\"" + logPath + "\"").c_str(), "w");
			if (!m_Pipe) {
				error = "popen failed";
				return false;
			}
			return true;
#endif
		}

		bool Write(const unsigned char* data, std::size_t size) {
#ifdef _WIN32
			while (size > 0) {
				DWORD written = 0;
				if (!m_Write || !WriteFile(m_Write, data, static_cast<DWORD>(std::min<std::size_t>(size, 1 << 30)), &written, nullptr) || written == 0) return false;
				data += written;
				size -= written;
			}
			return true;
#else
			return m_Pipe && std::fwrite(data, 1, size, m_Pipe) == size;
#endif
		}

		/// Ends the input and waits for the encoder to write its file; the exit code, or -1 when it did not finish in time.
		int Close(int timeoutMs) {
#ifdef _WIN32
			if (m_Write) {
				CloseHandle(m_Write);
				m_Write = nullptr;
			}
			if (!m_Process) return m_Exit;
			if (WaitForSingleObject(m_Process, static_cast<DWORD>(timeoutMs)) == WAIT_OBJECT_0) {
				DWORD code = 0;
				GetExitCodeProcess(m_Process, &code);
				m_Exit = static_cast<int>(code);
			}
			CloseHandle(m_Process);
			m_Process = nullptr;
			return m_Exit;
#else
			if (m_Pipe) {
				m_Exit = pclose(m_Pipe);
				m_Pipe = nullptr;
			}
			(void)timeoutMs;
			return m_Exit;
#endif
		}

	private:
#ifdef _WIN32
		static bool Fail(std::string& error, const char* step) {
			error = std::string(step) + " failed with " + std::to_string(GetLastError());
			return false;
		}
		HANDLE m_Write = nullptr;
		HANDLE m_Process = nullptr;
#else
		std::FILE* m_Pipe = nullptr;
#endif
		int m_Exit = -1;
	};

	namespace {
		/// The processor time the calling thread has used, in nanoseconds: what its work took from the machine, without the time
		/// it spent blocked on a pipe or a disk.
		int64_t ThreadCpuNanoseconds() {
#ifdef _WIN32
			FILETIME created, exited, kernel, user;
			if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
			const auto ticks = [](const FILETIME& time) { return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
			return (ticks(kernel) + ticks(user)) * 100;
#else
			timespec now{};
			if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) return 0;
			return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
#endif
		}

		std::string FrameLeaf(std::size_t index) {
			std::ostringstream name;
			name << "frame-" << std::setw(6) << std::setfill('0') << index << ".png";
			return name.str();
		}

		// The fastest deflate level with the Sub filter: a recording is read once by the encoder, so speed beats size.
		bool SaveRgbPng(const std::string& path, const unsigned char* pixels, int width, int height) {
			std::FILE* file = std::fopen(path.c_str(), "wb");
			if (!file) return false;
			png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
			png_infop info = png ? png_create_info_struct(png) : nullptr;
			bool saved = false;
			if (png && info && !setjmp(png_jmpbuf(png))) {
				png_init_io(png, file);
				png_set_compression_level(png, 1);
				png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
				png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
				png_write_info(png, info);
				for (int row = 0; row < height; ++row) png_write_row(png, const_cast<png_bytep>(pixels + static_cast<std::size_t>(row) * width * 3));
				png_write_end(png, nullptr);
				saved = true;
			}
			png_destroy_write_struct(png ? &png : nullptr, info ? &info : nullptr);
			return std::fclose(file) == 0 && saved;
		}

		/// Writers enough to keep a 4K capture's rate on a desktop CPU without taking the engine's own cores.
		std::size_t WriterCount() {
			return std::clamp<std::size_t>(std::thread::hardware_concurrency() / 5, 2, 6);
		}
	} // namespace

	FrameRecorder& FrameRecorder::Instance() {
		static FrameRecorder recorder;
		return recorder;
	}

	long long FrameRecorder::SteadyNowMS() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	FrameRecorder::FrameRecorder() = default;

	FrameRecorder::~FrameRecorder() {
		Finish();
	}

	bool FrameRecorder::Start(const std::string& directory, int fps, std::string* error) {
		return Start(directory, fps, c_DefaultQueueBound, error);
	}

	bool FrameRecorder::Start(const std::string& directory, int fps, std::size_t queueBound, std::string* error) {
		const auto refuse = [error](const std::string& reason) {
			if (error) *error = reason;
			return false;
		};
		if (m_Enabled) return refuse("already recording to " + m_Directory);
		if (fps < 1 || fps > c_MaxFps) return refuse("frame rate " + std::to_string(fps) + " is outside 1-" + std::to_string(c_MaxFps));
		std::error_code code;
		if (!std::filesystem::is_directory(directory, code)) return refuse("no such output directory: " + directory);
		if (!std::filesystem::is_empty(directory, code) || code) return refuse("output directory is not empty: " + directory);
		const std::filesystem::path frames = std::filesystem::path(directory) / "frames";
		if (!std::filesystem::create_directory(frames, code) || code) return refuse("could not create " + frames.string());
		m_Index.open(std::filesystem::path(directory) / "frames.jsonl", std::ios::out);
		if (!m_Index) return refuse("could not open the frame index in " + directory);
		m_Events.open(std::filesystem::path(directory) / "events.jsonl", std::ios::out);
		if (!m_Events) return refuse("could not open the event index in " + directory);
		m_DroppedIndex.open(std::filesystem::path(directory) / "dropped.jsonl", std::ios::out);
		if (!m_DroppedIndex) return refuse("could not open the dropped-frame index in " + directory);

		m_Directory = directory;
		m_FramesDirectory = frames.string();
		m_Fps = fps;
		m_QueueBound = queueBound;
		m_Finished = false;
		m_StartedWallMS = SteadyNowMS();
		m_StartedUnixMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		m_Enabled = true;
		const char* encoder = std::getenv("CCCP_TEST_RECORD_ENCODER");
		const char* codec = std::getenv("CCCP_TEST_RECORD_CODEC");
		m_EncoderPath = encoder ? encoder : "";
		m_EncoderCodec = codec && *codec ? codec : "libx264";
		// An encoder takes the frames in order on one pipe, so it has one writer; PNGs encode on a pool.
		const std::size_t writers = m_EncoderPath.empty() ? WriterCount() : 1;
		for (std::size_t writer = 0; writer < writers; ++writer) m_Writers.emplace_back(&FrameRecorder::WriterLoop, this);
		return true;
	}

	void FrameRecorder::RecordEvent(const std::string& message) {
		if (!m_Enabled) return;
		const std::string row = nlohmann::json({{"wall_ms", SteadyNowMS()}, {"message", message}}).dump();
		std::lock_guard<std::mutex> lock(m_EventsMutex);
		if (m_Events.is_open()) m_Events << row << '\n' << std::flush;
	}

	// Called with the lock held; the pacer is the render thread's alone.
	bool FrameRecorder::DueAt(long long wallMS) {
		if (!m_PacerStarted) {
			m_PacerStarted = true;
			m_PacerOriginMS = wallMS;
		}
		const long long elapsed = wallMS - m_PacerOriginMS;
		if (elapsed < 0) return false;
		// The slot this wall time falls in at the capture rate; a slot is admitted once.
		const std::size_t slot = static_cast<std::size_t>((elapsed * m_Fps) / 1000);
		if (slot < m_Admitted) return false;
		m_Admitted = slot + 1;
		return true;
	}

	unsigned char* FrameRecorder::BeginFrame(long long wallMS, std::size_t bytes) {
		if (!m_Enabled || m_StagingHeld || bytes == 0) return nullptr;
		std::unique_lock<std::mutex> lock(m_Mutex);
		++m_Submitted;
		if (!DueAt(wallMS)) {
			++m_RateLimited;
			return nullptr;
		}
		if (m_Queue.size() >= m_QueueBound) {
			++m_Dropped;
			m_PendingDrops.emplace_back(wallMS, m_Admitted - 1);
			m_Wake.notify_one();
			return nullptr;
		}
		if (!m_Pool.empty()) {
			m_Staging = std::move(m_Pool.back());
			m_Pool.pop_back();
		}
		m_StagingSlot = m_Admitted - 1;
		lock.unlock();
		// From here to EndFrame the frame is read back on this thread: the recorder's cost to the frame it runs in.
		m_ReadbackSpan.emplace();
		m_Staging.resize(bytes);
		m_StagingHeld = true;
		return m_Staging.data();
	}

	void FrameRecorder::EndFrame(const FrameMeta& meta) {
		if (!m_StagingHeld) return;
		m_StagingHeld = false;
		if (m_ReadbackSpan) {
			HarnessCost::Charge(HarnessCost::Recorder, m_ReadbackSpan->Stop());
			m_ReadbackSpan.reset();
		}
		QueuedFrame frame;
		frame.pixels = std::move(m_Staging);
		frame.meta = meta;
		frame.slot = m_StagingSlot;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			frame.index = m_NextIndex++;
			m_Queue.push_back(std::move(frame));
		}
		m_Wake.notify_one();
	}

	void FrameRecorder::WriterLoop() {
		for (;;) {
			QueuedFrame frame;
			{
				std::unique_lock<std::mutex> lock(m_Mutex);
				m_Wake.wait(lock, [this] { return !m_Queue.empty() || !m_PendingDrops.empty() || m_Stopping; });
				if (m_Queue.empty() && m_PendingDrops.empty()) return;
				if (m_Queue.empty()) {
					lock.unlock();
					WritePendingDrops();
					continue;
				}
				frame = std::move(m_Queue.front());
				m_Queue.pop_front();
			}
			WritePendingDrops();
			// The writer's processor time is the recorder's cost: a write blocked on the encoder's pipe or the disk takes nothing from a frame.
			const int64_t cpuBefore = ThreadCpuNanoseconds();
			std::string row = m_EncoderPath.empty() ? WriteFrame(frame) : EncodeFrame(frame);
			HarnessCost::Charge(HarnessCost::Recorder, ThreadCpuNanoseconds() - cpuBefore);
			frame.pixels.clear();
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Pool.push_back(std::move(frame.pixels));
				// Flushed per frame and in frame order: a scenario that kills a peer still keeps the index of what it saw.
				m_FinishedRows.emplace(frame.index, std::move(row));
				for (auto next = m_FinishedRows.find(m_NextRow); next != m_FinishedRows.end(); next = m_FinishedRows.find(m_NextRow)) {
					m_Index << next->second << '\n';
					m_FinishedRows.erase(next);
					++m_NextRow;
				}
				m_Index << std::flush;
			}
		}
	}

	std::string FrameRecorder::WriteFrame(const QueuedFrame& frame) {
		const std::string path = (std::filesystem::path(m_FramesDirectory) / FrameLeaf(frame.index)).string();
		const bool saved = SaveRgbPng(path, frame.pixels.data(), frame.meta.width, frame.meta.height);

		nlohmann::json line = {{"frame", frame.index}, {"wall_ms", frame.meta.wallMS}, {"sim_tick", frame.meta.simTick},
		    {"screen", frame.meta.screen}, {"resolution", {frame.meta.width, frame.meta.height}}, {"saved", saved}};
		if (!frame.meta.serviceState.empty()) line["service_state"] = frame.meta.serviceState;

		std::lock_guard<std::mutex> lock(m_Mutex);
		if (saved) {
			++m_Saved;
		} else {
			++m_WriteFailures;
		}
		m_Width = frame.meta.width;
		m_Height = frame.meta.height;
		// Writers finish out of order, so the span is the least and the greatest tick seen.
		m_FirstSimTick = m_SawFrame ? std::min<unsigned long long>(m_FirstSimTick, frame.meta.simTick) : frame.meta.simTick;
		m_SawFrame = true;
		m_LastSimTick = std::max<unsigned long long>(m_LastSimTick, frame.meta.simTick);
		return line.dump();
	}

	std::string FrameRecorder::EncodeFrame(QueuedFrame& frame) {
		if (!m_EncoderTried) {
			m_EncoderTried = true;
			m_EncodedWidth = frame.meta.width;
			m_EncodedHeight = frame.meta.height;
			m_FirstSlot = frame.slot;
			m_NextSlot = frame.slot;
			const std::string preset = m_EncoderCodec.find("nvenc") != std::string::npos ? "-preset p1 -cq 23" : "-preset ultrafast -crf 20";
			const std::string command = "\"" + m_EncoderPath + "\" -hide_banner -loglevel warning -y -f rawvideo -pix_fmt rgb24 -s " +
			    std::to_string(m_EncodedWidth) + "x" + std::to_string(m_EncodedHeight) + " -framerate " + std::to_string(m_Fps) +
			    " -i - -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -c:v " + m_EncoderCodec + " " + preset + " -pix_fmt yuv420p \"" +
			    (std::filesystem::path(m_Directory) / "capture.mp4").string() + "\"";
			auto encoder = std::make_unique<EncoderPipe>();
			if (encoder->Open(command, (std::filesystem::path(m_Directory) / "encoder.log").string(), m_EncoderError)) m_Encoder = std::move(encoder);
			RecordEvent("encoder " + m_EncoderCodec + (m_Encoder ? " started" : " refused: " + m_EncoderError));
		}
		bool written = false;
		if (m_Encoder && frame.meta.width == m_EncodedWidth && frame.meta.height == m_EncodedHeight) {
			// Slots nothing filled keep the last picture, so the video runs on the wall clock the index is stamped with.
			written = true;
			for (; written && !m_LastPicture.empty() && m_NextSlot < frame.slot; ++m_NextSlot, ++m_Repeated) written = m_Encoder->Write(m_LastPicture.data(), m_LastPicture.size());
			written = written && m_Encoder->Write(frame.pixels.data(), frame.pixels.size());
			m_NextSlot = frame.slot + 1;
			std::swap(frame.pixels, m_LastPicture);
			if (!written) m_EncoderError = "the encoder stopped reading";
		}
		nlohmann::json line = {{"frame", frame.index}, {"video_frame", frame.slot - m_FirstSlot}, {"wall_ms", frame.meta.wallMS}, {"sim_tick", frame.meta.simTick},
		    {"screen", frame.meta.screen}, {"resolution", {frame.meta.width, frame.meta.height}}, {"saved", written}};
		if (!frame.meta.serviceState.empty()) line["service_state"] = frame.meta.serviceState;
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (written) {
			++m_Saved;
		} else {
			++m_WriteFailures;
		}
		m_Width = frame.meta.width;
		m_Height = frame.meta.height;
		m_FirstSimTick = m_SawFrame ? std::min<unsigned long long>(m_FirstSimTick, frame.meta.simTick) : frame.meta.simTick;
		m_SawFrame = true;
		m_LastSimTick = std::max<unsigned long long>(m_LastSimTick, frame.meta.simTick);
		return line.dump();
	}

	// A gap between two saved frames is the recorder's own when its slots were turned away here, so the review can tell
	// a starved recording from a screen that presented nothing new.
	void FrameRecorder::WritePendingDrops() {
		// Every writer files drops, so the file is written under the lock as the frame index is.
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::vector<std::pair<long long, std::size_t>> drops;
		drops.swap(m_PendingDrops);
		if (drops.empty()) return;
		for (const auto& [wallMS, slot]: drops) m_DroppedIndex << nlohmann::json({{"wall_ms", wallMS}, {"slot", slot}}).dump() << '\n';
		m_DroppedIndex << std::flush;
	}

	void FrameRecorder::WriteManifest() {
		nlohmann::json manifest = {{"schema", 1}, {"fps", m_Fps}, {"queue_bound", m_QueueBound},
		    {"frames_saved", m_Saved}, {"frames_dropped", m_Dropped}, {"frames_rate_limited", m_RateLimited},
		    {"frames_submitted", m_Submitted}, {"write_failures", m_WriteFailures},
		    {"first_sim_tick", m_FirstSimTick}, {"last_sim_tick", m_LastSimTick},
		    {"resolution", {m_Width, m_Height}}, {"exe_sha256", System::GetThisExeSha256()},
		    {"started_wall_ms", m_StartedWallMS}, {"ended_wall_ms", m_EndedWallMS},
		    {"started_unix_ms", m_StartedUnixMS}, {"ended_unix_ms", m_EndedUnixMS},
		    {"directory", std::filesystem::path(m_Directory).generic_string()}};
		if (!m_EncoderPath.empty()) {
			manifest["encoder"] = {{"codec", m_EncoderCodec}, {"video", "capture.mp4"}, {"exit", m_EncoderExit}, {"error", m_EncoderError},
			    {"repeated_slots", m_Repeated}, {"first_slot", m_FirstSlot}};
		}
		std::ofstream out(std::filesystem::path(m_Directory) / "manifest.json", std::ios::out);
		if (out) out << manifest.dump(2) << '\n';
	}

	void FrameRecorder::Finish() {
		if (!m_Enabled || m_Finished) return;
		m_Finished = true;
		if (m_StagingHeld) {
			m_StagingHeld = false;
			m_Staging.clear();
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Stopping = true;
		}
		m_Wake.notify_all();
		for (std::thread& writer: m_Writers) {
			if (writer.joinable()) writer.join();
		}
		m_Writers.clear();
		if (m_Encoder) {
			m_EncoderExit = m_Encoder->Close(120000);
			m_Encoder.reset();
		}
		m_EndedWallMS = SteadyNowMS();
		m_EndedUnixMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		if (m_FinishAction) {
			auto action = std::move(m_FinishAction);
			action();
		}
		WritePendingDrops();
		m_Index.flush();
		WriteManifest();
		m_Index.close();
		m_DroppedIndex.close();
		{
			std::lock_guard<std::mutex> lock(m_EventsMutex);
			m_Events.close();
		}
		m_Enabled = false;
	}

	std::size_t FrameRecorder::FramesSaved() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Saved;
	}

	std::size_t FrameRecorder::FramesDropped() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Dropped;
	}

	std::size_t FrameRecorder::FramesRateLimited() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_RateLimited;
	}

} // namespace RTE
