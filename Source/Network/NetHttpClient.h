#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RTE {

	/// One asynchronous HTTPS JSON request on its own thread. The caller owns the object, polls it
	/// without blocking, and destroys it only after Poll() reports Done or after Cancel() joined.
	/// One NetHttpClient serves exactly one request: Start() on a used object finishes an error
	/// response instead of running again. Cancel() interrupts the in-flight transfer and returns
	/// promptly, well under the network timeouts.
	class NetHttpClient {
	public:
		enum class PollResult {
			Pending,
			Done,
		};

		struct Response {
			long statusCode = 0;   //!< HTTP status when a response arrived; 0 on transport/cert failure.
			std::string body;      //!< Response body on success.
			std::string error;     //!< Non-empty when the request failed before/instead of a status.
		};

		static constexpr int c_ConnectTimeoutMs = 5000;
		static constexpr int c_TotalTimeoutMs = 15000;

		NetHttpClient() = default;
		NetHttpClient(const NetHttpClient&) = delete;
		NetHttpClient& operator=(const NetHttpClient&) = delete;
		~NetHttpClient();

		/// Starts the request on a worker thread and returns immediately. certPinSha256 is empty or
		/// 64 lowercase hex of the server certificate's DER bytes; pinned mode accepts exactly that
		/// certificate and verifies it before any request byte is sent, otherwise the system chain
		/// must validate. Only https URLs are accepted. A second Start() on a used object finishes
		/// an error response and runs nothing.
		void Start(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, const std::string& certPinSha256 = "");
		PollResult Poll();
		Response GetResponse() const;
		void Cancel();

	private:
		void WorkerMain(std::string method, std::string url, std::vector<std::pair<std::string, std::string>> headers, std::string body, std::string certPinSha256);
		void WorkerMainImpl(std::string method, std::string url, std::vector<std::pair<std::string, std::string>> headers, std::string body, std::string certPinSha256);
		void Finish(const Response& response);

		std::thread m_Worker;
		std::atomic<bool> m_Started{false};
		std::atomic<bool> m_Done{false};
		std::atomic<bool> m_CancelRequested{false};
		mutable std::mutex m_Mutex;
		Response m_Response;
#ifdef _WIN32
		std::mutex m_HandleMutex;   //!< Serializes the m_Async pointer between the worker and Cancel().
		void* m_Async = nullptr;    //!< AsyncRequest, owned by the worker; defined in the .cpp.
#endif
	};

} // namespace RTE
