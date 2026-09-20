#include "FrameRecorder.h"

#include "System.h"

#include "SDL3/SDL_surface.h"
#include <SDL3_image/SDL_image.h>

#include "nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace RTE {

	namespace {
		std::string FrameLeaf(std::size_t index) {
			std::ostringstream name;
			name << "frame-" << std::setw(6) << std::setfill('0') << index << ".png";
			return name.str();
		}
	} // namespace

	FrameRecorder& FrameRecorder::Instance() {
		static FrameRecorder recorder;
		return recorder;
	}

	long long FrameRecorder::SteadyNowMS() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

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

		m_Directory = directory;
		m_FramesDirectory = frames.string();
		m_Fps = fps;
		m_QueueBound = queueBound;
		m_Finished = false;
		m_StartedWallMS = SteadyNowMS();
		m_StartedUnixMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		m_Enabled = true;
		m_Writer = std::thread(&FrameRecorder::WriterLoop, this);
		return true;
	}

	void FrameRecorder::RecordEvent(const std::string& message) {
		if (!m_Enabled) return;
		m_Events << nlohmann::json({{"wall_ms", SteadyNowMS()}, {"message", message}}).dump() << '\n' << std::flush;
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
			return nullptr;
		}
		if (!m_Pool.empty()) {
			m_Staging = std::move(m_Pool.back());
			m_Pool.pop_back();
		}
		lock.unlock();
		m_Staging.resize(bytes);
		m_StagingHeld = true;
		return m_Staging.data();
	}

	void FrameRecorder::EndFrame(const FrameMeta& meta) {
		if (!m_StagingHeld) return;
		m_StagingHeld = false;
		QueuedFrame frame;
		frame.pixels = std::move(m_Staging);
		frame.meta = meta;
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
				m_Wake.wait(lock, [this] { return !m_Queue.empty() || m_Stopping; });
				if (m_Queue.empty()) return;
				frame = std::move(m_Queue.front());
				m_Queue.pop_front();
			}
			WriteFrame(frame);
			frame.pixels.clear();
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Pool.push_back(std::move(frame.pixels));
			}
		}
	}

	void FrameRecorder::WriteFrame(const QueuedFrame& frame) {
		const std::string path = (std::filesystem::path(m_FramesDirectory) / FrameLeaf(frame.index)).string();
		SDL_Surface* surface = SDL_CreateSurfaceFrom(frame.meta.width, frame.meta.height, SDL_PIXELFORMAT_RGB24,
		    const_cast<unsigned char*>(frame.pixels.data()), frame.meta.width * 3);
		const bool saved = surface != nullptr && IMG_SavePNG(surface, path.c_str());
		if (surface) SDL_DestroySurface(surface);

		nlohmann::json line = {{"frame", frame.index}, {"wall_ms", frame.meta.wallMS}, {"sim_tick", frame.meta.simTick},
		    {"screen", frame.meta.screen}, {"resolution", {frame.meta.width, frame.meta.height}}, {"saved", saved}};
		if (!frame.meta.serviceState.empty()) line["service_state"] = frame.meta.serviceState;
		// Flushed per frame: a scenario that kills a peer still keeps the index of what it saw.
		m_Index << line.dump() << '\n' << std::flush;

		std::lock_guard<std::mutex> lock(m_Mutex);
		if (saved) {
			++m_Saved;
		} else {
			++m_WriteFailures;
		}
		m_Width = frame.meta.width;
		m_Height = frame.meta.height;
		if (!m_SawFrame) {
			m_SawFrame = true;
			m_FirstSimTick = frame.meta.simTick;
		}
		m_LastSimTick = frame.meta.simTick;
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
		if (m_Writer.joinable()) m_Writer.join();
		m_EndedWallMS = SteadyNowMS();
		m_EndedUnixMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		m_Index.flush();
		WriteManifest();
		m_Index.close();
		m_Events.close();
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
