#include "NetDirectoryClient.h"

#include "NetHttpClient.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

using json = nlohmann::json;

namespace RTE {

	namespace {
		uint64_t SteadyNowMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		/// The production transport: one NetHttpClient per request, fired on its own thread.
		class NetHttpTransport final : public NetDirectoryClient::Transport {
		public:
			NetHttpTransport(std::string baseUrl, std::string installKey, std::string certPinSha256) :
				m_BaseUrl(std::move(baseUrl)), m_CertPinSha256(std::move(certPinSha256)) {
				m_Headers = {
					{"Content-Type", "application/json"},
					{"X-Install-Key", std::move(installKey)},
				};
			}

			void Start(const NetDirectoryClient::Request& request) override {
				m_Client.Start(request.method, m_BaseUrl + request.path, m_Headers, request.body, m_CertPinSha256);
			}
			bool Finished() override { return m_Client.Poll() == NetHttpClient::PollResult::Done; }
			NetDirectoryClient::Reply Take() override {
				const NetHttpClient::Response response = m_Client.GetResponse();
				return {response.statusCode, response.body, response.error};
			}
			void Abort() override { m_Client.Cancel(); }

		private:
			NetHttpClient m_Client;
			std::string m_BaseUrl;
			std::string m_CertPinSha256;
			std::vector<std::pair<std::string, std::string>> m_Headers;
		};

		/// Reads retry_after_s from a 429 error body; 1s when the field is absent or unreadable.
		int64_t ParseRetryAfterS(const std::string& body) {
			try {
				const json parsed = json::parse(body);
				if (parsed.is_object() && parsed.contains("retry_after_s") && parsed["retry_after_s"].is_number()) {
					return std::max<int64_t>(1, parsed["retry_after_s"].get<int64_t>());
				}
			} catch (...) {
			}
			return 1;
		}

		/// The join-screen refusal labels for an IsJoinable reason ("incompatible: <field>").
		const char* MapMismatchReason(const std::string& reason) {
			const std::string field = reason.rfind("incompatible: ", 0) == 0 ? reason.substr(14) : reason;
			if (field == "network_protocol_version") {
				return "protocol";
			}
			if (field == "lockstep_codec_version") {
				return "codec";
			}
			if (field == "controller_frame_version") {
				return "controller frames";
			}
			if (field == "session_identity_hash") {
				return "identity";
			}
			if (field == "module_manifest_hash") {
				return "modules";
			}
			return "incompatible";
		}
	}

	NetDirectoryClient::NetDirectoryClient() = default;

	NetDirectoryClient::~NetDirectoryClient() {
		if (m_Request) {
			m_Request->Abort();
			m_Request.reset();
		}
	}

	void NetDirectoryClient::SetTransportFactory(TransportFactory factory) { m_Factory = std::move(factory); }

	void NetDirectoryClient::Configure(std::string baseUrl, std::string installKey, std::string certPinSha256) {
		m_BaseUrl = std::move(baseUrl);
		m_InstallKey = std::move(installKey);
		m_CertPinSha256 = std::move(certPinSha256);
		while (!m_BaseUrl.empty() && m_BaseUrl.back() == '/') {
			m_BaseUrl.pop_back();
		}
		if (m_BaseUrl.empty()) {
			m_Listed = false;
			m_BrowseWanted = false;
			if (m_Request) {
				m_Request->Abort();
				m_Request.reset();
				m_RequestKind = RequestKind::None;
			}
			SetState(State::Disabled);
			return;
		}
		if (!m_Factory) {
			const std::string url = m_BaseUrl;
			const std::string key = m_InstallKey;
			const std::string pin = m_CertPinSha256;
			m_Factory = [url, key, pin]() { return std::make_unique<NetHttpTransport>(url, key, pin); };
		}
		if (m_State == State::Disabled) {
			SetState(State::Idle);
		}
	}

	void NetDirectoryClient::Advertise(const NetDirectoryRegisterRequest& row, bool running) {
		m_Row = row;
		m_Running = running;
		if (!m_Listed) {
			m_Reregistered = false;
			m_BackoffMs = 0;
			m_NextAttemptMs = 0;
			if (m_State == State::Failed) {
				SetState(State::Idle);
			}
		}
		m_Listed = true;
	}

	void NetDirectoryClient::Retract() { m_Listed = false; }

	void NetDirectoryClient::PollList(uint64_t nowMs) {
		m_BrowseWanted = true;
		Update(nowMs);
	}

	void NetDirectoryClient::Update(uint64_t nowMs) {
		if (m_State == State::Disabled) {
			return;
		}
		if (m_Request && m_Request->Finished()) {
			const RequestKind kind = m_RequestKind;
			const Reply reply = m_Request->Take();
			m_Request.reset();
			m_RequestKind = RequestKind::None;
			HandleReply(kind, reply, nowMs);
		}
		if (m_Request || nowMs < m_NextAttemptMs) {
			return;
		}
		switch (m_State) {
			case State::Idle:
				if (m_Listed) {
					IssueRegister(nowMs);
				}
				break;
			case State::Registering:
				// A transient register failure waits out its retry slot here; an unlist that beat the
				// first response drops the intent without a delete since no row exists yet.
				if (m_Listed) {
					IssueRegister(nowMs);
				} else if (m_SessionId.empty()) {
					SetState(State::Idle);
				} else {
					IssueDelete(nowMs);
				}
				break;
			case State::Registered:
				if (!m_Listed) {
					IssueDelete(nowMs);
				} else if (nowMs >= m_NextHeartbeatMs) {
					IssueHeartbeat(nowMs);
				}
				break;
			case State::Deleting:
				if (m_SessionId.empty()) {
					SetState(State::Idle);
				} else {
					IssueDelete(nowMs);
				}
				break;
			case State::Failed:
				if (!m_Listed) {
					SetState(State::Idle);
				}
				break;
			default:
				break;
		}
		if (!m_Request && m_State == State::Idle && m_BrowseWanted && nowMs >= m_BrowseNextMs) {
			IssueList(nowMs);
		}
	}

	void NetDirectoryClient::Shutdown() {
		m_Listed = false;
		m_BrowseWanted = false;
		if (m_State == State::Disabled) {
			return;
		}
		const uint64_t begin = SteadyNowMs();
		while (SteadyNowMs() - begin < c_ShutdownBudgetMs) {
			Update(SteadyNowMs());
			if (!m_Request && m_State != State::Deleting && m_SessionId.empty()) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (m_Request) {
			m_Request->Abort();
			m_Request.reset();
			m_RequestKind = RequestKind::None;
		}
		m_SessionId.clear();
		m_Token.clear();
		if (m_State != State::Disabled) {
			SetState(State::Idle);
		}
	}

	void NetDirectoryClient::SetState(State state) {
		if (m_State == state) {
			return;
		}
		std::cout << "[net-directory] state: " << StateName(m_State) << " -> " << StateName(state) << std::endl;
		m_State = state;
	}

	void NetDirectoryClient::NoteError(const std::string& error) {
		m_LastError = error;
		std::cout << "[net-directory] " << error << std::endl;
	}

	void NetDirectoryClient::ScheduleRetry(uint64_t nowMs) {
		m_BackoffMs = m_BackoffMs == 0 ? c_RetryBaseMs : std::min<uint64_t>(m_BackoffMs * 2, c_RetryMaxMs);
		m_NextAttemptMs = nowMs + m_BackoffMs;
	}

	void NetDirectoryClient::StartRequest(RequestKind kind, const Request& request) {
		if (!m_Factory) {
			NoteError("no transport configured");
			SetState(State::Failed);
			return;
		}
		m_Request = m_Factory();
		m_RequestKind = kind;
		m_Request->Start(request);
	}

	void NetDirectoryClient::HandleReply(RequestKind kind, const Reply& reply, uint64_t nowMs) {
		if (reply.statusCode > 0) {
			m_LastStatus = reply.statusCode;
		}
		switch (kind) {
			case RequestKind::Register: HandleRegisterReply(reply, nowMs); break;
			case RequestKind::Heartbeat: HandleHeartbeatReply(reply, nowMs); break;
			case RequestKind::Delete: HandleDeleteReply(reply, nowMs); break;
			case RequestKind::List: HandleListReply(reply, nowMs); break;
			default: break;
		}
	}

	void NetDirectoryClient::HandleRegisterReply(const Reply& reply, uint64_t nowMs) {
		if (!reply.error.empty() || reply.statusCode == 0) {
			NoteError("register: " + (reply.error.empty() ? "transport error" : reply.error));
			ScheduleRetry(nowMs);
			return;
		}
		if (reply.statusCode == 200) {
			NetDirectoryRegisterResponse response;
			std::string error;
			if (!NetDirectoryCodec::DecodeRegisterResponse(reply.body, &response, &error)) {
				NoteError("register: " + error);
				ScheduleRetry(nowMs);
				return;
			}
			m_SessionId = response.sessionId;
			m_Token = response.token;
			m_HeartbeatS = std::max<int64_t>(c_MinHeartbeatS, response.heartbeatS);
			m_ExpiresInS = response.expiresInS;
			m_NextHeartbeatMs = nowMs + static_cast<uint64_t>(m_HeartbeatS) * 1000;
			m_BackoffMs = 0;
			std::cout << "[net-directory] registered session_id=" << m_SessionId << " heartbeat_s=" << m_HeartbeatS << std::endl;
			SetState(State::Registered);
			if (!m_Listed) {
				// The unlist beat the response: delete the row we just created.
				IssueDelete(nowMs);
			}
			return;
		}
		if (reply.statusCode == 429) {
			const int64_t retryS = ParseRetryAfterS(reply.body);
			NoteError("register throttled (429), retrying in " + std::to_string(retryS) + "s");
			m_NextAttemptMs = nowMs + static_cast<uint64_t>(retryS) * 1000;
			return;
		}
		if (reply.statusCode >= 500) {
			NoteError("register: HTTP " + std::to_string(reply.statusCode));
			ScheduleRetry(nowMs);
			return;
		}
		NoteError("register refused: HTTP " + std::to_string(reply.statusCode));
		SetState(State::Failed);
	}

	void NetDirectoryClient::HandleHeartbeatReply(const Reply& reply, uint64_t nowMs) {
		if (!reply.error.empty() || reply.statusCode == 0) {
			NoteError("heartbeat: " + (reply.error.empty() ? "transport error" : reply.error));
			ScheduleRetry(nowMs);
			return;
		}
		if (reply.statusCode == 200) {
			NetDirectoryHeartbeatResponse response;
			std::string error;
			if (!NetDirectoryCodec::DecodeHeartbeatResponse(reply.body, &response, &error)) {
				NoteError("heartbeat: " + error);
				ScheduleRetry(nowMs);
				return;
			}
			m_HeartbeatS = std::max<int64_t>(c_MinHeartbeatS, response.heartbeatS);
			m_ExpiresInS = response.expiresInS;
			m_NextHeartbeatMs = nowMs + static_cast<uint64_t>(m_HeartbeatS) * 1000;
			m_BackoffMs = 0;
			return;
		}
		if (reply.statusCode == 404) {
			// The row expired or the service forgot it: exactly one re-register is allowed.
			if (m_Reregistered) {
				NoteError("heartbeat: row lost again after re-register");
				SetState(State::Failed);
				return;
			}
			m_Reregistered = true;
			m_SessionId.clear();
			m_Token.clear();
			NoteError("heartbeat: row gone (404), re-registering once");
			SetState(State::Registering);
			return;
		}
		if (reply.statusCode == 429) {
			const int64_t retryS = ParseRetryAfterS(reply.body);
			NoteError("heartbeat throttled (429), retrying in " + std::to_string(retryS) + "s");
			m_NextHeartbeatMs = nowMs + static_cast<uint64_t>(retryS) * 1000;
			return;
		}
		if (reply.statusCode >= 500) {
			NoteError("heartbeat: HTTP " + std::to_string(reply.statusCode));
			ScheduleRetry(nowMs);
			return;
		}
		NoteError("heartbeat refused: HTTP " + std::to_string(reply.statusCode));
		SetState(State::Failed);
	}

	void NetDirectoryClient::HandleDeleteReply(const Reply& reply, uint64_t nowMs) {
		if (!reply.error.empty() || reply.statusCode == 0) {
			NoteError("delete: " + (reply.error.empty() ? "transport error" : reply.error));
		} else if (reply.statusCode != 200 && reply.statusCode != 404) {
			NoteError("delete: HTTP " + std::to_string(reply.statusCode));
		}
		// Whatever the answer, the row is gone on our side or expires on its own.
		m_SessionId.clear();
		m_Token.clear();
		SetState(State::Idle);
	}

	void NetDirectoryClient::HandleListReply(const Reply& reply, uint64_t nowMs) {
		m_BrowseNextMs = nowMs + c_ListIntervalMs;
		if (!reply.error.empty() || reply.statusCode == 0) {
			m_ListError = reply.error.empty() ? "transport error" : reply.error;
			return;
		}
		if (reply.statusCode != 200) {
			m_ListError = "HTTP " + std::to_string(reply.statusCode);
			return;
		}
		NetDirectorySessionList list;
		std::string error;
		if (!NetDirectoryCodec::DecodeSessionList(reply.body, &list, &error)) {
			m_ListError = error;
			return;
		}
		m_ListError.clear();
		m_Rows = std::move(list.sessions);
	}

	void NetDirectoryClient::IssueRegister(uint64_t nowMs) {
		Request request;
		request.method = "POST";
		request.path = "/v1/sessions";
		request.body = NetDirectoryCodec::EncodeRegister(m_Row);
		++m_Registers;
		SetState(State::Registering);
		StartRequest(RequestKind::Register, request);
	}

	void NetDirectoryClient::IssueHeartbeat(uint64_t nowMs) {
		NetDirectoryHeartbeatRequest heartbeat;
		heartbeat.token = m_Token;
		heartbeat.peerCount = m_Row.peerCount;
		heartbeat.seatsFree = m_Row.seatsFree;
		heartbeat.state = m_Running ? "running" : "lobby";
		Request request;
		request.method = "POST";
		request.path = "/v1/sessions/" + m_SessionId + "/heartbeat";
		request.body = NetDirectoryCodec::EncodeHeartbeat(heartbeat);
		++m_Heartbeats;
		StartRequest(RequestKind::Heartbeat, request);
	}

	void NetDirectoryClient::IssueDelete(uint64_t nowMs) {
		Request request;
		request.method = "DELETE";
		request.path = "/v1/sessions/" + m_SessionId;
		const json body = {{"token", m_Token}};
		request.body = body.dump();
		++m_Deletes;
		SetState(State::Deleting);
		StartRequest(RequestKind::Delete, request);
	}

	void NetDirectoryClient::IssueList(uint64_t nowMs) {
		Request request;
		request.method = "GET";
		request.path = "/v1/sessions";
		StartRequest(RequestKind::List, request);
	}

	std::vector<NetDirectoryClient::GameRow> NetDirectoryClient::MergeGameLists(const std::vector<NetLanHostInfo>& lan,
	                                                                          const std::vector<NetDirectorySessionRow>& directory,
	                                                                          const NetDirectoryLocalIdentity& local) {
		std::vector<GameRow> rows;
		rows.reserve(lan.size() + directory.size());
		for (const NetLanHostInfo& host : lan) {
			GameRow row;
			row.source = "LAN";
			row.name = host.hostName;
			row.activity = host.activity;
			row.mode = host.mode;
			row.players = std::to_string(host.playerCount) + "/" + std::to_string(host.maxPlayers);
			row.address = host.address;
			row.port = host.port;
			row.joinable = true;
			rows.push_back(std::move(row));
		}
		for (const NetDirectorySessionRow& session : directory) {
			GameRow row;
			row.source = "NET";
			row.name = session.name;
			row.activity = session.activity;
			row.mode = session.mode;
			row.players = std::to_string(std::max<int64_t>(0, session.peerCount - session.seatsFree)) + "/" + std::to_string(session.peerCount);
			if (!session.listenAddrs.empty()) {
				row.address = session.listenAddrs.front();
			}
			row.port = static_cast<uint16_t>(session.listenPort);
			row.sessionId = session.sessionId;
			std::string why;
			if (!NetDirectoryCodec::IsJoinable(session, local, &why)) {
				row.reason = MapMismatchReason(why);
			} else if (session.seatsFree == 0) {
				row.reason = "full";
			} else if (row.address.empty() || row.port == 0) {
				row.reason = "address";
			} else {
				row.joinable = true;
			}
			rows.push_back(std::move(row));
		}
		return rows;
	}

	const char* NetDirectoryClient::StateName(State state) {
		switch (state) {
			case State::Disabled: return "disabled";
			case State::Idle: return "idle";
			case State::Registering: return "registering";
			case State::Registered: return "registered";
			case State::Deleting: return "deleting";
			case State::Failed: return "failed";
		}
		return "unknown";
	}

	std::string NetDirectoryClient::BuildReportJson() const {
		const json report = {
			{"state", StateName(m_State)},
			{"session_id", m_SessionId},
			{"registers", m_Registers},
			{"heartbeats", m_Heartbeats},
			{"deletes", m_Deletes},
			{"last_status", m_LastStatus},
			{"last_error", m_LastError},
		};
		return report.dump();
	}

} // namespace RTE
