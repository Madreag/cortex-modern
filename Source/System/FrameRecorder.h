#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RTE {

	/// Writes a run's presented frames to numbered PNGs and a JSONL index on one worker thread.
	/// The render thread only copies pixels into a pooled buffer; encoding and file writes never
	/// touch it, and a full queue drops the frame instead of waiting.
	class FrameRecorder {

	public:
		/// What one saved frame records beside its pixels.
		struct FrameMeta {
			long long wallMS = 0;
			unsigned long long simTick = 0;
			std::string screen;
			std::string serviceState;
			int width = 0;
			int height = 0;
		};

		static constexpr int c_DefaultFps = 30;
		static constexpr int c_MaxFps = 60;
		/// Frames the writer may fall behind by before the render thread starts dropping.
		static constexpr std::size_t c_DefaultQueueBound = 8;

		FrameRecorder() = default;
		~FrameRecorder();
		FrameRecorder(const FrameRecorder&) = delete;
		FrameRecorder& operator=(const FrameRecorder&) = delete;

		/// The recorder the run flag arms.
		static FrameRecorder& Instance();

		/// The clock the frame index and the manifest are stamped on.
		static long long SteadyNowMS();

		/// Opens directory/frames and starts the writer. The directory must exist and be empty.
		/// @return Whether recording started; error names the directory and the reason when it did not.
		bool Start(const std::string& directory, int fps, std::string* error);

		/// Same, with an explicit queue bound. A bound of zero drops every frame.
		bool Start(const std::string& directory, int fps, std::size_t queueBound, std::string* error);

		bool Enabled() const { return m_Enabled; }
		int Fps() const { return m_Fps; }

		/// Records an automation observation on the frame index's clock.
		void RecordEvent(const std::string& message);

		/// A buffer of bytes for this wall time, or null when the recorder is off, the frame is not
		/// due at the capture rate, or the queue is full. Render thread only, paired with EndFrame.
		unsigned char* BeginFrame(long long wallMS, std::size_t bytes);

		/// Queues the buffer BeginFrame returned. Render thread only.
		void EndFrame(const FrameMeta& meta);

		/// Drains the queue, writes the manifest and stops the writer. Safe to call twice.
		void Finish();
		void SetFinishAction(std::function<void()> action) { m_FinishAction = std::move(action); }

		std::size_t FramesSaved() const;
		std::size_t FramesDropped() const;
		std::size_t FramesRateLimited() const;

	private:
		struct QueuedFrame {
			std::vector<unsigned char> pixels;
			FrameMeta meta;
			std::size_t index = 0;
		};

		/// Whether the capture rate admits a frame at this wall time, advancing the pacer when it does.
		bool DueAt(long long wallMS);
		void WriterLoop();
		void WriteFrame(const QueuedFrame& frame);
		void WriteManifest();

		bool m_Enabled = false;
		bool m_Finished = false;
		std::function<void()> m_FinishAction;
		int m_Fps = c_DefaultFps;
		std::size_t m_QueueBound = c_DefaultQueueBound;
		std::string m_Directory;
		std::string m_FramesDirectory;
		std::ofstream m_Index;
		std::ofstream m_Events;

		long long m_StartedWallMS = 0;
		long long m_EndedWallMS = 0;
		long long m_StartedUnixMS = 0;
		long long m_EndedUnixMS = 0;
		bool m_PacerStarted = false;
		long long m_PacerOriginMS = 0;
		std::size_t m_Admitted = 0;
		std::size_t m_Submitted = 0;
		std::size_t m_RateLimited = 0;
		std::size_t m_NextIndex = 0;

		std::vector<unsigned char> m_Staging;
		bool m_StagingHeld = false;

		mutable std::mutex m_Mutex;
		std::condition_variable m_Wake;
		std::deque<QueuedFrame> m_Queue;
		std::vector<std::vector<unsigned char>> m_Pool;
		std::thread m_Writer;
		bool m_Stopping = false;

		std::size_t m_Saved = 0;
		std::size_t m_Dropped = 0;
		std::size_t m_WriteFailures = 0;
		int m_Width = 0;
		int m_Height = 0;
		unsigned long long m_FirstSimTick = 0;
		unsigned long long m_LastSimTick = 0;
		bool m_SawFrame = false;
	};

} // namespace RTE
