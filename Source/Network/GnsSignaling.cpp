#include "GnsSignaling.h"

#ifdef CCCP_WITH_GNS

#include "nlohmann/json.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

using json = nlohmann::json;

namespace RTE {

	namespace {
		constexpr NetPeerId c_HostPeer = 1; // GnsTransport's client end names its one connection peer 1.
		constexpr int c_RefusedEndReason = k_ESteamNetConnectionEnd_App_Min + 1;
		constexpr size_t c_IdentityChars = 24;

		uint64_t SteadyNowMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		std::string MakeFrame(GnsSignalFrame kind, const void* body, size_t size) {
			std::string frame(1, static_cast<char>(kind));
			frame.append(static_cast<const char*>(body), size);
			return frame;
		}

		std::string KindName(const std::string& frame) {
			if (frame.empty()) {
				return "empty frame";
			}
			const uint8_t kind = static_cast<uint8_t>(frame[0]);
			if (kind == static_cast<uint8_t>(GnsSignalFrame::Rendezvous)) {
				return "rendezvous";
			}
			if (kind == static_cast<uint8_t>(GnsSignalFrame::Refusal)) {
				return "refusal";
			}
			char text[16];
			std::snprintf(text, sizeof(text), "kind 0x%02x", static_cast<unsigned>(kind));
			return text;
		}

		uint64_t ChannelCount(const NetDirectorySignalChannel& channel, const char* name) {
			const json report = json::parse(channel.BuildReportJson(), nullptr, false);
			return report.is_object() ? report.value(name, uint64_t{0}) : 0;
		}

		std::string IdentityText(const SteamNetworkingIdentity& identity) {
			char text[SteamNetworkingIdentity::k_cchMaxString] = {};
			identity.ToString(text, sizeof(text));
			return text;
		}
	}

	GnsDirectorySignaling::GnsDirectorySignaling(std::string peer, int copies, std::shared_ptr<GnsSignalingTally> tally) :
		m_Peer(std::move(peer)), m_Copies(std::max(1, copies)), m_Tally(std::move(tally)) {
		++m_Tally->created;
	}

	GnsDirectorySignaling::~GnsDirectorySignaling() { ++m_Tally->deleted; }

	bool GnsDirectorySignaling::SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* blob, int size) {
		if (!blob || size <= 0) {
			return true;
		}
		const std::string frame = MakeFrame(GnsSignalFrame::Rendezvous, blob, static_cast<size_t>(size));
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Detached) {
			m_Tally->dropped += m_Copies;
			return true;
		}
		for (int copy = 0; copy < m_Copies; ++copy) {
			m_Outbox.push_back({frame, copy > 0});
		}
		return true;
	}

	void GnsDirectorySignaling::Release() {
		++m_Tally->released;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Released = true;
		}
		DropHolder();
	}

	GnsDirectorySignaling::PumpResult GnsDirectorySignaling::Pump(NetDirectorySignalChannel& channel, const Observer& observe) {
		PumpResult result;
		std::vector<Frame> frames;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			frames.swap(m_Outbox);
			result.done = m_Released;
		}
		for (const Frame& frame : frames) {
			const bool posted = channel.Post(m_Peer, frame.bytes);
			result.posted += posted ? 1 : 0;
			result.duplicates += posted && frame.duplicate ? 1 : 0;
			result.refused += posted ? 0 : 1;
			if (observe) {
				observe(frame.bytes, frame.duplicate, posted);
			}
		}
		return result;
	}

	void GnsDirectorySignaling::Detach() {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Detached = true;
			m_Tally->dropped += static_cast<int>(m_Outbox.size());
			m_Outbox.clear();
		}
		DropHolder();
	}

	void GnsDirectorySignaling::DropHolder() {
		if (m_Holders.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			delete this;
		}
	}

	ISteamNetworkingConnectionSignaling* GnsDirectoryRecvContext::OnConnectRequest(HSteamNetConnection connection, const SteamNetworkingIdentity& identityPeer, int localVirtualPort) {
		GnsDirectorySignalDispatcher& dispatcher = m_Dispatcher;
		++dispatcher.m_Counters.connectRequests;
		const std::string identity = IdentityText(identityPeer);
		const std::string request = "OnConnectRequest(connection " + std::to_string(connection) + ", peer " + identity + ", local virtual port " + std::to_string(localVirtualPort) + ") from " + m_Peer;
		if (dispatcher.m_Stopping) {
			dispatcher.Note(request + ": ignored, the dispatcher is stopping");
			return nullptr;
		}
		const std::string refusal = dispatcher.m_Admission ? dispatcher.m_Admission(m_Peer, identity) : std::string();
		if (!refusal.empty()) {
			m_Refusal = refusal.substr(0, GnsDirectorySignalDispatcher::c_MaxRefusalBytes);
			dispatcher.Note(request + ": refused (\"" + m_Refusal + "\"): CloseConnection and return null");
			SteamNetworkingSockets()->CloseConnection(connection, c_RefusedEndReason, m_Refusal.c_str(), false);
			return nullptr;
		}
		dispatcher.Note(request + ": accepted with a signaling object addressed to " + m_Peer);
		return dispatcher.Adopt(m_Peer);
	}

	void GnsDirectoryRecvContext::SendRejectionSignal(const SteamNetworkingIdentity& identityPeer, const void*, int size) {
		// GNS's rejection blob carries no from_identity, so the joiner would drop it; our refusal frame goes instead.
		m_Dispatcher.Note("SendRejectionSignal(peer " + IdentityText(identityPeer) + ", " + std::to_string(size) + " bytes): not forwarded; a refusal frame goes to " + m_Peer);
		m_Dispatcher.PostRefusal(m_Peer, m_Refusal.empty() ? std::string("the host could not take the request") : m_Refusal);
	}

	GnsDirectorySignalDispatcher::GnsDirectorySignalDispatcher() : m_Tally(std::make_shared<GnsSignalingTally>()) {}

	GnsDirectorySignalDispatcher::~GnsDirectorySignalDispatcher() { Stop(); }

	bool GnsDirectorySignalDispatcher::Start(GnsTransport& transport, const Config& config) {
		Stop();
		m_Transport = &transport;
		m_Role = config.role;
		m_SessionId = config.sessionId;
		m_Stopping = false;
		m_Counters = {};
		m_PollArmed = false;
		m_PollWindows.clear();
		m_Channel.SetSink([this](const NetDirectorySignalChannel::Signal& signal) { return Deliver(signal); });
		if (m_Role == Role::Host) {
			m_Channel.ConfigureHost(config.baseUrl, config.installKey, config.certPinSha256, config.sessionId, config.sessionToken);
		} else {
			m_Channel.ConfigureClient(config.baseUrl, config.installKey, config.certPinSha256, config.sessionId);
		}
		return m_Channel.GetState() == NetDirectorySignalChannel::State::Open;
	}

	GnsDirectorySignaling* GnsDirectorySignalDispatcher::CreateJoinSignaling() {
		return m_Transport && m_Role == Role::Joiner ? Adopt("host") : nullptr;
	}

	GnsDirectorySignaling* GnsDirectorySignalDispatcher::Adopt(const std::string& peer) {
		GnsDirectorySignaling* signaling = new GnsDirectorySignaling(peer, m_Copies, m_Tally);
		m_Signalings.push_back(signaling);
		return signaling;
	}

	void GnsDirectorySignalDispatcher::SetPolling(bool armed, uint64_t nowMs) {
		if (armed == m_PollArmed || !m_Transport) {
			return;
		}
		m_PollArmed = armed;
		m_Channel.SetPolling(armed);
		const uint64_t polls = ChannelCount(m_Channel, "polls");
		const uint64_t posted = ChannelCount(m_Channel, "signals_posted");
		if (armed) {
			m_PollsAtArm = polls;
			m_PostsAtArm = posted;
			m_PollWindows.push_back({nowMs, 0, 0, 0});
			Note("polling armed");
			return;
		}
		PollWindow& window = m_PollWindows.back();
		window.disarmedMs = nowMs;
		window.polls = polls - m_PollsAtArm;
		window.signalsPosted = posted - m_PostsAtArm;
		Note("polling disarmed after " + std::to_string(window.disarmedMs - window.armedMs) + " ms: " + std::to_string(window.polls) + " poll(s) and " +
		     std::to_string(window.signalsPosted) + " post(s) while armed");
	}

	void GnsDirectorySignalDispatcher::Update(uint64_t nowMs) {
		if (!m_Transport) {
			return;
		}
		PumpOutboxes();
		m_Channel.Update(nowMs);
	}

	void GnsDirectorySignalDispatcher::Stop() {
		if (!m_Transport) {
			return;
		}
		m_Stopping = true;
		PumpOutboxes();
		m_Counters.unpostedAtStop = m_Channel.PendingPosts();
		SetPolling(false, SteadyNowMs());
		m_Channel.Drain();
		for (GnsDirectorySignaling* signaling : m_Signalings) {
			signaling->Detach();
		}
		m_Signalings.clear();
		m_LastFrameFrom.clear();
		m_Transport = nullptr;
	}

	void GnsDirectorySignalDispatcher::PumpOutboxes() {
		const auto observe = [this](const std::string& frame, bool duplicate, bool posted) {
			if (m_Trace) {
				Note("out " + KindName(frame) + " " + std::to_string(frame.size()) + " bytes" + (duplicate ? " (duplicate copy)" : "") + (posted ? ": queued for POST" : ": the channel refused it"));
			}
		};
		for (auto it = m_Signalings.begin(); it != m_Signalings.end();) {
			const GnsDirectorySignaling::PumpResult result = (*it)->Pump(m_Channel, observe);
			m_Counters.signalsOut += result.posted;
			m_Counters.duplicatesOut += result.duplicates;
			m_Counters.postsRefused += result.refused;
			if (result.done) {
				(*it)->Detach();
				it = m_Signalings.erase(it);
			} else {
				++it;
			}
		}
	}

	bool GnsDirectorySignalDispatcher::Deliver(const NetDirectorySignalChannel::Signal& signal) {
		++m_Counters.signalsIn;
		const std::string& frame = signal.bytes;
		std::string& previous = m_LastFrameFrom[signal.from];
		const bool duplicate = !frame.empty() && frame == previous;
		previous = frame;
		m_Counters.duplicatesIn += duplicate ? 1 : 0;
		const std::string seq = "in seq=" + std::to_string(signal.seq);
		const std::string label = seq + " from=" + signal.from + " " + KindName(frame) + " " + std::to_string(frame.size()) + " bytes" +
		                          (duplicate ? ", byte-identical to the previous frame from this peer" : "");
		const bool fromPeer = m_Role == Role::Host ? signal.from.rfind("client:", 0) == 0 : signal.from == "host";
		const uint8_t kind = frame.empty() ? 0 : static_cast<uint8_t>(frame[0]);
		if (fromPeer && kind == static_cast<uint8_t>(GnsSignalFrame::Rendezvous) && frame.size() > 1) {
			Note(label + ": to ReceivedP2PCustomSignal");
			bool accepted = false;
			if (m_Role == Role::Host) {
				GnsDirectoryRecvContext context(*this, signal.from);
				accepted = m_Transport->ReceiveP2PSignal(frame.data() + 1, static_cast<int>(frame.size() - 1), &context);
			} else {
				// Connections run client to host only, so a joiner has no context to accept a request with.
				accepted = m_Transport->ReceiveP2PSignal(frame.data() + 1, static_cast<int>(frame.size() - 1), nullptr);
			}
			m_Counters.gnsRefused += accepted ? 0 : 1;
			Note(seq + ": ReceivedP2PCustomSignal returned " + (accepted ? "true" : "false"));
		} else if (fromPeer && kind == static_cast<uint8_t>(GnsSignalFrame::Refusal) && m_Role == Role::Joiner) {
			++m_Counters.refusals;
			const std::string reason = "refused by the host: " + frame.substr(1, c_MaxRefusalBytes);
			Note(label + ": Disconnect(\"" + reason + "\")");
			m_Transport->Disconnect(c_HostPeer, reason);
		} else {
			++m_Counters.framesIgnored;
			Note(label + ": ignored");
		}
		return true;
	}

	void GnsDirectorySignalDispatcher::PostRefusal(const std::string& peer, const std::string& reason) {
		const std::string text = reason.substr(0, c_MaxRefusalBytes);
		const std::string frame = MakeFrame(GnsSignalFrame::Refusal, text.data(), text.size());
		if (m_Channel.Post(peer, frame)) {
			++m_Counters.signalsOut;
			++m_Counters.refusals;
			Note("out refusal " + std::to_string(frame.size()) + " bytes to " + peer + ": queued for POST");
		} else {
			++m_Counters.postsRefused;
			Note("out refusal to " + peer + ": the channel refused it");
		}
	}

	std::string GnsDirectorySignalDispatcher::HostIdentity(const std::string& sessionId) {
		std::string digits;
		for (const char ch : sessionId) {
			if (ch != '-' && digits.size() < c_IdentityChars) {
				digits += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
		}
		return "str:h-" + digits;
	}

	std::string GnsDirectorySignalDispatcher::JoinerIdentity(const std::string& joinNonce) {
		return "str:c-" + joinNonce.substr(0, c_IdentityChars);
	}

	std::string GnsDirectorySignalDispatcher::LocalIdentity() const {
		return m_Role == Role::Host ? HostIdentity(m_SessionId) : JoinerIdentity(m_Channel.GetJoinNonce());
	}

	std::string GnsDirectorySignalDispatcher::BuildReportJson() const {
		json windows = json::array();
		for (const PollWindow& window : m_PollWindows) {
			windows.push_back({{"ms", window.disarmedMs >= window.armedMs ? window.disarmedMs - window.armedMs : 0}, {"polls", window.polls}, {"signals_posted", window.signalsPosted}});
		}
		const json report = {
			{"role", m_Role == Role::Host ? "host" : "joiner"},
			{"local_identity", LocalIdentity()},
			{"signals_out", m_Counters.signalsOut},
			{"signals_in", m_Counters.signalsIn},
			{"frames_ignored", m_Counters.framesIgnored},
			{"refusals", m_Counters.refusals},
			{"connect_requests", m_Counters.connectRequests},
			{"duplicates_out", m_Counters.duplicatesOut},
			{"duplicates_in", m_Counters.duplicatesIn},
			{"gns_refused", m_Counters.gnsRefused},
			{"posts_refused", m_Counters.postsRefused},
			{"unposted_at_stop", m_Counters.unpostedAtStop},
			{"poll_windows", windows},
			{"channel", json::parse(m_Channel.BuildReportJson(), nullptr, false)},
		};
		return report.dump();
	}

	void GnsDirectorySignalDispatcher::Note(const std::string& line) const {
		if (m_Trace) {
			m_Trace(line);
		}
	}

} // namespace RTE

#endif
