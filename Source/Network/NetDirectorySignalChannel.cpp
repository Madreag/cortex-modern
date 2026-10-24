#include "NetDirectorySignalChannel.h"
#include "SteadyClock.h"
#include "DiagnosticLine.h"

#include "NetHttpClient.h"

#include "Base64/base64.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
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
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyNow().time_since_epoch()).count());
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
				return {response.statusCode, response.body, response.error, response.retryAfter};
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

		bool IsHostPeer(const std::string& peer) {
			return peer == "host" || (peer.rfind("host:", 0) == 0 && IsPeerSecret(peer.substr(5)));
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


	}

	NetDirectorySignalChannel::~NetDirectorySignalChannel() { AbortRequest(); }

	void NetDirectorySignalChannel::SetTransportFactory(NetDirectoryClient::TransportFactory factory) { m_Factory = std::move(factory); m_HttpTransport = false; }

	void NetDirectorySignalChannel::ConfigureHost(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId, std::string sessionToken, std::string signalPeer) {
		m_JoinNonce.clear();
		if (!IsHostPeer(signalPeer)) { Fail("invalid host signal peer"); return; }
		Configure(std::move(baseUrl), std::move(installKey), std::move(certPinSha256), std::move(sessionId), std::move(signalPeer), std::move(sessionToken));
	}

	void NetDirectorySignalChannel::ConfigureClient(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId) {
		m_JoinNonce = MintJoinNonce();
		Configure(std::move(baseUrl), std::move(installKey), std::move(certPinSha256), std::move(sessionId), "client:" + m_JoinNonce, m_JoinNonce);
	}

	void NetDirectorySignalChannel::RebindHost(const std::string& sessionId, const std::string& sessionToken) {
		if (!IsHostPeer(m_LocalPeer) || m_BaseUrl.empty() || !IsSessionId(sessionId) || !IsPeerSecret(sessionToken)) return;
		AbortRequest();
		m_SessionPath = "/v1/sessions/" + sessionId;
		m_Credential = sessionToken;
		for (auto& header: m_Headers) if (header.first == "X-Session-Token") header.second = sessionToken;
		// A recovered service can start its per-peer sequences over.
		m_Cursor = 0;
		m_NextPollMs = 0;
		m_DrainPolled = false;
		m_LastError.clear();
		SetState(State::Open);
	}

	void NetDirectorySignalChannel::Configure(std::string baseUrl, std::string installKey, std::string certPinSha256, std::string sessionId, std::string localPeer, std::string credential) {
		AbortRequest();
		m_Outbox.clear();
		m_Cursor = 0;
		m_NextPollMs = 0;
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
		m_InstallKey = installKey;
		m_Headers = {{"Content-Type", "application/json"}, {"X-Install-Key", std::move(installKey)}, {"X-Signal-Peer", m_LocalPeer}};
		if (IsHostPeer(m_LocalPeer)) {
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
		const bool reachable = IsHostPeer(m_LocalPeer) ? IsClientPeer(to) : IsHostPeer(to);
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
        if (m_HttpTransport && NetDirectoryBudget::SessionEnded(m_BaseUrl, m_InstallKey, m_SessionPath.substr(13), nowMs)) {
            Fail("This match is no longer in the directory. Choose another game."); return;
        }
		const RequestKind first = m_PostRequest && m_PollRequest && m_PollRequestSeq < m_PostRequestSeq ? RequestKind::Poll : RequestKind::Post;
		for (const RequestKind kind : {first, first == RequestKind::Post ? RequestKind::Poll : RequestKind::Post}) {
			TakeReply(kind, nowMs);
			if (m_State != State::Open && m_State != State::Draining) {
				return;
			}
		}
		if (nowMs < m_NextAttemptMs) return;
		const bool host = IsHostPeer(m_LocalPeer);
		const bool pollReady = nowMs >= m_PollRetry.nextMs && (!m_HttpTransport || NetDirectoryBudget::Ready(m_BaseUrl, m_InstallKey, host ? "admission" : "general", nowMs));
		const bool postReady = nowMs >= m_PostRetry.nextMs && (!m_HttpTransport || NetDirectoryBudget::Ready(m_BaseUrl, m_InstallKey, "general", nowMs));
		if (m_State == State::Draining) {
			if (m_PostRequest || m_PollRequest) {
				return;
			}
			if (m_DrainPolled) {
				SetState(State::Closed);
			} else {
				if (!pollReady) return;
				m_DrainPolled = true;
				IssuePoll(nowMs);
			}
			return;
		}
		// A post never waits behind the held poll, except that after a refusal the host's poll is answered first.
		if (!m_PostRequest && !m_Outbox.empty() && postReady && nowMs >= m_NextPostMs && !(m_PollFirst && m_PollArmed)) {
			IssuePost();
		}
		if (!m_PollRequest && pollReady && m_PollArmed && (m_PollFirst || nowMs >= m_NextPollMs)) {
			IssuePoll(nowMs);
		}
	}

	void NetDirectorySignalChannel::TakeReply(RequestKind kind, uint64_t nowMs) {
		std::unique_ptr<NetDirectoryClient::Transport>& request = kind == RequestKind::Post ? m_PostRequest : m_PollRequest;
		if (!request || !request->Finished()) {
			return;
		}
		const Reply reply = request->Take();
		request.reset();
		if (reply.statusCode > 0) {
			m_LastStatus = reply.statusCode;
		}
		// An answer proves the admission poll reached the service before another request is released.
		if (kind == RequestKind::Poll && m_HttpTransport && IsHostPeer(m_LocalPeer) && reply.statusCode > 0 && reply.statusCode != 429)
			NetDirectoryBudget::PollAnswered(m_BaseUrl, m_InstallKey, nowMs);
		m_ReplyKind = kind; m_RetryReply = reply;
		if (kind == RequestKind::Post) {
			HandlePostReply(reply, nowMs);
		} else {
			HandlePollReply(reply, nowMs);
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

	void NetDirectorySignalChannel::Abandon() {
		if (m_State != State::Open && m_State != State::Draining) {
			return;
		}
		AbortRequest();
		m_Outbox.clear();
		SetState(State::Closed);
	}

	const char* NetDirectorySignalChannel::Role() const { return IsHostPeer(m_LocalPeer) ? "host" : "client"; }

	void NetDirectorySignalChannel::SetState(State state) {
		if (m_State == state) {
			return;
		}
		// Disabled means no URL was configured; the channel is silent there.
		if (state != State::Disabled) {
			DiagnosticLine() << "[net-directory-signal] " << Role() << " state: " << StateName(m_State) << " -> " << StateName(state) << std::endl;
		}
		m_State = state;
	}

	void NetDirectorySignalChannel::NoteError(const std::string& error) {
		m_LastError = error;

	}

	void NetDirectorySignalChannel::Fail(const std::string& reason, const std::string& detail) {
		m_LastError = reason;
		if (m_ReplyKind == RequestKind::None) DiagnosticLine() << "[net-directory-signal] " << Role() << " failed: " << reason << (detail.empty() ? "" : " (" + detail + ")") << std::endl;
		AbortRequest();
		if (!IsHostPeer(m_LocalPeer)) m_Outbox.clear();
		SetState(State::Failed);
	}

	void NetDirectorySignalChannel::AbortRequest() {
		for (std::unique_ptr<NetDirectoryClient::Transport>* request : {&m_PostRequest, &m_PollRequest}) {
			if (*request) {
				(*request)->Abort();
				request->reset();
			}
		}
	}

	void NetDirectorySignalChannel::ScheduleRetry(uint64_t nowMs) {
		auto& retry = m_ReplyKind == RequestKind::Post ? m_PostRetry : m_PollRetry;
		const uint64_t until = retry.Failed(nowMs, m_RetryReply.statusCode, m_RetryReply.body, m_RetryReply.retryAfter, m_ReplyKind == RequestKind::Post ? "signal-post" : "signal-poll");
		const bool postQueueFull = m_ReplyKind == RequestKind::Post &&
		    (m_RetryReply.statusCode == 400 || m_RetryReply.statusCode == 503) && ErrorCode(m_RetryReply.body) == "queue_full";
		// A full post queue still needs its reader; other failed exchanges hold the whole channel.
		if (!postQueueFull) m_NextAttemptMs = std::max(m_NextAttemptMs, until);
	}

	void NetDirectorySignalChannel::StartRequest(RequestKind kind, const Request& request) {
		if (!m_Factory) {
			Fail("no transport configured");
			return;
		}
		std::unique_ptr<NetDirectoryClient::Transport>& slot = kind == RequestKind::Post ? m_PostRequest : m_PollRequest;
		(kind == RequestKind::Post ? m_PostRequestSeq : m_PollRequestSeq) = ++m_RequestSeq;
		slot = m_Factory();
		slot->Start(request);
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

	void NetDirectorySignalChannel::IssuePoll(uint64_t nowMs) {
		m_PollStartedMs = nowMs;
		++m_Polls;
		// The last poll of a teardown reads what is there without being held.
		const int waitS = m_State == State::Draining ? 0 : m_PollWaitS;
		std::string path = m_SessionPath + "/signals?peer=" + m_LocalPeer + "&after=" + std::to_string(m_Cursor);
		if (waitS > 0) {
			path += "&wait=" + std::to_string(waitS);
		}
		StartRequest(RequestKind::Poll, {"GET", path, ""});
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
		if (m_LastError == NetDirectoryClient::c_CapacityNotice) m_LastError.clear();
		m_Outbox.pop_front();
		++m_SignalsPosted;
		m_BackoffMs = 0;
		m_PostBackoffMs = 0; m_PostRetry.Succeeded();
	}

	void NetDirectorySignalChannel::HandlePollReply(const Reply& reply, uint64_t nowMs) {
		m_PollFirst = false;
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
		m_BackoffMs = 0; m_PollRetry.Succeeded();
		// An empty answer is an idle wait only as long as the service held the poll: the directory holds a long poll for less than
		// asked and names its retry.
		if (m_PollWaitS == 0) {
			m_NextPollMs = nowMs + c_PollIntervalMs;
		} else if (!inbound.empty()) {
			m_NextPollMs = nowMs;
		} else if (const uint64_t retryMs = NetDirectoryRetry::RetryAfterMs(reply.body, reply.retryAfter); retryMs > 0) {
			m_NextPollMs = std::max(nowMs + retryMs, m_PollStartedMs + c_PollIntervalMs);
		} else {
			// A server that answers an empty long poll early without naming a retry still gets the idle cadence.
			m_NextPollMs = std::max(nowMs, m_PollStartedMs + std::max<uint64_t>(c_PollIntervalMs, m_PollWaitS * 1000ULL));
		}
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
		ScheduleRetry(nowMs);
		if (reply.statusCode == 429) {
			const uint64_t until = std::max(m_PostRetry.nextMs, m_PollRetry.nextMs);
			m_NextAttemptMs = std::max(m_NextAttemptMs, until); m_BusyUntilMs.store(until);
			m_PollFirst = IsHostPeer(m_LocalPeer);
			if (m_HttpTransport) NetDirectoryBudget::Refused(m_BaseUrl, m_InstallKey, m_PollFirst && std::string(what) == "poll" ? "admission" : "general", nowMs, until, reply.body, m_PollFirst);
			NoteError("The directory is busy. The host retries in " + std::to_string((until - nowMs + 999) / 1000) + " seconds. Cancel returns to Multiplayer.");
		} else if (NetDirectoryRetry::SessionGone(reply.statusCode, reply.body)) {
			if (m_HttpTransport) NetDirectoryBudget::EndSession(m_BaseUrl, m_InstallKey, m_SessionPath.substr(13), nowMs);
			Fail("This match is no longer in the directory. Choose another game.");
		} else if (reply.statusCode == 403) {
			Fail("The directory could not confirm your place in this match. Choose another game.");
		} else if (NetDirectoryClient::IsCapacityReply(reply)) NoteError(NetDirectoryClient::c_CapacityNotice);
		else NoteError("The directory is unavailable. Joining will retry; Cancel returns to Multiplayer.");
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
		return report.dump(-1, ' ', false, json::error_handler_t::replace);
	}

} // namespace RTE
