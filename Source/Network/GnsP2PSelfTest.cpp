#include "GnsP2PSelfTest.h"

#include "GnsTransport.h"

#include <iostream>

#ifdef CCCP_WITH_GNS
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#endif

namespace RTE {

#ifdef CCCP_WITH_GNS

	namespace {

		using Clock = std::chrono::steady_clock;

		constexpr int c_HostVirtualPort = 47641;
		constexpr int c_MessageSize = 64;
		constexpr int c_LogLevel = k_ESteamNetworkingSocketsDebugOutputType_Verbose;
		constexpr int c_RejectReason = k_ESteamNetConnectionEnd_App_Min + 1;
		constexpr double c_PromptMs = 2000;

		Clock::time_point s_Start = Clock::now();
		std::mutex s_OutputMutex;
		std::vector<std::string> s_LocalCandidates; //!< Every candidate GNS queued for a peer, as "address port typ type".
		std::set<std::string> s_RemoteHostCandidates; //!< "address:port" of every host candidate a peer sent.

		double ElapsedMs() {
			return std::chrono::duration<double, std::milli>(Clock::now() - s_Start).count();
		}

		std::string Ms(double ms) {
			char text[32];
			std::snprintf(text, sizeof(text), "%.1f", ms);
			return text;
		}

		std::string Hex(int value) {
			char text[16];
			std::snprintf(text, sizeof(text), "0x%x", value);
			return text;
		}

		void Say(const std::string& line) {
			std::lock_guard<std::mutex> lock(s_OutputMutex);
			std::cout << "[net-p2p-selftest] t=" << Ms(ElapsedMs()) << "ms " << line << std::endl;
		}

		int Finish(const std::string& failure) {
			SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
			std::lock_guard<std::mutex> lock(s_OutputMutex);
			std::cout << "[net-p2p-selftest] " << (failure.empty() ? std::string("PASS") : "FAIL: " + failure) << std::endl;
			return failure.empty() ? 0 : 1;
		}

		// Reads "candidate:<foundation> <component> udp <priority> <address> <port> typ <type>" after opener.
		bool ParseCandidate(const std::string& line, const std::string& opener, std::string* address, std::string* port, std::string* type) {
			const std::string marker = opener + "candidate:";
			const size_t at = line.find(marker);
			if (at == std::string::npos) {
				return false;
			}
			std::istringstream words(line.substr(at + marker.size()));
			std::string foundation;
			std::string component;
			std::string protocol;
			std::string priority;
			std::string typ;
			words >> foundation >> component >> protocol >> priority >> *address >> *port >> typ >> *type;
			while (!type->empty() && (type->back() == '"' || type->back() == '\'')) {
				type->pop_back();
			}
			return typ == "typ" && !address->empty();
		}

		std::string AddressKey(const std::string& address, const std::string& port) {
			return address.find(':') != std::string::npos ? "[" + address + "]:" + port : address + ":" + port;
		}

		void NoteCandidates(const std::string& line) {
			std::string address;
			std::string port;
			std::string type;
			if (line.find("LocalCandidateAdded") != std::string::npos && ParseCandidate(line, "\"", &address, &port, &type)) {
				s_LocalCandidates.push_back(address + " " + port + " typ " + type);
			} else if (line.find("Got remote candidate") != std::string::npos && ParseCandidate(line, "'", &address, &port, &type) && type == "host") {
				s_RemoteHostCandidates.insert(AddressKey(address, port));
			}
		}

		void GnsDebugOutput(ESteamNetworkingSocketsDebugOutputType type, const char* message) {
			// GNS may call this on its service thread while holding its lock: print and parse only.
			std::istringstream lines(message ? message : "");
			std::lock_guard<std::mutex> lock(s_OutputMutex);
			for (std::string line; std::getline(lines, line);) {
				if (!line.empty()) {
					std::cout << "[net-p2p-selftest] t=" << Ms(ElapsedMs()) << "ms gns(" << static_cast<int>(type) << ") " << line << '\n';
					NoteCandidates(line);
				}
			}
			std::cout.flush();
		}

		void EnableGnsOutput() {
			SteamNetworkingUtils()->SetDebugOutputFunction(static_cast<ESteamNetworkingSocketsDebugOutputType>(c_LogLevel), GnsDebugOutput);
			SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, c_LogLevel);
			Say("GNS debug output: SetDebugOutputFunction(" + std::to_string(c_LogLevel) + " = Verbose), global LogLevel_P2PRendezvous=" + std::to_string(c_LogLevel));
		}

		std::string StateName(int state) {
			switch (state) {
				case k_ESteamNetworkingConnectionState_None: return "None";
				case k_ESteamNetworkingConnectionState_Connecting: return "Connecting";
				case k_ESteamNetworkingConnectionState_FindingRoute: return "FindingRoute";
				case k_ESteamNetworkingConnectionState_Connected: return "Connected";
				case k_ESteamNetworkingConnectionState_ClosedByPeer: return "ClosedByPeer";
				case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return "ProblemDetectedLocally";
				default: return std::to_string(state);
			}
		}

		std::string FlagNames(int flags) {
			std::string names;
			const auto add = [&](int bit, const char* name) {
				if (flags & bit) {
					names += (names.empty() ? "" : "|") + std::string(name);
				}
			};
			add(k_nSteamNetworkConnectionInfoFlags_Unauthenticated, "Unauthenticated");
			add(k_nSteamNetworkConnectionInfoFlags_Unencrypted, "Unencrypted");
			add(k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers, "LoopbackBuffers");
			add(k_nSteamNetworkConnectionInfoFlags_Fast, "Fast");
			add(k_nSteamNetworkConnectionInfoFlags_Relayed, "Relayed");
			add(k_nSteamNetworkConnectionInfoFlags_DualWifi, "DualWifi");
			return names.empty() ? "none" : names;
		}

		std::string IceEnableNames(int value) {
			if (value == k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All) {
				return "All";
			}
			std::string names;
			const auto add = [&](int bit, const char* name) {
				if (value & bit) {
					names += (names.empty() ? "" : "|") + std::string(name);
				}
			};
			add(k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay, "Relay");
			add(k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Private, "Private");
			add(k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public, "Public");
			return names.empty() ? "Disable" : names;
		}

		std::string EventName(NetTransportEventType type) {
			switch (type) {
				case NetTransportEventType::PeerConnected: return "PeerConnected";
				case NetTransportEventType::PeerDisconnected: return "PeerDisconnected";
				case NetTransportEventType::PacketReceived: return "PacketReceived";
				case NetTransportEventType::ConnectionFailed: return "ConnectionFailed";
				case NetTransportEventType::TransportError: return "TransportError";
				case NetTransportEventType::LocalTransportFault: return "LocalTransportFault";
			}
			return "?";
		}

		void PrintConfig(const std::string& side, const GnsP2PConfig& config) {
			Say("config " + side + ": P2P_Transport_ICE_Enable=" + std::to_string(config.iceEnable) + " (" + IceEnableNames(config.iceEnable) + ")" +
			    " P2P_STUN_ServerList=\"" + config.stunServerList + "\"" +
			    " P2P_Transport_ICE_Implementation=" + std::to_string(config.iceImplementation) + " (1 = native ICE client)" +
			    " LogLevel_P2PRendezvous=" + std::to_string(config.rendezvousLogLevel) +
			    " local_identity=" + (config.localIdentity.empty() ? "(process default, no ResetIdentity)" : config.localIdentity) +
			    " local_virtual_port=" + std::to_string(config.localVirtualPort));
		}

		std::vector<uint8_t> Payload(char tag) {
			std::vector<uint8_t> bytes(c_MessageSize);
			for (int i = 0; i < c_MessageSize; ++i) {
				bytes[i] = static_cast<uint8_t>(i == 0 ? tag : i);
			}
			return bytes;
		}

		struct Signal {
			std::vector<uint8_t> bytes;
			double queuedMs = 0;
			int number = 0;
		};

		/// One direction of the rendezvous. GNS pushes from any of its threads; the test thread takes.
		class SignalQueue {
		public:
			explicit SignalQueue(std::string name) : m_Name(std::move(name)) {}

			void Push(const void* data, int size, const char* source) {
				int number = 0;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					number = ++m_Pushed;
					Signal signal;
					signal.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
					signal.queuedMs = ElapsedMs();
					signal.number = number;
					m_Signals.push_back(std::move(signal));
				}
				Say("signal " + m_Name + " #" + std::to_string(number) + " queued by " + source + " (" + std::to_string(size) + " bytes)");
			}

			std::deque<Signal> TakeAll() {
				std::lock_guard<std::mutex> lock(m_Mutex);
				std::deque<Signal> taken;
				taken.swap(m_Signals);
				return taken;
			}

			const std::string& Name() const { return m_Name; }

		private:
			const std::string m_Name;
			std::mutex m_Mutex;
			std::deque<Signal> m_Signals;
			int m_Pushed = 0;
		};

		/// The stub's side of one connection: every rendezvous GNS emits for it is queued towards the peer.
		class StubConnectionSignaling final : public ISteamNetworkingConnectionSignaling {
		public:
			StubConnectionSignaling(std::shared_ptr<SignalQueue> out, std::shared_ptr<std::atomic<int>> releases) : m_Out(std::move(out)), m_Releases(std::move(releases)) {}

			bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* message, int size) override {
				m_Out->Push(message, size, "SendSignal");
				return true;
			}

			void Release() override {
				++*m_Releases;
				delete this;
			}

		private:
			std::shared_ptr<SignalQueue> m_Out;
			std::shared_ptr<std::atomic<int>> m_Releases;
		};

		enum class Answer { Accept, Reject, Ignore };

		/// The stub's receive context: answers a connect request and queues the rejections GNS marshals.
		class StubRecvContext final : public ISteamNetworkingSignalingRecvContext {
		public:
			StubRecvContext(std::shared_ptr<SignalQueue> out, std::shared_ptr<std::atomic<int>> releases, Answer answer) : m_Out(std::move(out)), m_Releases(std::move(releases)), m_Answer(answer) {}

			ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection connection, const SteamNetworkingIdentity& identityPeer, int localVirtualPort) override {
				char peer[SteamNetworkingIdentity::k_cchMaxString] = {};
				identityPeer.ToString(peer, sizeof(peer));
				const std::string request = "OnConnectRequest(connection " + std::to_string(connection) + ", peer " + peer + ", local virtual port " + std::to_string(localVirtualPort) + ")";
				if (m_Requests++ == 0) {
					m_FirstRequestMs = ElapsedMs();
				}
				if (m_Answer == Answer::Reject) {
					Say(request + ": CloseConnection(" + std::to_string(c_RejectReason) + ") and return null, so GNS marshals a rejection");
					SteamNetworkingSockets()->CloseConnection(connection, c_RejectReason, "net-p2p-selftest rejects the request", false);
					return nullptr;
				}
				if (m_Answer == Answer::Ignore) {
					Say(request + ": ignored");
					return nullptr;
				}
				Say(request + ": returning a stub signaling object; the transport accepts on the Connecting callback");
				return new StubConnectionSignaling(m_Out, m_Releases);
			}

			void SendRejectionSignal(const SteamNetworkingIdentity& identityPeer, const void* message, int size) override {
				char peer[SteamNetworkingIdentity::k_cchMaxString] = {};
				identityPeer.ToString(peer, sizeof(peer));
				++m_Rejections;
				Say("SendRejectionSignal(peer " + std::string(peer) + ", " + std::to_string(size) + " bytes)");
				m_Out->Push(message, size, "SendRejectionSignal");
			}

			int Requests() const { return m_Requests; }
			int Rejections() const { return m_Rejections; }
			double FirstRequestMs() const { return m_FirstRequestMs; }

		private:
			std::shared_ptr<SignalQueue> m_Out;
			std::shared_ptr<std::atomic<int>> m_Releases;
			Answer m_Answer;
			int m_Requests = 0;
			int m_Rejections = 0;
			double m_FirstRequestMs = -1;
		};

		struct DropRule {
			std::string direction; //!< "joiner->host" or "host->joiner".
			int number = 0;
			double droppedMs = -1;
			double nextMs = -1;
		};

		struct Side {
			explicit Side(const char* sideName) : name(sideName) {}

			const char* name;
			GnsTransport transport;
			NetPeerId peer = c_InvalidNetPeerId;
			bool connected = false;
			bool closed = false;
			int lastState = -1;
			std::string closeReason; //!< Of the first close event; the transport can report more after it.
			double closedMs = -1;
			std::vector<std::vector<uint8_t>> received;
		};

		/// Returns how many of the delivered signals ReceivedP2PCustomSignal refused.
		int Deliver(SignalQueue& queue, GnsTransport& receiver, ISteamNetworkingSignalingRecvContext& context, DropRule* drop) {
			int refused = 0;
			for (Signal& signal : queue.TakeAll()) {
				const std::string label = "signal " + queue.Name() + " #" + std::to_string(signal.number);
				if (drop && drop->direction == queue.Name()) {
					if (signal.number == drop->number) {
						drop->droppedMs = signal.queuedMs;
						Say(label + " DROPPED by the stub");
						continue;
					}
					if (drop->droppedMs >= 0 && drop->nextMs < 0) {
						drop->nextMs = signal.queuedMs;
						Say(label + " is the first one queued after the drop, " + Ms(drop->nextMs - drop->droppedMs) + "ms after the dropped one");
					}
				}
				const bool accepted = receiver.ReceiveP2PSignal(signal.bytes.data(), static_cast<int>(signal.bytes.size()), &context);
				refused += accepted ? 0 : 1;
				Say(label + " delivered: ReceivedP2PCustomSignal returned " + (accepted ? "true" : "false") + " after " + Ms(ElapsedMs() - signal.queuedMs) + "ms queued");
			}
			return refused;
		}

		void Drain(Side& side) {
			for (NetTransportEvent& event : side.transport.PollEvents()) {
				const std::string what = std::string(side.name) + " event " + EventName(event.type) + " peer=" + std::to_string(event.peerId);
				switch (event.type) {
					case NetTransportEventType::PeerConnected:
						side.peer = event.peerId;
						side.connected = true;
						Say(what);
						break;
					case NetTransportEventType::PacketReceived:
						Say(what + " bytes=" + std::to_string(event.bytes.size()));
						side.received.push_back(std::move(event.bytes));
						break;
					default:
						if (!side.closed) {
							side.closed = true;
							side.closeReason = event.reason;
							side.closedMs = ElapsedMs();
						}
						Say(what + " reason=\"" + event.reason + "\"");
						break;
				}
			}
			if (side.peer != c_InvalidNetPeerId && !side.closed) {
				const int state = side.transport.GetPeerConnectionInfo(side.peer).state;
				if (state != side.lastState) {
					side.lastState = state;
					Say(std::string(side.name) + " GNS connection state " + StateName(state));
				}
			}
		}

		bool WaitUntil(double timeoutMs, const std::function<void()>& pump, const std::function<bool()>& done) {
			const double deadline = ElapsedMs() + timeoutMs;
			while (!done()) {
				if (ElapsedMs() > deadline) {
					return false;
				}
				pump();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}

		bool IsConnected(Side& side) {
			return side.peer != c_InvalidNetPeerId && side.transport.GetPeerConnectionInfo(side.peer).state == k_ESteamNetworkingConnectionState_Connected;
		}

		void PrintConnection(Side& side) {
			const GnsPeerConnectionInfo info = side.transport.GetPeerConnectionInfo(side.peer);
			const std::string name = side.name;
			Say(name + " GetConnectionInfo: state=" + StateName(info.state) + " remote_identity=" + info.remoteIdentity +
			    " remote_address=" + (info.remoteAddress.empty() ? "(none)" : info.remoteAddress) + " flags=" + Hex(info.flags) + " (" + FlagNames(info.flags) + ")" +
			    " relay_pop=" + std::to_string(info.relayPop) + " end_reason=" + std::to_string(info.endReason) + " description=\"" + info.description + "\"");
			for (const std::string& value : info.config) {
				Say(name + " connection config " + value);
			}
			std::istringstream lines(side.transport.GetPeerDetailedStatus(side.peer));
			for (std::string line; std::getline(lines, line);) {
				if (!line.empty()) {
					Say(name + " GetDetailedConnectionStatus: " + line);
				}
			}
		}

		// A host-candidate route: not relayed, not an in-process pipe, and the remote address is a host candidate the peer sent.
		bool CheckHostRoute(Side& side, std::string* failure) {
			const GnsPeerConnectionInfo info = side.transport.GetPeerConnectionInfo(side.peer);
			bool offered = false;
			{
				std::lock_guard<std::mutex> lock(s_OutputMutex);
				offered = s_RemoteHostCandidates.count(info.remoteAddress) != 0;
			}
			const bool relayed = (info.flags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0 || info.relayPop != 0;
			const bool pipe = (info.flags & k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers) != 0;
			Say(std::string(side.name) + " route: remote address " + (info.remoteAddress.empty() ? "(none)" : info.remoteAddress) +
			    (offered ? " is a host candidate the peer sent" : " is NOT a host candidate the peer sent") +
			    (relayed ? "; RELAYED" : "; not relayed (Relayed flag clear, relay POP 0)") +
			    (pipe ? "; an in-process pipe (LoopbackBuffers)" : "; not an in-process pipe (LoopbackBuffers clear)"));
			if (relayed || pipe || !offered || info.state != k_ESteamNetworkingConnectionState_Connected) {
				*failure = std::string(side.name) + " is not connected over a peer host candidate";
				return false;
			}
			return true;
		}

		std::string ConnectExchangeClose(Side& host, Side& joiner, const std::function<void()>& pump, double connectMs) {
			if (!WaitUntil(15000, pump, [&] { return host.closed || joiner.closed || (joiner.connected && IsConnected(joiner) && IsConnected(host)); }) || host.closed || joiner.closed) {
				return "the connection did not reach Connected on both sides within 15s (host " + StateName(host.transport.GetPeerConnectionInfo(host.peer).state) + " \"" + host.closeReason +
				       "\", joiner " + StateName(joiner.transport.GetPeerConnectionInfo(joiner.peer).state) + " \"" + joiner.closeReason + "\")";
			}
			Say("both sides Connected " + Ms(ElapsedMs() - connectMs) + "ms after ConnectP2P");

			const std::vector<uint8_t> fromJoiner = Payload('J');
			const std::vector<uint8_t> fromHost = Payload('H');
			std::string error;
			if (!joiner.transport.Send(joiner.peer, NetTransportLane::ControlReliable, fromJoiner, &error)) {
				return "joiner Send: " + error;
			}
			Say("joiner sent " + std::to_string(c_MessageSize) + " bytes (reliable) to peer " + std::to_string(joiner.peer));
			if (!host.transport.Send(host.peer, NetTransportLane::ControlReliable, fromHost, &error)) {
				return "host Send: " + error;
			}
			Say("host sent " + std::to_string(c_MessageSize) + " bytes (reliable) to peer " + std::to_string(host.peer));
			if (!WaitUntil(5000, pump, [&] { return !host.received.empty() && !joiner.received.empty(); })) {
				return "the 64-byte messages did not cross both ways within 5s";
			}
			if (host.received.front() != fromJoiner || joiner.received.front() != fromHost) {
				return "a 64-byte message arrived altered";
			}
			Say("host received the joiner's 64 bytes intact; joiner received the host's 64 bytes intact");

			PrintConnection(joiner);
			PrintConnection(host);
			std::string failure;
			if (!CheckHostRoute(joiner, &failure) || !CheckHostRoute(host, &failure)) {
				return failure;
			}

			joiner.transport.Disconnect(joiner.peer, "net-p2p-selftest done");
			Say("joiner Disconnect: CloseConnection with linger");
			if (!WaitUntil(5000, pump, [&] { return host.closed; })) {
				return "the host did not see the joiner close within 5s";
			}
			if (host.closeReason.find("net-p2p-selftest done") == std::string::npos) {
				return "the host saw the close with reason \"" + host.closeReason + "\"";
			}
			Say("host saw the joiner's close " + Ms(host.closedMs - joiner.closedMs) + "ms after it, with the joiner's reason");
			host.transport.Stop();
			joiner.transport.Stop();
			Say("host and joiner Stop");
			return {};
		}

		void SingleProcessConfigs(GnsP2PConfig* hostConfig, GnsP2PConfig* joinerConfig) {
			hostConfig->rendezvousLogLevel = c_LogLevel;
			*joinerConfig = *hostConfig;
			joinerConfig->localVirtualPort = c_HostVirtualPort + 1;
			PrintConfig("host", *hostConfig);
			PrintConfig("joiner", *joinerConfig);
		}

		int RunSingleProcess(DropRule* drop) {
			Say(std::string("mode: single process, two GnsTransport instances joined by the in-memory signaling stub") + (drop ? "; the stub drops " + drop->direction + " #" + std::to_string(drop->number) : ""));
			EnableGnsOutput();
			GnsP2PConfig hostConfig;
			GnsP2PConfig joinerConfig;
			SingleProcessConfigs(&hostConfig, &joinerConfig);

			const auto toHost = std::make_shared<SignalQueue>("joiner->host");
			const auto toJoiner = std::make_shared<SignalQueue>("host->joiner");
			const auto releases = std::make_shared<std::atomic<int>>(0);
			std::string failure;
			double connectMs = -1;
			double connectedMs = -1;
			{
				Side host("host");
				Side joiner("joiner");
				StubRecvContext hostContext(toJoiner, releases, Answer::Accept);
				StubRecvContext joinerContext(toHost, releases, Answer::Ignore);
				const auto pump = [&] {
					Deliver(*toHost, host.transport, hostContext, drop);
					Deliver(*toJoiner, joiner.transport, joinerContext, drop);
					Drain(host);
					Drain(joiner);
					if (connectedMs < 0 && joiner.connected && IsConnected(joiner) && IsConnected(host)) {
						connectedMs = ElapsedMs();
					}
				};

				std::string error;
				if (!host.transport.StartHostP2P(c_HostVirtualPort, hostConfig, &error)) {
					failure = "StartHostP2P: " + error;
				} else {
					Say("host StartHostP2P(" + std::to_string(c_HostVirtualPort) + "): CreateListenSocketP2P and CreatePollGroup succeeded");
					const std::string identity = host.transport.GetLocalIdentity();
					Say("process identity (GameNetworkingSockets_Init(nullptr), no ResetIdentity): " + identity);
					connectMs = ElapsedMs();
					if (!joiner.transport.ConnectP2P(new StubConnectionSignaling(toHost, releases), identity, c_HostVirtualPort, joinerConfig, &error)) {
						failure = "ConnectP2P: " + error;
					} else {
						joiner.peer = 1;
						Say("joiner ConnectP2P(peer " + identity + ", remote virtual port " + std::to_string(c_HostVirtualPort) + "): ConnectP2PCustomSignaling returned a connection");
						failure = ConnectExchangeClose(host, joiner, pump, connectMs);
					}
				}
			}
			Say("transports destroyed; GNS released " + std::to_string(releases->load()) + " stub signaling object(s)");
			if (failure.empty() && releases->load() != 2) {
				failure = "GNS released " + std::to_string(releases->load()) + " signaling objects, expected 2";
			}
			if (drop) {
				if (drop->droppedMs < 0) {
					failure = failure.empty() ? "the stub never saw " + drop->direction + " #" + std::to_string(drop->number) + " to drop" : failure;
				} else {
					Say("drop summary: " + drop->direction + " #" + std::to_string(drop->number) + " dropped at t=" + Ms(drop->droppedMs) + "ms; next signal in that direction " +
					    (drop->nextMs < 0 ? std::string("never came") : Ms(drop->nextMs - drop->droppedMs) + "ms later") +
					    "; both Connected " + (connectedMs < 0 ? std::string("never") : Ms(connectedMs - connectMs) + "ms after ConnectP2P"));
				}
			}
			return Finish(failure);
		}

		/// reject: the host's context rejects every request (SendRejectionSignal). close: the host accepts, then closes at once.
		int RunRefusal(bool closeOnAccept) {
			Say(closeOnAccept ? "mode: single process; the host accepts the connect request and closes the connection at once"
			                  : "mode: single process; the host's receive context rejects the connect request");
			EnableGnsOutput();
			GnsP2PConfig hostConfig;
			GnsP2PConfig joinerConfig;
			SingleProcessConfigs(&hostConfig, &joinerConfig);

			const auto toHost = std::make_shared<SignalQueue>("joiner->host");
			const auto toJoiner = std::make_shared<SignalQueue>("host->joiner");
			const auto releases = std::make_shared<std::atomic<int>>(0);
			std::string failure;
			{
				Side host("host");
				Side joiner("joiner");
				StubRecvContext hostContext(toJoiner, releases, closeOnAccept ? Answer::Accept : Answer::Reject);
				StubRecvContext joinerContext(toHost, releases, Answer::Ignore);
				int refusedByJoiner = 0;
				double hostCloseMs = -1;
				const auto pump = [&] {
					Deliver(*toHost, host.transport, hostContext, nullptr);
					refusedByJoiner += Deliver(*toJoiner, joiner.transport, joinerContext, nullptr);
					Drain(host);
					if (closeOnAccept && host.connected && hostCloseMs < 0) {
						hostCloseMs = ElapsedMs();
						host.transport.Disconnect(host.peer, "net-p2p-selftest closes on accept");
						Say("host Disconnect of peer " + std::to_string(host.peer) + " right after accepting it");
					}
					Drain(joiner);
				};

				std::string error;
				if (!host.transport.StartHostP2P(c_HostVirtualPort, hostConfig, &error)) {
					failure = "StartHostP2P: " + error;
				} else {
					Say("host StartHostP2P(" + std::to_string(c_HostVirtualPort) + ") succeeded");
					const std::string identity = host.transport.GetLocalIdentity();
					const double connectMs = ElapsedMs();
					if (!joiner.transport.ConnectP2P(new StubConnectionSignaling(toHost, releases), identity, c_HostVirtualPort, joinerConfig, &error)) {
						failure = "ConnectP2P: " + error;
					} else {
						joiner.peer = 1;
						Say("joiner ConnectP2P(peer " + identity + ", remote virtual port " + std::to_string(c_HostVirtualPort) + ") returned a connection");
						if (!WaitUntil(15000, pump, [&] { return joiner.closed; })) {
							failure = "the joiner's connect did not end within 15s";
						} else {
							const double refusedMs = closeOnAccept ? hostCloseMs : hostContext.FirstRequestMs();
							const double afterRefusal = joiner.closedMs - refusedMs;
							Say("joiner connect ended " + Ms(joiner.closedMs - connectMs) + "ms after ConnectP2P and " + Ms(afterRefusal) + "ms after the host " +
							    (closeOnAccept ? "closed it" : "first rejected it") + ": \"" + joiner.closeReason + "\"");
							Say("the host answered " + std::to_string(hostContext.Requests()) + " connect request(s); SendRejectionSignal calls " + std::to_string(hostContext.Rejections()) +
							    "; signals the joiner's ReceivedP2PCustomSignal refused: " + std::to_string(refusedByJoiner));
							if (refusedMs < 0 || afterRefusal > c_PromptMs) {
								failure = std::string("the ") + (closeOnAccept ? "close" : "rejection") + " did not end the joiner's connect promptly: it ended " + Ms(afterRefusal) +
								          "ms after it (\"" + joiner.closeReason + "\"); the joiner's ReceivedP2PCustomSignal refused " + std::to_string(refusedByJoiner) + " signal(s)";
							}
						}
					}
					pump();
					host.transport.Stop();
					joiner.transport.Stop();
				}
			}
			Say("transports destroyed; GNS released " + std::to_string(releases->load()) + " stub signaling object(s)");
			return Finish(failure);
		}

		int RunGather(int iceEnable) {
			Say("mode: gather only; one joiner whose rendezvous the stub keeps, so ICE never learns a peer candidate and sends nothing");
			EnableGnsOutput();
			GnsP2PConfig config;
			config.iceEnable = iceEnable;
			config.rendezvousLogLevel = c_LogLevel;
			config.localVirtualPort = c_HostVirtualPort + 1;
			PrintConfig("joiner", config);

			const auto toHost = std::make_shared<SignalQueue>("joiner->nobody");
			const auto releases = std::make_shared<std::atomic<int>>(0);
			std::string failure;
			{
				Side joiner("joiner");
				std::string error;
				if (!joiner.transport.ConnectP2P(new StubConnectionSignaling(toHost, releases), std::string(), c_HostVirtualPort, config, &error)) {
					failure = "ConnectP2P: " + error;
				} else {
					joiner.peer = 1;
					Say("joiner ConnectP2P(no peer identity, remote virtual port " + std::to_string(c_HostVirtualPort) + "); its signals are kept, never delivered");
					const auto pump = [&] {
						for (const Signal& signal : toHost->TakeAll()) {
							Say("signal " + toHost->Name() + " #" + std::to_string(signal.number) + " kept, not delivered");
						}
						Drain(joiner);
					};
					WaitUntil(1500, pump, [] { return false; });
					joiner.transport.Stop();
					pump();
				}
			}
			std::vector<std::string> candidates;
			{
				std::lock_guard<std::mutex> lock(s_OutputMutex);
				candidates = s_LocalCandidates;
			}
			int ipv4 = 0;
			int ipv6 = 0;
			for (const std::string& candidate : candidates) {
				++(candidate.substr(0, candidate.find(' ')).find(':') == std::string::npos ? ipv4 : ipv6);
				Say("local candidate: " + candidate);
			}
			Say("local candidates GNS queued: " + std::to_string(candidates.size()) + " (IPv4 " + std::to_string(ipv4) + ", IPv6 " + std::to_string(ipv6) + ") with P2P_Transport_ICE_Enable=" +
			    std::to_string(iceEnable) + " (" + IceEnableNames(iceEnable) + ")");
			if (failure.empty() && candidates.empty()) {
				failure = "GNS queued no local candidate";
			}
			return Finish(failure);
		}

		std::string ToHex(const std::vector<uint8_t>& bytes) {
			static const char digits[] = "0123456789abcdef";
			std::string text;
			text.reserve(bytes.size() * 2);
			for (const uint8_t byte : bytes) {
				text += digits[byte >> 4];
				text += digits[byte & 15];
			}
			return text;
		}

		std::vector<uint8_t> FromHex(const std::string& text) {
			const auto value = [](char digit) { return digit >= 'a' ? digit - 'a' + 10 : digit - '0'; };
			std::vector<uint8_t> bytes;
			for (size_t i = 0; i + 1 < text.size(); i += 2) {
				bytes.push_back(static_cast<uint8_t>(value(text[i]) << 4 | value(text[i + 1])));
			}
			return bytes;
		}

		/// Signaling between two processes: each side appends one hex line per signal to its file and polls the peer's.
		class FileWire {
		public:
			FileWire(std::filesystem::path out, std::filesystem::path in) : m_Out(std::move(out)), m_In(std::move(in)) {}

			void Write(SignalQueue& queue) {
				for (const Signal& signal : queue.TakeAll()) {
					std::ofstream file(m_Out, std::ios::app | std::ios::binary);
					file << ToHex(signal.bytes) << '\n';
					file.flush();
					Say("signal " + queue.Name() + " #" + std::to_string(signal.number) + (file ? " appended to " : " FAILED to append to ") + m_Out.filename().string());
				}
			}

			void Read(GnsTransport& receiver, ISteamNetworkingSignalingRecvContext& context, const std::string& name) {
				std::ifstream file(m_In, std::ios::binary);
				if (!file) {
					return;
				}
				file.seekg(m_Offset);
				const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				size_t start = 0;
				for (size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
					const std::vector<uint8_t> bytes = FromHex(text.substr(start, end - start));
					start = end + 1;
					++m_Read;
					const bool accepted = receiver.ReceiveP2PSignal(bytes.data(), static_cast<int>(bytes.size()), &context);
					Say("signal " + name + " #" + std::to_string(m_Read) + " read from " + m_In.filename().string() + " (" + std::to_string(bytes.size()) +
					    " bytes): ReceivedP2PCustomSignal returned " + (accepted ? "true" : "false"));
				}
				m_Offset += static_cast<std::streamoff>(start);
			}

		private:
			std::filesystem::path m_Out;
			std::filesystem::path m_In;
			std::streamoff m_Offset = 0;
			int m_Read = 0;
		};

		int RunTwoProcess(bool isHost, int port) {
			const std::filesystem::path directory = std::filesystem::temp_directory_path();
			const std::string prefix = "net-p2p-" + std::to_string(port);
			const std::filesystem::path hostToJoin = directory / (prefix + "-host-to-join.sig");
			const std::filesystem::path joinToHost = directory / (prefix + "-join-to-host.sig");
			const std::string hostIdentity = "str:p2p-host-" + std::to_string(port);
			Say(std::string("mode: two processes, this one the ") + (isHost ? "host" : "joiner") + "; signal files " + hostToJoin.string() + " and " + joinToHost.string());
			EnableGnsOutput();
			GnsP2PConfig config;
			config.rendezvousLogLevel = c_LogLevel;
			config.localIdentity = isHost ? hostIdentity : "str:p2p-join-" + std::to_string(port);
			if (!isHost) {
				config.localVirtualPort = port + 1;
			}
			PrintConfig(isHost ? "host" : "joiner", config);

			const auto out = std::make_shared<SignalQueue>(isHost ? "host->joiner" : "joiner->host");
			const auto releases = std::make_shared<std::atomic<int>>(0);
			FileWire wire(isHost ? hostToJoin : joinToHost, isHost ? joinToHost : hostToJoin);
			std::string failure;
			{
				Side side(isHost ? "host" : "joiner");
				StubRecvContext context(out, releases, isHost ? Answer::Accept : Answer::Ignore);
				const auto pump = [&] {
					wire.Write(*out);
					wire.Read(side.transport, context, isHost ? "joiner->host" : "host->joiner");
					Drain(side);
				};
				const std::vector<uint8_t> mine = Payload(isHost ? 'H' : 'J');
				const std::vector<uint8_t> theirs = Payload(isHost ? 'J' : 'H');
				std::string error;
				if (isHost) {
					if (!side.transport.StartHostP2P(port, config, &error)) {
						failure = "StartHostP2P: " + error;
					} else {
						Say("host StartHostP2P(" + std::to_string(port) + ") succeeded; identity after ResetIdentity: " + side.transport.GetLocalIdentity());
						if (!WaitUntil(30000, pump, [&] { return side.closed || (IsConnected(side) && !side.received.empty()); }) || side.closed) {
							failure = "no joiner connected and sent its 64 bytes within 30s";
						} else if (side.received.front() != theirs) {
							failure = "the joiner's 64 bytes arrived altered";
						} else if (!side.transport.Send(side.peer, NetTransportLane::ControlReliable, mine, &error)) {
							failure = "host Send: " + error;
						} else {
							Say("host is Connected, received the joiner's 64 bytes intact and sent its own 64 bytes back");
							PrintConnection(side);
							if (CheckHostRoute(side, &failure)) {
								if (!WaitUntil(15000, pump, [&] { return side.closed; })) {
									failure = "the joiner did not close within 15s";
								} else if (side.closeReason.find("net-p2p-selftest done") == std::string::npos) {
									failure = "the host saw the close with reason \"" + side.closeReason + "\"";
								} else {
									Say("host saw the joiner's close with the joiner's reason");
								}
							}
						}
						side.transport.Stop();
					}
				} else {
					const double connectMs = ElapsedMs();
					if (!side.transport.ConnectP2P(new StubConnectionSignaling(out, releases), hostIdentity, port, config, &error)) {
						failure = "ConnectP2P: " + error;
					} else {
						side.peer = 1;
						Say("joiner ConnectP2P(peer " + hostIdentity + ", remote virtual port " + std::to_string(port) + ") returned a connection; identity after ResetIdentity: " + side.transport.GetLocalIdentity());
						if (!WaitUntil(30000, pump, [&] { return side.closed || (side.connected && IsConnected(side)); }) || side.closed) {
							failure = "the joiner did not reach Connected within 30s: \"" + side.closeReason + "\"";
						} else if (!side.transport.Send(side.peer, NetTransportLane::ControlReliable, mine, &error)) {
							failure = "joiner Send: " + error;
						} else {
							Say("joiner Connected " + Ms(ElapsedMs() - connectMs) + "ms after ConnectP2P; sent 64 bytes");
							if (!WaitUntil(10000, pump, [&] { return side.closed || !side.received.empty(); }) || side.received.empty()) {
								failure = "the host's 64 bytes did not arrive within 10s";
							} else if (side.received.front() != theirs) {
								failure = "the host's 64 bytes arrived altered";
							} else {
								Say("joiner received the host's 64 bytes intact");
								PrintConnection(side);
								if (CheckHostRoute(side, &failure)) {
									side.transport.Disconnect(side.peer, "net-p2p-selftest done");
									Say("joiner Disconnect: CloseConnection with linger");
									WaitUntil(1000, pump, [] { return false; });
								}
							}
						}
						side.transport.Stop();
						Say("joiner Stop");
					}
				}
			}
			Say("transport destroyed; GNS released " + std::to_string(releases->load()) + " stub signaling object(s)");
			return Finish(failure);
		}

		bool ParseNumber(const std::string& text, int* value) {
			char* end = nullptr;
			const long number = std::strtol(text.c_str(), &end, 0);
			if (text.empty() || !end || *end != '\0' || number < 0 || number > 0x7fffffff) {
				return false;
			}
			*value = static_cast<int>(number);
			return true;
		}

	} // namespace

	int GnsP2PSelfTest::Run(const std::vector<std::string>& args) {
		s_Start = Clock::now();
		int value = 0;
		if (args.empty()) {
			return RunSingleProcess(nullptr);
		}
		if (args.size() == 1 && (args[0] == "reject" || args[0] == "close-on-accept")) {
			return RunRefusal(args[0] == "close-on-accept");
		}
		if (args[0] == "gather" && args.size() == 2 && ParseNumber(args[1], &value)) {
			return RunGather(value);
		}
		if (args[0] == "drop" && args.size() == 3 && (args[1] == "j2h" || args[1] == "h2j") && ParseNumber(args[2], &value) && value > 0) {
			DropRule drop;
			drop.direction = args[1] == "j2h" ? "joiner->host" : "host->joiner";
			drop.number = value;
			return RunSingleProcess(&drop);
		}
		if ((args[0] == "host" || args[0] == "join") && args.size() == 2 && ParseNumber(args[1], &value) && value < 0xffff) {
			return RunTwoProcess(args[0] == "host", value);
		}
		std::cout << "[net-p2p-selftest] FAIL: usage: -net-p2p-selftest [reject | close-on-accept | gather <iceEnable> | drop j2h|h2j <n> | host <port> | join <port>]" << std::endl;
		return 1;
	}

#else

	int GnsP2PSelfTest::Run(const std::vector<std::string>&) {
		std::cout << "[net-p2p-selftest] FAIL: GameNetworkingSockets support is not compiled in; rebuild with CCCP_WITH_GNS" << std::endl;
		return 1;
	}

#endif

} // namespace RTE
