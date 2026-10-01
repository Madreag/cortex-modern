#include "FrameRecorder.h"

#include "System.h"

#include "png.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
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
		for (std::size_t writer = 0, count = WriterCount(); writer < count; ++writer) m_Writers.emplace_back(&FrameRecorder::WriterLoop, this);
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
			m_PendingDrops.emplace_back(wallMS, m_Admitted - 1);
			m_Wake.notify_one();
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
			std::string row = WriteFrame(frame);
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
