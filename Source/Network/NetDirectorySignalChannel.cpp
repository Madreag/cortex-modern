#include "NetDirectorySignalChannel.h"

#include "NetHttpClient.h"

#include "Base64/base64.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>

using json = nlohmann::json;

namespace RTE {

	namespace {
		using Request = NetDirectoryClient::Request;
		using Reply = NetDirectoryClient::Reply;

		constexpr char c_InstallKeyAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";
		static_assert(sizeof(c_InstallKeyAlphabet) == 65, "the nonce draws 6 bits per character");

		uint64_t SteadyNowMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		/// The production transport: one NetHttpClient per request, carrying the channel's headers.
		class SignalHttpTransport final : public NetDirectoryClient::Transport {
		public:
			SignalHttpTransport(std::string baseUrl, std::string certPinSha256, NetDirectorySignalChannel::Headers headers) :
				m_BaseUrl(std::move(baseUrl)), m_CertPinSha256(std::move(certPinSha256)), m_Headers(std::move(headers)) {}

			void Start(const Request& request) override {
				m_Client.Start(request.method, m_BaseUrl + request.path, m_Headers, request.body, m_CertPinSha256);
			}
			bool Finished() override { return m_Client.Poll() == NetHttpClient::PollResult::Done; }
			Reply Take() override {
				const NetHttpClient::Response response = m_Client.GetResponse();
				return {response.statusCode, response.body, response.error};
			}
			void Abort() override { m_Client.Cancel(); }

		private:
			NetHttpClient m_Client;
			std::string m_BaseUrl;
			std::string m_CertPinSha256;
			NetDirectorySignalChannel::Headers m_Headers;
		};

		bool IsInstallKeyChar(char ch) {
			const unsigned char c = static_cast<unsigned char>(ch);
			return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || ch == '-' || ch == '_';
		}

		/// A session token or join nonce as the service's valid_peer takes it: 1..64 install-key characters.
		bool IsPeerSecret(const std::string& value) {
			return !value.empty() && value.size() <= NetDirectoryLimits::c_MaxStringChars && std::all_of(value.begin(), value.end(), IsInstallKeyChar);
		}

		bool IsClientPeer(const std::string& peer) {
			return peer.rfind("client:", 0) == 0 && IsPeerSecret(peer.substr(7));
		}

		/// The session id rides the request path, so only a UUID's characters are let through.
		bool IsSessionId(const std::string& id) {
			return !id.empty() && id.size() <= NetDirectoryLimits::c_MaxStringChars &&
			       std::all_of(id.begin(), id.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0 || ch == '-'; });
		}

		bool IsBase64Char(char ch) {
			const unsigned char c = static_cast<unsigned char>(ch);
			return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || ch == '+' || ch == '/';
		}

		/// Standard alphabet, whole quads, '=' only as the last one or two characters: what the service stores.
		bool DecodePayload(const std::string& text, std::string& out) {
			if (text.size() % 4 != 0) {
				return false;
			}
			size_t pad = 0;
			while (pad < 2 && pad < text.size() && text[text.size() - 1 - pad] == '=') {
				++pad;
			}
			if (!std::all_of(text.begin(), text.end() - static_cast<std::ptrdiff_t>(pad), IsBase64Char)) {
				return false;
			}
			out = base64_decode(text);
			return true;
		}

		/// The service's error body as "<error>[:<field>]"; empty when the body carries none.
		std::string ErrorCode(const std::string& body) {
			const json parsed = json::parse(body, nullptr, false);
			if (parsed.is_discarded() || !parsed.is_object()) {
				return {};
			}
			std::string code;
			if (const auto it = parsed.find("error"); it != parsed.end() && it->is_string()) {
				code = it->get<std::string>();
			}
			if (const auto it = parsed.find("field"); !code.empty() && it != parsed.end() && it->is_string()) {
				code += ":" + it->get<std::string>();
			}
			return code;
		}

		/// retry_after_s from a 429 body, held to the service's 1..60 s window; 1 s when absent or unreadable.
		uint64_t RetryAfterMs(const std::string& body) {
			const json parsed = json::parse(body, nullptr, false);
			int64_t seconds = 1;
			if (parsed.is_object()) {
				if (const auto it = parsed.find("retry_after_s"); it != parsed.end() && it->is_number_integer()) {
					seconds = it->get<int64_t>();
				}
			}
			return static_cast<uint64_t>(std::clamp<int64_t>(seconds, 1, static_cast<int64_t>(NetDirectorySignalChannel::c_RetryMaxMs / 1000))) * 1000;
		}

		uint64_t NextBackoff(uint64_t backoffMs) {
			return backoffMs == 0 ? NetDirectorySignalChannel::c_RetryBaseMs : std::min<uint64_t>(backoffMs * 2, NetDirectorySignalChannel::c_RetryMaxMs);
		}
	}

	NetDirectorySignalChannel::~NetDirectorySignalChannel() { AbortRequest(); }

	void NetDirectorySignalChannel::SetTransportFactory(NetDirectoryClient::TransportFactory factory) { m_Factory = std::move(factory); }

	void NetDirectorySignalChannel::ConfigureHost(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId, std::string sessionToken) {
		m_JoinNonce.clear();
		Configure(std::move(baseUrl), std::move(installKey), std::move(certPinSha256), std::move(sessionId), "host", std::move(sessionToken));
	}

	void NetDirectorySignalChannel::ConfigureClient(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId) {
		m_JoinNonce = MintJoinNonce();
		Configure(std::move(baseUrl), std::move(installKey), std::move(certPinSha256), std::move(sessionId), "client:" + m_JoinNonce, m_JoinNonce);
	}

	void NetDirectorySignalChannel::Configure(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId, std::string localPeer, std::string credential) {
		AbortRequest();
		m_Outbox.clear();
		m_Cursor = 0;
		m_NextPollMs = 0;
		m_NextAttemptMs = 0;
		m_BackoffMs = 0;
		m_NextPostMs = 0;
		m_PostBackoffMs = 0;
		m_DrainPolled = false;
		m_SignalsPosted = 0;
		m_SignalsReceived = 0;
		m_Polls = 0;
		m_LastStatus = 0;
		m_LastError.clear();

		while (!baseUrl.empty() && baseUrl.back() == '/') {
			baseUrl.pop_back();
		}
		// The same scheme-less SessionDirectoryUrl NetDirectoryClient reads: the directory is TLS-only.
		if (!baseUrl.empty() && baseUrl.rfind("https://", 0) != 0) {
			baseUrl = "https://" + baseUrl;
		}
		m_BaseUrl = std::move(baseUrl);
		m_CertPinSha256 = std::move(certPinSha256);
		m_SessionPath = "/v1/sessions/" + sessionId;
		m_LocalPeer = std::move(localPeer);
		m_Credential = std::move(credential);
		m_Headers = {{"Content-Type", "application/json"}, {"X-Install-Key", std::move(installKey)}, {"X-Signal-Peer", m_LocalPeer}};
		if (m_LocalPeer == "host") {
			m_Headers.emplace_back("X-Session-Token", m_Credential);
		}
		if (!m_Factory) {
			m_Factory = [this]() { return std::make_unique<SignalHttpTransport>(m_BaseUrl, m_CertPinSha256, m_Headers); };
		}

		if (m_BaseUrl.empty()) {
			SetState(State::Disabled);
		} else if (!IsSessionId(sessionId)) {
			Fail("invalid session id");
		} else if (!IsPeerSecret(m_Credential)) {
			Fail("invalid session token");
		} else {
			SetState(State::Open);
		}
	}

	bool NetDirectorySignalChannel::Post(const std::string& to, const std::string& bytes) {
		// A joiner only ever signals the host; the host answers joiners, never itself.
		const bool reachable = m_LocalPeer == "host" ? IsClientPeer(to) : to == "host";
		if (m_State != State::Open || !reachable || bytes.size() > c_MaxSignalBytes || m_Outbox.size() >= c_MaxPendingPosts) {
			return false;
		}
		m_Outbox.push_back({to, base64_encode(bytes, false)});
		return true;
	}

	void NetDirectorySignalChannel::Update(uint64_t nowMs) {
		if (m_State != State::Open && m_State != State::Draining) {
			return;
		}
		if (m_Request && m_Request->Finished()) {
			const RequestKind kind = m_RequestKind;
			const Reply reply = m_Request->Take();
			m_Request.reset();
			m_RequestKind = RequestKind::None;
			if (reply.statusCode > 0) {
				m_LastStatus = reply.statusCode;
			}
			switch (kind) {
				case RequestKind::Post: HandlePostReply(reply, nowMs); break;
				case RequestKind::Poll: HandlePollReply(reply, nowMs); break;
				default: break;
			}
		}
		if (m_Request) {
			return;
		}
		if (m_State == State::Draining) {
			if (m_DrainPolled) {
				SetState(State::Closed);
			} else {
				m_DrainPolled = true;
				IssuePoll();
			}
			return;
		}
		if (m_State != State::Open || nowMs < m_NextAttemptMs) {
			return;
		}
		if (!m_Outbox.empty() && nowMs >= m_NextPostMs) {
			IssuePost();
		} else if (m_PollArmed && nowMs >= m_NextPollMs) {
			IssuePoll();
		}
	}

	void NetDirectorySignalChannel::Drain() {
		if (m_State != State::Open) {
			return;
		}
		m_DrainPolled = false;
		SetState(State::Draining);
		const uint64_t begin = SteadyNowMs();
		while (m_State == State::Draining && SteadyNowMs() - begin < c_DrainBudgetMs) {
			Update(SteadyNowMs());
			if (m_State == State::Draining) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
		if (m_State == State::Draining) {
			AbortRequest();
			NoteError("drain: no answer within the budget");
			SetState(State::Closed);
		}
		m_Outbox.clear();
	}

	const char* NetDirectorySignalChannel::Role() const { return m_LocalPeer == "host" ? "host" : "client"; }

	void NetDirectorySignalChannel::SetState(State state) {
		if (m_State == state) {
			return;
		}
		// Disabled means no URL was configured; the channel is silent there.
		if (state != State::Disabled) {
			std::cout << "[net-directory-signal] " << Role() << " state: " << StateName(m_State) << " -> " << StateName(state) << std::endl;
		}
		m_State = state;
	}

	void NetDirectorySignalChannel::NoteError(const std::string& error) {
		m_LastError = error;
		std::cout << "[net-directory-signal] " << Role() << " " << error << std::endl;
	}

	void NetDirectorySignalChannel::Fail(const std::string& reason, const std::string& detail) {
		m_LastError = reason;
		std::cout << "[net-directory-signal] " << Role() << " failed: " << reason << (detail.empty() ? "" : " (" + detail + ")") << std::endl;
		AbortRequest();
		m_Outbox.clear();
		SetState(State::Failed);
	}

	void NetDirectorySignalChannel::AbortRequest() {
		if (m_Request) {
			m_Request->Abort();
			m_Request.reset();
		}
		m_RequestKind = RequestKind::None;
	}

	void NetDirectorySignalChannel::ScheduleRetry(uint64_t nowMs) {
		m_BackoffMs = NextBackoff(m_BackoffMs);
		m_NextAttemptMs = nowMs + m_BackoffMs;
	}

	void NetDirectorySignalChannel::StartRequest(RequestKind kind, const Request& request) {
		if (!m_Factory) {
			Fail("no transport configured");
			return;
		}
		m_Request = m_Factory();
		m_RequestKind = kind;
		m_Request->Start(request);
	}

	void NetDirectorySignalChannel::IssuePost() {
		const Outbound& head = m_Outbox.front();
		NetDirectorySignalPost post;
		post.tokenOrJoinNonce = m_Credential;
		post.from = m_LocalPeer;
		post.to = head.to;
		post.payloadB64 = head.payloadB64;
		StartRequest(RequestKind::Post, {"POST", m_SessionPath + "/signal", NetDirectoryCodec::EncodeSignalPost(post)});
	}

	void NetDirectorySignalChannel::IssuePoll() {
		++m_Polls;
		StartRequest(RequestKind::Poll, {"GET", m_SessionPath + "/signals?peer=" + m_LocalPeer + "&after=" + std::to_string(m_Cursor), ""});
	}

	void NetDirectorySignalChannel::HandlePostReply(const Reply& reply, uint64_t nowMs) {
		if (!reply.error.empty() || reply.statusCode == 0) {
			NoteError("post: " + (reply.error.empty() ? std::string("transport error") : reply.error));
			ScheduleRetry(nowMs);
			return;
		}
		if (reply.statusCode != 200) {
			HandleRefusal("post", reply, nowMs);
			return;
		}
		NetDirectorySignalPostResponse response;
		std::string reason;
		if (!NetDirectoryCodec::DecodeSignalPostResponse(reply.body, response, reason) || !response.ok) {
			NoteError("post: " + (reason.empty() ? std::string("not ok") : reason));
			ScheduleRetry(nowMs);
			return;
		}
		m_Outbox.pop_front();
		++m_SignalsPosted;
		m_BackoffMs = 0;
		m_PostBackoffMs = 0;
	}

	void NetDirectorySignalChannel::HandlePollReply(const Reply& reply, uint64_t nowMs) {
		if (!reply.error.empty() || reply.statusCode == 0) {
			NoteError("poll: " + (reply.error.empty() ? std::string("transport error") : reply.error));
			ScheduleRetry(nowMs);
			return;
		}
		if (reply.statusCode != 200) {
			HandleRefusal("poll", reply, nowMs);
			return;
		}
		NetDirectorySignalList list;
		std::vector<Signal> inbound;
		std::string reason;
		if (!NetDirectoryCodec::DecodeSignalList(reply.body, list, reason) || !DecodeInbound(list, inbound, reason)) {
			NoteError("poll: " + reason);
			ScheduleRetry(nowMs);
			return;
		}
		m_BackoffMs = 0;
		m_NextPollMs = nowMs + c_PollIntervalMs;
		for (const Signal& signal : inbound) {
			if (signal.seq <= m_Cursor) {
				continue; // re-delivered: the sink already took it
			}
			if (!m_Sink || !m_Sink(signal)) {
				break; // it and every later one stay at the service for the next poll
			}
			m_Cursor = signal.seq;
			++m_SignalsReceived;
		}
	}

	void NetDirectorySignalChannel::HandleRefusal(const char* what, const Reply& reply, uint64_t nowMs) {
		const std::string code = ErrorCode(reply.body);
		const std::string status = std::string(what) + ": HTTP " + std::to_string(reply.statusCode) + (code.empty() ? "" : " " + code);
		if (reply.statusCode == 429) {
			const uint64_t waitMs = RetryAfterMs(reply.body);
			NoteError(status + ", retrying in " + std::to_string(waitMs) + " ms");
			m_NextAttemptMs = nowMs + waitMs;
		} else if (reply.statusCode == 404) {
			Fail("session gone", status);
		} else if (reply.statusCode == 403) {
			Fail("bad credential", status);
		} else if (reply.statusCode == 400 && code == "queue_full") {
			NoteError(status);
			m_PostBackoffMs = NextBackoff(m_PostBackoffMs);
			m_NextPostMs = nowMs + m_PostBackoffMs;
		} else if (reply.statusCode >= 500) {
			NoteError(status);
			ScheduleRetry(nowMs);
		} else {
			Fail(status);
		}
	}

	bool NetDirectorySignalChannel::DecodeInbound(const NetDirectorySignalList& list, std::vector<Signal>& out, std::string& reason) const {
		out.clear();
		for (const NetDirectorySignal& item : list.signals) {
			Signal signal;
			signal.seq = item.seq;
			signal.from = item.from;
			if (item.to != m_LocalPeer) {
				reason = "signal " + std::to_string(item.seq) + " is addressed to another peer";
				return false;
			}
			if (!DecodePayload(item.payloadB64, signal.bytes) || signal.bytes.size() > c_MaxSignalBytes) {
				reason = "signal " + std::to_string(item.seq) + " carries no valid payload";
				return false;
			}
			out.push_back(std::move(signal));
		}
		std::stable_sort(out.begin(), out.end(), [](const Signal& left, const Signal& right) { return left.seq < right.seq; });
		return true;
	}

	std::string NetDirectorySignalChannel::MintJoinNonce() {
		std::random_device device;
		std::string nonce(c_JoinNonceChars, '0');
		for (char& ch : nonce) {
			ch = c_InstallKeyAlphabet[device() % 64];
		}
		return nonce;
	}

	const char* NetDirectorySignalChannel::StateName(State state) {
		switch (state) {
			case State::Disabled: return "disabled";
			case State::Open: return "open";
			case State::Draining: return "draining";
			case State::Closed: return "closed";
			case State::Failed: return "failed";
		}
		return "unknown";
	}

	std::string NetDirectorySignalChannel::BuildReportJson() const {
		const json report = {
			{"state", StateName(m_State)},
			{"signals_posted", m_SignalsPosted},
			{"signals_received", m_SignalsReceived},
			{"polls", m_Polls},
			{"last_status", m_LastStatus},
			{"last_error", m_LastError},
		};
		return report.dump();
	}

} // namespace RTE
