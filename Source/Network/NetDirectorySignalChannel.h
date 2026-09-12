#pragma once

#include "NetDirectoryClient.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

	/// One peer's end of the session directory's signal relay, game-thread only. Post() queues a
	/// signal; Update() keeps at most one request in flight: a POST for the oldest queued signal,
	/// else, while polling is armed, a GET every c_PollIntervalMs for the signals addressed to this
	/// peer. Those reach the sink in seq order and the cursor moves only past a signal the sink took,
	/// so the service keeps the rest for the next poll.
	class NetDirectorySignalChannel {
	public:
		enum class State : uint8_t {
			Disabled, //!< No directory URL configured.
			Open,     //!< Posting, and polling while armed.
			Draining, //!< Drain(): the final poll is pending or in flight.
			Closed,   //!< Drained; nothing more is sent.
			Failed,   //!< Terminal: session gone (404), bad credential (403) or a request the service refused.
		};
		static const char* StateName(State state);

		struct Signal {
			int64_t seq = 0;
			std::string from;  //!< "host" | "client:<nonce>"
			std::string bytes; //!< The decoded payload.
		};
		/// Returns true when it took the signal; false leaves it, and every later one, at the service.
		using Sink = std::function<bool(const Signal& signal)>;
		using Headers = std::vector<std::pair<std::string, std::string>>;

		NetDirectorySignalChannel() = default;
		NetDirectorySignalChannel(const NetDirectorySignalChannel&) = delete;
		NetDirectorySignalChannel& operator=(const NetDirectorySignalChannel&) = delete;
		~NetDirectorySignalChannel();

		/// Test seam: installs the request transport. Call before Configure*(); the default factory
		/// wraps one NetHttpClient per request, as NetDirectoryClient's does.
		void SetTransportFactory(NetDirectoryClient::TransportFactory factory);
		/// The host end: reads peer=host, proving the session token in X-Session-Token (never the URL).
		void ConfigureHost(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId, std::string sessionToken);
		/// A joiner end: mints a fresh join nonce (GetJoinNonce) and signals as "client:<nonce>".
		void ConfigureClient(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId);
		void SetSink(Sink sink) { m_Sink = std::move(sink); }
		void SetPolling(bool armed) { m_PollArmed = armed; }
		/// Long-poll: polls carry wait=<seconds> (clamped to c_MaxPollWaitS) and the next GET issues
		/// right after the previous returns, instead of after c_PollIntervalMs. 0 keeps short polls.
		void SetPollWait(int seconds) { m_PollWaitS = std::clamp(seconds, 0, static_cast<int>(c_MaxPollWaitS)); }

		/// Queues bytes for the peer `to`: a joiner signals "host", the host answers "client:<nonce>".
		/// False when the channel is not open, `to` is not a peer this end may signal, the payload is
		/// over c_MaxSignalBytes, or c_MaxPendingPosts signals are already queued.
		bool Post(const std::string& to, const std::string& bytes);
		void Update(uint64_t nowMs);
		/// Teardown: settles the in-flight request, polls once more and closes within c_DrainBudgetMs;
		/// signals still queued are dropped.
		void Drain();

		State GetState() const { return m_State; }
		const std::string& GetLocalPeer() const { return m_LocalPeer; }
		/// The joiner's bearer nonce; empty on the host end.
		const std::string& GetJoinNonce() const { return m_JoinNonce; }
		int64_t GetCursor() const { return m_Cursor; }
		size_t PendingPosts() const { return m_Outbox.size(); }
		/// The reason once Failed ("session gone", "bad credential", ...), else the last transient error.
		const std::string& GetLastError() const { return m_LastError; }
		/// What every request carries: the install key, X-Signal-Peer and, on the host end, X-Session-Token.
		const Headers& RequestHeaders() const { return m_Headers; }

		/// The report section: {state, signals_posted, signals_received, polls, last_status, last_error}.
		std::string BuildReportJson() const;

		/// c_JoinNonceChars install-key-alphabet characters drawn from std::random_device.
		static std::string MintJoinNonce();

		static constexpr uint64_t c_PollIntervalMs = 500;
		static constexpr uint64_t c_MaxPollWaitS = 12;        //!< NetHttpClient's total timeout is 15 s; a held poll must answer inside it.
		static constexpr size_t c_MaxSignalBytes = 64 * 1024; //!< The service's MAX_PAYLOAD.
		static constexpr size_t c_MaxPendingPosts = 256;      //!< The service's MAX_QUEUE.
		static constexpr size_t c_JoinNonceChars = 32;
		static constexpr uint64_t c_RetryBaseMs = 5000;       //!< Backoff starts here, doubles to the cap.
		static constexpr uint64_t c_RetryMaxMs = 60000;
		static constexpr uint64_t c_DrainBudgetMs = 2000;

	private:
		enum class RequestKind : uint8_t { None, Post, Poll };
		struct Outbound {
			std::string to;
			std::string payloadB64;
		};

		void Configure(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId, std::string localPeer, std::string credential);
		const char* Role() const;
		void SetState(State state);
		void NoteError(const std::string& error);
		void Fail(const std::string& reason, const std::string& detail = "");
		void AbortRequest();
		void ScheduleRetry(uint64_t nowMs); //!< Transport error, bad body or 5xx: every request waits 5s,10s,20s..60s.
		void StartRequest(RequestKind kind, const NetDirectoryClient::Request& request);
		void IssuePost();
		void IssuePoll();
		void HandlePostReply(const NetDirectoryClient::Reply& reply, uint64_t nowMs);
		void HandlePollReply(const NetDirectoryClient::Reply& reply, uint64_t nowMs);
		/// 429 holds every request for retry_after_s, queue_full backs the posts off, 5xx backs everything
		/// off, 404 and 403 fail with their reason, any other status fails.
		void HandleRefusal(const char* what, const NetDirectoryClient::Reply& reply, uint64_t nowMs);
		bool DecodeInbound(const NetDirectorySignalList& list, std::vector<Signal>& out, std::string& reason) const;

		State m_State = State::Disabled;
		NetDirectoryClient::TransportFactory m_Factory;
		std::string m_BaseUrl;
		std::string m_CertPinSha256;
		std::string m_SessionPath;     //!< "/v1/sessions/<id>"
		std::string m_LocalPeer;
		std::string m_JoinNonce;
		std::string m_Credential;      //!< token_or_join_nonce: the session token, or the join nonce.
		Headers m_Headers;
		Sink m_Sink;
		bool m_PollArmed = false;

		std::deque<Outbound> m_Outbox;
		int64_t m_Cursor = 0;          //!< The highest seq the sink took; the next poll reads after it.
		int m_PollWaitS = 0;           //!< Long-poll seconds on the wire; 0 = a plain GET every c_PollIntervalMs.
		uint64_t m_NextPollMs = 0;
		uint64_t m_NextAttemptMs = 0;  //!< Every request waits for this slot after a transport error, 5xx or 429.
		uint64_t m_BackoffMs = 0;
		uint64_t m_NextPostMs = 0;     //!< Only posts wait for this slot after queue_full, so polling goes on.
		uint64_t m_PostBackoffMs = 0;
		bool m_DrainPolled = false;

		std::unique_ptr<NetDirectoryClient::Transport> m_Request;
		RequestKind m_RequestKind = RequestKind::None;

		uint64_t m_SignalsPosted = 0;
		uint64_t m_SignalsReceived = 0;
		uint64_t m_Polls = 0;
		long m_LastStatus = 0;
		std::string m_LastError;
	};

} // namespace RTE
