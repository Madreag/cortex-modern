#pragma once

#include "NetDirectoryCodec.h"
#include "NetLanDiscovery.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RTE {

	/// The session-directory client, game-thread only. A host advertises its match (register,
	/// heartbeat at the service's interval, delete at teardown); a joiner browses the list through
	/// a separate instance. Every request is asynchronous: Update() issues and polls it, so a
	/// directory call never blocks a frame and never rides the session pump.
	class NetDirectoryClient {
	public:
		enum class State : uint8_t {
			Disabled,    //!< No SessionDirectoryUrl configured.
			Idle,        //!< Configured, nothing listed and no request pending.
			Registering, //!< A register is in flight or awaiting its retry slot.
			Registered,  //!< The row is listed; heartbeats keep it alive.
			Deleting,    //!< A delete is in flight.
			Failed,      //!< The directory refused the row; stays until the listing intent changes.
		};
		static const char* StateName(State state);

		struct Request {
			std::string method;
			std::string path;
			std::string body;
		};
		struct Reply {
			long statusCode = 0; //!< HTTP status when one arrived; 0 on transport/cert failure.
			std::string body;
			std::string error;   //!< Non-empty when the request failed before/instead of a status.
		};

		/// One in-flight request. The production transport wraps NetHttpClient; the selftest injects
		/// canned replies so no socket is opened.
		class Transport {
		public:
			virtual ~Transport() = default;
			virtual void Start(const Request& request) = 0;
			virtual bool Finished() = 0;
			virtual Reply Take() = 0;
			virtual void Abort() = 0;
		};
		using TransportFactory = std::function<std::unique_ptr<Transport>()>;

		/// One merged join-screen row: a LAN beacon or a directory session.
		struct GameRow {
			std::string source;    //!< "LAN" | "NET"
			std::string name;
			std::string activity;
			std::string mode;
			std::string players;   //!< "taken/max"
			std::string address;
			uint16_t port = 0;
			bool joinable = false;
			std::string reason;    //!< Empty when joinable, else the refusal label.
			std::string sessionId; //!< NET rows only.
		};

		NetDirectoryClient();
		NetDirectoryClient(const NetDirectoryClient&) = delete;
		NetDirectoryClient& operator=(const NetDirectoryClient&) = delete;
		~NetDirectoryClient();

		/// Test seam: installs the request transport. Call before Configure(); the default factory
		/// (installed by Configure when none is set) wraps NetHttpClient.
		void SetTransportFactory(TransportFactory factory);
		void Configure(std::string baseUrl, std::string installKey, std::string certPinSha256);

		State GetState() const { return m_State; }
		const std::string& GetSessionId() const { return m_SessionId; }

		/// Host: keep the row listed. The first call after Idle registers; the row passed on each
		/// call carries the live peer_count/seats_free for the next heartbeat.
		void Advertise(const NetDirectoryRegisterRequest& row, bool running);
		/// Host: take the row down. Once a session exists the delete rides the request pump; a row
		/// still mid-register is answered first so the delete can target the issued session id.
		void Retract();
		void Update(uint64_t nowMs);
		/// Teardown: retracts, then drains the pending delete and any in-flight request within the
		/// budget, aborting whatever is still open. Called from the game thread on Destroy.
		void Shutdown();

		/// Browse side - a separate instance only ever browses: issues GET /v1/sessions every
		/// c_ListIntervalMs while polling.
		void PollList(uint64_t nowMs);
		void StopBrowsing() { m_BrowseWanted = false; }
		const std::vector<NetDirectorySessionRow>& Rows() const { return m_Rows; }
		/// Non-empty when the last list GET failed or answered an undecodable body.
		const std::string& ListError() const { return m_ListError; }

		static std::vector<GameRow> MergeGameLists(const std::vector<NetLanHostInfo>& lan,
		                                         const std::vector<NetDirectorySessionRow>& directory,
		                                         const NetDirectoryLocalIdentity& local);

		/// The service.directory report section: {state, session_id, registers, heartbeats, deletes,
		/// last_status, last_error}.
		std::string BuildReportJson() const;

		static constexpr uint64_t c_ListIntervalMs = 5000;
		static constexpr uint64_t c_ShutdownBudgetMs = 2000;
		static constexpr uint64_t c_RetryBaseMs = 5000;  //!< Backoff starts here, doubles to the cap.
		static constexpr uint64_t c_RetryMaxMs = 60000;
		static constexpr int64_t c_MinHeartbeatS = 1;    //!< The service's floor is respected.

	private:
		enum class RequestKind : uint8_t { None, Register, Heartbeat, Delete, List };

		void SetState(State state);
		void NoteError(const std::string& error);
		void ScheduleRetry(uint64_t nowMs); //!< Transport/TLS/5xx: backoff 5s,10s,20s..60s.
		void StartRequest(RequestKind kind, const Request& request);
		void HandleReply(RequestKind kind, const Reply& reply, uint64_t nowMs);
		void HandleRegisterReply(const Reply& reply, uint64_t nowMs);
		void HandleHeartbeatReply(const Reply& reply, uint64_t nowMs);
		void HandleDeleteReply(const Reply& reply, uint64_t nowMs);
		void HandleListReply(const Reply& reply, uint64_t nowMs);
		void IssueRegister(uint64_t nowMs);
		void IssueHeartbeat(uint64_t nowMs);
		void IssueDelete(uint64_t nowMs);
		void IssueList(uint64_t nowMs);

		State m_State = State::Idle;    //!< Disabled once Configure() sees an empty URL.
		TransportFactory m_Factory;
		std::string m_BaseUrl;
		std::string m_InstallKey;
		std::string m_CertPinSha256;

		bool m_Listed = false;          //!< Whether the host wants the row up.
		bool m_BrowseWanted = false;
		NetDirectoryRegisterRequest m_Row;
		bool m_Running = false;
		std::string m_SessionId;
		std::string m_Token;
		int64_t m_HeartbeatS = 0;
		int64_t m_ExpiresInS = 0;
		uint64_t m_NextHeartbeatMs = 0;
		uint64_t m_NextAttemptMs = 0;   //!< The retry slot a transient failure or a 429 set.
		uint64_t m_BackoffMs = 0;
		bool m_Reregistered = false;    //!< The one re-register a heartbeat 404 is allowed.
		uint64_t m_BrowseNextMs = 0;
		std::vector<NetDirectorySessionRow> m_Rows;
		std::string m_ListError;

		std::unique_ptr<Transport> m_Request;
		RequestKind m_RequestKind = RequestKind::None;

		uint64_t m_Registers = 0;
		uint64_t m_Heartbeats = 0;
		uint64_t m_Deletes = 0;
		long m_LastStatus = 0;
		std::string m_LastError;
	};

} // namespace RTE
