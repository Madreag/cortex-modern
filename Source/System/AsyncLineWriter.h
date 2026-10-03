#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

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
				m_Queue.push_back({std::move(text), nullptr});
			}
			m_Wake.notify_one();
		}

		/// Queues text the writer thread makes when its turn comes, in order with the rest.
		void WriteMade(std::function<std::string()> make) {
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Queue.push_back({std::string(), std::move(make)});
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
				std::deque<Entry> batch;
				batch.swap(m_Queue);
				m_Queued += batch.size();
				const bool stopping = m_Stopping;
				lock.unlock();
				for (const Entry& entry: batch) {
					m_Out << (entry.make ? entry.make() : entry.text);
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
		/// Text to write, or what makes it on the writer thread.
		struct Entry {
			std::string text;
			std::function<std::string()> make;
		};
		std::deque<Entry> m_Queue;
		unsigned long long m_Queued = 0; //!< Lines taken off the queue by the writer.
		unsigned long long m_Written = 0; //!< Lines the writer has flushed.
		bool m_Stopping = false;
	};

	/// Rewrites whole files on its own thread, so a report refreshed from a tick never opens a file on that thread. A
	/// rewrite replaces one of the same file still waiting; Flush and Close wait until everything queued is on disk.
	class AsyncFileRewriter {

	public:
		AsyncFileRewriter() = default;
		~AsyncFileRewriter() { Close(); }
		AsyncFileRewriter(const AsyncFileRewriter&) = delete;
		AsyncFileRewriter& operator=(const AsyncFileRewriter&) = delete;

		/// Queues the file's whole new text; false after Close, with one line the first time. The writer stores its own thread in
		/// opener, when given, just before it opens the file.
		bool Rewrite(std::string path, std::string text, std::atomic<std::thread::id>* opener = nullptr) {
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_Stopping) {
					if (!std::exchange(m_ClosedRewriteReported, true)) {
						std::cerr << "[async-rewriter] a rewrite of " << path << " came after its writer closed: dropped" << std::endl;
					}
					return false;
				}
				if (!m_Thread.joinable()) {
					m_Thread = std::thread([this] { Run(); });
				}
				m_Waiting[std::move(path)] = Pending{std::move(text), opener};
				++m_Queued;
			}
			m_Wake.notify_one();
			return true;
		}

		/// Returns once every rewrite queued before the call is on disk.
		void Flush() {
			std::unique_lock<std::mutex> lock(m_Mutex);
			const unsigned long long target = m_Queued;
			m_Drained.wait(lock, [&] { return m_Written >= target || m_Exited; });
		}

		void Close() {
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Stopping = true;
			}
			m_Wake.notify_one();
			// Rewrite never starts the thread once m_Stopping is set.
			if (m_Thread.joinable()) {
				m_Thread.join();
			}
		}

	private:
		struct Pending {
			std::string text;
			std::atomic<std::thread::id>* opener = nullptr;
		};

		void Run() {
			std::unique_lock<std::mutex> lock(m_Mutex);
			while (true) {
				m_Wake.wait(lock, [this] { return m_Stopping || !m_Waiting.empty(); });
				std::map<std::string, Pending> batch;
				batch.swap(m_Waiting);
				const unsigned long long covered = m_Queued;
				lock.unlock();
				for (const auto& [path, pending]: batch) {
					if (pending.opener) {
						pending.opener->store(std::this_thread::get_id());
					}
					std::ofstream out(path, std::ios::trunc);
					if (out) {
						out << pending.text;
					}
				}
				lock.lock();
				m_Written = covered;
				if (m_Stopping && m_Waiting.empty()) {
					m_Exited = true;
					m_Drained.notify_all();
					return;
				}
				m_Drained.notify_all();
			}
		}

		std::thread m_Thread;
		std::mutex m_Mutex;
		std::condition_variable m_Wake;
		std::condition_variable m_Drained;
		std::map<std::string, Pending> m_Waiting;
		unsigned long long m_Queued = 0; //!< Rewrites queued so far.
		unsigned long long m_Written = 0; //!< Rewrites queued before the last batch the writer finished.
		bool m_Stopping = false;
		bool m_Exited = false; //!< The writer has returned; Flush reads this under the mutex instead of the thread object Close joins.
		bool m_ClosedRewriteReported = false;
	};
} // namespace RTE
