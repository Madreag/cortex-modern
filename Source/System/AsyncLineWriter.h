#pragma once

#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace RTE {

	/// Appends text to one file on its own thread, so a record written every tick or every loop pass never waits on
	/// the disk. Writes keep their order; Flush and Close wait until everything queued is on disk.
	class AsyncLineWriter {

	public:
		AsyncLineWriter() = default;
		~AsyncLineWriter() { Close(); }
		AsyncLineWriter(const AsyncLineWriter&) = delete;
		AsyncLineWriter& operator=(const AsyncLineWriter&) = delete;

		/// Opens the file (truncated or appended) and starts the writer. False when the file does not open.
		bool Open(const std::string& path, bool append = false) {
			Close();
			m_Out.open(path, append ? std::ios::app : std::ios::trunc);
			if (!m_Out) {
				return false;
			}
			m_Stopping = false;
			m_Thread = std::thread([this] { Run(); });
			return true;
		}

		bool IsOpen() const { return m_Thread.joinable(); }

		/// Queues one line; the newline is added.
		void Write(std::string line) {
			line += '\n';
			WriteBlock(std::move(line));
		}

		/// Queues text written exactly as it is.
		void WriteBlock(std::string text) {
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Queue.push_back(std::move(text));
			}
			m_Wake.notify_one();
		}

		/// Returns once every line queued before the call is written and flushed.
		void Flush() {
			std::unique_lock<std::mutex> lock(m_Mutex);
			const unsigned long long target = m_Queued + m_Queue.size();
			m_Wake.notify_one();
			m_Drained.wait(lock, [&] { return m_Written >= target || !m_Thread.joinable(); });
		}

		void Close() {
			if (!m_Thread.joinable()) {
				return;
			}
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Stopping = true;
			}
			m_Wake.notify_one();
			m_Thread.join();
			m_Out.close();
		}

	private:
		void Run() {
			std::unique_lock<std::mutex> lock(m_Mutex);
			while (true) {
				m_Wake.wait(lock, [this] { return m_Stopping || !m_Queue.empty(); });
				std::deque<std::string> batch;
				batch.swap(m_Queue);
				m_Queued += batch.size();
				const bool stopping = m_Stopping;
				lock.unlock();
				for (const std::string& text: batch) {
					m_Out << text;
				}
				m_Out.flush();
				lock.lock();
				m_Written += batch.size();
				m_Drained.notify_all();
				if (stopping && m_Queue.empty()) {
					return;
				}
			}
		}

		std::ofstream m_Out;
		std::thread m_Thread;
		std::mutex m_Mutex;
		std::condition_variable m_Wake;
		std::condition_variable m_Drained;
		std::deque<std::string> m_Queue;
		unsigned long long m_Queued = 0; //!< Lines taken off the queue by the writer.
		unsigned long long m_Written = 0; //!< Lines the writer has flushed.
		bool m_Stopping = false;
	};
} // namespace RTE
